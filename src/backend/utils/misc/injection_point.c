/*-------------------------------------------------------------------------
 *
 * injection_point.c
 *	  Routines to control and run injection points in the code.
 *
 * Injection points can be used to run arbitrary code by attaching callbacks
 * that would be executed in place of the named injection point.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/injection_point.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"

#ifdef USE_INJECTION_POINTS

/*
 * Hash table for storing injection points.
 *
 * InjectionPointHash is used to find an injection point by name.
 */
static HTAB *InjectionPointHash;	/* find points from names */

/* Field sizes */
#define INJ_NAME_MAXLEN		64
#define INJ_LIB_MAXLEN		128
#define INJ_FUNC_MAXLEN		128
#define INJ_PRIVATE_MAXLEN	1024

/* Single injection point stored in InjectionPointHash */
typedef struct InjectionPointEntry
{
	char		name[INJ_NAME_MAXLEN];	/* hash key */
	char		library[INJ_LIB_MAXLEN];	/* library */
	char		function[INJ_FUNC_MAXLEN];	/* function */

	/*
	 * Opaque data area that modules can use to pass some custom data to
	 * callbacks, registered when attached.
	 */
	char		private_data[INJ_PRIVATE_MAXLEN];
} InjectionPointEntry;

#define INJECTION_POINT_HASH_INIT_SIZE	16
#define INJECTION_POINT_HASH_MAX_SIZE	128

/*
 * Backend local cache of injection callbacks already loaded, stored in
 * TopMemoryContext.
 */
typedef struct InjectionPointCacheEntry
{
	char		name[INJ_NAME_MAXLEN];
	char		private_data[INJ_PRIVATE_MAXLEN];
	InjectionPointCallback callback;
} InjectionPointCacheEntry;

static HTAB *InjectionPointCache = NULL;

/*
 * injection_point_cache_add
 *
 * Add an injection point to the local cache.
 */
static void
injection_point_cache_add(const char *name,
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
										  INJECTION_POINT_HASH_MAX_SIZE,
										  &hash_ctl,
										  HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
	}

	entry = (InjectionPointCacheEntry *)
		hash_search(InjectionPointCache, name, HASH_ENTER, &found);

	Assert(!found);
	strlcpy(entry->name, name, sizeof(entry->name));
	entry->callback = callback;
	if (private_data != NULL)
		memcpy(entry->private_data, private_data, INJ_PRIVATE_MAXLEN);
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
	/* leave if no cache */
	if (InjectionPointCache == NULL)
		return;

	(void) hash_search(InjectionPointCache, name, HASH_REMOVE, NULL);
}

/*
 * injection_point_cache_get
 *
 * Retrieve an injection point from the local cache, if any.
 */
static InjectionPointCallback
injection_point_cache_get(const char *name, const void **private_data)
{
	bool		found;
	InjectionPointCacheEntry *entry;

	if (private_data)
		*private_data = NULL;

	/* no callback if no cache yet */
	if (InjectionPointCache == NULL)
		return NULL;

	entry = (InjectionPointCacheEntry *)
		hash_search(InjectionPointCache, name, HASH_FIND, &found);

	if (found)
	{
		if (private_data)
			*private_data = entry->private_data;
		return entry->callback;
	}

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

	sz = add_size(sz, hash_estimate_size(INJECTION_POINT_HASH_MAX_SIZE,
										 sizeof(InjectionPointEntry)));
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
	HASHCTL		info;

	/* key is a NULL-terminated string */
	info.keysize = sizeof(char[INJ_NAME_MAXLEN]);
	info.entrysize = sizeof(InjectionPointEntry);
	InjectionPointHash = ShmemInitHash("InjectionPoint hash",
									   INJECTION_POINT_HASH_INIT_SIZE,
									   INJECTION_POINT_HASH_MAX_SIZE,
									   &info,
									   HASH_ELEM | HASH_FIXED_SIZE | HASH_STRINGS);
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
	InjectionPointEntry *entry_by_name;
	bool		found;

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
	entry_by_name = (InjectionPointEntry *)
		hash_search(InjectionPointHash, name,
					HASH_ENTER, &found);
	if (found)
	{
		LWLockRelease(InjectionPointLock);
		elog(ERROR, "injection point \"%s\" already defined", name);
	}

	/* Save the entry */
	strlcpy(entry_by_name->name, name, sizeof(entry_by_name->name));
	entry_by_name->name[INJ_NAME_MAXLEN - 1] = '\0';
	strlcpy(entry_by_name->library, library, sizeof(entry_by_name->library));
	entry_by_name->library[INJ_LIB_MAXLEN - 1] = '\0';
	strlcpy(entry_by_name->function, function, sizeof(entry_by_name->function));
	entry_by_name->function[INJ_FUNC_MAXLEN - 1] = '\0';
	if (private_data != NULL)
		memcpy(entry_by_name->private_data, private_data, private_data_size);

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
	bool		found;

	LWLockAcquire(InjectionPointLock, LW_EXCLUSIVE);
	hash_search(InjectionPointHash, name, HASH_REMOVE, &found);
	LWLockRelease(InjectionPointLock);

	if (!found)
		return false;

	return true;
#else
	elog(ERROR, "Injection points are not supported by this build");
	return true;				/* silence compiler */
#endif
}

/*
 * Execute an injection point, if defined.
 *
 * Check first the shared hash table, and adapt the local cache depending
 * on that as it could be possible that an entry to run has been removed.
 */
void
InjectionPointRun(const char *name)
{
#ifdef USE_INJECTION_POINTS
	InjectionPointEntry *entry_by_name;
	bool		found;
	InjectionPointCallback injection_callback;
	const void *private_data;

	LWLockAcquire(InjectionPointLock, LW_SHARED);
	entry_by_name = (InjectionPointEntry *)
		hash_search(InjectionPointHash, name,
					HASH_FIND, &found);
	LWLockRelease(InjectionPointLock);

	/*
	 * If not found, do nothing and remove it from the local cache if it
	 * existed there.
	 */
	if (!found)
	{
		injection_point_cache_remove(name);
		return;
	}

	/*
	 * Check if the callback exists in the local cache, to avoid unnecessary
	 * external loads.
	 */
	if (injection_point_cache_get(name, NULL) == NULL)
	{
		char		path[MAXPGPATH];
		InjectionPointCallback injection_callback_local;

		/* not found in local cache, so load and register */
		snprintf(path, MAXPGPATH, "%s/%s%s", pkglib_path,
				 entry_by_name->library, DLSUFFIX);

		if (!pg_file_exists(path))
			elog(ERROR, "could not find library \"%s\" for injection point \"%s\"",
				 path, name);

		injection_callback_local = (InjectionPointCallback)
			load_external_function(path, entry_by_name->function, false, NULL);

		if (injection_callback_local == NULL)
			elog(ERROR, "could not find function \"%s\" in library \"%s\" for injection point \"%s\"",
				 entry_by_name->function, path, name);

		/* add it to the local cache when found */
		injection_point_cache_add(name, injection_callback_local,
								  entry_by_name->private_data);
	}

	/* Now loaded, so get it. */
	injection_callback = injection_point_cache_get(name, &private_data);
	injection_callback(name, private_data);
#else
	elog(ERROR, "Injection points are not supported by this build");
#endif
}
