/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planner.h,v 1.19 2001/01/24 19:43:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"


extern Plan *planner(Query *parse);
extern Plan *subquery_planner(Query *parse, double tuple_fraction);

extern Plan *make_sortplan(List *tlist, Plan *plannode, List *sortcls);

#endif	 /* PLANNER_H */
