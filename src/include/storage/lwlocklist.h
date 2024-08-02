/*-------------------------------------------------------------------------
 *
 * lwlocklist.h
 *
 * The predefined LWLock list is kept in its own source file for use by
 * automatic tools.  The exact representation of a keyword is determined by
 * the PG_LWLOCK macro, which is not defined in this file; it can be
 * defined by the caller for special purposes.
 *
 * Also, generate-lwlocknames.pl processes this file to create lwlocknames.h.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/include/storage/lwlocklist.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * Some commonly-used locks have predefined positions within MainLWLockArray;
 * these are defined here.  If you add a lock, add it to the end to avoid
 * renumbering the existing locks; if you remove a lock, consider leaving a gap
 * in the numbering sequence for the benefit of DTrace and other external
 * debugging scripts.  Also, do not forget to update the section
 * WaitEventLWLock of src/backend/utils/activity/wait_event_names.txt.
 *
 * Note that the names here don't include the Lock suffix, to appease the
 * C preprocessor; it's added elsewhere.
 */

/* 0 is available; was formerly BufFreelistLock */
PG_LWLOCK(1, ShmemIndex)
PG_LWLOCK(2, OidGen)
PG_LWLOCK(3, XidGen)
PG_LWLOCK(4, ProcArray)
PG_LWLOCK(5, SInvalRead)
PG_LWLOCK(6, SInvalWrite)
PG_LWLOCK(7, WALBufMapping)
PG_LWLOCK(8, WALWrite)
PG_LWLOCK(9, ControlFile)
/* 10 was CheckpointLock */
/* 11 was XactSLRULock */
/* 12 was SubtransSLRULock */
PG_LWLOCK(13, MultiXactGen)
/* 14 was MultiXactOffsetSLRULock */
/* 15 was MultiXactMemberSLRULock */
PG_LWLOCK(16, RelCacheInit)
PG_LWLOCK(17, CheckpointerComm)
PG_LWLOCK(18, TwoPhaseState)
PG_LWLOCK(19, TablespaceCreate)
PG_LWLOCK(20, BtreeVacuum)
PG_LWLOCK(21, AddinShmemInit)
PG_LWLOCK(22, Autovacuum)
PG_LWLOCK(23, AutovacuumSchedule)
PG_LWLOCK(24, SyncScan)
PG_LWLOCK(25, RelationMapping)
/* 26 was NotifySLRULock */
PG_LWLOCK(27, NotifyQueue)
PG_LWLOCK(28, SerializableXactHash)
PG_LWLOCK(29, SerializableFinishedList)
PG_LWLOCK(30, SerializablePredicateList)
/* 31 was SerialSLRULock */
PG_LWLOCK(32, SyncRep)
PG_LWLOCK(33, BackgroundWorker)
PG_LWLOCK(34, DynamicSharedMemoryControl)
PG_LWLOCK(35, AutoFile)
PG_LWLOCK(36, ReplicationSlotAllocation)
PG_LWLOCK(37, ReplicationSlotControl)
/* 38 was CommitTsSLRULock */
PG_LWLOCK(39, CommitTs)
PG_LWLOCK(40, ReplicationOrigin)
PG_LWLOCK(41, MultiXactTruncation)
/* 42 was OldSnapshotTimeMapLock */
PG_LWLOCK(43, LogicalRepWorker)
PG_LWLOCK(44, XactTruncation)
/* 45 was XactTruncationLock until removal of BackendRandomLock */
PG_LWLOCK(46, WrapLimitsVacuum)
PG_LWLOCK(47, NotifyQueueTail)
PG_LWLOCK(48, WaitEventCustom)
PG_LWLOCK(49, WALSummarizer)
PG_LWLOCK(50, DSMRegistry)
PG_LWLOCK(51, InjectionPoint)
PG_LWLOCK(52, SerialControl)
PG_LWLOCK(53, WaitLSN)
