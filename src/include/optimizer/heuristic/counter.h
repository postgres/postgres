#ifndef COUNTER_H
#define COUNTER_H

#include "c.h"
#include "nodes/bitmapset.h"
#include "nodes/pathnodes.h"
#include "utils/hsearch.h"

typedef struct HyperNode {
	/*
	 * Bitmap of relations this Hypernode represents.
	 * Must be first field in structure - used as key.
	 */
	bitmapword set;

	/*
	 * Created 'RelOptInfo' for this HyperNode.
	 * During DPhyp algorithm this is not NULL only for base hypernodes.
	 * At the end of algorithm we build RelOptInfo for all
	 */
	RelOptInfo *rel;

	/*
	 * List of Hypernode pairs, that can contribute for creating this HyperNode.
	 * Used as an indicator that this HyperNode has a plan and can be created,
	 * even if actually 'make_join_rel' will not be able to create RelOptInfo
	 * from it.
	 */
	List *candidates;

	/*
	 * Cached bitmap of nodes that are connected with this HyperNode
	 * with simple edges.
	 * Just bit OR of 'simple_edges' of all 'nodes'.
	 */
	bitmapword simple_neighborhood;
} HyperNode;

/*
 * Pair of 'left' and 'right' bitmapwords representing hypernodes.
 * lhs and rhs must not intersect.
 * Each edge stored twice - with lhs/rhs swapped, so no need to swap
 * every time and check again.
 */
typedef struct HyperEdge {
	/* Left side of edge */
	bitmapword left;
	/* Right side of edge */
	bitmapword right;
} HyperEdge;

/*
 * Array of Hyperedges stored as plain C-array instead of List *.
 * Entries stored sorted by tuple (left, right) edges.
 */
typedef struct EdgeArray {
	/* Capacity of 'edges' array */
	int capacity;
	/* Actual size of 'edges' array (payload) */
	int size;
	/* Array of hyperedges */
	HyperEdge *edges;
	/* Size of 'start_idx' array */
	int start_idx_size;
	/* Index storing positions from which to start iterating */
	int8 *start_idx;
} EdgeArray;

/*
 * Structure used as state for enumerating subsets of given bitmap
 */
typedef struct SubsetIteratorState {
	/* Common state for subset iteration */
	/*
	 * Current subset value
	 */
	bitmapword subset;
	/*
	 * Current subset to return. 0 means no more subsets.
	 */
	bitmapword state;
	/*
	 * Initial bitmap that used as mask to iterate.
	 */
	bitmapword init;

	/* State to compute neighborhood using cache */
	/*
	 * Current iteration number. Used to decide which actions to take.
	 */
	bitmapword iteration;

	/*
	 * Cache of neighborhoods for different subsets.
	 * Indexed by number of leading zeros in subset.
	 */
	bitmapword cached_neighborhood[BITS_PER_BITMAPWORD];
} SubsetIteratorState;

/*
 * Context object, that passed along with any function invocation.
 */
typedef struct DPHypContext {
	/*
	 * Original planner info
	 */
	PlannerInfo *root;

	/*
	 * List of initial passed 'RelOptInfo' objects
	 */
	List *initial_rels;

	/*
	 * Number of nodes in current run
	 *
	 * Corresponds with 'simple_hypernodes', 'simple_edges' and
	 * 'complex_edges' arrays sizes
	 */
	int nodes_count;

	/*
	 * List of Hypernodes created for initial relations
	 */
	List *simple_hypernodes;

	/*
	 * Map of base hypernode to bitmaps containing all nodes
	 * that node has simple edge with
	 */
	bitmapword *simple_edges;

	/*
	 * Array of hyperedges appearing in query graph. Index is id
	 * of hypernode and each array contains all hyperedges that
	 * this node appears in
	 */
	EdgeArray *complex_edges;

	/*
	 * Dynamic programming table that maps: bitmapword -> HyperNode
	 */
	HTAB *dptable;
} DPHypContext;

extern void initialize_edges(PlannerInfo *root, List *initial_rels,
							 DPHypContext *context);
extern uint64 count_cc(DPHypContext *context, uint64 max);
#endif
