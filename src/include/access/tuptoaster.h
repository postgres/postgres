/*-------------------------------------------------------------------------
 *
 * tuptoaster.h
 *	  POSTGRES definitions for external and compressed storage
 *	  of variable size attributes.
 *
 * Copyright (c) 2000, PostgreSQL Development Team
 *
 * $Id: tuptoaster.h,v 1.6 2000/07/21 10:31:31 wieck Exp $
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

/*
 * DO NOT ENABLE THIS
 * until we have crash safe file versioning and you've
 * changed VACUUM to recreate indices that use possibly
 * toasted values. 2000/07/20 Jan
 */
#undef TOAST_INDICES


#define	TOAST_MAX_CHUNK_SIZE	((MaxTupleSize -							\
				MAXALIGN(												\
					MAXALIGN(offsetof(HeapTupleHeaderData, t_bits)) +	\
					MAXALIGN(sizeof(Oid)) +								\
					MAXALIGN(sizeof(int32)) +							\
					MAXALIGN(VARHDRSZ))) / 4)


#ifdef TOAST_INDICES
extern void heap_tuple_toast_attrs(Relation rel,
				HeapTuple newtup, HeapTuple oldtup);
#else
extern void heap_tuple_toast_attrs(Relation rel,
				HeapTuple newtup, HeapTuple oldtup, 
				HeapTupleHeader *plaintdata, int32 *plaintlen);
#endif

extern varattrib *heap_tuple_untoast_attr(varattrib * attr);

#endif	 /* TUPLE_TOASTER_ACTIVE */


#endif	 /* TUPTOASTER_H */
