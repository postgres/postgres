/*-------------------------------------------------------------------------
 *
 * nodeNestloop.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeNestloop.h,v 1.6 1998/02/13 03:43:38 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODENESTLOOP_H
#define NODENESTLOOP_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern TupleTableSlot *ExecNestLoop(NestLoop *node, Plan *parent);
extern bool ExecInitNestLoop(NestLoop *node, EState *estate, Plan *parent);
extern int	ExecCountSlotsNestLoop(NestLoop *node);
extern void ExecEndNestLoop(NestLoop *node);
extern void ExecReScanNestLoop(NestLoop *node, ExprContext *exprCtxt, Plan *parent);

#endif							/* NODENESTLOOP_H */
