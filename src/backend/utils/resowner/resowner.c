/*-------------------------------------------------------------------------
 *
 * resowner.c
 *	  POSTGRES resource owner management code.
 *
 * Query-lifespan resources are tracked by associating them with
 * ResourceOwner objects.  This provides a simple mechanism for ensuring
 * that such resources are freed at the right time.
 * See utils/resowner/README for more info.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/resowner/resowner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/cryptohash.h"
#include "common/hashfn.h"
#include "common/hmac.h"
#include "jit/jit.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner_private.h"
#include "utils/snapmgr.h"


/*
 * All resource IDs managed by this code are required to fit into a Datum,
 * which is fine since they are generally pointers or integers.
 *
 * Provide Datum conversion macros for a couple of things that are really
 * just "int".
 */
#define FileGetDatum(file) Int32GetDatum(file)
#define DatumGetFile(datum) ((File) DatumGetInt32(datum))
#define BufferGetDatum(buffer) Int32GetDatum(buffer)
#define DatumGetBuffer(datum) ((Buffer) DatumGetInt32(datum))

/*
 * ResourceArray is a common structure for storing all types of resource IDs.
 *
 * We manage small sets of resource IDs by keeping them in a simple array:
 * itemsarr[k] holds an ID, for 0 <= k < nitems <= maxitems = capacity.
 *
 * If a set grows large, we switch over to using open-addressing hashing.
 * Then, itemsarr[] is a hash table of "capacity" slots, with each
 * slot holding either an ID or "invalidval".  nitems is the number of valid
 * items present; if it would exceed maxitems, we enlarge the array and
 * re-hash.  In this mode, maxitems should be rather less than capacity so
 * that we don't waste too much time searching for empty slots.
 *
 * In either mode, lastidx remembers the location of the last item inserted
 * or returned by GetAny; this speeds up searches in ResourceArrayRemove.
 */
typedef struct ResourceArray
{
	Datum	   *itemsarr;		/* buffer for storing values */
	Datum		invalidval;		/* value that is considered invalid */
	uint32		capacity;		/* allocated length of itemsarr[] */
	uint32		nitems;			/* how many items are stored in items array */
	uint32		maxitems;		/* current limit on nitems before enlarging */
	uint32		lastidx;		/* index of last item returned by GetAny */
} ResourceArray;

/*
 * Initially allocated size of a ResourceArray.  Must be power of two since
 * we'll use (arraysize - 1) as mask for hashing.
 */
#define RESARRAY_INIT_SIZE 16

/*
 * When to switch to hashing vs. simple array logic in a ResourceArray.
 */
#define RESARRAY_MAX_ARRAY 64
#define RESARRAY_IS_ARRAY(resarr) ((resarr)->capacity <= RESARRAY_MAX_ARRAY)

/*
 * How many items may be stored in a resource array of given capacity.
 * When this number is reached, we must resize.
 */
#define RESARRAY_MAX_ITEMS(capacity) \
	((capacity) <= RESARRAY_MAX_ARRAY ? (capacity) : (capacity)/4 * 3)

/*
 * To speed up bulk releasing or reassigning locks from a resource owner to
 * its parent, each resource owner has a small cache of locks it owns. The
 * lock manager has the same information in its local lock hash table, and
 * we fall back on that if cache overflows, but traversing the hash table
 * is slower when there are a lot of locks belonging to other resource owners.
 *
 * MAX_RESOWNER_LOCKS is the size of the per-resource owner cache. It's
 * chosen based on some testing with pg_dump with a large schema. When the
 * tests were done (on 9.2), resource owners in a pg_dump run contained up
 * to 9 locks, regardless of the schema size, except for the top resource
 * owner which contained much more (overflowing the cache). 15 seems like a
 * nice round number that's somewhat higher than what pg_dump needs. Note that
 * making this number larger is not free - the bigger the cache, the slower
 * it is to release locks (in retail), when a resource owner holds many locks.
 */
#define MAX_RESOWNER_LOCKS 15

/*
 * ResourceOwner objects look like this
 */
typedef struct ResourceOwnerData
{
	ResourceOwner parent;		/* NULL if no parent (toplevel owner) */
	ResourceOwner firstchild;	/* head of linked list of children */
	ResourceOwner nextchild;	/* next child of same parent */
	const char *name;			/* name (just for debugging) */

	/* We have built-in support for remembering: */
	ResourceArray bufferarr;	/* owned buffers */
	ResourceArray catrefarr;	/* catcache references */
	ResourceArray catlistrefarr;	/* catcache-list pins */
	ResourceArray relrefarr;	/* relcache references */
	ResourceArray planrefarr;	/* plancache references */
	ResourceArray tupdescarr;	/* tupdesc references */
	ResourceArray snapshotarr;	/* snapshot references */
	ResourceArray filearr;		/* open temporary files */
	ResourceArray dsmarr;		/* dynamic shmem segments */
	ResourceArray jitarr;		/* JIT contexts */
	ResourceArray cryptohasharr;	/* cryptohash contexts */
	ResourceArray hmacarr;		/* HMAC contexts */

	/* We can remember up to MAX_RESOWNER_LOCKS references to local locks. */
	int			nlocks;			/* number of owned locks */
	LOCALLOCK  *locks[MAX_RESOWNER_LOCKS];	/* list of owned locks */
}			ResourceOwnerData;


/*****************************************************************************
 *	  GLOBAL MEMORY															 *
 *****************************************************************************/

ResourceOwner CurrentResourceOwner = NULL;
ResourceOwner CurTransactionResourceOwner = NULL;
ResourceOwner TopTransactionResourceOwner = NULL;
ResourceOwner AuxProcessResourceOwner = NULL;

/*
 * List of add-on callbacks for resource releasing
 */
typedef struct ResourceReleaseCallbackItem
{
	struct ResourceReleaseCallbackItem *next;
	ResourceReleaseCallback callback;
	void	   *arg;
} ResourceReleaseCallbackItem;

static ResourceReleaseCallbackItem *ResourceRelease_callbacks = NULL;


/* Internal routines */
static void ResourceArrayInit(ResourceArray *resarr, Datum invalidval);
static void ResourceArrayEnlarge(ResourceArray *resarr);
static void ResourceArrayAdd(ResourceArray *resarr, Datum value);
static bool ResourceArrayRemove(ResourceArray *resarr, Datum value);
static bool ResourceArrayGetAny(ResourceArray *resarr, Datum *value);
static void ResourceArrayFree(ResourceArray *resarr);
static void ResourceOwnerReleaseInternal(ResourceOwner owner,
										 ResourceReleasePhase phase,
										 bool isCommit,
										 bool isTopLevel);
static void ReleaseAuxProcessResourcesCallback(int code, Datum arg);
static void PrintRelCacheLeakWarning(Relation rel);
static void PrintPlanCacheLeakWarning(CachedPlan *plan);
static void PrintTupleDescLeakWarning(TupleDesc tupdesc);
static void PrintSnapshotLeakWarning(Snapshot snapshot);
static void PrintFileLeakWarning(File file);
static void PrintDSMLeakWarning(dsm_segment *seg);
static void PrintCryptoHashLeakWarning(Datum handle);
static void PrintHMACLeakWarning(Datum handle);


/*****************************************************************************
 *	  INTERNAL ROUTINES														 *
 *****************************************************************************/


/*
 * Initialize a ResourceArray
 */
static void
ResourceArrayInit(ResourceArray *resarr, Datum invalidval)
{
	/* Assert it's empty */
	Assert(resarr->itemsarr == NULL);
	Assert(resarr->capacity == 0);
	Assert(resarr->nitems == 0);
	Assert(resarr->maxitems == 0);
	/* Remember the appropriate "invalid" value */
	resarr->invalidval = invalidval;
	/* We don't allocate any storage until needed */
}

/*
 * Make sure there is room for at least one more resource in an array.
 *
 * This is separate from actually inserting a resource because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
static void
ResourceArrayEnlarge(ResourceArray *resarr)
{
	uint32		i,
				oldcap,
				newcap;
	Datum	   *olditemsarr;
	Datum	   *newitemsarr;

	if (resarr->nitems < resarr->maxitems)
		return;					/* no work needed */

	olditemsarr = resarr->itemsarr;
	oldcap = resarr->capacity;

	/* Double the capacity of the array (capacity must stay a power of 2!) */
	newcap = (oldcap > 0) ? oldcap * 2 : RESARRAY_INIT_SIZE;
	newitemsarr = (Datum *) MemoryContextAlloc(TopMemoryContext,
											   newcap * sizeof(Datum));
	for (i = 0; i < newcap; i++)
		newitemsarr[i] = resarr->invalidval;

	/* We assume we can't fail below this point, so OK to scribble on resarr */
	resarr->itemsarr = newitemsarr;
	resarr->capacity = newcap;
	resarr->maxitems = RESARRAY_MAX_ITEMS(newcap);
	resarr->nitems = 0;

	if (olditemsarr != NULL)
	{
		/*
		 * Transfer any pre-existing entries into the new array; they don't
		 * necessarily go where they were before, so this simple logic is the
		 * best way.  Note that if we were managing the set as a simple array,
		 * the entries after nitems are garbage, but that shouldn't matter
		 * because we won't get here unless nitems was equal to oldcap.
		 */
		for (i = 0; i < oldcap; i++)
		{
			if (olditemsarr[i] != resarr->invalidval)
				ResourceArrayAdd(resarr, olditemsarr[i]);
		}

		/* And release old array. */
		pfree(olditemsarr);
	}

	Assert(resarr->nitems < resarr->maxitems);
}

/*
 * Add a resource to ResourceArray
 *
 * Caller must have previously done ResourceArrayEnlarge()
 */
static void
ResourceArrayAdd(ResourceArray *resarr, Datum value)
{
	uint32		idx;

	Assert(value != resarr->invalidval);
	Assert(resarr->nitems < resarr->maxitems);

	if (RESARRAY_IS_ARRAY(resarr))
	{
		/* Append to linear array. */
		idx = resarr->nitems;
	}
	else
	{
		/* Insert into first free slot at or after hash location. */
		uint32		mask = resarr->capacity - 1;

		idx = DatumGetUInt32(hash_any((void *) &value, sizeof(value))) & mask;
		for (;;)
		{
			if (resarr->itemsarr[idx] == resarr->invalidval)
				break;
			idx = (idx + 1) & mask;
		}
	}
	resarr->lastidx = idx;
	resarr->itemsarr[idx] = value;
	resarr->nitems++;
}

/*
 * Remove a resource from ResourceArray
 *
 * Returns true on success, false if resource was not found.
 *
 * Note: if same resource ID appears more than once, one instance is removed.
 */
static bool
ResourceArrayRemove(ResourceArray *resarr, Datum value)
{
	uint32		i,
				idx,
				lastidx = resarr->lastidx;

	Assert(value != resarr->invalidval);

	/* Search through all items, but try lastidx first. */
	if (RESARRAY_IS_ARRAY(resarr))
	{
		if (lastidx < resarr->nitems &&
			resarr->itemsarr[lastidx] == value)
		{
			resarr->itemsarr[lastidx] = resarr->itemsarr[resarr->nitems - 1];
			resarr->nitems--;
			/* Update lastidx to make reverse-order removals fast. */
			resarr->lastidx = resarr->nitems - 1;
			return true;
		}
		for (i = 0; i < resarr->nitems; i++)
		{
			if (resarr->itemsarr[i] == value)
			{
				resarr->itemsarr[i] = resarr->itemsarr[resarr->nitems - 1];
				resarr->nitems--;
				/* Update lastidx to make reverse-order removals fast. */
				resarr->lastidx = resarr->nitems - 1;
				return true;
			}
		}
	}
	else
	{
		uint32		mask = resarr->capacity - 1;

		if (lastidx < resarr->capacity &&
			resarr->itemsarr[lastidx] == value)
		{
			resarr->itemsarr[lastidx] = resarr->invalidval;
			resarr->nitems--;
			return true;
		}
		idx = DatumGetUInt32(hash_any((void *) &value, sizeof(value))) & mask;
		for (i = 0; i < resarr->capacity; i++)
		{
			if (resarr->itemsarr[idx] == value)
			{
				resarr->itemsarr[idx] = resarr->invalidval;
				resarr->nitems--;
				return true;
			}
			idx = (idx + 1) & mask;
		}
	}

	return false;
}

/*
 * Get any convenient entry in a ResourceArray.
 *
 * "Convenient" is defined as "easy for ResourceArrayRemove to remove";
 * we help that along by setting lastidx to match.  This avoids O(N^2) cost
 * when removing all ResourceArray items during ResourceOwner destruction.
 *
 * Returns true if we found an element, or false if the array is empty.
 */
static bool
ResourceArrayGetAny(ResourceArray *resarr, Datum *value)
{
	if (resarr->nitems == 0)
		return false;

	if (RESARRAY_IS_ARRAY(resarr))
	{
		/* Linear array: just return the first element. */
		resarr->lastidx = 0;
	}
	else
	{
		/* Hash: search forward from wherever we were last. */
		uint32		mask = resarr->capacity - 1;

		for (;;)
		{
			resarr->lastidx &= mask;
			if (resarr->itemsarr[resarr->lastidx] != resarr->invalidval)
				break;
			resarr->lastidx++;
		}
	}

	*value = resarr->itemsarr[resarr->lastidx];
	return true;
}

/*
 * Trash a ResourceArray (we don't care about its state after this)
 */
static void
ResourceArrayFree(ResourceArray *resarr)
{
	if (resarr->itemsarr)
		pfree(resarr->itemsarr);
}


/*****************************************************************************
 *	  EXPORTED ROUTINES														 *
 *****************************************************************************/


/*
 * ResourceOwnerCreate
 *		Create an empty ResourceOwner.
 *
 * All ResourceOwner objects are kept in TopMemoryContext, since they should
 * only be freed explicitly.
 */
ResourceOwner
ResourceOwnerCreate(ResourceOwner parent, const char *name)
{
	ResourceOwner owner;

	owner = (ResourceOwner) MemoryContextAllocZero(TopMemoryContext,
												   sizeof(ResourceOwnerData));
	owner->name = name;

	if (parent)
	{
		owner->parent = parent;
		owner->nextchild = parent->firstchild;
		parent->firstchild = owner;
	}

	ResourceArrayInit(&(owner->bufferarr), BufferGetDatum(InvalidBuffer));
	ResourceArrayInit(&(owner->catrefarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->catlistrefarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->relrefarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->planrefarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->tupdescarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->snapshotarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->filearr), FileGetDatum(-1));
	ResourceArrayInit(&(owner->dsmarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->jitarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->cryptohasharr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->hmacarr), PointerGetDatum(NULL));

	return owner;
}

/*
 * ResourceOwnerRelease
 *		Release all resources owned by a ResourceOwner and its descendants,
 *		but don't delete the owner objects themselves.
 *
 * Note that this executes just one phase of release, and so typically
 * must be called three times.  We do it this way because (a) we want to
 * do all the recursion separately for each phase, thereby preserving
 * the needed order of operations; and (b) xact.c may have other operations
 * to do between the phases.
 *
 * phase: release phase to execute
 * isCommit: true for successful completion of a query or transaction,
 *			false for unsuccessful
 * isTopLevel: true if completing a main transaction, else false
 *
 * isCommit is passed because some modules may expect that their resources
 * were all released already if the transaction or portal finished normally.
 * If so it is reasonable to give a warning (NOT an error) should any
 * unreleased resources be present.  When isCommit is false, such warnings
 * are generally inappropriate.
 *
 * isTopLevel is passed when we are releasing TopTransactionResourceOwner
 * at completion of a main transaction.  This generally means that *all*
 * resources will be released, and so we can optimize things a bit.
 */
void
ResourceOwnerRelease(ResourceOwner owner,
					 ResourceReleasePhase phase,
					 bool isCommit,
					 bool isTopLevel)
{
	/* There's not currently any setup needed before recursing */
	ResourceOwnerReleaseInternal(owner, phase, isCommit, isTopLevel);
}

static void
ResourceOwnerReleaseInternal(ResourceOwner owner,
							 ResourceReleasePhase phase,
							 bool isCommit,
							 bool isTopLevel)
{
	ResourceOwner child;
	ResourceOwner save;
	ResourceReleaseCallbackItem *item;
	Datum		foundres;

	/* Recurse to handle descendants */
	for (child = owner->firstchild; child != NULL; child = child->nextchild)
		ResourceOwnerReleaseInternal(child, phase, isCommit, isTopLevel);

	/*
	 * Make CurrentResourceOwner point to me, so that ReleaseBuffer etc don't
	 * get confused.
	 */
	save = CurrentResourceOwner;
	CurrentResourceOwner = owner;

	if (phase == RESOURCE_RELEASE_BEFORE_LOCKS)
	{
		/*
		 * Release buffer pins.  Note that ReleaseBuffer will remove the
		 * buffer entry from our array, so we just have to iterate till there
		 * are none.
		 *
		 * During a commit, there shouldn't be any remaining pins --- that
		 * would indicate failure to clean up the executor correctly --- so
		 * issue warnings.  In the abort case, just clean up quietly.
		 */
		while (ResourceArrayGetAny(&(owner->bufferarr), &foundres))
		{
			Buffer		res = DatumGetBuffer(foundres);

			if (isCommit)
				PrintBufferLeakWarning(res);
			ReleaseBuffer(res);
		}

		/* Ditto for relcache references */
		while (ResourceArrayGetAny(&(owner->relrefarr), &foundres))
		{
			Relation	res = (Relation) DatumGetPointer(foundres);

			if (isCommit)
				PrintRelCacheLeakWarning(res);
			RelationClose(res);
		}

		/* Ditto for dynamic shared memory segments */
		while (ResourceArrayGetAny(&(owner->dsmarr), &foundres))
		{
			dsm_segment *res = (dsm_segment *) DatumGetPointer(foundres);

			if (isCommit)
				PrintDSMLeakWarning(res);
			dsm_detach(res);
		}

		/* Ditto for JIT contexts */
		while (ResourceArrayGetAny(&(owner->jitarr), &foundres))
		{
			JitContext *context = (JitContext *) PointerGetDatum(foundres);

			jit_release_context(context);
		}

		/* Ditto for cryptohash contexts */
		while (ResourceArrayGetAny(&(owner->cryptohasharr), &foundres))
		{
			pg_cryptohash_ctx *context =
			(pg_cryptohash_ctx *) PointerGetDatum(foundres);

			if (isCommit)
				PrintCryptoHashLeakWarning(foundres);
			pg_cryptohash_free(context);
		}

		/* Ditto for HMAC contexts */
		while (ResourceArrayGetAny(&(owner->hmacarr), &foundres))
		{
			pg_hmac_ctx *context = (pg_hmac_ctx *) PointerGetDatum(foundres);

			if (isCommit)
				PrintHMACLeakWarning(foundres);
			pg_hmac_free(context);
		}
	}
	else if (phase == RESOURCE_RELEASE_LOCKS)
	{
		if (isTopLevel)
		{
			/*
			 * For a top-level xact we are going to release all locks (or at
			 * least all non-session locks), so just do a single lmgr call at
			 * the top of the recursion.
			 */
			if (owner == TopTransactionResourceOwner)
			{
				ProcReleaseLocks(isCommit);
				ReleasePredicateLocks(isCommit, false);
			}
		}
		else
		{
			/*
			 * Release locks retail.  Note that if we are committing a
			 * subtransaction, we do NOT release its locks yet, but transfer
			 * them to the parent.
			 */
			LOCALLOCK **locks;
			int			nlocks;

			Assert(owner->parent != NULL);

			/*
			 * Pass the list of locks owned by this resource owner to the lock
			 * manager, unless it has overflowed.
			 */
			if (owner->nlocks > MAX_RESOWNER_LOCKS)
			{
				locks = NULL;
				nlocks = 0;
			}
			else
			{
				locks = owner->locks;
				nlocks = owner->nlocks;
			}

			if (isCommit)
				LockReassignCurrentOwner(locks, nlocks);
			else
				LockReleaseCurrentOwner(locks, nlocks);
		}
	}
	else if (phase == RESOURCE_RELEASE_AFTER_LOCKS)
	{
		/*
		 * Release catcache references.  Note that ReleaseCatCache will remove
		 * the catref entry from our array, so we just have to iterate till
		 * there are none.
		 *
		 * As with buffer pins, warn if any are left at commit time.
		 */
		while (ResourceArrayGetAny(&(owner->catrefarr), &foundres))
		{
			HeapTuple	res = (HeapTuple) DatumGetPointer(foundres);

			if (isCommit)
				PrintCatCacheLeakWarning(res);
			ReleaseCatCache(res);
		}

		/* Ditto for catcache lists */
		while (ResourceArrayGetAny(&(owner->catlistrefarr), &foundres))
		{
			CatCList   *res = (CatCList *) DatumGetPointer(foundres);

			if (isCommit)
				PrintCatCacheListLeakWarning(res);
			ReleaseCatCacheList(res);
		}

		/* Ditto for plancache references */
		while (ResourceArrayGetAny(&(owner->planrefarr), &foundres))
		{
			CachedPlan *res = (CachedPlan *) DatumGetPointer(foundres);

			if (isCommit)
				PrintPlanCacheLeakWarning(res);
			ReleaseCachedPlan(res, owner);
		}

		/* Ditto for tupdesc references */
		while (ResourceArrayGetAny(&(owner->tupdescarr), &foundres))
		{
			TupleDesc	res = (TupleDesc) DatumGetPointer(foundres);

			if (isCommit)
				PrintTupleDescLeakWarning(res);
			DecrTupleDescRefCount(res);
		}

		/* Ditto for snapshot references */
		while (ResourceArrayGetAny(&(owner->snapshotarr), &foundres))
		{
			Snapshot	res = (Snapshot) DatumGetPointer(foundres);

			if (isCommit)
				PrintSnapshotLeakWarning(res);
			UnregisterSnapshot(res);
		}

		/* Ditto for temporary files */
		while (ResourceArrayGetAny(&(owner->filearr), &foundres))
		{
			File		res = DatumGetFile(foundres);

			if (isCommit)
				PrintFileLeakWarning(res);
			FileClose(res);
		}
	}

	/* Let add-on modules get a chance too */
	for (item = ResourceRelease_callbacks; item; item = item->next)
		item->callback(phase, isCommit, isTopLevel, item->arg);

	CurrentResourceOwner = save;
}

/*
 * ResourceOwnerReleaseAllPlanCacheRefs
 *		Release the plancache references (only) held by this owner.
 *
 * We might eventually add similar functions for other resource types,
 * but for now, only this is needed.
 */
void
ResourceOwnerReleaseAllPlanCacheRefs(ResourceOwner owner)
{
	Datum		foundres;

	while (ResourceArrayGetAny(&(owner->planrefarr), &foundres))
	{
		CachedPlan *res = (CachedPlan *) DatumGetPointer(foundres);

		ReleaseCachedPlan(res, owner);
	}
}

/*
 * ResourceOwnerDelete
 *		Delete an owner object and its descendants.
 *
 * The caller must have already released all resources in the object tree.
 */
void
ResourceOwnerDelete(ResourceOwner owner)
{
	/* We had better not be deleting CurrentResourceOwner ... */
	Assert(owner != CurrentResourceOwner);

	/* And it better not own any resources, either */
	Assert(owner->bufferarr.nitems == 0);
	Assert(owner->catrefarr.nitems == 0);
	Assert(owner->catlistrefarr.nitems == 0);
	Assert(owner->relrefarr.nitems == 0);
	Assert(owner->planrefarr.nitems == 0);
	Assert(owner->tupdescarr.nitems == 0);
	Assert(owner->snapshotarr.nitems == 0);
	Assert(owner->filearr.nitems == 0);
	Assert(owner->dsmarr.nitems == 0);
	Assert(owner->jitarr.nitems == 0);
	Assert(owner->cryptohasharr.nitems == 0);
	Assert(owner->hmacarr.nitems == 0);
	Assert(owner->nlocks == 0 || owner->nlocks == MAX_RESOWNER_LOCKS + 1);

	/*
	 * Delete children.  The recursive call will delink the child from me, so
	 * just iterate as long as there is a child.
	 */
	while (owner->firstchild != NULL)
		ResourceOwnerDelete(owner->firstchild);

	/*
	 * We delink the owner from its parent before deleting it, so that if
	 * there's an error we won't have deleted/busted owners still attached to
	 * the owner tree.  Better a leak than a crash.
	 */
	ResourceOwnerNewParent(owner, NULL);

	/* And free the object. */
	ResourceArrayFree(&(owner->bufferarr));
	ResourceArrayFree(&(owner->catrefarr));
	ResourceArrayFree(&(owner->catlistrefarr));
	ResourceArrayFree(&(owner->relrefarr));
	ResourceArrayFree(&(owner->planrefarr));
	ResourceArrayFree(&(owner->tupdescarr));
	ResourceArrayFree(&(owner->snapshotarr));
	ResourceArrayFree(&(owner->filearr));
	ResourceArrayFree(&(owner->dsmarr));
	ResourceArrayFree(&(owner->jitarr));
	ResourceArrayFree(&(owner->cryptohasharr));
	ResourceArrayFree(&(owner->hmacarr));

	pfree(owner);
}

/*
 * Fetch parent of a ResourceOwner (returns NULL if top-level owner)
 */
ResourceOwner
ResourceOwnerGetParent(ResourceOwner owner)
{
	return owner->parent;
}

/*
 * Reassign a ResourceOwner to have a new parent
 */
void
ResourceOwnerNewParent(ResourceOwner owner,
					   ResourceOwner newparent)
{
	ResourceOwner oldparent = owner->parent;

	if (oldparent)
	{
		if (owner == oldparent->firstchild)
			oldparent->firstchild = owner->nextchild;
		else
		{
			ResourceOwner child;

			for (child = oldparent->firstchild; child; child = child->nextchild)
			{
				if (owner == child->nextchild)
				{
					child->nextchild = owner->nextchild;
					break;
				}
			}
		}
	}

	if (newparent)
	{
		Assert(owner != newparent);
		owner->parent = newparent;
		owner->nextchild = newparent->firstchild;
		newparent->firstchild = owner;
	}
	else
	{
		owner->parent = NULL;
		owner->nextchild = NULL;
	}
}

/*
 * Register or deregister callback functions for resource cleanup
 *
 * These functions are intended for use by dynamically loaded modules.
 * For built-in modules we generally just hardwire the appropriate calls.
 *
 * Note that the callback occurs post-commit or post-abort, so the callback
 * functions can only do noncritical cleanup.
 */
void
RegisterResourceReleaseCallback(ResourceReleaseCallback callback, void *arg)
{
	ResourceReleaseCallbackItem *item;

	item = (ResourceReleaseCallbackItem *)
		MemoryContextAlloc(TopMemoryContext,
						   sizeof(ResourceReleaseCallbackItem));
	item->callback = callback;
	item->arg = arg;
	item->next = ResourceRelease_callbacks;
	ResourceRelease_callbacks = item;
}

void
UnregisterResourceReleaseCallback(ResourceReleaseCallback callback, void *arg)
{
	ResourceReleaseCallbackItem *item;
	ResourceReleaseCallbackItem *prev;

	prev = NULL;
	for (item = ResourceRelease_callbacks; item; prev = item, item = item->next)
	{
		if (item->callback == callback && item->arg == arg)
		{
			if (prev)
				prev->next = item->next;
			else
				ResourceRelease_callbacks = item->next;
			pfree(item);
			break;
		}
	}
}

/*
 * Establish an AuxProcessResourceOwner for the current process.
 */
void
CreateAuxProcessResourceOwner(void)
{
	Assert(AuxProcessResourceOwner == NULL);
	Assert(CurrentResourceOwner == NULL);
	AuxProcessResourceOwner = ResourceOwnerCreate(NULL, "AuxiliaryProcess");
	CurrentResourceOwner = AuxProcessResourceOwner;

	/*
	 * Register a shmem-exit callback for cleanup of aux-process resource
	 * owner.  (This needs to run after, e.g., ShutdownXLOG.)
	 */
	on_shmem_exit(ReleaseAuxProcessResourcesCallback, 0);
}

/*
 * Convenience routine to release all resources tracked in
 * AuxProcessResourceOwner (but that resowner is not destroyed here).
 * Warn about leaked resources if isCommit is true.
 */
void
ReleaseAuxProcessResources(bool isCommit)
{
	/*
	 * At this writing, the only thing that could actually get released is
	 * buffer pins; but we may as well do the full release protocol.
	 */
	ResourceOwnerRelease(AuxProcessResourceOwner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 isCommit, true);
	ResourceOwnerRelease(AuxProcessResourceOwner,
						 RESOURCE_RELEASE_LOCKS,
						 isCommit, true);
	ResourceOwnerRelease(AuxProcessResourceOwner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 isCommit, true);
}

/*
 * Shmem-exit callback for the same.
 * Warn about leaked resources if process exit code is zero (ie normal).
 */
static void
ReleaseAuxProcessResourcesCallback(int code, Datum arg)
{
	bool		isCommit = (code == 0);

	ReleaseAuxProcessResources(isCommit);
}


/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * buffer array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeBuffers(ResourceOwner owner)
{
	/* We used to allow pinning buffers without a resowner, but no more */
	Assert(owner != NULL);
	ResourceArrayEnlarge(&(owner->bufferarr));
}

/*
 * Remember that a buffer pin is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeBuffers()
 */
void
ResourceOwnerRememberBuffer(ResourceOwner owner, Buffer buffer)
{
	ResourceArrayAdd(&(owner->bufferarr), BufferGetDatum(buffer));
}

/*
 * Forget that a buffer pin is owned by a ResourceOwner
 */
void
ResourceOwnerForgetBuffer(ResourceOwner owner, Buffer buffer)
{
	if (!ResourceArrayRemove(&(owner->bufferarr), BufferGetDatum(buffer)))
		elog(ERROR, "buffer %d is not owned by resource owner %s",
			 buffer, owner->name);
}

/*
 * Remember that a Local Lock is owned by a ResourceOwner
 *
 * This is different from the other Remember functions in that the list of
 * locks is only a lossy cache. It can hold up to MAX_RESOWNER_LOCKS entries,
 * and when it overflows, we stop tracking locks. The point of only remembering
 * only up to MAX_RESOWNER_LOCKS entries is that if a lot of locks are held,
 * ResourceOwnerForgetLock doesn't need to scan through a large array to find
 * the entry.
 */
void
ResourceOwnerRememberLock(ResourceOwner owner, LOCALLOCK *locallock)
{
	Assert(locallock != NULL);

	if (owner->nlocks > MAX_RESOWNER_LOCKS)
		return;					/* we have already overflowed */

	if (owner->nlocks < MAX_RESOWNER_LOCKS)
		owner->locks[owner->nlocks] = locallock;
	else
	{
		/* overflowed */
	}
	owner->nlocks++;
}

/*
 * Forget that a Local Lock is owned by a ResourceOwner
 */
void
ResourceOwnerForgetLock(ResourceOwner owner, LOCALLOCK *locallock)
{
	int			i;

	if (owner->nlocks > MAX_RESOWNER_LOCKS)
		return;					/* we have overflowed */

	Assert(owner->nlocks > 0);
	for (i = owner->nlocks - 1; i >= 0; i--)
	{
		if (locallock == owner->locks[i])
		{
			owner->locks[i] = owner->locks[owner->nlocks - 1];
			owner->nlocks--;
			return;
		}
	}
	elog(ERROR, "lock reference %p is not owned by resource owner %s",
		 locallock, owner->name);
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * catcache reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeCatCacheRefs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->catrefarr));
}

/*
 * Remember that a catcache reference is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeCatCacheRefs()
 */
void
ResourceOwnerRememberCatCacheRef(ResourceOwner owner, HeapTuple tuple)
{
	ResourceArrayAdd(&(owner->catrefarr), PointerGetDatum(tuple));
}

/*
 * Forget that a catcache reference is owned by a ResourceOwner
 */
void
ResourceOwnerForgetCatCacheRef(ResourceOwner owner, HeapTuple tuple)
{
	if (!ResourceArrayRemove(&(owner->catrefarr), PointerGetDatum(tuple)))
		elog(ERROR, "catcache reference %p is not owned by resource owner %s",
			 tuple, owner->name);
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * catcache-list reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeCatCacheListRefs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->catlistrefarr));
}

/*
 * Remember that a catcache-list reference is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeCatCacheListRefs()
 */
void
ResourceOwnerRememberCatCacheListRef(ResourceOwner owner, CatCList *list)
{
	ResourceArrayAdd(&(owner->catlistrefarr), PointerGetDatum(list));
}

/*
 * Forget that a catcache-list reference is owned by a ResourceOwner
 */
void
ResourceOwnerForgetCatCacheListRef(ResourceOwner owner, CatCList *list)
{
	if (!ResourceArrayRemove(&(owner->catlistrefarr), PointerGetDatum(list)))
		elog(ERROR, "catcache list reference %p is not owned by resource owner %s",
			 list, owner->name);
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * relcache reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeRelationRefs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->relrefarr));
}

/*
 * Remember that a relcache reference is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeRelationRefs()
 */
void
ResourceOwnerRememberRelationRef(ResourceOwner owner, Relation rel)
{
	ResourceArrayAdd(&(owner->relrefarr), PointerGetDatum(rel));
}

/*
 * Forget that a relcache reference is owned by a ResourceOwner
 */
void
ResourceOwnerForgetRelationRef(ResourceOwner owner, Relation rel)
{
	if (!ResourceArrayRemove(&(owner->relrefarr), PointerGetDatum(rel)))
		elog(ERROR, "relcache reference %s is not owned by resource owner %s",
			 RelationGetRelationName(rel), owner->name);
}

/*
 * Debugging subroutine
 */
static void
PrintRelCacheLeakWarning(Relation rel)
{
	elog(WARNING, "relcache reference leak: relation \"%s\" not closed",
		 RelationGetRelationName(rel));
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * plancache reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargePlanCacheRefs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->planrefarr));
}

/*
 * Remember that a plancache reference is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargePlanCacheRefs()
 */
void
ResourceOwnerRememberPlanCacheRef(ResourceOwner owner, CachedPlan *plan)
{
	ResourceArrayAdd(&(owner->planrefarr), PointerGetDatum(plan));
}

/*
 * Forget that a plancache reference is owned by a ResourceOwner
 */
void
ResourceOwnerForgetPlanCacheRef(ResourceOwner owner, CachedPlan *plan)
{
	if (!ResourceArrayRemove(&(owner->planrefarr), PointerGetDatum(plan)))
		elog(ERROR, "plancache reference %p is not owned by resource owner %s",
			 plan, owner->name);
}

/*
 * Debugging subroutine
 */
static void
PrintPlanCacheLeakWarning(CachedPlan *plan)
{
	elog(WARNING, "plancache reference leak: plan %p not closed", plan);
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * tupdesc reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeTupleDescs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->tupdescarr));
}

/*
 * Remember that a tupdesc reference is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeTupleDescs()
 */
void
ResourceOwnerRememberTupleDesc(ResourceOwner owner, TupleDesc tupdesc)
{
	ResourceArrayAdd(&(owner->tupdescarr), PointerGetDatum(tupdesc));
}

/*
 * Forget that a tupdesc reference is owned by a ResourceOwner
 */
void
ResourceOwnerForgetTupleDesc(ResourceOwner owner, TupleDesc tupdesc)
{
	if (!ResourceArrayRemove(&(owner->tupdescarr), PointerGetDatum(tupdesc)))
		elog(ERROR, "tupdesc reference %p is not owned by resource owner %s",
			 tupdesc, owner->name);
}

/*
 * Debugging subroutine
 */
static void
PrintTupleDescLeakWarning(TupleDesc tupdesc)
{
	elog(WARNING,
		 "TupleDesc reference leak: TupleDesc %p (%u,%d) still referenced",
		 tupdesc, tupdesc->tdtypeid, tupdesc->tdtypmod);
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * snapshot reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeSnapshots(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->snapshotarr));
}

/*
 * Remember that a snapshot reference is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeSnapshots()
 */
void
ResourceOwnerRememberSnapshot(ResourceOwner owner, Snapshot snapshot)
{
	ResourceArrayAdd(&(owner->snapshotarr), PointerGetDatum(snapshot));
}

/*
 * Forget that a snapshot reference is owned by a ResourceOwner
 */
void
ResourceOwnerForgetSnapshot(ResourceOwner owner, Snapshot snapshot)
{
	if (!ResourceArrayRemove(&(owner->snapshotarr), PointerGetDatum(snapshot)))
		elog(ERROR, "snapshot reference %p is not owned by resource owner %s",
			 snapshot, owner->name);
}

/*
 * Debugging subroutine
 */
static void
PrintSnapshotLeakWarning(Snapshot snapshot)
{
	elog(WARNING, "Snapshot reference leak: Snapshot %p still referenced",
		 snapshot);
}


/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * files reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeFiles(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->filearr));
}

/*
 * Remember that a temporary file is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeFiles()
 */
void
ResourceOwnerRememberFile(ResourceOwner owner, File file)
{
	ResourceArrayAdd(&(owner->filearr), FileGetDatum(file));
}

/*
 * Forget that a temporary file is owned by a ResourceOwner
 */
void
ResourceOwnerForgetFile(ResourceOwner owner, File file)
{
	if (!ResourceArrayRemove(&(owner->filearr), FileGetDatum(file)))
		elog(ERROR, "temporary file %d is not owned by resource owner %s",
			 file, owner->name);
}

/*
 * Debugging subroutine
 */
static void
PrintFileLeakWarning(File file)
{
	elog(WARNING, "temporary file leak: File %d still referenced",
		 file);
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * dynamic shmem segment reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeDSMs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->dsmarr));
}

/*
 * Remember that a dynamic shmem segment is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeDSMs()
 */
void
ResourceOwnerRememberDSM(ResourceOwner owner, dsm_segment *seg)
{
	ResourceArrayAdd(&(owner->dsmarr), PointerGetDatum(seg));
}

/*
 * Forget that a dynamic shmem segment is owned by a ResourceOwner
 */
void
ResourceOwnerForgetDSM(ResourceOwner owner, dsm_segment *seg)
{
	if (!ResourceArrayRemove(&(owner->dsmarr), PointerGetDatum(seg)))
		elog(ERROR, "dynamic shared memory segment %u is not owned by resource owner %s",
			 dsm_segment_handle(seg), owner->name);
}

/*
 * Debugging subroutine
 */
static void
PrintDSMLeakWarning(dsm_segment *seg)
{
	elog(WARNING, "dynamic shared memory leak: segment %u still referenced",
		 dsm_segment_handle(seg));
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * JIT context reference array.
 *
 * This is separate from actually inserting an entry because if we run out of
 * memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeJIT(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->jitarr));
}

/*
 * Remember that a JIT context is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeJIT()
 */
void
ResourceOwnerRememberJIT(ResourceOwner owner, Datum handle)
{
	ResourceArrayAdd(&(owner->jitarr), handle);
}

/*
 * Forget that a JIT context is owned by a ResourceOwner
 */
void
ResourceOwnerForgetJIT(ResourceOwner owner, Datum handle)
{
	if (!ResourceArrayRemove(&(owner->jitarr), handle))
		elog(ERROR, "JIT context %p is not owned by resource owner %s",
			 DatumGetPointer(handle), owner->name);
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * cryptohash context reference array.
 *
 * This is separate from actually inserting an entry because if we run out of
 * memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeCryptoHash(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->cryptohasharr));
}

/*
 * Remember that a cryptohash context is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeCryptoHash()
 */
void
ResourceOwnerRememberCryptoHash(ResourceOwner owner, Datum handle)
{
	ResourceArrayAdd(&(owner->cryptohasharr), handle);
}

/*
 * Forget that a cryptohash context is owned by a ResourceOwner
 */
void
ResourceOwnerForgetCryptoHash(ResourceOwner owner, Datum handle)
{
	if (!ResourceArrayRemove(&(owner->cryptohasharr), handle))
		elog(ERROR, "cryptohash context %p is not owned by resource owner %s",
			 DatumGetPointer(handle), owner->name);
}

/*
 * Debugging subroutine
 */
static void
PrintCryptoHashLeakWarning(Datum handle)
{
	elog(WARNING, "cryptohash context reference leak: context %p still referenced",
		 DatumGetPointer(handle));
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * hmac context reference array.
 *
 * This is separate from actually inserting an entry because if we run out of
 * memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeHMAC(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->hmacarr));
}

/*
 * Remember that a HMAC context is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeHMAC()
 */
void
ResourceOwnerRememberHMAC(ResourceOwner owner, Datum handle)
{
	ResourceArrayAdd(&(owner->hmacarr), handle);
}

/*
 * Forget that a HMAC context is owned by a ResourceOwner
 */
void
ResourceOwnerForgetHMAC(ResourceOwner owner, Datum handle)
{
	if (!ResourceArrayRemove(&(owner->hmacarr), handle))
		elog(ERROR, "HMAC context %p is not owned by resource owner %s",
			 DatumGetPointer(handle), owner->name);
}

/*
 * Debugging subroutine
 */
static void
PrintHMACLeakWarning(Datum handle)
{
	elog(WARNING, "HMAC context reference leak: context %p still referenced",
		 DatumGetPointer(handle));
}
