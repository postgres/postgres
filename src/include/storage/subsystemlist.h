/*---------------------------------------------------------------------------
 * subsystemlist.h
 *
 * List of initialization callbacks of built-in subsystems. This is kept in
 * its own source file for possible use by automatic tools.
 * PG_SHMEM_SUBSYSTEM is defined in the callers depending on how the list is
 * used.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/subsystemlist.h
 *---------------------------------------------------------------------------
 */

/* there is deliberately not an #ifndef SUBSYSTEMLIST_H here */

/*
 * Note: there are some inter-dependencies between these, so the order of some
 * of these matter.
 */

/*
 * LWLocks first, in case any of the other shmem init functions use LWLocks.
 * (Nothing else can be running during startup, so they don't need to do any
 * locking yet, but we nevertheless allow it.)
 */
PG_SHMEM_SUBSYSTEM(LWLockCallbacks)

PG_SHMEM_SUBSYSTEM(dsm_shmem_callbacks)
PG_SHMEM_SUBSYSTEM(DSMRegistryShmemCallbacks)

/* xlog, clog, and buffers */
PG_SHMEM_SUBSYSTEM(VarsupShmemCallbacks)
PG_SHMEM_SUBSYSTEM(CLOGShmemCallbacks)
PG_SHMEM_SUBSYSTEM(CommitTsShmemCallbacks)
PG_SHMEM_SUBSYSTEM(SUBTRANSShmemCallbacks)
PG_SHMEM_SUBSYSTEM(MultiXactShmemCallbacks)

/* predicate lock manager */
PG_SHMEM_SUBSYSTEM(PredicateLockShmemCallbacks)

/* process table */
PG_SHMEM_SUBSYSTEM(ProcGlobalShmemCallbacks)
PG_SHMEM_SUBSYSTEM(ProcArrayShmemCallbacks)

/* shared-inval messaging */
PG_SHMEM_SUBSYSTEM(SharedInvalShmemCallbacks)

/* interprocess signaling mechanisms */
PG_SHMEM_SUBSYSTEM(PMSignalShmemCallbacks)
PG_SHMEM_SUBSYSTEM(ProcSignalShmemCallbacks)

/* other modules that need some shared memory space */
PG_SHMEM_SUBSYSTEM(AsyncShmemCallbacks)

/* AIO subsystem. This delegates to the method-specific callbacks */
PG_SHMEM_SUBSYSTEM(AioShmemCallbacks)
