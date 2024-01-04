/*-------------------------------------------------------------------------
 *
 * nodeTableFuncscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeTableFuncscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODETABLEFUNCSCAN_H
#define NODETABLEFUNCSCAN_H

#include "nodes/execnodes.h"

extern TableFuncScanState *ExecInitTableFuncScan(TableFuncScan *node, EState *estate, int eflags);
extern void ExecEndTableFuncScan(TableFuncScanState *node);
extern void ExecReScanTableFuncScan(TableFuncScanState *node);

#endif							/* NODETABLEFUNCSCAN_H */
