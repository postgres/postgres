/*-------------------------------------------------------------------------
 *
 * nodeResult.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeResult.h,v 1.6 1998/02/13 03:43:39 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODERESULT_H
#define NODERESULT_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecResult(Result *node);
extern bool ExecInitResult(Result *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsResult(Result *node);
extern void ExecEndResult(Result *node);
extern void ExecReScanResult(Result *node, ExprContext *exprCtxt, Plan *parent);

#endif							/* NODERESULT_H */
