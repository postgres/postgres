/*-------------------------------------------------------------------------
 *
 * planner.h--
 *	  prototypes for planner.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planner.h,v 1.6 1997/11/25 22:06:37 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

/*
*/

#include <parser/parse_node.h>

extern Plan *planner(Query *parse);
extern void pg_checkretval(Oid rettype, QueryTreeList *querytree_list);

#endif							/* PLANNER_H */
