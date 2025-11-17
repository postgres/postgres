#include "optimizer/heuristic/graph_utils.h"
#include "c.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "optimizer/heuristic/counter.h"
#include "optimizer/joininfo.h"
#include "optimizer/paths.h"
#include "postgres.h"
#include <limits.h>

static const uint64 dphyp_geqo_cc_threshold = 10000;
static const double THRESH = 0.9;
static const uint64 border_chain = 1000;
static const uint64 border_cycle = 1000;
static const uint64 border_star = 1000;
static const uint64 border_density_graph = 1000;

static void set_complexity_topology(Topology *topology);
static void set_complexity_component(PlannerInfo *root, Topology *component);
static List *dfs_component(Vertex *v, List *comp, bool *used_vertexes);
static List *dfs(Vertex *start, Vertex *cur, List *stack, List *cycles,
				 bool *visited,
				 bool *used_vertexes_comp); // stack is list of Vertex*
static bool is_star(Vertex *center, const bool *used_vertexes);
static List *find_star(Vertex *center, bool *used_vertexes);
static void print_graph(List *graph);
static void print_list_vertexes(List *vertexes);
static Vertex *find_min_degree_vertex(List *sub);
static double density(List *sub);
static int count_edges(List *sub);
static uint64 binom_centr(uint64 n, uint64 n2);
////////////////////////////////////

bool has_simple_inner_edge(PlannerInfo *root, RelOptInfo *rel1,
						   RelOptInfo *rel2) {
	bool result = !bms_overlap(rel1->relids, rel2->relids) &&
				  (have_relevant_joinclause(root, rel1, rel2) ||
				   have_join_order_restriction(root, rel1, rel2));
	return result;
}

List *build_join_graph(PlannerInfo *root, List *initial_rels) {
	List *vertexes = NIL;
	size_t index = 0;
	ListCell *lc;
	foreach (lc, initial_rels) {
		RelOptInfo *rel = (RelOptInfo *)lfirst(lc);
		Vertex *v = (Vertex *)palloc0(sizeof(Vertex));
		v->rel = rel;
		v->adj = NIL;
		v->index = index++;
		vertexes = lappend(vertexes, v);
	}
	ListCell *i;
	ListCell *j;

	foreach (i, vertexes) {
		foreach (j, vertexes)
			if (i != j) {
				Vertex *vi = (Vertex *)lfirst(i);
				Vertex *vj = (Vertex *)lfirst(j);
				if (has_simple_inner_edge(root, vi->rel, vj->rel)) {
					vi->adj = lappend(vi->adj, vj);
					vj->adj = lappend(vj->adj, vi);
				}
			}
	}
	print_graph(vertexes);
	return vertexes;
}
static void print_graph(List *graph) {
	ListCell *lc;
	foreach (lc, graph) {
		Vertex *vertex = (Vertex *)lfirst(lc);
		StringInfoData buf;
		initStringInfo(&buf);
		appendStringInfo(&buf, "%p :", (void *)vertex);
		ListCell *lc2;
		foreach (lc2, vertex->adj) {
			Vertex *neighbor = (Vertex *)lfirst(lc2);
			appendStringInfo(&buf, " %p", (void *)neighbor);
		}
		ereport(NOTICE, (errmsg("%s\n", buf.data)));
		pfree(buf.data);
	}
}
static void print_list_vertexes(List *vertexes) {
	if (vertexes == NIL) {
		return;
	}

	StringInfoData buf;
	initStringInfo(&buf);
	ListCell *lc;

	foreach (lc, vertexes) {
		Vertex *v = (Vertex *)lfirst(lc);
		appendStringInfo(&buf, "%p ", (void *)v);
	}

	ereport(NOTICE, (errmsg("%s\n", buf.data)));
	pfree(buf.data);
}
static List *dfs_component(Vertex *v, List *comp, bool *used_vertexes) {
	used_vertexes[v->index] = true;
	comp = lappend(comp, v);
	ListCell *lc;
	foreach (lc, v->adj) {
		Vertex *next = (Vertex *)lfirst(lc);
		if (!used_vertexes[next->index]) {
			comp = dfs_component(next, comp, used_vertexes);
		}
	}
	return comp;
}

List *split_components(List *vertexes) {
	List *comps = NIL; // List* of Component*
	int number_of_rels = list_length(vertexes);
	bool *used_vertexes = (bool *)palloc0(number_of_rels * sizeof(bool));
	ListCell *lc;
	foreach (lc, vertexes) {
		Vertex *v = (Vertex *)lfirst(lc);
		if (!used_vertexes[v->index]) {
			Topology *component = (Topology *)palloc0(sizeof(Topology));
			List *sub = NIL;
			sub = dfs_component(v, sub, used_vertexes);
			component->vertexes = sub;
			set_complexity_topology(component);
			comps = lappend(comps, component);
		}
	}
	pfree(used_vertexes);
	return comps;
}

static List *dfs(Vertex *start, Vertex *cur, List *stack, List *cycles,
				 bool *visited, bool *used_vertexes_comp) {
	visited[cur->index] = true;
	stack = lappend(stack, cur);
	ListCell *lc;
	foreach (lc, cur->adj) {
		Vertex *nbr = (Vertex *)lfirst(lc);

		if (nbr->index < start->index) {
			continue;
		}

		if (nbr == start) {
			if (stack->length >= 3) {
				List *cycle = NIL;
				ListCell *lc2;
				foreach (lc2, stack) {
					Vertex *it = (Vertex *)lfirst(lc2);
					cycle = lappend(cycle, it);
					used_vertexes_comp[it->index] = true;
				}
				cycles = lappend(cycles, cycle);
				break;
			}
		} else if (!visited[nbr->index] && !used_vertexes_comp[nbr->index]) {
			cycles =
				dfs(start, nbr, stack, cycles, visited, used_vertexes_comp);
		}
	}

	visited[cur->index] = false;
	stack = list_delete_nth_cell(stack, stack->length - 1);
	return cycles;
}

List *find_cycles(List *vertexes, bool *used_vertexes_comp) {
	int nverts_global = list_length(vertexes);
	List *cycles = NIL; // List* of List* of Vertex*
	bool *visited = (bool *)palloc0(nverts_global * sizeof(bool));
	List *stack;
	ListCell *lc;
	foreach (lc, vertexes) {
		Vertex *v = (Vertex *)lfirst(lc);
		memset(visited, false, nverts_global * sizeof(bool));

		cycles = dfs(v, v, stack, cycles, visited, used_vertexes_comp);
	}
	List *cyclic_topologies = NIL;
	foreach (lc, cycles) {
		List *cycle = (List *)lfirst(lc);
		Topology *topology = (Topology *)palloc0(sizeof(Topology));
		topology->vertexes = cycle;
		topology->topology = CYCLE;
		set_complexity_topology(topology);
		cyclic_topologies = lappend(cyclic_topologies, topology);
	}
	pfree(visited);
	return cyclic_topologies;
}

static bool is_star(Vertex *center, const bool *used_vertexes) {
	int count_unused_neighbors = 0;
	int count_light_neighbors = 0;
	double volume_center = center->rel->rows;
	ListCell *lc;
	foreach (lc, center->adj) {
		Vertex *neighbor = (Vertex *)lfirst(lc);
		if (!used_vertexes[neighbor->index]) {
			double volume_neighbor = neighbor->rel->rows;
			if (volume_center >= 10 * volume_neighbor) {
				count_light_neighbors++;
			}
			count_unused_neighbors++;
		}
	}
	return count_unused_neighbors >= 3 || count_light_neighbors >= 2;
}
static List *find_star(Vertex *center, bool *used_vertexes) {
	List *star = NIL;
	star = lappend(star, center);
	used_vertexes[center->index] = true;
	ListCell *lc;
	foreach (lc, center->adj) {
		Vertex *neighbor = (Vertex *)lfirst(lc);
		do {
			if (used_vertexes[neighbor->index]) {
				break;
			}
			star = lappend(star, neighbor);
			used_vertexes[neighbor->index] = true;
			if (is_star(neighbor, used_vertexes)) {
				break;
			}
			if (neighbor->adj == NIL) {
				break;
			}
			neighbor = (Vertex *)list_head(neighbor->adj);
		} while (true);
	}
	return star;
}

List *find_remaining_chains(List *vertexes, bool *used_vertexes) {
	List *remaining_chains = NIL; // List* of Topology*
	ListCell *lc;
	foreach (lc, vertexes) {
		Vertex *v = (Vertex *)lfirst(lc);
		if (!used_vertexes[v->index]) {
			List *sub = NIL;
			sub = dfs_component(v, sub, used_vertexes);
			Topology *topology = (Topology *)palloc0(sizeof(Topology));
			topology->vertexes = sub;
			topology->topology = CHAIN;
			set_complexity_topology(topology);
			remaining_chains = lappend(remaining_chains, sub);
		}
	}
	return remaining_chains;
}

List *find_stars(List *vertexes, bool *used_vertexes) {
	List *stars = NIL;
	ListCell *lc;
	foreach (lc, vertexes) {
		Vertex *v = (Vertex *)lfirst(lc);
		if (used_vertexes[v->index] || !is_star(v, used_vertexes)) {
			continue;
		}
		List *star = find_star(v, used_vertexes); // List* of Vertex*
		Topology *topology = (Topology *)palloc0(sizeof(Topology));
		topology->vertexes = star;
		topology->topology = STAR;
		set_complexity_topology(topology);
		stars = lappend(stars, star);
	}
	return stars;
}

static int count_edges(List *sub) {
	int m = 0;
	ListCell *lc;
	foreach (lc, sub) {
		Vertex *v1 = (Vertex *)lfirst(lc);
		ListCell *lc2;
		foreach (lc2, v1->adj) {
			Vertex *v2 = (Vertex *)lfirst(lc2);
			if (v1->index < v2->index && list_member_ptr(sub, v2)) {
				m++;
			}
		}
	}
	return m;
}

static double density(List *sub) {
	int n = list_length(sub);
	if (n < 2) {
		return 0.0;
	}

	int m = count_edges(sub);
	double d = (double)m / ((double)n * (n - 1) / 2.0);
	return d;
}

static Vertex *find_min_degree_vertex(List *sub) {
	int best_deg = INT_MAX;
	Vertex *best_v = NULL;
	ListCell *lc;
	foreach (lc, sub) {
		Vertex *v = (Vertex *)lfirst(lc);
		int deg = 0;
		ListCell *lc2;
		foreach (lc2, v->adj)
			if (list_member_ptr(sub, lfirst(lc2))) {
				deg++;
			}

		if (deg < best_deg) {
			best_deg = deg;
			best_v = v;
		}
	}
	return best_v;
}

List *find_dense_subgraphs(List *vertexes, bool *used) {
	List *dense_sets = NIL; // List* of Topology*

	while (true) {

		List *S = NIL;
		ListCell *lc;
		foreach (lc, vertexes) {
			Vertex *v = (Vertex *)lfirst(lc);
			if (!used[v->index]) {
				S = lappend(S, v);
			}
		}

		if (list_length(S) < 5) {
			break;
		}

		while (density(S) < THRESH && list_length(S) >= 4) {
			Vertex *vmin = find_min_degree_vertex(S);
			if (vmin == NULL) {
				break;
			}
			S = list_delete_ptr(S, vmin);
		}

		if (list_length(S) >= 4 && density(S) >= THRESH) {
			Topology *topology = (Topology *)palloc0(sizeof(Topology));
			topology->vertexes = S;
			topology->topology = DENSITY_GRAPH;
			set_complexity_topology(topology);
			dense_sets = lappend(dense_sets, topology);

			foreach (lc, S) {
				Vertex *v = (Vertex *)lfirst(lc);
				used[v->index] = true;
			}
		} else {

			break;
		}
	}

	return dense_sets;
}

static void set_complexity_topology(Topology *topology) {
	uint64 n = list_length(topology->vertexes);
	if (n >= 20) {
		topology->complexity = UINT64_MAX;
		return;
	}
	if (topology->topology == CHAIN) {
		topology->complexity =
			(12 * (n * n * n * n) + 6 * (n * n * n) - 18 * (n * n)) / 48;
		return;
	}
	if (topology->topology == CYCLE) {
		topology->complexity = (n * n * n * n - n * n * n - n * n + n) / 4;
		return;
	}
	if (topology->topology == STAR) {
		topology->complexity = (1 << (2 * n - 4)) -
							   binom_centr(2 * n - 2, n - 1) / 4 +
							   binom_centr(2 * n - 4, n - 2) / 4;
		topology->complexity +=
			n * (1 << (n - 1)) - 5 * (1 << (n - 3)) + (n * n - 5 * n + 4) / 2;
		return;
	}
	topology->complexity = (1 << (2 * n - 2)) -
						   5 * (1 << (n - 2)) * binom_centr(2 * n, n) / 4 +
						   1; // DENSITY_GRAPH
}
static void set_complexity_component(PlannerInfo *root, Topology *component) {
	List *initial_rels = NIL;
	ListCell *lc;
	foreach (lc, component->vertexes) {
		Vertex *v = (Vertex *)lfirst(lc);
		initial_rels = lappend(initial_rels, v->rel);
	}
	DPHypContext *context;
	context->initial_rels = initial_rels;
	context->root = root;
	context->simple_hypernodes = NIL;

	initialize_edges(root, initial_rels, context);

	uint64 subgraphs_count = count_cc(context, dphyp_geqo_cc_threshold);
	component->complexity = subgraphs_count;
}
static uint64 binom_centr(uint64 n, uint64 n2) {
	uint64 l = 1;
	for (uint64 i = n2 - n + 1; i <= n2; i++) {
		l *= i;
	}
	uint64 r = 1;
	for (uint64 i = 1; i <= n; i++) {
		r *= i;
	}
	return l / r;
}