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
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/ipci.c,v 1.69 2004/07/01 00:50:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/subtrans.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
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
 * It is also called by a backend forked from the postmaster under
 * the EXEC_BACKEND case.  (In the non EXEC_BACKEND case, backends
 * start life already attached to shared memory.)  The initialization
 * functions are set up to simply "attach" to pre-existing shared memory
 * structures in the latter case.  We have to do that in order to
 * initialize pointers in local memory that reference the shared structures.
 * (In the non EXEC_BACKEND case, these pointer values are inherited via
 * fork() from the postmaster.)
 *
 * If "makePrivate" is true then we only need private memory, not shared
 * memory.	This is true for a standalone backend, false for a postmaster.
 */
void
CreateSharedMemoryAndSemaphores(bool makePrivate,
								int maxBackends,
								int port)
{
	PGShmemHeader *seghdr = NULL;
	if (!IsUnderPostmaster)
	{
		int	size;
		int	numSemas;

		/*
		 * Size of the Postgres shared-memory block is estimated via
		 * moderately-accurate estimates for the big hogs, plus 100K for the
		 * stuff that's too small to bother with estimating.
		 */
		size = hash_estimate_size(SHMEM_INDEX_SIZE, sizeof(ShmemIndexEnt));
		size += BufferShmemSize();
		size += LockShmemSize(maxBackends);
		size += XLOGShmemSize();
		size += CLOGShmemSize();
		size += SUBTRANSShmemSize();
		size += LWLockShmemSize();
		size += SInvalShmemSize(maxBackends);
		size += FreeSpaceShmemSize();
		size += BgWriterShmemSize();
#ifdef EXEC_BACKEND
		size += ShmemBackendArraySize();
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
	}
	else
	{
		/*
		 * Attach to the shmem segment.
		 * (this should only ever be reached by EXEC_BACKEND code,
		 *  and only then with makePrivate == false)
		 */
#ifdef EXEC_BACKEND
		Assert(!makePrivate);
		seghdr = PGSharedMemoryCreate(-1, makePrivate, 0);
#else
		Assert(false);
#endif
	}


	/*
	 * Set up shared memory allocation mechanism
	 */
	InitShmemAllocation(seghdr, !IsUnderPostmaster);

	/*
	 * Now initialize LWLocks, which do shared memory allocation and are
	 * needed for InitShmemIndex.
	 */
	if (!IsUnderPostmaster)
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
	SUBTRANSShmemInit();
	InitBufferPool();

	/*
	 * Set up lock manager
	 */
	InitLocks();
	InitLockTable(maxBackends);

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
	 * Set up interprocess signaling mechanisms
	 */
	PMSignalInit();
	BgWriterShmemInit();

#ifdef EXEC_BACKEND
	/*
	 * Alloc the win32 shared backend array
	 */
	if (!IsUnderPostmaster)
		ShmemBackendArrayAllocation();
#endif
}
