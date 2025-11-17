#include "postgres.h"

#include <limits.h>

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "optimizer/geqo.h"
#include "optimizer/heuristic/counter.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"

#if PG_MAJORVERSION_NUM < 17
#if BITS_PER_BITMAPWORD == 32
#define bmw_leftmost_one_pos(w) pg_leftmost_one_pos32(w)
#define bmw_rightmost_one_pos(w) pg_rightmost_one_pos32(w)
#else
#define bmw_leftmost_one_pos(w) pg_leftmost_one_pos64(w)
#define bmw_rightmost_one_pos(w) pg_rightmost_one_pos64(w)
#endif
#endif

#if PG_MAJORVERSION_NUM < 15
#define pg_rotate_left32(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
#endif

#define ValidateBmwPosition(x) Assert(0 <= (x) && (x) < BITS_PER_BITMAPWORD)
#define MAKE_BMW(x) ((bitmapword)(1 << (x)))

/*
 * Create bitmapword with single bit set at 'x' position
 */
static inline bitmapword bmw_make_singleton(int x) {
	ValidateBmwPosition(x);
	return MAKE_BMW(x);
}

/*
 * Create bitmapword with all bits prior to 'x' position set
 */
static inline bitmapword bmw_make_b_v(int x) {
	ValidateBmwPosition(x);
	return (((bitmapword)(1 << x)) - 1) | MAKE_BMW(x);
}

/*
 * Add member 'x' to 'bmw' bitmapword
 */
static inline bitmapword bmw_add_member(bitmapword bmw, int x) {
	ValidateBmwPosition(x);
	return bmw | MAKE_BMW(x);
}

/*
 * Get all elements from 'a' without elements from 'b'
 */
static inline bitmapword bmw_difference(bitmapword a, bitmapword b) {
	return a & ~b;
}

/*
 * Check that 'a' is subset of 'b'
 */
static inline bool bmw_is_subset(bitmapword a, bitmapword b) {
	return (a & b) == a;
}

/*
 * Check if 'x' is member of 'bmw'
 */
static inline bitmapword bmw_is_member(bitmapword bmw, int x) {
	ValidateBmwPosition(x);
	return (bmw & MAKE_BMW(x)) != 0;
}

/*
 * Check if 'a' and 'b' have any common members
 */
static inline bool bmw_overlap(bitmapword a, bitmapword b) {
	return (a & b) != 0;
}

/*
 * Get index of first member of bitmapword from start.
 * Used to get representative of hypernode.
 */
static inline int bmw_first(bitmapword bmw) {
	if (bmw == 0)
		return 0;
	else
		return bmw_rightmost_one_pos(bmw);
}

static inline int bmw_lowest_bit(bitmapword bmw) { return bmw & (-bmw); }

/*
 * Get next member of 'bmw' starting from 'prevbit'.
 * Pass -1 to 'prevbit' at the start.
 * Returns -1 if there are no more members.
 */
static inline bitmapword bmw_next_member(bitmapword bmw, int prevbit) {
	bitmapword mask;

	if (prevbit != -1)
		ValidateBmwPosition(prevbit);

	mask = (~(bitmapword)0) << (prevbit + 1);
	bmw &= mask;

	if (bmw == 0)
		return -1;

	return bmw_rightmost_one_pos(bmw);
}

/*
 * Get previous member of 'bmw' starting from 'prevbit'.
 * Pass -1 to 'prevbit' at the start.
 * Returns -1 if there are no more members.
 */
static inline bitmapword bmw_prev_member(bitmapword bmw, int prevbit) {
	bitmapword mask;

	if (prevbit == 0)
		return -1;

	if (prevbit == -1) {
		prevbit = BITS_PER_BITMAPWORD - 1;
	} else {
		ValidateBmwPosition(prevbit);
		--prevbit;
	}

	mask = (~(bitmapword)0) >> (BITS_PER_BITMAPWORD - (prevbit + 1));
	bmw &= mask;

	if (bmw == 0)
		return -1;

	return bmw_leftmost_one_pos(bmw);
}

/*
 * Hash function for bitmapword to be used in HTAB
 */
static inline bitmapword bmw_hash_value(bitmapword x) {
	/* Copied from hashfn.c */
	uint32 a, b, c;

	a = b = c = 0x9e3779b9 + (uint32)sizeof(uint32) + 3923095;
	a += (int32)x;

	c ^= b;
	c -= pg_rotate_left32(b, 14);
	a ^= c;
	a -= pg_rotate_left32(c, 11);
	b ^= a;
	b -= pg_rotate_left32(a, 25);
	c ^= b;
	c -= pg_rotate_left32(b, 16);
	a ^= c;
	a -= pg_rotate_left32(c, 4);
	b ^= a;
	b -= pg_rotate_left32(a, 14);
	c ^= b;
	c -= pg_rotate_left32(b, 24);

#if BITS_PER_BITMAPWORD == 64

	a += (int32)(x >> 32);

	c ^= b;
	c -= pg_rotate_left32(b, 14);
	a ^= c;
	a -= pg_rotate_left32(c, 11);
	b ^= a;
	b -= pg_rotate_left32(a, 25);
	c ^= b;
	c -= pg_rotate_left32(b, 16);
	a ^= c;
	a -= pg_rotate_left32(c, 4);
	b ^= a;
	b -= pg_rotate_left32(a, 14);
	c ^= b;
	c -= pg_rotate_left32(b, 24);

#endif

	/* report the result */
	return c;
}

/*
 * Generic hash function for bitmapword
 */
static inline uint32 bmw_hash(const void *key, Size keysize) {
	Assert(keysize == sizeof(bitmapword));

	return bmw_hash_value(*(const bitmapword *)key);
}

/*
 * Comparison function for bitmapword members in HTAB
 */
static inline int bmw_match(const void *key1, const void *key2, Size keysize) {
	Assert(keysize == sizeof(bitmapword));

	return *(const bitmapword *)key1 != *(const bitmapword *)key2;
}

/*
 * Check that 'bmw' contains only single member 'x'
 */
static inline bool bmw_single_element(bitmapword bmw, int x) {
	ValidateBmwPosition(x);
	return bmw == MAKE_BMW(x);
}

/*
 * Check that 'bmw' has only single bit set.
 * Does not check that 'bmw' is empty.
 */
static inline bool bmw_is_singleton(bitmapword bmw) {
	return (bmw & (bmw - 1)) == 0;
}

/*
 * Check if 'bmw' is empty
 */
static inline bool bmw_is_empty(bitmapword bmw) { return bmw == 0; }

#define IS_ODD(number) (((number) & 1) == 1)

typedef enum CrossJoinStrategy {
	/* No actions are performed - transfer control to DPsize/GEQO */
	CJ_STRATEGY_NO,
	/*
	 * Perform check during edges initialization and create hyperedges for
	 * disjoint hypernodes (relations).
	 */
	CJ_STRATEGY_DETECT,
	/* Collect all relations we were able to create and pass them to DPsize/GEQO
	 */
	CJ_STRATEGY_PASS,
} CrossJoinStrategy;

static const struct config_enum_entry cross_join_strategy_options[] = {
	{"no", CJ_STRATEGY_NO, false},
	{"detect", CJ_STRATEGY_DETECT, false},
	{"pass", CJ_STRATEGY_PASS, false},
	{NULL, 0, false},
};

/* GUC */
/* Extension is enabled and should run DPhyp */
static bool dphyp_enabled = true;
/*
 * In case of CROSS JOINs we can get disjoint subgraphs for tree, so let user
 * decide how to handle them.
 */
static int dphyp_cj_strategy = CJ_STRATEGY_PASS;
/* Minimal number of tables to run DPhyp */
static int dphyp_min_relations = 0;
/* Maximal number of table after which GEQO is used */
static int dphyp_max_relations = 16;
/*
 * Whether we should count number of connected subgraphs.
 * This can be useful, if cc_threshold is disabled, but
 * hash table preallocation can give improvements.
 */
static bool dphyp_count_cc = true;
/* If this amount is hit, then run GEQO */
static int dphyp_geqo_cc_threshold = 10000;

static join_search_hook_type prev_join_search_hook = NULL;

/* Initialization */
static bitmapword map_to_internal_bms(List *initial_rels, Bitmapset *original);
static void process_edge_bms_pair(DPHypContext *context, Bitmapset *lhs,
								  Bitmapset *rhs);
static void distribute_cjs(DPHypContext *context, bitmapword cjs);
static void distribute_hyperedge(DPHypContext *context, HyperEdge edge);
static void initialize_hypernodes(DPHypContext *context,
								  uint64 subgraphs_count);
static void hyperedge_array_add(EdgeArray *array, HyperEdge edge);
static void initialize_start_index(DPHypContext *context);
static HyperEdge hyperedge_swap(HyperEdge edge);
static uint64 count_cc_recursive(DPHypContext *context, bitmapword set,
								 bitmapword excluded, uint64 count,
								 uint64 budget, bitmapword base_neighborhood);

/* Runtime */
static HyperNode *get_hypernode(DPHypContext *context, bitmapword nodes);
static void subset_iterator_init(SubsetIteratorState *state, bitmapword bmw);
static bool subset_iterator_next(SubsetIteratorState *state);
static bitmapword get_neighbors_iter(DPHypContext *context, bitmapword subgroup,
									 bitmapword excluded,
									 SubsetIteratorState *iter_state);
static bitmapword get_neighbors(DPHypContext *context, HyperNode *node,
								bitmapword excluded);
static bitmapword get_neighbors_base(DPHypContext *context, int id,
									 bitmapword excluded);
static int get_start_index(EdgeArray *edges, bitmapword bmw);
static bool hypernode_has_direct_edge_with(DPHypContext *context,
										   HyperNode *node, int id);
static bool hypernode_has_edge_with(DPHypContext *context, HyperNode *node,
									bitmapword bms);
static void emit_csg_cmp(DPHypContext *context, HyperNode *subgroup,
						 HyperNode *complement);
static void enumerate_cmp_recursive(DPHypContext *context, HyperNode *node,
									HyperNode *complement, bitmapword excluded,
									bitmapword neighborhood);
static void emit_csg(DPHypContext *context, HyperNode *node,
					 bitmapword excluded, bitmapword neighborhood);
static void enumerate_csg_recursive(DPHypContext *context, HyperNode *node,
									bitmapword excluded,
									bitmapword neighborhood);
static void solve(DPHypContext *context);
static RelOptInfo *hypernode_get_rel(DPHypContext *context, HyperNode *node);

/* Extension */
static RelOptInfo *dphyp(DPHypContext *context, PlannerInfo *root,
						 List *initial_rels);
static RelOptInfo *dphyp_join_search(PlannerInfo *root, int levels_needed,
									 List *initial_rels);

static inline bool hyperedge_is_simple(HyperEdge edge) {
	return bms_is_singleton(edge.left) && bmw_is_singleton(edge.right);
}

static inline bool hyperedge_is_valid(HyperEdge edge) {
	/*
	 * Vertexes must be not empty and they must not intersect
	 */
	return !(bmw_is_empty(edge.left) || bmw_is_empty(edge.right) ||
			 bmw_overlap(edge.left, edge.right));
}

static inline int hyperedge_cmp(HyperEdge a, HyperEdge b) {
	/* Simple integer tuple (lowest(right), left, right) comparison */
	bitmapword t;

	/* Use lowest_bit instead of bmw_first - same semantic, but faster */
	t = bmw_lowest_bit(a.right) - bmw_lowest_bit(b.right);
	if (t != 0)
		return t;

	t = a.left - b.left;
	if (t != 0)
		return t;

	t = a.right - b.right;
	return t;
}

/*
 * Check that we calculated any query plan for this hypernode
 */
static inline bool hypernode_has_rel(HyperNode *node) {
	return node->rel != NULL || node->candidates != NIL;
}

/* Calculate neighborhood for single base node */
static bitmapword get_neighbors_base(DPHypContext *context, int id,
									 bitmapword excluded) {
	int i;
	bitmapword neighborhood;
	EdgeArray *edges;
	bitmapword set;

	set = bmw_make_singleton(id);
	neighborhood = context->simple_edges[id];

	edges = &context->complex_edges[id];
	i = get_start_index(edges, excluded);
	for (; i < edges->size; ++i) {
		HyperEdge edge = edges->edges[i];
		if (edge.left == set &&
			!bmw_overlap(edge.right, neighborhood | excluded)) {
			neighborhood |= bmw_lowest_bit(edge.right);
		}
	}

	neighborhood = bmw_difference(neighborhood, excluded);
	return neighborhood;
}

/*
 * Get neighborhood of given hypernode excluding 'excluded' set.
 * Neighborhood calculated from ground.
 */
static bitmapword get_neighbors(DPHypContext *context, HyperNode *node,
								bitmapword excluded) {
	bitmapword neighbors;
	int idx;

	excluded |= node->set;
	neighbors = node->simple_neighborhood;

	idx = -1;
	while ((idx = bmw_next_member(node->set, idx)) >= 0) {
		EdgeArray *complex_edges = &context->complex_edges[idx];
		int i = get_start_index(complex_edges, neighbors | excluded);
		for (; i < complex_edges->size; i++) {
			HyperEdge edge = complex_edges->edges[i];
			if (bmw_is_subset(edge.left, node->set) &&
				!bmw_overlap(edge.right, neighbors | excluded)) {
				neighbors |= bmw_lowest_bit(edge.right);
			}
		}
	}

	neighbors = bmw_difference(neighbors, excluded);

	return neighbors;
}

/*
 * Get neighborhood, that should be used as base during cached subset
 * enumeration.
 */
static inline bitmapword
get_parent_neighborhood(DPHypContext *context,
						SubsetIteratorState *iter_state) {
	int zero_count;
	bitmapword last_bit_removed;

	Assert(iter_state->iteration != 0);
	last_bit_removed = bmw_difference(iter_state->iteration,
									  bmw_lowest_bit(iter_state->iteration));
	if (unlikely(bmw_is_empty(last_bit_removed))) {
		/* Don't have any parent */
		return 0;
	}

	zero_count = bmw_rightmost_one_pos(last_bit_removed);
	return iter_state->cached_neighborhood[zero_count];
}

/*
 * Get bitmap of neighbors for node excluding all specified.
 * Corresponds to 'N(S, X)' function in paper.
 */
static bitmapword get_neighbors_iter(DPHypContext *context, bitmapword subgroup,
									 bitmapword excluded,
									 SubsetIteratorState *iter_state) {
	int i;
	int idx;
	bitmapword neighbors;
	EdgeArray *complex_edges;

	excluded |= subgroup;

	iter_state->iteration++;
	Assert(!bmw_is_empty(iter_state->subset));
	idx = bmw_rightmost_one_pos(iter_state->subset);

	/* Starting point is parent neighborhood */
	neighbors = get_parent_neighborhood(context, iter_state);

	/* Add simple neighborhood */
	neighbors |= bmw_difference(context->simple_edges[idx], excluded);

	/* And neighbors from complex edges */
	complex_edges = &context->complex_edges[idx];

	i = get_start_index(complex_edges, neighbors | excluded);
	for (; i < complex_edges->size; i++) {
		HyperEdge edge = complex_edges->edges[i];
		if (bmw_is_subset(edge.left, subgroup) &&
			!bmw_overlap(edge.right, neighbors | excluded)) {
			neighbors |= bmw_lowest_bit(edge.right);
		}
	}

	neighbors = bmw_difference(neighbors, excluded);

	/*
	 * Save current neighborhood in table, but for performance
	 * skip odd-numbered iterations, because they are not used anymore.
	 */
	if (!IS_ODD(iter_state->iteration)) {
		int zero_count;

		zero_count = bmw_rightmost_one_pos(iter_state->iteration);
		iter_state->cached_neighborhood[zero_count] = neighbors;
	}

	return neighbors;
}

/*
 * Check that 'node' has direct edge with node 'id'.
 * This is not the same as 'has_edge_with' because we must check
 * that it has simple edge
 */
static bool hypernode_has_direct_edge_with(DPHypContext *context,
										   HyperNode *node, int id) {
	int i;
	EdgeArray *edges;
	bitmapword right_bmw;

	/* If we have direct simple edge, then we are done */
	if (bmw_is_member(node->simple_neighborhood, id))
		return true;

	/* Otherwise, we may have complex edge with single 'id' node at right side
	 */
	edges = &context->complex_edges[id];

	i = get_start_index(edges, node->set - 1);

	right_bmw = bmw_make_singleton(id);
	for (; i < edges->size; i++) {
		HyperEdge edge = edges->edges[i];
		if (edge.left != right_bmw)
			continue;

		if (bmw_is_subset(edge.right, node->set))
			return true;
	}

	return false;
}

/*
 * Check that 'node' has any edge that can be used as connection to 'bmw'.
 * This is used to check that subgroup and complement can be connected
 * to further call 'emit_csg_cmp' and create join rel for them.
 */
static bool hypernode_has_edge_with(DPHypContext *context, HyperNode *node,
									bitmapword bmw) {
	int idx;

	Assert(!bmw_overlap(node->set, bmw));

	/* Check that we have simple edges that connect to 'bmw' */
	if (bmw_overlap(node->simple_neighborhood, bmw))
		return true;

	/* Now check any complex edge has connection to 'bmw' */
	idx = -1;
	while ((idx = bmw_next_member(node->set, idx)) >= 0) {
		int i;
		EdgeArray *edges = &context->complex_edges[idx];

		/*
		 * We should find edge 'right' side of which is subset of 'bmw'.
		 * You can see that we can skip all 'right's if it has any relations
		 * less than any relation from 'bmw'. Simple case:
		 *
		 * right: 000110
		 *   bmw: 001100
		 *
		 * 'right' is not subset of 'bmw' because of second bit set, so
		 * using 'start_index' on 'right' we can quickly find start index
		 * for traverse.
		 * 'get_start_index' works with 'excluded', but not 'bmw' (as in this
		 * case), but as you can see all leading zeros in 'bmw' can be used
		 * as excluded bitmask - we just decrement 1 and all leading 0 become 1:
		 * 001100 -> 001011
		 */
		i = get_start_index(edges, bmw - 1);
		for (; i < edges->size; i++) {
			HyperEdge edge = edges->edges[i];
			if (bmw_is_subset(edge.left, node->set) &&
				bmw_is_subset(edge.right, bmw))
				return true;
		}
	}

	return false;
}

/*
 * Begin iteration on neighborhood subsets.
 */
static void subset_iterator_init(SubsetIteratorState *state,
								 bitmapword neighborhood) {
	state->init = neighborhood;
	state->state = (-neighborhood) & neighborhood;
	state->subset = 0;
	state->iteration = 0;
}

/*
 * Make iteration step on neighborhood subsets.
 */
static bool subset_iterator_next(SubsetIteratorState *state) {
	/* 'iteration' required only in 'get_neighbors_iter', so it's updated there
	 */
	if (state->state == 0)
		return false;

	state->subset = state->state;
	state->state = (state->state - state->init) & state->init;
	return true;
}

/* Store 'subgraph'/'complement' pair to further use them to search query plan
 */
static void emit_csg_cmp(DPHypContext *context, HyperNode *subgraph,
						 HyperNode *complement) {
	HyperNode *hypernode;

	/*
	 * Now we do not create 'RelOptInfo' for this join, but instead
	 * save pair of hypernodes that can be joined together.
	 *
	 * PostgreSQL's planner designed highly cohesion with DPsize algorithm,
	 * so during processing 1 level of join we just call 'make_join_rel'
	 * with nodes of lower level and add more available paths and at the
	 * end we call 'set_cheapest' to find best paths among discovered.
	 * It would be easier to code to just call 'make_join_rel' here and
	 * 'set_cheapest' at the end, but we can not do this, because
	 * 'make_join_rel' expects that 'set_cheapest' was already called with rel
	 * at lower level. So adding 'make_join_rel' + 'set_cheapest' (and some
	 * other functions) here will add overhead by calling them multiple times
	 * for same rel.
	 */
	hypernode = get_hypernode(context, subgraph->set | complement->set);
	if (hypernode->candidates != NIL) {
		hypernode->candidates = lappend(hypernode->candidates, subgraph);
		hypernode->candidates = lappend(hypernode->candidates, complement);
	} else {
		hypernode->candidates = list_make2(subgraph, complement);
	}
}

/*
 * For given 'complement' of 'subgraph' try to enlarge 'complement' using
 * it's neighborhood.
 */
static void enumerate_cmp_recursive(DPHypContext *context, HyperNode *subgraph,
									HyperNode *complement, bitmapword excluded,
									bitmapword complement_neighborhood) {
	SubsetIteratorState subset_iter;

	Assert(!bmw_is_empty(complement_neighborhood));

	subset_iterator_init(&subset_iter, complement_neighborhood);
	while (subset_iterator_next(&subset_iter)) {
		HyperNode *expanded_complement;

		expanded_complement =
			get_hypernode(context, complement->set | subset_iter.subset);

		if (hypernode_has_rel(expanded_complement) &&
			hypernode_has_edge_with(context, subgraph,
									expanded_complement->set))
			emit_csg_cmp(context, subgraph, expanded_complement);
	}

	excluded |= complement_neighborhood;

	subset_iterator_init(&subset_iter, complement_neighborhood);
	while (subset_iterator_next(&subset_iter)) {
		HyperNode *expanded_complement;
		bitmapword current_neighborhood;
		current_neighborhood =
			get_neighbors_iter(context, complement->set | subset_iter.subset,
							   excluded, &subset_iter);
		if (bmw_is_empty(current_neighborhood))
			continue;

		expanded_complement =
			get_hypernode(context, complement->set | subset_iter.subset);
		enumerate_cmp_recursive(context, subgraph, expanded_complement,
								excluded, current_neighborhood);
	}
}

/* Find complement for specified 'subgraph' */
static void emit_csg(DPHypContext *context, HyperNode *subgraph,
					 bitmapword excluded, bitmapword subgraph_neighborhood) {
	int i;

	Assert(!bmw_is_empty(subgraph_neighborhood));

	i = -1;
	while ((i = bmw_prev_member(subgraph_neighborhood, i)) >= 0) {
		HyperNode *complement;
		bitmapword complement_neighborhood;

		complement = (HyperNode *)list_nth(context->simple_hypernodes, i);

		/*
		 * Here in original paper we create S = {v} and then check that
		 * edge rhs is subset of S.  But as you can see subset of single element
		 * set is that set itself, so we can make optimized searching
		 * for such edge.
		 */
		if (hypernode_has_direct_edge_with(context, subgraph, i))
			emit_csg_cmp(context, subgraph, complement);

		/*
		 * We are iterating backwards on neighbors, so we have to exclude
		 * all nodes lower current, otherwise, we will get duplicates
		 * and execution time will skyrocket.
		 */
		complement_neighborhood = get_neighbors_base(context, i, excluded);
		if (!bmw_is_empty(complement_neighborhood))
			enumerate_cmp_recursive(context, subgraph, complement, excluded,
									complement_neighborhood);
	}
}

/*
 * Expand 'subgraph' using it's neighborhood and try to find complement for it
 */
static void enumerate_csg_recursive(DPHypContext *context, HyperNode *subgraph,
									bitmapword excluded,
									bitmapword subgraph_neighborhood) {
	SubsetIteratorState subset_iter;
	bitmapword expanded_subgraph_excluded;

	Assert(!bmw_is_empty(subgraph_neighborhood));

	/*
	 * Excluded set used in 'EmitCsg' calculated using Bv, but
	 * you can notice, that minimal element in CSG across all
	 * function invocations is the same - it is current element
	 * used in 'Solve', so we can calculate it only once.
	 */
	expanded_subgraph_excluded = bmw_make_b_v(bmw_first(subgraph->set));
	subset_iterator_init(&subset_iter, subgraph_neighborhood);
	while (subset_iterator_next(&subset_iter)) {
		HyperNode *expanded_subgraph;

		expanded_subgraph =
			get_hypernode(context, subgraph->set | subset_iter.subset);
		if (hypernode_has_rel(expanded_subgraph)) {
			bitmapword subgraph_excluded;
			bitmapword expanded_neighborhood;

			/*
			 * Here we can not use caching strategy, because excluded set
			 * is different for each iteration, so fairly calculate it.
			 */
			subgraph_excluded =
				expanded_subgraph->set | expanded_subgraph_excluded;
			expanded_neighborhood =
				get_neighbors(context, expanded_subgraph, subgraph_excluded);
			if (!bmw_is_empty(expanded_neighborhood))
				emit_csg(context, expanded_subgraph, subgraph_excluded,
						 expanded_neighborhood);
		}
	}

	excluded |= subgraph_neighborhood;

	subset_iterator_init(&subset_iter, subgraph_neighborhood);
	while (subset_iterator_next(&subset_iter)) {
		bitmapword current_neighborhood;
		bitmapword expanded_set;
		HyperNode *expanded_subgraph;

		expanded_set = subgraph->set | subset_iter.subset;
		current_neighborhood =
			get_neighbors_iter(context, expanded_set, excluded, &subset_iter);
		if (bmw_is_empty(current_neighborhood))
			continue;

		expanded_subgraph = get_hypernode(context, expanded_set);
		enumerate_csg_recursive(context, expanded_subgraph, excluded,
								current_neighborhood);
	}
}

/* Entry point of DPHyp join search */
static void solve(DPHypContext *context) {
	/*
	 * For initial nodes we must iterate backwards to prevent exploring
	 * duplicates
	 */
	for (int i = context->nodes_count - 1; i >= 0; i--) {
		bitmapword neighborhood;
		bitmapword excluded;
		HyperNode *subgraph =
			(HyperNode *)list_nth(context->simple_hypernodes, i);

		excluded = bmw_make_b_v(i);
		neighborhood = get_neighbors_base(context, i, excluded);

		if (!bmw_is_empty(neighborhood)) {
			emit_csg(context, subgraph, excluded, neighborhood);
			enumerate_csg_recursive(context, subgraph, excluded, neighborhood);
		}

		/*
		 * Add this in case planning will take too long and user
		 * request cancellation.
		 */
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Map Relids specified in 'original' to internal presentation based on id of
 * relation
 */
static bitmapword map_to_internal_bms(List *initial_rels, Bitmapset *original) {
	bitmapword target;
	ListCell *lc;
	int i;

	/*
	 * We must iterate over initial_rels, because RelOptInfo
	 * can represent join, thus it's 'relids' is not singleton.
	 */
	target = 0;
	i = 0;
	foreach (lc, initial_rels) {
		RelOptInfo *rel = (RelOptInfo *)lfirst(lc);
		if (bms_is_subset(rel->relids, original))
			target = bmw_add_member(target, i);
		++i;
	}

	return target;
}

static HyperNode *get_hypernode(DPHypContext *context, bitmapword set) {
	HyperNode *node;
	bitmapword key = set;
	bool found;

	node = hash_search(context->dptable, &key, HASH_ENTER, &found);

	if (!found) {
		int idx;

		node->set = set;
		node->rel = NULL;
		node->candidates = NIL;

		node->simple_neighborhood = 0;
		idx = -1;
		while ((idx = bmw_next_member(set, idx)) >= 0) {
			node->simple_neighborhood |= context->simple_edges[idx];
		}
		node->simple_neighborhood =
			bmw_difference(node->simple_neighborhood, node->set);
	}

	return node;
}

/*
 * Get 'RelOptInfo' for given 'HyperNode' and possibly building it.
 * This is called at the end of DPhyp when we are building plan.
 */
static RelOptInfo *hypernode_get_rel(DPHypContext *context, HyperNode *node) {
	ListCell *lc;
	RelOptInfo *final_rel;
	HyperNode *left_node;
	HyperNode *right_node;

	/*
	 * To build final relation we act like vanilla PostgreSQL, but in
	 * top-down approach - build final relation by recursively building
	 * all candidates.
	 */
	if (node->rel != NULL)
		return node->rel;

	/*
	 * 'candidates' list stores plain array, but each iteration we should
	 * process 2 items. This is more effective than creating new objects
	 * (i.e. 'list_make2') for each pair - such allocations took near 1%
	 * of overall performance.
	 */
	final_rel = NULL;
	left_node = NULL;
	right_node = NULL;
	foreach (lc, node->candidates) {
		RelOptInfo *left_rel;
		RelOptInfo *right_rel;
		RelOptInfo *join_rel;

		if (left_node == NULL) {
			left_node = (HyperNode *)lfirst(lc);
			continue;
		}

		right_node = (HyperNode *)lfirst(lc);

		left_rel = hypernode_get_rel(context, left_node);
		if (left_rel == NULL)
			goto loop_end;

		right_rel = hypernode_get_rel(context, right_node);
		if (right_rel == NULL)
			goto loop_end;

		join_rel = make_join_rel(context->root, left_rel, right_rel);
		if (join_rel == NULL)
			goto loop_end;

		if (final_rel == NULL)
			final_rel = join_rel;

	loop_end:
		left_node = NULL;
		right_node = NULL;
	}

	if (final_rel == NULL) {
		/*
		 * If we are here, then we are unable to create rel from this node,
		 * then mark this node as invalid to prevent multiple recursive calls
		 * by clearing candidate List.
		 */
		node->candidates = NIL;
		return NULL;
	}

	generate_partitionwise_join_paths(context->root, final_rel);
#if PG_MAJORVERSION_NUM < 16
	if (!bms_equal(context->root->all_baserels, final_rel->relids))
#else
	if (!bms_equal(context->root->all_query_rels, final_rel->relids))
#endif
		generate_useful_gather_paths(context->root, final_rel, false);
	set_cheapest(final_rel);
	node->rel = final_rel;
	return final_rel;
}

static void initialize_hypernodes(DPHypContext *context,
								  uint64 subgraphs_count) {
	ListCell *lc;
	HTAB *dptable;
	HASHCTL hctl;
	int i;

	/* Initial size of HTAB given from 'build_join_rel_hash' */
	hctl.keysize = sizeof(bitmapword);
	hctl.entrysize = sizeof(HyperNode);
	hctl.hash = bmw_hash;
	hctl.match = bmw_match;
	hctl.hcxt = CurrentMemoryContext;
	dptable = (HTAB *)hash_create(
		"DPhyp hypernode table", subgraphs_count, &hctl,
		HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
	i = 0;
	foreach (lc, context->initial_rels) {
		RelOptInfo *rel = (RelOptInfo *)lfirst(lc);
		HyperNode *entry;
		bitmapword set;
		bool found;

		set = bmw_make_singleton(i);
		entry = (HyperNode *)hash_search(dptable, &set, HASH_ENTER, &found);

		Assert(!found);

		entry->rel = rel;
		entry->candidates = NIL;
		entry->set = set;
		entry->simple_neighborhood = context->simple_edges[i];

		context->simple_hypernodes = lappend(context->simple_hypernodes, entry);
		++i;
	}

	context->dptable = dptable;
}

/* Structure that stores information of Union/Set algorithm */
typedef struct us_state {
	/* Array of leaders */
	int *leaders;
	/* Array of ranks for each node */
	int *ranks;
	/* Size of 'leaders' and 'ranks' arrays */
	int size;
} us_state;

static void us_init(us_state *state, int size) {
	int *leaders;
	int *ranks;

	leaders = palloc(sizeof(int) * size);
	for (size_t i = 0; i < size; i++)
		leaders[i] = i;

	ranks = palloc0(sizeof(int) * size);

	state->leaders = leaders;
	state->ranks = ranks;
	state->size = size;
}

static int us_leader(us_state *state, int node) {
	Assert(node < state->size);
	if (state->leaders[node] == node)
		return node;
	else
		return state->leaders[node] = us_leader(state, state->leaders[node]);
}

static void us_union(us_state *state, int a, int b) {
	int a_leader;
	int b_leader;

	a_leader = us_leader(state, a);
	b_leader = us_leader(state, b);

	if (state->ranks[a_leader] == state->ranks[b_leader])
		state->ranks[a_leader]++;

	if (state->ranks[a_leader] < state->ranks[b_leader])
		state->leaders[a_leader] = b_leader;
	else
		state->leaders[b_leader] = a_leader;
}

/*
 * Detect if all nodes are already connected, so we can stop
 * and do not process remaining elements.
 */
static bool us_all_connected(us_state *state) {
	int prev_leader = -1;
	for (size_t i = 0; i < state->size; i++) {
		int leader = us_leader(state, i);
		if (prev_leader == -1)
			prev_leader = leader;
		else if (prev_leader != leader)
			return false;
	}

	return true;
}

static bitmapword *us_collect(us_state *state, int *size) {
	bitmapword *disjoint_sets;
	bitmapword *result;
	int result_size;
	int idx;

	disjoint_sets = palloc0(sizeof(bitmapword) * state->size);
	result_size = 0;
	for (size_t i = 0; i < state->size; i++) {
		int leader = us_leader(state, i);
		if (bmw_is_empty(disjoint_sets[leader])) {
			disjoint_sets[leader] = bmw_make_singleton(i);
			result_size++;
		} else
			disjoint_sets[leader] = bmw_add_member(disjoint_sets[leader], i);
	}

	result = palloc(sizeof(bitmapword) * result_size);
	idx = 0;
	for (size_t i = 0; i < state->size; i++) {
		if (bmw_is_empty(disjoint_sets[i]))
			continue;

		result[idx] = disjoint_sets[i];
		idx++;
	}

	pfree(disjoint_sets);
	*size = result_size;
	return result;
}

static void us_free(us_state *state) {
	pfree(state->leaders);
	pfree(state->ranks);
}

/*
 * Add hyperedge to sorted array omitting duplicates
 */
static void hyperedge_array_add(EdgeArray *array, HyperEdge edge) {
	int low;
	int high;
	int mid;

	if (array->size == 0) {
		/* If array is empty just do allocation and insert edge */
		array->capacity = 4;
		array->size = 1;
		array->edges = palloc(sizeof(HyperEdge) * array->capacity);
		array->edges[0] = edge;
		return;
	}

	/*
	 * Edge array stored in sorted way for 2 reasons:
	 *
	 * 1. make use of 'start_index'
	 * 2. prevent duplicates in different hyperedges representation
	 *
	 * So, here we just perform binary search to find insertion place
	 * and also comparison tells us if there is any duplicate.
	 */
	low = 0;
	high = array->size;
	while (low < high) {
		int cmp;
		mid = low + ((high - low) / 2);

		cmp = hyperedge_cmp(edge, array->edges[mid]);

		/* Found duplicate */
		if (cmp == 0)
			return;

		if (cmp < 0)
			high = mid;
		else
			low = mid + 1;
	}

	/* Suitable position found - adjust edges and insert */
	if (array->size == array->capacity) {
		array->capacity *= 2;
		array->edges =
			repalloc(array->edges, sizeof(HyperEdge) * array->capacity);
	}

	Assert(low <= array->size);
	if (low == array->size) {
		array->edges[array->size] = edge;
	} else {
		memmove(&array->edges[low + 1], &array->edges[low],
				sizeof(HyperEdge) * (array->size - low));
		array->edges[low] = edge;
	}

	array->size++;
}

/*
 * Create new hyperedge with left and right parts swapped
 */
static inline HyperEdge hyperedge_swap(HyperEdge edge) {
	HyperEdge new_edge;
	new_edge.left = edge.right;
	new_edge.right = edge.left;
	return new_edge;
}

/*
 * Save given simple hyperedge to simple neighborhood
 */
static void distribute_simple_hyperedge(DPHypContext *context, HyperEdge edge) {
	bitmapword left_bmw;
	bitmapword right_bmw;
	int left_idx;
	int right_idx;

	Assert(hyperedge_is_simple(edge));

	left_idx = bmw_first(edge.left);
	right_idx = bmw_first(edge.right);

	left_bmw = context->simple_edges[left_idx];
	right_bmw = context->simple_edges[right_idx];
	context->simple_edges[left_idx] = bmw_add_member(left_bmw, right_idx);
	context->simple_edges[right_idx] = bmw_add_member(right_bmw, left_idx);
}

/*
 * Determine hyperedge's shape and store in suitable place
 */
static void distribute_hyperedge(DPHypContext *context, HyperEdge edge) {
	Assert(hyperedge_is_valid(edge));

	if (hyperedge_is_simple(edge)) {
		distribute_simple_hyperedge(context, edge);
	} else {
		/* Add hyperedge only to it's representative, not every node in vertexes
		 */
		hyperedge_array_add(&context->complex_edges[bmw_first(edge.left)],
							edge);
		distribute_cjs(context, edge.left);
		edge = hyperedge_swap(edge);
		hyperedge_array_add(&context->complex_edges[bmw_first(edge.left)],
							edge);
		distribute_cjs(context, edge.left);
	}
}

/*
 * Distribute cross join set - create all simple hyperedge pairs
 */
static void distribute_cjs(DPHypContext *context, bitmapword cjs) {
	HyperEdge edge;
	int idx1;
	int idx2;

	if (bmw_is_empty(cjs) || bmw_is_singleton(cjs))
		return;

	idx1 = -1;
	while ((idx1 = bmw_next_member(cjs, idx1)) >= 0) {
		edge.left = bmw_make_singleton(idx1);
		idx2 = idx1;

		while ((idx2 = bmw_next_member(cjs, idx2)) >= 0) {
			edge.right = bmw_make_singleton(idx2);
			distribute_simple_hyperedge(context, edge);
		}
	}
}

/*
 * Collect and return all disjoint sets of nodes.
 * If all nodes are connected, then NULL is returned, otherwise
 * array of 'bitmapword' representing such connected subgraphs
 * returned and 'out_size' is set to it's size.
 */
static bitmapword *collect_disjoint_sets(DPHypContext *context, int *out_size) {
	us_state state;
	bitmapword *disjoint_sets;

	us_init(&state, context->nodes_count);
	for (int i = 0; i < context->nodes_count; ++i) {
		bitmapword simple_edge = context->simple_edges[i];
		int idx;

		idx = -1;
		while ((idx = bmw_next_member(simple_edge, idx)) >= 0) {
			us_union(&state, i, idx);
		}
	}

	/*
	 * As simple heuristic we may find that 'simple_edges' can detect
	 * that all nodes are connected to each other and we can stop now.
	 */
	if (us_all_connected(&state)) {
		us_free(&state);
		return NULL;
	}

	/*
	 * Disjoint sets exist and we have to generate hyperedges
	 * covering all such disjoint sets.  So process complex edges,
	 * collect disjoint sets and generate hyperedges.
	 */
	for (int i = 0; i < context->nodes_count; ++i) {
		EdgeArray *edges = &context->complex_edges[i];

		for (int j = 0; j < edges->size; ++j) {
			HyperEdge edge = edges->edges[j];
			List *left_vertices;
			int idx;

			idx = -1;
			left_vertices = NIL;
			while ((idx = bmw_next_member(edge.left, idx)) >= 0)
				left_vertices = lappend_int(left_vertices, idx);

			idx = -1;
			while ((idx = bmw_next_member(edge.right, idx)) >= 0) {
				ListCell *lc;
				foreach (lc, left_vertices) {
					int left_vertex = lfirst_int(lc);
					us_union(&state, left_vertex, idx);
				}
			}

			list_free(left_vertices);
		}
	}

	disjoint_sets = us_collect(&state, out_size);
	us_free(&state);
	if (*out_size <= 1) {
		/* All nodes are connected to each other */
		if (disjoint_sets != NULL)
			pfree(disjoint_sets);
		return NULL;
	}

	return disjoint_sets;
}

static List *collect_disjoint_rels(DPHypContext *context) {
	List *result;
	int disjoint_sets_size;
	bitmapword *disjoint_sets;

	disjoint_sets = collect_disjoint_sets(context, &disjoint_sets_size);
	if (disjoint_sets == NULL)
		return NIL;

	/* For each disjoint set collect it's RelOptInfo (build lazy) */
	result = NIL;
	for (int i = 0; i < disjoint_sets_size; ++i) {
		bitmapword set = disjoint_sets[i];
		RelOptInfo *rel;
		HyperNode *node;

		node = hash_search(context->dptable, &set, HASH_FIND, NULL);
		if (!(node && hypernode_has_rel(node))) {
			list_free(result);
			result = NIL;
			break;
		}

		rel = hypernode_get_rel(context, node);
		if (!rel) {
			/* This relation is unable to build */
			list_free(result);
			result = NIL;
			break;
		}

		result = lappend(result, rel);
	}

	pfree(disjoint_sets);
	return result;
}

/*
 * Get start index to iterate over complex edges.
 * Suitable index found using 'excluded' set - first 0 from start.
 */
static int get_start_index(EdgeArray *edges, bitmapword excluded) {
	int index;
	int lowest_bit;

	if (edges->start_idx_size == 0)
		return edges->size;

	/*
	 * 'start_idx' primarily used to effectively truncate edges that will not
	 * satisfy 'bmw_overlaps' with 'excluded' set of nodes.
	 * The main observation is that often we have all leading 1 in 'excluded',
	 * so right vertex in any edge with first bit in that range definitely will
	 * return 'false'.
	 * To address this 'start_idx' is used. It is an array:
	 *
	 * [number of leading 0] -> index in 'edges' array
	 *
	 * 'edges' array is sorted by number of leading 0, so we can assert that
	 * if we have 0010 then 0100 will also not overlap with 0001.
	 *
	 * To search suitable position we find first 0 bit after some leading 1.
	 * This is done by inverse - add 1 to sequence of leading 1 and count
	 * produced amount of 0. e.g.
	 *
	 * 1001111 + 1 -> 1010000 (4 leading 1s == 4 leading 0s)
	 *
	 * This is also used when finding connection between hypernodes.
	 * Difference is that caller must decrement 1 from right side (for
	 * which to check for subset), e.g.
	 *
	 * 01010100 - 1 -> 01010011
	 */
	Assert(excluded != ~((bitmapword)0));
	lowest_bit = bmw_rightmost_one_pos(excluded + 1);

	if (edges->start_idx_size <= lowest_bit)
		return edges->size;

	index = (int)edges->start_idx[lowest_bit];
	Assert(0 <= index && index < BITS_PER_BITMAPWORD);
	return index;
}

/*
 * Initialize 'start_index' for each node with complex edges.
 */
static void initialize_start_index(DPHypContext *context) {
	for (size_t i = 0; i < context->nodes_count; i++) {
		EdgeArray *edges = &context->complex_edges[i];
		char prev_idx;
		int prev_lowest;

		if (edges->size == 0) {
			edges->start_idx = NULL;
			edges->start_idx_size = 0;
			continue;
		}

		/*
		 * Array indexed by number of bits, so there 2 observations:
		 *
		 * 1. Maximum useful size of this index does not exceed largest
		 *    number of leading bits, so we allocate that amount.
		 *    Array is sorted, so just get size of last hyperedge.
		 * 2. We should reserve special value for 0 number of set bits. This
		 *    value always is 0 (have to traverse all array).
		 */
		edges->start_idx_size =
			bmw_first(edges->edges[edges->size - 1].right) + 1;
		edges->start_idx = palloc(sizeof(int8) * edges->start_idx_size);

		if (edges->size == 1) {
			/*
			 * In case of simple query there may be single complex edge.
			 * You can observe, that this will be array of 0.
			 */
			memset(edges->start_idx, 0, sizeof(int8) * edges->start_idx_size);
			continue;
		}

		/* Set -1 as indicator, that we do not have value set yet */
		memset(edges->start_idx, -1, sizeof(int8) * edges->start_idx_size);

		edges->start_idx[0] = 0;
		prev_lowest = 0;

		/*
		 * Proceed in 2 runs:
		 *
		 * 1. Iterate over all edges and for each possible leading zero bit
		 *    count save position where it starts. Here we use knowledge,
		 *    that hyperedges are sorted, so just track previous 'lowest'
		 *    number and compare with current
		 * 2. Iterate over 'start_index' array and fill missing indexes.
		 *    If value is absent (-1), then set it to previous value (we
		 *    iterate left->right).
		 */

		/* First run - set all possible values */
		for (size_t j = 0; j < edges->size; j++) {
			int cur_lowest = bmw_first(edges->edges[j].right);
			if (cur_lowest == prev_lowest)
				continue;

			prev_lowest = cur_lowest;
			edges->start_idx[cur_lowest] = j;
		}

		/* Second run - fill missing indexes */
		prev_idx = 0;
		for (size_t j = 0; j < edges->start_idx_size; j++) {
			if (edges->start_idx[j] == -1)
				edges->start_idx[j] = prev_idx;
			else
				prev_idx = edges->start_idx[j];
		}
	}
}

/*
 * For given non-overlapping {left}-{right} Bitmapset pair
 * create associated hyperedge and distribute it.
 */
static void process_edge_bms_pair(DPHypContext *context, Bitmapset *lhs,
								  Bitmapset *rhs) {
	HyperEdge edge;

	edge.left = map_to_internal_bms(context->initial_rels, lhs);
	if (bmw_is_empty(edge.left))
		return;
	edge.right = map_to_internal_bms(context->initial_rels, rhs);
	if (bmw_is_empty(edge.right))
		return;

	distribute_hyperedge(context, edge);
}

static uint64 count_cc_recursive(DPHypContext *context, bitmapword subgraph,
								 bitmapword excluded, uint64 count, uint64 max,
								 bitmapword base_neighborhood) {
	SubsetIteratorState subset_iter;
	subset_iterator_init(&subset_iter, base_neighborhood);
	while (subset_iterator_next(&subset_iter)) {
		bitmapword set;
		bitmapword excluded_ext;
		bitmapword neighborhood;

		count++;
		if (count > max)
			break;

		excluded_ext = excluded | base_neighborhood;
		set = subgraph | subset_iter.subset;
		neighborhood =
			get_neighbors_iter(context, set, excluded_ext, &subset_iter);
		count = count_cc_recursive(context, set, excluded_ext, count, max,
								   neighborhood);
	}

	return count;
}

/*
 * Count number of connected subgraphs for this graph.
 * Function taken from "Adaptive Optimization of Very Large Join Queries".
 */
uint64 count_cc(DPHypContext *context, uint64 max) {
	int64 count = 0;
	int rels_count;

	rels_count = list_length(context->initial_rels);
	for (size_t i = 0; i < rels_count; i++) {
		bitmapword excluded;
		bitmapword neighborhood;

		count++;
		if (count > max)
			break;

		excluded = bmw_make_b_v(i);
		neighborhood = get_neighbors_base(context, i, excluded);
		count = count_cc_recursive(context, bmw_make_singleton(i), excluded,
								   count, max, neighborhood);
	}

	return count;
}
/*
 * Initialize all hyperedges including simple neighborhoods for
 * all simple hyperedges.
 */
void initialize_edges(PlannerInfo *root, List *initial_rels,
					  DPHypContext *context) {
	ListCell *lc1;
	ListCell *lc2;
	bool has_eclass_joins;

	/*
	 * Edges for algorithm taken from 3 places:
	 *
	 * 1. RelOptInfo->joininfo - generic expressions
	 * 2. PlannerInfo->eq_classes - INNER equi-joins
	 * 3. PlannerInfo->join_info_list - non-INNER joins
	 *
	 * Note that this does not cover all possible edges.
	 *
	 * Example is 'joininfo' which can have multiple versions for same
	 * expression, but different Relids for left/right side.
	 * This can arise  from non-INNER joins where such redundant RestrictInfo's
	 * store different set of required relations (which turn into vertices of
	 * hyperedges).
	 * When this happens in RestrictInfo->syn_
	 */
	context->nodes_count = list_length(initial_rels);
	context->simple_edges =
		palloc0(sizeof(bitmapword) * list_length(initial_rels));
	context->complex_edges =
		palloc0(sizeof(EdgeArray) * list_length(initial_rels));

	has_eclass_joins = false;
	foreach (lc1, initial_rels) {
		RelOptInfo *rel = (RelOptInfo *)lfirst(lc1);

		if (rel->has_eclass_joins)
			has_eclass_joins = true;

		foreach (lc2, rel->joininfo) {
			RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc2);

			if (!bms_is_empty(rinfo->left_relids) &&
				!bms_is_empty(rinfo->right_relids) &&
				!bms_overlap(rinfo->left_relids, rinfo->right_relids)) {
				process_edge_bms_pair(context, rinfo->left_relids,
									  rinfo->right_relids);
			} else {
				/*
				 * For CJS we must generate all pairs of simple hypernodes
				 */
				bitmapword required_nodes =
					map_to_internal_bms(initial_rels, rinfo->required_relids);

				if (bmw_is_empty(required_nodes) ||
					bmw_is_singleton(required_nodes))
					continue;

				distribute_cjs(context, required_nodes);
			}
		}
	}

	if (has_eclass_joins) {
		/*
		 * Now, we must traverse through all eclasses that can be used as join
		 * clauses and generate edges for them
		 */
		foreach (lc1, root->eq_classes) {
			EquivalenceClass *eclass = (EquivalenceClass *)lfirst(lc1);
			bitmapword *eclass_nodes;
			int eclass_nodes_size;

			/* There are definitely no join clauses */
			if (bms_membership(eclass->ec_relids) != BMS_MULTIPLE)
				continue;

			eclass_nodes =
				palloc0(sizeof(bitmapword) * list_length(eclass->ec_members));
			eclass_nodes_size = 0;

			foreach (lc2, eclass->ec_members) {
				EquivalenceMember *member = (EquivalenceMember *)lfirst(lc2);

				if (member->em_is_const || bms_is_empty(member->em_relids))
					continue;

				eclass_nodes[eclass_nodes_size] =
					map_to_internal_bms(initial_rels, member->em_relids);
				if (bmw_is_empty(eclass_nodes[eclass_nodes_size]))
					continue;
				eclass_nodes_size++;
			}

			if (eclass_nodes_size == 0) {
				pfree(eclass_nodes);
				continue;
			}

			for (int i = 0; i < eclass_nodes_size; i++) {
				bitmapword left = eclass_nodes[i];
				for (int j = i + 1; j < eclass_nodes_size; j++) {
					bitmapword right = eclass_nodes[j];

					if (bmw_overlap(left, right)) {
						distribute_cjs(context, left | right);
					} else {
						HyperEdge edge;
						edge.left = left;
						edge.right = right;
						distribute_hyperedge(context, edge);
					}
				}
			}

			pfree(eclass_nodes);
		}
	}

	/*
	 * Join order restrictions also impose restrictions on join order.
	 */
	foreach (lc1, root->join_info_list) {
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *)lfirst(lc1);

		process_edge_bms_pair(context, sjinfo->syn_lefthand,
							  sjinfo->syn_righthand);
		process_edge_bms_pair(context, sjinfo->min_lefthand,
							  sjinfo->min_righthand);
	}

	if (dphyp_cj_strategy == CJ_STRATEGY_DETECT) {
		/* Generate all hyperedges for each disjoint set */
		bitmapword *disjoint_sets;
		int disjoint_sets_size;

		disjoint_sets = collect_disjoint_sets(context, &disjoint_sets_size);
		if (disjoint_sets != NULL && 1 < disjoint_sets_size) {
			for (size_t i = 0; i < disjoint_sets_size - 1; i++) {
				bitmapword left = disjoint_sets[i];
				for (size_t j = i + 1; j < disjoint_sets_size; j++) {
					bitmapword right = disjoint_sets[j];
					HyperEdge edge;

					edge.left = left;
					edge.right = right;

					distribute_hyperedge(context, edge);
				}
			}

			pfree(disjoint_sets);
		}
	}

	initialize_start_index(context);
}
