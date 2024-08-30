/*-------------------------------------------------------------------------
 *
 * slot.c
 *	   Replication slot management.
 *
 *
 * Copyright (c) 2012-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/slot.c
 *
 * NOTES
 *
 * Replication slots are used to keep state about replication streams
 * originating from this cluster.  Their primary purpose is to prevent the
 * premature removal of WAL or of old tuple versions in a manner that would
 * interfere with replication; they are also useful for monitoring purposes.
 * Slots need to be permanent (to allow restarts), crash-safe, and allocatable
 * on standbys (to support cascading setups).  The requirement that slots be
 * usable on standbys precludes storing them in the system catalogs.
 *
 * Each replication slot gets its own directory inside the directory
 * $PGDATA / PG_REPLSLOT_DIR.  Inside that directory the state file will
 * contain the slot's own data.  Additional data can be stored alongside that
 * file if required.  While the server is running, the state data is also
 * cached in memory for efficiency.
 *
 * ReplicationSlotAllocationLock must be taken in exclusive mode to allocate
 * or free a slot. ReplicationSlotControlLock must be taken in shared mode
 * to iterate over the slots, and in exclusive mode to change the in_use flag
 * of a slot.  The remaining data in each slot is protected by its mutex.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "common/file_utils.h"
#include "common/string.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/slotsync.h"
#include "replication/slot.h"
#include "replication/walsender_private.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc_hooks.h"
#include "utils/varlena.h"

/*
 * Replication slot on-disk data structure.
 */
typedef struct ReplicationSlotOnDisk
{
	/* first part of this struct needs to be version independent */

	/* data not covered by checksum */
	uint32		magic;
	pg_crc32c	checksum;

	/* data covered by checksum */
	uint32		version;
	uint32		length;

	/*
	 * The actual data in the slot that follows can differ based on the above
	 * 'version'.
	 */

	ReplicationSlotPersistentData slotdata;
} ReplicationSlotOnDisk;

/*
 * Struct for the configuration of synchronized_standby_slots.
 *
 * Note: this must be a flat representation that can be held in a single chunk
 * of guc_malloc'd memory, so that it can be stored as the "extra" data for the
 * synchronized_standby_slots GUC.
 */
typedef struct
{
	/* Number of slot names in the slot_names[] */
	int			nslotnames;

	/*
	 * slot_names contains 'nslotnames' consecutive null-terminated C strings.
	 */
	char		slot_names[FLEXIBLE_ARRAY_MEMBER];
} SyncStandbySlotsConfigData;

/*
 * Lookup table for slot invalidation causes.
 */
const char *const SlotInvalidationCauses[] = {
	[RS_INVAL_NONE] = "none",
	[RS_INVAL_WAL_REMOVED] = "wal_removed",
	[RS_INVAL_HORIZON] = "rows_removed",
	[RS_INVAL_WAL_LEVEL] = "wal_level_insufficient",
};

/* Maximum number of invalidation causes */
#define	RS_INVAL_MAX_CAUSES RS_INVAL_WAL_LEVEL

StaticAssertDecl(lengthof(SlotInvalidationCauses) == (RS_INVAL_MAX_CAUSES + 1),
				 "array length mismatch");

/* size of version independent data */
#define ReplicationSlotOnDiskConstantSize \
	offsetof(ReplicationSlotOnDisk, slotdata)
/* size of the part of the slot not covered by the checksum */
#define ReplicationSlotOnDiskNotChecksummedSize  \
	offsetof(ReplicationSlotOnDisk, version)
/* size of the part covered by the checksum */
#define ReplicationSlotOnDiskChecksummedSize \
	sizeof(ReplicationSlotOnDisk) - ReplicationSlotOnDiskNotChecksummedSize
/* size of the slot data that is version dependent */
#define ReplicationSlotOnDiskV2Size \
	sizeof(ReplicationSlotOnDisk) - ReplicationSlotOnDiskConstantSize

#define SLOT_MAGIC		0x1051CA1	/* format identifier */
#define SLOT_VERSION	5		/* version for new files */

/* Control array for replication slot management */
ReplicationSlotCtlData *ReplicationSlotCtl = NULL;

/* My backend's replication slot in the shared memory array */
ReplicationSlot *MyReplicationSlot = NULL;

/* GUC variables */
int			max_replication_slots = 10; /* the maximum number of replication
										 * slots */

/*
 * This GUC lists streaming replication standby server slot names that
 * logical WAL sender processes will wait for.
 */
char	   *synchronized_standby_slots;

/* This is the parsed and cached configuration for synchronized_standby_slots */
static SyncStandbySlotsConfigData *synchronized_standby_slots_config;

/*
 * Oldest LSN that has been confirmed to be flushed to the standbys
 * corresponding to the physical slots specified in the synchronized_standby_slots GUC.
 */
static XLogRecPtr ss_oldest_flush_lsn = InvalidXLogRecPtr;

static void ReplicationSlotShmemExit(int code, Datum arg);
static void ReplicationSlotDropPtr(ReplicationSlot *slot);

/* internal persistency functions */
static void RestoreSlotFromDisk(const char *name);
static void CreateSlotOnDisk(ReplicationSlot *slot);
static void SaveSlotToPath(ReplicationSlot *slot, const char *dir, int elevel);

/*
 * Report shared-memory space needed by ReplicationSlotsShmemInit.
 */
Size
ReplicationSlotsShmemSize(void)
{
	Size		size = 0;

	if (max_replication_slots == 0)
		return size;

	size = offsetof(ReplicationSlotCtlData, replication_slots);
	size = add_size(size,
					mul_size(max_replication_slots, sizeof(ReplicationSlot)));

	return size;
}

/*
 * Allocate and initialize shared memory for replication slots.
 */
void
ReplicationSlotsShmemInit(void)
{
	bool		found;

	if (max_replication_slots == 0)
		return;

	ReplicationSlotCtl = (ReplicationSlotCtlData *)
		ShmemInitStruct("ReplicationSlot Ctl", ReplicationSlotsShmemSize(),
						&found);

	if (!found)
	{
		int			i;

		/* First time through, so initialize */
		MemSet(ReplicationSlotCtl, 0, ReplicationSlotsShmemSize());

		for (i = 0; i < max_replication_slots; i++)
		{
			ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];

			/* everything else is zeroed by the memset above */
			SpinLockInit(&slot->mutex);
			LWLockInitialize(&slot->io_in_progress_lock,
							 LWTRANCHE_REPLICATION_SLOT_IO);
			ConditionVariableInit(&slot->active_cv);
		}
	}
}

/*
 * Register the callback for replication slot cleanup and releasing.
 */
void
ReplicationSlotInitialize(void)
{
	before_shmem_exit(ReplicationSlotShmemExit, 0);
}

/*
 * Release and cleanup replication slots.
 */
static void
ReplicationSlotShmemExit(int code, Datum arg)
{
	/* Make sure active replication slots are released */
	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	/* Also cleanup all the temporary slots. */
	ReplicationSlotCleanup(false);
}

/*
 * Check whether the passed slot name is valid and report errors at elevel.
 *
 * Slot names may consist out of [a-z0-9_]{1,NAMEDATALEN-1} which should allow
 * the name to be used as a directory name on every supported OS.
 *
 * Returns whether the directory name is valid or not if elevel < ERROR.
 */
bool
ReplicationSlotValidateName(const char *name, int elevel)
{
	const char *cp;

	if (strlen(name) == 0)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("replication slot name \"%s\" is too short",
						name)));
		return false;
	}

	if (strlen(name) >= NAMEDATALEN)
	{
		ereport(elevel,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("replication slot name \"%s\" is too long",
						name)));
		return false;
	}

	for (cp = name; *cp; cp++)
	{
		if (!((*cp >= 'a' && *cp <= 'z')
			  || (*cp >= '0' && *cp <= '9')
			  || (*cp == '_')))
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("replication slot name \"%s\" contains invalid character",
							name),
					 errhint("Replication slot names may only contain lower case letters, numbers, and the underscore character.")));
			return false;
		}
	}
	return true;
}

/*
 * Create a new replication slot and mark it as used by this backend.
 *
 * name: Name of the slot
 * db_specific: logical decoding is db specific; if the slot is going to
 *	   be used for that pass true, otherwise false.
 * two_phase: Allows decoding of prepared transactions. We allow this option
 *     to be enabled only at the slot creation time. If we allow this option
 *     to be changed during decoding then it is quite possible that we skip
 *     prepare first time because this option was not enabled. Now next time
 *     during getting changes, if the two_phase option is enabled it can skip
 *     prepare because by that time start decoding point has been moved. So the
 *     user will only get commit prepared.
 * failover: If enabled, allows the slot to be synced to standbys so
 *     that logical replication can be resumed after failover.
 * synced: True if the slot is synchronized from the primary server.
 */
void
ReplicationSlotCreate(const char *name, bool db_specific,
					  ReplicationSlotPersistency persistency,
					  bool two_phase, bool failover, bool synced)
{
	ReplicationSlot *slot = NULL;
	int			i;

	Assert(MyReplicationSlot == NULL);

	ReplicationSlotValidateName(name, ERROR);

	if (failover)
	{
		/*
		 * Do not allow users to create the failover enabled slots on the
		 * standby as we do not support sync to the cascading standby.
		 *
		 * However, failover enabled slots can be created during slot
		 * synchronization because we need to retain the same values as the
		 * remote slot.
		 */
		if (RecoveryInProgress() && !IsSyncingReplicationSlots())
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot enable failover for a replication slot created on the standby"));

		/*
		 * Do not allow users to create failover enabled temporary slots,
		 * because temporary slots will not be synced to the standby.
		 *
		 * However, failover enabled temporary slots can be created during
		 * slot synchronization. See the comments atop slotsync.c for details.
		 */
		if (persistency == RS_TEMPORARY && !IsSyncingReplicationSlots())
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot enable failover for a temporary replication slot"));
	}

	/*
	 * If some other backend ran this code concurrently with us, we'd likely
	 * both allocate the same slot, and that would be bad.  We'd also be at
	 * risk of missing a name collision.  Also, we don't want to try to create
	 * a new slot while somebody's busy cleaning up an old one, because we
	 * might both be monkeying with the same directory.
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

	/*
	 * Check for name collision, and identify an allocatable slot.  We need to
	 * hold ReplicationSlotControlLock in shared mode for this, so that nobody
	 * else can change the in_use flags while we're looking at them.
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("replication slot \"%s\" already exists", name)));
		if (!s->in_use && slot == NULL)
			slot = s;
	}
	LWLockRelease(ReplicationSlotControlLock);

	/* If all slots are in use, we're out of luck. */
	if (slot == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("all replication slots are in use"),
				 errhint("Free one or increase \"max_replication_slots\".")));

	/*
	 * Since this slot is not in use, nobody should be looking at any part of
	 * it other than the in_use field unless they're trying to allocate it.
	 * And since we hold ReplicationSlotAllocationLock, nobody except us can
	 * be doing that.  So it's safe to initialize the slot.
	 */
	Assert(!slot->in_use);
	Assert(slot->active_pid == 0);

	/* first initialize persistent data */
	memset(&slot->data, 0, sizeof(ReplicationSlotPersistentData));
	namestrcpy(&slot->data.name, name);
	slot->data.database = db_specific ? MyDatabaseId : InvalidOid;
	slot->data.persistency = persistency;
	slot->data.two_phase = two_phase;
	slot->data.two_phase_at = InvalidXLogRecPtr;
	slot->data.failover = failover;
	slot->data.synced = synced;

	/* and then data only present in shared memory */
	slot->just_dirtied = false;
	slot->dirty = false;
	slot->effective_xmin = InvalidTransactionId;
	slot->effective_catalog_xmin = InvalidTransactionId;
	slot->candidate_catalog_xmin = InvalidTransactionId;
	slot->candidate_xmin_lsn = InvalidXLogRecPtr;
	slot->candidate_restart_valid = InvalidXLogRecPtr;
	slot->candidate_restart_lsn = InvalidXLogRecPtr;
	slot->last_saved_confirmed_flush = InvalidXLogRecPtr;
	slot->inactive_since = 0;

	/*
	 * Create the slot on disk.  We haven't actually marked the slot allocated
	 * yet, so no special cleanup is required if this errors out.
	 */
	CreateSlotOnDisk(slot);

	/*
	 * We need to briefly prevent any other backend from iterating over the
	 * slots while we flip the in_use flag. We also need to set the active
	 * flag while holding the ControlLock as otherwise a concurrent
	 * ReplicationSlotAcquire() could acquire the slot as well.
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);

	slot->in_use = true;

	/* We can now mark the slot active, and that makes it our slot. */
	SpinLockAcquire(&slot->mutex);
	Assert(slot->active_pid == 0);
	slot->active_pid = MyProcPid;
	SpinLockRelease(&slot->mutex);
	MyReplicationSlot = slot;

	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * Create statistics entry for the new logical slot. We don't collect any
	 * stats for physical slots, so no need to create an entry for the same.
	 * See ReplicationSlotDropPtr for why we need to do this before releasing
	 * ReplicationSlotAllocationLock.
	 */
	if (SlotIsLogical(slot))
		pgstat_create_replslot(slot);

	/*
	 * Now that the slot has been marked as in_use and active, it's safe to
	 * let somebody else try to allocate a slot.
	 */
	LWLockRelease(ReplicationSlotAllocationLock);

	/* Let everybody know we've modified this slot */
	ConditionVariableBroadcast(&slot->active_cv);
}

/*
 * Search for the named replication slot.
 *
 * Return the replication slot if found, otherwise NULL.
 */
ReplicationSlot *
SearchNamedReplicationSlot(const char *name, bool need_lock)
{
	int			i;
	ReplicationSlot *slot = NULL;

	if (need_lock)
		LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0)
		{
			slot = s;
			break;
		}
	}

	if (need_lock)
		LWLockRelease(ReplicationSlotControlLock);

	return slot;
}

/*
 * Return the index of the replication slot in
 * ReplicationSlotCtl->replication_slots.
 *
 * This is mainly useful to have an efficient key for storing replication slot
 * stats.
 */
int
ReplicationSlotIndex(ReplicationSlot *slot)
{
	Assert(slot >= ReplicationSlotCtl->replication_slots &&
		   slot < ReplicationSlotCtl->replication_slots + max_replication_slots);

	return slot - ReplicationSlotCtl->replication_slots;
}

/*
 * If the slot at 'index' is unused, return false. Otherwise 'name' is set to
 * the slot's name and true is returned.
 *
 * This likely is only useful for pgstat_replslot.c during shutdown, in other
 * cases there are obvious TOCTOU issues.
 */
bool
ReplicationSlotName(int index, Name name)
{
	ReplicationSlot *slot;
	bool		found;

	slot = &ReplicationSlotCtl->replication_slots[index];

	/*
	 * Ensure that the slot cannot be dropped while we copy the name. Don't
	 * need the spinlock as the name of an existing slot cannot change.
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	found = slot->in_use;
	if (slot->in_use)
		namestrcpy(name, NameStr(slot->data.name));
	LWLockRelease(ReplicationSlotControlLock);

	return found;
}

/*
 * Find a previously created slot and mark it as used by this process.
 *
 * An error is raised if nowait is true and the slot is currently in use. If
 * nowait is false, we sleep until the slot is released by the owning process.
 */
void
ReplicationSlotAcquire(const char *name, bool nowait)
{
	ReplicationSlot *s;
	int			active_pid;

	Assert(name != NULL);

retry:
	Assert(MyReplicationSlot == NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	/* Check if the slot exits with the given name. */
	s = SearchNamedReplicationSlot(name, false);
	if (s == NULL || !s->in_use)
	{
		LWLockRelease(ReplicationSlotControlLock);

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication slot \"%s\" does not exist",
						name)));
	}

	/*
	 * This is the slot we want; check if it's active under some other
	 * process.  In single user mode, we don't need this check.
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * Get ready to sleep on the slot in case it is active.  (We may end
		 * up not sleeping, but we don't want to do this while holding the
		 * spinlock.)
		 */
		if (!nowait)
			ConditionVariablePrepareToSleep(&s->active_cv);

		SpinLockAcquire(&s->mutex);
		if (s->active_pid == 0)
			s->active_pid = MyProcPid;
		active_pid = s->active_pid;
		SpinLockRelease(&s->mutex);
	}
	else
		active_pid = MyProcPid;
	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * If we found the slot but it's already active in another process, we
	 * wait until the owning process signals us that it's been released, or
	 * error out.
	 */
	if (active_pid != MyProcPid)
	{
		if (!nowait)
		{
			/* Wait here until we get signaled, and then restart */
			ConditionVariableSleep(&s->active_cv,
								   WAIT_EVENT_REPLICATION_SLOT_DROP);
			ConditionVariableCancelSleep();
			goto retry;
		}

		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("replication slot \"%s\" is active for PID %d",
						NameStr(s->data.name), active_pid)));
	}
	else if (!nowait)
		ConditionVariableCancelSleep(); /* no sleep needed after all */

	/* Let everybody know we've modified this slot */
	ConditionVariableBroadcast(&s->active_cv);

	/* We made this slot active, so it's ours now. */
	MyReplicationSlot = s;

	/*
	 * The call to pgstat_acquire_replslot() protects against stats for a
	 * different slot, from before a restart or such, being present during
	 * pgstat_report_replslot().
	 */
	if (SlotIsLogical(s))
		pgstat_acquire_replslot(s);

	/*
	 * Reset the time since the slot has become inactive as the slot is active
	 * now.
	 */
	SpinLockAcquire(&s->mutex);
	s->inactive_since = 0;
	SpinLockRelease(&s->mutex);

	if (am_walsender)
	{
		ereport(log_replication_commands ? LOG : DEBUG1,
				SlotIsLogical(s)
				? errmsg("acquired logical replication slot \"%s\"",
						 NameStr(s->data.name))
				: errmsg("acquired physical replication slot \"%s\"",
						 NameStr(s->data.name)));
	}
}

/*
 * Release the replication slot that this backend considers to own.
 *
 * This or another backend can re-acquire the slot later.
 * Resources this slot requires will be preserved.
 */
void
ReplicationSlotRelease(void)
{
	ReplicationSlot *slot = MyReplicationSlot;
	char	   *slotname = NULL;	/* keep compiler quiet */
	bool		is_logical = false; /* keep compiler quiet */
	TimestampTz now = 0;

	Assert(slot != NULL && slot->active_pid != 0);

	if (am_walsender)
	{
		slotname = pstrdup(NameStr(slot->data.name));
		is_logical = SlotIsLogical(slot);
	}

	if (slot->data.persistency == RS_EPHEMERAL)
	{
		/*
		 * Delete the slot. There is no !PANIC case where this is allowed to
		 * fail, all that may happen is an incomplete cleanup of the on-disk
		 * data.
		 */
		ReplicationSlotDropAcquired();
	}

	/*
	 * If slot needed to temporarily restrain both data and catalog xmin to
	 * create the catalog snapshot, remove that temporary constraint.
	 * Snapshots can only be exported while the initial snapshot is still
	 * acquired.
	 */
	if (!TransactionIdIsValid(slot->data.xmin) &&
		TransactionIdIsValid(slot->effective_xmin))
	{
		SpinLockAcquire(&slot->mutex);
		slot->effective_xmin = InvalidTransactionId;
		SpinLockRelease(&slot->mutex);
		ReplicationSlotsComputeRequiredXmin(false);
	}

	/*
	 * Set the time since the slot has become inactive. We get the current
	 * time beforehand to avoid system call while holding the spinlock.
	 */
	now = GetCurrentTimestamp();

	if (slot->data.persistency == RS_PERSISTENT)
	{
		/*
		 * Mark persistent slot inactive.  We're not freeing it, just
		 * disconnecting, but wake up others that may be waiting for it.
		 */
		SpinLockAcquire(&slot->mutex);
		slot->active_pid = 0;
		slot->inactive_since = now;
		SpinLockRelease(&slot->mutex);
		ConditionVariableBroadcast(&slot->active_cv);
	}
	else
	{
		SpinLockAcquire(&slot->mutex);
		slot->inactive_since = now;
		SpinLockRelease(&slot->mutex);
	}

	MyReplicationSlot = NULL;

	/* might not have been set when we've been a plain slot */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags &= ~PROC_IN_LOGICAL_DECODING;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
	LWLockRelease(ProcArrayLock);

	if (am_walsender)
	{
		ereport(log_replication_commands ? LOG : DEBUG1,
				is_logical
				? errmsg("released logical replication slot \"%s\"",
						 slotname)
				: errmsg("released physical replication slot \"%s\"",
						 slotname));

		pfree(slotname);
	}
}

/*
 * Cleanup temporary slots created in current session.
 *
 * Cleanup only synced temporary slots if 'synced_only' is true, else
 * cleanup all temporary slots.
 */
void
ReplicationSlotCleanup(bool synced_only)
{
	int			i;

	Assert(MyReplicationSlot == NULL);

restart:
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (!s->in_use)
			continue;

		SpinLockAcquire(&s->mutex);
		if ((s->active_pid == MyProcPid &&
			 (!synced_only || s->data.synced)))
		{
			Assert(s->data.persistency == RS_TEMPORARY);
			SpinLockRelease(&s->mutex);
			LWLockRelease(ReplicationSlotControlLock);	/* avoid deadlock */

			ReplicationSlotDropPtr(s);

			ConditionVariableBroadcast(&s->active_cv);
			goto restart;
		}
		else
			SpinLockRelease(&s->mutex);
	}

	LWLockRelease(ReplicationSlotControlLock);
}

/*
 * Permanently drop replication slot identified by the passed in name.
 */
void
ReplicationSlotDrop(const char *name, bool nowait)
{
	Assert(MyReplicationSlot == NULL);

	ReplicationSlotAcquire(name, nowait);

	/*
	 * Do not allow users to drop the slots which are currently being synced
	 * from the primary to the standby.
	 */
	if (RecoveryInProgress() && MyReplicationSlot->data.synced)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot drop replication slot \"%s\"", name),
				errdetail("This replication slot is being synchronized from the primary server."));

	ReplicationSlotDropAcquired();
}

/*
 * Change the definition of the slot identified by the specified name.
 */
void
ReplicationSlotAlter(const char *name, const bool *failover,
					 const bool *two_phase)
{
	bool		update_slot = false;

	Assert(MyReplicationSlot == NULL);
	Assert(failover || two_phase);

	ReplicationSlotAcquire(name, false);

	if (SlotIsPhysical(MyReplicationSlot))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot use %s with a physical replication slot",
					   "ALTER_REPLICATION_SLOT"));

	if (RecoveryInProgress())
	{
		/*
		 * Do not allow users to alter the slots which are currently being
		 * synced from the primary to the standby.
		 */
		if (MyReplicationSlot->data.synced)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("cannot alter replication slot \"%s\"", name),
					errdetail("This replication slot is being synchronized from the primary server."));

		/*
		 * Do not allow users to enable failover on the standby as we do not
		 * support sync to the cascading standby.
		 */
		if (failover && *failover)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot enable failover for a replication slot"
						   " on the standby"));
	}

	if (failover)
	{
		/*
		 * Do not allow users to enable failover for temporary slots as we do
		 * not support syncing temporary slots to the standby.
		 */
		if (*failover && MyReplicationSlot->data.persistency == RS_TEMPORARY)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot enable failover for a temporary replication slot"));

		if (MyReplicationSlot->data.failover != *failover)
		{
			SpinLockAcquire(&MyReplicationSlot->mutex);
			MyReplicationSlot->data.failover = *failover;
			SpinLockRelease(&MyReplicationSlot->mutex);

			update_slot = true;
		}
	}

	if (two_phase && MyReplicationSlot->data.two_phase != *two_phase)
	{
		SpinLockAcquire(&MyReplicationSlot->mutex);
		MyReplicationSlot->data.two_phase = *two_phase;
		SpinLockRelease(&MyReplicationSlot->mutex);

		update_slot = true;
	}

	if (update_slot)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
	}

	ReplicationSlotRelease();
}

/*
 * Permanently drop the currently acquired replication slot.
 */
void
ReplicationSlotDropAcquired(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(MyReplicationSlot != NULL);

	/* slot isn't acquired anymore */
	MyReplicationSlot = NULL;

	ReplicationSlotDropPtr(slot);
}

/*
 * Permanently drop the replication slot which will be released by the point
 * this function returns.
 */
static void
ReplicationSlotDropPtr(ReplicationSlot *slot)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];

	/*
	 * If some other backend ran this code concurrently with us, we might try
	 * to delete a slot with a certain name while someone else was trying to
	 * create a slot with the same name.
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

	/* Generate pathnames. */
	sprintf(path, "%s/%s", PG_REPLSLOT_DIR, NameStr(slot->data.name));
	sprintf(tmppath, "%s/%s.tmp", PG_REPLSLOT_DIR, NameStr(slot->data.name));

	/*
	 * Rename the slot directory on disk, so that we'll no longer recognize
	 * this as a valid slot.  Note that if this fails, we've got to mark the
	 * slot inactive before bailing out.  If we're dropping an ephemeral or a
	 * temporary slot, we better never fail hard as the caller won't expect
	 * the slot to survive and this might get called during error handling.
	 */
	if (rename(path, tmppath) == 0)
	{
		/*
		 * We need to fsync() the directory we just renamed and its parent to
		 * make sure that our changes are on disk in a crash-safe fashion.  If
		 * fsync() fails, we can't be sure whether the changes are on disk or
		 * not.  For now, we handle that by panicking;
		 * StartupReplicationSlots() will try to straighten it out after
		 * restart.
		 */
		START_CRIT_SECTION();
		fsync_fname(tmppath, true);
		fsync_fname(PG_REPLSLOT_DIR, true);
		END_CRIT_SECTION();
	}
	else
	{
		bool		fail_softly = slot->data.persistency != RS_PERSISTENT;

		SpinLockAcquire(&slot->mutex);
		slot->active_pid = 0;
		SpinLockRelease(&slot->mutex);

		/* wake up anyone waiting on this slot */
		ConditionVariableBroadcast(&slot->active_cv);

		ereport(fail_softly ? WARNING : ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						path, tmppath)));
	}

	/*
	 * The slot is definitely gone.  Lock out concurrent scans of the array
	 * long enough to kill it.  It's OK to clear the active PID here without
	 * grabbing the mutex because nobody else can be scanning the array here,
	 * and nobody can be attached to this slot and thus access it without
	 * scanning the array.
	 *
	 * Also wake up processes waiting for it.
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);
	slot->active_pid = 0;
	slot->in_use = false;
	LWLockRelease(ReplicationSlotControlLock);
	ConditionVariableBroadcast(&slot->active_cv);

	/*
	 * Slot is dead and doesn't prevent resource removal anymore, recompute
	 * limits.
	 */
	ReplicationSlotsComputeRequiredXmin(false);
	ReplicationSlotsComputeRequiredLSN();

	/*
	 * If removing the directory fails, the worst thing that will happen is
	 * that the user won't be able to create a new slot with the same name
	 * until the next server restart.  We warn about it, but that's all.
	 */
	if (!rmtree(tmppath, true))
		ereport(WARNING,
				(errmsg("could not remove directory \"%s\"", tmppath)));

	/*
	 * Drop the statistics entry for the replication slot.  Do this while
	 * holding ReplicationSlotAllocationLock so that we don't drop a
	 * statistics entry for another slot with the same name just created in
	 * another session.
	 */
	if (SlotIsLogical(slot))
		pgstat_drop_replslot(slot);

	/*
	 * We release this at the very end, so that nobody starts trying to create
	 * a slot while we're still cleaning up the detritus of the old one.
	 */
	LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * Serialize the currently acquired slot's state from memory to disk, thereby
 * guaranteeing the current state will survive a crash.
 */
void
ReplicationSlotSave(void)
{
	char		path[MAXPGPATH];

	Assert(MyReplicationSlot != NULL);

	sprintf(path, "%s/%s", PG_REPLSLOT_DIR, NameStr(MyReplicationSlot->data.name));
	SaveSlotToPath(MyReplicationSlot, path, ERROR);
}

/*
 * Signal that it would be useful if the currently acquired slot would be
 * flushed out to disk.
 *
 * Note that the actual flush to disk can be delayed for a long time, if
 * required for correctness explicitly do a ReplicationSlotSave().
 */
void
ReplicationSlotMarkDirty(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(MyReplicationSlot != NULL);

	SpinLockAcquire(&slot->mutex);
	MyReplicationSlot->just_dirtied = true;
	MyReplicationSlot->dirty = true;
	SpinLockRelease(&slot->mutex);
}

/*
 * Convert a slot that's marked as RS_EPHEMERAL or RS_TEMPORARY to a
 * RS_PERSISTENT slot, guaranteeing it will be there after an eventual crash.
 */
void
ReplicationSlotPersist(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(slot->data.persistency != RS_PERSISTENT);

	SpinLockAcquire(&slot->mutex);
	slot->data.persistency = RS_PERSISTENT;
	SpinLockRelease(&slot->mutex);

	ReplicationSlotMarkDirty();
	ReplicationSlotSave();
}

/*
 * Compute the oldest xmin across all slots and store it in the ProcArray.
 *
 * If already_locked is true, ProcArrayLock has already been acquired
 * exclusively.
 */
void
ReplicationSlotsComputeRequiredXmin(bool already_locked)
{
	int			i;
	TransactionId agg_xmin = InvalidTransactionId;
	TransactionId agg_catalog_xmin = InvalidTransactionId;

	Assert(ReplicationSlotCtl != NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		TransactionId effective_xmin;
		TransactionId effective_catalog_xmin;
		bool		invalidated;

		if (!s->in_use)
			continue;

		SpinLockAcquire(&s->mutex);
		effective_xmin = s->effective_xmin;
		effective_catalog_xmin = s->effective_catalog_xmin;
		invalidated = s->data.invalidated != RS_INVAL_NONE;
		SpinLockRelease(&s->mutex);

		/* invalidated slots need not apply */
		if (invalidated)
			continue;

		/* check the data xmin */
		if (TransactionIdIsValid(effective_xmin) &&
			(!TransactionIdIsValid(agg_xmin) ||
			 TransactionIdPrecedes(effective_xmin, agg_xmin)))
			agg_xmin = effective_xmin;

		/* check the catalog xmin */
		if (TransactionIdIsValid(effective_catalog_xmin) &&
			(!TransactionIdIsValid(agg_catalog_xmin) ||
			 TransactionIdPrecedes(effective_catalog_xmin, agg_catalog_xmin)))
			agg_catalog_xmin = effective_catalog_xmin;
	}

	LWLockRelease(ReplicationSlotControlLock);

	ProcArraySetReplicationSlotXmin(agg_xmin, agg_catalog_xmin, already_locked);
}

/*
 * Compute the oldest restart LSN across all slots and inform xlog module.
 *
 * Note: while max_slot_wal_keep_size is theoretically relevant for this
 * purpose, we don't try to account for that, because this module doesn't
 * know what to compare against.
 */
void
ReplicationSlotsComputeRequiredLSN(void)
{
	int			i;
	XLogRecPtr	min_required = InvalidXLogRecPtr;

	Assert(ReplicationSlotCtl != NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		XLogRecPtr	restart_lsn;
		bool		invalidated;

		if (!s->in_use)
			continue;

		SpinLockAcquire(&s->mutex);
		restart_lsn = s->data.restart_lsn;
		invalidated = s->data.invalidated != RS_INVAL_NONE;
		SpinLockRelease(&s->mutex);

		/* invalidated slots need not apply */
		if (invalidated)
			continue;

		if (restart_lsn != InvalidXLogRecPtr &&
			(min_required == InvalidXLogRecPtr ||
			 restart_lsn < min_required))
			min_required = restart_lsn;
	}
	LWLockRelease(ReplicationSlotControlLock);

	XLogSetReplicationSlotMinimumLSN(min_required);
}

/*
 * Compute the oldest WAL LSN required by *logical* decoding slots..
 *
 * Returns InvalidXLogRecPtr if logical decoding is disabled or no logical
 * slots exist.
 *
 * NB: this returns a value >= ReplicationSlotsComputeRequiredLSN(), since it
 * ignores physical replication slots.
 *
 * The results aren't required frequently, so we don't maintain a precomputed
 * value like we do for ComputeRequiredLSN() and ComputeRequiredXmin().
 */
XLogRecPtr
ReplicationSlotsComputeLogicalRestartLSN(void)
{
	XLogRecPtr	result = InvalidXLogRecPtr;
	int			i;

	if (max_replication_slots <= 0)
		return InvalidXLogRecPtr;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s;
		XLogRecPtr	restart_lsn;
		bool		invalidated;

		s = &ReplicationSlotCtl->replication_slots[i];

		/* cannot change while ReplicationSlotCtlLock is held */
		if (!s->in_use)
			continue;

		/* we're only interested in logical slots */
		if (!SlotIsLogical(s))
			continue;

		/* read once, it's ok if it increases while we're checking */
		SpinLockAcquire(&s->mutex);
		restart_lsn = s->data.restart_lsn;
		invalidated = s->data.invalidated != RS_INVAL_NONE;
		SpinLockRelease(&s->mutex);

		/* invalidated slots need not apply */
		if (invalidated)
			continue;

		if (restart_lsn == InvalidXLogRecPtr)
			continue;

		if (result == InvalidXLogRecPtr ||
			restart_lsn < result)
			result = restart_lsn;
	}

	LWLockRelease(ReplicationSlotControlLock);

	return result;
}

/*
 * ReplicationSlotsCountDBSlots -- count the number of slots that refer to the
 * passed database oid.
 *
 * Returns true if there are any slots referencing the database. *nslots will
 * be set to the absolute number of slots in the database, *nactive to ones
 * currently active.
 */
bool
ReplicationSlotsCountDBSlots(Oid dboid, int *nslots, int *nactive)
{
	int			i;

	*nslots = *nactive = 0;

	if (max_replication_slots <= 0)
		return false;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s;

		s = &ReplicationSlotCtl->replication_slots[i];

		/* cannot change while ReplicationSlotCtlLock is held */
		if (!s->in_use)
			continue;

		/* only logical slots are database specific, skip */
		if (!SlotIsLogical(s))
			continue;

		/* not our database, skip */
		if (s->data.database != dboid)
			continue;

		/* NB: intentionally counting invalidated slots */

		/* count slots with spinlock held */
		SpinLockAcquire(&s->mutex);
		(*nslots)++;
		if (s->active_pid != 0)
			(*nactive)++;
		SpinLockRelease(&s->mutex);
	}
	LWLockRelease(ReplicationSlotControlLock);

	if (*nslots > 0)
		return true;
	return false;
}

/*
 * ReplicationSlotsDropDBSlots -- Drop all db-specific slots relating to the
 * passed database oid. The caller should hold an exclusive lock on the
 * pg_database oid for the database to prevent creation of new slots on the db
 * or replay from existing slots.
 *
 * Another session that concurrently acquires an existing slot on the target DB
 * (most likely to drop it) may cause this function to ERROR. If that happens
 * it may have dropped some but not all slots.
 *
 * This routine isn't as efficient as it could be - but we don't drop
 * databases often, especially databases with lots of slots.
 */
void
ReplicationSlotsDropDBSlots(Oid dboid)
{
	int			i;

	if (max_replication_slots <= 0)
		return;

restart:
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s;
		char	   *slotname;
		int			active_pid;

		s = &ReplicationSlotCtl->replication_slots[i];

		/* cannot change while ReplicationSlotCtlLock is held */
		if (!s->in_use)
			continue;

		/* only logical slots are database specific, skip */
		if (!SlotIsLogical(s))
			continue;

		/* not our database, skip */
		if (s->data.database != dboid)
			continue;

		/* NB: intentionally including invalidated slots */

		/* acquire slot, so ReplicationSlotDropAcquired can be reused  */
		SpinLockAcquire(&s->mutex);
		/* can't change while ReplicationSlotControlLock is held */
		slotname = NameStr(s->data.name);
		active_pid = s->active_pid;
		if (active_pid == 0)
		{
			MyReplicationSlot = s;
			s->active_pid = MyProcPid;
		}
		SpinLockRelease(&s->mutex);

		/*
		 * Even though we hold an exclusive lock on the database object a
		 * logical slot for that DB can still be active, e.g. if it's
		 * concurrently being dropped by a backend connected to another DB.
		 *
		 * That's fairly unlikely in practice, so we'll just bail out.
		 *
		 * The slot sync worker holds a shared lock on the database before
		 * operating on synced logical slots to avoid conflict with the drop
		 * happening here. The persistent synced slots are thus safe but there
		 * is a possibility that the slot sync worker has created a temporary
		 * slot (which stays active even on release) and we are trying to drop
		 * that here. In practice, the chances of hitting this scenario are
		 * less as during slot synchronization, the temporary slot is
		 * immediately converted to persistent and thus is safe due to the
		 * shared lock taken on the database. So, we'll just bail out in such
		 * a case.
		 *
		 * XXX: We can consider shutting down the slot sync worker before
		 * trying to drop synced temporary slots here.
		 */
		if (active_pid)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("replication slot \"%s\" is active for PID %d",
							slotname, active_pid)));

		/*
		 * To avoid duplicating ReplicationSlotDropAcquired() and to avoid
		 * holding ReplicationSlotControlLock over filesystem operations,
		 * release ReplicationSlotControlLock and use
		 * ReplicationSlotDropAcquired.
		 *
		 * As that means the set of slots could change, restart scan from the
		 * beginning each time we release the lock.
		 */
		LWLockRelease(ReplicationSlotControlLock);
		ReplicationSlotDropAcquired();
		goto restart;
	}
	LWLockRelease(ReplicationSlotControlLock);
}


/*
 * Check whether the server's configuration supports using replication
 * slots.
 */
void
CheckSlotRequirements(void)
{
	/*
	 * NB: Adding a new requirement likely means that RestoreSlotFromDisk()
	 * needs the same check.
	 */

	if (max_replication_slots == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slots can only be used if \"max_replication_slots\" > 0")));

	if (wal_level < WAL_LEVEL_REPLICA)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slots can only be used if \"wal_level\" >= \"replica\"")));
}

/*
 * Check whether the user has privilege to use replication slots.
 */
void
CheckSlotPermissions(void)
{
	if (!has_rolreplication(GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to use replication slots"),
				 errdetail("Only roles with the %s attribute may use replication slots.",
						   "REPLICATION")));
}

/*
 * Reserve WAL for the currently active slot.
 *
 * Compute and set restart_lsn in a manner that's appropriate for the type of
 * the slot and concurrency safe.
 */
void
ReplicationSlotReserveWal(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(slot->data.restart_lsn == InvalidXLogRecPtr);

	/*
	 * The replication slot mechanism is used to prevent removal of required
	 * WAL. As there is no interlock between this routine and checkpoints, WAL
	 * segments could concurrently be removed when a now stale return value of
	 * ReplicationSlotsComputeRequiredLSN() is used. In the unlikely case that
	 * this happens we'll just retry.
	 */
	while (true)
	{
		XLogSegNo	segno;
		XLogRecPtr	restart_lsn;

		/*
		 * For logical slots log a standby snapshot and start logical decoding
		 * at exactly that position. That allows the slot to start up more
		 * quickly. But on a standby we cannot do WAL writes, so just use the
		 * replay pointer; effectively, an attempt to create a logical slot on
		 * standby will cause it to wait for an xl_running_xact record to be
		 * logged independently on the primary, so that a snapshot can be
		 * built using the record.
		 *
		 * None of this is needed (or indeed helpful) for physical slots as
		 * they'll start replay at the last logged checkpoint anyway. Instead
		 * return the location of the last redo LSN. While that slightly
		 * increases the chance that we have to retry, it's where a base
		 * backup has to start replay at.
		 */
		if (SlotIsPhysical(slot))
			restart_lsn = GetRedoRecPtr();
		else if (RecoveryInProgress())
			restart_lsn = GetXLogReplayRecPtr(NULL);
		else
			restart_lsn = GetXLogInsertRecPtr();

		SpinLockAcquire(&slot->mutex);
		slot->data.restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);

		/* prevent WAL removal as fast as possible */
		ReplicationSlotsComputeRequiredLSN();

		/*
		 * If all required WAL is still there, great, otherwise retry. The
		 * slot should prevent further removal of WAL, unless there's a
		 * concurrent ReplicationSlotsComputeRequiredLSN() after we've written
		 * the new restart_lsn above, so normally we should never need to loop
		 * more than twice.
		 */
		XLByteToSeg(slot->data.restart_lsn, segno, wal_segment_size);
		if (XLogGetLastRemovedSegno() < segno)
			break;
	}

	if (!RecoveryInProgress() && SlotIsLogical(slot))
	{
		XLogRecPtr	flushptr;

		/* make sure we have enough information to start */
		flushptr = LogStandbySnapshot();

		/* and make sure it's fsynced to disk */
		XLogFlush(flushptr);
	}
}

/*
 * Report that replication slot needs to be invalidated
 */
static void
ReportSlotInvalidation(ReplicationSlotInvalidationCause cause,
					   bool terminating,
					   int pid,
					   NameData slotname,
					   XLogRecPtr restart_lsn,
					   XLogRecPtr oldestLSN,
					   TransactionId snapshotConflictHorizon)
{
	StringInfoData err_detail;
	bool		hint = false;

	initStringInfo(&err_detail);

	switch (cause)
	{
		case RS_INVAL_WAL_REMOVED:
			{
				unsigned long long ex = oldestLSN - restart_lsn;

				hint = true;
				appendStringInfo(&err_detail,
								 ngettext("The slot's restart_lsn %X/%X exceeds the limit by %llu byte.",
										  "The slot's restart_lsn %X/%X exceeds the limit by %llu bytes.",
										  ex),
								 LSN_FORMAT_ARGS(restart_lsn),
								 ex);
				break;
			}
		case RS_INVAL_HORIZON:
			appendStringInfo(&err_detail, _("The slot conflicted with xid horizon %u."),
							 snapshotConflictHorizon);
			break;

		case RS_INVAL_WAL_LEVEL:
			appendStringInfoString(&err_detail, _("Logical decoding on standby requires \"wal_level\" >= \"logical\" on the primary server."));
			break;
		case RS_INVAL_NONE:
			pg_unreachable();
	}

	ereport(LOG,
			terminating ?
			errmsg("terminating process %d to release replication slot \"%s\"",
				   pid, NameStr(slotname)) :
			errmsg("invalidating obsolete replication slot \"%s\"",
				   NameStr(slotname)),
			errdetail_internal("%s", err_detail.data),
			hint ? errhint("You might need to increase \"%s\".", "max_slot_wal_keep_size") : 0);

	pfree(err_detail.data);
}

/*
 * Helper for InvalidateObsoleteReplicationSlots
 *
 * Acquires the given slot and mark it invalid, if necessary and possible.
 *
 * Returns whether ReplicationSlotControlLock was released in the interim (and
 * in that case we're not holding the lock at return, otherwise we are).
 *
 * Sets *invalidated true if the slot was invalidated. (Untouched otherwise.)
 *
 * This is inherently racy, because we release the LWLock
 * for syscalls, so caller must restart if we return true.
 */
static bool
InvalidatePossiblyObsoleteSlot(ReplicationSlotInvalidationCause cause,
							   ReplicationSlot *s,
							   XLogRecPtr oldestLSN,
							   Oid dboid, TransactionId snapshotConflictHorizon,
							   bool *invalidated)
{
	int			last_signaled_pid = 0;
	bool		released_lock = false;
	bool		terminated = false;
	TransactionId initial_effective_xmin = InvalidTransactionId;
	TransactionId initial_catalog_effective_xmin = InvalidTransactionId;
	XLogRecPtr	initial_restart_lsn = InvalidXLogRecPtr;
	ReplicationSlotInvalidationCause invalidation_cause_prev PG_USED_FOR_ASSERTS_ONLY = RS_INVAL_NONE;

	for (;;)
	{
		XLogRecPtr	restart_lsn;
		NameData	slotname;
		int			active_pid = 0;
		ReplicationSlotInvalidationCause invalidation_cause = RS_INVAL_NONE;

		Assert(LWLockHeldByMeInMode(ReplicationSlotControlLock, LW_SHARED));

		if (!s->in_use)
		{
			if (released_lock)
				LWLockRelease(ReplicationSlotControlLock);
			break;
		}

		/*
		 * Check if the slot needs to be invalidated. If it needs to be
		 * invalidated, and is not currently acquired, acquire it and mark it
		 * as having been invalidated.  We do this with the spinlock held to
		 * avoid race conditions -- for example the restart_lsn could move
		 * forward, or the slot could be dropped.
		 */
		SpinLockAcquire(&s->mutex);

		restart_lsn = s->data.restart_lsn;

		/* we do nothing if the slot is already invalid */
		if (s->data.invalidated == RS_INVAL_NONE)
		{
			/*
			 * The slot's mutex will be released soon, and it is possible that
			 * those values change since the process holding the slot has been
			 * terminated (if any), so record them here to ensure that we
			 * would report the correct invalidation cause.
			 */
			if (!terminated)
			{
				initial_restart_lsn = s->data.restart_lsn;
				initial_effective_xmin = s->effective_xmin;
				initial_catalog_effective_xmin = s->effective_catalog_xmin;
			}

			switch (cause)
			{
				case RS_INVAL_WAL_REMOVED:
					if (initial_restart_lsn != InvalidXLogRecPtr &&
						initial_restart_lsn < oldestLSN)
						invalidation_cause = cause;
					break;
				case RS_INVAL_HORIZON:
					if (!SlotIsLogical(s))
						break;
					/* invalid DB oid signals a shared relation */
					if (dboid != InvalidOid && dboid != s->data.database)
						break;
					if (TransactionIdIsValid(initial_effective_xmin) &&
						TransactionIdPrecedesOrEquals(initial_effective_xmin,
													  snapshotConflictHorizon))
						invalidation_cause = cause;
					else if (TransactionIdIsValid(initial_catalog_effective_xmin) &&
							 TransactionIdPrecedesOrEquals(initial_catalog_effective_xmin,
														   snapshotConflictHorizon))
						invalidation_cause = cause;
					break;
				case RS_INVAL_WAL_LEVEL:
					if (SlotIsLogical(s))
						invalidation_cause = cause;
					break;
				case RS_INVAL_NONE:
					pg_unreachable();
			}
		}

		/*
		 * The invalidation cause recorded previously should not change while
		 * the process owning the slot (if any) has been terminated.
		 */
		Assert(!(invalidation_cause_prev != RS_INVAL_NONE && terminated &&
				 invalidation_cause_prev != invalidation_cause));

		/* if there's no invalidation, we're done */
		if (invalidation_cause == RS_INVAL_NONE)
		{
			SpinLockRelease(&s->mutex);
			if (released_lock)
				LWLockRelease(ReplicationSlotControlLock);
			break;
		}

		slotname = s->data.name;
		active_pid = s->active_pid;

		/*
		 * If the slot can be acquired, do so and mark it invalidated
		 * immediately.  Otherwise we'll signal the owning process, below, and
		 * retry.
		 */
		if (active_pid == 0)
		{
			MyReplicationSlot = s;
			s->active_pid = MyProcPid;
			s->data.invalidated = invalidation_cause;

			/*
			 * XXX: We should consider not overwriting restart_lsn and instead
			 * just rely on .invalidated.
			 */
			if (invalidation_cause == RS_INVAL_WAL_REMOVED)
				s->data.restart_lsn = InvalidXLogRecPtr;

			/* Let caller know */
			*invalidated = true;
		}

		SpinLockRelease(&s->mutex);

		/*
		 * The logical replication slots shouldn't be invalidated as GUC
		 * max_slot_wal_keep_size is set to -1 during the binary upgrade. See
		 * check_old_cluster_for_valid_slots() where we ensure that no
		 * invalidated before the upgrade.
		 */
		Assert(!(*invalidated && SlotIsLogical(s) && IsBinaryUpgrade));

		if (active_pid != 0)
		{
			/*
			 * Prepare the sleep on the slot's condition variable before
			 * releasing the lock, to close a possible race condition if the
			 * slot is released before the sleep below.
			 */
			ConditionVariablePrepareToSleep(&s->active_cv);

			LWLockRelease(ReplicationSlotControlLock);
			released_lock = true;

			/*
			 * Signal to terminate the process that owns the slot, if we
			 * haven't already signalled it.  (Avoidance of repeated
			 * signalling is the only reason for there to be a loop in this
			 * routine; otherwise we could rely on caller's restart loop.)
			 *
			 * There is the race condition that other process may own the slot
			 * after its current owner process is terminated and before this
			 * process owns it. To handle that, we signal only if the PID of
			 * the owning process has changed from the previous time. (This
			 * logic assumes that the same PID is not reused very quickly.)
			 */
			if (last_signaled_pid != active_pid)
			{
				ReportSlotInvalidation(invalidation_cause, true, active_pid,
									   slotname, restart_lsn,
									   oldestLSN, snapshotConflictHorizon);

				if (MyBackendType == B_STARTUP)
					(void) SendProcSignal(active_pid,
										  PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT,
										  INVALID_PROC_NUMBER);
				else
					(void) kill(active_pid, SIGTERM);

				last_signaled_pid = active_pid;
				terminated = true;
				invalidation_cause_prev = invalidation_cause;
			}

			/* Wait until the slot is released. */
			ConditionVariableSleep(&s->active_cv,
								   WAIT_EVENT_REPLICATION_SLOT_DROP);

			/*
			 * Re-acquire lock and start over; we expect to invalidate the
			 * slot next time (unless another process acquires the slot in the
			 * meantime).
			 */
			LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
			continue;
		}
		else
		{
			/*
			 * We hold the slot now and have already invalidated it; flush it
			 * to ensure that state persists.
			 *
			 * Don't want to hold ReplicationSlotControlLock across file
			 * system operations, so release it now but be sure to tell caller
			 * to restart from scratch.
			 */
			LWLockRelease(ReplicationSlotControlLock);
			released_lock = true;

			/* Make sure the invalidated state persists across server restart */
			ReplicationSlotMarkDirty();
			ReplicationSlotSave();
			ReplicationSlotRelease();

			ReportSlotInvalidation(invalidation_cause, false, active_pid,
								   slotname, restart_lsn,
								   oldestLSN, snapshotConflictHorizon);

			/* done with this slot for now */
			break;
		}
	}

	Assert(released_lock == !LWLockHeldByMe(ReplicationSlotControlLock));

	return released_lock;
}

/*
 * Invalidate slots that require resources about to be removed.
 *
 * Returns true when any slot have got invalidated.
 *
 * Whether a slot needs to be invalidated depends on the cause. A slot is
 * removed if it:
 * - RS_INVAL_WAL_REMOVED: requires a LSN older than the given segment
 * - RS_INVAL_HORIZON: requires a snapshot <= the given horizon in the given
 *   db; dboid may be InvalidOid for shared relations
 * - RS_INVAL_WAL_LEVEL: is logical
 *
 * NB - this runs as part of checkpoint, so avoid raising errors if possible.
 */
bool
InvalidateObsoleteReplicationSlots(ReplicationSlotInvalidationCause cause,
								   XLogSegNo oldestSegno, Oid dboid,
								   TransactionId snapshotConflictHorizon)
{
	XLogRecPtr	oldestLSN;
	bool		invalidated = false;

	Assert(cause != RS_INVAL_HORIZON || TransactionIdIsValid(snapshotConflictHorizon));
	Assert(cause != RS_INVAL_WAL_REMOVED || oldestSegno > 0);
	Assert(cause != RS_INVAL_NONE);

	if (max_replication_slots == 0)
		return invalidated;

	XLogSegNoOffsetToRecPtr(oldestSegno, 0, wal_segment_size, oldestLSN);

restart:
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (!s->in_use)
			continue;

		if (InvalidatePossiblyObsoleteSlot(cause, s, oldestLSN, dboid,
										   snapshotConflictHorizon,
										   &invalidated))
		{
			/* if the lock was released, start from scratch */
			goto restart;
		}
	}
	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * If any slots have been invalidated, recalculate the resource limits.
	 */
	if (invalidated)
	{
		ReplicationSlotsComputeRequiredXmin(false);
		ReplicationSlotsComputeRequiredLSN();
	}

	return invalidated;
}

/*
 * Flush all replication slots to disk.
 *
 * It is convenient to flush dirty replication slots at the time of checkpoint.
 * Additionally, in case of a shutdown checkpoint, we also identify the slots
 * for which the confirmed_flush LSN has been updated since the last time it
 * was saved and flush them.
 */
void
CheckPointReplicationSlots(bool is_shutdown)
{
	int			i;

	elog(DEBUG1, "performing replication slot checkpoint");

	/*
	 * Prevent any slot from being created/dropped while we're active. As we
	 * explicitly do *not* want to block iterating over replication_slots or
	 * acquiring a slot we cannot take the control lock - but that's OK,
	 * because holding ReplicationSlotAllocationLock is strictly stronger, and
	 * enough to guarantee that nobody can change the in_use bits on us.
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		char		path[MAXPGPATH];

		if (!s->in_use)
			continue;

		/* save the slot to disk, locking is handled in SaveSlotToPath() */
		sprintf(path, "%s/%s", PG_REPLSLOT_DIR, NameStr(s->data.name));

		/*
		 * Slot's data is not flushed each time the confirmed_flush LSN is
		 * updated as that could lead to frequent writes.  However, we decide
		 * to force a flush of all logical slot's data at the time of shutdown
		 * if the confirmed_flush LSN is changed since we last flushed it to
		 * disk.  This helps in avoiding an unnecessary retreat of the
		 * confirmed_flush LSN after restart.
		 */
		if (is_shutdown && SlotIsLogical(s))
		{
			SpinLockAcquire(&s->mutex);

			if (s->data.invalidated == RS_INVAL_NONE &&
				s->data.confirmed_flush > s->last_saved_confirmed_flush)
			{
				s->just_dirtied = true;
				s->dirty = true;
			}
			SpinLockRelease(&s->mutex);
		}

		SaveSlotToPath(s, path, LOG);
	}
	LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * Load all replication slots from disk into memory at server startup. This
 * needs to be run before we start crash recovery.
 */
void
StartupReplicationSlots(void)
{
	DIR		   *replication_dir;
	struct dirent *replication_de;

	elog(DEBUG1, "starting up replication slots");

	/* restore all slots by iterating over all on-disk entries */
	replication_dir = AllocateDir(PG_REPLSLOT_DIR);
	while ((replication_de = ReadDir(replication_dir, PG_REPLSLOT_DIR)) != NULL)
	{
		char		path[MAXPGPATH + sizeof(PG_REPLSLOT_DIR)];
		PGFileType	de_type;

		if (strcmp(replication_de->d_name, ".") == 0 ||
			strcmp(replication_de->d_name, "..") == 0)
			continue;

		snprintf(path, sizeof(path), "%s/%s", PG_REPLSLOT_DIR, replication_de->d_name);
		de_type = get_dirent_type(path, replication_de, false, DEBUG1);

		/* we're only creating directories here, skip if it's not our's */
		if (de_type != PGFILETYPE_ERROR && de_type != PGFILETYPE_DIR)
			continue;

		/* we crashed while a slot was being setup or deleted, clean up */
		if (pg_str_endswith(replication_de->d_name, ".tmp"))
		{
			if (!rmtree(path, true))
			{
				ereport(WARNING,
						(errmsg("could not remove directory \"%s\"",
								path)));
				continue;
			}
			fsync_fname(PG_REPLSLOT_DIR, true);
			continue;
		}

		/* looks like a slot in a normal state, restore */
		RestoreSlotFromDisk(replication_de->d_name);
	}
	FreeDir(replication_dir);

	/* currently no slots exist, we're done. */
	if (max_replication_slots <= 0)
		return;

	/* Now that we have recovered all the data, compute replication xmin */
	ReplicationSlotsComputeRequiredXmin(false);
	ReplicationSlotsComputeRequiredLSN();
}

/* ----
 * Manipulation of on-disk state of replication slots
 *
 * NB: none of the routines below should take any notice whether a slot is the
 * current one or not, that's all handled a layer above.
 * ----
 */
static void
CreateSlotOnDisk(ReplicationSlot *slot)
{
	char		tmppath[MAXPGPATH];
	char		path[MAXPGPATH];
	struct stat st;

	/*
	 * No need to take out the io_in_progress_lock, nobody else can see this
	 * slot yet, so nobody else will write. We're reusing SaveSlotToPath which
	 * takes out the lock, if we'd take the lock here, we'd deadlock.
	 */

	sprintf(path, "%s/%s", PG_REPLSLOT_DIR, NameStr(slot->data.name));
	sprintf(tmppath, "%s/%s.tmp", PG_REPLSLOT_DIR, NameStr(slot->data.name));

	/*
	 * It's just barely possible that some previous effort to create or drop a
	 * slot with this name left a temp directory lying around. If that seems
	 * to be the case, try to remove it.  If the rmtree() fails, we'll error
	 * out at the MakePGDirectory() below, so we don't bother checking
	 * success.
	 */
	if (stat(tmppath, &st) == 0 && S_ISDIR(st.st_mode))
		rmtree(tmppath, true);

	/* Create and fsync the temporary slot directory. */
	if (MakePGDirectory(tmppath) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m",
						tmppath)));
	fsync_fname(tmppath, true);

	/* Write the actual state file. */
	slot->dirty = true;			/* signal that we really need to write */
	SaveSlotToPath(slot, tmppath, ERROR);

	/* Rename the directory into place. */
	if (rename(tmppath, path) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmppath, path)));

	/*
	 * If we'd now fail - really unlikely - we wouldn't know whether this slot
	 * would persist after an OS crash or not - so, force a restart. The
	 * restart would try to fsync this again till it works.
	 */
	START_CRIT_SECTION();

	fsync_fname(path, true);
	fsync_fname(PG_REPLSLOT_DIR, true);

	END_CRIT_SECTION();
}

/*
 * Shared functionality between saving and creating a replication slot.
 */
static void
SaveSlotToPath(ReplicationSlot *slot, const char *dir, int elevel)
{
	char		tmppath[MAXPGPATH];
	char		path[MAXPGPATH];
	int			fd;
	ReplicationSlotOnDisk cp;
	bool		was_dirty;

	/* first check whether there's something to write out */
	SpinLockAcquire(&slot->mutex);
	was_dirty = slot->dirty;
	slot->just_dirtied = false;
	SpinLockRelease(&slot->mutex);

	/* and don't do anything if there's nothing to write */
	if (!was_dirty)
		return;

	LWLockAcquire(&slot->io_in_progress_lock, LW_EXCLUSIVE);

	/* silence valgrind :( */
	memset(&cp, 0, sizeof(ReplicationSlotOnDisk));

	sprintf(tmppath, "%s/state.tmp", dir);
	sprintf(path, "%s/state", dir);

	fd = OpenTransientFile(tmppath, O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (fd < 0)
	{
		/*
		 * If not an ERROR, then release the lock before returning.  In case
		 * of an ERROR, the error recovery path automatically releases the
		 * lock, but no harm in explicitly releasing even in that case.  Note
		 * that LWLockRelease() could affect errno.
		 */
		int			save_errno = errno;

		LWLockRelease(&slot->io_in_progress_lock);
		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						tmppath)));
		return;
	}

	cp.magic = SLOT_MAGIC;
	INIT_CRC32C(cp.checksum);
	cp.version = SLOT_VERSION;
	cp.length = ReplicationSlotOnDiskV2Size;

	SpinLockAcquire(&slot->mutex);

	memcpy(&cp.slotdata, &slot->data, sizeof(ReplicationSlotPersistentData));

	SpinLockRelease(&slot->mutex);

	COMP_CRC32C(cp.checksum,
				(char *) (&cp) + ReplicationSlotOnDiskNotChecksummedSize,
				ReplicationSlotOnDiskChecksummedSize);
	FIN_CRC32C(cp.checksum);

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_WRITE);
	if ((write(fd, &cp, sizeof(cp))) != sizeof(cp))
	{
		int			save_errno = errno;

		pgstat_report_wait_end();
		CloseTransientFile(fd);
		LWLockRelease(&slot->io_in_progress_lock);

		/* if write didn't set errno, assume problem is no disk space */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						tmppath)));
		return;
	}
	pgstat_report_wait_end();

	/* fsync the temporary file */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_SYNC);
	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		pgstat_report_wait_end();
		CloseTransientFile(fd);
		LWLockRelease(&slot->io_in_progress_lock);
		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						tmppath)));
		return;
	}
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
	{
		int			save_errno = errno;

		LWLockRelease(&slot->io_in_progress_lock);
		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						tmppath)));
		return;
	}

	/* rename to permanent file, fsync file and directory */
	if (rename(tmppath, path) != 0)
	{
		int			save_errno = errno;

		LWLockRelease(&slot->io_in_progress_lock);
		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmppath, path)));
		return;
	}

	/*
	 * Check CreateSlotOnDisk() for the reasoning of using a critical section.
	 */
	START_CRIT_SECTION();

	fsync_fname(path, false);
	fsync_fname(dir, true);
	fsync_fname(PG_REPLSLOT_DIR, true);

	END_CRIT_SECTION();

	/*
	 * Successfully wrote, unset dirty bit, unless somebody dirtied again
	 * already and remember the confirmed_flush LSN value.
	 */
	SpinLockAcquire(&slot->mutex);
	if (!slot->just_dirtied)
		slot->dirty = false;
	slot->last_saved_confirmed_flush = cp.slotdata.confirmed_flush;
	SpinLockRelease(&slot->mutex);

	LWLockRelease(&slot->io_in_progress_lock);
}

/*
 * Load a single slot from disk into memory.
 */
static void
RestoreSlotFromDisk(const char *name)
{
	ReplicationSlotOnDisk cp;
	int			i;
	char		slotdir[MAXPGPATH + sizeof(PG_REPLSLOT_DIR)];
	char		path[MAXPGPATH + sizeof(PG_REPLSLOT_DIR) + 10];
	int			fd;
	bool		restored = false;
	int			readBytes;
	pg_crc32c	checksum;

	/* no need to lock here, no concurrent access allowed yet */

	/* delete temp file if it exists */
	sprintf(slotdir, "%s/%s", PG_REPLSLOT_DIR, name);
	sprintf(path, "%s/state.tmp", slotdir);
	if (unlink(path) < 0 && errno != ENOENT)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));

	sprintf(path, "%s/state", slotdir);

	elog(DEBUG1, "restoring replication slot from \"%s\"", path);

	/* on some operating systems fsyncing a file requires O_RDWR */
	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);

	/*
	 * We do not need to handle this as we are rename()ing the directory into
	 * place only after we fsync()ed the state file.
	 */
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/*
	 * Sync state file before we're reading from it. We might have crashed
	 * while it wasn't synced yet and we shouldn't continue on that basis.
	 */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_RESTORE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						path)));
	pgstat_report_wait_end();

	/* Also sync the parent directory */
	START_CRIT_SECTION();
	fsync_fname(slotdir, true);
	END_CRIT_SECTION();

	/* read part of statefile that's guaranteed to be version independent */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_READ);
	readBytes = read(fd, &cp, ReplicationSlotOnDiskConstantSize);
	pgstat_report_wait_end();
	if (readBytes != ReplicationSlotOnDiskConstantSize)
	{
		if (readBytes < 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		else
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes,
							(Size) ReplicationSlotOnDiskConstantSize)));
	}

	/* verify magic */
	if (cp.magic != SLOT_MAGIC)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot file \"%s\" has wrong magic number: %u instead of %u",
						path, cp.magic, SLOT_MAGIC)));

	/* verify version */
	if (cp.version != SLOT_VERSION)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot file \"%s\" has unsupported version %u",
						path, cp.version)));

	/* boundary check on length */
	if (cp.length != ReplicationSlotOnDiskV2Size)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot file \"%s\" has corrupted length %u",
						path, cp.length)));

	/* Now that we know the size, read the entire file */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_READ);
	readBytes = read(fd,
					 (char *) &cp + ReplicationSlotOnDiskConstantSize,
					 cp.length);
	pgstat_report_wait_end();
	if (readBytes != cp.length)
	{
		if (readBytes < 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		else
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes, (Size) cp.length)));
	}

	if (CloseTransientFile(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	/* now verify the CRC */
	INIT_CRC32C(checksum);
	COMP_CRC32C(checksum,
				(char *) &cp + ReplicationSlotOnDiskNotChecksummedSize,
				ReplicationSlotOnDiskChecksummedSize);
	FIN_CRC32C(checksum);

	if (!EQ_CRC32C(checksum, cp.checksum))
		ereport(PANIC,
				(errmsg("checksum mismatch for replication slot file \"%s\": is %u, should be %u",
						path, checksum, cp.checksum)));

	/*
	 * If we crashed with an ephemeral slot active, don't restore but delete
	 * it.
	 */
	if (cp.slotdata.persistency != RS_PERSISTENT)
	{
		if (!rmtree(slotdir, true))
		{
			ereport(WARNING,
					(errmsg("could not remove directory \"%s\"",
							slotdir)));
		}
		fsync_fname(PG_REPLSLOT_DIR, true);
		return;
	}

	/*
	 * Verify that requirements for the specific slot type are met. That's
	 * important because if these aren't met we're not guaranteed to retain
	 * all the necessary resources for the slot.
	 *
	 * NB: We have to do so *after* the above checks for ephemeral slots,
	 * because otherwise a slot that shouldn't exist anymore could prevent
	 * restarts.
	 *
	 * NB: Changing the requirements here also requires adapting
	 * CheckSlotRequirements() and CheckLogicalDecodingRequirements().
	 */
	if (cp.slotdata.database != InvalidOid && wal_level < WAL_LEVEL_LOGICAL)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication slot \"%s\" exists, but \"wal_level\" < \"logical\"",
						NameStr(cp.slotdata.name)),
				 errhint("Change \"wal_level\" to be \"logical\" or higher.")));
	else if (wal_level < WAL_LEVEL_REPLICA)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("physical replication slot \"%s\" exists, but \"wal_level\" < \"replica\"",
						NameStr(cp.slotdata.name)),
				 errhint("Change \"wal_level\" to be \"replica\" or higher.")));

	/* nothing can be active yet, don't lock anything */
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *slot;

		slot = &ReplicationSlotCtl->replication_slots[i];

		if (slot->in_use)
			continue;

		/* restore the entire set of persistent data */
		memcpy(&slot->data, &cp.slotdata,
			   sizeof(ReplicationSlotPersistentData));

		/* initialize in memory state */
		slot->effective_xmin = cp.slotdata.xmin;
		slot->effective_catalog_xmin = cp.slotdata.catalog_xmin;
		slot->last_saved_confirmed_flush = cp.slotdata.confirmed_flush;

		slot->candidate_catalog_xmin = InvalidTransactionId;
		slot->candidate_xmin_lsn = InvalidXLogRecPtr;
		slot->candidate_restart_lsn = InvalidXLogRecPtr;
		slot->candidate_restart_valid = InvalidXLogRecPtr;

		slot->in_use = true;
		slot->active_pid = 0;

		/*
		 * Set the time since the slot has become inactive after loading the
		 * slot from the disk into memory. Whoever acquires the slot i.e.
		 * makes the slot active will reset it.
		 */
		slot->inactive_since = GetCurrentTimestamp();

		restored = true;
		break;
	}

	if (!restored)
		ereport(FATAL,
				(errmsg("too many replication slots active before shutdown"),
				 errhint("Increase \"max_replication_slots\" and try again.")));
}

/*
 * Maps an invalidation reason for a replication slot to
 * ReplicationSlotInvalidationCause.
 */
ReplicationSlotInvalidationCause
GetSlotInvalidationCause(const char *invalidation_reason)
{
	ReplicationSlotInvalidationCause cause;
	ReplicationSlotInvalidationCause result = RS_INVAL_NONE;
	bool		found PG_USED_FOR_ASSERTS_ONLY = false;

	Assert(invalidation_reason);

	for (cause = RS_INVAL_NONE; cause <= RS_INVAL_MAX_CAUSES; cause++)
	{
		if (strcmp(SlotInvalidationCauses[cause], invalidation_reason) == 0)
		{
			found = true;
			result = cause;
			break;
		}
	}

	Assert(found);
	return result;
}

/*
 * A helper function to validate slots specified in GUC synchronized_standby_slots.
 *
 * The rawname will be parsed, and the result will be saved into *elemlist.
 */
static bool
validate_sync_standby_slots(char *rawname, List **elemlist)
{
	bool		ok;

	/* Verify syntax and parse string into a list of identifiers */
	ok = SplitIdentifierString(rawname, ',', elemlist);

	if (!ok)
	{
		GUC_check_errdetail("List syntax is invalid.");
	}
	else if (!ReplicationSlotCtl)
	{
		/*
		 * We cannot validate the replication slot if the replication slots'
		 * data has not been initialized. This is ok as we will anyway
		 * validate the specified slot when waiting for them to catch up. See
		 * StandbySlotsHaveCaughtup() for details.
		 */
	}
	else
	{
		/* Check that the specified slots exist and are logical slots */
		LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

		foreach_ptr(char, name, *elemlist)
		{
			ReplicationSlot *slot;

			slot = SearchNamedReplicationSlot(name, false);

			if (!slot)
			{
				GUC_check_errdetail("replication slot \"%s\" does not exist",
									name);
				ok = false;
				break;
			}

			if (!SlotIsPhysical(slot))
			{
				GUC_check_errdetail("\"%s\" is not a physical replication slot",
									name);
				ok = false;
				break;
			}
		}

		LWLockRelease(ReplicationSlotControlLock);
	}

	return ok;
}

/*
 * GUC check_hook for synchronized_standby_slots
 */
bool
check_synchronized_standby_slots(char **newval, void **extra, GucSource source)
{
	char	   *rawname;
	char	   *ptr;
	List	   *elemlist;
	int			size;
	bool		ok;
	SyncStandbySlotsConfigData *config;

	if ((*newval)[0] == '\0')
		return true;

	/* Need a modifiable copy of the GUC string */
	rawname = pstrdup(*newval);

	/* Now verify if the specified slots exist and have correct type */
	ok = validate_sync_standby_slots(rawname, &elemlist);

	if (!ok || elemlist == NIL)
	{
		pfree(rawname);
		list_free(elemlist);
		return ok;
	}

	/* Compute the size required for the SyncStandbySlotsConfigData struct */
	size = offsetof(SyncStandbySlotsConfigData, slot_names);
	foreach_ptr(char, slot_name, elemlist)
		size += strlen(slot_name) + 1;

	/* GUC extra value must be guc_malloc'd, not palloc'd */
	config = (SyncStandbySlotsConfigData *) guc_malloc(LOG, size);

	/* Transform the data into SyncStandbySlotsConfigData */
	config->nslotnames = list_length(elemlist);

	ptr = config->slot_names;
	foreach_ptr(char, slot_name, elemlist)
	{
		strcpy(ptr, slot_name);
		ptr += strlen(slot_name) + 1;
	}

	*extra = (void *) config;

	pfree(rawname);
	list_free(elemlist);
	return true;
}

/*
 * GUC assign_hook for synchronized_standby_slots
 */
void
assign_synchronized_standby_slots(const char *newval, void *extra)
{
	/*
	 * The standby slots may have changed, so we must recompute the oldest
	 * LSN.
	 */
	ss_oldest_flush_lsn = InvalidXLogRecPtr;

	synchronized_standby_slots_config = (SyncStandbySlotsConfigData *) extra;
}

/*
 * Check if the passed slot_name is specified in the synchronized_standby_slots GUC.
 */
bool
SlotExistsInSyncStandbySlots(const char *slot_name)
{
	const char *standby_slot_name;

	/* Return false if there is no value in synchronized_standby_slots */
	if (synchronized_standby_slots_config == NULL)
		return false;

	/*
	 * XXX: We are not expecting this list to be long so a linear search
	 * shouldn't hurt but if that turns out not to be true then we can cache
	 * this information for each WalSender as well.
	 */
	standby_slot_name = synchronized_standby_slots_config->slot_names;
	for (int i = 0; i < synchronized_standby_slots_config->nslotnames; i++)
	{
		if (strcmp(standby_slot_name, slot_name) == 0)
			return true;

		standby_slot_name += strlen(standby_slot_name) + 1;
	}

	return false;
}

/*
 * Return true if the slots specified in synchronized_standby_slots have caught up to
 * the given WAL location, false otherwise.
 *
 * The elevel parameter specifies the error level used for logging messages
 * related to slots that do not exist, are invalidated, or are inactive.
 */
bool
StandbySlotsHaveCaughtup(XLogRecPtr wait_for_lsn, int elevel)
{
	const char *name;
	int			caught_up_slot_num = 0;
	XLogRecPtr	min_restart_lsn = InvalidXLogRecPtr;

	/*
	 * Don't need to wait for the standbys to catch up if there is no value in
	 * synchronized_standby_slots.
	 */
	if (synchronized_standby_slots_config == NULL)
		return true;

	/*
	 * Don't need to wait for the standbys to catch up if we are on a standby
	 * server, since we do not support syncing slots to cascading standbys.
	 */
	if (RecoveryInProgress())
		return true;

	/*
	 * Don't need to wait for the standbys to catch up if they are already
	 * beyond the specified WAL location.
	 */
	if (!XLogRecPtrIsInvalid(ss_oldest_flush_lsn) &&
		ss_oldest_flush_lsn >= wait_for_lsn)
		return true;

	/*
	 * To prevent concurrent slot dropping and creation while filtering the
	 * slots, take the ReplicationSlotControlLock outside of the loop.
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	name = synchronized_standby_slots_config->slot_names;
	for (int i = 0; i < synchronized_standby_slots_config->nslotnames; i++)
	{
		XLogRecPtr	restart_lsn;
		bool		invalidated;
		bool		inactive;
		ReplicationSlot *slot;

		slot = SearchNamedReplicationSlot(name, false);

		if (!slot)
		{
			/*
			 * If a slot name provided in synchronized_standby_slots does not
			 * exist, report a message and exit the loop. A user can specify a
			 * slot name that does not exist just before the server startup.
			 * The GUC check_hook(validate_sync_standby_slots) cannot validate
			 * such a slot during startup as the ReplicationSlotCtl shared
			 * memory is not initialized at that time. It is also possible for
			 * a user to drop the slot in synchronized_standby_slots
			 * afterwards.
			 */
			ereport(elevel,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("replication slot \"%s\" specified in parameter \"%s\" does not exist",
						   name, "synchronized_standby_slots"),
					errdetail("Logical replication is waiting on the standby associated with replication slot \"%s\".",
							  name),
					errhint("Create the replication slot \"%s\" or amend parameter \"%s\".",
							name, "synchronized_standby_slots"));
			break;
		}

		if (SlotIsLogical(slot))
		{
			/*
			 * If a logical slot name is provided in
			 * synchronized_standby_slots, report a message and exit the loop.
			 * Similar to the non-existent case, a user can specify a logical
			 * slot name in synchronized_standby_slots before the server
			 * startup, or drop an existing physical slot and recreate a
			 * logical slot with the same name.
			 */
			ereport(elevel,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("cannot specify logical replication slot \"%s\" in parameter \"%s\"",
						   name, "synchronized_standby_slots"),
					errdetail("Logical replication is waiting for correction on replication slot \"%s\".",
							  name),
					errhint("Remove the logical replication slot \"%s\" from parameter \"%s\".",
							name, "synchronized_standby_slots"));
			break;
		}

		SpinLockAcquire(&slot->mutex);
		restart_lsn = slot->data.restart_lsn;
		invalidated = slot->data.invalidated != RS_INVAL_NONE;
		inactive = slot->active_pid == 0;
		SpinLockRelease(&slot->mutex);

		if (invalidated)
		{
			/* Specified physical slot has been invalidated */
			ereport(elevel,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("physical replication slot \"%s\" specified in parameter \"%s\" has been invalidated",
						   name, "synchronized_standby_slots"),
					errdetail("Logical replication is waiting on the standby associated with replication slot \"%s\".",
							  name),
					errhint("Drop and recreate the replication slot \"%s\", or amend parameter \"%s\".",
							name, "synchronized_standby_slots"));
			break;
		}

		if (XLogRecPtrIsInvalid(restart_lsn) || restart_lsn < wait_for_lsn)
		{
			/* Log a message if no active_pid for this physical slot */
			if (inactive)
				ereport(elevel,
						errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("replication slot \"%s\" specified in parameter \"%s\" does not have active_pid",
							   name, "synchronized_standby_slots"),
						errdetail("Logical replication is waiting on the standby associated with replication slot \"%s\".",
								  name),
						errhint("Start the standby associated with the replication slot \"%s\", or amend parameter \"%s\".",
								name, "synchronized_standby_slots"));

			/* Continue if the current slot hasn't caught up. */
			break;
		}

		Assert(restart_lsn >= wait_for_lsn);

		if (XLogRecPtrIsInvalid(min_restart_lsn) ||
			min_restart_lsn > restart_lsn)
			min_restart_lsn = restart_lsn;

		caught_up_slot_num++;

		name += strlen(name) + 1;
	}

	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * Return false if not all the standbys have caught up to the specified
	 * WAL location.
	 */
	if (caught_up_slot_num != synchronized_standby_slots_config->nslotnames)
		return false;

	/* The ss_oldest_flush_lsn must not retreat. */
	Assert(XLogRecPtrIsInvalid(ss_oldest_flush_lsn) ||
		   min_restart_lsn >= ss_oldest_flush_lsn);

	ss_oldest_flush_lsn = min_restart_lsn;

	return true;
}

/*
 * Wait for physical standbys to confirm receiving the given lsn.
 *
 * Used by logical decoding SQL functions. It waits for physical standbys
 * corresponding to the physical slots specified in the synchronized_standby_slots GUC.
 */
void
WaitForStandbyConfirmation(XLogRecPtr wait_for_lsn)
{
	/*
	 * Don't need to wait for the standby to catch up if the current acquired
	 * slot is not a logical failover slot, or there is no value in
	 * synchronized_standby_slots.
	 */
	if (!MyReplicationSlot->data.failover || !synchronized_standby_slots_config)
		return;

	ConditionVariablePrepareToSleep(&WalSndCtl->wal_confirm_rcv_cv);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* Exit if done waiting for every slot. */
		if (StandbySlotsHaveCaughtup(wait_for_lsn, WARNING))
			break;

		/*
		 * Wait for the slots in the synchronized_standby_slots to catch up,
		 * but use a timeout (1s) so we can also check if the
		 * synchronized_standby_slots has been changed.
		 */
		ConditionVariableTimedSleep(&WalSndCtl->wal_confirm_rcv_cv, 1000,
									WAIT_EVENT_WAIT_FOR_STANDBY_CONFIRMATION);
	}

	ConditionVariableCancelSleep();
}
