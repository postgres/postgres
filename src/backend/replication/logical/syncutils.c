/*-------------------------------------------------------------------------
 * syncutils.c
 *	  PostgreSQL logical replication: common synchronization code
 *
 * Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/syncutils.c
 *
 * NOTES
 *	  This file contains code common for synchronization workers.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_subscription_rel.h"
#include "pgstat.h"
#include "replication/logicallauncher.h"
#include "replication/worker_internal.h"
#include "storage/ipc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

/*
 * Enum for phases of the subscription relations state.
 *
 * SYNC_RELATIONS_STATE_NEEDS_REBUILD indicates that the subscription relations
 * state is no longer valid, and the subscription relations should be rebuilt.
 *
 * SYNC_RELATIONS_STATE_REBUILD_STARTED indicates that the subscription
 * relations state is being rebuilt.
 *
 * SYNC_RELATIONS_STATE_VALID indicates that the subscription relation state is
 * up-to-date and valid.
 */
typedef enum
{
	SYNC_RELATIONS_STATE_NEEDS_REBUILD,
	SYNC_RELATIONS_STATE_REBUILD_STARTED,
	SYNC_RELATIONS_STATE_VALID,
} SyncingRelationsState;

static SyncingRelationsState relation_states_validity = SYNC_RELATIONS_STATE_NEEDS_REBUILD;

/*
 * Exit routine for synchronization worker.
 */
pg_noreturn void
FinishSyncWorker(void)
{
	Assert(am_sequencesync_worker() || am_tablesync_worker());

	/*
	 * Commit any outstanding transaction. This is the usual case, unless
	 * there was nothing to do for the table.
	 */
	if (IsTransactionState())
	{
		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	/* And flush all writes. */
	XLogFlush(GetXLogWriteRecPtr());

	if (am_sequencesync_worker())
	{
		ereport(LOG,
				errmsg("logical replication sequence synchronization worker for subscription \"%s\" has finished",
					   MySubscription->name));

		/*
		 * Reset last_seqsync_start_time, so that next time a sequencesync
		 * worker is needed it can be started promptly.
		 */
		logicalrep_reset_seqsync_start_time();
	}
	else
	{
		StartTransactionCommand();
		ereport(LOG,
				errmsg("logical replication table synchronization worker for subscription \"%s\", table \"%s\" has finished",
					   MySubscription->name,
					   get_rel_name(MyLogicalRepWorker->relid)));
		CommitTransactionCommand();

		/* Find the leader apply worker and signal it. */
		logicalrep_worker_wakeup(WORKERTYPE_APPLY, MyLogicalRepWorker->subid,
								 InvalidOid);
	}

	/* Stop gracefully */
	proc_exit(0);
}

/*
 * Callback from syscache invalidation.
 */
void
InvalidateSyncingRelStates(Datum arg, int cacheid, uint32 hashvalue)
{
	relation_states_validity = SYNC_RELATIONS_STATE_NEEDS_REBUILD;
}

/*
 * Attempt to launch a sync worker for one or more sequences or a table, if
 * a worker slot is available and the retry interval has elapsed.
 *
 * wtype: sync worker type.
 * nsyncworkers: Number of currently running sync workers for the subscription.
 * relid:  InvalidOid for sequencesync worker, actual relid for tablesync
 * worker.
 * last_start_time: Pointer to the last start time of the worker.
 */
void
launch_sync_worker(LogicalRepWorkerType wtype, int nsyncworkers, Oid relid,
				   TimestampTz *last_start_time)
{
	TimestampTz now;

	Assert((wtype == WORKERTYPE_TABLESYNC && OidIsValid(relid)) ||
		   (wtype == WORKERTYPE_SEQUENCESYNC && !OidIsValid(relid)));

	/* If there is a free sync worker slot, start a new sync worker */
	if (nsyncworkers >= max_sync_workers_per_subscription)
		return;

	now = GetCurrentTimestamp();

	if (!(*last_start_time) ||
		TimestampDifferenceExceeds(*last_start_time, now,
								   wal_retrieve_retry_interval))
	{
		/*
		 * Set the last_start_time even if we fail to start the worker, so
		 * that we won't retry until wal_retrieve_retry_interval has elapsed.
		 */
		*last_start_time = now;
		(void) logicalrep_worker_launch(wtype,
										MyLogicalRepWorker->dbid,
										MySubscription->oid,
										MySubscription->name,
										MyLogicalRepWorker->userid,
										relid, DSM_HANDLE_INVALID, false);
	}
}

/*
 * Process possible state change(s) of relations that are being synchronized
 * and start new tablesync workers for the newly added tables. Also, start a
 * new sequencesync worker for the newly added sequences.
 */
void
ProcessSyncingRelations(XLogRecPtr current_lsn)
{
	switch (MyLogicalRepWorker->type)
	{
		case WORKERTYPE_PARALLEL_APPLY:

			/*
			 * Skip for parallel apply workers because they only operate on
			 * tables that are in a READY state. See pa_can_start() and
			 * should_apply_changes_for_rel().
			 */
			break;

		case WORKERTYPE_TABLESYNC:
			ProcessSyncingTablesForSync(current_lsn);
			break;

		case WORKERTYPE_APPLY:
			ProcessSyncingTablesForApply(current_lsn);
			ProcessSequencesForSync();
			break;

		case WORKERTYPE_SEQUENCESYNC:
			/* Should never happen. */
			elog(ERROR, "sequence synchronization worker is not expected to process relations");
			break;

		case WORKERTYPE_UNKNOWN:
			/* Should never happen. */
			elog(ERROR, "Unknown worker type");
	}
}

/*
 * Common code to fetch the up-to-date sync state info for tables and sequences.
 *
 * The pg_subscription_rel catalog is shared by tables and sequences. Changes
 * to either sequences or tables can affect the validity of relation states, so
 * we identify non-READY tables and non-READY sequences together to ensure
 * consistency.
 *
 * has_pending_subtables: true if the subscription has one or more tables that
 * are not in READY state, otherwise false.
 * has_pending_subsequences: true if the subscription has one or more sequences
 * that are not in READY state, otherwise false.
 */
void
FetchRelationStates(bool *has_pending_subtables,
					bool *has_pending_subsequences,
					bool *started_tx)
{
	/*
	 * has_subtables and has_subsequences_non_ready are declared as static,
	 * since the same value can be used until the system table is invalidated.
	 */
	static bool has_subtables = false;
	static bool has_subsequences_non_ready = false;

	*started_tx = false;

	if (relation_states_validity != SYNC_RELATIONS_STATE_VALID)
	{
		MemoryContext oldctx;
		List	   *rstates;
		SubscriptionRelState *rstate;

		relation_states_validity = SYNC_RELATIONS_STATE_REBUILD_STARTED;
		has_subsequences_non_ready = false;

		/* Clean the old lists. */
		list_free_deep(table_states_not_ready);
		table_states_not_ready = NIL;

		if (!IsTransactionState())
		{
			StartTransactionCommand();
			*started_tx = true;
		}

		/* Fetch tables and sequences that are in non-READY state. */
		rstates = GetSubscriptionRelations(MySubscription->oid, true, true,
										   true);

		/* Allocate the tracking info in a permanent memory context. */
		oldctx = MemoryContextSwitchTo(CacheMemoryContext);
		foreach_ptr(SubscriptionRelState, subrel, rstates)
		{
			if (get_rel_relkind(subrel->relid) == RELKIND_SEQUENCE)
				has_subsequences_non_ready = true;
			else
			{
				rstate = palloc_object(SubscriptionRelState);
				memcpy(rstate, subrel, sizeof(SubscriptionRelState));
				table_states_not_ready = lappend(table_states_not_ready,
												 rstate);
			}
		}
		MemoryContextSwitchTo(oldctx);

		/*
		 * Does the subscription have tables?
		 *
		 * If there were not-READY tables found then we know it does. But if
		 * table_states_not_ready was empty we still need to check again to
		 * see if there are 0 tables.
		 */
		has_subtables = (table_states_not_ready != NIL) ||
			HasSubscriptionTables(MySubscription->oid);

		/*
		 * If the subscription relation cache has been invalidated since we
		 * entered this routine, we still use and return the relations we just
		 * finished constructing, to avoid infinite loops, but we leave the
		 * table states marked as stale so that we'll rebuild it again on next
		 * access. Otherwise, we mark the table states as valid.
		 */
		if (relation_states_validity == SYNC_RELATIONS_STATE_REBUILD_STARTED)
			relation_states_validity = SYNC_RELATIONS_STATE_VALID;
	}

	if (has_pending_subtables)
		*has_pending_subtables = has_subtables;

	if (has_pending_subsequences)
		*has_pending_subsequences = has_subsequences_non_ready;
}
