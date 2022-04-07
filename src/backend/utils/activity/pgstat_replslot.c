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
 * Replication slot stats work a bit different than other other
 * variable-numbered stats. Slots do not have oids (so they can be created on
 * physical replicas). Use the slot index as object id while running. However,
 * the slot index can change when restarting. That is addressed by using the
 * name when (de-)serializing. After a restart it is possible for slots to
 * have been dropped while shut down, which is addressed by not restoring
 * stats for slots that cannot be found by name when starting up.
 *
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_replslot.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "replication/slot.h"
#include "utils/builtins.h"		/* for namestrcpy() */
#include "utils/pgstat_internal.h"


static int	get_replslot_index(const char *name);


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

	AssertArg(name != NULL);

	/* Check if the slot exits with the given name. */
	slot = SearchNamedReplicationSlot(name, true);

	if (!slot)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("replication slot \"%s\" does not exist",
						name)));

	/*
	 * Nothing to do for physical slots as we collect stats only for logical
	 * slots.
	 */
	if (SlotIsPhysical(slot))
		return;

	/* reset this one entry */
	pgstat_reset(PGSTAT_KIND_REPLSLOT, InvalidOid,
				 ReplicationSlotIndex(slot));
}

/*
 * Report replication slot statistics.
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

	/*
	 * Any mismatch should have been fixed in pgstat_create_replslot() or
	 * pgstat_acquire_replslot().
	 */
	Assert(namestrcmp(&statent->slotname, NameStr(slot->data.name)) == 0);

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

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_REPLSLOT, InvalidOid,
											ReplicationSlotIndex(slot), false);
	shstatent = (PgStatShared_ReplSlot *) entry_ref->shared_stats;

	/*
	 * NB: need to accept that there might be stats from an older slot, e.g.
	 * if we previously crashed after dropping a slot.
	 */
	memset(&shstatent->stats, 0, sizeof(shstatent->stats));
	namestrcpy(&shstatent->stats.slotname, NameStr(slot->data.name));

	pgstat_unlock_entry(entry_ref);
}

/*
 * Report replication slot has been acquired.
 */
void
pgstat_acquire_replslot(ReplicationSlot *slot)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_ReplSlot *shstatent;
	PgStat_StatReplSlotEntry *statent;

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_REPLSLOT, InvalidOid,
											ReplicationSlotIndex(slot), false);
	shstatent = (PgStatShared_ReplSlot *) entry_ref->shared_stats;
	statent = &shstatent->stats;

	/*
	 * NB: need to accept that there might be stats from an older slot, e.g.
	 * if we previously crashed after dropping a slot.
	 */
	if (NameStr(statent->slotname)[0] == 0 ||
		namestrcmp(&statent->slotname, NameStr(slot->data.name)) != 0)
	{
		memset(statent, 0, sizeof(*statent));
		namestrcpy(&statent->slotname, NameStr(slot->data.name));
	}

	pgstat_unlock_entry(entry_ref);
}

/*
 * Report replication slot drop.
 */
void
pgstat_drop_replslot(ReplicationSlot *slot)
{
	pgstat_drop_entry(PGSTAT_KIND_REPLSLOT, InvalidOid,
					  ReplicationSlotIndex(slot));
}

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * a pointer to the replication slot statistics struct.
 */
PgStat_StatReplSlotEntry *
pgstat_fetch_replslot(NameData slotname)
{
	int			idx = get_replslot_index(NameStr(slotname));

	if (idx == -1)
		return NULL;

	return (PgStat_StatReplSlotEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_REPLSLOT, InvalidOid, idx);
}

void
pgstat_replslot_to_serialized_name_cb(const PgStatShared_Common *header, NameData *name)
{
	namestrcpy(name, NameStr(((PgStatShared_ReplSlot *) header)->stats.slotname));
}

bool
pgstat_replslot_from_serialized_name_cb(const NameData *name, PgStat_HashKey *key)
{
	int			idx = get_replslot_index(NameStr(*name));

	/* slot might have been deleted */
	if (idx == -1)
		return false;

	key->kind = PGSTAT_KIND_REPLSLOT;
	key->dboid = InvalidOid;
	key->objoid = idx;

	return true;
}

void
pgstat_replslot_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_ReplSlot *) header)->stats.stat_reset_timestamp = ts;
}

static int
get_replslot_index(const char *name)
{
	ReplicationSlot *slot;

	AssertArg(name != NULL);

	slot = SearchNamedReplicationSlot(name, true);

	if (!slot)
		return -1;

	return ReplicationSlotIndex(slot);
}
