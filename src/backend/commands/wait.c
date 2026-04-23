/*-------------------------------------------------------------------------
 *
 * wait.c
 *	  Implements WAIT FOR, which allows waiting for events such as
 *	  time passing or LSN having been replayed, flushed, or written.
 *
 * Portions Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/wait.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "access/xlogwait.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/wait.h"
#include "executor/executor.h"
#include "parser/parse_node.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"
#include "utils/snapmgr.h"


void
ExecWaitStmt(ParseState *pstate, WaitStmt *stmt, bool isTopLevel,
			 DestReceiver *dest)
{
	XLogRecPtr	lsn;
	int64		timeout = 0;
	WaitLSNResult waitLSNResult;
	WaitLSNType lsnType = WAIT_LSN_TYPE_STANDBY_REPLAY; /* default */
	bool		throw = true;
	TupleDesc	tupdesc;
	TupOutputState *tstate;
	const char *result = "<unset>";
	bool		timeout_specified = false;
	bool		no_throw_specified = false;
	bool		mode_specified = false;

	/*
	 * WAIT FOR must not be run as a non-top-level statement (e.g., inside a
	 * function, procedure, or DO block). Forbid this case upfront.
	 */
	if (!isTopLevel)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s can only be executed as a top-level statement",
						"WAIT FOR"),
				 errdetail("WAIT FOR cannot be used within a function, procedure, or DO block.")));

	/* Parse and validate the mandatory LSN */
	lsn = DatumGetLSN(DirectFunctionCall1(pg_lsn_in,
										  CStringGetDatum(stmt->lsn_literal)));

	foreach_node(DefElem, defel, stmt->options)
	{
		if (strcmp(defel->defname, "mode") == 0)
		{
			char	   *mode_str;

			if (mode_specified)
				errorConflictingDefElem(defel, pstate);
			mode_specified = true;

			mode_str = defGetString(defel);

			if (pg_strcasecmp(mode_str, "standby_replay") == 0)
				lsnType = WAIT_LSN_TYPE_STANDBY_REPLAY;
			else if (pg_strcasecmp(mode_str, "standby_write") == 0)
				lsnType = WAIT_LSN_TYPE_STANDBY_WRITE;
			else if (pg_strcasecmp(mode_str, "standby_flush") == 0)
				lsnType = WAIT_LSN_TYPE_STANDBY_FLUSH;
			else if (pg_strcasecmp(mode_str, "primary_flush") == 0)
				lsnType = WAIT_LSN_TYPE_PRIMARY_FLUSH;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized value for %s option \"%s\": \"%s\"",
								"WAIT", defel->defname, mode_str),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "timeout") == 0)
		{
			char	   *timeout_str;
			const char *hintmsg;
			double		dval;

			if (timeout_specified)
				errorConflictingDefElem(defel, pstate);
			timeout_specified = true;

			timeout_str = defGetString(defel);

			if (!parse_real(timeout_str, &dval, GUC_UNIT_MS, &hintmsg))
			{
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid timeout value: \"%s\"", timeout_str),
						hintmsg ? errhint("%s", _(hintmsg)) : 0);
			}

			/*
			 * Get rid of any fractional part in the input. This is so we
			 * don't fail on just-out-of-range values that would round into
			 * range.
			 */
			dval = rint(dval);

			/* Range check */
			if (unlikely(isnan(dval) || !FLOAT8_FITS_IN_INT64(dval)))
				ereport(ERROR,
						errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("timeout value is out of range"));

			if (dval < 0)
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("timeout cannot be negative"));

			timeout = (int64) dval;
		}
		else if (strcmp(defel->defname, "no_throw") == 0)
		{
			if (no_throw_specified)
				errorConflictingDefElem(defel, pstate);

			no_throw_specified = true;

			throw = !defGetBoolean(defel);
		}
		else
		{
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("option \"%s\" not recognized",
						   defel->defname),
					parser_errposition(pstate, defel->location));
		}
	}

	/*
	 * We are going to wait for the LSN.  We should first care that we don't
	 * hold a snapshot and correspondingly our MyProc->xmin is invalid.
	 * Otherwise, our snapshot could prevent the replay of WAL records
	 * implying a kind of self-deadlock.  This is the reason why WAIT FOR is a
	 * command, not a procedure or function.
	 *
	 * Non-top-level contexts are rejected above, but be defensive and pop any
	 * active snapshot if one is present.  PortalRunUtility() can tolerate
	 * utility commands that remove the active snapshot.
	 */
	if (ActiveSnapshotSet())
		PopActiveSnapshot();

	/*
	 * At second, invalidate a catalog snapshot if any.  And we should be done
	 * with the preparation.
	 */
	InvalidateCatalogSnapshot();

	/* Give up if there is still an active or registered snapshot. */
	if (HaveRegisteredOrActiveSnapshot())
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("WAIT FOR must be called without an active or registered snapshot"),
				errdetail("WAIT FOR cannot be executed within a transaction with an isolation level higher than READ COMMITTED."));

	/*
	 * As the result we should hold no snapshot, and correspondingly our xmin
	 * should be unset.
	 */
	Assert(MyProc->xmin == InvalidTransactionId);

	/*
	 * Validate that the requested mode matches the current server state.
	 * Primary modes can only be used on a primary.
	 */
	if (lsnType == WAIT_LSN_TYPE_PRIMARY_FLUSH)
	{
		if (RecoveryInProgress())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("recovery is in progress"),
					 errhint("Waiting for primary_flush can only be done on a primary server. "
							 "Use standby_flush mode on a standby server.")));
	}

	/* Now wait for the LSN */
	waitLSNResult = WaitForLSN(lsnType, lsn, timeout);

	/*
	 * Process the result of WaitForLSN().  Throw appropriate error if needed.
	 */
	switch (waitLSNResult)
	{
		case WAIT_LSN_RESULT_SUCCESS:
			/* Nothing to do on success */
			result = "success";
			break;

		case WAIT_LSN_RESULT_TIMEOUT:
			if (throw)
			{
				XLogRecPtr	currentLSN = GetCurrentLSNForWaitType(lsnType);

				switch (lsnType)
				{
					case WAIT_LSN_TYPE_STANDBY_REPLAY:
						ereport(ERROR,
								errcode(ERRCODE_QUERY_CANCELED),
								errmsg("timed out while waiting for target LSN %X/%08X to be replayed; current standby_replay LSN %X/%08X",
									   LSN_FORMAT_ARGS(lsn),
									   LSN_FORMAT_ARGS(currentLSN)));
						break;

					case WAIT_LSN_TYPE_STANDBY_WRITE:
						ereport(ERROR,
								errcode(ERRCODE_QUERY_CANCELED),
								errmsg("timed out while waiting for target LSN %X/%08X to be written; current standby_write LSN %X/%08X",
									   LSN_FORMAT_ARGS(lsn),
									   LSN_FORMAT_ARGS(currentLSN)));
						break;

					case WAIT_LSN_TYPE_STANDBY_FLUSH:
						ereport(ERROR,
								errcode(ERRCODE_QUERY_CANCELED),
								errmsg("timed out while waiting for target LSN %X/%08X to be flushed; current standby_flush LSN %X/%08X",
									   LSN_FORMAT_ARGS(lsn),
									   LSN_FORMAT_ARGS(currentLSN)));
						break;

					case WAIT_LSN_TYPE_PRIMARY_FLUSH:
						ereport(ERROR,
								errcode(ERRCODE_QUERY_CANCELED),
								errmsg("timed out while waiting for target LSN %X/%08X to be flushed; current primary_flush LSN %X/%08X",
									   LSN_FORMAT_ARGS(lsn),
									   LSN_FORMAT_ARGS(currentLSN)));
						break;

					default:
						elog(ERROR, "unexpected wait LSN type %d", lsnType);
				}
			}
			else
				result = "timeout";
			break;

		case WAIT_LSN_RESULT_NOT_IN_RECOVERY:
			if (throw)
			{
				if (PromoteIsTriggered())
				{
					XLogRecPtr	currentLSN = GetCurrentLSNForWaitType(lsnType);

					switch (lsnType)
					{
						case WAIT_LSN_TYPE_STANDBY_REPLAY:
							ereport(ERROR,
									errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
									errmsg("recovery is not in progress"),
									errdetail("Recovery ended before target LSN %X/%08X was replayed; last standby_replay LSN %X/%08X.",
											  LSN_FORMAT_ARGS(lsn),
											  LSN_FORMAT_ARGS(currentLSN)));
							break;

						case WAIT_LSN_TYPE_STANDBY_WRITE:
							ereport(ERROR,
									errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
									errmsg("recovery is not in progress"),
									errdetail("Recovery ended before target LSN %X/%08X was written; last standby_write LSN %X/%08X.",
											  LSN_FORMAT_ARGS(lsn),
											  LSN_FORMAT_ARGS(currentLSN)));
							break;

						case WAIT_LSN_TYPE_STANDBY_FLUSH:
							ereport(ERROR,
									errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
									errmsg("recovery is not in progress"),
									errdetail("Recovery ended before target LSN %X/%08X was flushed; last standby_flush LSN %X/%08X.",
											  LSN_FORMAT_ARGS(lsn),
											  LSN_FORMAT_ARGS(currentLSN)));
							break;

						default:
							elog(ERROR, "unexpected wait LSN type %d", lsnType);
					}
				}
				else
				{
					switch (lsnType)
					{
						case WAIT_LSN_TYPE_STANDBY_REPLAY:
							ereport(ERROR,
									errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
									errmsg("recovery is not in progress"),
									errhint("Waiting for the standby_replay LSN can only be executed during recovery."));
							break;

						case WAIT_LSN_TYPE_STANDBY_WRITE:
							ereport(ERROR,
									errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
									errmsg("recovery is not in progress"),
									errhint("Waiting for the standby_write LSN can only be executed during recovery."));
							break;

						case WAIT_LSN_TYPE_STANDBY_FLUSH:
							ereport(ERROR,
									errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
									errmsg("recovery is not in progress"),
									errhint("Waiting for the standby_flush LSN can only be executed during recovery."));
							break;

						default:
							elog(ERROR, "unexpected wait LSN type %d", lsnType);
					}
				}
			}
			else
				result = "not in recovery";
			break;
	}

	/* need a tuple descriptor representing a single TEXT column */
	tupdesc = WaitStmtResultDesc(stmt);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* Send it */
	do_text_output_oneline(tstate, result);

	end_tup_output(tstate);
}

TupleDesc
WaitStmtResultDesc(WaitStmt *stmt)
{
	TupleDesc	tupdesc;

	/*
	 * Need a tuple descriptor representing a single TEXT column.
	 *
	 * We use TupleDescInitBuiltinEntry instead of TupleDescInitEntry to avoid
	 * syscache access. This is important because WaitStmtResultDesc may be
	 * called after snapshots have been released, and we must not re-establish
	 * a catalog snapshot which could cause recovery conflicts on a standby.
	 */
	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "status",
							  TEXTOID, -1, 0);
	TupleDescFinalize(tupdesc);
	return tupdesc;
}
