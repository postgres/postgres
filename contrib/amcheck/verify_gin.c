/*-------------------------------------------------------------------------
 *
 * verify_gin.c
 *		Verifies the integrity of GIN indexes based on invariants.
 *
 *
 * GIN index verification checks a number of invariants:
 *
 * - consistency: Paths in GIN graph have to contain consistent keys: tuples
 *   on parent pages consistently include tuples from children pages.
 *
 * - graph invariants: Each internal page must have at least one downlink, and
 *   can reference either only leaf pages or only internal pages.
 *
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/amcheck/verify_gin.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin_private.h"
#include "access/nbtree.h"
#include "catalog/pg_am.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "verify_common.h"
#include "string.h"

/*
 * GinScanItem represents one item of depth-first scan of the index.
 */
typedef struct GinScanItem
{
	int			depth;
	IndexTuple	parenttup;
	BlockNumber parentblk;
	BlockNumber blkno;
	struct GinScanItem *next;
} GinScanItem;

/*
 * GinPostingTreeScanItem represents one item of a depth-first posting tree scan.
 */
typedef struct GinPostingTreeScanItem
{
	int			depth;
	ItemPointerData parentkey;
	BlockNumber parentblk;
	BlockNumber blkno;
	struct GinPostingTreeScanItem *next;
} GinPostingTreeScanItem;


PG_FUNCTION_INFO_V1(gin_index_check);

static void gin_check_parent_keys_consistency(Relation rel,
											  Relation heaprel,
											  void *callback_state, bool readonly);
static void check_index_page(Relation rel, Buffer buffer, BlockNumber blockNo);
static IndexTuple gin_refind_parent(Relation rel,
									BlockNumber parentblkno,
									BlockNumber childblkno,
									BufferAccessStrategy strategy);
static ItemId PageGetItemIdCareful(Relation rel, BlockNumber block, Page page,
								   OffsetNumber offset);

/*
 * gin_index_check(index regclass)
 *
 * Verify integrity of GIN index.
 *
 * Acquires AccessShareLock on heap & index relations.
 */
Datum
gin_index_check(PG_FUNCTION_ARGS)
{
	Oid			indrelid = PG_GETARG_OID(0);

	amcheck_lock_relation_and_check(indrelid,
									GIN_AM_OID,
									gin_check_parent_keys_consistency,
									AccessShareLock,
									NULL);

	PG_RETURN_VOID();
}

/*
 * Read item pointers from leaf entry tuple.
 *
 * Returns a palloc'd array of ItemPointers. The number of items is returned
 * in *nitems.
 */
static ItemPointer
ginReadTupleWithoutState(IndexTuple itup, int *nitems)
{
	Pointer		ptr = GinGetPosting(itup);
	int			nipd = GinGetNPosting(itup);
	ItemPointer ipd;
	int			ndecoded;

	if (GinItupIsCompressed(itup))
	{
		if (nipd > 0)
		{
			ipd = ginPostingListDecode((GinPostingList *) ptr, &ndecoded);
			if (nipd != ndecoded)
				elog(ERROR, "number of items mismatch in GIN entry tuple, %d in tuple header, %d decoded",
					 nipd, ndecoded);
		}
		else
			ipd = palloc(0);
	}
	else
	{
		ipd = (ItemPointer) palloc(sizeof(ItemPointerData) * nipd);
		memcpy(ipd, ptr, sizeof(ItemPointerData) * nipd);
	}
	*nitems = nipd;
	return ipd;
}

/*
 * Scans through a posting tree (given by the root), and verifies that the keys
 * on a child keys are consistent with the parent.
 *
 * Allocates a separate memory context and scans through posting tree graph.
 */
static void
gin_check_posting_tree_parent_keys_consistency(Relation rel, BlockNumber posting_tree_root)
{
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	GinPostingTreeScanItem *stack;
	MemoryContext mctx;
	MemoryContext oldcontext;

	int			leafdepth;

	mctx = AllocSetContextCreate(CurrentMemoryContext,
								 "posting tree check context",
								 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(mctx);

	/*
	 * We don't know the height of the tree yet, but as soon as we encounter a
	 * leaf page, we will set 'leafdepth' to its depth.
	 */
	leafdepth = -1;

	/* Start the scan at the root page */
	stack = (GinPostingTreeScanItem *) palloc0(sizeof(GinPostingTreeScanItem));
	stack->depth = 0;
	ItemPointerSetInvalid(&stack->parentkey);
	stack->parentblk = InvalidBlockNumber;
	stack->blkno = posting_tree_root;

	elog(DEBUG3, "processing posting tree at blk %u", posting_tree_root);

	while (stack)
	{
		GinPostingTreeScanItem *stack_next;
		Buffer		buffer;
		Page		page;
		OffsetNumber i,
					maxoff;
		BlockNumber rightlink;

		CHECK_FOR_INTERRUPTS();

		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, stack->blkno,
									RBM_NORMAL, strategy);
		LockBuffer(buffer, GIN_SHARE);
		page = (Page) BufferGetPage(buffer);

		Assert(GinPageIsData(page));

		/* Check that the tree has the same height in all branches */
		if (GinPageIsLeaf(page))
		{
			ItemPointerData minItem;
			int			nlist;
			ItemPointerData *list;
			char		tidrange_buf[MAXPGPATH];

			ItemPointerSetMin(&minItem);

			elog(DEBUG1, "page blk: %u, type leaf", stack->blkno);

			if (leafdepth == -1)
				leafdepth = stack->depth;
			else if (stack->depth != leafdepth)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\": internal pages traversal encountered leaf page unexpectedly on block %u",
								RelationGetRelationName(rel), stack->blkno)));
			list = GinDataLeafPageGetItems(page, &nlist, minItem);

			if (nlist > 0)
				snprintf(tidrange_buf, sizeof(tidrange_buf),
						 "%d tids (%u, %u) - (%u, %u)",
						 nlist,
						 ItemPointerGetBlockNumberNoCheck(&list[0]),
						 ItemPointerGetOffsetNumberNoCheck(&list[0]),
						 ItemPointerGetBlockNumberNoCheck(&list[nlist - 1]),
						 ItemPointerGetOffsetNumberNoCheck(&list[nlist - 1]));
			else
				snprintf(tidrange_buf, sizeof(tidrange_buf), "0 tids");

			if (stack->parentblk != InvalidBlockNumber)
				elog(DEBUG3, "blk %u: parent %u highkey (%u, %u), %s",
					 stack->blkno,
					 stack->parentblk,
					 ItemPointerGetBlockNumberNoCheck(&stack->parentkey),
					 ItemPointerGetOffsetNumberNoCheck(&stack->parentkey),
					 tidrange_buf);
			else
				elog(DEBUG3, "blk %u: root leaf, %s",
					 stack->blkno,
					 tidrange_buf);

			if (stack->parentblk != InvalidBlockNumber &&
				ItemPointerGetOffsetNumberNoCheck(&stack->parentkey) != InvalidOffsetNumber &&
				nlist > 0 && ItemPointerCompare(&stack->parentkey, &list[nlist - 1]) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\": tid exceeds parent's high key in postingTree leaf on block %u",
								RelationGetRelationName(rel), stack->blkno)));
		}
		else
		{
			LocationIndex pd_lower;
			ItemPointerData bound;
			int			lowersize;

			/*
			 * Check that tuples in each page are properly ordered and
			 * consistent with parent high key
			 */
			maxoff = GinPageGetOpaque(page)->maxoff;
			rightlink = GinPageGetOpaque(page)->rightlink;

			elog(DEBUG1, "page blk: %u, type data, maxoff %d", stack->blkno, maxoff);

			if (stack->parentblk != InvalidBlockNumber)
				elog(DEBUG3, "blk %u: internal posting tree page with %u items, parent %u highkey (%u, %u)",
					 stack->blkno, maxoff, stack->parentblk,
					 ItemPointerGetBlockNumberNoCheck(&stack->parentkey),
					 ItemPointerGetOffsetNumberNoCheck(&stack->parentkey));
			else
				elog(DEBUG3, "blk %u: root internal posting tree page with %u items",
					 stack->blkno, maxoff);

			/*
			 * A GIN posting tree internal page stores PostingItems in the
			 * 'lower' part of the page. The 'upper' part is unused. The
			 * number of elements is stored in the opaque area (maxoff). Make
			 * sure the size of the 'lower' part agrees with 'maxoff'
			 *
			 * We didn't set pd_lower until PostgreSQL version 9.4, so if this
			 * check fails, it could also be because the index was
			 * binary-upgraded from an earlier version. That was a long time
			 * ago, though, so let's warn if it doesn't match.
			 */
			pd_lower = ((PageHeader) page)->pd_lower;
			lowersize = pd_lower - MAXALIGN(SizeOfPageHeaderData);
			if ((lowersize - MAXALIGN(sizeof(ItemPointerData))) / sizeof(PostingItem) != maxoff)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\" has unexpected pd_lower %u in posting tree block %u with maxoff %u)",
								RelationGetRelationName(rel), pd_lower, stack->blkno, maxoff)));

			/*
			 * Before the PostingItems, there's one ItemPointerData in the
			 * 'lower' part that stores the page's high key.
			 */
			bound = *GinDataPageGetRightBound(page);

			/*
			 * Gin page right bound has a sane value only when not a highkey
			 * on the rightmost page (at a given level). For the rightmost
			 * page does not store the highkey explicitly, and the value is
			 * infinity.
			 */
			if (ItemPointerIsValid(&stack->parentkey) &&
				rightlink != InvalidBlockNumber &&
				!ItemPointerEquals(&stack->parentkey, &bound))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\": posting tree page's high key (%u, %u) doesn't match the downlink on block %u (parent blk %u, key (%u, %u))",
								RelationGetRelationName(rel),
								ItemPointerGetBlockNumberNoCheck(&bound),
								ItemPointerGetOffsetNumberNoCheck(&bound),
								stack->blkno, stack->parentblk,
								ItemPointerGetBlockNumberNoCheck(&stack->parentkey),
								ItemPointerGetOffsetNumberNoCheck(&stack->parentkey))));

			for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
			{
				GinPostingTreeScanItem *ptr;
				PostingItem *posting_item = GinDataPageGetPostingItem(page, i);

				/* ItemPointerGetOffsetNumber expects a valid pointer */
				if (!(i == maxoff &&
					  rightlink == InvalidBlockNumber))
					elog(DEBUG3, "key (%u, %u) -> %u",
						 ItemPointerGetBlockNumber(&posting_item->key),
						 ItemPointerGetOffsetNumber(&posting_item->key),
						 BlockIdGetBlockNumber(&posting_item->child_blkno));
				else
					elog(DEBUG3, "key (%u, %u) -> %u",
						 0, 0, BlockIdGetBlockNumber(&posting_item->child_blkno));

				if (i == maxoff && rightlink == InvalidBlockNumber)
				{
					/*
					 * The rightmost item in the tree level has (0, 0) as the
					 * key
					 */
					if (ItemPointerGetBlockNumberNoCheck(&posting_item->key) != 0 ||
						ItemPointerGetOffsetNumberNoCheck(&posting_item->key) != 0)
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("index \"%s\": rightmost posting tree page (blk %u) has unexpected last key (%u, %u)",
										RelationGetRelationName(rel),
										stack->blkno,
										ItemPointerGetBlockNumberNoCheck(&posting_item->key),
										ItemPointerGetOffsetNumberNoCheck(&posting_item->key))));
				}
				else if (i != FirstOffsetNumber)
				{
					PostingItem *previous_posting_item = GinDataPageGetPostingItem(page, i - 1);

					if (ItemPointerCompare(&posting_item->key, &previous_posting_item->key) < 0)
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("index \"%s\" has wrong tuple order in posting tree, block %u, offset %u",
										RelationGetRelationName(rel), stack->blkno, i)));
				}

				/*
				 * Check if this tuple is consistent with the downlink in the
				 * parent.
				 */
				if (i == maxoff && ItemPointerIsValid(&stack->parentkey) &&
					ItemPointerCompare(&stack->parentkey, &posting_item->key) < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("index \"%s\": posting item exceeds parent's high key in postingTree internal page on block %u offset %u",
									RelationGetRelationName(rel),
									stack->blkno, i)));

				/* This is an internal page, recurse into the child. */
				ptr = (GinPostingTreeScanItem *) palloc(sizeof(GinPostingTreeScanItem));
				ptr->depth = stack->depth + 1;

				/*
				 * The rightmost parent key is always invalid item pointer.
				 * Its value is 'Infinity' and not explicitly stored.
				 */
				ptr->parentkey = posting_item->key;
				ptr->parentblk = stack->blkno;
				ptr->blkno = BlockIdGetBlockNumber(&posting_item->child_blkno);
				ptr->next = stack->next;
				stack->next = ptr;
			}
		}
		LockBuffer(buffer, GIN_UNLOCK);
		ReleaseBuffer(buffer);

		/* Step to next item in the queue */
		stack_next = stack->next;
		pfree(stack);
		stack = stack_next;
	}

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(mctx);
}

/*
 * Main entry point for GIN checks.
 *
 * Allocates memory context and scans through the whole GIN graph.
 */
static void
gin_check_parent_keys_consistency(Relation rel,
								  Relation heaprel,
								  void *callback_state,
								  bool readonly)
{
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	GinScanItem *stack;
	MemoryContext mctx;
	MemoryContext oldcontext;
	GinState	state;
	int			leafdepth;

	mctx = AllocSetContextCreate(CurrentMemoryContext,
								 "amcheck consistency check context",
								 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(mctx);
	initGinState(&state, rel);

	/*
	 * We don't know the height of the tree yet, but as soon as we encounter a
	 * leaf page, we will set 'leafdepth' to its depth.
	 */
	leafdepth = -1;

	/* Start the scan at the root page */
	stack = (GinScanItem *) palloc0(sizeof(GinScanItem));
	stack->depth = 0;
	stack->parenttup = NULL;
	stack->parentblk = InvalidBlockNumber;
	stack->blkno = GIN_ROOT_BLKNO;

	while (stack)
	{
		GinScanItem *stack_next;
		Buffer		buffer;
		Page		page;
		OffsetNumber i,
					maxoff,
					prev_attnum;
		IndexTuple	prev_tuple;
		BlockNumber rightlink;

		CHECK_FOR_INTERRUPTS();

		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, stack->blkno,
									RBM_NORMAL, strategy);
		LockBuffer(buffer, GIN_SHARE);
		page = (Page) BufferGetPage(buffer);
		maxoff = PageGetMaxOffsetNumber(page);
		rightlink = GinPageGetOpaque(page)->rightlink;

		/* Do basic sanity checks on the page headers */
		check_index_page(rel, buffer, stack->blkno);

		elog(DEBUG3, "processing entry tree page at blk %u, maxoff: %u", stack->blkno, maxoff);

		/*
		 * It's possible that the page was split since we looked at the
		 * parent, so that we didn't missed the downlink of the right sibling
		 * when we scanned the parent.  If so, add the right sibling to the
		 * stack now.
		 */
		if (stack->parenttup != NULL)
		{
			GinNullCategory parent_key_category;
			Datum		parent_key = gintuple_get_key(&state,
													  stack->parenttup,
													  &parent_key_category);
			OffsetNumber parent_key_attnum = gintuple_get_attrnum(&state, stack->parenttup);
			ItemId		iid = PageGetItemIdCareful(rel, stack->blkno,
												   page, maxoff);
			IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);
			OffsetNumber page_max_key_attnum = gintuple_get_attrnum(&state, idxtuple);
			GinNullCategory page_max_key_category;
			Datum		page_max_key = gintuple_get_key(&state, idxtuple, &page_max_key_category);

			if (rightlink != InvalidBlockNumber &&
				ginCompareAttEntries(&state, page_max_key_attnum, page_max_key,
									 page_max_key_category, parent_key_attnum,
									 parent_key, parent_key_category) < 0)
			{
				/* split page detected, install right link to the stack */
				GinScanItem *ptr;

				elog(DEBUG3, "split detected for blk: %u, parent blk: %u", stack->blkno, stack->parentblk);

				ptr = (GinScanItem *) palloc(sizeof(GinScanItem));
				ptr->depth = stack->depth;
				ptr->parenttup = CopyIndexTuple(stack->parenttup);
				ptr->parentblk = stack->parentblk;
				ptr->blkno = rightlink;
				ptr->next = stack->next;
				stack->next = ptr;
			}
		}

		/* Check that the tree has the same height in all branches */
		if (GinPageIsLeaf(page))
		{
			if (leafdepth == -1)
				leafdepth = stack->depth;
			else if (stack->depth != leafdepth)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\": internal pages traversal encountered leaf page unexpectedly on block %u",
								RelationGetRelationName(rel), stack->blkno)));
		}

		/*
		 * Check that tuples in each page are properly ordered and consistent
		 * with parent high key
		 */
		prev_tuple = NULL;
		prev_attnum = InvalidAttrNumber;
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			ItemId		iid = PageGetItemIdCareful(rel, stack->blkno, page, i);
			IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);
			OffsetNumber current_attnum = gintuple_get_attrnum(&state, idxtuple);
			GinNullCategory current_key_category;
			Datum		current_key;

			if (MAXALIGN(ItemIdGetLength(iid)) != MAXALIGN(IndexTupleSize(idxtuple)))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\" has inconsistent tuple sizes, block %u, offset %u",
								RelationGetRelationName(rel), stack->blkno, i)));

			current_key = gintuple_get_key(&state, idxtuple, &current_key_category);

			/*
			 * Compare the entry to the preceding one.
			 *
			 * Don't check for high key on the rightmost inner page, as this
			 * key is not really stored explicitly.
			 *
			 * The entries may be for different attributes, so make sure to
			 * use ginCompareAttEntries for comparison.
			 */
			if ((i != FirstOffsetNumber) &&
				!(i == maxoff && rightlink == InvalidBlockNumber && !GinPageIsLeaf(page)))
			{
				Datum		prev_key;
				GinNullCategory prev_key_category;

				prev_key = gintuple_get_key(&state, prev_tuple, &prev_key_category);
				if (ginCompareAttEntries(&state, prev_attnum, prev_key,
										 prev_key_category, current_attnum,
										 current_key, current_key_category) >= 0)
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("index \"%s\" has wrong tuple order on entry tree page, block %u, offset %u, rightlink %u",
									RelationGetRelationName(rel), stack->blkno, i, rightlink)));
			}

			/*
			 * Check if this tuple is consistent with the downlink in the
			 * parent.
			 */
			if (stack->parenttup &&
				i == maxoff)
			{
				GinNullCategory parent_key_category;
				OffsetNumber parent_key_attnum = gintuple_get_attrnum(&state, stack->parenttup);
				Datum		parent_key = gintuple_get_key(&state,
														  stack->parenttup,
														  &parent_key_category);

				if (ginCompareAttEntries(&state, current_attnum, current_key,
										 current_key_category, parent_key_attnum,
										 parent_key, parent_key_category) > 0)
				{
					/*
					 * There was a discrepancy between parent and child
					 * tuples. We need to verify it is not a result of
					 * concurrent call of gistplacetopage(). So, lock parent
					 * and try to find downlink for current page. It may be
					 * missing due to concurrent page split, this is OK.
					 */
					pfree(stack->parenttup);
					stack->parenttup = gin_refind_parent(rel, stack->parentblk,
														 stack->blkno, strategy);

					/* We found it - make a final check before failing */
					if (!stack->parenttup)
						elog(NOTICE, "Unable to find parent tuple for block %u on block %u due to concurrent split",
							 stack->blkno, stack->parentblk);
					else
					{
						parent_key_attnum = gintuple_get_attrnum(&state, stack->parenttup);
						parent_key = gintuple_get_key(&state,
													  stack->parenttup,
													  &parent_key_category);

						/*
						 * Check if it is properly adjusted. If succeed,
						 * proceed to the next key.
						 */
						if (ginCompareAttEntries(&state, current_attnum, current_key,
												 current_key_category, parent_key_attnum,
												 parent_key, parent_key_category) > 0)
							ereport(ERROR,
									(errcode(ERRCODE_INDEX_CORRUPTED),
									 errmsg("index \"%s\" has inconsistent records on page %u offset %u",
											RelationGetRelationName(rel), stack->blkno, i)));
					}
				}
			}

			/* If this is an internal page, recurse into the child */
			if (!GinPageIsLeaf(page))
			{
				GinScanItem *ptr;

				ptr = (GinScanItem *) palloc(sizeof(GinScanItem));
				ptr->depth = stack->depth + 1;
				/* last tuple in layer has no high key */
				if (i == maxoff && rightlink == InvalidBlockNumber)
					ptr->parenttup = NULL;
				else
					ptr->parenttup = CopyIndexTuple(idxtuple);
				ptr->parentblk = stack->blkno;
				ptr->blkno = GinGetDownlink(idxtuple);
				ptr->next = stack->next;
				stack->next = ptr;
			}
			/* If this item is a pointer to a posting tree, recurse into it */
			else if (GinIsPostingTree(idxtuple))
			{
				BlockNumber rootPostingTree = GinGetPostingTree(idxtuple);

				gin_check_posting_tree_parent_keys_consistency(rel, rootPostingTree);
			}
			else
			{
				ItemPointer ipd;
				int			nipd;

				ipd = ginReadTupleWithoutState(idxtuple, &nipd);

				for (int j = 0; j < nipd; j++)
				{
					if (!OffsetNumberIsValid(ItemPointerGetOffsetNumber(&ipd[j])))
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("index \"%s\": posting list contains invalid heap pointer on block %u",
										RelationGetRelationName(rel), stack->blkno)));
				}
				pfree(ipd);
			}

			prev_tuple = CopyIndexTuple(idxtuple);
			prev_attnum = current_attnum;
		}

		LockBuffer(buffer, GIN_UNLOCK);
		ReleaseBuffer(buffer);

		/* Step to next item in the queue */
		stack_next = stack->next;
		if (stack->parenttup)
			pfree(stack->parenttup);
		pfree(stack);
		stack = stack_next;
	}

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(mctx);
}

/*
 * Verify that a freshly-read page looks sane.
 */
static void
check_index_page(Relation rel, Buffer buffer, BlockNumber blockNo)
{
	Page		page = BufferGetPage(buffer);

	/*
	 * ReadBuffer verifies that every newly-read page passes
	 * PageHeaderIsValid, which means it either contains a reasonably sane
	 * page header or is all-zero.  We have to defend against the all-zero
	 * case, however.
	 */
	if (PageIsNew(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains unexpected zero page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buffer)),
				 errhint("Please REINDEX it.")));

	/*
	 * Additionally check that the special area looks sane.
	 */
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GinPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buffer)),
				 errhint("Please REINDEX it.")));

	if (GinPageIsDeleted(page))
	{
		if (!GinPageIsLeaf(page))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has deleted internal page %u",
							RelationGetRelationName(rel), blockNo)));
		if (PageGetMaxOffsetNumber(page) > InvalidOffsetNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has deleted page %u with tuples",
							RelationGetRelationName(rel), blockNo)));
	}
	else if (PageGetMaxOffsetNumber(page) > MaxIndexTuplesPerPage)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has page %u with exceeding count of tuples",
						RelationGetRelationName(rel), blockNo)));
}

/*
 * Try to re-find downlink pointing to 'blkno', in 'parentblkno'.
 *
 * If found, returns a palloc'd copy of the downlink tuple. Otherwise,
 * returns NULL.
 */
static IndexTuple
gin_refind_parent(Relation rel, BlockNumber parentblkno,
				  BlockNumber childblkno, BufferAccessStrategy strategy)
{
	Buffer		parentbuf;
	Page		parentpage;
	OffsetNumber o,
				parent_maxoff;
	IndexTuple	result = NULL;

	parentbuf = ReadBufferExtended(rel, MAIN_FORKNUM, parentblkno, RBM_NORMAL,
								   strategy);

	LockBuffer(parentbuf, GIN_SHARE);
	parentpage = BufferGetPage(parentbuf);

	if (GinPageIsLeaf(parentpage))
	{
		UnlockReleaseBuffer(parentbuf);
		return result;
	}

	parent_maxoff = PageGetMaxOffsetNumber(parentpage);
	for (o = FirstOffsetNumber; o <= parent_maxoff; o = OffsetNumberNext(o))
	{
		ItemId		p_iid = PageGetItemIdCareful(rel, parentblkno, parentpage, o);
		IndexTuple	itup = (IndexTuple) PageGetItem(parentpage, p_iid);

		if (GinGetDownlink(itup) == childblkno)
		{
			/* Found it! Make copy and return it */
			result = CopyIndexTuple(itup);
			break;
		}
	}

	UnlockReleaseBuffer(parentbuf);

	return result;
}

static ItemId
PageGetItemIdCareful(Relation rel, BlockNumber block, Page page,
					 OffsetNumber offset)
{
	ItemId		itemid = PageGetItemId(page, offset);

	if (ItemIdGetOffset(itemid) + ItemIdGetLength(itemid) >
		BLCKSZ - MAXALIGN(sizeof(GinPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("line pointer points past end of tuple space in index \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_internal("Index tid=(%u,%u) lp_off=%u, lp_len=%u lp_flags=%u.",
									block, offset, ItemIdGetOffset(itemid),
									ItemIdGetLength(itemid),
									ItemIdGetFlags(itemid))));

	/*
	 * Verify that line pointer isn't LP_REDIRECT or LP_UNUSED or LP_DEAD,
	 * since GIN never uses all three.  Verify that line pointer has storage,
	 * too.
	 */
	if (ItemIdIsRedirected(itemid) || !ItemIdIsUsed(itemid) ||
		ItemIdIsDead(itemid) || ItemIdGetLength(itemid) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("invalid line pointer storage in index \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_internal("Index tid=(%u,%u) lp_off=%u, lp_len=%u lp_flags=%u.",
									block, offset, ItemIdGetOffset(itemid),
									ItemIdGetLength(itemid),
									ItemIdGetFlags(itemid))));

	return itemid;
}
