/*-------------------------------------------------------------------------
 *
 * ipci.c
 *	  POSTGRES inter-process communication initialization code.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/ipci.c,v 1.33 2000/04/12 17:15:37 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/sinval.h"

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
	extern int	XLOGShmemSize(void);
	extern void XLOGShmemInit(void);

#ifdef HAS_TEST_AND_SET

	/*
	 * Create shared memory for slocks
	 */
	CreateAndInitSLockMemory(IPCKeyGetSLockSharedMemoryKey(key));
#endif

	/*
	 * Kill and create the buffer manager buffer pool (and semaphore)
	 */
	CreateSpinlocks(IPCKeyGetSpinLockSemaphoreKey(key));

	/*
	 * Size of the primary shared-memory block is estimated via
	 * moderately-accurate estimates for the big hogs, plus 100K for the
	 * stuff that's too small to bother with estimating.
	 */
	size = BufferShmemSize() + LockShmemSize(maxBackends) + XLOGShmemSize();
#ifdef STABLE_MEMORY_STORAGE
	size += MMShmemSize();
#endif
	size += 100000;
	/* might as well round it off to a multiple of a K or so... */
	size += 1024 - (size % 1024);

	if (DebugLvl > 1)
	{
		fprintf(stderr, "binding ShmemCreate(key=%x, size=%d)\n",
				IPCKeyGetBufferMemoryKey(key), size);
	}
	ShmemCreate(IPCKeyGetBufferMemoryKey(key), size);
	ShmemIndexReset();
	InitShmem(key, size);
	XLOGShmemInit();
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

	CreateSharedInvalidationState(key, maxBackends);
}


/*
 * AttachSharedMemoryAndSemaphores
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
		CreateSharedMemoryAndSemaphores(key, 16);
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
	if (InitLockTable() == INVALID_TABLEID)
		elog(FATAL, "Couldn't attach to the lock table");

	AttachSharedInvalidationState(key);
}
