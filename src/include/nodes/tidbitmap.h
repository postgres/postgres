/*-------------------------------------------------------------------------
 *
 * tidbitmap.h
 *	  PostgreSQL tuple-id (TID) bitmap package
 *
 * This module provides bitmap data structures that are spiritually
 * similar to Bitmapsets, but are specially adapted to store sets of
 * tuple identifiers (TIDs), or ItemPointers.  In particular, the division
 * of an ItemPointer into BlockNumber and OffsetNumber is catered for.
 * Also, since we wish to be able to store very large tuple sets in
 * memory with this data structure, we support "lossy" storage, in which
 * we no longer remember individual tuple offsets on a page but only the
 * fact that a particular page needs to be visited.
 *
 *
 * Copyright (c) 2003-2025, PostgreSQL Global Development Group
 *
 * src/include/nodes/tidbitmap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIDBITMAP_H
#define TIDBITMAP_H

#include "storage/itemptr.h"
#include "utils/dsa.h"


/*
 * Actual bitmap representation is private to tidbitmap.c.  Callers can
 * do IsA(x, TIDBitmap) on it, but nothing else.
 */
typedef struct TIDBitmap TIDBitmap;

/* Likewise, TBMPrivateIterator is private */
typedef struct TBMPrivateIterator TBMPrivateIterator;
typedef struct TBMSharedIterator TBMSharedIterator;

/*
 * Callers with both private and shared implementations can use this unified
 * API.
 */
typedef struct TBMIterator
{
	bool		shared;
	union
	{
		TBMPrivateIterator *private_iterator;
		TBMSharedIterator *shared_iterator;
	}			i;
} TBMIterator;

/* Result structure for tbm_iterate */
typedef struct TBMIterateResult
{
	BlockNumber blockno;		/* page number containing tuples */
	int			ntuples;		/* -1 indicates lossy result */
	bool		recheck;		/* should the tuples be rechecked? */
	/* Note: recheck is always true if ntuples < 0 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} TBMIterateResult;

/* function prototypes in nodes/tidbitmap.c */

extern TIDBitmap *tbm_create(Size maxbytes, dsa_area *dsa);
extern void tbm_free(TIDBitmap *tbm);
extern void tbm_free_shared_area(dsa_area *dsa, dsa_pointer dp);

extern void tbm_add_tuples(TIDBitmap *tbm,
						   const ItemPointer tids, int ntids,
						   bool recheck);
extern void tbm_add_page(TIDBitmap *tbm, BlockNumber pageno);

extern void tbm_union(TIDBitmap *a, const TIDBitmap *b);
extern void tbm_intersect(TIDBitmap *a, const TIDBitmap *b);

extern bool tbm_is_empty(const TIDBitmap *tbm);

extern TBMPrivateIterator *tbm_begin_private_iterate(TIDBitmap *tbm);
extern dsa_pointer tbm_prepare_shared_iterate(TIDBitmap *tbm);
extern TBMIterateResult *tbm_private_iterate(TBMPrivateIterator *iterator);
extern TBMIterateResult *tbm_shared_iterate(TBMSharedIterator *iterator);
extern void tbm_end_private_iterate(TBMPrivateIterator *iterator);
extern void tbm_end_shared_iterate(TBMSharedIterator *iterator);
extern TBMSharedIterator *tbm_attach_shared_iterate(dsa_area *dsa,
													dsa_pointer dp);
extern int	tbm_calculate_entries(Size maxbytes);

extern TBMIterator tbm_begin_iterate(TIDBitmap *tbm,
									 dsa_area *dsa, dsa_pointer dsp);
extern void tbm_end_iterate(TBMIterator *iterator);

extern TBMIterateResult *tbm_iterate(TBMIterator *iterator);

static inline bool
tbm_exhausted(TBMIterator *iterator)
{
	/*
	 * It doesn't matter if we check the private or shared iterator here. If
	 * tbm_end_iterate() was called, they will be NULL
	 */
	return !iterator->i.private_iterator;
}

#endif							/* TIDBITMAP_H */
