/*------------------------------------------------------------------------
 *
 * geqo_misc.c--
 *	   misc. printout and debug stuff
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_misc.c,v 1.16 1999/02/12 17:24:47 momjian Exp $
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


#include <stdio.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"

#include "utils/palloc.h"
#include "utils/elog.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"

#include "optimizer/geqo_gene.h"
#include "optimizer/geqo_pool.h"
#include "optimizer/geqo_recombination.h"
#include "optimizer/geqo_misc.h"

static float avg_pool(Pool *pool);

/* avg_pool--
 *
 */
static float
avg_pool(Pool *pool)
{
	int			i;
	double		cumulative = 0.0;

	if (pool->size == 0)
		elog(ERROR, "avg_pool: pool_size of zero");

	for (i = 0; i < pool->size; i++)
		cumulative = cumulative + pool->data[i].worth;

	return (float) cumulative / pool->size;
}

/* print_pool--
 */
void
print_pool(FILE *fp, Pool *pool, int start, int stop)
{
	int			i,
				j;

	/* be extra careful that start and stop are valid inputs */

	if (start < 0)
		start = 0;
	if (stop > pool->size)
		stop = pool->size;

	if (start + stop > pool->size)
	{
		start = 0;
		stop = pool->size;
	}

	for (i = start; i < stop; i++)
	{
		fprintf(fp, "%d)\t", i);
		for (j = 0; j < pool->string_length; j++)
			fprintf(fp, "%d ", pool->data[i].string[j]);
		fprintf(fp, "%f\n", pool->data[i].worth);
	}
}

/* print_gen--
 *
 *	 printout for chromosome: best, worst, mean, average
 *
 */
void
print_gen(FILE *fp, Pool *pool, int generation)
{
	int			lowest;

	/* Get index to lowest ranking gene in poplulation. */
	/* Use 2nd to last since last is buffer. */
	lowest = pool->size > 1 ? pool->size - 2 : 0;

	fprintf(fp,
			"%5d | Bst: %f  Wst: %f  Mean: %f  Avg: %f\n",
			generation,
			pool->data[0].worth,
			pool->data[lowest].worth,
			pool->data[pool->size / 2].worth,
			avg_pool(pool));
}


void
print_edge_table(FILE *fp, Edge *edge_table, int num_gene)
{
	int			i,
				j;

	fprintf(fp, "\nEDGE TABLE\n");

	for (i = 1; i <= num_gene; i++)
	{
		fprintf(fp, "%d :", i);
		for (j = 0; j < edge_table[i].unused_edges; j++)
			fprintf(fp, " %d", edge_table[i].edge_list[j]);
		fprintf(fp, "\n");
	}

	fprintf(fp, "\n");
}

/*************************************************************
 Debug output subroutines
 *************************************************************/

void
geqo_print_joinclauses(Query *root, List *clauses)
{
	List	   *l;
	extern void print_expr(Node *expr, List *rtable);	/* in print.c */

	foreach(l, clauses)
	{
		RestrictInfo *c = lfirst(l);

		print_expr((Node *) c->clause, root->rtable);
		if (lnext(l))
			printf(" ");
	}
}

void
geqo_print_path(Query *root, Path *path, int indent)
{
	char	   *ptype = NULL;
	JoinPath   *jp;
	bool		join = false;
	int			i;

	for (i = 0; i < indent; i++)
		printf("\t");

	switch (nodeTag(path))
	{
		case T_Path:
			ptype = "SeqScan";
			join = false;
			break;
		case T_IndexPath:
			ptype = "IdxScan";
			join = false;
			break;
		case T_NestPath:
			ptype = "Nestloop";
			join = true;
			break;
		case T_MergePath:
			ptype = "MergeJoin";
			join = true;
			break;
		case T_HashPath:
			ptype = "HashJoin";
			join = true;
			break;
		default:
			break;
	}
	if (join)
	{
		int			size = path->parent->size;

		jp = (JoinPath *) path;
		printf("%s size=%d cost=%f\n", ptype, size, path->path_cost);
		switch (nodeTag(path))
		{
			case T_MergePath:
			case T_HashPath:
				for (i = 0; i < indent + 1; i++)
					printf("\t");
				printf("   clauses=(");
				geqo_print_joinclauses(root, ((JoinPath *) path)->pathinfo);
				printf(")\n");

				if (nodeTag(path) == T_MergePath)
				{
					MergePath  *mp = (MergePath *) path;

					if (mp->outersortkeys || mp->innersortkeys)
					{
						for (i = 0; i < indent + 1; i++)
							printf("\t");
						printf("   sortouter=%d sortinner=%d\n",
							   ((mp->outersortkeys) ? 1 : 0),
							   ((mp->innersortkeys) ? 1 : 0));
					}
				}
				break;
			default:
				break;
		}
		geqo_print_path(root, jp->outerjoinpath, indent + 1);
		geqo_print_path(root, jp->innerjoinpath, indent + 1);
	}
	else
	{
		int			size = path->parent->size;
		int			relid = lfirsti(path->parent->relids);

		printf("%s(%d) size=%d cost=%f",
			   ptype, relid, size, path->path_cost);

		if (nodeTag(path) == T_IndexPath)
		{
			List	   *k,
					   *l;

			printf(" pathkeys=");
			foreach(k, path->pathkeys)
			{
				printf("(");
				foreach(l, lfirst(k))
				{
					Var		   *var = lfirst(l);

					printf("%d.%d", var->varnoold, var->varoattno);
					if (lnext(l))
						printf(", ");
				}
				printf(")");
				if (lnext(k))
					printf(", ");
			}
		}
		printf("\n");
	}
}

void
geqo_print_rel(Query *root, RelOptInfo *rel)
{
	List	   *l;

	printf("______________________________\n");
	printf("(");
	foreach(l, rel->relids)
		printf("%d ", lfirsti(l));
	printf("): size=%d width=%d\n", rel->size, rel->width);

	printf("\tpath list:\n");
	foreach(l, rel->pathlist)
		geqo_print_path(root, lfirst(l), 1);

	printf("\tcheapest path:\n");
	geqo_print_path(root, rel->cheapestpath, 1);
}
