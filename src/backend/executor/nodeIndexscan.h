/*-------------------------------------------------------------------------
 *
 * nodeIndexscan.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeIndexscan.h,v 1.1.1.1 1996/07/09 06:21:26 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	NODEINDEXSCAN_H
#define	NODEINDEXSCAN_H

extern TupleTableSlot *ExecIndexScan(IndexScan *node);

extern void ExecIndexReScan(IndexScan *node, ExprContext *exprCtxt, Plan *parent);

extern void ExecEndIndexScan(IndexScan *node);

extern void ExecIndexMarkPos(IndexScan *node);

extern void ExecIndexRestrPos(IndexScan *node);

extern void ExecUpdateIndexScanKeys(IndexScan *node, ExprContext *econtext);

extern bool ExecInitIndexScan(IndexScan *node, EState *estate, Plan *parent);

extern int ExecCountSlotsIndexScan(IndexScan *node);

#endif	/* NODEINDEXSCAN_H */
