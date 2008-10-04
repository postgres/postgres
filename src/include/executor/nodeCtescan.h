/*-------------------------------------------------------------------------
 *
 * nodeCtescan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeCtescan.h,v 1.1 2008/10/04 21:56:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODECTESCAN_H
#define NODECTESCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsCteScan(CteScan *node);
extern CteScanState *ExecInitCteScan(CteScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecCteScan(CteScanState *node);
extern void ExecEndCteScan(CteScanState *node);
extern void ExecCteScanReScan(CteScanState *node, ExprContext *exprCtxt);

#endif   /* NODECTESCAN_H */
