/*------------------------------------------------------------------------
 *
 * geqo_eval.c
 *	  Routines to evaluate query trees
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_eval.c,v 1.52 2000/07/12 22:59:01 petere Exp $
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
#include <limits.h>
#ifdef HAVE_VALUES_H
#include <values.h>
#endif

#include "optimizer/geqo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "utils/memutils.h"


/*
 * geqo_eval
 *
 * Returns cost of a query tree as an individual of the population.
 */
Cost
geqo_eval(Query *root, Gene *tour, int num_gene)
{
	MemoryContext mycontext;
	MemoryContext oldcxt;
	RelOptInfo *joinrel;
	Cost		fitness;
	List	   *savelist;

	/*
	 * Create a private memory context that will hold all temp storage
	 * allocated inside gimme_tree().
	 *
	 * Since geqo_eval() will be called many times, we can't afford to let
	 * all that memory go unreclaimed until end of statement.  Note we make
	 * the temp context a child of TransactionCommandContext, so that
	 * it will be freed even if we abort via elog(ERROR).
	 */
	mycontext = AllocSetContextCreate(TransactionCommandContext,
									  "GEQO",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(mycontext);

	/* preserve root->join_rel_list, which gimme_tree changes */
	savelist = root->join_rel_list;

	/* construct the best path for the given combination of relations */
	joinrel = gimme_tree(root, tour, 0, num_gene, NULL);

	/*
	 * compute fitness
	 *
	 * XXX geqo does not currently support optimization for partial result
	 * retrieval --- how to fix?
	 */
	fitness = joinrel->cheapest_total_path->total_cost;

	/* restore join_rel_list */
	root->join_rel_list = savelist;

	/* release all the memory acquired within gimme_tree */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(mycontext);

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
			List	   *acceptable_rels = lcons(inner_rel, NIL);

			new_rel = make_rels_by_clause_joins(root, old_rel,
												acceptable_rels);
			if (!new_rel)
			{
				new_rel = make_rels_by_clauseless_joins(root, old_rel,
														acceptable_rels);
				if (!new_rel)
					elog(ERROR, "gimme_tree: failed to construct join rel");
			}

			rel_count++;
			Assert(length(new_rel->relids) == rel_count);

			/* Find and save the cheapest paths for this rel */
			set_cheapest(new_rel);

			return gimme_tree(root, tour, rel_count, num_gene, new_rel);
		}
	}

	return old_rel;				/* tree finished ... */
}
