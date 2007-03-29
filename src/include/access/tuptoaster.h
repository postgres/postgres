/*-------------------------------------------------------------------------
 *
 * tuptoaster.h
 *	  POSTGRES definitions for external and compressed storage
 *	  of variable size attributes.
 *
 * Copyright (c) 2000-2007, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/access/tuptoaster.h,v 1.33 2007/03/29 00:15:39 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPTOASTER_H
#define TUPTOASTER_H

#include "access/htup.h"
#include "storage/bufpage.h"

/*
 * This enables de-toasting of index entries.  Needed until VACUUM is
 * smart enough to rebuild indexes from scratch.
 */
#define TOAST_INDEX_HACK


/*
 * These symbols control toaster activation.  If a tuple is larger than
 * TOAST_TUPLE_THRESHOLD, we will try to toast it down to no more than
 * TOAST_TUPLE_TARGET bytes.  Both numbers include all tuple header overhead
 * and between-fields alignment padding, but we do *not* consider any
 * end-of-tuple alignment padding; hence the values can be compared directly
 * to a tuple's t_len field.  We choose TOAST_TUPLE_THRESHOLD with the
 * knowledge that toast-table tuples will be exactly that size, and we'd
 * like to fit four of them per page with minimal space wastage.
 *
 * The numbers need not be the same, though they currently are.
 *
 * Note: sizeof(PageHeaderData) includes the first ItemId, but we have
 * to allow for 3 more, if we want to fit 4 tuples on a page.
 */
#define TOAST_TUPLE_THRESHOLD	\
	MAXALIGN_DOWN((BLCKSZ - \
				   MAXALIGN(sizeof(PageHeaderData) + 3 * sizeof(ItemIdData))) \
				  / 4)

#define TOAST_TUPLE_TARGET		TOAST_TUPLE_THRESHOLD

/*
 * If an index value is larger than TOAST_INDEX_TARGET, we will try to
 * compress it (we can't move it out-of-line, however).  Note that this
 * number is per-datum, not per-tuple, for simplicity in index_form_tuple().
 */
#define TOAST_INDEX_TARGET		(MaxHeapTupleSize / 16)

/*
 * When we store an oversize datum externally, we divide it into chunks
 * containing at most TOAST_MAX_CHUNK_SIZE data bytes.	This number *must*
 * be small enough that the completed toast-table tuple (including the
 * ID and sequence fields and all overhead) is no more than MaxHeapTupleSize
 * bytes.  It *should* be small enough to make toast-table tuples no more
 * than TOAST_TUPLE_THRESHOLD bytes, else heapam.c will uselessly invoke
 * the toaster on toast-table tuples.  The current coding ensures that the
 * maximum tuple length is exactly TOAST_TUPLE_THRESHOLD bytes.
 *
 * NB: you cannot change this value without forcing initdb, at least not
 * if your DB contains any multi-chunk toasted values.
 */
#define TOAST_MAX_CHUNK_SIZE	(TOAST_TUPLE_THRESHOLD -			\
				MAXALIGN(offsetof(HeapTupleHeaderData, t_bits)) -	\
				sizeof(Oid) -										\
				sizeof(int32) -										\
				VARHDRSZ)


/* ----------
 * toast_insert_or_update -
 *
 *	Called by heap_insert() and heap_update().
 * ----------
 */
extern HeapTuple toast_insert_or_update(Relation rel,
										HeapTuple newtup, HeapTuple oldtup,
										bool use_wal, bool use_fsm);

/* ----------
 * toast_delete -
 *
 *	Called by heap_delete().
 * ----------
 */
extern void toast_delete(Relation rel, HeapTuple oldtup);

/* ----------
 * heap_tuple_fetch_attr() -
 *
 *		Fetches an external stored attribute from the toast
 *		relation. Does NOT decompress it, if stored external
 *		in compressed format.
 * ----------
 */
extern varattrib *heap_tuple_fetch_attr(varattrib *attr);

/* ----------
 * heap_tuple_untoast_attr() -
 *
 *		Fully detoasts one attribute, fetching and/or decompressing
 *		it as needed.
 * ----------
 */
extern varattrib *heap_tuple_untoast_attr(varattrib *attr);

/* ----------
 * heap_tuple_untoast_attr_slice() -
 *
 *		Fetches only the specified portion of an attribute.
 *		(Handles all cases for attribute storage)
 * ----------
 */
extern varattrib *heap_tuple_untoast_attr_slice(varattrib *attr,
							  int32 sliceoffset,
							  int32 slicelength);

/* ----------
 * toast_flatten_tuple_attribute -
 *
 *	If a Datum is of composite type, "flatten" it to contain no toasted fields.
 *	This must be invoked on any potentially-composite field that is to be
 *	inserted into a tuple.	Doing this preserves the invariant that toasting
 *	goes only one level deep in a tuple.
 * ----------
 */
extern Datum toast_flatten_tuple_attribute(Datum value,
							  Oid typeId, int32 typeMod);

/* ----------
 * toast_compress_datum -
 *
 *	Create a compressed version of a varlena datum, if possible
 * ----------
 */
extern Datum toast_compress_datum(Datum value);

/* ----------
 * toast_raw_datum_size -
 *
 *	Return the raw (detoasted) size of a varlena datum
 * ----------
 */
extern Size toast_raw_datum_size(Datum value);

/* ----------
 * toast_datum_size -
 *
 *	Return the storage size of a varlena datum
 * ----------
 */
extern Size toast_datum_size(Datum value);

#endif   /* TUPTOASTER_H */
