/*-------------------------------------------------------------------------
 *
 * nodeWorktablescan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeWorktablescan.h,v 1.5 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEWORKTABLESCAN_H
#define NODEWORKTABLESCAN_H

#include "nodes/execnodes.h"

extern WorkTableScanState *ExecInitWorkTableScan(WorkTableScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecWorkTableScan(WorkTableScanState *node);
extern void ExecEndWorkTableScan(WorkTableScanState *node);
extern void ExecReScanWorkTableScan(WorkTableScanState *node);

#endif   /* NODEWORKTABLESCAN_H */
