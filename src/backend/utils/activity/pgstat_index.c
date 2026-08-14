/* -------------------------------------------------------------------------
 *
 * pgstat_index.c
 *	  Implementation of index statistics.
 *
 * This file contains the implementation of index statistics.
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_index.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "catalog/catalog.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"
#include "utils/rel.h"
#include "utils/timestamp.h"


/*
 * Flush out pending stats for an index entry.
 *
 * If nowait is true and the lock could not be immediately acquired, returns
 * false without flushing the entry.  Otherwise returns true.
 *
 * Some of the stats are copied to the corresponding pending database stats
 * entry when successfully flushing.
 */
bool
pgstat_index_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	Oid			dboid;
	PgStat_RelationStatus *lstats;	/* pending stats entry */
	PgStatShared_Index *shidxstats;
	PgStat_StatIdxEntry *idxentry;	/* index entry of shared stats */
	PgStat_StatDBEntry *dbentry;	/* pending database entry */

	dboid = entry_ref->shared_entry->key.dboid;
	lstats = (PgStat_RelationStatus *) entry_ref->pending;
	shidxstats = (PgStatShared_Index *) entry_ref->shared_stats;

	/*
	 * Ignore entries that didn't accumulate any actual counts, such as
	 * indexes that were opened by the planner but not used.
	 */
	if (pg_memory_is_all_zeros(&lstats->idx,
							   sizeof(struct PgStat_IndexCounts)))
		return true;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	/* Add the values to the shared entry. */
	idxentry = &shidxstats->stats;

	idxentry->numscans += lstats->idx.numscans;
	if (lstats->idx.numscans)
	{
		TimestampTz t = GetCurrentTransactionStopTimestamp();

		if (t > idxentry->lastscan)
			idxentry->lastscan = t;
	}
	idxentry->tuples_returned += lstats->idx.tuples_returned;
	idxentry->tuples_fetched += lstats->idx.tuples_fetched;
	idxentry->blocks_fetched += lstats->idx.blocks_fetched;
	idxentry->blocks_hit += lstats->idx.blocks_hit;

	pgstat_unlock_entry(entry_ref);

	/* The entry was successfully flushed, add the same to database stats */
	dbentry = pgstat_prep_database_pending(dboid);
	dbentry->tuples_returned += lstats->idx.tuples_returned;
	dbentry->tuples_fetched += lstats->idx.tuples_fetched;
	dbentry->blocks_fetched += lstats->idx.blocks_fetched;
	dbentry->blocks_hit += lstats->idx.blocks_hit;

	return true;
}

/*
 * Callback to delete pending index stats.
 */
void
pgstat_index_delete_pending_cb(PgStat_EntryRef *entry_ref)
{
	PgStat_RelationStatus *pending = (PgStat_RelationStatus *) entry_ref->pending;

	if (pending->relation)
		pgstat_unlink_relation(pending->relation);
}

/*
 * Callback to reset the timestamp on an index stats entry.
 */
void
pgstat_index_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_Index *) header)->stats.stat_reset_time = ts;
}

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * the collected statistics for one index or NULL. NULL doesn't mean
 * that the index doesn't exist, just that there are no statistics, so the
 * caller is better off to report ZERO instead.
 */
PgStat_StatIdxEntry *
pgstat_fetch_stat_idxentry(Oid relid)
{
	return pgstat_fetch_stat_idxentry_ext(IsSharedRelation(relid), relid, NULL);
}

/*
 * More efficient version of pgstat_fetch_stat_idxentry(), allowing to specify
 * whether the to-be-accessed index is a shared relation or not.  This version
 * also returns whether the caller can pfree() the result if desired.
 */
PgStat_StatIdxEntry *
pgstat_fetch_stat_idxentry_ext(bool shared, Oid reloid, bool *may_free)
{
	Oid			dboid = (shared ? InvalidOid : MyDatabaseId);

	return (PgStat_StatIdxEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_INDEX, dboid, reloid, may_free);
}
