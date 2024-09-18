/* -------------------------------------------------------------------------
 *
 * pgstat_replslot.c
 *	  Implementation of replication slot statistics.
 *
 * This file contains the implementation of replication slot statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Replication slot stats work a bit different than other variable-numbered
 * stats. Slots do not have oids (so they can be created on physical
 * replicas). Use the slot index as object id while running. However, the slot
 * index can change when restarting. That is addressed by using the name when
 * (de-)serializing. After a restart it is possible for slots to have been
 * dropped while shut down, which is addressed by not restoring stats for
 * slots that cannot be found by name when starting up.
 *
 * Copyright (c) 2001-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_replslot.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "replication/slot.h"
#include "utils/pgstat_internal.h"


static int	get_replslot_index(const char *name, bool need_lock);


/*
 * Reset counters for a single replication slot.
 *
 * Permission checking for this function is managed through the normal
 * GRANT system.
 */
void
pgstat_reset_replslot(const char *name)
{
	ReplicationSlot *slot;

	Assert(name != NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	/* Check if the slot exits with the given name. */
	slot = SearchNamedReplicationSlot(name, false);

	if (!slot)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("replication slot \"%s\" does not exist",
						name)));

	/*
	 * Reset stats if it is a logical slot. Nothing to do for physical slots
	 * as we collect stats only for logical slots.
	 */
	if (SlotIsLogical(slot))
		pgstat_reset(PGSTAT_KIND_REPLSLOT, InvalidOid,
					 ReplicationSlotIndex(slot));

	LWLockRelease(ReplicationSlotControlLock);
}

/*
 * Report replication slot statistics.
 *
 * We can rely on the stats for the slot to exist and to belong to this
 * slot. We can only get here if pgstat_create_replslot() or
 * pgstat_acquire_replslot() have already been called.
 */
void
pgstat_report_replslot(ReplicationSlot *slot, const PgStat_StatReplSlotEntry *repSlotStat)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_ReplSlot *shstatent;
	PgStat_StatReplSlotEntry *statent;

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_REPLSLOT, InvalidOid,
											ReplicationSlotIndex(slot), false);
	shstatent = (PgStatShared_ReplSlot *) entry_ref->shared_stats;
	statent = &shstatent->stats;

	/* Update the replication slot statistics */
#define REPLSLOT_ACC(fld) statent->fld += repSlotStat->fld
	REPLSLOT_ACC(spill_txns);
	REPLSLOT_ACC(spill_count);
	REPLSLOT_ACC(spill_bytes);
	REPLSLOT_ACC(stream_txns);
	REPLSLOT_ACC(stream_count);
	REPLSLOT_ACC(stream_bytes);
	REPLSLOT_ACC(total_txns);
	REPLSLOT_ACC(total_bytes);
#undef REPLSLOT_ACC

	pgstat_unlock_entry(entry_ref);
}

/*
 * Report replication slot creation.
 *
 * NB: This gets called with ReplicationSlotAllocationLock already held, be
 * careful about calling back into slot.c.
 */
void
pgstat_create_replslot(ReplicationSlot *slot)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_ReplSlot *shstatent;

	Assert(LWLockHeldByMeInMode(ReplicationSlotAllocationLock, LW_EXCLUSIVE));

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_REPLSLOT, InvalidOid,
											ReplicationSlotIndex(slot), false);
	shstatent = (PgStatShared_ReplSlot *) entry_ref->shared_stats;

	/*
	 * NB: need to accept that there might be stats from an older slot, e.g.
	 * if we previously crashed after dropping a slot.
	 */
	memset(&shstatent->stats, 0, sizeof(shstatent->stats));

	pgstat_unlock_entry(entry_ref);
}

/*
 * Report replication slot has been acquired.
 *
 * This guarantees that a stats entry exists during later
 * pgstat_report_replslot() calls.
 *
 * If we previously crashed, no stats data exists. But if we did not crash,
 * the stats do belong to this slot:
 * - the stats cannot belong to a dropped slot, pgstat_drop_replslot() would
 *   have been called
 * - if the slot was removed while shut down,
 *   pgstat_replslot_from_serialized_name_cb() returning false would have
 *   caused the stats to be dropped
 */
void
pgstat_acquire_replslot(ReplicationSlot *slot)
{
	pgstat_get_entry_ref(PGSTAT_KIND_REPLSLOT, InvalidOid,
						 ReplicationSlotIndex(slot), true, NULL);
}

/*
 * Report replication slot drop.
 */
void
pgstat_drop_replslot(ReplicationSlot *slot)
{
	Assert(LWLockHeldByMeInMode(ReplicationSlotAllocationLock, LW_EXCLUSIVE));

	if (!pgstat_drop_entry(PGSTAT_KIND_REPLSLOT, InvalidOid,
						   ReplicationSlotIndex(slot)))
		pgstat_request_entry_refs_gc();
}

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * a pointer to the replication slot statistics struct.
 */
PgStat_StatReplSlotEntry *
pgstat_fetch_replslot(NameData slotname)
{
	int			idx;
	PgStat_StatReplSlotEntry *slotentry = NULL;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	idx = get_replslot_index(NameStr(slotname), false);

	if (idx != -1)
		slotentry = (PgStat_StatReplSlotEntry *) pgstat_fetch_entry(PGSTAT_KIND_REPLSLOT,
																	InvalidOid, idx);

	LWLockRelease(ReplicationSlotControlLock);

	return slotentry;
}

void
pgstat_replslot_to_serialized_name_cb(const PgStat_HashKey *key, const PgStatShared_Common *header, NameData *name)
{
	/*
	 * This is only called late during shutdown. The set of existing slots
	 * isn't allowed to change at this point, we can assume that a slot exists
	 * at the offset.
	 */
	if (!ReplicationSlotName(key->objid, name))
		elog(ERROR, "could not find name for replication slot index %llu",
			 (unsigned long long) key->objid);
}

bool
pgstat_replslot_from_serialized_name_cb(const NameData *name, PgStat_HashKey *key)
{
	int			idx = get_replslot_index(NameStr(*name), true);

	/* slot might have been deleted */
	if (idx == -1)
		return false;

	key->kind = PGSTAT_KIND_REPLSLOT;
	key->dboid = InvalidOid;
	key->objid = idx;

	return true;
}

void
pgstat_replslot_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_ReplSlot *) header)->stats.stat_reset_timestamp = ts;
}

static int
get_replslot_index(const char *name, bool need_lock)
{
	ReplicationSlot *slot;

	Assert(name != NULL);

	slot = SearchNamedReplicationSlot(name, need_lock);

	if (!slot)
		return -1;

	return ReplicationSlotIndex(slot);
}
