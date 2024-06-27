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
#include "nodes/pg_list.h"
#include "storage/lwlock.h"

typedef struct TdeSharedState
{
	LWLock *principalKeyLock;
	int principalKeyHashTrancheId;
	void *rawDsaArea; /* DSA area pointer to store cache hashes */
	dshash_table_handle principalKeyHashHandle;
} TdeSharedState;

typedef struct TDELocalState
{
	TdeSharedState *sharedTdeState;
	dsa_area **dsa; /* local dsa area for backend attached to the
					 * dsa area created by postmaster at startup.
					 */
	dshash_table *principalKeySharedHash;
} TDELocalState;

static void tde_shmem_shutdown(int code, Datum arg);

List *registeredShmemRequests = NIL;
bool shmemInited = false;

void RegisterShmemRequest(const TDEShmemSetupRoutine *routine)
{
	Assert(shmemInited == false);
	registeredShmemRequests = lappend(registeredShmemRequests, (void *)routine);
}

Size TdeRequiredSharedMemorySize(void)
{
	Size sz = 0;
	ListCell *lc;
	foreach (lc, registeredShmemRequests)
	{
		TDEShmemSetupRoutine *routine = (TDEShmemSetupRoutine *)lfirst(lc);
		if (routine->required_shared_mem_size)
			sz = add_size(sz, routine->required_shared_mem_size());
	}
	sz = add_size(sz, sizeof(TdeSharedState));
	return MAXALIGN(sz);
}

int TdeRequiredLocksCount(void)
{
	int count = 0;
	ListCell *lc;
	foreach (lc, registeredShmemRequests)
	{
		TDEShmemSetupRoutine *routine = (TDEShmemSetupRoutine *)lfirst(lc);
		if (routine->required_locks_count)
			count += routine->required_locks_count();
	}
	return count;
}

void TdeShmemInit(void)
{
	bool found;
	TdeSharedState *tdeState;
	Size required_shmem_size = TdeRequiredSharedMemorySize();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	/* Create or attach to the shared memory state */
	ereport(NOTICE, (errmsg("TdeShmemInit: requested %ld bytes", required_shmem_size)));
	tdeState = ShmemInitStruct("pg_tde", required_shmem_size, &found);

	if (!found)
	{
		/* First time through ... */
		char *p = (char *)tdeState;
		dsa_area *dsa;
		ListCell *lc;
		Size used_size = 0;
		Size dsa_area_size;

		p += MAXALIGN(sizeof(TdeSharedState));
		used_size += MAXALIGN(sizeof(TdeSharedState));
		/* Now place all shared state structures */
		foreach (lc, registeredShmemRequests)
		{
			Size sz = 0;
			TDEShmemSetupRoutine *routine = (TDEShmemSetupRoutine *)lfirst(lc);
			if (routine->init_shared_state)
			{
				sz = routine->init_shared_state(p);
				used_size += MAXALIGN(sz);
				p += MAXALIGN(sz);
				Assert(used_size <= required_shmem_size);
			}
		}
		/* Create DSA area */
		dsa_area_size = required_shmem_size - used_size;
		Assert(dsa_area_size > 0);
		tdeState->rawDsaArea = p;

		ereport(LOG, (errmsg("creating DSA area of size %lu", dsa_area_size)));
		dsa = dsa_create_in_place(tdeState->rawDsaArea,
								  dsa_area_size,
								  LWLockNewTrancheId(), 0);
		dsa_pin(dsa);
		dsa_set_size_limit(dsa, dsa_area_size);

		/* Initialize all DSA area objects */
		foreach (lc, registeredShmemRequests)
		{
			TDEShmemSetupRoutine *routine = (TDEShmemSetupRoutine *)lfirst(lc);
			if (routine->init_dsa_area_objects)
				routine->init_dsa_area_objects(dsa, tdeState->rawDsaArea);
		}
		ereport(LOG, (errmsg("setting no limit to DSA area of size %lu", dsa_area_size)));

		dsa_set_size_limit(dsa, -1); /* Let it grow outside the shared memory */

		shmemInited = true;
	}
	LWLockRelease(AddinShmemInitLock);
	on_shmem_exit(tde_shmem_shutdown, (Datum)0);
}

static void
tde_shmem_shutdown(int code, Datum arg)
{
	ListCell *lc;
	foreach (lc, registeredShmemRequests)
	{
		TDEShmemSetupRoutine *routine = (TDEShmemSetupRoutine *)lfirst(lc);
		if (routine->shmem_kill)
			routine->shmem_kill(code, arg);
	}
}

/*
 * Returns a lock from registered named tranch.
 * You must already have indicated number of required locks
 * through required_locks_count callback before requesting
 * the lock from this function.
 */
LWLock*
GetNewLWLock(void)
{
	return &(GetNamedLWLockTranche(TDE_TRANCHE_NAME))->lock;
}
