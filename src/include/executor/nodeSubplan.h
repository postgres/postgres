/*-------------------------------------------------------------------------
 *
 * nodeSubplan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeSubplan.h,v 1.24 2006/03/05 15:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESUBPLAN_H
#define NODESUBPLAN_H

#include "nodes/execnodes.h"

extern void ExecInitSubPlan(SubPlanState *node, EState *estate, int eflags);
extern Datum ExecSubPlan(SubPlanState *node,
			ExprContext *econtext,
			bool *isNull,
			ExprDoneCond *isDone);
extern void ExecEndSubPlan(SubPlanState *node);
extern void ExecReScanSetParamPlan(SubPlanState *node, PlanState *parent);

extern void ExecSetParamPlan(SubPlanState *node, ExprContext *econtext);

#endif   /* NODESUBPLAN_H */
