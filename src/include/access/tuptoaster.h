/*-------------------------------------------------------------------------
 *
 * tuptoaster.h
 *	  POSTGRES definitions for external and compressed storage
 *	  of variable size attributes.
 *
 * Copyright (c) 2000, PostgreSQL Development Team
 *
 * $Id: tuptoaster.h,v 1.7 2000/07/22 11:18:47 wieck Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPTOASTER_H
#define TUPTOASTER_H

#ifdef TUPLE_TOASTER_ACTIVE

#include "access/heapam.h"
#include "access/htup.h"
#include "access/tupmacs.h"
#include "utils/rel.h"

#define TOAST_INDEX_HACK


#define	TOAST_MAX_CHUNK_SIZE	((MaxTupleSize -							\
				MAXALIGN(												\
					MAXALIGN(offsetof(HeapTupleHeaderData, t_bits)) +	\
					MAXALIGN(sizeof(Oid)) +								\
					MAXALIGN(sizeof(int32)) +							\
					MAXALIGN(VARHDRSZ))) / 4)


/* ----------
 * heap_tuple_toast_attrs() -
 *
 *		Called by heap_insert(), heap_update() and heap_delete().
 *		Outdates not any longer needed toast entries referenced
 *		by oldtup and creates new ones until newtup is smaller
 *		that ~2K (or running out of toastable values).
 *		Possibly modifies newtup by replacing the t_data part!
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
extern varattrib *heap_tuple_fetch_attr(varattrib * attr);

/* ----------
 * heap_tuple_untoast_attr() -
 *
 *		Fully detoasts one attribute, fetching and/or decompressing
 *		it as needed.
 * ----------
 */
extern varattrib *heap_tuple_untoast_attr(varattrib * attr);

#endif	 /* TUPLE_TOASTER_ACTIVE */


#endif	 /* TUPTOASTER_H */
