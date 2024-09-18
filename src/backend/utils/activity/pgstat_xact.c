/* -------------------------------------------------------------------------
 *
 * pgstat_xact.c
 *	  Transactional integration for the cumulative statistics system.
 *
 * Copyright (c) 2001-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_xact.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"


typedef struct PgStat_PendingDroppedStatsItem
{
	xl_xact_stats_item item;
	bool		is_create;
	dlist_node	node;
} PgStat_PendingDroppedStatsItem;


static void AtEOXact_PgStat_DroppedStats(PgStat_SubXactStatus *xact_state, bool isCommit);
static void AtEOSubXact_PgStat_DroppedStats(PgStat_SubXactStatus *xact_state,
											bool isCommit, int nestDepth);

static PgStat_SubXactStatus *pgStatXactStack = NULL;


/*
 * Called from access/transam/xact.c at top-level transaction commit/abort.
 */
void
AtEOXact_PgStat(bool isCommit, bool parallel)
{
	PgStat_SubXactStatus *xact_state;

	AtEOXact_PgStat_Database(isCommit, parallel);

	/* handle transactional stats information */
	xact_state = pgStatXactStack;
	if (xact_state != NULL)
	{
		Assert(xact_state->nest_level == 1);
		Assert(xact_state->prev == NULL);

		AtEOXact_PgStat_Relations(xact_state, isCommit);
		AtEOXact_PgStat_DroppedStats(xact_state, isCommit);
	}
	pgStatXactStack = NULL;

	/* Make sure any stats snapshot is thrown away */
	pgstat_clear_snapshot();
}

/*
 * When committing, drop stats for objects dropped in the transaction. When
 * aborting, drop stats for objects created in the transaction.
 */
static void
AtEOXact_PgStat_DroppedStats(PgStat_SubXactStatus *xact_state, bool isCommit)
{
	dlist_mutable_iter iter;
	int			not_freed_count = 0;

	if (dclist_count(&xact_state->pending_drops) == 0)
		return;

	dclist_foreach_modify(iter, &xact_state->pending_drops)
	{
		PgStat_PendingDroppedStatsItem *pending =
			dclist_container(PgStat_PendingDroppedStatsItem, node, iter.cur);
		xl_xact_stats_item *it = &pending->item;
		uint64		objid = ((uint64) it->objid_hi) << 32 | it->objid_lo;

		if (isCommit && !pending->is_create)
		{
			/*
			 * Transaction that dropped an object committed. Drop the stats
			 * too.
			 */
			if (!pgstat_drop_entry(it->kind, it->dboid, objid))
				not_freed_count++;
		}
		else if (!isCommit && pending->is_create)
		{
			/*
			 * Transaction that created an object aborted. Drop the stats
			 * associated with the object.
			 */
			if (!pgstat_drop_entry(it->kind, it->dboid, objid))
				not_freed_count++;
		}

		dclist_delete_from(&xact_state->pending_drops, &pending->node);
		pfree(pending);
	}

	if (not_freed_count > 0)
		pgstat_request_entry_refs_gc();
}

/*
 * Called from access/transam/xact.c at subtransaction commit/abort.
 */
void
AtEOSubXact_PgStat(bool isCommit, int nestDepth)
{
	PgStat_SubXactStatus *xact_state;

	/* merge the sub-transaction's transactional stats into the parent */
	xact_state = pgStatXactStack;
	if (xact_state != NULL &&
		xact_state->nest_level >= nestDepth)
	{
		/* delink xact_state from stack immediately to simplify reuse case */
		pgStatXactStack = xact_state->prev;

		AtEOSubXact_PgStat_Relations(xact_state, isCommit, nestDepth);
		AtEOSubXact_PgStat_DroppedStats(xact_state, isCommit, nestDepth);

		pfree(xact_state);
	}
}

/*
 * Like AtEOXact_PgStat_DroppedStats(), but for subtransactions.
 */
static void
AtEOSubXact_PgStat_DroppedStats(PgStat_SubXactStatus *xact_state,
								bool isCommit, int nestDepth)
{
	PgStat_SubXactStatus *parent_xact_state;
	dlist_mutable_iter iter;
	int			not_freed_count = 0;

	if (dclist_count(&xact_state->pending_drops) == 0)
		return;

	parent_xact_state = pgstat_get_xact_stack_level(nestDepth - 1);

	dclist_foreach_modify(iter, &xact_state->pending_drops)
	{
		PgStat_PendingDroppedStatsItem *pending =
			dclist_container(PgStat_PendingDroppedStatsItem, node, iter.cur);
		xl_xact_stats_item *it = &pending->item;
		uint64		objid = ((uint64) it->objid_hi) << 32 | it->objid_lo;

		dclist_delete_from(&xact_state->pending_drops, &pending->node);

		if (!isCommit && pending->is_create)
		{
			/*
			 * Subtransaction creating a new stats object aborted. Drop the
			 * stats object.
			 */
			if (!pgstat_drop_entry(it->kind, it->dboid, objid))
				not_freed_count++;
			pfree(pending);
		}
		else if (isCommit)
		{
			/*
			 * Subtransaction dropping a stats object committed. Can't yet
			 * remove the stats object, the surrounding transaction might
			 * still abort. Pass it on to the parent.
			 */
			dclist_push_tail(&parent_xact_state->pending_drops, &pending->node);
		}
		else
		{
			pfree(pending);
		}
	}

	Assert(dclist_count(&xact_state->pending_drops) == 0);
	if (not_freed_count > 0)
		pgstat_request_entry_refs_gc();
}

/*
 * Save the transactional stats state at 2PC transaction prepare.
 */
void
AtPrepare_PgStat(void)
{
	PgStat_SubXactStatus *xact_state;

	xact_state = pgStatXactStack;
	if (xact_state != NULL)
	{
		Assert(xact_state->nest_level == 1);
		Assert(xact_state->prev == NULL);

		AtPrepare_PgStat_Relations(xact_state);
	}
}

/*
 * Clean up after successful PREPARE.
 *
 * Note: AtEOXact_PgStat is not called during PREPARE.
 */
void
PostPrepare_PgStat(void)
{
	PgStat_SubXactStatus *xact_state;

	/*
	 * We don't bother to free any of the transactional state, since it's all
	 * in TopTransactionContext and will go away anyway.
	 */
	xact_state = pgStatXactStack;
	if (xact_state != NULL)
	{
		Assert(xact_state->nest_level == 1);
		Assert(xact_state->prev == NULL);

		PostPrepare_PgStat_Relations(xact_state);
	}
	pgStatXactStack = NULL;

	/* Make sure any stats snapshot is thrown away */
	pgstat_clear_snapshot();
}

/*
 * Ensure (sub)transaction stack entry for the given nest_level exists, adding
 * it if needed.
 */
PgStat_SubXactStatus *
pgstat_get_xact_stack_level(int nest_level)
{
	PgStat_SubXactStatus *xact_state;

	xact_state = pgStatXactStack;
	if (xact_state == NULL || xact_state->nest_level != nest_level)
	{
		xact_state = (PgStat_SubXactStatus *)
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(PgStat_SubXactStatus));
		dclist_init(&xact_state->pending_drops);
		xact_state->nest_level = nest_level;
		xact_state->prev = pgStatXactStack;
		xact_state->first = NULL;
		pgStatXactStack = xact_state;
	}
	return xact_state;
}

/*
 * Get stat items that need to be dropped at commit / abort.
 *
 * When committing, stats for objects that have been dropped in the
 * transaction are returned. When aborting, stats for newly created objects are
 * returned.
 *
 * Used by COMMIT / ABORT and 2PC PREPARE processing when building their
 * respective WAL records, to ensure stats are dropped in case of a crash / on
 * standbys.
 *
 * The list of items is allocated in CurrentMemoryContext and must be freed by
 * the caller (directly or via memory context reset).
 */
int
pgstat_get_transactional_drops(bool isCommit, xl_xact_stats_item **items)
{
	PgStat_SubXactStatus *xact_state = pgStatXactStack;
	int			nitems = 0;
	dlist_iter	iter;

	if (xact_state == NULL)
		return 0;

	/*
	 * We expect to be called for subtransaction abort (which logs a WAL
	 * record), but not for subtransaction commit (which doesn't).
	 */
	Assert(!isCommit || xact_state->nest_level == 1);
	Assert(!isCommit || xact_state->prev == NULL);

	*items = palloc(dclist_count(&xact_state->pending_drops)
					* sizeof(xl_xact_stats_item));

	dclist_foreach(iter, &xact_state->pending_drops)
	{
		PgStat_PendingDroppedStatsItem *pending =
			dclist_container(PgStat_PendingDroppedStatsItem, node, iter.cur);

		if (isCommit && pending->is_create)
			continue;
		if (!isCommit && !pending->is_create)
			continue;

		Assert(nitems < dclist_count(&xact_state->pending_drops));
		(*items)[nitems++] = pending->item;
	}

	return nitems;
}

/*
 * Execute scheduled drops post-commit. Called from xact_redo_commit() /
 * xact_redo_abort() during recovery, and from FinishPreparedTransaction()
 * during normal 2PC COMMIT/ABORT PREPARED processing.
 */
void
pgstat_execute_transactional_drops(int ndrops, struct xl_xact_stats_item *items, bool is_redo)
{
	int			not_freed_count = 0;

	if (ndrops == 0)
		return;

	for (int i = 0; i < ndrops; i++)
	{
		xl_xact_stats_item *it = &items[i];
		uint64		objid = ((uint64) it->objid_hi) << 32 | it->objid_lo;

		if (!pgstat_drop_entry(it->kind, it->dboid, objid))
			not_freed_count++;
	}

	if (not_freed_count > 0)
		pgstat_request_entry_refs_gc();
}

static void
create_drop_transactional_internal(PgStat_Kind kind, Oid dboid, uint64 objid, bool is_create)
{
	int			nest_level = GetCurrentTransactionNestLevel();
	PgStat_SubXactStatus *xact_state;
	PgStat_PendingDroppedStatsItem *drop = (PgStat_PendingDroppedStatsItem *)
		MemoryContextAlloc(TopTransactionContext, sizeof(PgStat_PendingDroppedStatsItem));

	xact_state = pgstat_get_xact_stack_level(nest_level);

	drop->is_create = is_create;
	drop->item.kind = kind;
	drop->item.dboid = dboid;
	drop->item.objid_lo = (uint32) objid;
	drop->item.objid_hi = (uint32) (objid >> 32);

	dclist_push_tail(&xact_state->pending_drops, &drop->node);
}

/*
 * Create a stats entry for a newly created database object in a transactional
 * manner.
 *
 * I.e. if the current (sub-)transaction aborts, the stats entry will also be
 * dropped.
 */
void
pgstat_create_transactional(PgStat_Kind kind, Oid dboid, uint64 objid)
{
	if (pgstat_get_entry_ref(kind, dboid, objid, false, NULL))
	{
		ereport(WARNING,
				errmsg("resetting existing statistics for kind %s, db=%u, oid=%llu",
					   (pgstat_get_kind_info(kind))->name, dboid,
					   (unsigned long long) objid));

		pgstat_reset(kind, dboid, objid);
	}

	create_drop_transactional_internal(kind, dboid, objid, /* create */ true);
}

/*
 * Drop a stats entry for a just dropped database object in a transactional
 * manner.
 *
 * I.e. if the current (sub-)transaction aborts, the stats entry will stay
 * alive.
 */
void
pgstat_drop_transactional(PgStat_Kind kind, Oid dboid, uint64 objid)
{
	create_drop_transactional_internal(kind, dboid, objid, /* create */ false);
}
