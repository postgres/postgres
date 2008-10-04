/*-------------------------------------------------------------------------
 *
 * nodeWorktablescan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeWorktablescan.h,v 1.1 2008/10/04 21:56:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEWORKTABLESCAN_H
#define NODEWORKTABLESCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsWorkTableScan(WorkTableScan *node);
extern WorkTableScanState *ExecInitWorkTableScan(WorkTableScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecWorkTableScan(WorkTableScanState *node);
extern void ExecEndWorkTableScan(WorkTableScanState *node);
extern void ExecWorkTableScanReScan(WorkTableScanState *node, ExprContext *exprCtxt);

#endif   /* NODEWORKTABLESCAN_H */
