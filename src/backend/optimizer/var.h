/*-------------------------------------------------------------------------
 *
 * var.h--
 *    prototypes for var.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: var.h,v 1.1.1.1 1996/07/09 06:21:35 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VAR_H
#define VAR_H

extern List *pull_varnos(Node *me);
extern bool contain_var_clause(Node *clause);
extern List *pull_var_clause(Node *clause);
extern bool var_equal(Var *var1, Var *var2);

#endif /* VAR_H */
