/*-------------------------------------------------------------------------
 *
 * relscan.h
 *	  POSTGRES internal relation scan descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relscan.h,v 1.23 2001/10/25 05:49:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELSCAN_H
#define RELSCAN_H

#include "utils/tqual.h"


typedef struct HeapScanDescData
{
	Relation	rs_rd;			/* pointer to relation descriptor */
	HeapTupleData rs_ctup;		/* current tuple in scan, if any */
	Buffer		rs_cbuf;		/* current buffer in scan, if any */
	/* NB: if rs_cbuf is not InvalidBuffer, we hold a pin on that buffer */
	ItemPointerData rs_mctid;	/* marked tid, if any */
	Snapshot	rs_snapshot;	/* snapshot to see */
	uint16		rs_nkeys;		/* number of scan keys to select tuples */
	ScanKey		rs_key;			/* key descriptors */

	PgStat_Info rs_pgstat_info; /* statistics collector hook */
} HeapScanDescData;

typedef HeapScanDescData *HeapScanDesc;

typedef struct IndexScanDescData
{
	Relation	relation;		/* relation descriptor */
	void	   *opaque;			/* access-method-specific info */
	ItemPointerData currentItemData;	/* current index pointer */
	ItemPointerData currentMarkData;	/* marked current pointer */
	uint8		flags;			/* scan position flags */
	bool		scanFromEnd;	/* restart scan at end? */
	uint16		numberOfKeys;	/* number of scan keys to select tuples */
	ScanKey		keyData;		/* key descriptors */
	FmgrInfo	fn_getnext;		/* cached lookup info for am's getnext fn */

	PgStat_Info xs_pgstat_info; /* statistics collector hook */
} IndexScanDescData;

typedef IndexScanDescData *IndexScanDesc;

/* IndexScanDesc flag bits (none of these are actually used currently) */
#define ScanUnmarked			0x01
#define ScanUncheckedPrevious	0x02
#define ScanUncheckedNext		0x04


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
#endif	 /* RELSCAN_H */
