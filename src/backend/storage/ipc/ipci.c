/*-------------------------------------------------------------------------
 *
 * ipci.c
 *	  POSTGRES inter-process communication initialization code.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/ipci.c,v 1.74 2004/12/31 22:00:56 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/subtrans.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "postmaster/postmaster.h"
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
 * It is also called by a backend forked from the postmaster in the
 * EXEC_BACKEND case.  In the latter case, the shared memory segment
 * already exists and has been physically attached to, but we have to
 * initialize pointers in local memory that reference the shared structures,
 * because we didn't inherit the correct pointer values from the postmaster
 * as we do in the fork() scenario.  The easiest way to do that is to run
 * through the same code as before.  (Note that the called routines mostly
 * check IsUnderPostmaster, rather than EXEC_BACKEND, to detect this case.
 * This is a bit code-wasteful and could be cleaned up.)
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
		int			size;
		int			numSemas;

		/*
		 * Size of the Postgres shared-memory block is estimated via
		 * moderately-accurate estimates for the big hogs, plus 100K for
		 * the stuff that's too small to bother with estimating.
		 */
		size = hash_estimate_size(SHMEM_INDEX_SIZE, sizeof(ShmemIndexEnt));
		size += BufferShmemSize();
		size += LockShmemSize(maxBackends);
		size += ProcGlobalShmemSize(maxBackends);
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
		 * We are reattaching to an existing shared memory segment.
		 * This should only be reached in the EXEC_BACKEND case, and
		 * even then only with makePrivate == false.
		 */
#ifdef EXEC_BACKEND
		Assert(!makePrivate);
		Assert(UsedShmemSegAddr != NULL);
		seghdr = UsedShmemSegAddr;
#else
		elog(PANIC, "should be attached to shared memory already");
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
