/*-------------------------------------------------------------------------
 *
 * evtcache.c
 *	  Special-purpose cache for event trigger data.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/evtcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/evtcache.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/hsearch.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

typedef struct
{
	EventTriggerEvent	event;
	List	   *triggerlist;
} EventTriggerCacheEntry;

static HTAB *EventTriggerCache;
static MemoryContext EventTriggerCacheContext;

static void BuildEventTriggerCache(void);
static void InvalidateEventCacheCallback(Datum arg,
							 int cacheid, uint32 hashvalue);
static int DecodeTextArrayToCString(Datum array, char ***cstringp);

/*
 * Search the event cache by trigger event.
 *
 * Note that the caller had better copy any data it wants to keep around
 * across any operation that might touch a system catalog into some other
 * memory context, since a cache reset could blow the return value away.
 */
List *
EventCacheLookup(EventTriggerEvent event)
{
	EventTriggerCacheEntry *entry;

	if (EventTriggerCache == NULL)
		BuildEventTriggerCache();
	entry = hash_search(EventTriggerCache, &event, HASH_FIND, NULL);
	return entry != NULL ? entry->triggerlist : NULL;
}

/*
 * Rebuild the event trigger cache.
 */
static void
BuildEventTriggerCache(void)
{
	HASHCTL         ctl;
	HTAB		   *cache;
	MemoryContext	oldcontext;
	Relation		rel;
	Relation		irel;
	SysScanDesc		scan;

	if (EventTriggerCacheContext != NULL)
	{
		/*
		 * The cache has been previously built, and subsequently invalidated,
		 * and now we're trying to rebuild it.  Normally, there won't be
		 * anything in the context at this point, because the invalidation
		 * will have already reset it.  But if the previous attempt to rebuild
		 * the cache failed, then there might be some junk lying around
		 * that we want to reclaim.
		 */
		MemoryContextReset(EventTriggerCacheContext);
	}
	else
	{
		/*
		 * This is our first time attempting to build the cache, so we need
		 * to set up the memory context and register a syscache callback to
		 * capture future invalidation events.
		 */
		if (CacheMemoryContext == NULL)
			CreateCacheMemoryContext();
		EventTriggerCacheContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "EventTriggerCache",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
		CacheRegisterSyscacheCallback(EVENTTRIGGEROID,
									  InvalidateEventCacheCallback,
									  (Datum) 0);
	}

	/* Switch to correct memory context. */
	oldcontext = MemoryContextSwitchTo(EventTriggerCacheContext);

	/*
	 * Create a new hash table, but don't assign it to the global variable
	 * until it accurately represents the state of the catalogs, so that
	 * if we fail midway through this we won't end up with incorrect cache
	 * contents.
	 */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(EventTriggerEvent);
	ctl.entrysize = sizeof(EventTriggerCacheEntry);
	ctl.hash = tag_hash;
	cache = hash_create("Event Trigger Cache", 32, &ctl,
						HASH_ELEM | HASH_FUNCTION);

	/*
	 * Prepare to scan pg_event_trigger in name order.  We use an MVCC
	 * snapshot to avoid getting inconsistent results if the table is
	 * being concurrently updated.
	 */
	rel = relation_open(EventTriggerRelationId, AccessShareLock);
	irel = index_open(EventTriggerNameIndexId, AccessShareLock);
	scan = systable_beginscan_ordered(rel, irel, GetLatestSnapshot(), 0, NULL);

	/*
	 * Build a cache item for each pg_event_trigger tuple, and append each
	 * one to the appropriate cache entry.
	 */
	for (;;)
	{
		HeapTuple		tup;
		Form_pg_event_trigger	form;
		char	   *evtevent;
		EventTriggerEvent	event;
		EventTriggerCacheItem *item;
		Datum		evttags;
		bool		evttags_isnull;
		EventTriggerCacheEntry *entry;
		bool		found;

		/* Get next tuple. */
		tup = systable_getnext_ordered(scan, ForwardScanDirection);
		if (!HeapTupleIsValid(tup))
			break;

		/* Skip trigger if disabled. */
		form = (Form_pg_event_trigger) GETSTRUCT(tup);
		if (form->evtenabled == TRIGGER_DISABLED)
			continue;

		/* Decode event name. */
		evtevent = NameStr(form->evtevent);
		if (strcmp(evtevent, "ddl_command_start") == 0)
			event = EVT_DDLCommandStart;
		else
			continue;

		/* Allocate new cache item. */
		item = palloc0(sizeof(EventTriggerCacheItem));
		item->fnoid = form->evtfoid;
		item->enabled = form->evtenabled;

		/* Decode and sort tags array. */
		evttags = heap_getattr(tup, Anum_pg_event_trigger_evttags,
							   RelationGetDescr(rel), &evttags_isnull);
		if (!evttags_isnull)
		{
			item->ntags = DecodeTextArrayToCString(evttags, &item->tag);
			qsort(item->tag, item->ntags, sizeof(char *), pg_qsort_strcmp);
		}

		/* Add to cache entry. */
		entry = hash_search(cache, &event, HASH_ENTER, &found);
		if (found)
			entry->triggerlist = lappend(entry->triggerlist, item);
		else
			entry->triggerlist = list_make1(item);
	}

	/* Done with pg_event_trigger scan. */
	systable_endscan_ordered(scan);
	index_close(irel, AccessShareLock);
	relation_close(rel, AccessShareLock);

	/* Restore previous memory context. */
	MemoryContextSwitchTo(oldcontext);

	/* Cache is now valid. */
	EventTriggerCache = cache;
}

/*
 * Decode text[] to an array of C strings.
 *
 * We could avoid a bit of overhead here if we were willing to duplicate some
 * of the logic from deconstruct_array, but it doesn't seem worth the code
 * complexity.
 */
static int
DecodeTextArrayToCString(Datum array, char ***cstringp)
{
	ArrayType  *arr = DatumGetArrayTypeP(array);
	Datum	   *elems;
	char	  **cstring;
	int			i;
	int			nelems;

	if (ARR_NDIM(arr) != 1 || ARR_HASNULL(arr) || ARR_ELEMTYPE(arr) != TEXTOID)
		elog(ERROR, "expected 1-D text array");
	deconstruct_array(arr, TEXTOID, -1, false, 'i', &elems, NULL, &nelems);

	cstring = palloc(nelems * sizeof(char *));
	for (i = 0; i < nelems; ++i)
		cstring[i] = TextDatumGetCString(elems[i]);

	pfree(elems);
	*cstringp = cstring;
	return nelems;
}

/*
 * Flush all cache entries when pg_event_trigger is updated.
 *
 * This should be rare enough that we don't need to be very granular about
 * it, so we just blow away everything, which also avoids the possibility of
 * memory leaks.
 */
static void
InvalidateEventCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	MemoryContextReset(EventTriggerCacheContext);
	EventTriggerCache = NULL;
}
