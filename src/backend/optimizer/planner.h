/*-------------------------------------------------------------------------
 *
 * planner.h--
 *    prototypes for planner.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planner.h,v 1.1.1.1 1996/07/09 06:21:34 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

/*
#include "optimizer/internal.h"
#include "parser/parse_query.h"
*/

extern Plan *planner(Query *parse);
extern void pg_checkretval(Oid rettype, QueryTreeList *querytree_list);

#endif	/* PLANNER_H */
