/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeSeqscan.h,v 1.6 1997/11/26 01:13:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESEQSCAN_H
#define NODESEQSCAN_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecSeqScan(SeqScan *node);
extern bool ExecInitSeqScan(SeqScan *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsSeqScan(SeqScan *node);
extern void ExecEndSeqScan(SeqScan *node);
extern void ExecSeqReScan(SeqScan *node, ExprContext *exprCtxt, Plan *parent);
extern void ExecSeqMarkPos(SeqScan *node);
extern void ExecSeqRestrPos(SeqScan *node);

#endif							/* NODESEQSCAN_H */
