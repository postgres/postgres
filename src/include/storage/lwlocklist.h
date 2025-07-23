/*-------------------------------------------------------------------------
 *
 * lwlocklist.h
 *
 * The list of predefined LWLocks and built-in LWLock tranches is kept in
 * its own source file for use by automatic tools.  The exact
 * representation of a keyword is determined by the PG_LWLOCK and
 * PG_LWLOCKTRANCHE macros, which are not defined in this file; they can be
 * defined by the caller for special purposes.
 *
 * Also, generate-lwlocknames.pl processes this file to create lwlocknames.h.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
/* 7 was WALBufMapping */
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
PG_LWLOCK(53, AioWorkerSubmissionQueue)

/*
 * There also exist several built-in LWLock tranches.  As with the predefined
 * LWLocks, be sure to update the WaitEventLWLock section of
 * src/backend/utils/activity/wait_event_names.txt when modifying this list.
 *
 * Note that the IDs here (the first value) don't include the LWTRANCHE_
 * prefix.  It's added elsewhere.
 */
PG_LWLOCKTRANCHE(XACT_BUFFER, XactBuffer)
PG_LWLOCKTRANCHE(COMMITTS_BUFFER, CommitTsBuffer)
PG_LWLOCKTRANCHE(SUBTRANS_BUFFER, SubtransBuffer)
PG_LWLOCKTRANCHE(MULTIXACTOFFSET_BUFFER, MultiXactOffsetBuffer)
PG_LWLOCKTRANCHE(MULTIXACTMEMBER_BUFFER, MultiXactMemberBuffer)
PG_LWLOCKTRANCHE(NOTIFY_BUFFER, NotifyBuffer)
PG_LWLOCKTRANCHE(SERIAL_BUFFER, SerialBuffer)
PG_LWLOCKTRANCHE(WAL_INSERT, WALInsert)
PG_LWLOCKTRANCHE(BUFFER_CONTENT, BufferContent)
PG_LWLOCKTRANCHE(REPLICATION_ORIGIN_STATE, ReplicationOriginState)
PG_LWLOCKTRANCHE(REPLICATION_SLOT_IO, ReplicationSlotIO)
PG_LWLOCKTRANCHE(LOCK_FASTPATH, LockFastPath)
PG_LWLOCKTRANCHE(BUFFER_MAPPING, BufferMapping)
PG_LWLOCKTRANCHE(LOCK_MANAGER, LockManager)
PG_LWLOCKTRANCHE(PREDICATE_LOCK_MANAGER, PredicateLockManager)
PG_LWLOCKTRANCHE(PARALLEL_HASH_JOIN, ParallelHashJoin)
PG_LWLOCKTRANCHE(PARALLEL_BTREE_SCAN, ParallelBtreeScan)
PG_LWLOCKTRANCHE(PARALLEL_QUERY_DSA, ParallelQueryDSA)
PG_LWLOCKTRANCHE(PER_SESSION_DSA, PerSessionDSA)
PG_LWLOCKTRANCHE(PER_SESSION_RECORD_TYPE, PerSessionRecordType)
PG_LWLOCKTRANCHE(PER_SESSION_RECORD_TYPMOD, PerSessionRecordTypmod)
PG_LWLOCKTRANCHE(SHARED_TUPLESTORE, SharedTupleStore)
PG_LWLOCKTRANCHE(SHARED_TIDBITMAP, SharedTidBitmap)
PG_LWLOCKTRANCHE(PARALLEL_APPEND, ParallelAppend)
PG_LWLOCKTRANCHE(PER_XACT_PREDICATE_LIST, PerXactPredicateList)
PG_LWLOCKTRANCHE(PGSTATS_DSA, PgStatsDSA)
PG_LWLOCKTRANCHE(PGSTATS_HASH, PgStatsHash)
PG_LWLOCKTRANCHE(PGSTATS_DATA, PgStatsData)
PG_LWLOCKTRANCHE(LAUNCHER_DSA, LogicalRepLauncherDSA)
PG_LWLOCKTRANCHE(LAUNCHER_HASH, LogicalRepLauncherHash)
PG_LWLOCKTRANCHE(DSM_REGISTRY_DSA, DSMRegistryDSA)
PG_LWLOCKTRANCHE(DSM_REGISTRY_HASH, DSMRegistryHash)
PG_LWLOCKTRANCHE(COMMITTS_SLRU, CommitTsSLRU)
PG_LWLOCKTRANCHE(MULTIXACTOFFSET_SLRU, MultiXactOffsetSLRU)
PG_LWLOCKTRANCHE(MULTIXACTMEMBER_SLRU, MultiXactMemberSLRU)
PG_LWLOCKTRANCHE(NOTIFY_SLRU, NotifySLRU)
PG_LWLOCKTRANCHE(SERIAL_SLRU, SerialSLRU)
PG_LWLOCKTRANCHE(SUBTRANS_SLRU, SubtransSLRU)
PG_LWLOCKTRANCHE(XACT_SLRU, XactSLRU)
PG_LWLOCKTRANCHE(PARALLEL_VACUUM_DSA, ParallelVacuumDSA)
PG_LWLOCKTRANCHE(AIO_URING_COMPLETION, AioUringCompletion)
