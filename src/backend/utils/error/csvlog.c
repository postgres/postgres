/*-------------------------------------------------------------------------
 *
 * csvlog.c
 *	  CSV logging
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/csvlog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "postmaster/syslogger.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/backend_status.h"
#include "utils/guc.h"
#include "utils/ps_status.h"


/*
 * append a CSV'd version of a string to a StringInfo
 * We use the PostgreSQL defaults for CSV, i.e. quote = escape = '"'
 * If it's NULL, append nothing.
 */
static inline void
appendCSVLiteral(StringInfo buf, const char *data)
{
	const char *p = data;
	char		c;

	/* avoid confusing an empty string with NULL */
	if (p == NULL)
		return;

	appendStringInfoCharMacro(buf, '"');
	while ((c = *p++) != '\0')
	{
		if (c == '"')
			appendStringInfoCharMacro(buf, '"');
		appendStringInfoCharMacro(buf, c);
	}
	appendStringInfoCharMacro(buf, '"');
}

/*
 * write_csvlog -- Generate and write CSV log entry
 *
 * Constructs the error message, depending on the Errordata it gets, in a CSV
 * format which is described in doc/src/sgml/config.sgml.
 */
void
write_csvlog(ErrorData *edata)
{
	StringInfoData buf;
	bool		print_stmt = false;

	/* static counter for line numbers */
	static long log_line_number = 0;

	/* has counter been reset in current process? */
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

	/* timestamp with milliseconds */
	appendStringInfoString(&buf, get_formatted_log_time());
	appendStringInfoChar(&buf, ',');

	/* username */
	if (MyProcPort)
		appendCSVLiteral(&buf, MyProcPort->user_name);
	appendStringInfoChar(&buf, ',');

	/* database name */
	if (MyProcPort)
		appendCSVLiteral(&buf, MyProcPort->database_name);
	appendStringInfoChar(&buf, ',');

	/* Process id  */
	if (MyProcPid != 0)
		appendStringInfo(&buf, "%d", MyProcPid);
	appendStringInfoChar(&buf, ',');

	/* Remote host and port */
	if (MyProcPort && MyProcPort->remote_host)
	{
		appendStringInfoChar(&buf, '"');
		appendStringInfoString(&buf, MyProcPort->remote_host);
		if (MyProcPort->remote_port && MyProcPort->remote_port[0] != '\0')
		{
			appendStringInfoChar(&buf, ':');
			appendStringInfoString(&buf, MyProcPort->remote_port);
		}
		appendStringInfoChar(&buf, '"');
	}
	appendStringInfoChar(&buf, ',');

	/* session id */
	appendStringInfo(&buf, INT64_HEX_FORMAT ".%x", MyStartTime, MyProcPid);
	appendStringInfoChar(&buf, ',');

	/* Line number */
	appendStringInfo(&buf, "%ld", log_line_number);
	appendStringInfoChar(&buf, ',');

	/* PS display */
	if (MyProcPort)
	{
		StringInfoData msgbuf;
		const char *psdisp;
		int			displen;

		initStringInfo(&msgbuf);

		psdisp = get_ps_display(&displen);
		appendBinaryStringInfo(&msgbuf, psdisp, displen);
		appendCSVLiteral(&buf, msgbuf.data);

		pfree(msgbuf.data);
	}
	appendStringInfoChar(&buf, ',');

	/* session start timestamp */
	appendStringInfoString(&buf, get_formatted_start_time());
	appendStringInfoChar(&buf, ',');

	/* Virtual transaction id */
	/* keep VXID format in sync with lockfuncs.c */
	if (MyProc != NULL && MyProc->vxid.procNumber != INVALID_PROC_NUMBER)
		appendStringInfo(&buf, "%d/%u", MyProc->vxid.procNumber, MyProc->vxid.lxid);
	appendStringInfoChar(&buf, ',');

	/* Transaction id */
	appendStringInfo(&buf, "%u", GetTopTransactionIdIfAny());
	appendStringInfoChar(&buf, ',');

	/* Error severity */
	appendStringInfoString(&buf, _(error_severity(edata->elevel)));
	appendStringInfoChar(&buf, ',');

	/* SQL state code */
	appendStringInfoString(&buf, unpack_sql_state(edata->sqlerrcode));
	appendStringInfoChar(&buf, ',');

	/* errmessage */
	appendCSVLiteral(&buf, edata->message);
	appendStringInfoChar(&buf, ',');

	/* errdetail or errdetail_log */
	if (edata->detail_log)
		appendCSVLiteral(&buf, edata->detail_log);
	else
		appendCSVLiteral(&buf, edata->detail);
	appendStringInfoChar(&buf, ',');

	/* errhint */
	appendCSVLiteral(&buf, edata->hint);
	appendStringInfoChar(&buf, ',');

	/* internal query */
	appendCSVLiteral(&buf, edata->internalquery);
	appendStringInfoChar(&buf, ',');

	/* if printed internal query, print internal pos too */
	if (edata->internalpos > 0 && edata->internalquery != NULL)
		appendStringInfo(&buf, "%d", edata->internalpos);
	appendStringInfoChar(&buf, ',');

	/* errcontext */
	if (!edata->hide_ctx)
		appendCSVLiteral(&buf, edata->context);
	appendStringInfoChar(&buf, ',');

	/* user query --- only reported if not disabled by the caller */
	print_stmt = check_log_of_query(edata);
	if (print_stmt)
		appendCSVLiteral(&buf, debug_query_string);
	appendStringInfoChar(&buf, ',');
	if (print_stmt && edata->cursorpos > 0)
		appendStringInfo(&buf, "%d", edata->cursorpos);
	appendStringInfoChar(&buf, ',');

	/* file error location */
	if (Log_error_verbosity >= PGERROR_VERBOSE)
	{
		StringInfoData msgbuf;

		initStringInfo(&msgbuf);

		if (edata->funcname && edata->filename)
			appendStringInfo(&msgbuf, "%s, %s:%d",
							 edata->funcname, edata->filename,
							 edata->lineno);
		else if (edata->filename)
			appendStringInfo(&msgbuf, "%s:%d",
							 edata->filename, edata->lineno);
		appendCSVLiteral(&buf, msgbuf.data);
		pfree(msgbuf.data);
	}
	appendStringInfoChar(&buf, ',');

	/* application name */
	if (application_name)
		appendCSVLiteral(&buf, application_name);

	appendStringInfoChar(&buf, ',');

	/* backend type */
	appendCSVLiteral(&buf, get_backend_type_for_log());
	appendStringInfoChar(&buf, ',');

	/* leader PID */
	if (MyProc)
	{
		PGPROC	   *leader = MyProc->lockGroupLeader;

		/*
		 * Show the leader only for active parallel workers.  This leaves out
		 * the leader of a parallel group.
		 */
		if (leader && leader->pid != MyProcPid)
			appendStringInfo(&buf, "%d", leader->pid);
	}
	appendStringInfoChar(&buf, ',');

	/* query id */
	appendStringInfo(&buf, "%lld", (long long) pgstat_get_my_query_id());

	appendStringInfoChar(&buf, '\n');

	/* If in the syslogger process, try to write messages direct to file */
	if (MyBackendType == B_LOGGER)
		write_syslogger_file(buf.data, buf.len, LOG_DESTINATION_CSVLOG);
	else
		write_pipe_chunks(buf.data, buf.len, LOG_DESTINATION_CSVLOG);

	pfree(buf.data);
}
