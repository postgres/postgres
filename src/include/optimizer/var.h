/*-------------------------------------------------------------------------
 *
 * var.h
 *	  prototypes for optimizer/util/var.c.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/var.h,v 1.40 2009/01/01 17:24:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VAR_H
#define VAR_H

#include "nodes/relation.h"


extern Relids pull_varnos(Node *node);
extern void pull_varattnos(Node *node, Bitmapset **varattnos);
extern bool contain_var_clause(Node *node);
extern bool contain_vars_of_level(Node *node, int levelsup);
extern int	locate_var_of_level(Node *node, int levelsup);
extern int	locate_var_of_relation(Node *node, int relid, int levelsup);
extern int	find_minimum_var_level(Node *node);
extern List *pull_var_clause(Node *node, bool includePlaceHolderVars);
extern Node *flatten_join_alias_vars(PlannerInfo *root, Node *node);

#endif   /* VAR_H */
