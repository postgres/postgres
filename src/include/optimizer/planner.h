/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/planner.h,v 1.35 2006/03/05 15:58:57 momjian Exp $
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
