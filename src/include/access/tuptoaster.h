/*-------------------------------------------------------------------------
 *
 * tuptoaster.h
 *	  POSTGRES definitions for external and compressed storage
 *	  of variable size attributes.
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Id: tuptoaster.h,v 1.16 2003/08/04 23:59:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPTOASTER_H
#define TUPTOASTER_H

#include "access/heapam.h"
#include "access/htup.h"
#include "access/tupmacs.h"
#include "utils/rel.h"


/*
 * This enables de-toasting of index entries.  Needed until VACUUM is
 * smart enough to rebuild indexes from scratch.
 */
#define TOAST_INDEX_HACK


/*
 * These symbols control toaster activation.  If a tuple is larger than
 * TOAST_TUPLE_THRESHOLD, we will try to toast it down to no more than
 * TOAST_TUPLE_TARGET bytes.  Both numbers include all tuple header and
 * alignment-padding overhead.
 *
 * The numbers need not be the same, though they currently are.
 */
#define TOAST_TUPLE_THRESHOLD	(MaxTupleSize / 4)

#define TOAST_TUPLE_TARGET		(MaxTupleSize / 4)

/*
 * If an index value is larger than TOAST_INDEX_TARGET, we will try to
 * compress it (we can't move it out-of-line, however).  Note that this
 * number is per-datum, not per-tuple, for simplicity in index_formtuple().
 */
#define TOAST_INDEX_TARGET		(MaxTupleSize / 16)

/*
 * When we store an oversize datum externally, we divide it into chunks
 * containing at most TOAST_MAX_CHUNK_SIZE data bytes.	This number *must*
 * be small enough that the completed toast-table tuple (including the
 * ID and sequence fields and all overhead) is no more than MaxTupleSize
 * bytes.  It *should* be small enough to make toast-table tuples no more
 * than TOAST_TUPLE_THRESHOLD bytes, else heapam.c will uselessly invoke
 * the toaster on toast-table tuples.
 *
 * NB: you cannot change this value without forcing initdb, at least not
 * if your DB contains any multi-chunk toasted values.
 */
#define TOAST_MAX_CHUNK_SIZE	(TOAST_TUPLE_THRESHOLD -			\
			MAXALIGN(												\
				MAXALIGN(offsetof(HeapTupleHeaderData, t_bits)) +	\
				sizeof(Oid) +										\
				sizeof(int32) +										\
				VARHDRSZ))


/* ----------
 * heap_tuple_toast_attrs() -
 *
 *		Called by heap_insert(), heap_update() and heap_delete().
 *		Outdates any no-longer-needed toast entries referenced
 *		by oldtup and creates new ones until newtup is no more than
 *		TOAST_TUPLE_TARGET (or we run out of toastable values).
 *		Possibly modifies newtup by replacing the t_data part!
 *
 *		oldtup is NULL if insert, newtup is NULL if delete.
 * ----------
 */
extern void heap_tuple_toast_attrs(Relation rel,
					   HeapTuple newtup, HeapTuple oldtup);

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

#endif   /* TUPTOASTER_H */
