/*-------------------------------------------------------------------------
 *
 * ipci.c
 *	  POSTGRES inter-process communication initialization code.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/ipci.c,v 1.57 2003/09/12 02:13:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "miscadmin.h"
#include "access/clog.h"
#include "access/xlog.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/pg_sema.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "storage/spin.h"


/*
 * CreateSharedMemoryAndSemaphores
 *		Creates and initializes shared memory and semaphores.
 *
 * This is called by the postmaster or by a standalone backend.
 * It is NEVER called by a backend forked from the postmaster;
 * for such a backend, the shared memory is already ready-to-go.
 *
 * If "makePrivate" is true then we only need private memory, not shared
 * memory.	This is true for a standalone backend, false for a postmaster.
 */
void
CreateSharedMemoryAndSemaphores(bool makePrivate,
								int maxBackends,
								int port)
{
	int			size;
	int			numSemas;
	PGShmemHeader *seghdr;

	/*
	 * Size of the Postgres shared-memory block is estimated via
	 * moderately-accurate estimates for the big hogs, plus 100K for the
	 * stuff that's too small to bother with estimating.
	 */
	size = BufferShmemSize();
	size += LockShmemSize(maxBackends);
	size += XLOGShmemSize();
	size += CLOGShmemSize();
	size += LWLockShmemSize();
	size += SInvalShmemSize(maxBackends);
	size += FreeSpaceShmemSize();
#ifdef STABLE_MEMORY_STORAGE
	size += MMShmemSize();
#endif
	size += 100000;
	/* might as well round it off to a multiple of a typical page size */
	size += 8192 - (size % 8192);

	elog(DEBUG3, "invoking IpcMemoryCreate(size=%d)", size);

	/*
	 * Create the shmem segment
	 */
	seghdr = PGSharedMemoryCreate(size, makePrivate, port);

	/*
	 * Create semaphores
	 */
	numSemas = ProcGlobalSemas(maxBackends);
	numSemas += SpinlockSemas();
	PGReserveSemaphores(numSemas, port);

	/*
	 * Set up shared memory allocation mechanism
	 */
	InitShmemAllocation(seghdr);

	/*
	 * Now initialize LWLocks, which do shared memory allocation and are
	 * needed for InitShmemIndex.
	 */
	CreateLWLocks();

	/*
	 * Set up shmem.c index hashtable
	 */
	InitShmemIndex();

	/*
	 * Set up xlog, clog, and buffers
	 */
	XLOGShmemInit();
	CLOGShmemInit();
	InitBufferPool();

	/*
	 * Set up lock manager
	 */
	InitLocks();
	if (InitLockTable(maxBackends) == INVALID_TABLEID)
		elog(FATAL, "could not create the lock table");

	/*
	 * Set up process table
	 */
	InitProcGlobal(maxBackends);

	/*
	 * Set up shared-inval messaging
	 */
	CreateSharedInvalidationState(maxBackends);

	/*
	 * Set up free-space map
	 */
	InitFreeSpaceMap();

	/*
	 * Set up child-to-postmaster signaling mechanism
	 */
	PMSignalInit();
}


/*
 * AttachSharedMemoryAndSemaphores
 *		Attaches to the existing shared resources.
 */
void
AttachSharedMemoryAndSemaphores(void)
{
	CLOGShmemInit();
}
