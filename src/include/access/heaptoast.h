/*-------------------------------------------------------------------------
 *
 * heaptoast.h
 *	  Heap-specific definitions for external and compressed storage
 *	  of variable size attributes.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/include/access/heaptoast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPTOAST_H
#define HEAPTOAST_H

#include "access/htup_details.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"

/*
 * Find the maximum size of a tuple if there are to be N tuples per page.
 */
#define MaximumBytesPerTuple(tuplesPerPage) \
	MAXALIGN_DOWN((BLCKSZ - \
				   MAXALIGN(SizeOfPageHeaderData + (tuplesPerPage) * sizeof(ItemIdData))) \
				  / (tuplesPerPage))

/*
 * These symbols control toaster activation.  If a tuple is larger than
 * TOAST_TUPLE_THRESHOLD, we will try to toast it down to no more than
 * TOAST_TUPLE_TARGET bytes through compressing compressible fields and
 * moving EXTENDED and EXTERNAL data out-of-line.
 *
 * The numbers need not be the same, though they currently are.  It doesn't
 * make sense for TARGET to exceed THRESHOLD, but it could be useful to make
 * it be smaller.
 *
 * Currently we choose both values to match the largest tuple size for which
 * TOAST_TUPLES_PER_PAGE tuples can fit on a heap page.
 *
 * XXX while these can be modified without initdb, some thought needs to be
 * given to needs_toast_table() in toasting.c before unleashing random
 * changes.  Also see LOBLKSIZE in large_object.h, which can *not* be
 * changed without initdb.
 */
#define TOAST_TUPLES_PER_PAGE	4

#define TOAST_TUPLE_THRESHOLD	MaximumBytesPerTuple(TOAST_TUPLES_PER_PAGE)

#define TOAST_TUPLE_TARGET		TOAST_TUPLE_THRESHOLD

/*
 * The code will also consider moving MAIN data out-of-line, but only as a
 * last resort if the previous steps haven't reached the target tuple size.
 * In this phase we use a different target size, currently equal to the
 * largest tuple that will fit on a heap page.  This is reasonable since
 * the user has told us to keep the data in-line if at all possible.
 */
#define TOAST_TUPLES_PER_PAGE_MAIN	1

#define TOAST_TUPLE_TARGET_MAIN MaximumBytesPerTuple(TOAST_TUPLES_PER_PAGE_MAIN)

/*
 * If an index value is larger than TOAST_INDEX_TARGET, we will try to
 * compress it (we can't move it out-of-line, however).  Note that this
 * number is per-datum, not per-tuple, for simplicity in index_form_tuple().
 */
#define TOAST_INDEX_TARGET		(MaxHeapTupleSize / 16)

/*
 * When we store an oversize datum externally, we divide it into chunks
 * containing at most TOAST_MAX_CHUNK_SIZE data bytes.  This number *must*
 * be small enough that the completed toast-table tuple (including the
 * ID and sequence fields and all overhead) will fit on a page.
 * The coding here sets the size on the theory that we want to fit
 * EXTERN_TUPLES_PER_PAGE tuples of maximum size onto a page.
 *
 * NB: Changing TOAST_MAX_CHUNK_SIZE requires an initdb.
 */
#define EXTERN_TUPLES_PER_PAGE	4	/* tweak only this */

#define EXTERN_TUPLE_MAX_SIZE	MaximumBytesPerTuple(EXTERN_TUPLES_PER_PAGE)

#define TOAST_MAX_CHUNK_SIZE	\
	(EXTERN_TUPLE_MAX_SIZE -							\
	 MAXALIGN(SizeofHeapTupleHeader) -					\
	 sizeof(Oid) -										\
	 sizeof(int32) -									\
	 VARHDRSZ)

/* ----------
 * heap_toast_insert_or_update -
 *
 *	Called by heap_insert() and heap_update().
 * ----------
 */
extern HeapTuple heap_toast_insert_or_update(Relation rel, HeapTuple newtup,
											 HeapTuple oldtup, int options);

/* ----------
 * heap_toast_delete -
 *
 *	Called by heap_delete().
 * ----------
 */
extern void heap_toast_delete(Relation rel, HeapTuple oldtup,
							  bool is_speculative);

/* ----------
 * toast_flatten_tuple -
 *
 *	"Flatten" a tuple to contain no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 * ----------
 */
extern HeapTuple toast_flatten_tuple(HeapTuple tup, TupleDesc tupleDesc);

/* ----------
 * toast_flatten_tuple_to_datum -
 *
 *	"Flatten" a tuple containing out-of-line toasted fields into a Datum.
 * ----------
 */
extern Datum toast_flatten_tuple_to_datum(HeapTupleHeader tup,
										  uint32 tup_len,
										  TupleDesc tupleDesc);

/* ----------
 * toast_build_flattened_tuple -
 *
 *	Build a tuple containing no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 * ----------
 */
extern HeapTuple toast_build_flattened_tuple(TupleDesc tupleDesc,
											 Datum *values,
											 bool *isnull);

/* ----------
 * heap_fetch_toast_slice
 *
 *	Fetch a slice from a toast value stored in a heap table.
 * ----------
 */
extern void heap_fetch_toast_slice(Relation toastrel, Oid valueid,
								   int32 attrsize, int32 sliceoffset,
								   int32 slicelength, struct varlena *result);

#endif							/* HEAPTOAST_H */
