/*-------------------------------------------------------------------------
 *
 * fe-exec.c--
 *	  functions related to sending a query down to the backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-exec.c,v 1.38 1997/09/08 21:55:41 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include "postgres.h"
#include "libpq/pqcomm.h"
#include "libpq/pqsignal.h"
#include "libpq-fe.h"
#include <sys/ioctl.h>
#ifndef HAVE_TERMIOS_H
#include <sys/termios.h>
#else
#include <termios.h>
#endif


#ifdef TIOCGWINSZ
struct winsize screen_size;

#else
struct winsize
{
	int			ws_row;
	int			ws_col;
}			screen_size;

#endif

/* the rows array in a PGresGroup  has to grow to accommodate the rows */
/* returned.  Each time, we grow by this much: */
#define TUPARR_GROW_BY 100

/* keep this in same order as ExecStatusType in pgtclCmds.h */
const char *pgresStatus[] = {
	"PGRES_EMPTY_QUERY",
	"PGRES_COMMAND_OK",
	"PGRES_TUPLES_OK",
	"PGRES_BAD_RESPONSE",
	"PGRES_NONFATAL_ERROR",
	"PGRES_FATAL_ERROR"
};


static PGresult *makePGresult(PGconn *conn, char *pname);
static void addTuple(PGresult *res, PGresAttValue *tup);
static PGresAttValue *getTuple(PGconn *conn, PGresult *res, int binary);
static PGresult *makeEmptyPGresult(PGconn *conn, ExecStatusType status);
static void fill(int length, int max, char filler, FILE *fp);
static char *
do_header(FILE *fout, PQprintOpt *po, const int nFields,
		  int fieldMax[], char *fieldNames[], unsigned char fieldNotNum[],
		  const int fs_len, PGresult *res);

/*
 * PQclear -
 *	  free's the memory associated with a PGresult
 *
 */
void
PQclear(PGresult *res)
{
	int			i,
				j;

	if (!res)
		return;

	/* free all the rows */
	for (i = 0; i < res->ntups; i++)
	{
		for (j = 0; j < res->numAttributes; j++)
		{
			if (res->tuples[i][j].value)
				free(res->tuples[i][j].value);
		}
		if (res->tuples[i])
			free(res->tuples[i]);
	}
	if (res->tuples)
		free(res->tuples);

	/* free all the attributes */
	for (i = 0; i < res->numAttributes; i++)
	{
		if (res->attDescs[i].name)
			free(res->attDescs[i].name);
	}
	if (res->attDescs)
		free(res->attDescs);

	/* free the structure itself */
	free(res);
}

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
 * getTuple -
 *	 get the next row from the stream
 *
 *	the CALLER is responsible from freeing the PGresAttValue returned
 */

static PGresAttValue *
getTuple(PGconn *conn, PGresult *result, int binary)
{
	char		bitmap[MAX_FIELDS];		/* the backend sends us a bitmap
										 * of  */

	/* which attributes are null */
	int			bitmap_index = 0;
	int			i;
	int			nbytes;			/* the number of bytes in bitmap  */
	char		bmap;			/* One byte of the bitmap */
	int			bitcnt = 0;		/* number of bits examined in current byte */
	int			vlen;			/* length of the current field value */
	FILE	   *pfin = conn->Pfin;
	FILE	   *pfdebug = conn->Pfdebug;

	PGresAttValue *tup;

	int			nfields = result->numAttributes;

	result->binary = binary;

	tup = (PGresAttValue *) malloc(nfields * sizeof(PGresAttValue));

	nbytes = nfields / BYTELEN;
	if ((nfields % BYTELEN) > 0)
		nbytes++;

	if (pqGetnchar(bitmap, nbytes, pfin, pfdebug) == 1)
	{
		sprintf(conn->errorMessage,
			  "Error reading null-values bitmap from row data stream\n");
		return NULL;
	}

	bmap = bitmap[bitmap_index];

	for (i = 0; i < nfields; i++)
	{
		if (!(bmap & 0200))
		{
			/* if the field value is absent, make it '\0' */
			tup[i].value = (char *) malloc(1);
			tup[i].value[0] = '\0';
			tup[i].len = NULL_LEN;
		}
		else
		{
			/* get the value length (the first four bytes are for length) */
			pqGetInt(&vlen, VARHDRSZ, pfin, pfdebug);
			if (binary == 0)
			{
				vlen = vlen - VARHDRSZ;
			}
			if (vlen < 0)
				vlen = 0;
			tup[i].len = vlen;
			tup[i].value = (char *) malloc(vlen + 1);
			/* read in the value; */
			if (vlen > 0)
				pqGetnchar((char *) (tup[i].value), vlen, pfin, pfdebug);
			tup[i].value[vlen] = '\0';
		}
		/* get the appropriate bitmap */
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

	return tup;
}


/*
 * addTuple
 *	  add a row to the PGresult structure, growing it if necessary
 *	to accommodate
 *
 */
static void
addTuple(PGresult *res, PGresAttValue *tup)
{
	if (res->ntups == res->tupArrSize)
	{
		/* grow the array */
		res->tupArrSize += TUPARR_GROW_BY;

		if (res->ntups == 0)
			res->tuples = (PGresAttValue **)
				malloc(res->tupArrSize * sizeof(PGresAttValue *));
		else

			/*
			 * we can use realloc because shallow copying of the structure
			 * is okay
			 */
			res->tuples = (PGresAttValue **)
				realloc(res->tuples, res->tupArrSize * sizeof(PGresAttValue *));
	}

	res->tuples[res->ntups] = tup;
	res->ntups++;
}

/*
 * PGresult
 *	  fill out the PGresult structure with result rows from the backend
 *	this is called after query has been successfully run and we have
 *	a portal name
 *
 *	ASSUMPTION: we assume only *1* row group is returned from the backend
 *
 *	the CALLER is reponsible for free'ing the new PGresult allocated here
 *
 */

static PGresult *
makePGresult(PGconn *conn, char *pname)
{
	PGresult   *result;
	int			id;
	int			nfields;
	int			i;
	int			done = 0;

	PGresAttValue *newTup;

	FILE	   *pfin = conn->Pfin;
	FILE	   *pfdebug = conn->Pfdebug;

	result = makeEmptyPGresult(conn, PGRES_TUPLES_OK);

	/* makePGresult() should only be called when the */
	/* id of the stream is 'T' to start with */

	/* the next two bytes are the number of fields	*/
	if (pqGetInt(&nfields, 2, pfin, pfdebug) == 1)
	{
		sprintf(conn->errorMessage,
			"could not get the number of fields from the 'T' message\n");
		goto makePGresult_badResponse_return;
	}
	else
		result->numAttributes = nfields;

	/* allocate space for the attribute descriptors */
	if (nfields > 0)
	{
		result->attDescs = (PGresAttDesc *) malloc(nfields * sizeof(PGresAttDesc));
	}

	/* get type info */
	for (i = 0; i < nfields; i++)
	{
		char		typName[MAX_MESSAGE_LEN];
		int			adtid;
		int			adtsize;

		if (pqGets(typName, MAX_MESSAGE_LEN, pfin, pfdebug) ||
			pqGetInt(&adtid, 4, pfin, pfdebug) ||
			pqGetInt(&adtsize, 2, pfin, pfdebug))
		{
			sprintf(conn->errorMessage,
				"error reading type information from the 'T' message\n");
			goto makePGresult_badResponse_return;
		}
		result->attDescs[i].name = malloc(strlen(typName) + 1);
		strcpy(result->attDescs[i].name, typName);
		result->attDescs[i].adtid = adtid;
		result->attDescs[i].adtsize = adtsize;	/* casting from int to
												 * int2 here */
	}

	id = pqGetc(pfin, pfdebug);

	/* process the data stream until we're finished */
	while (!done)
	{
		switch (id)
		{
			case 'T':			/* a new row group */
				sprintf(conn->errorMessage,
						"makePGresult() -- "
					 "is not equipped to handle multiple row groups.\n");
				goto makePGresult_badResponse_return;
			case 'B':			/* a row in binary format */
			case 'D':			/* a row in ASCII format */
				newTup = getTuple(conn, result, (id == 'B'));
				if (newTup == NULL)
					goto makePGresult_badResponse_return;
				addTuple(result, newTup);
				break;
			case 'C':			/* end of portal row stream */
				{
					char		command[MAX_MESSAGE_LEN];

					pqGets(command, MAX_MESSAGE_LEN, pfin, pfdebug);	/* read command tag */
					done = 1;
				}
				break;
			case 'E':			/* errors */
				if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, pfin, pfdebug) == 1)
				{
					sprintf(conn->errorMessage,
							"Error return detected from backend, "
							"but error message cannot be read");
				}
				result->resultStatus = PGRES_FATAL_ERROR;
				return result;
				break;
			case 'N':			/* notices from the backend */
				if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, pfin, pfdebug) == 1)
				{
					sprintf(conn->errorMessage,
							"Notice return detected from backend, "
							"but error message cannot be read");
				}
				else
					/* XXXX send Notices to stderr for now */
					fprintf(stderr, "%s\n", conn->errorMessage);
				break;
			default:			/* uh-oh this should never happen but
								 * frequently does when the backend dumps
								 * core */
				sprintf(conn->errorMessage,
						"FATAL:  unrecognized data from the backend.  "
						"It probably dumped core.\n");
				fprintf(stderr, conn->errorMessage);
				result->resultStatus = PGRES_FATAL_ERROR;
				return result;
				break;
		}
		if (!done)
			id = getc(pfin);
	}							/* while (1) */

	result->resultStatus = PGRES_TUPLES_OK;
	return result;

makePGresult_badResponse_return:
	result->resultStatus = PGRES_BAD_RESPONSE;
	return result;

}


/*
 * Assuming that we just sent a query to the backend, read the backend's
 * response from stream <pfin> and respond accordingly.
 *
 * If <pfdebug> is non-null, write to that stream whatever we receive
 * (it's a debugging trace).
 *
 * Return as <result> a pointer to a proper final PGresult structure,
 * newly allocated, for the query based on the response we get.  If the
 * response we get indicates that the query didn't execute, return a
 * null pointer and don't allocate any space, but also place a text
 * string explaining the problem at <*reason>.
 */

static void
process_response_from_backend(FILE *pfin, FILE *pfout, FILE *pfdebug,
							  PGconn *conn,
							  PGresult **result_p, char *const reason)
{

	int			id;

	/*
	 * The protocol character received from the backend.  The protocol
	 * character is the first character in the backend's response to our
	 * query.  It defines the nature of the response.
	 */
	PGnotify   *newNotify;
	bool		done;

	/* We're all done with the query and ready to return the result. */
	int			emptiesSent;

	/*
	 * Number of empty queries we have sent in order to flush out multiple
	 * responses, less the number of corresponding responses we have
	 * received.
	 */
	int			errors;

	/*
	 * If an error is received, we must still drain out the empty queries
	 * sent. So we need another flag.
	 */
	char		cmdStatus[MAX_MESSAGE_LEN];
	char		pname[MAX_MESSAGE_LEN]; /* portal name */

	/*
	 * loop because multiple messages, especially NOTICES, can come back
	 * from the backend.  NOTICES are output directly to stderr
	 */

	emptiesSent = 0;			/* No empty queries sent yet */
	errors = 0;					/* No errors received yet */
	pname[0] = '\0';

	done = false;				/* initial value */
	while (!done)
	{
		/* read the result id */
		id = pqGetc(pfin, pfdebug);
		if (id == EOF)
		{
			/* hmm,  no response from the backend-end, that's bad */
			(void) sprintf(reason,
				  "PQexec() -- Request was sent to backend, but backend "
						   "closed the channel before "
						   "responding.  This probably means the backend "
					  "terminated abnormally before or while processing "
						   "the request.\n");
			conn->status = CONNECTION_BAD;		/* No more connection to
												 * backend */
			*result_p = (PGresult *) NULL;
			done = true;
		}
		else
		{
			switch (id)
			{
				case 'A':
					newNotify = (PGnotify *) malloc(sizeof(PGnotify));
					pqGetInt(&(newNotify->be_pid), 4, pfin, pfdebug);
					pqGets(newNotify->relname, NAMEDATALEN, pfin, pfdebug);
					DLAddTail(conn->notifyList, DLNewElem(newNotify));

					/*
					 * async messages are piggy'ed back on other messages,
					 * so we stay in the while loop for other messages
					 */
					break;
				case 'C':		/* portal query command, no rows returned */
					if (pqGets(cmdStatus, MAX_MESSAGE_LEN, pfin, pfdebug) == 1)
					{
						sprintf(reason,
								"PQexec() -- query command completed, "
								"but return message from backend cannot be read.");
						*result_p = (PGresult *) NULL;
						done = true;
					}
					else
					{

						/*
						 * since backend may produce more than one result
						 * for some commands need to poll until clear send
						 * an empty query down, and keep reading out of
						 * the pipe until an 'I' is received.
						 */
						pqPuts("Q ", pfout, pfdebug);	/* send an empty query */

						/*
						 * Increment a flag and process messages in the
						 * usual way because there may be async
						 * notifications pending.  DZ - 31-8-1996
						 */
						emptiesSent++;
					}
					break;
				case 'E':		/* error return */
					if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, pfin, pfdebug) == 1)
					{
						(void) sprintf(reason,
						"PQexec() -- error return detected from backend, "
						"but attempt to read the error message failed.");
					}
					*result_p = (PGresult *) NULL;
					errors++;
					if (emptiesSent == 0)
					{
						done = true;
					}
					break;
				case 'I':
					{			/* empty query */
						/* read and throw away the closing '\0' */
						int			c;

						if ((c = pqGetc(pfin, pfdebug)) != '\0')
						{
							fprintf(stderr, "error!, unexpected character %c following 'I'\n", c);
						}
						if (emptiesSent)
						{
							if (--emptiesSent == 0)
							{	/* is this the last one? */

								/*
								 * If this is the result of a portal query
								 * command set the command status and
								 * message accordingly.  DZ - 31-8-1996
								 */
								if (!errors)
								{
									*result_p = makeEmptyPGresult(conn, PGRES_COMMAND_OK);
									strncpy((*result_p)->cmdStatus, cmdStatus, CMDSTATUS_LEN - 1);
								}
								else
								{
									*result_p = (PGresult *) NULL;
								}
								done = true;
							}
						}
						else
						{
							if (!errors)
							{
								*result_p = makeEmptyPGresult(conn, PGRES_EMPTY_QUERY);
							}
							else
							{
								*result_p = (PGresult *) NULL;
							}
							done = true;
						}
					}
					break;
				case 'N':		/* notices from the backend */
					if (pqGets(reason, ERROR_MSG_LENGTH, pfin, pfdebug) == 1)
					{
						sprintf(reason,
							 "PQexec() -- Notice detected from backend, "
								"but attempt to read the notice failed.");
						*result_p = (PGresult *) NULL;
						done = true;
					}
					else

						/*
						 * Should we really be doing this?	These notices
						 * are not important enough for us to presume to
						 * put them on stderr.	Maybe the caller should
						 * decide whether to put them on stderr or not.
						 * BJH 96.12.27
						 */
						fprintf(stderr, "%s", reason);
					break;
				case 'P':		/* synchronous (normal) portal */
					pqGets(pname, MAX_MESSAGE_LEN, pfin, pfdebug);		/* read in portal name */
					break;
				case 'T':		/* actual row results: */
					*result_p = makePGresult(conn, pname);
					done = true;
					break;
				case 'D':		/* copy command began successfully */
					*result_p = makeEmptyPGresult(conn, PGRES_COPY_IN);
					done = true;
					break;
				case 'B':		/* copy command began successfully */
					*result_p = makeEmptyPGresult(conn, PGRES_COPY_OUT);
					done = true;
					break;
				default:
					sprintf(reason,
					"unknown protocol character '%c' read from backend.  "
					"(The protocol character is the first character the "
							"backend sends in response to a query it receives).\n",
							id);
					*result_p = (PGresult *) NULL;
					done = true;
			}					/* switch on protocol character */
		}						/* if character was received */
	}							/* while not done */
}



/*
 * PQexec
 *	  send a query to the backend and package up the result in a Pgresult
 *
 *	if the query failed, return NULL, conn->errorMessage is set to
 * a relevant message
 *	if query is successful, a new PGresult is returned
 * the use is responsible for freeing that structure when done with it
 *
 */

PGresult   *
PQexec(PGconn *conn, const char *query)
{
	PGresult   *result;
	char		buffer[MAX_MESSAGE_LEN];

	if (!conn)
		return NULL;
	if (!query)
	{
		sprintf(conn->errorMessage, "PQexec() -- query pointer is null.");
		return NULL;
	}

	/* clear the error string */
	conn->errorMessage[0] = '\0';

	/* check to see if the query string is too long */
	if (strlen(query) > MAX_MESSAGE_LEN)
	{
		sprintf(conn->errorMessage, "PQexec() -- query is too long.  "
				"Maximum length is %d\n", MAX_MESSAGE_LEN - 2);
		return NULL;
	}

	/* Don't try to send if we know there's no live connection. */
	if (conn->status != CONNECTION_OK)
	{
		sprintf(conn->errorMessage, "PQexec() -- There is no connection "
				"to the backend.\n");
		return NULL;
	}

	/* the frontend-backend protocol uses 'Q' to designate queries */
	sprintf(buffer, "Q%s", query);

	/* send the query to the backend; */
	if (pqPuts(buffer, conn->Pfout, conn->Pfdebug) == 1)
	{
		(void) sprintf(conn->errorMessage,
					   "PQexec() -- while sending query:  %s\n"
					   "-- fprintf to Pfout failed: errno=%d\n%s\n",
					   query, errno, strerror(errno));
		return NULL;
	}

	process_response_from_backend(conn->Pfin, conn->Pfout, conn->Pfdebug, conn,
								  &result, conn->errorMessage);
	return (result);
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

	if (!conn)
		return NULL;

	if (conn->status != CONNECTION_OK)
		return NULL;
	/* RemHead returns NULL if list is empy */
	e = DLRemHead(conn->notifyList);
	if (e)
		return (PGnotify *) DLE_VAL(e);
	else
		return NULL;
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
 *		1 in other cases
 */
int
PQgetline(PGconn *conn, char *s, int maxlen)
{
	int			c = '\0';

	if (!conn)
		return EOF;

	if (!conn->Pfin || !s || maxlen <= 1)
		return (EOF);

	for (; maxlen > 1 &&
		 (c = pqGetc(conn->Pfin, conn->Pfdebug)) != '\n' &&
		 c != EOF;
		 --maxlen)
	{
		*s++ = c;
	}
	*s = '\0';

	if (c == EOF)
	{
		return (EOF);			/* error -- reached EOF before \n */
	}
	else if (c == '\n')
	{
		return (0);				/* done with this line */
	}
	return (1);					/* returning a full buffer */
}

/*
 * PQputline -- sends a string to the backend.
 *
 * Chiefly here so that applications can use "COPY <rel> from stdin".
 *
 */
void
PQputline(PGconn *conn, const char *s)
{
	if (conn && (conn->Pfout))
	{
		(void) fputs(s, conn->Pfout);
		fflush(conn->Pfout);
	}
}

/*
 * PQendcopy
 *		called while waiting for the backend to respond with success/failure
 *		to a "copy".
 *
 * RETURNS:
 *		0 on success
 *		1 on failure
 */
int
PQendcopy(PGconn *conn)
{
	FILE	   *pfin,
			   *pfdebug;
	bool		valid = true;

	if (!conn)
		return (int) NULL;

	pfin = conn->Pfin;
	pfdebug = conn->Pfdebug;

	if (pqGetc(pfin, pfdebug) == 'C')
	{
		char		command[MAX_MESSAGE_LEN];

		pqGets(command, MAX_MESSAGE_LEN, pfin, pfdebug);		/* read command tag */
	}
	else
		valid = false;

	if (valid)
		return (0);
	else
	{
		sprintf(conn->errorMessage,
				"Error return detected from backend, "
				"but attempt to read the message failed.");
		fprintf(stderr, "resetting connection\n");
		PQreset(conn);
		return (1);
	}
}

/* simply send out max-length number of filler characters to fp */
static void
fill(int length, int max, char filler, FILE *fp)
{
	int			count;
	char		filltmp[2];

	filltmp[0] = filler;
	filltmp[1] = 0;
	count = max - length;
	while (count-- >= 0)
	{
		fprintf(fp, "%s", filltmp);
	}
}

/*
 * PQdisplayTuples()
 * kept for backward compatibility
 */
void
PQdisplayTuples(PGresult *res,
				FILE *fp,		/* where to send the output */
				int fillAlign,	/* pad the fields with spaces */
				const char *fieldSep,	/* field separator */
				int printHeader,/* display headers? */
				int quiet
)
{
#define DEFAULT_FIELD_SEP " "

	int			i,
				j;
	int			nFields;
	int			nTuples;
	int			fLength[MAX_FIELDS];

	if (fieldSep == NULL)
		fieldSep = DEFAULT_FIELD_SEP;

	/* Get some useful info about the results */
	nFields = PQnfields(res);
	nTuples = PQntuples(res);

	if (fp == NULL)
		fp = stdout;

	/* Zero the initial field lengths */
	for (j = 0; j < nFields; j++)
	{
		fLength[j] = strlen(PQfname(res, j));
	}
	/* Find the max length of each field in the result */
	/* will be somewhat time consuming for very large results */
	if (fillAlign)
	{
		for (i = 0; i < nTuples; i++)
		{
			for (j = 0; j < nFields; j++)
			{
				if (PQgetlength(res, i, j) > fLength[j])
					fLength[j] = PQgetlength(res, i, j);
			}
		}
	}

	if (printHeader)
	{
		/* first, print out the attribute names */
		for (i = 0; i < nFields; i++)
		{
			fputs(PQfname(res, i), fp);
			if (fillAlign)
				fill(strlen(PQfname(res, i)), fLength[i], ' ', fp);
			fputs(fieldSep, fp);
		}
		fprintf(fp, "\n");

		/* Underline the attribute names */
		for (i = 0; i < nFields; i++)
		{
			if (fillAlign)
				fill(0, fLength[i], '-', fp);
			fputs(fieldSep, fp);
		}
		fprintf(fp, "\n");
	}

	/* next, print out the instances */
	for (i = 0; i < nTuples; i++)
	{
		for (j = 0; j < nFields; j++)
		{
			fprintf(fp, "%s", PQgetvalue(res, i, j));
			if (fillAlign)
				fill(strlen(PQgetvalue(res, i, j)), fLength[j], ' ', fp);
			fputs(fieldSep, fp);
		}
		fprintf(fp, "\n");
	}

	if (!quiet)
		fprintf(fp, "\nQuery returned %d row%s.\n", PQntuples(res),
				(PQntuples(res) == 1) ? "" : "s");

	fflush(fp);
}



/*
 * PQprintTuples()
 *
 * kept for backward compatibility
 *
 */
void
PQprintTuples(PGresult *res,
			  FILE *fout,		/* output stream */
			  int PrintAttNames,/* print attribute names or not */
			  int TerseOutput,	/* delimiter bars or not? */
			  int colWidth		/* width of column, if 0, use variable
								 * width */
)
{
	int			nFields;
	int			nTups;
	int			i,
				j;
	char		formatString[80];

	char	   *tborder = NULL;

	nFields = PQnfields(res);
	nTups = PQntuples(res);

	if (colWidth > 0)
	{
		sprintf(formatString, "%%s %%-%ds", colWidth);
	}
	else
		sprintf(formatString, "%%s %%s");

	if (nFields > 0)
	{							/* only print rows with at least 1 field.  */

		if (!TerseOutput)
		{
			int			width;

			width = nFields * 14;
			tborder = malloc(width + 1);
			for (i = 0; i <= width; i++)
				tborder[i] = '-';
			tborder[i] = '\0';
			fprintf(fout, "%s\n", tborder);
		}

		for (i = 0; i < nFields; i++)
		{
			if (PrintAttNames)
			{
				fprintf(fout, formatString,
						TerseOutput ? "" : "|",
						PQfname(res, i));
			}
		}

		if (PrintAttNames)
		{
			if (TerseOutput)
				fprintf(fout, "\n");
			else
				fprintf(fout, "|\n%s\n", tborder);
		}

		for (i = 0; i < nTups; i++)
		{
			for (j = 0; j < nFields; j++)
			{
				char	   *pval = PQgetvalue(res, i, j);

				fprintf(fout, formatString,
						TerseOutput ? "" : "|",
						pval ? pval : "");
			}
			if (TerseOutput)
				fprintf(fout, "\n");
			else
				fprintf(fout, "|\n%s\n", tborder);
		}
	}
}



static void
do_field(PQprintOpt *po, PGresult *res,
		 const int i, const int j, char *buf, const int fs_len,
		 char *fields[],
		 const int nFields, char *fieldNames[],
		 unsigned char fieldNotNum[], int fieldMax[],
		 const int fieldMaxLen, FILE *fout
)
{

	char	   *pval,
			   *p,
			   *o;
	int			plen;
	bool		skipit;

	plen = PQgetlength(res, i, j);
	pval = PQgetvalue(res, i, j);

	if (plen < 1 || !pval || !*pval)
	{
		if (po->align || po->expanded)
			skipit = true;
		else
		{
			skipit = false;
			goto efield;
		}
	}
	else
		skipit = false;

	if (!skipit)
	{
		for (p = pval, o = buf; *p; *(o++) = *(p++))
		{
			if ((fs_len == 1 && (*p == *(po->fieldSep))) || *p == '\\' || *p == '\n')
				*(o++) = '\\';
			if (po->align && (*pval == 'E' || *pval == 'e' ||
							  !((*p >= '0' && *p <= '9') ||
								*p == '.' ||
								*p == 'E' ||
								*p == 'e' ||
								*p == ' ' ||
								*p == '-')))
				fieldNotNum[j] = 1;
		}
		*o = '\0';
		if (!po->expanded && (po->align || po->html3))
		{
			int			n = strlen(buf);

			if (n > fieldMax[j])
				fieldMax[j] = n;
			if (!(fields[i * nFields + j] = (char *) malloc(n + 1)))
			{
				perror("malloc");
				exit(1);
			}
			strcpy(fields[i * nFields + j], buf);
		}
		else
		{
			if (po->expanded)
			{
				if (po->html3)
					fprintf(fout,
							"<tr><td align=left><b>%s</b></td>"
							"<td align=%s>%s</td></tr>\n",
							fieldNames[j],
							fieldNotNum[j] ? "left" : "right",
							buf);
				else
				{
					if (po->align)
						fprintf(fout,
								"%-*s%s %s\n",
						fieldMaxLen - fs_len, fieldNames[j], po->fieldSep,
								buf);
					else
						fprintf(fout, "%s%s%s\n", fieldNames[j], po->fieldSep, buf);
				}
			}
			else
			{
				if (!po->html3)
				{
					fputs(buf, fout);
			efield:
					if ((j + 1) < nFields)
						fputs(po->fieldSep, fout);
					else
						fputc('\n', fout);
				}
			}
		}
	}
}


static char *
do_header(FILE *fout, PQprintOpt *po, const int nFields, int fieldMax[],
		  char *fieldNames[], unsigned char fieldNotNum[],
		  const int fs_len, PGresult *res)
{

	int			j;				/* for loop index */
	char	   *border = NULL;

	if (po->html3)
		fputs("<tr>", fout);
	else
	{
		int			j;			/* for loop index */
		int			tot = 0;
		int			n = 0;
		char	   *p = NULL;

		for (; n < nFields; n++)
			tot += fieldMax[n] + fs_len + (po->standard ? 2 : 0);
		if (po->standard)
			tot += fs_len * 2 + 2;
		border = malloc(tot + 1);
		if (!border)
		{
			perror("malloc");
			exit(1);
		}
		p = border;
		if (po->standard)
		{
			char	   *fs = po->fieldSep;

			while (*fs++)
				*p++ = '+';
		}
		for (j = 0; j < nFields; j++)
		{
			int			len;

			for (len = fieldMax[j] + (po->standard ? 2 : 0); len--; *p++ = '-');
			if (po->standard || (j + 1) < nFields)
			{
				char	   *fs = po->fieldSep;

				while (*fs++)
					*p++ = '+';
			}
		}
		*p = '\0';
		if (po->standard)
			fprintf(fout, "%s\n", border);
	}
	if (po->standard)
		fputs(po->fieldSep, fout);
	for (j = 0; j < nFields; j++)
	{
		char	   *s = PQfname(res, j);

		if (po->html3)
		{
			fprintf(fout, "<th align=%s>%s</th>",
					fieldNotNum[j] ? "left" : "right", fieldNames[j]);
		}
		else
		{
			int			n = strlen(s);

			if (n > fieldMax[j])
				fieldMax[j] = n;
			if (po->standard)
				fprintf(fout,
						fieldNotNum[j] ? " %-*s " : " %*s ",
						fieldMax[j], s);
			else
				fprintf(fout, fieldNotNum[j] ? "%-*s" : "%*s", fieldMax[j], s);
			if (po->standard || (j + 1) < nFields)
				fputs(po->fieldSep, fout);
		}
	}
	if (po->html3)
		fputs("</tr>\n", fout);
	else
		fprintf(fout, "\n%s\n", border);
	return border;
}


static void
output_row(FILE *fout, PQprintOpt *po, const int nFields, char *fields[],
		   unsigned char fieldNotNum[], int fieldMax[], char *border,
		   const int row_index)
{

	int			field_index;	/* for loop index */

	if (po->html3)
		fputs("<tr>", fout);
	else if (po->standard)
		fputs(po->fieldSep, fout);
	for (field_index = 0; field_index < nFields; field_index++)
	{
		char	   *p = fields[row_index * nFields + field_index];

		if (po->html3)
			fprintf(fout, "<td align=%s>%s</td>",
				fieldNotNum[field_index] ? "left" : "right", p ? p : "");
		else
		{
			fprintf(fout,
					fieldNotNum[field_index] ?
					(po->standard ? " %-*s " : "%-*s") :
					(po->standard ? " %*s " : "%*s"),
					fieldMax[field_index],
					p ? p : "");
			if (po->standard || field_index + 1 < nFields)
				fputs(po->fieldSep, fout);
		}
		if (p)
			free(p);
	}
	if (po->html3)
		fputs("</tr>", fout);
	else if (po->standard)
		fprintf(fout, "\n%s", border);
	fputc('\n', fout);
}




/*
 * PQprint()
 *
 * Format results of a query for printing.
 *
 * PQprintOpt is a typedef (structure) that containes
 * various flags and options. consult libpq-fe.h for
 * details
 *
 * Obsoletes PQprintTuples.
 */

void
PQprint(FILE *fout,
		PGresult *res,
		PQprintOpt *po
)
{
	int			nFields;

	nFields = PQnfields(res);

	if (nFields > 0)
	{							/* only print rows with at least 1 field.  */
		int			i,
					j;
		int			nTups;
		int		   *fieldMax = NULL;	/* in case we don't use them */
		unsigned char *fieldNotNum = NULL;
		char	   *border = NULL;
		char	  **fields = NULL;
		char	  **fieldNames;
		int			fieldMaxLen = 0;
		int			numFieldName;
		int			fs_len = strlen(po->fieldSep);
		int			total_line_length = 0;
		int			usePipe = 0;
		char	   *pagerenv;
		char		buf[8192 * 2 + 1];

		nTups = PQntuples(res);
		if (!(fieldNames = (char **) calloc(nFields, sizeof(char *))))
		{
			perror("calloc");
			exit(1);
		}
		if (!(fieldNotNum = (unsigned char *) calloc(nFields, 1)))
		{
			perror("calloc");
			exit(1);
		}
		if (!(fieldMax = (int *) calloc(nFields, sizeof(int))))
		{
			perror("calloc");
			exit(1);
		}
		for (numFieldName = 0;
			 po->fieldName && po->fieldName[numFieldName];
			 numFieldName++)
			;
		for (j = 0; j < nFields; j++)
		{
			int			len;
			char	   *s =
			(j < numFieldName && po->fieldName[j][0]) ?
			po->fieldName[j] : PQfname(res, j);

			fieldNames[j] = s;
			len = s ? strlen(s) : 0;
			fieldMax[j] = len;
			len += fs_len;
			if (len > fieldMaxLen)
				fieldMaxLen = len;
			total_line_length += len;
		}

		total_line_length += nFields * strlen(po->fieldSep) + 1;

		if (fout == NULL)
			fout = stdout;
		if (po->pager && fout == stdout &&
			isatty(fileno(stdin)) &&
			isatty(fileno(stdout)))
		{
			/* try to pipe to the pager program if possible */
#ifdef TIOCGWINSZ
			if (ioctl(fileno(stdout), TIOCGWINSZ, &screen_size) == -1 ||
				screen_size.ws_col == 0 ||
				screen_size.ws_row == 0)
			{
#endif
				screen_size.ws_row = 24;
				screen_size.ws_col = 80;
#ifdef TIOCGWINSZ
			}
#endif
			pagerenv = getenv("PAGER");
			if (pagerenv != NULL &&
				pagerenv[0] != '\0' &&
				!po->html3 &&
				((po->expanded &&
				  nTups * (nFields + 1) >= screen_size.ws_row) ||
				 (!po->expanded &&
				  nTups * (total_line_length / screen_size.ws_col + 1) *
				  (1 + (po->standard != 0)) >=
				  screen_size.ws_row -
				  (po->header != 0) *
				  (total_line_length / screen_size.ws_col + 1) * 2
				  - (po->header != 0) * 2		/* row count and newline */
				  )))
			{
				fout = popen(pagerenv, "w");
				if (fout)
				{
					usePipe = 1;
					pqsignal(SIGPIPE, SIG_IGN);
				}
				else
					fout = stdout;
			}
		}

		if (!po->expanded && (po->align || po->html3))
		{
			if (!(fields = (char **) calloc(nFields * (nTups + 1), sizeof(char *))))
			{
				perror("calloc");
				exit(1);
			}
		}
		else if (po->header && !po->html3)
		{
			if (po->expanded)
			{
				if (po->align)
					fprintf(fout, "%-*s%s Value\n",
							fieldMaxLen - fs_len, "Field", po->fieldSep);
				else
					fprintf(fout, "%s%sValue\n", "Field", po->fieldSep);
			}
			else
			{
				int			len = 0;

				for (j = 0; j < nFields; j++)
				{
					char	   *s = fieldNames[j];

					fputs(s, fout);
					len += strlen(s) + fs_len;
					if ((j + 1) < nFields)
						fputs(po->fieldSep, fout);
				}
				fputc('\n', fout);
				for (len -= fs_len; len--; fputc('-', fout));
				fputc('\n', fout);
			}
		}
		if (po->expanded && po->html3)
		{
			if (po->caption)
				fprintf(fout, "<centre><h2>%s</h2></centre>\n", po->caption);
			else
				fprintf(fout,
						"<centre><h2>"
						"Query retrieved %d rows * %d fields"
						"</h2></centre>\n",
						nTups, nFields);
		}
		for (i = 0; i < nTups; i++)
		{
			if (po->expanded)
			{
				if (po->html3)
					fprintf(fout,
						  "<table %s><caption align=high>%d</caption>\n",
							po->tableOpt ? po->tableOpt : "", i);
				else
					fprintf(fout, "-- RECORD %d --\n", i);
			}
			for (j = 0; j < nFields; j++)
				do_field(po, res, i, j, buf, fs_len, fields, nFields,
						 fieldNames, fieldNotNum,
						 fieldMax, fieldMaxLen, fout);
			if (po->html3 && po->expanded)
				fputs("</table>\n", fout);
		}
		if (!po->expanded && (po->align || po->html3))
		{
			if (po->html3)
			{
				if (po->header)
				{
					if (po->caption)
						fprintf(fout,
						  "<table %s><caption align=high>%s</caption>\n",
								po->tableOpt ? po->tableOpt : "",
								po->caption);
					else
						fprintf(fout,
								"<table %s><caption align=high>"
								"Retrieved %d rows * %d fields"
								"</caption>\n",
						po->tableOpt ? po->tableOpt : "", nTups, nFields);
				}
				else
					fprintf(fout, "<table %s>", po->tableOpt ? po->tableOpt : "");
			}
			if (po->header)
				border = do_header(fout, po, nFields, fieldMax, fieldNames,
								   fieldNotNum, fs_len, res);
			for (i = 0; i < nTups; i++)
				output_row(fout, po, nFields, fields,
						   fieldNotNum, fieldMax, border, i);
			free(fields);
			if (border)
				free(border);
		}
		if (po->header && !po->html3)
			fprintf(fout, "(%d row%s)\n\n", PQntuples(res),
					(PQntuples(res) == 1) ? "" : "s");
		free(fieldMax);
		free(fieldNotNum);
		free(fieldNames);
		if (usePipe)
		{
			pclose(fout);
			pqsignal(SIGPIPE, SIG_DFL);
		}
		if (po->html3 && !po->expanded)
			fputs("</table>\n", fout);
	}
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
 *		args			: pointer to a NULL terminated arg array.
 *						  (length, if integer, and result-pointer)
 *		nargs			: # of arguments in args array.
 *
 * RETURNS
 *		NULL on failure.  PQerrormsg will be set.
 *		"G" if there is a return value.
 *		"V" if there is no return value.
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
	FILE	   *pfin,
			   *pfout,
			   *pfdebug;
	int			id;
	int			i;

	if (!conn)
		return NULL;

	pfin = conn->Pfin;
	pfout = conn->Pfout;
	pfdebug = conn->Pfdebug;

	/* clear the error string */
	conn->errorMessage[0] = '\0';

	pqPuts("F ", pfout, pfdebug);		/* function */
	pqPutInt(fnid, 4, pfout, pfdebug);	/* function id */
	pqPutInt(nargs, 4, pfout, pfdebug); /* # of args */

	for (i = 0; i < nargs; ++i)
	{							/* len.int4 + contents	   */
		pqPutInt(args[i].len, 4, pfout, pfdebug);
		if (args[i].isint)
		{
			pqPutInt(args[i].u.integer, 4, pfout, pfdebug);
		}
		else
		{
			pqPutnchar((char *) args[i].u.ptr, args[i].len, pfout, pfdebug);
		}
	}
	pqFlush(pfout, pfdebug);

	id = pqGetc(pfin, pfdebug);
	if (id != 'V')
	{
		if (id == 'E')
		{
			pqGets(conn->errorMessage, ERROR_MSG_LENGTH, pfin, pfdebug);
		}
		else
			sprintf(conn->errorMessage,
			   "PQfn: expected a 'V' from the backend. Got '%c' instead",
					id);
		return makeEmptyPGresult(conn, PGRES_FATAL_ERROR);
	}

	id = pqGetc(pfin, pfdebug);
	for (;;)
	{
		int			c;

		switch (id)
		{
			case 'G':			/* function returned properly */
				pqGetInt(actual_result_len, 4, pfin, pfdebug);
				if (result_is_int)
				{
					pqGetInt(result_buf, 4, pfin, pfdebug);
				}
				else
				{
					pqGetnchar((char *) result_buf, *actual_result_len,
							   pfin, pfdebug);
				}
				c = pqGetc(pfin, pfdebug);		/* get the last '0' */
				return makeEmptyPGresult(conn, PGRES_COMMAND_OK);
			case 'E':
				sprintf(conn->errorMessage,
						"PQfn: returned an error");
				return makeEmptyPGresult(conn, PGRES_FATAL_ERROR);
			case 'N':
				/* print notice and go back to processing return values */
				if (pqGets(conn->errorMessage, ERROR_MSG_LENGTH, pfin, pfdebug)
					== 1)
				{
					sprintf(conn->errorMessage,
					  "Notice return detected from backend, but message "
							"cannot be read");
				}
				else
					fprintf(stderr, "%s\n", conn->errorMessage);
				/* keep iterating */
				break;
			case '0':			/* no return value */
				return makeEmptyPGresult(conn, PGRES_COMMAND_OK);
			default:
				/* The backend violates the protocol. */
				sprintf(conn->errorMessage,
						"FATAL: PQfn: protocol error: id=%x\n", id);
				return makeEmptyPGresult(conn, PGRES_FATAL_ERROR);
		}
	}
}

/* ====== accessor funcs for PGresult ======== */

ExecStatusType
PQresultStatus(PGresult *res)
{
	if (!res)
	{
		fprintf(stderr, "PQresultStatus() -- pointer to PQresult is null");
		return PGRES_NONFATAL_ERROR;
	}

	return res->resultStatus;
}

int
PQntuples(PGresult *res)
{
	if (!res)
	{
		fprintf(stderr, "PQntuples() -- pointer to PQresult is null");
		return (int) NULL;
	}
	return res->ntups;
}

int
PQnfields(PGresult *res)
{
	if (!res)
	{
		fprintf(stderr, "PQnfields() -- pointer to PQresult is null");
		return (int) NULL;
	}
	return res->numAttributes;
}

/*
   returns NULL if the field_num is invalid
*/
char	   *
PQfname(PGresult *res, int field_num)
{
	if (!res)
	{
		fprintf(stderr, "PQfname() -- pointer to PQresult is null");
		return NULL;
	}

	if (field_num > (res->numAttributes - 1))
	{
		fprintf(stderr,
			  "PQfname: ERROR! name of field %d(of %d) is not available",
				field_num, res->numAttributes - 1);
		return NULL;
	}
	if (res->attDescs)
	{
		return res->attDescs[field_num].name;
	}
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

	if (!res)
	{
		fprintf(stderr, "PQfnumber() -- pointer to PQresult is null");
		return -1;
	}

	if (field_name == NULL ||
		field_name[0] == '\0' ||
		res->attDescs == NULL)
		return -1;

	for (i = 0; i < res->numAttributes; i++)
	{
		if (strcasecmp(field_name, res->attDescs[i].name) == 0)
			return i;
	}
	return -1;

}

Oid
PQftype(PGresult *res, int field_num)
{
	if (!res)
	{
		fprintf(stderr, "PQftype() -- pointer to PQresult is null");
		return InvalidOid;
	}

	if (field_num > (res->numAttributes - 1))
	{
		fprintf(stderr,
			  "PQftype: ERROR! type of field %d(of %d) is not available",
				field_num, res->numAttributes - 1);
	}
	if (res->attDescs)
	{
		return res->attDescs[field_num].adtid;
	}
	else
		return InvalidOid;
}

int2
PQfsize(PGresult *res, int field_num)
{
	if (!res)
	{
		fprintf(stderr, "PQfsize() -- pointer to PQresult is null");
		return (int2) NULL;
	}

	if (field_num > (res->numAttributes - 1))
	{
		fprintf(stderr,
			  "PQfsize: ERROR! size of field %d(of %d) is not available",
				field_num, res->numAttributes - 1);
	}
	if (res->attDescs)
	{
		return res->attDescs[field_num].adtsize;
	}
	else
		return 0;
}

char	   *
PQcmdStatus(PGresult *res)
{
	if (!res)
	{
		fprintf(stderr, "PQcmdStatus() -- pointer to PQresult is null");
		return NULL;
	}
	return res->cmdStatus;
}

/*
   PQoidStatus -
	if the last command was an INSERT, return the oid string
	if not, return ""
*/
static char oidStatus[32] = {0};
const char *
PQoidStatus(PGresult *res)
{
	if (!res)
	{
		fprintf(stderr, "PQoidStatus () -- pointer to PQresult is null");
		return NULL;
	}

	oidStatus[0] = 0;
	if (!res->cmdStatus)
		return oidStatus;

	if (strncmp(res->cmdStatus, "INSERT", 6) == 0)
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
	{
		fprintf(stderr, "PQcmdTuples () -- pointer to PQresult is null");
		return NULL;
	}

	if (!res->cmdStatus)
		return "";

	if (strncmp(res->cmdStatus, "INSERT", 6) == 0 ||
		strncmp(res->cmdStatus, "DELETE", 6) == 0 ||
		strncmp(res->cmdStatus, "UPDATE", 6) == 0)
	{
		char	   *p = res->cmdStatus + 6;

		if (*p == 0)
		{
			fprintf(stderr, "PQcmdTuples (%s) -- short input from server",
					res->cmdStatus);
			return NULL;
		}
		p++;
		if (*(res->cmdStatus) != 'I')	/* UPDATE/DELETE */
			return (p);
		while (*p != ' ' && *p)
			p++;				/* INSERT: skip oid */
		if (*p == 0)
		{
			fprintf(stderr, "PQcmdTuples (INSERT) -- there's no # of tuples");
			return NULL;
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
char	   *
PQgetvalue(PGresult *res, int tup_num, int field_num)
{
	if (!res)
	{
		fprintf(stderr, "PQgetvalue: pointer to PQresult is null\n");
		return NULL;
	}
	else if (tup_num > (res->ntups - 1))
	{
		fprintf(stderr,
				"PQgetvalue: There is no row %d in the query results.  "
				"The highest numbered row is %d.\n",
				tup_num, res->ntups - 1);
		return NULL;
	}
	else if (field_num > (res->numAttributes - 1))
	{
		fprintf(stderr,
				"PQgetvalue: There is no field %d in the query results.  "
				"The highest numbered field is %d.\n",
				field_num, res->numAttributes - 1);
		return NULL;
	}

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
	if (!res)
	{
		fprintf(stderr, "PQgetlength() -- pointer to PQresult is null");
		return (int) NULL;
	}

	if (tup_num > (res->ntups - 1) ||
		field_num > (res->numAttributes - 1))
	{
		fprintf(stderr,
				"PQgetlength: ERROR! field %d(of %d) of row %d(of %d) "
				"is not available",
				field_num, res->numAttributes - 1, tup_num, res->ntups);
	}

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
	if (!res)
	{
		fprintf(stderr, "PQgetisnull() -- pointer to PQresult is null");
		return (int) NULL;
	}

	if (tup_num > (res->ntups - 1) ||
		field_num > (res->numAttributes - 1))
	{
		fprintf(stderr,
				"PQgetisnull: ERROR! field %d(of %d) of row %d(of %d) "
				"is not available",
				field_num, res->numAttributes - 1, tup_num, res->ntups);
	}

	if (res->tuples[tup_num][field_num].len == NULL_LEN)
		return 1;
	else
		return 0;
}
