#ifndef HEURISTIC_MANAGER_H
#define HEURISTIC_MANAGER_H

#include "nodes/pathnodes.h"
extern RelOptInfo *heuristic_join_search(PlannerInfo *root, List *initial_rels,
										 int budget);

#endif
