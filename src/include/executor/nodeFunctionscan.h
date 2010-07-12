/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeFunctionscan.h,v 1.16 2010/07/12 17:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEFUNCTIONSCAN_H
#define NODEFUNCTIONSCAN_H

#include "nodes/execnodes.h"

extern FunctionScanState *ExecInitFunctionScan(FunctionScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecFunctionScan(FunctionScanState *node);
extern void ExecEndFunctionScan(FunctionScanState *node);
extern void ExecReScanFunctionScan(FunctionScanState *node);

#endif   /* NODEFUNCTIONSCAN_H */
