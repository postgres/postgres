/*-------------------------------------------------------------------------
 *
 * paths.h
 *	  prototypes for various files in optimizer/path
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/paths.h,v 1.103 2008/01/01 19:45:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHS_H
#define PATHS_H

#include "nodes/relation.h"


/*
 * allpaths.c
 */
extern bool enable_geqo;
extern int	geqo_threshold;

/* Hook for plugins to replace standard_join_search() */
typedef RelOptInfo *(*join_search_hook_type) (PlannerInfo *root,
														  int levels_needed,
														  List *initial_rels);
extern PGDLLIMPORT join_search_hook_type join_search_hook;


extern RelOptInfo *make_one_rel(PlannerInfo *root, List *joinlist);
extern RelOptInfo *standard_join_search(PlannerInfo *root, int levels_needed,
					 List *initial_rels);

#ifdef OPTIMIZER_DEBUG
extern void debug_print_rel(PlannerInfo *root, RelOptInfo *rel);
#endif

/*
 * indxpath.c
 *	  routines to generate index paths
 */
typedef enum
{
	/* Whether to use ScalarArrayOpExpr to build index qualifications */
	SAOP_FORBID,				/* Do not use ScalarArrayOpExpr */
	SAOP_ALLOW,					/* OK to use ScalarArrayOpExpr */
	SAOP_REQUIRE				/* Require ScalarArrayOpExpr */
} SaOpControl;

extern void create_index_paths(PlannerInfo *root, RelOptInfo *rel);
extern List *generate_bitmap_or_paths(PlannerInfo *root, RelOptInfo *rel,
						 List *clauses, List *outer_clauses,
						 RelOptInfo *outer_rel);
extern void best_inner_indexscan(PlannerInfo *root, RelOptInfo *rel,
					 RelOptInfo *outer_rel, JoinType jointype,
					 Path **cheapest_startup, Path **cheapest_total);
extern List *group_clauses_by_indexkey(IndexOptInfo *index,
						  List *clauses, List *outer_clauses,
						  Relids outer_relids,
						  SaOpControl saop_control,
						  bool *found_clause);
extern bool eclass_matches_any_index(EquivalenceClass *ec,
						 EquivalenceMember *em,
						 RelOptInfo *rel);
extern bool match_index_to_operand(Node *operand, int indexcol,
					   IndexOptInfo *index);
extern List *expand_indexqual_conditions(IndexOptInfo *index,
							List *clausegroups);
extern void check_partial_indexes(PlannerInfo *root, RelOptInfo *rel);
extern List *flatten_clausegroups_list(List *clausegroups);

/*
 * orindxpath.c
 *	  additional routines for indexable OR clauses
 */
extern bool create_or_index_quals(PlannerInfo *root, RelOptInfo *rel);

/*
 * tidpath.h
 *	  routines to generate tid paths
 */
extern void create_tidscan_paths(PlannerInfo *root, RelOptInfo *rel);

/*
 * joinpath.c
 *	   routines to create join paths
 */
extern void add_paths_to_joinrel(PlannerInfo *root, RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 JoinType jointype,
					 List *restrictlist);

/*
 * joinrels.c
 *	  routines to determine which relations to join
 */
extern List *join_search_one_level(PlannerInfo *root, int level,
					  List **joinrels);
extern RelOptInfo *make_join_rel(PlannerInfo *root,
			  RelOptInfo *rel1, RelOptInfo *rel2);
extern bool have_join_order_restriction(PlannerInfo *root,
							RelOptInfo *rel1, RelOptInfo *rel2);

/*
 * equivclass.c
 *	  routines for managing EquivalenceClasses
 */
extern bool process_equivalence(PlannerInfo *root, RestrictInfo *restrictinfo,
					bool below_outer_join);
extern void reconsider_outer_join_clauses(PlannerInfo *root);
extern EquivalenceClass *get_eclass_for_sort_expr(PlannerInfo *root,
						 Expr *expr,
						 Oid expr_datatype,
						 List *opfamilies,
						 Index sortref);
extern void generate_base_implied_equalities(PlannerInfo *root);
extern List *generate_join_implied_equalities(PlannerInfo *root,
								 RelOptInfo *joinrel,
								 RelOptInfo *outer_rel,
								 RelOptInfo *inner_rel);
extern bool exprs_known_equal(PlannerInfo *root, Node *item1, Node *item2);
extern void add_child_rel_equivalences(PlannerInfo *root,
						   AppendRelInfo *appinfo,
						   RelOptInfo *parent_rel,
						   RelOptInfo *child_rel);
extern List *find_eclass_clauses_for_index_join(PlannerInfo *root,
								   RelOptInfo *rel,
								   Relids outer_relids);
extern bool have_relevant_eclass_joinclause(PlannerInfo *root,
								RelOptInfo *rel1, RelOptInfo *rel2);
extern bool has_relevant_eclass_joinclause(PlannerInfo *root,
							   RelOptInfo *rel1);
extern bool eclass_useful_for_merging(EquivalenceClass *eclass,
						  RelOptInfo *rel);

/*
 * pathkeys.c
 *	  utilities for matching and building path keys
 */
typedef enum
{
	PATHKEYS_EQUAL,				/* pathkeys are identical */
	PATHKEYS_BETTER1,			/* pathkey 1 is a superset of pathkey 2 */
	PATHKEYS_BETTER2,			/* vice versa */
	PATHKEYS_DIFFERENT			/* neither pathkey includes the other */
} PathKeysComparison;

extern List *canonicalize_pathkeys(PlannerInfo *root, List *pathkeys);
extern PathKeysComparison compare_pathkeys(List *keys1, List *keys2);
extern bool pathkeys_contained_in(List *keys1, List *keys2);
extern Path *get_cheapest_path_for_pathkeys(List *paths, List *pathkeys,
							   CostSelector cost_criterion);
extern Path *get_cheapest_fractional_path_for_pathkeys(List *paths,
										  List *pathkeys,
										  double fraction);
extern List *build_index_pathkeys(PlannerInfo *root, IndexOptInfo *index,
					 ScanDirection scandir);
extern List *convert_subquery_pathkeys(PlannerInfo *root, RelOptInfo *rel,
						  List *subquery_pathkeys);
extern List *build_join_pathkeys(PlannerInfo *root,
					RelOptInfo *joinrel,
					JoinType jointype,
					List *outer_pathkeys);
extern List *make_pathkeys_for_sortclauses(PlannerInfo *root,
							  List *sortclauses,
							  List *tlist,
							  bool canonicalize);
extern void cache_mergeclause_eclasses(PlannerInfo *root,
						   RestrictInfo *restrictinfo);
extern List *find_mergeclauses_for_pathkeys(PlannerInfo *root,
							   List *pathkeys,
							   bool outer_keys,
							   List *restrictinfos);
extern List *select_outer_pathkeys_for_merge(PlannerInfo *root,
								List *mergeclauses,
								RelOptInfo *joinrel);
extern List *make_inner_pathkeys_for_merge(PlannerInfo *root,
							  List *mergeclauses,
							  List *outer_pathkeys);
extern int pathkeys_useful_for_merging(PlannerInfo *root,
							RelOptInfo *rel,
							List *pathkeys);
extern int	pathkeys_useful_for_ordering(PlannerInfo *root, List *pathkeys);
extern List *truncate_useless_pathkeys(PlannerInfo *root,
						  RelOptInfo *rel,
						  List *pathkeys);
extern bool has_useful_pathkeys(PlannerInfo *root, RelOptInfo *rel);

#endif   /* PATHS_H */
