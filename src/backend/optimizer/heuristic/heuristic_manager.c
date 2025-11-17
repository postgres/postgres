#include "optimizer/heuristic/heuristic_manager.h"
#include "nodes/nodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "optimizer/graph_utils.h"
#include "optimizer/heuristic/graph_utils.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "postgres.h"
#include <float.h>

static double b1 = 3 / 4;
static double q = 1 / 4;

typedef enum { STANDARD, GOO, GEQO } TypeHeuristic;

typedef enum GooComp { VOLUME, COST } GooComp;

static RelOptInfo *plan_subgraph(PlannerInfo *root, Topology *topology,
								 int *cost_plan);

static RelOptInfo *goo(PlannerInfo *root, List *component_plans,
					   GooComp gooComp, bool clauseless);

static Cost get_cost(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2);
static Selectivity get_selectivity(PlannerInfo *root, RelOptInfo *rel1,
								   RelOptInfo *rel2);
static void split_budget_among_topologies(List *topologies, int budget);
static void split_budget_among_components(List *components, int budget);
static uint64 get_cost_heuristic(Topology *topology,
								 TypeHeuristic type_heuristic);
///////////////////////////////////////////////////////////////////////////////////

static uint64 get_cost_heuristic(Topology *topology,
								 TypeHeuristic type_heuristic) {
	return 0;
	if (topology->topology == CHAIN) {
	}
	if (topology->topology == CYCLE) {
	}
	if (topology->topology == STAR) {
	}
	if (topology->topology == DENSITY_GRAPH) {
	}
}

static void split_budget_among_topologies(List *topologies, int budget) {
	uint64 sum_complexities = 0;
	ListCell *lc;
	foreach (lc, topologies) {
		Topology *topology = (Topology *)lfirst(lc);
		sum_complexities += topology->complexity;
	}
	foreach (lc, topologies) {
		Topology *topology = (Topology *)lfirst(lc);
		topology->budget = topology->complexity * budget / sum_complexities;
	}
}

RelOptInfo *heuristic_join_search(PlannerInfo *root, List *initial_rels,
								  int budget) {
	List *graph = build_join_graph(root, initial_rels); // List* of Vertex*
	List *components = split_components(graph);			// list of Component*
	split_budget_among_components(components, budget);
	List *component_plans = NIL;
	ListCell *lc;
	foreach (lc, components) {
		Topology *component = (Topology *)lfirst(lc);

		List *comp_vertexes = component->vertexes; // List* of Vertex*

		int component_budget = component->budget;
		int current_budget = component_budget * b1;
		while (list_length(comp_vertexes) > 1) {

			bool *used_vertexes =
				(bool *)palloc0(list_length(comp_vertexes) * sizeof(bool));

			List *topologies = NIL; // List* of Topology*
			List *dense_subgraphs =
				find_dense_subgraphs(comp_vertexes, used_vertexes);
			topologies = list_concat(topologies, dense_subgraphs);
			List *cycles = find_cycles(comp_vertexes, used_vertexes);
			topologies = list_concat(topologies, cycles);
			List *stars = find_stars(comp_vertexes, used_vertexes);
			topologies = list_concat(topologies, stars);
			List *remaining_chains =
				find_remaining_chains(comp_vertexes, used_vertexes);
			topologies = list_concat(topologies, remaining_chains);

			split_budget_among_topologies(topologies, current_budget);

			List *topology_plans = NIL;
			foreach (lc, topologies) {
				Topology *topology = (Topology *)lfirst(lc);
				int cost_plan = 0;
				RelOptInfo *plan = plan_subgraph(root, topology, &cost_plan);
				topology_plans = lappend(topology_plans, plan);
				current_budget -= cost_plan;
			}
			component_budget += current_budget; // maybe remain budget
			current_budget = component_budget * q;
			list_free_deep(comp_vertexes);
			comp_vertexes = build_join_graph(root, topology_plans);
			pfree(used_vertexes);
		}
		Vertex *v = (Vertex *)linitial(comp_vertexes);
		RelOptInfo *comp_plan = v->rel;
		component_plans = lappend(component_plans, comp_plan);
	}
	RelOptInfo *final_plan = goo(root, component_plans, VOLUME, true);
	list_free_deep(graph);
	return final_plan;
}
static Cost get_cost(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2) {
	RelOptInfo *tmp = make_join_rel(root, rel1, rel2);
	Cost cost = tmp->cheapest_total_path->total_cost;
	return cost;
}
static Selectivity get_selectivity(PlannerInfo *root, RelOptInfo *rel1,
								   RelOptInfo *rel2) { // TODO
	SpecialJoinInfo *sjinfo = NULL;
	SpecialJoinInfo sjinfo_data;
	sjinfo = &sjinfo_data;
	sjinfo->type = T_SpecialJoinInfo;
	sjinfo->min_lefthand = rel1->relids;
	sjinfo->min_righthand = rel2->relids;
	sjinfo->syn_lefthand = rel1->relids;
	sjinfo->syn_righthand = rel2->relids;
	sjinfo->jointype = JOIN_INNER;
	sjinfo->lhs_strict = false;

	List *joininfo = NIL;
	Relids other_relids;
	List *clauses = NIL;
	if (list_length(rel1->joininfo) <= list_length(rel2->joininfo)) {
		joininfo = rel1->joininfo;
		other_relids = rel2->relids;
	} else {
		joininfo = rel2->joininfo;
		other_relids = rel1->relids;
	}
	ListCell *lc;
	foreach (lc, joininfo) {
		RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc);
		if (bms_overlap(other_relids, rinfo->required_relids)) {
			clauses = lappend(clauses, rinfo);
		}
	}

	Selectivity result =
		clauselist_selectivity(root, clauses, 0, JOIN_INNER, sjinfo);
	return result;
}
static RelOptInfo *goo(PlannerInfo *root, List *initial_rels, GooComp gooComp,
					   bool clauseless) {
	List *rels; // List of RelOptInfo* as set c++
	ListCell *lc;
	foreach (lc, initial_rels) {
		RelOptInfo *plan = (RelOptInfo *)lfirst(lc);
		rels = lappend(rels, plan);
	}
	while (list_length(rels) > 1) {
		RelOptInfo *parent1 = NULL, *parent2 = NULL;
		Selectivity best_sel = 1;
		Cost best_cost = DBL_MAX;
		ListCell *i, *j;

		foreach (i, rels) {
			foreach (j, rels) {
				if (i == j)
					continue;
				RelOptInfo *r_i = (RelOptInfo *)lfirst(i);
				RelOptInfo *r_j = (RelOptInfo *)lfirst(j);
				if (!clauseless && !has_simple_inner_edge(root, r_i, r_j)) {
					continue;
				}
				if (gooComp == COST) {
					Cost tmp_cost = get_cost(root, r_i, r_j);
					if (tmp_cost < best_cost) {
						best_cost = tmp_cost;
						parent1 = r_i;
						parent2 = r_j;
					}
				} else {
					Selectivity tmp_sel = get_selectivity(root, r_i, r_j);
					if (tmp_sel < best_sel) {
						best_sel = tmp_sel;
						parent1 = r_i;
						parent2 = r_j;
					}
				}
			}
		}
		RelOptInfo *best_rel = make_join_rel(root, parent1, parent2);
		set_cheapest(best_rel);
		rels = lappend(rels, best_rel);
		rels = list_delete_cell(rels, parent1);
		rels = list_delete_cell(rels, parent2);
	}

	RelOptInfo *plan = (RelOptInfo *)linitial(rels);
	return plan;
}
static RelOptInfo *plan_subgraph(PlannerInfo *root, Topology *topology,
								 int *cost_plan) {
	List *initial_rels = NIL; // List* of RelOptInfo*
	ListCell *lc;
	foreach (lc, topology->vertexes) {
		Vertex *v = (Vertex *)lfirst(lc);
		RelOptInfo *rel = v->rel;
		initial_rels = lappend(initial_rels, rel);
	}
	RelOptInfo *plan = NULL;
	int nrels = list_length(initial_rels);
	uint64 cost_geqo = get_cost_heuristic(topology, GEQO);
	uint64 cost_standard = get_cost_heuristic(topology, STANDARD);
	bool use_heuristic = topology->budget < cost_standard;
	if (!use_heuristic)
		use_heuristic = is_easy_topology(topology);
	plan = use_heuristic ? standard_join_search(root, nrels, initial_rels)
						 : geqo(root, nrels, initial_rels);
	/*if (use_dp)
		plan = standard_join_search(root, nrels, initial_rels);
	else {
		switch (topology) {
			case (DENSITY_GRAPH):
				plan = enable_geqo ? geqo(root, nrels, initial_rels) :
	standard_join_search(root, nrels, initial_rels); break; case (STAR): plan =
	plan_star(root, nrels, initial_rels); break; case (CYCLE): plan =
	plan_cycle(root, nrels, initial_rels); break; case (CHAIN): plan =
	plan_chain(root, nrels, initial_rels); break;
		}
	}*/
	return plan;
}
