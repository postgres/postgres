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
 * A DSA can be created in or retrieved from the registry by calling
 * GetNamedDSA().  As with GetNamedDSMSegment(), if a DSA with the provided
 * name does not yet exist, it is created.  Otherwise, GetNamedDSA()
 * ensures the DSA is attached to the current backend.  This function
 * guarantees that only one backend initializes the DSA and that all other
 * backends just attach it.
 *
 * A dshash table can be created in or retrieved from the registry by
 * calling GetNamedDSHash().  As with GetNamedDSMSegment(), if a hash
 * table with the provided name does not yet exist, it is created.
 * Otherwise, GetNamedDSHash() ensures the hash table is attached to the
 * current backend.  This function guarantees that only one backend
 * initializes the table and that all other backends just attach it.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm_registry.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "lib/dshash.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#define DSMR_NAME_LEN				128

#define DSMR_DSA_TRANCHE_SUFFIX		" DSA"
#define DSMR_DSA_TRANCHE_SUFFIX_LEN (sizeof(DSMR_DSA_TRANCHE_SUFFIX) - 1)
#define DSMR_DSA_TRANCHE_NAME_LEN	(DSMR_NAME_LEN + DSMR_DSA_TRANCHE_SUFFIX_LEN)

typedef struct DSMRegistryCtxStruct
{
	dsa_handle	dsah;
	dshash_table_handle dshh;
} DSMRegistryCtxStruct;

static DSMRegistryCtxStruct *DSMRegistryCtx;

typedef struct NamedDSMState
{
	dsm_handle	handle;
	size_t		size;
} NamedDSMState;

typedef struct NamedDSAState
{
	dsa_handle	handle;
	int			tranche;
	char		tranche_name[DSMR_DSA_TRANCHE_NAME_LEN];
} NamedDSAState;

typedef struct NamedDSHState
{
	NamedDSAState dsa;
	dshash_table_handle handle;
	int			tranche;
	char		tranche_name[DSMR_NAME_LEN];
} NamedDSHState;

typedef enum DSMREntryType
{
	DSMR_ENTRY_TYPE_DSM,
	DSMR_ENTRY_TYPE_DSA,
	DSMR_ENTRY_TYPE_DSH,
} DSMREntryType;

static const char *const DSMREntryTypeNames[] =
{
	[DSMR_ENTRY_TYPE_DSM] = "segment",
	[DSMR_ENTRY_TYPE_DSA] = "area",
	[DSMR_ENTRY_TYPE_DSH] = "hash",
};

typedef struct DSMRegistryEntry
{
	char		name[DSMR_NAME_LEN];
	DSMREntryType type;
	union
	{
		NamedDSMState dsm;
		NamedDSAState dsa;
		NamedDSHState dsh;
	}			data;
} DSMRegistryEntry;

static const dshash_parameters dsh_params = {
	offsetof(DSMRegistryEntry, type),
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

	if (strlen(name) >= offsetof(DSMRegistryEntry, type))
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
		NamedDSMState *state = &entry->data.dsm;
		dsm_segment *seg;

		entry->type = DSMR_ENTRY_TYPE_DSM;

		/* Initialize the segment. */
		seg = dsm_create(size, 0);

		dsm_pin_segment(seg);
		dsm_pin_mapping(seg);
		state->handle = dsm_segment_handle(seg);
		state->size = size;
		ret = dsm_segment_address(seg);

		if (init_callback)
			(*init_callback) (ret);
	}
	else if (entry->type != DSMR_ENTRY_TYPE_DSM)
		ereport(ERROR,
				(errmsg("requested DSM segment does not match type of existing entry")));
	else if (entry->data.dsm.size != size)
		ereport(ERROR,
				(errmsg("requested DSM segment size does not match size of existing segment")));
	else
	{
		NamedDSMState *state = &entry->data.dsm;
		dsm_segment *seg;

		/* If the existing segment is not already attached, attach it now. */
		seg = dsm_find_mapping(state->handle);
		if (seg == NULL)
		{
			seg = dsm_attach(state->handle);
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

/*
 * Initialize or attach a named DSA.
 *
 * This routine returns a pointer to the DSA.  A new LWLock tranche ID will be
 * generated if needed.  Note that the lock tranche will be registered with the
 * provided name.  Also note that this should be called at most once for a
 * given DSA in each backend.
 */
dsa_area *
GetNamedDSA(const char *name, bool *found)
{
	DSMRegistryEntry *entry;
	MemoryContext oldcontext;
	dsa_area   *ret;

	Assert(found);

	if (!name || *name == '\0')
		ereport(ERROR,
				(errmsg("DSA name cannot be empty")));

	if (strlen(name) >= offsetof(DSMRegistryEntry, type))
		ereport(ERROR,
				(errmsg("DSA name too long")));

	/* Be sure any local memory allocated by DSM/DSA routines is persistent. */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* Connect to the registry. */
	init_dsm_registry();

	entry = dshash_find_or_insert(dsm_registry_table, name, found);
	if (!(*found))
	{
		NamedDSAState *state = &entry->data.dsa;

		entry->type = DSMR_ENTRY_TYPE_DSA;

		/* Initialize the LWLock tranche for the DSA. */
		state->tranche = LWLockNewTrancheId();
		strcpy(state->tranche_name, name);
		LWLockRegisterTranche(state->tranche, state->tranche_name);

		/* Initialize the DSA. */
		ret = dsa_create(state->tranche);
		dsa_pin(ret);
		dsa_pin_mapping(ret);

		/* Store handle for other backends to use. */
		state->handle = dsa_get_handle(ret);
	}
	else if (entry->type != DSMR_ENTRY_TYPE_DSA)
		ereport(ERROR,
				(errmsg("requested DSA does not match type of existing entry")));
	else
	{
		NamedDSAState *state = &entry->data.dsa;

		if (dsa_is_attached(state->handle))
			ereport(ERROR,
					(errmsg("requested DSA already attached to current process")));

		/* Initialize existing LWLock tranche for the DSA. */
		LWLockRegisterTranche(state->tranche, state->tranche_name);

		/* Attach to existing DSA. */
		ret = dsa_attach(state->handle);
		dsa_pin_mapping(ret);
	}

	dshash_release_lock(dsm_registry_table, entry);
	MemoryContextSwitchTo(oldcontext);

	return ret;
}

/*
 * Initialize or attach a named dshash table.
 *
 * This routine returns the address of the table.  The tranche_id member of
 * params is ignored; new tranche IDs will be generated if needed.  Note that
 * the DSA lock tranche will be registered with the provided name with " DSA"
 * appended.  The dshash lock tranche will be registered with the provided
 * name.  Also note that this should be called at most once for a given table
 * in each backend.
 */
dshash_table *
GetNamedDSHash(const char *name, const dshash_parameters *params, bool *found)
{
	DSMRegistryEntry *entry;
	MemoryContext oldcontext;
	dshash_table *ret;

	Assert(params);
	Assert(found);

	if (!name || *name == '\0')
		ereport(ERROR,
				(errmsg("DSHash name cannot be empty")));

	if (strlen(name) >= offsetof(DSMRegistryEntry, type))
		ereport(ERROR,
				(errmsg("DSHash name too long")));

	/* Be sure any local memory allocated by DSM/DSA routines is persistent. */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* Connect to the registry. */
	init_dsm_registry();

	entry = dshash_find_or_insert(dsm_registry_table, name, found);
	if (!(*found))
	{
		NamedDSAState *dsa_state = &entry->data.dsh.dsa;
		NamedDSHState *dsh_state = &entry->data.dsh;
		dshash_parameters params_copy;
		dsa_area   *dsa;

		entry->type = DSMR_ENTRY_TYPE_DSH;

		/* Initialize the LWLock tranche for the DSA. */
		dsa_state->tranche = LWLockNewTrancheId();
		sprintf(dsa_state->tranche_name, "%s%s", name, DSMR_DSA_TRANCHE_SUFFIX);
		LWLockRegisterTranche(dsa_state->tranche, dsa_state->tranche_name);

		/* Initialize the LWLock tranche for the dshash table. */
		dsh_state->tranche = LWLockNewTrancheId();
		strcpy(dsh_state->tranche_name, name);
		LWLockRegisterTranche(dsh_state->tranche, dsh_state->tranche_name);

		/* Initialize the DSA for the hash table. */
		dsa = dsa_create(dsa_state->tranche);
		dsa_pin(dsa);
		dsa_pin_mapping(dsa);

		/* Initialize the dshash table. */
		memcpy(&params_copy, params, sizeof(dshash_parameters));
		params_copy.tranche_id = dsh_state->tranche;
		ret = dshash_create(dsa, &params_copy, NULL);

		/* Store handles for other backends to use. */
		dsa_state->handle = dsa_get_handle(dsa);
		dsh_state->handle = dshash_get_hash_table_handle(ret);
	}
	else if (entry->type != DSMR_ENTRY_TYPE_DSH)
		ereport(ERROR,
				(errmsg("requested DSHash does not match type of existing entry")));
	else
	{
		NamedDSAState *dsa_state = &entry->data.dsh.dsa;
		NamedDSHState *dsh_state = &entry->data.dsh;
		dsa_area   *dsa;

		/* XXX: Should we verify params matches what table was created with? */

		if (dsa_is_attached(dsa_state->handle))
			ereport(ERROR,
					(errmsg("requested DSHash already attached to current process")));

		/* Initialize existing LWLock tranches for the DSA and dshash table. */
		LWLockRegisterTranche(dsa_state->tranche, dsa_state->tranche_name);
		LWLockRegisterTranche(dsh_state->tranche, dsh_state->tranche_name);

		/* Attach to existing DSA for the hash table. */
		dsa = dsa_attach(dsa_state->handle);
		dsa_pin_mapping(dsa);

		/* Attach to existing dshash table. */
		ret = dshash_attach(dsa, params, dsh_state->handle, NULL);
	}

	dshash_release_lock(dsm_registry_table, entry);
	MemoryContextSwitchTo(oldcontext);

	return ret;
}

Datum
pg_get_dsm_registry_allocations(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	DSMRegistryEntry *entry;
	MemoryContext oldcontext;
	dshash_seq_status status;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	/* Be sure any local memory allocated by DSM/DSA routines is persistent. */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	init_dsm_registry();
	MemoryContextSwitchTo(oldcontext);

	dshash_seq_init(&status, dsm_registry_table, false);
	while ((entry = dshash_seq_next(&status)) != NULL)
	{
		Datum		vals[3];
		bool		nulls[3] = {0};

		vals[0] = CStringGetTextDatum(entry->name);
		vals[1] = CStringGetTextDatum(DSMREntryTypeNames[entry->type]);

		/*
		 * Since we can't know the size of DSA/dshash entries without first
		 * attaching to them, return NULL for those.
		 */
		if (entry->type == DSMR_ENTRY_TYPE_DSM)
			vals[2] = Int64GetDatum(entry->data.dsm.size);
		else
			nulls[2] = true;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
	}
	dshash_seq_term(&status);

	return (Datum) 0;
}
