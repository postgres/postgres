/*-------------------------------------------------------------------------
 *
 * subselect.h
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: subselect.h,v 1.19 2003/06/06 15:04:03 tgl Exp $
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

#endif   /* SUBSELECT_H */
