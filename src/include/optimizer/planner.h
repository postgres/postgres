/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planner.h,v 1.20 2001/06/05 05:26:05 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"


extern Plan *planner(Query *parse);
extern Plan *subquery_planner(Query *parse, double tuple_fraction);

extern Plan *make_sortplan(Query *parse, List *tlist,
						   Plan *plannode, List *sortcls);

#endif	 /* PLANNER_H */
