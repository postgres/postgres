/*-------------------------------------------------------------------------
 *
 * pgpa_join.h
 *	  analysis of joins in Plan trees
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_join.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPA_JOIN_H
#define PGPA_JOIN_H

#include "nodes/plannodes.h"

typedef struct pgpa_plan_walker_context pgpa_plan_walker_context;
typedef struct pgpa_join_unroller pgpa_join_unroller;
typedef struct pgpa_unrolled_join pgpa_unrolled_join;

/*
 * Although there are three main join strategies, we try to classify things
 * more precisely here: merge joins have the option of using materialization
 * on the inner side, and nested loops can use either materialization or
 * memoization.
 */
typedef enum
{
	JSTRAT_MERGE_JOIN_PLAIN = 0,
	JSTRAT_MERGE_JOIN_MATERIALIZE,
	JSTRAT_NESTED_LOOP_PLAIN,
	JSTRAT_NESTED_LOOP_MATERIALIZE,
	JSTRAT_NESTED_LOOP_MEMOIZE,
	JSTRAT_HASH_JOIN
	/* update NUM_PGPA_JOIN_STRATEGY if you add anything here */
} pgpa_join_strategy;

#define NUM_PGPA_JOIN_STRATEGY		((int) JSTRAT_HASH_JOIN + 1)

/*
 * In an outer-deep join tree, every member of an unrolled join will be a scan,
 * but join trees with other shapes can contain unrolled joins.
 *
 * The plan node we store here will be the inner or outer child of the join
 * node, as appropriate, except that we look through subnodes that we regard as
 * part of the join method itself. For instance, for a Nested Loop that
 * materializes the inner input, we'll store the child of the Materialize node,
 * not the Materialize node itself.
 *
 * If setrefs processing elided one or more nodes from the plan tree, then
 * we'll store details about the topmost of those in elided_node; otherwise,
 * it will be NULL.
 *
 * Exactly one of scan and unrolled_join will be non-NULL.
 */
typedef struct
{
	Plan	   *plan;
	ElidedNode *elided_node;
	struct pgpa_scan *scan;
	pgpa_unrolled_join *unrolled_join;
} pgpa_join_member;

/*
 * We convert outer-deep join trees to a flat structure; that is, ((A JOIN B)
 * JOIN C) JOIN D gets converted to outer = A, inner = <B C D>.  When joins
 * aren't outer-deep, substructure is required, e.g. (A JOIN B) JOIN (C JOIN D)
 * is represented as outer = A, inner = <B X>, where X is a pgpa_unrolled_join
 * covering C-D.
 */
struct pgpa_unrolled_join
{
	/* Outermost member; must not itself be an unrolled join. */
	pgpa_join_member outer;

	/* Number of inner members. Length of the strategy and inner arrays. */
	unsigned	ninner;

	/* Array of strategies, one per non-outermost member. */
	pgpa_join_strategy *strategy;

	/* Array of members, excluding the outermost. Deepest first. */
	pgpa_join_member *inner;
};

/*
 * Does this plan node inherit from Join?
 */
static inline bool
pgpa_is_join(Plan *plan)
{
	return IsA(plan, NestLoop) || IsA(plan, MergeJoin) || IsA(plan, HashJoin);
}

extern pgpa_join_unroller *pgpa_create_join_unroller(void);
extern void pgpa_unroll_join(pgpa_plan_walker_context *walker,
							 Plan *plan, bool beneath_any_gather,
							 pgpa_join_unroller *join_unroller,
							 pgpa_join_unroller **outer_join_unroller,
							 pgpa_join_unroller **inner_join_unroller);
extern pgpa_unrolled_join *pgpa_build_unrolled_join(pgpa_plan_walker_context *walker,
													pgpa_join_unroller *join_unroller);
extern void pgpa_destroy_join_unroller(pgpa_join_unroller *join_unroller);

#endif
