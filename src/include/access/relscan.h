/*-------------------------------------------------------------------------
 *
 * relscan.h
 *	  POSTGRES relation scan descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relscan.h,v 1.32 2003/08/04 02:40:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELSCAN_H
#define RELSCAN_H

#include "utils/tqual.h"


typedef struct HeapScanDescData
{
	/* scan parameters */
	Relation	rs_rd;			/* heap relation descriptor */
	Snapshot	rs_snapshot;	/* snapshot to see */
	int			rs_nkeys;		/* number of scan keys */
	ScanKey		rs_key;			/* array of scan key descriptors */

	/* scan current state */
	HeapTupleData rs_ctup;		/* current tuple in scan, if any */
	Buffer		rs_cbuf;		/* current buffer in scan, if any */
	/* NB: if rs_cbuf is not InvalidBuffer, we hold a pin on that buffer */
	ItemPointerData rs_mctid;	/* marked scan position, if any */

	PgStat_Info rs_pgstat_info; /* statistics collector hook */
} HeapScanDescData;

typedef HeapScanDescData *HeapScanDesc;


typedef struct IndexScanDescData
{
	/* scan parameters */
	Relation	heapRelation;	/* heap relation descriptor, or NULL */
	Relation	indexRelation;	/* index relation descriptor */
	Snapshot	xs_snapshot;	/* snapshot to see */
	int			numberOfKeys;	/* number of scan keys */
	ScanKey		keyData;		/* array of scan key descriptors */

	/* signaling to index AM about killing index tuples */
	bool		kill_prior_tuple;		/* last-returned tuple is dead */
	bool		ignore_killed_tuples;	/* do not return killed entries */

	/* set by index AM if scan keys satisfy index's uniqueness constraint */
	bool		keys_are_unique;

	/* scan current state */
	bool		got_tuple;		/* true after successful index_getnext */
	void	   *opaque;			/* access-method-specific info */
	ItemPointerData currentItemData;	/* current index pointer */
	ItemPointerData currentMarkData;	/* marked position, if any */

	/*
	 * xs_ctup/xs_cbuf are valid after a successful index_getnext. After
	 * index_getnext_indexitem, xs_ctup.t_self contains the heap tuple TID
	 * from the index entry, but its other fields are not valid.
	 */
	HeapTupleData xs_ctup;		/* current heap tuple, if any */
	Buffer		xs_cbuf;		/* current heap buffer in scan, if any */
	/* NB: if xs_cbuf is not InvalidBuffer, we hold a pin on that buffer */

	FmgrInfo	fn_getnext;		/* cached lookup info for AM's getnext fn */

	/*
	 * If keys_are_unique and got_tuple are both true, we stop calling the
	 * index AM; it is then necessary for index_getnext to keep track of
	 * the logical scan position for itself.  It does that using
	 * unique_tuple_pos: -1 = before row, 0 = on row, +1 = after row.
	 */
	int			unique_tuple_pos;		/* logical position */
	int			unique_tuple_mark;		/* logical marked position */

	PgStat_Info xs_pgstat_info; /* statistics collector hook */
} IndexScanDescData;

typedef IndexScanDescData *IndexScanDesc;


/* ----------------
 *		IndexScanDescPtr is used in the executor where we have to
 *		keep track of several index scans when using several indices
 *		- cim 9/10/89
 * ----------------
 */
typedef IndexScanDesc *IndexScanDescPtr;

/*
 * HeapScanIsValid
 *		True iff the heap scan is valid.
 */
#define HeapScanIsValid(scan) PointerIsValid(scan)

/*
 * IndexScanIsValid
 *		True iff the index scan is valid.
 */
#define IndexScanIsValid(scan) PointerIsValid(scan)

#endif   /* RELSCAN_H */
