/*-------------------------------------------------------------------------
 *
 * var.h
 *	  prototypes for optimizer/util/var.c.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/var.h,v 1.34 2006/03/05 15:58:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VAR_H
#define VAR_H

#include "nodes/relation.h"


extern Relids pull_varnos(Node *node);
extern bool contain_var_reference(Node *node, int varno, int varattno,
					  int levelsup);
extern bool contain_var_clause(Node *node);
extern bool contain_vars_of_level(Node *node, int levelsup);
extern bool contain_vars_above_level(Node *node, int levelsup);
extern int	find_minimum_var_level(Node *node);
extern List *pull_var_clause(Node *node, bool includeUpperVars);
extern Node *flatten_join_alias_vars(PlannerInfo *root, Node *node);

#endif   /* VAR_H */
