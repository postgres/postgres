/*------------------------------------------------------------------------
 *
 * geqo_eval.c
 *	  Routines to evaluate query trees
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_eval.c,v 1.55 2000/09/19 18:42:33 tgl Exp $
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
geqo_eval(Query *root, List *initial_rels, Gene *tour, int num_gene)
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
	joinrel = gimme_tree(root, initial_rels, tour, num_gene, 0, NULL);

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
 *	  this routine considers only LEFT-SIDED TREES!
 *
 *	 'root' is the Query
 *	 'initial_rels' is the list of initial relations (FROM-list items)
 *	 'tour' is the proposed join order, of length 'num_gene'
 *	 'rel_count' is number of initial_rels items already joined (initially 0)
 *	 'old_rel' is the preceding join (initially NULL)
 *
 * Returns a new join relation incorporating all joins in a left-sided tree.
 */
RelOptInfo *
gimme_tree(Query *root, List *initial_rels,
		   Gene *tour, int num_gene,
		   int rel_count, RelOptInfo *old_rel)
{
	RelOptInfo *inner_rel;		/* current relation */
	int			init_rel_index;

	if (rel_count < num_gene)
	{
		/* tree not yet finished */
		init_rel_index = (int) tour[rel_count];

		inner_rel = (RelOptInfo *) nth(init_rel_index - 1, initial_rels);

		if (rel_count == 0)
		{
			/* processing first join with init_rel_index = (int) tour[0] */
			rel_count++;
			return gimme_tree(root, initial_rels,
							  tour, num_gene,
							  rel_count, inner_rel);
		}
		else
		{
			/* tree main part */
			List	   *acceptable_rels = lcons(inner_rel, NIL);
			List	   *new_rels;
			RelOptInfo *new_rel;

			new_rels = make_rels_by_clause_joins(root, old_rel,
												 acceptable_rels);
			/* Shouldn't get more than one result */
			Assert(length(new_rels) <= 1);
			if (new_rels == NIL)
			{
				new_rels = make_rels_by_clauseless_joins(root, old_rel,
														 acceptable_rels);
				Assert(length(new_rels) <= 1);
				if (new_rels == NIL)
					elog(ERROR, "gimme_tree: failed to construct join rel");
			}
			new_rel = (RelOptInfo *) lfirst(new_rels);

			/* Find and save the cheapest paths for this rel */
			set_cheapest(new_rel);

			/* and recurse... */
			rel_count++;
			return gimme_tree(root, initial_rels,
							  tour, num_gene,
							  rel_count, new_rel);
		}
	}

	return old_rel;				/* tree finished ... */
}
