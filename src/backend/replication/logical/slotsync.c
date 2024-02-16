/*-------------------------------------------------------------------------
 * slotsync.c
 *	   Functionality for synchronizing slots to a standby server from the
 *         primary server.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/slotsync.c
 *
 * This file contains the code for slot synchronization on a physical standby
 * to fetch logical failover slots information from the primary server, create
 * the slots on the standby and synchronize them. This is done by a call to SQL
 * function pg_sync_replication_slots.
 *
 * If on physical standby, the WAL corresponding to the remote's restart_lsn
 * is not available or the remote's catalog_xmin precedes the oldest xid for which
 * it is guaranteed that rows wouldn't have been removed then we cannot create
 * the local standby slot because that would mean moving the local slot
 * backward and decoding won't be possible via such a slot. In this case, the
 * slot will be marked as RS_TEMPORARY. Once the primary server catches up,
 * the slot will be marked as RS_PERSISTENT (which means sync-ready) after
 * which we can call pg_sync_replication_slots() periodically to perform
 * syncs.
 *
 * Any standby synchronized slots will be dropped if they no longer need
 * to be synchronized. See comment atop drop_local_obsolete_slots() for more
 * details.
 *---------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "replication/logical.h"
#include "replication/slotsync.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

/* Struct for sharing information to control slot synchronization. */
typedef struct SlotSyncCtxStruct
{
	/* prevents concurrent slot syncs to avoid slot overwrites */
	bool		syncing;
	slock_t		mutex;
} SlotSyncCtxStruct;

SlotSyncCtxStruct *SlotSyncCtx = NULL;

/*
 * Flag to tell if we are syncing replication slots. Unlike the 'syncing' flag
 * in SlotSyncCtxStruct, this flag is true only if the current process is
 * performing slot synchronization.
 */
static bool syncing_slots = false;

/*
 * Structure to hold information fetched from the primary server about a logical
 * replication slot.
 */
typedef struct RemoteSlot
{
	char	   *name;
	char	   *plugin;
	char	   *database;
	bool		two_phase;
	bool		failover;
	XLogRecPtr	restart_lsn;
	XLogRecPtr	confirmed_lsn;
	TransactionId catalog_xmin;

	/* RS_INVAL_NONE if valid, or the reason of invalidation */
	ReplicationSlotInvalidationCause invalidated;
} RemoteSlot;

/*
 * If necessary, update the local synced slot's metadata based on the data
 * from the remote slot.
 *
 * If no update was needed (the data of the remote slot is the same as the
 * local slot) return false, otherwise true.
 */
static bool
update_local_synced_slot(RemoteSlot *remote_slot, Oid remote_dbid)
{
	ReplicationSlot *slot = MyReplicationSlot;
	bool		xmin_changed;
	bool		restart_lsn_changed;
	NameData	plugin_name;

	Assert(slot->data.invalidated == RS_INVAL_NONE);

	xmin_changed = (remote_slot->catalog_xmin != slot->data.catalog_xmin);
	restart_lsn_changed = (remote_slot->restart_lsn != slot->data.restart_lsn);

	if (!xmin_changed &&
		!restart_lsn_changed &&
		remote_dbid == slot->data.database &&
		remote_slot->two_phase == slot->data.two_phase &&
		remote_slot->failover == slot->data.failover &&
		remote_slot->confirmed_lsn == slot->data.confirmed_flush &&
		strcmp(remote_slot->plugin, NameStr(slot->data.plugin)) == 0)
		return false;

	/* Avoid expensive operations while holding a spinlock. */
	namestrcpy(&plugin_name, remote_slot->plugin);

	SpinLockAcquire(&slot->mutex);
	slot->data.plugin = plugin_name;
	slot->data.database = remote_dbid;
	slot->data.two_phase = remote_slot->two_phase;
	slot->data.failover = remote_slot->failover;
	slot->data.restart_lsn = remote_slot->restart_lsn;
	slot->data.confirmed_flush = remote_slot->confirmed_lsn;
	slot->data.catalog_xmin = remote_slot->catalog_xmin;
	slot->effective_catalog_xmin = remote_slot->catalog_xmin;
	SpinLockRelease(&slot->mutex);

	if (xmin_changed)
		ReplicationSlotsComputeRequiredXmin(false);

	if (restart_lsn_changed)
		ReplicationSlotsComputeRequiredLSN();

	return true;
}

/*
 * Get the list of local logical slots that are synchronized from the
 * primary server.
 */
static List *
get_local_synced_slots(void)
{
	List	   *local_slots = NIL;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		/* Check if it is a synchronized slot */
		if (s->in_use && s->data.synced)
		{
			Assert(SlotIsLogical(s));
			local_slots = lappend(local_slots, s);
		}
	}

	LWLockRelease(ReplicationSlotControlLock);

	return local_slots;
}

/*
 * Helper function to check if local_slot is required to be retained.
 *
 * Return false either if local_slot does not exist in the remote_slots list
 * or is invalidated while the corresponding remote slot is still valid,
 * otherwise true.
 */
static bool
local_sync_slot_required(ReplicationSlot *local_slot, List *remote_slots)
{
	bool		remote_exists = false;
	bool		locally_invalidated = false;

	foreach_ptr(RemoteSlot, remote_slot, remote_slots)
	{
		if (strcmp(remote_slot->name, NameStr(local_slot->data.name)) == 0)
		{
			remote_exists = true;

			/*
			 * If remote slot is not invalidated but local slot is marked as
			 * invalidated, then set locally_invalidated flag.
			 */
			SpinLockAcquire(&local_slot->mutex);
			locally_invalidated =
				(remote_slot->invalidated == RS_INVAL_NONE) &&
				(local_slot->data.invalidated != RS_INVAL_NONE);
			SpinLockRelease(&local_slot->mutex);

			break;
		}
	}

	return (remote_exists && !locally_invalidated);
}

/*
 * Drop local obsolete slots.
 *
 * Drop the local slots that no longer need to be synced i.e. these either do
 * not exist on the primary or are no longer enabled for failover.
 *
 * Additionally, drop any slots that are valid on the primary but got
 * invalidated on the standby. This situation may occur due to the following
 * reasons:
 * - The 'max_slot_wal_keep_size' on the standby is insufficient to retain WAL
 *   records from the restart_lsn of the slot.
 * - 'primary_slot_name' is temporarily reset to null and the physical slot is
 *   removed.
 * These dropped slots will get recreated in next sync-cycle and it is okay to
 * drop and recreate such slots as long as these are not consumable on the
 * standby (which is the case currently).
 *
 * Note: Change of 'wal_level' on the primary server to a level lower than
 * logical may also result in slot invalidation and removal on the standby.
 * This is because such 'wal_level' change is only possible if the logical
 * slots are removed on the primary server, so it's expected to see the
 * slots being invalidated and removed on the standby too (and re-created
 * if they are re-created on the primary server).
 */
static void
drop_local_obsolete_slots(List *remote_slot_list)
{
	List	   *local_slots = get_local_synced_slots();

	foreach_ptr(ReplicationSlot, local_slot, local_slots)
	{
		/* Drop the local slot if it is not required to be retained. */
		if (!local_sync_slot_required(local_slot, remote_slot_list))
		{
			bool		synced_slot;

			/*
			 * Use shared lock to prevent a conflict with
			 * ReplicationSlotsDropDBSlots(), trying to drop the same slot
			 * during a drop-database operation.
			 */
			LockSharedObject(DatabaseRelationId, local_slot->data.database,
							 0, AccessShareLock);

			/*
			 * In the small window between getting the slot to drop and
			 * locking the database, there is a possibility of a parallel
			 * database drop by the startup process and the creation of a new
			 * slot by the user. This new user-created slot may end up using
			 * the same shared memory as that of 'local_slot'. Thus check if
			 * local_slot is still the synced one before performing actual
			 * drop.
			 */
			SpinLockAcquire(&local_slot->mutex);
			synced_slot = local_slot->in_use && local_slot->data.synced;
			SpinLockRelease(&local_slot->mutex);

			if (synced_slot)
			{
				ReplicationSlotAcquire(NameStr(local_slot->data.name), true);
				ReplicationSlotDropAcquired();
			}

			UnlockSharedObject(DatabaseRelationId, local_slot->data.database,
							   0, AccessShareLock);

			ereport(LOG,
					errmsg("dropped replication slot \"%s\" of dbid %d",
						   NameStr(local_slot->data.name),
						   local_slot->data.database));
		}
	}
}

/*
 * Reserve WAL for the currently active local slot using the specified WAL
 * location (restart_lsn).
 *
 * If the given WAL location has been removed, reserve WAL using the oldest
 * existing WAL segment.
 */
static void
reserve_wal_for_local_slot(XLogRecPtr restart_lsn)
{
	XLogSegNo	oldest_segno;
	XLogSegNo	segno;
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(XLogRecPtrIsInvalid(slot->data.restart_lsn));

	while (true)
	{
		SpinLockAcquire(&slot->mutex);
		slot->data.restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);

		/* Prevent WAL removal as fast as possible */
		ReplicationSlotsComputeRequiredLSN();

		XLByteToSeg(slot->data.restart_lsn, segno, wal_segment_size);

		/*
		 * Find the oldest existing WAL segment file.
		 *
		 * Normally, we can determine it by using the last removed segment
		 * number. However, if no WAL segment files have been removed by a
		 * checkpoint since startup, we need to search for the oldest segment
		 * file from the current timeline existing in XLOGDIR.
		 *
		 * XXX: Currently, we are searching for the oldest segment in the
		 * current timeline as there is less chance of the slot's restart_lsn
		 * from being some prior timeline, and even if it happens, in the
		 * worst case, we will wait to sync till the slot's restart_lsn moved
		 * to the current timeline.
		 */
		oldest_segno = XLogGetLastRemovedSegno() + 1;

		if (oldest_segno == 1)
		{
			TimeLineID	cur_timeline;

			GetWalRcvFlushRecPtr(NULL, &cur_timeline);
			oldest_segno = XLogGetOldestSegno(cur_timeline);
		}

		elog(DEBUG1, "segno: %ld of purposed restart_lsn for the synced slot, oldest_segno: %ld available",
			 segno, oldest_segno);

		/*
		 * If all required WAL is still there, great, otherwise retry. The
		 * slot should prevent further removal of WAL, unless there's a
		 * concurrent ReplicationSlotsComputeRequiredLSN() after we've written
		 * the new restart_lsn above, so normally we should never need to loop
		 * more than twice.
		 */
		if (segno >= oldest_segno)
			break;

		/* Retry using the location of the oldest wal segment */
		XLogSegNoOffsetToRecPtr(oldest_segno, 0, wal_segment_size, restart_lsn);
	}
}

/*
 * If the remote restart_lsn and catalog_xmin have caught up with the
 * local ones, then update the LSNs and persist the local synced slot for
 * future synchronization; otherwise, do nothing.
 */
static void
update_and_persist_local_synced_slot(RemoteSlot *remote_slot, Oid remote_dbid)
{
	ReplicationSlot *slot = MyReplicationSlot;

	/*
	 * Check if the primary server has caught up. Refer to the comment atop
	 * the file for details on this check.
	 */
	if (remote_slot->restart_lsn < slot->data.restart_lsn ||
		TransactionIdPrecedes(remote_slot->catalog_xmin,
							  slot->data.catalog_xmin))
	{
		/*
		 * The remote slot didn't catch up to locally reserved position.
		 *
		 * We do not drop the slot because the restart_lsn can be ahead of the
		 * current location when recreating the slot in the next cycle. It may
		 * take more time to create such a slot. Therefore, we keep this slot
		 * and attempt the synchronization in the next cycle.
		 *
		 * XXX should this be changed to elog(DEBUG1) perhaps?
		 */
		ereport(LOG,
				errmsg("could not sync slot information as remote slot precedes local slot:"
					   " remote slot \"%s\": LSN (%X/%X), catalog xmin (%u) local slot: LSN (%X/%X), catalog xmin (%u)",
					   remote_slot->name,
					   LSN_FORMAT_ARGS(remote_slot->restart_lsn),
					   remote_slot->catalog_xmin,
					   LSN_FORMAT_ARGS(slot->data.restart_lsn),
					   slot->data.catalog_xmin));

		return;
	}

	/* First time slot update, the function must return true */
	if (!update_local_synced_slot(remote_slot, remote_dbid))
		elog(ERROR, "failed to update slot");

	ReplicationSlotPersist();

	ereport(LOG,
			errmsg("newly created slot \"%s\" is sync-ready now",
				   remote_slot->name));
}

/*
 * Synchronize a single slot to the given position.
 *
 * This creates a new slot if there is no existing one and updates the
 * metadata of the slot as per the data received from the primary server.
 *
 * The slot is created as a temporary slot and stays in the same state until the
 * the remote_slot catches up with locally reserved position and local slot is
 * updated. The slot is then persisted and is considered as sync-ready for
 * periodic syncs.
 */
static void
synchronize_one_slot(RemoteSlot *remote_slot, Oid remote_dbid)
{
	ReplicationSlot *slot;
	XLogRecPtr	latestFlushPtr;

	/*
	 * Make sure that concerned WAL is received and flushed before syncing
	 * slot to target lsn received from the primary server.
	 */
	latestFlushPtr = GetStandbyFlushRecPtr(NULL);
	if (remote_slot->confirmed_lsn > latestFlushPtr)
		elog(ERROR,
			 "skipping slot synchronization as the received slot sync"
			 " LSN %X/%X for slot \"%s\" is ahead of the standby position %X/%X",
			 LSN_FORMAT_ARGS(remote_slot->confirmed_lsn),
			 remote_slot->name,
			 LSN_FORMAT_ARGS(latestFlushPtr));

	/* Search for the named slot */
	if ((slot = SearchNamedReplicationSlot(remote_slot->name, true)))
	{
		bool		synced;

		SpinLockAcquire(&slot->mutex);
		synced = slot->data.synced;
		SpinLockRelease(&slot->mutex);

		/* User-created slot with the same name exists, raise ERROR. */
		if (!synced)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("exiting from slot synchronization because same"
						   " name slot \"%s\" already exists on the standby",
						   remote_slot->name));

		/*
		 * The slot has been synchronized before.
		 *
		 * It is important to acquire the slot here before checking
		 * invalidation. If we don't acquire the slot first, there could be a
		 * race condition that the local slot could be invalidated just after
		 * checking the 'invalidated' flag here and we could end up
		 * overwriting 'invalidated' flag to remote_slot's value. See
		 * InvalidatePossiblyObsoleteSlot() where it invalidates slot directly
		 * if the slot is not acquired by other processes.
		 */
		ReplicationSlotAcquire(remote_slot->name, true);

		Assert(slot == MyReplicationSlot);

		/*
		 * Copy the invalidation cause from remote only if local slot is not
		 * invalidated locally, we don't want to overwrite existing one.
		 */
		if (slot->data.invalidated == RS_INVAL_NONE &&
			remote_slot->invalidated != RS_INVAL_NONE)
		{
			SpinLockAcquire(&slot->mutex);
			slot->data.invalidated = remote_slot->invalidated;
			SpinLockRelease(&slot->mutex);

			/* Make sure the invalidated state persists across server restart */
			ReplicationSlotMarkDirty();
			ReplicationSlotSave();
		}

		/* Skip the sync of an invalidated slot */
		if (slot->data.invalidated != RS_INVAL_NONE)
		{
			ReplicationSlotRelease();
			return;
		}

		/* Slot not ready yet, let's attempt to make it sync-ready now. */
		if (slot->data.persistency == RS_TEMPORARY)
		{
			update_and_persist_local_synced_slot(remote_slot, remote_dbid);
		}

		/* Slot ready for sync, so sync it. */
		else
		{
			/*
			 * Sanity check: As long as the invalidations are handled
			 * appropriately as above, this should never happen.
			 */
			if (remote_slot->restart_lsn < slot->data.restart_lsn)
				elog(ERROR,
					 "cannot synchronize local slot \"%s\" LSN(%X/%X)"
					 " to remote slot's LSN(%X/%X) as synchronization"
					 " would move it backwards", remote_slot->name,
					 LSN_FORMAT_ARGS(slot->data.restart_lsn),
					 LSN_FORMAT_ARGS(remote_slot->restart_lsn));

			/* Make sure the slot changes persist across server restart */
			if (update_local_synced_slot(remote_slot, remote_dbid))
			{
				ReplicationSlotMarkDirty();
				ReplicationSlotSave();
			}
		}
	}
	/* Otherwise create the slot first. */
	else
	{
		NameData	plugin_name;
		TransactionId xmin_horizon = InvalidTransactionId;

		/* Skip creating the local slot if remote_slot is invalidated already */
		if (remote_slot->invalidated != RS_INVAL_NONE)
			return;

		/*
		 * We create temporary slots instead of ephemeral slots here because
		 * we want the slots to survive after releasing them. This is done to
		 * avoid dropping and re-creating the slots in each synchronization
		 * cycle if the restart_lsn or catalog_xmin of the remote slot has not
		 * caught up.
		 */
		ReplicationSlotCreate(remote_slot->name, true, RS_TEMPORARY,
							  remote_slot->two_phase,
							  remote_slot->failover,
							  true);

		/* For shorter lines. */
		slot = MyReplicationSlot;

		/* Avoid expensive operations while holding a spinlock. */
		namestrcpy(&plugin_name, remote_slot->plugin);

		SpinLockAcquire(&slot->mutex);
		slot->data.database = remote_dbid;
		slot->data.plugin = plugin_name;
		SpinLockRelease(&slot->mutex);

		reserve_wal_for_local_slot(remote_slot->restart_lsn);

		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		xmin_horizon = GetOldestSafeDecodingTransactionId(true);
		SpinLockAcquire(&slot->mutex);
		slot->effective_catalog_xmin = xmin_horizon;
		slot->data.catalog_xmin = xmin_horizon;
		SpinLockRelease(&slot->mutex);
		ReplicationSlotsComputeRequiredXmin(true);
		LWLockRelease(ProcArrayLock);

		update_and_persist_local_synced_slot(remote_slot, remote_dbid);
	}

	ReplicationSlotRelease();
}

/*
 * Synchronize slots.
 *
 * Gets the failover logical slots info from the primary server and updates
 * the slots locally. Creates the slots if not present on the standby.
 */
static void
synchronize_slots(WalReceiverConn *wrconn)
{
#define SLOTSYNC_COLUMN_COUNT 9
	Oid			slotRow[SLOTSYNC_COLUMN_COUNT] = {TEXTOID, TEXTOID, LSNOID,
	LSNOID, XIDOID, BOOLOID, BOOLOID, TEXTOID, TEXTOID};

	WalRcvExecResult *res;
	TupleTableSlot *tupslot;
	StringInfoData s;
	List	   *remote_slot_list = NIL;

	SpinLockAcquire(&SlotSyncCtx->mutex);
	if (SlotSyncCtx->syncing)
	{
		SpinLockRelease(&SlotSyncCtx->mutex);
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot synchronize replication slots concurrently"));
	}

	SlotSyncCtx->syncing = true;
	SpinLockRelease(&SlotSyncCtx->mutex);

	syncing_slots = true;

	initStringInfo(&s);

	/* Construct query to fetch slots with failover enabled. */
	appendStringInfo(&s,
					 "SELECT slot_name, plugin, confirmed_flush_lsn,"
					 " restart_lsn, catalog_xmin, two_phase, failover,"
					 " database, conflict_reason"
					 " FROM pg_catalog.pg_replication_slots"
					 " WHERE failover and NOT temporary");

	/* Execute the query */
	res = walrcv_exec(wrconn, s.data, SLOTSYNC_COLUMN_COUNT, slotRow);
	pfree(s.data);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				errmsg("could not fetch failover logical slots info from the primary server: %s",
					   res->err));

	/* Construct the remote_slot tuple and synchronize each slot locally */
	tupslot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, tupslot))
	{
		bool		isnull;
		RemoteSlot *remote_slot = palloc0(sizeof(RemoteSlot));
		Datum		d;
		int			col = 0;

		remote_slot->name = TextDatumGetCString(slot_getattr(tupslot, ++col,
															 &isnull));
		Assert(!isnull);

		remote_slot->plugin = TextDatumGetCString(slot_getattr(tupslot, ++col,
															   &isnull));
		Assert(!isnull);

		/*
		 * It is possible to get null values for LSN and Xmin if slot is
		 * invalidated on the primary server, so handle accordingly.
		 */
		d = slot_getattr(tupslot, ++col, &isnull);
		remote_slot->confirmed_lsn = isnull ? InvalidXLogRecPtr :
			DatumGetLSN(d);

		d = slot_getattr(tupslot, ++col, &isnull);
		remote_slot->restart_lsn = isnull ? InvalidXLogRecPtr : DatumGetLSN(d);

		d = slot_getattr(tupslot, ++col, &isnull);
		remote_slot->catalog_xmin = isnull ? InvalidTransactionId :
			DatumGetTransactionId(d);

		remote_slot->two_phase = DatumGetBool(slot_getattr(tupslot, ++col,
														   &isnull));
		Assert(!isnull);

		remote_slot->failover = DatumGetBool(slot_getattr(tupslot, ++col,
														  &isnull));
		Assert(!isnull);

		remote_slot->database = TextDatumGetCString(slot_getattr(tupslot,
																 ++col, &isnull));
		Assert(!isnull);

		d = slot_getattr(tupslot, ++col, &isnull);
		remote_slot->invalidated = isnull ? RS_INVAL_NONE :
			GetSlotInvalidationCause(TextDatumGetCString(d));

		/* Sanity check */
		Assert(col == SLOTSYNC_COLUMN_COUNT);

		/*
		 * If restart_lsn, confirmed_lsn or catalog_xmin is invalid but the
		 * slot is valid, that means we have fetched the remote_slot in its
		 * RS_EPHEMERAL state. In such a case, don't sync it; we can always
		 * sync it in the next sync cycle when the remote_slot is persisted
		 * and has valid lsn(s) and xmin values.
		 *
		 * XXX: In future, if we plan to expose 'slot->data.persistency' in
		 * pg_replication_slots view, then we can avoid fetching RS_EPHEMERAL
		 * slots in the first place.
		 */
		if ((XLogRecPtrIsInvalid(remote_slot->restart_lsn) ||
			 XLogRecPtrIsInvalid(remote_slot->confirmed_lsn) ||
			 !TransactionIdIsValid(remote_slot->catalog_xmin)) &&
			remote_slot->invalidated == RS_INVAL_NONE)
			pfree(remote_slot);
		else
			/* Create list of remote slots */
			remote_slot_list = lappend(remote_slot_list, remote_slot);

		ExecClearTuple(tupslot);
	}

	/* Drop local slots that no longer need to be synced. */
	drop_local_obsolete_slots(remote_slot_list);

	/* Now sync the slots locally */
	foreach_ptr(RemoteSlot, remote_slot, remote_slot_list)
	{
		Oid			remote_dbid = get_database_oid(remote_slot->database, false);

		/*
		 * Use shared lock to prevent a conflict with
		 * ReplicationSlotsDropDBSlots(), trying to drop the same slot during
		 * a drop-database operation.
		 */
		LockSharedObject(DatabaseRelationId, remote_dbid, 0, AccessShareLock);

		synchronize_one_slot(remote_slot, remote_dbid);

		UnlockSharedObject(DatabaseRelationId, remote_dbid, 0, AccessShareLock);
	}

	/* We are done, free remote_slot_list elements */
	list_free_deep(remote_slot_list);

	walrcv_clear_result(res);

	SpinLockAcquire(&SlotSyncCtx->mutex);
	SlotSyncCtx->syncing = false;
	SpinLockRelease(&SlotSyncCtx->mutex);

	syncing_slots = false;
}

/*
 * Checks the remote server info.
 *
 * We ensure that the 'primary_slot_name' exists on the remote server and the
 * remote server is not a standby node.
 */
static void
validate_remote_info(WalReceiverConn *wrconn)
{
#define PRIMARY_INFO_OUTPUT_COL_COUNT 2
	WalRcvExecResult *res;
	Oid			slotRow[PRIMARY_INFO_OUTPUT_COL_COUNT] = {BOOLOID, BOOLOID};
	StringInfoData cmd;
	bool		isnull;
	TupleTableSlot *tupslot;
	bool		remote_in_recovery;
	bool		primary_slot_valid;

	initStringInfo(&cmd);
	appendStringInfo(&cmd,
					 "SELECT pg_is_in_recovery(), count(*) = 1"
					 " FROM pg_catalog.pg_replication_slots"
					 " WHERE slot_type='physical' AND slot_name=%s",
					 quote_literal_cstr(PrimarySlotName));

	res = walrcv_exec(wrconn, cmd.data, PRIMARY_INFO_OUTPUT_COL_COUNT, slotRow);
	pfree(cmd.data);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				errmsg("could not fetch primary_slot_name \"%s\" info from the primary server: %s",
					   PrimarySlotName, res->err),
				errhint("Check if \"primary_slot_name\" is configured correctly."));

	tupslot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	if (!tuplestore_gettupleslot(res->tuplestore, true, false, tupslot))
		elog(ERROR,
			 "failed to fetch tuple for the primary server slot specified by \"primary_slot_name\"");

	remote_in_recovery = DatumGetBool(slot_getattr(tupslot, 1, &isnull));
	Assert(!isnull);

	if (remote_in_recovery)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot synchronize replication slots from a standby server"));

	primary_slot_valid = DatumGetBool(slot_getattr(tupslot, 2, &isnull));
	Assert(!isnull);

	if (!primary_slot_valid)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("bad configuration for slot synchronization"),
		/* translator: second %s is a GUC variable name */
				errdetail("The replication slot \"%s\" specified by \"%s\" does not exist on the primary server.",
						  PrimarySlotName, "primary_slot_name"));

	ExecClearTuple(tupslot);
	walrcv_clear_result(res);
}

/*
 * Check all necessary GUCs for slot synchronization are set
 * appropriately, otherwise, raise ERROR.
 */
void
ValidateSlotSyncParams(void)
{
	char	   *dbname;

	/*
	 * A physical replication slot(primary_slot_name) is required on the
	 * primary to ensure that the rows needed by the standby are not removed
	 * after restarting, so that the synchronized slot on the standby will not
	 * be invalidated.
	 */
	if (PrimarySlotName == NULL || *PrimarySlotName == '\0')
		ereport(ERROR,
		/* translator: %s is a GUC variable name */
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("bad configuration for slot synchronization"),
				errhint("\"%s\" must be defined.", "primary_slot_name"));

	/*
	 * hot_standby_feedback must be enabled to cooperate with the physical
	 * replication slot, which allows informing the primary about the xmin and
	 * catalog_xmin values on the standby.
	 */
	if (!hot_standby_feedback)
		ereport(ERROR,
		/* translator: %s is a GUC variable name */
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("bad configuration for slot synchronization"),
				errhint("\"%s\" must be enabled.", "hot_standby_feedback"));

	/* Logical slot sync/creation requires wal_level >= logical. */
	if (wal_level < WAL_LEVEL_LOGICAL)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("bad configuration for slot synchronization"),
				errhint("\"wal_level\" must be >= logical."));

	/*
	 * The primary_conninfo is required to make connection to primary for
	 * getting slots information.
	 */
	if (PrimaryConnInfo == NULL || *PrimaryConnInfo == '\0')
		ereport(ERROR,
		/* translator: %s is a GUC variable name */
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("bad configuration for slot synchronization"),
				errhint("\"%s\" must be defined.", "primary_conninfo"));

	/*
	 * The slot synchronization needs a database connection for walrcv_exec to
	 * work.
	 */
	dbname = walrcv_get_dbname_from_conninfo(PrimaryConnInfo);
	if (dbname == NULL)
		ereport(ERROR,

		/*
		 * translator: 'dbname' is a specific option; %s is a GUC variable
		 * name
		 */
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("bad configuration for slot synchronization"),
				errhint("'dbname' must be specified in \"%s\".", "primary_conninfo"));
}

/*
 * Is current process syncing replication slots ?
 */
bool
IsSyncingReplicationSlots(void)
{
	return syncing_slots;
}

/*
 * Amount of shared memory required for slot synchronization.
 */
Size
SlotSyncShmemSize(void)
{
	return sizeof(SlotSyncCtxStruct);
}

/*
 * Allocate and initialize the shared memory of slot synchronization.
 */
void
SlotSyncShmemInit(void)
{
	bool		found;

	SlotSyncCtx = (SlotSyncCtxStruct *)
		ShmemInitStruct("Slot Sync Data", SlotSyncShmemSize(), &found);

	if (!found)
	{
		SlotSyncCtx->syncing = false;
		SpinLockInit(&SlotSyncCtx->mutex);
	}
}

/*
 * Error cleanup callback for slot synchronization.
 */
static void
slotsync_failure_callback(int code, Datum arg)
{
	WalReceiverConn *wrconn = (WalReceiverConn *) DatumGetPointer(arg);

	if (syncing_slots)
	{
		/*
		 * If syncing_slots is true, it indicates that the process errored out
		 * without resetting the flag. So, we need to clean up shared memory
		 * and reset the flag here.
		 */
		SpinLockAcquire(&SlotSyncCtx->mutex);
		SlotSyncCtx->syncing = false;
		SpinLockRelease(&SlotSyncCtx->mutex);

		syncing_slots = false;
	}

	walrcv_disconnect(wrconn);
}

/*
 * Synchronize the failover enabled replication slots using the specified
 * primary server connection.
 */
void
SyncReplicationSlots(WalReceiverConn *wrconn)
{
	PG_ENSURE_ERROR_CLEANUP(slotsync_failure_callback, PointerGetDatum(wrconn));
	{
		validate_remote_info(wrconn);

		synchronize_slots(wrconn);
	}
	PG_END_ENSURE_ERROR_CLEANUP(slotsync_failure_callback, PointerGetDatum(wrconn));
}
