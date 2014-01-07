/*-------------------------------------------------------------------------
 *
 * fsm_internal.h
 *	  internal functions for free space map
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fsm_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FSM_INTERNALS_H
#define FSM_INTERNALS_H

#include "storage/buf.h"
#include "storage/bufpage.h"

/*
 * Structure of a FSM page. See src/backend/storage/freespace/README for
 * details.
 */
typedef struct
{
	/*
	 * fsm_search_avail() tries to spread the load of multiple backends by
	 * returning different pages to different backends in a round-robin
	 * fashion. fp_next_slot points to the next slot to be returned (assuming
	 * there's enough space on it for the request). It's defined as an int,
	 * because it's updated without an exclusive lock. uint16 would be more
	 * appropriate, but int is more likely to be atomically
	 * fetchable/storable.
	 */
	int			fp_next_slot;

	/*
	 * fp_nodes contains the binary tree, stored in array. The first
	 * NonLeafNodesPerPage elements are upper nodes, and the following
	 * LeafNodesPerPage elements are leaf nodes. Unused nodes are zero.
	 */
	uint8		fp_nodes[1];
} FSMPageData;

typedef FSMPageData *FSMPage;

/*
 * Number of non-leaf and leaf nodes, and nodes in total, on an FSM page.
 * These definitions are internal to fsmpage.c.
 */
#define NodesPerPage (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
					  offsetof(FSMPageData, fp_nodes))

#define NonLeafNodesPerPage (BLCKSZ / 2 - 1)
#define LeafNodesPerPage (NodesPerPage - NonLeafNodesPerPage)

/*
 * Number of FSM "slots" on a FSM page. This is what should be used
 * outside fsmpage.c.
 */
#define SlotsPerFSMPage LeafNodesPerPage

/* Prototypes for functions in fsmpage.c */
extern int fsm_search_avail(Buffer buf, uint8 min_cat, bool advancenext,
				 bool exclusive_lock_held);
extern uint8 fsm_get_avail(Page page, int slot);
extern uint8 fsm_get_max_avail(Page page);
extern bool fsm_set_avail(Page page, int slot, uint8 value);
extern bool fsm_truncate_avail(Page page, int nslots);
extern bool fsm_rebuild_page(Page page);

#endif   /* FSM_INTERNALS_H */
