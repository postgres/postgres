/*-------------------------------------------------------------------------
 *
 * xfunc.h--
 *	  prototypes for xfunc.c and predmig.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: xfunc.h,v 1.10 1998/09/01 04:37:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef XFUNC_H
#define XFUNC_H

#include <utils/rel.h>
#include <nodes/relation.h>

/* command line arg flags */
#define XFUNC_OFF -1			/* do no optimization of expensive preds */
#define XFUNC_NOR 2				/* do no optimization of OR clauses */
#define XFUNC_NOPULL 4			/* never pull restrictions above joins */
#define XFUNC_NOPM 8			/* don't do predicate migration */
#define XFUNC_WAIT 16			/* don't do pullup until predicate
								 * migration */
#define XFUNC_PULLALL 32		/* pull all expensive restrictions up,
								 * always */

/* constants for local and join predicates */
#define XFUNC_LOCPRD 1
#define XFUNC_JOINPRD 2
#define XFUNC_UNKNOWN 0

extern int	XfuncMode;			/* defined in tcop/postgres.c */

/* defaults for function attributes used for expensive function calculations */
#define BYTE_PCT 100
#define PERBYTE_CPU 0
#define PERCALL_CPU 0
#define OUTIN_RATIO 100

/* default width assumed for variable length attributes */
#define VARLEN_DEFAULT 128;

/* Macro to get group rank out of group cost and group sel */
#define get_grouprank(a) ((get_groupsel(a) - 1) / get_groupcost(a))

/* Macro to see if a path node is actually a Join */
#define is_join(pathnode) (length(get_relids(get_parent(pathnode))) > 1 ? 1 : 0)

/* function prototypes from planner/path/xfunc.c */
extern void xfunc_trypullup(RelOptInfo * rel);
extern int xfunc_shouldpull(Path *childpath, JoinPath *parentpath,
				 int whichchild, ClauseInfo * maxcinfopt);
extern ClauseInfo *xfunc_pullup(Path *childpath, JoinPath *parentpath, ClauseInfo * cinfo,
			 int whichchild, int clausetype);
extern Cost xfunc_rank(Expr *clause);
extern Cost xfunc_expense(Query *queryInfo, Expr *clause);
extern Cost xfunc_join_expense(JoinPath *path, int whichchild);
extern Cost xfunc_local_expense(Expr *clause);
extern Cost xfunc_func_expense(Expr *node, List *args);
extern int	xfunc_width(Expr *clause);

/* static, moved to xfunc.c */
/* extern int xfunc_card_unreferenced(Expr *clause, Relid referenced); */
extern int	xfunc_card_product(Relid relids);
extern List *xfunc_find_references(List *clause);
extern List *xfunc_primary_join(JoinPath *pathnode);
extern Cost xfunc_get_path_cost(Path *pathnode);
extern Cost xfunc_total_path_cost(JoinPath *pathnode);
extern Cost xfunc_expense_per_tuple(JoinPath *joinnode, int whichchild);
extern void xfunc_fixvars(Expr *clause, RelOptInfo * rel, int varno);
extern int	xfunc_cinfo_compare(void *arg1, void *arg2);
extern int	xfunc_clause_compare(void *arg1, void *arg2);
extern void xfunc_disjunct_sort(List *clause_list);
extern int	xfunc_disjunct_compare(void *arg1, void *arg2);
extern int	xfunc_func_width(RegProcedure funcid, List *args);
extern int	xfunc_tuple_width(Relation rd);
extern int	xfunc_num_join_clauses(JoinPath *path);
extern List *xfunc_LispRemove(List *foo, List *bar);
extern bool xfunc_copyrel(RelOptInfo * from, RelOptInfo ** to);

/*
 * function prototypes for path/predmig.c
 */
extern bool xfunc_do_predmig(Path root);

#endif	 /* XFUNC_H */
