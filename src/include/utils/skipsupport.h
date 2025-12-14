/*-------------------------------------------------------------------------
 *
 * skipsupport.h
 *	  Support routines for B-Tree skip scan.
 *
 * B-Tree operator classes for discrete types can optionally provide a support
 * function for skipping.  This is used during skip scans.
 *
 * A B-tree operator class that implements skip support provides B-tree index
 * scans with a way of enumerating and iterating through every possible value
 * from the domain of indexable values.  This gives scans a way to determine
 * the next value in line for a given skip array/scan key/skipped attribute.
 * Scans request the next (or previous) value whenever they run out of tuples
 * matching the skip array's current element value.  The next (or previous)
 * value can be used to relocate the scan; it is applied in combination with
 * at least one additional lower-order non-skip key, taken from the query.
 *
 * Skip support is used by discrete type (e.g., integer and date) opclasses.
 * Indexes with an attribute whose input opclass is of one of these types tend
 * to store adjacent values in adjoining groups of index tuples.  Each time a
 * skip scan with skip support successfully guesses that the next value in the
 * index (for a given skipped column) is indeed the value that skip support
 * just incremented its skip array to, it will have saved the scan some work.
 * The scan will have avoided an index probe that directly finds the next
 * value that appears in the index.  (When skip support guesses wrong, then it
 * won't have saved any work, but it also won't have added any useless work.
 * The failed attempt to locate exactly-matching index tuples acts just like
 * an explicit probe would; it'll still find the index's true next value.)
 *
 * It usually isn't feasible to implement skip support for an opclass whose
 * input type is continuous.  The B-Tree code falls back on next-key sentinel
 * values for any opclass that doesn't provide its own skip support function.
 * This isn't really an implementation restriction; there is no benefit to
 * providing skip support for an opclass where guessing that the next indexed
 * value is the next possible indexable value never (or hardly ever) works out.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/skipsupport.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SKIPSUPPORT_H
#define SKIPSUPPORT_H

#include "utils/relcache.h"

typedef struct SkipSupportData *SkipSupport;
typedef Datum (*SkipSupportIncDec) (Relation rel,
									Datum existing,
									bool *overflow);

/*
 * State/callbacks used by skip arrays to procedurally generate elements.
 *
 * A BTSKIPSUPPORT_PROC function must set each and every field when called
 * (there are no optional fields).
 */
typedef struct SkipSupportData
{
	/*
	 * low_elem and high_elem must be set with the lowest and highest possible
	 * values from the domain of indexable values (assuming ascending order)
	 */
	Datum		low_elem;		/* lowest sorting/leftmost non-NULL value */
	Datum		high_elem;		/* highest sorting/rightmost non-NULL value */

	/*
	 * Decrement/increment functions.
	 *
	 * Returns a decremented/incremented copy of caller's existing datum,
	 * allocated in caller's memory context (for pass-by-reference types).
	 * It's not okay for these functions to leak any memory.
	 *
	 * When the decrement function (or increment function) is called with a
	 * value that already matches low_elem (or high_elem), function must set
	 * the *overflow argument.  The return value is treated as undefined by
	 * the B-Tree code; it shouldn't need to be (and won't be) pfree'd.
	 *
	 * The B-Tree code's "existing" datum argument is often just a straight
	 * copy of a value from an index tuple.  Operator classes must accept
	 * every possible representational variation within the underlying type.
	 * On the other hand, opclasses are _not_ required to preserve information
	 * that doesn't affect how datums are sorted (e.g., skip support for a
	 * fixed precision numeric type needn't preserve datum display scale).
	 * Operator class decrement/increment functions will never be called with
	 * a NULL "existing" argument, either.
	 */
	SkipSupportIncDec decrement;
	SkipSupportIncDec increment;
} SkipSupportData;

extern SkipSupport PrepareSkipSupportFromOpclass(Oid opfamily, Oid opcintype,
												 bool reverse);

#endif							/* SKIPSUPPORT_H */
