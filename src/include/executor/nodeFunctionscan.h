/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeFunctionscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEFUNCTIONSCAN_H
#define NODEFUNCTIONSCAN_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern FunctionScanState *ExecInitFunctionScan(FunctionScan *node, EState *estate, int eflags);
extern void ExecEndFunctionScan(FunctionScanState *node);
extern void ExecReScanFunctionScan(FunctionScanState *node);
extern void ExecFunctionScanEstimate(FunctionScanState *node,
									 ParallelContext *pcxt);
extern void ExecFunctionScanInitializeDSM(FunctionScanState *node,
										  ParallelContext *pcxt);
extern void ExecFunctionScanInitializeWorker(FunctionScanState *node,
											 ParallelWorkerContext *pwcxt);
extern void ExecFunctionScanRetrieveInstrumentation(FunctionScanState *node);

#endif							/* NODEFUNCTIONSCAN_H */
