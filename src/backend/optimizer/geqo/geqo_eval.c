/*------------------------------------------------------------------------
 *
 * geqo_eval.c
 *	  Routines to evaluate query trees
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_eval.c,v 1.40 1999/07/15 15:19:11 momjian Exp $
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

#include "utils/portal.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/tlist.h"
#include "optimizer/joininfo.h"

#include "optimizer/geqo_gene.h"
#include "optimizer/geqo.h"

/*
 * Variables set by geqo_eval_startup for use within a single GEQO run
 */
static MemoryContext geqo_eval_context;

/*
 * geqo_eval_startup:
 *	 Must be called during geqo_main startup (before geqo_eval may be called)
 *
 * The main thing we need to do here is prepare a private memory context for
 * allocation of temp storage used while constructing a path in geqo_eval().
 * Since geqo_eval() will be called many times, we can't afford to let all
 * that memory go unreclaimed until end of statement.  We use a special
 * named portal to hold the context, so that it will be freed even if
 * we abort via elog(ERROR).  The working data is allocated in the portal's
 * heap memory context.
 */
void
geqo_eval_startup(void)
{
#define GEQO_PORTAL_NAME	"<geqo workspace>"
	Portal		geqo_portal = GetPortalByName(GEQO_PORTAL_NAME);

	if (!PortalIsValid(geqo_portal))
	{
		/* First time through (within current transaction, that is) */
		geqo_portal = CreatePortal(GEQO_PORTAL_NAME);
		Assert(PortalIsValid(geqo_portal));
	}

	geqo_eval_context = (MemoryContext) PortalGetHeapMemory(geqo_portal);
}

/*
 * geqo_eval
 *
 * Returns cost of a query tree as an individual of the population.
 */
Cost
geqo_eval(Query *root, Gene *tour, int num_gene)
{
	MemoryContext oldcxt;
	RelOptInfo *joinrel;
	Cost		fitness;
	List	   *savelist;

	/* preserve root->join_rel_list, which gimme_tree changes */
	savelist = root->join_rel_list;

	/*
	 * create a temporary allocation context for the path construction
	 * work
	 */
	oldcxt = MemoryContextSwitchTo(geqo_eval_context);
	StartPortalAllocMode(DefaultAllocMode, 0);

	/* construct the best path for the given combination of relations */
	joinrel = gimme_tree(root, tour, 0, num_gene, NULL);

	/* compute fitness */
	fitness = (Cost) joinrel->cheapestpath->path_cost;

	/* restore join_rel_list */
	root->join_rel_list = savelist;

	/* release all the memory acquired within gimme_tree */
	EndPortalAllocMode();
	MemoryContextSwitchTo(oldcxt);

	return fitness;
}

/*
 * gimme_tree
 *	  this program presumes that only LEFT-SIDED TREES are considered!
 *
 * 'old_rel' is the preceding join
 *
 * Returns a new join relation incorporating all joins in a left-sided tree.
 */
RelOptInfo *
gimme_tree(Query *root, Gene *tour, int rel_count, int num_gene, RelOptInfo *old_rel)
{
	RelOptInfo *inner_rel;		/* current relation */
	int			base_rel_index;
	List	   *new_rels;
	RelOptInfo *new_rel;

	if (rel_count < num_gene)
	{							/* tree not yet finished */

		/* tour[0] = 3; tour[1] = 1; tour[2] = 2 */
		base_rel_index = (int) tour[rel_count];

		inner_rel = (RelOptInfo *) nth(base_rel_index - 1, root->base_rel_list);

		if (rel_count == 0)
		{						/* processing first join with
								 * base_rel_index = (int) tour[0] */
			rel_count++;
			return gimme_tree(root, tour, rel_count, num_gene, inner_rel);
		}
		else
		{						/* tree main part */
			if (!(new_rels = make_rels_by_clause_joins(root, old_rel,
													   old_rel->joininfo,
													 inner_rel->relids)))
			{
				new_rels = make_rels_by_clauseless_joins(old_rel,
												  lcons(inner_rel, NIL));

				/*
				 * we don't do bushy plans in geqo, do we?  bjm 02/18/1999
				 * new_rels = append(new_rels,
				 * make_rels_by_clauseless_joins(old_rel,
				 * lcons(old_rel,NIL));
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

			rels_set_cheapest(new_rels);

			/* get essential new relation */
			new_rel = (RelOptInfo *) lfirst(new_rels);
			rel_count++;

			/* processing of other new_rel attributes */
			if (new_rel->size <= 0)
				new_rel->size = compute_rel_size(new_rel);
			new_rel->width = compute_rel_width(new_rel);

			root->join_rel_list = lcons(new_rel, NIL);

			return gimme_tree(root, tour, rel_count, num_gene, new_rel);
		}
	}

	return old_rel;				/* tree finished ... */
}
