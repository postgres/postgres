/*-------------------------------------------------------------------------
 *
 * genam.h--
 *    POSTGRES general access method definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: genam.h,v 1.1.1.1 1996/07/09 06:21:08 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	GENAM_H
#define GENAM_H

#include "postgres.h"

#include "access/attnum.h"
#include "access/htup.h"
#include "access/istrat.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/sdir.h"
#include "access/funcindex.h"

/* ----------------
 *	generalized index_ interface routines
 * ----------------
 */
extern Relation index_open(Oid relationId);
extern Relation index_openr(char *relationName);
extern void index_close(Relation relation);
extern InsertIndexResult index_insert(Relation relation,
				      IndexTuple indexTuple);
extern void index_delete(Relation relation, ItemPointer indexItem);
extern IndexScanDesc index_beginscan(Relation relation, bool scanFromEnd,
     uint16 numberOfKeys, ScanKey key);
extern void index_rescan(IndexScanDesc scan, bool scanFromEnd, ScanKey key);
extern void index_endscan(IndexScanDesc scan);
extern void index_markpos(IndexScanDesc scan);
extern void index_restrpos(IndexScanDesc scan);
extern RetrieveIndexResult index_getnext(IndexScanDesc scan,
					 ScanDirection direction);
extern RegProcedure index_getprocid(Relation irel, AttrNumber attnum,
				    uint16 procnum);
extern Datum GetIndexValue(HeapTuple tuple, TupleDesc hTupDesc,
     int attOff, AttrNumber attrNums[], FuncIndexInfo *fInfo,
     bool *attNull, Buffer buffer);

/* in genam.c */
extern IndexScanDesc RelationGetIndexScan(Relation relation, bool scanFromEnd,
					  uint16 numberOfKeys, ScanKey key);
extern void IndexScanRestart(IndexScanDesc scan, bool scanFromEnd,
			     ScanKey key);
extern void IndexScanEnd(IndexScanDesc scan);
extern void IndexScanMarkPosition(IndexScanDesc scan);
extern void IndexScanRestorePosition(IndexScanDesc scan);

#endif	/* GENAM_H */
