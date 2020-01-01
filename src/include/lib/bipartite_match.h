/*
 * bipartite_match.h
 *
 * Copyright (c) 2015-2020, PostgreSQL Global Development Group
 *
 * src/include/lib/bipartite_match.h
 */
#ifndef BIPARTITE_MATCH_H
#define BIPARTITE_MATCH_H

/*
 * Given a bipartite graph consisting of nodes U numbered 1..nU, nodes V
 * numbered 1..nV, and an adjacency map of undirected edges in the form
 * adjacency[u] = [k, v1, v2, v3, ... vk], we wish to find a "maximum
 * cardinality matching", which is defined as follows: a matching is a subset
 * of the original edges such that no node has more than one edge, and a
 * matching has maximum cardinality if there exists no other matching with a
 * greater number of edges.
 *
 * This matching has various applications in graph theory, but the motivating
 * example here is Dilworth's theorem: a partially-ordered set can be divided
 * into the minimum number of chains (i.e. subsets X where x1 < x2 < x3 ...) by
 * a bipartite graph construction. This gives us a polynomial-time solution to
 * the problem of planning a collection of grouping sets with the provably
 * minimal number of sort operations.
 */
typedef struct BipartiteMatchState
{
	/* inputs: */
	int			u_size;			/* size of U */
	int			v_size;			/* size of V */
	short	  **adjacency;		/* adjacency[u] = [k, v1,v2,v3,...,vk] */
	/* outputs: */
	int			matching;		/* number of edges in matching */
	short	   *pair_uv;		/* pair_uv[u] -> v */
	short	   *pair_vu;		/* pair_vu[v] -> u */
	/* private state for matching algorithm: */
	short	   *distance;		/* distance[u] */
	short	   *queue;			/* queue storage for breadth search */
} BipartiteMatchState;

extern BipartiteMatchState *BipartiteMatch(int u_size, int v_size, short **adjacency);

extern void BipartiteMatchFree(BipartiteMatchState *state);

#endif							/* BIPARTITE_MATCH_H */
