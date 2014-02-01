/*-------------------------------------------------------------------------
 * slot.h
 *	   Replication slot management.
 *
 * Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLOT_H
#define SLOT_H

#include "fmgr.h"
#include "access/xlog.h"
#include "access/xlogreader.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"

typedef struct ReplicationSlotPersistentData
{
	/* The slot's identifier */
	NameData	name;

	/* database the slot is active on */
	Oid			database;

	/*
	 * xmin horizon for data
	 *
	 * NB: This may represent a value that hasn't been written to disk yet;
	 * see notes for effective_xmin, below.
	 */
	TransactionId xmin;

	/* oldest LSN that might be required by this replication slot */
	XLogRecPtr	restart_lsn;

} ReplicationSlotPersistentData;

/*
 * Shared memory state of a single replication slot.
 */
typedef struct ReplicationSlot
{
	/* lock, on same cacheline as effective_xmin */
	slock_t		mutex;

	/* is this slot defined */
	bool		in_use;

	/* is somebody streaming out changes for this slot */
	bool		active;

	/* any outstanding modifications? */
	bool		just_dirtied;
	bool		dirty;

	/*
	 * For logical decoding, it's extremely important that we never remove any
	 * data that's still needed for decoding purposes, even after a crash;
	 * otherwise, decoding will produce wrong answers.  Ordinary streaming
	 * replication also needs to prevent old row versions from being removed
	 * too soon, but the worst consequence we might encounter there is unwanted
	 * query cancellations on the standby.  Thus, for logical decoding,
	 * this value represents the latest xmin that has actually been
	 * written to disk, whereas for streaming replication, it's just the
	 * same as the persistent value (data.xmin).
	 */
	TransactionId effective_xmin;

	/* data surviving shutdowns and crashes */
	ReplicationSlotPersistentData data;

	/* is somebody performing io on this slot? */
	LWLock	   *io_in_progress_lock;
} ReplicationSlot;

/*
 * Shared memory control area for all of replication slots.
 */
typedef struct ReplicationSlotCtlData
{
	ReplicationSlot replication_slots[1];
} ReplicationSlotCtlData;

/*
 * Pointers to shared memory
 */
extern ReplicationSlotCtlData *ReplicationSlotCtl;
extern ReplicationSlot *MyReplicationSlot;

/* GUCs */
extern PGDLLIMPORT int max_replication_slots;

/* shmem initialization functions */
extern Size ReplicationSlotsShmemSize(void);
extern void ReplicationSlotsShmemInit(void);

/* management of individual slots */
extern void ReplicationSlotCreate(const char *name, bool db_specific);
extern void ReplicationSlotDrop(const char *name);
extern void ReplicationSlotAcquire(const char *name);
extern void ReplicationSlotRelease(void);
extern void ReplicationSlotSave(void);
extern void ReplicationSlotMarkDirty(void);

/* misc stuff */
extern bool ReplicationSlotValidateName(const char *name, int elevel);
extern void ReplicationSlotsComputeRequiredXmin(void);
extern void ReplicationSlotsComputeRequiredLSN(void);
extern void StartupReplicationSlots(XLogRecPtr checkPointRedo);
extern void CheckPointReplicationSlots(void);

extern void CheckSlotRequirements(void);
extern void ReplicationSlotAtProcExit(void);

/* SQL callable functions */
extern Datum pg_get_replication_slots(PG_FUNCTION_ARGS);

#endif /* SLOT_H */
