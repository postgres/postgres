/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planner.h,v 1.11 1999/05/13 07:29:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

/*
*/

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "parser/parse_node.h"

extern Plan *planner(Query *parse);
extern Plan *union_planner(Query *parse);
extern void pg_checkretval(Oid rettype, List *querytree_list);

#endif	 /* PLANNER_H */
