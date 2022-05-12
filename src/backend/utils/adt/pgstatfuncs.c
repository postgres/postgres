/*-------------------------------------------------------------------------
 *
 * pgstatfuncs.c
 *	  Functions for accessing various forms of statistics data
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pgstatfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xlog.h"
#include "access/xlogprefetcher.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "common/ip.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/postmaster.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inet.h"
#include "utils/timestamp.h"

#define UINT32_ACCESS_ONCE(var)		 ((uint32)(*((volatile uint32 *)&(var))))

#define HAS_PGSTAT_PERMISSIONS(role)	 (has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS) || has_privs_of_role(GetUserId(), role))

Datum
pg_stat_get_numscans(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->numscans);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_returned(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_returned);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_fetched(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_inserted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_inserted);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_updated(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_updated);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_deleted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_deleted);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_tuples_hot_updated(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->tuples_hot_updated);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_live_tuples(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->n_live_tuples);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_dead_tuples(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->n_dead_tuples);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_mod_since_analyze(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->changes_since_analyze);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_ins_since_vacuum(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->inserts_since_vacuum);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_blocks_fetched(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->blocks_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_blocks_hit(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->blocks_hit);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_last_vacuum_time(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = tabentry->vacuum_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_last_autovacuum_time(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = tabentry->autovac_vacuum_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_last_analyze_time(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = tabentry->analyze_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_last_autoanalyze_time(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = tabentry->autovac_analyze_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_vacuum_count(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->vacuum_count);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_autovacuum_count(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->autovac_vacuum_count);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_analyze_count(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->analyze_count);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_autoanalyze_count(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->autovac_analyze_count);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_function_calls(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_StatFuncEntry *funcentry;

	if ((funcentry = pgstat_fetch_stat_funcentry(funcid)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_INT64(funcentry->f_numcalls);
}

Datum
pg_stat_get_function_total_time(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_StatFuncEntry *funcentry;

	if ((funcentry = pgstat_fetch_stat_funcentry(funcid)) == NULL)
		PG_RETURN_NULL();
	/* convert counter from microsec to millisec for display */
	PG_RETURN_FLOAT8(((double) funcentry->f_total_time) / 1000.0);
}

Datum
pg_stat_get_function_self_time(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_StatFuncEntry *funcentry;

	if ((funcentry = pgstat_fetch_stat_funcentry(funcid)) == NULL)
		PG_RETURN_NULL();
	/* convert counter from microsec to millisec for display */
	PG_RETURN_FLOAT8(((double) funcentry->f_self_time) / 1000.0);
}

Datum
pg_stat_get_backend_idset(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int		   *fctx;
	int32		result;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		fctx = MemoryContextAlloc(funcctx->multi_call_memory_ctx,
								  2 * sizeof(int));
		funcctx->user_fctx = fctx;

		fctx[0] = 0;
		fctx[1] = pgstat_fetch_stat_numbackends();
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	fctx[0] += 1;
	result = fctx[0];

	if (result <= fctx[1])
	{
		/* do when there is more left to send */
		SRF_RETURN_NEXT(funcctx, Int32GetDatum(result));
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * Returns command progress information for the named command.
 */
Datum
pg_stat_get_progress_info(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_PROGRESS_COLS	PGSTAT_NUM_PROGRESS_PARAM + 3
	int			num_backends = pgstat_fetch_stat_numbackends();
	int			curr_backend;
	char	   *cmd = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ProgressCommandType cmdtype;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* Translate command name into command type code. */
	if (pg_strcasecmp(cmd, "VACUUM") == 0)
		cmdtype = PROGRESS_COMMAND_VACUUM;
	else if (pg_strcasecmp(cmd, "ANALYZE") == 0)
		cmdtype = PROGRESS_COMMAND_ANALYZE;
	else if (pg_strcasecmp(cmd, "CLUSTER") == 0)
		cmdtype = PROGRESS_COMMAND_CLUSTER;
	else if (pg_strcasecmp(cmd, "CREATE INDEX") == 0)
		cmdtype = PROGRESS_COMMAND_CREATE_INDEX;
	else if (pg_strcasecmp(cmd, "BASEBACKUP") == 0)
		cmdtype = PROGRESS_COMMAND_BASEBACKUP;
	else if (pg_strcasecmp(cmd, "COPY") == 0)
		cmdtype = PROGRESS_COMMAND_COPY;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid command name: \"%s\"", cmd)));

	SetSingleFuncCall(fcinfo, 0);

	/* 1-based index */
	for (curr_backend = 1; curr_backend <= num_backends; curr_backend++)
	{
		LocalPgBackendStatus *local_beentry;
		PgBackendStatus *beentry;
		Datum		values[PG_STAT_GET_PROGRESS_COLS];
		bool		nulls[PG_STAT_GET_PROGRESS_COLS];
		int			i;

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		local_beentry = pgstat_fetch_stat_local_beentry(curr_backend);

		if (!local_beentry)
			continue;

		beentry = &local_beentry->backendStatus;

		/*
		 * Report values for only those backends which are running the given
		 * command.
		 */
		if (!beentry || beentry->st_progress_command != cmdtype)
			continue;

		/* Value available to all callers */
		values[0] = Int32GetDatum(beentry->st_procpid);
		values[1] = ObjectIdGetDatum(beentry->st_databaseid);

		/* show rest of the values including relid only to role members */
		if (HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		{
			values[2] = ObjectIdGetDatum(beentry->st_progress_command_target);
			for (i = 0; i < PGSTAT_NUM_PROGRESS_PARAM; i++)
				values[i + 3] = Int64GetDatum(beentry->st_progress_param[i]);
		}
		else
		{
			nulls[2] = true;
			for (i = 0; i < PGSTAT_NUM_PROGRESS_PARAM; i++)
				nulls[i + 3] = true;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/*
 * Returns activity of PG backends.
 */
Datum
pg_stat_get_activity(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_ACTIVITY_COLS	30
	int			num_backends = pgstat_fetch_stat_numbackends();
	int			curr_backend;
	int			pid = PG_ARGISNULL(0) ? -1 : PG_GETARG_INT32(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	SetSingleFuncCall(fcinfo, 0);

	/* 1-based index */
	for (curr_backend = 1; curr_backend <= num_backends; curr_backend++)
	{
		/* for each row */
		Datum		values[PG_STAT_GET_ACTIVITY_COLS];
		bool		nulls[PG_STAT_GET_ACTIVITY_COLS];
		LocalPgBackendStatus *local_beentry;
		PgBackendStatus *beentry;
		PGPROC	   *proc;
		const char *wait_event_type = NULL;
		const char *wait_event = NULL;

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		/* Get the next one in the list */
		local_beentry = pgstat_fetch_stat_local_beentry(curr_backend);
		if (!local_beentry)
		{
			int			i;

			/* Ignore missing entries if looking for specific PID */
			if (pid != -1)
				continue;

			for (i = 0; i < lengthof(nulls); i++)
				nulls[i] = true;

			nulls[5] = false;
			values[5] = CStringGetTextDatum("<backend information not available>");

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
			continue;
		}

		beentry = &local_beentry->backendStatus;

		/* If looking for specific PID, ignore all the others */
		if (pid != -1 && beentry->st_procpid != pid)
			continue;

		/* Values available to all callers */
		if (beentry->st_databaseid != InvalidOid)
			values[0] = ObjectIdGetDatum(beentry->st_databaseid);
		else
			nulls[0] = true;

		values[1] = Int32GetDatum(beentry->st_procpid);

		if (beentry->st_userid != InvalidOid)
			values[2] = ObjectIdGetDatum(beentry->st_userid);
		else
			nulls[2] = true;

		if (beentry->st_appname)
			values[3] = CStringGetTextDatum(beentry->st_appname);
		else
			nulls[3] = true;

		if (TransactionIdIsValid(local_beentry->backend_xid))
			values[15] = TransactionIdGetDatum(local_beentry->backend_xid);
		else
			nulls[15] = true;

		if (TransactionIdIsValid(local_beentry->backend_xmin))
			values[16] = TransactionIdGetDatum(local_beentry->backend_xmin);
		else
			nulls[16] = true;

		/* Values only available to role member or pg_read_all_stats */
		if (HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		{
			SockAddr	zero_clientaddr;
			char	   *clipped_activity;

			switch (beentry->st_state)
			{
				case STATE_IDLE:
					values[4] = CStringGetTextDatum("idle");
					break;
				case STATE_RUNNING:
					values[4] = CStringGetTextDatum("active");
					break;
				case STATE_IDLEINTRANSACTION:
					values[4] = CStringGetTextDatum("idle in transaction");
					break;
				case STATE_FASTPATH:
					values[4] = CStringGetTextDatum("fastpath function call");
					break;
				case STATE_IDLEINTRANSACTION_ABORTED:
					values[4] = CStringGetTextDatum("idle in transaction (aborted)");
					break;
				case STATE_DISABLED:
					values[4] = CStringGetTextDatum("disabled");
					break;
				case STATE_UNDEFINED:
					nulls[4] = true;
					break;
			}

			clipped_activity = pgstat_clip_activity(beentry->st_activity_raw);
			values[5] = CStringGetTextDatum(clipped_activity);
			pfree(clipped_activity);

			/* leader_pid */
			nulls[28] = true;

			proc = BackendPidGetProc(beentry->st_procpid);

			if (proc == NULL && (beentry->st_backendType != B_BACKEND))
			{
				/*
				 * For an auxiliary process, retrieve process info from
				 * AuxiliaryProcs stored in shared-memory.
				 */
				proc = AuxiliaryPidGetProc(beentry->st_procpid);
			}

			/*
			 * If a PGPROC entry was retrieved, display wait events and lock
			 * group leader information if any.  To avoid extra overhead, no
			 * extra lock is being held, so there is no guarantee of
			 * consistency across multiple rows.
			 */
			if (proc != NULL)
			{
				uint32		raw_wait_event;
				PGPROC	   *leader;

				raw_wait_event = UINT32_ACCESS_ONCE(proc->wait_event_info);
				wait_event_type = pgstat_get_wait_event_type(raw_wait_event);
				wait_event = pgstat_get_wait_event(raw_wait_event);

				leader = proc->lockGroupLeader;

				/*
				 * Show the leader only for active parallel workers.  This
				 * leaves the field as NULL for the leader of a parallel
				 * group.
				 */
				if (leader && leader->pid != beentry->st_procpid)
				{
					values[28] = Int32GetDatum(leader->pid);
					nulls[28] = false;
				}
			}

			if (wait_event_type)
				values[6] = CStringGetTextDatum(wait_event_type);
			else
				nulls[6] = true;

			if (wait_event)
				values[7] = CStringGetTextDatum(wait_event);
			else
				nulls[7] = true;

			/*
			 * Don't expose transaction time for walsenders; it confuses
			 * monitoring, particularly because we don't keep the time up-to-
			 * date.
			 */
			if (beentry->st_xact_start_timestamp != 0 &&
				beentry->st_backendType != B_WAL_SENDER)
				values[8] = TimestampTzGetDatum(beentry->st_xact_start_timestamp);
			else
				nulls[8] = true;

			if (beentry->st_activity_start_timestamp != 0)
				values[9] = TimestampTzGetDatum(beentry->st_activity_start_timestamp);
			else
				nulls[9] = true;

			if (beentry->st_proc_start_timestamp != 0)
				values[10] = TimestampTzGetDatum(beentry->st_proc_start_timestamp);
			else
				nulls[10] = true;

			if (beentry->st_state_start_timestamp != 0)
				values[11] = TimestampTzGetDatum(beentry->st_state_start_timestamp);
			else
				nulls[11] = true;

			/* A zeroed client addr means we don't know */
			memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
			if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
					   sizeof(zero_clientaddr)) == 0)
			{
				nulls[12] = true;
				nulls[13] = true;
				nulls[14] = true;
			}
			else
			{
				if (beentry->st_clientaddr.addr.ss_family == AF_INET
#ifdef HAVE_IPV6
					|| beentry->st_clientaddr.addr.ss_family == AF_INET6
#endif
					)
				{
					char		remote_host[NI_MAXHOST];
					char		remote_port[NI_MAXSERV];
					int			ret;

					remote_host[0] = '\0';
					remote_port[0] = '\0';
					ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
											 beentry->st_clientaddr.salen,
											 remote_host, sizeof(remote_host),
											 remote_port, sizeof(remote_port),
											 NI_NUMERICHOST | NI_NUMERICSERV);
					if (ret == 0)
					{
						clean_ipv6_addr(beentry->st_clientaddr.addr.ss_family, remote_host);
						values[12] = DirectFunctionCall1(inet_in,
														 CStringGetDatum(remote_host));
						if (beentry->st_clienthostname &&
							beentry->st_clienthostname[0])
							values[13] = CStringGetTextDatum(beentry->st_clienthostname);
						else
							nulls[13] = true;
						values[14] = Int32GetDatum(atoi(remote_port));
					}
					else
					{
						nulls[12] = true;
						nulls[13] = true;
						nulls[14] = true;
					}
				}
				else if (beentry->st_clientaddr.addr.ss_family == AF_UNIX)
				{
					/*
					 * Unix sockets always reports NULL for host and -1 for
					 * port, so it's possible to tell the difference to
					 * connections we have no permissions to view, or with
					 * errors.
					 */
					nulls[12] = true;
					nulls[13] = true;
					values[14] = Int32GetDatum(-1);
				}
				else
				{
					/* Unknown address type, should never happen */
					nulls[12] = true;
					nulls[13] = true;
					nulls[14] = true;
				}
			}
			/* Add backend type */
			if (beentry->st_backendType == B_BG_WORKER)
			{
				const char *bgw_type;

				bgw_type = GetBackgroundWorkerTypeByPid(beentry->st_procpid);
				if (bgw_type)
					values[17] = CStringGetTextDatum(bgw_type);
				else
					nulls[17] = true;
			}
			else
				values[17] =
					CStringGetTextDatum(GetBackendTypeDesc(beentry->st_backendType));

			/* SSL information */
			if (beentry->st_ssl)
			{
				values[18] = BoolGetDatum(true);	/* ssl */
				values[19] = CStringGetTextDatum(beentry->st_sslstatus->ssl_version);
				values[20] = CStringGetTextDatum(beentry->st_sslstatus->ssl_cipher);
				values[21] = Int32GetDatum(beentry->st_sslstatus->ssl_bits);

				if (beentry->st_sslstatus->ssl_client_dn[0])
					values[22] = CStringGetTextDatum(beentry->st_sslstatus->ssl_client_dn);
				else
					nulls[22] = true;

				if (beentry->st_sslstatus->ssl_client_serial[0])
					values[23] = DirectFunctionCall3(numeric_in,
													 CStringGetDatum(beentry->st_sslstatus->ssl_client_serial),
													 ObjectIdGetDatum(InvalidOid),
													 Int32GetDatum(-1));
				else
					nulls[23] = true;

				if (beentry->st_sslstatus->ssl_issuer_dn[0])
					values[24] = CStringGetTextDatum(beentry->st_sslstatus->ssl_issuer_dn);
				else
					nulls[24] = true;
			}
			else
			{
				values[18] = BoolGetDatum(false);	/* ssl */
				nulls[19] = nulls[20] = nulls[21] = nulls[22] = nulls[23] = nulls[24] = true;
			}

			/* GSSAPI information */
			if (beentry->st_gss)
			{
				values[25] = BoolGetDatum(beentry->st_gssstatus->gss_auth); /* gss_auth */
				values[26] = CStringGetTextDatum(beentry->st_gssstatus->gss_princ);
				values[27] = BoolGetDatum(beentry->st_gssstatus->gss_enc);	/* GSS Encryption in use */
			}
			else
			{
				values[25] = BoolGetDatum(false);	/* gss_auth */
				nulls[26] = true;	/* No GSS principal */
				values[27] = BoolGetDatum(false);	/* GSS Encryption not in
													 * use */
			}
			if (beentry->st_query_id == 0)
				nulls[29] = true;
			else
				values[29] = UInt64GetDatum(beentry->st_query_id);
		}
		else
		{
			/* No permissions to view data about this session */
			values[5] = CStringGetTextDatum("<insufficient privilege>");
			nulls[4] = true;
			nulls[6] = true;
			nulls[7] = true;
			nulls[8] = true;
			nulls[9] = true;
			nulls[10] = true;
			nulls[11] = true;
			nulls[12] = true;
			nulls[13] = true;
			nulls[14] = true;
			nulls[17] = true;
			nulls[18] = true;
			nulls[19] = true;
			nulls[20] = true;
			nulls[21] = true;
			nulls[22] = true;
			nulls[23] = true;
			nulls[24] = true;
			nulls[25] = true;
			nulls[26] = true;
			nulls[27] = true;
			nulls[28] = true;
			nulls[29] = true;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

		/* If only a single backend was requested, and we found it, break. */
		if (pid != -1)
			break;
	}

	return (Datum) 0;
}


Datum
pg_backend_pid(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MyProcPid);
}


Datum
pg_stat_get_backend_pid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_INT32(beentry->st_procpid);
}


Datum
pg_stat_get_backend_dbid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(beentry->st_databaseid);
}


Datum
pg_stat_get_backend_userid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(beentry->st_userid);
}


Datum
pg_stat_get_backend_activity(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	const char *activity;
	char	   *clipped_activity;
	text	   *ret;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		activity = "<backend information not available>";
	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		activity = "<insufficient privilege>";
	else if (*(beentry->st_activity_raw) == '\0')
		activity = "<command string not enabled>";
	else
		activity = beentry->st_activity_raw;

	clipped_activity = pgstat_clip_activity(activity);
	ret = cstring_to_text(activity);
	pfree(clipped_activity);

	PG_RETURN_TEXT_P(ret);
}

Datum
pg_stat_get_backend_wait_event_type(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	PGPROC	   *proc;
	const char *wait_event_type = NULL;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		wait_event_type = "<backend information not available>";
	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		wait_event_type = "<insufficient privilege>";
	else if ((proc = BackendPidGetProc(beentry->st_procpid)) != NULL)
		wait_event_type = pgstat_get_wait_event_type(proc->wait_event_info);

	if (!wait_event_type)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(wait_event_type));
}

Datum
pg_stat_get_backend_wait_event(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	PGPROC	   *proc;
	const char *wait_event = NULL;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		wait_event = "<backend information not available>";
	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		wait_event = "<insufficient privilege>";
	else if ((proc = BackendPidGetProc(beentry->st_procpid)) != NULL)
		wait_event = pgstat_get_wait_event(proc->wait_event_info);

	if (!wait_event)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(wait_event));
}


Datum
pg_stat_get_backend_activity_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	result = beentry->st_activity_start_timestamp;

	/*
	 * No time recorded for start of current query -- this is the case if the
	 * user hasn't enabled query-level stats collection.
	 */
	if (result == 0)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_xact_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	result = beentry->st_xact_start_timestamp;

	if (result == 0)			/* not in a transaction */
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	result = beentry->st_proc_start_timestamp;

	if (result == 0)			/* probably can't happen? */
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_client_addr(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	SockAddr	zero_clientaddr;
	char		remote_host[NI_MAXHOST];
	int			ret;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	/* A zeroed client addr means we don't know */
	memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
	if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
			   sizeof(zero_clientaddr)) == 0)
		PG_RETURN_NULL();

	switch (beentry->st_clientaddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_host[0] = '\0';
	ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
							 beentry->st_clientaddr.salen,
							 remote_host, sizeof(remote_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(beentry->st_clientaddr.addr.ss_family, remote_host);

	PG_RETURN_INET_P(DirectFunctionCall1(inet_in,
										 CStringGetDatum(remote_host)));
}

Datum
pg_stat_get_backend_client_port(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	SockAddr	zero_clientaddr;
	char		remote_port[NI_MAXSERV];
	int			ret;

	if ((beentry = pgstat_fetch_stat_beentry(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	/* A zeroed client addr means we don't know */
	memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
	if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
			   sizeof(zero_clientaddr)) == 0)
		PG_RETURN_NULL();

	switch (beentry->st_clientaddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		case AF_UNIX:
			PG_RETURN_INT32(-1);
		default:
			PG_RETURN_NULL();
	}

	remote_port[0] = '\0';
	ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
							 beentry->st_clientaddr.salen,
							 NULL, 0,
							 remote_port, sizeof(remote_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in,
										CStringGetDatum(remote_port)));
}


Datum
pg_stat_get_db_numbackends(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int32		result;
	int			tot_backends = pgstat_fetch_stat_numbackends();
	int			beid;

	result = 0;
	for (beid = 1; beid <= tot_backends; beid++)
	{
		PgBackendStatus *beentry = pgstat_fetch_stat_beentry(beid);

		if (beentry && beentry->st_databaseid == dbid)
			result++;
	}

	PG_RETURN_INT32(result);
}


Datum
pg_stat_get_db_xact_commit(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_xact_commit);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_xact_rollback(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_xact_rollback);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_blocks_fetched(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_blocks_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_blocks_hit(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_blocks_hit);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_tuples_returned(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_tuples_returned);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_tuples_fetched(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_tuples_fetched);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_tuples_inserted(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_tuples_inserted);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_tuples_updated(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_tuples_updated);

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_tuples_deleted(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_tuples_deleted);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_stat_reset_time(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = dbentry->stat_reset_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_db_temp_files(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = dbentry->n_temp_files;

	PG_RETURN_INT64(result);
}


Datum
pg_stat_get_db_temp_bytes(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = dbentry->n_temp_bytes;

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_tablespace(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_tablespace);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_lock(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_lock);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_snapshot(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_snapshot);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_bufferpin(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_bufferpin);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_startup_deadlock(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_startup_deadlock);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_conflict_all(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_conflict_tablespace +
						  dbentry->n_conflict_lock +
						  dbentry->n_conflict_snapshot +
						  dbentry->n_conflict_bufferpin +
						  dbentry->n_conflict_startup_deadlock);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_deadlocks(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_deadlocks);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_checksum_failures(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if (!DataChecksumsEnabled())
		PG_RETURN_NULL();

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->n_checksum_failures);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_checksum_last_failure(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatDBEntry *dbentry;

	if (!DataChecksumsEnabled())
		PG_RETURN_NULL();

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = dbentry->last_checksum_failure;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

Datum
pg_stat_get_db_blk_read_time(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	double		result;
	PgStat_StatDBEntry *dbentry;

	/* convert counter from microsec to millisec for display */
	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = ((double) dbentry->n_block_read_time) / 1000.0;

	PG_RETURN_FLOAT8(result);
}

Datum
pg_stat_get_db_blk_write_time(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	double		result;
	PgStat_StatDBEntry *dbentry;

	/* convert counter from microsec to millisec for display */
	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = ((double) dbentry->n_block_write_time) / 1000.0;

	PG_RETURN_FLOAT8(result);
}

Datum
pg_stat_get_db_session_time(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	double		result = 0.0;
	PgStat_StatDBEntry *dbentry;

	/* convert counter from microsec to millisec for display */
	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) != NULL)
		result = ((double) dbentry->total_session_time) / 1000.0;

	PG_RETURN_FLOAT8(result);
}

Datum
pg_stat_get_db_active_time(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	double		result = 0.0;
	PgStat_StatDBEntry *dbentry;

	/* convert counter from microsec to millisec for display */
	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) != NULL)
		result = ((double) dbentry->total_active_time) / 1000.0;

	PG_RETURN_FLOAT8(result);
}

Datum
pg_stat_get_db_idle_in_transaction_time(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	double		result = 0.0;
	PgStat_StatDBEntry *dbentry;

	/* convert counter from microsec to millisec for display */
	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) != NULL)
		result = ((double) dbentry->total_idle_in_xact_time) / 1000.0;

	PG_RETURN_FLOAT8(result);
}

Datum
pg_stat_get_db_sessions(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result = 0;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) != NULL)
		result = (int64) (dbentry->n_sessions);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_sessions_abandoned(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result = 0;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) != NULL)
		result = (int64) (dbentry->n_sessions_abandoned);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_sessions_fatal(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result = 0;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) != NULL)
		result = (int64) (dbentry->n_sessions_fatal);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_sessions_killed(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result = 0;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) != NULL)
		result = (int64) (dbentry->n_sessions_killed);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_bgwriter_timed_checkpoints(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_checkpointer()->timed_checkpoints);
}

Datum
pg_stat_get_bgwriter_requested_checkpoints(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_checkpointer()->requested_checkpoints);
}

Datum
pg_stat_get_bgwriter_buf_written_checkpoints(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_checkpointer()->buf_written_checkpoints);
}

Datum
pg_stat_get_bgwriter_buf_written_clean(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_bgwriter()->buf_written_clean);
}

Datum
pg_stat_get_bgwriter_maxwritten_clean(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_bgwriter()->maxwritten_clean);
}

Datum
pg_stat_get_checkpoint_write_time(PG_FUNCTION_ARGS)
{
	/* time is already in msec, just convert to double for presentation */
	PG_RETURN_FLOAT8((double)
					 pgstat_fetch_stat_checkpointer()->checkpoint_write_time);
}

Datum
pg_stat_get_checkpoint_sync_time(PG_FUNCTION_ARGS)
{
	/* time is already in msec, just convert to double for presentation */
	PG_RETURN_FLOAT8((double)
					 pgstat_fetch_stat_checkpointer()->checkpoint_sync_time);
}

Datum
pg_stat_get_bgwriter_stat_reset_time(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(pgstat_fetch_stat_bgwriter()->stat_reset_timestamp);
}

Datum
pg_stat_get_buf_written_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_checkpointer()->buf_written_backend);
}

Datum
pg_stat_get_buf_fsync_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_checkpointer()->buf_fsync_backend);
}

Datum
pg_stat_get_buf_alloc(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_bgwriter()->buf_alloc);
}

/*
 * Returns statistics of WAL activity
 */
Datum
pg_stat_get_wal(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_WAL_COLS	9
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_GET_WAL_COLS];
	bool		nulls[PG_STAT_GET_WAL_COLS];
	char		buf[256];
	PgStat_WalStats *wal_stats;

	/* Initialise values and NULL flags arrays */
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* Initialise attributes information in the tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(PG_STAT_GET_WAL_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "wal_records",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "wal_fpi",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "wal_bytes",
					   NUMERICOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "wal_buffers_full",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "wal_write",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "wal_sync",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "wal_write_time",
					   FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "wal_sync_time",
					   FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 9, "stats_reset",
					   TIMESTAMPTZOID, -1, 0);

	BlessTupleDesc(tupdesc);

	/* Get statistics about WAL activity */
	wal_stats = pgstat_fetch_stat_wal();

	/* Fill values and NULLs */
	values[0] = Int64GetDatum(wal_stats->wal_records);
	values[1] = Int64GetDatum(wal_stats->wal_fpi);

	/* Convert to numeric. */
	snprintf(buf, sizeof buf, UINT64_FORMAT, wal_stats->wal_bytes);
	values[2] = DirectFunctionCall3(numeric_in,
									CStringGetDatum(buf),
									ObjectIdGetDatum(0),
									Int32GetDatum(-1));

	values[3] = Int64GetDatum(wal_stats->wal_buffers_full);
	values[4] = Int64GetDatum(wal_stats->wal_write);
	values[5] = Int64GetDatum(wal_stats->wal_sync);

	/* Convert counters from microsec to millisec for display */
	values[6] = Float8GetDatum(((double) wal_stats->wal_write_time) / 1000.0);
	values[7] = Float8GetDatum(((double) wal_stats->wal_sync_time) / 1000.0);

	values[8] = TimestampTzGetDatum(wal_stats->stat_reset_timestamp);

	/* Returns the record as Datum */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Returns statistics of SLRU caches.
 */
Datum
pg_stat_get_slru(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_SLRU_COLS	9
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			i;
	PgStat_SLRUStats *stats;

	SetSingleFuncCall(fcinfo, 0);

	/* request SLRU stats from the cumulative stats system */
	stats = pgstat_fetch_slru();

	for (i = 0;; i++)
	{
		/* for each row */
		Datum		values[PG_STAT_GET_SLRU_COLS];
		bool		nulls[PG_STAT_GET_SLRU_COLS];
		PgStat_SLRUStats stat;
		const char *name;

		name = pgstat_get_slru_name(i);

		if (!name)
			break;

		stat = stats[i];
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		values[0] = PointerGetDatum(cstring_to_text(name));
		values[1] = Int64GetDatum(stat.blocks_zeroed);
		values[2] = Int64GetDatum(stat.blocks_hit);
		values[3] = Int64GetDatum(stat.blocks_read);
		values[4] = Int64GetDatum(stat.blocks_written);
		values[5] = Int64GetDatum(stat.blocks_exists);
		values[6] = Int64GetDatum(stat.flush);
		values[7] = Int64GetDatum(stat.truncate);
		values[8] = TimestampTzGetDatum(stat.stat_reset_timestamp);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

Datum
pg_stat_get_xact_numscans(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_numscans);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_returned(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_tuples_returned);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_fetched(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_tuples_fetched);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_inserted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;
	PgStat_TableXactStatus *trans;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
	{
		result = tabentry->t_counts.t_tuples_inserted;
		/* live subtransactions' counts aren't in t_tuples_inserted yet */
		for (trans = tabentry->trans; trans != NULL; trans = trans->upper)
			result += trans->tuples_inserted;
	}

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_updated(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;
	PgStat_TableXactStatus *trans;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
	{
		result = tabentry->t_counts.t_tuples_updated;
		/* live subtransactions' counts aren't in t_tuples_updated yet */
		for (trans = tabentry->trans; trans != NULL; trans = trans->upper)
			result += trans->tuples_updated;
	}

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_deleted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;
	PgStat_TableXactStatus *trans;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
	{
		result = tabentry->t_counts.t_tuples_deleted;
		/* live subtransactions' counts aren't in t_tuples_deleted yet */
		for (trans = tabentry->trans; trans != NULL; trans = trans->upper)
			result += trans->tuples_deleted;
	}

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_hot_updated(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_tuples_hot_updated);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_blocks_fetched(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_blocks_fetched);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_blocks_hit(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->t_counts.t_blocks_hit);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_function_calls(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_BackendFunctionEntry *funcentry;

	if ((funcentry = find_funcstat_entry(funcid)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_INT64(funcentry->f_counts.f_numcalls);
}

Datum
pg_stat_get_xact_function_total_time(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_BackendFunctionEntry *funcentry;

	if ((funcentry = find_funcstat_entry(funcid)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8(INSTR_TIME_GET_MILLISEC(funcentry->f_counts.f_total_time));
}

Datum
pg_stat_get_xact_function_self_time(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_BackendFunctionEntry *funcentry;

	if ((funcentry = find_funcstat_entry(funcid)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_FLOAT8(INSTR_TIME_GET_MILLISEC(funcentry->f_counts.f_self_time));
}


/* Get the timestamp of the current statistics snapshot */
Datum
pg_stat_get_snapshot_timestamp(PG_FUNCTION_ARGS)
{
	bool		have_snapshot;
	TimestampTz ts;

	ts = pgstat_get_stat_snapshot_timestamp(&have_snapshot);

	if (!have_snapshot)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(ts);
}

/* Discard the active statistics snapshot */
Datum
pg_stat_clear_snapshot(PG_FUNCTION_ARGS)
{
	pgstat_clear_snapshot();

	PG_RETURN_VOID();
}


/* Force statistics to be reported at the next occasion */
Datum
pg_stat_force_next_flush(PG_FUNCTION_ARGS)
{
	pgstat_force_next_flush();

	PG_RETURN_VOID();
}


/* Reset all counters for the current database */
Datum
pg_stat_reset(PG_FUNCTION_ARGS)
{
	pgstat_reset_counters();

	PG_RETURN_VOID();
}

/* Reset some shared cluster-wide counters */
Datum
pg_stat_reset_shared(PG_FUNCTION_ARGS)
{
	char	   *target = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (strcmp(target, "archiver") == 0)
		pgstat_reset_of_kind(PGSTAT_KIND_ARCHIVER);
	else if (strcmp(target, "bgwriter") == 0)
	{
		/*
		 * Historically checkpointer was part of bgwriter, continue to reset
		 * both for now.
		 */
		pgstat_reset_of_kind(PGSTAT_KIND_BGWRITER);
		pgstat_reset_of_kind(PGSTAT_KIND_CHECKPOINTER);
	}
	else if (strcmp(target, "recovery_prefetch") == 0)
		XLogPrefetchResetStats();
	else if (strcmp(target, "wal") == 0)
		pgstat_reset_of_kind(PGSTAT_KIND_WAL);
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized reset target: \"%s\"", target),
				 errhint("Target must be \"archiver\", \"bgwriter\", \"recovery_prefetch\", or \"wal\".")));

	PG_RETURN_VOID();
}

/* Reset a single counter in the current database */
Datum
pg_stat_reset_single_table_counters(PG_FUNCTION_ARGS)
{
	Oid			taboid = PG_GETARG_OID(0);

	pgstat_reset(PGSTAT_KIND_RELATION, MyDatabaseId, taboid);

	PG_RETURN_VOID();
}

Datum
pg_stat_reset_single_function_counters(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);

	pgstat_reset(PGSTAT_KIND_FUNCTION, MyDatabaseId, funcoid);

	PG_RETURN_VOID();
}

/* Reset SLRU counters (a specific one or all of them). */
Datum
pg_stat_reset_slru(PG_FUNCTION_ARGS)
{
	char	   *target = NULL;

	if (PG_ARGISNULL(0))
		pgstat_reset_of_kind(PGSTAT_KIND_SLRU);
	else
	{
		target = text_to_cstring(PG_GETARG_TEXT_PP(0));
		pgstat_reset_slru(target);
	}

	PG_RETURN_VOID();
}

/* Reset replication slots stats (a specific one or all of them). */
Datum
pg_stat_reset_replication_slot(PG_FUNCTION_ARGS)
{
	char	   *target = NULL;

	if (PG_ARGISNULL(0))
		pgstat_reset_of_kind(PGSTAT_KIND_REPLSLOT);
	else
	{
		target = text_to_cstring(PG_GETARG_TEXT_PP(0));
		pgstat_reset_replslot(target);
	}

	PG_RETURN_VOID();
}

/* Reset subscription stats (a specific one or all of them) */
Datum
pg_stat_reset_subscription_stats(PG_FUNCTION_ARGS)
{
	Oid			subid;

	if (PG_ARGISNULL(0))
	{
		/* Clear all subscription stats */
		pgstat_reset_of_kind(PGSTAT_KIND_SUBSCRIPTION);
	}
	else
	{
		subid = PG_GETARG_OID(0);

		if (!OidIsValid(subid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid subscription OID %u", subid)));
		pgstat_reset(PGSTAT_KIND_SUBSCRIPTION, InvalidOid, subid);
	}

	PG_RETURN_VOID();
}

Datum
pg_stat_get_archiver(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[7];
	bool		nulls[7];
	PgStat_ArchiverStats *archiver_stats;

	/* Initialise values and NULL flags arrays */
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* Initialise attributes information in the tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(7);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "archived_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "last_archived_wal",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "last_archived_time",
					   TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "failed_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "last_failed_wal",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "last_failed_time",
					   TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "stats_reset",
					   TIMESTAMPTZOID, -1, 0);

	BlessTupleDesc(tupdesc);

	/* Get statistics about the archiver process */
	archiver_stats = pgstat_fetch_stat_archiver();

	/* Fill values and NULLs */
	values[0] = Int64GetDatum(archiver_stats->archived_count);
	if (*(archiver_stats->last_archived_wal) == '\0')
		nulls[1] = true;
	else
		values[1] = CStringGetTextDatum(archiver_stats->last_archived_wal);

	if (archiver_stats->last_archived_timestamp == 0)
		nulls[2] = true;
	else
		values[2] = TimestampTzGetDatum(archiver_stats->last_archived_timestamp);

	values[3] = Int64GetDatum(archiver_stats->failed_count);
	if (*(archiver_stats->last_failed_wal) == '\0')
		nulls[4] = true;
	else
		values[4] = CStringGetTextDatum(archiver_stats->last_failed_wal);

	if (archiver_stats->last_failed_timestamp == 0)
		nulls[5] = true;
	else
		values[5] = TimestampTzGetDatum(archiver_stats->last_failed_timestamp);

	if (archiver_stats->stat_reset_timestamp == 0)
		nulls[6] = true;
	else
		values[6] = TimestampTzGetDatum(archiver_stats->stat_reset_timestamp);

	/* Returns the record as Datum */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Get the statistics for the replication slot. If the slot statistics is not
 * available, return all-zeroes stats.
 */
Datum
pg_stat_get_replication_slot(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_REPLICATION_SLOT_COLS 10
	text	   *slotname_text = PG_GETARG_TEXT_P(0);
	NameData	slotname;
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_GET_REPLICATION_SLOT_COLS];
	bool		nulls[PG_STAT_GET_REPLICATION_SLOT_COLS];
	PgStat_StatReplSlotEntry *slotent;
	PgStat_StatReplSlotEntry allzero;

	/* Initialise values and NULL flags arrays */
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* Initialise attributes information in the tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(PG_STAT_GET_REPLICATION_SLOT_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "slot_name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "spill_txns",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "spill_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "spill_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "stream_txns",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "stream_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "stream_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "total_txns",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 9, "total_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 10, "stats_reset",
					   TIMESTAMPTZOID, -1, 0);
	BlessTupleDesc(tupdesc);

	namestrcpy(&slotname, text_to_cstring(slotname_text));
	slotent = pgstat_fetch_replslot(slotname);
	if (!slotent)
	{
		/*
		 * If the slot is not found, initialise its stats. This is possible if
		 * the create slot message is lost.
		 */
		memset(&allzero, 0, sizeof(PgStat_StatReplSlotEntry));
		slotent = &allzero;
	}

	values[0] = CStringGetTextDatum(NameStr(slotname));
	values[1] = Int64GetDatum(slotent->spill_txns);
	values[2] = Int64GetDatum(slotent->spill_count);
	values[3] = Int64GetDatum(slotent->spill_bytes);
	values[4] = Int64GetDatum(slotent->stream_txns);
	values[5] = Int64GetDatum(slotent->stream_count);
	values[6] = Int64GetDatum(slotent->stream_bytes);
	values[7] = Int64GetDatum(slotent->total_txns);
	values[8] = Int64GetDatum(slotent->total_bytes);

	if (slotent->stat_reset_timestamp == 0)
		nulls[9] = true;
	else
		values[9] = TimestampTzGetDatum(slotent->stat_reset_timestamp);

	/* Returns the record as Datum */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Get the subscription statistics for the given subscription. If the
 * subscription statistics is not available, return all-zeros stats.
 */
Datum
pg_stat_get_subscription_stats(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_SUBSCRIPTION_STATS_COLS	4
	Oid			subid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_GET_SUBSCRIPTION_STATS_COLS];
	bool		nulls[PG_STAT_GET_SUBSCRIPTION_STATS_COLS];
	PgStat_StatSubEntry *subentry;
	PgStat_StatSubEntry allzero;

	/* Get subscription stats */
	subentry = pgstat_fetch_stat_subscription(subid);

	/* Initialise attributes information in the tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(PG_STAT_GET_SUBSCRIPTION_STATS_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "subid",
					   OIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "apply_error_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "sync_error_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "stats_reset",
					   TIMESTAMPTZOID, -1, 0);
	BlessTupleDesc(tupdesc);

	/* Initialise values and NULL flags arrays */
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	if (!subentry)
	{
		/* If the subscription is not found, initialise its stats */
		memset(&allzero, 0, sizeof(PgStat_StatSubEntry));
		subentry = &allzero;
	}

	/* subid */
	values[0] = ObjectIdGetDatum(subid);

	/* apply_error_count */
	values[1] = Int64GetDatum(subentry->apply_error_count);

	/* sync_error_count */
	values[2] = Int64GetDatum(subentry->sync_error_count);

	/* stats_reset */
	if (subentry->stat_reset_timestamp == 0)
		nulls[3] = true;
	else
		values[3] = TimestampTzGetDatum(subentry->stat_reset_timestamp);

	/* Returns the record as Datum */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Checks for presence of stats for object with provided kind, database oid,
 * object oid.
 *
 * This is useful for tests, but not really anything else. Therefore not
 * documented.
 */
Datum
pg_stat_have_stats(PG_FUNCTION_ARGS)
{
	char	   *stats_type = text_to_cstring(PG_GETARG_TEXT_P(0));
	Oid			dboid = PG_GETARG_OID(1);
	Oid			objoid = PG_GETARG_OID(2);
	PgStat_Kind kind = pgstat_get_kind_from_str(stats_type);

	PG_RETURN_BOOL(pgstat_have_entry(kind, dboid, objoid));
}
