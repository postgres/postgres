/*-------------------------------------------------------------------------
 *
 * planner.h--
 *	  prototypes for planner.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planner.h,v 1.7 1997/11/26 01:13:49 momjian Exp $
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
extern void pg_checkretval(Oid rettype, QueryTreeList *querytree_list);

#endif							/* PLANNER_H */
