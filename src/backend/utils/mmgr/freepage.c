/*-------------------------------------------------------------------------
 *
 * freepage.c
 *	  Management of free memory pages.
 *
 * The intention of this code is to provide infrastructure for memory
 * allocators written specifically for PostgreSQL.  At least in the case
 * of dynamic shared memory, we can't simply use malloc() or even
 * relatively thin wrappers like palloc() which sit on top of it, because
 * no allocator built into the operating system will deal with relative
 * pointers.  In the future, we may find other cases in which greater
 * control over our own memory management seems desirable.
 *
 * A FreePageManager keeps track of which 4kB pages of memory are currently
 * unused from the point of view of some higher-level memory allocator.
 * Unlike a user-facing allocator such as palloc(), a FreePageManager can
 * only allocate and free in units of whole pages, and freeing an
 * allocation can only be done given knowledge of its length in pages.
 *
 * Since a free page manager has only a fixed amount of dedicated memory,
 * and since there is no underlying allocator, it uses the free pages
 * it is given to manage to store its bookkeeping data.  It keeps multiple
 * freelists of runs of pages, sorted by the size of the run; the head of
 * each freelist is stored in the FreePageManager itself, and the first
 * page of each run contains a relative pointer to the next run. See
 * FreePageManagerGetInternal for more details on how the freelists are
 * managed.
 *
 * To avoid memory fragmentation, it's important to consolidate adjacent
 * spans of pages whenever possible; otherwise, large allocation requests
 * might not be satisfied even when sufficient contiguous space is
 * available.  Therefore, in addition to the freelists, we maintain an
 * in-memory btree of free page ranges ordered by page number.  If a
 * range being freed precedes or follows a range that is already free,
 * the existing range is extended; if it exactly bridges the gap between
 * free ranges, then the two existing ranges are consolidated with the
 * newly-freed range to form one great big range of free pages.
 *
 * When there is only one range of free pages, the btree is trivial and
 * is stored within the FreePageManager proper; otherwise, pages are
 * allocated from the area under management as needed.  Even in cases
 * where memory fragmentation is very severe, only a tiny fraction of
 * the pages under management are consumed by this btree.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/freepage.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"

#include "utils/freepage.h"
#include "utils/relptr.h"


/* Magic numbers to identify various page types */
#define FREE_PAGE_SPAN_LEADER_MAGIC		0xea4020f0
#define FREE_PAGE_LEAF_MAGIC			0x98eae728
#define FREE_PAGE_INTERNAL_MAGIC		0x19aa32c9

/* Doubly linked list of spans of free pages; stored in first page of span. */
struct FreePageSpanLeader
{
	int			magic;			/* always FREE_PAGE_SPAN_LEADER_MAGIC */
	Size		npages;			/* number of pages in span */
	RelptrFreePageSpanLeader prev;
	RelptrFreePageSpanLeader next;
};

/* Common header for btree leaf and internal pages. */
typedef struct FreePageBtreeHeader
{
	int			magic;			/* FREE_PAGE_LEAF_MAGIC or
								 * FREE_PAGE_INTERNAL_MAGIC */
	Size		nused;			/* number of items used */
	RelptrFreePageBtree parent; /* uplink */
} FreePageBtreeHeader;

/* Internal key; points to next level of btree. */
typedef struct FreePageBtreeInternalKey
{
	Size		first_page;		/* low bound for keys on child page */
	RelptrFreePageBtree child;	/* downlink */
} FreePageBtreeInternalKey;

/* Leaf key; no payload data. */
typedef struct FreePageBtreeLeafKey
{
	Size		first_page;		/* first page in span */
	Size		npages;			/* number of pages in span */
} FreePageBtreeLeafKey;

/* Work out how many keys will fit on a page. */
#define FPM_ITEMS_PER_INTERNAL_PAGE \
	((FPM_PAGE_SIZE - sizeof(FreePageBtreeHeader)) / \
		sizeof(FreePageBtreeInternalKey))
#define FPM_ITEMS_PER_LEAF_PAGE \
	((FPM_PAGE_SIZE - sizeof(FreePageBtreeHeader)) / \
		sizeof(FreePageBtreeLeafKey))

/* A btree page of either sort */
struct FreePageBtree
{
	FreePageBtreeHeader hdr;
	union
	{
		FreePageBtreeInternalKey internal_key[FPM_ITEMS_PER_INTERNAL_PAGE];
		FreePageBtreeLeafKey leaf_key[FPM_ITEMS_PER_LEAF_PAGE];
	}			u;
};

/* Results of a btree search */
typedef struct FreePageBtreeSearchResult
{
	FreePageBtree *page;
	Size		index;
	bool		found;
	unsigned	split_pages;
} FreePageBtreeSearchResult;

/* Helper functions */
static void FreePageBtreeAdjustAncestorKeys(FreePageManager *fpm,
											FreePageBtree *btp);
static Size FreePageBtreeCleanup(FreePageManager *fpm);
static FreePageBtree *FreePageBtreeFindLeftSibling(char *base,
												   FreePageBtree *btp);
static FreePageBtree *FreePageBtreeFindRightSibling(char *base,
													FreePageBtree *btp);
static Size FreePageBtreeFirstKey(FreePageBtree *btp);
static FreePageBtree *FreePageBtreeGetRecycled(FreePageManager *fpm);
static void FreePageBtreeInsertInternal(char *base, FreePageBtree *btp,
										Size index, Size first_page, FreePageBtree *child);
static void FreePageBtreeInsertLeaf(FreePageBtree *btp, Size index,
									Size first_page, Size npages);
static void FreePageBtreeRecycle(FreePageManager *fpm, Size pageno);
static void FreePageBtreeRemove(FreePageManager *fpm, FreePageBtree *btp,
								Size index);
static void FreePageBtreeRemovePage(FreePageManager *fpm, FreePageBtree *btp);
static void FreePageBtreeSearch(FreePageManager *fpm, Size first_page,
								FreePageBtreeSearchResult *result);
static Size FreePageBtreeSearchInternal(FreePageBtree *btp, Size first_page);
static Size FreePageBtreeSearchLeaf(FreePageBtree *btp, Size first_page);
static FreePageBtree *FreePageBtreeSplitPage(FreePageManager *fpm,
											 FreePageBtree *btp);
static void FreePageBtreeUpdateParentPointers(char *base, FreePageBtree *btp);
static void FreePageManagerDumpBtree(FreePageManager *fpm, FreePageBtree *btp,
									 FreePageBtree *parent, int level, StringInfo buf);
static void FreePageManagerDumpSpans(FreePageManager *fpm,
									 FreePageSpanLeader *span, Size expected_pages,
									 StringInfo buf);
static bool FreePageManagerGetInternal(FreePageManager *fpm, Size npages,
									   Size *first_page);
static Size FreePageManagerPutInternal(FreePageManager *fpm, Size first_page,
									   Size npages, bool soft);
static void FreePagePopSpanLeader(FreePageManager *fpm, Size pageno);
static void FreePagePushSpanLeader(FreePageManager *fpm, Size first_page,
								   Size npages);
static Size FreePageManagerLargestContiguous(FreePageManager *fpm);
static void FreePageManagerUpdateLargest(FreePageManager *fpm);

#ifdef FPM_EXTRA_ASSERTS
static Size sum_free_pages(FreePageManager *fpm);
#endif

/*
 * Initialize a new, empty free page manager.
 *
 * 'fpm' should reference caller-provided memory large enough to contain a
 * FreePageManager.  We'll initialize it here.
 *
 * 'base' is the address to which all pointers are relative.  When managing
 * a dynamic shared memory segment, it should normally be the base of the
 * segment.  When managing backend-private memory, it can be either NULL or,
 * if managing a single contiguous extent of memory, the start of that extent.
 */
void
FreePageManagerInitialize(FreePageManager *fpm, char *base)
{
	Size		f;

	relptr_store(base, fpm->self, fpm);
	relptr_store(base, fpm->btree_root, (FreePageBtree *) NULL);
	relptr_store(base, fpm->btree_recycle, (FreePageSpanLeader *) NULL);
	fpm->btree_depth = 0;
	fpm->btree_recycle_count = 0;
	fpm->singleton_first_page = 0;
	fpm->singleton_npages = 0;
	fpm->contiguous_pages = 0;
	fpm->contiguous_pages_dirty = true;
#ifdef FPM_EXTRA_ASSERTS
	fpm->free_pages = 0;
#endif

	for (f = 0; f < FPM_NUM_FREELISTS; f++)
		relptr_store(base, fpm->freelist[f], (FreePageSpanLeader *) NULL);
}

/*
 * Allocate a run of pages of the given length from the free page manager.
 * The return value indicates whether we were able to satisfy the request;
 * if true, the first page of the allocation is stored in *first_page.
 */
bool
FreePageManagerGet(FreePageManager *fpm, Size npages, Size *first_page)
{
	bool		result;
	Size		contiguous_pages;

	result = FreePageManagerGetInternal(fpm, npages, first_page);

	/*
	 * It's a bit counterintuitive, but allocating pages can actually create
	 * opportunities for cleanup that create larger ranges.  We might pull a
	 * key out of the btree that enables the item at the head of the btree
	 * recycle list to be inserted; and then if there are more items behind it
	 * one of those might cause two currently-separated ranges to merge,
	 * creating a single range of contiguous pages larger than any that
	 * existed previously.  It might be worth trying to improve the cleanup
	 * algorithm to avoid such corner cases, but for now we just notice the
	 * condition and do the appropriate reporting.
	 */
	contiguous_pages = FreePageBtreeCleanup(fpm);
	if (fpm->contiguous_pages < contiguous_pages)
		fpm->contiguous_pages = contiguous_pages;

	/*
	 * FreePageManagerGetInternal may have set contiguous_pages_dirty.
	 * Recompute contiguous_pages if so.
	 */
	FreePageManagerUpdateLargest(fpm);

#ifdef FPM_EXTRA_ASSERTS
	if (result)
	{
		Assert(fpm->free_pages >= npages);
		fpm->free_pages -= npages;
	}
	Assert(fpm->free_pages == sum_free_pages(fpm));
	Assert(fpm->contiguous_pages == FreePageManagerLargestContiguous(fpm));
#endif
	return result;
}

#ifdef FPM_EXTRA_ASSERTS
static void
sum_free_pages_recurse(FreePageManager *fpm, FreePageBtree *btp, Size *sum)
{
	char	   *base = fpm_segment_base(fpm);

	Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC ||
		   btp->hdr.magic == FREE_PAGE_LEAF_MAGIC);
	++*sum;
	if (btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC)
	{
		Size		index;


		for (index = 0; index < btp->hdr.nused; ++index)
		{
			FreePageBtree *child;

			child = relptr_access(base, btp->u.internal_key[index].child);
			sum_free_pages_recurse(fpm, child, sum);
		}
	}
}
static Size
sum_free_pages(FreePageManager *fpm)
{
	FreePageSpanLeader *recycle;
	char	   *base = fpm_segment_base(fpm);
	Size		sum = 0;
	int			list;

	/* Count the spans by scanning the freelists. */
	for (list = 0; list < FPM_NUM_FREELISTS; ++list)
	{

		if (!relptr_is_null(fpm->freelist[list]))
		{
			FreePageSpanLeader *candidate =
			relptr_access(base, fpm->freelist[list]);

			do
			{
				sum += candidate->npages;
				candidate = relptr_access(base, candidate->next);
			} while (candidate != NULL);
		}
	}

	/* Count btree internal pages. */
	if (fpm->btree_depth > 0)
	{
		FreePageBtree *root = relptr_access(base, fpm->btree_root);

		sum_free_pages_recurse(fpm, root, &sum);
	}

	/* Count the recycle list. */
	for (recycle = relptr_access(base, fpm->btree_recycle);
		 recycle != NULL;
		 recycle = relptr_access(base, recycle->next))
	{
		Assert(recycle->npages == 1);
		++sum;
	}

	return sum;
}
#endif

/*
 * Compute the size of the largest run of pages that the user could
 * successfully get.
 */
static Size
FreePageManagerLargestContiguous(FreePageManager *fpm)
{
	char	   *base;
	Size		largest;

	base = fpm_segment_base(fpm);
	largest = 0;
	if (!relptr_is_null(fpm->freelist[FPM_NUM_FREELISTS - 1]))
	{
		FreePageSpanLeader *candidate;

		candidate = relptr_access(base, fpm->freelist[FPM_NUM_FREELISTS - 1]);
		do
		{
			if (candidate->npages > largest)
				largest = candidate->npages;
			candidate = relptr_access(base, candidate->next);
		} while (candidate != NULL);
	}
	else
	{
		Size		f = FPM_NUM_FREELISTS - 1;

		do
		{
			--f;
			if (!relptr_is_null(fpm->freelist[f]))
			{
				largest = f + 1;
				break;
			}
		} while (f > 0);
	}

	return largest;
}

/*
 * Recompute the size of the largest run of pages that the user could
 * successfully get, if it has been marked dirty.
 */
static void
FreePageManagerUpdateLargest(FreePageManager *fpm)
{
	if (!fpm->contiguous_pages_dirty)
		return;

	fpm->contiguous_pages = FreePageManagerLargestContiguous(fpm);
	fpm->contiguous_pages_dirty = false;
}

/*
 * Transfer a run of pages to the free page manager.
 */
void
FreePageManagerPut(FreePageManager *fpm, Size first_page, Size npages)
{
	Size		contiguous_pages;

	Assert(npages > 0);

	/* Record the new pages. */
	contiguous_pages =
		FreePageManagerPutInternal(fpm, first_page, npages, false);

	/*
	 * If the new range we inserted into the page manager was contiguous with
	 * an existing range, it may have opened up cleanup opportunities.
	 */
	if (contiguous_pages > npages)
	{
		Size		cleanup_contiguous_pages;

		cleanup_contiguous_pages = FreePageBtreeCleanup(fpm);
		if (cleanup_contiguous_pages > contiguous_pages)
			contiguous_pages = cleanup_contiguous_pages;
	}

	/* See if we now have a new largest chunk. */
	if (fpm->contiguous_pages < contiguous_pages)
		fpm->contiguous_pages = contiguous_pages;

	/*
	 * The earlier call to FreePageManagerPutInternal may have set
	 * contiguous_pages_dirty if it needed to allocate internal pages, so
	 * recompute contiguous_pages if necessary.
	 */
	FreePageManagerUpdateLargest(fpm);

#ifdef FPM_EXTRA_ASSERTS
	fpm->free_pages += npages;
	Assert(fpm->free_pages == sum_free_pages(fpm));
	Assert(fpm->contiguous_pages == FreePageManagerLargestContiguous(fpm));
#endif
}

/*
 * Produce a debugging dump of the state of a free page manager.
 */
char *
FreePageManagerDump(FreePageManager *fpm)
{
	char	   *base = fpm_segment_base(fpm);
	StringInfoData buf;
	FreePageSpanLeader *recycle;
	bool		dumped_any_freelist = false;
	Size		f;

	/* Initialize output buffer. */
	initStringInfo(&buf);

	/* Dump general stuff. */
	appendStringInfo(&buf, "metadata: self %zu max contiguous pages = %zu\n",
					 relptr_offset(fpm->self), fpm->contiguous_pages);

	/* Dump btree. */
	if (fpm->btree_depth > 0)
	{
		FreePageBtree *root;

		appendStringInfo(&buf, "btree depth %u:\n", fpm->btree_depth);
		root = relptr_access(base, fpm->btree_root);
		FreePageManagerDumpBtree(fpm, root, NULL, 0, &buf);
	}
	else if (fpm->singleton_npages > 0)
	{
		appendStringInfo(&buf, "singleton: %zu(%zu)\n",
						 fpm->singleton_first_page, fpm->singleton_npages);
	}

	/* Dump btree recycle list. */
	recycle = relptr_access(base, fpm->btree_recycle);
	if (recycle != NULL)
	{
		appendStringInfoString(&buf, "btree recycle:");
		FreePageManagerDumpSpans(fpm, recycle, 1, &buf);
	}

	/* Dump free lists. */
	for (f = 0; f < FPM_NUM_FREELISTS; ++f)
	{
		FreePageSpanLeader *span;

		if (relptr_is_null(fpm->freelist[f]))
			continue;
		if (!dumped_any_freelist)
		{
			appendStringInfoString(&buf, "freelists:\n");
			dumped_any_freelist = true;
		}
		appendStringInfo(&buf, "  %zu:", f + 1);
		span = relptr_access(base, fpm->freelist[f]);
		FreePageManagerDumpSpans(fpm, span, f + 1, &buf);
	}

	/* And return result to caller. */
	return buf.data;
}


/*
 * The first_page value stored at index zero in any non-root page must match
 * the first_page value stored in its parent at the index which points to that
 * page.  So when the value stored at index zero in a btree page changes, we've
 * got to walk up the tree adjusting ancestor keys until we reach an ancestor
 * where that key isn't index zero.  This function should be called after
 * updating the first key on the target page; it will propagate the change
 * upward as far as needed.
 *
 * We assume here that the first key on the page has not changed enough to
 * require changes in the ordering of keys on its ancestor pages.  Thus,
 * if we search the parent page for the first key greater than or equal to
 * the first key on the current page, the downlink to this page will be either
 * the exact index returned by the search (if the first key decreased)
 * or one less (if the first key increased).
 */
static void
FreePageBtreeAdjustAncestorKeys(FreePageManager *fpm, FreePageBtree *btp)
{
	char	   *base = fpm_segment_base(fpm);
	Size		first_page;
	FreePageBtree *parent;
	FreePageBtree *child;

	/* This might be either a leaf or an internal page. */
	Assert(btp->hdr.nused > 0);
	if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
	{
		Assert(btp->hdr.nused <= FPM_ITEMS_PER_LEAF_PAGE);
		first_page = btp->u.leaf_key[0].first_page;
	}
	else
	{
		Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		Assert(btp->hdr.nused <= FPM_ITEMS_PER_INTERNAL_PAGE);
		first_page = btp->u.internal_key[0].first_page;
	}
	child = btp;

	/* Loop until we find an ancestor that does not require adjustment. */
	for (;;)
	{
		Size		s;

		parent = relptr_access(base, child->hdr.parent);
		if (parent == NULL)
			break;
		s = FreePageBtreeSearchInternal(parent, first_page);

		/* Key is either at index s or index s-1; figure out which. */
		if (s >= parent->hdr.nused)
		{
			Assert(s == parent->hdr.nused);
			--s;
		}
		else
		{
			FreePageBtree *check;

			check = relptr_access(base, parent->u.internal_key[s].child);
			if (check != child)
			{
				Assert(s > 0);
				--s;
			}
		}

#ifdef USE_ASSERT_CHECKING
		/* Debugging double-check. */
		{
			FreePageBtree *check;

			check = relptr_access(base, parent->u.internal_key[s].child);
			Assert(s < parent->hdr.nused);
			Assert(child == check);
		}
#endif

		/* Update the parent key. */
		parent->u.internal_key[s].first_page = first_page;

		/*
		 * If this is the first key in the parent, go up another level; else
		 * done.
		 */
		if (s > 0)
			break;
		child = parent;
	}
}

/*
 * Attempt to reclaim space from the free-page btree.  The return value is
 * the largest range of contiguous pages created by the cleanup operation.
 */
static Size
FreePageBtreeCleanup(FreePageManager *fpm)
{
	char	   *base = fpm_segment_base(fpm);
	Size		max_contiguous_pages = 0;

	/* Attempt to shrink the depth of the btree. */
	while (!relptr_is_null(fpm->btree_root))
	{
		FreePageBtree *root = relptr_access(base, fpm->btree_root);

		/* If the root contains only one key, reduce depth by one. */
		if (root->hdr.nused == 1)
		{
			/* Shrink depth of tree by one. */
			Assert(fpm->btree_depth > 0);
			--fpm->btree_depth;
			if (root->hdr.magic == FREE_PAGE_LEAF_MAGIC)
			{
				/* If root is a leaf, convert only entry to singleton range. */
				relptr_store(base, fpm->btree_root, (FreePageBtree *) NULL);
				fpm->singleton_first_page = root->u.leaf_key[0].first_page;
				fpm->singleton_npages = root->u.leaf_key[0].npages;
			}
			else
			{
				FreePageBtree *newroot;

				/* If root is an internal page, make only child the root. */
				Assert(root->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
				relptr_copy(fpm->btree_root, root->u.internal_key[0].child);
				newroot = relptr_access(base, fpm->btree_root);
				relptr_store(base, newroot->hdr.parent, (FreePageBtree *) NULL);
			}
			FreePageBtreeRecycle(fpm, fpm_pointer_to_page(base, root));
		}
		else if (root->hdr.nused == 2 &&
				 root->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		{
			Size		end_of_first;
			Size		start_of_second;

			end_of_first = root->u.leaf_key[0].first_page +
				root->u.leaf_key[0].npages;
			start_of_second = root->u.leaf_key[1].first_page;

			if (end_of_first + 1 == start_of_second)
			{
				Size		root_page = fpm_pointer_to_page(base, root);

				if (end_of_first == root_page)
				{
					FreePagePopSpanLeader(fpm, root->u.leaf_key[0].first_page);
					FreePagePopSpanLeader(fpm, root->u.leaf_key[1].first_page);
					fpm->singleton_first_page = root->u.leaf_key[0].first_page;
					fpm->singleton_npages = root->u.leaf_key[0].npages +
						root->u.leaf_key[1].npages + 1;
					fpm->btree_depth = 0;
					relptr_store(base, fpm->btree_root,
								 (FreePageBtree *) NULL);
					FreePagePushSpanLeader(fpm, fpm->singleton_first_page,
										   fpm->singleton_npages);
					Assert(max_contiguous_pages == 0);
					max_contiguous_pages = fpm->singleton_npages;
				}
			}

			/* Whether it worked or not, it's time to stop. */
			break;
		}
		else
		{
			/* Nothing more to do.  Stop. */
			break;
		}
	}

	/*
	 * Attempt to free recycled btree pages.  We skip this if releasing the
	 * recycled page would require a btree page split, because the page we're
	 * trying to recycle would be consumed by the split, which would be
	 * counterproductive.
	 *
	 * We also currently only ever attempt to recycle the first page on the
	 * list; that could be made more aggressive, but it's not clear that the
	 * complexity would be worthwhile.
	 */
	while (fpm->btree_recycle_count > 0)
	{
		FreePageBtree *btp;
		Size		first_page;
		Size		contiguous_pages;

		btp = FreePageBtreeGetRecycled(fpm);
		first_page = fpm_pointer_to_page(base, btp);
		contiguous_pages = FreePageManagerPutInternal(fpm, first_page, 1, true);
		if (contiguous_pages == 0)
		{
			FreePageBtreeRecycle(fpm, first_page);
			break;
		}
		else
		{
			if (contiguous_pages > max_contiguous_pages)
				max_contiguous_pages = contiguous_pages;
		}
	}

	return max_contiguous_pages;
}

/*
 * Consider consolidating the given page with its left or right sibling,
 * if it's fairly empty.
 */
static void
FreePageBtreeConsolidate(FreePageManager *fpm, FreePageBtree *btp)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageBtree *np;
	Size		max;

	/*
	 * We only try to consolidate pages that are less than a third full. We
	 * could be more aggressive about this, but that might risk performing
	 * consolidation only to end up splitting again shortly thereafter.  Since
	 * the btree should be very small compared to the space under management,
	 * our goal isn't so much to ensure that it always occupies the absolutely
	 * smallest possible number of pages as to reclaim pages before things get
	 * too egregiously out of hand.
	 */
	if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		max = FPM_ITEMS_PER_LEAF_PAGE;
	else
	{
		Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		max = FPM_ITEMS_PER_INTERNAL_PAGE;
	}
	if (btp->hdr.nused >= max / 3)
		return;

	/*
	 * If we can fit our right sibling's keys onto this page, consolidate.
	 */
	np = FreePageBtreeFindRightSibling(base, btp);
	if (np != NULL && btp->hdr.nused + np->hdr.nused <= max)
	{
		if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		{
			memcpy(&btp->u.leaf_key[btp->hdr.nused], &np->u.leaf_key[0],
				   sizeof(FreePageBtreeLeafKey) * np->hdr.nused);
			btp->hdr.nused += np->hdr.nused;
		}
		else
		{
			memcpy(&btp->u.internal_key[btp->hdr.nused], &np->u.internal_key[0],
				   sizeof(FreePageBtreeInternalKey) * np->hdr.nused);
			btp->hdr.nused += np->hdr.nused;
			FreePageBtreeUpdateParentPointers(base, btp);
		}
		FreePageBtreeRemovePage(fpm, np);
		return;
	}

	/*
	 * If we can fit our keys onto our left sibling's page, consolidate. In
	 * this case, we move our keys onto the other page rather than vice versa,
	 * to avoid having to adjust ancestor keys.
	 */
	np = FreePageBtreeFindLeftSibling(base, btp);
	if (np != NULL && btp->hdr.nused + np->hdr.nused <= max)
	{
		if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		{
			memcpy(&np->u.leaf_key[np->hdr.nused], &btp->u.leaf_key[0],
				   sizeof(FreePageBtreeLeafKey) * btp->hdr.nused);
			np->hdr.nused += btp->hdr.nused;
		}
		else
		{
			memcpy(&np->u.internal_key[np->hdr.nused], &btp->u.internal_key[0],
				   sizeof(FreePageBtreeInternalKey) * btp->hdr.nused);
			np->hdr.nused += btp->hdr.nused;
			FreePageBtreeUpdateParentPointers(base, np);
		}
		FreePageBtreeRemovePage(fpm, btp);
		return;
	}
}

/*
 * Find the passed page's left sibling; that is, the page at the same level
 * of the tree whose keyspace immediately precedes ours.
 */
static FreePageBtree *
FreePageBtreeFindLeftSibling(char *base, FreePageBtree *btp)
{
	FreePageBtree *p = btp;
	int			levels = 0;

	/* Move up until we can move left. */
	for (;;)
	{
		Size		first_page;
		Size		index;

		first_page = FreePageBtreeFirstKey(p);
		p = relptr_access(base, p->hdr.parent);

		if (p == NULL)
			return NULL;		/* we were passed the rightmost page */

		index = FreePageBtreeSearchInternal(p, first_page);
		if (index > 0)
		{
			Assert(p->u.internal_key[index].first_page == first_page);
			p = relptr_access(base, p->u.internal_key[index - 1].child);
			break;
		}
		Assert(index == 0);
		++levels;
	}

	/* Descend left. */
	while (levels > 0)
	{
		Assert(p->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		p = relptr_access(base, p->u.internal_key[p->hdr.nused - 1].child);
		--levels;
	}
	Assert(p->hdr.magic == btp->hdr.magic);

	return p;
}

/*
 * Find the passed page's right sibling; that is, the page at the same level
 * of the tree whose keyspace immediately follows ours.
 */
static FreePageBtree *
FreePageBtreeFindRightSibling(char *base, FreePageBtree *btp)
{
	FreePageBtree *p = btp;
	int			levels = 0;

	/* Move up until we can move right. */
	for (;;)
	{
		Size		first_page;
		Size		index;

		first_page = FreePageBtreeFirstKey(p);
		p = relptr_access(base, p->hdr.parent);

		if (p == NULL)
			return NULL;		/* we were passed the rightmost page */

		index = FreePageBtreeSearchInternal(p, first_page);
		if (index < p->hdr.nused - 1)
		{
			Assert(p->u.internal_key[index].first_page == first_page);
			p = relptr_access(base, p->u.internal_key[index + 1].child);
			break;
		}
		Assert(index == p->hdr.nused - 1);
		++levels;
	}

	/* Descend left. */
	while (levels > 0)
	{
		Assert(p->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		p = relptr_access(base, p->u.internal_key[0].child);
		--levels;
	}
	Assert(p->hdr.magic == btp->hdr.magic);

	return p;
}

/*
 * Get the first key on a btree page.
 */
static Size
FreePageBtreeFirstKey(FreePageBtree *btp)
{
	Assert(btp->hdr.nused > 0);

	if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		return btp->u.leaf_key[0].first_page;
	else
	{
		Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		return btp->u.internal_key[0].first_page;
	}
}

/*
 * Get a page from the btree recycle list for use as a btree page.
 */
static FreePageBtree *
FreePageBtreeGetRecycled(FreePageManager *fpm)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageSpanLeader *victim = relptr_access(base, fpm->btree_recycle);
	FreePageSpanLeader *newhead;

	Assert(victim != NULL);
	newhead = relptr_access(base, victim->next);
	if (newhead != NULL)
		relptr_copy(newhead->prev, victim->prev);
	relptr_store(base, fpm->btree_recycle, newhead);
	Assert(fpm_pointer_is_page_aligned(base, victim));
	fpm->btree_recycle_count--;
	return (FreePageBtree *) victim;
}

/*
 * Insert an item into an internal page.
 */
static void
FreePageBtreeInsertInternal(char *base, FreePageBtree *btp, Size index,
							Size first_page, FreePageBtree *child)
{
	Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
	Assert(btp->hdr.nused <= FPM_ITEMS_PER_INTERNAL_PAGE);
	Assert(index <= btp->hdr.nused);
	memmove(&btp->u.internal_key[index + 1], &btp->u.internal_key[index],
			sizeof(FreePageBtreeInternalKey) * (btp->hdr.nused - index));
	btp->u.internal_key[index].first_page = first_page;
	relptr_store(base, btp->u.internal_key[index].child, child);
	++btp->hdr.nused;
}

/*
 * Insert an item into a leaf page.
 */
static void
FreePageBtreeInsertLeaf(FreePageBtree *btp, Size index, Size first_page,
						Size npages)
{
	Assert(btp->hdr.magic == FREE_PAGE_LEAF_MAGIC);
	Assert(btp->hdr.nused <= FPM_ITEMS_PER_LEAF_PAGE);
	Assert(index <= btp->hdr.nused);
	memmove(&btp->u.leaf_key[index + 1], &btp->u.leaf_key[index],
			sizeof(FreePageBtreeLeafKey) * (btp->hdr.nused - index));
	btp->u.leaf_key[index].first_page = first_page;
	btp->u.leaf_key[index].npages = npages;
	++btp->hdr.nused;
}

/*
 * Put a page on the btree recycle list.
 */
static void
FreePageBtreeRecycle(FreePageManager *fpm, Size pageno)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageSpanLeader *head = relptr_access(base, fpm->btree_recycle);
	FreePageSpanLeader *span;

	span = (FreePageSpanLeader *) fpm_page_to_pointer(base, pageno);
	span->magic = FREE_PAGE_SPAN_LEADER_MAGIC;
	span->npages = 1;
	relptr_store(base, span->next, head);
	relptr_store(base, span->prev, (FreePageSpanLeader *) NULL);
	if (head != NULL)
		relptr_store(base, head->prev, span);
	relptr_store(base, fpm->btree_recycle, span);
	fpm->btree_recycle_count++;
}

/*
 * Remove an item from the btree at the given position on the given page.
 */
static void
FreePageBtreeRemove(FreePageManager *fpm, FreePageBtree *btp, Size index)
{
	Assert(btp->hdr.magic == FREE_PAGE_LEAF_MAGIC);
	Assert(index < btp->hdr.nused);

	/* When last item is removed, extirpate entire page from btree. */
	if (btp->hdr.nused == 1)
	{
		FreePageBtreeRemovePage(fpm, btp);
		return;
	}

	/* Physically remove the key from the page. */
	--btp->hdr.nused;
	if (index < btp->hdr.nused)
		memmove(&btp->u.leaf_key[index], &btp->u.leaf_key[index + 1],
				sizeof(FreePageBtreeLeafKey) * (btp->hdr.nused - index));

	/* If we just removed the first key, adjust ancestor keys. */
	if (index == 0)
		FreePageBtreeAdjustAncestorKeys(fpm, btp);

	/* Consider whether to consolidate this page with a sibling. */
	FreePageBtreeConsolidate(fpm, btp);
}

/*
 * Remove a page from the btree.  Caller is responsible for having relocated
 * any keys from this page that are still wanted.  The page is placed on the
 * recycled list.
 */
static void
FreePageBtreeRemovePage(FreePageManager *fpm, FreePageBtree *btp)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageBtree *parent;
	Size		index;
	Size		first_page;

	for (;;)
	{
		/* Find parent page. */
		parent = relptr_access(base, btp->hdr.parent);
		if (parent == NULL)
		{
			/* We are removing the root page. */
			relptr_store(base, fpm->btree_root, (FreePageBtree *) NULL);
			fpm->btree_depth = 0;
			Assert(fpm->singleton_first_page == 0);
			Assert(fpm->singleton_npages == 0);
			return;
		}

		/*
		 * If the parent contains only one item, we need to remove it as well.
		 */
		if (parent->hdr.nused > 1)
			break;
		FreePageBtreeRecycle(fpm, fpm_pointer_to_page(base, btp));
		btp = parent;
	}

	/* Find and remove the downlink. */
	first_page = FreePageBtreeFirstKey(btp);
	if (parent->hdr.magic == FREE_PAGE_LEAF_MAGIC)
	{
		index = FreePageBtreeSearchLeaf(parent, first_page);
		Assert(index < parent->hdr.nused);
		if (index < parent->hdr.nused - 1)
			memmove(&parent->u.leaf_key[index],
					&parent->u.leaf_key[index + 1],
					sizeof(FreePageBtreeLeafKey)
					* (parent->hdr.nused - index - 1));
	}
	else
	{
		index = FreePageBtreeSearchInternal(parent, first_page);
		Assert(index < parent->hdr.nused);
		if (index < parent->hdr.nused - 1)
			memmove(&parent->u.internal_key[index],
					&parent->u.internal_key[index + 1],
					sizeof(FreePageBtreeInternalKey)
					* (parent->hdr.nused - index - 1));
	}
	parent->hdr.nused--;
	Assert(parent->hdr.nused > 0);

	/* Recycle the page. */
	FreePageBtreeRecycle(fpm, fpm_pointer_to_page(base, btp));

	/* Adjust ancestor keys if needed. */
	if (index == 0)
		FreePageBtreeAdjustAncestorKeys(fpm, parent);

	/* Consider whether to consolidate the parent with a sibling. */
	FreePageBtreeConsolidate(fpm, parent);
}

/*
 * Search the btree for an entry for the given first page and initialize
 * *result with the results of the search.  result->page and result->index
 * indicate either the position of an exact match or the position at which
 * the new key should be inserted.  result->found is true for an exact match,
 * otherwise false.  result->split_pages will contain the number of additional
 * btree pages that will be needed when performing a split to insert a key.
 * Except as described above, the contents of fields in the result object are
 * undefined on return.
 */
static void
FreePageBtreeSearch(FreePageManager *fpm, Size first_page,
					FreePageBtreeSearchResult *result)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageBtree *btp = relptr_access(base, fpm->btree_root);
	Size		index;

	result->split_pages = 1;

	/* If the btree is empty, there's nothing to find. */
	if (btp == NULL)
	{
		result->page = NULL;
		result->found = false;
		return;
	}

	/* Descend until we hit a leaf. */
	while (btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC)
	{
		FreePageBtree *child;
		bool		found_exact;

		index = FreePageBtreeSearchInternal(btp, first_page);
		found_exact = index < btp->hdr.nused &&
			btp->u.internal_key[index].first_page == first_page;

		/*
		 * If we found an exact match we descend directly.  Otherwise, we
		 * descend into the child to the left if possible so that we can find
		 * the insertion point at that child's high end.
		 */
		if (!found_exact && index > 0)
			--index;

		/* Track required split depth for leaf insert. */
		if (btp->hdr.nused >= FPM_ITEMS_PER_INTERNAL_PAGE)
		{
			Assert(btp->hdr.nused == FPM_ITEMS_PER_INTERNAL_PAGE);
			result->split_pages++;
		}
		else
			result->split_pages = 0;

		/* Descend to appropriate child page. */
		Assert(index < btp->hdr.nused);
		child = relptr_access(base, btp->u.internal_key[index].child);
		Assert(relptr_access(base, child->hdr.parent) == btp);
		btp = child;
	}

	/* Track required split depth for leaf insert. */
	if (btp->hdr.nused >= FPM_ITEMS_PER_LEAF_PAGE)
	{
		Assert(btp->hdr.nused == FPM_ITEMS_PER_INTERNAL_PAGE);
		result->split_pages++;
	}
	else
		result->split_pages = 0;

	/* Search leaf page. */
	index = FreePageBtreeSearchLeaf(btp, first_page);

	/* Assemble results. */
	result->page = btp;
	result->index = index;
	result->found = index < btp->hdr.nused &&
		first_page == btp->u.leaf_key[index].first_page;
}

/*
 * Search an internal page for the first key greater than or equal to a given
 * page number.  Returns the index of that key, or one greater than the number
 * of keys on the page if none.
 */
static Size
FreePageBtreeSearchInternal(FreePageBtree *btp, Size first_page)
{
	Size		low = 0;
	Size		high = btp->hdr.nused;

	Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
	Assert(high > 0 && high <= FPM_ITEMS_PER_INTERNAL_PAGE);

	while (low < high)
	{
		Size		mid = (low + high) / 2;
		Size		val = btp->u.internal_key[mid].first_page;

		if (first_page == val)
			return mid;
		else if (first_page < val)
			high = mid;
		else
			low = mid + 1;
	}

	return low;
}

/*
 * Search a leaf page for the first key greater than or equal to a given
 * page number.  Returns the index of that key, or one greater than the number
 * of keys on the page if none.
 */
static Size
FreePageBtreeSearchLeaf(FreePageBtree *btp, Size first_page)
{
	Size		low = 0;
	Size		high = btp->hdr.nused;

	Assert(btp->hdr.magic == FREE_PAGE_LEAF_MAGIC);
	Assert(high > 0 && high <= FPM_ITEMS_PER_LEAF_PAGE);

	while (low < high)
	{
		Size		mid = (low + high) / 2;
		Size		val = btp->u.leaf_key[mid].first_page;

		if (first_page == val)
			return mid;
		else if (first_page < val)
			high = mid;
		else
			low = mid + 1;
	}

	return low;
}

/*
 * Allocate a new btree page and move half the keys from the provided page
 * to the new page.  Caller is responsible for making sure that there's a
 * page available from fpm->btree_recycle.  Returns a pointer to the new page,
 * to which caller must add a downlink.
 */
static FreePageBtree *
FreePageBtreeSplitPage(FreePageManager *fpm, FreePageBtree *btp)
{
	FreePageBtree *newsibling;

	newsibling = FreePageBtreeGetRecycled(fpm);
	newsibling->hdr.magic = btp->hdr.magic;
	newsibling->hdr.nused = btp->hdr.nused / 2;
	relptr_copy(newsibling->hdr.parent, btp->hdr.parent);
	btp->hdr.nused -= newsibling->hdr.nused;

	if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		memcpy(&newsibling->u.leaf_key,
			   &btp->u.leaf_key[btp->hdr.nused],
			   sizeof(FreePageBtreeLeafKey) * newsibling->hdr.nused);
	else
	{
		Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		memcpy(&newsibling->u.internal_key,
			   &btp->u.internal_key[btp->hdr.nused],
			   sizeof(FreePageBtreeInternalKey) * newsibling->hdr.nused);
		FreePageBtreeUpdateParentPointers(fpm_segment_base(fpm), newsibling);
	}

	return newsibling;
}

/*
 * When internal pages are split or merged, the parent pointers of their
 * children must be updated.
 */
static void
FreePageBtreeUpdateParentPointers(char *base, FreePageBtree *btp)
{
	Size		i;

	Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
	for (i = 0; i < btp->hdr.nused; ++i)
	{
		FreePageBtree *child;

		child = relptr_access(base, btp->u.internal_key[i].child);
		relptr_store(base, child->hdr.parent, btp);
	}
}

/*
 * Debugging dump of btree data.
 */
static void
FreePageManagerDumpBtree(FreePageManager *fpm, FreePageBtree *btp,
						 FreePageBtree *parent, int level, StringInfo buf)
{
	char	   *base = fpm_segment_base(fpm);
	Size		pageno = fpm_pointer_to_page(base, btp);
	Size		index;
	FreePageBtree *check_parent;

	check_stack_depth();
	check_parent = relptr_access(base, btp->hdr.parent);
	appendStringInfo(buf, "  %zu@%d %c", pageno, level,
					 btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC ? 'i' : 'l');
	if (parent != check_parent)
		appendStringInfo(buf, " [actual parent %zu, expected %zu]",
						 fpm_pointer_to_page(base, check_parent),
						 fpm_pointer_to_page(base, parent));
	appendStringInfoChar(buf, ':');
	for (index = 0; index < btp->hdr.nused; ++index)
	{
		if (btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC)
			appendStringInfo(buf, " %zu->%zu",
							 btp->u.internal_key[index].first_page,
							 relptr_offset(btp->u.internal_key[index].child) / FPM_PAGE_SIZE);
		else
			appendStringInfo(buf, " %zu(%zu)",
							 btp->u.leaf_key[index].first_page,
							 btp->u.leaf_key[index].npages);
	}
	appendStringInfoChar(buf, '\n');

	if (btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC)
	{
		for (index = 0; index < btp->hdr.nused; ++index)
		{
			FreePageBtree *child;

			child = relptr_access(base, btp->u.internal_key[index].child);
			FreePageManagerDumpBtree(fpm, child, btp, level + 1, buf);
		}
	}
}

/*
 * Debugging dump of free-span data.
 */
static void
FreePageManagerDumpSpans(FreePageManager *fpm, FreePageSpanLeader *span,
						 Size expected_pages, StringInfo buf)
{
	char	   *base = fpm_segment_base(fpm);

	while (span != NULL)
	{
		if (span->npages != expected_pages)
			appendStringInfo(buf, " %zu(%zu)", fpm_pointer_to_page(base, span),
							 span->npages);
		else
			appendStringInfo(buf, " %zu", fpm_pointer_to_page(base, span));
		span = relptr_access(base, span->next);
	}

	appendStringInfoChar(buf, '\n');
}

/*
 * This function allocates a run of pages of the given length from the free
 * page manager.
 */
static bool
FreePageManagerGetInternal(FreePageManager *fpm, Size npages, Size *first_page)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageSpanLeader *victim = NULL;
	FreePageSpanLeader *prev;
	FreePageSpanLeader *next;
	FreePageBtreeSearchResult result;
	Size		victim_page = 0;	/* placate compiler */
	Size		f;

	/*
	 * Search for a free span.
	 *
	 * Right now, we use a simple best-fit policy here, but it's possible for
	 * this to result in memory fragmentation if we're repeatedly asked to
	 * allocate chunks just a little smaller than what we have available.
	 * Hopefully, this is unlikely, because we expect most requests to be
	 * single pages or superblock-sized chunks -- but no policy can be optimal
	 * under all circumstances unless it has knowledge of future allocation
	 * patterns.
	 */
	for (f = Min(npages, FPM_NUM_FREELISTS) - 1; f < FPM_NUM_FREELISTS; ++f)
	{
		/* Skip empty freelists. */
		if (relptr_is_null(fpm->freelist[f]))
			continue;

		/*
		 * All of the freelists except the last one contain only items of a
		 * single size, so we just take the first one.  But the final free
		 * list contains everything too big for any of the other lists, so we
		 * need to search the list.
		 */
		if (f < FPM_NUM_FREELISTS - 1)
			victim = relptr_access(base, fpm->freelist[f]);
		else
		{
			FreePageSpanLeader *candidate;

			candidate = relptr_access(base, fpm->freelist[f]);
			do
			{
				if (candidate->npages >= npages && (victim == NULL ||
													victim->npages > candidate->npages))
				{
					victim = candidate;
					if (victim->npages == npages)
						break;
				}
				candidate = relptr_access(base, candidate->next);
			} while (candidate != NULL);
		}
		break;
	}

	/* If we didn't find an allocatable span, return failure. */
	if (victim == NULL)
		return false;

	/* Remove span from free list. */
	Assert(victim->magic == FREE_PAGE_SPAN_LEADER_MAGIC);
	prev = relptr_access(base, victim->prev);
	next = relptr_access(base, victim->next);
	if (prev != NULL)
		relptr_copy(prev->next, victim->next);
	else
		relptr_copy(fpm->freelist[f], victim->next);
	if (next != NULL)
		relptr_copy(next->prev, victim->prev);
	victim_page = fpm_pointer_to_page(base, victim);

	/* Decide whether we might be invalidating contiguous_pages. */
	if (f == FPM_NUM_FREELISTS - 1 &&
		victim->npages == fpm->contiguous_pages)
	{
		/*
		 * The victim span came from the oversized freelist, and had the same
		 * size as the longest span.  There may or may not be another one of
		 * the same size, so contiguous_pages must be recomputed just to be
		 * safe.
		 */
		fpm->contiguous_pages_dirty = true;
	}
	else if (f + 1 == fpm->contiguous_pages &&
			 relptr_is_null(fpm->freelist[f]))
	{
		/*
		 * The victim span came from a fixed sized freelist, and it was the
		 * list for spans of the same size as the current longest span, and
		 * the list is now empty after removing the victim.  So
		 * contiguous_pages must be recomputed without a doubt.
		 */
		fpm->contiguous_pages_dirty = true;
	}

	/*
	 * If we haven't initialized the btree yet, the victim must be the single
	 * span stored within the FreePageManager itself.  Otherwise, we need to
	 * update the btree.
	 */
	if (relptr_is_null(fpm->btree_root))
	{
		Assert(victim_page == fpm->singleton_first_page);
		Assert(victim->npages == fpm->singleton_npages);
		Assert(victim->npages >= npages);
		fpm->singleton_first_page += npages;
		fpm->singleton_npages -= npages;
		if (fpm->singleton_npages > 0)
			FreePagePushSpanLeader(fpm, fpm->singleton_first_page,
								   fpm->singleton_npages);
	}
	else
	{
		/*
		 * If the span we found is exactly the right size, remove it from the
		 * btree completely.  Otherwise, adjust the btree entry to reflect the
		 * still-unallocated portion of the span, and put that portion on the
		 * appropriate free list.
		 */
		FreePageBtreeSearch(fpm, victim_page, &result);
		Assert(result.found);
		if (victim->npages == npages)
			FreePageBtreeRemove(fpm, result.page, result.index);
		else
		{
			FreePageBtreeLeafKey *key;

			/* Adjust btree to reflect remaining pages. */
			Assert(victim->npages > npages);
			key = &result.page->u.leaf_key[result.index];
			Assert(key->npages == victim->npages);
			key->first_page += npages;
			key->npages -= npages;
			if (result.index == 0)
				FreePageBtreeAdjustAncestorKeys(fpm, result.page);

			/* Put the unallocated pages back on the appropriate free list. */
			FreePagePushSpanLeader(fpm, victim_page + npages,
								   victim->npages - npages);
		}
	}

	/* Return results to caller. */
	*first_page = fpm_pointer_to_page(base, victim);
	return true;
}

/*
 * Put a range of pages into the btree and freelists, consolidating it with
 * existing free spans just before and/or after it.  If 'soft' is true,
 * only perform the insertion if it can be done without allocating new btree
 * pages; if false, do it always.  Returns 0 if the soft flag caused the
 * insertion to be skipped, or otherwise the size of the contiguous span
 * created by the insertion.  This may be larger than npages if we're able
 * to consolidate with an adjacent range.
 */
static Size
FreePageManagerPutInternal(FreePageManager *fpm, Size first_page, Size npages,
						   bool soft)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageBtreeSearchResult result;
	FreePageBtreeLeafKey *prevkey = NULL;
	FreePageBtreeLeafKey *nextkey = NULL;
	FreePageBtree *np;
	Size		nindex;

	Assert(npages > 0);

	/* We can store a single free span without initializing the btree. */
	if (fpm->btree_depth == 0)
	{
		if (fpm->singleton_npages == 0)
		{
			/* Don't have a span yet; store this one. */
			fpm->singleton_first_page = first_page;
			fpm->singleton_npages = npages;
			FreePagePushSpanLeader(fpm, first_page, npages);
			return fpm->singleton_npages;
		}
		else if (fpm->singleton_first_page + fpm->singleton_npages ==
				 first_page)
		{
			/* New span immediately follows sole existing span. */
			fpm->singleton_npages += npages;
			FreePagePopSpanLeader(fpm, fpm->singleton_first_page);
			FreePagePushSpanLeader(fpm, fpm->singleton_first_page,
								   fpm->singleton_npages);
			return fpm->singleton_npages;
		}
		else if (first_page + npages == fpm->singleton_first_page)
		{
			/* New span immediately precedes sole existing span. */
			FreePagePopSpanLeader(fpm, fpm->singleton_first_page);
			fpm->singleton_first_page = first_page;
			fpm->singleton_npages += npages;
			FreePagePushSpanLeader(fpm, fpm->singleton_first_page,
								   fpm->singleton_npages);
			return fpm->singleton_npages;
		}
		else
		{
			/* Not contiguous; we need to initialize the btree. */
			Size		root_page;
			FreePageBtree *root;

			if (!relptr_is_null(fpm->btree_recycle))
				root = FreePageBtreeGetRecycled(fpm);
			else if (soft)
				return 0;		/* Should not allocate if soft. */
			else if (FreePageManagerGetInternal(fpm, 1, &root_page))
				root = (FreePageBtree *) fpm_page_to_pointer(base, root_page);
			else
			{
				/* We'd better be able to get a page from the existing range. */
				elog(FATAL, "free page manager btree is corrupt");
			}

			/* Create the btree and move the preexisting range into it. */
			root->hdr.magic = FREE_PAGE_LEAF_MAGIC;
			root->hdr.nused = 1;
			relptr_store(base, root->hdr.parent, (FreePageBtree *) NULL);
			root->u.leaf_key[0].first_page = fpm->singleton_first_page;
			root->u.leaf_key[0].npages = fpm->singleton_npages;
			relptr_store(base, fpm->btree_root, root);
			fpm->singleton_first_page = 0;
			fpm->singleton_npages = 0;
			fpm->btree_depth = 1;

			/*
			 * Corner case: it may be that the btree root took the very last
			 * free page.  In that case, the sole btree entry covers a zero
			 * page run, which is invalid.  Overwrite it with the entry we're
			 * trying to insert and get out.
			 */
			if (root->u.leaf_key[0].npages == 0)
			{
				root->u.leaf_key[0].first_page = first_page;
				root->u.leaf_key[0].npages = npages;
				FreePagePushSpanLeader(fpm, first_page, npages);
				return npages;
			}

			/* Fall through to insert the new key. */
		}
	}

	/* Search the btree. */
	FreePageBtreeSearch(fpm, first_page, &result);
	Assert(!result.found);
	if (result.index > 0)
		prevkey = &result.page->u.leaf_key[result.index - 1];
	if (result.index < result.page->hdr.nused)
	{
		np = result.page;
		nindex = result.index;
		nextkey = &result.page->u.leaf_key[result.index];
	}
	else
	{
		np = FreePageBtreeFindRightSibling(base, result.page);
		nindex = 0;
		if (np != NULL)
			nextkey = &np->u.leaf_key[0];
	}

	/* Consolidate with the previous entry if possible. */
	if (prevkey != NULL && prevkey->first_page + prevkey->npages >= first_page)
	{
		bool		remove_next = false;
		Size		result;

		Assert(prevkey->first_page + prevkey->npages == first_page);
		prevkey->npages = (first_page - prevkey->first_page) + npages;

		/* Check whether we can *also* consolidate with the following entry. */
		if (nextkey != NULL &&
			prevkey->first_page + prevkey->npages >= nextkey->first_page)
		{
			Assert(prevkey->first_page + prevkey->npages ==
				   nextkey->first_page);
			prevkey->npages = (nextkey->first_page - prevkey->first_page)
				+ nextkey->npages;
			FreePagePopSpanLeader(fpm, nextkey->first_page);
			remove_next = true;
		}

		/* Put the span on the correct freelist and save size. */
		FreePagePopSpanLeader(fpm, prevkey->first_page);
		FreePagePushSpanLeader(fpm, prevkey->first_page, prevkey->npages);
		result = prevkey->npages;

		/*
		 * If we consolidated with both the preceding and following entries,
		 * we must remove the following entry.  We do this last, because
		 * removing an element from the btree may invalidate pointers we hold
		 * into the current data structure.
		 *
		 * NB: The btree is technically in an invalid state a this point
		 * because we've already updated prevkey to cover the same key space
		 * as nextkey.  FreePageBtreeRemove() shouldn't notice that, though.
		 */
		if (remove_next)
			FreePageBtreeRemove(fpm, np, nindex);

		return result;
	}

	/* Consolidate with the next entry if possible. */
	if (nextkey != NULL && first_page + npages >= nextkey->first_page)
	{
		Size		newpages;

		/* Compute new size for span. */
		Assert(first_page + npages == nextkey->first_page);
		newpages = (nextkey->first_page - first_page) + nextkey->npages;

		/* Put span on correct free list. */
		FreePagePopSpanLeader(fpm, nextkey->first_page);
		FreePagePushSpanLeader(fpm, first_page, newpages);

		/* Update key in place. */
		nextkey->first_page = first_page;
		nextkey->npages = newpages;

		/* If reducing first key on page, ancestors might need adjustment. */
		if (nindex == 0)
			FreePageBtreeAdjustAncestorKeys(fpm, np);

		return nextkey->npages;
	}

	/* Split leaf page and as many of its ancestors as necessary. */
	if (result.split_pages > 0)
	{
		/*
		 * NB: We could consider various coping strategies here to avoid a
		 * split; most obviously, if np != result.page, we could target that
		 * page instead.   More complicated shuffling strategies could be
		 * possible as well; basically, unless every single leaf page is 100%
		 * full, we can jam this key in there if we try hard enough.  It's
		 * unlikely that trying that hard is worthwhile, but it's possible we
		 * might need to make more than no effort.  For now, we just do the
		 * easy thing, which is nothing.
		 */

		/* If this is a soft insert, it's time to give up. */
		if (soft)
			return 0;

		/* Check whether we need to allocate more btree pages to split. */
		if (result.split_pages > fpm->btree_recycle_count)
		{
			Size		pages_needed;
			Size		recycle_page;
			Size		i;

			/*
			 * Allocate the required number of pages and split each one in
			 * turn.  This should never fail, because if we've got enough
			 * spans of free pages kicking around that we need additional
			 * storage space just to remember them all, then we should
			 * certainly have enough to expand the btree, which should only
			 * ever use a tiny number of pages compared to the number under
			 * management.  If it does, something's badly screwed up.
			 */
			pages_needed = result.split_pages - fpm->btree_recycle_count;
			for (i = 0; i < pages_needed; ++i)
			{
				if (!FreePageManagerGetInternal(fpm, 1, &recycle_page))
					elog(FATAL, "free page manager btree is corrupt");
				FreePageBtreeRecycle(fpm, recycle_page);
			}

			/*
			 * The act of allocating pages to recycle may have invalidated the
			 * results of our previous btree research, so repeat it. (We could
			 * recheck whether any of our split-avoidance strategies that were
			 * not viable before now are, but it hardly seems worthwhile, so
			 * we don't bother. Consolidation can't be possible now if it
			 * wasn't previously.)
			 */
			FreePageBtreeSearch(fpm, first_page, &result);

			/*
			 * The act of allocating pages for use in constructing our btree
			 * should never cause any page to become more full, so the new
			 * split depth should be no greater than the old one, and perhaps
			 * less if we fortuitously allocated a chunk that freed up a slot
			 * on the page we need to update.
			 */
			Assert(result.split_pages <= fpm->btree_recycle_count);
		}

		/* If we still need to perform a split, do it. */
		if (result.split_pages > 0)
		{
			FreePageBtree *split_target = result.page;
			FreePageBtree *child = NULL;
			Size		key = first_page;

			for (;;)
			{
				FreePageBtree *newsibling;
				FreePageBtree *parent;

				/* Identify parent page, which must receive downlink. */
				parent = relptr_access(base, split_target->hdr.parent);

				/* Split the page - downlink not added yet. */
				newsibling = FreePageBtreeSplitPage(fpm, split_target);

				/*
				 * At this point in the loop, we're always carrying a pending
				 * insertion.  On the first pass, it's the actual key we're
				 * trying to insert; on subsequent passes, it's the downlink
				 * that needs to be added as a result of the split performed
				 * during the previous loop iteration.  Since we've just split
				 * the page, there's definitely room on one of the two
				 * resulting pages.
				 */
				if (child == NULL)
				{
					Size		index;
					FreePageBtree *insert_into;

					insert_into = key < newsibling->u.leaf_key[0].first_page ?
						split_target : newsibling;
					index = FreePageBtreeSearchLeaf(insert_into, key);
					FreePageBtreeInsertLeaf(insert_into, index, key, npages);
					if (index == 0 && insert_into == split_target)
						FreePageBtreeAdjustAncestorKeys(fpm, split_target);
				}
				else
				{
					Size		index;
					FreePageBtree *insert_into;

					insert_into =
						key < newsibling->u.internal_key[0].first_page ?
						split_target : newsibling;
					index = FreePageBtreeSearchInternal(insert_into, key);
					FreePageBtreeInsertInternal(base, insert_into, index,
												key, child);
					relptr_store(base, child->hdr.parent, insert_into);
					if (index == 0 && insert_into == split_target)
						FreePageBtreeAdjustAncestorKeys(fpm, split_target);
				}

				/* If the page we just split has no parent, split the root. */
				if (parent == NULL)
				{
					FreePageBtree *newroot;

					newroot = FreePageBtreeGetRecycled(fpm);
					newroot->hdr.magic = FREE_PAGE_INTERNAL_MAGIC;
					newroot->hdr.nused = 2;
					relptr_store(base, newroot->hdr.parent,
								 (FreePageBtree *) NULL);
					newroot->u.internal_key[0].first_page =
						FreePageBtreeFirstKey(split_target);
					relptr_store(base, newroot->u.internal_key[0].child,
								 split_target);
					relptr_store(base, split_target->hdr.parent, newroot);
					newroot->u.internal_key[1].first_page =
						FreePageBtreeFirstKey(newsibling);
					relptr_store(base, newroot->u.internal_key[1].child,
								 newsibling);
					relptr_store(base, newsibling->hdr.parent, newroot);
					relptr_store(base, fpm->btree_root, newroot);
					fpm->btree_depth++;

					break;
				}

				/* If the parent page isn't full, insert the downlink. */
				key = newsibling->u.internal_key[0].first_page;
				if (parent->hdr.nused < FPM_ITEMS_PER_INTERNAL_PAGE)
				{
					Size		index;

					index = FreePageBtreeSearchInternal(parent, key);
					FreePageBtreeInsertInternal(base, parent, index,
												key, newsibling);
					relptr_store(base, newsibling->hdr.parent, parent);
					if (index == 0)
						FreePageBtreeAdjustAncestorKeys(fpm, parent);
					break;
				}

				/* The parent also needs to be split, so loop around. */
				child = newsibling;
				split_target = parent;
			}

			/*
			 * The loop above did the insert, so just need to update the free
			 * list, and we're done.
			 */
			FreePagePushSpanLeader(fpm, first_page, npages);

			return npages;
		}
	}

	/* Physically add the key to the page. */
	Assert(result.page->hdr.nused < FPM_ITEMS_PER_LEAF_PAGE);
	FreePageBtreeInsertLeaf(result.page, result.index, first_page, npages);

	/* If new first key on page, ancestors might need adjustment. */
	if (result.index == 0)
		FreePageBtreeAdjustAncestorKeys(fpm, result.page);

	/* Put it on the free list. */
	FreePagePushSpanLeader(fpm, first_page, npages);

	return npages;
}

/*
 * Remove a FreePageSpanLeader from the linked-list that contains it, either
 * because we're changing the size of the span, or because we're allocating it.
 */
static void
FreePagePopSpanLeader(FreePageManager *fpm, Size pageno)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageSpanLeader *span;
	FreePageSpanLeader *next;
	FreePageSpanLeader *prev;

	span = (FreePageSpanLeader *) fpm_page_to_pointer(base, pageno);

	next = relptr_access(base, span->next);
	prev = relptr_access(base, span->prev);
	if (next != NULL)
		relptr_copy(next->prev, span->prev);
	if (prev != NULL)
		relptr_copy(prev->next, span->next);
	else
	{
		Size		f = Min(span->npages, FPM_NUM_FREELISTS) - 1;

		Assert(relptr_offset(fpm->freelist[f]) == pageno * FPM_PAGE_SIZE);
		relptr_copy(fpm->freelist[f], span->next);
	}
}

/*
 * Initialize a new FreePageSpanLeader and put it on the appropriate free list.
 */
static void
FreePagePushSpanLeader(FreePageManager *fpm, Size first_page, Size npages)
{
	char	   *base = fpm_segment_base(fpm);
	Size		f = Min(npages, FPM_NUM_FREELISTS) - 1;
	FreePageSpanLeader *head = relptr_access(base, fpm->freelist[f]);
	FreePageSpanLeader *span;

	span = (FreePageSpanLeader *) fpm_page_to_pointer(base, first_page);
	span->magic = FREE_PAGE_SPAN_LEADER_MAGIC;
	span->npages = npages;
	relptr_store(base, span->next, head);
	relptr_store(base, span->prev, (FreePageSpanLeader *) NULL);
	if (head != NULL)
		relptr_store(base, head->prev, span);
	relptr_store(base, fpm->freelist[f], span);
}
