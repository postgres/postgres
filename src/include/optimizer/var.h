/*-------------------------------------------------------------------------
 *
 * var.h
 *	  prototypes for var.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: var.h,v 1.25 2003/01/20 18:55:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VAR_H
#define VAR_H

#include "nodes/parsenodes.h"


extern List *pull_varnos(Node *node);
extern bool contain_var_reference(Node *node, int varno, int varattno,
					  int levelsup);
extern bool contain_whole_tuple_var(Node *node, int varno, int levelsup);
extern bool contain_var_clause(Node *node);
extern bool contain_vars_of_level(Node *node, int levelsup);
extern bool contain_vars_above_level(Node *node, int levelsup);
extern List *pull_var_clause(Node *node, bool includeUpperVars);
extern Node *flatten_join_alias_vars(Query *root, Node *node);

#endif   /* VAR_H */
