/*-------------------------------------------------------------------------
 *
 * relscan.h
 *	  POSTGRES relation scan descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/relscan.h,v 1.54 2007/05/30 20:12:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELSCAN_H
#define RELSCAN_H

#include "access/skey.h"
#include "storage/bufpage.h"
#include "utils/tqual.h"


typedef struct HeapScanDescData
{
	/* scan parameters */
	Relation	rs_rd;			/* heap relation descriptor */
	Snapshot	rs_snapshot;	/* snapshot to see */
	int			rs_nkeys;		/* number of scan keys */
	ScanKey		rs_key;			/* array of scan key descriptors */
	BlockNumber rs_nblocks;		/* number of blocks to scan */
	BufferAccessStrategy rs_strategy;	/* access strategy for reads */
	bool		rs_pageatatime; /* verify visibility page-at-a-time? */

	/* scan current state */
	bool		rs_inited;		/* false = scan not init'd yet */
	HeapTupleData rs_ctup;		/* current tuple in scan, if any */
	BlockNumber rs_cblock;		/* current block # in scan, if any */
	Buffer		rs_cbuf;		/* current buffer in scan, if any */
	/* NB: if rs_cbuf is not InvalidBuffer, we hold a pin on that buffer */
	ItemPointerData rs_mctid;	/* marked scan position, if any */

	/* these fields only used in page-at-a-time mode */
	int			rs_cindex;		/* current tuple's index in vistuples */
	int			rs_mindex;		/* marked tuple's saved index */
	int			rs_ntuples;		/* number of visible tuples on page */
	OffsetNumber rs_vistuples[MaxHeapTuplesPerPage];	/* their offsets */
} HeapScanDescData;

typedef HeapScanDescData *HeapScanDesc;

/*
 * We use the same IndexScanDescData structure for both amgettuple-based
 * and amgetmulti-based index scans.  Some fields are only relevant in
 * amgettuple-based scans.
 */
typedef struct IndexScanDescData
{
	/* scan parameters */
	Relation	heapRelation;	/* heap relation descriptor, or NULL */
	Relation	indexRelation;	/* index relation descriptor */
	Snapshot	xs_snapshot;	/* snapshot to see */
	int			numberOfKeys;	/* number of scan keys */
	ScanKey		keyData;		/* array of scan key descriptors */
	bool		is_multiscan;	/* TRUE = using amgetmulti */

	/* signaling to index AM about killing index tuples */
	bool		kill_prior_tuple;		/* last-returned tuple is dead */
	bool		ignore_killed_tuples;	/* do not return killed entries */

	/* index access method's private state */
	void	   *opaque;			/* access-method-specific info */

	/*
	 * xs_ctup/xs_cbuf are valid after a successful index_getnext. After
	 * index_getnext_indexitem, xs_ctup.t_self contains the heap tuple TID
	 * from the index entry, but its other fields are not valid.
	 */
	HeapTupleData xs_ctup;		/* current heap tuple, if any */
	Buffer		xs_cbuf;		/* current heap buffer in scan, if any */
	/* NB: if xs_cbuf is not InvalidBuffer, we hold a pin on that buffer */
} IndexScanDescData;

typedef IndexScanDescData *IndexScanDesc;


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
