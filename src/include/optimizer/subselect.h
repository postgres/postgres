/*-------------------------------------------------------------------------
 *
 * subselect.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUBSELECT_H
#define SUBSELECT_H

#include "nodes/plannodes.h"

extern Index PlannerQueryLevel; /* level of current query */
extern List *PlannerInitPlan;	/* init subplans for current query */
extern List *PlannerParamVar;	/* to get Var from Param->paramid */
extern int	PlannerPlanId;		/* to assign unique ID to subquery plans */

extern List *SS_finalize_plan(Plan *plan);
extern Node *SS_replace_correlation_vars(Node *expr);
extern Node *SS_process_sublinks(Node *expr);

#endif	 /* SUBSELECT_H */
