/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeSeqscan.h,v 1.1 1996/08/28 07:22:24 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	NODESEQSCAN_H
#define	NODESEQSCAN_H

extern TupleTableSlot *SeqNext(SeqScan *node);
extern TupleTableSlot *ExecSeqScan(SeqScan *node);
extern Oid InitScanRelation(SeqScan *node, EState *estate,
			    CommonScanState *scanstate, Plan *outerPlan);
extern bool ExecInitSeqScan(SeqScan *node, EState *estate, Plan *parent);
extern int ExecCountSlotsSeqScan(SeqScan *node);
extern void ExecEndSeqScan(SeqScan *node);
extern void ExecSeqReScan(SeqScan *node, ExprContext *exprCtxt, Plan* parent);
extern void ExecSeqMarkPos(SeqScan *node);
extern void ExecSeqRestrPos(SeqScan *node);

#endif	/* NODESEQSCAN_H */
