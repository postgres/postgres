#ifndef GRAPH_UTILS_H
#define GRAPH_UTILS_H
#include "c.h"
#include "nodes/pathnodes.h"
#include <stdbool.h>

typedef struct Vertex {
	RelOptInfo *rel;
	List *adj;
	size_t index;
} Vertex;

typedef enum { CHAIN, CYCLE, STAR, DENSITY_GRAPH, COMPONENT } TypeTopology;
typedef struct Topology {
	List *vertexes;
	uint64 complexity;
	uint64 budget;
	TypeTopology topology;
} Topology;

extern bool has_simple_inner_edge(PlannerInfo *root, RelOptInfo *rel1,
								  RelOptInfo *rel2);
extern List *build_join_graph(PlannerInfo *root, List *initial_rels);
extern List *split_components(List *vertexes);
extern List *find_cycles(List *vertexes, bool *used_vertexes_comp);
extern List *find_stars(List *vertexes, bool *used_vertexes);
extern List *find_remaining_chains(List *vertexes, bool *used_vertexes);
extern List *find_dense_subgraphs(List *vertexes, bool *used_vertexes);
#endif
