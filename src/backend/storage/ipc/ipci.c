/*-------------------------------------------------------------------------
 *
 * ipci.c
 *	  POSTGRES inter-process communication initialization code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/ipci.c,v 1.20 1999/02/19 07:10:47 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <sys/types.h>

#include "postgres.h"

#include "storage/ipc.h"
#include "storage/sinval.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "storage/lock.h"
#include "miscadmin.h"			/* for DebugLvl */

/*
 * SystemPortAddressCreateMemoryKey 
 *		Returns a memory key given a port address.
 */
IPCKey
SystemPortAddressCreateIPCKey(SystemPortAddress address)
{
	Assert(address < 32768);	/* XXX */

	return SystemPortAddressGetIPCKey(address);
}

/*
 * CreateSharedMemoryAndSemaphores 
 *		Creates and initializes shared memory and semaphores.
 */
/**************************************************

  CreateSharedMemoryAndSemaphores
  is called exactly *ONCE* by the postmaster.
  It is *NEVER* called by the postgres backend,
  except in the case of a standalone backend.

  0) destroy any existing semaphores for both buffer
  and lock managers.
  1) create the appropriate *SHARED* memory segments
  for the two resource managers.
  2) create shared semaphores as needed.

  **************************************************/

void
CreateSharedMemoryAndSemaphores(IPCKey key, int maxBackends)
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
	size = BufferShmemSize() + LockShmemSize();

#ifdef STABLE_MEMORY_STORAGE
	size += MMShmemSize();
#endif

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
	if (InitLockTable() == INVALID_TABLEID)
		elog(FATAL, "Couldn't create the lock table");

	/* ----------------
	 *	do process table stuff
	 * ----------------
	 */
	InitProcGlobal(key, maxBackends);

	CreateSharedInvalidationState(key);
}


/*
 * AttachSharedMemoryAndSemaphores 
 *		Attachs existant shared memory and semaphores.
 */
void
AttachSharedMemoryAndSemaphores(IPCKey key)
{
	int			size;

	/* ----------------
	 *	create rather than attach if using private key
	 * ----------------
	 */
	if (key == PrivateIPCKey)
	{
		CreateSharedMemoryAndSemaphores(key, 1);
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
	size = BufferShmemSize() + LockShmemSize();
	InitShmem(key, size);
	InitBufferPool(key);

	/* ----------------
	 *	initialize lock table stuff
	 * ----------------
	 */
	InitLocks();
	if (InitLockTable() == INVALID_TABLEID)
		elog(FATAL, "Couldn't attach to the lock table");

	AttachSharedInvalidationState(key);
}
