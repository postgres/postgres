/*-------------------------------------------------------------------------
 *
 * var.h
 *	  prototypes for var.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: var.h,v 1.13 2001/04/18 20:42:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VAR_H
#define VAR_H

#include "nodes/primnodes.h"

extern List *pull_varnos(Node *node);
extern bool contain_whole_tuple_var(Node *node, int varno, int levelsup);
extern bool contain_var_clause(Node *node);
extern List *pull_var_clause(Node *node, bool includeUpperVars);

#endif	 /* VAR_H */
