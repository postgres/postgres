/*-------------------------------------------------------------------------
 *
 * joininfo.h--
 *	  prototypes for joininfo.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: joininfo.h,v 1.8 1998/09/01 04:37:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef JOININFO_H
#define JOININFO_H

#include "nodes/nodes.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"

extern JoinInfo *joininfo_member(List *join_relids, List *joininfo_list);
extern JoinInfo *find_joininfo_node(RelOptInfo * this_rel, List *join_relids);
extern Var *other_join_clause_var(Var *var, Expr *clause);

#endif	 /* JOININFO_H */
