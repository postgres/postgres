/*------------------------------------------------------------------------
*
* geqo_erx.c
*	 edge recombination crossover [ER]
*
* src/backend/optimizer/geqo/geqo_erx.c
*
*-------------------------------------------------------------------------
*/

/* contributed by:
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch				 * Institute of Automatic Control	   *
   =							 = University of Mining and Technology =
   *  utesch@aut.tu-freiberg.de  * Freiberg, Germany				   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */

/* the edge recombination algorithm is adopted from Genitor : */
/*************************************************************/
/*															 */
/*	Copyright (c) 1990										 */
/*	Darrell L. Whitley										 */
/*	Computer Science Department								 */
/*	Colorado State University								 */
/*															 */
/*	Permission is hereby granted to copy all or any part of  */
/*	this program for free distribution.   The author's name  */
/*	and this copyright notice must be included in any copy.  */
/*															 */
/*************************************************************/


#include "postgres.h"
#include "optimizer/geqo.h"

#if defined(ERX)

#include "optimizer/geqo_random.h"
#include "optimizer/geqo_recombination.h"

static int	gimme_edge(PlannerInfo *root, Gene gene1, Gene gene2, Edge *edge_table);
static void remove_gene(PlannerInfo *root, Gene gene, Edge edge, Edge *edge_table);
static Gene gimme_gene(PlannerInfo *root, Edge edge, Edge *edge_table);

static Gene edge_failure(PlannerInfo *root, Gene *gene, int index, Edge *edge_table, int num_gene);


/* alloc_edge_table
 *
 *	 allocate memory for edge table
 *
 */

Edge *
alloc_edge_table(PlannerInfo *root, int num_gene)
{
	Edge	   *edge_table;

	/*
	 * palloc one extra location so that nodes numbered 1..n can be indexed
	 * directly; 0 will not be used
	 */

	edge_table = (Edge *) palloc((num_gene + 1) * sizeof(Edge));

	return edge_table;
}

/* free_edge_table
 *
 *	  deallocate memory of edge table
 *
 */
void
free_edge_table(PlannerInfo *root, Edge *edge_table)
{
	pfree(edge_table);
}

/* gimme_edge_table
 *
 *	 fills a data structure which represents the set of explicit
 *	 edges between points in the (2) input genes
 *
 *	 assumes circular tours and bidirectional edges
 *
 *	 gimme_edge() will set "shared" edges to negative values
 *
 *	 returns average number edges/city in range 2.0 - 4.0
 *	 where 2.0=homogeneous; 4.0=diverse
 *
 */
float
gimme_edge_table(PlannerInfo *root, Gene *tour1, Gene *tour2,
				 int num_gene, Edge *edge_table)
{
	int			i,
				index1,
				index2;
	int			edge_total;		/* total number of unique edges in two genes */

	/* at first clear the edge table's old data */
	for (i = 1; i <= num_gene; i++)
	{
		edge_table[i].total_edges = 0;
		edge_table[i].unused_edges = 0;
	}

	/* fill edge table with new data */

	edge_total = 0;

	for (index1 = 0; index1 < num_gene; index1++)
	{
		/*
		 * presume the tour is circular, i.e. 1->2, 2->3, 3->1 this operation
		 * maps n back to 1
		 */

		index2 = (index1 + 1) % num_gene;

		/*
		 * edges are bidirectional, i.e. 1->2 is same as 2->1 call gimme_edge
		 * twice per edge
		 */

		edge_total += gimme_edge(root, tour1[index1], tour1[index2], edge_table);
		gimme_edge(root, tour1[index2], tour1[index1], edge_table);

		edge_total += gimme_edge(root, tour2[index1], tour2[index2], edge_table);
		gimme_edge(root, tour2[index2], tour2[index1], edge_table);
	}

	/* return average number of edges per index */
	return ((float) (edge_total * 2) / (float) num_gene);
}

/* gimme_edge
 *
 *	  registers edge from city1 to city2 in input edge table
 *
 *	  no assumptions about directionality are made;
 *	  therefore it is up to the calling routine to
 *	  call gimme_edge twice to make a bi-directional edge
 *	  between city1 and city2;
 *	  uni-directional edges are possible as well (just call gimme_edge
 *	  once with the direction from city1 to city2)
 *
 *	  returns 1 if edge was not already registered and was just added;
 *			  0 if edge was already registered and edge_table is unchanged
 */
static int
gimme_edge(PlannerInfo *root, Gene gene1, Gene gene2, Edge *edge_table)
{
	int			i;
	int			edges;
	int			city1 = (int) gene1;
	int			city2 = (int) gene2;


	/* check whether edge city1->city2 already exists */
	edges = edge_table[city1].total_edges;

	for (i = 0; i < edges; i++)
	{
		if ((Gene) abs(edge_table[city1].edge_list[i]) == city2)
		{

			/* mark shared edges as negative */
			edge_table[city1].edge_list[i] = 0 - city2;

			return 0;
		}
	}

	/* add city1->city2; */
	edge_table[city1].edge_list[edges] = city2;

	/* increment the number of edges from city1 */
	edge_table[city1].total_edges++;
	edge_table[city1].unused_edges++;

	return 1;
}

/* gimme_tour
 *
 *	  creates a new tour using edges from the edge table.
 *	  priority is given to "shared" edges (i.e. edges which
 *	  all parent genes possess and are marked as negative
 *	  in the edge table.)
 *
 */
int
gimme_tour(PlannerInfo *root, Edge *edge_table, Gene *new_gene, int num_gene)
{
	int			i;
	int			edge_failures = 0;

	/* choose int between 1 and num_gene */
	new_gene[0] = (Gene) geqo_randint(root, num_gene, 1);

	for (i = 1; i < num_gene; i++)
	{
		/*
		 * as each point is entered into the tour, remove it from the edge
		 * table
		 */

		remove_gene(root, new_gene[i - 1], edge_table[(int) new_gene[i - 1]], edge_table);

		/* find destination for the newly entered point */

		if (edge_table[new_gene[i - 1]].unused_edges > 0)
			new_gene[i] = gimme_gene(root, edge_table[(int) new_gene[i - 1]], edge_table);

		else
		{						/* cope with fault */
			edge_failures++;

			new_gene[i] = edge_failure(root, new_gene, i - 1, edge_table, num_gene);
		}

		/* mark this node as incorporated */
		edge_table[(int) new_gene[i - 1]].unused_edges = -1;
	}							/* for (i=1; i<num_gene; i++) */

	return edge_failures;
}

/* remove_gene
 *
 *	 removes input gene from edge_table.
 *	 input edge is used
 *	 to identify deletion locations within edge table.
 *
 */
static void
remove_gene(PlannerInfo *root, Gene gene, Edge edge, Edge *edge_table)
{
	int			i,
				j;
	int			possess_edge;
	int			genes_remaining;

	/*
	 * do for every gene known to have an edge to input gene (i.e. in
	 * edge_list for input edge)
	 */

	for (i = 0; i < edge.unused_edges; i++)
	{
		possess_edge = abs(edge.edge_list[i]);
		genes_remaining = edge_table[possess_edge].unused_edges;

		/* find the input gene in all edge_lists and delete it */
		for (j = 0; j < genes_remaining; j++)
		{

			if ((Gene) abs(edge_table[possess_edge].edge_list[j]) == gene)
			{

				edge_table[possess_edge].unused_edges--;

				edge_table[possess_edge].edge_list[j] =
					edge_table[possess_edge].edge_list[genes_remaining - 1];

				break;
			}
		}
	}
}

/* gimme_gene
 *
 *	  priority is given to "shared" edges
 *	  (i.e. edges which both genes possess)
 *
 */
static Gene
gimme_gene(PlannerInfo *root, Edge edge, Edge *edge_table)
{
	int			i;
	Gene		friend;
	int			minimum_edges;
	int			minimum_count = -1;
	int			rand_decision;

	/*
	 * no point has edges to more than 4 other points thus, this contrived
	 * minimum will be replaced
	 */

	minimum_edges = 5;

	/* consider candidate destination points in edge list */

	for (i = 0; i < edge.unused_edges; i++)
	{
		friend = (Gene) edge.edge_list[i];

		/*
		 * give priority to shared edges that are negative; so return 'em
		 */

		/*
		 * negative values are caught here so we need not worry about
		 * converting to absolute values
		 */
		if (friend < 0)
			return (Gene) abs(friend);


		/*
		 * give priority to candidates with fewest remaining unused edges;
		 * find out what the minimum number of unused edges is
		 * (minimum_edges); if there is more than one candidate with the
		 * minimum number of unused edges keep count of this number
		 * (minimum_count);
		 */

		/*
		 * The test for minimum_count can probably be removed at some point
		 * but comments should probably indicate exactly why it is guaranteed
		 * that the test will always succeed the first time around.  If it can
		 * fail then the code is in error
		 */


		if (edge_table[(int) friend].unused_edges < minimum_edges)
		{
			minimum_edges = edge_table[(int) friend].unused_edges;
			minimum_count = 1;
		}
		else if (minimum_count == -1)
			elog(ERROR, "minimum_count not set");
		else if (edge_table[(int) friend].unused_edges == minimum_edges)
			minimum_count++;
	}							/* for (i=0; i<edge.unused_edges; i++) */


	/* random decision of the possible candidates to use */
	rand_decision = geqo_randint(root, minimum_count - 1, 0);


	for (i = 0; i < edge.unused_edges; i++)
	{
		friend = (Gene) edge.edge_list[i];

		/* return the chosen candidate point */
		if (edge_table[(int) friend].unused_edges == minimum_edges)
		{
			minimum_count--;

			if (minimum_count == rand_decision)
				return friend;
		}
	}

	/* ... should never be reached */
	elog(ERROR, "neither shared nor minimum number nor random edge found");
	return 0;					/* to keep the compiler quiet */
}

/* edge_failure
 *
 *	  routine for handling edge failure
 *
 */
static Gene
edge_failure(PlannerInfo *root, Gene *gene, int index, Edge *edge_table, int num_gene)
{
	int			i;
	Gene		fail_gene = gene[index];
	int			remaining_edges = 0;
	int			four_count = 0;
	int			rand_decision;


	/*
	 * how many edges remain? how many gene with four total (initial) edges
	 * remain?
	 */

	for (i = 1; i <= num_gene; i++)
	{
		if ((edge_table[i].unused_edges != -1) && (i != (int) fail_gene))
		{
			remaining_edges++;

			if (edge_table[i].total_edges == 4)
				four_count++;
		}
	}

	/*
	 * random decision of the gene with remaining edges and whose total_edges
	 * == 4
	 */

	if (four_count != 0)
	{

		rand_decision = geqo_randint(root, four_count - 1, 0);

		for (i = 1; i <= num_gene; i++)
		{

			if ((Gene) i != fail_gene &&
				edge_table[i].unused_edges != -1 &&
				edge_table[i].total_edges == 4)
			{

				four_count--;

				if (rand_decision == four_count)
					return (Gene) i;
			}
		}

		elog(LOG, "no edge found via random decision and total_edges == 4");
	}
	else if (remaining_edges != 0)
	{
		/* random decision of the gene with remaining edges */
		rand_decision = geqo_randint(root, remaining_edges - 1, 0);

		for (i = 1; i <= num_gene; i++)
		{

			if ((Gene) i != fail_gene &&
				edge_table[i].unused_edges != -1)
			{

				remaining_edges--;

				if (rand_decision == remaining_edges)
					return i;
			}
		}

		elog(LOG, "no edge found via random decision with remaining edges");
	}

	/*
	 * edge table seems to be empty; this happens sometimes on the last point
	 * due to the fact that the first point is removed from the table even
	 * though only one of its edges has been determined
	 */

	else
	{							/* occurs only at the last point in the tour;
								 * simply look for the point which is not yet
								 * used */

		for (i = 1; i <= num_gene; i++)
			if (edge_table[i].unused_edges >= 0)
				return (Gene) i;

		elog(LOG, "no edge found via looking for the last unused point");
	}


	/* ... should never be reached */
	elog(ERROR, "no edge found");
	return 0;					/* to keep the compiler quiet */
}

#endif							/* defined(ERX) */
