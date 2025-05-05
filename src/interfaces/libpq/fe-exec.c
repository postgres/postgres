/*-------------------------------------------------------------------------
 *
 * fe-exec.c
 *	  functions related to sending a query down to the backend
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-exec.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#endif

#include "libpq-fe.h"
#include "libpq-int.h"
#include "mb/pg_wchar.h"

/* keep this in same order as ExecStatusType in libpq-fe.h */
char	   *const pgresStatus[] = {
	"PGRES_EMPTY_QUERY",
	"PGRES_COMMAND_OK",
	"PGRES_TUPLES_OK",
	"PGRES_COPY_OUT",
	"PGRES_COPY_IN",
	"PGRES_BAD_RESPONSE",
	"PGRES_NONFATAL_ERROR",
	"PGRES_FATAL_ERROR",
	"PGRES_COPY_BOTH",
	"PGRES_SINGLE_TUPLE",
	"PGRES_PIPELINE_SYNC",
	"PGRES_PIPELINE_ABORTED",
	"PGRES_TUPLES_CHUNK"
};

/* We return this if we're unable to make a PGresult at all */
static const PGresult OOM_result = {
	.resultStatus = PGRES_FATAL_ERROR,
	.client_encoding = PG_SQL_ASCII,
	.errMsg = "out of memory\n",
};

/*
 * static state needed by PQescapeString and PQescapeBytea; initialize to
 * values that result in backward-compatible behavior
 */
static int	static_client_encoding = PG_SQL_ASCII;
static bool static_std_strings = false;


static PGEvent *dupEvents(PGEvent *events, int count, size_t *memSize);
static bool pqAddTuple(PGresult *res, PGresAttValue *tup,
					   const char **errmsgp);
static int	PQsendQueryInternal(PGconn *conn, const char *query, bool newQuery);
static bool PQsendQueryStart(PGconn *conn, bool newQuery);
static int	PQsendQueryGuts(PGconn *conn,
							const char *command,
							const char *stmtName,
							int nParams,
							const Oid *paramTypes,
							const char *const *paramValues,
							const int *paramLengths,
							const int *paramFormats,
							int resultFormat);
static void parseInput(PGconn *conn);
static PGresult *getCopyResult(PGconn *conn, ExecStatusType copytype);
static bool PQexecStart(PGconn *conn);
static PGresult *PQexecFinish(PGconn *conn);
static int	PQsendTypedCommand(PGconn *conn, char command, char type,
							   const char *target);
static int	check_field_number(const PGresult *res, int field_num);
static void pqPipelineProcessQueue(PGconn *conn);
static int	pqPipelineSyncInternal(PGconn *conn, bool immediate_flush);
static int	pqPipelineFlush(PGconn *conn);


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
 *
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
 *
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
 *	 blocks, instead of being crammed into a regular allocation block.
 * Requirements for correct function are:
 * PGRESULT_ALIGN_BOUNDARY must be a multiple of the alignment requirements
 *		of all machine data types.  (Currently this is set from configure
 *		tests, so it should be OK automatically.)
 * PGRESULT_SEP_ALLOC_THRESHOLD + PGRESULT_BLOCK_OVERHEAD <=
 *			PGRESULT_DATA_BLOCKSIZE
 *		pqResultAlloc assumes an object smaller than the threshold will fit
 *		in a new block.
 * The amount of space wasted at the end of a block could be as much as
 * PGRESULT_SEP_ALLOC_THRESHOLD, so it doesn't pay to make that too large.
 * ----------------
 */

#define PGRESULT_DATA_BLOCKSIZE		2048
#define PGRESULT_ALIGN_BOUNDARY		MAXIMUM_ALIGNOF /* from configure */
#define PGRESULT_BLOCK_OVERHEAD		Max(sizeof(PGresult_data), PGRESULT_ALIGN_BOUNDARY)
#define PGRESULT_SEP_ALLOC_THRESHOLD	(PGRESULT_DATA_BLOCKSIZE / 2)


/*
 * PQmakeEmptyPGresult
 *	 returns a newly allocated, initialized PGresult with given status.
 *	 If conn is not NULL and status indicates an error, the conn's
 *	 errorMessage is copied.  Also, any PGEvents are copied from the conn.
 *
 * Note: the logic to copy the conn's errorMessage is now vestigial;
 * no internal caller uses it.  However, that behavior is documented for
 * outside callers, so we'd better keep it.
 */
PGresult *
PQmakeEmptyPGresult(PGconn *conn, ExecStatusType status)
{
	PGresult   *result;

	result = (PGresult *) malloc(sizeof(PGresult));
	if (!result)
		return NULL;

	result->ntups = 0;
	result->numAttributes = 0;
	result->attDescs = NULL;
	result->tuples = NULL;
	result->tupArrSize = 0;
	result->numParameters = 0;
	result->paramDescs = NULL;
	result->resultStatus = status;
	result->cmdStatus[0] = '\0';
	result->binary = 0;
	result->events = NULL;
	result->nEvents = 0;
	result->errMsg = NULL;
	result->errFields = NULL;
	result->errQuery = NULL;
	result->null_field[0] = '\0';
	result->curBlock = NULL;
	result->curOffset = 0;
	result->spaceLeft = 0;
	result->memorySize = sizeof(PGresult);

	if (conn)
	{
		/* copy connection data we might need for operations on PGresult */
		result->noticeHooks = conn->noticeHooks;
		result->client_encoding = conn->client_encoding;

		/* consider copying conn's errorMessage */
		switch (status)
		{
			case PGRES_EMPTY_QUERY:
			case PGRES_COMMAND_OK:
			case PGRES_TUPLES_OK:
			case PGRES_COPY_OUT:
			case PGRES_COPY_IN:
			case PGRES_COPY_BOTH:
			case PGRES_SINGLE_TUPLE:
			case PGRES_TUPLES_CHUNK:
				/* non-error cases */
				break;
			default:
				/* we intentionally do not use or modify errorReported here */
				pqSetResultError(result, &conn->errorMessage, 0);
				break;
		}

		/* copy events last; result must be valid if we need to PQclear */
		if (conn->nEvents > 0)
		{
			result->events = dupEvents(conn->events, conn->nEvents,
									   &result->memorySize);
			if (!result->events)
			{
				PQclear(result);
				return NULL;
			}
			result->nEvents = conn->nEvents;
		}
	}
	else
	{
		/* defaults... */
		result->noticeHooks.noticeRec = NULL;
		result->noticeHooks.noticeRecArg = NULL;
		result->noticeHooks.noticeProc = NULL;
		result->noticeHooks.noticeProcArg = NULL;
		result->client_encoding = PG_SQL_ASCII;
	}

	return result;
}

/*
 * PQsetResultAttrs
 *
 * Set the attributes for a given result.  This function fails if there are
 * already attributes contained in the provided result.  The call is
 * ignored if numAttributes is zero or attDescs is NULL.  If the
 * function fails, it returns zero.  If the function succeeds, it
 * returns a non-zero value.
 */
int
PQsetResultAttrs(PGresult *res, int numAttributes, PGresAttDesc *attDescs)
{
	int			i;

	/* Fail if argument is NULL or OOM_result */
	if (!res || (const PGresult *) res == &OOM_result)
		return false;

	/* If attrs already exist, they cannot be overwritten. */
	if (res->numAttributes > 0)
		return false;

	/* ignore no-op request */
	if (numAttributes <= 0 || !attDescs)
		return true;

	res->attDescs = (PGresAttDesc *)
		PQresultAlloc(res, numAttributes * sizeof(PGresAttDesc));

	if (!res->attDescs)
		return false;

	res->numAttributes = numAttributes;
	memcpy(res->attDescs, attDescs, numAttributes * sizeof(PGresAttDesc));

	/* deep-copy the attribute names, and determine format */
	res->binary = 1;
	for (i = 0; i < res->numAttributes; i++)
	{
		if (res->attDescs[i].name)
			res->attDescs[i].name = pqResultStrdup(res, res->attDescs[i].name);
		else
			res->attDescs[i].name = res->null_field;

		if (!res->attDescs[i].name)
			return false;

		if (res->attDescs[i].format == 0)
			res->binary = 0;
	}

	return true;
}

/*
 * PQcopyResult
 *
 * Returns a deep copy of the provided 'src' PGresult, which cannot be NULL.
 * The 'flags' argument controls which portions of the result will or will
 * NOT be copied.  The created result is always put into the
 * PGRES_TUPLES_OK status.  The source result error message is not copied,
 * although cmdStatus is.
 *
 * To set custom attributes, use PQsetResultAttrs.  That function requires
 * that there are no attrs contained in the result, so to use that
 * function you cannot use the PG_COPYRES_ATTRS or PG_COPYRES_TUPLES
 * options with this function.
 *
 * Options:
 *	 PG_COPYRES_ATTRS - Copy the source result's attributes
 *
 *	 PG_COPYRES_TUPLES - Copy the source result's tuples.  This implies
 *	 copying the attrs, seeing how the attrs are needed by the tuples.
 *
 *	 PG_COPYRES_EVENTS - Copy the source result's events.
 *
 *	 PG_COPYRES_NOTICEHOOKS - Copy the source result's notice hooks.
 */
PGresult *
PQcopyResult(const PGresult *src, int flags)
{
	PGresult   *dest;
	int			i;

	if (!src)
		return NULL;

	dest = PQmakeEmptyPGresult(NULL, PGRES_TUPLES_OK);
	if (!dest)
		return NULL;

	/* Always copy these over.  Is cmdStatus really useful here? */
	dest->client_encoding = src->client_encoding;
	strcpy(dest->cmdStatus, src->cmdStatus);

	/* Wants attrs? */
	if (flags & (PG_COPYRES_ATTRS | PG_COPYRES_TUPLES))
	{
		if (!PQsetResultAttrs(dest, src->numAttributes, src->attDescs))
		{
			PQclear(dest);
			return NULL;
		}
	}

	/* Wants to copy tuples? */
	if (flags & PG_COPYRES_TUPLES)
	{
		int			tup,
					field;

		for (tup = 0; tup < src->ntups; tup++)
		{
			for (field = 0; field < src->numAttributes; field++)
			{
				if (!PQsetvalue(dest, tup, field,
								src->tuples[tup][field].value,
								src->tuples[tup][field].len))
				{
					PQclear(dest);
					return NULL;
				}
			}
		}
	}

	/* Wants to copy notice hooks? */
	if (flags & PG_COPYRES_NOTICEHOOKS)
		dest->noticeHooks = src->noticeHooks;

	/* Wants to copy PGEvents? */
	if ((flags & PG_COPYRES_EVENTS) && src->nEvents > 0)
	{
		dest->events = dupEvents(src->events, src->nEvents,
								 &dest->memorySize);
		if (!dest->events)
		{
			PQclear(dest);
			return NULL;
		}
		dest->nEvents = src->nEvents;
	}

	/* Okay, trigger PGEVT_RESULTCOPY event */
	for (i = 0; i < dest->nEvents; i++)
	{
		/* We don't fire events that had some previous failure */
		if (src->events[i].resultInitialized)
		{
			PGEventResultCopy evt;

			evt.src = src;
			evt.dest = dest;
			if (dest->events[i].proc(PGEVT_RESULTCOPY, &evt,
									 dest->events[i].passThrough))
				dest->events[i].resultInitialized = true;
		}
	}

	return dest;
}

/*
 * Copy an array of PGEvents (with no extra space for more).
 * Does not duplicate the event instance data, sets this to NULL.
 * Also, the resultInitialized flags are all cleared.
 * The total space allocated is added to *memSize.
 */
static PGEvent *
dupEvents(PGEvent *events, int count, size_t *memSize)
{
	PGEvent    *newEvents;
	size_t		msize;
	int			i;

	if (!events || count <= 0)
		return NULL;

	msize = count * sizeof(PGEvent);
	newEvents = (PGEvent *) malloc(msize);
	if (!newEvents)
		return NULL;

	for (i = 0; i < count; i++)
	{
		newEvents[i].proc = events[i].proc;
		newEvents[i].passThrough = events[i].passThrough;
		newEvents[i].data = NULL;
		newEvents[i].resultInitialized = false;
		newEvents[i].name = strdup(events[i].name);
		if (!newEvents[i].name)
		{
			while (--i >= 0)
				free(newEvents[i].name);
			free(newEvents);
			return NULL;
		}
		msize += strlen(events[i].name) + 1;
	}

	*memSize += msize;
	return newEvents;
}


/*
 * Sets the value for a tuple field.  The tup_num must be less than or
 * equal to PQntuples(res).  If it is equal, a new tuple is created and
 * added to the result.
 * Returns a non-zero value for success and zero for failure.
 * (On failure, we report the specific problem via pqInternalNotice.)
 */
int
PQsetvalue(PGresult *res, int tup_num, int field_num, char *value, int len)
{
	PGresAttValue *attval;
	const char *errmsg = NULL;

	/* Fail if argument is NULL or OOM_result */
	if (!res || (const PGresult *) res == &OOM_result)
		return false;

	/* Invalid field_num? */
	if (!check_field_number(res, field_num))
		return false;

	/* Invalid tup_num, must be <= ntups */
	if (tup_num < 0 || tup_num > res->ntups)
	{
		pqInternalNotice(&res->noticeHooks,
						 "row number %d is out of range 0..%d",
						 tup_num, res->ntups);
		return false;
	}

	/* need to allocate a new tuple? */
	if (tup_num == res->ntups)
	{
		PGresAttValue *tup;
		int			i;

		tup = (PGresAttValue *)
			pqResultAlloc(res, res->numAttributes * sizeof(PGresAttValue),
						  true);

		if (!tup)
			goto fail;

		/* initialize each column to NULL */
		for (i = 0; i < res->numAttributes; i++)
		{
			tup[i].len = NULL_LEN;
			tup[i].value = res->null_field;
		}

		/* add it to the array */
		if (!pqAddTuple(res, tup, &errmsg))
			goto fail;
	}

	attval = &res->tuples[tup_num][field_num];

	/* treat either NULL_LEN or NULL value pointer as a NULL field */
	if (len == NULL_LEN || value == NULL)
	{
		attval->len = NULL_LEN;
		attval->value = res->null_field;
	}
	else if (len <= 0)
	{
		attval->len = 0;
		attval->value = res->null_field;
	}
	else
	{
		attval->value = (char *) pqResultAlloc(res, len + 1, true);
		if (!attval->value)
			goto fail;
		attval->len = len;
		memcpy(attval->value, value, len);
		attval->value[len] = '\0';
	}

	return true;

	/*
	 * Report failure via pqInternalNotice.  If preceding code didn't provide
	 * an error message, assume "out of memory" was meant.
	 */
fail:
	if (!errmsg)
		errmsg = libpq_gettext("out of memory");
	pqInternalNotice(&res->noticeHooks, "%s", errmsg);

	return false;
}

/*
 * pqResultAlloc - exported routine to allocate local storage in a PGresult.
 *
 * We force all such allocations to be maxaligned, since we don't know
 * whether the value might be binary.
 */
void *
PQresultAlloc(PGresult *res, size_t nBytes)
{
	/* Fail if argument is NULL or OOM_result */
	if (!res || (const PGresult *) res == &OOM_result)
		return NULL;

	return pqResultAlloc(res, nBytes, true);
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
pqResultAlloc(PGresult *res, size_t nBytes, bool isBinary)
{
	char	   *space;
	PGresult_data *block;

	if (!res)
		return NULL;

	if (nBytes <= 0)
		return res->null_field;

	/*
	 * If alignment is needed, round up the current position to an alignment
	 * boundary.
	 */
	if (isBinary)
	{
		int			offset = res->curOffset % PGRESULT_ALIGN_BOUNDARY;

		if (offset)
		{
			res->curOffset += PGRESULT_ALIGN_BOUNDARY - offset;
			res->spaceLeft -= PGRESULT_ALIGN_BOUNDARY - offset;
		}
	}

	/* If there's enough space in the current block, no problem. */
	if (nBytes <= (size_t) res->spaceLeft)
	{
		space = res->curBlock->space + res->curOffset;
		res->curOffset += nBytes;
		res->spaceLeft -= nBytes;
		return space;
	}

	/*
	 * If the requested object is very large, give it its own block; this
	 * avoids wasting what might be most of the current block to start a new
	 * block.  (We'd have to special-case requests bigger than the block size
	 * anyway.)  The object is always given binary alignment in this case.
	 */
	if (nBytes >= PGRESULT_SEP_ALLOC_THRESHOLD)
	{
		size_t		alloc_size = nBytes + PGRESULT_BLOCK_OVERHEAD;

		block = (PGresult_data *) malloc(alloc_size);
		if (!block)
			return NULL;
		res->memorySize += alloc_size;
		space = block->space + PGRESULT_BLOCK_OVERHEAD;
		if (res->curBlock)
		{
			/*
			 * Tuck special block below the active block, so that we don't
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
			res->spaceLeft = 0; /* be sure it's marked full */
		}
		return space;
	}

	/* Otherwise, start a new block. */
	block = (PGresult_data *) malloc(PGRESULT_DATA_BLOCKSIZE);
	if (!block)
		return NULL;
	res->memorySize += PGRESULT_DATA_BLOCKSIZE;
	block->next = res->curBlock;
	res->curBlock = block;
	if (isBinary)
	{
		/* object needs full alignment */
		res->curOffset = PGRESULT_BLOCK_OVERHEAD;
		res->spaceLeft = PGRESULT_DATA_BLOCKSIZE - PGRESULT_BLOCK_OVERHEAD;
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
 * PQresultMemorySize -
 *		Returns total space allocated for the PGresult.
 */
size_t
PQresultMemorySize(const PGresult *res)
{
	if (!res)
		return 0;
	return res->memorySize;
}

/*
 * pqResultStrdup -
 *		Like strdup, but the space is subsidiary PGresult space.
 */
char *
pqResultStrdup(PGresult *res, const char *str)
{
	char	   *space = (char *) pqResultAlloc(res, strlen(str) + 1, false);

	if (space)
		strcpy(space, str);
	return space;
}

/*
 * pqSetResultError -
 *		assign a new error message to a PGresult
 *
 * Copy text from errorMessage buffer beginning at given offset
 * (it's caller's responsibility that offset is valid)
 */
void
pqSetResultError(PGresult *res, PQExpBuffer errorMessage, int offset)
{
	char	   *msg;

	if (!res)
		return;

	/*
	 * We handle two OOM scenarios here.  The errorMessage buffer might be
	 * marked "broken" due to having previously failed to allocate enough
	 * memory for the message, or it might be fine but pqResultStrdup fails
	 * and returns NULL.  In either case, just make res->errMsg point directly
	 * at a constant "out of memory" string.
	 */
	if (!PQExpBufferBroken(errorMessage))
		msg = pqResultStrdup(res, errorMessage->data + offset);
	else
		msg = NULL;
	if (msg)
		res->errMsg = msg;
	else
		res->errMsg = libpq_gettext("out of memory\n");
}

/*
 * PQclear -
 *	  free's the memory associated with a PGresult
 */
void
PQclear(PGresult *res)
{
	PGresult_data *block;
	int			i;

	/* As a convenience, do nothing for a NULL pointer */
	if (!res)
		return;
	/* Also, do nothing if the argument is OOM_result */
	if ((const PGresult *) res == &OOM_result)
		return;

	/* Close down any events we may have */
	for (i = 0; i < res->nEvents; i++)
	{
		/* only send DESTROY to successfully-initialized event procs */
		if (res->events[i].resultInitialized)
		{
			PGEventResultDestroy evt;

			evt.result = res;
			(void) res->events[i].proc(PGEVT_RESULTDESTROY, &evt,
									   res->events[i].passThrough);
		}
		free(res->events[i].name);
	}

	free(res->events);

	/* Free all the subsidiary blocks */
	while ((block = res->curBlock) != NULL)
	{
		res->curBlock = block->next;
		free(block);
	}

	/* Free the top-level tuple pointer array */
	free(res->tuples);

	/* zero out the pointer fields to catch programming errors */
	res->attDescs = NULL;
	res->tuples = NULL;
	res->paramDescs = NULL;
	res->errFields = NULL;
	res->events = NULL;
	res->nEvents = 0;
	/* res->curBlock was zeroed out earlier */

	/* Free the PGresult structure itself */
	free(res);
}

/*
 * Handy subroutine to deallocate any partially constructed async result.
 *
 * Any "saved" result gets cleared too.
 */
void
pqClearAsyncResult(PGconn *conn)
{
	PQclear(conn->result);
	conn->result = NULL;
	conn->error_result = false;
	PQclear(conn->saved_result);
	conn->saved_result = NULL;
}

/*
 * pqSaveErrorResult -
 *	  remember that we have an error condition
 *
 * In much of libpq, reporting an error just requires appending text to
 * conn->errorMessage and returning a failure code to one's caller.
 * Where returning a failure code is impractical, instead call this
 * function to remember that an error needs to be reported.
 *
 * (It might seem that appending text to conn->errorMessage should be
 * sufficient, but we can't rely on that working under out-of-memory
 * conditions.  The OOM hazard is also why we don't try to make a new
 * PGresult right here.)
 */
void
pqSaveErrorResult(PGconn *conn)
{
	/* Drop any pending result ... */
	pqClearAsyncResult(conn);
	/* ... and set flag to remember to make an error result later */
	conn->error_result = true;
}

/*
 * pqSaveWriteError -
 *	  report a write failure
 *
 * As above, after appending conn->write_err_msg to whatever other error we
 * have.  This is used when we've detected a write failure and have exhausted
 * our chances of reporting something else instead.
 */
static void
pqSaveWriteError(PGconn *conn)
{
	/*
	 * If write_err_msg is null because of previous strdup failure, do what we
	 * can.  (It's likely our machinations here will get OOM failures as well,
	 * but might as well try.)
	 */
	if (conn->write_err_msg)
	{
		appendPQExpBufferStr(&conn->errorMessage, conn->write_err_msg);
		/* Avoid possibly appending the same message twice */
		conn->write_err_msg[0] = '\0';
	}
	else
		libpq_append_conn_error(conn, "write to server failed");

	pqSaveErrorResult(conn);
}

/*
 * pqPrepareAsyncResult -
 *	  prepare the current async result object for return to the caller
 *
 * If there is not already an async result object, build an error object
 * using whatever is in conn->errorMessage.  In any case, clear the async
 * result storage, and update our notion of how much error text has been
 * returned to the application.
 *
 * Note that in no case (not even OOM) do we return NULL.
 */
PGresult *
pqPrepareAsyncResult(PGconn *conn)
{
	PGresult   *res;

	res = conn->result;
	if (res)
	{
		/*
		 * If the pre-existing result is an ERROR (presumably something
		 * received from the server), assume that it represents whatever is in
		 * conn->errorMessage, and advance errorReported.
		 */
		if (res->resultStatus == PGRES_FATAL_ERROR)
			conn->errorReported = conn->errorMessage.len;
	}
	else
	{
		/*
		 * We get here after internal-to-libpq errors.  We should probably
		 * always have error_result = true, but if we don't, gin up some error
		 * text.
		 */
		if (!conn->error_result)
			libpq_append_conn_error(conn, "no error text available");

		/* Paranoia: be sure errorReported offset is sane */
		if (conn->errorReported < 0 ||
			conn->errorReported >= conn->errorMessage.len)
			conn->errorReported = 0;

		/*
		 * Make a PGresult struct for the error.  We temporarily lie about the
		 * result status, so that PQmakeEmptyPGresult doesn't uselessly copy
		 * all of conn->errorMessage.
		 */
		res = PQmakeEmptyPGresult(conn, PGRES_EMPTY_QUERY);
		if (res)
		{
			/*
			 * Report whatever new error text we have, and advance
			 * errorReported.
			 */
			res->resultStatus = PGRES_FATAL_ERROR;
			pqSetResultError(res, &conn->errorMessage, conn->errorReported);
			conn->errorReported = conn->errorMessage.len;
		}
		else
		{
			/*
			 * Ouch, not enough memory for a PGresult.  Fortunately, we have a
			 * card up our sleeve: we can use the static OOM_result.  Casting
			 * away const here is a bit ugly, but it seems best to declare
			 * OOM_result as const, in hopes it will be allocated in read-only
			 * storage.
			 */
			res = unconstify(PGresult *, &OOM_result);

			/*
			 * Don't advance errorReported.  Perhaps we'll be able to report
			 * the text later.
			 */
		}
	}

	/*
	 * Replace conn->result with saved_result, if any.  In the normal case
	 * there isn't a saved result and we're just dropping ownership of the
	 * current result.  In partial-result mode this restores the situation to
	 * what it was before we created the current partial result.
	 */
	conn->result = conn->saved_result;
	conn->error_result = false; /* saved_result is never an error */
	conn->saved_result = NULL;

	return res;
}

/*
 * pqInternalNotice - produce an internally-generated notice message
 *
 * A format string and optional arguments can be passed.  Note that we do
 * libpq_gettext() here, so callers need not.
 *
 * The supplied text is taken as primary message (ie., it should not include
 * a trailing newline, and should not be more than one line).
 */
void
pqInternalNotice(const PGNoticeHooks *hooks, const char *fmt,...)
{
	char		msgBuf[1024];
	va_list		args;
	PGresult   *res;

	if (hooks->noticeRec == NULL)
		return;					/* nobody home to receive notice? */

	/* Format the message */
	va_start(args, fmt);
	vsnprintf(msgBuf, sizeof(msgBuf), libpq_gettext(fmt), args);
	va_end(args);
	msgBuf[sizeof(msgBuf) - 1] = '\0';	/* make real sure it's terminated */

	/* Make a PGresult to pass to the notice receiver */
	res = PQmakeEmptyPGresult(NULL, PGRES_NONFATAL_ERROR);
	if (!res)
		return;
	res->noticeHooks = *hooks;

	/*
	 * Set up fields of notice.
	 */
	pqSaveMessageField(res, PG_DIAG_MESSAGE_PRIMARY, msgBuf);
	pqSaveMessageField(res, PG_DIAG_SEVERITY, libpq_gettext("NOTICE"));
	pqSaveMessageField(res, PG_DIAG_SEVERITY_NONLOCALIZED, "NOTICE");
	/* XXX should provide a SQLSTATE too? */

	/*
	 * Result text is always just the primary message + newline.  If we can't
	 * allocate it, substitute "out of memory", as in pqSetResultError.
	 */
	res->errMsg = (char *) pqResultAlloc(res, strlen(msgBuf) + 2, false);
	if (res->errMsg)
		sprintf(res->errMsg, "%s\n", msgBuf);
	else
		res->errMsg = libpq_gettext("out of memory\n");

	/*
	 * Pass to receiver, then free it.
	 */
	res->noticeHooks.noticeRec(res->noticeHooks.noticeRecArg, res);
	PQclear(res);
}

/*
 * pqAddTuple
 *	  add a row pointer to the PGresult structure, growing it if necessary
 *	  Returns true if OK, false if an error prevented adding the row
 *
 * On error, *errmsgp can be set to an error string to be returned.
 * If it is left NULL, the error is presumed to be "out of memory".
 */
static bool
pqAddTuple(PGresult *res, PGresAttValue *tup, const char **errmsgp)
{
	if (res->ntups >= res->tupArrSize)
	{
		/*
		 * Try to grow the array.
		 *
		 * We can use realloc because shallow copying of the structure is
		 * okay. Note that the first time through, res->tuples is NULL. While
		 * ANSI says that realloc() should act like malloc() in that case,
		 * some old C libraries (like SunOS 4.1.x) coredump instead. On
		 * failure realloc is supposed to return NULL without damaging the
		 * existing allocation. Note that the positions beyond res->ntups are
		 * garbage, not necessarily NULL.
		 */
		int			newSize;
		PGresAttValue **newTuples;

		/*
		 * Since we use integers for row numbers, we can't support more than
		 * INT_MAX rows.  Make sure we allow that many, though.
		 */
		if (res->tupArrSize <= INT_MAX / 2)
			newSize = (res->tupArrSize > 0) ? res->tupArrSize * 2 : 128;
		else if (res->tupArrSize < INT_MAX)
			newSize = INT_MAX;
		else
		{
			*errmsgp = libpq_gettext("PGresult cannot support more than INT_MAX tuples");
			return false;
		}

		/*
		 * Also, on 32-bit platforms we could, in theory, overflow size_t even
		 * before newSize gets to INT_MAX.  (In practice we'd doubtless hit
		 * OOM long before that, but let's check.)
		 */
#if INT_MAX >= (SIZE_MAX / 2)
		if (newSize > SIZE_MAX / sizeof(PGresAttValue *))
		{
			*errmsgp = libpq_gettext("size_t overflow");
			return false;
		}
#endif

		if (res->tuples == NULL)
			newTuples = (PGresAttValue **)
				malloc(newSize * sizeof(PGresAttValue *));
		else
			newTuples = (PGresAttValue **)
				realloc(res->tuples, newSize * sizeof(PGresAttValue *));
		if (!newTuples)
			return false;		/* malloc or realloc failed */
		res->memorySize +=
			(newSize - res->tupArrSize) * sizeof(PGresAttValue *);
		res->tupArrSize = newSize;
		res->tuples = newTuples;
	}
	res->tuples[res->ntups] = tup;
	res->ntups++;
	return true;
}

/*
 * pqSaveMessageField - save one field of an error or notice message
 */
void
pqSaveMessageField(PGresult *res, char code, const char *value)
{
	PGMessageField *pfield;

	pfield = (PGMessageField *)
		pqResultAlloc(res,
					  offsetof(PGMessageField, contents) +
					  strlen(value) + 1,
					  true);
	if (!pfield)
		return;					/* out of memory? */
	pfield->code = code;
	strcpy(pfield->contents, value);
	pfield->next = res->errFields;
	res->errFields = pfield;
}

/*
 * pqSaveParameterStatus - remember parameter status sent by backend
 */
void
pqSaveParameterStatus(PGconn *conn, const char *name, const char *value)
{
	pgParameterStatus *pstatus;
	pgParameterStatus *prev;

	/*
	 * Forget any old information about the parameter
	 */
	for (pstatus = conn->pstatus, prev = NULL;
		 pstatus != NULL;
		 prev = pstatus, pstatus = pstatus->next)
	{
		if (strcmp(pstatus->name, name) == 0)
		{
			if (prev)
				prev->next = pstatus->next;
			else
				conn->pstatus = pstatus->next;
			free(pstatus);		/* frees name and value strings too */
			break;
		}
	}

	/*
	 * Store new info as a single malloc block
	 */
	pstatus = (pgParameterStatus *) malloc(sizeof(pgParameterStatus) +
										   strlen(name) + strlen(value) + 2);
	if (pstatus)
	{
		char	   *ptr;

		ptr = ((char *) pstatus) + sizeof(pgParameterStatus);
		pstatus->name = ptr;
		strcpy(ptr, name);
		ptr += strlen(name) + 1;
		pstatus->value = ptr;
		strcpy(ptr, value);
		pstatus->next = conn->pstatus;
		conn->pstatus = pstatus;
	}

	/*
	 * Save values of settings that are of interest to libpq in fields of the
	 * PGconn object.  We keep client_encoding and standard_conforming_strings
	 * in static variables as well, so that PQescapeString and PQescapeBytea
	 * can behave somewhat sanely (at least in single-connection-using
	 * programs).
	 */
	if (strcmp(name, "client_encoding") == 0)
	{
		conn->client_encoding = pg_char_to_encoding(value);
		/* if we don't recognize the encoding name, fall back to SQL_ASCII */
		if (conn->client_encoding < 0)
			conn->client_encoding = PG_SQL_ASCII;
		static_client_encoding = conn->client_encoding;
	}
	else if (strcmp(name, "standard_conforming_strings") == 0)
	{
		conn->std_strings = (strcmp(value, "on") == 0);
		static_std_strings = conn->std_strings;
	}
	else if (strcmp(name, "server_version") == 0)
	{
		/* We convert the server version to numeric form. */
		int			cnt;
		int			vmaj,
					vmin,
					vrev;

		cnt = sscanf(value, "%d.%d.%d", &vmaj, &vmin, &vrev);

		if (cnt == 3)
		{
			/* old style, e.g. 9.6.1 */
			conn->sversion = (100 * vmaj + vmin) * 100 + vrev;
		}
		else if (cnt == 2)
		{
			if (vmaj >= 10)
			{
				/* new style, e.g. 10.1 */
				conn->sversion = 100 * 100 * vmaj + vmin;
			}
			else
			{
				/* old style without minor version, e.g. 9.6devel */
				conn->sversion = (100 * vmaj + vmin) * 100;
			}
		}
		else if (cnt == 1)
		{
			/* new style without minor version, e.g. 10devel */
			conn->sversion = 100 * 100 * vmaj;
		}
		else
			conn->sversion = 0; /* unknown */
	}
	else if (strcmp(name, "default_transaction_read_only") == 0)
	{
		conn->default_transaction_read_only =
			(strcmp(value, "on") == 0) ? PG_BOOL_YES : PG_BOOL_NO;
	}
	else if (strcmp(name, "in_hot_standby") == 0)
	{
		conn->in_hot_standby =
			(strcmp(value, "on") == 0) ? PG_BOOL_YES : PG_BOOL_NO;
	}
	else if (strcmp(name, "scram_iterations") == 0)
	{
		conn->scram_sha_256_iterations = atoi(value);
	}
}


/*
 * pqRowProcessor
 *	  Add the received row to the current async result (conn->result).
 *	  Returns 1 if OK, 0 if error occurred.
 *
 * On error, *errmsgp can be set to an error string to be returned.
 * (Such a string should already be translated via libpq_gettext().)
 * If it is left NULL, the error is presumed to be "out of memory".
 */
int
pqRowProcessor(PGconn *conn, const char **errmsgp)
{
	PGresult   *res = conn->result;
	int			nfields = res->numAttributes;
	const PGdataValue *columns = conn->rowBuf;
	PGresAttValue *tup;
	int			i;

	/*
	 * In partial-result mode, if we don't already have a partial PGresult
	 * then make one by cloning conn->result (which should hold the correct
	 * result metadata by now).  Then the original conn->result is moved over
	 * to saved_result so that we can re-use it as a reference for future
	 * partial results.  The saved result will become active again after
	 * pqPrepareAsyncResult() returns the partial result to the application.
	 */
	if (conn->partialResMode && conn->saved_result == NULL)
	{
		/* Copy everything that should be in the result at this point */
		res = PQcopyResult(res,
						   PG_COPYRES_ATTRS | PG_COPYRES_EVENTS |
						   PG_COPYRES_NOTICEHOOKS);
		if (!res)
			return 0;
		/* Change result status to appropriate special value */
		res->resultStatus = (conn->singleRowMode ? PGRES_SINGLE_TUPLE : PGRES_TUPLES_CHUNK);
		/* And stash it as the active result */
		conn->saved_result = conn->result;
		conn->result = res;
	}

	/*
	 * Basically we just allocate space in the PGresult for each field and
	 * copy the data over.
	 *
	 * Note: on malloc failure, we return 0 leaving *errmsgp still NULL, which
	 * caller will take to mean "out of memory".  This is preferable to trying
	 * to set up such a message here, because evidently there's not enough
	 * memory for gettext() to do anything.
	 */
	tup = (PGresAttValue *)
		pqResultAlloc(res, nfields * sizeof(PGresAttValue), true);
	if (tup == NULL)
		return 0;

	for (i = 0; i < nfields; i++)
	{
		int			clen = columns[i].len;

		if (clen < 0)
		{
			/* null field */
			tup[i].len = NULL_LEN;
			tup[i].value = res->null_field;
		}
		else
		{
			bool		isbinary = (res->attDescs[i].format != 0);
			char	   *val;

			val = (char *) pqResultAlloc(res, clen + 1, isbinary);
			if (val == NULL)
				return 0;

			/* copy and zero-terminate the data (even if it's binary) */
			memcpy(val, columns[i].value, clen);
			val[clen] = '\0';

			tup[i].len = clen;
			tup[i].value = val;
		}
	}

	/* And add the tuple to the PGresult's tuple array */
	if (!pqAddTuple(res, tup, errmsgp))
		return 0;

	/*
	 * Success.  In partial-result mode, if we have enough rows then make the
	 * result available to the client immediately.
	 */
	if (conn->partialResMode && res->ntups >= conn->maxChunkSize)
		conn->asyncStatus = PGASYNC_READY_MORE;

	return 1;
}


/*
 * pqAllocCmdQueueEntry
 *		Get a command queue entry for caller to fill.
 *
 * If the recycle queue has a free element, that is returned; if not, a
 * fresh one is allocated.  Caller is responsible for adding it to the
 * command queue (pqAppendCmdQueueEntry) once the struct is filled in, or
 * releasing the memory (pqRecycleCmdQueueEntry) if an error occurs.
 *
 * If allocation fails, sets the error message and returns NULL.
 */
static PGcmdQueueEntry *
pqAllocCmdQueueEntry(PGconn *conn)
{
	PGcmdQueueEntry *entry;

	if (conn->cmd_queue_recycle == NULL)
	{
		entry = (PGcmdQueueEntry *) malloc(sizeof(PGcmdQueueEntry));
		if (entry == NULL)
		{
			libpq_append_conn_error(conn, "out of memory");
			return NULL;
		}
	}
	else
	{
		entry = conn->cmd_queue_recycle;
		conn->cmd_queue_recycle = entry->next;
	}
	entry->next = NULL;
	entry->query = NULL;

	return entry;
}

/*
 * pqAppendCmdQueueEntry
 *		Append a caller-allocated entry to the command queue, and update
 *		conn->asyncStatus to account for it.
 *
 * The query itself must already have been put in the output buffer by the
 * caller.
 */
static void
pqAppendCmdQueueEntry(PGconn *conn, PGcmdQueueEntry *entry)
{
	Assert(entry->next == NULL);

	if (conn->cmd_queue_head == NULL)
		conn->cmd_queue_head = entry;
	else
		conn->cmd_queue_tail->next = entry;

	conn->cmd_queue_tail = entry;

	switch (conn->pipelineStatus)
	{
		case PQ_PIPELINE_OFF:
		case PQ_PIPELINE_ON:

			/*
			 * When not in pipeline aborted state, if there's a result ready
			 * to be consumed, let it be so (that is, don't change away from
			 * READY or READY_MORE); otherwise set us busy to wait for
			 * something to arrive from the server.
			 */
			if (conn->asyncStatus == PGASYNC_IDLE)
				conn->asyncStatus = PGASYNC_BUSY;
			break;

		case PQ_PIPELINE_ABORTED:

			/*
			 * In aborted pipeline state, we don't expect anything from the
			 * server (since we don't send any queries that are queued).
			 * Therefore, if IDLE then do what PQgetResult would do to let
			 * itself consume commands from the queue; if we're in any other
			 * state, we don't have to do anything.
			 */
			if (conn->asyncStatus == PGASYNC_IDLE ||
				conn->asyncStatus == PGASYNC_PIPELINE_IDLE)
				pqPipelineProcessQueue(conn);
			break;
	}
}

/*
 * pqRecycleCmdQueueEntry
 *		Push a command queue entry onto the freelist.
 */
static void
pqRecycleCmdQueueEntry(PGconn *conn, PGcmdQueueEntry *entry)
{
	if (entry == NULL)
		return;

	/* recyclable entries should not have a follow-on command */
	Assert(entry->next == NULL);

	if (entry->query)
	{
		free(entry->query);
		entry->query = NULL;
	}

	entry->next = conn->cmd_queue_recycle;
	conn->cmd_queue_recycle = entry;
}


/*
 * PQsendQuery
 *	 Submit a query, but don't wait for it to finish
 *
 * Returns: 1 if successfully submitted
 *			0 if error (conn->errorMessage is set)
 *
 * PQsendQueryContinue is a non-exported version that behaves identically
 * except that it doesn't reset conn->errorMessage.
 */
int
PQsendQuery(PGconn *conn, const char *query)
{
	return PQsendQueryInternal(conn, query, true);
}

int
PQsendQueryContinue(PGconn *conn, const char *query)
{
	return PQsendQueryInternal(conn, query, false);
}

static int
PQsendQueryInternal(PGconn *conn, const char *query, bool newQuery)
{
	PGcmdQueueEntry *entry = NULL;

	if (!PQsendQueryStart(conn, newQuery))
		return 0;

	/* check the argument */
	if (!query)
	{
		libpq_append_conn_error(conn, "command string is a null pointer");
		return 0;
	}

	if (conn->pipelineStatus != PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "%s not allowed in pipeline mode",
								"PQsendQuery");
		return 0;
	}

	entry = pqAllocCmdQueueEntry(conn);
	if (entry == NULL)
		return 0;				/* error msg already set */

	/* Send the query message(s) */
	/* construct the outgoing Query message */
	if (pqPutMsgStart(PqMsg_Query, conn) < 0 ||
		pqPuts(query, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
	{
		/* error message should be set up already */
		pqRecycleCmdQueueEntry(conn, entry);
		return 0;
	}

	/* remember we are using simple query protocol */
	entry->queryclass = PGQUERY_SIMPLE;
	/* and remember the query text too, if possible */
	entry->query = strdup(query);

	/*
	 * Give the data a push.  In nonblock mode, don't complain if we're unable
	 * to send it all; PQgetResult() will do any additional flushing needed.
	 */
	if (pqFlush(conn) < 0)
		goto sendFailed;

	/* OK, it's launched! */
	pqAppendCmdQueueEntry(conn, entry);

	return 1;

sendFailed:
	pqRecycleCmdQueueEntry(conn, entry);
	/* error message should be set up already */
	return 0;
}

/*
 * PQsendQueryParams
 *		Like PQsendQuery, but use extended query protocol so we can pass parameters
 */
int
PQsendQueryParams(PGconn *conn,
				  const char *command,
				  int nParams,
				  const Oid *paramTypes,
				  const char *const *paramValues,
				  const int *paramLengths,
				  const int *paramFormats,
				  int resultFormat)
{
	if (!PQsendQueryStart(conn, true))
		return 0;

	/* check the arguments */
	if (!command)
	{
		libpq_append_conn_error(conn, "command string is a null pointer");
		return 0;
	}
	if (nParams < 0 || nParams > PQ_QUERY_PARAM_MAX_LIMIT)
	{
		libpq_append_conn_error(conn, "number of parameters must be between 0 and %d",
								PQ_QUERY_PARAM_MAX_LIMIT);
		return 0;
	}

	return PQsendQueryGuts(conn,
						   command,
						   "",	/* use unnamed statement */
						   nParams,
						   paramTypes,
						   paramValues,
						   paramLengths,
						   paramFormats,
						   resultFormat);
}

/*
 * PQsendPrepare
 *	 Submit a Parse message, but don't wait for it to finish
 *
 * Returns: 1 if successfully submitted
 *			0 if error (conn->errorMessage is set)
 */
int
PQsendPrepare(PGconn *conn,
			  const char *stmtName, const char *query,
			  int nParams, const Oid *paramTypes)
{
	PGcmdQueueEntry *entry = NULL;

	if (!PQsendQueryStart(conn, true))
		return 0;

	/* check the arguments */
	if (!stmtName)
	{
		libpq_append_conn_error(conn, "statement name is a null pointer");
		return 0;
	}
	if (!query)
	{
		libpq_append_conn_error(conn, "command string is a null pointer");
		return 0;
	}
	if (nParams < 0 || nParams > PQ_QUERY_PARAM_MAX_LIMIT)
	{
		libpq_append_conn_error(conn, "number of parameters must be between 0 and %d",
								PQ_QUERY_PARAM_MAX_LIMIT);
		return 0;
	}

	entry = pqAllocCmdQueueEntry(conn);
	if (entry == NULL)
		return 0;				/* error msg already set */

	/* construct the Parse message */
	if (pqPutMsgStart(PqMsg_Parse, conn) < 0 ||
		pqPuts(stmtName, conn) < 0 ||
		pqPuts(query, conn) < 0)
		goto sendFailed;

	if (nParams > 0 && paramTypes)
	{
		int			i;

		if (pqPutInt(nParams, 2, conn) < 0)
			goto sendFailed;
		for (i = 0; i < nParams; i++)
		{
			if (pqPutInt(paramTypes[i], 4, conn) < 0)
				goto sendFailed;
		}
	}
	else
	{
		if (pqPutInt(0, 2, conn) < 0)
			goto sendFailed;
	}
	if (pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/* Add a Sync, unless in pipeline mode. */
	if (conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		if (pqPutMsgStart(PqMsg_Sync, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			goto sendFailed;
	}

	/* remember we are doing just a Parse */
	entry->queryclass = PGQUERY_PREPARE;

	/* and remember the query text too, if possible */
	/* if insufficient memory, query just winds up NULL */
	entry->query = strdup(query);

	/*
	 * Give the data a push (in pipeline mode, only if we're past the size
	 * threshold).  In nonblock mode, don't complain if we're unable to send
	 * it all; PQgetResult() will do any additional flushing needed.
	 */
	if (pqPipelineFlush(conn) < 0)
		goto sendFailed;

	/* OK, it's launched! */
	pqAppendCmdQueueEntry(conn, entry);

	return 1;

sendFailed:
	pqRecycleCmdQueueEntry(conn, entry);
	/* error message should be set up already */
	return 0;
}

/*
 * PQsendQueryPrepared
 *		Like PQsendQuery, but execute a previously prepared statement,
 *		using extended query protocol so we can pass parameters
 */
int
PQsendQueryPrepared(PGconn *conn,
					const char *stmtName,
					int nParams,
					const char *const *paramValues,
					const int *paramLengths,
					const int *paramFormats,
					int resultFormat)
{
	if (!PQsendQueryStart(conn, true))
		return 0;

	/* check the arguments */
	if (!stmtName)
	{
		libpq_append_conn_error(conn, "statement name is a null pointer");
		return 0;
	}
	if (nParams < 0 || nParams > PQ_QUERY_PARAM_MAX_LIMIT)
	{
		libpq_append_conn_error(conn, "number of parameters must be between 0 and %d",
								PQ_QUERY_PARAM_MAX_LIMIT);
		return 0;
	}

	return PQsendQueryGuts(conn,
						   NULL,	/* no command to parse */
						   stmtName,
						   nParams,
						   NULL,	/* no param types */
						   paramValues,
						   paramLengths,
						   paramFormats,
						   resultFormat);
}

/*
 * PQsendQueryStart
 *	Common startup code for PQsendQuery and sibling routines
 */
static bool
PQsendQueryStart(PGconn *conn, bool newQuery)
{
	if (!conn)
		return false;

	/*
	 * If this is the beginning of a query cycle, reset the error state.
	 * However, in pipeline mode with something already queued, the error
	 * buffer belongs to that command and we shouldn't clear it.
	 */
	if (newQuery && conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	/* Don't try to send if we know there's no live connection. */
	if (conn->status != CONNECTION_OK)
	{
		libpq_append_conn_error(conn, "no connection to the server");
		return false;
	}

	/* Can't send while already busy, either, unless enqueuing for later */
	if (conn->asyncStatus != PGASYNC_IDLE &&
		conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "another command is already in progress");
		return false;
	}

	if (conn->pipelineStatus != PQ_PIPELINE_OFF)
	{
		/*
		 * When enqueuing commands we don't change much of the connection
		 * state since it's already in use for the current command. The
		 * connection state will get updated when pqPipelineProcessQueue()
		 * advances to start processing the queued message.
		 *
		 * Just make sure we can safely enqueue given the current connection
		 * state. We can enqueue behind another queue item, or behind a
		 * non-queue command (one that sends its own sync), but we can't
		 * enqueue if the connection is in a copy state.
		 */
		switch (conn->asyncStatus)
		{
			case PGASYNC_IDLE:
			case PGASYNC_PIPELINE_IDLE:
			case PGASYNC_READY:
			case PGASYNC_READY_MORE:
			case PGASYNC_BUSY:
				/* ok to queue */
				break;

			case PGASYNC_COPY_IN:
			case PGASYNC_COPY_OUT:
			case PGASYNC_COPY_BOTH:
				libpq_append_conn_error(conn, "cannot queue commands during COPY");
				return false;
		}
	}
	else
	{
		/*
		 * This command's results will come in immediately. Initialize async
		 * result-accumulation state
		 */
		pqClearAsyncResult(conn);

		/* reset partial-result mode */
		conn->partialResMode = false;
		conn->singleRowMode = false;
		conn->maxChunkSize = 0;
	}

	/* ready to send command message */
	return true;
}

/*
 * PQsendQueryGuts
 *		Common code for sending a query with extended query protocol
 *		PQsendQueryStart should be done already
 *
 * command may be NULL to indicate we use an already-prepared statement
 */
static int
PQsendQueryGuts(PGconn *conn,
				const char *command,
				const char *stmtName,
				int nParams,
				const Oid *paramTypes,
				const char *const *paramValues,
				const int *paramLengths,
				const int *paramFormats,
				int resultFormat)
{
	int			i;
	PGcmdQueueEntry *entry;

	entry = pqAllocCmdQueueEntry(conn);
	if (entry == NULL)
		return 0;				/* error msg already set */

	/*
	 * We will send Parse (if needed), Bind, Describe Portal, Execute, Sync
	 * (if not in pipeline mode), using specified statement name and the
	 * unnamed portal.
	 */

	if (command)
	{
		/* construct the Parse message */
		if (pqPutMsgStart(PqMsg_Parse, conn) < 0 ||
			pqPuts(stmtName, conn) < 0 ||
			pqPuts(command, conn) < 0)
			goto sendFailed;
		if (nParams > 0 && paramTypes)
		{
			if (pqPutInt(nParams, 2, conn) < 0)
				goto sendFailed;
			for (i = 0; i < nParams; i++)
			{
				if (pqPutInt(paramTypes[i], 4, conn) < 0)
					goto sendFailed;
			}
		}
		else
		{
			if (pqPutInt(0, 2, conn) < 0)
				goto sendFailed;
		}
		if (pqPutMsgEnd(conn) < 0)
			goto sendFailed;
	}

	/* Construct the Bind message */
	if (pqPutMsgStart(PqMsg_Bind, conn) < 0 ||
		pqPuts("", conn) < 0 ||
		pqPuts(stmtName, conn) < 0)
		goto sendFailed;

	/* Send parameter formats */
	if (nParams > 0 && paramFormats)
	{
		if (pqPutInt(nParams, 2, conn) < 0)
			goto sendFailed;
		for (i = 0; i < nParams; i++)
		{
			if (pqPutInt(paramFormats[i], 2, conn) < 0)
				goto sendFailed;
		}
	}
	else
	{
		if (pqPutInt(0, 2, conn) < 0)
			goto sendFailed;
	}

	if (pqPutInt(nParams, 2, conn) < 0)
		goto sendFailed;

	/* Send parameters */
	for (i = 0; i < nParams; i++)
	{
		if (paramValues && paramValues[i])
		{
			int			nbytes;

			if (paramFormats && paramFormats[i] != 0)
			{
				/* binary parameter */
				if (paramLengths)
					nbytes = paramLengths[i];
				else
				{
					libpq_append_conn_error(conn, "length must be given for binary parameter");
					goto sendFailed;
				}
			}
			else
			{
				/* text parameter, do not use paramLengths */
				nbytes = strlen(paramValues[i]);
			}
			if (pqPutInt(nbytes, 4, conn) < 0 ||
				pqPutnchar(paramValues[i], nbytes, conn) < 0)
				goto sendFailed;
		}
		else
		{
			/* take the param as NULL */
			if (pqPutInt(-1, 4, conn) < 0)
				goto sendFailed;
		}
	}
	if (pqPutInt(1, 2, conn) < 0 ||
		pqPutInt(resultFormat, 2, conn))
		goto sendFailed;
	if (pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/* construct the Describe Portal message */
	if (pqPutMsgStart(PqMsg_Describe, conn) < 0 ||
		pqPutc('P', conn) < 0 ||
		pqPuts("", conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/* construct the Execute message */
	if (pqPutMsgStart(PqMsg_Execute, conn) < 0 ||
		pqPuts("", conn) < 0 ||
		pqPutInt(0, 4, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/* construct the Sync message if not in pipeline mode */
	if (conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		if (pqPutMsgStart(PqMsg_Sync, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			goto sendFailed;
	}

	/* remember we are using extended query protocol */
	entry->queryclass = PGQUERY_EXTENDED;

	/* and remember the query text too, if possible */
	/* if insufficient memory, query just winds up NULL */
	if (command)
		entry->query = strdup(command);

	/*
	 * Give the data a push (in pipeline mode, only if we're past the size
	 * threshold).  In nonblock mode, don't complain if we're unable to send
	 * it all; PQgetResult() will do any additional flushing needed.
	 */
	if (pqPipelineFlush(conn) < 0)
		goto sendFailed;

	/* OK, it's launched! */
	pqAppendCmdQueueEntry(conn, entry);

	return 1;

sendFailed:
	pqRecycleCmdQueueEntry(conn, entry);
	/* error message should be set up already */
	return 0;
}

/*
 * Is it OK to change partial-result mode now?
 */
static bool
canChangeResultMode(PGconn *conn)
{
	/*
	 * Only allow changing the mode when we have launched a query and not yet
	 * received any results.
	 */
	if (!conn)
		return false;
	if (conn->asyncStatus != PGASYNC_BUSY)
		return false;
	if (!conn->cmd_queue_head ||
		(conn->cmd_queue_head->queryclass != PGQUERY_SIMPLE &&
		 conn->cmd_queue_head->queryclass != PGQUERY_EXTENDED))
		return false;
	if (pgHavePendingResult(conn))
		return false;
	return true;
}

/*
 * Select row-by-row processing mode
 */
int
PQsetSingleRowMode(PGconn *conn)
{
	if (canChangeResultMode(conn))
	{
		conn->partialResMode = true;
		conn->singleRowMode = true;
		conn->maxChunkSize = 1;
		return 1;
	}
	else
		return 0;
}

/*
 * Select chunked results processing mode
 */
int
PQsetChunkedRowsMode(PGconn *conn, int chunkSize)
{
	if (chunkSize > 0 && canChangeResultMode(conn))
	{
		conn->partialResMode = true;
		conn->singleRowMode = false;
		conn->maxChunkSize = chunkSize;
		return 1;
	}
	else
		return 0;
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
	 * for non-blocking connections try to flush the send-queue, otherwise we
	 * may never get a response for something that may not have already been
	 * sent because it's in our write buffer!
	 */
	if (pqIsnonblocking(conn))
	{
		if (pqFlush(conn) < 0)
			return 0;
	}

	/*
	 * Load more data, if available. We do this no matter what state we are
	 * in, since we are probably getting called because the application wants
	 * to get rid of a read-select condition. Note that we will NOT block
	 * waiting for more input.
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
	pqParseInput3(conn);
}

/*
 * PQisBusy
 *	 Return true if PQgetResult would block waiting for input.
 */

int
PQisBusy(PGconn *conn)
{
	if (!conn)
		return false;

	/* Parse any available data, if our state permits. */
	parseInput(conn);

	/*
	 * PQgetResult will return immediately in all states except BUSY.  Also,
	 * if we've detected read EOF and dropped the connection, we can expect
	 * that PQgetResult will fail immediately.  Note that we do *not* check
	 * conn->write_failed here --- once that's become set, we know we have
	 * trouble, but we need to keep trying to read until we have a complete
	 * server message or detect read EOF.
	 */
	return conn->asyncStatus == PGASYNC_BUSY && conn->status != CONNECTION_BAD;
}

/*
 * PQgetResult
 *	  Get the next PGresult produced by a query.  Returns NULL if no
 *	  query work remains or an error has occurred (e.g. out of
 *	  memory).
 *
 *	  In pipeline mode, once all the result of a query have been returned,
 *	  PQgetResult returns NULL to let the user know that the next
 *	  query is being processed.  At the end of the pipeline, returns a
 *	  result with PQresultStatus(result) == PGRES_PIPELINE_SYNC.
 */
PGresult *
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
		int			flushResult;

		/*
		 * If data remains unsent, send it.  Else we might be waiting for the
		 * result of a command the backend hasn't even got yet.
		 */
		while ((flushResult = pqFlush(conn)) > 0)
		{
			if (pqWait(false, true, conn))
			{
				flushResult = -1;
				break;
			}
		}

		/*
		 * Wait for some more data, and load it.  (Note: if the connection has
		 * been lost, pqWait should return immediately because the socket
		 * should be read-ready, either with the last server data or with an
		 * EOF indication.  We expect therefore that this won't result in any
		 * undue delay in reporting a previous write failure.)
		 */
		if (flushResult ||
			pqWait(true, false, conn) ||
			pqReadData(conn) < 0)
		{
			/* Report the error saved by pqWait or pqReadData */
			pqSaveErrorResult(conn);
			conn->asyncStatus = PGASYNC_IDLE;
			return pqPrepareAsyncResult(conn);
		}

		/* Parse it. */
		parseInput(conn);

		/*
		 * If we had a write error, but nothing above obtained a query result
		 * or detected a read error, report the write error.
		 */
		if (conn->write_failed && conn->asyncStatus == PGASYNC_BUSY)
		{
			pqSaveWriteError(conn);
			conn->asyncStatus = PGASYNC_IDLE;
			return pqPrepareAsyncResult(conn);
		}
	}

	/* Return the appropriate thing. */
	switch (conn->asyncStatus)
	{
		case PGASYNC_IDLE:
			res = NULL;			/* query is complete */
			break;
		case PGASYNC_PIPELINE_IDLE:
			Assert(conn->pipelineStatus != PQ_PIPELINE_OFF);

			/*
			 * We're about to return the NULL that terminates the round of
			 * results from the current query; prepare to send the results of
			 * the next query, if any, when we're called next.  If there's no
			 * next element in the command queue, this gets us in IDLE state.
			 */
			pqPipelineProcessQueue(conn);
			res = NULL;			/* query is complete */
			break;

		case PGASYNC_READY:
			res = pqPrepareAsyncResult(conn);

			/*
			 * Normally pqPrepareAsyncResult will have left conn->result
			 * empty.  Otherwise, "res" must be a not-full PGRES_TUPLES_CHUNK
			 * result, which we want to return to the caller while staying in
			 * PGASYNC_READY state.  Then the next call here will return the
			 * empty PGRES_TUPLES_OK result that was restored from
			 * saved_result, after which we can proceed.
			 */
			if (conn->result)
			{
				Assert(res->resultStatus == PGRES_TUPLES_CHUNK);
				break;
			}

			/* Advance the queue as appropriate */
			pqCommandQueueAdvance(conn, false,
								  res->resultStatus == PGRES_PIPELINE_SYNC);

			if (conn->pipelineStatus != PQ_PIPELINE_OFF)
			{
				/*
				 * We're about to send the results of the current query.  Set
				 * us idle now, and ...
				 */
				conn->asyncStatus = PGASYNC_PIPELINE_IDLE;

				/*
				 * ... in cases when we're sending a pipeline-sync result,
				 * move queue processing forwards immediately, so that next
				 * time we're called, we're prepared to return the next result
				 * received from the server.  In all other cases, leave the
				 * queue state change for next time, so that a terminating
				 * NULL result is sent.
				 *
				 * (In other words: we don't return a NULL after a pipeline
				 * sync.)
				 */
				if (res->resultStatus == PGRES_PIPELINE_SYNC)
					pqPipelineProcessQueue(conn);
			}
			else
			{
				/* Set the state back to BUSY, allowing parsing to proceed. */
				conn->asyncStatus = PGASYNC_BUSY;
			}
			break;
		case PGASYNC_READY_MORE:
			res = pqPrepareAsyncResult(conn);
			/* Set the state back to BUSY, allowing parsing to proceed. */
			conn->asyncStatus = PGASYNC_BUSY;
			break;
		case PGASYNC_COPY_IN:
			res = getCopyResult(conn, PGRES_COPY_IN);
			break;
		case PGASYNC_COPY_OUT:
			res = getCopyResult(conn, PGRES_COPY_OUT);
			break;
		case PGASYNC_COPY_BOTH:
			res = getCopyResult(conn, PGRES_COPY_BOTH);
			break;
		default:
			libpq_append_conn_error(conn, "unexpected asyncStatus: %d", (int) conn->asyncStatus);
			pqSaveErrorResult(conn);
			conn->asyncStatus = PGASYNC_IDLE;	/* try to restore valid state */
			res = pqPrepareAsyncResult(conn);
			break;
	}

	/* Time to fire PGEVT_RESULTCREATE events, if there are any */
	if (res && res->nEvents > 0)
		(void) PQfireResultCreateEvents(conn, res);

	return res;
}

/*
 * getCopyResult
 *	  Helper for PQgetResult: generate result for COPY-in-progress cases
 */
static PGresult *
getCopyResult(PGconn *conn, ExecStatusType copytype)
{
	/*
	 * If the server connection has been lost, don't pretend everything is
	 * hunky-dory; instead return a PGRES_FATAL_ERROR result, and reset the
	 * asyncStatus to idle (corresponding to what we'd do if we'd detected I/O
	 * error in the earlier steps in PQgetResult).  The text returned in the
	 * result is whatever is in conn->errorMessage; we hope that was filled
	 * with something relevant when the lost connection was detected.
	 */
	if (conn->status != CONNECTION_OK)
	{
		pqSaveErrorResult(conn);
		conn->asyncStatus = PGASYNC_IDLE;
		return pqPrepareAsyncResult(conn);
	}

	/* If we have an async result for the COPY, return that */
	if (conn->result && conn->result->resultStatus == copytype)
		return pqPrepareAsyncResult(conn);

	/* Otherwise, invent a suitable PGresult */
	return PQmakeEmptyPGresult(conn, copytype);
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
PGresult *
PQexec(PGconn *conn, const char *query)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendQuery(conn, query))
		return NULL;
	return PQexecFinish(conn);
}

/*
 * PQexecParams
 *		Like PQexec, but use extended query protocol so we can pass parameters
 */
PGresult *
PQexecParams(PGconn *conn,
			 const char *command,
			 int nParams,
			 const Oid *paramTypes,
			 const char *const *paramValues,
			 const int *paramLengths,
			 const int *paramFormats,
			 int resultFormat)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendQueryParams(conn, command,
						   nParams, paramTypes, paramValues, paramLengths,
						   paramFormats, resultFormat))
		return NULL;
	return PQexecFinish(conn);
}

/*
 * PQprepare
 *	  Creates a prepared statement by issuing a Parse message.
 *
 * If the query was not even sent, return NULL; conn->errorMessage is set to
 * a relevant message.
 * If the query was sent, a new PGresult is returned (which could indicate
 * either success or failure).
 * The user is responsible for freeing the PGresult via PQclear()
 * when done with it.
 */
PGresult *
PQprepare(PGconn *conn,
		  const char *stmtName, const char *query,
		  int nParams, const Oid *paramTypes)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendPrepare(conn, stmtName, query, nParams, paramTypes))
		return NULL;
	return PQexecFinish(conn);
}

/*
 * PQexecPrepared
 *		Like PQexec, but execute a previously prepared statement,
 *		using extended query protocol so we can pass parameters
 */
PGresult *
PQexecPrepared(PGconn *conn,
			   const char *stmtName,
			   int nParams,
			   const char *const *paramValues,
			   const int *paramLengths,
			   const int *paramFormats,
			   int resultFormat)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendQueryPrepared(conn, stmtName,
							 nParams, paramValues, paramLengths,
							 paramFormats, resultFormat))
		return NULL;
	return PQexecFinish(conn);
}

/*
 * Common code for PQexec and sibling routines: prepare to send command
 */
static bool
PQexecStart(PGconn *conn)
{
	PGresult   *result;

	if (!conn)
		return false;

	/*
	 * Since this is the beginning of a query cycle, reset the error state.
	 * However, in pipeline mode with something already queued, the error
	 * buffer belongs to that command and we shouldn't clear it.
	 */
	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	if (conn->pipelineStatus != PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "synchronous command execution functions are not allowed in pipeline mode");
		return false;
	}

	/*
	 * Silently discard any prior query result that application didn't eat.
	 * This is probably poor design, but it's here for backward compatibility.
	 */
	while ((result = PQgetResult(conn)) != NULL)
	{
		ExecStatusType resultStatus = result->resultStatus;

		PQclear(result);		/* only need its status */
		if (resultStatus == PGRES_COPY_IN)
		{
			/* get out of a COPY IN state */
			if (PQputCopyEnd(conn,
							 libpq_gettext("COPY terminated by new PQexec")) < 0)
				return false;
			/* keep waiting to swallow the copy's failure message */
		}
		else if (resultStatus == PGRES_COPY_OUT)
		{
			/*
			 * Get out of a COPY OUT state: we just switch back to BUSY and
			 * allow the remaining COPY data to be dropped on the floor.
			 */
			conn->asyncStatus = PGASYNC_BUSY;
			/* keep waiting to swallow the copy's completion message */
		}
		else if (resultStatus == PGRES_COPY_BOTH)
		{
			/* We don't allow PQexec during COPY BOTH */
			libpq_append_conn_error(conn, "PQexec not allowed during COPY BOTH");
			return false;
		}
		/* check for loss of connection, too */
		if (conn->status == CONNECTION_BAD)
			return false;
	}

	/* OK to send a command */
	return true;
}

/*
 * Common code for PQexec and sibling routines: wait for command result
 */
static PGresult *
PQexecFinish(PGconn *conn)
{
	PGresult   *result;
	PGresult   *lastResult;

	/*
	 * For backwards compatibility, return the last result if there are more
	 * than one.  (We used to have logic here to concatenate successive error
	 * messages, but now that happens automatically, since conn->errorMessage
	 * will continue to accumulate errors throughout this loop.)
	 *
	 * We have to stop if we see copy in/out/both, however. We will resume
	 * parsing after application performs the data transfer.
	 *
	 * Also stop if the connection is lost (else we'll loop infinitely).
	 */
	lastResult = NULL;
	while ((result = PQgetResult(conn)) != NULL)
	{
		PQclear(lastResult);
		lastResult = result;
		if (result->resultStatus == PGRES_COPY_IN ||
			result->resultStatus == PGRES_COPY_OUT ||
			result->resultStatus == PGRES_COPY_BOTH ||
			conn->status == CONNECTION_BAD)
			break;
	}

	return lastResult;
}

/*
 * PQdescribePrepared
 *	  Obtain information about a previously prepared statement
 *
 * If the query was not even sent, return NULL; conn->errorMessage is set to
 * a relevant message.
 * If the query was sent, a new PGresult is returned (which could indicate
 * either success or failure).  On success, the PGresult contains status
 * PGRES_COMMAND_OK, and its parameter and column-heading fields describe
 * the statement's inputs and outputs respectively.
 * The user is responsible for freeing the PGresult via PQclear()
 * when done with it.
 */
PGresult *
PQdescribePrepared(PGconn *conn, const char *stmt)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendTypedCommand(conn, PqMsg_Describe, 'S', stmt))
		return NULL;
	return PQexecFinish(conn);
}

/*
 * PQdescribePortal
 *	  Obtain information about a previously created portal
 *
 * This is much like PQdescribePrepared, except that no parameter info is
 * returned.  Note that at the moment, libpq doesn't really expose portals
 * to the client; but this can be used with a portal created by a SQL
 * DECLARE CURSOR command.
 */
PGresult *
PQdescribePortal(PGconn *conn, const char *portal)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendTypedCommand(conn, PqMsg_Describe, 'P', portal))
		return NULL;
	return PQexecFinish(conn);
}

/*
 * PQsendDescribePrepared
 *	 Submit a Describe Statement command, but don't wait for it to finish
 *
 * Returns: 1 if successfully submitted
 *			0 if error (conn->errorMessage is set)
 */
int
PQsendDescribePrepared(PGconn *conn, const char *stmt)
{
	return PQsendTypedCommand(conn, PqMsg_Describe, 'S', stmt);
}

/*
 * PQsendDescribePortal
 *	 Submit a Describe Portal command, but don't wait for it to finish
 *
 * Returns: 1 if successfully submitted
 *			0 if error (conn->errorMessage is set)
 */
int
PQsendDescribePortal(PGconn *conn, const char *portal)
{
	return PQsendTypedCommand(conn, PqMsg_Describe, 'P', portal);
}

/*
 * PQclosePrepared
 *	  Close a previously prepared statement
 *
 * If the query was not even sent, return NULL; conn->errorMessage is set to
 * a relevant message.
 * If the query was sent, a new PGresult is returned (which could indicate
 * either success or failure).  On success, the PGresult contains status
 * PGRES_COMMAND_OK. The user is responsible for freeing the PGresult via
 * PQclear() when done with it.
 */
PGresult *
PQclosePrepared(PGconn *conn, const char *stmt)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendTypedCommand(conn, PqMsg_Close, 'S', stmt))
		return NULL;
	return PQexecFinish(conn);
}

/*
 * PQclosePortal
 *	  Close a previously created portal
 *
 * This is exactly like PQclosePrepared, but for portals.  Note that at the
 * moment, libpq doesn't really expose portals to the client; but this can be
 * used with a portal created by a SQL DECLARE CURSOR command.
 */
PGresult *
PQclosePortal(PGconn *conn, const char *portal)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendTypedCommand(conn, PqMsg_Close, 'P', portal))
		return NULL;
	return PQexecFinish(conn);
}

/*
 * PQsendClosePrepared
 *	 Submit a Close Statement command, but don't wait for it to finish
 *
 * Returns: 1 if successfully submitted
 *			0 if error (conn->errorMessage is set)
 */
int
PQsendClosePrepared(PGconn *conn, const char *stmt)
{
	return PQsendTypedCommand(conn, PqMsg_Close, 'S', stmt);
}

/*
 * PQsendClosePortal
 *	 Submit a Close Portal command, but don't wait for it to finish
 *
 * Returns: 1 if successfully submitted
 *			0 if error (conn->errorMessage is set)
 */
int
PQsendClosePortal(PGconn *conn, const char *portal)
{
	return PQsendTypedCommand(conn, PqMsg_Close, 'P', portal);
}

/*
 * PQsendTypedCommand
 *	 Common code to send a Describe or Close command
 *
 * Available options for "command" are
 *	 PqMsg_Close for Close; or
 *	 PqMsg_Describe for Describe.
 *
 * Available options for "type" are
 *	 'S' to run a command on a prepared statement; or
 *	 'P' to run a command on a portal.
 *
 * Returns 1 on success and 0 on failure.
 */
static int
PQsendTypedCommand(PGconn *conn, char command, char type, const char *target)
{
	PGcmdQueueEntry *entry = NULL;

	/* Treat null target as empty string */
	if (!target)
		target = "";

	if (!PQsendQueryStart(conn, true))
		return 0;

	entry = pqAllocCmdQueueEntry(conn);
	if (entry == NULL)
		return 0;				/* error msg already set */

	/* construct the Close message */
	if (pqPutMsgStart(command, conn) < 0 ||
		pqPutc(type, conn) < 0 ||
		pqPuts(target, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/* construct the Sync message */
	if (conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		if (pqPutMsgStart(PqMsg_Sync, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			goto sendFailed;
	}

	/* remember if we are doing a Close or a Describe */
	if (command == PqMsg_Close)
	{
		entry->queryclass = PGQUERY_CLOSE;
	}
	else if (command == PqMsg_Describe)
	{
		entry->queryclass = PGQUERY_DESCRIBE;
	}
	else
	{
		libpq_append_conn_error(conn, "unrecognized message type \"%c\"", command);
		goto sendFailed;
	}

	/*
	 * Give the data a push (in pipeline mode, only if we're past the size
	 * threshold).  In nonblock mode, don't complain if we're unable to send
	 * it all; PQgetResult() will do any additional flushing needed.
	 */
	if (pqPipelineFlush(conn) < 0)
		goto sendFailed;

	/* OK, it's launched! */
	pqAppendCmdQueueEntry(conn, entry);

	return 1;

sendFailed:
	pqRecycleCmdQueueEntry(conn, entry);
	/* error message should be set up already */
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
 *
 * Note that this function does not read any new data from the socket;
 * so usually, caller should call PQconsumeInput() first.
 */
PGnotify *
PQnotifies(PGconn *conn)
{
	PGnotify   *event;

	if (!conn)
		return NULL;

	/* Parse any available data to see if we can extract NOTIFY messages. */
	parseInput(conn);

	event = conn->notifyHead;
	if (event)
	{
		conn->notifyHead = event->next;
		if (!conn->notifyHead)
			conn->notifyTail = NULL;
		event->next = NULL;		/* don't let app see the internal state */
	}
	return event;
}

/*
 * PQputCopyData - send some data to the backend during COPY IN or COPY BOTH
 *
 * Returns 1 if successful, 0 if data could not be sent (only possible
 * in nonblock mode), or -1 if an error occurs.
 */
int
PQputCopyData(PGconn *conn, const char *buffer, int nbytes)
{
	if (!conn)
		return -1;
	if (conn->asyncStatus != PGASYNC_COPY_IN &&
		conn->asyncStatus != PGASYNC_COPY_BOTH)
	{
		libpq_append_conn_error(conn, "no COPY in progress");
		return -1;
	}

	/*
	 * Process any NOTICE or NOTIFY messages that might be pending in the
	 * input buffer.  Since the server might generate many notices during the
	 * COPY, we want to clean those out reasonably promptly to prevent
	 * indefinite expansion of the input buffer.  (Note: the actual read of
	 * input data into the input buffer happens down inside pqSendSome, but
	 * it's not authorized to get rid of the data again.)
	 */
	parseInput(conn);

	if (nbytes > 0)
	{
		/*
		 * Try to flush any previously sent data in preference to growing the
		 * output buffer.  If we can't enlarge the buffer enough to hold the
		 * data, return 0 in the nonblock case, else hard error. (For
		 * simplicity, always assume 5 bytes of overhead.)
		 */
		if ((conn->outBufSize - conn->outCount - 5) < nbytes)
		{
			if (pqFlush(conn) < 0)
				return -1;
			if (pqCheckOutBufferSpace(conn->outCount + 5 + (size_t) nbytes,
									  conn))
				return pqIsnonblocking(conn) ? 0 : -1;
		}
		/* Send the data (too simple to delegate to fe-protocol files) */
		if (pqPutMsgStart(PqMsg_CopyData, conn) < 0 ||
			pqPutnchar(buffer, nbytes, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			return -1;
	}
	return 1;
}

/*
 * PQputCopyEnd - send EOF indication to the backend during COPY IN
 *
 * After calling this, use PQgetResult() to check command completion status.
 *
 * Returns 1 if successful, or -1 if an error occurs.
 */
int
PQputCopyEnd(PGconn *conn, const char *errormsg)
{
	if (!conn)
		return -1;
	if (conn->asyncStatus != PGASYNC_COPY_IN &&
		conn->asyncStatus != PGASYNC_COPY_BOTH)
	{
		libpq_append_conn_error(conn, "no COPY in progress");
		return -1;
	}

	/*
	 * Send the COPY END indicator.  This is simple enough that we don't
	 * bother delegating it to the fe-protocol files.
	 */
	if (errormsg)
	{
		/* Send COPY FAIL */
		if (pqPutMsgStart(PqMsg_CopyFail, conn) < 0 ||
			pqPuts(errormsg, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			return -1;
	}
	else
	{
		/* Send COPY DONE */
		if (pqPutMsgStart(PqMsg_CopyDone, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			return -1;
	}

	/*
	 * If we sent the COPY command in extended-query mode, we must issue a
	 * Sync as well.
	 */
	if (conn->cmd_queue_head &&
		conn->cmd_queue_head->queryclass != PGQUERY_SIMPLE)
	{
		if (pqPutMsgStart(PqMsg_Sync, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			return -1;
	}

	/* Return to active duty */
	if (conn->asyncStatus == PGASYNC_COPY_BOTH)
		conn->asyncStatus = PGASYNC_COPY_OUT;
	else
		conn->asyncStatus = PGASYNC_BUSY;

	/* Try to flush data */
	if (pqFlush(conn) < 0)
		return -1;

	return 1;
}

/*
 * PQgetCopyData - read a row of data from the backend during COPY OUT
 * or COPY BOTH
 *
 * If successful, sets *buffer to point to a malloc'd row of data, and
 * returns row length (always > 0) as result.
 * Returns 0 if no row available yet (only possible if async is true),
 * -1 if end of copy (consult PQgetResult), or -2 if error (consult
 * PQerrorMessage).
 */
int
PQgetCopyData(PGconn *conn, char **buffer, int async)
{
	*buffer = NULL;				/* for all failure cases */
	if (!conn)
		return -2;
	if (conn->asyncStatus != PGASYNC_COPY_OUT &&
		conn->asyncStatus != PGASYNC_COPY_BOTH)
	{
		libpq_append_conn_error(conn, "no COPY in progress");
		return -2;
	}
	return pqGetCopyData3(conn, buffer, async);
}

/*
 * PQgetline - gets a newline-terminated string from the backend.
 *
 * Chiefly here so that applications can use "COPY <rel> to stdout"
 * and read the output string.  Returns a null-terminated string in `buffer`.
 *
 * XXX this routine is now deprecated, because it can't handle binary data.
 * If called during a COPY BINARY we return EOF.
 *
 * PQgetline reads up to `length`-1 characters (like fgets(3)) but strips
 * the terminating \n (like gets(3)).
 *
 * CAUTION: the caller is responsible for detecting the end-of-copy signal
 * (a line containing just "\.") when using this routine.
 *
 * RETURNS:
 *		EOF if error (eg, invalid arguments are given)
 *		0 if EOL is reached (i.e., \n has been read)
 *				(this is required for backward-compatibility -- this
 *				 routine used to always return EOF or 0, assuming that
 *				 the line ended within `length` bytes.)
 *		1 in other cases (i.e., the buffer was filled before \n is reached)
 */
int
PQgetline(PGconn *conn, char *buffer, int length)
{
	if (!buffer || length <= 0)
		return EOF;
	*buffer = '\0';
	/* length must be at least 3 to hold the \. terminator! */
	if (length < 3)
		return EOF;

	if (!conn)
		return EOF;

	return pqGetline3(conn, buffer, length);
}

/*
 * PQgetlineAsync - gets a COPY data row without blocking.
 *
 * This routine is for applications that want to do "COPY <rel> to stdout"
 * asynchronously, that is without blocking.  Having issued the COPY command
 * and gotten a PGRES_COPY_OUT response, the app should call PQconsumeInput
 * and this routine until the end-of-data signal is detected.  Unlike
 * PQgetline, this routine takes responsibility for detecting end-of-data.
 *
 * On each call, PQgetlineAsync will return data if a complete data row
 * is available in libpq's input buffer.  Otherwise, no data is returned
 * until the rest of the row arrives.
 *
 * If -1 is returned, the end-of-data signal has been recognized (and removed
 * from libpq's input buffer).  The caller *must* next call PQendcopy and
 * then return to normal processing.
 *
 * RETURNS:
 *	 -1    if the end-of-copy-data marker has been recognized
 *	 0	   if no data is available
 *	 >0    the number of bytes returned.
 *
 * The data returned will not extend beyond a data-row boundary.  If possible
 * a whole row will be returned at one time.  But if the buffer offered by
 * the caller is too small to hold a row sent by the backend, then a partial
 * data row will be returned.  In text mode this can be detected by testing
 * whether the last returned byte is '\n' or not.
 *
 * The returned data is *not* null-terminated.
 */

int
PQgetlineAsync(PGconn *conn, char *buffer, int bufsize)
{
	if (!conn)
		return -1;

	return pqGetlineAsync3(conn, buffer, bufsize);
}

/*
 * PQputline -- sends a string to the backend during COPY IN.
 * Returns 0 if OK, EOF if not.
 *
 * This is deprecated primarily because the return convention doesn't allow
 * caller to tell the difference between a hard error and a nonblock-mode
 * send failure.
 */
int
PQputline(PGconn *conn, const char *string)
{
	return PQputnbytes(conn, string, strlen(string));
}

/*
 * PQputnbytes -- like PQputline, but buffer need not be null-terminated.
 * Returns 0 if OK, EOF if not.
 */
int
PQputnbytes(PGconn *conn, const char *buffer, int nbytes)
{
	if (PQputCopyData(conn, buffer, nbytes) > 0)
		return 0;
	else
		return EOF;
}

/*
 * PQendcopy
 *		After completing the data transfer portion of a copy in/out,
 *		the application must call this routine to finish the command protocol.
 *
 * This is deprecated; it's cleaner to use PQgetResult to get the transfer
 * status.
 *
 * RETURNS:
 *		0 on success
 *		1 on failure
 */
int
PQendcopy(PGconn *conn)
{
	if (!conn)
		return 0;

	return pqEndcopy3(conn);
}


/* ----------------
 *		PQfn -	Send a function call to the POSTGRES backend.
 *
 *		conn			: backend connection
 *		fnid			: OID of function to be called
 *		result_buf		: pointer to result buffer
 *		result_len		: actual length of result is returned here
 *		result_is_int	: If the result is an integer, this must be 1,
 *						  otherwise this should be 0
 *		args			: pointer to an array of function arguments
 *						  (each has length, if integer, and value/pointer)
 *		nargs			: # of arguments in args array.
 *
 * RETURNS
 *		PGresult with status = PGRES_COMMAND_OK if successful.
 *			*result_len is > 0 if there is a return value, 0 if not.
 *		PGresult with status = PGRES_FATAL_ERROR if backend returns an error.
 *		NULL on communications failure.  conn->errorMessage will be set.
 * ----------------
 */

PGresult *
PQfn(PGconn *conn,
	 int fnid,
	 int *result_buf,
	 int *result_len,
	 int result_is_int,
	 const PQArgBlock *args,
	 int nargs)
{
	*result_len = 0;

	if (!conn)
		return NULL;

	/*
	 * Since this is the beginning of a query cycle, reset the error state.
	 * However, in pipeline mode with something already queued, the error
	 * buffer belongs to that command and we shouldn't clear it.
	 */
	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	if (conn->pipelineStatus != PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "%s not allowed in pipeline mode", "PQfn");
		return NULL;
	}

	if (conn->sock == PGINVALID_SOCKET || conn->asyncStatus != PGASYNC_IDLE ||
		pgHavePendingResult(conn))
	{
		libpq_append_conn_error(conn, "connection in wrong state");
		return NULL;
	}

	return pqFunctionCall3(conn, fnid,
						   result_buf, result_len,
						   result_is_int,
						   args, nargs);
}

/* ====== Pipeline mode support ======== */

/*
 * PQenterPipelineMode
 *		Put an idle connection in pipeline mode.
 *
 * Returns 1 on success. On failure, errorMessage is set and 0 is returned.
 *
 * Commands submitted after this can be pipelined on the connection;
 * there's no requirement to wait for one to finish before the next is
 * dispatched.
 *
 * Queuing of a new query or syncing during COPY is not allowed.
 *
 * A set of commands is terminated by a PQpipelineSync.  Multiple sync
 * points can be established while in pipeline mode.  Pipeline mode can
 * be exited by calling PQexitPipelineMode() once all results are processed.
 *
 * This doesn't actually send anything on the wire, it just puts libpq
 * into a state where it can pipeline work.
 */
int
PQenterPipelineMode(PGconn *conn)
{
	if (!conn)
		return 0;

	/* succeed with no action if already in pipeline mode */
	if (conn->pipelineStatus != PQ_PIPELINE_OFF)
		return 1;

	if (conn->asyncStatus != PGASYNC_IDLE)
	{
		libpq_append_conn_error(conn, "cannot enter pipeline mode, connection not idle");
		return 0;
	}

	conn->pipelineStatus = PQ_PIPELINE_ON;

	return 1;
}

/*
 * PQexitPipelineMode
 *		End pipeline mode and return to normal command mode.
 *
 * Returns 1 in success (pipeline mode successfully ended, or not in pipeline
 * mode).
 *
 * Returns 0 if in pipeline mode and cannot be ended yet.  Error message will
 * be set.
 */
int
PQexitPipelineMode(PGconn *conn)
{
	if (!conn)
		return 0;

	if (conn->pipelineStatus == PQ_PIPELINE_OFF &&
		(conn->asyncStatus == PGASYNC_IDLE ||
		 conn->asyncStatus == PGASYNC_PIPELINE_IDLE) &&
		conn->cmd_queue_head == NULL)
		return 1;

	switch (conn->asyncStatus)
	{
		case PGASYNC_READY:
		case PGASYNC_READY_MORE:
			/* there are some uncollected results */
			libpq_append_conn_error(conn, "cannot exit pipeline mode with uncollected results");
			return 0;

		case PGASYNC_BUSY:
			libpq_append_conn_error(conn, "cannot exit pipeline mode while busy");
			return 0;

		case PGASYNC_IDLE:
		case PGASYNC_PIPELINE_IDLE:
			/* OK */
			break;

		case PGASYNC_COPY_IN:
		case PGASYNC_COPY_OUT:
		case PGASYNC_COPY_BOTH:
			libpq_append_conn_error(conn, "cannot exit pipeline mode while in COPY");
	}

	/* still work to process */
	if (conn->cmd_queue_head != NULL)
	{
		libpq_append_conn_error(conn, "cannot exit pipeline mode with uncollected results");
		return 0;
	}

	conn->pipelineStatus = PQ_PIPELINE_OFF;
	conn->asyncStatus = PGASYNC_IDLE;

	/* Flush any pending data in out buffer */
	if (pqFlush(conn) < 0)
		return 0;				/* error message is setup already */
	return 1;
}

/*
 * pqCommandQueueAdvance
 *		Remove one query from the command queue, if appropriate.
 *
 * If we have received all results corresponding to the head element
 * in the command queue, remove it.
 *
 * In simple query protocol we must not advance the command queue until the
 * ReadyForQuery message has been received.  This is because in simple mode a
 * command can have multiple queries, and we must process result for all of
 * them before moving on to the next command.
 *
 * Another consideration is synchronization during error processing in
 * extended query protocol: we refuse to advance the queue past a SYNC queue
 * element, unless the result we've received is also a SYNC.  In particular
 * this protects us from advancing when an error is received at an
 * inappropriate moment.
 */
void
pqCommandQueueAdvance(PGconn *conn, bool isReadyForQuery, bool gotSync)
{
	PGcmdQueueEntry *prevquery;

	if (conn->cmd_queue_head == NULL)
		return;

	/*
	 * If processing a query of simple query protocol, we only advance the
	 * queue when we receive the ReadyForQuery message for it.
	 */
	if (conn->cmd_queue_head->queryclass == PGQUERY_SIMPLE && !isReadyForQuery)
		return;

	/*
	 * If we're waiting for a SYNC, don't advance the queue until we get one.
	 */
	if (conn->cmd_queue_head->queryclass == PGQUERY_SYNC && !gotSync)
		return;

	/* delink element from queue */
	prevquery = conn->cmd_queue_head;
	conn->cmd_queue_head = conn->cmd_queue_head->next;

	/* If the queue is now empty, reset the tail too */
	if (conn->cmd_queue_head == NULL)
		conn->cmd_queue_tail = NULL;

	/* and make the queue element recyclable */
	prevquery->next = NULL;
	pqRecycleCmdQueueEntry(conn, prevquery);
}

/*
 * pqPipelineProcessQueue: subroutine for PQgetResult
 *		In pipeline mode, start processing the results of the next query in the queue.
 */
static void
pqPipelineProcessQueue(PGconn *conn)
{
	switch (conn->asyncStatus)
	{
		case PGASYNC_COPY_IN:
		case PGASYNC_COPY_OUT:
		case PGASYNC_COPY_BOTH:
		case PGASYNC_READY:
		case PGASYNC_READY_MORE:
		case PGASYNC_BUSY:
			/* client still has to process current query or results */
			return;

		case PGASYNC_IDLE:

			/*
			 * If we're in IDLE mode and there's some command in the queue,
			 * get us into PIPELINE_IDLE mode and process normally.  Otherwise
			 * there's nothing for us to do.
			 */
			if (conn->cmd_queue_head != NULL)
			{
				conn->asyncStatus = PGASYNC_PIPELINE_IDLE;
				break;
			}
			return;

		case PGASYNC_PIPELINE_IDLE:
			Assert(conn->pipelineStatus != PQ_PIPELINE_OFF);
			/* next query please */
			break;
	}

	/*
	 * Reset partial-result mode.  (Client has to set it up for each query, if
	 * desired.)
	 */
	conn->partialResMode = false;
	conn->singleRowMode = false;
	conn->maxChunkSize = 0;

	/*
	 * If there are no further commands to process in the queue, get us in
	 * "real idle" mode now.
	 */
	if (conn->cmd_queue_head == NULL)
	{
		conn->asyncStatus = PGASYNC_IDLE;
		return;
	}

	/*
	 * Reset the error state.  This and the next couple of steps correspond to
	 * what PQsendQueryStart didn't do for this query.
	 */
	pqClearConnErrorState(conn);

	/* Initialize async result-accumulation state */
	pqClearAsyncResult(conn);

	if (conn->pipelineStatus == PQ_PIPELINE_ABORTED &&
		conn->cmd_queue_head->queryclass != PGQUERY_SYNC)
	{
		/*
		 * In an aborted pipeline we don't get anything from the server for
		 * each result; we're just discarding commands from the queue until we
		 * get to the next sync from the server.
		 *
		 * The PGRES_PIPELINE_ABORTED results tell the client that its queries
		 * got aborted.
		 */
		conn->result = PQmakeEmptyPGresult(conn, PGRES_PIPELINE_ABORTED);
		if (!conn->result)
		{
			libpq_append_conn_error(conn, "out of memory");
			pqSaveErrorResult(conn);
			return;
		}
		conn->asyncStatus = PGASYNC_READY;
	}
	else
	{
		/* allow parsing to continue */
		conn->asyncStatus = PGASYNC_BUSY;
	}
}

/*
 * PQpipelineSync
 *		Send a Sync message as part of a pipeline, and flush to server
 */
int
PQpipelineSync(PGconn *conn)
{
	return pqPipelineSyncInternal(conn, true);
}

/*
 * PQsendPipelineSync
 *		Send a Sync message as part of a pipeline, without flushing to server
 */
int
PQsendPipelineSync(PGconn *conn)
{
	return pqPipelineSyncInternal(conn, false);
}

/*
 * Workhorse function for PQpipelineSync and PQsendPipelineSync.
 *
 * immediate_flush controls if the flush happens immediately after sending the
 * Sync message or not.
 */
static int
pqPipelineSyncInternal(PGconn *conn, bool immediate_flush)
{
	PGcmdQueueEntry *entry;

	if (!conn)
		return 0;

	if (conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "cannot send pipeline when not in pipeline mode");
		return 0;
	}

	switch (conn->asyncStatus)
	{
		case PGASYNC_COPY_IN:
		case PGASYNC_COPY_OUT:
		case PGASYNC_COPY_BOTH:
			/* should be unreachable */
			appendPQExpBufferStr(&conn->errorMessage,
								 "internal error: cannot send pipeline while in COPY\n");
			return 0;
		case PGASYNC_READY:
		case PGASYNC_READY_MORE:
		case PGASYNC_BUSY:
		case PGASYNC_IDLE:
		case PGASYNC_PIPELINE_IDLE:
			/* OK to send sync */
			break;
	}

	entry = pqAllocCmdQueueEntry(conn);
	if (entry == NULL)
		return 0;				/* error msg already set */

	entry->queryclass = PGQUERY_SYNC;
	entry->query = NULL;

	/* construct the Sync message */
	if (pqPutMsgStart(PqMsg_Sync, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/*
	 * Give the data a push.  In nonblock mode, don't complain if we're unable
	 * to send it all; PQgetResult() will do any additional flushing needed.
	 * If immediate_flush is disabled, the data is pushed if we are past the
	 * size threshold.
	 */
	if (immediate_flush)
	{
		if (pqFlush(conn) < 0)
			goto sendFailed;
	}
	else
	{
		if (pqPipelineFlush(conn) < 0)
			goto sendFailed;
	}

	/* OK, it's launched! */
	pqAppendCmdQueueEntry(conn, entry);

	return 1;

sendFailed:
	pqRecycleCmdQueueEntry(conn, entry);
	/* error message should be set up already */
	return 0;
}

/*
 * PQsendFlushRequest
 *		Send request for server to flush its buffer.  Useful in pipeline
 *		mode when a sync point is not desired.
 */
int
PQsendFlushRequest(PGconn *conn)
{
	if (!conn)
		return 0;

	/* Don't try to send if we know there's no live connection. */
	if (conn->status != CONNECTION_OK)
	{
		libpq_append_conn_error(conn, "no connection to the server");
		return 0;
	}

	/* Can't send while already busy, either, unless enqueuing for later */
	if (conn->asyncStatus != PGASYNC_IDLE &&
		conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "another command is already in progress");
		return 0;
	}

	if (pqPutMsgStart(PqMsg_Flush, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
	{
		return 0;
	}

	/*
	 * Give the data a push (in pipeline mode, only if we're past the size
	 * threshold).  In nonblock mode, don't complain if we're unable to send
	 * it all; PQgetResult() will do any additional flushing needed.
	 */
	if (pqPipelineFlush(conn) < 0)
		return 0;

	return 1;
}

/* ====== accessor funcs for PGresult ======== */

ExecStatusType
PQresultStatus(const PGresult *res)
{
	if (!res)
		return PGRES_FATAL_ERROR;
	return res->resultStatus;
}

char *
PQresStatus(ExecStatusType status)
{
	if ((unsigned int) status >= lengthof(pgresStatus))
		return libpq_gettext("invalid ExecStatusType code");
	return pgresStatus[status];
}

char *
PQresultErrorMessage(const PGresult *res)
{
	if (!res || !res->errMsg)
		return "";
	return res->errMsg;
}

char *
PQresultVerboseErrorMessage(const PGresult *res,
							PGVerbosity verbosity,
							PGContextVisibility show_context)
{
	PQExpBufferData workBuf;

	/*
	 * Because the caller is expected to free the result string, we must
	 * strdup any constant result.  We use plain strdup and document that
	 * callers should expect NULL if out-of-memory.
	 */
	if (!res ||
		(res->resultStatus != PGRES_FATAL_ERROR &&
		 res->resultStatus != PGRES_NONFATAL_ERROR))
		return strdup(libpq_gettext("PGresult is not an error result\n"));

	initPQExpBuffer(&workBuf);

	pqBuildErrorMessage3(&workBuf, res, verbosity, show_context);

	/* If insufficient memory to format the message, fail cleanly */
	if (PQExpBufferDataBroken(workBuf))
	{
		termPQExpBuffer(&workBuf);
		return strdup(libpq_gettext("out of memory\n"));
	}

	return workBuf.data;
}

char *
PQresultErrorField(const PGresult *res, int fieldcode)
{
	PGMessageField *pfield;

	if (!res)
		return NULL;
	for (pfield = res->errFields; pfield != NULL; pfield = pfield->next)
	{
		if (pfield->code == fieldcode)
			return pfield->contents;
	}
	return NULL;
}

int
PQntuples(const PGresult *res)
{
	if (!res)
		return 0;
	return res->ntups;
}

int
PQnfields(const PGresult *res)
{
	if (!res)
		return 0;
	return res->numAttributes;
}

int
PQbinaryTuples(const PGresult *res)
{
	if (!res)
		return 0;
	return res->binary;
}

/*
 * Helper routines to range-check field numbers and tuple numbers.
 * Return true if OK, false if not
 */

static int
check_field_number(const PGresult *res, int field_num)
{
	if (!res)
		return false;			/* no way to display error message... */
	if (field_num < 0 || field_num >= res->numAttributes)
	{
		pqInternalNotice(&res->noticeHooks,
						 "column number %d is out of range 0..%d",
						 field_num, res->numAttributes - 1);
		return false;
	}
	return true;
}

static int
check_tuple_field_number(const PGresult *res,
						 int tup_num, int field_num)
{
	if (!res)
		return false;			/* no way to display error message... */
	if (tup_num < 0 || tup_num >= res->ntups)
	{
		pqInternalNotice(&res->noticeHooks,
						 "row number %d is out of range 0..%d",
						 tup_num, res->ntups - 1);
		return false;
	}
	if (field_num < 0 || field_num >= res->numAttributes)
	{
		pqInternalNotice(&res->noticeHooks,
						 "column number %d is out of range 0..%d",
						 field_num, res->numAttributes - 1);
		return false;
	}
	return true;
}

static int
check_param_number(const PGresult *res, int param_num)
{
	if (!res)
		return false;			/* no way to display error message... */
	if (param_num < 0 || param_num >= res->numParameters)
	{
		pqInternalNotice(&res->noticeHooks,
						 "parameter number %d is out of range 0..%d",
						 param_num, res->numParameters - 1);
		return false;
	}

	return true;
}

/*
 * returns NULL if the field_num is invalid
 */
char *
PQfname(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return NULL;
	if (res->attDescs)
		return res->attDescs[field_num].name;
	else
		return NULL;
}

/*
 * PQfnumber: find column number given column name
 *
 * The column name is parsed as if it were in a SQL statement, including
 * case-folding and double-quote processing.  But note a possible gotcha:
 * downcasing in the frontend might follow different locale rules than
 * downcasing in the backend...
 *
 * Returns -1 if no match.  In the present backend it is also possible
 * to have multiple matches, in which case the first one is found.
 */
int
PQfnumber(const PGresult *res, const char *field_name)
{
	char	   *field_case;
	bool		in_quotes;
	bool		all_lower = true;
	const char *iptr;
	char	   *optr;
	int			i;

	if (!res)
		return -1;

	/*
	 * Note: it is correct to reject a zero-length input string; the proper
	 * input to match a zero-length field name would be "".
	 */
	if (field_name == NULL ||
		field_name[0] == '\0' ||
		res->attDescs == NULL)
		return -1;

	/*
	 * Check if we can avoid the strdup() and related work because the
	 * passed-in string wouldn't be changed before we do the check anyway.
	 */
	for (iptr = field_name; *iptr; iptr++)
	{
		char		c = *iptr;

		if (c == '"' || c != pg_tolower((unsigned char) c))
		{
			all_lower = false;
			break;
		}
	}

	if (all_lower)
		for (i = 0; i < res->numAttributes; i++)
			if (strcmp(field_name, res->attDescs[i].name) == 0)
				return i;

	/* Fall through to the normal check if that didn't work out. */

	/*
	 * Note: this code will not reject partially quoted strings, eg
	 * foo"BAR"foo will become fooBARfoo when it probably ought to be an error
	 * condition.
	 */
	field_case = strdup(field_name);
	if (field_case == NULL)
		return -1;				/* grotty */

	in_quotes = false;
	optr = field_case;
	for (iptr = field_case; *iptr; iptr++)
	{
		char		c = *iptr;

		if (in_quotes)
		{
			if (c == '"')
			{
				if (iptr[1] == '"')
				{
					/* doubled quotes become a single quote */
					*optr++ = '"';
					iptr++;
				}
				else
					in_quotes = false;
			}
			else
				*optr++ = c;
		}
		else if (c == '"')
			in_quotes = true;
		else
		{
			c = pg_tolower((unsigned char) c);
			*optr++ = c;
		}
	}
	*optr = '\0';

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
PQftable(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return InvalidOid;
	if (res->attDescs)
		return res->attDescs[field_num].tableid;
	else
		return InvalidOid;
}

int
PQftablecol(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return 0;
	if (res->attDescs)
		return res->attDescs[field_num].columnid;
	else
		return 0;
}

int
PQfformat(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return 0;
	if (res->attDescs)
		return res->attDescs[field_num].format;
	else
		return 0;
}

Oid
PQftype(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return InvalidOid;
	if (res->attDescs)
		return res->attDescs[field_num].typid;
	else
		return InvalidOid;
}

int
PQfsize(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return 0;
	if (res->attDescs)
		return res->attDescs[field_num].typlen;
	else
		return 0;
}

int
PQfmod(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
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
 * PQoidStatus -
 *	if the last command was an INSERT, return the oid string
 *	if not, return ""
 */
char *
PQoidStatus(const PGresult *res)
{
	/*
	 * This must be enough to hold the result. Don't laugh, this is better
	 * than what this function used to do.
	 */
	static char buf[24];

	size_t		len;

	if (!res || strncmp(res->cmdStatus, "INSERT ", 7) != 0)
		return "";

	len = strspn(res->cmdStatus + 7, "0123456789");
	if (len > sizeof(buf) - 1)
		len = sizeof(buf) - 1;
	memcpy(buf, res->cmdStatus + 7, len);
	buf[len] = '\0';

	return buf;
}

/*
 * PQoidValue -
 *	a perhaps preferable form of the above which just returns
 *	an Oid type
 */
Oid
PQoidValue(const PGresult *res)
{
	char	   *endptr = NULL;
	unsigned long result;

	if (!res ||
		strncmp(res->cmdStatus, "INSERT ", 7) != 0 ||
		res->cmdStatus[7] < '0' ||
		res->cmdStatus[7] > '9')
		return InvalidOid;

	result = strtoul(res->cmdStatus + 7, &endptr, 10);

	if (!endptr || (*endptr != ' ' && *endptr != '\0'))
		return InvalidOid;
	else
		return (Oid) result;
}


/*
 * PQcmdTuples -
 *	If the last command was INSERT/UPDATE/DELETE/MERGE/MOVE/FETCH/COPY,
 *	return a string containing the number of inserted/affected tuples.
 *	If not, return "".
 *
 *	XXX: this should probably return an int
 */
char *
PQcmdTuples(PGresult *res)
{
	char	   *p,
			   *c;

	if (!res)
		return "";

	if (strncmp(res->cmdStatus, "INSERT ", 7) == 0)
	{
		p = res->cmdStatus + 7;
		/* INSERT: skip oid and space */
		while (*p && *p != ' ')
			p++;
		if (*p == 0)
			goto interpret_error;	/* no space? */
		p++;
	}
	else if (strncmp(res->cmdStatus, "SELECT ", 7) == 0 ||
			 strncmp(res->cmdStatus, "DELETE ", 7) == 0 ||
			 strncmp(res->cmdStatus, "UPDATE ", 7) == 0)
		p = res->cmdStatus + 7;
	else if (strncmp(res->cmdStatus, "FETCH ", 6) == 0 ||
			 strncmp(res->cmdStatus, "MERGE ", 6) == 0)
		p = res->cmdStatus + 6;
	else if (strncmp(res->cmdStatus, "MOVE ", 5) == 0 ||
			 strncmp(res->cmdStatus, "COPY ", 5) == 0)
		p = res->cmdStatus + 5;
	else
		return "";

	/* check that we have an integer (at least one digit, nothing else) */
	for (c = p; *c; c++)
	{
		if (!isdigit((unsigned char) *c))
			goto interpret_error;
	}
	if (c == p)
		goto interpret_error;

	return p;

interpret_error:
	pqInternalNotice(&res->noticeHooks,
					 "could not interpret result from server: %s",
					 res->cmdStatus);
	return "";
}

/*
 * PQgetvalue:
 *	return the value of field 'field_num' of row 'tup_num'
 */
char *
PQgetvalue(const PGresult *res, int tup_num, int field_num)
{
	if (!check_tuple_field_number(res, tup_num, field_num))
		return NULL;
	return res->tuples[tup_num][field_num].value;
}

/* PQgetlength:
 *	returns the actual length of a field value in bytes.
 */
int
PQgetlength(const PGresult *res, int tup_num, int field_num)
{
	if (!check_tuple_field_number(res, tup_num, field_num))
		return 0;
	if (res->tuples[tup_num][field_num].len != NULL_LEN)
		return res->tuples[tup_num][field_num].len;
	else
		return 0;
}

/* PQgetisnull:
 *	returns the null status of a field value.
 */
int
PQgetisnull(const PGresult *res, int tup_num, int field_num)
{
	if (!check_tuple_field_number(res, tup_num, field_num))
		return 1;				/* pretend it is null */
	if (res->tuples[tup_num][field_num].len == NULL_LEN)
		return 1;
	else
		return 0;
}

/* PQnparams:
 *	returns the number of input parameters of a prepared statement.
 */
int
PQnparams(const PGresult *res)
{
	if (!res)
		return 0;
	return res->numParameters;
}

/* PQparamtype:
 *	returns type Oid of the specified statement parameter.
 */
Oid
PQparamtype(const PGresult *res, int param_num)
{
	if (!check_param_number(res, param_num))
		return InvalidOid;
	if (res->paramDescs)
		return res->paramDescs[param_num].typid;
	else
		return InvalidOid;
}


/* PQsetnonblocking:
 *	sets the PGconn's database connection non-blocking if the arg is true
 *	or makes it blocking if the arg is false, this will not protect
 *	you from PQexec(), you'll only be safe when using the non-blocking API.
 *	Needs to be called only on a connected database connection.
 */
int
PQsetnonblocking(PGconn *conn, int arg)
{
	bool		barg;

	if (!conn || conn->status == CONNECTION_BAD)
		return -1;

	barg = (arg ? true : false);

	/* early out if the socket is already in the state requested */
	if (barg == conn->nonblocking)
		return 0;

	/*
	 * to guarantee constancy for flushing/query/result-polling behavior we
	 * need to flush the send queue at this point in order to guarantee proper
	 * behavior. this is ok because either they are making a transition _from_
	 * or _to_ blocking mode, either way we can block them.
	 *
	 * Clear error state in case pqFlush adds to it, unless we're actively
	 * pipelining, in which case it seems best not to.
	 */
	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	/* if we are going from blocking to non-blocking flush here */
	if (pqFlush(conn))
		return -1;

	conn->nonblocking = barg;

	return 0;
}

/*
 * return the blocking status of the database connection
 *		true == nonblocking, false == blocking
 */
int
PQisnonblocking(const PGconn *conn)
{
	if (!conn || conn->status == CONNECTION_BAD)
		return false;
	return pqIsnonblocking(conn);
}

/* libpq is thread-safe? */
int
PQisthreadsafe(void)
{
	return true;
}


/* try to force data out, really only useful for non-blocking users */
int
PQflush(PGconn *conn)
{
	if (!conn || conn->status == CONNECTION_BAD)
		return -1;
	return pqFlush(conn);
}

/*
 * pqPipelineFlush
 *
 * In pipeline mode, data will be flushed only when the out buffer reaches the
 * threshold value.  In non-pipeline mode, it behaves as stock pqFlush.
 *
 * Returns 0 on success.
 */
static int
pqPipelineFlush(PGconn *conn)
{
	if ((conn->pipelineStatus != PQ_PIPELINE_ON) ||
		(conn->outCount >= OUTBUFFER_THRESHOLD))
		return pqFlush(conn);
	return 0;
}


/*
 *		PQfreemem - safely frees memory allocated
 *
 * Needed mostly by Win32, unless multithreaded DLL (/MD in VC6)
 * Used for freeing memory from PQescapeBytea()/PQunescapeBytea()
 */
void
PQfreemem(void *ptr)
{
	free(ptr);
}

/*
 * PQfreeNotify - free's the memory associated with a PGnotify
 *
 * This function is here only for binary backward compatibility.
 * New code should use PQfreemem().  A macro will automatically map
 * calls to PQfreemem.  It should be removed in the future.  bjm 2003-03-24
 */

#undef PQfreeNotify
void		PQfreeNotify(PGnotify *notify);

void
PQfreeNotify(PGnotify *notify)
{
	PQfreemem(notify);
}


/*
 * Escaping arbitrary strings to get valid SQL literal strings.
 *
 * Replaces "'" with "''", and if not std_strings, replaces "\" with "\\".
 *
 * length is the length of the source string.  (Note: if a terminating NUL
 * is encountered sooner, PQescapeString stops short of "length"; the behavior
 * is thus rather like strncpy.)
 *
 * For safety the buffer at "to" must be at least 2*length + 1 bytes long.
 * A terminating NUL character is added to the output string, whether the
 * input is NUL-terminated or not.
 *
 * Returns the actual length of the output (not counting the terminating NUL).
 */
static size_t
PQescapeStringInternal(PGconn *conn,
					   char *to, const char *from, size_t length,
					   int *error,
					   int encoding, bool std_strings)
{
	const char *source = from;
	char	   *target = to;
	size_t		remaining = strnlen(from, length);
	bool		already_complained = false;

	if (error)
		*error = 0;

	while (remaining > 0)
	{
		char		c = *source;
		int			charlen;
		int			i;

		/* Fast path for plain ASCII */
		if (!IS_HIGHBIT_SET(c))
		{
			/* Apply quoting if needed */
			if (SQL_STR_DOUBLE(c, !std_strings))
				*target++ = c;
			/* Copy the character */
			*target++ = c;
			source++;
			remaining--;
			continue;
		}

		/* Slow path for possible multibyte characters */
		charlen = pg_encoding_mblen_or_incomplete(encoding,
												  source, remaining);

		if (remaining < charlen ||
			pg_encoding_verifymbchar(encoding, source, charlen) == -1)
		{
			/*
			 * Multibyte character is invalid.  It's important to verify that
			 * as invalid multibyte characters could e.g. be used to "skip"
			 * over quote characters, e.g. when parsing
			 * character-by-character.
			 *
			 * Report an error if possible, and replace the character's first
			 * byte with an invalid sequence. The invalid sequence ensures
			 * that the escaped string will trigger an error on the
			 * server-side, even if we can't directly report an error here.
			 *
			 * This isn't *that* crucial when we can report an error to the
			 * caller; but if we can't or the caller ignores it, the caller
			 * will use this string unmodified and it needs to be safe for
			 * parsing.
			 *
			 * We know there's enough space for the invalid sequence because
			 * the "to" buffer needs to be at least 2 * length + 1 long, and
			 * at worst we're replacing a single input byte with two invalid
			 * bytes.
			 *
			 * It would be a bit faster to verify the whole string the first
			 * time we encounter a set highbit, but this way we can replace
			 * just the invalid data, which probably makes it easier for users
			 * to find the invalidly encoded portion of a larger string.
			 */
			if (error)
				*error = 1;
			if (conn && !already_complained)
			{
				if (remaining < charlen)
					libpq_append_conn_error(conn, "incomplete multibyte character");
				else
					libpq_append_conn_error(conn, "invalid multibyte character");
				/* Issue a complaint only once per string */
				already_complained = true;
			}

			pg_encoding_set_invalid(encoding, target);
			target += 2;

			/*
			 * Handle the following bytes as if this byte didn't exist. That's
			 * safer in case the subsequent bytes contain important characters
			 * for the caller (e.g. '>' in html).
			 */
			source++;
			remaining--;
		}
		else
		{
			/* Copy the character */
			for (i = 0; i < charlen; i++)
			{
				*target++ = *source++;
				remaining--;
			}
		}
	}

	/* Write the terminating NUL character. */
	*target = '\0';

	return target - to;
}

size_t
PQescapeStringConn(PGconn *conn,
				   char *to, const char *from, size_t length,
				   int *error)
{
	if (!conn)
	{
		/* force empty-string result */
		*to = '\0';
		if (error)
			*error = 1;
		return 0;
	}

	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	return PQescapeStringInternal(conn, to, from, length, error,
								  conn->client_encoding,
								  conn->std_strings);
}

size_t
PQescapeString(char *to, const char *from, size_t length)
{
	return PQescapeStringInternal(NULL, to, from, length, NULL,
								  static_client_encoding,
								  static_std_strings);
}


/*
 * Escape arbitrary strings.  If as_ident is true, we escape the result
 * as an identifier; if false, as a literal.  The result is returned in
 * a newly allocated buffer.  If we fail due to an encoding violation or out
 * of memory condition, we return NULL, storing an error message into conn.
 */
static char *
PQescapeInternal(PGconn *conn, const char *str, size_t len, bool as_ident)
{
	const char *s;
	char	   *result;
	char	   *rp;
	int			num_quotes = 0; /* single or double, depending on as_ident */
	int			num_backslashes = 0;
	size_t		input_len = strnlen(str, len);
	size_t		result_size;
	char		quote_char = as_ident ? '"' : '\'';
	bool		validated_mb = false;

	/* We must have a connection, else fail immediately. */
	if (!conn)
		return NULL;

	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	/*
	 * Scan the string for characters that must be escaped and for invalidly
	 * encoded data.
	 */
	s = str;
	for (size_t remaining = input_len; remaining > 0; remaining--, s++)
	{
		if (*s == quote_char)
			++num_quotes;
		else if (*s == '\\')
			++num_backslashes;
		else if (IS_HIGHBIT_SET(*s))
		{
			int			charlen;

			/* Slow path for possible multibyte characters */
			charlen = pg_encoding_mblen_or_incomplete(conn->client_encoding,
													  s, remaining);

			if (charlen > remaining)
			{
				/* Multibyte character overruns allowable length. */
				libpq_append_conn_error(conn, "incomplete multibyte character");
				return NULL;
			}

			/*
			 * If we haven't already, check that multibyte characters are
			 * valid. It's important to verify that as invalid multi-byte
			 * characters could e.g. be used to "skip" over quote characters,
			 * e.g. when parsing character-by-character.
			 *
			 * We check validity once, for the whole remainder of the string,
			 * when we first encounter any multi-byte character. Some
			 * encodings have optimized implementations for longer strings.
			 */
			if (!validated_mb)
			{
				if (pg_encoding_verifymbstr(conn->client_encoding, s, remaining)
					!= remaining)
				{
					libpq_append_conn_error(conn, "invalid multibyte character");
					return NULL;
				}
				validated_mb = true;
			}

			/* Adjust s, bearing in mind that for loop will increment it. */
			s += charlen - 1;
			remaining -= charlen - 1;
		}
	}

	/* Allocate output buffer. */
	result_size = input_len + num_quotes + 3;	/* two quotes, plus a NUL */
	if (!as_ident && num_backslashes > 0)
		result_size += num_backslashes + 2;
	result = rp = (char *) malloc(result_size);
	if (rp == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return NULL;
	}

	/*
	 * If we are escaping a literal that contains backslashes, we use the
	 * escape string syntax so that the result is correct under either value
	 * of standard_conforming_strings.  We also emit a leading space in this
	 * case, to guard against the possibility that the result might be
	 * interpolated immediately following an identifier.
	 */
	if (!as_ident && num_backslashes > 0)
	{
		*rp++ = ' ';
		*rp++ = 'E';
	}

	/* Opening quote. */
	*rp++ = quote_char;

	/*
	 * Use fast path if possible.
	 *
	 * We've already verified that the input string is well-formed in the
	 * current encoding.  If it contains no quotes and, in the case of
	 * literal-escaping, no backslashes, then we can just copy it directly to
	 * the output buffer, adding the necessary quotes.
	 *
	 * If not, we must rescan the input and process each character
	 * individually.
	 */
	if (num_quotes == 0 && (num_backslashes == 0 || as_ident))
	{
		memcpy(rp, str, input_len);
		rp += input_len;
	}
	else
	{
		s = str;
		for (size_t remaining = input_len; remaining > 0; remaining--, s++)
		{
			if (*s == quote_char || (!as_ident && *s == '\\'))
			{
				*rp++ = *s;
				*rp++ = *s;
			}
			else if (!IS_HIGHBIT_SET(*s))
				*rp++ = *s;
			else
			{
				int			i = pg_encoding_mblen(conn->client_encoding, s);

				while (1)
				{
					*rp++ = *s;
					if (--i == 0)
						break;
					remaining--;
					++s;		/* for loop will provide the final increment */
				}
			}
		}
	}

	/* Closing quote and terminating NUL. */
	*rp++ = quote_char;
	*rp = '\0';

	return result;
}

char *
PQescapeLiteral(PGconn *conn, const char *str, size_t len)
{
	return PQescapeInternal(conn, str, len, false);
}

char *
PQescapeIdentifier(PGconn *conn, const char *str, size_t len)
{
	return PQescapeInternal(conn, str, len, true);
}

/* HEX encoding support for bytea */
static const char hextbl[] = "0123456789abcdef";

static const int8 hexlookup[128] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static inline char
get_hex(char c)
{
	int			res = -1;

	if (c > 0 && c < 127)
		res = hexlookup[(unsigned char) c];

	return (char) res;
}


/*
 *		PQescapeBytea	- converts from binary string to the
 *		minimal encoding necessary to include the string in an SQL
 *		INSERT statement with a bytea type column as the target.
 *
 *		We can use either hex or escape (traditional) encoding.
 *		In escape mode, the following transformations are applied:
 *		'\0' == ASCII  0 == \000
 *		'\'' == ASCII 39 == ''
 *		'\\' == ASCII 92 == \\
 *		anything < 0x20, or > 0x7e ---> \ooo
 *										(where ooo is an octal expression)
 *
 *		If not std_strings, all backslashes sent to the output are doubled.
 */
static unsigned char *
PQescapeByteaInternal(PGconn *conn,
					  const unsigned char *from, size_t from_length,
					  size_t *to_length, bool std_strings, bool use_hex)
{
	const unsigned char *vp;
	unsigned char *rp;
	unsigned char *result;
	size_t		i;
	size_t		len;
	size_t		bslash_len = (std_strings ? 1 : 2);

	/*
	 * empty string has 1 char ('\0')
	 */
	len = 1;

	if (use_hex)
	{
		len += bslash_len + 1 + 2 * from_length;
	}
	else
	{
		vp = from;
		for (i = from_length; i > 0; i--, vp++)
		{
			if (*vp < 0x20 || *vp > 0x7e)
				len += bslash_len + 3;
			else if (*vp == '\'')
				len += 2;
			else if (*vp == '\\')
				len += bslash_len + bslash_len;
			else
				len++;
		}
	}

	*to_length = len;
	rp = result = (unsigned char *) malloc(len);
	if (rp == NULL)
	{
		if (conn)
			libpq_append_conn_error(conn, "out of memory");
		return NULL;
	}

	if (use_hex)
	{
		if (!std_strings)
			*rp++ = '\\';
		*rp++ = '\\';
		*rp++ = 'x';
	}

	vp = from;
	for (i = from_length; i > 0; i--, vp++)
	{
		unsigned char c = *vp;

		if (use_hex)
		{
			*rp++ = hextbl[(c >> 4) & 0xF];
			*rp++ = hextbl[c & 0xF];
		}
		else if (c < 0x20 || c > 0x7e)
		{
			if (!std_strings)
				*rp++ = '\\';
			*rp++ = '\\';
			*rp++ = (c >> 6) + '0';
			*rp++ = ((c >> 3) & 07) + '0';
			*rp++ = (c & 07) + '0';
		}
		else if (c == '\'')
		{
			*rp++ = '\'';
			*rp++ = '\'';
		}
		else if (c == '\\')
		{
			if (!std_strings)
			{
				*rp++ = '\\';
				*rp++ = '\\';
			}
			*rp++ = '\\';
			*rp++ = '\\';
		}
		else
			*rp++ = c;
	}
	*rp = '\0';

	return result;
}

unsigned char *
PQescapeByteaConn(PGconn *conn,
				  const unsigned char *from, size_t from_length,
				  size_t *to_length)
{
	if (!conn)
		return NULL;

	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	return PQescapeByteaInternal(conn, from, from_length, to_length,
								 conn->std_strings,
								 (conn->sversion >= 90000));
}

unsigned char *
PQescapeBytea(const unsigned char *from, size_t from_length, size_t *to_length)
{
	return PQescapeByteaInternal(NULL, from, from_length, to_length,
								 static_std_strings,
								 false /* can't use hex */ );
}


#define ISFIRSTOCTDIGIT(CH) ((CH) >= '0' && (CH) <= '3')
#define ISOCTDIGIT(CH) ((CH) >= '0' && (CH) <= '7')
#define OCTVAL(CH) ((CH) - '0')

/*
 *		PQunescapeBytea - converts the null terminated string representation
 *		of a bytea, strtext, into binary, filling a buffer. It returns a
 *		pointer to the buffer (or NULL on error), and the size of the
 *		buffer in retbuflen. The pointer may subsequently be used as an
 *		argument to the function PQfreemem.
 *
 *		The following transformations are made:
 *		\\	 == ASCII 92 == \
 *		\ooo == a byte whose value = ooo (ooo is an octal number)
 *		\x	 == x (x is any character not matched by the above transformations)
 */
unsigned char *
PQunescapeBytea(const unsigned char *strtext, size_t *retbuflen)
{
	size_t		strtextlen,
				buflen;
	unsigned char *buffer,
			   *tmpbuf;
	size_t		i,
				j;

	if (strtext == NULL)
		return NULL;

	strtextlen = strlen((const char *) strtext);

	if (strtext[0] == '\\' && strtext[1] == 'x')
	{
		const unsigned char *s;
		unsigned char *p;

		buflen = (strtextlen - 2) / 2;
		/* Avoid unportable malloc(0) */
		buffer = (unsigned char *) malloc(buflen > 0 ? buflen : 1);
		if (buffer == NULL)
			return NULL;

		s = strtext + 2;
		p = buffer;
		while (*s)
		{
			char		v1,
						v2;

			/*
			 * Bad input is silently ignored.  Note that this includes
			 * whitespace between hex pairs, which is allowed by byteain.
			 */
			v1 = get_hex(*s++);
			if (!*s || v1 == (char) -1)
				continue;
			v2 = get_hex(*s++);
			if (v2 != (char) -1)
				*p++ = (v1 << 4) | v2;
		}

		buflen = p - buffer;
	}
	else
	{
		/*
		 * Length of input is max length of output, but add one to avoid
		 * unportable malloc(0) if input is zero-length.
		 */
		buffer = (unsigned char *) malloc(strtextlen + 1);
		if (buffer == NULL)
			return NULL;

		for (i = j = 0; i < strtextlen;)
		{
			switch (strtext[i])
			{
				case '\\':
					i++;
					if (strtext[i] == '\\')
						buffer[j++] = strtext[i++];
					else
					{
						if ((ISFIRSTOCTDIGIT(strtext[i])) &&
							(ISOCTDIGIT(strtext[i + 1])) &&
							(ISOCTDIGIT(strtext[i + 2])))
						{
							int			byte;

							byte = OCTVAL(strtext[i++]);
							byte = (byte << 3) + OCTVAL(strtext[i++]);
							byte = (byte << 3) + OCTVAL(strtext[i++]);
							buffer[j++] = byte;
						}
					}

					/*
					 * Note: if we see '\' followed by something that isn't a
					 * recognized escape sequence, we loop around having done
					 * nothing except advance i.  Therefore the something will
					 * be emitted as ordinary data on the next cycle. Corner
					 * case: '\' at end of string will just be discarded.
					 */
					break;

				default:
					buffer[j++] = strtext[i++];
					break;
			}
		}
		buflen = j;				/* buflen is the length of the dequoted data */
	}

	/* Shrink the buffer to be no larger than necessary */
	/* +1 avoids unportable behavior when buflen==0 */
	tmpbuf = realloc(buffer, buflen + 1);

	/* It would only be a very brain-dead realloc that could fail, but... */
	if (!tmpbuf)
	{
		free(buffer);
		return NULL;
	}

	*retbuflen = buflen;
	return tmpbuf;
}
