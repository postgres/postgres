/*-------------------------------------------------------------------------
 *
 * var.h--
 *	  prototypes for var.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: var.h,v 1.5 1997/11/26 01:13:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VAR_H
#define VAR_H

#include "nodes/nodes.h"
#include "nodes/primnodes.h"

extern List *pull_varnos(Node *me);
extern bool contain_var_clause(Node *clause);
extern List *pull_var_clause(Node *clause);
extern bool var_equal(Var *var1, Var *var2);

#endif							/* VAR_H */
