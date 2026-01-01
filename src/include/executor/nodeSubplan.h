/*-------------------------------------------------------------------------
 *
 * nodeSubplan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeSubplan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESUBPLAN_H
#define NODESUBPLAN_H

#include "nodes/execnodes.h"

extern SubPlanState *ExecInitSubPlan(SubPlan *subplan, PlanState *parent);

extern Datum ExecSubPlan(SubPlanState *node, ExprContext *econtext, bool *isNull);

extern Size EstimateSubplanHashTableSpace(double nentries,
										  Size tupleWidth,
										  bool unknownEqFalse);

extern void ExecReScanSetParamPlan(SubPlanState *node, PlanState *parent);

extern void ExecSetParamPlan(SubPlanState *node, ExprContext *econtext);

extern void ExecSetParamPlanMulti(const Bitmapset *params, ExprContext *econtext);

#endif							/* NODESUBPLAN_H */
