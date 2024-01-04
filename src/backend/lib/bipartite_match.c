/*-------------------------------------------------------------------------
 *
 * bipartite_match.c
 *	  Hopcroft-Karp maximum cardinality algorithm for bipartite graphs
 *
 * This implementation is based on pseudocode found at:
 *
 * https://en.wikipedia.org/w/index.php?title=Hopcroft%E2%80%93Karp_algorithm&oldid=593898016
 *
 * Copyright (c) 2015-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/bipartite_match.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "lib/bipartite_match.h"
#include "miscadmin.h"

/*
 * The distances computed in hk_breadth_search can easily be seen to never
 * exceed u_size.  Since we restrict u_size to be less than SHRT_MAX, we
 * can therefore use SHRT_MAX as the "infinity" distance needed as a marker.
 */
#define HK_INFINITY  SHRT_MAX

static bool hk_breadth_search(BipartiteMatchState *state);
static bool hk_depth_search(BipartiteMatchState *state, int u);

/*
 * Given the size of U and V, where each is indexed 1..size, and an adjacency
 * list, perform the matching and return the resulting state.
 */
BipartiteMatchState *
BipartiteMatch(int u_size, int v_size, short **adjacency)
{
	BipartiteMatchState *state = palloc(sizeof(BipartiteMatchState));

	if (u_size < 0 || u_size >= SHRT_MAX ||
		v_size < 0 || v_size >= SHRT_MAX)
		elog(ERROR, "invalid set size for BipartiteMatch");

	state->u_size = u_size;
	state->v_size = v_size;
	state->adjacency = adjacency;
	state->matching = 0;
	state->pair_uv = (short *) palloc0((u_size + 1) * sizeof(short));
	state->pair_vu = (short *) palloc0((v_size + 1) * sizeof(short));
	state->distance = (short *) palloc((u_size + 1) * sizeof(short));
	state->queue = (short *) palloc((u_size + 2) * sizeof(short));

	while (hk_breadth_search(state))
	{
		int			u;

		for (u = 1; u <= u_size; u++)
		{
			if (state->pair_uv[u] == 0)
				if (hk_depth_search(state, u))
					state->matching++;
		}

		CHECK_FOR_INTERRUPTS(); /* just in case */
	}

	return state;
}

/*
 * Free a state returned by BipartiteMatch, except for the original adjacency
 * list, which is owned by the caller. This only frees memory, so it's optional.
 */
void
BipartiteMatchFree(BipartiteMatchState *state)
{
	/* adjacency matrix is treated as owned by the caller */
	pfree(state->pair_uv);
	pfree(state->pair_vu);
	pfree(state->distance);
	pfree(state->queue);
	pfree(state);
}

/*
 * Perform the breadth-first search step of H-K matching.
 * Returns true if successful.
 */
static bool
hk_breadth_search(BipartiteMatchState *state)
{
	int			usize = state->u_size;
	short	   *queue = state->queue;
	short	   *distance = state->distance;
	int			qhead = 0;		/* we never enqueue any node more than once */
	int			qtail = 0;		/* so don't have to worry about wrapping */
	int			u;

	distance[0] = HK_INFINITY;

	for (u = 1; u <= usize; u++)
	{
		if (state->pair_uv[u] == 0)
		{
			distance[u] = 0;
			queue[qhead++] = u;
		}
		else
			distance[u] = HK_INFINITY;
	}

	while (qtail < qhead)
	{
		u = queue[qtail++];

		if (distance[u] < distance[0])
		{
			short	   *u_adj = state->adjacency[u];
			int			i = u_adj ? u_adj[0] : 0;

			for (; i > 0; i--)
			{
				int			u_next = state->pair_vu[u_adj[i]];

				if (distance[u_next] == HK_INFINITY)
				{
					distance[u_next] = 1 + distance[u];
					Assert(qhead < usize + 2);
					queue[qhead++] = u_next;
				}
			}
		}
	}

	return (distance[0] != HK_INFINITY);
}

/*
 * Perform the depth-first search step of H-K matching.
 * Returns true if successful.
 */
static bool
hk_depth_search(BipartiteMatchState *state, int u)
{
	short	   *distance = state->distance;
	short	   *pair_uv = state->pair_uv;
	short	   *pair_vu = state->pair_vu;
	short	   *u_adj = state->adjacency[u];
	int			i = u_adj ? u_adj[0] : 0;
	short		nextdist;

	if (u == 0)
		return true;
	if (distance[u] == HK_INFINITY)
		return false;
	nextdist = distance[u] + 1;

	check_stack_depth();

	for (; i > 0; i--)
	{
		int			v = u_adj[i];

		if (distance[pair_vu[v]] == nextdist)
		{
			if (hk_depth_search(state, pair_vu[v]))
			{
				pair_vu[v] = u;
				pair_uv[u] = v;
				return true;
			}
		}
	}

	distance[u] = HK_INFINITY;
	return false;
}
