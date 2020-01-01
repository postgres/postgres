/*-------------------------------------------------------------------------
 *
 * nodeWorktablescan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeWorktablescan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEWORKTABLESCAN_H
#define NODEWORKTABLESCAN_H

#include "nodes/execnodes.h"

extern WorkTableScanState *ExecInitWorkTableScan(WorkTableScan *node, EState *estate, int eflags);
extern void ExecEndWorkTableScan(WorkTableScanState *node);
extern void ExecReScanWorkTableScan(WorkTableScanState *node);

#endif							/* NODEWORKTABLESCAN_H */
