/*-------------------------------------------------------------------------
 * logicalctl.c
 *		Functionality to control logical decoding status online.
 *
 * This module enables dynamic control of logical decoding availability.
 * Logical decoding becomes active under two conditions: when the wal_level
 * parameter is set to 'logical', or when at least one valid logical replication
 * slot exists with wal_level set to 'replica'. The system disables logical
 * decoding when neither condition is met. Therefore, the dynamic control
 * of logical decoding availability is required only when wal_level is set
 * to 'replica'. Logical decoding is always enabled when wal_level='logical'
 * and always disabled when wal_level='minimal'.
 *
 * The core concept of dynamically enabling and disabling logical decoding
 * is to separately control two aspects: writing information required for
 * logical decoding to WAL records, and using logical decoding itself. During
 * activation, we first enable logical WAL writing while keeping logical
 * decoding disabled. This change is reflected in the read-only
 * effective_wal_level GUC parameter. Once we ensure that all processes have
 * updated to the latest effective_wal_level value, we then enable logical
 * decoding. Deactivation follows a similar careful, multi-step process
 * in reverse order.
 *
 * While activation occurs synchronously right after creating the first
 * logical slot, deactivation happens asynchronously through the checkpointer
 * process. This design avoids a race condition at the end of recovery; see
 * the comments in UpdateLogicalDecodingStatusEndOfRecovery() for details.
 * Asynchronous deactivation also avoids excessive toggling of the logical
 * decoding status in workloads that repeatedly create and drop a single
 * logical slot. On the other hand, this lazy approach can delay changes
 * to effective_wal_level and the disabling logical decoding, especially
 * when the checkpointer is busy with other tasks. We chose this lazy approach
 * in all deactivation paths to keep the implementation simple, even though
 * laziness is strictly required only for end-of-recovery cases. Future work
 * might address this limitation either by using a dedicated worker instead
 * of the checkpointer, or by implementing synchronous waiting during slot
 * drops if workloads are significantly affected by the lazy deactivation
 * of logical decoding.
 *
 * Standby servers use the primary server's effective_wal_level and logical
 * decoding status. Unlike normal activation and deactivation, these
 * are updated simultaneously without status change coordination, solely by
 * replaying XLOG_LOGICAL_DECODING_STATUS_CHANGE records. The local wal_level
 * setting has no effect during this time. Upon promotion, we update the
 * logical decoding status based on local conditions: the wal_level value and
 * the presence of logical slots.
 *
 * In the future, we could extend support to include automatic transitions
 * of effective_wal_level between 'minimal' and 'logical' WAL levels. However,
 * this enhancement would require additional coordination mechanisms and
 * careful implementation of operations such as terminating walsenders and
 * archiver processes while carefully considering the sequence of operations
 * to ensure system stability during these transitions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * 	  src/backend/replication/logical/logicalctl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xloginsert.h"
#include "catalog/pg_control.h"
#include "miscadmin.h"
#include "replication/slot.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "utils/injection_point.h"

/*
 * Struct for controlling the logical decoding status.
 *
 * This struct is protected by LogicalDecodingControlLock.
 */
typedef struct LogicalDecodingCtlData
{
	/*
	 * This is the authoritative value used by all processes to determine
	 * whether to write additional information required by logical decoding to
	 * WAL. Since this information could be checked frequently, each process
	 * caches this value in XLogLogicalInfo for better performance.
	 */
	bool		xlog_logical_info;

	/* True if logical decoding is available in the system */
	bool		logical_decoding_enabled;

	/* True if logical decoding might need to be disabled */
	bool		pending_disable;
} LogicalDecodingCtlData;

static LogicalDecodingCtlData *LogicalDecodingCtl = NULL;

/*
 * A process-local cache of LogicalDecodingCtl->xlog_logical_info. This is
 * initialized at process startup, and updated when processing the process
 * barrier signal in ProcessBarrierUpdateXLogLogicalInfo(). If the process
 * is in an XID-assigned transaction, the cache update is delayed until the
 * transaction ends. See the comments for XLogLogicalInfoUpdatePending for details.
 */
bool		XLogLogicalInfo = false;

/*
 * When receiving the PROCSIGNAL_BARRIER_UPDATE_XLOG_LOGICAL_INFO signal, if
 * an XID is assigned to the current transaction, the process sets this flag and
 * delays the XLogLogicalInfo update until the transaction ends. This ensures
 * that the XLogLogicalInfo value (typically accessed via XLogLogicalInfoActive)
 * remains consistent throughout the transaction.
 */
static bool XLogLogicalInfoUpdatePending = false;

static void update_xlog_logical_info(void);
static void abort_logical_decoding_activation(int code, Datum arg);
static void write_logical_decoding_status_update_record(bool status);

Size
LogicalDecodingCtlShmemSize(void)
{
	return sizeof(LogicalDecodingCtlData);
}

void
LogicalDecodingCtlShmemInit(void)
{
	bool		found;

	LogicalDecodingCtl = ShmemInitStruct("Logical decoding control",
										 LogicalDecodingCtlShmemSize(),
										 &found);

	if (!found)
		MemSet(LogicalDecodingCtl, 0, LogicalDecodingCtlShmemSize());
}

/*
 * Initialize the logical decoding status in shmem at server startup. This
 * must be called ONCE during postmaster or standalone-backend startup.
 */
void
StartupLogicalDecodingStatus(bool last_status)
{
	/* Logical decoding is always disabled when 'minimal' WAL level */
	if (wal_level == WAL_LEVEL_MINIMAL)
		return;

	/*
	 * Set the initial logical decoding status based on the last status. If
	 * logical decoding was enabled before the last shutdown, it remains
	 * enabled as we might have set wal_level='logical' or have at least one
	 * logical slot.
	 */
	LogicalDecodingCtl->xlog_logical_info = last_status;
	LogicalDecodingCtl->logical_decoding_enabled = last_status;
}

/*
 * Update the XLogLogicalInfo cache.
 */
static inline void
update_xlog_logical_info(void)
{
	XLogLogicalInfo = IsXLogLogicalInfoEnabled();
}

/*
 * Initialize XLogLogicalInfo backend-private cache. This routine is called
 * during process initialization.
 */
void
InitializeProcessXLogLogicalInfo(void)
{
	update_xlog_logical_info();
}

/*
 * This routine is called when we are told to update XLogLogicalInfo
 * by a ProcSignalBarrier.
 */
bool
ProcessBarrierUpdateXLogLogicalInfo(void)
{
	if (GetTopTransactionIdIfAny() != InvalidTransactionId)
	{
		/* Delay updating XLogLogicalInfo until the transaction end */
		XLogLogicalInfoUpdatePending = true;
	}
	else
		update_xlog_logical_info();

	return true;
}

/*
 * Check the shared memory state and return true if logical decoding is
 * enabled on the system.
 */
bool
IsLogicalDecodingEnabled(void)
{
	bool		enabled;

	LWLockAcquire(LogicalDecodingControlLock, LW_SHARED);
	enabled = LogicalDecodingCtl->logical_decoding_enabled;
	LWLockRelease(LogicalDecodingControlLock);

	return enabled;
}

/*
 * Returns true if logical WAL logging is enabled based on the shared memory
 * status.
 */
bool
IsXLogLogicalInfoEnabled(void)
{
	bool		xlog_logical_info;

	LWLockAcquire(LogicalDecodingControlLock, LW_SHARED);
	xlog_logical_info = LogicalDecodingCtl->xlog_logical_info;
	LWLockRelease(LogicalDecodingControlLock);

	return xlog_logical_info;
}

/*
 * Reset the local cache at end of the transaction.
 */
void
AtEOXact_LogicalCtl(void)
{
	/* Update the local cache if there is a pending update */
	if (XLogLogicalInfoUpdatePending)
	{
		update_xlog_logical_info();
		XLogLogicalInfoUpdatePending = false;
	}
}

/*
 * Writes an XLOG_LOGICAL_DECODING_STATUS_CHANGE WAL record with the given
 * status.
 */
static void
write_logical_decoding_status_update_record(bool status)
{
	XLogRecPtr	recptr;

	XLogBeginInsert();
	XLogRegisterData(&status, sizeof(bool));
	recptr = XLogInsert(RM_XLOG_ID, XLOG_LOGICAL_DECODING_STATUS_CHANGE);
	XLogFlush(recptr);
}

/*
 * A PG_ENSURE_ERROR_CLEANUP callback for activating logical decoding, resetting
 * the shared flags to revert the logical decoding activation process.
 */
static void
abort_logical_decoding_activation(int code, Datum arg)
{
	Assert(MyReplicationSlot);
	Assert(!LogicalDecodingCtl->logical_decoding_enabled);

	elog(DEBUG1, "aborting logical decoding activation process");

	/*
	 * Abort the change to xlog_logical_info. We don't need to check
	 * CheckLogicalSlotExists() as we're still holding a logical slot.
	 */
	LWLockAcquire(LogicalDecodingControlLock, LW_EXCLUSIVE);
	LogicalDecodingCtl->xlog_logical_info = false;
	LWLockRelease(LogicalDecodingControlLock);

	/*
	 * Some processes might have already started logical info WAL logging, so
	 * tell all running processes to update their caches. We don't need to
	 * wait for all processes to disable xlog_logical_info locally as it's
	 * always safe to write logical information to WAL records, even when not
	 * strictly required.
	 */
	EmitProcSignalBarrier(PROCSIGNAL_BARRIER_UPDATE_XLOG_LOGICAL_INFO);
}

/*
 * Enable logical decoding if disabled.
 *
 * If this function is called during recovery, it simply returns without
 * action since the logical decoding status change is not allowed during
 * this time. The logical decoding status depends on the status on the primary.
 * The caller should use CheckLogicalDecodingRequirements() before calling this
 * function to make sure that the logical decoding status can be modified.
 *
 * Note that there is no interlock between logical decoding activation
 * and slot creation. To ensure enabling logical decoding, the caller
 * needs to call this function after creating a logical slot before
 * initializing the logical decoding context.
 */
void
EnsureLogicalDecodingEnabled(void)
{
	Assert(MyReplicationSlot);
	Assert(wal_level >= WAL_LEVEL_REPLICA);

	/* Logical decoding is always enabled */
	if (wal_level >= WAL_LEVEL_LOGICAL)
		return;

	if (RecoveryInProgress())
	{
		/*
		 * CheckLogicalDecodingRequirements() must have already errored out if
		 * logical decoding is not enabled since we cannot enable the logical
		 * decoding status during recovery.
		 */
		Assert(IsLogicalDecodingEnabled());
		return;
	}

	/*
	 * Ensure to abort the activation process in cases where there in an
	 * interruption during the wait.
	 */
	PG_ENSURE_ERROR_CLEANUP(abort_logical_decoding_activation, (Datum) 0);
	{
		EnableLogicalDecoding();
	}
	PG_END_ENSURE_ERROR_CLEANUP(abort_logical_decoding_activation, (Datum) 0);
}

/*
 * A workhorse function to enable logical decoding.
 */
void
EnableLogicalDecoding(void)
{
	bool		in_recovery;

	LWLockAcquire(LogicalDecodingControlLock, LW_EXCLUSIVE);

	/* Return if it is already enabled */
	if (LogicalDecodingCtl->logical_decoding_enabled)
	{
		LogicalDecodingCtl->pending_disable = false;
		LWLockRelease(LogicalDecodingControlLock);
		return;
	}

	/*
	 * Set logical info WAL logging in shmem. All process starts after this
	 * point will include the information required by logical decoding to WAL
	 * records.
	 */
	LogicalDecodingCtl->xlog_logical_info = true;

	LWLockRelease(LogicalDecodingControlLock);

	/*
	 * Tell all running processes to reflect the xlog_logical_info update, and
	 * wait. This ensures that all running processes have enabled logical
	 * information WAL logging.
	 */
	WaitForProcSignalBarrier(
							 EmitProcSignalBarrier(PROCSIGNAL_BARRIER_UPDATE_XLOG_LOGICAL_INFO));

	INJECTION_POINT("logical-decoding-activation", NULL);

	in_recovery = RecoveryInProgress();

	/*
	 * There could be some transactions that might have started with the old
	 * status, but we don't need to wait for these transactions to complete as
	 * long as they have valid XIDs. These transactions will appear in the
	 * xl_running_xacts record and therefore the snapshot builder will not try
	 * to decode the transaction during the logical decoding initialization.
	 *
	 * There is a theoretical case where a transaction decides whether to
	 * include logical-info to WAL records before getting an XID. In this
	 * case, the transaction won't appear in xl_running_xacts.
	 *
	 * For operations that do not require an XID assignment, the process
	 * starts including logical-info immediately upon receiving the signal
	 * (barrier). If such an operation checks the effective_wal_level multiple
	 * times within a single execution, the resulting WAL records might be
	 * inconsistent (i.e., logical-info is included in some records but not in
	 * others). However, this is harmless because logical decoding generally
	 * ignores WAL records that are not associated with an assigned XID.
	 *
	 * One might think we need to wait for all running transactions, including
	 * those without XIDs and read-only transactions, to finish before
	 * enabling logical decoding. However, such a requirement would force the
	 * slot creation to wait for a potentially very long time due to
	 * long-running read queries, which is practically unacceptable.
	 */

	START_CRIT_SECTION();

	/*
	 * We enable logical decoding first, followed by writing the WAL record.
	 * This sequence ensures logical decoding becomes available on the primary
	 * first.
	 */
	LWLockAcquire(LogicalDecodingControlLock, LW_EXCLUSIVE);

	LogicalDecodingCtl->logical_decoding_enabled = true;

	if (!in_recovery)
		write_logical_decoding_status_update_record(true);

	LogicalDecodingCtl->pending_disable = false;

	LWLockRelease(LogicalDecodingControlLock);

	END_CRIT_SECTION();

	if (!in_recovery)
		ereport(LOG,
				errmsg("logical decoding is enabled upon creating a new logical replication slot"));
}

/*
 * Initiate a request for disabling logical decoding.
 *
 * Note that this function does not verify whether logical slots exist. The
 * checkpointer will verify if logical decoding should actually be disabled.
 */
void
RequestDisableLogicalDecoding(void)
{
	if (wal_level != WAL_LEVEL_REPLICA)
		return;

	/*
	 * It's possible that we might not actually need to disable logical
	 * decoding if someone creates a new logical slot concurrently. We set the
	 * flag anyway and the checkpointer will check it and disable logical
	 * decoding if necessary.
	 */
	LWLockAcquire(LogicalDecodingControlLock, LW_EXCLUSIVE);
	LogicalDecodingCtl->pending_disable = true;
	LWLockRelease(LogicalDecodingControlLock);

	WakeupCheckpointer();

	elog(DEBUG1, "requested disabling logical decoding");
}

/*
 * Disable logical decoding if necessary.
 *
 * This function disables logical decoding upon a request initiated by
 * RequestDisableLogicalDecoding(). Otherwise, it performs no action.
 */
void
DisableLogicalDecodingIfNecessary(void)
{
	bool		pending_disable;

	if (wal_level != WAL_LEVEL_REPLICA)
		return;

	/*
	 * Sanity check as we cannot disable logical decoding while holding a
	 * logical slot.
	 */
	Assert(!MyReplicationSlot);

	if (RecoveryInProgress())
		return;

	LWLockAcquire(LogicalDecodingControlLock, LW_SHARED);
	pending_disable = LogicalDecodingCtl->pending_disable;
	LWLockRelease(LogicalDecodingControlLock);

	/* Quick return if no pending disable request */
	if (!pending_disable)
		return;

	DisableLogicalDecoding();
}

/*
 * A workhorse function to disable logical decoding.
 */
void
DisableLogicalDecoding(void)
{
	bool		in_recovery = RecoveryInProgress();

	LWLockAcquire(LogicalDecodingControlLock, LW_EXCLUSIVE);

	/*
	 * Check if we can disable logical decoding.
	 *
	 * Skip CheckLogicalSlotExists() check during recovery because the
	 * existing slots will be invalidated after disabling logical decoding.
	 */
	if (!LogicalDecodingCtl->logical_decoding_enabled ||
		(!in_recovery && CheckLogicalSlotExists()))
	{
		LogicalDecodingCtl->pending_disable = false;
		LWLockRelease(LogicalDecodingControlLock);
		return;
	}

	START_CRIT_SECTION();

	/*
	 * We need to disable logical decoding first and then disable logical
	 * information WAL logging in order to ensure that no logical decoding
	 * processes WAL records with insufficient information.
	 */
	LogicalDecodingCtl->logical_decoding_enabled = false;

	/* Write the WAL to disable logical decoding on standbys too */
	if (!in_recovery)
		write_logical_decoding_status_update_record(false);

	/* Now disable logical information WAL logging */
	LogicalDecodingCtl->xlog_logical_info = false;
	LogicalDecodingCtl->pending_disable = false;

	END_CRIT_SECTION();

	if (!in_recovery)
		ereport(LOG,
				errmsg("logical decoding is disabled because there are no valid logical replication slots"));

	LWLockRelease(LogicalDecodingControlLock);

	/*
	 * Tell all running processes to reflect the xlog_logical_info update.
	 * Unlike when enabling logical decoding, we don't need to wait for all
	 * processes to complete it in this case. We already disabled logical
	 * decoding and it's always safe to write logical information to WAL
	 * records, even when not strictly required. Therefore, we don't need to
	 * wait for all running transactions to finish either.
	 */
	EmitProcSignalBarrier(PROCSIGNAL_BARRIER_UPDATE_XLOG_LOGICAL_INFO);
}

/*
 * Updates the logical decoding status at end of recovery, and ensures that
 * all running processes have the updated XLogLogicalInfo status. This
 * function must be called before accepting writes.
 */
void
UpdateLogicalDecodingStatusEndOfRecovery(void)
{
	bool		new_status = false;

	Assert(RecoveryInProgress());

	/*
	 * With 'minimal' WAL level, there are no logical replication slots during
	 * recovery. Logical decoding is always disabled, so there is no need to
	 * synchronize XLogLogicalInfo.
	 */
	if (wal_level == WAL_LEVEL_MINIMAL)
	{
		Assert(!IsXLogLogicalInfoEnabled() && !IsLogicalDecodingEnabled());
		return;
	}

	LWLockAcquire(LogicalDecodingControlLock, LW_EXCLUSIVE);

	if (wal_level == WAL_LEVEL_LOGICAL || CheckLogicalSlotExists())
		new_status = true;

	/*
	 * When recovery ends, we need to either enable or disable logical
	 * decoding based on the wal_level setting and the presence of logical
	 * slots. We need to note that concurrent slot creation and deletion could
	 * happen but WAL writes are still not permitted until recovery fully
	 * completes. Here's how we handle concurrent toggling of logical
	 * decoding:
	 *
	 * For 'enable' case, if there's a concurrent disable request before
	 * recovery fully completes, the checkpointer will handle it after
	 * recovery is done. This means there might be a brief period after
	 * recovery where logical decoding remains enabled even with no logical
	 * replication slots present. This temporary state is not new - it can
	 * already occur due to the checkpointer's asynchronous deactivation
	 * process.
	 *
	 * For 'disable' case, backend cannot create logical replication slots
	 * during recovery (see checks in CheckLogicalDecodingRequirements()),
	 * which prevents a race condition between disabling logical decoding and
	 * concurrent slot creation.
	 */
	if (new_status != LogicalDecodingCtl->logical_decoding_enabled)
	{
		/*
		 * Update both the logical decoding status and logical WAL logging
		 * status. Unlike toggling these status during non-recovery, we don't
		 * need to worry about the operation order as WAL writes are still not
		 * permitted.
		 */
		LogicalDecodingCtl->xlog_logical_info = new_status;
		LogicalDecodingCtl->logical_decoding_enabled = new_status;

		elog(DEBUG1,
			 "update logical decoding status to %d at the end of recovery",
			 new_status);

		/*
		 * Now that we updated the logical decoding status, clear the pending
		 * disable flag. It's possible that a concurrent process drops the
		 * last logical slot and initiates the pending disable again. The
		 * checkpointer process will check it.
		 */
		LogicalDecodingCtl->pending_disable = false;

		LWLockRelease(LogicalDecodingControlLock);

		write_logical_decoding_status_update_record(new_status);
	}
	else
		LWLockRelease(LogicalDecodingControlLock);

	/*
	 * Ensure all running processes have the updated status. We don't need to
	 * wait for running transactions to finish as we don't accept any writes
	 * yet. On the other hand, we need to wait for synchronizing
	 * XLogLogicalInfo even if we've not updated the status above as the
	 * status have been turned on and off during recovery, having running
	 * processes have different status on their local caches.
	 */
	if (IsUnderPostmaster)
		WaitForProcSignalBarrier(
								 EmitProcSignalBarrier(PROCSIGNAL_BARRIER_UPDATE_XLOG_LOGICAL_INFO));

	INJECTION_POINT("startup-logical-decoding-status-change-end-of-recovery", NULL);
}
