/*-------------------------------------------------------------------------
 *
 * nodeSubplan.h--
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESUBPLAN_H
#define NODESUBPLAN_H

#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

extern Datum ExecSubPlan(SubPlan *node, List *pvar, ExprContext *econtext);
extern bool ExecInitSubPlan(SubPlan *node, EState *estate, Plan *parent);
extern void ExecReScanSetParamPlan (SubPlan *node, Plan *parent);
extern void ExecSetParamPlan (SubPlan *node);
extern void ExecEndSubPlan(SubPlan *node);

#endif							/* NODESUBPLAN_H */
