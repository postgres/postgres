/*-------------------------------------------------------------------------
 *
 * planner.h--
 *	  prototypes for planner.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planner.h,v 1.3 1997/09/07 04:59:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

/*
*/

extern Plan    *planner(Query * parse);
extern void		pg_checkretval(Oid rettype, QueryTreeList * querytree_list);

#endif							/* PLANNER_H */
