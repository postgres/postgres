/*-------------------------------------------------------------------------
 *
 * ipci.c--
 *	  POSTGRES inter-process communication initialization code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/ipci.c,v 1.16.2.1 1999/03/07 02:00:46 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <sys/types.h>

#include "postgres.h"

#include "storage/ipc.h"
#include "storage/multilev.h"
#include "storage/sinval.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "storage/lock.h"
#include "miscadmin.h"			/* for DebugLvl */

/*
 * SystemPortAddressCreateMemoryKey --
 *		Returns a memory key given a port address.
 */
IPCKey
SystemPortAddressCreateIPCKey(SystemPortAddress address)
{
	Assert(address < 32768);	/* XXX */

	return SystemPortAddressGetIPCKey(address);
}

/*
 * CreateSharedMemoryAndSemaphores --
 *		Creates and initializes shared memory and semaphores.
 */
/**************************************************

  CreateSharedMemoryAndSemaphores
  is called exactly *ONCE* by the postmaster.
  It is *NEVER* called by the postgres backend

  0) destroy any existing semaphores for both buffer
  and lock managers.
  1) create the appropriate *SHARED* memory segments
  for the two resource managers.

  **************************************************/

void
CreateSharedMemoryAndSemaphores(IPCKey key)
{
	int			size;

#ifdef HAS_TEST_AND_SET
	/* ---------------
	 *	create shared memory for slocks
	 * --------------
	 */
	CreateAndInitSLockMemory(IPCKeyGetSLockSharedMemoryKey(key));
#endif
	/* ----------------
	 *	kill and create the buffer manager buffer pool (and semaphore)
	 * ----------------
	 */
	CreateSpinlocks(IPCKeyGetSpinLockSemaphoreKey(key));

	/*
	 * Size of the primary shared-memory block is estimated via
	 * moderately-accurate estimates for the big hogs, plus 100K for
	 * the stuff that's too small to bother with estimating.
	 */
	size = BufferShmemSize() + LockShmemSize();
#ifdef STABLE_MEMORY_STORAGE
	size += MMShmemSize();
#endif
	size += 100000;

	if (DebugLvl > 1)
	{
		fprintf(stderr, "binding ShmemCreate(key=%x, size=%d)\n",
				IPCKeyGetBufferMemoryKey(key), size);
	}
	ShmemCreate(IPCKeyGetBufferMemoryKey(key), size);
	ShmemIndexReset();
	InitShmem(key, size);
	InitBufferPool(key);

	/* ----------------
	 *	do the lock table stuff
	 * ----------------
	 */
	InitLocks();
	if (InitMultiLevelLocks() == INVALID_TABLEID)
		elog(FATAL, "Couldn't create the lock table");

	/* ----------------
	 *	do process table stuff
	 * ----------------
	 */
	InitProcGlobal(key);
	on_shmem_exit(ProcFreeAllSemaphores, NULL);

	CreateSharedInvalidationState(key);
}


/*
 * AttachSharedMemoryAndSemaphores --
 *		Attachs existant shared memory and semaphores.
 */
void
AttachSharedMemoryAndSemaphores(IPCKey key)
{
	/* ----------------
	 *	create rather than attach if using private key
	 * ----------------
	 */
	if (key == PrivateIPCKey)
	{
		CreateSharedMemoryAndSemaphores(key);
		return;
	}

#ifdef HAS_TEST_AND_SET
	/* ----------------
	 *	attach the slock shared memory
	 * ----------------
	 */
	AttachSLockMemory(IPCKeyGetSLockSharedMemoryKey(key));
#endif
	/* ----------------
	 *	attach the buffer manager buffer pool (and semaphore)
	 * ----------------
	 */
	InitShmem(key, 0);
	InitBufferPool(key);

	/* ----------------
	 *	initialize lock table stuff
	 * ----------------
	 */
	InitLocks();
	if (InitMultiLevelLocks() == INVALID_TABLEID)
		elog(FATAL, "Couldn't attach to the lock table");

	AttachSharedInvalidationState(key);
}
