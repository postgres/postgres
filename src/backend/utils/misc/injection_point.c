/*-------------------------------------------------------------------------
 *
 * injection_point.c
 *	  Routines to control and run injection points in the code.
 *
 * Injection points can be used to run arbitrary code by attaching callbacks
 * that would be executed in place of the named injection point.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/injection_point.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/injection_point.h"

#ifdef USE_INJECTION_POINTS

#include <sys/stat.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/* Field sizes */
#define INJ_NAME_MAXLEN		64
#define INJ_LIB_MAXLEN		128
#define INJ_FUNC_MAXLEN		128
#define INJ_PRIVATE_MAXLEN	1024

/* Single injection point stored in shared memory */
typedef struct InjectionPointEntry
{
	/*
	 * Because injection points need to be usable without LWLocks, we use a
	 * generation counter on each entry to allow safe, lock-free reading.
	 *
	 * To read an entry, first read the current 'generation' value.  If it's
	 * even, then the slot is currently unused, and odd means it's in use.
	 * When reading the other fields, beware that they may change while
	 * reading them, if the entry is released and reused!  After reading the
	 * other fields, read 'generation' again: if its value hasn't changed, you
	 * can be certain that the other fields you read are valid.  Otherwise,
	 * the slot was concurrently recycled, and you should ignore it.
	 *
	 * When adding an entry, you must store all the other fields first, and
	 * then update the generation number, with an appropriate memory barrier
	 * in between. In addition to that protocol, you must also hold
	 * InjectionPointLock, to prevent two backends from modifying the array at
	 * the same time.
	 */
	pg_atomic_uint64 generation;

	char		name[INJ_NAME_MAXLEN];	/* point name */
	char		library[INJ_LIB_MAXLEN];	/* library */
	char		function[INJ_FUNC_MAXLEN];	/* function */

	/*
	 * Opaque data area that modules can use to pass some custom data to
	 * callbacks, registered when attached.
	 */
	char		private_data[INJ_PRIVATE_MAXLEN];
} InjectionPointEntry;

#define MAX_INJECTION_POINTS	128

/*
 * Shared memory array of active injection points.
 *
 * 'max_inuse' is the highest index currently in use, plus one.  It's just an
 * optimization to avoid scanning through the whole entry, in the common case
 * that there are no injection points, or only a few.
 */
typedef struct InjectionPointsCtl
{
	pg_atomic_uint32 max_inuse;
	InjectionPointEntry entries[MAX_INJECTION_POINTS];
} InjectionPointsCtl;

NON_EXEC_STATIC InjectionPointsCtl *ActiveInjectionPoints;

/*
 * Backend local cache of injection callbacks already loaded, stored in
 * TopMemoryContext.
 */
typedef struct InjectionPointCacheEntry
{
	char		name[INJ_NAME_MAXLEN];
	char		private_data[INJ_PRIVATE_MAXLEN];
	InjectionPointCallback callback;

	/*
	 * Shmem slot and copy of its generation number when this cache entry was
	 * created.  They can be used to validate if the cached entry is still
	 * valid.
	 */
	int			slot_idx;
	uint64		generation;
} InjectionPointCacheEntry;

static HTAB *InjectionPointCache = NULL;

/*
 * injection_point_cache_add
 *
 * Add an injection point to the local cache.
 */
static InjectionPointCacheEntry *
injection_point_cache_add(const char *name,
						  int slot_idx,
						  uint64 generation,
						  InjectionPointCallback callback,
						  const void *private_data)
{
	InjectionPointCacheEntry *entry;
	bool		found;

	/* If first time, initialize */
	if (InjectionPointCache == NULL)
	{
		HASHCTL		hash_ctl;

		hash_ctl.keysize = sizeof(char[INJ_NAME_MAXLEN]);
		hash_ctl.entrysize = sizeof(InjectionPointCacheEntry);
		hash_ctl.hcxt = TopMemoryContext;

		InjectionPointCache = hash_create("InjectionPoint cache hash",
										  MAX_INJECTION_POINTS,
										  &hash_ctl,
										  HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
	}

	entry = (InjectionPointCacheEntry *)
		hash_search(InjectionPointCache, name, HASH_ENTER, &found);

	Assert(!found);
	strlcpy(entry->name, name, sizeof(entry->name));
	entry->slot_idx = slot_idx;
	entry->generation = generation;
	entry->callback = callback;
	memcpy(entry->private_data, private_data, INJ_PRIVATE_MAXLEN);

	return entry;
}

/*
 * injection_point_cache_remove
 *
 * Remove entry from the local cache.  Note that this leaks a callback
 * loaded but removed later on, which should have no consequence from
 * a testing perspective.
 */
static void
injection_point_cache_remove(const char *name)
{
	bool		found PG_USED_FOR_ASSERTS_ONLY;

	(void) hash_search(InjectionPointCache, name, HASH_REMOVE, &found);
	Assert(found);
}

/*
 * injection_point_cache_load
 *
 * Load an injection point into the local cache.
 */
static InjectionPointCacheEntry *
injection_point_cache_load(InjectionPointEntry *entry, int slot_idx, uint64 generation)
{
	char		path[MAXPGPATH];
	void	   *injection_callback_local;

	snprintf(path, MAXPGPATH, "%s/%s%s", pkglib_path,
			 entry->library, DLSUFFIX);

	if (!pg_file_exists(path))
		elog(ERROR, "could not find library \"%s\" for injection point \"%s\"",
			 path, entry->name);

	injection_callback_local = (void *)
		load_external_function(path, entry->function, false, NULL);

	if (injection_callback_local == NULL)
		elog(ERROR, "could not find function \"%s\" in library \"%s\" for injection point \"%s\"",
			 entry->function, path, entry->name);

	/* add it to the local cache */
	return injection_point_cache_add(entry->name,
									 slot_idx,
									 generation,
									 injection_callback_local,
									 entry->private_data);
}

/*
 * injection_point_cache_get
 *
 * Retrieve an injection point from the local cache, if any.
 */
static InjectionPointCacheEntry *
injection_point_cache_get(const char *name)
{
	bool		found;
	InjectionPointCacheEntry *entry;

	/* no callback if no cache yet */
	if (InjectionPointCache == NULL)
		return NULL;

	entry = (InjectionPointCacheEntry *)
		hash_search(InjectionPointCache, name, HASH_FIND, &found);

	if (found)
		return entry;

	return NULL;
}
#endif							/* USE_INJECTION_POINTS */

/*
 * Return the space for dynamic shared hash table.
 */
Size
InjectionPointShmemSize(void)
{
#ifdef USE_INJECTION_POINTS
	Size		sz = 0;

	sz = add_size(sz, sizeof(InjectionPointsCtl));
	return sz;
#else
	return 0;
#endif
}

/*
 * Allocate shmem space for dynamic shared hash.
 */
void
InjectionPointShmemInit(void)
{
#ifdef USE_INJECTION_POINTS
	bool		found;

	ActiveInjectionPoints = ShmemInitStruct("InjectionPoint hash",
											sizeof(InjectionPointsCtl),
											&found);
	if (!IsUnderPostmaster)
	{
		Assert(!found);
		pg_atomic_init_u32(&ActiveInjectionPoints->max_inuse, 0);
		for (int i = 0; i < MAX_INJECTION_POINTS; i++)
			pg_atomic_init_u64(&ActiveInjectionPoints->entries[i].generation, 0);
	}
	else
		Assert(found);
#endif
}

/*
 * Attach a new injection point.
 */
void
InjectionPointAttach(const char *name,
					 const char *library,
					 const char *function,
					 const void *private_data,
					 int private_data_size)
{
#ifdef USE_INJECTION_POINTS
	InjectionPointEntry *entry;
	uint64		generation;
	uint32		max_inuse;
	int			free_idx;

	if (strlen(name) >= INJ_NAME_MAXLEN)
		elog(ERROR, "injection point name %s too long (maximum of %u)",
			 name, INJ_NAME_MAXLEN);
	if (strlen(library) >= INJ_LIB_MAXLEN)
		elog(ERROR, "injection point library %s too long (maximum of %u)",
			 library, INJ_LIB_MAXLEN);
	if (strlen(function) >= INJ_FUNC_MAXLEN)
		elog(ERROR, "injection point function %s too long (maximum of %u)",
			 function, INJ_FUNC_MAXLEN);
	if (private_data_size >= INJ_PRIVATE_MAXLEN)
		elog(ERROR, "injection point data too long (maximum of %u)",
			 INJ_PRIVATE_MAXLEN);

	/*
	 * Allocate and register a new injection point.  A new point should not
	 * exist.  For testing purposes this should be fine.
	 */
	LWLockAcquire(InjectionPointLock, LW_EXCLUSIVE);
	max_inuse = pg_atomic_read_u32(&ActiveInjectionPoints->max_inuse);
	free_idx = -1;

	for (int idx = 0; idx < max_inuse; idx++)
	{
		entry = &ActiveInjectionPoints->entries[idx];
		generation = pg_atomic_read_u64(&entry->generation);
		if (generation % 2 == 0)
		{
			/*
			 * Found a free slot where we can add the new entry, but keep
			 * going so that we will find out if the entry already exists.
			 */
			if (free_idx == -1)
				free_idx = idx;
		}
		else if (strcmp(entry->name, name) == 0)
			elog(ERROR, "injection point \"%s\" already defined", name);
	}
	if (free_idx == -1)
	{
		if (max_inuse == MAX_INJECTION_POINTS)
			elog(ERROR, "too many injection points");
		free_idx = max_inuse;
	}
	entry = &ActiveInjectionPoints->entries[free_idx];
	generation = pg_atomic_read_u64(&entry->generation);
	Assert(generation % 2 == 0);

	/* Save the entry */
	strlcpy(entry->name, name, sizeof(entry->name));
	entry->name[INJ_NAME_MAXLEN - 1] = '\0';
	strlcpy(entry->library, library, sizeof(entry->library));
	entry->library[INJ_LIB_MAXLEN - 1] = '\0';
	strlcpy(entry->function, function, sizeof(entry->function));
	entry->function[INJ_FUNC_MAXLEN - 1] = '\0';
	if (private_data != NULL)
		memcpy(entry->private_data, private_data, private_data_size);

	pg_write_barrier();
	pg_atomic_write_u64(&entry->generation, generation + 1);

	if (free_idx + 1 > max_inuse)
		pg_atomic_write_u32(&ActiveInjectionPoints->max_inuse, free_idx + 1);

	LWLockRelease(InjectionPointLock);

#else
	elog(ERROR, "injection points are not supported by this build");
#endif
}

/*
 * Detach an existing injection point.
 *
 * Returns true if the injection point was detached, false otherwise.
 */
bool
InjectionPointDetach(const char *name)
{
#ifdef USE_INJECTION_POINTS
	bool		found = false;
	int			idx;
	int			max_inuse;

	LWLockAcquire(InjectionPointLock, LW_EXCLUSIVE);

	/* Find it in the shmem array, and mark the slot as unused */
	max_inuse = (int) pg_atomic_read_u32(&ActiveInjectionPoints->max_inuse);
	for (idx = max_inuse - 1; idx >= 0; --idx)
	{
		InjectionPointEntry *entry = &ActiveInjectionPoints->entries[idx];
		uint64		generation;

		generation = pg_atomic_read_u64(&entry->generation);
		if (generation % 2 == 0)
			continue;			/* empty slot */

		if (strcmp(entry->name, name) == 0)
		{
			Assert(!found);
			found = true;
			pg_atomic_write_u64(&entry->generation, generation + 1);
			break;
		}
	}

	/* If we just removed the highest-numbered entry, update 'max_inuse' */
	if (found && idx == max_inuse - 1)
	{
		for (; idx >= 0; --idx)
		{
			InjectionPointEntry *entry = &ActiveInjectionPoints->entries[idx];
			uint64		generation;

			generation = pg_atomic_read_u64(&entry->generation);
			if (generation % 2 != 0)
				break;
		}
		pg_atomic_write_u32(&ActiveInjectionPoints->max_inuse, idx + 1);
	}
	LWLockRelease(InjectionPointLock);

	return found;
#else
	elog(ERROR, "Injection points are not supported by this build");
	return true;				/* silence compiler */
#endif
}

#ifdef USE_INJECTION_POINTS
/*
 * Common workhorse of InjectionPointRun() and InjectionPointLoad()
 *
 * Checks if an injection point exists in shared memory, and update
 * the local cache entry accordingly.
 */
static InjectionPointCacheEntry *
InjectionPointCacheRefresh(const char *name)
{
	uint32		max_inuse;
	int			namelen;
	InjectionPointEntry local_copy;
	InjectionPointCacheEntry *cached;

	/*
	 * First read the number of in-use slots.  More entries can be added or
	 * existing ones can be removed while we're reading them.  If the entry
	 * we're looking for is concurrently added or removed, we might or might
	 * not see it.  That's OK.
	 */
	max_inuse = pg_atomic_read_u32(&ActiveInjectionPoints->max_inuse);
	if (max_inuse == 0)
	{
		if (InjectionPointCache)
		{
			hash_destroy(InjectionPointCache);
			InjectionPointCache = NULL;
		}
		return NULL;
	}

	/*
	 * If we have this entry in the local cache already, check if the cached
	 * entry is still valid.
	 */
	cached = injection_point_cache_get(name);
	if (cached)
	{
		int			idx = cached->slot_idx;
		InjectionPointEntry *entry = &ActiveInjectionPoints->entries[idx];

		if (pg_atomic_read_u64(&entry->generation) == cached->generation)
		{
			/* still good */
			return cached;
		}
		injection_point_cache_remove(name);
		cached = NULL;
	}

	/*
	 * Search the shared memory array.
	 *
	 * It's possible that the entry we're looking for is concurrently detached
	 * or attached.  Or detached *and* re-attached, to the same slot or a
	 * different slot.  Detach and re-attach is not an atomic operation, so
	 * it's OK for us to return the old value, NULL, or the new value in such
	 * cases.
	 */
	namelen = strlen(name);
	for (int idx = 0; idx < max_inuse; idx++)
	{
		InjectionPointEntry *entry = &ActiveInjectionPoints->entries[idx];
		uint64		generation;

		/*
		 * Read the generation number so that we can detect concurrent
		 * modifications.  The read barrier ensures that the generation number
		 * is loaded before any of the other fields.
		 */
		generation = pg_atomic_read_u64(&entry->generation);
		if (generation % 2 == 0)
			continue;			/* empty slot */
		pg_read_barrier();

		/* Is this the injection point we're looking for? */
		if (memcmp(entry->name, name, namelen + 1) != 0)
			continue;

		/*
		 * The entry can change at any time, if the injection point is
		 * concurrently detached.  Copy it to local memory, and re-check the
		 * generation.  If the generation hasn't changed, we know our local
		 * copy is coherent.
		 */
		memcpy(&local_copy, entry, sizeof(InjectionPointEntry));

		pg_read_barrier();
		if (pg_atomic_read_u64(&entry->generation) != generation)
		{
			/*
			 * The entry was concurrently detached.
			 *
			 * Continue the search, because if the generation number changed,
			 * we cannot trust the result of the name comparison we did above.
			 * It's theoretically possible that it falsely matched a mixed-up
			 * state of the old and new name, if the slot was recycled with a
			 * different name.
			 */
			continue;
		}

		/* Success! Load it into the cache and return it */
		return injection_point_cache_load(&local_copy, idx, generation);
	}
	return NULL;
}
#endif

/*
 * Load an injection point into the local cache.
 *
 * This is useful to be able to load an injection point before running it,
 * especially if the injection point is called in a code path where memory
 * allocations cannot happen, like critical sections.
 */
void
InjectionPointLoad(const char *name)
{
#ifdef USE_INJECTION_POINTS
	InjectionPointCacheRefresh(name);
#else
	elog(ERROR, "Injection points are not supported by this build");
#endif
}

/*
 * Execute an injection point, if defined.
 */
void
InjectionPointRun(const char *name, void *arg)
{
#ifdef USE_INJECTION_POINTS
	InjectionPointCacheEntry *cache_entry;

	cache_entry = InjectionPointCacheRefresh(name);
	if (cache_entry)
		cache_entry->callback(name, cache_entry->private_data, arg);
#else
	elog(ERROR, "Injection points are not supported by this build");
#endif
}

/*
 * Execute an injection point directly from the cache, if defined.
 */
void
InjectionPointCached(const char *name, void *arg)
{
#ifdef USE_INJECTION_POINTS
	InjectionPointCacheEntry *cache_entry;

	cache_entry = injection_point_cache_get(name);
	if (cache_entry)
		cache_entry->callback(name, cache_entry->private_data, arg);
#else
	elog(ERROR, "Injection points are not supported by this build");
#endif
}

/*
 * Test if an injection point is defined.
 */
bool
IsInjectionPointAttached(const char *name)
{
#ifdef USE_INJECTION_POINTS
	return InjectionPointCacheRefresh(name) != NULL;
#else
	elog(ERROR, "Injection points are not supported by this build");
	return false;				/* silence compiler */
#endif
}

/*
 * Retrieve a list of all the injection points currently attached.
 *
 * This list is palloc'd in the current memory context.
 */
List *
InjectionPointList(void)
{
#ifdef USE_INJECTION_POINTS
	List	   *inj_points = NIL;
	uint32		max_inuse;

	LWLockAcquire(InjectionPointLock, LW_SHARED);

	max_inuse = pg_atomic_read_u32(&ActiveInjectionPoints->max_inuse);

	for (uint32 idx = 0; idx < max_inuse; idx++)
	{
		InjectionPointEntry *entry;
		InjectionPointData *inj_point;
		uint64		generation;

		entry = &ActiveInjectionPoints->entries[idx];
		generation = pg_atomic_read_u64(&entry->generation);

		/* skip free slots */
		if (generation % 2 == 0)
			continue;

		inj_point = (InjectionPointData *) palloc0(sizeof(InjectionPointData));
		inj_point->name = pstrdup(entry->name);
		inj_point->library = pstrdup(entry->library);
		inj_point->function = pstrdup(entry->function);
		inj_points = lappend(inj_points, inj_point);
	}

	LWLockRelease(InjectionPointLock);

	return inj_points;

#else
	elog(ERROR, "Injection points are not supported by this build");
	return NIL;					/* keep compiler quiet */
#endif
}
