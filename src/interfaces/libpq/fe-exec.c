/*-------------------------------------------------------------------------
 *
 * fe-exec.c
 *	  functions related to sending a query down to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-exec.c,v 1.78 1999/04/04 20:10:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "libpq-fe.h"
#include "libpq-int.h"
#include "postgres.h"

#ifdef WIN32
#include "win32.h"
#else
#if !defined(NO_UNISTD_H)
#include <unistd.h>
#endif
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>


/* keep this in same order as ExecStatusType in libpq-fe.h */
const char *const pgresStatus[] = {
	"PGRES_EMPTY_QUERY",
	"PGRES_COMMAND_OK",
	"PGRES_TUPLES_OK",
	"PGRES_COPY_OUT",
	"PGRES_COPY_IN",
	"PGRES_BAD_RESPONSE",
	"PGRES_NONFATAL_ERROR",
	"PGRES_FATAL_ERROR"
};


#define DONOTICE(conn,message) \
	((*(conn)->noticeHook) ((conn)->noticeArg, (message)))


static int	addTuple(PGresult *res, PGresAttValue *tup);
static void parseInput(PGconn *conn);
static int	getRowDescriptions(PGconn *conn);
static int	getAnotherTuple(PGconn *conn, int binary);
static int	getNotify(PGconn *conn);
static int	getNotice(PGconn *conn);


/* ----------------
 * Space management for PGresult.
 *
 * Formerly, libpq did a separate malloc() for each field of each tuple
 * returned by a query.  This was remarkably expensive --- malloc/free
 * consumed a sizable part of the application's runtime.  And there is
 * no real need to keep track of the fields separately, since they will
 * all be freed together when the PGresult is released.  So now, we grab
 * large blocks of storage from malloc and allocate space for query data
 * within these blocks, using a trivially simple allocator.  This reduces
 * the number of malloc/free calls dramatically, and it also avoids
 * fragmentation of the malloc storage arena.
 * The PGresult structure itself is still malloc'd separately.  We could
 * combine it with the first allocation block, but that would waste space
 * for the common case that no extra storage is actually needed (that is,
 * the SQL command did not return tuples).
 * We also malloc the top-level array of tuple pointers separately, because
 * we need to be able to enlarge it via realloc, and our trivial space
 * allocator doesn't handle that effectively.  (Too bad the FE/BE protocol
 * doesn't tell us up front how many tuples will be returned.)
 * All other subsidiary storage for a PGresult is kept in PGresult_data blocks
 * of size PGRESULT_DATA_BLOCKSIZE.  The overhead at the start of each block
 * is just a link to the next one, if any.  Free-space management info is
 * kept in the owning PGresult.
 * A query returning a small amount of data will thus require three malloc
 * calls: one for the PGresult, one for the tuples pointer array, and one
 * PGresult_data block.
 * Only the most recently allocated PGresult_data block is a candidate to
 * have more stuff added to it --- any extra space left over in older blocks
 * is wasted.  We could be smarter and search the whole chain, but the point
 * here is to be simple and fast.  Typical applications do not keep a PGresult
 * around very long anyway, so some wasted space within one is not a problem.
 *
 * Tuning constants for the space allocator are:
 * PGRESULT_DATA_BLOCKSIZE: size of a standard allocation block, in bytes
 * PGRESULT_ALIGN_BOUNDARY: assumed alignment requirement for binary data
 * PGRESULT_SEP_ALLOC_THRESHOLD: objects bigger than this are given separate
 *   blocks, instead of being crammed into a regular allocation block.
 * Requirements for correct function are:
 * PGRESULT_ALIGN_BOUNDARY >= sizeof(pointer)
 *		to ensure the initial pointer in a block is not overwritten.
 * PGRESULT_ALIGN_BOUNDARY must be a multiple of the alignment requirements
 *		of all machine data types.
 * PGRESULT_SEP_ALLOC_THRESHOLD + PGRESULT_ALIGN_BOUNDARY <=
 *			PGRESULT_DATA_BLOCKSIZE
 *		pqResultAlloc assumes an object smaller than the threshold will fit
 *		in a new block.
 * The amount of space wasted at the end of a block could be as much as
 * PGRESULT_SEP_ALLOC_THRESHOLD, so it doesn't pay to make that too large.
 * ----------------
 */

#define PGRESULT_DATA_BLOCKSIZE		2048
#define PGRESULT_ALIGN_BOUNDARY		MAXIMUM_ALIGNOF /* from configure */
#define PGRESULT_SEP_ALLOC_THRESHOLD	(PGRESULT_DATA_BLOCKSIZE / 2)


/*
 * PQmakeEmptyPGresult
 *	 returns a newly allocated, initialized PGresult with given status.
 *	 If conn is not NULL and status indicates an error, the conn's
 *	 errorMessage is copied.
 *
 * Note this is exported --- you wouldn't think an application would need
 * to build its own PGresults, but this has proven useful in both libpgtcl
 * and the Perl5 interface, so maybe it's not so unreasonable.
 */

PGresult *
PQmakeEmptyPGresult(PGconn *conn, ExecStatusType status)
{
	PGresult   *result;

	result = (PGresult *) malloc(sizeof(PGresult));

	result->conn = conn;		/* might be NULL */
	result->ntups = 0;
	result->numAttributes = 0;
	result->attDescs = NULL;
	result->tuples = NULL;
	result->tupArrSize = 0;
	result->resultStatus = status;
	result->cmdStatus[0] = '\0';
	result->binary = 0;
	result->errMsg = NULL;
	result->null_field[0] = '\0';
	result->curBlock = NULL;
	result->curOffset = 0;
	result->spaceLeft = 0;

	if (conn)					/* consider copying conn's errorMessage */
	{
		switch (status)
		{
			case PGRES_EMPTY_QUERY:
			case PGRES_COMMAND_OK:
			case PGRES_TUPLES_OK:
			case PGRES_COPY_OUT:
			case PGRES_COPY_IN:
				/* non-error cases */
				break;
			default:
				pqSetResultError(result, conn->errorMessage);
				break;
		}
	}
	return result;
}

/*
 * pqResultAlloc -
 *		Allocate subsidiary storage for a PGresult.
 *
 * nBytes is the amount of space needed for the object.
 * If isBinary is true, we assume that we need to align the object on
 * a machine allocation boundary.
 * If isBinary is false, we assume the object is a char string and can
 * be allocated on any byte boundary.
 */
void *
pqResultAlloc(PGresult *res, int nBytes, int isBinary)
{
	char		   *space;
	PGresult_data  *block;

	if (! res)
		return NULL;

	if (nBytes <= 0)
		return res->null_field;

	/* If alignment is needed, round up the current position to an
	 * alignment boundary.
	 */
	if (isBinary)
	{
		int offset = res->curOffset % PGRESULT_ALIGN_BOUNDARY;
		if (offset)
		{
			res->curOffset += PGRESULT_ALIGN_BOUNDARY - offset;
			res->spaceLeft -= PGRESULT_ALIGN_BOUNDARY - offset;
		}
	}

	/* If there's enough space in the current block, no problem. */
	if (nBytes <= res->spaceLeft)
	{
		space = res->curBlock->space + res->curOffset;
		res->curOffset += nBytes;
		res->spaceLeft -= nBytes;
		return space;
	}

	/* If the requested object is very large, give it its own block; this
	 * avoids wasting what might be most of the current block to start a new
	 * block.  (We'd have to special-case requests bigger than the block size
	 * anyway.)  The object is always given binary alignment in this case.
	 */
	if (nBytes >= PGRESULT_SEP_ALLOC_THRESHOLD)
	{
		block = (PGresult_data *) malloc(nBytes + PGRESULT_ALIGN_BOUNDARY);
		if (! block)
			return NULL;
		space = block->space + PGRESULT_ALIGN_BOUNDARY;
		if (res->curBlock)
		{
			/* Tuck special block below the active block, so that we don't
			 * have to waste the free space in the active block.
			 */
			block->next = res->curBlock->next;
			res->curBlock->next = block;
		}
		else
		{
			/* Must set up the new block as the first active block. */
			block->next = NULL;
			res->curBlock = block;
			res->spaceLeft = 0;	/* be sure it's marked full */
		}
		return space;
	}

	/* Otherwise, start a new block. */
	block = (PGresult_data *) malloc(PGRESULT_DATA_BLOCKSIZE);
	if (! block)
		return NULL;
	block->next = res->curBlock;
	res->curBlock = block;
	if (isBinary)
	{
		/* object needs full alignment */
		res->curOffset = PGRESULT_ALIGN_BOUNDARY;
		res->spaceLeft = PGRESULT_DATA_BLOCKSIZE - PGRESULT_ALIGN_BOUNDARY;
	}
	else
	{
		/* we can cram it right after the overhead pointer */
		res->curOffset = sizeof(PGresult_data);
		res->spaceLeft = PGRESULT_DATA_BLOCKSIZE - sizeof(PGresult_data);
	}

	space = block->space + res->curOffset;
	res->curOffset += nBytes;
	res->spaceLeft -= nBytes;
	return space;
}

/*
 * pqResultStrdup -
 *		Like strdup, but the space is subsidiary PGresult space.
 */
char *
pqResultStrdup(PGresult *res, const char *str)
{
	char	*space = (char*) pqResultAlloc(res, strlen(str)+1, FALSE);
	if (space)
		strcpy(space, str);
	return space;
}

/*
 * pqSetResultError -
 *		assign a new error message to a PGresult
 */
void
pqSetResultError(PGresult *res, const char *msg)
{
	if (!res)
		return;
	if (msg && *msg)
		res->errMsg = pqResultStrdup(res, msg);
	else
		res->errMsg = NULL;
}

/*
 * PQclear -
 *	  free's the memory associated with a PGresult
 */
void
PQclear(PGresult *res)
{
	PGresult_data  *block;

	if (!res)
		return;

	/* Free all the subsidiary blocks */
	while ((block = res->curBlock) != NULL) {
		res->curBlock = block->next;
		free(block);
	}

	/* Free the top-level tuple pointer array */
	if (res->tuples)
		free(res->tuples);

	/* Free the PGresult structure itself */
	free(res);
}

/*
 * Handy subroutine to deallocate any partially constructed async result.
 */

void
pqClearAsyncResult(PGconn *conn)
{
	if (conn->result)
		PQclear(conn->result);
	conn->result = NULL;
	conn->curTuple = NULL;
}


/*
 * addTuple
 *	  add a row pointer to the PGresult structure, growing it if necessary
 *	  Returns TRUE if OK, FALSE if not enough memory to add the row
 */
static int
addTuple(PGresult *res, PGresAttValue *tup)
{
	if (res->ntups >= res->tupArrSize)
	{
		/*
		 * Try to grow the array.
		 *
		 * We can use realloc because shallow copying of the structure is
		 * okay.  Note that the first time through, res->tuples is NULL.
		 * While ANSI says that realloc() should act like malloc() in that
		 * case, some old C libraries (like SunOS 4.1.x) coredump instead.
		 * On failure realloc is supposed to return NULL without damaging
		 * the existing allocation.
		 * Note that the positions beyond res->ntups are garbage, not
		 * necessarily NULL.
		 */
		int newSize = (res->tupArrSize > 0) ? res->tupArrSize * 2 : 128;
		PGresAttValue ** newTuples;
		if (res->tuples == NULL)
			newTuples = (PGresAttValue **)
				malloc(newSize * sizeof(PGresAttValue *));
		else
			newTuples = (PGresAttValue **)
				realloc(res->tuples, newSize * sizeof(PGresAttValue *));
		if (! newTuples)
			return FALSE;		/* malloc or realloc failed */
		res->tupArrSize = newSize;
		res->tuples = newTuples;
	}
	res->tuples[res->ntups] = tup;
	res->ntups++;
	return TRUE;
}


/*
 * PQsendQuery
 *	 Submit a query, but don't wait for it to finish
 *
 * Returns: 1 if successfully submitted
 *			0 if error (conn->errorMessage is set)
 */

int
PQsendQuery(PGconn *conn, const char *query)
{
	if (!conn)
		return 0;
	if (!query)
	{
		sprintf(conn->errorMessage, "PQsendQuery() -- query pointer is null.");
		return 0;
	}
	/* check to see if the query string is too long */
	if (strlen(query) > MAX_MESSAGE_LEN - 2)
	{
		sprintf(conn->errorMessage, "PQsendQuery() -- query is too long.  "
				"Maximum length is %d\n", MAX_MESSAGE_LEN - 2);
		return 0;
	}

	/* Don't try to send if we know there's no live connection. */
	if (conn->status != CONNECTION_OK)
	{
		sprintf(conn->errorMessage, "PQsendQuery() -- There is no connection "
				"to the backend.\n");
		return 0;
	}
	/* Can't send while already busy, either. */
	if (conn->asyncStatus != PGASYNC_IDLE)
	{
		sprintf(conn->errorMessage,
				"PQsendQuery() -- another query already in progress.");
		return 0;
	}

	/* clear the error string */
	conn->errorMessage[0] = '\0';

	/* initialize async result-accumulation state */
	conn->result = NULL;
	conn->curTuple = NULL;

	/* send the query to the backend; */
	/* the frontend-backend protocol uses 'Q' to designate queries */
	if (pqPutnchar("Q", 1, conn))
		return 0;
	if (pqPuts(query, conn))
		return 0;
	if (pqFlush(conn))
		return 0;

	/* OK, it's launched! */
	conn->asyncStatus = PGASYNC_BUSY;
	return 1;
}


/*
 * Consume any available input from the backend
 * 0 return: some kind of trouble
 * 1 return: no problem
 */

int
PQconsumeInput(PGconn *conn)
{
	if (!conn)
		return 0;

	/*
	 * Load more data, if available. We do this no matter what state we
	 * are in, since we are probably getting called because the
	 * application wants to get rid of a read-select condition. Note that
	 * we will NOT block waiting for more input.
	 */
	if (pqReadData(conn) < 0)
		return 0;
	/* Parsing of the data waits till later. */
	return 1;
}


/*
 * parseInput: if appropriate, parse input data from backend
 * until input is exhausted or a stopping state is reached.
 * Note that this function will NOT attempt to read more data from the backend.
 */

static void
parseInput(PGconn *conn)
{
	char		id;

	/*
	 * Loop to parse successive complete messages available in the buffer.
	 */
	for (;;)
	{

		/*
		 * Quit if in COPY_OUT state: we expect raw data from the server
		 * until PQendcopy is called.  Don't try to parse it according to
		 * the normal protocol.  (This is bogus.  The data lines ought to
		 * be part of the protocol and have identifying leading
		 * characters.)
		 */
		if (conn->asyncStatus == PGASYNC_COPY_OUT)
			return;

		/*
		 * OK to try to read a message type code.
		 */
		conn->inCursor = conn->inStart;
		if (pqGetc(&id, conn))
			return;

		/*
		 * NOTIFY and NOTICE messages can happen in any state besides COPY
		 * OUT; always process them right away.
		 */
		if (id == 'A')
		{
			if (getNotify(conn))
				return;
		}
		else if (id == 'N')
		{
			if (getNotice(conn))
				return;
		}
		else
		{

			/*
			 * Other messages should only be processed while in BUSY
			 * state. (In particular, in READY state we hold off further
			 * parsing until the application collects the current
			 * PGresult.) If the state is IDLE then we got trouble.
			 */
			if (conn->asyncStatus != PGASYNC_BUSY)
			{
				if (conn->asyncStatus == PGASYNC_IDLE)
				{
					sprintf(conn->errorMessage,
					  "Backend message type 0x%02x arrived while idle\n",
							id);
					DONOTICE(conn, conn->errorMessage);
					/* Discard the unexpected message; good idea?? */
					conn->inStart = conn->inEnd;
				}
				return;
			}
			switch (id)
			{
				case 'C':		/* command complete */
					if (conn->result == NULL)
						conn->result = PQmakeEmptyPGresult(conn,
														   PGRES_COMMAND_OK);
					if (pqGets(conn->result->cmdStatus, CMDSTATUS_LEN, conn))
						return;
					conn->asyncStatus = PGASYNC_READY;
					break;
				case 'E':		/* error return */
					if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, conn))
						return;
					/* delete any partially constructed result */
					pqClearAsyncResult(conn);
					/* and build an error result holding the error message */
					conn->result = PQmakeEmptyPGresult(conn,
													   PGRES_FATAL_ERROR);
					conn->asyncStatus = PGASYNC_READY;
					break;
				case 'Z':		/* backend is ready for new query */
					conn->asyncStatus = PGASYNC_IDLE;
					break;
				case 'I':		/* empty query */
					/* read and throw away the closing '\0' */
					if (pqGetc(&id, conn))
						return;
					if (id != '\0')
					{
						sprintf(conn->errorMessage,
						  "unexpected character %c following 'I'\n", id);
						DONOTICE(conn, conn->errorMessage);
					}
					if (conn->result == NULL)
						conn->result = PQmakeEmptyPGresult(conn,
														   PGRES_EMPTY_QUERY);
					conn->asyncStatus = PGASYNC_READY;
					break;
				case 'K':		/* secret key data from the backend */

					/*
					 * This is expected only during backend startup, but
					 * it's just as easy to handle it as part of the main
					 * loop.  Save the data and continue processing.
					 */
					if (pqGetInt(&(conn->be_pid), 4, conn))
						return;
					if (pqGetInt(&(conn->be_key), 4, conn))
						return;
					break;
				case 'P':		/* synchronous (normal) portal */
					if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, conn))
						return;
					/* We pretty much ignore this message type... */
					break;
				case 'T':		/* row descriptions (start of query
								 * results) */
					if (conn->result == NULL)
					{
						/* First 'T' in a query sequence */
						if (getRowDescriptions(conn))
							return;
					}
					else
					{

						/*
						 * A new 'T' message is treated as the start of
						 * another PGresult.  (It is not clear that this
						 * is really possible with the current backend.)
						 * We stop parsing until the application accepts
						 * the current result.
						 */
						conn->asyncStatus = PGASYNC_READY;
						return;
					}
					break;
				case 'D':		/* ASCII data tuple */
					if (conn->result != NULL)
					{
						/* Read another tuple of a normal query response */
						if (getAnotherTuple(conn, FALSE))
							return;
					}
					else
					{
						sprintf(conn->errorMessage,
							 "Backend sent D message without prior T\n");
						DONOTICE(conn, conn->errorMessage);
						/* Discard the unexpected message; good idea?? */
						conn->inStart = conn->inEnd;
						return;
					}
					break;
				case 'B':		/* Binary data tuple */
					if (conn->result != NULL)
					{
						/* Read another tuple of a normal query response */
						if (getAnotherTuple(conn, TRUE))
							return;
					}
					else
					{
						sprintf(conn->errorMessage,
							 "Backend sent B message without prior T\n");
						DONOTICE(conn, conn->errorMessage);
						/* Discard the unexpected message; good idea?? */
						conn->inStart = conn->inEnd;
						return;
					}
					break;
				case 'G':		/* Start Copy In */
					conn->asyncStatus = PGASYNC_COPY_IN;
					break;
				case 'H':		/* Start Copy Out */
					conn->asyncStatus = PGASYNC_COPY_OUT;
					break;
				default:
					sprintf(conn->errorMessage,
					"unknown protocol character '%c' read from backend.  "
					"(The protocol character is the first character the "
					"backend sends in response to a query it receives).\n",
							id);
					/* Discard the unexpected message; good idea?? */
					conn->inStart = conn->inEnd;
					/* delete any partially constructed result */
					pqClearAsyncResult(conn);
					/* and build an error result holding the error message */
					conn->result = PQmakeEmptyPGresult(conn,
													   PGRES_FATAL_ERROR);
					conn->asyncStatus = PGASYNC_READY;
					return;
			}					/* switch on protocol character */
		}
		/* Successfully consumed this message */
		conn->inStart = conn->inCursor;
	}
}


/*
 * parseInput subroutine to read a 'T' (row descriptions) message.
 * We build a PGresult structure containing the attribute data.
 * Returns: 0 if completed message, EOF if not enough data yet.
 *
 * Note that if we run out of data, we have to release the partially
 * constructed PGresult, and rebuild it again next time.  Fortunately,
 * that shouldn't happen often, since 'T' messages usually fit in a packet.
 */

static int
getRowDescriptions(PGconn *conn)
{
	PGresult   *result;
	int			nfields;
	int			i;

	result = PQmakeEmptyPGresult(conn, PGRES_TUPLES_OK);

	/* parseInput already read the 'T' label. */
	/* the next two bytes are the number of fields	*/
	if (pqGetInt(&(result->numAttributes), 2, conn))
	{
		PQclear(result);
		return EOF;
	}
	nfields = result->numAttributes;

	/* allocate space for the attribute descriptors */
	if (nfields > 0)
	{
		result->attDescs = (PGresAttDesc *)
			pqResultAlloc(result, nfields * sizeof(PGresAttDesc), TRUE);
		MemSet((char *) result->attDescs, 0, nfields * sizeof(PGresAttDesc));
	}

	/* get type info */
	for (i = 0; i < nfields; i++)
	{
		char		typName[MAX_MESSAGE_LEN];
		int			typid;
		int			typlen;
		int			atttypmod;

		if (pqGets(typName, MAX_MESSAGE_LEN, conn) ||
			pqGetInt(&typid, 4, conn) ||
			pqGetInt(&typlen, 2, conn) ||
			pqGetInt(&atttypmod, 4, conn))
		{
			PQclear(result);
			return EOF;
		}
		/*
		 * Since pqGetInt treats 2-byte integers as unsigned, we need to
		 * coerce the special value "-1" to signed form.  (-1 is sent for
		 * variable-length fields.)  Formerly, libpq effectively did a
		 * sign-extension on the 2-byte value by storing it in a signed short.
		 * Now we only coerce the single value 65535 == -1; values
		 * 32768..65534 are taken as valid field lengths.
		 */
		if (typlen == 0xFFFF)
			typlen = -1;
		result->attDescs[i].name = pqResultStrdup(result, typName);
		result->attDescs[i].typid = typid;
		result->attDescs[i].typlen = typlen;
		result->attDescs[i].atttypmod = atttypmod;
	}

	/* Success! */
	conn->result = result;
	return 0;
}

/*
 * parseInput subroutine to read a 'B' or 'D' (row data) message.
 * We add another tuple to the existing PGresult structure.
 * Returns: 0 if completed message, EOF if error or not enough data yet.
 *
 * Note that if we run out of data, we have to suspend and reprocess
 * the message after more data is received.  We keep a partially constructed
 * tuple in conn->curTuple, and avoid reallocating already-allocated storage.
 */

static int
getAnotherTuple(PGconn *conn, int binary)
{
	PGresult   *result = conn->result;
	int			nfields = result->numAttributes;
	PGresAttValue *tup;
	char		bitmap[MAX_FIELDS];		/* the backend sends us a bitmap
										 * of which attributes are null */
	int			i;
	int			nbytes;			/* the number of bytes in bitmap  */
	char		bmap;			/* One byte of the bitmap */
	int			bitmap_index;	/* Its index */
	int			bitcnt;			/* number of bits examined in current byte */
	int			vlen;			/* length of the current field value */

	result->binary = binary;

	/* Allocate tuple space if first time for this data message */
	if (conn->curTuple == NULL)
	{
		conn->curTuple = (PGresAttValue *)
			pqResultAlloc(result, nfields * sizeof(PGresAttValue), TRUE);
		if (conn->curTuple == NULL)
			goto outOfMemory;
		MemSet((char *) conn->curTuple, 0, nfields * sizeof(PGresAttValue));
	}
	tup = conn->curTuple;

	/* Get the null-value bitmap */
	nbytes = (nfields + BYTELEN - 1) / BYTELEN;
	if (nbytes >= MAX_FIELDS)
	{
		/* Replace partially constructed result with an error result */
		pqClearAsyncResult(conn);
		sprintf(conn->errorMessage,
				"getAnotherTuple() -- null-values bitmap is too large\n");
		conn->result = PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
		conn->asyncStatus = PGASYNC_READY;
		/* Discard the broken message */
		conn->inStart = conn->inEnd;
		return EOF;
	}

	if (pqGetnchar(bitmap, nbytes, conn))
		return EOF;

	/* Scan the fields */
	bitmap_index = 0;
	bmap = bitmap[bitmap_index];
	bitcnt = 0;

	for (i = 0; i < nfields; i++)
	{
		if (!(bmap & 0200))
		{
			/* if the field value is absent, make it a null string */
			tup[i].value = result->null_field;
			tup[i].len = NULL_LEN;
		}
		else
		{
			/* get the value length (the first four bytes are for length) */
			if (pqGetInt(&vlen, 4, conn))
				return EOF;
			if (binary == 0)
				vlen = vlen - 4;
			if (vlen < 0)
				vlen = 0;
			if (tup[i].value == NULL)
			{
				tup[i].value = (char *) pqResultAlloc(result, vlen+1, binary);
				if (tup[i].value == NULL)
					goto outOfMemory;
			}
			tup[i].len = vlen;
			/* read in the value */
			if (vlen > 0)
				if (pqGetnchar((char *) (tup[i].value), vlen, conn))
					return EOF;
			/* we have to terminate this ourselves */
			tup[i].value[vlen] = '\0';
		}
		/* advance the bitmap stuff */
		bitcnt++;
		if (bitcnt == BYTELEN)
		{
			bitmap_index++;
			bmap = bitmap[bitmap_index];
			bitcnt = 0;
		}
		else
			bmap <<= 1;
	}

	/* Success!  Store the completed tuple in the result */
	if (! addTuple(result, tup))
		goto outOfMemory;
	/* and reset for a new message */
	conn->curTuple = NULL;
	return 0;

outOfMemory:
	/* Replace partially constructed result with an error result */
	pqClearAsyncResult(conn);
	sprintf(conn->errorMessage,
			"getAnotherTuple() -- out of memory for result\n");
	conn->result = PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
	conn->asyncStatus = PGASYNC_READY;
	/* Discard the failed message --- good idea? */
	conn->inStart = conn->inEnd;
	return EOF;
}


/*
 * PQisBusy
 *	 Return TRUE if PQgetResult would block waiting for input.
 */

int
PQisBusy(PGconn *conn)
{
	if (!conn)
		return FALSE;

	/* Parse any available data, if our state permits. */
	parseInput(conn);

	/* PQgetResult will return immediately in all states except BUSY. */
	return conn->asyncStatus == PGASYNC_BUSY;
}


/*
 * PQgetResult
 *	  Get the next PGresult produced by a query.
 *	  Returns NULL if and only if no query work remains.
 */

PGresult   *
PQgetResult(PGconn *conn)
{
	PGresult   *res;

	if (!conn)
		return NULL;

	/* Parse any available data, if our state permits. */
	parseInput(conn);

	/* If not ready to return something, block until we are. */
	while (conn->asyncStatus == PGASYNC_BUSY)
	{
		/* Wait for some more data, and load it. */
		if (pqWait(TRUE, FALSE, conn) ||
			pqReadData(conn) < 0)
		{
			pqClearAsyncResult(conn);
			conn->asyncStatus = PGASYNC_IDLE;
			/* conn->errorMessage has been set by pqWait or pqReadData. */
			return PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
		}
		/* Parse it. */
		parseInput(conn);
	}

	/* Return the appropriate thing. */
	switch (conn->asyncStatus)
	{
		case PGASYNC_IDLE:
			res = NULL;			/* query is complete */
			break;
		case PGASYNC_READY:
			/*
			 * conn->result is the PGresult to return.  If it is NULL
			 * (which probably shouldn't happen) we assume there is
			 * an appropriate error message in conn->errorMessage.
			 */
			res = conn->result;
			conn->result = NULL;		/* handing over ownership to caller */
			conn->curTuple = NULL;		/* just in case */
			if (!res)
			{
				res = PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
			}
			else
			{
				/* Make sure PQerrorMessage agrees with result; it could be
				 * that we have done other operations that changed
				 * errorMessage since the result's error message was saved.
				 */
				strcpy(conn->errorMessage, PQresultErrorMessage(res));
			}
			/* Set the state back to BUSY, allowing parsing to proceed. */
			conn->asyncStatus = PGASYNC_BUSY;
			break;
		case PGASYNC_COPY_IN:
			res = PQmakeEmptyPGresult(conn, PGRES_COPY_IN);
			break;
		case PGASYNC_COPY_OUT:
			res = PQmakeEmptyPGresult(conn, PGRES_COPY_OUT);
			break;
		default:
			sprintf(conn->errorMessage,
					"PQgetResult: Unexpected asyncStatus %d\n",
					(int) conn->asyncStatus);
			res = PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
			break;
	}

	return res;
}


/*
 * PQexec
 *	  send a query to the backend and package up the result in a PGresult
 *
 * If the query was not even sent, return NULL; conn->errorMessage is set to
 * a relevant message.
 * If the query was sent, a new PGresult is returned (which could indicate
 * either success or failure).
 * The user is responsible for freeing the PGresult via PQclear()
 * when done with it.
 */

PGresult   *
PQexec(PGconn *conn, const char *query)
{
	PGresult   *result;
	PGresult   *lastResult;

	/*
	 * Silently discard any prior query result that application didn't
	 * eat. This is probably poor design, but it's here for backward
	 * compatibility.
	 */
	while ((result = PQgetResult(conn)) != NULL)
	{
		if (result->resultStatus == PGRES_COPY_IN ||
			result->resultStatus == PGRES_COPY_OUT)
		{
			PQclear(result);
			sprintf(conn->errorMessage,
				"PQexec: you gotta get out of a COPY state yourself.\n");
			return NULL;
		}
		PQclear(result);
	}

	/* OK to send the message */
	if (!PQsendQuery(conn, query))
		return NULL;

	/*
	 * For backwards compatibility, return the last result if there are
	 * more than one.  We have to stop if we see copy in/out, however. We
	 * will resume parsing when application calls PQendcopy.
	 */
	lastResult = NULL;
	while ((result = PQgetResult(conn)) != NULL)
	{
		if (lastResult)
			PQclear(lastResult);
		lastResult = result;
		if (result->resultStatus == PGRES_COPY_IN ||
			result->resultStatus == PGRES_COPY_OUT)
			break;
	}
	return lastResult;
}


/*
 * Attempt to read a Notice response message.
 * This is possible in several places, so we break it out as a subroutine.
 * Entry: 'N' flag character has already been consumed.
 * Exit: returns 0 if successfully consumed Notice message.
 *		 returns EOF if not enough data.
 */
static int
getNotice(PGconn *conn)
{
	if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, conn))
		return EOF;
	DONOTICE(conn, conn->errorMessage);
	return 0;
}

/*
 * Attempt to read a Notify response message.
 * This is possible in several places, so we break it out as a subroutine.
 * Entry: 'A' flag character has already been consumed.
 * Exit: returns 0 if successfully consumed Notify message.
 *		 returns EOF if not enough data.
 */
static int
getNotify(PGconn *conn)
{
	PGnotify	tempNotify;
	PGnotify   *newNotify;

	if (pqGetInt(&(tempNotify.be_pid), 4, conn))
		return EOF;
	if (pqGets(tempNotify.relname, NAMEDATALEN, conn))
		return EOF;
	newNotify = (PGnotify *) malloc(sizeof(PGnotify));
	memcpy(newNotify, &tempNotify, sizeof(PGnotify));
	DLAddTail(conn->notifyList, DLNewElem(newNotify));
	return 0;
}

/*
 * PQnotifies
 *	  returns a PGnotify* structure of the latest async notification
 * that has not yet been handled
 *
 * returns NULL, if there is currently
 * no unhandled async notification from the backend
 *
 * the CALLER is responsible for FREE'ing the structure returned
 */

PGnotify   *
PQnotifies(PGconn *conn)
{
	Dlelem	   *e;
	PGnotify   *event;

	if (!conn)
		return NULL;

	/* Parse any available data to see if we can extract NOTIFY messages. */
	parseInput(conn);

	/* RemHead returns NULL if list is empty */
	e = DLRemHead(conn->notifyList);
	if (!e)
		return NULL;
	event = (PGnotify *) DLE_VAL(e);
	DLFreeElem(e);
	return event;
}

/*
 * PQgetline - gets a newline-terminated string from the backend.
 *
 * Chiefly here so that applications can use "COPY <rel> to stdout"
 * and read the output string.	Returns a null-terminated string in s.
 *
 * PQgetline reads up to maxlen-1 characters (like fgets(3)) but strips
 * the terminating \n (like gets(3)).
 *
 * CAUTION: the caller is responsible for detecting the end-of-copy signal
 * (a line containing just "\.") when using this routine.
 *
 * RETURNS:
 *		EOF if it is detected or invalid arguments are given
 *		0 if EOL is reached (i.e., \n has been read)
 *				(this is required for backward-compatibility -- this
 *				 routine used to always return EOF or 0, assuming that
 *				 the line ended within maxlen bytes.)
 *		1 in other cases (i.e., the buffer was filled before \n is reached)
 */
int
PQgetline(PGconn *conn, char *s, int maxlen)
{
	int			result = 1;		/* return value if buffer overflows */

	if (!s || maxlen <= 0)
		return EOF;

	if (!conn || conn->sock < 0)
	{
		*s = '\0';
		return EOF;
	}

	/*
	 * Since this is a purely synchronous routine, we don't bother to
	 * maintain conn->inCursor; there is no need to back up.
	 */
	while (maxlen > 1)
	{
		if (conn->inStart < conn->inEnd)
		{
			char		c = conn->inBuffer[conn->inStart++];

			if (c == '\n')
			{
				result = 0;		/* success exit */
				break;
			}
			*s++ = c;
			maxlen--;
		}
		else
		{
			/* need to load more data */
			if (pqWait(TRUE, FALSE, conn) ||
				pqReadData(conn) < 0)
			{
				result = EOF;
				break;
			}
		}
	}
	*s = '\0';

	return result;
}

/*
 * PQgetlineAsync - gets a newline-terminated string without blocking.
 *
 * This routine is for applications that want to do "COPY <rel> to stdout"
 * asynchronously, that is without blocking.  Having issued the COPY command
 * and gotten a PGRES_COPY_OUT response, the app should call PQconsumeInput
 * and this routine until the end-of-data signal is detected.  Unlike
 * PQgetline, this routine takes responsibility for detecting end-of-data.
 *
 * On each call, PQgetlineAsync will return data if a complete newline-
 * terminated data line is available in libpq's input buffer, or if the
 * incoming data line is too long to fit in the buffer offered by the caller.
 * Otherwise, no data is returned until the rest of the line arrives.
 *
 * If -1 is returned, the end-of-data signal has been recognized (and removed
 * from libpq's input buffer).  The caller *must* next call PQendcopy and
 * then return to normal processing.
 *
 * RETURNS:
 *   -1    if the end-of-copy-data marker has been recognized
 *   0     if no data is available
 *   >0    the number of bytes returned.
 * The data returned will not extend beyond a newline character.  If possible
 * a whole line will be returned at one time.  But if the buffer offered by
 * the caller is too small to hold a line sent by the backend, then a partial
 * data line will be returned.  This can be detected by testing whether the
 * last returned byte is '\n' or not.
 * The returned string is *not* null-terminated.
 */

int
PQgetlineAsync(PGconn *conn, char *buffer, int bufsize)
{
    int		avail;

	if (!conn || conn->asyncStatus != PGASYNC_COPY_OUT)
		return -1;				/* we are not doing a copy... */

	/*
	 * Move data from libpq's buffer to the caller's.
	 * We want to accept data only in units of whole lines,
	 * not partial lines.  This ensures that we can recognize
	 * the terminator line "\\.\n".  (Otherwise, if it happened
	 * to cross a packet/buffer boundary, we might hand the first
	 * one or two characters off to the caller, which we shouldn't.)
	 */

	conn->inCursor = conn->inStart;

	avail = bufsize;
	while (avail > 0 && conn->inCursor < conn->inEnd)
	{
		char c = conn->inBuffer[conn->inCursor++];
		*buffer++ = c;
		--avail;
		if (c == '\n')
		{
			/* Got a complete line; mark the data removed from libpq */
			conn->inStart = conn->inCursor;
			/* Is it the endmarker line? */
			if (bufsize-avail == 3 && buffer[-3] == '\\' && buffer[-2] == '.')
				return -1;
			/* No, return the data line to the caller */
			return bufsize - avail;
		}
	}

	/*
	 * We don't have a complete line.
	 * We'd prefer to leave it in libpq's buffer until the rest arrives,
	 * but there is a special case: what if the line is longer than the
	 * buffer the caller is offering us?  In that case we'd better hand over
	 * a partial line, else we'd get into an infinite loop.
	 * Do this in a way that ensures we can't misrecognize a terminator
	 * line later: leave last 3 characters in libpq buffer.
	 */
	if (avail == 0 && bufsize > 3)
	{
		conn->inStart = conn->inCursor - 3;
		return bufsize - 3;
	}
	return 0;
}

/*
 * PQputline -- sends a string to the backend.
 * Returns 0 if OK, EOF if not.
 *
 * Chiefly here so that applications can use "COPY <rel> from stdin".
 */
int
PQputline(PGconn *conn, const char *s)
{
	if (!conn || conn->sock < 0)
		return EOF;
	return pqPutnchar(s, strlen(s), conn);
}

/*
 * PQputnbytes -- like PQputline, but buffer need not be null-terminated.
 * Returns 0 if OK, EOF if not.
 */
int
PQputnbytes(PGconn *conn, const char *buffer, int nbytes)
{
	if (!conn || conn->sock < 0)
		return EOF;
	return pqPutnchar(buffer, nbytes, conn);
}

/*
 * PQendcopy
 *		After completing the data transfer portion of a copy in/out,
 *		the application must call this routine to finish the command protocol.
 *
 * RETURNS:
 *		0 on success
 *		1 on failure
 */
int
PQendcopy(PGconn *conn)
{
	PGresult   *result;

	if (!conn)
		return 0;

	if (conn->asyncStatus != PGASYNC_COPY_IN &&
		conn->asyncStatus != PGASYNC_COPY_OUT)
	{
		sprintf(conn->errorMessage,
			 "PQendcopy() -- I don't think there's a copy in progress.");
		return 1;
	}

	(void) pqFlush(conn);		/* make sure no data is waiting to be sent */

	/* Return to active duty */
	conn->asyncStatus = PGASYNC_BUSY;
	conn->errorMessage[0] = '\0';

	/* Wait for the completion response */
	result = PQgetResult(conn);

	/* Expecting a successful result */
	if (result && result->resultStatus == PGRES_COMMAND_OK)
	{
		PQclear(result);
		return 0;
	}

	/*
	 * Trouble. The worst case is that we've lost sync with the backend
	 * entirely due to application screwup of the copy in/out protocol. To
	 * recover, reset the connection (talk about using a sledgehammer...)
	 */
	PQclear(result);

	if (conn->errorMessage[0])
		DONOTICE(conn, conn->errorMessage);

	DONOTICE(conn, "PQendcopy: resetting connection\n");

	PQreset(conn);

	return 1;
}


/* ----------------
 *		PQfn -	Send a function call to the POSTGRES backend.
 *
 *		conn			: backend connection
 *		fnid			: function id
 *		result_buf		: pointer to result buffer (&int if integer)
 *		result_len		: length of return value.
 *		actual_result_len: actual length returned. (differs from result_len
 *						  for varlena structures.)
 *		result_type		: If the result is an integer, this must be 1,
 *						  otherwise this should be 0
 *		args			: pointer to an array of function arguments.
 *						  (each has length, if integer, and value/pointer)
 *		nargs			: # of arguments in args array.
 *
 * RETURNS
 *		PGresult with status = PGRES_COMMAND_OK if successful.
 *			*actual_result_len is > 0 if there is a return value, 0 if not.
 *		PGresult with status = PGRES_FATAL_ERROR if backend returns an error.
 *		NULL on communications failure.  conn->errorMessage will be set.
 * ----------------
 */

PGresult   *
PQfn(PGconn *conn,
	 int fnid,
	 int *result_buf,
	 int *actual_result_len,
	 int result_is_int,
	 PQArgBlock *args,
	 int nargs)
{
	bool		needInput = false;
	ExecStatusType status = PGRES_FATAL_ERROR;
	char		id;
	int			i;

	*actual_result_len = 0;

	if (!conn)
		return NULL;

	if (conn->sock < 0 || conn->asyncStatus != PGASYNC_IDLE)
	{
		sprintf(conn->errorMessage, "PQfn() -- connection in wrong state\n");
		return NULL;
	}

	/* clear the error string */
	conn->errorMessage[0] = '\0';

	if (pqPuts("F ", conn))		/* function */
		return NULL;
	if (pqPutInt(fnid, 4, conn))/* function id */
		return NULL;
	if (pqPutInt(nargs, 4, conn))		/* # of args */
		return NULL;

	for (i = 0; i < nargs; ++i)
	{							/* len.int4 + contents	   */
		if (pqPutInt(args[i].len, 4, conn))
			return NULL;

		if (args[i].isint)
		{
			if (pqPutInt(args[i].u.integer, 4, conn))
				return NULL;
		}
		else
		{
			if (pqPutnchar((char *) args[i].u.ptr, args[i].len, conn))
				return NULL;
		}
	}
	if (pqFlush(conn))
		return NULL;

	for (;;)
	{
		if (needInput)
		{
			/* Wait for some data to arrive (or for the channel to close) */
			if (pqWait(TRUE, FALSE, conn) ||
				pqReadData(conn) < 0)
				break;
		}

		/*
		 * Scan the message. If we run out of data, loop around to try
		 * again.
		 */
		conn->inCursor = conn->inStart;
		needInput = true;

		if (pqGetc(&id, conn))
			continue;

		/*
		 * We should see V or E response to the command, but might get N
		 * and/or A notices first. We also need to swallow the final Z
		 * before returning.
		 */
		switch (id)
		{
			case 'V':			/* function result */
				if (pqGetc(&id, conn))
					continue;
				if (id == 'G')
				{
					/* function returned nonempty value */
					if (pqGetInt(actual_result_len, 4, conn))
						continue;
					if (result_is_int)
					{
						if (pqGetInt(result_buf, 4, conn))
							continue;
					}
					else
					{
						if (pqGetnchar((char *) result_buf,
									   *actual_result_len,
									   conn))
							continue;
					}
					if (pqGetc(&id, conn))		/* get the last '0' */
						continue;
				}
				if (id == '0')
				{
					/* correctly finished function result message */
					status = PGRES_COMMAND_OK;
				}
				else
				{
					/* The backend violates the protocol. */
					sprintf(conn->errorMessage,
							"FATAL: PQfn: protocol error: id=%x\n", id);
					conn->inStart = conn->inCursor;
					return PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
				}
				break;
			case 'E':			/* error return */
				if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, conn))
					continue;
				status = PGRES_FATAL_ERROR;
				break;
			case 'A':			/* notify message */
				/* handle notify and go back to processing return values */
				if (getNotify(conn))
					continue;
				break;
			case 'N':			/* notice */
				/* handle notice and go back to processing return values */
				if (getNotice(conn))
					continue;
				break;
			case 'Z':			/* backend is ready for new query */
				/* consume the message and exit */
				conn->inStart = conn->inCursor;
				return PQmakeEmptyPGresult(conn, status);
			default:
				/* The backend violates the protocol. */
				sprintf(conn->errorMessage,
						"FATAL: PQfn: protocol error: id=%x\n", id);
				conn->inStart = conn->inCursor;
				return PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
		}
		/* Completed this message, keep going */
		conn->inStart = conn->inCursor;
		needInput = false;
	}

	/* we fall out of the loop only upon failing to read data */
	return PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
}


/* ====== accessor funcs for PGresult ======== */

ExecStatusType
PQresultStatus(PGresult *res)
{
	if (!res)
		return PGRES_NONFATAL_ERROR;
	return res->resultStatus;
}

const char *
PQresStatus(ExecStatusType status)
{
	if (((int) status) < 0 ||
		((int) status) >= (sizeof(pgresStatus) / sizeof(pgresStatus[0])))
		return "Invalid ExecStatusType code";
	return pgresStatus[status];
}

const char *
PQresultErrorMessage(PGresult *res)
{
	if (!res || !res->errMsg)
		return "";
	return res->errMsg;
}

int
PQntuples(PGresult *res)
{
	if (!res)
		return 0;
	return res->ntups;
}

int
PQnfields(PGresult *res)
{
	if (!res)
		return 0;
	return res->numAttributes;
}

int
PQbinaryTuples(PGresult *res)
{
	if (!res)
		return 0;
	return res->binary;
}

/*
 * Helper routines to range-check field numbers and tuple numbers.
 * Return TRUE if OK, FALSE if not
 */

static int
check_field_number(const char *routineName, PGresult *res, int field_num)
{
	if (!res)
		return FALSE;			/* no way to display error message... */
	if (field_num < 0 || field_num >= res->numAttributes)
	{
		if (res->conn)
		{
			sprintf(res->conn->errorMessage,
					"%s: ERROR! field number %d is out of range 0..%d\n",
					routineName, field_num, res->numAttributes - 1);
			DONOTICE(res->conn, res->conn->errorMessage);
		}
		return FALSE;
	}
	return TRUE;
}

static int
check_tuple_field_number(const char *routineName, PGresult *res,
						 int tup_num, int field_num)
{
	if (!res)
		return FALSE;			/* no way to display error message... */
	if (tup_num < 0 || tup_num >= res->ntups)
	{
		if (res->conn)
		{
			sprintf(res->conn->errorMessage,
					"%s: ERROR! tuple number %d is out of range 0..%d\n",
					routineName, tup_num, res->ntups - 1);
			DONOTICE(res->conn, res->conn->errorMessage);
		}
		return FALSE;
	}
	if (field_num < 0 || field_num >= res->numAttributes)
	{
		if (res->conn)
		{
			sprintf(res->conn->errorMessage,
					"%s: ERROR! field number %d is out of range 0..%d\n",
					routineName, field_num, res->numAttributes - 1);
			DONOTICE(res->conn, res->conn->errorMessage);
		}
		return FALSE;
	}
	return TRUE;
}

/*
   returns NULL if the field_num is invalid
*/
char *
PQfname(PGresult *res, int field_num)
{
	if (!check_field_number("PQfname", res, field_num))
		return NULL;
	if (res->attDescs)
		return res->attDescs[field_num].name;
	else
		return NULL;
}

/*
   returns -1 on a bad field name
*/
int
PQfnumber(PGresult *res, const char *field_name)
{
	int			i;
	char	   *field_case;

	if (!res)
		return -1;

	if (field_name == NULL ||
		field_name[0] == '\0' ||
		res->attDescs == NULL)
		return -1;

	field_case = strdup(field_name);
	if (*field_case == '"')
	{
		strcpy(field_case, field_case + 1);
		*(field_case + strlen(field_case) - 1) = '\0';
	}
	else
		for (i = 0; field_case[i]; i++)
			if (isascii((unsigned char) field_case[i]) &&
				isupper(field_case[i]))
				field_case[i] = tolower(field_case[i]);

	for (i = 0; i < res->numAttributes; i++)
	{
		if (strcmp(field_case, res->attDescs[i].name) == 0)
		{
			free(field_case);
			return i;
		}
	}
	free(field_case);
	return -1;
}

Oid
PQftype(PGresult *res, int field_num)
{
	if (!check_field_number("PQftype", res, field_num))
		return InvalidOid;
	if (res->attDescs)
		return res->attDescs[field_num].typid;
	else
		return InvalidOid;
}

int
PQfsize(PGresult *res, int field_num)
{
	if (!check_field_number("PQfsize", res, field_num))
		return 0;
	if (res->attDescs)
		return res->attDescs[field_num].typlen;
	else
		return 0;
}

int
PQfmod(PGresult *res, int field_num)
{
	if (!check_field_number("PQfmod", res, field_num))
		return 0;
	if (res->attDescs)
		return res->attDescs[field_num].atttypmod;
	else
		return 0;
}

char *
PQcmdStatus(PGresult *res)
{
	if (!res)
		return NULL;
	return res->cmdStatus;
}

/*
   PQoidStatus -
	if the last command was an INSERT, return the oid string
	if not, return ""
*/
const char *
PQoidStatus(PGresult *res)
{
	char	   *p,
			   *e,
			   *scan;
	int			slen,
				olen;

	if (!res)
		return "";

	if (strncmp(res->cmdStatus, "INSERT ", 7) != 0)
		return "";

	/*----------
	 * The cmdStatus string looks like
	 *     INSERT oid count\0
	 * In order to be able to return an ordinary C string without
	 * damaging the result for PQcmdStatus or PQcmdTuples, we copy
	 * the oid part of the string to just after the null, so that
	 * cmdStatus looks like
	 *     INSERT oid count\0oid\0
	 *                       ^ our return value points here
	 * Pretty klugy eh?  This routine should've just returned an Oid value.
	 *----------
	 */

	slen = strlen(res->cmdStatus);
	p = res->cmdStatus + 7;		/* where oid is now */
	e = res->cmdStatus + slen + 1;		/* where to put the oid string */

	for (scan = p; *scan && *scan != ' ';)
		scan++;
	olen = scan - p;
	if (slen + olen + 2 > sizeof(res->cmdStatus))
		return "";				/* something very wrong if it doesn't fit */

	strncpy(e, p, olen);
	e[olen] = '\0';

	return e;
}

/*
   PQcmdTuples -
	if the last command was an INSERT/UPDATE/DELETE, return number
	of inserted/affected tuples, if not, return ""
*/
const char *
PQcmdTuples(PGresult *res)
{
	if (!res)
		return "";

	if (strncmp(res->cmdStatus, "INSERT", 6) == 0 ||
		strncmp(res->cmdStatus, "DELETE", 6) == 0 ||
		strncmp(res->cmdStatus, "UPDATE", 6) == 0)
	{
		char	   *p = res->cmdStatus + 6;

		if (*p == 0)
		{
			if (res->conn)
			{
				sprintf(res->conn->errorMessage,
						"PQcmdTuples (%s) -- bad input from server\n",
						res->cmdStatus);
				DONOTICE(res->conn, res->conn->errorMessage);
			}
			return "";
		}
		p++;
		if (*(res->cmdStatus) != 'I')	/* UPDATE/DELETE */
			return p;
		while (*p != ' ' && *p)
			p++;				/* INSERT: skip oid */
		if (*p == 0)
		{
			if (res->conn)
			{
				sprintf(res->conn->errorMessage,
						"PQcmdTuples (INSERT) -- there's no # of tuples\n");
				DONOTICE(res->conn, res->conn->errorMessage);
			}
			return "";
		}
		p++;
		return p;
	}
	return "";
}

/*
   PQgetvalue:
	return the value of field 'field_num' of row 'tup_num'

	If res is binary, then the value returned is NOT a null-terminated
	ASCII string, but the binary representation in the server's native
	format.

	if res is not binary, a null-terminated ASCII string is returned.
*/
char *
PQgetvalue(PGresult *res, int tup_num, int field_num)
{
	if (!check_tuple_field_number("PQgetvalue", res, tup_num, field_num))
		return NULL;
	return res->tuples[tup_num][field_num].value;
}

/* PQgetlength:
	 returns the length of a field value in bytes.	If res is binary,
	 i.e. a result of a binary portal, then the length returned does
	 NOT include the size field of the varlena.  (The data returned
	 by PQgetvalue doesn't either.)
*/
int
PQgetlength(PGresult *res, int tup_num, int field_num)
{
	if (!check_tuple_field_number("PQgetlength", res, tup_num, field_num))
		return 0;
	if (res->tuples[tup_num][field_num].len != NULL_LEN)
		return res->tuples[tup_num][field_num].len;
	else
		return 0;
}

/* PQgetisnull:
	 returns the null status of a field value.
*/
int
PQgetisnull(PGresult *res, int tup_num, int field_num)
{
	if (!check_tuple_field_number("PQgetisnull", res, tup_num, field_num))
		return 1;				/* pretend it is null */
	if (res->tuples[tup_num][field_num].len == NULL_LEN)
		return 1;
	else
		return 0;
}
