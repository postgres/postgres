/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planner.h,v 1.26 2003/03/10 03:53:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"


extern Plan *planner(Query *parse, bool isCursor, int cursorOptions);
extern Plan *subquery_planner(Query *parse, double tuple_fraction);

#endif   /* PLANNER_H */
