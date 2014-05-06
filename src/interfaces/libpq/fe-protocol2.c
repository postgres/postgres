/*-------------------------------------------------------------------------
 *
 * fe-protocol2.c
 *	  functions that are specific to frontend/backend protocol version 2
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-protocol2.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <fcntl.h>

#include "libpq-fe.h"
#include "libpq-int.h"


#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#endif


static int	getRowDescriptions(PGconn *conn);
static int	getAnotherTuple(PGconn *conn, bool binary);
static int	pqGetErrorNotice2(PGconn *conn, bool isError);
static void checkXactStatus(PGconn *conn, const char *cmdTag);
static int	getNotify(PGconn *conn);


/*
 *		pqSetenvPoll
 *
 * Polls the process of passing the values of a standard set of environment
 * variables to the backend.
 */
PostgresPollingStatusType
pqSetenvPoll(PGconn *conn)
{
	PGresult   *res;

	if (conn == NULL || conn->status == CONNECTION_BAD)
		return PGRES_POLLING_FAILED;

	/* Check whether there are any data for us */
	switch (conn->setenv_state)
	{
			/* These are reading states */
		case SETENV_STATE_CLIENT_ENCODING_WAIT:
		case SETENV_STATE_OPTION_WAIT:
		case SETENV_STATE_QUERY1_WAIT:
		case SETENV_STATE_QUERY2_WAIT:
			{
				/* Load waiting data */
				int			n = pqReadData(conn);

				if (n < 0)
					goto error_return;
				if (n == 0)
					return PGRES_POLLING_READING;

				break;
			}

			/* These are writing states, so we just proceed. */
		case SETENV_STATE_CLIENT_ENCODING_SEND:
		case SETENV_STATE_OPTION_SEND:
		case SETENV_STATE_QUERY1_SEND:
		case SETENV_STATE_QUERY2_SEND:
			break;

			/* Should we raise an error if called when not active? */
		case SETENV_STATE_IDLE:
			return PGRES_POLLING_OK;

		default:
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext(
											"invalid setenv state %c, "
								 "probably indicative of memory corruption\n"
											),
							  conn->setenv_state);
			goto error_return;
	}

	/* We will loop here until there is nothing left to do in this call. */
	for (;;)
	{
		switch (conn->setenv_state)
		{
				/*
				 * The _CLIENT_ENCODING_SEND code is slightly different from
				 * _OPTION_SEND below (e.g., no getenv() call), which is why a
				 * different state is used.
				 */
			case SETENV_STATE_CLIENT_ENCODING_SEND:
				{
					char		setQuery[100];	/* note length limit in
												 * sprintf below */
					const char *val = conn->client_encoding_initial;

					if (val)
					{
						if (pg_strcasecmp(val, "default") == 0)
							sprintf(setQuery, "SET client_encoding = DEFAULT");
						else
							sprintf(setQuery, "SET client_encoding = '%.60s'",
									val);
#ifdef CONNECTDEBUG
						fprintf(stderr,
								"Sending client_encoding with %s\n",
								setQuery);
#endif
						if (!PQsendQuery(conn, setQuery))
							goto error_return;

						conn->setenv_state = SETENV_STATE_CLIENT_ENCODING_WAIT;
					}
					else
						conn->setenv_state = SETENV_STATE_OPTION_SEND;
					break;
				}

			case SETENV_STATE_OPTION_SEND:
				{
					/*
					 * Send SET commands for stuff directed by Environment
					 * Options.  Note: we assume that SET commands won't start
					 * transaction blocks, even in a 7.3 server with
					 * autocommit off.
					 */
					char		setQuery[100];	/* note length limit in
												 * sprintf below */

					if (conn->next_eo->envName)
					{
						const char *val;

						if ((val = getenv(conn->next_eo->envName)))
						{
							if (pg_strcasecmp(val, "default") == 0)
								sprintf(setQuery, "SET %s = DEFAULT",
										conn->next_eo->pgName);
							else
								sprintf(setQuery, "SET %s = '%.60s'",
										conn->next_eo->pgName, val);
#ifdef CONNECTDEBUG
							fprintf(stderr,
								  "Use environment variable %s to send %s\n",
									conn->next_eo->envName, setQuery);
#endif
							if (!PQsendQuery(conn, setQuery))
								goto error_return;

							conn->setenv_state = SETENV_STATE_OPTION_WAIT;
						}
						else
							conn->next_eo++;
					}
					else
					{
						/* No more options to send, so move on to querying */
						conn->setenv_state = SETENV_STATE_QUERY1_SEND;
					}
					break;
				}

			case SETENV_STATE_CLIENT_ENCODING_WAIT:
				{
					if (PQisBusy(conn))
						return PGRES_POLLING_READING;

					res = PQgetResult(conn);

					if (res)
					{
						if (PQresultStatus(res) != PGRES_COMMAND_OK)
						{
							PQclear(res);
							goto error_return;
						}
						PQclear(res);
						/* Keep reading until PQgetResult returns NULL */
					}
					else
					{
						/* Query finished, so send the next option */
						conn->setenv_state = SETENV_STATE_OPTION_SEND;
					}
					break;
				}

			case SETENV_STATE_OPTION_WAIT:
				{
					if (PQisBusy(conn))
						return PGRES_POLLING_READING;

					res = PQgetResult(conn);

					if (res)
					{
						if (PQresultStatus(res) != PGRES_COMMAND_OK)
						{
							PQclear(res);
							goto error_return;
						}
						PQclear(res);
						/* Keep reading until PQgetResult returns NULL */
					}
					else
					{
						/* Query finished, so send the next option */
						conn->next_eo++;
						conn->setenv_state = SETENV_STATE_OPTION_SEND;
					}
					break;
				}

			case SETENV_STATE_QUERY1_SEND:
				{
					/*
					 * Issue query to get information we need.  Here we must
					 * use begin/commit in case autocommit is off by default
					 * in a 7.3 server.
					 *
					 * Note: version() exists in all protocol-2.0-supporting
					 * backends.  In 7.3 it would be safer to write
					 * pg_catalog.version(), but we can't do that without
					 * causing problems on older versions.
					 */
					if (!PQsendQuery(conn, "begin; select version(); end"))
						goto error_return;

					conn->setenv_state = SETENV_STATE_QUERY1_WAIT;
					return PGRES_POLLING_READING;
				}

			case SETENV_STATE_QUERY1_WAIT:
				{
					if (PQisBusy(conn))
						return PGRES_POLLING_READING;

					res = PQgetResult(conn);

					if (res)
					{
						char	   *val;

						if (PQresultStatus(res) == PGRES_COMMAND_OK)
						{
							/* ignore begin/commit command results */
							PQclear(res);
							continue;
						}

						if (PQresultStatus(res) != PGRES_TUPLES_OK ||
							PQntuples(res) != 1)
						{
							PQclear(res);
							goto error_return;
						}

						/*
						 * Extract server version and save as if
						 * ParameterStatus
						 */
						val = PQgetvalue(res, 0, 0);
						if (val && strncmp(val, "PostgreSQL ", 11) == 0)
						{
							char	   *ptr;

							/* strip off PostgreSQL part */
							val += 11;

							/*
							 * strip off platform part (scribbles on result,
							 * naughty naughty)
							 */
							ptr = strchr(val, ' ');
							if (ptr)
								*ptr = '\0';

							pqSaveParameterStatus(conn, "server_version",
												  val);
						}

						PQclear(res);
						/* Keep reading until PQgetResult returns NULL */
					}
					else
					{
						/* Query finished, move to next */
						conn->setenv_state = SETENV_STATE_QUERY2_SEND;
					}
					break;
				}

			case SETENV_STATE_QUERY2_SEND:
				{
					const char *query;

					/*
					 * pg_client_encoding does not exist in pre-7.2 servers.
					 * So we need to be prepared for an error here.  Do *not*
					 * start a transaction block, except in 7.3 servers where
					 * we need to prevent autocommit-off from starting a
					 * transaction anyway.
					 */
					if (conn->sversion >= 70300 &&
						conn->sversion < 70400)
						query = "begin; select pg_catalog.pg_client_encoding(); end";
					else
						query = "select pg_client_encoding()";
					if (!PQsendQuery(conn, query))
						goto error_return;

					conn->setenv_state = SETENV_STATE_QUERY2_WAIT;
					return PGRES_POLLING_READING;
				}

			case SETENV_STATE_QUERY2_WAIT:
				{
					if (PQisBusy(conn))
						return PGRES_POLLING_READING;

					res = PQgetResult(conn);

					if (res)
					{
						const char *val;

						if (PQresultStatus(res) == PGRES_COMMAND_OK)
						{
							/* ignore begin/commit command results */
							PQclear(res);
							continue;
						}

						if (PQresultStatus(res) == PGRES_TUPLES_OK &&
							PQntuples(res) == 1)
						{
							/* Extract client encoding and save it */
							val = PQgetvalue(res, 0, 0);
							if (val && *val)	/* null should not happen, but */
								pqSaveParameterStatus(conn, "client_encoding",
													  val);
						}
						else
						{
							/*
							 * Error: presumably function not available, so
							 * use PGCLIENTENCODING or SQL_ASCII as the
							 * fallback.
							 */
							val = getenv("PGCLIENTENCODING");
							if (val && *val)
								pqSaveParameterStatus(conn, "client_encoding",
													  val);
							else
								pqSaveParameterStatus(conn, "client_encoding",
													  "SQL_ASCII");
						}

						PQclear(res);
						/* Keep reading until PQgetResult returns NULL */
					}
					else
					{
						/* Query finished, so we're done */
						conn->setenv_state = SETENV_STATE_IDLE;
						return PGRES_POLLING_OK;
					}
					break;
				}

			default:
				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("invalid state %c, "
							   "probably indicative of memory corruption\n"),
								  conn->setenv_state);
				goto error_return;
		}
	}

	/* Unreachable */

error_return:
	conn->setenv_state = SETENV_STATE_IDLE;
	return PGRES_POLLING_FAILED;
}


/*
 * parseInput: if appropriate, parse input data from backend
 * until input is exhausted or a stopping state is reached.
 * Note that this function will NOT attempt to read more data from the backend.
 */
void
pqParseInput2(PGconn *conn)
{
	char		id;

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
		 * NOTIFY and NOTICE messages can happen in any state besides COPY
		 * OUT; always process them right away.
		 *
		 * Most other messages should only be processed while in BUSY state.
		 * (In particular, in READY state we hold off further parsing until
		 * the application collects the current PGresult.)
		 *
		 * However, if the state is IDLE then we got trouble; we need to deal
		 * with the unexpected message somehow.
		 */
		if (id == 'A')
		{
			if (getNotify(conn))
				return;
		}
		else if (id == 'N')
		{
			if (pqGetErrorNotice2(conn, false))
				return;
		}
		else if (conn->asyncStatus != PGASYNC_BUSY)
		{
			/* If not IDLE state, just wait ... */
			if (conn->asyncStatus != PGASYNC_IDLE)
				return;

			/*
			 * Unexpected message in IDLE state; need to recover somehow.
			 * ERROR messages are displayed using the notice processor;
			 * anything else is just dropped on the floor after displaying a
			 * suitable warning notice.  (An ERROR is very possibly the
			 * backend telling us why it is about to close the connection, so
			 * we don't want to just discard it...)
			 */
			if (id == 'E')
			{
				if (pqGetErrorNotice2(conn, false /* treat as notice */ ))
					return;
			}
			else
			{
				pqInternalNotice(&conn->noticeHooks,
						"message type 0x%02x arrived from server while idle",
								 id);
				/* Discard the unexpected message; good idea?? */
				conn->inStart = conn->inEnd;
				break;
			}
		}
		else
		{
			/*
			 * In BUSY state, we can process everything.
			 */
			switch (id)
			{
				case 'C':		/* command complete */
					if (pqGets(&conn->workBuffer, conn))
						return;
					if (conn->result == NULL)
					{
						conn->result = PQmakeEmptyPGresult(conn,
														   PGRES_COMMAND_OK);
						if (!conn->result)
							return;
					}
					strlcpy(conn->result->cmdStatus, conn->workBuffer.data,
							CMDSTATUS_LEN);
					checkXactStatus(conn, conn->workBuffer.data);
					conn->asyncStatus = PGASYNC_READY;
					break;
				case 'E':		/* error return */
					if (pqGetErrorNotice2(conn, true))
						return;
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
						pqInternalNotice(&conn->noticeHooks,
										 "unexpected character %c following empty query response (\"I\" message)",
										 id);
					if (conn->result == NULL)
						conn->result = PQmakeEmptyPGresult(conn,
														   PGRES_EMPTY_QUERY);
					conn->asyncStatus = PGASYNC_READY;
					break;
				case 'K':		/* secret key data from the backend */

					/*
					 * This is expected only during backend startup, but it's
					 * just as easy to handle it as part of the main loop.
					 * Save the data and continue processing.
					 */
					if (pqGetInt(&(conn->be_pid), 4, conn))
						return;
					if (pqGetInt(&(conn->be_key), 4, conn))
						return;
					break;
				case 'P':		/* synchronous (normal) portal */
					if (pqGets(&conn->workBuffer, conn))
						return;
					/* We pretty much ignore this message type... */
					break;
				case 'T':		/* row descriptions (start of query results) */
					if (conn->result == NULL)
					{
						/* First 'T' in a query sequence */
						if (getRowDescriptions(conn))
							return;
						/* getRowDescriptions() moves inStart itself */
						continue;
					}
					else
					{
						/*
						 * A new 'T' message is treated as the start of
						 * another PGresult.  (It is not clear that this is
						 * really possible with the current backend.) We stop
						 * parsing until the application accepts the current
						 * result.
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
						/* getAnotherTuple() moves inStart itself */
						continue;
					}
					else
					{
						pqInternalNotice(&conn->noticeHooks,
										 "server sent data (\"D\" message) without prior row description (\"T\" message)");
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
						/* getAnotherTuple() moves inStart itself */
						continue;
					}
					else
					{
						pqInternalNotice(&conn->noticeHooks,
										 "server sent binary data (\"B\" message) without prior row description (\"T\" message)");
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

					/*
					 * Don't need to process CopyBothResponse here because it
					 * never arrives from the server during protocol 2.0.
					 */
				default:
					printfPQExpBuffer(&conn->errorMessage,
									  libpq_gettext(
													"unexpected response from server; first received character was \"%c\"\n"),
									  id);
					/* build an error result holding the error message */
					pqSaveErrorResult(conn);
					/* Discard the unexpected message; good idea?? */
					conn->inStart = conn->inEnd;
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
 * Returns: 0 if completed message, EOF if error or not enough data
 * received yet.
 *
 * Note that if we run out of data, we have to suspend and reprocess
 * the message after more data is received.  Otherwise, conn->inStart
 * must get advanced past the processed data.
 */
static int
getRowDescriptions(PGconn *conn)
{
	PGresult   *result;
	int			nfields;
	const char *errmsg;
	int			i;

	result = PQmakeEmptyPGresult(conn, PGRES_TUPLES_OK);
	if (!result)
	{
		errmsg = NULL;			/* means "out of memory", see below */
		goto advance_and_error;
	}

	/* parseInput already read the 'T' label. */
	/* the next two bytes are the number of fields	*/
	if (pqGetInt(&(result->numAttributes), 2, conn))
		goto EOFexit;
	nfields = result->numAttributes;

	/* allocate space for the attribute descriptors */
	if (nfields > 0)
	{
		result->attDescs = (PGresAttDesc *)
			pqResultAlloc(result, nfields * sizeof(PGresAttDesc), TRUE);
		if (!result->attDescs)
		{
			errmsg = NULL;		/* means "out of memory", see below */
			goto advance_and_error;
		}
		MemSet(result->attDescs, 0, nfields * sizeof(PGresAttDesc));
	}

	/* get type info */
	for (i = 0; i < nfields; i++)
	{
		int			typid;
		int			typlen;
		int			atttypmod;

		if (pqGets(&conn->workBuffer, conn) ||
			pqGetInt(&typid, 4, conn) ||
			pqGetInt(&typlen, 2, conn) ||
			pqGetInt(&atttypmod, 4, conn))
			goto EOFexit;

		/*
		 * Since pqGetInt treats 2-byte integers as unsigned, we need to
		 * coerce the result to signed form.
		 */
		typlen = (int) ((int16) typlen);

		result->attDescs[i].name = pqResultStrdup(result,
												  conn->workBuffer.data);
		if (!result->attDescs[i].name)
		{
			errmsg = NULL;		/* means "out of memory", see below */
			goto advance_and_error;
		}
		result->attDescs[i].tableid = 0;
		result->attDescs[i].columnid = 0;
		result->attDescs[i].format = 0;
		result->attDescs[i].typid = typid;
		result->attDescs[i].typlen = typlen;
		result->attDescs[i].atttypmod = atttypmod;
	}

	/* Success! */
	conn->result = result;

	/* Advance inStart to show that the "T" message has been processed. */
	conn->inStart = conn->inCursor;

	/*
	 * We could perform additional setup for the new result set here, but for
	 * now there's nothing else to do.
	 */

	/* And we're done. */
	return 0;

advance_and_error:

	/*
	 * Discard the failed message.  Unfortunately we don't know for sure where
	 * the end is, so just throw away everything in the input buffer. This is
	 * not very desirable but it's the best we can do in protocol v2.
	 */
	conn->inStart = conn->inEnd;

	/*
	 * Replace partially constructed result with an error result. First
	 * discard the old result to try to win back some memory.
	 */
	pqClearAsyncResult(conn);

	/*
	 * If preceding code didn't provide an error message, assume "out of
	 * memory" was meant.  The advantage of having this special case is that
	 * freeing the old result first greatly improves the odds that gettext()
	 * will succeed in providing a translation.
	 */
	if (!errmsg)
		errmsg = libpq_gettext("out of memory for query result");

	printfPQExpBuffer(&conn->errorMessage, "%s\n", errmsg);

	/*
	 * XXX: if PQmakeEmptyPGresult() fails, there's probably not much we can
	 * do to recover...
	 */
	conn->result = PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
	conn->asyncStatus = PGASYNC_READY;

EOFexit:
	if (result && result != conn->result)
		PQclear(result);
	return EOF;
}

/*
 * parseInput subroutine to read a 'B' or 'D' (row data) message.
 * We fill rowbuf with column pointers and then call the row processor.
 * Returns: 0 if completed message, EOF if error or not enough data
 * received yet.
 *
 * Note that if we run out of data, we have to suspend and reprocess
 * the message after more data is received.  Otherwise, conn->inStart
 * must get advanced past the processed data.
 */
static int
getAnotherTuple(PGconn *conn, bool binary)
{
	PGresult   *result = conn->result;
	int			nfields = result->numAttributes;
	const char *errmsg;
	PGdataValue *rowbuf;

	/* the backend sends us a bitmap of which attributes are null */
	char		std_bitmap[64]; /* used unless it doesn't fit */
	char	   *bitmap = std_bitmap;
	int			i;
	size_t		nbytes;			/* the number of bytes in bitmap  */
	char		bmap;			/* One byte of the bitmap */
	int			bitmap_index;	/* Its index */
	int			bitcnt;			/* number of bits examined in current byte */
	int			vlen;			/* length of the current field value */

	/* Resize row buffer if needed */
	rowbuf = conn->rowBuf;
	if (nfields > conn->rowBufLen)
	{
		rowbuf = (PGdataValue *) realloc(rowbuf,
										 nfields * sizeof(PGdataValue));
		if (!rowbuf)
		{
			errmsg = NULL;		/* means "out of memory", see below */
			goto advance_and_error;
		}
		conn->rowBuf = rowbuf;
		conn->rowBufLen = nfields;
	}

	/* Save format specifier */
	result->binary = binary;

	/*
	 * If it's binary, fix the column format indicators.  We assume the
	 * backend will consistently send either B or D, not a mix.
	 */
	if (binary)
	{
		for (i = 0; i < nfields; i++)
			result->attDescs[i].format = 1;
	}

	/* Get the null-value bitmap */
	nbytes = (nfields + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
	/* malloc() only for unusually large field counts... */
	if (nbytes > sizeof(std_bitmap))
	{
		bitmap = (char *) malloc(nbytes);
		if (!bitmap)
		{
			errmsg = NULL;		/* means "out of memory", see below */
			goto advance_and_error;
		}
	}

	if (pqGetnchar(bitmap, nbytes, conn))
		goto EOFexit;

	/* Scan the fields */
	bitmap_index = 0;
	bmap = bitmap[bitmap_index];
	bitcnt = 0;

	for (i = 0; i < nfields; i++)
	{
		/* get the value length */
		if (!(bmap & 0200))
			vlen = NULL_LEN;
		else if (pqGetInt(&vlen, 4, conn))
			goto EOFexit;
		else
		{
			if (!binary)
				vlen = vlen - 4;
			if (vlen < 0)
				vlen = 0;
		}
		rowbuf[i].len = vlen;

		/*
		 * rowbuf[i].value always points to the next address in the data
		 * buffer even if the value is NULL.  This allows row processors to
		 * estimate data sizes more easily.
		 */
		rowbuf[i].value = conn->inBuffer + conn->inCursor;

		/* Skip over the data value */
		if (vlen > 0)
		{
			if (pqSkipnchar(vlen, conn))
				goto EOFexit;
		}

		/* advance the bitmap stuff */
		bitcnt++;
		if (bitcnt == BITS_PER_BYTE)
		{
			bitmap_index++;
			bmap = bitmap[bitmap_index];
			bitcnt = 0;
		}
		else
			bmap <<= 1;
	}

	/* Release bitmap now if we allocated it */
	if (bitmap != std_bitmap)
		free(bitmap);
	bitmap = NULL;

	/* Advance inStart to show that the "D" message has been processed. */
	conn->inStart = conn->inCursor;

	/* Process the collected row */
	errmsg = NULL;
	if (pqRowProcessor(conn, &errmsg))
		return 0;				/* normal, successful exit */

	goto set_error_result;		/* pqRowProcessor failed, report it */

advance_and_error:

	/*
	 * Discard the failed message.  Unfortunately we don't know for sure where
	 * the end is, so just throw away everything in the input buffer. This is
	 * not very desirable but it's the best we can do in protocol v2.
	 */
	conn->inStart = conn->inEnd;

set_error_result:

	/*
	 * Replace partially constructed result with an error result. First
	 * discard the old result to try to win back some memory.
	 */
	pqClearAsyncResult(conn);

	/*
	 * If preceding code didn't provide an error message, assume "out of
	 * memory" was meant.  The advantage of having this special case is that
	 * freeing the old result first greatly improves the odds that gettext()
	 * will succeed in providing a translation.
	 */
	if (!errmsg)
		errmsg = libpq_gettext("out of memory for query result");

	printfPQExpBuffer(&conn->errorMessage, "%s\n", errmsg);

	/*
	 * XXX: if PQmakeEmptyPGresult() fails, there's probably not much we can
	 * do to recover...
	 */
	conn->result = PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
	conn->asyncStatus = PGASYNC_READY;

EOFexit:
	if (bitmap != NULL && bitmap != std_bitmap)
		free(bitmap);
	return EOF;
}


/*
 * Attempt to read an Error or Notice response message.
 * This is possible in several places, so we break it out as a subroutine.
 * Entry: 'E' or 'N' message type has already been consumed.
 * Exit: returns 0 if successfully consumed message.
 *		 returns EOF if not enough data.
 */
static int
pqGetErrorNotice2(PGconn *conn, bool isError)
{
	PGresult   *res = NULL;
	PQExpBufferData workBuf;
	char	   *startp;
	char	   *splitp;

	/*
	 * Since the message might be pretty long, we create a temporary
	 * PQExpBuffer rather than using conn->workBuffer.  workBuffer is intended
	 * for stuff that is expected to be short.
	 */
	initPQExpBuffer(&workBuf);
	if (pqGets(&workBuf, conn))
		goto failure;

	/*
	 * Make a PGresult to hold the message.  We temporarily lie about the
	 * result status, so that PQmakeEmptyPGresult doesn't uselessly copy
	 * conn->errorMessage.
	 */
	res = PQmakeEmptyPGresult(conn, PGRES_EMPTY_QUERY);
	if (!res)
		goto failure;
	res->resultStatus = isError ? PGRES_FATAL_ERROR : PGRES_NONFATAL_ERROR;
	res->errMsg = pqResultStrdup(res, workBuf.data);
	if (!res->errMsg)
		goto failure;

	/*
	 * Break the message into fields.  We can't do very much here, but we can
	 * split the severity code off, and remove trailing newlines. Also, we use
	 * the heuristic that the primary message extends only to the first
	 * newline --- anything after that is detail message.  (In some cases it'd
	 * be better classed as hint, but we can hardly be expected to guess that
	 * here.)
	 */
	while (workBuf.len > 0 && workBuf.data[workBuf.len - 1] == '\n')
		workBuf.data[--workBuf.len] = '\0';
	splitp = strstr(workBuf.data, ":  ");
	if (splitp)
	{
		/* what comes before the colon is severity */
		*splitp = '\0';
		pqSaveMessageField(res, PG_DIAG_SEVERITY, workBuf.data);
		startp = splitp + 3;
	}
	else
	{
		/* can't find a colon?  oh well... */
		startp = workBuf.data;
	}
	splitp = strchr(startp, '\n');
	if (splitp)
	{
		/* what comes before the newline is primary message */
		*splitp++ = '\0';
		pqSaveMessageField(res, PG_DIAG_MESSAGE_PRIMARY, startp);
		/* the rest is detail; strip any leading whitespace */
		while (*splitp && isspace((unsigned char) *splitp))
			splitp++;
		pqSaveMessageField(res, PG_DIAG_MESSAGE_DETAIL, splitp);
	}
	else
	{
		/* single-line message, so all primary */
		pqSaveMessageField(res, PG_DIAG_MESSAGE_PRIMARY, startp);
	}

	/*
	 * Either save error as current async result, or just emit the notice.
	 * Also, if it's an error and we were in a transaction block, assume the
	 * server has now gone to error-in-transaction state.
	 */
	if (isError)
	{
		pqClearAsyncResult(conn);
		conn->result = res;
		resetPQExpBuffer(&conn->errorMessage);
		appendPQExpBufferStr(&conn->errorMessage, res->errMsg);
		if (conn->xactStatus == PQTRANS_INTRANS)
			conn->xactStatus = PQTRANS_INERROR;
	}
	else
	{
		if (res->noticeHooks.noticeRec != NULL)
			(*res->noticeHooks.noticeRec) (res->noticeHooks.noticeRecArg, res);
		PQclear(res);
	}

	termPQExpBuffer(&workBuf);
	return 0;

failure:
	if (res)
		PQclear(res);
	termPQExpBuffer(&workBuf);
	return EOF;
}

/*
 * checkXactStatus - attempt to track transaction-block status of server
 *
 * This is called each time we receive a command-complete message.  By
 * watching for messages from BEGIN/COMMIT/ROLLBACK commands, we can do
 * a passable job of tracking the server's xact status.  BUT: this does
 * not work at all on 7.3 servers with AUTOCOMMIT OFF.  (Man, was that
 * feature ever a mistake.)  Caveat user.
 *
 * The tags known here are all those used as far back as 7.0; is it worth
 * adding those from even-older servers?
 */
static void
checkXactStatus(PGconn *conn, const char *cmdTag)
{
	if (strcmp(cmdTag, "BEGIN") == 0)
		conn->xactStatus = PQTRANS_INTRANS;
	else if (strcmp(cmdTag, "COMMIT") == 0)
		conn->xactStatus = PQTRANS_IDLE;
	else if (strcmp(cmdTag, "ROLLBACK") == 0)
		conn->xactStatus = PQTRANS_IDLE;
	else if (strcmp(cmdTag, "START TRANSACTION") == 0)	/* 7.3 only */
		conn->xactStatus = PQTRANS_INTRANS;

	/*
	 * Normally we get into INERROR state by detecting an Error message.
	 * However, if we see one of these tags then we know for sure the server
	 * is in abort state ...
	 */
	else if (strcmp(cmdTag, "*ABORT STATE*") == 0)		/* pre-7.3 only */
		conn->xactStatus = PQTRANS_INERROR;
}

/*
 * Attempt to read a Notify response message.
 * This is possible in several places, so we break it out as a subroutine.
 * Entry: 'A' message type and length have already been consumed.
 * Exit: returns 0 if successfully consumed Notify message.
 *		 returns EOF if not enough data.
 */
static int
getNotify(PGconn *conn)
{
	int			be_pid;
	int			nmlen;
	PGnotify   *newNotify;

	if (pqGetInt(&be_pid, 4, conn))
		return EOF;
	if (pqGets(&conn->workBuffer, conn))
		return EOF;

	/*
	 * Store the relation name right after the PQnotify structure so it can
	 * all be freed at once.  We don't use NAMEDATALEN because we don't want
	 * to tie this interface to a specific server name length.
	 */
	nmlen = strlen(conn->workBuffer.data);
	newNotify = (PGnotify *) malloc(sizeof(PGnotify) + nmlen + 1);
	if (newNotify)
	{
		newNotify->relname = (char *) newNotify + sizeof(PGnotify);
		strcpy(newNotify->relname, conn->workBuffer.data);
		/* fake up an empty-string extra field */
		newNotify->extra = newNotify->relname + nmlen;
		newNotify->be_pid = be_pid;
		newNotify->next = NULL;
		if (conn->notifyTail)
			conn->notifyTail->next = newNotify;
		else
			conn->notifyHead = newNotify;
		conn->notifyTail = newNotify;
	}

	return 0;
}


/*
 * PQgetCopyData - read a row of data from the backend during COPY OUT
 *
 * If successful, sets *buffer to point to a malloc'd row of data, and
 * returns row length (always > 0) as result.
 * Returns 0 if no row available yet (only possible if async is true),
 * -1 if end of copy (consult PQgetResult), or -2 if error (consult
 * PQerrorMessage).
 */
int
pqGetCopyData2(PGconn *conn, char **buffer, int async)
{
	bool		found;
	int			msgLength;

	for (;;)
	{
		/*
		 * Do we have a complete line of data?
		 */
		conn->inCursor = conn->inStart;
		found = false;
		while (conn->inCursor < conn->inEnd)
		{
			char		c = conn->inBuffer[conn->inCursor++];

			if (c == '\n')
			{
				found = true;
				break;
			}
		}
		if (!found)
			goto nodata;
		msgLength = conn->inCursor - conn->inStart;

		/*
		 * If it's the end-of-data marker, consume it, exit COPY_OUT mode, and
		 * let caller read status with PQgetResult().
		 */
		if (msgLength == 3 &&
			strncmp(&conn->inBuffer[conn->inStart], "\\.\n", 3) == 0)
		{
			conn->inStart = conn->inCursor;
			conn->asyncStatus = PGASYNC_BUSY;
			return -1;
		}

		/*
		 * Pass the line back to the caller.
		 */
		*buffer = (char *) malloc(msgLength + 1);
		if (*buffer == NULL)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("out of memory\n"));
			return -2;
		}
		memcpy(*buffer, &conn->inBuffer[conn->inStart], msgLength);
		(*buffer)[msgLength] = '\0';	/* Add terminating null */

		/* Mark message consumed */
		conn->inStart = conn->inCursor;

		return msgLength;

nodata:
		/* Don't block if async read requested */
		if (async)
			return 0;
		/* Need to load more data */
		if (pqWait(TRUE, FALSE, conn) ||
			pqReadData(conn) < 0)
			return -2;
	}
}


/*
 * PQgetline - gets a newline-terminated string from the backend.
 *
 * See fe-exec.c for documentation.
 */
int
pqGetline2(PGconn *conn, char *s, int maxlen)
{
	int			result = 1;		/* return value if buffer overflows */

	if (conn->sock < 0 ||
		conn->asyncStatus != PGASYNC_COPY_OUT)
	{
		*s = '\0';
		return EOF;
	}

	/*
	 * Since this is a purely synchronous routine, we don't bother to maintain
	 * conn->inCursor; there is no need to back up.
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
 * PQgetlineAsync - gets a COPY data row without blocking.
 *
 * See fe-exec.c for documentation.
 */
int
pqGetlineAsync2(PGconn *conn, char *buffer, int bufsize)
{
	int			avail;

	if (conn->asyncStatus != PGASYNC_COPY_OUT)
		return -1;				/* we are not doing a copy... */

	/*
	 * Move data from libpq's buffer to the caller's. We want to accept data
	 * only in units of whole lines, not partial lines.  This ensures that we
	 * can recognize the terminator line "\\.\n".  (Otherwise, if it happened
	 * to cross a packet/buffer boundary, we might hand the first one or two
	 * characters off to the caller, which we shouldn't.)
	 */

	conn->inCursor = conn->inStart;

	avail = bufsize;
	while (avail > 0 && conn->inCursor < conn->inEnd)
	{
		char		c = conn->inBuffer[conn->inCursor++];

		*buffer++ = c;
		--avail;
		if (c == '\n')
		{
			/* Got a complete line; mark the data removed from libpq */
			conn->inStart = conn->inCursor;
			/* Is it the endmarker line? */
			if (bufsize - avail == 3 && buffer[-3] == '\\' && buffer[-2] == '.')
				return -1;
			/* No, return the data line to the caller */
			return bufsize - avail;
		}
	}

	/*
	 * We don't have a complete line. We'd prefer to leave it in libpq's
	 * buffer until the rest arrives, but there is a special case: what if the
	 * line is longer than the buffer the caller is offering us?  In that case
	 * we'd better hand over a partial line, else we'd get into an infinite
	 * loop. Do this in a way that ensures we can't misrecognize a terminator
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
 * PQendcopy
 *
 * See fe-exec.c for documentation.
 */
int
pqEndcopy2(PGconn *conn)
{
	PGresult   *result;

	if (conn->asyncStatus != PGASYNC_COPY_IN &&
		conn->asyncStatus != PGASYNC_COPY_OUT)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("no COPY in progress\n"));
		return 1;
	}

	/*
	 * make sure no data is waiting to be sent, abort if we are non-blocking
	 * and the flush fails
	 */
	if (pqFlush(conn) && pqIsnonblocking(conn))
		return 1;

	/* non blocking connections may have to abort at this point. */
	if (pqIsnonblocking(conn) && PQisBusy(conn))
		return 1;

	/* Return to active duty */
	conn->asyncStatus = PGASYNC_BUSY;
	resetPQExpBuffer(&conn->errorMessage);

	/* Wait for the completion response */
	result = PQgetResult(conn);

	/* Expecting a successful result */
	if (result && result->resultStatus == PGRES_COMMAND_OK)
	{
		PQclear(result);
		return 0;
	}

	/*
	 * Trouble. For backwards-compatibility reasons, we issue the error
	 * message as if it were a notice (would be nice to get rid of this
	 * silliness, but too many apps probably don't handle errors from
	 * PQendcopy reasonably).  Note that the app can still obtain the error
	 * status from the PGconn object.
	 */
	if (conn->errorMessage.len > 0)
	{
		/* We have to strip the trailing newline ... pain in neck... */
		char		svLast = conn->errorMessage.data[conn->errorMessage.len - 1];

		if (svLast == '\n')
			conn->errorMessage.data[conn->errorMessage.len - 1] = '\0';
		pqInternalNotice(&conn->noticeHooks, "%s", conn->errorMessage.data);
		conn->errorMessage.data[conn->errorMessage.len - 1] = svLast;
	}

	PQclear(result);

	/*
	 * The worst case is that we've lost sync with the backend entirely due to
	 * application screwup of the copy in/out protocol. To recover, reset the
	 * connection (talk about using a sledgehammer...)
	 */
	pqInternalNotice(&conn->noticeHooks,
				   "lost synchronization with server, resetting connection");

	/*
	 * Users doing non-blocking connections need to handle the reset
	 * themselves, they'll need to check the connection status if we return an
	 * error.
	 */
	if (pqIsnonblocking(conn))
		PQresetStart(conn);
	else
		PQreset(conn);

	return 1;
}


/*
 * PQfn - Send a function call to the POSTGRES backend.
 *
 * See fe-exec.c for documentation.
 */
PGresult *
pqFunctionCall2(PGconn *conn, Oid fnid,
				int *result_buf, int *actual_result_len,
				int result_is_int,
				const PQArgBlock *args, int nargs)
{
	bool		needInput = false;
	ExecStatusType status = PGRES_FATAL_ERROR;
	char		id;
	int			i;

	/* PQfn already validated connection state */

	if (pqPutMsgStart('F', false, conn) < 0 ||	/* function call msg */
		pqPuts(" ", conn) < 0 ||	/* dummy string */
		pqPutInt(fnid, 4, conn) != 0 || /* function id */
		pqPutInt(nargs, 4, conn) != 0)	/* # of args */
	{
		pqHandleSendFailure(conn);
		return NULL;
	}

	for (i = 0; i < nargs; ++i)
	{							/* len.int4 + contents	   */
		if (pqPutInt(args[i].len, 4, conn))
		{
			pqHandleSendFailure(conn);
			return NULL;
		}

		if (args[i].isint)
		{
			if (pqPutInt(args[i].u.integer, 4, conn))
			{
				pqHandleSendFailure(conn);
				return NULL;
			}
		}
		else
		{
			if (pqPutnchar((char *) args[i].u.ptr, args[i].len, conn))
			{
				pqHandleSendFailure(conn);
				return NULL;
			}
		}
	}

	if (pqPutMsgEnd(conn) < 0 ||
		pqFlush(conn))
	{
		pqHandleSendFailure(conn);
		return NULL;
	}

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
		 * Scan the message. If we run out of data, loop around to try again.
		 */
		conn->inCursor = conn->inStart;
		needInput = true;

		if (pqGetc(&id, conn))
			continue;

		/*
		 * We should see V or E response to the command, but might get N
		 * and/or A notices first. We also need to swallow the final Z before
		 * returning.
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
					printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("protocol error: id=0x%x\n"),
									  id);
					pqSaveErrorResult(conn);
					conn->inStart = conn->inCursor;
					return pqPrepareAsyncResult(conn);
				}
				break;
			case 'E':			/* error return */
				if (pqGetErrorNotice2(conn, true))
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
				if (pqGetErrorNotice2(conn, false))
					continue;
				break;
			case 'Z':			/* backend is ready for new query */
				/* consume the message and exit */
				conn->inStart = conn->inCursor;
				/* if we saved a result object (probably an error), use it */
				if (conn->result)
					return pqPrepareAsyncResult(conn);
				return PQmakeEmptyPGresult(conn, status);
			default:
				/* The backend violates the protocol. */
				printfPQExpBuffer(&conn->errorMessage,
								  libpq_gettext("protocol error: id=0x%x\n"),
								  id);
				pqSaveErrorResult(conn);
				conn->inStart = conn->inCursor;
				return pqPrepareAsyncResult(conn);
		}
		/* Completed this message, keep going */
		conn->inStart = conn->inCursor;
		needInput = false;
	}

	/*
	 * We fall out of the loop only upon failing to read data.
	 * conn->errorMessage has been set by pqWait or pqReadData. We want to
	 * append it to any already-received error message.
	 */
	pqSaveErrorResult(conn);
	return pqPrepareAsyncResult(conn);
}


/*
 * Construct startup packet
 *
 * Returns a malloc'd packet buffer, or NULL if out of memory
 */
char *
pqBuildStartupPacket2(PGconn *conn, int *packetlen,
					  const PQEnvironmentOption *options)
{
	StartupPacket *startpacket;

	*packetlen = sizeof(StartupPacket);
	startpacket = (StartupPacket *) malloc(sizeof(StartupPacket));
	if (!startpacket)
		return NULL;

	MemSet(startpacket, 0, sizeof(StartupPacket));

	startpacket->protoVersion = htonl(conn->pversion);

	strncpy(startpacket->user, conn->pguser, SM_USER);
	strncpy(startpacket->database, conn->dbName, SM_DATABASE);
	strncpy(startpacket->tty, conn->pgtty, SM_TTY);

	if (conn->pgoptions)
		strncpy(startpacket->options, conn->pgoptions, SM_OPTIONS);

	return (char *) startpacket;
}
