/*-------------------------------------------------------------------------
 *
 * tuptable.h
 *	  tuple table support stuff
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/tuptable.h,v 1.27 2005/03/14 04:41:13 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPTABLE_H
#define TUPTABLE_H

#include "access/htup.h"


/*
 * The executor stores pointers to tuples in a "tuple table"
 * which is composed of TupleTableSlots.  Sometimes the tuples
 * are pointers to buffer pages, while others are pointers to
 * palloc'ed memory; the shouldFree variable tells us whether
 * we may call pfree() on a tuple.  When shouldFree is true,
 * the tuple is "owned" by the TupleTableSlot and should be
 * freed when the slot's reference to the tuple is dropped.
 *
 * shouldFreeDesc is similar to shouldFree: if it's true, then the
 * tupleDescriptor is "owned" by the TupleTableSlot and should be
 * freed when the slot's reference to the descriptor is dropped.
 *
 * If buffer is not InvalidBuffer, then the slot is holding a pin
 * on the indicated buffer page; drop the pin when we release the
 * slot's reference to that buffer.  (shouldFree should always be
 * false in such a case, since presumably val is pointing at the
 * buffer page.)
 *
 * The slot_getattr() routine allows extraction of attribute values from
 * a TupleTableSlot's current tuple.  It is equivalent to heap_getattr()
 * except that it can optimize fetching of multiple values more efficiently.
 * The cache_xxx fields of TupleTableSlot are support for slot_getattr().
 */
typedef struct TupleTableSlot
{
	NodeTag		type;			/* vestigial ... allows IsA tests */
	HeapTuple	val;			/* current tuple, or NULL if none */
	TupleDesc	ttc_tupleDescriptor;	/* tuple's descriptor */
	bool		ttc_shouldFree;			/* should pfree tuple? */
	bool		ttc_shouldFreeDesc;		/* should pfree descriptor? */
	Buffer		ttc_buffer;		/* tuple's buffer, or InvalidBuffer */
	MemoryContext ttc_mcxt;		/* slot itself is in this context */
	Datum	   *cache_values;	/* currently extracted values */
	int			cache_natts;	/* # of valid values in cache_values */
	bool		cache_slow;		/* saved state for slot_getattr */
	long		cache_off;		/* saved state for slot_getattr */
} TupleTableSlot;

/*
 * Tuple table data structure: an array of TupleTableSlots.
 */
typedef struct TupleTableData
{
	int			size;			/* size of the table (number of slots) */
	int			next;			/* next available slot number */
	TupleTableSlot array[1];	/* VARIABLE LENGTH ARRAY - must be last */
} TupleTableData;				/* VARIABLE LENGTH STRUCT */

typedef TupleTableData *TupleTable;


/* in access/common/heaptuple.c */
extern Datum slot_getattr(TupleTableSlot *slot, int attnum, bool *isnull);

#endif   /* TUPTABLE_H */
