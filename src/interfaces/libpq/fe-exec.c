/*-------------------------------------------------------------------------
 *
 * fe-exec.c--
 *	  functions related to sending a query down to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-exec.c,v 1.61 1998/08/09 02:59:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifdef WIN32
#include "win32.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#if !defined(NO_UNISTD_H)
#include <unistd.h>
#endif
#include "postgres.h"
#include "libpq/pqcomm.h"
#include "libpq-fe.h"


/* the rows array in a PGresGroup  has to grow to accommodate the rows */
/* returned.  Each time, we grow by this much: */
#define TUPARR_GROW_BY 100

/* keep this in same order as ExecStatusType in libpq-fe.h */
const char *pgresStatus[] = {
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


static PGresult *makeEmptyPGresult(PGconn *conn, ExecStatusType status);
static void freeTuple(PGresAttValue *tuple, int numAttributes);
static void addTuple(PGresult *res, PGresAttValue *tup);
static void parseInput(PGconn *conn);
static int getRowDescriptions(PGconn *conn);
static int getAnotherTuple(PGconn *conn, int binary);
static int getNotify(PGconn *conn);
static int getNotice(PGconn *conn);


/*
 * PGresult -
 *	 returns a newly allocated, initialized PGresult
 *
 */

static PGresult *
makeEmptyPGresult(PGconn *conn, ExecStatusType status)
{
	PGresult   *result;

	result = (PGresult *) malloc(sizeof(PGresult));

	result->conn = conn;
	result->ntups = 0;
	result->numAttributes = 0;
	result->attDescs = NULL;
	result->tuples = NULL;
	result->tupArrSize = 0;
	result->resultStatus = status;
	result->cmdStatus[0] = '\0';
	result->binary = 0;
	return result;
}

/*
 * PQclear -
 *	  free's the memory associated with a PGresult
 *
 */
void
PQclear(PGresult *res)
{
	int			i;

	if (!res)
		return;

	/* free all the rows */
	if (res->tuples)
	{
		for (i = 0; i < res->ntups; i++)
			freeTuple(res->tuples[i], res->numAttributes);
		free(res->tuples);
	}

	/* free all the attributes */
	if (res->attDescs)
	{
		for (i = 0; i < res->numAttributes; i++)
		{
			if (res->attDescs[i].name)
				free(res->attDescs[i].name);
		}
		free(res->attDescs);
	}

	/* free the structure itself */
	free(res);
}

/*
 * Free a single tuple structure.
 */

static void
freeTuple(PGresAttValue *tuple, int numAttributes)
{
	int		i;

	if (tuple)
	{
		for (i = 0; i < numAttributes; i++)
		{
			if (tuple[i].value)
				free(tuple[i].value);
		}
		free(tuple);
	}
}

/*
 * Handy subroutine to deallocate any partially constructed async result.
 */

void
PQclearAsyncResult(PGconn *conn)
{
	/* Get rid of incomplete result and any not-yet-added tuple */
	if (conn->result)
	{
		if (conn->curTuple)
			freeTuple(conn->curTuple, conn->result->numAttributes);
		PQclear(conn->result);
	}
	conn->result = NULL;
	conn->curTuple = NULL;
}


/*
 * addTuple
 *	  add a row to the PGresult structure, growing it if necessary
 */
static void
addTuple(PGresult *res, PGresAttValue *tup)
{
	if (res->ntups >= res->tupArrSize)
	{
		/* grow the array */
		res->tupArrSize += TUPARR_GROW_BY;
		/*
		 * we can use realloc because shallow copying of the structure
		 * is okay.  Note that the first time through, res->tuples is NULL.
		 * realloc is supposed to do the right thing in that case.
		 * Also note that the positions beyond res->ntups are garbage,
		 * not necessarily NULL.
		 */
		res->tuples = (PGresAttValue **)
			realloc(res->tuples, res->tupArrSize * sizeof(PGresAttValue *));
	}
	res->tuples[res->ntups] = tup;
	res->ntups++;
}


/*
 * PQsendQuery
 *   Submit a query, but don't wait for it to finish
 *
 * Returns: 1 if successfully submitted
 *          0 if error (conn->errorMessage is set)
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
	if (strlen(query) > MAX_MESSAGE_LEN-2)
	{
		sprintf(conn->errorMessage, "PQsendQuery() -- query is too long.  "
				"Maximum length is %d\n", MAX_MESSAGE_LEN - 2);
		return 0;
	}

	if (conn->asyncStatus != PGASYNC_IDLE)
	{
		sprintf(conn->errorMessage,
				"PQsendQuery() -- another query already in progress.");
		return 0;
	}

	/* Check for pending input (asynchronous Notice or Notify messages);
	 * also detect the case that the backend just closed the connection.
	 * Note: we have to loop if the first call to pqReadData successfully
	 * reads some data, since in that case pqReadData won't notice whether
	 * the connection is now closed.
	 */
	while (pqReadReady(conn)) {
		if (pqReadData(conn) < 0)
			return 0;			/* errorMessage already set */
		parseInput(conn);		/* deal with Notice or Notify, if any */
	}

	/* Don't try to send if we know there's no live connection. */
	if (conn->status != CONNECTION_OK)
	{
		sprintf(conn->errorMessage, "PQsendQuery() -- There is no connection "
				"to the backend.\n");
		return 0;
	}

	/* clear the error string */
	conn->errorMessage[0] = '\0';

	/* initialize async result-accumulation state */
	conn->result = NULL;
	conn->curTuple = NULL;
	conn->asyncErrorMessage[0] = '\0';

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
 */

void
PQconsumeInput(PGconn *conn)
{
	if (!conn)
		return;

	/* Load more data, if available.
	 * We do this no matter what state we are in, since we are probably
	 * getting called because the application wants to get rid
	 * of a read-select condition.
	 * Note that we will NOT block waiting for more input.
	 */
	if (pqReadData(conn) < 0)
		strcpy(conn->asyncErrorMessage, conn->errorMessage);
	/* Parsing of the data waits till later. */
}


/*
 * parseInput: if appropriate, parse input data from backend
 * until input is exhausted or a stopping state is reached.
 * Note that this function will NOT attempt to read more data from the backend.
 */

static void
parseInput(PGconn *conn)
{
	char	id;

	/* 
	 * Loop to parse successive complete messages available in the buffer.
	 */
	for (;;)
	{
		/* 
		 * Quit if in COPY_OUT state: we expect raw data from the server until
		 * PQendcopy is called.  Don't try to parse it according to the normal
		 * protocol.  (This is bogus.  The data lines ought to be part of the
		 * protocol and have identifying leading characters.)
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
		 * NOTIFY and NOTICE messages can happen in any state besides COPY OUT;
		 * always process them right away.
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
			 * Other messages should only be processed while in BUSY state.
			 * (In particular, in READY state we hold off further parsing
			 * until the application collects the current PGresult.)
			 * If the state is IDLE then we got trouble.
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
						conn->result = makeEmptyPGresult(conn,
														 PGRES_COMMAND_OK);
					if (pqGets(conn->result->cmdStatus, CMDSTATUS_LEN, conn))
						return;
					conn->asyncStatus = PGASYNC_READY;
					break;
				case 'E':		/* error return */
					if (pqGets(conn->asyncErrorMessage,ERROR_MSG_LENGTH,conn))
						return;
					/* delete any partially constructed result */
					PQclearAsyncResult(conn);
					/* we leave result NULL while setting asyncStatus=READY;
					 * this signals an error condition to PQgetResult.
					 */
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
						conn->result = makeEmptyPGresult(conn,
														 PGRES_EMPTY_QUERY);
					conn->asyncStatus = PGASYNC_READY;
					break;
				case 'K':		/* secret key data from the backend */
					/* This is expected only during backend startup,
					 * but it's just as easy to handle it as part of the
					 * main loop.  Save the data and continue processing.
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
				case 'T':		/* row descriptions (start of query results) */
					if (conn->result == NULL)
					{
						/* First 'T' in a query sequence */
						if (getRowDescriptions(conn))
							return;
					}
					else
					{
						/* A new 'T' message is treated as the start of
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
					sprintf(conn->asyncErrorMessage,
							"unknown protocol character '%c' read from backend.  "
							"(The protocol character is the first character the "
							"backend sends in response to a query it receives).\n",
							id);
					/* Discard the unexpected message; good idea?? */
					conn->inStart = conn->inEnd;
					/* delete any partially constructed result */
					PQclearAsyncResult(conn);
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

	result = makeEmptyPGresult(conn, PGRES_TUPLES_OK);

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
			malloc(nfields * sizeof(PGresAttDesc));
		MemSet((char *) result->attDescs, 0, nfields * sizeof(PGresAttDesc));
	}

	/* get type info */
	for (i = 0; i < nfields; i++)
	{
		char		typName[MAX_MESSAGE_LEN];
		int			typid;
		int			typlen;
		int			atttypmod = -1;

		if (pqGets(typName, MAX_MESSAGE_LEN, conn) ||
			pqGetInt(&typid, 4, conn) ||
			pqGetInt(&typlen, 2, conn) ||
			pqGetInt(&atttypmod, 4, conn))
		{
			PQclear(result);
			return EOF;
		}
		result->attDescs[i].name = strdup(typName);
		result->attDescs[i].typid = typid;
		result->attDescs[i].typlen = (short) typlen;
		result->attDescs[i].atttypmod = atttypmod;
	}

	/* Success! */
	conn->result = result;
	return 0;
}

/*
 * parseInput subroutine to read a 'B' or 'D' (row data) message.
 * We add another tuple to the existing PGresult structure.
 * Returns: 0 if completed message, EOF if not enough data yet.
 *
 * Note that if we run out of data, we have to suspend and reprocess
 * the message after more data is received.  We keep a partially constructed
 * tuple in conn->curTuple, and avoid reallocating already-allocated storage.
 */

static int
getAnotherTuple(PGconn *conn, int binary)
{
	int			nfields = conn->result->numAttributes;
	PGresAttValue *tup;
	char		bitmap[MAX_FIELDS];		/* the backend sends us a bitmap
										 * of which attributes are null */
	int			i;
	int			nbytes;			/* the number of bytes in bitmap  */
	char		bmap;			/* One byte of the bitmap */
	int			bitmap_index;	/* Its index */
	int			bitcnt;			/* number of bits examined in current byte */
	int			vlen;			/* length of the current field value */

	conn->result->binary = binary;

	/* Allocate tuple space if first time for this data message */
	if (conn->curTuple == NULL)
	{
		conn->curTuple = (PGresAttValue *)
			malloc(nfields * sizeof(PGresAttValue));
		MemSet((char *) conn->curTuple, 0, nfields * sizeof(PGresAttValue));
	}
	tup = conn->curTuple;

	/* Get the null-value bitmap */
	nbytes = (nfields + BYTELEN-1) / BYTELEN;
	if (nbytes >= MAX_FIELDS)
	{
		sprintf(conn->asyncErrorMessage,
				"getAnotherTuple() -- null-values bitmap is too large\n");
		PQclearAsyncResult(conn);
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
			if (tup[i].value == NULL)
				tup[i].value = strdup("");
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
				tup[i].value = (char *) malloc(vlen + 1);
			tup[i].len = vlen;
			/* read in the value */
			if (vlen > 0)
				if (pqGetnchar((char *) (tup[i].value), vlen, conn))
					return EOF;
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
	addTuple(conn->result, tup);
	/* and reset for a new message */
	conn->curTuple = NULL;
	return 0;
}


/*
 * PQisBusy
 *   Return TRUE if PQgetResult would block waiting for input.
 */

int
PQisBusy(PGconn *conn)
{
	if (!conn)
		return FALSE;

	/* Parse any available data, if our state permits. */
	parseInput(conn);

	/* PQgetResult will return immediately in all states except BUSY. */
	return (conn->asyncStatus == PGASYNC_BUSY);
}


/*
 * PQgetResult
 *    Get the next PGresult produced by a query.
 *    Returns NULL if and only if no query work remains.
 */

PGresult *
PQgetResult(PGconn *conn)
{
	PGresult	*res;

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
			PQclearAsyncResult(conn);
			conn->asyncStatus = PGASYNC_IDLE;
			/* conn->errorMessage has been set by pqWait or pqReadData. */
			return makeEmptyPGresult(conn, PGRES_FATAL_ERROR);
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
			 * conn->result is the PGresult to return, or possibly NULL
			 * indicating an error.
			 * conn->asyncErrorMessage holds the errorMessage to return.
			 * (We keep it stashed there so that other user calls can't
			 * overwrite it prematurely.)
			 */
			res = conn->result;
			conn->result = NULL; /* handing over ownership to caller */
			conn->curTuple = NULL; /* just in case */
			if (!res)
				res = makeEmptyPGresult(conn, PGRES_FATAL_ERROR);
			strcpy(conn->errorMessage, conn->asyncErrorMessage);
			/* Set the state back to BUSY, allowing parsing to proceed. */
			conn->asyncStatus = PGASYNC_BUSY;
			break;
		case PGASYNC_COPY_IN:
			res = makeEmptyPGresult(conn, PGRES_COPY_IN);
			break;
		case PGASYNC_COPY_OUT:
			res = makeEmptyPGresult(conn, PGRES_COPY_OUT);
			break;
		default:
			sprintf(conn->errorMessage,
					"PQgetResult: Unexpected asyncStatus %d\n",
					(int) conn->asyncStatus);
			res = makeEmptyPGresult(conn, PGRES_FATAL_ERROR);
			break;
	}

	return res;
}


/*
 * PQexec
 *	  send a query to the backend and package up the result in a PGresult
 *
 * if the query failed, return NULL, conn->errorMessage is set to
 * a relevant message
 * if query is successful, a new PGresult is returned
 * the user is responsible for freeing that structure when done with it
 *
 */

PGresult   *
PQexec(PGconn *conn, const char *query)
{
	PGresult	*result;
	PGresult	*lastResult;

	/* Silently discard any prior query result that application didn't eat.
	 * This is probably poor design, but it's here for backward compatibility.
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
	if (! PQsendQuery(conn, query))
		return NULL;

	/* For backwards compatibility, return the last result if there are
	 * more than one.  We have to stop if we see copy in/out, however.
	 * We will resume parsing when application calls PQendcopy.
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
 *       returns EOF if not enough data.
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
 *       returns EOF if not enough data.
 */
static int
getNotify(PGconn *conn)
{
	PGnotify   tempNotify;
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

	/* Since this is a purely synchronous routine, we don't bother to
	 * maintain conn->inCursor; there is no need to back up.
	 */
	while (maxlen > 1)
	{
		if (conn->inStart < conn->inEnd)
		{
			char c = conn->inBuffer[conn->inStart++];
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
 * PQputline -- sends a string to the backend.
 *
 * Chiefly here so that applications can use "COPY <rel> from stdin".
 */
void
PQputline(PGconn *conn, const char *s)
{
	if (conn && conn->sock >= 0)
		(void) pqPutnchar(s, strlen(s), conn);
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
	PGresult	*result;

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

	/* Wait for the completion response */
	result = PQgetResult(conn);

	/* Expecting a successful result */
	if (result && result->resultStatus == PGRES_COMMAND_OK)
	{
		PQclear(result);
		return 0;
	}

	/* Trouble.
	 * The worst case is that we've lost sync with the backend entirely
	 * due to application screwup of the copy in/out protocol.
	 * To recover, reset the connection (talk about using a sledgehammer...)
	 */
	PQclear(result);

	sprintf(conn->errorMessage, "PQendcopy: resetting connection\n");
	DONOTICE(conn, conn->errorMessage);

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
 *      PGresult with status = PGRES_COMMAND_OK if successful.
 *			*actual_result_len is > 0 if there is a return value, 0 if not.
 *      PGresult with status = PGRES_FATAL_ERROR if backend returns an error.
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
	ExecStatusType	status = PGRES_FATAL_ERROR;
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
	if (pqPutInt(fnid, 4, conn)) /* function id */
		return NULL;
	if (pqPutInt(nargs, 4, conn)) /* # of args */
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
		/* Scan the message.
		 * If we run out of data, loop around to try again.
		 */
		conn->inCursor = conn->inStart;
		needInput = true;

		if (pqGetc(&id, conn))
			continue;

		/* We should see V or E response to the command,
		 * but might get N and/or A notices first.
		 * We also need to swallow the final Z before returning.
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
					if (pqGetc(&id, conn)) /* get the last '0' */
						continue;
				}
				if (id == '0')
				{
					/* correctly finished function result message */
					status = PGRES_COMMAND_OK;
				}
				else {
					/* The backend violates the protocol. */
					sprintf(conn->errorMessage,
							"FATAL: PQfn: protocol error: id=%x\n", id);
					conn->inStart = conn->inCursor;
					return makeEmptyPGresult(conn, PGRES_FATAL_ERROR);
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
				return makeEmptyPGresult(conn, status);
			default:
				/* The backend violates the protocol. */
				sprintf(conn->errorMessage,
						"FATAL: PQfn: protocol error: id=%x\n", id);
				conn->inStart = conn->inCursor;
				return makeEmptyPGresult(conn, PGRES_FATAL_ERROR);
		}
		/* Completed this message, keep going */
		conn->inStart = conn->inCursor;
		needInput = false;
	}

	/* we fall out of the loop only upon failing to read data */
	return makeEmptyPGresult(conn, PGRES_FATAL_ERROR);
}


/* ====== accessor funcs for PGresult ======== */

ExecStatusType
PQresultStatus(PGresult *res)
{
	if (!res)
		return PGRES_NONFATAL_ERROR;
	return res->resultStatus;
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
		sprintf(res->conn->errorMessage,
				"%s: ERROR! field number %d is out of range 0..%d\n",
				routineName, field_num, res->numAttributes - 1);
		DONOTICE(res->conn, res->conn->errorMessage);
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
		sprintf(res->conn->errorMessage,
				"%s: ERROR! tuple number %d is out of range 0..%d\n",
				routineName, tup_num, res->ntups - 1);
		DONOTICE(res->conn, res->conn->errorMessage);
		return FALSE;
	}
	if (field_num < 0 || field_num >= res->numAttributes)
	{
		sprintf(res->conn->errorMessage,
				"%s: ERROR! field number %d is out of range 0..%d\n",
				routineName, field_num, res->numAttributes - 1);
		DONOTICE(res->conn, res->conn->errorMessage);
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
	if (! check_field_number("PQfname", res, field_num))
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
			if (isascii((unsigned char)field_case[i]) &&
			    isupper(field_case[i]))
				field_case[i] = tolower(field_case[i]);

	for (i = 0; i < res->numAttributes; i++)
	{
		if (strcmp(field_name, res->attDescs[i].name) == 0)
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
	if (! check_field_number("PQftype", res, field_num))
		return InvalidOid;
	if (res->attDescs)
		return res->attDescs[field_num].typid;
	else
		return InvalidOid;
}

short
PQfsize(PGresult *res, int field_num)
{
	if (! check_field_number("PQfsize", res, field_num))
		return 0;
	if (res->attDescs)
		return res->attDescs[field_num].typlen;
	else
		return 0;
}

int
PQfmod(PGresult *res, int field_num)
{
	if (! check_field_number("PQfmod", res, field_num))
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
	static char oidStatus[32] = {0};

	if (!res)
		return "";

	oidStatus[0] = 0;

	if (strncmp(res->cmdStatus, "INSERT ", 7) == 0)
	{
		char	   *p = res->cmdStatus + 7;
		char	   *e;

		for (e = p; *e != ' ' && *e;)
			e++;
		sprintf(oidStatus, "%.*s", e - p, p);
	}
	return oidStatus;
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
			sprintf(res->conn->errorMessage,
					"PQcmdTuples (%s) -- bad input from server\n",
					res->cmdStatus);
			DONOTICE(res->conn, res->conn->errorMessage);
			return "";
		}
		p++;
		if (*(res->cmdStatus) != 'I')	/* UPDATE/DELETE */
			return (p);
		while (*p != ' ' && *p)
			p++;				/* INSERT: skip oid */
		if (*p == 0)
		{
			sprintf(res->conn->errorMessage,
					"PQcmdTuples (INSERT) -- there's no # of tuples\n");
			DONOTICE(res->conn, res->conn->errorMessage);
			return "";
		}
		p++;
		return (p);
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
	if (! check_tuple_field_number("PQgetvalue", res, tup_num, field_num))
		return NULL;
	return res->tuples[tup_num][field_num].value;
}

/* PQgetlength:
	 returns the length of a field value in bytes.	If res is binary,
	 i.e. a result of a binary portal, then the length returned does
	 NOT include the size field of the varlena.
*/
int
PQgetlength(PGresult *res, int tup_num, int field_num)
{
	if (! check_tuple_field_number("PQgetlength", res, tup_num, field_num))
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
	if (! check_tuple_field_number("PQgetisnull", res, tup_num, field_num))
		return 1;				/* pretend it is null */
	if (res->tuples[tup_num][field_num].len == NULL_LEN)
		return 1;
	else
		return 0;
}
