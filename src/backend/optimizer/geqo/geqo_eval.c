/*------------------------------------------------------------------------
 *
 * geqo_eval.c
 *	  Routines to evaluate query trees
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_eval.c,v 1.35 1999/02/18 05:26:18 momjian Exp $
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

#include "postgres.h"

#include <math.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#ifndef MAXINT
#define MAXINT INT_MAX
#endif
#else
#include <values.h>
#endif

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
#include "optimizer/tlist.h"
#include "optimizer/joininfo.h"

#include "optimizer/geqo_gene.h"
#include "optimizer/geqo.h"

static RelOptInfo *geqo_nth(int stop, List *rels);

/*
 * geqo_eval
 *
 * Returns cost of a query tree as an individual of the population.
 */
Cost
geqo_eval(Query *root, Gene *tour, int num_gene)
{
	RelOptInfo *joinrel;
	Cost		fitness;
	List	   *temp;


/* remember root->join_rel_list ... */
/* because root->join_rel_list will be changed during the following */
	temp = listCopy(root->join_rel_list);

/* joinrel is readily processed query tree -- left-sided ! */
	joinrel = gimme_tree(root, tour, 0, num_gene, NULL);

/* compute fitness */
	fitness = (Cost) joinrel->cheapestpath->path_cost;

	root->join_rel_list = listCopy(temp);

	pfree(joinrel);
	freeList(temp);

	return fitness;

}

/*
 * gimme_tree 
 *	  this program presumes that only LEFT-SIDED TREES are considered!
 *
 * 'old_rel' is the preceeding join
 *
 * Returns a new join relation incorporating all joins in a left-sided tree.
 */
RelOptInfo *
gimme_tree(Query *root, Gene *tour, int rel_count, int num_gene, RelOptInfo *old_rel)
{
	RelOptInfo *inner_rel;		/* current relation */
	int			base_rel_index;

	List	   *new_rels = NIL;
	RelOptInfo *new_rel = NULL;

	if (rel_count < num_gene)
	{							/* tree not yet finished */

		/* tour[0] = 3; tour[1] = 1; tour[2] = 2 */
		base_rel_index = (int) tour[rel_count];

		inner_rel = (RelOptInfo *) geqo_nth(base_rel_index, root->base_rel_list);

		if (rel_count == 0)
		{						/* processing first join with
								 * base_rel_index = (int) tour[0] */
			rel_count++;
			return gimme_tree(root, tour, rel_count, num_gene, inner_rel);
		}
		else
		{						/* tree main part */
			if (!(new_rels = make_rels_by_clause_joins(root, old_rel,
													   inner_rel->joininfo,
													   inner_rel->relids)))
			{
				new_rels = make_rels_by_clauseless_joins(old_rel,
											 	lcons(inner_rel,NIL));
				/* we don't do bushy plans in geqo, do we?  bjm 02/18/1999
				new_rels = append(new_rels,
								  make_rels_by_clauseless_joins(old_rel,
													 lcons(old_rel,NIL));
				*/
			}

			/* process new_rel->pathlist */
			update_rels_pathlist_for_joins(root, new_rels);

			/* prune new_rels */
			/* MAU: is this necessary? */

			/*
			 * what's the matter if more than one new rel is left till
			 * now?
			 */

			/*
			 * joinrels in newrels with different ordering of relids are
			 * not possible
			 */
			if (length(new_rels) > 1)
				merge_rels_with_same_relids(new_rels);

			if (length(new_rels) > 1)
			{					/* should never be reached ... */
				elog(DEBUG, "gimme_tree: still %d relations left", length(new_rels));
			}

			/* get essential new relation */
			new_rel = (RelOptInfo *) lfirst(new_rels);
			rel_count++;

			set_cheapest(new_rel, new_rel->pathlist);

			/* processing of other new_rel attributes */
			if (new_rel->size <= 0)
				new_rel->size = compute_rel_size(new_rel);
			new_rel->width = compute_rel_width(new_rel);

			root->join_rel_list = lcons(new_rel, NIL);

			return gimme_tree(root, tour, rel_count, num_gene, new_rel);
		}

	}

	return old_rel;			/* tree finished ... */
}

static RelOptInfo *
geqo_nth(int stop, List *rels)
{
	List	   *r;
	int			i = 1;

	foreach(r, rels)
	{
		if (i == stop)
			return lfirst(r);
		i++;
	}
	elog(ERROR, "geqo_nth: Internal error - ran off end of list");
	return NULL;				/* to keep compiler happy */
}
