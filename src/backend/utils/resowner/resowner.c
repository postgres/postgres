/*-------------------------------------------------------------------------
 *
 * resowner.c
 *	  POSTGRES resource owner management code.
 *
 * Query-lifespan resources are tracked by associating them with
 * ResourceOwner objects.  This provides a simple mechanism for ensuring
 * that such resources are freed at the right time.
 * See utils/resowner/README for more info on how to use it.
 *
 * The implementation consists of a small fixed-size array and a hash table.
 * New entries are inserted to the fixed-size array, and when the array
 * fills up, all the entries are moved to the hash table.  This way, the
 * array always contains a few most recently remembered references.  To find
 * a particular reference, you need to search both the array and the hash
 * table.
 *
 * The most frequent usage is that a resource is remembered, and forgotten
 * shortly thereafter.  For example, pin a buffer, read one tuple from it,
 * release the pin.  Linearly scanning the small array handles that case
 * efficiently.  However, some resources are held for a longer time, and
 * sometimes a lot of resources need to be held simultaneously.  The hash
 * table handles those cases.
 *
 * When it's time to release the resources, we sort them according to the
 * release-priority of each resource, and release them in that order.
 *
 * Local lock references are special, they are not stored in the array or
 * the hash table.  Instead, each resource owner has a separate small cache
 * of locks it owns.  The lock manager has the same information in its local
 * lock hash table, and we fall back on that if the cache overflows, but
 * traversing the hash table is slower when there are a lot of locks
 * belonging to other resource owners.  This is to speed up bulk releasing
 * or reassigning locks from a resource owner to its parent.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/resowner/resowner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "common/int.h"
#include "lib/ilist.h"
#include "storage/aio.h"
#include "storage/ipc.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

/*
 * ResourceElem represents a reference associated with a resource owner.
 *
 * All objects managed by this code are required to fit into a Datum,
 * which is fine since they are generally pointers or integers.
 */
typedef struct ResourceElem
{
	Datum		item;
	const ResourceOwnerDesc *kind;	/* NULL indicates a free hash table slot */
} ResourceElem;

/*
 * Size of the fixed-size array to hold most-recently remembered resources.
 */
#define RESOWNER_ARRAY_SIZE 32

/*
 * Initially allocated size of a ResourceOwner's hash table.  Must be power of
 * two because we use (capacity - 1) as mask for hashing.
 */
#define RESOWNER_HASH_INIT_SIZE 64

/*
 * How many items may be stored in a hash table of given capacity.  When this
 * number is reached, we must resize.
 *
 * The hash table must always have enough free space that we can copy the
 * entries from the array to it, in ResourceOwnerSort.  We also insist that
 * the initial size is large enough that we don't hit the max size immediately
 * when it's created.  Aside from those limitations, 0.75 is a reasonable fill
 * factor.
 */
#define RESOWNER_HASH_MAX_ITEMS(capacity) \
	Min(capacity - RESOWNER_ARRAY_SIZE, (capacity)/4 * 3)

StaticAssertDecl(RESOWNER_HASH_MAX_ITEMS(RESOWNER_HASH_INIT_SIZE) >= RESOWNER_ARRAY_SIZE,
				 "initial hash size too small compared to array size");

/*
 * MAX_RESOWNER_LOCKS is the size of the per-resource owner locks cache. It's
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
struct ResourceOwnerData
{
	ResourceOwner parent;		/* NULL if no parent (toplevel owner) */
	ResourceOwner firstchild;	/* head of linked list of children */
	ResourceOwner nextchild;	/* next child of same parent */
	const char *name;			/* name (just for debugging) */

	/*
	 * When ResourceOwnerRelease is called, we sort the 'hash' and 'arr' by
	 * the release priority.  After that, no new resources can be remembered
	 * or forgotten in retail.  We have separate flags because
	 * ResourceOwnerReleaseAllOfKind() temporarily sets 'releasing' without
	 * sorting the arrays.
	 */
	bool		releasing;
	bool		sorted;			/* are 'hash' and 'arr' sorted by priority? */

	/*
	 * Number of items in the locks cache, array, and hash table respectively.
	 * (These are packed together to avoid padding in the struct.)
	 */
	uint8		nlocks;			/* number of owned locks */
	uint8		narr;			/* how many items are stored in the array */
	uint32		nhash;			/* how many items are stored in the hash */

	/*
	 * The fixed-size array for recent resources.
	 *
	 * If 'sorted' is set, the contents are sorted by release priority.
	 */
	ResourceElem arr[RESOWNER_ARRAY_SIZE];

	/*
	 * The hash table.  Uses open-addressing.  'nhash' is the number of items
	 * present; if it would exceed 'grow_at', we enlarge it and re-hash.
	 * 'grow_at' should be rather less than 'capacity' so that we don't waste
	 * too much time searching for empty slots.
	 *
	 * If 'sorted' is set, the contents are no longer hashed, but sorted by
	 * release priority.  The first 'nhash' elements are occupied, the rest
	 * are empty.
	 */
	ResourceElem *hash;
	uint32		capacity;		/* allocated length of hash[] */
	uint32		grow_at;		/* grow hash when reach this */

	/* The local locks cache. */
	LOCALLOCK  *locks[MAX_RESOWNER_LOCKS];	/* list of owned locks */

	/*
	 * AIO handles need be registered in critical sections and therefore
	 * cannot use the normal ResourceElem mechanism.
	 */
	dlist_head	aio_handles;
};


/*****************************************************************************
 *	  GLOBAL MEMORY															 *
 *****************************************************************************/

ResourceOwner CurrentResourceOwner = NULL;
ResourceOwner CurTransactionResourceOwner = NULL;
ResourceOwner TopTransactionResourceOwner = NULL;
ResourceOwner AuxProcessResourceOwner = NULL;

/* #define RESOWNER_STATS */

#ifdef RESOWNER_STATS
static int	narray_lookups = 0;
static int	nhash_lookups = 0;
#endif

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
static inline uint32 hash_resource_elem(Datum value, const ResourceOwnerDesc *kind);
static void ResourceOwnerAddToHash(ResourceOwner owner, Datum value,
								   const ResourceOwnerDesc *kind);
static int	resource_priority_cmp(const void *a, const void *b);
static void ResourceOwnerSort(ResourceOwner owner);
static void ResourceOwnerReleaseAll(ResourceOwner owner,
									ResourceReleasePhase phase,
									bool printLeakWarnings);
static void ResourceOwnerReleaseInternal(ResourceOwner owner,
										 ResourceReleasePhase phase,
										 bool isCommit,
										 bool isTopLevel);
static void ReleaseAuxProcessResourcesCallback(int code, Datum arg);


/*****************************************************************************
 *	  INTERNAL ROUTINES														 *
 *****************************************************************************/

/*
 * Hash function for value+kind combination.
 */
static inline uint32
hash_resource_elem(Datum value, const ResourceOwnerDesc *kind)
{
	/*
	 * Most resource kinds store a pointer in 'value', and pointers are unique
	 * all on their own.  But some resources store plain integers (Files and
	 * Buffers as of this writing), so we want to incorporate the 'kind' in
	 * the hash too, otherwise those resources will collide a lot.  But
	 * because there are only a few resource kinds like that - and only a few
	 * resource kinds to begin with - we don't need to work too hard to mix
	 * 'kind' into the hash.  Just add it with hash_combine(), it perturbs the
	 * result enough for our purposes.
	 */
#if SIZEOF_DATUM == 8
	return hash_combine64(murmurhash64((uint64) value), (uint64) kind);
#else
	return hash_combine(murmurhash32((uint32) value), (uint32) kind);
#endif
}

/*
 * Adds 'value' of given 'kind' to the ResourceOwner's hash table
 */
static void
ResourceOwnerAddToHash(ResourceOwner owner, Datum value, const ResourceOwnerDesc *kind)
{
	uint32		mask = owner->capacity - 1;
	uint32		idx;

	Assert(kind != NULL);

	/* Insert into first free slot at or after hash location. */
	idx = hash_resource_elem(value, kind) & mask;
	for (;;)
	{
		if (owner->hash[idx].kind == NULL)
			break;				/* found a free slot */
		idx = (idx + 1) & mask;
	}
	owner->hash[idx].item = value;
	owner->hash[idx].kind = kind;
	owner->nhash++;
}

/*
 * Comparison function to sort by release phase and priority
 */
static int
resource_priority_cmp(const void *a, const void *b)
{
	const ResourceElem *ra = (const ResourceElem *) a;
	const ResourceElem *rb = (const ResourceElem *) b;

	/* Note: reverse order */
	if (ra->kind->release_phase == rb->kind->release_phase)
		return pg_cmp_u32(rb->kind->release_priority, ra->kind->release_priority);
	else if (ra->kind->release_phase > rb->kind->release_phase)
		return -1;
	else
		return 1;
}

/*
 * Sort resources in reverse release priority.
 *
 * If the hash table is in use, all the elements from the fixed-size array are
 * moved to the hash table, and then the hash table is sorted.  If there is no
 * hash table, then the fixed-size array is sorted directly.  In either case,
 * the result is one sorted array that contains all the resources.
 */
static void
ResourceOwnerSort(ResourceOwner owner)
{
	ResourceElem *items;
	uint32		nitems;

	if (owner->nhash == 0)
	{
		items = owner->arr;
		nitems = owner->narr;
	}
	else
	{
		/*
		 * Compact the hash table, so that all the elements are in the
		 * beginning of the 'hash' array, with no empty elements.
		 */
		uint32		dst = 0;

		for (int idx = 0; idx < owner->capacity; idx++)
		{
			if (owner->hash[idx].kind != NULL)
			{
				if (dst != idx)
					owner->hash[dst] = owner->hash[idx];
				dst++;
			}
		}

		/*
		 * Move all entries from the fixed-size array to 'hash'.
		 *
		 * RESOWNER_HASH_MAX_ITEMS is defined so that there is always enough
		 * free space to move all the elements from the fixed-size array to
		 * the hash.
		 */
		Assert(dst + owner->narr <= owner->capacity);
		for (int idx = 0; idx < owner->narr; idx++)
		{
			owner->hash[dst] = owner->arr[idx];
			dst++;
		}
		Assert(dst == owner->nhash + owner->narr);
		owner->narr = 0;
		owner->nhash = dst;

		items = owner->hash;
		nitems = owner->nhash;
	}

	qsort(items, nitems, sizeof(ResourceElem), resource_priority_cmp);
}

/*
 * Call the ReleaseResource callback on entries with given 'phase'.
 */
static void
ResourceOwnerReleaseAll(ResourceOwner owner, ResourceReleasePhase phase,
						bool printLeakWarnings)
{
	ResourceElem *items;
	uint32		nitems;

	/*
	 * ResourceOwnerSort must've been called already.  All the resources are
	 * either in the array or the hash.
	 */
	Assert(owner->releasing);
	Assert(owner->sorted);
	if (owner->nhash == 0)
	{
		items = owner->arr;
		nitems = owner->narr;
	}
	else
	{
		Assert(owner->narr == 0);
		items = owner->hash;
		nitems = owner->nhash;
	}

	/*
	 * The resources are sorted in reverse priority order.  Release them
	 * starting from the end, until we hit the end of the phase that we are
	 * releasing now.  We will continue from there when called again for the
	 * next phase.
	 */
	while (nitems > 0)
	{
		uint32		idx = nitems - 1;
		Datum		value = items[idx].item;
		const ResourceOwnerDesc *kind = items[idx].kind;

		if (kind->release_phase > phase)
			break;
		Assert(kind->release_phase == phase);

		if (printLeakWarnings)
		{
			char	   *res_str;

			res_str = kind->DebugPrint ?
				kind->DebugPrint(value)
				: psprintf("%s %p", kind->name, DatumGetPointer(value));
			elog(WARNING, "resource was not closed: %s", res_str);
			pfree(res_str);
		}
		kind->ReleaseResource(value);
		nitems--;
	}
	if (owner->nhash == 0)
		owner->narr = nitems;
	else
		owner->nhash = nitems;
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
												   sizeof(struct ResourceOwnerData));
	owner->name = name;

	if (parent)
	{
		owner->parent = parent;
		owner->nextchild = parent->firstchild;
		parent->firstchild = owner;
	}

	dlist_init(&owner->aio_handles);

	return owner;
}

/*
 * Make sure there is room for at least one more resource in an array.
 *
 * This is separate from actually inserting a resource because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 *
 * NB: Make sure there are no unrelated ResourceOwnerRemember() calls between
 * your ResourceOwnerEnlarge() call and the ResourceOwnerRemember() call that
 * you reserved the space for!
 */
void
ResourceOwnerEnlarge(ResourceOwner owner)
{
	/*
	 * Mustn't try to remember more resources after we have already started
	 * releasing
	 */
	if (owner->releasing)
		elog(ERROR, "ResourceOwnerEnlarge called after release started");

	if (owner->narr < RESOWNER_ARRAY_SIZE)
		return;					/* no work needed */

	/*
	 * Is there space in the hash? If not, enlarge it.
	 */
	if (owner->narr + owner->nhash >= owner->grow_at)
	{
		uint32		i,
					oldcap,
					newcap;
		ResourceElem *oldhash;
		ResourceElem *newhash;

		oldhash = owner->hash;
		oldcap = owner->capacity;

		/* Double the capacity (it must stay a power of 2!) */
		newcap = (oldcap > 0) ? oldcap * 2 : RESOWNER_HASH_INIT_SIZE;
		newhash = (ResourceElem *) MemoryContextAllocZero(TopMemoryContext,
														  newcap * sizeof(ResourceElem));

		/*
		 * We assume we can't fail below this point, so OK to scribble on the
		 * owner
		 */
		owner->hash = newhash;
		owner->capacity = newcap;
		owner->grow_at = RESOWNER_HASH_MAX_ITEMS(newcap);
		owner->nhash = 0;

		if (oldhash != NULL)
		{
			/*
			 * Transfer any pre-existing entries into the new hash table; they
			 * don't necessarily go where they were before, so this simple
			 * logic is the best way.
			 */
			for (i = 0; i < oldcap; i++)
			{
				if (oldhash[i].kind != NULL)
					ResourceOwnerAddToHash(owner, oldhash[i].item, oldhash[i].kind);
			}

			/* And release old hash table. */
			pfree(oldhash);
		}
	}

	/* Move items from the array to the hash */
	for (int i = 0; i < owner->narr; i++)
		ResourceOwnerAddToHash(owner, owner->arr[i].item, owner->arr[i].kind);
	owner->narr = 0;

	Assert(owner->nhash <= owner->grow_at);
}

/*
 * Remember that an object is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlarge()
 */
void
ResourceOwnerRemember(ResourceOwner owner, Datum value, const ResourceOwnerDesc *kind)
{
	uint32		idx;

	/* sanity check the ResourceOwnerDesc */
	Assert(kind->release_phase != 0);
	Assert(kind->release_priority != 0);

	/*
	 * Mustn't try to remember more resources after we have already started
	 * releasing.  We already checked this in ResourceOwnerEnlarge.
	 */
	Assert(!owner->releasing);
	Assert(!owner->sorted);

	if (owner->narr >= RESOWNER_ARRAY_SIZE)
	{
		/* forgot to call ResourceOwnerEnlarge? */
		elog(ERROR, "ResourceOwnerRemember called but array was full");
	}

	/* Append to the array. */
	idx = owner->narr;
	owner->arr[idx].item = value;
	owner->arr[idx].kind = kind;
	owner->narr++;
}

/*
 * Forget that an object is owned by a ResourceOwner
 *
 * Note: If same resource ID is associated with the ResourceOwner more than
 * once, one instance is removed.
 *
 * Note: Forgetting a resource does not guarantee that there is room to
 * remember a new resource.  One exception is when you forget the most
 * recently remembered resource; that does make room for a new remember call.
 * Some code callers rely on that exception.
 */
void
ResourceOwnerForget(ResourceOwner owner, Datum value, const ResourceOwnerDesc *kind)
{
	/*
	 * Mustn't call this after we have already started releasing resources.
	 * (Release callback functions are not allowed to release additional
	 * resources.)
	 */
	if (owner->releasing)
		elog(ERROR, "ResourceOwnerForget called for %s after release started", kind->name);
	Assert(!owner->sorted);

	/* Search through all items in the array first. */
	for (int i = owner->narr - 1; i >= 0; i--)
	{
		if (owner->arr[i].item == value &&
			owner->arr[i].kind == kind)
		{
			owner->arr[i] = owner->arr[owner->narr - 1];
			owner->narr--;

#ifdef RESOWNER_STATS
			narray_lookups++;
#endif
			return;
		}
	}

	/* Search hash */
	if (owner->nhash > 0)
	{
		uint32		mask = owner->capacity - 1;
		uint32		idx;

		idx = hash_resource_elem(value, kind) & mask;
		for (uint32 i = 0; i < owner->capacity; i++)
		{
			if (owner->hash[idx].item == value &&
				owner->hash[idx].kind == kind)
			{
				owner->hash[idx].item = (Datum) 0;
				owner->hash[idx].kind = NULL;
				owner->nhash--;

#ifdef RESOWNER_STATS
				nhash_lookups++;
#endif
				return;
			}
			idx = (idx + 1) & mask;
		}
	}

	/*
	 * Use %p to print the reference, since most objects tracked by a resource
	 * owner are pointers.  It's a bit misleading if it's not a pointer, but
	 * this is a programmer error, anyway.
	 */
	elog(ERROR, "%s %p is not owned by resource owner %s",
		 kind->name, DatumGetPointer(value), owner->name);
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
 *
 * NOTE: After starting the release process, by calling this function, no new
 * resources can be remembered in the resource owner.  You also cannot call
 * ResourceOwnerForget on any previously remembered resources to release
 * resources "in retail" after that, you must let the bulk release take care
 * of them.
 */
void
ResourceOwnerRelease(ResourceOwner owner,
					 ResourceReleasePhase phase,
					 bool isCommit,
					 bool isTopLevel)
{
	/* There's not currently any setup needed before recursing */
	ResourceOwnerReleaseInternal(owner, phase, isCommit, isTopLevel);

#ifdef RESOWNER_STATS
	if (isTopLevel)
	{
		elog(LOG, "RESOWNER STATS: lookups: array %d, hash %d",
			 narray_lookups, nhash_lookups);
		narray_lookups = 0;
		nhash_lookups = 0;
	}
#endif
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
	ResourceReleaseCallbackItem *next;

	/* Recurse to handle descendants */
	for (child = owner->firstchild; child != NULL; child = child->nextchild)
		ResourceOwnerReleaseInternal(child, phase, isCommit, isTopLevel);

	/*
	 * To release the resources in the right order, sort them by phase and
	 * priority.
	 *
	 * The ReleaseResource callback functions are not allowed to remember or
	 * forget any other resources after this. Otherwise we lose track of where
	 * we are in processing the hash/array.
	 */
	if (!owner->releasing)
	{
		Assert(phase == RESOURCE_RELEASE_BEFORE_LOCKS);
		Assert(!owner->sorted);
		owner->releasing = true;
	}
	else
	{
		/*
		 * Phase is normally > RESOURCE_RELEASE_BEFORE_LOCKS, if this is not
		 * the first call to ResourceOwnerRelease. But if an error happens
		 * between the release phases, we might get called again for the same
		 * ResourceOwner from AbortTransaction.
		 */
	}
	if (!owner->sorted)
	{
		ResourceOwnerSort(owner);
		owner->sorted = true;
	}

	/*
	 * Make CurrentResourceOwner point to me, so that the release callback
	 * functions know which resource owner is been released.
	 */
	save = CurrentResourceOwner;
	CurrentResourceOwner = owner;

	if (phase == RESOURCE_RELEASE_BEFORE_LOCKS)
	{
		/*
		 * Release all resources that need to be released before the locks.
		 *
		 * During a commit, there shouldn't be any remaining resources ---
		 * that would indicate failure to clean up the executor correctly ---
		 * so issue warnings.  In the abort case, just clean up quietly.
		 */
		ResourceOwnerReleaseAll(owner, phase, isCommit);

		while (!dlist_is_empty(&owner->aio_handles))
		{
			dlist_node *node = dlist_head_node(&owner->aio_handles);

			pgaio_io_release_resowner(node, !isCommit);
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
		 * Release all resources that need to be released after the locks.
		 */
		ResourceOwnerReleaseAll(owner, phase, isCommit);
	}

	/* Let add-on modules get a chance too */
	for (item = ResourceRelease_callbacks; item; item = next)
	{
		/* allow callbacks to unregister themselves when called */
		next = item->next;
		item->callback(phase, isCommit, isTopLevel, item->arg);
	}

	CurrentResourceOwner = save;
}

/*
 * ResourceOwnerReleaseAllOfKind
 *		Release all resources of a certain type held by this owner.
 */
void
ResourceOwnerReleaseAllOfKind(ResourceOwner owner, const ResourceOwnerDesc *kind)
{
	/* Mustn't call this after we have already started releasing resources. */
	if (owner->releasing)
		elog(ERROR, "ResourceOwnerForget called for %s after release started", kind->name);
	Assert(!owner->sorted);

	/*
	 * Temporarily set 'releasing', to prevent calls to ResourceOwnerRemember
	 * while we're scanning the owner.  Enlarging the hash would cause us to
	 * lose track of the point we're scanning.
	 */
	owner->releasing = true;

	/* Array first */
	for (int i = 0; i < owner->narr; i++)
	{
		if (owner->arr[i].kind == kind)
		{
			Datum		value = owner->arr[i].item;

			owner->arr[i] = owner->arr[owner->narr - 1];
			owner->narr--;
			i--;

			kind->ReleaseResource(value);
		}
	}

	/* Then hash */
	for (int i = 0; i < owner->capacity; i++)
	{
		if (owner->hash[i].kind == kind)
		{
			Datum		value = owner->hash[i].item;

			owner->hash[i].item = (Datum) 0;
			owner->hash[i].kind = NULL;
			owner->nhash--;

			kind->ReleaseResource(value);
		}
	}
	owner->releasing = false;
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
	Assert(owner->narr == 0);
	Assert(owner->nhash == 0);
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
	if (owner->hash)
		pfree(owner->hash);
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
 * These functions can be used by dynamically loaded modules.  These used
 * to be the only way for an extension to register custom resource types
 * with a resource owner, but nowadays it is easier to define a new
 * ResourceOwnerDesc with custom callbacks.
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
	/* allow it to be reused */
	AuxProcessResourceOwner->releasing = false;
	AuxProcessResourceOwner->sorted = false;
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
 * Remember that a Local Lock is owned by a ResourceOwner
 *
 * This is different from the generic ResourceOwnerRemember in that the list of
 * locks is only a lossy cache.  It can hold up to MAX_RESOWNER_LOCKS entries,
 * and when it overflows, we stop tracking locks.  The point of only remembering
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

void
ResourceOwnerRememberAioHandle(ResourceOwner owner, struct dlist_node *ioh_node)
{
	dlist_push_tail(&owner->aio_handles, ioh_node);
}

void
ResourceOwnerForgetAioHandle(ResourceOwner owner, struct dlist_node *ioh_node)
{
	dlist_delete_from(&owner->aio_handles, ioh_node);
}
