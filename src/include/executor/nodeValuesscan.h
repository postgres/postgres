/*-------------------------------------------------------------------------
 *
 * nodeValuesscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeValuesscan.h,v 1.4 2008/01/01 19:45:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEVALUESSCAN_H
#define NODEVALUESSCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsValuesScan(ValuesScan *node);
extern ValuesScanState *ExecInitValuesScan(ValuesScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecValuesScan(ValuesScanState *node);
extern void ExecEndValuesScan(ValuesScanState *node);
extern void ExecValuesMarkPos(ValuesScanState *node);
extern void ExecValuesRestrPos(ValuesScanState *node);
extern void ExecValuesReScan(ValuesScanState *node, ExprContext *exprCtxt);

#endif   /* NODEVALUESSCAN_H */
