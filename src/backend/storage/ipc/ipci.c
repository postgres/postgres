/*-------------------------------------------------------------------------
 *
 * ipci.c
 *	  POSTGRES inter-process communication initialization code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/ipci.c,v 1.40 2001/03/22 03:59:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>

#include "miscadmin.h"
#include "access/xlog.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
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
CreateSharedMemoryAndSemaphores(bool makePrivate, int maxBackends)
{
	int			size;
	PGShmemHeader *seghdr;

	/*
	 * Size of the Postgres shared-memory block is estimated via
	 * moderately-accurate estimates for the big hogs, plus 100K for the
	 * stuff that's too small to bother with estimating.
	 */
	size = BufferShmemSize() + LockShmemSize(maxBackends) +
		XLOGShmemSize() + SLockShmemSize() + SInvalShmemSize(maxBackends);
#ifdef STABLE_MEMORY_STORAGE
	size += MMShmemSize();
#endif
	size += 100000;
	/* might as well round it off to a multiple of a typical page size */
	size += 8192 - (size % 8192);

	if (DebugLvl > 1)
		fprintf(stderr, "invoking IpcMemoryCreate(size=%d)\n", size);

	/*
	 * Create the shmem segment
	 */
	seghdr = IpcMemoryCreate(size, makePrivate, IPCProtection);

	/*
	 * First initialize spinlocks --- needed by InitShmemAllocation()
	 */
	CreateSpinlocks(seghdr);

	/*
	 * Set up shmem.c hashtable
	 */
	InitShmemAllocation(seghdr);

	/*
	 * Set up xlog and buffers
	 */
	XLOGShmemInit();
	InitBufferPool();

	/*
	 * Set up lock manager
	 */
	InitLocks();
	if (InitLockTable(maxBackends) == INVALID_TABLEID)
		elog(FATAL, "Couldn't create the lock table");

	/*
	 * Set up process table
	 */
	InitProcGlobal(maxBackends);

	/*
	 * Set up shared-inval messaging
	 */
	CreateSharedInvalidationState(maxBackends);
}
