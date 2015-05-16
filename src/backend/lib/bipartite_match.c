/*-------------------------------------------------------------------------
 *
 * bipartite_match.c
 *	  Hopcroft-Karp maximum cardinality algorithm for bipartite graphs
 *
 * This implementation is based on pseudocode found at:
 *
 * http://en.wikipedia.org/w/index.php?title=Hopcroft%E2%80%93Karp_algorithm&oldid=593898016
 *
 * Copyright (c) 2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/bipartite_match.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <math.h>
#include <limits.h>

#include "lib/bipartite_match.h"
#include "miscadmin.h"
#include "utils/builtins.h"


static bool hk_breadth_search(BipartiteMatchState *state);
static bool hk_depth_search(BipartiteMatchState *state, int u, int depth);

/*
 * Given the size of U and V, where each is indexed 1..size, and an adjacency
 * list, perform the matching and return the resulting state.
 */
BipartiteMatchState *
BipartiteMatch(int u_size, int v_size, short **adjacency)
{
	BipartiteMatchState *state = palloc(sizeof(BipartiteMatchState));

	Assert(u_size < SHRT_MAX);
	Assert(v_size < SHRT_MAX);

	state->u_size = u_size;
	state->v_size = v_size;
	state->matching = 0;
	state->adjacency = adjacency;
	state->pair_uv = palloc0((u_size + 1) * sizeof(short));
	state->pair_vu = palloc0((v_size + 1) * sizeof(short));
	state->distance = palloc((u_size + 1) * sizeof(float));
	state->queue = palloc((u_size + 2) * sizeof(short));

	while (hk_breadth_search(state))
	{
		int		u;

		for (u = 1; u <= u_size; ++u)
			if (state->pair_uv[u] == 0)
				if (hk_depth_search(state, u, 1))
					state->matching++;

		CHECK_FOR_INTERRUPTS();		/* just in case */
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

static bool
hk_breadth_search(BipartiteMatchState *state)
{
	int			usize = state->u_size;
	short	   *queue = state->queue;
	float	   *distance = state->distance;
	int			qhead = 0;		/* we never enqueue any node more than once */
	int			qtail = 0;		/* so don't have to worry about wrapping */
	int			u;

	distance[0] = get_float4_infinity();

	for (u = 1; u <= usize; ++u)
	{
		if (state->pair_uv[u] == 0)
		{
			distance[u] = 0;
			queue[qhead++] = u;
		}
		else
			distance[u] = get_float4_infinity();
	}

	while (qtail < qhead)
	{
		u = queue[qtail++];

		if (distance[u] < distance[0])
		{
			short  *u_adj = state->adjacency[u];
			int		i = u_adj ? u_adj[0] : 0;

			for (; i > 0; --i)
			{
				int	u_next = state->pair_vu[u_adj[i]];

				if (isinf(distance[u_next]))
				{
					distance[u_next] = 1 + distance[u];
					queue[qhead++] = u_next;
					Assert(qhead <= usize+2);
				}
			}
		}
	}

	return !isinf(distance[0]);
}

static bool
hk_depth_search(BipartiteMatchState *state, int u, int depth)
{
	float	   *distance = state->distance;
	short	   *pair_uv = state->pair_uv;
	short	   *pair_vu = state->pair_vu;
	short	   *u_adj = state->adjacency[u];
	int			i = u_adj ? u_adj[0] : 0;

	if (u == 0)
		return true;

	if ((depth % 8) == 0)
		check_stack_depth();

	for (; i > 0; --i)
	{
		int		v = u_adj[i];

		if (distance[pair_vu[v]] == distance[u] + 1)
		{
			if (hk_depth_search(state, pair_vu[v], depth+1))
			{
				pair_vu[v] = u;
				pair_uv[u] = v;
				return true;
			}
		}
	}

	distance[u] = get_float4_infinity();
	return false;
}
