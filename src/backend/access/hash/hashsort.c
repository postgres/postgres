/*-------------------------------------------------------------------------
 *
 * hashsort.c
 *		Sort tuples for insertion into a new hash index.
 *
 * When building a very large hash index, we pre-sort the tuples by bucket
 * number to improve locality of access to the index, and thereby avoid
 * thrashing.  We use tuplesort.c to sort the given index tuples into order.
 *
 * Note: if the number of rows in the table has been underestimated,
 * bucket splits may occur during the index build.  In that case we'd
 * be inserting into two or more buckets for each possible masked-off
 * hash code value.  That's no big problem though, since we'll still have
 * plenty of locality of access.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashsort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "miscadmin.h"
#include "utils/tuplesort.h"


/*
 * Status record for spooling/sorting phase.
 */
struct HSpool
{
	Tuplesortstate *sortstate;	/* state data for tuplesort.c */
	Relation	index;
	uint32		hash_mask;		/* bitmask for hash codes */
};


/*
 * create and initialize a spool structure
 */
HSpool *
_h_spoolinit(Relation heap, Relation index, uint32 num_buckets)
{
	HSpool	   *hspool = (HSpool *) palloc0(sizeof(HSpool));

	hspool->index = index;

	/*
	 * Determine the bitmask for hash code values.  Since there are currently
	 * num_buckets buckets in the index, the appropriate mask can be computed
	 * as follows.
	 *
	 * Note: at present, the passed-in num_buckets is always a power of 2, so
	 * we could just compute num_buckets - 1.  We prefer not to assume that
	 * here, though.
	 */
	hspool->hash_mask = (((uint32) 1) << _hash_log2(num_buckets)) - 1;

	/*
	 * We size the sort area as maintenance_work_mem rather than work_mem to
	 * speed index creation.  This should be OK since a single backend can't
	 * run multiple index creations in parallel.
	 */
	hspool->sortstate = tuplesort_begin_index_hash(heap,
												   index,
												   hspool->hash_mask,
												   maintenance_work_mem,
												   false);

	return hspool;
}

/*
 * clean up a spool structure and its substructures.
 */
void
_h_spooldestroy(HSpool *hspool)
{
	tuplesort_end(hspool->sortstate);
	pfree(hspool);
}

/*
 * spool an index entry into the sort file.
 */
void
_h_spool(HSpool *hspool, ItemPointer self, Datum *values, bool *isnull)
{
	tuplesort_putindextuplevalues(hspool->sortstate, hspool->index,
								  self, values, isnull);
}

/*
 * given a spool loaded by successive calls to _h_spool,
 * create an entire index.
 */
void
_h_indexbuild(HSpool *hspool)
{
	IndexTuple	itup;
	bool		should_free;
#ifdef USE_ASSERT_CHECKING
	uint32		hashkey = 0;
#endif

	tuplesort_performsort(hspool->sortstate);

	while ((itup = tuplesort_getindextuple(hspool->sortstate,
										   true, &should_free)) != NULL)
	{
		/*
		 * Technically, it isn't critical that hash keys be found in sorted
		 * order, since this sorting is only used to increase locality of
		 * access as a performance optimization.  It still seems like a good
		 * idea to test tuplesort.c's handling of hash index tuple sorts
		 * through an assertion, though.
		 */
#ifdef USE_ASSERT_CHECKING
		uint32		lasthashkey = hashkey;

		hashkey = _hash_get_indextuple_hashkey(itup) & hspool->hash_mask;
		Assert(hashkey >= lasthashkey);
#endif

		_hash_doinsert(hspool->index, itup);
		if (should_free)
			pfree(itup);
	}
}
