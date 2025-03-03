/*-------------------------------------------------------------------------
 *
 * pg_tde_shmem.c
 *      Shared memory area to manage cache and locks.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/pg_tde_shmem.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "storage/ipc.h"
#include "common/pg_tde_shmem.h"
#include "lib/dshash.h"
#include "nodes/pg_list.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

static void tde_shmem_shutdown(int code, Datum arg);

List	   *registeredShmemRequests = NIL;
bool		shmemInited = false;

void
RegisterShmemRequest(const TDEShmemSetupRoutine *routine)
{
	Assert(shmemInited == false);
	registeredShmemRequests = lappend(registeredShmemRequests, (void *) routine);
}

Size
TdeRequiredSharedMemorySize(void)
{
	Size		sz = 0;
	ListCell   *lc;

	foreach(lc, registeredShmemRequests)
	{
		TDEShmemSetupRoutine *routine = (TDEShmemSetupRoutine *) lfirst(lc);

		if (routine->required_shared_mem_size)
			sz = add_size(sz, routine->required_shared_mem_size());
	}
	return MAXALIGN(sz);
}

int
TdeRequiredLocksCount(void)
{
	return TDE_LWLOCK_COUNT;
}

void
TdeShmemInit(void)
{
	bool		found;
	char	   *free_start;
	Size		required_shmem_size = TdeRequiredSharedMemorySize();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	/* Create or attach to the shared memory state */
	ereport(NOTICE, errmsg("TdeShmemInit: requested %ld bytes", required_shmem_size));
	free_start = ShmemInitStruct("pg_tde", required_shmem_size, &found);

	if (!found)
	{
		/* First time through ... */
		dsa_area   *dsa;
		ListCell   *lc;
		Size		used_size = 0;
		Size		dsa_area_size;

		/* Now place all shared state structures */
		foreach(lc, registeredShmemRequests)
		{
			Size		sz = 0;
			TDEShmemSetupRoutine *routine = (TDEShmemSetupRoutine *) lfirst(lc);

			if (routine->init_shared_state)
			{
				sz = routine->init_shared_state(free_start);
				used_size += MAXALIGN(sz);
				free_start += MAXALIGN(sz);
				Assert(used_size <= required_shmem_size);
			}
		}
		/* Create DSA area */
		dsa_area_size = required_shmem_size - used_size;
		Assert(dsa_area_size > 0);

		ereport(LOG, errmsg("creating DSA area of size %lu", dsa_area_size));
		dsa = dsa_create_in_place(free_start,
								  dsa_area_size,
								  LWLockNewTrancheId(), 0);
		dsa_pin(dsa);
		dsa_set_size_limit(dsa, dsa_area_size);

		/* Initialize all DSA area objects */
		foreach(lc, registeredShmemRequests)
		{
			TDEShmemSetupRoutine *routine = (TDEShmemSetupRoutine *) lfirst(lc);

			if (routine->init_dsa_area_objects)
				routine->init_dsa_area_objects(dsa, free_start);
		}
		ereport(LOG, errmsg("setting no limit to DSA area of size %lu", dsa_area_size));

		dsa_set_size_limit(dsa, -1);	/* Let it grow outside the shared
										 * memory */

		shmemInited = true;
	}
	LWLockRelease(AddinShmemInitLock);
	on_shmem_exit(tde_shmem_shutdown, (Datum) 0);
}

static void
tde_shmem_shutdown(int code, Datum arg)
{
	ListCell   *lc;

	foreach(lc, registeredShmemRequests)
	{
		TDEShmemSetupRoutine *routine = (TDEShmemSetupRoutine *) lfirst(lc);

		if (routine->shmem_kill)
			routine->shmem_kill(code, arg);
	}
}
