/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/planner.h,v 1.40 2007/05/25 17:54:25 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

#include "nodes/plannodes.h"
#include "nodes/relation.h"


/* Hook for plugins to get control in planner() */
typedef PlannedStmt * (*planner_hook_type) (Query *parse,
											int cursorOptions,
											ParamListInfo boundParams);
extern DLLIMPORT planner_hook_type planner_hook;


extern PlannedStmt *planner(Query *parse, int cursorOptions,
		ParamListInfo boundParams);
extern PlannedStmt *standard_planner(Query *parse, int cursorOptions,
		ParamListInfo boundParams);
extern Plan *subquery_planner(PlannerGlobal *glob, Query *parse,
							  Index level, double tuple_fraction,
							  PlannerInfo **subroot);

#endif   /* PLANNER_H */
