/*-------------------------------------------------------------------------
 *
 * tuptable.h
 *	  tuple table support stuff
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tuptable.h,v 1.23 2003/08/04 02:40:13 momjian Exp $
 *
 * NOTES
 *	  The tuple table interface is getting pretty ugly.
 *	  It should be redesigned soon.
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPTABLE_H
#define TUPTABLE_H

#include "access/htup.h"

/* ----------------
 *		The executor tuple table is managed and manipulated by special
 *		code in executor/execTuples.c.
 *
 *		TupleTableSlot information
 *
 *			val					current tuple, or NULL if no tuple
 *			shouldFree			boolean - should we pfree() tuple
 *			descIsNew			boolean - true when tupleDescriptor changes
 *			tupleDescriptor		type information for the tuple data
 *			shouldFreeDesc		boolean - should we free tupleDescriptor
 *			buffer				the buffer for tuples pointing to disk pages
 *
 *		The executor stores pointers to tuples in a ``tuple table''
 *		which is composed of TupleTableSlots.  Sometimes the tuples
 *		are pointers to buffer pages, while others are pointers to
 *		palloc'ed memory; the shouldFree variable tells us when
 *		we may call pfree() on a tuple.  -cim 9/23/90
 *
 *		If buffer is not InvalidBuffer, then the slot is holding a pin
 *		on the indicated buffer page; drop the pin when we release the
 *		slot's reference to that buffer.
 *
 *		In the implementation of nested-dot queries such as
 *		"retrieve (EMP.hobbies.all)", a single scan may return tuples
 *		of many types, so now we return pointers to tuple descriptors
 *		along with tuples returned via the tuple table.  -cim 1/18/90
 *
 *		shouldFreeDesc is similar to shouldFree: if it's true, then the
 *		tupleDescriptor is "owned" by the TupleTableSlot and should be
 *		freed when the slot's reference to the descriptor is dropped.
 *
 *		See executor.h for decls of functions defined in execTuples.c
 *		-jolly
 *
 * ----------------
 */
typedef struct TupleTableSlot
{
	NodeTag		type;
	HeapTuple	val;
	bool		ttc_shouldFree;
	bool		ttc_descIsNew;
	bool		ttc_shouldFreeDesc;
	TupleDesc	ttc_tupleDescriptor;
	Buffer		ttc_buffer;
} TupleTableSlot;

/* ----------------
 *		tuple table data structure
 * ----------------
 */
typedef struct TupleTableData
{
	int			size;			/* size of the table */
	int			next;			/* next available slot number */
	TupleTableSlot *array;		/* array of TupleTableSlot's */
} TupleTableData;

typedef TupleTableData *TupleTable;

#endif   /* TUPTABLE_H */
