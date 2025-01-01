/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeFunctionscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEFUNCTIONSCAN_H
#define NODEFUNCTIONSCAN_H

#include "nodes/execnodes.h"

extern FunctionScanState *ExecInitFunctionScan(FunctionScan *node, EState *estate, int eflags);
extern void ExecEndFunctionScan(FunctionScanState *node);
extern void ExecReScanFunctionScan(FunctionScanState *node);

#endif							/* NODEFUNCTIONSCAN_H */
