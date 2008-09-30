/*-------------------------------------------------------------------------
 *
 * fsmpage.c
 *	  routines to search and manipulate one FSM page.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/freespace/fsmpage.c,v 1.1 2008/09/30 10:52:13 heikki Exp $
 *
 * NOTES:
 *
 *  The public functions in this file form an API that hides the internal
 *  structure of a FSM page. This allows freespace.c to treat each FSM page
 *  as a black box with SlotsPerPage "slots". fsm_set_avail() and
 *  fsm_get_avail() let's you get/set the value of a slot, and
 *  fsm_search_avail() let's you search for a slot with value >= X.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/fsm_internals.h"

/* macros to navigate the tree within a page. */
#define leftchild(x)	(2 * (x) + 1)
#define rightchild(x)	(2 * (x) + 2)
#define parentof(x)		(((x) - 1) / 2)

/* returns right sibling of x, wrapping around within the level */
static int
rightsibling(int x)
{
	/*
	 * Move right. This might wrap around, stepping to the leftmost node at
	 * the next level.
	 */
	x++;

	/*
	 * Check if we stepped to the leftmost node at next level, and correct
	 * if so. The leftmost nodes at each level are of form x = 2^level - 1, so
	 * check if (x + 1) is a power of two.
	 */
	if (((x + 1) & x) == 0)
		x = parentof(x);

	return x;
}

/*
 * Sets the value of a slot on page. Returns true if the page was
 * modified.
 *
 * The caller must hold an exclusive lock on the page.
 */
bool
fsm_set_avail(Page page, int slot, uint8 value)
{
	int nodeno = NonLeafNodesPerPage + slot;
	FSMPage fsmpage = (FSMPage) PageGetContents(page);
	uint8 oldvalue;

	Assert(slot < LeafNodesPerPage);

	oldvalue = fsmpage->fp_nodes[nodeno];

	/* If the value hasn't changed, we don't need to do anything */
	if (oldvalue == value && value <= fsmpage->fp_nodes[0])
		return false;

	fsmpage->fp_nodes[nodeno] = value;

	/*
	 * Propagate up, until we hit the root or a node that doesn't
	 * need to be updated.
	 */
	do
	{
		uint8 newvalue = 0;
		int lchild;
		int rchild;

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
	 * sanity check: if the new value value is higher than the value
	 * at the top, the tree is corrupt.
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
	FSMPage fsmpage = (FSMPage) PageGetContents(page);

	return fsmpage->fp_nodes[NonLeafNodesPerPage + slot];
}

/*
 * Returns the value at the root of a page.
 * Since this is just a read-only access of a single byte, the page doesn't
 * need to be locked.
 */
uint8
fsm_get_max_avail(Page page)
{
	FSMPage fsmpage = (FSMPage) PageGetContents(page);
	return fsmpage->fp_nodes[0];
}

/*
 * Searches for a slot with min. category. Returns slot number, or -1 if 
 * none found.
 *
 * The caller must hold at least a shared lock on the page, and this
 * function can unlock and lock the page again in exclusive mode if it
 * needs to be updated. exclusive_lock_held should be set to true if the
 * caller is already holding an exclusive lock, to avoid extra work.
 *
 * If advancenext is false, fp_next_slot is set to point to the returned
 * slot, and if it's true, to the slot next to the returned slot.
 */
int
fsm_search_avail(Buffer buf, uint8 minvalue, bool advancenext,
				 bool exclusive_lock_held)
{
	Page page = BufferGetPage(buf);
	FSMPage fsmpage = (FSMPage) PageGetContents(page);
	int nodeno;
	int target;
	uint16 slot;

 restart:
	/*
	 * Check the root first, and exit quickly if there's no page with
	 * enough free space
	 */
	if (fsmpage->fp_nodes[0] < minvalue)
		return -1;


	/* fp_next_slot is just a hint, so check that it's sane */
	target = fsmpage->fp_next_slot;
	if (target < 0 || target >= LeafNodesPerPage)
		target = 0;
	target += NonLeafNodesPerPage;

	/*
	 * Start the search from the target slot. At every step, move one
	 * node to the right, and climb up to the parent. Stop when we reach a
	 * node with enough free space. (note that moving to the right only
	 * makes a difference if we're on the right child of the parent)
	 *
	 * The idea is to graduall expand our "search triangle", that is, all
	 * nodes covered by the current node. In the beginning, just the target
	 * node is included, and more nodes to the right of the target node,
	 * taking wrap-around into account, is included at each step. Nodes are
	 * added to the search triangle in left-to-right order, starting from
	 * the target node. This ensures that we'll find the first suitable node
	 * to the right of the target node, and not some other node with enough
	 * free space.
	 *
	 * For example, consider this tree:
	 *
	 *         7
	 *     7       6
	 *   5   7   6   5
	 *  4 5 5 7 2 6 5 2
	 *              T
	 *
	 * Imagine that target node is the node indicated by the letter T, and
	 * we're searching for a node with value of 6 or higher. The search
	 * begins at T. At first iteration, we move to the right, and to the
	 * parent, arriving the rightmost 5. At the 2nd iteration, we move to the
	 * right, wrapping around, and climb up, arriving at the 7 at the 2nd
	 * level. 7 satisfies our search, so we descend down to the bottom,
	 * following the path of sevens.
	 */
	nodeno = target;
	while (nodeno > 0)
	{
		if (fsmpage->fp_nodes[nodeno] >= minvalue)
			break;
		
		/*
		 * Move to the right, wrapping around at the level if necessary, and
		 * climb up.
		 */
		nodeno = parentof(rightsibling(nodeno));
	}

	/*
	 * We're now at a node with enough free space, somewhere in the middle of
	 * the tree. Descend to the bottom, following a path with enough free
	 * space, preferring to move left if there's a choice.
	 */
	while (nodeno < NonLeafNodesPerPage)
	{
		int leftnodeno = leftchild(nodeno);
		int rightnodeno = leftnodeno + 1;
		bool leftok = (leftnodeno < NodesPerPage) &&
			(fsmpage->fp_nodes[leftnodeno] >= minvalue);
		bool rightok = (rightnodeno < NodesPerPage) &&
			(fsmpage->fp_nodes[rightnodeno] >= minvalue);

		if (leftok)
			nodeno = leftnodeno;
		else if (rightok)
			nodeno = rightnodeno;
		else
		{
			/*
			 * Oops. The parent node promised that either left or right
			 * child has enough space, but neither actually did. This can
			 * happen in case of a "torn page", IOW if we crashed earlier
			 * while writing the page to disk, and only part of the page
			 * made it to disk.
			 *
			 * Fix the corruption and restart.
			 */
			RelFileNode	rnode;
			ForkNumber	forknum;
			BlockNumber	blknum;

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
			MarkBufferDirty(buf);
			goto restart;
		}
	}

	/* We're now at the bottom level, at a node with enough space. */
	slot = nodeno - NonLeafNodesPerPage;

	/*
	 * Update the next slot pointer. Note that we do this even if we're only
	 * holding a shared lock, on the grounds that it's better to use a shared
	 * lock and get a garbled next pointer every now and then, than take the
	 * concurrency hit of an exlusive lock.
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
	FSMPage fsmpage = (FSMPage) PageGetContents(page);
	uint8 *ptr;
	bool changed = false;

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
	FSMPage fsmpage = (FSMPage) PageGetContents(page);
	bool	changed = false;
	int		nodeno;

	/*
	 * Start from the lowest non-leaflevel, at last node, working our way
	 * backwards, through all non-leaf nodes at all levels, up to the root.
	 */
	for (nodeno = NonLeafNodesPerPage - 1; nodeno >= 0; nodeno--)
	{
		int lchild = leftchild(nodeno);
		int rchild = lchild + 1;
		uint8 newvalue = 0;

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

