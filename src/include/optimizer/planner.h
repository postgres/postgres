/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/planner.h,v 1.36 2007/01/05 22:19:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"


extern ParamListInfo PlannerBoundParamList;		/* current boundParams */

extern Plan *planner(Query *parse, bool isCursor, int cursorOptions,
		ParamListInfo boundParams);
extern Plan *subquery_planner(Query *parse, double tuple_fraction,
				 List **subquery_pathkeys);

#endif   /* PLANNER_H */
