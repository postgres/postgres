/*-------------------------------------------------------------------------
 * slotsync.c
 *	   Functionality for synchronizing slots to a standby server from the
 *         primary server.
 *
 * Copyright (c) 2024-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/slotsync.c
 *
 * This file contains the code for slot synchronization on a physical standby
 * to fetch logical failover slots information from the primary server, create
 * the slots on the standby and synchronize them periodically.
 *
 * Slot synchronization can be performed either automatically by enabling slot
 * sync worker or manually by calling SQL function pg_sync_replication_slots().
 *
 * If the WAL corresponding to the remote's restart_lsn is not available on the
 * physical standby or the remote's catalog_xmin precedes the oldest xid for
 * which it is guaranteed that rows wouldn't have been removed then we cannot
 * create the local standby slot because that would mean moving the local slot
 * backward and decoding won't be possible via such a slot. In this case, the
 * slot will be marked as RS_TEMPORARY. Once the primary server catches up,
 * the slot will be marked as RS_PERSISTENT (which means sync-ready) after
 * which slot sync worker can perform the sync periodically or user can call
 * pg_sync_replication_slots() periodically to perform the syncs.
 *
 * If synchronized slots fail to build a consistent snapshot from the
 * restart_lsn before reaching confirmed_flush_lsn, they would become
 * unreliable after promotion due to potential data loss from changes
 * before reaching a consistent point. This can happen because the slots can
 * be synced at some random time and we may not reach the consistent point
 * at the same WAL location as the primary. So, we mark such slots as
 * RS_TEMPORARY. Once the decoding from corresponding LSNs can reach a
 * consistent point, they will be marked as RS_PERSISTENT.
 *
 * The slot sync worker waits for some time before the next synchronization,
 * with the duration varying based on whether any slots were updated during
 * the last cycle. Refer to the comments above wait_for_slot_activity() for
 * more details.
 *
 * Any standby synchronized slots will be dropped if they no longer need
 * to be synchronized. See comment atop drop_local_obsolete_slots() for more
 * details.
 *---------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>

#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "libpq/pqsignal.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/logical.h"
#include "replication/slotsync.h"
#include "replication/snapbuild.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"

/*
 * Struct for sharing information to control slot synchronization.
 *
 * The slot sync worker's pid is needed by the startup process to shut it
 * down during promotion. The startup process shuts down the slot sync worker
 * and also sets stopSignaled=true to handle the race condition when the
 * postmaster has not noticed the promotion yet and thus may end up restarting
 * the slot sync worker. If stopSignaled is set, the worker will exit in such a
 * case. The SQL function pg_sync_replication_slots() will also error out if
 * this flag is set. Note that we don't need to reset this variable as after
 * promotion the slot sync worker won't be restarted because the pmState
 * changes to PM_RUN from PM_HOT_STANDBY and we don't support demoting
 * primary without restarting the server. See LaunchMissingBackgroundProcesses.
 *
 * The 'syncing' flag is needed to prevent concurrent slot syncs to avoid slot
 * overwrites.
 *
 * The 'last_start_time' is needed by postmaster to start the slot sync worker
 * once per SLOTSYNC_RESTART_INTERVAL_SEC. In cases where an immediate restart
 * is expected (e.g., slot sync GUCs change), slot sync worker will reset
 * last_start_time before exiting, so that postmaster can start the worker
 * without waiting for SLOTSYNC_RESTART_INTERVAL_SEC.
 */
typedef struct SlotSyncCtxStruct
{
	pid_t		pid;
	bool		stopSignaled;
	bool		syncing;
	time_t		last_start_time;
	slock_t		mutex;
} SlotSyncCtxStruct;

static SlotSyncCtxStruct *SlotSyncCtx = NULL;

/* GUC variable */
bool		sync_replication_slots = false;

/*
 * The sleep time (ms) between slot-sync cycles varies dynamically
 * (within a MIN/MAX range) according to slot activity. See
 * wait_for_slot_activity() for details.
 */
#define MIN_SLOTSYNC_WORKER_NAPTIME_MS  200
#define MAX_SLOTSYNC_WORKER_NAPTIME_MS  30000	/* 30s */

static long sleep_ms = MIN_SLOTSYNC_WORKER_NAPTIME_MS;

/* The restart interval for slot sync work used by postmaster */
#define SLOTSYNC_RESTART_INTERVAL_SEC 10

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
	XLogRecPtr	two_phase_at;
	TransactionId catalog_xmin;

	/* RS_INVAL_NONE if valid, or the reason of invalidation */
	ReplicationSlotInvalidationCause invalidated;
} RemoteSlot;

static void slotsync_failure_callback(int code, Datum arg);
static void update_synced_slots_inactive_since(void);

/*
 * If necessary, update the local synced slot's metadata based on the data
 * from the remote slot.
 *
 * If no update was needed (the data of the remote slot is the same as the
 * local slot) return false, otherwise true.
 *
 * *found_consistent_snapshot will be true iff the remote slot's LSN or xmin is
 * modified, and decoding from the corresponding LSN's can reach a
 * consistent snapshot.
 *
 * *remote_slot_precedes will be true if the remote slot's LSN or xmin
 * precedes locally reserved position.
 */
static bool
update_local_synced_slot(RemoteSlot *remote_slot, Oid remote_dbid,
						 bool *found_consistent_snapshot,
						 bool *remote_slot_precedes)
{
	ReplicationSlot *slot = MyReplicationSlot;
	bool		updated_xmin_or_lsn = false;
	bool		updated_config = false;

	Assert(slot->data.invalidated == RS_INVAL_NONE);

	if (found_consistent_snapshot)
		*found_consistent_snapshot = false;

	if (remote_slot_precedes)
		*remote_slot_precedes = false;

	/*
	 * Don't overwrite if we already have a newer catalog_xmin and
	 * restart_lsn.
	 */
	if (remote_slot->restart_lsn < slot->data.restart_lsn ||
		TransactionIdPrecedes(remote_slot->catalog_xmin,
							  slot->data.catalog_xmin))
	{
		/*
		 * This can happen in following situations:
		 *
		 * If the slot is temporary, it means either the initial WAL location
		 * reserved for the local slot is ahead of the remote slot's
		 * restart_lsn or the initial xmin_horizon computed for the local slot
		 * is ahead of the remote slot.
		 *
		 * If the slot is persistent, both restart_lsn and catalog_xmin of the
		 * synced slot could still be ahead of the remote slot. Since we use
		 * slot advance functionality to keep snapbuild/slot updated, it is
		 * possible that the restart_lsn and catalog_xmin are advanced to a
		 * later position than it has on the primary. This can happen when
		 * slot advancing machinery finds running xacts record after reaching
		 * the consistent state at a later point than the primary where it
		 * serializes the snapshot and updates the restart_lsn.
		 *
		 * We LOG the message if the slot is temporary as it can help the user
		 * to understand why the slot is not sync-ready. In the case of a
		 * persistent slot, it would be a more common case and won't directly
		 * impact the users, so we used DEBUG1 level to log the message.
		 */
		ereport(slot->data.persistency == RS_TEMPORARY ? LOG : DEBUG1,
				errmsg("could not synchronize replication slot \"%s\"",
					   remote_slot->name),
				errdetail("Synchronization could lead to data loss, because the remote slot needs WAL at LSN %X/%08X and catalog xmin %u, but the standby has LSN %X/%08X and catalog xmin %u.",
						  LSN_FORMAT_ARGS(remote_slot->restart_lsn),
						  remote_slot->catalog_xmin,
						  LSN_FORMAT_ARGS(slot->data.restart_lsn),
						  slot->data.catalog_xmin));

		if (remote_slot_precedes)
			*remote_slot_precedes = true;

		/*
		 * Skip updating the configuration. This is required to avoid syncing
		 * two_phase_at without syncing confirmed_lsn. Otherwise, the prepared
		 * transaction between old confirmed_lsn and two_phase_at will
		 * unexpectedly get decoded and sent to the downstream after
		 * promotion. See comments in ReorderBufferFinishPrepared.
		 */
		return false;
	}

	/*
	 * Attempt to sync LSNs and xmins only if remote slot is ahead of local
	 * slot.
	 */
	if (remote_slot->confirmed_lsn > slot->data.confirmed_flush ||
		remote_slot->restart_lsn > slot->data.restart_lsn ||
		TransactionIdFollows(remote_slot->catalog_xmin,
							 slot->data.catalog_xmin))
	{
		/*
		 * We can't directly copy the remote slot's LSN or xmin unless there
		 * exists a consistent snapshot at that point. Otherwise, after
		 * promotion, the slots may not reach a consistent point before the
		 * confirmed_flush_lsn which can lead to a data loss. To avoid data
		 * loss, we let slot machinery advance the slot which ensures that
		 * snapbuilder/slot statuses are updated properly.
		 */
		if (SnapBuildSnapshotExists(remote_slot->restart_lsn))
		{
			/*
			 * Update the slot info directly if there is a serialized snapshot
			 * at the restart_lsn, as the slot can quickly reach consistency
			 * at restart_lsn by restoring the snapshot.
			 */
			SpinLockAcquire(&slot->mutex);
			slot->data.restart_lsn = remote_slot->restart_lsn;
			slot->data.confirmed_flush = remote_slot->confirmed_lsn;
			slot->data.catalog_xmin = remote_slot->catalog_xmin;
			SpinLockRelease(&slot->mutex);

			if (found_consistent_snapshot)
				*found_consistent_snapshot = true;
		}
		else
		{
			LogicalSlotAdvanceAndCheckSnapState(remote_slot->confirmed_lsn,
												found_consistent_snapshot);

			/* Sanity check */
			if (slot->data.confirmed_flush != remote_slot->confirmed_lsn)
				ereport(ERROR,
						errmsg_internal("synchronized confirmed_flush for slot \"%s\" differs from remote slot",
										remote_slot->name),
						errdetail_internal("Remote slot has LSN %X/%08X but local slot has LSN %X/%08X.",
										   LSN_FORMAT_ARGS(remote_slot->confirmed_lsn),
										   LSN_FORMAT_ARGS(slot->data.confirmed_flush)));
		}

		updated_xmin_or_lsn = true;
	}

	if (remote_dbid != slot->data.database ||
		remote_slot->two_phase != slot->data.two_phase ||
		remote_slot->failover != slot->data.failover ||
		strcmp(remote_slot->plugin, NameStr(slot->data.plugin)) != 0 ||
		remote_slot->two_phase_at != slot->data.two_phase_at)
	{
		NameData	plugin_name;

		/* Avoid expensive operations while holding a spinlock. */
		namestrcpy(&plugin_name, remote_slot->plugin);

		SpinLockAcquire(&slot->mutex);
		slot->data.plugin = plugin_name;
		slot->data.database = remote_dbid;
		slot->data.two_phase = remote_slot->two_phase;
		slot->data.two_phase_at = remote_slot->two_phase_at;
		slot->data.failover = remote_slot->failover;
		SpinLockRelease(&slot->mutex);

		updated_config = true;

		/*
		 * Ensure that there is no risk of sending prepared transactions
		 * unexpectedly after the promotion.
		 */
		Assert(slot->data.two_phase_at <= slot->data.confirmed_flush);
	}

	/*
	 * We have to write the changed xmin to disk *before* we change the
	 * in-memory value, otherwise after a crash we wouldn't know that some
	 * catalog tuples might have been removed already.
	 */
	if (updated_config || updated_xmin_or_lsn)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
	}

	/*
	 * Now the new xmin is safely on disk, we can let the global value
	 * advance. We do not take ProcArrayLock or similar since we only advance
	 * xmin here and there's not much harm done by a concurrent computation
	 * missing that.
	 */
	if (updated_xmin_or_lsn)
	{
		SpinLockAcquire(&slot->mutex);
		slot->effective_catalog_xmin = remote_slot->catalog_xmin;
		SpinLockRelease(&slot->mutex);

		ReplicationSlotsComputeRequiredXmin(false);
		ReplicationSlotsComputeRequiredLSN();
	}

	return updated_config || updated_xmin_or_lsn;
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
				ReplicationSlotAcquire(NameStr(local_slot->data.name), true, false);
				ReplicationSlotDropAcquired();
			}

			UnlockSharedObject(DatabaseRelationId, local_slot->data.database,
							   0, AccessShareLock);

			ereport(LOG,
					errmsg("dropped replication slot \"%s\" of database with OID %u",
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

		elog(DEBUG1, "segno: " UINT64_FORMAT " of purposed restart_lsn for the synced slot, oldest_segno: " UINT64_FORMAT " available",
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
 *
 * Return true if the slot is marked as RS_PERSISTENT (sync-ready), otherwise
 * false.
 */
static bool
update_and_persist_local_synced_slot(RemoteSlot *remote_slot, Oid remote_dbid)
{
	ReplicationSlot *slot = MyReplicationSlot;
	bool		found_consistent_snapshot = false;
	bool		remote_slot_precedes = false;

	(void) update_local_synced_slot(remote_slot, remote_dbid,
									&found_consistent_snapshot,
									&remote_slot_precedes);

	/*
	 * Check if the primary server has caught up. Refer to the comment atop
	 * the file for details on this check.
	 */
	if (remote_slot_precedes)
	{
		/*
		 * The remote slot didn't catch up to locally reserved position.
		 *
		 * We do not drop the slot because the restart_lsn can be ahead of the
		 * current location when recreating the slot in the next cycle. It may
		 * take more time to create such a slot. Therefore, we keep this slot
		 * and attempt the synchronization in the next cycle.
		 */
		return false;
	}

	/*
	 * Don't persist the slot if it cannot reach the consistent point from the
	 * restart_lsn. See comments atop this file.
	 */
	if (!found_consistent_snapshot)
	{
		ereport(LOG,
				errmsg("could not synchronize replication slot \"%s\"", remote_slot->name),
				errdetail("Synchronization could lead to data loss, because the standby could not build a consistent snapshot to decode WALs at LSN %X/%08X.",
						  LSN_FORMAT_ARGS(slot->data.restart_lsn)));

		return false;
	}

	ReplicationSlotPersist();

	ereport(LOG,
			errmsg("newly created replication slot \"%s\" is sync-ready now",
				   remote_slot->name));

	return true;
}

/*
 * Synchronize a single slot to the given position.
 *
 * This creates a new slot if there is no existing one and updates the
 * metadata of the slot as per the data received from the primary server.
 *
 * The slot is created as a temporary slot and stays in the same state until the
 * remote_slot catches up with locally reserved position and local slot is
 * updated. The slot is then persisted and is considered as sync-ready for
 * periodic syncs.
 *
 * Returns TRUE if the local slot is updated.
 */
static bool
synchronize_one_slot(RemoteSlot *remote_slot, Oid remote_dbid)
{
	ReplicationSlot *slot;
	XLogRecPtr	latestFlushPtr;
	bool		slot_updated = false;

	/*
	 * Make sure that concerned WAL is received and flushed before syncing
	 * slot to target lsn received from the primary server.
	 */
	latestFlushPtr = GetStandbyFlushRecPtr(NULL);
	if (remote_slot->confirmed_lsn > latestFlushPtr)
	{
		/*
		 * Can get here only if GUC 'synchronized_standby_slots' on the
		 * primary server was not configured correctly.
		 */
		ereport(AmLogicalSlotSyncWorkerProcess() ? LOG : ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("skipping slot synchronization because the received slot sync"
					   " LSN %X/%08X for slot \"%s\" is ahead of the standby position %X/%08X",
					   LSN_FORMAT_ARGS(remote_slot->confirmed_lsn),
					   remote_slot->name,
					   LSN_FORMAT_ARGS(latestFlushPtr)));

		return false;
	}

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
		 *
		 * XXX: If it ever turns out that slot acquire/release is costly for
		 * cases when none of the slot properties is changed then we can do a
		 * pre-check to ensure that at least one of the slot properties is
		 * changed before acquiring the slot.
		 */
		ReplicationSlotAcquire(remote_slot->name, true, false);

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

			slot_updated = true;
		}

		/* Skip the sync of an invalidated slot */
		if (slot->data.invalidated != RS_INVAL_NONE)
		{
			ReplicationSlotRelease();
			return slot_updated;
		}

		/* Slot not ready yet, let's attempt to make it sync-ready now. */
		if (slot->data.persistency == RS_TEMPORARY)
		{
			slot_updated = update_and_persist_local_synced_slot(remote_slot,
																remote_dbid);
		}

		/* Slot ready for sync, so sync it. */
		else
		{
			/*
			 * Sanity check: As long as the invalidations are handled
			 * appropriately as above, this should never happen.
			 *
			 * We don't need to check restart_lsn here. See the comments in
			 * update_local_synced_slot() for details.
			 */
			if (remote_slot->confirmed_lsn < slot->data.confirmed_flush)
				ereport(ERROR,
						errmsg_internal("cannot synchronize local slot \"%s\"",
										remote_slot->name),
						errdetail_internal("Local slot's start streaming location LSN(%X/%08X) is ahead of remote slot's LSN(%X/%08X).",
										   LSN_FORMAT_ARGS(slot->data.confirmed_flush),
										   LSN_FORMAT_ARGS(remote_slot->confirmed_lsn)));

			slot_updated = update_local_synced_slot(remote_slot, remote_dbid,
													NULL, NULL);
		}
	}
	/* Otherwise create the slot first. */
	else
	{
		NameData	plugin_name;
		TransactionId xmin_horizon = InvalidTransactionId;

		/* Skip creating the local slot if remote_slot is invalidated already */
		if (remote_slot->invalidated != RS_INVAL_NONE)
			return false;

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

		slot_updated = true;
	}

	ReplicationSlotRelease();

	return slot_updated;
}

/*
 * Synchronize slots.
 *
 * Gets the failover logical slots info from the primary server and updates
 * the slots locally. Creates the slots if not present on the standby.
 *
 * Returns TRUE if any of the slots gets updated in this sync-cycle.
 */
static bool
synchronize_slots(WalReceiverConn *wrconn)
{
#define SLOTSYNC_COLUMN_COUNT 10
	Oid			slotRow[SLOTSYNC_COLUMN_COUNT] = {TEXTOID, TEXTOID, LSNOID,
	LSNOID, XIDOID, BOOLOID, LSNOID, BOOLOID, TEXTOID, TEXTOID};

	WalRcvExecResult *res;
	TupleTableSlot *tupslot;
	List	   *remote_slot_list = NIL;
	bool		some_slot_updated = false;
	bool		started_tx = false;
	const char *query = "SELECT slot_name, plugin, confirmed_flush_lsn,"
		" restart_lsn, catalog_xmin, two_phase, two_phase_at, failover,"
		" database, invalidation_reason"
		" FROM pg_catalog.pg_replication_slots"
		" WHERE failover and NOT temporary";

	/* The syscache access in walrcv_exec() needs a transaction env. */
	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	/* Execute the query */
	res = walrcv_exec(wrconn, query, SLOTSYNC_COLUMN_COUNT, slotRow);
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

		d = slot_getattr(tupslot, ++col, &isnull);
		remote_slot->two_phase_at = isnull ? InvalidXLogRecPtr : DatumGetLSN(d);

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

		some_slot_updated |= synchronize_one_slot(remote_slot, remote_dbid);

		UnlockSharedObject(DatabaseRelationId, remote_dbid, 0, AccessShareLock);
	}

	/* We are done, free remote_slot_list elements */
	list_free_deep(remote_slot_list);

	walrcv_clear_result(res);

	if (started_tx)
		CommitTransactionCommand();

	return some_slot_updated;
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
	bool		started_tx = false;

	initStringInfo(&cmd);
	appendStringInfo(&cmd,
					 "SELECT pg_is_in_recovery(), count(*) = 1"
					 " FROM pg_catalog.pg_replication_slots"
					 " WHERE slot_type='physical' AND slot_name=%s",
					 quote_literal_cstr(PrimarySlotName));

	/* The syscache access in walrcv_exec() needs a transaction env. */
	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	res = walrcv_exec(wrconn, cmd.data, PRIMARY_INFO_OUTPUT_COL_COUNT, slotRow);
	pfree(cmd.data);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				errmsg("could not fetch primary slot name \"%s\" info from the primary server: %s",
					   PrimarySlotName, res->err),
				errhint("Check if \"primary_slot_name\" is configured correctly."));

	tupslot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	if (!tuplestore_gettupleslot(res->tuplestore, true, false, tupslot))
		elog(ERROR,
			 "failed to fetch tuple for the primary server slot specified by \"primary_slot_name\"");

	remote_in_recovery = DatumGetBool(slot_getattr(tupslot, 1, &isnull));
	Assert(!isnull);

	/*
	 * Slot sync is currently not supported on a cascading standby. This is
	 * because if we allow it, the primary server needs to wait for all the
	 * cascading standbys, otherwise, logical subscribers can still be ahead
	 * of one of the cascading standbys which we plan to promote. Thus, to
	 * avoid this additional complexity, we restrict it for the time being.
	 */
	if (remote_in_recovery)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot synchronize replication slots from a standby server"));

	primary_slot_valid = DatumGetBool(slot_getattr(tupslot, 2, &isnull));
	Assert(!isnull);

	if (!primary_slot_valid)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/* translator: second %s is a GUC variable name */
				errmsg("replication slot \"%s\" specified by \"%s\" does not exist on primary server",
					   PrimarySlotName, "primary_slot_name"));

	ExecClearTuple(tupslot);
	walrcv_clear_result(res);

	if (started_tx)
		CommitTransactionCommand();
}

/*
 * Checks if dbname is specified in 'primary_conninfo'.
 *
 * Error out if not specified otherwise return it.
 */
char *
CheckAndGetDbnameFromConninfo(void)
{
	char	   *dbname;

	/*
	 * The slot synchronization needs a database connection for walrcv_exec to
	 * work.
	 */
	dbname = walrcv_get_dbname_from_conninfo(PrimaryConnInfo);
	if (dbname == NULL)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),

		/*
		 * translator: first %s is a connection option; second %s is a GUC
		 * variable name
		 */
				errmsg("replication slot synchronization requires \"%s\" to be specified in \"%s\"",
					   "dbname", "primary_conninfo"));
	return dbname;
}

/*
 * Return true if all necessary GUCs for slot synchronization are set
 * appropriately, otherwise, return false.
 */
bool
ValidateSlotSyncParams(int elevel)
{
	/*
	 * Logical slot sync/creation requires wal_level >= logical.
	 *
	 * Since altering the wal_level requires a server restart, so error out in
	 * this case regardless of elevel provided by caller.
	 */
	if (wal_level < WAL_LEVEL_LOGICAL)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("replication slot synchronization requires \"wal_level\" >= \"logical\""));

	/*
	 * A physical replication slot(primary_slot_name) is required on the
	 * primary to ensure that the rows needed by the standby are not removed
	 * after restarting, so that the synchronized slot on the standby will not
	 * be invalidated.
	 */
	if (PrimarySlotName == NULL || *PrimarySlotName == '\0')
	{
		ereport(elevel,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/* translator: %s is a GUC variable name */
				errmsg("replication slot synchronization requires \"%s\" to be set", "primary_slot_name"));
		return false;
	}

	/*
	 * hot_standby_feedback must be enabled to cooperate with the physical
	 * replication slot, which allows informing the primary about the xmin and
	 * catalog_xmin values on the standby.
	 */
	if (!hot_standby_feedback)
	{
		ereport(elevel,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/* translator: %s is a GUC variable name */
				errmsg("replication slot synchronization requires \"%s\" to be enabled",
					   "hot_standby_feedback"));
		return false;
	}

	/*
	 * The primary_conninfo is required to make connection to primary for
	 * getting slots information.
	 */
	if (PrimaryConnInfo == NULL || *PrimaryConnInfo == '\0')
	{
		ereport(elevel,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/* translator: %s is a GUC variable name */
				errmsg("replication slot synchronization requires \"%s\" to be set",
					   "primary_conninfo"));
		return false;
	}

	return true;
}

/*
 * Re-read the config file.
 *
 * Exit if any of the slot sync GUCs have changed. The postmaster will
 * restart it.
 */
static void
slotsync_reread_config(void)
{
	char	   *old_primary_conninfo = pstrdup(PrimaryConnInfo);
	char	   *old_primary_slotname = pstrdup(PrimarySlotName);
	bool		old_sync_replication_slots = sync_replication_slots;
	bool		old_hot_standby_feedback = hot_standby_feedback;
	bool		conninfo_changed;
	bool		primary_slotname_changed;

	Assert(sync_replication_slots);

	ConfigReloadPending = false;
	ProcessConfigFile(PGC_SIGHUP);

	conninfo_changed = strcmp(old_primary_conninfo, PrimaryConnInfo) != 0;
	primary_slotname_changed = strcmp(old_primary_slotname, PrimarySlotName) != 0;
	pfree(old_primary_conninfo);
	pfree(old_primary_slotname);

	if (old_sync_replication_slots != sync_replication_slots)
	{
		ereport(LOG,
		/* translator: %s is a GUC variable name */
				errmsg("replication slot synchronization worker will shut down because \"%s\" is disabled", "sync_replication_slots"));
		proc_exit(0);
	}

	if (conninfo_changed ||
		primary_slotname_changed ||
		(old_hot_standby_feedback != hot_standby_feedback))
	{
		ereport(LOG,
				errmsg("replication slot synchronization worker will restart because of a parameter change"));

		/*
		 * Reset the last-start time for this worker so that the postmaster
		 * can restart it without waiting for SLOTSYNC_RESTART_INTERVAL_SEC.
		 */
		SlotSyncCtx->last_start_time = 0;

		proc_exit(0);
	}

}

/*
 * Interrupt handler for main loop of slot sync worker.
 */
static void
ProcessSlotSyncInterrupts(WalReceiverConn *wrconn)
{
	CHECK_FOR_INTERRUPTS();

	if (ShutdownRequestPending)
	{
		ereport(LOG,
				errmsg("replication slot synchronization worker is shutting down on receiving SIGINT"));

		proc_exit(0);
	}

	if (ConfigReloadPending)
		slotsync_reread_config();
}

/*
 * Connection cleanup function for slotsync worker.
 *
 * Called on slotsync worker exit.
 */
static void
slotsync_worker_disconnect(int code, Datum arg)
{
	WalReceiverConn *wrconn = (WalReceiverConn *) DatumGetPointer(arg);

	walrcv_disconnect(wrconn);
}

/*
 * Cleanup function for slotsync worker.
 *
 * Called on slotsync worker exit.
 */
static void
slotsync_worker_onexit(int code, Datum arg)
{
	/*
	 * We need to do slots cleanup here just like WalSndErrorCleanup() does.
	 *
	 * The startup process during promotion invokes ShutDownSlotSync() which
	 * waits for slot sync to finish and it does that by checking the
	 * 'syncing' flag. Thus the slot sync worker must be done with slots'
	 * release and cleanup to avoid any dangling temporary slots or active
	 * slots before it marks itself as finished syncing.
	 */

	/* Make sure active replication slots are released */
	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	/* Also cleanup the temporary slots. */
	ReplicationSlotCleanup(false);

	SpinLockAcquire(&SlotSyncCtx->mutex);

	SlotSyncCtx->pid = InvalidPid;

	/*
	 * If syncing_slots is true, it indicates that the process errored out
	 * without resetting the flag. So, we need to clean up shared memory and
	 * reset the flag here.
	 */
	if (syncing_slots)
	{
		SlotSyncCtx->syncing = false;
		syncing_slots = false;
	}

	SpinLockRelease(&SlotSyncCtx->mutex);
}

/*
 * Sleep for long enough that we believe it's likely that the slots on primary
 * get updated.
 *
 * If there is no slot activity the wait time between sync-cycles will double
 * (to a maximum of 30s). If there is some slot activity the wait time between
 * sync-cycles is reset to the minimum (200ms).
 */
static void
wait_for_slot_activity(bool some_slot_updated)
{
	int			rc;

	if (!some_slot_updated)
	{
		/*
		 * No slots were updated, so double the sleep time, but not beyond the
		 * maximum allowable value.
		 */
		sleep_ms = Min(sleep_ms * 2, MAX_SLOTSYNC_WORKER_NAPTIME_MS);
	}
	else
	{
		/*
		 * Some slots were updated since the last sleep, so reset the sleep
		 * time.
		 */
		sleep_ms = MIN_SLOTSYNC_WORKER_NAPTIME_MS;
	}

	rc = WaitLatch(MyLatch,
				   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
				   sleep_ms,
				   WAIT_EVENT_REPLICATION_SLOTSYNC_MAIN);

	if (rc & WL_LATCH_SET)
		ResetLatch(MyLatch);
}

/*
 * Emit an error if a promotion or a concurrent sync call is in progress.
 * Otherwise, advertise that a sync is in progress.
 */
static void
check_and_set_sync_info(pid_t worker_pid)
{
	SpinLockAcquire(&SlotSyncCtx->mutex);

	/* The worker pid must not be already assigned in SlotSyncCtx */
	Assert(worker_pid == InvalidPid || SlotSyncCtx->pid == InvalidPid);

	/*
	 * Emit an error if startup process signaled the slot sync machinery to
	 * stop. See comments atop SlotSyncCtxStruct.
	 */
	if (SlotSyncCtx->stopSignaled)
	{
		SpinLockRelease(&SlotSyncCtx->mutex);
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot synchronize replication slots when standby promotion is ongoing"));
	}

	if (SlotSyncCtx->syncing)
	{
		SpinLockRelease(&SlotSyncCtx->mutex);
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot synchronize replication slots concurrently"));
	}

	SlotSyncCtx->syncing = true;

	/*
	 * Advertise the required PID so that the startup process can kill the
	 * slot sync worker on promotion.
	 */
	SlotSyncCtx->pid = worker_pid;

	SpinLockRelease(&SlotSyncCtx->mutex);

	syncing_slots = true;
}

/*
 * Reset syncing flag.
 */
static void
reset_syncing_flag()
{
	SpinLockAcquire(&SlotSyncCtx->mutex);
	SlotSyncCtx->syncing = false;
	SpinLockRelease(&SlotSyncCtx->mutex);

	syncing_slots = false;
};

/*
 * The main loop of our worker process.
 *
 * It connects to the primary server, fetches logical failover slots
 * information periodically in order to create and sync the slots.
 */
void
ReplSlotSyncWorkerMain(const void *startup_data, size_t startup_data_len)
{
	WalReceiverConn *wrconn = NULL;
	char	   *dbname;
	char	   *err;
	sigjmp_buf	local_sigjmp_buf;
	StringInfoData app_name;

	Assert(startup_data_len == 0);

	MyBackendType = B_SLOTSYNC_WORKER;

	init_ps_display(NULL);

	Assert(GetProcessingMode() == InitProcessing);

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we access any shared memory.
	 */
	InitProcess();

	/*
	 * Early initialization.
	 */
	BaseInit();

	Assert(SlotSyncCtx != NULL);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * We just need to clean up, report the error, and go away.
	 *
	 * If we do not have this handling here, then since this worker process
	 * operates at the bottom of the exception stack, ERRORs turn into FATALs.
	 * Therefore, we create our own exception handler to catch ERRORs.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * We can now go away.  Note that because we called InitProcess, a
		 * callback was registered to do ProcKill, which will clean up
		 * necessary state.
		 */
		proc_exit(0);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/* Setup signal handling */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SignalHandlerForShutdownRequest);
	pqsignal(SIGTERM, die);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);

	check_and_set_sync_info(MyProcPid);

	ereport(LOG, errmsg("slot sync worker started"));

	/* Register it as soon as SlotSyncCtx->pid is initialized. */
	before_shmem_exit(slotsync_worker_onexit, (Datum) 0);

	/*
	 * Establishes SIGALRM handler and initialize timeout module. It is needed
	 * by InitPostgres to register different timeouts.
	 */
	InitializeTimeouts();

	/* Load the libpq-specific functions */
	load_file("libpqwalreceiver", false);

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Set always-secure search path, so malicious users can't redirect user
	 * code (e.g. operators).
	 *
	 * It's not strictly necessary since we won't be scanning or writing to
	 * any user table locally, but it's good to retain it here for added
	 * precaution.
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	dbname = CheckAndGetDbnameFromConninfo();

	/*
	 * Connect to the database specified by the user in primary_conninfo. We
	 * need a database connection for walrcv_exec to work which we use to
	 * fetch slot information from the remote node. See comments atop
	 * libpqrcv_exec.
	 *
	 * We do not specify a specific user here since the slot sync worker will
	 * operate as a superuser. This is safe because the slot sync worker does
	 * not interact with user tables, eliminating the risk of executing
	 * arbitrary code within triggers.
	 */
	InitPostgres(dbname, InvalidOid, NULL, InvalidOid, 0, NULL);

	SetProcessingMode(NormalProcessing);

	initStringInfo(&app_name);
	if (cluster_name[0])
		appendStringInfo(&app_name, "%s_%s", cluster_name, "slotsync worker");
	else
		appendStringInfoString(&app_name, "slotsync worker");

	/*
	 * Establish the connection to the primary server for slot
	 * synchronization.
	 */
	wrconn = walrcv_connect(PrimaryConnInfo, false, false, false,
							app_name.data, &err);
	pfree(app_name.data);

	if (!wrconn)
		ereport(ERROR,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("synchronization worker \"%s\" could not connect to the primary server: %s",
					   app_name.data, err));

	/*
	 * Register the disconnection callback.
	 *
	 * XXX: This can be combined with previous cleanup registration of
	 * slotsync_worker_onexit() but that will need the connection to be made
	 * global and we want to avoid introducing global for this purpose.
	 */
	before_shmem_exit(slotsync_worker_disconnect, PointerGetDatum(wrconn));

	/*
	 * Using the specified primary server connection, check that we are not a
	 * cascading standby and slot configured in 'primary_slot_name' exists on
	 * the primary server.
	 */
	validate_remote_info(wrconn);

	/* Main loop to synchronize slots */
	for (;;)
	{
		bool		some_slot_updated = false;

		ProcessSlotSyncInterrupts(wrconn);

		some_slot_updated = synchronize_slots(wrconn);

		wait_for_slot_activity(some_slot_updated);
	}

	/*
	 * The slot sync worker can't get here because it will only stop when it
	 * receives a SIGINT from the startup process, or when there is an error.
	 */
	Assert(false);
}

/*
 * Update the inactive_since property for synced slots.
 *
 * Note that this function is currently called when we shutdown the slot
 * sync machinery.
 */
static void
update_synced_slots_inactive_since(void)
{
	TimestampTz now = 0;

	/*
	 * We need to update inactive_since only when we are promoting standby to
	 * correctly interpret the inactive_since if the standby gets promoted
	 * without a restart. We don't want the slots to appear inactive for a
	 * long time after promotion if they haven't been synchronized recently.
	 * Whoever acquires the slot, i.e., makes the slot active, will reset it.
	 */
	if (!StandbyMode)
		return;

	/* The slot sync worker or SQL function mustn't be running by now */
	Assert((SlotSyncCtx->pid == InvalidPid) && !SlotSyncCtx->syncing);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		/* Check if it is a synchronized slot */
		if (s->in_use && s->data.synced)
		{
			Assert(SlotIsLogical(s));

			/* The slot must not be acquired by any process */
			Assert(s->active_pid == 0);

			/* Use the same inactive_since time for all the slots. */
			if (now == 0)
				now = GetCurrentTimestamp();

			ReplicationSlotSetInactiveSince(s, now, true);
		}
	}

	LWLockRelease(ReplicationSlotControlLock);
}

/*
 * Shut down the slot sync worker.
 *
 * This function sends signal to shutdown slot sync worker, if required. It
 * also waits till the slot sync worker has exited or
 * pg_sync_replication_slots() has finished.
 */
void
ShutDownSlotSync(void)
{
	pid_t		worker_pid;

	SpinLockAcquire(&SlotSyncCtx->mutex);

	SlotSyncCtx->stopSignaled = true;

	/*
	 * Return if neither the slot sync worker is running nor the function
	 * pg_sync_replication_slots() is executing.
	 */
	if (!SlotSyncCtx->syncing)
	{
		SpinLockRelease(&SlotSyncCtx->mutex);
		update_synced_slots_inactive_since();
		return;
	}

	worker_pid = SlotSyncCtx->pid;

	SpinLockRelease(&SlotSyncCtx->mutex);

	if (worker_pid != InvalidPid)
		kill(worker_pid, SIGINT);

	/* Wait for slot sync to end */
	for (;;)
	{
		int			rc;

		/* Wait a bit, we don't expect to have to wait long */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_REPLICATION_SLOTSYNC_SHUTDOWN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		SpinLockAcquire(&SlotSyncCtx->mutex);

		/* Ensure that no process is syncing the slots. */
		if (!SlotSyncCtx->syncing)
			break;

		SpinLockRelease(&SlotSyncCtx->mutex);
	}

	SpinLockRelease(&SlotSyncCtx->mutex);

	update_synced_slots_inactive_since();
}

/*
 * SlotSyncWorkerCanRestart
 *
 * Returns true if enough time (SLOTSYNC_RESTART_INTERVAL_SEC) has passed
 * since it was launched last. Otherwise returns false.
 *
 * This is a safety valve to protect against continuous respawn attempts if the
 * worker is dying immediately at launch. Note that since we will retry to
 * launch the worker from the postmaster main loop, we will get another
 * chance later.
 */
bool
SlotSyncWorkerCanRestart(void)
{
	time_t		curtime = time(NULL);

	/* Return false if too soon since last start. */
	if ((unsigned int) (curtime - SlotSyncCtx->last_start_time) <
		(unsigned int) SLOTSYNC_RESTART_INTERVAL_SEC)
		return false;

	SlotSyncCtx->last_start_time = curtime;

	return true;
}

/*
 * Is current process syncing replication slots?
 *
 * Could be either backend executing SQL function or slot sync worker.
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
	Size		size = SlotSyncShmemSize();
	bool		found;

	SlotSyncCtx = (SlotSyncCtxStruct *)
		ShmemInitStruct("Slot Sync Data", size, &found);

	if (!found)
	{
		memset(SlotSyncCtx, 0, size);
		SlotSyncCtx->pid = InvalidPid;
		SpinLockInit(&SlotSyncCtx->mutex);
	}
}

/*
 * Error cleanup callback for slot sync SQL function.
 */
static void
slotsync_failure_callback(int code, Datum arg)
{
	WalReceiverConn *wrconn = (WalReceiverConn *) DatumGetPointer(arg);

	/*
	 * We need to do slots cleanup here just like WalSndErrorCleanup() does.
	 *
	 * The startup process during promotion invokes ShutDownSlotSync() which
	 * waits for slot sync to finish and it does that by checking the
	 * 'syncing' flag. Thus the SQL function must be done with slots' release
	 * and cleanup to avoid any dangling temporary slots or active slots
	 * before it marks itself as finished syncing.
	 */

	/* Make sure active replication slots are released */
	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	/* Also cleanup the synced temporary slots. */
	ReplicationSlotCleanup(true);

	/*
	 * The set syncing_slots indicates that the process errored out without
	 * resetting the flag. So, we need to clean up shared memory and reset the
	 * flag here.
	 */
	if (syncing_slots)
		reset_syncing_flag();

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
		check_and_set_sync_info(InvalidPid);

		validate_remote_info(wrconn);

		synchronize_slots(wrconn);

		/* Cleanup the synced temporary slots */
		ReplicationSlotCleanup(true);

		/* We are done with sync, so reset sync flag */
		reset_syncing_flag();
	}
	PG_END_ENSURE_ERROR_CLEANUP(slotsync_failure_callback, PointerGetDatum(wrconn));
}
