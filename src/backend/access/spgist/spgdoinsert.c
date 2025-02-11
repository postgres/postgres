/*-------------------------------------------------------------------------
 *
 * spgdoinsert.c
 *	  implementation of insert algorithm
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgdoinsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/spgist_private.h"
#include "access/spgxlog.h"
#include "access/xloginsert.h"
#include "common/int.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"


/*
 * SPPageDesc tracks all info about a page we are inserting into.  In some
 * situations it actually identifies a tuple, or even a specific node within
 * an inner tuple.  But any of the fields can be invalid.  If the buffer
 * field is valid, it implies we hold pin and exclusive lock on that buffer.
 * page pointer should be valid exactly when buffer is.
 */
typedef struct SPPageDesc
{
	BlockNumber blkno;			/* block number, or InvalidBlockNumber */
	Buffer		buffer;			/* page's buffer number, or InvalidBuffer */
	Page		page;			/* pointer to page buffer, or NULL */
	OffsetNumber offnum;		/* offset of tuple, or InvalidOffsetNumber */
	int			node;			/* node number within inner tuple, or -1 */
} SPPageDesc;


/*
 * Set the item pointer in the nodeN'th entry in inner tuple tup.  This
 * is used to update the parent inner tuple's downlink after a move or
 * split operation.
 */
void
spgUpdateNodeLink(SpGistInnerTuple tup, int nodeN,
				  BlockNumber blkno, OffsetNumber offset)
{
	int			i;
	SpGistNodeTuple node;

	SGITITERATE(tup, i, node)
	{
		if (i == nodeN)
		{
			ItemPointerSet(&node->t_tid, blkno, offset);
			return;
		}
	}

	elog(ERROR, "failed to find requested node %d in SPGiST inner tuple",
		 nodeN);
}

/*
 * Form a new inner tuple containing one more node than the given one, with
 * the specified label datum, inserted at offset "offset" in the node array.
 * The new tuple's prefix is the same as the old one's.
 *
 * Note that the new node initially has an invalid downlink.  We'll find a
 * page to point it to later.
 */
static SpGistInnerTuple
addNode(SpGistState *state, SpGistInnerTuple tuple, Datum label, int offset)
{
	SpGistNodeTuple node,
			   *nodes;
	int			i;

	/* if offset is negative, insert at end */
	if (offset < 0)
		offset = tuple->nNodes;
	else if (offset > tuple->nNodes)
		elog(ERROR, "invalid offset for adding node to SPGiST inner tuple");

	nodes = palloc(sizeof(SpGistNodeTuple) * (tuple->nNodes + 1));
	SGITITERATE(tuple, i, node)
	{
		if (i < offset)
			nodes[i] = node;
		else
			nodes[i + 1] = node;
	}

	nodes[offset] = spgFormNodeTuple(state, label, false);

	return spgFormInnerTuple(state,
							 (tuple->prefixSize > 0),
							 SGITDATUM(tuple, state),
							 tuple->nNodes + 1,
							 nodes);
}

/* qsort comparator for sorting OffsetNumbers */
static int
cmpOffsetNumbers(const void *a, const void *b)
{
	return pg_cmp_u16(*(const OffsetNumber *) a, *(const OffsetNumber *) b);
}

/*
 * Delete multiple tuples from an index page, preserving tuple offset numbers.
 *
 * The first tuple in the given list is replaced with a dead tuple of type
 * "firststate" (REDIRECT/DEAD/PLACEHOLDER); the remaining tuples are replaced
 * with dead tuples of type "reststate".  If either firststate or reststate
 * is REDIRECT, blkno/offnum specify where to link to.
 *
 * NB: this is used during WAL replay, so beware of trying to make it too
 * smart.  In particular, it shouldn't use "state" except for calling
 * spgFormDeadTuple().  This is also used in a critical section, so no
 * pallocs either!
 */
void
spgPageIndexMultiDelete(SpGistState *state, Page page,
						OffsetNumber *itemnos, int nitems,
						int firststate, int reststate,
						BlockNumber blkno, OffsetNumber offnum)
{
	OffsetNumber firstItem;
	OffsetNumber sortednos[MaxIndexTuplesPerPage];
	SpGistDeadTuple tuple = NULL;
	int			i;

	if (nitems == 0)
		return;					/* nothing to do */

	/*
	 * For efficiency we want to use PageIndexMultiDelete, which requires the
	 * targets to be listed in sorted order, so we have to sort the itemnos
	 * array.  (This also greatly simplifies the math for reinserting the
	 * replacement tuples.)  However, we must not scribble on the caller's
	 * array, so we have to make a copy.
	 */
	memcpy(sortednos, itemnos, sizeof(OffsetNumber) * nitems);
	if (nitems > 1)
		qsort(sortednos, nitems, sizeof(OffsetNumber), cmpOffsetNumbers);

	PageIndexMultiDelete(page, sortednos, nitems);

	firstItem = itemnos[0];

	for (i = 0; i < nitems; i++)
	{
		OffsetNumber itemno = sortednos[i];
		int			tupstate;

		tupstate = (itemno == firstItem) ? firststate : reststate;
		if (tuple == NULL || tuple->tupstate != tupstate)
			tuple = spgFormDeadTuple(state, tupstate, blkno, offnum);

		if (PageAddItem(page, (Item) tuple, tuple->size,
						itemno, false, false) != itemno)
			elog(ERROR, "failed to add item of size %u to SPGiST index page",
				 tuple->size);

		if (tupstate == SPGIST_REDIRECT)
			SpGistPageGetOpaque(page)->nRedirection++;
		else if (tupstate == SPGIST_PLACEHOLDER)
			SpGistPageGetOpaque(page)->nPlaceholder++;
	}
}

/*
 * Update the parent inner tuple's downlink, and mark the parent buffer
 * dirty (this must be the last change to the parent page in the current
 * WAL action).
 */
static void
saveNodeLink(Relation index, SPPageDesc *parent,
			 BlockNumber blkno, OffsetNumber offnum)
{
	SpGistInnerTuple innerTuple;

	innerTuple = (SpGistInnerTuple) PageGetItem(parent->page,
												PageGetItemId(parent->page, parent->offnum));

	spgUpdateNodeLink(innerTuple, parent->node, blkno, offnum);

	MarkBufferDirty(parent->buffer);
}

/*
 * Add a leaf tuple to a leaf page where there is known to be room for it
 */
static void
addLeafTuple(Relation index, SpGistState *state, SpGistLeafTuple leafTuple,
			 SPPageDesc *current, SPPageDesc *parent, bool isNulls, bool isNew)
{
	spgxlogAddLeaf xlrec;

	xlrec.newPage = isNew;
	xlrec.storesNulls = isNulls;

	/* these will be filled below as needed */
	xlrec.offnumLeaf = InvalidOffsetNumber;
	xlrec.offnumHeadLeaf = InvalidOffsetNumber;
	xlrec.offnumParent = InvalidOffsetNumber;
	xlrec.nodeI = 0;

	START_CRIT_SECTION();

	if (current->offnum == InvalidOffsetNumber ||
		SpGistBlockIsRoot(current->blkno))
	{
		/* Tuple is not part of a chain */
		SGLT_SET_NEXTOFFSET(leafTuple, InvalidOffsetNumber);
		current->offnum = SpGistPageAddNewItem(state, current->page,
											   (Item) leafTuple, leafTuple->size,
											   NULL, false);

		xlrec.offnumLeaf = current->offnum;

		/* Must update parent's downlink if any */
		if (parent->buffer != InvalidBuffer)
		{
			xlrec.offnumParent = parent->offnum;
			xlrec.nodeI = parent->node;

			saveNodeLink(index, parent, current->blkno, current->offnum);
		}
	}
	else
	{
		/*
		 * Tuple must be inserted into existing chain.  We mustn't change the
		 * chain's head address, but we don't need to chase the entire chain
		 * to put the tuple at the end; we can insert it second.
		 *
		 * Also, it's possible that the "chain" consists only of a DEAD tuple,
		 * in which case we should replace the DEAD tuple in-place.
		 */
		SpGistLeafTuple head;
		OffsetNumber offnum;

		head = (SpGistLeafTuple) PageGetItem(current->page,
											 PageGetItemId(current->page, current->offnum));
		if (head->tupstate == SPGIST_LIVE)
		{
			SGLT_SET_NEXTOFFSET(leafTuple, SGLT_GET_NEXTOFFSET(head));
			offnum = SpGistPageAddNewItem(state, current->page,
										  (Item) leafTuple, leafTuple->size,
										  NULL, false);

			/*
			 * re-get head of list because it could have been moved on page,
			 * and set new second element
			 */
			head = (SpGistLeafTuple) PageGetItem(current->page,
												 PageGetItemId(current->page, current->offnum));
			SGLT_SET_NEXTOFFSET(head, offnum);

			xlrec.offnumLeaf = offnum;
			xlrec.offnumHeadLeaf = current->offnum;
		}
		else if (head->tupstate == SPGIST_DEAD)
		{
			SGLT_SET_NEXTOFFSET(leafTuple, InvalidOffsetNumber);
			PageIndexTupleDelete(current->page, current->offnum);
			if (PageAddItem(current->page,
							(Item) leafTuple, leafTuple->size,
							current->offnum, false, false) != current->offnum)
				elog(ERROR, "failed to add item of size %u to SPGiST index page",
					 leafTuple->size);

			/* WAL replay distinguishes this case by equal offnums */
			xlrec.offnumLeaf = current->offnum;
			xlrec.offnumHeadLeaf = current->offnum;
		}
		else
			elog(ERROR, "unexpected SPGiST tuple state: %d", head->tupstate);
	}

	MarkBufferDirty(current->buffer);

	if (RelationNeedsWAL(index) && !state->isBuild)
	{
		XLogRecPtr	recptr;
		int			flags;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));
		XLogRegisterData(leafTuple, leafTuple->size);

		flags = REGBUF_STANDARD;
		if (xlrec.newPage)
			flags |= REGBUF_WILL_INIT;
		XLogRegisterBuffer(0, current->buffer, flags);
		if (xlrec.offnumParent != InvalidOffsetNumber)
			XLogRegisterBuffer(1, parent->buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_ADD_LEAF);

		PageSetLSN(current->page, recptr);

		/* update parent only if we actually changed it */
		if (xlrec.offnumParent != InvalidOffsetNumber)
		{
			PageSetLSN(parent->page, recptr);
		}
	}

	END_CRIT_SECTION();
}

/*
 * Count the number and total size of leaf tuples in the chain starting at
 * current->offnum.  Return number into *nToSplit and total size as function
 * result.
 *
 * Klugy special case when considering the root page (i.e., root is a leaf
 * page, but we're about to split for the first time): return fake large
 * values to force spgdoinsert() to take the doPickSplit rather than
 * moveLeafs code path.  moveLeafs is not prepared to deal with root page.
 */
static int
checkSplitConditions(Relation index, SpGistState *state,
					 SPPageDesc *current, int *nToSplit)
{
	int			i,
				n = 0,
				totalSize = 0;

	if (SpGistBlockIsRoot(current->blkno))
	{
		/* return impossible values to force split */
		*nToSplit = BLCKSZ;
		return BLCKSZ;
	}

	i = current->offnum;
	while (i != InvalidOffsetNumber)
	{
		SpGistLeafTuple it;

		Assert(i >= FirstOffsetNumber &&
			   i <= PageGetMaxOffsetNumber(current->page));
		it = (SpGistLeafTuple) PageGetItem(current->page,
										   PageGetItemId(current->page, i));
		if (it->tupstate == SPGIST_LIVE)
		{
			n++;
			totalSize += it->size + sizeof(ItemIdData);
		}
		else if (it->tupstate == SPGIST_DEAD)
		{
			/* We could see a DEAD tuple as first/only chain item */
			Assert(i == current->offnum);
			Assert(SGLT_GET_NEXTOFFSET(it) == InvalidOffsetNumber);
			/* Don't count it in result, because it won't go to other page */
		}
		else
			elog(ERROR, "unexpected SPGiST tuple state: %d", it->tupstate);

		i = SGLT_GET_NEXTOFFSET(it);
	}

	*nToSplit = n;

	return totalSize;
}

/*
 * current points to a leaf-tuple chain that we wanted to add newLeafTuple to,
 * but the chain has to be moved because there's not enough room to add
 * newLeafTuple to its page.  We use this method when the chain contains
 * very little data so a split would be inefficient.  We are sure we can
 * fit the chain plus newLeafTuple on one other page.
 */
static void
moveLeafs(Relation index, SpGistState *state,
		  SPPageDesc *current, SPPageDesc *parent,
		  SpGistLeafTuple newLeafTuple, bool isNulls)
{
	int			i,
				nDelete,
				nInsert,
				size;
	Buffer		nbuf;
	Page		npage;
	OffsetNumber r = InvalidOffsetNumber,
				startOffset = InvalidOffsetNumber;
	bool		replaceDead = false;
	OffsetNumber *toDelete;
	OffsetNumber *toInsert;
	BlockNumber nblkno;
	spgxlogMoveLeafs xlrec;
	char	   *leafdata,
			   *leafptr;

	/* This doesn't work on root page */
	Assert(parent->buffer != InvalidBuffer);
	Assert(parent->buffer != current->buffer);

	/* Locate the tuples to be moved, and count up the space needed */
	i = PageGetMaxOffsetNumber(current->page);
	toDelete = (OffsetNumber *) palloc(sizeof(OffsetNumber) * i);
	toInsert = (OffsetNumber *) palloc(sizeof(OffsetNumber) * (i + 1));

	size = newLeafTuple->size + sizeof(ItemIdData);

	nDelete = 0;
	i = current->offnum;
	while (i != InvalidOffsetNumber)
	{
		SpGistLeafTuple it;

		Assert(i >= FirstOffsetNumber &&
			   i <= PageGetMaxOffsetNumber(current->page));
		it = (SpGistLeafTuple) PageGetItem(current->page,
										   PageGetItemId(current->page, i));

		if (it->tupstate == SPGIST_LIVE)
		{
			toDelete[nDelete] = i;
			size += it->size + sizeof(ItemIdData);
			nDelete++;
		}
		else if (it->tupstate == SPGIST_DEAD)
		{
			/* We could see a DEAD tuple as first/only chain item */
			Assert(i == current->offnum);
			Assert(SGLT_GET_NEXTOFFSET(it) == InvalidOffsetNumber);
			/* We don't want to move it, so don't count it in size */
			toDelete[nDelete] = i;
			nDelete++;
			replaceDead = true;
		}
		else
			elog(ERROR, "unexpected SPGiST tuple state: %d", it->tupstate);

		i = SGLT_GET_NEXTOFFSET(it);
	}

	/* Find a leaf page that will hold them */
	nbuf = SpGistGetBuffer(index, GBUF_LEAF | (isNulls ? GBUF_NULLS : 0),
						   size, &xlrec.newPage);
	npage = BufferGetPage(nbuf);
	nblkno = BufferGetBlockNumber(nbuf);
	Assert(nblkno != current->blkno);

	leafdata = leafptr = palloc(size);

	START_CRIT_SECTION();

	/* copy all the old tuples to new page, unless they're dead */
	nInsert = 0;
	if (!replaceDead)
	{
		for (i = 0; i < nDelete; i++)
		{
			SpGistLeafTuple it;

			it = (SpGistLeafTuple) PageGetItem(current->page,
											   PageGetItemId(current->page, toDelete[i]));
			Assert(it->tupstate == SPGIST_LIVE);

			/*
			 * Update chain link (notice the chain order gets reversed, but we
			 * don't care).  We're modifying the tuple on the source page
			 * here, but it's okay since we're about to delete it.
			 */
			SGLT_SET_NEXTOFFSET(it, r);

			r = SpGistPageAddNewItem(state, npage, (Item) it, it->size,
									 &startOffset, false);

			toInsert[nInsert] = r;
			nInsert++;

			/* save modified tuple into leafdata as well */
			memcpy(leafptr, it, it->size);
			leafptr += it->size;
		}
	}

	/* add the new tuple as well */
	SGLT_SET_NEXTOFFSET(newLeafTuple, r);
	r = SpGistPageAddNewItem(state, npage,
							 (Item) newLeafTuple, newLeafTuple->size,
							 &startOffset, false);
	toInsert[nInsert] = r;
	nInsert++;
	memcpy(leafptr, newLeafTuple, newLeafTuple->size);
	leafptr += newLeafTuple->size;

	/*
	 * Now delete the old tuples, leaving a redirection pointer behind for the
	 * first one, unless we're doing an index build; in which case there can't
	 * be any concurrent scan so we need not provide a redirect.
	 */
	spgPageIndexMultiDelete(state, current->page, toDelete, nDelete,
							state->isBuild ? SPGIST_PLACEHOLDER : SPGIST_REDIRECT,
							SPGIST_PLACEHOLDER,
							nblkno, r);

	/* Update parent's downlink and mark parent page dirty */
	saveNodeLink(index, parent, nblkno, r);

	/* Mark the leaf pages too */
	MarkBufferDirty(current->buffer);
	MarkBufferDirty(nbuf);

	if (RelationNeedsWAL(index) && !state->isBuild)
	{
		XLogRecPtr	recptr;

		/* prepare WAL info */
		STORE_STATE(state, xlrec.stateSrc);

		xlrec.nMoves = nDelete;
		xlrec.replaceDead = replaceDead;
		xlrec.storesNulls = isNulls;

		xlrec.offnumParent = parent->offnum;
		xlrec.nodeI = parent->node;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfSpgxlogMoveLeafs);
		XLogRegisterData(toDelete,
						 sizeof(OffsetNumber) * nDelete);
		XLogRegisterData(toInsert,
						 sizeof(OffsetNumber) * nInsert);
		XLogRegisterData(leafdata, leafptr - leafdata);

		XLogRegisterBuffer(0, current->buffer, REGBUF_STANDARD);
		XLogRegisterBuffer(1, nbuf, REGBUF_STANDARD | (xlrec.newPage ? REGBUF_WILL_INIT : 0));
		XLogRegisterBuffer(2, parent->buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_MOVE_LEAFS);

		PageSetLSN(current->page, recptr);
		PageSetLSN(npage, recptr);
		PageSetLSN(parent->page, recptr);
	}

	END_CRIT_SECTION();

	/* Update local free-space cache and release new buffer */
	SpGistSetLastUsedPage(index, nbuf);
	UnlockReleaseBuffer(nbuf);
}

/*
 * Update previously-created redirection tuple with appropriate destination
 *
 * We use this when it's not convenient to know the destination first.
 * The tuple should have been made with the "impossible" destination of
 * the metapage.
 */
static void
setRedirectionTuple(SPPageDesc *current, OffsetNumber position,
					BlockNumber blkno, OffsetNumber offnum)
{
	SpGistDeadTuple dt;

	dt = (SpGistDeadTuple) PageGetItem(current->page,
									   PageGetItemId(current->page, position));
	Assert(dt->tupstate == SPGIST_REDIRECT);
	Assert(ItemPointerGetBlockNumber(&dt->pointer) == SPGIST_METAPAGE_BLKNO);
	ItemPointerSet(&dt->pointer, blkno, offnum);
}

/*
 * Test to see if the user-defined picksplit function failed to do its job,
 * ie, it put all the leaf tuples into the same node.
 * If so, randomly divide the tuples into several nodes (all with the same
 * label) and return true to select allTheSame mode for this inner tuple.
 *
 * (This code is also used to forcibly select allTheSame mode for nulls.)
 *
 * If we know that the leaf tuples wouldn't all fit on one page, then we
 * exclude the last tuple (which is the incoming new tuple that forced a split)
 * from the check to see if more than one node is used.  The reason for this
 * is that if the existing tuples are put into only one chain, then even if
 * we move them all to an empty page, there would still not be room for the
 * new tuple, so we'd get into an infinite loop of picksplit attempts.
 * Forcing allTheSame mode dodges this problem by ensuring the old tuples will
 * be split across pages.  (Exercise for the reader: figure out why this
 * fixes the problem even when there is only one old tuple.)
 */
static bool
checkAllTheSame(spgPickSplitIn *in, spgPickSplitOut *out, bool tooBig,
				bool *includeNew)
{
	int			theNode;
	int			limit;
	int			i;

	/* For the moment, assume we can include the new leaf tuple */
	*includeNew = true;

	/* If there's only the new leaf tuple, don't select allTheSame mode */
	if (in->nTuples <= 1)
		return false;

	/* If tuple set doesn't fit on one page, ignore the new tuple in test */
	limit = tooBig ? in->nTuples - 1 : in->nTuples;

	/* Check to see if more than one node is populated */
	theNode = out->mapTuplesToNodes[0];
	for (i = 1; i < limit; i++)
	{
		if (out->mapTuplesToNodes[i] != theNode)
			return false;
	}

	/* Nope, so override the picksplit function's decisions */

	/* If the new tuple is in its own node, it can't be included in split */
	if (tooBig && out->mapTuplesToNodes[in->nTuples - 1] != theNode)
		*includeNew = false;

	out->nNodes = 8;			/* arbitrary number of child nodes */

	/* Random assignment of tuples to nodes (note we include new tuple) */
	for (i = 0; i < in->nTuples; i++)
		out->mapTuplesToNodes[i] = i % out->nNodes;

	/* The opclass may not use node labels, but if it does, duplicate 'em */
	if (out->nodeLabels)
	{
		Datum		theLabel = out->nodeLabels[theNode];

		out->nodeLabels = (Datum *) palloc(sizeof(Datum) * out->nNodes);
		for (i = 0; i < out->nNodes; i++)
			out->nodeLabels[i] = theLabel;
	}

	/* We don't touch the prefix or the leaf tuple datum assignments */

	return true;
}

/*
 * current points to a leaf-tuple chain that we wanted to add newLeafTuple to,
 * but the chain has to be split because there's not enough room to add
 * newLeafTuple to its page.
 *
 * This function splits the leaf tuple set according to picksplit's rules,
 * creating one or more new chains that are spread across the current page
 * and an additional leaf page (we assume that two leaf pages will be
 * sufficient).  A new inner tuple is created, and the parent downlink
 * pointer is updated to point to that inner tuple instead of the leaf chain.
 *
 * On exit, current contains the address of the new inner tuple.
 *
 * Returns true if we successfully inserted newLeafTuple during this function,
 * false if caller still has to do it (meaning another picksplit operation is
 * probably needed).  Failure could occur if the picksplit result is fairly
 * unbalanced, or if newLeafTuple is just plain too big to fit on a page.
 * Because we force the picksplit result to be at least two chains, each
 * cycle will get rid of at least one leaf tuple from the chain, so the loop
 * will eventually terminate if lack of balance is the issue.  If the tuple
 * is too big, we assume that repeated picksplit operations will eventually
 * make it small enough by repeated prefix-stripping.  A broken opclass could
 * make this an infinite loop, though, so spgdoinsert() checks that the
 * leaf datums get smaller each time.
 */
static bool
doPickSplit(Relation index, SpGistState *state,
			SPPageDesc *current, SPPageDesc *parent,
			SpGistLeafTuple newLeafTuple,
			int level, bool isNulls, bool isNew)
{
	bool		insertedNew = false;
	spgPickSplitIn in;
	spgPickSplitOut out;
	FmgrInfo   *procinfo;
	bool		includeNew;
	int			i,
				max,
				n;
	SpGistInnerTuple innerTuple;
	SpGistNodeTuple node,
			   *nodes;
	Buffer		newInnerBuffer,
				newLeafBuffer;
	uint8	   *leafPageSelect;
	int		   *leafSizes;
	OffsetNumber *toDelete;
	OffsetNumber *toInsert;
	OffsetNumber redirectTuplePos = InvalidOffsetNumber;
	OffsetNumber startOffsets[2];
	SpGistLeafTuple *oldLeafs;
	SpGistLeafTuple *newLeafs;
	Datum		leafDatums[INDEX_MAX_KEYS];
	bool		leafIsnulls[INDEX_MAX_KEYS];
	int			spaceToDelete;
	int			currentFreeSpace;
	int			totalLeafSizes;
	bool		allTheSame;
	spgxlogPickSplit xlrec;
	char	   *leafdata,
			   *leafptr;
	SPPageDesc	saveCurrent;
	int			nToDelete,
				nToInsert,
				maxToInclude;

	in.level = level;

	/*
	 * Allocate per-leaf-tuple work arrays with max possible size
	 */
	max = PageGetMaxOffsetNumber(current->page);
	n = max + 1;
	in.datums = (Datum *) palloc(sizeof(Datum) * n);
	toDelete = (OffsetNumber *) palloc(sizeof(OffsetNumber) * n);
	toInsert = (OffsetNumber *) palloc(sizeof(OffsetNumber) * n);
	oldLeafs = (SpGistLeafTuple *) palloc(sizeof(SpGistLeafTuple) * n);
	newLeafs = (SpGistLeafTuple *) palloc(sizeof(SpGistLeafTuple) * n);
	leafPageSelect = (uint8 *) palloc(sizeof(uint8) * n);

	STORE_STATE(state, xlrec.stateSrc);

	/*
	 * Form list of leaf tuples which will be distributed as split result;
	 * also, count up the amount of space that will be freed from current.
	 * (Note that in the non-root case, we won't actually delete the old
	 * tuples, only replace them with redirects or placeholders.)
	 */
	nToInsert = 0;
	nToDelete = 0;
	spaceToDelete = 0;
	if (SpGistBlockIsRoot(current->blkno))
	{
		/*
		 * We are splitting the root (which up to now is also a leaf page).
		 * Its tuples are not linked, so scan sequentially to get them all. We
		 * ignore the original value of current->offnum.
		 */
		for (i = FirstOffsetNumber; i <= max; i++)
		{
			SpGistLeafTuple it;

			it = (SpGistLeafTuple) PageGetItem(current->page,
											   PageGetItemId(current->page, i));
			if (it->tupstate == SPGIST_LIVE)
			{
				in.datums[nToInsert] =
					isNulls ? (Datum) 0 : SGLTDATUM(it, state);
				oldLeafs[nToInsert] = it;
				nToInsert++;
				toDelete[nToDelete] = i;
				nToDelete++;
				/* we will delete the tuple altogether, so count full space */
				spaceToDelete += it->size + sizeof(ItemIdData);
			}
			else				/* tuples on root should be live */
				elog(ERROR, "unexpected SPGiST tuple state: %d", it->tupstate);
		}
	}
	else
	{
		/* Normal case, just collect the leaf tuples in the chain */
		i = current->offnum;
		while (i != InvalidOffsetNumber)
		{
			SpGistLeafTuple it;

			Assert(i >= FirstOffsetNumber && i <= max);
			it = (SpGistLeafTuple) PageGetItem(current->page,
											   PageGetItemId(current->page, i));
			if (it->tupstate == SPGIST_LIVE)
			{
				in.datums[nToInsert] =
					isNulls ? (Datum) 0 : SGLTDATUM(it, state);
				oldLeafs[nToInsert] = it;
				nToInsert++;
				toDelete[nToDelete] = i;
				nToDelete++;
				/* we will not delete the tuple, only replace with dead */
				Assert(it->size >= SGDTSIZE);
				spaceToDelete += it->size - SGDTSIZE;
			}
			else if (it->tupstate == SPGIST_DEAD)
			{
				/* We could see a DEAD tuple as first/only chain item */
				Assert(i == current->offnum);
				Assert(SGLT_GET_NEXTOFFSET(it) == InvalidOffsetNumber);
				toDelete[nToDelete] = i;
				nToDelete++;
				/* replacing it with redirect will save no space */
			}
			else
				elog(ERROR, "unexpected SPGiST tuple state: %d", it->tupstate);

			i = SGLT_GET_NEXTOFFSET(it);
		}
	}
	in.nTuples = nToInsert;

	/*
	 * We may not actually insert new tuple because another picksplit may be
	 * necessary due to too large value, but we will try to allocate enough
	 * space to include it; and in any case it has to be included in the input
	 * for the picksplit function.  So don't increment nToInsert yet.
	 */
	in.datums[in.nTuples] =
		isNulls ? (Datum) 0 : SGLTDATUM(newLeafTuple, state);
	oldLeafs[in.nTuples] = newLeafTuple;
	in.nTuples++;

	memset(&out, 0, sizeof(out));

	if (!isNulls)
	{
		/*
		 * Perform split using user-defined method.
		 */
		procinfo = index_getprocinfo(index, 1, SPGIST_PICKSPLIT_PROC);
		FunctionCall2Coll(procinfo,
						  index->rd_indcollation[0],
						  PointerGetDatum(&in),
						  PointerGetDatum(&out));

		/*
		 * Form new leaf tuples and count up the total space needed.
		 */
		totalLeafSizes = 0;
		for (i = 0; i < in.nTuples; i++)
		{
			if (state->leafTupDesc->natts > 1)
				spgDeformLeafTuple(oldLeafs[i],
								   state->leafTupDesc,
								   leafDatums,
								   leafIsnulls,
								   isNulls);

			leafDatums[spgKeyColumn] = out.leafTupleDatums[i];
			leafIsnulls[spgKeyColumn] = false;

			newLeafs[i] = spgFormLeafTuple(state, &oldLeafs[i]->heapPtr,
										   leafDatums,
										   leafIsnulls);
			totalLeafSizes += newLeafs[i]->size + sizeof(ItemIdData);
		}
	}
	else
	{
		/*
		 * Perform dummy split that puts all tuples into one node.
		 * checkAllTheSame will override this and force allTheSame mode.
		 */
		out.hasPrefix = false;
		out.nNodes = 1;
		out.nodeLabels = NULL;
		out.mapTuplesToNodes = palloc0(sizeof(int) * in.nTuples);

		/*
		 * Form new leaf tuples and count up the total space needed.
		 */
		totalLeafSizes = 0;
		for (i = 0; i < in.nTuples; i++)
		{
			if (state->leafTupDesc->natts > 1)
				spgDeformLeafTuple(oldLeafs[i],
								   state->leafTupDesc,
								   leafDatums,
								   leafIsnulls,
								   isNulls);

			/*
			 * Nulls tree can contain only null key values.
			 */
			leafDatums[spgKeyColumn] = (Datum) 0;
			leafIsnulls[spgKeyColumn] = true;

			newLeafs[i] = spgFormLeafTuple(state, &oldLeafs[i]->heapPtr,
										   leafDatums,
										   leafIsnulls);
			totalLeafSizes += newLeafs[i]->size + sizeof(ItemIdData);
		}
	}

	/*
	 * Check to see if the picksplit function failed to separate the values,
	 * ie, it put them all into the same child node.  If so, select allTheSame
	 * mode and create a random split instead.  See comments for
	 * checkAllTheSame as to why we need to know if the new leaf tuples could
	 * fit on one page.
	 */
	allTheSame = checkAllTheSame(&in, &out,
								 totalLeafSizes > SPGIST_PAGE_CAPACITY,
								 &includeNew);

	/*
	 * If checkAllTheSame decided we must exclude the new tuple, don't
	 * consider it any further.
	 */
	if (includeNew)
		maxToInclude = in.nTuples;
	else
	{
		maxToInclude = in.nTuples - 1;
		totalLeafSizes -= newLeafs[in.nTuples - 1]->size + sizeof(ItemIdData);
	}

	/*
	 * Allocate per-node work arrays.  Since checkAllTheSame could replace
	 * out.nNodes with a value larger than the number of tuples on the input
	 * page, we can't allocate these arrays before here.
	 */
	nodes = (SpGistNodeTuple *) palloc(sizeof(SpGistNodeTuple) * out.nNodes);
	leafSizes = (int *) palloc0(sizeof(int) * out.nNodes);

	/*
	 * Form nodes of inner tuple and inner tuple itself
	 */
	for (i = 0; i < out.nNodes; i++)
	{
		Datum		label = (Datum) 0;
		bool		labelisnull = (out.nodeLabels == NULL);

		if (!labelisnull)
			label = out.nodeLabels[i];
		nodes[i] = spgFormNodeTuple(state, label, labelisnull);
	}
	innerTuple = spgFormInnerTuple(state,
								   out.hasPrefix, out.prefixDatum,
								   out.nNodes, nodes);
	innerTuple->allTheSame = allTheSame;

	/*
	 * Update nodes[] array to point into the newly formed innerTuple, so that
	 * we can adjust their downlinks below.
	 */
	SGITITERATE(innerTuple, i, node)
	{
		nodes[i] = node;
	}

	/*
	 * Re-scan new leaf tuples and count up the space needed under each node.
	 */
	for (i = 0; i < maxToInclude; i++)
	{
		n = out.mapTuplesToNodes[i];
		if (n < 0 || n >= out.nNodes)
			elog(ERROR, "inconsistent result of SPGiST picksplit function");
		leafSizes[n] += newLeafs[i]->size + sizeof(ItemIdData);
	}

	/*
	 * To perform the split, we must insert a new inner tuple, which can't go
	 * on a leaf page; and unless we are splitting the root page, we must then
	 * update the parent tuple's downlink to point to the inner tuple.  If
	 * there is room, we'll put the new inner tuple on the same page as the
	 * parent tuple, otherwise we need another non-leaf buffer. But if the
	 * parent page is the root, we can't add the new inner tuple there,
	 * because the root page must have only one inner tuple.
	 */
	xlrec.initInner = false;
	if (parent->buffer != InvalidBuffer &&
		!SpGistBlockIsRoot(parent->blkno) &&
		(SpGistPageGetFreeSpace(parent->page, 1) >=
		 innerTuple->size + sizeof(ItemIdData)))
	{
		/* New inner tuple will fit on parent page */
		newInnerBuffer = parent->buffer;
	}
	else if (parent->buffer != InvalidBuffer)
	{
		/* Send tuple to page with next triple parity (see README) */
		newInnerBuffer = SpGistGetBuffer(index,
										 GBUF_INNER_PARITY(parent->blkno + 1) |
										 (isNulls ? GBUF_NULLS : 0),
										 innerTuple->size + sizeof(ItemIdData),
										 &xlrec.initInner);
	}
	else
	{
		/* Root page split ... inner tuple will go to root page */
		newInnerBuffer = InvalidBuffer;
	}

	/*
	 * The new leaf tuples converted from the existing ones should require the
	 * same or less space, and therefore should all fit onto one page
	 * (although that's not necessarily the current page, since we can't
	 * delete the old tuples but only replace them with placeholders).
	 * However, the incoming new tuple might not also fit, in which case we
	 * might need another picksplit cycle to reduce it some more.
	 *
	 * If there's not room to put everything back onto the current page, then
	 * we decide on a per-node basis which tuples go to the new page. (We do
	 * it like that because leaf tuple chains can't cross pages, so we must
	 * place all leaf tuples belonging to the same parent node on the same
	 * page.)
	 *
	 * If we are splitting the root page (turning it from a leaf page into an
	 * inner page), then no leaf tuples can go back to the current page; they
	 * must all go somewhere else.
	 */
	if (!SpGistBlockIsRoot(current->blkno))
		currentFreeSpace = PageGetExactFreeSpace(current->page) + spaceToDelete;
	else
		currentFreeSpace = 0;	/* prevent assigning any tuples to current */

	xlrec.initDest = false;

	if (totalLeafSizes <= currentFreeSpace)
	{
		/* All the leaf tuples will fit on current page */
		newLeafBuffer = InvalidBuffer;
		/* mark new leaf tuple as included in insertions, if allowed */
		if (includeNew)
		{
			nToInsert++;
			insertedNew = true;
		}
		for (i = 0; i < nToInsert; i++)
			leafPageSelect[i] = 0;	/* signifies current page */
	}
	else if (in.nTuples == 1 && totalLeafSizes > SPGIST_PAGE_CAPACITY)
	{
		/*
		 * We're trying to split up a long value by repeated suffixing, but
		 * it's not going to fit yet.  Don't bother allocating a second leaf
		 * buffer that we won't be able to use.
		 */
		newLeafBuffer = InvalidBuffer;
		Assert(includeNew);
		Assert(nToInsert == 0);
	}
	else
	{
		/* We will need another leaf page */
		uint8	   *nodePageSelect;
		int			curspace;
		int			newspace;

		newLeafBuffer = SpGistGetBuffer(index,
										GBUF_LEAF | (isNulls ? GBUF_NULLS : 0),
										Min(totalLeafSizes,
											SPGIST_PAGE_CAPACITY),
										&xlrec.initDest);

		/*
		 * Attempt to assign node groups to the two pages.  We might fail to
		 * do so, even if totalLeafSizes is less than the available space,
		 * because we can't split a group across pages.
		 */
		nodePageSelect = (uint8 *) palloc(sizeof(uint8) * out.nNodes);

		curspace = currentFreeSpace;
		newspace = PageGetExactFreeSpace(BufferGetPage(newLeafBuffer));
		for (i = 0; i < out.nNodes; i++)
		{
			if (leafSizes[i] <= curspace)
			{
				nodePageSelect[i] = 0;	/* signifies current page */
				curspace -= leafSizes[i];
			}
			else
			{
				nodePageSelect[i] = 1;	/* signifies new leaf page */
				newspace -= leafSizes[i];
			}
		}
		if (curspace >= 0 && newspace >= 0)
		{
			/* Successful assignment, so we can include the new leaf tuple */
			if (includeNew)
			{
				nToInsert++;
				insertedNew = true;
			}
		}
		else if (includeNew)
		{
			/* We must exclude the new leaf tuple from the split */
			int			nodeOfNewTuple = out.mapTuplesToNodes[in.nTuples - 1];

			leafSizes[nodeOfNewTuple] -=
				newLeafs[in.nTuples - 1]->size + sizeof(ItemIdData);

			/* Repeat the node assignment process --- should succeed now */
			curspace = currentFreeSpace;
			newspace = PageGetExactFreeSpace(BufferGetPage(newLeafBuffer));
			for (i = 0; i < out.nNodes; i++)
			{
				if (leafSizes[i] <= curspace)
				{
					nodePageSelect[i] = 0;	/* signifies current page */
					curspace -= leafSizes[i];
				}
				else
				{
					nodePageSelect[i] = 1;	/* signifies new leaf page */
					newspace -= leafSizes[i];
				}
			}
			if (curspace < 0 || newspace < 0)
				elog(ERROR, "failed to divide leaf tuple groups across pages");
		}
		else
		{
			/* oops, we already excluded new tuple ... should not get here */
			elog(ERROR, "failed to divide leaf tuple groups across pages");
		}
		/* Expand the per-node assignments to be shown per leaf tuple */
		for (i = 0; i < nToInsert; i++)
		{
			n = out.mapTuplesToNodes[i];
			leafPageSelect[i] = nodePageSelect[n];
		}
	}

	/* Start preparing WAL record */
	xlrec.nDelete = 0;
	xlrec.initSrc = isNew;
	xlrec.storesNulls = isNulls;
	xlrec.isRootSplit = SpGistBlockIsRoot(current->blkno);

	leafdata = leafptr = (char *) palloc(totalLeafSizes);

	/* Here we begin making the changes to the target pages */
	START_CRIT_SECTION();

	/*
	 * Delete old leaf tuples from current buffer, except when we're splitting
	 * the root; in that case there's no need because we'll re-init the page
	 * below.  We do this first to make room for reinserting new leaf tuples.
	 */
	if (!SpGistBlockIsRoot(current->blkno))
	{
		/*
		 * Init buffer instead of deleting individual tuples, but only if
		 * there aren't any other live tuples and only during build; otherwise
		 * we need to set a redirection tuple for concurrent scans.
		 */
		if (state->isBuild &&
			nToDelete + SpGistPageGetOpaque(current->page)->nPlaceholder ==
			PageGetMaxOffsetNumber(current->page))
		{
			SpGistInitBuffer(current->buffer,
							 SPGIST_LEAF | (isNulls ? SPGIST_NULLS : 0));
			xlrec.initSrc = true;
		}
		else if (isNew)
		{
			/* don't expose the freshly init'd buffer as a backup block */
			Assert(nToDelete == 0);
		}
		else
		{
			xlrec.nDelete = nToDelete;

			if (!state->isBuild)
			{
				/*
				 * Need to create redirect tuple (it will point to new inner
				 * tuple) but right now the new tuple's location is not known
				 * yet.  So, set the redirection pointer to "impossible" value
				 * and remember its position to update tuple later.
				 */
				if (nToDelete > 0)
					redirectTuplePos = toDelete[0];
				spgPageIndexMultiDelete(state, current->page,
										toDelete, nToDelete,
										SPGIST_REDIRECT,
										SPGIST_PLACEHOLDER,
										SPGIST_METAPAGE_BLKNO,
										FirstOffsetNumber);
			}
			else
			{
				/*
				 * During index build there is not concurrent searches, so we
				 * don't need to create redirection tuple.
				 */
				spgPageIndexMultiDelete(state, current->page,
										toDelete, nToDelete,
										SPGIST_PLACEHOLDER,
										SPGIST_PLACEHOLDER,
										InvalidBlockNumber,
										InvalidOffsetNumber);
			}
		}
	}

	/*
	 * Put leaf tuples on proper pages, and update downlinks in innerTuple's
	 * nodes.
	 */
	startOffsets[0] = startOffsets[1] = InvalidOffsetNumber;
	for (i = 0; i < nToInsert; i++)
	{
		SpGistLeafTuple it = newLeafs[i];
		Buffer		leafBuffer;
		BlockNumber leafBlock;
		OffsetNumber newoffset;

		/* Which page is it going to? */
		leafBuffer = leafPageSelect[i] ? newLeafBuffer : current->buffer;
		leafBlock = BufferGetBlockNumber(leafBuffer);

		/* Link tuple into correct chain for its node */
		n = out.mapTuplesToNodes[i];

		if (ItemPointerIsValid(&nodes[n]->t_tid))
		{
			Assert(ItemPointerGetBlockNumber(&nodes[n]->t_tid) == leafBlock);
			SGLT_SET_NEXTOFFSET(it, ItemPointerGetOffsetNumber(&nodes[n]->t_tid));
		}
		else
			SGLT_SET_NEXTOFFSET(it, InvalidOffsetNumber);

		/* Insert it on page */
		newoffset = SpGistPageAddNewItem(state, BufferGetPage(leafBuffer),
										 (Item) it, it->size,
										 &startOffsets[leafPageSelect[i]],
										 false);
		toInsert[i] = newoffset;

		/* ... and complete the chain linking */
		ItemPointerSet(&nodes[n]->t_tid, leafBlock, newoffset);

		/* Also copy leaf tuple into WAL data */
		memcpy(leafptr, newLeafs[i], newLeafs[i]->size);
		leafptr += newLeafs[i]->size;
	}

	/*
	 * We're done modifying the other leaf buffer (if any), so mark it dirty.
	 * current->buffer will be marked below, after we're entirely done
	 * modifying it.
	 */
	if (newLeafBuffer != InvalidBuffer)
	{
		MarkBufferDirty(newLeafBuffer);
	}

	/* Remember current buffer, since we're about to change "current" */
	saveCurrent = *current;

	/*
	 * Store the new innerTuple
	 */
	if (newInnerBuffer == parent->buffer && newInnerBuffer != InvalidBuffer)
	{
		/*
		 * new inner tuple goes to parent page
		 */
		Assert(current->buffer != parent->buffer);

		/* Repoint "current" at the new inner tuple */
		current->blkno = parent->blkno;
		current->buffer = parent->buffer;
		current->page = parent->page;
		xlrec.offnumInner = current->offnum =
			SpGistPageAddNewItem(state, current->page,
								 (Item) innerTuple, innerTuple->size,
								 NULL, false);

		/*
		 * Update parent node link and mark parent page dirty
		 */
		xlrec.innerIsParent = true;
		xlrec.offnumParent = parent->offnum;
		xlrec.nodeI = parent->node;
		saveNodeLink(index, parent, current->blkno, current->offnum);

		/*
		 * Update redirection link (in old current buffer)
		 */
		if (redirectTuplePos != InvalidOffsetNumber)
			setRedirectionTuple(&saveCurrent, redirectTuplePos,
								current->blkno, current->offnum);

		/* Done modifying old current buffer, mark it dirty */
		MarkBufferDirty(saveCurrent.buffer);
	}
	else if (parent->buffer != InvalidBuffer)
	{
		/*
		 * new inner tuple will be stored on a new page
		 */
		Assert(newInnerBuffer != InvalidBuffer);

		/* Repoint "current" at the new inner tuple */
		current->buffer = newInnerBuffer;
		current->blkno = BufferGetBlockNumber(current->buffer);
		current->page = BufferGetPage(current->buffer);
		xlrec.offnumInner = current->offnum =
			SpGistPageAddNewItem(state, current->page,
								 (Item) innerTuple, innerTuple->size,
								 NULL, false);

		/* Done modifying new current buffer, mark it dirty */
		MarkBufferDirty(current->buffer);

		/*
		 * Update parent node link and mark parent page dirty
		 */
		xlrec.innerIsParent = (parent->buffer == current->buffer);
		xlrec.offnumParent = parent->offnum;
		xlrec.nodeI = parent->node;
		saveNodeLink(index, parent, current->blkno, current->offnum);

		/*
		 * Update redirection link (in old current buffer)
		 */
		if (redirectTuplePos != InvalidOffsetNumber)
			setRedirectionTuple(&saveCurrent, redirectTuplePos,
								current->blkno, current->offnum);

		/* Done modifying old current buffer, mark it dirty */
		MarkBufferDirty(saveCurrent.buffer);
	}
	else
	{
		/*
		 * Splitting root page, which was a leaf but now becomes inner page
		 * (and so "current" continues to point at it)
		 */
		Assert(SpGistBlockIsRoot(current->blkno));
		Assert(redirectTuplePos == InvalidOffsetNumber);

		SpGistInitBuffer(current->buffer, (isNulls ? SPGIST_NULLS : 0));
		xlrec.initInner = true;
		xlrec.innerIsParent = false;

		xlrec.offnumInner = current->offnum =
			PageAddItem(current->page, (Item) innerTuple, innerTuple->size,
						InvalidOffsetNumber, false, false);
		if (current->offnum != FirstOffsetNumber)
			elog(ERROR, "failed to add item of size %u to SPGiST index page",
				 innerTuple->size);

		/* No parent link to update, nor redirection to do */
		xlrec.offnumParent = InvalidOffsetNumber;
		xlrec.nodeI = 0;

		/* Done modifying new current buffer, mark it dirty */
		MarkBufferDirty(current->buffer);

		/* saveCurrent doesn't represent a different buffer */
		saveCurrent.buffer = InvalidBuffer;
	}

	if (RelationNeedsWAL(index) && !state->isBuild)
	{
		XLogRecPtr	recptr;
		int			flags;

		XLogBeginInsert();

		xlrec.nInsert = nToInsert;
		XLogRegisterData(&xlrec, SizeOfSpgxlogPickSplit);

		XLogRegisterData(toDelete,
						 sizeof(OffsetNumber) * xlrec.nDelete);
		XLogRegisterData(toInsert,
						 sizeof(OffsetNumber) * xlrec.nInsert);
		XLogRegisterData(leafPageSelect,
						 sizeof(uint8) * xlrec.nInsert);
		XLogRegisterData(innerTuple, innerTuple->size);
		XLogRegisterData(leafdata, leafptr - leafdata);

		/* Old leaf page */
		if (BufferIsValid(saveCurrent.buffer))
		{
			flags = REGBUF_STANDARD;
			if (xlrec.initSrc)
				flags |= REGBUF_WILL_INIT;
			XLogRegisterBuffer(0, saveCurrent.buffer, flags);
		}

		/* New leaf page */
		if (BufferIsValid(newLeafBuffer))
		{
			flags = REGBUF_STANDARD;
			if (xlrec.initDest)
				flags |= REGBUF_WILL_INIT;
			XLogRegisterBuffer(1, newLeafBuffer, flags);
		}

		/* Inner page */
		flags = REGBUF_STANDARD;
		if (xlrec.initInner)
			flags |= REGBUF_WILL_INIT;
		XLogRegisterBuffer(2, current->buffer, flags);

		/* Parent page, if different from inner page */
		if (parent->buffer != InvalidBuffer)
		{
			if (parent->buffer != current->buffer)
				XLogRegisterBuffer(3, parent->buffer, REGBUF_STANDARD);
			else
				Assert(xlrec.innerIsParent);
		}

		/* Issue the WAL record */
		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_PICKSPLIT);

		/* Update page LSNs on all affected pages */
		if (newLeafBuffer != InvalidBuffer)
		{
			Page		page = BufferGetPage(newLeafBuffer);

			PageSetLSN(page, recptr);
		}

		if (saveCurrent.buffer != InvalidBuffer)
		{
			Page		page = BufferGetPage(saveCurrent.buffer);

			PageSetLSN(page, recptr);
		}

		PageSetLSN(current->page, recptr);

		if (parent->buffer != InvalidBuffer)
		{
			PageSetLSN(parent->page, recptr);
		}
	}

	END_CRIT_SECTION();

	/* Update local free-space cache and unlock buffers */
	if (newLeafBuffer != InvalidBuffer)
	{
		SpGistSetLastUsedPage(index, newLeafBuffer);
		UnlockReleaseBuffer(newLeafBuffer);
	}
	if (saveCurrent.buffer != InvalidBuffer)
	{
		SpGistSetLastUsedPage(index, saveCurrent.buffer);
		UnlockReleaseBuffer(saveCurrent.buffer);
	}

	return insertedNew;
}

/*
 * spgMatchNode action: descend to N'th child node of current inner tuple
 */
static void
spgMatchNodeAction(Relation index, SpGistState *state,
				   SpGistInnerTuple innerTuple,
				   SPPageDesc *current, SPPageDesc *parent, int nodeN)
{
	int			i;
	SpGistNodeTuple node;

	/* Release previous parent buffer if any */
	if (parent->buffer != InvalidBuffer &&
		parent->buffer != current->buffer)
	{
		SpGistSetLastUsedPage(index, parent->buffer);
		UnlockReleaseBuffer(parent->buffer);
	}

	/* Repoint parent to specified node of current inner tuple */
	parent->blkno = current->blkno;
	parent->buffer = current->buffer;
	parent->page = current->page;
	parent->offnum = current->offnum;
	parent->node = nodeN;

	/* Locate that node */
	SGITITERATE(innerTuple, i, node)
	{
		if (i == nodeN)
			break;
	}

	if (i != nodeN)
		elog(ERROR, "failed to find requested node %d in SPGiST inner tuple",
			 nodeN);

	/* Point current to the downlink location, if any */
	if (ItemPointerIsValid(&node->t_tid))
	{
		current->blkno = ItemPointerGetBlockNumber(&node->t_tid);
		current->offnum = ItemPointerGetOffsetNumber(&node->t_tid);
	}
	else
	{
		/* Downlink is empty, so we'll need to find a new page */
		current->blkno = InvalidBlockNumber;
		current->offnum = InvalidOffsetNumber;
	}

	current->buffer = InvalidBuffer;
	current->page = NULL;
}

/*
 * spgAddNode action: add a node to the inner tuple at current
 */
static void
spgAddNodeAction(Relation index, SpGistState *state,
				 SpGistInnerTuple innerTuple,
				 SPPageDesc *current, SPPageDesc *parent,
				 int nodeN, Datum nodeLabel)
{
	SpGistInnerTuple newInnerTuple;
	spgxlogAddNode xlrec;

	/* Should not be applied to nulls */
	Assert(!SpGistPageStoresNulls(current->page));

	/* Construct new inner tuple with additional node */
	newInnerTuple = addNode(state, innerTuple, nodeLabel, nodeN);

	/* Prepare WAL record */
	STORE_STATE(state, xlrec.stateSrc);
	xlrec.offnum = current->offnum;

	/* we don't fill these unless we need to change the parent downlink */
	xlrec.parentBlk = -1;
	xlrec.offnumParent = InvalidOffsetNumber;
	xlrec.nodeI = 0;

	/* we don't fill these unless tuple has to be moved */
	xlrec.offnumNew = InvalidOffsetNumber;
	xlrec.newPage = false;

	if (PageGetExactFreeSpace(current->page) >=
		newInnerTuple->size - innerTuple->size)
	{
		/*
		 * We can replace the inner tuple by new version in-place
		 */
		START_CRIT_SECTION();

		PageIndexTupleDelete(current->page, current->offnum);
		if (PageAddItem(current->page,
						(Item) newInnerTuple, newInnerTuple->size,
						current->offnum, false, false) != current->offnum)
			elog(ERROR, "failed to add item of size %u to SPGiST index page",
				 newInnerTuple->size);

		MarkBufferDirty(current->buffer);

		if (RelationNeedsWAL(index) && !state->isBuild)
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterData(&xlrec, sizeof(xlrec));
			XLogRegisterData(newInnerTuple, newInnerTuple->size);

			XLogRegisterBuffer(0, current->buffer, REGBUF_STANDARD);

			recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_ADD_NODE);

			PageSetLSN(current->page, recptr);
		}

		END_CRIT_SECTION();
	}
	else
	{
		/*
		 * move inner tuple to another page, and update parent
		 */
		SpGistDeadTuple dt;
		SPPageDesc	saveCurrent;

		/*
		 * It should not be possible to get here for the root page, since we
		 * allow only one inner tuple on the root page, and spgFormInnerTuple
		 * always checks that inner tuples don't exceed the size of a page.
		 */
		if (SpGistBlockIsRoot(current->blkno))
			elog(ERROR, "cannot enlarge root tuple any more");
		Assert(parent->buffer != InvalidBuffer);

		saveCurrent = *current;

		xlrec.offnumParent = parent->offnum;
		xlrec.nodeI = parent->node;

		/*
		 * obtain new buffer with the same parity as current, since it will be
		 * a child of same parent tuple
		 */
		current->buffer = SpGistGetBuffer(index,
										  GBUF_INNER_PARITY(current->blkno),
										  newInnerTuple->size + sizeof(ItemIdData),
										  &xlrec.newPage);
		current->blkno = BufferGetBlockNumber(current->buffer);
		current->page = BufferGetPage(current->buffer);

		/*
		 * Let's just make real sure new current isn't same as old.  Right now
		 * that's impossible, but if SpGistGetBuffer ever got smart enough to
		 * delete placeholder tuples before checking space, maybe it wouldn't
		 * be impossible.  The case would appear to work except that WAL
		 * replay would be subtly wrong, so I think a mere assert isn't enough
		 * here.
		 */
		if (current->blkno == saveCurrent.blkno)
			elog(ERROR, "SPGiST new buffer shouldn't be same as old buffer");

		/*
		 * New current and parent buffer will both be modified; but note that
		 * parent buffer could be same as either new or old current.
		 */
		if (parent->buffer == saveCurrent.buffer)
			xlrec.parentBlk = 0;
		else if (parent->buffer == current->buffer)
			xlrec.parentBlk = 1;
		else
			xlrec.parentBlk = 2;

		START_CRIT_SECTION();

		/* insert new ... */
		xlrec.offnumNew = current->offnum =
			SpGistPageAddNewItem(state, current->page,
								 (Item) newInnerTuple, newInnerTuple->size,
								 NULL, false);

		MarkBufferDirty(current->buffer);

		/* update parent's downlink and mark parent page dirty */
		saveNodeLink(index, parent, current->blkno, current->offnum);

		/*
		 * Replace old tuple with a placeholder or redirection tuple.  Unless
		 * doing an index build, we have to insert a redirection tuple for
		 * possible concurrent scans.  We can't just delete it in any case,
		 * because that could change the offsets of other tuples on the page,
		 * breaking downlinks from their parents.
		 */
		if (state->isBuild)
			dt = spgFormDeadTuple(state, SPGIST_PLACEHOLDER,
								  InvalidBlockNumber, InvalidOffsetNumber);
		else
			dt = spgFormDeadTuple(state, SPGIST_REDIRECT,
								  current->blkno, current->offnum);

		PageIndexTupleDelete(saveCurrent.page, saveCurrent.offnum);
		if (PageAddItem(saveCurrent.page, (Item) dt, dt->size,
						saveCurrent.offnum,
						false, false) != saveCurrent.offnum)
			elog(ERROR, "failed to add item of size %u to SPGiST index page",
				 dt->size);

		if (state->isBuild)
			SpGistPageGetOpaque(saveCurrent.page)->nPlaceholder++;
		else
			SpGistPageGetOpaque(saveCurrent.page)->nRedirection++;

		MarkBufferDirty(saveCurrent.buffer);

		if (RelationNeedsWAL(index) && !state->isBuild)
		{
			XLogRecPtr	recptr;
			int			flags;

			XLogBeginInsert();

			/* orig page */
			XLogRegisterBuffer(0, saveCurrent.buffer, REGBUF_STANDARD);
			/* new page */
			flags = REGBUF_STANDARD;
			if (xlrec.newPage)
				flags |= REGBUF_WILL_INIT;
			XLogRegisterBuffer(1, current->buffer, flags);
			/* parent page (if different from orig and new) */
			if (xlrec.parentBlk == 2)
				XLogRegisterBuffer(2, parent->buffer, REGBUF_STANDARD);

			XLogRegisterData(&xlrec, sizeof(xlrec));
			XLogRegisterData(newInnerTuple, newInnerTuple->size);

			recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_ADD_NODE);

			/* we don't bother to check if any of these are redundant */
			PageSetLSN(current->page, recptr);
			PageSetLSN(parent->page, recptr);
			PageSetLSN(saveCurrent.page, recptr);
		}

		END_CRIT_SECTION();

		/* Release saveCurrent if it's not same as current or parent */
		if (saveCurrent.buffer != current->buffer &&
			saveCurrent.buffer != parent->buffer)
		{
			SpGistSetLastUsedPage(index, saveCurrent.buffer);
			UnlockReleaseBuffer(saveCurrent.buffer);
		}
	}
}

/*
 * spgSplitNode action: split inner tuple at current into prefix and postfix
 */
static void
spgSplitNodeAction(Relation index, SpGistState *state,
				   SpGistInnerTuple innerTuple,
				   SPPageDesc *current, spgChooseOut *out)
{
	SpGistInnerTuple prefixTuple,
				postfixTuple;
	SpGistNodeTuple node,
			   *nodes;
	BlockNumber postfixBlkno;
	OffsetNumber postfixOffset;
	int			i;
	spgxlogSplitTuple xlrec;
	Buffer		newBuffer = InvalidBuffer;

	/* Should not be applied to nulls */
	Assert(!SpGistPageStoresNulls(current->page));

	/* Check opclass gave us sane values */
	if (out->result.splitTuple.prefixNNodes <= 0 ||
		out->result.splitTuple.prefixNNodes > SGITMAXNNODES)
		elog(ERROR, "invalid number of prefix nodes: %d",
			 out->result.splitTuple.prefixNNodes);
	if (out->result.splitTuple.childNodeN < 0 ||
		out->result.splitTuple.childNodeN >=
		out->result.splitTuple.prefixNNodes)
		elog(ERROR, "invalid child node number: %d",
			 out->result.splitTuple.childNodeN);

	/*
	 * Construct new prefix tuple with requested number of nodes.  We'll fill
	 * in the childNodeN'th node's downlink below.
	 */
	nodes = (SpGistNodeTuple *) palloc(sizeof(SpGistNodeTuple) *
									   out->result.splitTuple.prefixNNodes);

	for (i = 0; i < out->result.splitTuple.prefixNNodes; i++)
	{
		Datum		label = (Datum) 0;
		bool		labelisnull;

		labelisnull = (out->result.splitTuple.prefixNodeLabels == NULL);
		if (!labelisnull)
			label = out->result.splitTuple.prefixNodeLabels[i];
		nodes[i] = spgFormNodeTuple(state, label, labelisnull);
	}

	prefixTuple = spgFormInnerTuple(state,
									out->result.splitTuple.prefixHasPrefix,
									out->result.splitTuple.prefixPrefixDatum,
									out->result.splitTuple.prefixNNodes,
									nodes);

	/* it must fit in the space that innerTuple now occupies */
	if (prefixTuple->size > innerTuple->size)
		elog(ERROR, "SPGiST inner-tuple split must not produce longer prefix");

	/*
	 * Construct new postfix tuple, containing all nodes of innerTuple with
	 * same node datums, but with the prefix specified by the picksplit
	 * function.
	 */
	nodes = palloc(sizeof(SpGistNodeTuple) * innerTuple->nNodes);
	SGITITERATE(innerTuple, i, node)
	{
		nodes[i] = node;
	}

	postfixTuple = spgFormInnerTuple(state,
									 out->result.splitTuple.postfixHasPrefix,
									 out->result.splitTuple.postfixPrefixDatum,
									 innerTuple->nNodes, nodes);

	/* Postfix tuple is allTheSame if original tuple was */
	postfixTuple->allTheSame = innerTuple->allTheSame;

	/* prep data for WAL record */
	xlrec.newPage = false;

	/*
	 * If we can't fit both tuples on the current page, get a new page for the
	 * postfix tuple.  In particular, can't split to the root page.
	 *
	 * For the space calculation, note that prefixTuple replaces innerTuple
	 * but postfixTuple will be a new entry.
	 */
	if (SpGistBlockIsRoot(current->blkno) ||
		SpGistPageGetFreeSpace(current->page, 1) + innerTuple->size <
		prefixTuple->size + postfixTuple->size + sizeof(ItemIdData))
	{
		/*
		 * Choose page with next triple parity, because postfix tuple is a
		 * child of prefix one
		 */
		newBuffer = SpGistGetBuffer(index,
									GBUF_INNER_PARITY(current->blkno + 1),
									postfixTuple->size + sizeof(ItemIdData),
									&xlrec.newPage);
	}

	START_CRIT_SECTION();

	/*
	 * Replace old tuple by prefix tuple
	 */
	PageIndexTupleDelete(current->page, current->offnum);
	xlrec.offnumPrefix = PageAddItem(current->page,
									 (Item) prefixTuple, prefixTuple->size,
									 current->offnum, false, false);
	if (xlrec.offnumPrefix != current->offnum)
		elog(ERROR, "failed to add item of size %u to SPGiST index page",
			 prefixTuple->size);

	/*
	 * put postfix tuple into appropriate page
	 */
	if (newBuffer == InvalidBuffer)
	{
		postfixBlkno = current->blkno;
		xlrec.offnumPostfix = postfixOffset =
			SpGistPageAddNewItem(state, current->page,
								 (Item) postfixTuple, postfixTuple->size,
								 NULL, false);
		xlrec.postfixBlkSame = true;
	}
	else
	{
		postfixBlkno = BufferGetBlockNumber(newBuffer);
		xlrec.offnumPostfix = postfixOffset =
			SpGistPageAddNewItem(state, BufferGetPage(newBuffer),
								 (Item) postfixTuple, postfixTuple->size,
								 NULL, false);
		MarkBufferDirty(newBuffer);
		xlrec.postfixBlkSame = false;
	}

	/*
	 * And set downlink pointer in the prefix tuple to point to postfix tuple.
	 * (We can't avoid this step by doing the above two steps in opposite
	 * order, because there might not be enough space on the page to insert
	 * the postfix tuple first.)  We have to update the local copy of the
	 * prefixTuple too, because that's what will be written to WAL.
	 */
	spgUpdateNodeLink(prefixTuple, out->result.splitTuple.childNodeN,
					  postfixBlkno, postfixOffset);
	prefixTuple = (SpGistInnerTuple) PageGetItem(current->page,
												 PageGetItemId(current->page, current->offnum));
	spgUpdateNodeLink(prefixTuple, out->result.splitTuple.childNodeN,
					  postfixBlkno, postfixOffset);

	MarkBufferDirty(current->buffer);

	if (RelationNeedsWAL(index) && !state->isBuild)
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));
		XLogRegisterData(prefixTuple, prefixTuple->size);
		XLogRegisterData(postfixTuple, postfixTuple->size);

		XLogRegisterBuffer(0, current->buffer, REGBUF_STANDARD);
		if (newBuffer != InvalidBuffer)
		{
			int			flags;

			flags = REGBUF_STANDARD;
			if (xlrec.newPage)
				flags |= REGBUF_WILL_INIT;
			XLogRegisterBuffer(1, newBuffer, flags);
		}

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_SPLIT_TUPLE);

		PageSetLSN(current->page, recptr);

		if (newBuffer != InvalidBuffer)
		{
			PageSetLSN(BufferGetPage(newBuffer), recptr);
		}
	}

	END_CRIT_SECTION();

	/* Update local free-space cache and release buffer */
	if (newBuffer != InvalidBuffer)
	{
		SpGistSetLastUsedPage(index, newBuffer);
		UnlockReleaseBuffer(newBuffer);
	}
}

/*
 * Insert one item into the index.
 *
 * Returns true on success, false if we failed to complete the insertion
 * (typically because of conflict with a concurrent insert).  In the latter
 * case, caller should re-call spgdoinsert() with the same args.
 */
bool
spgdoinsert(Relation index, SpGistState *state,
			ItemPointer heapPtr, Datum *datums, bool *isnulls)
{
	bool		result = true;
	TupleDesc	leafDescriptor = state->leafTupDesc;
	bool		isnull = isnulls[spgKeyColumn];
	int			level = 0;
	Datum		leafDatums[INDEX_MAX_KEYS];
	int			leafSize;
	int			bestLeafSize;
	int			numNoProgressCycles = 0;
	SPPageDesc	current,
				parent;
	FmgrInfo   *procinfo = NULL;

	/*
	 * Look up FmgrInfo of the user-defined choose function once, to save
	 * cycles in the loop below.
	 */
	if (!isnull)
		procinfo = index_getprocinfo(index, 1, SPGIST_CHOOSE_PROC);

	/*
	 * Prepare the leaf datum to insert.
	 *
	 * If an optional "compress" method is provided, then call it to form the
	 * leaf key datum from the input datum.  Otherwise, store the input datum
	 * as is.  Since we don't use index_form_tuple in this AM, we have to make
	 * sure value to be inserted is not toasted; FormIndexDatum doesn't
	 * guarantee that.  But we assume the "compress" method to return an
	 * untoasted value.
	 */
	if (!isnull)
	{
		if (OidIsValid(index_getprocid(index, 1, SPGIST_COMPRESS_PROC)))
		{
			FmgrInfo   *compressProcinfo = NULL;

			compressProcinfo = index_getprocinfo(index, 1, SPGIST_COMPRESS_PROC);
			leafDatums[spgKeyColumn] =
				FunctionCall1Coll(compressProcinfo,
								  index->rd_indcollation[spgKeyColumn],
								  datums[spgKeyColumn]);
		}
		else
		{
			Assert(state->attLeafType.type == state->attType.type);

			if (state->attType.attlen == -1)
				leafDatums[spgKeyColumn] =
					PointerGetDatum(PG_DETOAST_DATUM(datums[spgKeyColumn]));
			else
				leafDatums[spgKeyColumn] = datums[spgKeyColumn];
		}
	}
	else
		leafDatums[spgKeyColumn] = (Datum) 0;

	/* Likewise, ensure that any INCLUDE values are not toasted */
	for (int i = spgFirstIncludeColumn; i < leafDescriptor->natts; i++)
	{
		if (!isnulls[i])
		{
			if (TupleDescCompactAttr(leafDescriptor, i)->attlen == -1)
				leafDatums[i] = PointerGetDatum(PG_DETOAST_DATUM(datums[i]));
			else
				leafDatums[i] = datums[i];
		}
		else
			leafDatums[i] = (Datum) 0;
	}

	/*
	 * Compute space needed for a leaf tuple containing the given data.
	 */
	leafSize = SpGistGetLeafTupleSize(leafDescriptor, leafDatums, isnulls);
	/* Account for an item pointer, too */
	leafSize += sizeof(ItemIdData);

	/*
	 * If it isn't gonna fit, and the opclass can't reduce the datum size by
	 * suffixing, bail out now rather than doing a lot of useless work.
	 */
	if (leafSize > SPGIST_PAGE_CAPACITY &&
		(isnull || !state->config.longValuesOK))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
						leafSize - sizeof(ItemIdData),
						SPGIST_PAGE_CAPACITY - sizeof(ItemIdData),
						RelationGetRelationName(index)),
				 errhint("Values larger than a buffer page cannot be indexed.")));
	bestLeafSize = leafSize;

	/* Initialize "current" to the appropriate root page */
	current.blkno = isnull ? SPGIST_NULL_BLKNO : SPGIST_ROOT_BLKNO;
	current.buffer = InvalidBuffer;
	current.page = NULL;
	current.offnum = FirstOffsetNumber;
	current.node = -1;

	/* "parent" is invalid for the moment */
	parent.blkno = InvalidBlockNumber;
	parent.buffer = InvalidBuffer;
	parent.page = NULL;
	parent.offnum = InvalidOffsetNumber;
	parent.node = -1;

	/*
	 * Before entering the loop, try to clear any pending interrupt condition.
	 * If a query cancel is pending, we might as well accept it now not later;
	 * while if a non-canceling condition is pending, servicing it here avoids
	 * having to restart the insertion and redo all the work so far.
	 */
	CHECK_FOR_INTERRUPTS();

	for (;;)
	{
		bool		isNew = false;

		/*
		 * Bail out if query cancel is pending.  We must have this somewhere
		 * in the loop since a broken opclass could produce an infinite
		 * picksplit loop.  However, because we'll be holding buffer lock(s)
		 * after the first iteration, ProcessInterrupts() wouldn't be able to
		 * throw a cancel error here.  Hence, if we see that an interrupt is
		 * pending, break out of the loop and deal with the situation below.
		 * Set result = false because we must restart the insertion if the
		 * interrupt isn't a query-cancel-or-die case.
		 */
		if (INTERRUPTS_PENDING_CONDITION())
		{
			result = false;
			break;
		}

		if (current.blkno == InvalidBlockNumber)
		{
			/*
			 * Create a leaf page.  If leafSize is too large to fit on a page,
			 * we won't actually use the page yet, but it simplifies the API
			 * for doPickSplit to always have a leaf page at hand; so just
			 * quietly limit our request to a page size.
			 */
			current.buffer =
				SpGistGetBuffer(index,
								GBUF_LEAF | (isnull ? GBUF_NULLS : 0),
								Min(leafSize, SPGIST_PAGE_CAPACITY),
								&isNew);
			current.blkno = BufferGetBlockNumber(current.buffer);
		}
		else if (parent.buffer == InvalidBuffer)
		{
			/* we hold no parent-page lock, so no deadlock is possible */
			current.buffer = ReadBuffer(index, current.blkno);
			LockBuffer(current.buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (current.blkno != parent.blkno)
		{
			/* descend to a new child page */
			current.buffer = ReadBuffer(index, current.blkno);

			/*
			 * Attempt to acquire lock on child page.  We must beware of
			 * deadlock against another insertion process descending from that
			 * page to our parent page (see README).  If we fail to get lock,
			 * abandon the insertion and tell our caller to start over.
			 *
			 * XXX this could be improved, because failing to get lock on a
			 * buffer is not proof of a deadlock situation; the lock might be
			 * held by a reader, or even just background writer/checkpointer
			 * process.  Perhaps it'd be worth retrying after sleeping a bit?
			 */
			if (!ConditionalLockBuffer(current.buffer))
			{
				ReleaseBuffer(current.buffer);
				UnlockReleaseBuffer(parent.buffer);
				return false;
			}
		}
		else
		{
			/* inner tuple can be stored on the same page as parent one */
			current.buffer = parent.buffer;
		}
		current.page = BufferGetPage(current.buffer);

		/* should not arrive at a page of the wrong type */
		if (isnull ? !SpGistPageStoresNulls(current.page) :
			SpGistPageStoresNulls(current.page))
			elog(ERROR, "SPGiST index page %u has wrong nulls flag",
				 current.blkno);

		if (SpGistPageIsLeaf(current.page))
		{
			SpGistLeafTuple leafTuple;
			int			nToSplit,
						sizeToSplit;

			leafTuple = spgFormLeafTuple(state, heapPtr, leafDatums, isnulls);
			if (leafTuple->size + sizeof(ItemIdData) <=
				SpGistPageGetFreeSpace(current.page, 1))
			{
				/* it fits on page, so insert it and we're done */
				addLeafTuple(index, state, leafTuple,
							 &current, &parent, isnull, isNew);
				break;
			}
			else if ((sizeToSplit =
					  checkSplitConditions(index, state, &current,
										   &nToSplit)) < SPGIST_PAGE_CAPACITY / 2 &&
					 nToSplit < 64 &&
					 leafTuple->size + sizeof(ItemIdData) + sizeToSplit <= SPGIST_PAGE_CAPACITY)
			{
				/*
				 * the amount of data is pretty small, so just move the whole
				 * chain to another leaf page rather than splitting it.
				 */
				Assert(!isNew);
				moveLeafs(index, state, &current, &parent, leafTuple, isnull);
				break;			/* we're done */
			}
			else
			{
				/* picksplit */
				if (doPickSplit(index, state, &current, &parent,
								leafTuple, level, isnull, isNew))
					break;		/* doPickSplit installed new tuples */

				/* leaf tuple will not be inserted yet */
				pfree(leafTuple);

				/*
				 * current now describes new inner tuple, go insert into it
				 */
				Assert(!SpGistPageIsLeaf(current.page));
				goto process_inner_tuple;
			}
		}
		else					/* non-leaf page */
		{
			/*
			 * Apply the opclass choose function to figure out how to insert
			 * the given datum into the current inner tuple.
			 */
			SpGistInnerTuple innerTuple;
			spgChooseIn in;
			spgChooseOut out;

			/*
			 * spgAddNode and spgSplitTuple cases will loop back to here to
			 * complete the insertion operation.  Just in case the choose
			 * function is broken and produces add or split requests
			 * repeatedly, check for query cancel (see comments above).
			 */
	process_inner_tuple:
			if (INTERRUPTS_PENDING_CONDITION())
			{
				result = false;
				break;
			}

			innerTuple = (SpGistInnerTuple) PageGetItem(current.page,
														PageGetItemId(current.page, current.offnum));

			in.datum = datums[spgKeyColumn];
			in.leafDatum = leafDatums[spgKeyColumn];
			in.level = level;
			in.allTheSame = innerTuple->allTheSame;
			in.hasPrefix = (innerTuple->prefixSize > 0);
			in.prefixDatum = SGITDATUM(innerTuple, state);
			in.nNodes = innerTuple->nNodes;
			in.nodeLabels = spgExtractNodeLabels(state, innerTuple);

			memset(&out, 0, sizeof(out));

			if (!isnull)
			{
				/* use user-defined choose method */
				FunctionCall2Coll(procinfo,
								  index->rd_indcollation[0],
								  PointerGetDatum(&in),
								  PointerGetDatum(&out));
			}
			else
			{
				/* force "match" action (to insert to random subnode) */
				out.resultType = spgMatchNode;
			}

			if (innerTuple->allTheSame)
			{
				/*
				 * It's not allowed to do an AddNode at an allTheSame tuple.
				 * Opclass must say "match", in which case we choose a random
				 * one of the nodes to descend into, or "split".
				 */
				if (out.resultType == spgAddNode)
					elog(ERROR, "cannot add a node to an allTheSame inner tuple");
				else if (out.resultType == spgMatchNode)
					out.result.matchNode.nodeN =
						pg_prng_uint64_range(&pg_global_prng_state,
											 0, innerTuple->nNodes - 1);
			}

			switch (out.resultType)
			{
				case spgMatchNode:
					/* Descend to N'th child node */
					spgMatchNodeAction(index, state, innerTuple,
									   &current, &parent,
									   out.result.matchNode.nodeN);
					/* Adjust level as per opclass request */
					level += out.result.matchNode.levelAdd;
					/* Replace leafDatum and recompute leafSize */
					if (!isnull)
					{
						leafDatums[spgKeyColumn] = out.result.matchNode.restDatum;
						leafSize = SpGistGetLeafTupleSize(leafDescriptor,
														  leafDatums, isnulls);
						leafSize += sizeof(ItemIdData);
					}

					/*
					 * Check new tuple size; fail if it can't fit, unless the
					 * opclass says it can handle the situation by suffixing.
					 *
					 * However, the opclass can only shorten the leaf datum,
					 * which may not be enough to ever make the tuple fit,
					 * since INCLUDE columns might alone use more than a page.
					 * Depending on the opclass' behavior, that could lead to
					 * an infinite loop --- spgtextproc.c, for example, will
					 * just repeatedly generate an empty-string leaf datum
					 * once it runs out of data.  Actual bugs in opclasses
					 * might cause infinite looping, too.  To detect such a
					 * loop, check to see if we are making progress by
					 * reducing the leafSize in each pass.  This is a bit
					 * tricky though.  Because of alignment considerations,
					 * the total tuple size might not decrease on every pass.
					 * Also, there are edge cases where the choose method
					 * might seem to not make progress for a cycle or two.
					 * Somewhat arbitrarily, we allow up to 10 no-progress
					 * iterations before failing.  (This limit should be more
					 * than MAXALIGN, to accommodate opclasses that trim one
					 * byte from the leaf datum per pass.)
					 */
					if (leafSize > SPGIST_PAGE_CAPACITY)
					{
						bool		ok = false;

						if (state->config.longValuesOK && !isnull)
						{
							if (leafSize < bestLeafSize)
							{
								ok = true;
								bestLeafSize = leafSize;
								numNoProgressCycles = 0;
							}
							else if (++numNoProgressCycles < 10)
								ok = true;
						}
						if (!ok)
							ereport(ERROR,
									(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									 errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
											leafSize - sizeof(ItemIdData),
											SPGIST_PAGE_CAPACITY - sizeof(ItemIdData),
											RelationGetRelationName(index)),
									 errhint("Values larger than a buffer page cannot be indexed.")));
					}

					/*
					 * Loop around and attempt to insert the new leafDatum at
					 * "current" (which might reference an existing child
					 * tuple, or might be invalid to force us to find a new
					 * page for the tuple).
					 */
					break;
				case spgAddNode:
					/* AddNode is not sensible if nodes don't have labels */
					if (in.nodeLabels == NULL)
						elog(ERROR, "cannot add a node to an inner tuple without node labels");
					/* Add node to inner tuple, per request */
					spgAddNodeAction(index, state, innerTuple,
									 &current, &parent,
									 out.result.addNode.nodeN,
									 out.result.addNode.nodeLabel);

					/*
					 * Retry insertion into the enlarged node.  We assume that
					 * we'll get a MatchNode result this time.
					 */
					goto process_inner_tuple;
					break;
				case spgSplitTuple:
					/* Split inner tuple, per request */
					spgSplitNodeAction(index, state, innerTuple,
									   &current, &out);

					/* Retry insertion into the split node */
					goto process_inner_tuple;
					break;
				default:
					elog(ERROR, "unrecognized SPGiST choose result: %d",
						 (int) out.resultType);
					break;
			}
		}
	}							/* end loop */

	/*
	 * Release any buffers we're still holding.  Beware of possibility that
	 * current and parent reference same buffer.
	 */
	if (current.buffer != InvalidBuffer)
	{
		SpGistSetLastUsedPage(index, current.buffer);
		UnlockReleaseBuffer(current.buffer);
	}
	if (parent.buffer != InvalidBuffer &&
		parent.buffer != current.buffer)
	{
		SpGistSetLastUsedPage(index, parent.buffer);
		UnlockReleaseBuffer(parent.buffer);
	}

	/*
	 * We do not support being called while some outer function is holding a
	 * buffer lock (or any other reason to postpone query cancels).  If that
	 * were the case, telling the caller to retry would create an infinite
	 * loop.
	 */
	Assert(INTERRUPTS_CAN_BE_PROCESSED());

	/*
	 * Finally, check for interrupts again.  If there was a query cancel,
	 * ProcessInterrupts() will be able to throw the error here.  If it was
	 * some other kind of interrupt that can just be cleared, return false to
	 * tell our caller to retry.
	 */
	CHECK_FOR_INTERRUPTS();

	return result;
}
