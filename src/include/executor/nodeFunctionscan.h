/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeFunctionscan.h,v 1.2 2002/06/20 20:29:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEFUNCTIONSCAN_H
#define NODEFUNCTIONSCAN_H

#include "nodes/plannodes.h"

extern TupleTableSlot *ExecFunctionScan(FunctionScan *node);
extern void ExecEndFunctionScan(FunctionScan *node);
extern bool ExecInitFunctionScan(FunctionScan *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsFunctionScan(FunctionScan *node);
extern void ExecFunctionMarkPos(FunctionScan *node);
extern void ExecFunctionRestrPos(FunctionScan *node);
extern void ExecFunctionReScan(FunctionScan *node, ExprContext *exprCtxt, Plan *parent);

#endif   /* NODEFUNCTIONSCAN_H */
