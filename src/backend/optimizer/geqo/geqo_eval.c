/*------------------------------------------------------------------------
 *
 * geqo_eval.c
 *	  Routines to evaluate query trees
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/optimizer/geqo/geqo_eval.c,v 1.65 2003/08/04 02:39:59 momjian Exp $
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

#include <float.h>
#include <limits.h>
#include <math.h>

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
	 * Because gimme_tree considers both left- and right-sided trees,
	 * there is no difference between a tour (a,b,c,d,...) and a tour
	 * (b,a,c,d,...) --- the same join orders will be considered. To avoid
	 * redundant cost calculations, we simply reject tours where tour[0] >
	 * tour[1], assigning them an artificially bad fitness.
	 *
	 * (It would be better to tweak the GEQO logic to not generate such tours
	 * in the first place, but I'm not sure of all the implications in the
	 * mutation logic.)
	 */
	if (num_gene >= 2 && tour[0] > tour[1])
		return DBL_MAX;

	/*
	 * Create a private memory context that will hold all temp storage
	 * allocated inside gimme_tree().
	 *
	 * Since geqo_eval() will be called many times, we can't afford to let
	 * all that memory go unreclaimed until end of statement.  Note we
	 * make the temp context a child of the planner's normal context, so
	 * that it will be freed even if we abort via ereport(ERROR).
	 */
	mycontext = AllocSetContextCreate(CurrentMemoryContext,
									  "GEQO",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(mycontext);

	/*
	 * preserve root->join_rel_list, which gimme_tree changes; without
	 * this, it'll be pointing at recycled storage after the
	 * MemoryContextDelete below.
	 */
	savelist = root->join_rel_list;

	/* construct the best path for the given combination of relations */
	joinrel = gimme_tree(root, initial_rels, tour, num_gene);

	/*
	 * compute fitness
	 *
	 * XXX geqo does not currently support optimization for partial result
	 * retrieval --- how to fix?
	 */
	if (joinrel)
		fitness = joinrel->cheapest_total_path->total_cost;
	else
		fitness = DBL_MAX;

	/* restore join_rel_list */
	root->join_rel_list = savelist;

	/* release all the memory acquired within gimme_tree */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(mycontext);

	return fitness;
}

/*
 * gimme_tree
 *	  Form planner estimates for a join tree constructed in the specified
 *	  order.
 *
 *	 'root' is the Query
 *	 'initial_rels' is the list of initial relations (FROM-list items)
 *	 'tour' is the proposed join order, of length 'num_gene'
 *
 * Returns a new join relation whose cheapest path is the best plan for
 * this join order.  NB: will return NULL if join order is invalid.
 *
 * Note that at each step we consider using the next rel as both left and
 * right side of a join.  However, we cannot build general ("bushy") plan
 * trees this way, only left-sided and right-sided trees.
 */
RelOptInfo *
gimme_tree(Query *root, List *initial_rels,
		   Gene *tour, int num_gene)
{
	RelOptInfo *joinrel;
	int			cur_rel_index;
	int			rel_count;

	/*
	 * Start with the first relation ...
	 */
	cur_rel_index = (int) tour[0];

	joinrel = (RelOptInfo *) nth(cur_rel_index - 1, initial_rels);

	/*
	 * And add on each relation in the specified order ...
	 */
	for (rel_count = 1; rel_count < num_gene; rel_count++)
	{
		RelOptInfo *inner_rel;
		RelOptInfo *new_rel;

		cur_rel_index = (int) tour[rel_count];

		inner_rel = (RelOptInfo *) nth(cur_rel_index - 1, initial_rels);

		/*
		 * Construct a RelOptInfo representing the previous joinrel joined
		 * to inner_rel.  These are always inner joins.  Note that we
		 * expect the joinrel not to exist in root->join_rel_list yet, and
		 * so the paths constructed for it will only include the ones we
		 * want.
		 */
		new_rel = make_join_rel(root, joinrel, inner_rel, JOIN_INNER);

		/* Fail if join order is not valid */
		if (new_rel == NULL)
			return NULL;

		/* Find and save the cheapest paths for this rel */
		set_cheapest(new_rel);

		/* and repeat... */
		joinrel = new_rel;
	}

	return joinrel;
}
