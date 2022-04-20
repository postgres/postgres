/*-------------------------------------------------------------------------
 *
 *	fe-trace.c
 *	  functions for libpq protocol tracing
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-trace.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#endif

#include "libpq-fe.h"
#include "libpq-int.h"
#include "port/pg_bswap.h"


/* Enable tracing */
void
PQtrace(PGconn *conn, FILE *debug_port)
{
	if (conn == NULL)
		return;
	PQuntrace(conn);
	if (debug_port == NULL)
		return;

	conn->Pfdebug = debug_port;
	conn->traceFlags = 0;
}

/* Disable tracing */
void
PQuntrace(PGconn *conn)
{
	if (conn == NULL)
		return;
	if (conn->Pfdebug)
	{
		fflush(conn->Pfdebug);
		conn->Pfdebug = NULL;
	}

	conn->traceFlags = 0;
}

/* Set flags for current tracing session */
void
PQsetTraceFlags(PGconn *conn, int flags)
{
	if (conn == NULL)
		return;
	/* If PQtrace() failed, do nothing. */
	if (conn->Pfdebug == NULL)
		return;
	conn->traceFlags = flags;
}

/*
 * Print the current time, with microseconds, into a caller-supplied
 * buffer.
 * Cribbed from setup_formatted_log_time, but much simpler.
 */
static void
pqTraceFormatTimestamp(char *timestr, size_t ts_len)
{
	struct timeval tval;
	time_t		now;

	gettimeofday(&tval, NULL);

	/*
	 * MSVC's implementation of timeval uses a long for tv_sec, however,
	 * localtime() expects a time_t pointer.  Here we'll assign tv_sec to a
	 * local time_t variable so that we pass localtime() the correct pointer
	 * type.
	 */
	now = tval.tv_sec;
	strftime(timestr, ts_len,
			 "%Y-%m-%d %H:%M:%S",
			 localtime(&now));
	/* append microseconds */
	snprintf(timestr + strlen(timestr), ts_len - strlen(timestr),
			 ".%06u", (unsigned int) (tval.tv_usec));
}

/*
 *   pqTraceOutputByte1: output a 1-char message to the log
 */
static void
pqTraceOutputByte1(FILE *pfdebug, const char *data, int *cursor)
{
	const char *v = data + *cursor;

	/*
	 * Show non-printable data in hex format, including the terminating \0
	 * that completes ErrorResponse and NoticeResponse messages.
	 */
	if (!isprint((unsigned char) *v))
		fprintf(pfdebug, " \\x%02x", *v);
	else
		fprintf(pfdebug, " %c", *v);
	*cursor += 1;
}

/*
 *   pqTraceOutputInt16: output a 2-byte integer message to the log
 */
static int
pqTraceOutputInt16(FILE *pfdebug, const char *data, int *cursor)
{
	uint16		tmp;
	int			result;

	memcpy(&tmp, data + *cursor, 2);
	*cursor += 2;
	result = (int) pg_ntoh16(tmp);
	fprintf(pfdebug, " %d", result);

	return result;
}

/*
 *   pqTraceOutputInt32: output a 4-byte integer message to the log
 *
 * If 'suppress' is true, print a literal NNNN instead of the actual number.
 */
static int
pqTraceOutputInt32(FILE *pfdebug, const char *data, int *cursor, bool suppress)
{
	int			result;

	memcpy(&result, data + *cursor, 4);
	*cursor += 4;
	result = (int) pg_ntoh32(result);
	if (suppress)
		fprintf(pfdebug, " NNNN");
	else
		fprintf(pfdebug, " %d", result);

	return result;
}

/*
 *   pqTraceOutputString: output a string message to the log
 */
static void
pqTraceOutputString(FILE *pfdebug, const char *data, int *cursor, bool suppress)
{
	int			len;

	if (suppress)
	{
		fprintf(pfdebug, " \"SSSS\"");
		*cursor += strlen(data + *cursor) + 1;
	}
	else
	{
		len = fprintf(pfdebug, " \"%s\"", data + *cursor);

		/*
		 * This is a null-terminated string. So add 1 after subtracting 3
		 * which is the double quotes and space length from len.
		 */
		*cursor += (len - 3 + 1);
	}
}

/*
 * pqTraceOutputNchar: output a string of exactly len bytes message to the log
 */
static void
pqTraceOutputNchar(FILE *pfdebug, int len, const char *data, int *cursor)
{
	int			i,
				next;			/* first char not yet printed */
	const char *v = data + *cursor;

	fprintf(pfdebug, " \'");

	for (next = i = 0; i < len; ++i)
	{
		if (isprint((unsigned char) v[i]))
			continue;
		else
		{
			fwrite(v + next, 1, i - next, pfdebug);
			fprintf(pfdebug, "\\x%02x", v[i]);
			next = i + 1;
		}
	}
	if (next < len)
		fwrite(v + next, 1, len - next, pfdebug);

	fprintf(pfdebug, "\'");
	*cursor += len;
}

/*
 * Output functions by protocol message type
 */

/* NotificationResponse */
static void
pqTraceOutputA(FILE *f, const char *message, int *cursor, bool regress)
{
	fprintf(f, "NotificationResponse\t");
	pqTraceOutputInt32(f, message, cursor, regress);
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
}

/* Bind */
static void
pqTraceOutputB(FILE *f, const char *message, int *cursor)
{
	int			nparams;

	fprintf(f, "Bind\t");
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
	nparams = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt16(f, message, cursor);

	nparams = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nparams; i++)
	{
		int			nbytes;

		nbytes = pqTraceOutputInt32(f, message, cursor, false);
		if (nbytes == -1)
			continue;
		pqTraceOutputNchar(f, nbytes, message, cursor);
	}

	nparams = pqTraceOutputInt16(f, message, cursor);
	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt16(f, message, cursor);
}

/* Close(F) or CommandComplete(B) */
static void
pqTraceOutputC(FILE *f, bool toServer, const char *message, int *cursor)
{
	if (toServer)
	{
		fprintf(f, "Close\t");
		pqTraceOutputByte1(f, message, cursor);
		pqTraceOutputString(f, message, cursor, false);
	}
	else
	{
		fprintf(f, "CommandComplete\t");
		pqTraceOutputString(f, message, cursor, false);
	}
}

/* Describe(F) or DataRow(B) */
static void
pqTraceOutputD(FILE *f, bool toServer, const char *message, int *cursor)
{
	if (toServer)
	{
		fprintf(f, "Describe\t");
		pqTraceOutputByte1(f, message, cursor);
		pqTraceOutputString(f, message, cursor, false);
	}
	else
	{
		int			nfields;
		int			len;
		int			i;

		fprintf(f, "DataRow\t");
		nfields = pqTraceOutputInt16(f, message, cursor);
		for (i = 0; i < nfields; i++)
		{
			len = pqTraceOutputInt32(f, message, cursor, false);
			if (len == -1)
				continue;
			pqTraceOutputNchar(f, len, message, cursor);
		}
	}
}

/* NoticeResponse / ErrorResponse */
static void
pqTraceOutputNR(FILE *f, const char *type, const char *message, int *cursor,
				bool regress)
{
	fprintf(f, "%s\t", type);
	for (;;)
	{
		char		field;
		bool		suppress;

		pqTraceOutputByte1(f, message, cursor);
		field = message[*cursor - 1];
		if (field == '\0')
			break;

		suppress = regress && (field == 'L' || field == 'F' || field == 'R');
		pqTraceOutputString(f, message, cursor, suppress);
	}
}

/* Execute(F) or ErrorResponse(B) */
static void
pqTraceOutputE(FILE *f, bool toServer, const char *message, int *cursor, bool regress)
{
	if (toServer)
	{
		fprintf(f, "Execute\t");
		pqTraceOutputString(f, message, cursor, false);
		pqTraceOutputInt32(f, message, cursor, false);
	}
	else
		pqTraceOutputNR(f, "ErrorResponse", message, cursor, regress);
}

/* CopyFail */
static void
pqTraceOutputf(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "CopyFail\t");
	pqTraceOutputString(f, message, cursor, false);
}

/* FunctionCall */
static void
pqTraceOutputF(FILE *f, const char *message, int *cursor, bool regress)
{
	int			nfields;
	int			nbytes;

	fprintf(f, "FunctionCall\t");
	pqTraceOutputInt32(f, message, cursor, regress);
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(f, message, cursor);

	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
	{
		nbytes = pqTraceOutputInt32(f, message, cursor, false);
		if (nbytes == -1)
			continue;
		pqTraceOutputNchar(f, nbytes, message, cursor);
	}

	pqTraceOutputInt16(f, message, cursor);
}

/* CopyInResponse */
static void
pqTraceOutputG(FILE *f, const char *message, int *cursor)
{
	int			nfields;

	fprintf(f, "CopyInResponse\t");
	pqTraceOutputByte1(f, message, cursor);
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(f, message, cursor);
}

/* CopyOutResponse */
static void
pqTraceOutputH(FILE *f, const char *message, int *cursor)
{
	int			nfields;

	fprintf(f, "CopyOutResponse\t");
	pqTraceOutputByte1(f, message, cursor);
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(f, message, cursor);
}

/* BackendKeyData */
static void
pqTraceOutputK(FILE *f, const char *message, int *cursor, bool regress)
{
	fprintf(f, "BackendKeyData\t");
	pqTraceOutputInt32(f, message, cursor, regress);
	pqTraceOutputInt32(f, message, cursor, regress);
}

/* Parse */
static void
pqTraceOutputP(FILE *f, const char *message, int *cursor, bool regress)
{
	int			nparams;

	fprintf(f, "Parse\t");
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
	nparams = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt32(f, message, cursor, regress);
}

/* Query */
static void
pqTraceOutputQ(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "Query\t");
	pqTraceOutputString(f, message, cursor, false);
}

/* Authentication */
static void
pqTraceOutputR(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "Authentication\t");
	pqTraceOutputInt32(f, message, cursor, false);
}

/* ParameterStatus */
static void
pqTraceOutputS(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "ParameterStatus\t");
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
}

/* ParameterDescription */
static void
pqTraceOutputt(FILE *f, const char *message, int *cursor, bool regress)
{
	int			nfields;

	fprintf(f, "ParameterDescription\t");
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt32(f, message, cursor, regress);
}

/* RowDescription */
static void
pqTraceOutputT(FILE *f, const char *message, int *cursor, bool regress)
{
	int			nfields;

	fprintf(f, "RowDescription\t");
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
	{
		pqTraceOutputString(f, message, cursor, false);
		pqTraceOutputInt32(f, message, cursor, regress);
		pqTraceOutputInt16(f, message, cursor);
		pqTraceOutputInt32(f, message, cursor, regress);
		pqTraceOutputInt16(f, message, cursor);
		pqTraceOutputInt32(f, message, cursor, false);
		pqTraceOutputInt16(f, message, cursor);
	}
}

/* NegotiateProtocolVersion */
static void
pqTraceOutputv(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "NegotiateProtocolVersion\t");
	pqTraceOutputInt32(f, message, cursor, false);
	pqTraceOutputInt32(f, message, cursor, false);
}

/* FunctionCallResponse */
static void
pqTraceOutputV(FILE *f, const char *message, int *cursor)
{
	int			len;

	fprintf(f, "FunctionCallResponse\t");
	len = pqTraceOutputInt32(f, message, cursor, false);
	if (len != -1)
		pqTraceOutputNchar(f, len, message, cursor);
}

/* CopyBothResponse */
static void
pqTraceOutputW(FILE *f, const char *message, int *cursor, int length)
{
	fprintf(f, "CopyBothResponse\t");
	pqTraceOutputByte1(f, message, cursor);

	while (length > *cursor)
		pqTraceOutputInt16(f, message, cursor);
}

/* ReadyForQuery */
static void
pqTraceOutputZ(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "ReadyForQuery\t");
	pqTraceOutputByte1(f, message, cursor);
}

/*
 * Print the given message to the trace output stream.
 */
void
pqTraceOutputMessage(PGconn *conn, const char *message, bool toServer)
{
	char		id;
	int			length;
	char	   *prefix = toServer ? "F" : "B";
	int			logCursor = 0;
	bool		regress;

	if ((conn->traceFlags & PQTRACE_SUPPRESS_TIMESTAMPS) == 0)
	{
		char		timestr[128];

		pqTraceFormatTimestamp(timestr, sizeof(timestr));
		fprintf(conn->Pfdebug, "%s\t", timestr);
	}
	regress = (conn->traceFlags & PQTRACE_REGRESS_MODE) != 0;

	id = message[logCursor++];

	memcpy(&length, message + logCursor, 4);
	length = (int) pg_ntoh32(length);
	logCursor += 4;

	/*
	 * In regress mode, suppress the length of ErrorResponse and
	 * NoticeResponse.  The F (file name), L (line number) and R (routine
	 * name) fields can change as server code is modified, and if their
	 * lengths differ from the originals, that would break tests.
	 */
	if (regress && !toServer && (id == 'E' || id == 'N'))
		fprintf(conn->Pfdebug, "%s\tNN\t", prefix);
	else
		fprintf(conn->Pfdebug, "%s\t%d\t", prefix, length);

	switch (id)
	{
		case '1':
			fprintf(conn->Pfdebug, "ParseComplete");
			/* No message content */
			break;
		case '2':
			fprintf(conn->Pfdebug, "BindComplete");
			/* No message content */
			break;
		case '3':
			fprintf(conn->Pfdebug, "CloseComplete");
			/* No message content */
			break;
		case 'A':				/* Notification Response */
			pqTraceOutputA(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'B':				/* Bind */
			pqTraceOutputB(conn->Pfdebug, message, &logCursor);
			break;
		case 'c':
			fprintf(conn->Pfdebug, "CopyDone");
			/* No message content */
			break;
		case 'C':				/* Close(F) or Command Complete(B) */
			pqTraceOutputC(conn->Pfdebug, toServer, message, &logCursor);
			break;
		case 'd':				/* Copy Data */
			/* Drop COPY data to reduce the overhead of logging. */
			break;
		case 'D':				/* Describe(F) or Data Row(B) */
			pqTraceOutputD(conn->Pfdebug, toServer, message, &logCursor);
			break;
		case 'E':				/* Execute(F) or Error Response(B) */
			pqTraceOutputE(conn->Pfdebug, toServer, message, &logCursor,
						   regress);
			break;
		case 'f':				/* Copy Fail */
			pqTraceOutputf(conn->Pfdebug, message, &logCursor);
			break;
		case 'F':				/* Function Call */
			pqTraceOutputF(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'G':				/* Start Copy In */
			pqTraceOutputG(conn->Pfdebug, message, &logCursor);
			break;
		case 'H':				/* Flush(F) or Start Copy Out(B) */
			if (!toServer)
				pqTraceOutputH(conn->Pfdebug, message, &logCursor);
			else
				fprintf(conn->Pfdebug, "Flush");	/* no message content */
			break;
		case 'I':
			fprintf(conn->Pfdebug, "EmptyQueryResponse");
			/* No message content */
			break;
		case 'K':				/* secret key data from the backend */
			pqTraceOutputK(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'n':
			fprintf(conn->Pfdebug, "NoData");
			/* No message content */
			break;
		case 'N':
			pqTraceOutputNR(conn->Pfdebug, "NoticeResponse", message,
							&logCursor, regress);
			break;
		case 'P':				/* Parse */
			pqTraceOutputP(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'Q':				/* Query */
			pqTraceOutputQ(conn->Pfdebug, message, &logCursor);
			break;
		case 'R':				/* Authentication */
			pqTraceOutputR(conn->Pfdebug, message, &logCursor);
			break;
		case 's':
			fprintf(conn->Pfdebug, "PortalSuspended");
			/* No message content */
			break;
		case 'S':				/* Parameter Status(B) or Sync(F) */
			if (!toServer)
				pqTraceOutputS(conn->Pfdebug, message, &logCursor);
			else
				fprintf(conn->Pfdebug, "Sync"); /* no message content */
			break;
		case 't':				/* Parameter Description */
			pqTraceOutputt(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'T':				/* Row Description */
			pqTraceOutputT(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'v':				/* Negotiate Protocol Version */
			pqTraceOutputv(conn->Pfdebug, message, &logCursor);
			break;
		case 'V':				/* Function Call response */
			pqTraceOutputV(conn->Pfdebug, message, &logCursor);
			break;
		case 'W':				/* Start Copy Both */
			pqTraceOutputW(conn->Pfdebug, message, &logCursor, length);
			break;
		case 'X':
			fprintf(conn->Pfdebug, "Terminate");
			/* No message content */
			break;
		case 'Z':				/* Ready For Query */
			pqTraceOutputZ(conn->Pfdebug, message, &logCursor);
			break;
		default:
			fprintf(conn->Pfdebug, "Unknown message: %02x", id);
			break;
	}

	fputc('\n', conn->Pfdebug);

	/*
	 * Verify the printing routine did it right.  Note that the one-byte
	 * message identifier is not included in the length, but our cursor does
	 * include it.
	 */
	if (logCursor - 1 != length)
		fprintf(conn->Pfdebug,
				"mismatched message length: consumed %d, expected %d\n",
				logCursor - 1, length);
}

/*
 * Print special messages (those containing no type byte) to the trace output
 * stream.
 */
void
pqTraceOutputNoTypeByteMessage(PGconn *conn, const char *message)
{
	int			length;
	int			logCursor = 0;

	if ((conn->traceFlags & PQTRACE_SUPPRESS_TIMESTAMPS) == 0)
	{
		char		timestr[128];

		pqTraceFormatTimestamp(timestr, sizeof(timestr));
		fprintf(conn->Pfdebug, "%s\t", timestr);
	}

	memcpy(&length, message + logCursor, 4);
	length = (int) pg_ntoh32(length);
	logCursor += 4;

	fprintf(conn->Pfdebug, "F\t%d\t", length);

	switch (length)
	{
		case 16:				/* CancelRequest */
			fprintf(conn->Pfdebug, "CancelRequest\t");
			pqTraceOutputInt32(conn->Pfdebug, message, &logCursor, false);
			pqTraceOutputInt32(conn->Pfdebug, message, &logCursor, false);
			pqTraceOutputInt32(conn->Pfdebug, message, &logCursor, false);
			break;
		case 8:					/* GSSENCRequest or SSLRequest */
			/* These messages do not reach here. */
		default:
			fprintf(conn->Pfdebug, "Unknown message: length is %d", length);
			break;
	}

	fputc('\n', conn->Pfdebug);
}
