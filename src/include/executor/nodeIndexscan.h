/*-------------------------------------------------------------------------
 *
 * nodeIndexscan.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeIndexscan.h,v 1.2 1996/10/23 07:41:34 scrappy Exp $
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

extern void ExecIndexReScan(IndexScan *node, ExprContext *exprCtxt, Plan* parent);
#endif	/* NODEINDEXSCAN_H */
