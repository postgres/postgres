/*-------------------------------------------------------------------------
 *
 * tuptable.h--
 *	  tuple table support stuff
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tuptable.h,v 1.8 1998/09/01 04:36:13 momjian Exp $
 *
 * NOTES
 *	  The tuple table interface is getting pretty ugly.
 *	  It should be redesigned soon.
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPTABLE_H
#define TUPTABLE_H

#include <storage/buf.h>
#include <access/tupdesc.h>
#include <access/htup.h>

/* ----------------
 *		Note:  the executor tuple table is managed and manipulated by special
 *		code and macros in executor/execTuples.c and tupTable.h
 *
 *		TupleTableSlot information
 *
 *			shouldFree			boolean - should we call pfree() on tuple
 *			descIsNew			boolean - true when tupleDescriptor changes
 *			tupleDescriptor		type information kept regarding the tuple data
 *			buffer				the buffer for tuples pointing to disk pages
 *
 *		The executor stores pointers to tuples in a ``tuple table''
 *		which is composed of TupleTableSlot's.  Some of the tuples
 *		are pointers to buffer pages and others are pointers to
 *		palloc'ed memory and the shouldFree variable tells us when
 *		we may call pfree() on a tuple.  -cim 9/23/90
 *
 *		In the implementation of nested-dot queries such as
 *		"retrieve (EMP.hobbies.all)", a single scan may return tuples
 *		of many types, so now we return pointers to tuple descriptors
 *		along with tuples returned via the tuple table.  -cim 1/18/90
 * ----------------
 */
typedef struct TupleTableSlot
{
	NodeTag		type;
	HeapTuple	val;
	bool		ttc_shouldFree;
	bool		ttc_descIsNew;
	TupleDesc	ttc_tupleDescriptor;
	Buffer		ttc_buffer;
	int			ttc_whichplan;
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

/*
  tuple table macros are all excised from the system now
  see executor.h for decls of functions defined in execTuples.c

  - jolly
*/

#endif	 /* TUPTABLE_H */
