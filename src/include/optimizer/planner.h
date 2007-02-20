/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/planner.h,v 1.38 2007/02/20 17:32:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

#include "nodes/plannodes.h"
#include "nodes/relation.h"


extern PlannedStmt *planner(Query *parse, bool isCursor, int cursorOptions,
		ParamListInfo boundParams);
extern Plan *subquery_planner(PlannerGlobal *glob, Query *parse,
							  Index level, double tuple_fraction,
							  PlannerInfo **subroot);

#endif   /* PLANNER_H */
