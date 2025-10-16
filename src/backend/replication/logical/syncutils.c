/*-------------------------------------------------------------------------
 * syncutils.c
 *	  PostgreSQL logical replication: common synchronization code
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
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

	StartTransactionCommand();
	ereport(LOG,
			(errmsg("logical replication table synchronization worker for subscription \"%s\", table \"%s\" has finished",
					MySubscription->name,
					get_rel_name(MyLogicalRepWorker->relid))));
	CommitTransactionCommand();

	/* Find the leader apply worker and signal it. */
	logicalrep_worker_wakeup(MyLogicalRepWorker->subid, InvalidOid);

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
 * Process possible state change(s) of relations that are being synchronized.
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
			break;

		case WORKERTYPE_UNKNOWN:
			/* Should never happen. */
			elog(ERROR, "Unknown worker type");
	}
}

/*
 * Common code to fetch the up-to-date sync state info into the static lists.
 *
 * Returns true if subscription has 1 or more tables, else false.
 *
 * Note: If this function started the transaction (indicated by the parameter)
 * then it is the caller's responsibility to commit it.
 */
bool
FetchRelationStates(bool *started_tx)
{
	static bool has_subtables = false;

	*started_tx = false;

	if (relation_states_validity != SYNC_RELATIONS_STATE_VALID)
	{
		MemoryContext oldctx;
		List	   *rstates;
		ListCell   *lc;
		SubscriptionRelState *rstate;

		relation_states_validity = SYNC_RELATIONS_STATE_REBUILD_STARTED;

		/* Clean the old lists. */
		list_free_deep(table_states_not_ready);
		table_states_not_ready = NIL;

		if (!IsTransactionState())
		{
			StartTransactionCommand();
			*started_tx = true;
		}

		/* Fetch tables and sequences that are in non-ready state. */
		rstates = GetSubscriptionRelations(MySubscription->oid, true);

		/* Allocate the tracking info in a permanent memory context. */
		oldctx = MemoryContextSwitchTo(CacheMemoryContext);
		foreach(lc, rstates)
		{
			rstate = palloc(sizeof(SubscriptionRelState));
			memcpy(rstate, lfirst(lc), sizeof(SubscriptionRelState));
			table_states_not_ready = lappend(table_states_not_ready, rstate);
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

	return has_subtables;
}
