/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeSeqscan.h,v 1.3 1997/09/07 04:57:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESEQSCAN_H
#define NODESEQSCAN_H

extern TupleTableSlot *ExecSeqScan(SeqScan * node);
extern bool		ExecInitSeqScan(SeqScan * node, EState * estate, Plan * parent);
extern int		ExecCountSlotsSeqScan(SeqScan * node);
extern void		ExecEndSeqScan(SeqScan * node);
extern void		ExecSeqReScan(SeqScan * node, ExprContext * exprCtxt, Plan * parent);
extern void		ExecSeqMarkPos(SeqScan * node);
extern void		ExecSeqRestrPos(SeqScan * node);

#endif							/* NODESEQSCAN_H */
