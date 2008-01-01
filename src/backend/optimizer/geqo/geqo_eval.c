/*------------------------------------------------------------------------
 *
 * geqo_eval.c
 *	  Routines to evaluate query trees
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/optimizer/geqo/geqo_eval.c,v 1.87 2008/01/01 19:45:50 momjian Exp $
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
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "utils/memutils.h"


static bool desirable_join(PlannerInfo *root,
			   RelOptInfo *outer_rel, RelOptInfo *inner_rel);


/*
 * geqo_eval
 *
 * Returns cost of a query tree as an individual of the population.
 */
Cost
geqo_eval(Gene *tour, int num_gene, GeqoEvalData *evaldata)
{
	MemoryContext mycontext;
	MemoryContext oldcxt;
	RelOptInfo *joinrel;
	Cost		fitness;
	int			savelength;
	struct HTAB *savehash;

	/*
	 * Because gimme_tree considers both left- and right-sided trees, there is
	 * no difference between a tour (a,b,c,d,...) and a tour (b,a,c,d,...) ---
	 * the same join orders will be considered. To avoid redundant cost
	 * calculations, we simply reject tours where tour[0] > tour[1], assigning
	 * them an artificially bad fitness.
	 *
	 * init_tour() is aware of this rule and so we should never reject a tour
	 * during the initial filling of the pool.	It seems difficult to persuade
	 * the recombination logic never to break the rule, however.
	 */
	if (num_gene >= 2 && tour[0] > tour[1])
		return DBL_MAX;

	/*
	 * Create a private memory context that will hold all temp storage
	 * allocated inside gimme_tree().
	 *
	 * Since geqo_eval() will be called many times, we can't afford to let all
	 * that memory go unreclaimed until end of statement.  Note we make the
	 * temp context a child of the planner's normal context, so that it will
	 * be freed even if we abort via ereport(ERROR).
	 */
	mycontext = AllocSetContextCreate(CurrentMemoryContext,
									  "GEQO",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(mycontext);

	/*
	 * gimme_tree will add entries to root->join_rel_list, which may or may
	 * not already contain some entries.  The newly added entries will be
	 * recycled by the MemoryContextDelete below, so we must ensure that the
	 * list is restored to its former state before exiting.  We can do this by
	 * truncating the list to its original length.	NOTE this assumes that any
	 * added entries are appended at the end!
	 *
	 * We also must take care not to mess up the outer join_rel_hash, if there
	 * is one.	We can do this by just temporarily setting the link to NULL.
	 * (If we are dealing with enough join rels, which we very likely are, a
	 * new hash table will get built and used locally.)
	 */
	savelength = list_length(evaldata->root->join_rel_list);
	savehash = evaldata->root->join_rel_hash;

	evaldata->root->join_rel_hash = NULL;

	/* construct the best path for the given combination of relations */
	joinrel = gimme_tree(tour, num_gene, evaldata);

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

	/*
	 * Restore join_rel_list to its former state, and put back original
	 * hashtable if any.
	 */
	evaldata->root->join_rel_list = list_truncate(evaldata->root->join_rel_list,
												  savelength);
	evaldata->root->join_rel_hash = savehash;

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
 *	 'tour' is the proposed join order, of length 'num_gene'
 *	 'evaldata' contains the context we need
 *
 * Returns a new join relation whose cheapest path is the best plan for
 * this join order.  NB: will return NULL if join order is invalid.
 *
 * The original implementation of this routine always joined in the specified
 * order, and so could only build left-sided plans (and right-sided and
 * mixtures, as a byproduct of the fact that make_join_rel() is symmetric).
 * It could never produce a "bushy" plan.  This had a couple of big problems,
 * of which the worst was that as of 7.4, there are situations involving IN
 * subqueries where the only valid plans are bushy.
 *
 * The present implementation takes the given tour as a guideline, but
 * postpones joins that seem unsuitable according to some heuristic rules.
 * This allows correct bushy plans to be generated at need, and as a nice
 * side-effect it seems to materially improve the quality of the generated
 * plans.
 */
RelOptInfo *
gimme_tree(Gene *tour, int num_gene, GeqoEvalData *evaldata)
{
	RelOptInfo **stack;
	int			stack_depth;
	RelOptInfo *joinrel;
	int			rel_count;

	/*
	 * Create a stack to hold not-yet-joined relations.
	 */
	stack = (RelOptInfo **) palloc(num_gene * sizeof(RelOptInfo *));
	stack_depth = 0;

	/*
	 * Push each relation onto the stack in the specified order.  After
	 * pushing each relation, see whether the top two stack entries are
	 * joinable according to the desirable_join() heuristics.  If so, join
	 * them into one stack entry, and try again to combine with the next stack
	 * entry down (if any).  When the stack top is no longer joinable,
	 * continue to the next input relation.  After we have pushed the last
	 * input relation, the heuristics are disabled and we force joining all
	 * the remaining stack entries.
	 *
	 * If desirable_join() always returns true, this produces a straight
	 * left-to-right join just like the old code.  Otherwise we may produce a
	 * bushy plan or a left/right-sided plan that really corresponds to some
	 * tour other than the one given.  To the extent that the heuristics are
	 * helpful, however, this will be a better plan than the raw tour.
	 *
	 * Also, when a join attempt fails (because of OJ or IN constraints), we
	 * may be able to recover and produce a workable plan, where the old code
	 * just had to give up.  This case acts the same as a false result from
	 * desirable_join().
	 */
	for (rel_count = 0; rel_count < num_gene; rel_count++)
	{
		int			cur_rel_index;

		/* Get the next input relation and push it */
		cur_rel_index = (int) tour[rel_count];
		stack[stack_depth] = (RelOptInfo *) list_nth(evaldata->initial_rels,
													 cur_rel_index - 1);
		stack_depth++;

		/*
		 * While it's feasible, pop the top two stack entries and replace with
		 * their join.
		 */
		while (stack_depth >= 2)
		{
			RelOptInfo *outer_rel = stack[stack_depth - 2];
			RelOptInfo *inner_rel = stack[stack_depth - 1];

			/*
			 * Don't pop if heuristics say not to join now.  However, once we
			 * have exhausted the input, the heuristics can't prevent popping.
			 */
			if (rel_count < num_gene - 1 &&
				!desirable_join(evaldata->root, outer_rel, inner_rel))
				break;

			/*
			 * Construct a RelOptInfo representing the join of these two input
			 * relations.  Note that we expect the joinrel not to exist in
			 * root->join_rel_list yet, and so the paths constructed for it
			 * will only include the ones we want.
			 */
			joinrel = make_join_rel(evaldata->root, outer_rel, inner_rel);

			/* Can't pop stack here if join order is not valid */
			if (!joinrel)
				break;

			/* Find and save the cheapest paths for this rel */
			set_cheapest(joinrel);

			/* Pop the stack and replace the inputs with their join */
			stack_depth--;
			stack[stack_depth - 1] = joinrel;
		}
	}

	/* Did we succeed in forming a single join relation? */
	if (stack_depth == 1)
		joinrel = stack[0];
	else
		joinrel = NULL;

	pfree(stack);

	return joinrel;
}

/*
 * Heuristics for gimme_tree: do we want to join these two relations?
 */
static bool
desirable_join(PlannerInfo *root,
			   RelOptInfo *outer_rel, RelOptInfo *inner_rel)
{
	/*
	 * Join if there is an applicable join clause, or if there is a join order
	 * restriction forcing these rels to be joined.
	 */
	if (have_relevant_joinclause(root, outer_rel, inner_rel) ||
		have_join_order_restriction(root, outer_rel, inner_rel))
		return true;

	/* Otherwise postpone the join till later. */
	return false;
}
