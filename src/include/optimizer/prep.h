/*-------------------------------------------------------------------------
 *
 * prep.h
 *	  prototypes for files in optimizer/prep/
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: prep.h,v 1.41 2003/08/04 02:40:14 momjian Exp $
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
extern List *canonicalize_qual(Expr *qual, bool removeAndFlag);
extern List *cnfify(Expr *qual, bool removeAndFlag);

/*
 * prototypes for preptlist.c
 */
extern List *preprocess_targetlist(List *tlist, int command_type,
					  Index result_relation, List *range_table);

/*
 * prototypes for prepunion.c
 */
extern Plan *plan_set_operations(Query *parse);

extern List *find_all_inheritors(Oid parentrel);

extern List *expand_inherited_rtentry(Query *parse, Index rti,
						 bool dup_parent);

extern Node *adjust_inherited_attrs(Node *node,
					   Index old_rt_index, Oid old_relid,
					   Index new_rt_index, Oid new_relid);

#endif   /* PREP_H */
