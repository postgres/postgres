/*-------------------------------------------------------------------------
 *
 * var.h
 *	  prototypes for var.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: var.h,v 1.20 2002/05/18 02:25:50 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VAR_H
#define VAR_H

#include "nodes/primnodes.h"


extern List *pull_varnos(Node *node);
extern bool contain_var_reference(Node *node, int varno, int varattno,
					  int levelsup);
extern bool contain_whole_tuple_var(Node *node, int varno, int levelsup);
extern bool contain_var_clause(Node *node);
extern List *pull_var_clause(Node *node, bool includeUpperVars);
extern Node *flatten_join_alias_vars(Node *node, List *rtable, bool force);

#endif   /* VAR_H */
