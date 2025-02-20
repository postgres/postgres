/*-------------------------------------------------------------------------
 *
 * jsonlog.c
 *	  JSON logging
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/jsonlog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "libpq/libpq-be.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "postmaster/syslogger.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/backend_status.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/ps_status.h"

static void appendJSONKeyValueFmt(StringInfo buf, const char *key,
								  bool escape_key,
								  const char *fmt,...) pg_attribute_printf(4, 5);

/*
 * appendJSONKeyValue
 *
 * Append to a StringInfo a comma followed by a JSON key and a value.
 * The key is always escaped.  The value can be escaped optionally, that
 * is dependent on the data type of the key.
 */
static void
appendJSONKeyValue(StringInfo buf, const char *key, const char *value,
				   bool escape_value)
{
	Assert(key != NULL);

	if (value == NULL)
		return;

	appendStringInfoChar(buf, ',');
	escape_json(buf, key);
	appendStringInfoChar(buf, ':');

	if (escape_value)
		escape_json(buf, value);
	else
		appendStringInfoString(buf, value);
}


/*
 * appendJSONKeyValueFmt
 *
 * Evaluate the fmt string and then invoke appendJSONKeyValue() as the
 * value of the JSON property.  Both the key and value will be escaped by
 * appendJSONKeyValue().
 */
static void
appendJSONKeyValueFmt(StringInfo buf, const char *key,
					  bool escape_key, const char *fmt,...)
{
	int			save_errno = errno;
	size_t		len = 128;		/* initial assumption about buffer size */
	char	   *value;

	for (;;)
	{
		va_list		args;
		size_t		newlen;

		/* Allocate result buffer */
		value = (char *) palloc(len);

		/* Try to format the data. */
		errno = save_errno;
		va_start(args, fmt);
		newlen = pvsnprintf(value, len, fmt, args);
		va_end(args);

		if (newlen < len)
			break;				/* success */

		/* Release buffer and loop around to try again with larger len. */
		pfree(value);
		len = newlen;
	}

	appendJSONKeyValue(buf, key, value, escape_key);

	/* Clean up */
	pfree(value);
}

/*
 * Write logs in json format.
 */
void
write_jsonlog(ErrorData *edata)
{
	StringInfoData buf;
	char	   *start_time;
	char	   *log_time;

	/* static counter for line numbers */
	static long log_line_number = 0;

	/* Has the counter been reset in the current process? */
	static int	log_my_pid = 0;

	/*
	 * This is one of the few places where we'd rather not inherit a static
	 * variable's value from the postmaster.  But since we will, reset it when
	 * MyProcPid changes.
	 */
	if (log_my_pid != MyProcPid)
	{
		log_line_number = 0;
		log_my_pid = MyProcPid;
		reset_formatted_start_time();
	}
	log_line_number++;

	initStringInfo(&buf);

	/* Initialize string */
	appendStringInfoChar(&buf, '{');

	/* timestamp with milliseconds */
	log_time = get_formatted_log_time();

	/*
	 * First property does not use appendJSONKeyValue as it does not have
	 * comma prefix.
	 */
	escape_json(&buf, "timestamp");
	appendStringInfoChar(&buf, ':');
	escape_json(&buf, log_time);

	/* username */
	if (MyProcPort)
		appendJSONKeyValue(&buf, "user", MyProcPort->user_name, true);

	/* database name */
	if (MyProcPort)
		appendJSONKeyValue(&buf, "dbname", MyProcPort->database_name, true);

	/* Process ID */
	if (MyProcPid != 0)
		appendJSONKeyValueFmt(&buf, "pid", false, "%d", MyProcPid);

	/* Remote host and port */
	if (MyProcPort && MyProcPort->remote_host)
	{
		appendJSONKeyValue(&buf, "remote_host", MyProcPort->remote_host, true);
		if (MyProcPort->remote_port && MyProcPort->remote_port[0] != '\0')
			appendJSONKeyValue(&buf, "remote_port", MyProcPort->remote_port, false);
	}

	/* Session id */
	appendJSONKeyValueFmt(&buf, "session_id", true, INT64_HEX_FORMAT ".%x",
						  MyStartTime, MyProcPid);

	/* Line number */
	appendJSONKeyValueFmt(&buf, "line_num", false, "%ld", log_line_number);

	/* PS display */
	if (MyProcPort)
	{
		StringInfoData msgbuf;
		const char *psdisp;
		int			displen;

		initStringInfo(&msgbuf);
		psdisp = get_ps_display(&displen);
		appendBinaryStringInfo(&msgbuf, psdisp, displen);
		appendJSONKeyValue(&buf, "ps", msgbuf.data, true);

		pfree(msgbuf.data);
	}

	/* session start timestamp */
	start_time = get_formatted_start_time();
	appendJSONKeyValue(&buf, "session_start", start_time, true);

	/* Virtual transaction id */
	/* keep VXID format in sync with lockfuncs.c */
	if (MyProc != NULL && MyProc->vxid.procNumber != INVALID_PROC_NUMBER)
		appendJSONKeyValueFmt(&buf, "vxid", true, "%d/%u",
							  MyProc->vxid.procNumber, MyProc->vxid.lxid);

	/* Transaction id */
	appendJSONKeyValueFmt(&buf, "txid", false, "%u",
						  GetTopTransactionIdIfAny());

	/* Error severity */
	if (edata->elevel)
		appendJSONKeyValue(&buf, "error_severity",
						   error_severity(edata->elevel), true);

	/* SQL state code */
	if (edata->sqlerrcode)
		appendJSONKeyValue(&buf, "state_code",
						   unpack_sql_state(edata->sqlerrcode), true);

	/* errmessage */
	appendJSONKeyValue(&buf, "message", edata->message, true);

	/* errdetail or error_detail log */
	if (edata->detail_log)
		appendJSONKeyValue(&buf, "detail", edata->detail_log, true);
	else
		appendJSONKeyValue(&buf, "detail", edata->detail, true);

	/* errhint */
	if (edata->hint)
		appendJSONKeyValue(&buf, "hint", edata->hint, true);

	/* internal query */
	if (edata->internalquery)
		appendJSONKeyValue(&buf, "internal_query", edata->internalquery,
						   true);

	/* if printed internal query, print internal pos too */
	if (edata->internalpos > 0 && edata->internalquery != NULL)
		appendJSONKeyValueFmt(&buf, "internal_position", false, "%d",
							  edata->internalpos);

	/* errcontext */
	if (edata->context && !edata->hide_ctx)
		appendJSONKeyValue(&buf, "context", edata->context, true);

	/* user query --- only reported if not disabled by the caller */
	if (check_log_of_query(edata))
	{
		appendJSONKeyValue(&buf, "statement", debug_query_string, true);
		if (edata->cursorpos > 0)
			appendJSONKeyValueFmt(&buf, "cursor_position", false, "%d",
								  edata->cursorpos);
	}

	/* file error location */
	if (Log_error_verbosity >= PGERROR_VERBOSE)
	{
		if (edata->funcname)
			appendJSONKeyValue(&buf, "func_name", edata->funcname, true);
		if (edata->filename)
		{
			appendJSONKeyValue(&buf, "file_name", edata->filename, true);
			appendJSONKeyValueFmt(&buf, "file_line_num", false, "%d",
								  edata->lineno);
		}
	}

	/* Application name */
	if (application_name && application_name[0] != '\0')
		appendJSONKeyValue(&buf, "application_name", application_name, true);

	/* backend type */
	appendJSONKeyValue(&buf, "backend_type", get_backend_type_for_log(), true);

	/* leader PID */
	if (MyProc)
	{
		PGPROC	   *leader = MyProc->lockGroupLeader;

		/*
		 * Show the leader only for active parallel workers.  This leaves out
		 * the leader of a parallel group.
		 */
		if (leader && leader->pid != MyProcPid)
			appendJSONKeyValueFmt(&buf, "leader_pid", false, "%d",
								  leader->pid);
	}

	/* query id */
	appendJSONKeyValueFmt(&buf, "query_id", false, "%lld",
						  (long long) pgstat_get_my_query_id());

	/* Finish string */
	appendStringInfoChar(&buf, '}');
	appendStringInfoChar(&buf, '\n');

	/* If in the syslogger process, try to write messages direct to file */
	if (MyBackendType == B_LOGGER)
		write_syslogger_file(buf.data, buf.len, LOG_DESTINATION_JSONLOG);
	else
		write_pipe_chunks(buf.data, buf.len, LOG_DESTINATION_JSONLOG);

	pfree(buf.data);
}
