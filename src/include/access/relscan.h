/*-------------------------------------------------------------------------
 *
 * relscan.h--
 *    POSTGRES internal relation scan descriptor definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relscan.h,v 1.5 1996/11/04 08:52:41 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	RELSCAN_H
#define RELSCAN_H

#include <utils/tqual.h>
#include <storage/buf.h>
#include <utils/rel.h>

typedef ItemPointerData	MarkData;

typedef struct HeapScanDescData {
	Relation	rs_rd;		/* pointer to relation descriptor */
	HeapTuple	rs_ptup;	/* previous tuple in scan */
	HeapTuple	rs_ctup;	/* current tuple in scan */
	HeapTuple	rs_ntup;	/* next tuple in scan */
	Buffer		rs_pbuf;	/* previous buffer in scan */
	Buffer		rs_cbuf;	/* current buffer in scan */
	Buffer		rs_nbuf;	/* next buffer in scan */
	ItemPointerData	rs_mptid;	/* marked previous tid */
	ItemPointerData	rs_mctid;	/* marked current tid */
	ItemPointerData	rs_mntid;	/* marked next tid */
	ItemPointerData	rs_mcd;		/* marked current delta XXX ??? */
	bool		rs_atend;	/* restart scan at end? */
	TimeQual	rs_tr;		/* time qualification */
	uint16		rs_cdelta;	/* current delta in chain */
	uint16		rs_nkeys;	/* number of attributes in keys */
	ScanKey		rs_key;		/* key descriptors */
} HeapScanDescData;

typedef HeapScanDescData *HeapScanDesc;

typedef struct IndexScanDescData {
	Relation	relation;		/* relation descriptor */
	void		*opaque;		/* am-specific slot */
	ItemPointerData	previousItemData;	/* previous index pointer */
	ItemPointerData	currentItemData;	/* current index pointer */
	ItemPointerData	nextItemData;		/* next index pointer */
	MarkData	previousMarkData;	/* marked previous pointer */
	MarkData	currentMarkData;	/* marked current  pointer */
	MarkData	nextMarkData;		/* marked next pointer */
	uint8		flags;			/* scan position flags */
	bool		scanFromEnd;		/* restart scan at end? */
	uint16		numberOfKeys;		/* number of key attributes */
	ScanKey		keyData;		/* key descriptor */
} IndexScanDescData;

typedef IndexScanDescData	*IndexScanDesc;

/* ----------------
 *	IndexScanDescPtr is used in the executor where we have to
 *	keep track of several index scans when using several indices
 *	- cim 9/10/89
 * ----------------
 */
typedef IndexScanDesc		*IndexScanDescPtr;

/*
 * HeapScanIsValid --
 *	True iff the heap scan is valid.
 */
#define	HeapScanIsValid(scan) PointerIsValid(scan)

/*
 * IndexScanIsValid --
 *	True iff the index scan is valid.
 */
#define IndexScanIsValid(scan) PointerIsValid(scan)

#endif	/* RELSCAN_H */
