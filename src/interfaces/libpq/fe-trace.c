/*-------------------------------------------------------------------------
 *
 *	fe-trace.c
 *	  functions for libpq protocol tracing
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
 * Cribbed from get_formatted_log_time, but much simpler.
 */
static void
pqTraceFormatTimestamp(char *timestr, size_t ts_len)
{
	struct timeval tval;
	time_t		now;
	struct tm	tmbuf;

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
			 localtime_r(&now, &tmbuf));
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
 *
 * If 'suppress' is true, print a literal "SSSS" instead of the actual string.
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
 *
 * If 'suppress' is true, print a literal 'BBBB' instead of the actual bytes.
 */
static void
pqTraceOutputNchar(FILE *pfdebug, int len, const char *data, int *cursor, bool suppress)
{
	int			i,
				next;			/* first char not yet printed */
	const char *v = data + *cursor;

	if (suppress)
	{
		fprintf(pfdebug, " 'BBBB'");
		*cursor += len;
		return;
	}

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

static void
pqTraceOutput_NotificationResponse(FILE *f, const char *message, int *cursor, bool regress)
{
	fprintf(f, "NotificationResponse\t");
	pqTraceOutputInt32(f, message, cursor, regress);
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
}

static void
pqTraceOutput_Bind(FILE *f, const char *message, int *cursor)
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
		pqTraceOutputNchar(f, nbytes, message, cursor, false);
	}

	nparams = pqTraceOutputInt16(f, message, cursor);
	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt16(f, message, cursor);
}

static void
pqTraceOutput_Close(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "Close\t");
	pqTraceOutputByte1(f, message, cursor);
	pqTraceOutputString(f, message, cursor, false);
}

static void
pqTraceOutput_CommandComplete(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "CommandComplete\t");
	pqTraceOutputString(f, message, cursor, false);
}

static void
pqTraceOutput_CopyData(FILE *f, const char *message, int *cursor, int length,
					   bool suppress)
{
	fprintf(f, "CopyData\t");
	pqTraceOutputNchar(f, length - *cursor + 1, message, cursor, suppress);
}

static void
pqTraceOutput_DataRow(FILE *f, const char *message, int *cursor)
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
		pqTraceOutputNchar(f, len, message, cursor, false);
	}
}

static void
pqTraceOutput_Describe(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "Describe\t");
	pqTraceOutputByte1(f, message, cursor);
	pqTraceOutputString(f, message, cursor, false);
}

/* shared code NoticeResponse / ErrorResponse */
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

static void
pqTraceOutput_ErrorResponse(FILE *f, const char *message, int *cursor, bool regress)
{
	pqTraceOutputNR(f, "ErrorResponse", message, cursor, regress);
}

static void
pqTraceOutput_NoticeResponse(FILE *f, const char *message, int *cursor, bool regress)
{
	pqTraceOutputNR(f, "NoticeResponse", message, cursor, regress);
}

static void
pqTraceOutput_Execute(FILE *f, const char *message, int *cursor, bool regress)
{
	fprintf(f, "Execute\t");
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputInt32(f, message, cursor, false);
}

static void
pqTraceOutput_CopyFail(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "CopyFail\t");
	pqTraceOutputString(f, message, cursor, false);
}

static void
pqTraceOutput_GSSResponse(FILE *f, const char *message, int *cursor,
						  int length, bool regress)
{
	fprintf(f, "GSSResponse\t");
	pqTraceOutputNchar(f, length - *cursor + 1, message, cursor, regress);
}

static void
pqTraceOutput_PasswordMessage(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "PasswordMessage\t");
	pqTraceOutputString(f, message, cursor, false);
}

static void
pqTraceOutput_SASLInitialResponse(FILE *f, const char *message, int *cursor,
								  bool regress)
{
	int			initialResponse;

	fprintf(f, "SASLInitialResponse\t");
	pqTraceOutputString(f, message, cursor, false);
	initialResponse = pqTraceOutputInt32(f, message, cursor, false);
	if (initialResponse != -1)
		pqTraceOutputNchar(f, initialResponse, message, cursor, regress);
}

static void
pqTraceOutput_SASLResponse(FILE *f, const char *message, int *cursor,
						   int length, bool regress)
{
	fprintf(f, "SASLResponse\t");
	pqTraceOutputNchar(f, length - *cursor + 1, message, cursor, regress);
}

static void
pqTraceOutput_FunctionCall(FILE *f, const char *message, int *cursor, bool regress)
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
		pqTraceOutputNchar(f, nbytes, message, cursor, false);
	}

	pqTraceOutputInt16(f, message, cursor);
}

static void
pqTraceOutput_CopyInResponse(FILE *f, const char *message, int *cursor)
{
	int			nfields;

	fprintf(f, "CopyInResponse\t");
	pqTraceOutputByte1(f, message, cursor);
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(f, message, cursor);
}

static void
pqTraceOutput_CopyOutResponse(FILE *f, const char *message, int *cursor)
{
	int			nfields;

	fprintf(f, "CopyOutResponse\t");
	pqTraceOutputByte1(f, message, cursor);
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(f, message, cursor);
}

static void
pqTraceOutput_BackendKeyData(FILE *f, const char *message, int *cursor, bool regress)
{
	fprintf(f, "BackendKeyData\t");
	pqTraceOutputInt32(f, message, cursor, regress);
	pqTraceOutputInt32(f, message, cursor, regress);
}

static void
pqTraceOutput_Parse(FILE *f, const char *message, int *cursor, bool regress)
{
	int			nparams;

	fprintf(f, "Parse\t");
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
	nparams = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt32(f, message, cursor, regress);
}

static void
pqTraceOutput_Query(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "Query\t");
	pqTraceOutputString(f, message, cursor, false);
}

static void
pqTraceOutput_Authentication(FILE *f, const char *message, int *cursor,
							 int length, bool suppress)
{
	int			authType = 0;

	memcpy(&authType, message + *cursor, 4);
	authType = (int) pg_ntoh32(authType);
	*cursor += 4;
	switch (authType)
	{
		case AUTH_REQ_OK:
			fprintf(f, "AuthenticationOk");
			break;
			/* AUTH_REQ_KRB4 not supported */
			/* AUTH_REQ_KRB5 not supported */
		case AUTH_REQ_PASSWORD:
			fprintf(f, "AuthenticationCleartextPassword");
			break;
			/* AUTH_REQ_CRYPT not supported */
		case AUTH_REQ_MD5:
			fprintf(f, "AuthenticationMD5Password");
			break;
		case AUTH_REQ_GSS:
			fprintf(f, "AuthenticationGSS");
			break;
		case AUTH_REQ_GSS_CONT:
			fprintf(f, "AuthenticationGSSContinue\t");
			pqTraceOutputNchar(f, length - *cursor + 1, message, cursor,
							   suppress);
			break;
		case AUTH_REQ_SSPI:
			fprintf(f, "AuthenticationSSPI");
			break;
		case AUTH_REQ_SASL:
			fprintf(f, "AuthenticationSASL\t");
			while (message[*cursor] != '\0')
				pqTraceOutputString(f, message, cursor, false);
			pqTraceOutputString(f, message, cursor, false);
			break;
		case AUTH_REQ_SASL_CONT:
			fprintf(f, "AuthenticationSASLContinue\t");
			pqTraceOutputNchar(f, length - *cursor + 1, message, cursor,
							   suppress);
			break;
		case AUTH_REQ_SASL_FIN:
			fprintf(f, "AuthenticationSASLFinal\t");
			pqTraceOutputNchar(f, length - *cursor + 1, message, cursor,
							   suppress);
			break;
		default:
			fprintf(f, "Unknown authentication message %d", authType);
	}
}

static void
pqTraceOutput_ParameterStatus(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "ParameterStatus\t");
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
}

static void
pqTraceOutput_ParameterDescription(FILE *f, const char *message, int *cursor, bool regress)
{
	int			nfields;

	fprintf(f, "ParameterDescription\t");
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt32(f, message, cursor, regress);
}

static void
pqTraceOutput_RowDescription(FILE *f, const char *message, int *cursor, bool regress)
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

static void
pqTraceOutput_NegotiateProtocolVersion(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "NegotiateProtocolVersion\t");
	pqTraceOutputInt32(f, message, cursor, false);
	pqTraceOutputInt32(f, message, cursor, false);
}

static void
pqTraceOutput_FunctionCallResponse(FILE *f, const char *message, int *cursor)
{
	int			len;

	fprintf(f, "FunctionCallResponse\t");
	len = pqTraceOutputInt32(f, message, cursor, false);
	if (len != -1)
		pqTraceOutputNchar(f, len, message, cursor, false);
}

static void
pqTraceOutput_CopyBothResponse(FILE *f, const char *message, int *cursor, int length)
{
	fprintf(f, "CopyBothResponse\t");
	pqTraceOutputByte1(f, message, cursor);

	while (length > *cursor)
		pqTraceOutputInt16(f, message, cursor);
}

static void
pqTraceOutput_ReadyForQuery(FILE *f, const char *message, int *cursor)
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
	if (regress && !toServer && (id == PqMsg_ErrorResponse || id == PqMsg_NoticeResponse))
		fprintf(conn->Pfdebug, "%s\tNN\t", prefix);
	else
		fprintf(conn->Pfdebug, "%s\t%d\t", prefix, length);

	switch (id)
	{
		case PqMsg_ParseComplete:
			fprintf(conn->Pfdebug, "ParseComplete");
			/* No message content */
			break;
		case PqMsg_BindComplete:
			fprintf(conn->Pfdebug, "BindComplete");
			/* No message content */
			break;
		case PqMsg_CloseComplete:
			fprintf(conn->Pfdebug, "CloseComplete");
			/* No message content */
			break;
		case PqMsg_NotificationResponse:
			pqTraceOutput_NotificationResponse(conn->Pfdebug, message, &logCursor, regress);
			break;
		case PqMsg_Bind:
			pqTraceOutput_Bind(conn->Pfdebug, message, &logCursor);
			break;
		case PqMsg_CopyDone:
			fprintf(conn->Pfdebug, "CopyDone");
			/* No message content */
			break;
		case PqMsg_CommandComplete:
			/* Close(F) and CommandComplete(B) use the same identifier. */
			Assert(PqMsg_Close == PqMsg_CommandComplete);
			if (toServer)
				pqTraceOutput_Close(conn->Pfdebug, message, &logCursor);
			else
				pqTraceOutput_CommandComplete(conn->Pfdebug, message, &logCursor);
			break;
		case PqMsg_CopyData:
			pqTraceOutput_CopyData(conn->Pfdebug, message, &logCursor,
								   length, regress);
			break;
		case PqMsg_Describe:
			/* Describe(F) and DataRow(B) use the same identifier. */
			Assert(PqMsg_Describe == PqMsg_DataRow);
			if (toServer)
				pqTraceOutput_Describe(conn->Pfdebug, message, &logCursor);
			else
				pqTraceOutput_DataRow(conn->Pfdebug, message, &logCursor);
			break;
		case PqMsg_Execute:
			/* Execute(F) and ErrorResponse(B) use the same identifier. */
			Assert(PqMsg_Execute == PqMsg_ErrorResponse);
			if (toServer)
				pqTraceOutput_Execute(conn->Pfdebug, message, &logCursor, regress);
			else
				pqTraceOutput_ErrorResponse(conn->Pfdebug, message, &logCursor, regress);
			break;
		case PqMsg_CopyFail:
			pqTraceOutput_CopyFail(conn->Pfdebug, message, &logCursor);
			break;
		case PqMsg_GSSResponse:
			Assert(PqMsg_GSSResponse == PqMsg_PasswordMessage);
			Assert(PqMsg_GSSResponse == PqMsg_SASLInitialResponse);
			Assert(PqMsg_GSSResponse == PqMsg_SASLResponse);

			/*
			 * These messages share a common type byte, so we discriminate by
			 * having the code store the auth type separately.
			 */
			switch (conn->current_auth_response)
			{
				case AUTH_RESPONSE_GSS:
					pqTraceOutput_GSSResponse(conn->Pfdebug, message,
											  &logCursor, length, regress);
					break;
				case AUTH_RESPONSE_PASSWORD:
					pqTraceOutput_PasswordMessage(conn->Pfdebug, message,
												  &logCursor);
					break;
				case AUTH_RESPONSE_SASL_INITIAL:
					pqTraceOutput_SASLInitialResponse(conn->Pfdebug, message,
													  &logCursor, regress);
					break;
				case AUTH_RESPONSE_SASL:
					pqTraceOutput_SASLResponse(conn->Pfdebug, message,
											   &logCursor, length, regress);
					break;
				default:
					fprintf(conn->Pfdebug, "UnknownAuthenticationResponse");
					break;
			}
			conn->current_auth_response = '\0';
			break;
		case PqMsg_FunctionCall:
			pqTraceOutput_FunctionCall(conn->Pfdebug, message, &logCursor, regress);
			break;
		case PqMsg_CopyInResponse:
			pqTraceOutput_CopyInResponse(conn->Pfdebug, message, &logCursor);
			break;
		case PqMsg_Flush:
			/* Flush(F) and CopyOutResponse(B) use the same identifier */
			Assert(PqMsg_CopyOutResponse == PqMsg_Flush);
			if (toServer)
				fprintf(conn->Pfdebug, "Flush");	/* no message content */
			else
				pqTraceOutput_CopyOutResponse(conn->Pfdebug, message, &logCursor);
			break;
		case PqMsg_EmptyQueryResponse:
			fprintf(conn->Pfdebug, "EmptyQueryResponse");
			/* No message content */
			break;
		case PqMsg_BackendKeyData:
			pqTraceOutput_BackendKeyData(conn->Pfdebug, message, &logCursor, regress);
			break;
		case PqMsg_NoData:
			fprintf(conn->Pfdebug, "NoData");
			/* No message content */
			break;
		case PqMsg_NoticeResponse:
			pqTraceOutput_NoticeResponse(conn->Pfdebug, message, &logCursor, regress);
			break;
		case PqMsg_Parse:
			pqTraceOutput_Parse(conn->Pfdebug, message, &logCursor, regress);
			break;
		case PqMsg_Query:
			pqTraceOutput_Query(conn->Pfdebug, message, &logCursor);
			break;
		case PqMsg_AuthenticationRequest:
			pqTraceOutput_Authentication(conn->Pfdebug, message, &logCursor,
										 length, regress);
			break;
		case PqMsg_PortalSuspended:
			fprintf(conn->Pfdebug, "PortalSuspended");
			/* No message content */
			break;
		case PqMsg_Sync:
			/* ParameterStatus(B) and Sync(F) use the same identifier */
			Assert(PqMsg_ParameterStatus == PqMsg_Sync);
			if (toServer)
				fprintf(conn->Pfdebug, "Sync"); /* no message content */
			else
				pqTraceOutput_ParameterStatus(conn->Pfdebug, message, &logCursor);
			break;
		case PqMsg_ParameterDescription:
			pqTraceOutput_ParameterDescription(conn->Pfdebug, message, &logCursor, regress);
			break;
		case PqMsg_RowDescription:
			pqTraceOutput_RowDescription(conn->Pfdebug, message, &logCursor, regress);
			break;
		case PqMsg_NegotiateProtocolVersion:
			pqTraceOutput_NegotiateProtocolVersion(conn->Pfdebug, message, &logCursor);
			break;
		case PqMsg_FunctionCallResponse:
			pqTraceOutput_FunctionCallResponse(conn->Pfdebug, message, &logCursor);
			break;
		case PqMsg_CopyBothResponse:
			pqTraceOutput_CopyBothResponse(conn->Pfdebug, message, &logCursor, length);
			break;
		case PqMsg_Terminate:
			fprintf(conn->Pfdebug, "Terminate");
			/* No message content */
			break;
		case PqMsg_ReadyForQuery:
			pqTraceOutput_ReadyForQuery(conn->Pfdebug, message, &logCursor);
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
	int			version;
	bool		regress;
	int			logCursor = 0;

	regress = (conn->traceFlags & PQTRACE_REGRESS_MODE) != 0;

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

	if (length < 8)
	{
		fprintf(conn->Pfdebug, "Unknown message\n");
		return;
	}

	memcpy(&version, message + logCursor, 4);
	version = (int) pg_ntoh32(version);

	if (version == CANCEL_REQUEST_CODE && length >= 16)
	{
		fprintf(conn->Pfdebug, "CancelRequest\t");
		pqTraceOutputInt16(conn->Pfdebug, message, &logCursor);
		pqTraceOutputInt16(conn->Pfdebug, message, &logCursor);
		pqTraceOutputInt32(conn->Pfdebug, message, &logCursor, regress);
		pqTraceOutputInt32(conn->Pfdebug, message, &logCursor, regress);
	}
	else if (version == NEGOTIATE_SSL_CODE)
	{
		fprintf(conn->Pfdebug, "SSLRequest\t");
		pqTraceOutputInt16(conn->Pfdebug, message, &logCursor);
		pqTraceOutputInt16(conn->Pfdebug, message, &logCursor);
	}
	else if (version == NEGOTIATE_GSS_CODE)
	{
		fprintf(conn->Pfdebug, "GSSENCRequest\t");
		pqTraceOutputInt16(conn->Pfdebug, message, &logCursor);
		pqTraceOutputInt16(conn->Pfdebug, message, &logCursor);
	}
	else
	{
		fprintf(conn->Pfdebug, "StartupMessage\t");
		pqTraceOutputInt16(conn->Pfdebug, message, &logCursor);
		pqTraceOutputInt16(conn->Pfdebug, message, &logCursor);
		while (message[logCursor] != '\0')
		{
			/* XXX should we suppress anything in regress mode? */
			pqTraceOutputString(conn->Pfdebug, message, &logCursor, false);
			pqTraceOutputString(conn->Pfdebug, message, &logCursor, false);
		}
	}

	fputc('\n', conn->Pfdebug);
}

/*
 * Trace a single-byte backend response received for a known request
 * type the frontend previously sent.  Only useful for the simplest of
 * FE/BE interaction workflows such as SSL/GSS encryption requests.
 */
void
pqTraceOutputCharResponse(PGconn *conn, const char *responseType,
						  char response)
{
	if ((conn->traceFlags & PQTRACE_SUPPRESS_TIMESTAMPS) == 0)
	{
		char		timestr[128];

		pqTraceFormatTimestamp(timestr, sizeof(timestr));
		fprintf(conn->Pfdebug, "%s\t", timestr);
	}

	fprintf(conn->Pfdebug, "B\t1\t%s\t %c\n", responseType, response);
}
