/*-------------------------------------------------------------------------
 *
 * fsmpage.c
 *	  routines to search and manipulate one FSM page.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/freespace/fsmpage.c
 *
 * NOTES:
 *
 *	The public functions in this file form an API that hides the internal
 *	structure of a FSM page. This allows freespace.c to treat each FSM page
 *	as a black box with SlotsPerPage "slots". fsm_set_avail() and
 *	fsm_get_avail() let you get/set the value of a slot, and
 *	fsm_search_avail() lets you search for a slot with value >= X.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/fsm_internals.h"

/* Macros to navigate the tree within a page. Root has index zero. */
#define leftchild(x)	(2 * (x) + 1)
#define rightchild(x)	(2 * (x) + 2)
#define parentof(x)		(((x) - 1) / 2)

/*
 * Find right neighbor of x, wrapping around within the level
 */
static int
rightneighbor(int x)
{
	/*
	 * Move right. This might wrap around, stepping to the leftmost node at
	 * the next level.
	 */
	x++;

	/*
	 * Check if we stepped to the leftmost node at next level, and correct if
	 * so. The leftmost nodes at each level are numbered x = 2^level - 1, so
	 * check if (x + 1) is a power of two, using a standard
	 * twos-complement-arithmetic trick.
	 */
	if (((x + 1) & x) == 0)
		x = parentof(x);

	return x;
}

/*
 * Sets the value of a slot on page. Returns true if the page was modified.
 *
 * The caller must hold an exclusive lock on the page.
 */
bool
fsm_set_avail(Page page, int slot, uint8 value)
{
	int			nodeno = NonLeafNodesPerPage + slot;
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	uint8		oldvalue;

	Assert(slot < LeafNodesPerPage);

	oldvalue = fsmpage->fp_nodes[nodeno];

	/* If the value hasn't changed, we don't need to do anything */
	if (oldvalue == value && value <= fsmpage->fp_nodes[0])
		return false;

	fsmpage->fp_nodes[nodeno] = value;

	/*
	 * Propagate up, until we hit the root or a node that doesn't need to be
	 * updated.
	 */
	do
	{
		uint8		newvalue = 0;
		int			lchild;
		int			rchild;

		nodeno = parentof(nodeno);
		lchild = leftchild(nodeno);
		rchild = lchild + 1;

		newvalue = fsmpage->fp_nodes[lchild];
		if (rchild < NodesPerPage)
			newvalue = Max(newvalue,
						   fsmpage->fp_nodes[rchild]);

		oldvalue = fsmpage->fp_nodes[nodeno];
		if (oldvalue == newvalue)
			break;

		fsmpage->fp_nodes[nodeno] = newvalue;
	} while (nodeno > 0);

	/*
	 * sanity check: if the new value is (still) higher than the value at the
	 * top, the tree is corrupt.  If so, rebuild.
	 */
	if (value > fsmpage->fp_nodes[0])
		fsm_rebuild_page(page);

	return true;
}

/*
 * Returns the value of given slot on page.
 *
 * Since this is just a read-only access of a single byte, the page doesn't
 * need to be locked.
 */
uint8
fsm_get_avail(Page page, int slot)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);

	Assert(slot < LeafNodesPerPage);

	return fsmpage->fp_nodes[NonLeafNodesPerPage + slot];
}

/*
 * Returns the value at the root of a page.
 *
 * Since this is just a read-only access of a single byte, the page doesn't
 * need to be locked.
 */
uint8
fsm_get_max_avail(Page page)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);

	return fsmpage->fp_nodes[0];
}

/*
 * Searches for a slot with category at least minvalue.
 * Returns slot number, or -1 if none found.
 *
 * The caller must hold at least a shared lock on the page, and this
 * function can unlock and lock the page again in exclusive mode if it
 * needs to be updated. exclusive_lock_held should be set to true if the
 * caller is already holding an exclusive lock, to avoid extra work.
 *
 * If advancenext is false, fp_next_slot is set to point to the returned
 * slot, and if it's true, to the slot after the returned slot.
 */
int
fsm_search_avail(Buffer buf, uint8 minvalue, bool advancenext,
				 bool exclusive_lock_held)
{
	Page		page = BufferGetPage(buf);
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	int			nodeno;
	int			target;
	uint16		slot;

restart:

	/*
	 * Check the root first, and exit quickly if there's no leaf with enough
	 * free space
	 */
	if (fsmpage->fp_nodes[0] < minvalue)
		return -1;

	/*
	 * Start search using fp_next_slot.  It's just a hint, so check that it's
	 * sane.  (This also handles wrapping around when the prior call returned
	 * the last slot on the page.)
	 */
	target = fsmpage->fp_next_slot;
	if (target < 0 || target >= LeafNodesPerPage)
		target = 0;
	target += NonLeafNodesPerPage;

	/*----------
	 * Start the search from the target slot.  At every step, move one
	 * node to the right, then climb up to the parent.  Stop when we reach
	 * a node with enough free space (as we must, since the root has enough
	 * space).
	 *
	 * The idea is to gradually expand our "search triangle", that is, all
	 * nodes covered by the current node, and to be sure we search to the
	 * right from the start point.  At the first step, only the target slot
	 * is examined.  When we move up from a left child to its parent, we are
	 * adding the right-hand subtree of that parent to the search triangle.
	 * When we move right then up from a right child, we are dropping the
	 * current search triangle (which we know doesn't contain any suitable
	 * page) and instead looking at the next-larger-size triangle to its
	 * right.  So we never look left from our original start point, and at
	 * each step the size of the search triangle doubles, ensuring it takes
	 * only log2(N) work to search N pages.
	 *
	 * The "move right" operation will wrap around if it hits the right edge
	 * of the tree, so the behavior is still good if we start near the right.
	 * Note also that the move-and-climb behavior ensures that we can't end
	 * up on one of the missing nodes at the right of the leaf level.
	 *
	 * For example, consider this tree:
	 *
	 *		   7
	 *	   7	   6
	 *	 5	 7	 6	 5
	 *	4 5 5 7 2 6 5 2
	 *				T
	 *
	 * Assume that the target node is the node indicated by the letter T,
	 * and we're searching for a node with value of 6 or higher. The search
	 * begins at T. At the first iteration, we move to the right, then to the
	 * parent, arriving at the rightmost 5. At the second iteration, we move
	 * to the right, wrapping around, then climb up, arriving at the 7 on the
	 * third level.  7 satisfies our search, so we descend down to the bottom,
	 * following the path of sevens.  This is in fact the first suitable page
	 * to the right of (allowing for wraparound) our start point.
	 *----------
	 */
	nodeno = target;
	while (nodeno > 0)
	{
		if (fsmpage->fp_nodes[nodeno] >= minvalue)
			break;

		/*
		 * Move to the right, wrapping around on same level if necessary, then
		 * climb up.
		 */
		nodeno = parentof(rightneighbor(nodeno));
	}

	/*
	 * We're now at a node with enough free space, somewhere in the middle of
	 * the tree. Descend to the bottom, following a path with enough free
	 * space, preferring to move left if there's a choice.
	 */
	while (nodeno < NonLeafNodesPerPage)
	{
		int			childnodeno = leftchild(nodeno);

		if (childnodeno < NodesPerPage &&
			fsmpage->fp_nodes[childnodeno] >= minvalue)
		{
			nodeno = childnodeno;
			continue;
		}
		childnodeno++;			/* point to right child */
		if (childnodeno < NodesPerPage &&
			fsmpage->fp_nodes[childnodeno] >= minvalue)
		{
			nodeno = childnodeno;
		}
		else
		{
			/*
			 * Oops. The parent node promised that either left or right child
			 * has enough space, but neither actually did. This can happen in
			 * case of a "torn page", IOW if we crashed earlier while writing
			 * the page to disk, and only part of the page made it to disk.
			 *
			 * Fix the corruption and restart.
			 */
			RelFileNode rnode;
			ForkNumber	forknum;
			BlockNumber blknum;

			BufferGetTag(buf, &rnode, &forknum, &blknum);
			elog(DEBUG1, "fixing corrupt FSM block %u, relation %u/%u/%u",
				 blknum, rnode.spcNode, rnode.dbNode, rnode.relNode);

			/* make sure we hold an exclusive lock */
			if (!exclusive_lock_held)
			{
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				exclusive_lock_held = true;
			}
			fsm_rebuild_page(page);
			MarkBufferDirtyHint(buf, false);
			goto restart;
		}
	}

	/* We're now at the bottom level, at a node with enough space. */
	slot = nodeno - NonLeafNodesPerPage;

	/*
	 * Update the next-target pointer. Note that we do this even if we're only
	 * holding a shared lock, on the grounds that it's better to use a shared
	 * lock and get a garbled next pointer every now and then, than take the
	 * concurrency hit of an exclusive lock.
	 *
	 * Wrap-around is handled at the beginning of this function.
	 */
	fsmpage->fp_next_slot = slot + (advancenext ? 1 : 0);

	return slot;
}

/*
 * Sets the available space to zero for all slots numbered >= nslots.
 * Returns true if the page was modified.
 */
bool
fsm_truncate_avail(Page page, int nslots)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	uint8	   *ptr;
	bool		changed = false;

	Assert(nslots >= 0 && nslots < LeafNodesPerPage);

	/* Clear all truncated leaf nodes */
	ptr = &fsmpage->fp_nodes[NonLeafNodesPerPage + nslots];
	for (; ptr < &fsmpage->fp_nodes[NodesPerPage]; ptr++)
	{
		if (*ptr != 0)
			changed = true;
		*ptr = 0;
	}

	/* Fix upper nodes. */
	if (changed)
		fsm_rebuild_page(page);

	return changed;
}

/*
 * Reconstructs the upper levels of a page. Returns true if the page
 * was modified.
 */
bool
fsm_rebuild_page(Page page)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	bool		changed = false;
	int			nodeno;

	/*
	 * Start from the lowest non-leaf level, at last node, working our way
	 * backwards, through all non-leaf nodes at all levels, up to the root.
	 */
	for (nodeno = NonLeafNodesPerPage - 1; nodeno >= 0; nodeno--)
	{
		int			lchild = leftchild(nodeno);
		int			rchild = lchild + 1;
		uint8		newvalue = 0;

		/* The first few nodes we examine might have zero or one child. */
		if (lchild < NodesPerPage)
			newvalue = fsmpage->fp_nodes[lchild];

		if (rchild < NodesPerPage)
			newvalue = Max(newvalue,
						   fsmpage->fp_nodes[rchild]);

		if (fsmpage->fp_nodes[nodeno] != newvalue)
		{
			fsmpage->fp_nodes[nodeno] = newvalue;
			changed = true;
		}
	}

	return changed;
}
