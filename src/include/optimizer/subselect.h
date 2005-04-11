/*-------------------------------------------------------------------------
 *
 * subselect.h
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/subselect.h,v 1.24 2005/04/11 23:06:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUBSELECT_H
#define SUBSELECT_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

extern Index PlannerQueryLevel; /* level of current query */
extern List *PlannerInitPlan;	/* init subplans for current query */
extern List *PlannerParamList;	/* to keep track of cross-level Params */
extern int	PlannerPlanId;		/* to assign unique ID to subquery plans */

extern Node *convert_IN_to_join(Query *parse, SubLink *sublink);
extern Node *SS_replace_correlation_vars(Node *expr);
extern Node *SS_process_sublinks(Node *expr, bool isQual);
extern void SS_finalize_plan(Plan *plan, List *rtable);
extern Param *SS_make_initplan_from_plan(Query *root, Plan *plan,
										 Oid resulttype, int32 resulttypmod);

#endif   /* SUBSELECT_H */
