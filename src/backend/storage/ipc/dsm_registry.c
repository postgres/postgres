/*-------------------------------------------------------------------------
 *
 * dsm_registry.c
 *	  Functions for interfacing with the dynamic shared memory registry.
 *
 * This provides a way for libraries to use shared memory without needing
 * to request it at startup time via a shmem_request_hook.  The registry
 * stores dynamic shared memory (DSM) segment handles keyed by a
 * library-specified string.
 *
 * The registry is accessed by calling GetNamedDSMSegment().  If a segment
 * with the provided name does not yet exist, it is created and initialized
 * with the provided init_callback callback function.  Otherwise,
 * GetNamedDSMSegment() simply ensures that the segment is attached to the
 * current backend.  This function guarantees that only one backend
 * initializes the segment and that all other backends just attach it.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm_registry.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/dshash.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/memutils.h"

typedef struct DSMRegistryCtxStruct
{
	dsa_handle	dsah;
	dshash_table_handle dshh;
} DSMRegistryCtxStruct;

static DSMRegistryCtxStruct *DSMRegistryCtx;

typedef struct DSMRegistryEntry
{
	char		name[64];
	dsm_handle	handle;
	size_t		size;
} DSMRegistryEntry;

static const dshash_parameters dsh_params = {
	offsetof(DSMRegistryEntry, handle),
	sizeof(DSMRegistryEntry),
	dshash_strcmp,
	dshash_strhash,
	dshash_strcpy,
	LWTRANCHE_DSM_REGISTRY_HASH
};

static dsa_area *dsm_registry_dsa;
static dshash_table *dsm_registry_table;

Size
DSMRegistryShmemSize(void)
{
	return MAXALIGN(sizeof(DSMRegistryCtxStruct));
}

void
DSMRegistryShmemInit(void)
{
	bool		found;

	DSMRegistryCtx = (DSMRegistryCtxStruct *)
		ShmemInitStruct("DSM Registry Data",
						DSMRegistryShmemSize(),
						&found);

	if (!found)
	{
		DSMRegistryCtx->dsah = DSA_HANDLE_INVALID;
		DSMRegistryCtx->dshh = DSHASH_HANDLE_INVALID;
	}
}

/*
 * Initialize or attach to the dynamic shared hash table that stores the DSM
 * registry entries, if not already done.  This must be called before accessing
 * the table.
 */
static void
init_dsm_registry(void)
{
	/* Quick exit if we already did this. */
	if (dsm_registry_table)
		return;

	/* Otherwise, use a lock to ensure only one process creates the table. */
	LWLockAcquire(DSMRegistryLock, LW_EXCLUSIVE);

	if (DSMRegistryCtx->dshh == DSHASH_HANDLE_INVALID)
	{
		/* Initialize dynamic shared hash table for registry. */
		dsm_registry_dsa = dsa_create(LWTRANCHE_DSM_REGISTRY_DSA);
		dsa_pin(dsm_registry_dsa);
		dsa_pin_mapping(dsm_registry_dsa);
		dsm_registry_table = dshash_create(dsm_registry_dsa, &dsh_params, NULL);

		/* Store handles in shared memory for other backends to use. */
		DSMRegistryCtx->dsah = dsa_get_handle(dsm_registry_dsa);
		DSMRegistryCtx->dshh = dshash_get_hash_table_handle(dsm_registry_table);
	}
	else
	{
		/* Attach to existing dynamic shared hash table. */
		dsm_registry_dsa = dsa_attach(DSMRegistryCtx->dsah);
		dsa_pin_mapping(dsm_registry_dsa);
		dsm_registry_table = dshash_attach(dsm_registry_dsa, &dsh_params,
										   DSMRegistryCtx->dshh, NULL);
	}

	LWLockRelease(DSMRegistryLock);
}

/*
 * Initialize or attach a named DSM segment.
 *
 * This routine returns the address of the segment.  init_callback is called to
 * initialize the segment when it is first created.
 */
void *
GetNamedDSMSegment(const char *name, size_t size,
				   void (*init_callback) (void *ptr), bool *found)
{
	DSMRegistryEntry *entry;
	MemoryContext oldcontext;
	void	   *ret;

	Assert(found);

	if (!name || *name == '\0')
		ereport(ERROR,
				(errmsg("DSM segment name cannot be empty")));

	if (strlen(name) >= offsetof(DSMRegistryEntry, handle))
		ereport(ERROR,
				(errmsg("DSM segment name too long")));

	if (size == 0)
		ereport(ERROR,
				(errmsg("DSM segment size must be nonzero")));

	/* Be sure any local memory allocated by DSM/DSA routines is persistent. */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* Connect to the registry. */
	init_dsm_registry();

	entry = dshash_find_or_insert(dsm_registry_table, name, found);
	if (!(*found))
	{
		/* Initialize the segment. */
		dsm_segment *seg = dsm_create(size, 0);

		dsm_pin_segment(seg);
		dsm_pin_mapping(seg);
		entry->handle = dsm_segment_handle(seg);
		entry->size = size;
		ret = dsm_segment_address(seg);

		if (init_callback)
			(*init_callback) (ret);
	}
	else if (entry->size != size)
	{
		ereport(ERROR,
				(errmsg("requested DSM segment size does not match size of "
						"existing segment")));
	}
	else
	{
		dsm_segment *seg = dsm_find_mapping(entry->handle);

		/* If the existing segment is not already attached, attach it now. */
		if (seg == NULL)
		{
			seg = dsm_attach(entry->handle);
			if (seg == NULL)
				elog(ERROR, "could not map dynamic shared memory segment");

			dsm_pin_mapping(seg);
		}

		ret = dsm_segment_address(seg);
	}

	dshash_release_lock(dsm_registry_table, entry);
	MemoryContextSwitchTo(oldcontext);

	return ret;
}
