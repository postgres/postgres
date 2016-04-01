/*--------------------------------------------------------------------------
 *
 * test_slot_timelines.c
 *              Test harness code for slot timeline following
 *
 * Copyright (c) 2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *              src/test/modules/test_slot_timelines/test_slot_timelines.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "replication/slot.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_slot_timelines_create_logical_slot);
PG_FUNCTION_INFO_V1(test_slot_timelines_advance_logical_slot);

static void clear_slot_transient_state(void);

/*
 * Create a new logical slot, with invalid LSN and xid, directly. This does not
 * use the snapshot builder or logical decoding machinery. It's only intended
 * for creating a slot on a replica that mirrors the state of a slot on an
 * upstream master.
 *
 * Note that this is test harness code. You shouldn't expose slot internals
 * to SQL like this for any real world usage. See the README.
 */
Datum
test_slot_timelines_create_logical_slot(PG_FUNCTION_ARGS)
{
	char	   *slotname = text_to_cstring(PG_GETARG_TEXT_P(0));
	char	   *plugin = text_to_cstring(PG_GETARG_TEXT_P(1));

	CheckSlotRequirements();

	ReplicationSlotCreate(slotname, true, RS_PERSISTENT);

	/* register the plugin name with the slot */
	StrNCpy(NameStr(MyReplicationSlot->data.plugin), plugin, NAMEDATALEN);

	/*
	 * Initialize persistent state to placeholders to be set by
	 * test_slot_timelines_advance_logical_slot .
	 */
	MyReplicationSlot->data.xmin = InvalidTransactionId;
	MyReplicationSlot->data.catalog_xmin = InvalidTransactionId;
	MyReplicationSlot->data.restart_lsn = InvalidXLogRecPtr;
	MyReplicationSlot->data.confirmed_flush = InvalidXLogRecPtr;

	clear_slot_transient_state();

	ReplicationSlotRelease();

	PG_RETURN_VOID();
}

/*
 * Set the state of a slot.
 *
 * This doesn't maintain the non-persistent state at all,
 * but since the slot isn't in use that's OK.
 *
 * There's intentionally no check to prevent slots going backwards
 * because they can actually go backwards if the master crashes when
 * it hasn't yet flushed slot state to disk then we copy the older
 * slot state after recovery.
 *
 * There's no checking done for xmin or catalog xmin either, since
 * we can't really do anything useful that accounts for xid wrap-around.
 *
 * Note that this is test harness code. You shouldn't expose slot internals
 * to SQL like this for any real world usage. See the README.
 */
Datum
test_slot_timelines_advance_logical_slot(PG_FUNCTION_ARGS)
{
	char	   *slotname = text_to_cstring(PG_GETARG_TEXT_P(0));
	TransactionId new_xmin = DatumGetTransactionId(PG_GETARG_DATUM(1));
	TransactionId new_catalog_xmin = DatumGetTransactionId(PG_GETARG_DATUM(2));
	XLogRecPtr	restart_lsn = PG_GETARG_LSN(3);
	XLogRecPtr	confirmed_lsn = PG_GETARG_LSN(4);

	CheckSlotRequirements();

	ReplicationSlotAcquire(slotname);

	if (MyReplicationSlot->data.database != MyDatabaseId)
		elog(ERROR, "trying to update a slot on a different database");

	MyReplicationSlot->data.xmin = new_xmin;
	MyReplicationSlot->data.catalog_xmin = new_catalog_xmin;
	MyReplicationSlot->data.restart_lsn = restart_lsn;
	MyReplicationSlot->data.confirmed_flush = confirmed_lsn;

	clear_slot_transient_state();

	ReplicationSlotMarkDirty();
	ReplicationSlotSave();
	ReplicationSlotRelease();

	ReplicationSlotsComputeRequiredXmin(false);
	ReplicationSlotsComputeRequiredLSN();

	PG_RETURN_VOID();
}

static void
clear_slot_transient_state(void)
{
	Assert(MyReplicationSlot != NULL);

	/*
	 * Make sure the slot state is the same as if it were newly loaded from
	 * disk on recovery.
	 */
	MyReplicationSlot->effective_xmin = MyReplicationSlot->data.xmin;
	MyReplicationSlot->effective_catalog_xmin = MyReplicationSlot->data.catalog_xmin;

	MyReplicationSlot->candidate_catalog_xmin = InvalidTransactionId;
	MyReplicationSlot->candidate_xmin_lsn = InvalidXLogRecPtr;
	MyReplicationSlot->candidate_restart_lsn = InvalidXLogRecPtr;
	MyReplicationSlot->candidate_restart_valid = InvalidXLogRecPtr;
}
