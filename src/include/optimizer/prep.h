/*-------------------------------------------------------------------------
 *
 * prep.h
 *	  prototypes for files in optimizer/prep/
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/prep.h,v 1.48 2005/03/17 23:45:09 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREP_H
#define PREP_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/relation.h"


/*
 * prototypes for prepjointree.c
 */
extern int	from_collapse_limit;
extern int	join_collapse_limit;

extern Node *pull_up_IN_clauses(Query *parse, Node *node);
extern Node *pull_up_subqueries(Query *parse, Node *jtnode,
				   bool below_outer_join);
extern void reduce_outer_joins(Query *parse);
extern Node *simplify_jointree(Query *parse, Node *jtnode);
extern Relids get_relids_in_jointree(Node *jtnode);
extern Relids get_relids_for_join(Query *parse, int joinrelid);

/*
 * prototypes for prepqual.c
 */
extern Expr *canonicalize_qual(Expr *qual);
extern Node *flatten_andors(Node *node);

/*
 * prototypes for preptlist.c
 */
extern List *preprocess_targetlist(Query *parse, List *tlist);

/*
 * prototypes for prepunion.c
 */
extern Plan *plan_set_operations(Query *parse, List **sortClauses);

extern List *find_all_inheritors(Oid parentrel);

extern List *expand_inherited_rtentry(Query *parse, Index rti);

extern Node *adjust_inherited_attrs(Node *node,
					   Index old_rt_index, Oid old_relid,
					   Index new_rt_index, Oid new_relid);

#endif   /* PREP_H */
