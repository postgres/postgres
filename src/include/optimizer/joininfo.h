/*-------------------------------------------------------------------------
 *
 * joininfo.h--
 *	  prototypes for joininfo.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: joininfo.h,v 1.6 1998/07/18 04:22:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef JOININFO_H
#define JOININFO_H

#include "nodes/nodes.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"

extern JInfo *joininfo_member(List *join_relids, List *joininfo_list);
extern JInfo *find_joininfo_node(RelOptInfo *this_rel, List *join_relids);
extern Var *other_join_clause_var(Var *var, Expr *clause);

#endif							/* JOININFO_H */
