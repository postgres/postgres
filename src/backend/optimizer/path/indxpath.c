/*-------------------------------------------------------------------------
 *
 * indxpath.c
 *	  Routines to determine which indexes are usable for scanning a
 *	  given relation, and create Paths accordingly.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/indxpath.c,v 1.191.2.8 2006/05/18 19:56:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/skey.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/predtest.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/selfuncs.h"


/*
 * DoneMatchingIndexKeys() - MACRO
 */
#define DoneMatchingIndexKeys(classes)	(classes[0] == InvalidOid)

#define is_indexable_operator(clause,opclass,indexkey_on_left) \
	(indexable_operator(clause,opclass,indexkey_on_left) != InvalidOid)

#define IsBooleanOpclass(opclass) \
	((opclass) == BOOL_BTREE_OPS_OID || (opclass) == BOOL_HASH_OPS_OID)


static List *find_usable_indexes(PlannerInfo *root, RelOptInfo *rel,
					List *clauses, List *outer_clauses,
					bool istoplevel, bool isjoininner,
					Relids outer_relids);
static Path *choose_bitmap_and(PlannerInfo *root, RelOptInfo *rel, List *paths);
static int	bitmap_path_comparator(const void *a, const void *b);
static Cost bitmap_and_cost_est(PlannerInfo *root, RelOptInfo *rel, List *paths);
static List *pull_indexpath_quals(Path *bitmapqual);
static bool lists_intersect_ptr(List *list1, List *list2);
static bool match_clause_to_indexcol(IndexOptInfo *index,
						 int indexcol, Oid opclass,
						 RestrictInfo *rinfo,
						 Relids outer_relids);
static Oid indexable_operator(Expr *clause, Oid opclass,
				   bool indexkey_on_left);
static Relids indexable_outerrelids(RelOptInfo *rel);
static bool matches_any_index(RestrictInfo *rinfo, RelOptInfo *rel,
				  Relids outer_relids);
static List *find_clauses_for_join(PlannerInfo *root, RelOptInfo *rel,
					  Relids outer_relids, bool isouterjoin);
static ScanDirection match_variant_ordering(PlannerInfo *root,
					   IndexOptInfo *index,
					   List *restrictclauses);
static List *identify_ignorable_ordering_cols(PlannerInfo *root,
								 IndexOptInfo *index,
								 List *restrictclauses);
static bool match_index_to_query_keys(PlannerInfo *root,
						  IndexOptInfo *index,
						  ScanDirection indexscandir,
						  List *ignorables);
static bool match_boolean_index_clause(Node *clause, int indexcol,
						   IndexOptInfo *index);
static bool match_special_index_operator(Expr *clause, Oid opclass,
							 bool indexkey_on_left);
static Expr *expand_boolean_index_clause(Node *clause, int indexcol,
							IndexOptInfo *index);
static List *expand_indexqual_condition(RestrictInfo *rinfo, Oid opclass);
static List *prefix_quals(Node *leftop, Oid opclass,
			 Const *prefix, Pattern_Prefix_Status pstatus);
static List *network_prefix_quals(Node *leftop, Oid expr_op, Oid opclass,
					 Datum rightop);
static Datum string_to_datum(const char *str, Oid datatype);
static Const *string_to_const(const char *str, Oid datatype);


/*
 * create_index_paths()
 *	  Generate all interesting index paths for the given relation.
 *	  Candidate paths are added to the rel's pathlist (using add_path).
 *
 * To be considered for an index scan, an index must match one or more
 * restriction clauses or join clauses from the query's qual condition,
 * or match the query's ORDER BY condition, or have a predicate that
 * matches the query's qual condition.
 *
 * There are two basic kinds of index scans.  A "plain" index scan uses
 * only restriction clauses (possibly none at all) in its indexqual,
 * so it can be applied in any context.  An "innerjoin" index scan uses
 * join clauses (plus restriction clauses, if available) in its indexqual.
 * Therefore it can only be used as the inner relation of a nestloop
 * join against an outer rel that includes all the other rels mentioned
 * in its join clauses.  In that context, values for the other rels'
 * attributes are available and fixed during any one scan of the indexpath.
 *
 * An IndexPath is generated and submitted to add_path() for each plain index
 * scan this routine deems potentially interesting for the current query.
 *
 * We also determine the set of other relids that participate in join
 * clauses that could be used with each index.	The actually best innerjoin
 * path will be generated for each outer relation later on, but knowing the
 * set of potential otherrels allows us to identify equivalent outer relations
 * and avoid repeated computation.
 *
 * 'rel' is the relation for which we want to generate index paths
 *
 * Note: check_partial_indexes() must have been run previously.
 */
void
create_index_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *indexpaths;
	List	   *bitindexpaths;
	ListCell   *l;

	/* Skip the whole mess if no indexes */
	if (rel->indexlist == NIL)
	{
		rel->index_outer_relids = NULL;
		return;
	}

	/*
	 * Examine join clauses to see which ones are potentially usable with
	 * indexes of this rel, and generate the set of all other relids that
	 * participate in such join clauses.  We'll use this set later to
	 * recognize outer rels that are equivalent for joining purposes.
	 */
	rel->index_outer_relids = indexable_outerrelids(rel);

	/*
	 * Find all the index paths that are directly usable for this relation
	 * (ie, are valid without considering OR or JOIN clauses).
	 */
	indexpaths = find_usable_indexes(root, rel,
									 rel->baserestrictinfo, NIL,
									 true, false, NULL);

	/*
	 * We can submit them all to add_path.	(This generates access paths for
	 * plain IndexScan plans.)	However, for the next step we will only want
	 * the ones that have some selectivity; we must discard anything that was
	 * generated solely for ordering purposes.
	 */
	bitindexpaths = NIL;
	foreach(l, indexpaths)
	{
		IndexPath  *ipath = (IndexPath *) lfirst(l);

		add_path(rel, (Path *) ipath);

		if (ipath->indexselectivity < 1.0 &&
			!ScanDirectionIsBackward(ipath->indexscandir))
			bitindexpaths = lappend(bitindexpaths, ipath);
	}

	/*
	 * Generate BitmapOrPaths for any suitable OR-clauses present in the
	 * restriction list.  Add these to bitindexpaths.
	 */
	indexpaths = generate_bitmap_or_paths(root, rel,
										  rel->baserestrictinfo, NIL,
										  false, NULL);
	bitindexpaths = list_concat(bitindexpaths, indexpaths);

	/*
	 * If we found anything usable, generate a BitmapHeapPath for the most
	 * promising combination of bitmap index paths.
	 */
	if (bitindexpaths != NIL)
	{
		Path	   *bitmapqual;
		BitmapHeapPath *bpath;

		bitmapqual = choose_bitmap_and(root, rel, bitindexpaths);
		bpath = create_bitmap_heap_path(root, rel, bitmapqual, false);
		add_path(rel, (Path *) bpath);
	}
}


/*----------
 * find_usable_indexes
 *	  Given a list of restriction clauses, find all the potentially usable
 *	  indexes for the given relation, and return a list of IndexPaths.
 *
 * The caller actually supplies two lists of restriction clauses: some
 * "current" ones and some "outer" ones.  Both lists can be used freely
 * to match keys of the index, but an index must use at least one of the
 * "current" clauses to be considered usable.  The motivation for this is
 * examples like
 *		WHERE (x = 42) AND (... OR (y = 52 AND z = 77) OR ....)
 * While we are considering the y/z subclause of the OR, we can use "x = 42"
 * as one of the available index conditions; but we shouldn't match the
 * subclause to any index on x alone, because such a Path would already have
 * been generated at the upper level.  So we could use an index on x,y,z
 * or an index on x,y for the OR subclause, but not an index on just x.
 * When dealing with a partial index, a match of the index predicate to
 * one of the "current" clauses also makes the index usable.
 *
 * If istoplevel is true (indicating we are considering the top level of a
 * rel's restriction clauses), we will include indexes in the result that
 * have an interesting sort order, even if they have no matching restriction
 * clauses.
 *
 * 'rel' is the relation for which we want to generate index paths
 * 'clauses' is the current list of clauses (RestrictInfo nodes)
 * 'outer_clauses' is the list of additional upper-level clauses
 * 'istoplevel' is true if clauses are the rel's top-level restriction list
 *		(outer_clauses must be NIL when this is true)
 * 'isjoininner' is true if forming an inner indexscan (so some of the
 *		given clauses are join clauses)
 * 'outer_relids' identifies the outer side of the join (pass NULL
 *		if not isjoininner)
 *
 * Note: check_partial_indexes() must have been run previously.
 *----------
 */
static List *
find_usable_indexes(PlannerInfo *root, RelOptInfo *rel,
					List *clauses, List *outer_clauses,
					bool istoplevel, bool isjoininner,
					Relids outer_relids)
{
	List	   *result = NIL;
	List	   *all_clauses = NIL;		/* not computed till needed */
	ListCell   *ilist;

	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
		IndexPath  *ipath;
		List	   *restrictclauses;
		List	   *index_pathkeys;
		List	   *useful_pathkeys;
		bool		useful_predicate;
		bool		found_clause;
		bool		index_is_ordered;

		/*
		 * Ignore partial indexes that do not match the query.	If a partial
		 * index is marked predOK then we know it's OK; otherwise, if we are
		 * at top level we know it's not OK (since predOK is exactly whether
		 * its predicate could be proven from the toplevel clauses).
		 * Otherwise, we have to test whether the added clauses are sufficient
		 * to imply the predicate.	If so, we could use the index in the
		 * current context.
		 *
		 * We set useful_predicate to true iff the predicate was proven using
		 * the current set of clauses.	This is needed to prevent matching a
		 * predOK index to an arm of an OR, which would be a legal but
		 * pointlessly inefficient plan.  (A better plan will be generated by
		 * just scanning the predOK index alone, no OR.)
		 */
		useful_predicate = false;
		if (index->indpred != NIL)
		{
			if (index->predOK)
			{
				if (istoplevel)
				{
					/* we know predicate was proven from these clauses */
					useful_predicate = true;
				}
			}
			else
			{
				if (istoplevel)
					continue;	/* no point in trying to prove it */

				/* Form all_clauses if not done already */
				if (all_clauses == NIL)
					all_clauses = list_concat(list_copy(clauses),
											  outer_clauses);

				if (!predicate_implied_by(index->indpred, all_clauses))
					continue;	/* can't use it at all */

				if (!predicate_implied_by(index->indpred, outer_clauses))
					useful_predicate = true;
			}
		}

		/*
		 * 1. Match the index against the available restriction clauses.
		 * found_clause is set true only if at least one of the current
		 * clauses was used.
		 */
		restrictclauses = group_clauses_by_indexkey(index,
													clauses,
													outer_clauses,
													outer_relids,
													&found_clause);

		/*
		 * Not all index AMs support scans with no restriction clauses. We
		 * can't generate a scan over an index with amoptionalkey = false
		 * unless there's at least one restriction clause.
		 */
		if (restrictclauses == NIL && !index->amoptionalkey)
			continue;

		/*
		 * 2. Compute pathkeys describing index's ordering, if any, then see
		 * how many of them are actually useful for this query.  This is not
		 * relevant unless we are at top level.
		 */
		index_is_ordered = OidIsValid(index->ordering[0]);
		if (istoplevel && index_is_ordered && !isjoininner)
		{
			index_pathkeys = build_index_pathkeys(root, index,
												  ForwardScanDirection,
												  true);
			useful_pathkeys = truncate_useless_pathkeys(root, rel,
														index_pathkeys);
		}
		else
			useful_pathkeys = NIL;

		/*
		 * 3. Generate an indexscan path if there are relevant restriction
		 * clauses in the current clauses, OR the index ordering is
		 * potentially useful for later merging or final output ordering, OR
		 * the index has a predicate that was proven by the current clauses.
		 */
		if (found_clause || useful_pathkeys != NIL || useful_predicate)
		{
			ipath = create_index_path(root, index,
									  restrictclauses,
									  useful_pathkeys,
									  index_is_ordered ?
									  ForwardScanDirection :
									  NoMovementScanDirection,
									  isjoininner);
			result = lappend(result, ipath);
		}

		/*
		 * 4. If the index is ordered, and there is a requested query ordering
		 * that we failed to match, consider variant ways of achieving the
		 * ordering.  Again, this is only interesting at top level.
		 */
		if (istoplevel && index_is_ordered && !isjoininner &&
			root->query_pathkeys != NIL &&
			pathkeys_useful_for_ordering(root, useful_pathkeys) == 0)
		{
			ScanDirection scandir;

			scandir = match_variant_ordering(root, index, restrictclauses);
			if (!ScanDirectionIsNoMovement(scandir))
			{
				ipath = create_index_path(root, index,
										  restrictclauses,
										  root->query_pathkeys,
										  scandir,
										  false);
				result = lappend(result, ipath);
			}
		}
	}

	return result;
}


/*
 * generate_bitmap_or_paths
 *		Look through the list of clauses to find OR clauses, and generate
 *		a BitmapOrPath for each one we can handle that way.  Return a list
 *		of the generated BitmapOrPaths.
 *
 * outer_clauses is a list of additional clauses that can be assumed true
 * for the purpose of generating indexquals, but are not to be searched for
 * ORs.  (See find_usable_indexes() for motivation.)
 */
List *
generate_bitmap_or_paths(PlannerInfo *root, RelOptInfo *rel,
						 List *clauses, List *outer_clauses,
						 bool isjoininner,
						 Relids outer_relids)
{
	List	   *result = NIL;
	List	   *all_clauses;
	ListCell   *l;

	/*
	 * We can use both the current and outer clauses as context for
	 * find_usable_indexes
	 */
	all_clauses = list_concat(list_copy(clauses), outer_clauses);

	foreach(l, clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
		List	   *pathlist;
		Path	   *bitmapqual;
		ListCell   *j;

		Assert(IsA(rinfo, RestrictInfo));
		/* Ignore RestrictInfos that aren't ORs */
		if (!restriction_is_or_clause(rinfo))
			continue;

		/*
		 * We must be able to match at least one index to each of the arms of
		 * the OR, else we can't use it.
		 */
		pathlist = NIL;
		foreach(j, ((BoolExpr *) rinfo->orclause)->args)
		{
			Node	   *orarg = (Node *) lfirst(j);
			List	   *indlist;

			/* OR arguments should be ANDs or sub-RestrictInfos */
			if (and_clause(orarg))
			{
				List	   *andargs = ((BoolExpr *) orarg)->args;

				indlist = find_usable_indexes(root, rel,
											  andargs,
											  all_clauses,
											  false,
											  isjoininner,
											  outer_relids);
				/* Recurse in case there are sub-ORs */
				indlist = list_concat(indlist,
									  generate_bitmap_or_paths(root, rel,
															   andargs,
															   all_clauses,
															   isjoininner,
															   outer_relids));
			}
			else
			{
				Assert(IsA(orarg, RestrictInfo));
				Assert(!restriction_is_or_clause((RestrictInfo *) orarg));
				indlist = find_usable_indexes(root, rel,
											  list_make1(orarg),
											  all_clauses,
											  false,
											  isjoininner,
											  outer_relids);
			}

			/*
			 * If nothing matched this arm, we can't do anything with this OR
			 * clause.
			 */
			if (indlist == NIL)
			{
				pathlist = NIL;
				break;
			}

			/*
			 * OK, pick the most promising AND combination, and add it to
			 * pathlist.
			 */
			bitmapqual = choose_bitmap_and(root, rel, indlist);
			pathlist = lappend(pathlist, bitmapqual);
		}

		/*
		 * If we have a match for every arm, then turn them into a
		 * BitmapOrPath, and add to result list.
		 */
		if (pathlist != NIL)
		{
			bitmapqual = (Path *) create_bitmap_or_path(root, rel, pathlist);
			result = lappend(result, bitmapqual);
		}
	}

	return result;
}


/*
 * choose_bitmap_and
 *		Given a nonempty list of bitmap paths, AND them into one path.
 *
 * This is a nontrivial decision since we can legally use any subset of the
 * given path set.	We want to choose a good tradeoff between selectivity
 * and cost of computing the bitmap.
 *
 * The result is either a single one of the inputs, or a BitmapAndPath
 * combining multiple inputs.
 */
static Path *
choose_bitmap_and(PlannerInfo *root, RelOptInfo *rel, List *paths)
{
	int			npaths = list_length(paths);
	Path	  **patharray;
	Cost		costsofar;
	List	   *qualsofar;
	ListCell   *lastcell;
	int			i;
	ListCell   *l;

	Assert(npaths > 0);			/* else caller error */
	if (npaths == 1)
		return (Path *) linitial(paths);		/* easy case */

	/*
	 * In theory we should consider every nonempty subset of the given paths.
	 * In practice that seems like overkill, given the crude nature of the
	 * estimates, not to mention the possible effects of higher-level AND and
	 * OR clauses.	As a compromise, we sort the paths by selectivity.  We
	 * always take the first, and sequentially add on paths that result in a
	 * lower estimated cost.
	 *
	 * We also make some effort to detect directly redundant input paths, as
	 * can happen if there are multiple possibly usable indexes.  We
	 * consider an index redundant if any of its index conditions were already
	 * used by earlier indexes.  (We could use predicate_implied_by to have a
	 * more intelligent, but much more expensive, check --- but in most cases
	 * simple pointer equality should suffice, since after all the index
	 * conditions are all coming from the same RestrictInfo lists.)
	 *
	 * You might think the condition for redundancy should be "all index
	 * conditions already used", not "any", but this turns out to be wrong.
	 * For example, if we use an index on A, and then come to an index with
	 * conditions on A and B, the only way that the second index can be later
	 * in the selectivity-order sort is if the condition on B is completely
	 * non-selective.  In any case, we'd surely be drastically misestimating
	 * the selectivity if we count the same condition twice.
	 *
	 * We include index predicate conditions in the redundancy test.  Because
	 * the test is just for pointer equality and not equal(), the effect is
	 * that use of the same partial index in two different AND elements is
	 * considered redundant.  (XXX is this too strong?)
	 *
	 * Note: outputting the selected sub-paths in selectivity order is a good
	 * thing even if we weren't using that as part of the selection method,
	 * because it makes the short-circuit case in MultiExecBitmapAnd() more
	 * likely to apply.
	 */

	/* Convert list to array so we can apply qsort */
	patharray = (Path **) palloc(npaths * sizeof(Path *));
	i = 0;
	foreach(l, paths)
	{
		patharray[i++] = (Path *) lfirst(l);
	}
	qsort(patharray, npaths, sizeof(Path *), bitmap_path_comparator);

	paths = list_make1(patharray[0]);
	costsofar = bitmap_and_cost_est(root, rel, paths);
	qualsofar = pull_indexpath_quals(patharray[0]);
	lastcell = list_head(paths);	/* for quick deletions */

	for (i = 1; i < npaths; i++)
	{
		Path	   *newpath = patharray[i];
		List	   *newqual;
		Cost		newcost;

		newqual = pull_indexpath_quals(newpath);
		if (lists_intersect_ptr(newqual, qualsofar))
			continue;			/* consider it redundant */
		/* tentatively add newpath to paths, so we can estimate cost */
		paths = lappend(paths, newpath);
		newcost = bitmap_and_cost_est(root, rel, paths);
		if (newcost < costsofar)
		{
			/* keep newpath in paths, update subsidiary variables */
			costsofar = newcost;
			qualsofar = list_concat(qualsofar, newqual);
			lastcell = lnext(lastcell);
		}
		else
		{
			/* reject newpath, remove it from paths list */
			paths = list_delete_cell(paths, lnext(lastcell), lastcell);
		}
		Assert(lnext(lastcell) == NULL);
	}

	if (list_length(paths) == 1)
		return (Path *) linitial(paths);		/* no need for AND */
	return (Path *) create_bitmap_and_path(root, rel, paths);
}

/* qsort comparator to sort in increasing selectivity order */
static int
bitmap_path_comparator(const void *a, const void *b)
{
	Path	   *pa = *(Path *const *) a;
	Path	   *pb = *(Path *const *) b;
	Cost		acost;
	Cost		bcost;
	Selectivity aselec;
	Selectivity bselec;
	Selectivity diff;

	cost_bitmap_tree_node(pa, &acost, &aselec);
	cost_bitmap_tree_node(pb, &bcost, &bselec);

	/*
	 * Since selectivities are often pretty crude, don't put blind faith
	 * in them; if the selectivities are within 1% of being the same, treat
	 * them as equal and sort by cost instead.
	 */
	diff = aselec - bselec;
	if (diff < -0.01)
		return -1;
	if (diff > 0.01)
		return 1;

	if (acost < bcost)
		return -1;
	if (acost > bcost)
		return 1;

	return 0;
}

/*
 * Estimate the cost of actually executing a BitmapAnd with the given
 * inputs.
 */
static Cost
bitmap_and_cost_est(PlannerInfo *root, RelOptInfo *rel, List *paths)
{
	BitmapAndPath apath;
	Path		bpath;

	/* Set up a dummy BitmapAndPath */
	apath.path.type = T_BitmapAndPath;
	apath.path.parent = rel;
	apath.bitmapquals = paths;
	cost_bitmap_and_node(&apath, root);

	/* Now we can do cost_bitmap_heap_scan */
	cost_bitmap_heap_scan(&bpath, root, rel, (Path *) &apath, false);

	return bpath.total_cost;
}

/*
 * pull_indexpath_quals
 *
 * Given the Path structure for a plain or bitmap indexscan, extract a list
 * of all the indexquals and index predicate conditions used in the Path.
 *
 * This is sort of a simplified version of make_restrictinfo_from_bitmapqual;
 * here, we are not trying to produce an accurate representation of the AND/OR
 * semantics of the Path, but just find out all the base conditions used.
 *
 * The result list contains pointers to the expressions used in the Path,
 * but all the list cells are freshly built, so it's safe to destructively
 * modify the list (eg, by concat'ing it with other lists).
 */
static List *
pull_indexpath_quals(Path *bitmapqual)
{
	List	   *result = NIL;
	ListCell   *l;

	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;

		foreach(l, apath->bitmapquals)
		{
			List	   *sublist;

			sublist = pull_indexpath_quals((Path *) lfirst(l));
			result = list_concat(result, sublist);
		}
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;

		foreach(l, opath->bitmapquals)
		{
			List	   *sublist;

			sublist = pull_indexpath_quals((Path *) lfirst(l));
			result = list_concat(result, sublist);
		}
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;

		result = get_actual_clauses(ipath->indexclauses);
		result = list_concat(result, list_copy(ipath->indexinfo->indpred));
	}
	else
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));

	return result;
}


/*
 * lists_intersect_ptr
 *		Detect whether two lists have a nonempty intersection,
 *		using pointer equality to compare members.
 *
 * This possibly should go into list.c, but it doesn't yet have any use
 * except in choose_bitmap_and.
 */
static bool
lists_intersect_ptr(List *list1, List *list2)
{
	ListCell   *cell1;

	foreach(cell1, list1)
	{
		void   *datum1 = lfirst(cell1);
		ListCell   *cell2;

		foreach(cell2, list2)
		{
			if (lfirst(cell2) == datum1)
				return true;
		}
	}

	return false;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK RESTRICTIONS  ----
 ****************************************************************************/


/*
 * group_clauses_by_indexkey
 *	  Find restriction clauses that can be used with an index.
 *
 * Returns a list of sublists of RestrictInfo nodes for clauses that can be
 * used with this index.  Each sublist contains clauses that can be used
 * with one index key (in no particular order); the top list is ordered by
 * index key.  (This is depended on by expand_indexqual_conditions().)
 *
 * We can use clauses from either the current clauses or outer_clauses lists,
 * but *found_clause is set TRUE only if we used at least one clause from
 * the "current clauses" list.	See find_usable_indexes() for motivation.
 *
 * outer_relids determines what Vars will be allowed on the other side
 * of a possible index qual; see match_clause_to_indexcol().
 *
 * If the index has amoptionalkey = false, we give up and return NIL when
 * there are no restriction clauses matching the first index key.  Otherwise,
 * we return NIL if there are no restriction clauses matching any index key.
 * A non-NIL result will have one (possibly empty) sublist for each index key.
 *
 * Example: given an index on (A,B,C), we would return ((C1 C2) () (C3 C4))
 * if we find that clauses C1 and C2 use column A, clauses C3 and C4 use
 * column C, and no clauses use column B.
 *
 * Note: in some circumstances we may find the same RestrictInfos coming
 * from multiple places.  Defend against redundant outputs by using
 * list_append_unique_ptr (pointer equality should be good enough).
 */
List *
group_clauses_by_indexkey(IndexOptInfo *index,
						  List *clauses, List *outer_clauses,
						  Relids outer_relids,
						  bool *found_clause)
{
	List	   *clausegroup_list = NIL;
	bool		found_outer_clause = false;
	int			indexcol = 0;
	Oid		   *classes = index->classlist;

	*found_clause = false;		/* default result */

	if (clauses == NIL && outer_clauses == NIL)
		return NIL;				/* cannot succeed */

	do
	{
		Oid			curClass = classes[0];
		List	   *clausegroup = NIL;
		ListCell   *l;

		/* check the current clauses */
		foreach(l, clauses)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			Assert(IsA(rinfo, RestrictInfo));
			if (match_clause_to_indexcol(index,
										 indexcol,
										 curClass,
										 rinfo,
										 outer_relids))
			{
				clausegroup = list_append_unique_ptr(clausegroup, rinfo);
				*found_clause = true;
			}
		}

		/* check the outer clauses */
		foreach(l, outer_clauses)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			Assert(IsA(rinfo, RestrictInfo));
			if (match_clause_to_indexcol(index,
										 indexcol,
										 curClass,
										 rinfo,
										 outer_relids))
			{
				clausegroup = list_append_unique_ptr(clausegroup, rinfo);
				found_outer_clause = true;
			}
		}

		/*
		 * If no clauses match this key, check for amoptionalkey restriction.
		 */
		if (clausegroup == NIL && !index->amoptionalkey && indexcol == 0)
			return NIL;

		clausegroup_list = lappend(clausegroup_list, clausegroup);

		indexcol++;
		classes++;

	} while (!DoneMatchingIndexKeys(classes));

	if (!*found_clause && !found_outer_clause)
		return NIL;				/* no indexable clauses anywhere */

	return clausegroup_list;
}


/*
 * match_clause_to_indexcol()
 *	  Determines whether a restriction clause matches a column of an index.
 *
 *	  To match a normal index, the clause:
 *
 *	  (1)  must be in the form (indexkey op const) or (const op indexkey);
 *		   and
 *	  (2)  must contain an operator which is in the same class as the index
 *		   operator for this column, or is a "special" operator as recognized
 *		   by match_special_index_operator().
 *
 *	  Our definition of "const" is pretty liberal: we allow Vars belonging
 *	  to the caller-specified outer_relids relations (which had better not
 *	  include the relation whose index is being tested).  outer_relids should
 *	  be NULL when checking simple restriction clauses, and the outer side
 *	  of the join when building a join inner scan.	Other than that, the
 *	  only thing we don't like is volatile functions.
 *
 *	  Note: in most cases we already know that the clause as a whole uses
 *	  vars from the interesting set of relations.  The reason for the
 *	  outer_relids test is to reject clauses like (a.f1 OP (b.f2 OP a.f3));
 *	  that's not processable by an indexscan nestloop join on A, whereas
 *	  (a.f1 OP (b.f2 OP c.f3)) is.
 *
 *	  Presently, the executor can only deal with indexquals that have the
 *	  indexkey on the left, so we can only use clauses that have the indexkey
 *	  on the right if we can commute the clause to put the key on the left.
 *	  We do not actually do the commuting here, but we check whether a
 *	  suitable commutator operator is available.
 *
 *	  For boolean indexes, it is also possible to match the clause directly
 *	  to the indexkey; or perhaps the clause is (NOT indexkey).
 *
 * 'index' is the index of interest.
 * 'indexcol' is a column number of 'index' (counting from 0).
 * 'opclass' is the corresponding operator class.
 * 'rinfo' is the clause to be tested (as a RestrictInfo node).
 *
 * Returns true if the clause can be used with this index key.
 *
 * NOTE:  returns false if clause is an OR or AND clause; it is the
 * responsibility of higher-level routines to cope with those.
 */
static bool
match_clause_to_indexcol(IndexOptInfo *index,
						 int indexcol,
						 Oid opclass,
						 RestrictInfo *rinfo,
						 Relids outer_relids)
{
	Expr	   *clause = rinfo->clause;
	Node	   *leftop,
			   *rightop;

	/* First check for boolean-index cases. */
	if (IsBooleanOpclass(opclass))
	{
		if (match_boolean_index_clause((Node *) clause, indexcol, index))
			return true;
	}

	/* Else clause must be a binary opclause. */
	if (!is_opclause(clause))
		return false;
	leftop = get_leftop(clause);
	rightop = get_rightop(clause);
	if (!leftop || !rightop)
		return false;

	/*
	 * Check for clauses of the form: (indexkey operator constant) or
	 * (constant operator indexkey).  See above notes about const-ness.
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		bms_is_subset(rinfo->right_relids, outer_relids) &&
		!contain_volatile_functions(rightop))
	{
		if (is_indexable_operator(clause, opclass, true))
			return true;

		/*
		 * If we didn't find a member of the index's opclass, see whether it
		 * is a "special" indexable operator.
		 */
		if (match_special_index_operator(clause, opclass, true))
			return true;
		return false;
	}

	if (match_index_to_operand(rightop, indexcol, index) &&
		bms_is_subset(rinfo->left_relids, outer_relids) &&
		!contain_volatile_functions(leftop))
	{
		if (is_indexable_operator(clause, opclass, false))
			return true;

		/*
		 * If we didn't find a member of the index's opclass, see whether it
		 * is a "special" indexable operator.
		 */
		if (match_special_index_operator(clause, opclass, false))
			return true;
		return false;
	}

	return false;
}

/*
 * indexable_operator
 *	  Does a binary opclause contain an operator matching the index opclass?
 *
 * If the indexkey is on the right, what we actually want to know
 * is whether the operator has a commutator operator that matches
 * the index's opclass.
 *
 * Returns the OID of the matching operator, or InvalidOid if no match.
 * (Formerly, this routine might return a binary-compatible operator
 * rather than the original one, but that kluge is history.)
 */
static Oid
indexable_operator(Expr *clause, Oid opclass, bool indexkey_on_left)
{
	Oid			expr_op = ((OpExpr *) clause)->opno;
	Oid			commuted_op;

	/* Get the commuted operator if necessary */
	if (indexkey_on_left)
		commuted_op = expr_op;
	else
		commuted_op = get_commutator(expr_op);
	if (commuted_op == InvalidOid)
		return InvalidOid;

	/* OK if the (commuted) operator is a member of the index's opclass */
	if (op_in_opclass(commuted_op, opclass))
		return expr_op;

	return InvalidOid;
}

/****************************************************************************
 *				----  ROUTINES TO DO PARTIAL INDEX PREDICATE TESTS	----
 ****************************************************************************/

/*
 * check_partial_indexes
 *		Check each partial index of the relation, and mark it predOK or not
 *		depending on whether the predicate is satisfied for this query.
 */
void
check_partial_indexes(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *restrictinfo_list = rel->baserestrictinfo;
	ListCell   *ilist;

	/*
	 * Note: if Postgres tried to optimize queries by forming equivalence
	 * classes over equi-joined attributes (i.e., if it recognized that a
	 * qualification such as "where a.b=c.d and a.b=5" could make use of an
	 * index on c.d), then we could use that equivalence class info here with
	 * joininfo lists to do more complete tests for the usability of a partial
	 * index.  For now, the test only uses restriction clauses (those in
	 * baserestrictinfo). --Nels, Dec '92
	 *
	 * XXX as of 7.1, equivalence class info *is* available.  Consider
	 * improving this code as foreseen by Nels.
	 */

	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);

		if (index->indpred == NIL)
			continue;			/* ignore non-partial indexes */

		index->predOK = predicate_implied_by(index->indpred,
											 restrictinfo_list);
	}
}

/****************************************************************************
 *				----  ROUTINES TO CHECK JOIN CLAUSES  ----
 ****************************************************************************/

/*
 * indexable_outerrelids
 *	  Finds all other relids that participate in any indexable join clause
 *	  for the specified table.	Returns a set of relids.
 */
static Relids
indexable_outerrelids(RelOptInfo *rel)
{
	Relids		outer_relids = NULL;
	ListCell   *l;

	/*
	 * Examine each joinclause in the joininfo list to see if it matches any
	 * key of any index.  If so, add the clause's other rels to the result.
	 */
	foreach(l, rel->joininfo)
	{
		RestrictInfo *joininfo = (RestrictInfo *) lfirst(l);
		Relids		other_rels;

		other_rels = bms_difference(joininfo->required_relids, rel->relids);
		if (matches_any_index(joininfo, rel, other_rels))
			outer_relids = bms_join(outer_relids, other_rels);
		else
			bms_free(other_rels);
	}

	return outer_relids;
}

/*
 * matches_any_index
 *	  Workhorse for indexable_outerrelids: see if a joinclause can be
 *	  matched to any index of the given rel.
 */
static bool
matches_any_index(RestrictInfo *rinfo, RelOptInfo *rel, Relids outer_relids)
{
	ListCell   *l;

	Assert(IsA(rinfo, RestrictInfo));

	if (restriction_is_or_clause(rinfo))
	{
		foreach(l, ((BoolExpr *) rinfo->orclause)->args)
		{
			Node	   *orarg = (Node *) lfirst(l);

			/* OR arguments should be ANDs or sub-RestrictInfos */
			if (and_clause(orarg))
			{
				ListCell   *j;

				/* Recurse to examine AND items and sub-ORs */
				foreach(j, ((BoolExpr *) orarg)->args)
				{
					RestrictInfo *arinfo = (RestrictInfo *) lfirst(j);

					if (matches_any_index(arinfo, rel, outer_relids))
						return true;
				}
			}
			else
			{
				/* Recurse to examine simple clause */
				Assert(IsA(orarg, RestrictInfo));
				Assert(!restriction_is_or_clause((RestrictInfo *) orarg));
				if (matches_any_index((RestrictInfo *) orarg, rel,
									  outer_relids))
					return true;
			}
		}

		return false;
	}

	/* Normal case for a simple restriction clause */
	foreach(l, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(l);
		int			indexcol = 0;
		Oid		   *classes = index->classlist;

		do
		{
			Oid			curClass = classes[0];

			if (match_clause_to_indexcol(index,
										 indexcol,
										 curClass,
										 rinfo,
										 outer_relids))
				return true;

			indexcol++;
			classes++;
		} while (!DoneMatchingIndexKeys(classes));
	}

	return false;
}

/*
 * best_inner_indexscan
 *	  Finds the best available inner indexscan for a nestloop join
 *	  with the given rel on the inside and the given outer_relids outside.
 *	  May return NULL if there are no possible inner indexscans.
 *
 * We ignore ordering considerations (since a nestloop's inner scan's order
 * is uninteresting).  Also, we consider only total cost when deciding which
 * of two possible paths is better --- this assumes that all indexpaths have
 * negligible startup cost.  (True today, but someday we might have to think
 * harder.)  Therefore, there is only one dimension of comparison and so it's
 * sufficient to return a single "best" path.
 */
Path *
best_inner_indexscan(PlannerInfo *root, RelOptInfo *rel,
					 Relids outer_relids, JoinType jointype)
{
	Path	   *cheapest;
	bool		isouterjoin;
	List	   *clause_list;
	List	   *indexpaths;
	List	   *bitindexpaths;
	ListCell   *l;
	InnerIndexscanInfo *info;
	MemoryContext oldcontext;

	/*
	 * Nestloop only supports inner, left, and IN joins.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_IN:
		case JOIN_UNIQUE_OUTER:
			isouterjoin = false;
			break;
		case JOIN_LEFT:
			isouterjoin = true;
			break;
		default:
			return NULL;
	}

	/*
	 * If there are no indexable joinclauses for this rel, exit quickly.
	 */
	if (bms_is_empty(rel->index_outer_relids))
		return NULL;

	/*
	 * Otherwise, we have to do path selection in the memory context of the
	 * given rel, so that any created path can be safely attached to the rel's
	 * cache of best inner paths.  (This is not currently an issue for normal
	 * planning, but it is an issue for GEQO planning.)
	 */
	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

	/*
	 * Intersect the given outer_relids with index_outer_relids to find the
	 * set of outer relids actually relevant for this rel. If there are none,
	 * again we can fail immediately.
	 */
	outer_relids = bms_intersect(rel->index_outer_relids, outer_relids);
	if (bms_is_empty(outer_relids))
	{
		bms_free(outer_relids);
		MemoryContextSwitchTo(oldcontext);
		return NULL;
	}

	/*
	 * Look to see if we already computed the result for this set of relevant
	 * outerrels.  (We include the isouterjoin status in the cache lookup key
	 * for safety.	In practice I suspect this is not necessary because it
	 * should always be the same for a given innerrel.)
	 */
	foreach(l, rel->index_inner_paths)
	{
		info = (InnerIndexscanInfo *) lfirst(l);
		if (bms_equal(info->other_relids, outer_relids) &&
			info->isouterjoin == isouterjoin)
		{
			bms_free(outer_relids);
			MemoryContextSwitchTo(oldcontext);
			return info->best_innerpath;
		}
	}

	/*
	 * Find all the relevant restriction and join clauses.
	 *
	 * Note: because we include restriction clauses, we will find indexscans
	 * that could be plain indexscans, ie, they don't require the join context
	 * at all.  This may seem redundant, but we need to include those scans in
	 * the input given to choose_bitmap_and() to be sure we find optimal AND
	 * combinations of join and non-join scans.  The worst case is that we
	 * might return a "best inner indexscan" that's really just a plain
	 * indexscan, causing some redundant effort in joinpath.c.
	 */
	clause_list = find_clauses_for_join(root, rel, outer_relids, isouterjoin);

	/*
	 * Find all the index paths that are usable for this join, except for
	 * stuff involving OR clauses.
	 */
	indexpaths = find_usable_indexes(root, rel,
									 clause_list, NIL,
									 false, true,
									 outer_relids);

	/*
	 * Generate BitmapOrPaths for any suitable OR-clauses present in the
	 * clause list.
	 */
	bitindexpaths = generate_bitmap_or_paths(root, rel,
											 clause_list, NIL,
											 true,
											 outer_relids);

	/*
	 * Include the regular index paths in bitindexpaths.
	 */
	bitindexpaths = list_concat(bitindexpaths, list_copy(indexpaths));

	/*
	 * If we found anything usable, generate a BitmapHeapPath for the most
	 * promising combination of bitmap index paths.
	 */
	if (bitindexpaths != NIL)
	{
		Path	   *bitmapqual;
		BitmapHeapPath *bpath;

		bitmapqual = choose_bitmap_and(root, rel, bitindexpaths);
		bpath = create_bitmap_heap_path(root, rel, bitmapqual, true);
		indexpaths = lappend(indexpaths, bpath);
	}

	/*
	 * Now choose the cheapest member of indexpaths.
	 */
	cheapest = NULL;
	foreach(l, indexpaths)
	{
		Path	   *path = (Path *) lfirst(l);

		if (cheapest == NULL ||
			compare_path_costs(path, cheapest, TOTAL_COST) < 0)
			cheapest = path;
	}

	/* Cache the result --- whether positive or negative */
	info = makeNode(InnerIndexscanInfo);
	info->other_relids = outer_relids;
	info->isouterjoin = isouterjoin;
	info->best_innerpath = cheapest;
	rel->index_inner_paths = lcons(info, rel->index_inner_paths);

	MemoryContextSwitchTo(oldcontext);

	return cheapest;
}

/*
 * find_clauses_for_join
 *	  Generate a list of clauses that are potentially useful for
 *	  scanning rel as the inner side of a nestloop join.
 *
 * We consider both join and restriction clauses.  Any joinclause that uses
 * only otherrels in the specified outer_relids is fair game.  But there must
 * be at least one such joinclause in the final list, otherwise we return NIL
 * indicating that there isn't any potential win here.
 */
static List *
find_clauses_for_join(PlannerInfo *root, RelOptInfo *rel,
					  Relids outer_relids, bool isouterjoin)
{
	List	   *clause_list = NIL;
	Relids		join_relids;
	ListCell   *l;

	/* Look for joinclauses that are usable with given outer_relids */
	join_relids = bms_union(rel->relids, outer_relids);

	foreach(l, rel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		/* Can't use pushed-down join clauses in outer join */
		if (isouterjoin && rinfo->is_pushed_down)
			continue;
		if (!bms_is_subset(rinfo->required_relids, join_relids))
			continue;

		clause_list = lappend(clause_list, rinfo);
	}

	bms_free(join_relids);

	/* if no join clause was matched then forget it, per comments above */
	if (clause_list == NIL)
		return NIL;

	/*
	 * We can also use any plain restriction clauses for the rel.  We put
	 * these at the front of the clause list for the convenience of
	 * remove_redundant_join_clauses, which can never remove non-join clauses
	 * and hence won't be able to get rid of a non-join clause if it appears
	 * after a join clause it is redundant with.
	 */
	clause_list = list_concat(list_copy(rel->baserestrictinfo), clause_list);

	/*
	 * We may now have clauses that are known redundant.  Get rid of 'em.
	 */
	if (list_length(clause_list) > 1)
	{
		clause_list = remove_redundant_join_clauses(root,
													clause_list,
													isouterjoin);
	}

	return clause_list;
}

/****************************************************************************
 *				----  ROUTINES TO HANDLE PATHKEYS  ----
 ****************************************************************************/

/*
 * match_variant_ordering
 *		Try to match an index's ordering to the query's requested ordering
 *
 * This is used when the index is ordered but a naive comparison fails to
 * match its ordering (pathkeys) to root->query_pathkeys.  It may be that
 * we need to scan the index backwards.  Also, a less naive comparison can
 * help for both forward and backward indexscans.  Columns of the index
 * that have an equality restriction clause can be ignored in the match;
 * that is, an index on (x,y) can be considered to match the ordering of
 *		... WHERE x = 42 ORDER BY y;
 *
 * Note: it would be possible to similarly ignore useless ORDER BY items;
 * that is, an index on just y could be considered to match the ordering of
 *		... WHERE x = 42 ORDER BY x, y;
 * But proving that this is safe would require finding a btree opclass
 * containing both the = operator and the < or > operator in the ORDER BY
 * item.  That's significantly more expensive than what we do here, since
 * we'd have to look at restriction clauses unrelated to the current index
 * and search for opclasses without any hint from the index.  The practical
 * use-cases seem to be mostly covered by ignoring index columns, so that's
 * all we do for now.
 *
 * Inputs:
 * 'index' is the index of interest.
 * 'restrictclauses' is the list of sublists of restriction clauses
 *		matching the columns of the index (NIL if none)
 *
 * If able to match the requested query pathkeys, returns either
 * ForwardScanDirection or BackwardScanDirection to indicate the proper index
 * scan direction.	If no match, returns NoMovementScanDirection.
 */
static ScanDirection
match_variant_ordering(PlannerInfo *root,
					   IndexOptInfo *index,
					   List *restrictclauses)
{
	List	   *ignorables;

	/*
	 * Forget the whole thing if not a btree index; our check for ignorable
	 * columns assumes we are dealing with btree opclasses.  (It'd be possible
	 * to factor out just the try for backwards indexscan, but considering
	 * that we presently have no orderable indexes except btrees anyway, it's
	 * hardly worth contorting this code for that case.)
	 *
	 * Note: if you remove this, you probably need to put in a check on
	 * amoptionalkey to prevent possible clauseless scan on an index that
	 * won't cope.
	 */
	if (index->relam != BTREE_AM_OID)
		return NoMovementScanDirection;

	/*
	 * Figure out which index columns can be optionally ignored because they
	 * have an equality constraint.  This is the same set for either forward
	 * or backward scan, so we do it just once.
	 */
	ignorables = identify_ignorable_ordering_cols(root, index,
												  restrictclauses);

	/*
	 * Try to match to forward scan, then backward scan.  However, we can skip
	 * the forward-scan case if there are no ignorable columns, because
	 * find_usable_indexes() would have found the match already.
	 */
	if (ignorables &&
		match_index_to_query_keys(root, index, ForwardScanDirection,
								  ignorables))
		return ForwardScanDirection;

	if (match_index_to_query_keys(root, index, BackwardScanDirection,
								  ignorables))
		return BackwardScanDirection;

	return NoMovementScanDirection;
}

/*
 * identify_ignorable_ordering_cols
 *		Determine which index columns can be ignored for ordering purposes
 *
 * Returns an integer List of column numbers (1-based) of ignorable
 * columns.  The ignorable columns are those that have equality constraints
 * against pseudoconstants.
 */
static List *
identify_ignorable_ordering_cols(PlannerInfo *root,
								 IndexOptInfo *index,
								 List *restrictclauses)
{
	List	   *result = NIL;
	int			indexcol = 0;	/* note this is 0-based */
	ListCell   *l;

	/* restrictclauses is either NIL or has a sublist per column */
	foreach(l, restrictclauses)
	{
		List	   *sublist = (List *) lfirst(l);
		Oid			opclass = index->classlist[indexcol];
		ListCell   *l2;

		foreach(l2, sublist)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l2);
			OpExpr	   *clause = (OpExpr *) rinfo->clause;
			Oid			clause_op;
			int			op_strategy;
			bool		varonleft;
			bool		ispc;

			/* We know this clause passed match_clause_to_indexcol */

			/* First check for boolean-index cases. */
			if (IsBooleanOpclass(opclass))
			{
				if (match_boolean_index_clause((Node *) clause, indexcol,
											   index))
				{
					/*
					 * The clause means either col = TRUE or col = FALSE; we
					 * do not care which, it's an equality constraint either
					 * way.
					 */
					result = lappend_int(result, indexcol + 1);
					break;
				}
			}

			/* Else clause must be a binary opclause. */
			Assert(IsA(clause, OpExpr));

			/* Determine left/right sides and check the operator */
			clause_op = clause->opno;
			if (match_index_to_operand(linitial(clause->args), indexcol,
									   index))
			{
				/* clause_op is correct */
				varonleft = true;
			}
			else
			{
				Assert(match_index_to_operand(lsecond(clause->args), indexcol,
											  index));
				/* Must flip operator to get the opclass member */
				clause_op = get_commutator(clause_op);
				varonleft = false;
			}
			if (!OidIsValid(clause_op))
				continue;		/* ignore non match, per next comment */
			op_strategy = get_op_opclass_strategy(clause_op, opclass);

			/*
			 * You might expect to see Assert(op_strategy != 0) here, but you
			 * won't: the clause might contain a special indexable operator
			 * rather than an ordinary opclass member.	Currently none of the
			 * special operators are very likely to expand to an equality
			 * operator; we do not bother to check, but just assume no match.
			 */
			if (op_strategy != BTEqualStrategyNumber)
				continue;

			/* Now check that other side is pseudoconstant */
			if (varonleft)
				ispc = is_pseudo_constant_clause_relids(lsecond(clause->args),
														rinfo->right_relids);
			else
				ispc = is_pseudo_constant_clause_relids(linitial(clause->args),
														rinfo->left_relids);
			if (ispc)
			{
				result = lappend_int(result, indexcol + 1);
				break;
			}
		}
		indexcol++;
	}
	return result;
}

/*
 * match_index_to_query_keys
 *		Check a single scan direction for "intelligent" match to query keys
 *
 * 'index' is the index of interest.
 * 'indexscandir' is the scan direction to consider
 * 'ignorables' is an integer list of indexes of ignorable index columns
 *
 * Returns TRUE on successful match (ie, the query_pathkeys can be considered
 * to match this index).
 */
static bool
match_index_to_query_keys(PlannerInfo *root,
						  IndexOptInfo *index,
						  ScanDirection indexscandir,
						  List *ignorables)
{
	List	   *index_pathkeys;
	ListCell   *index_cell;
	int			index_col;
	ListCell   *r;

	/* Get the pathkeys that exactly describe the index */
	index_pathkeys = build_index_pathkeys(root, index, indexscandir, false);

	/*
	 * Can we match to the query's requested pathkeys?  The inner loop skips
	 * over ignorable index columns while trying to match.
	 */
	index_cell = list_head(index_pathkeys);
	index_col = 0;

	foreach(r, root->query_pathkeys)
	{
		List	   *rsubkey = (List *) lfirst(r);

		for (;;)
		{
			List	   *isubkey;

			if (index_cell == NULL)
				return false;
			isubkey = (List *) lfirst(index_cell);
			index_cell = lnext(index_cell);
			index_col++;		/* index_col is now 1-based */

			/*
			 * Since we are dealing with canonicalized pathkeys, pointer
			 * comparison is sufficient to determine a match.
			 */
			if (rsubkey == isubkey)
				break;			/* matched current query pathkey */

			if (!list_member_int(ignorables, index_col))
				return false;	/* definite failure to match */
			/* otherwise loop around and try to match to next index col */
		}
	}

	return true;
}

/****************************************************************************
 *				----  PATH CREATION UTILITIES  ----
 ****************************************************************************/

/*
 * flatten_clausegroups_list
 *	  Given a list of lists of RestrictInfos, flatten it to a list
 *	  of RestrictInfos.
 *
 * This is used to flatten out the result of group_clauses_by_indexkey()
 * to produce an indexclauses list.  The original list structure mustn't
 * be altered, but it's OK to share copies of the underlying RestrictInfos.
 */
List *
flatten_clausegroups_list(List *clausegroups)
{
	List	   *allclauses = NIL;
	ListCell   *l;

	foreach(l, clausegroups)
		allclauses = list_concat(allclauses, list_copy((List *) lfirst(l)));
	return allclauses;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK OPERANDS  ----
 ****************************************************************************/

/*
 * match_index_to_operand()
 *	  Generalized test for a match between an index's key
 *	  and the operand on one side of a restriction or join clause.
 *
 * operand: the nodetree to be compared to the index
 * indexcol: the column number of the index (counting from 0)
 * index: the index of interest
 */
bool
match_index_to_operand(Node *operand,
					   int indexcol,
					   IndexOptInfo *index)
{
	int			indkey;

	/*
	 * Ignore any RelabelType node above the operand.	This is needed to be
	 * able to apply indexscanning in binary-compatible-operator cases. Note:
	 * we can assume there is at most one RelabelType node;
	 * eval_const_expressions() will have simplified if more than one.
	 */
	if (operand && IsA(operand, RelabelType))
		operand = (Node *) ((RelabelType *) operand)->arg;

	indkey = index->indexkeys[indexcol];
	if (indkey != 0)
	{
		/*
		 * Simple index column; operand must be a matching Var.
		 */
		if (operand && IsA(operand, Var) &&
			index->rel->relid == ((Var *) operand)->varno &&
			indkey == ((Var *) operand)->varattno)
			return true;
	}
	else
	{
		/*
		 * Index expression; find the correct expression.  (This search could
		 * be avoided, at the cost of complicating all the callers of this
		 * routine; doesn't seem worth it.)
		 */
		ListCell   *indexpr_item;
		int			i;
		Node	   *indexkey;

		indexpr_item = list_head(index->indexprs);
		for (i = 0; i < indexcol; i++)
		{
			if (index->indexkeys[i] == 0)
			{
				if (indexpr_item == NULL)
					elog(ERROR, "wrong number of index expressions");
				indexpr_item = lnext(indexpr_item);
			}
		}
		if (indexpr_item == NULL)
			elog(ERROR, "wrong number of index expressions");
		indexkey = (Node *) lfirst(indexpr_item);

		/*
		 * Does it match the operand?  Again, strip any relabeling.
		 */
		if (indexkey && IsA(indexkey, RelabelType))
			indexkey = (Node *) ((RelabelType *) indexkey)->arg;

		if (equal(indexkey, operand))
			return true;
	}

	return false;
}

/****************************************************************************
 *			----  ROUTINES FOR "SPECIAL" INDEXABLE OPERATORS  ----
 ****************************************************************************/

/*----------
 * These routines handle special optimization of operators that can be
 * used with index scans even though they are not known to the executor's
 * indexscan machinery.  The key idea is that these operators allow us
 * to derive approximate indexscan qual clauses, such that any tuples
 * that pass the operator clause itself must also satisfy the simpler
 * indexscan condition(s).	Then we can use the indexscan machinery
 * to avoid scanning as much of the table as we'd otherwise have to,
 * while applying the original operator as a qpqual condition to ensure
 * we deliver only the tuples we want.	(In essence, we're using a regular
 * index as if it were a lossy index.)
 *
 * An example of what we're doing is
 *			textfield LIKE 'abc%'
 * from which we can generate the indexscanable conditions
 *			textfield >= 'abc' AND textfield < 'abd'
 * which allow efficient scanning of an index on textfield.
 * (In reality, character set and collation issues make the transformation
 * from LIKE to indexscan limits rather harder than one might think ...
 * but that's the basic idea.)
 *
 * Another thing that we do with this machinery is to provide special
 * smarts for "boolean" indexes (that is, indexes on boolean columns
 * that support boolean equality).	We can transform a plain reference
 * to the indexkey into "indexkey = true", or "NOT indexkey" into
 * "indexkey = false", so as to make the expression indexable using the
 * regular index operators.  (As of Postgres 8.1, we must do this here
 * because constant simplification does the reverse transformation;
 * without this code there'd be no way to use such an index at all.)
 *
 * Three routines are provided here:
 *
 * match_special_index_operator() is just an auxiliary function for
 * match_clause_to_indexcol(); after the latter fails to recognize a
 * restriction opclause's operator as a member of an index's opclass,
 * it asks match_special_index_operator() whether the clause should be
 * considered an indexqual anyway.
 *
 * match_boolean_index_clause() similarly detects clauses that can be
 * converted into boolean equality operators.
 *
 * expand_indexqual_conditions() converts a list of lists of RestrictInfo
 * nodes (with implicit AND semantics across list elements) into
 * a list of clauses that the executor can actually handle.  For operators
 * that are members of the index's opclass this transformation is a no-op,
 * but clauses recognized by match_special_index_operator() or
 * match_boolean_index_clause() must be converted into one or more "regular"
 * indexqual conditions.
 *----------
 */

/*
 * match_boolean_index_clause
 *	  Recognize restriction clauses that can be matched to a boolean index.
 *
 * This should be called only when IsBooleanOpclass() recognizes the
 * index's operator class.  We check to see if the clause matches the
 * index's key.
 */
static bool
match_boolean_index_clause(Node *clause,
						   int indexcol,
						   IndexOptInfo *index)
{
	/* Direct match? */
	if (match_index_to_operand(clause, indexcol, index))
		return true;
	/* NOT clause? */
	if (not_clause(clause))
	{
		if (match_index_to_operand((Node *) get_notclausearg((Expr *) clause),
								   indexcol, index))
			return true;
	}

	/*
	 * Since we only consider clauses at top level of WHERE, we can convert
	 * indexkey IS TRUE and indexkey IS FALSE to index searches as well. The
	 * different meaning for NULL isn't important.
	 */
	else if (clause && IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;

		if (btest->booltesttype == IS_TRUE ||
			btest->booltesttype == IS_FALSE)
			if (match_index_to_operand((Node *) btest->arg,
									   indexcol, index))
				return true;
	}
	return false;
}

/*
 * match_special_index_operator
 *	  Recognize restriction clauses that can be used to generate
 *	  additional indexscanable qualifications.
 *
 * The given clause is already known to be a binary opclause having
 * the form (indexkey OP pseudoconst) or (pseudoconst OP indexkey),
 * but the OP proved not to be one of the index's opclass operators.
 * Return 'true' if we can do something with it anyway.
 */
static bool
match_special_index_operator(Expr *clause, Oid opclass,
							 bool indexkey_on_left)
{
	bool		isIndexable = false;
	Node	   *rightop;
	Oid			expr_op;
	Const	   *patt;
	Const	   *prefix = NULL;
	Const	   *rest = NULL;

	/*
	 * Currently, all known special operators require the indexkey on the
	 * left, but this test could be pushed into the switch statement if some
	 * are added that do not...
	 */
	if (!indexkey_on_left)
		return false;

	/* we know these will succeed */
	rightop = get_rightop(clause);
	expr_op = ((OpExpr *) clause)->opno;

	/* again, required for all current special ops: */
	if (!IsA(rightop, Const) ||
		((Const *) rightop)->constisnull)
		return false;
	patt = (Const *) rightop;

	switch (expr_op)
	{
		case OID_TEXT_LIKE_OP:
		case OID_BPCHAR_LIKE_OP:
		case OID_NAME_LIKE_OP:
			/* the right-hand const is type text for all of these */
			isIndexable = pattern_fixed_prefix(patt, Pattern_Type_Like,
									  &prefix, &rest) != Pattern_Prefix_None;
			break;

		case OID_BYTEA_LIKE_OP:
			isIndexable = pattern_fixed_prefix(patt, Pattern_Type_Like,
									  &prefix, &rest) != Pattern_Prefix_None;
			break;

		case OID_TEXT_ICLIKE_OP:
		case OID_BPCHAR_ICLIKE_OP:
		case OID_NAME_ICLIKE_OP:
			/* the right-hand const is type text for all of these */
			isIndexable = pattern_fixed_prefix(patt, Pattern_Type_Like_IC,
									  &prefix, &rest) != Pattern_Prefix_None;
			break;

		case OID_TEXT_REGEXEQ_OP:
		case OID_BPCHAR_REGEXEQ_OP:
		case OID_NAME_REGEXEQ_OP:
			/* the right-hand const is type text for all of these */
			isIndexable = pattern_fixed_prefix(patt, Pattern_Type_Regex,
									  &prefix, &rest) != Pattern_Prefix_None;
			break;

		case OID_TEXT_ICREGEXEQ_OP:
		case OID_BPCHAR_ICREGEXEQ_OP:
		case OID_NAME_ICREGEXEQ_OP:
			/* the right-hand const is type text for all of these */
			isIndexable = pattern_fixed_prefix(patt, Pattern_Type_Regex_IC,
									  &prefix, &rest) != Pattern_Prefix_None;
			break;

		case OID_INET_SUB_OP:
		case OID_INET_SUBEQ_OP:
		case OID_CIDR_SUB_OP:
		case OID_CIDR_SUBEQ_OP:
			isIndexable = true;
			break;
	}

	if (prefix)
	{
		pfree(DatumGetPointer(prefix->constvalue));
		pfree(prefix);
	}

	/* done if the expression doesn't look indexable */
	if (!isIndexable)
		return false;

	/*
	 * Must also check that index's opclass supports the operators we will
	 * want to apply.  (A hash index, for example, will not support ">=".)
	 * Currently, only btree supports the operators we need.
	 *
	 * We insist on the opclass being the specific one we expect, else we'd do
	 * the wrong thing if someone were to make a reverse-sort opclass with the
	 * same operators.
	 */
	switch (expr_op)
	{
		case OID_TEXT_LIKE_OP:
		case OID_TEXT_ICLIKE_OP:
		case OID_TEXT_REGEXEQ_OP:
		case OID_TEXT_ICREGEXEQ_OP:
			/* text operators will be used for varchar inputs, too */
			isIndexable =
				(opclass == TEXT_PATTERN_BTREE_OPS_OID) ||
				(opclass == TEXT_BTREE_OPS_OID && lc_collate_is_c()) ||
				(opclass == VARCHAR_PATTERN_BTREE_OPS_OID) ||
				(opclass == VARCHAR_BTREE_OPS_OID && lc_collate_is_c());
			break;

		case OID_BPCHAR_LIKE_OP:
		case OID_BPCHAR_ICLIKE_OP:
		case OID_BPCHAR_REGEXEQ_OP:
		case OID_BPCHAR_ICREGEXEQ_OP:
			isIndexable =
				(opclass == BPCHAR_PATTERN_BTREE_OPS_OID) ||
				(opclass == BPCHAR_BTREE_OPS_OID && lc_collate_is_c());
			break;

		case OID_NAME_LIKE_OP:
		case OID_NAME_ICLIKE_OP:
		case OID_NAME_REGEXEQ_OP:
		case OID_NAME_ICREGEXEQ_OP:
			isIndexable =
				(opclass == NAME_PATTERN_BTREE_OPS_OID) ||
				(opclass == NAME_BTREE_OPS_OID && lc_collate_is_c());
			break;

		case OID_BYTEA_LIKE_OP:
			isIndexable = (opclass == BYTEA_BTREE_OPS_OID);
			break;

		case OID_INET_SUB_OP:
		case OID_INET_SUBEQ_OP:
			isIndexable = (opclass == INET_BTREE_OPS_OID);
			break;

		case OID_CIDR_SUB_OP:
		case OID_CIDR_SUBEQ_OP:
			isIndexable = (opclass == CIDR_BTREE_OPS_OID);
			break;
	}

	return isIndexable;
}

/*
 * expand_indexqual_conditions
 *	  Given a list of sublists of RestrictInfo nodes, produce a flat list
 *	  of index qual clauses.  Standard qual clauses (those in the index's
 *	  opclass) are passed through unchanged.  Boolean clauses and "special"
 *	  index operators are expanded into clauses that the indexscan machinery
 *	  will know what to do with.
 *
 * The input list is ordered by index key, and so the output list is too.
 * (The latter is not depended on by any part of the core planner, I believe,
 * but parts of the executor require it, and so do the amcostestimate
 * functions.)
 */
List *
expand_indexqual_conditions(IndexOptInfo *index, List *clausegroups)
{
	List	   *resultquals = NIL;
	ListCell   *clausegroup_item;
	int			indexcol = 0;
	Oid		   *classes = index->classlist;

	if (clausegroups == NIL)
		return NIL;

	clausegroup_item = list_head(clausegroups);
	do
	{
		Oid			curClass = classes[0];
		ListCell   *l;

		foreach(l, (List *) lfirst(clausegroup_item))
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			/* First check for boolean cases */
			if (IsBooleanOpclass(curClass))
			{
				Expr	   *boolqual;

				boolqual = expand_boolean_index_clause((Node *) rinfo->clause,
													   indexcol,
													   index);
				if (boolqual)
				{
					resultquals = lappend(resultquals,
										  make_restrictinfo(boolqual,
															true,
															false,
															NULL));
					continue;
				}
			}

			resultquals = list_concat(resultquals,
									  expand_indexqual_condition(rinfo,
																 curClass));
		}

		clausegroup_item = lnext(clausegroup_item);

		indexcol++;
		classes++;
	} while (clausegroup_item != NULL && !DoneMatchingIndexKeys(classes));

	Assert(clausegroup_item == NULL);	/* else more groups than indexkeys */

	return resultquals;
}

/*
 * expand_boolean_index_clause
 *	  Convert a clause recognized by match_boolean_index_clause into
 *	  a boolean equality operator clause.
 *
 * Returns NULL if the clause isn't a boolean index qual.
 */
static Expr *
expand_boolean_index_clause(Node *clause,
							int indexcol,
							IndexOptInfo *index)
{
	/* Direct match? */
	if (match_index_to_operand(clause, indexcol, index))
	{
		/* convert to indexkey = TRUE */
		return make_opclause(BooleanEqualOperator, BOOLOID, false,
							 (Expr *) clause,
							 (Expr *) makeBoolConst(true, false));
	}
	/* NOT clause? */
	if (not_clause(clause))
	{
		Node	   *arg = (Node *) get_notclausearg((Expr *) clause);

		/* It must have matched the indexkey */
		Assert(match_index_to_operand(arg, indexcol, index));
		/* convert to indexkey = FALSE */
		return make_opclause(BooleanEqualOperator, BOOLOID, false,
							 (Expr *) arg,
							 (Expr *) makeBoolConst(false, false));
	}
	if (clause && IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;
		Node	   *arg = (Node *) btest->arg;

		/* It must have matched the indexkey */
		Assert(match_index_to_operand(arg, indexcol, index));
		if (btest->booltesttype == IS_TRUE)
		{
			/* convert to indexkey = TRUE */
			return make_opclause(BooleanEqualOperator, BOOLOID, false,
								 (Expr *) arg,
								 (Expr *) makeBoolConst(true, false));
		}
		if (btest->booltesttype == IS_FALSE)
		{
			/* convert to indexkey = FALSE */
			return make_opclause(BooleanEqualOperator, BOOLOID, false,
								 (Expr *) arg,
								 (Expr *) makeBoolConst(false, false));
		}
		/* Oops */
		Assert(false);
	}

	return NULL;
}

/*
 * expand_indexqual_condition --- expand a single indexqual condition
 *		(other than a boolean-qual case)
 *
 * The input is a single RestrictInfo, the output a list of RestrictInfos
 */
static List *
expand_indexqual_condition(RestrictInfo *rinfo, Oid opclass)
{
	Expr	   *clause = rinfo->clause;

	/* we know these will succeed */
	Node	   *leftop = get_leftop(clause);
	Node	   *rightop = get_rightop(clause);
	Oid			expr_op = ((OpExpr *) clause)->opno;
	Const	   *patt = (Const *) rightop;
	Const	   *prefix = NULL;
	Const	   *rest = NULL;
	Pattern_Prefix_Status pstatus;
	List	   *result;

	switch (expr_op)
	{
			/*
			 * LIKE and regex operators are not members of any index opclass,
			 * so if we find one in an indexqual list we can assume that it
			 * was accepted by match_special_index_operator().
			 */
		case OID_TEXT_LIKE_OP:
		case OID_BPCHAR_LIKE_OP:
		case OID_NAME_LIKE_OP:
		case OID_BYTEA_LIKE_OP:
			pstatus = pattern_fixed_prefix(patt, Pattern_Type_Like,
										   &prefix, &rest);
			result = prefix_quals(leftop, opclass, prefix, pstatus);
			break;

		case OID_TEXT_ICLIKE_OP:
		case OID_BPCHAR_ICLIKE_OP:
		case OID_NAME_ICLIKE_OP:
			/* the right-hand const is type text for all of these */
			pstatus = pattern_fixed_prefix(patt, Pattern_Type_Like_IC,
										   &prefix, &rest);
			result = prefix_quals(leftop, opclass, prefix, pstatus);
			break;

		case OID_TEXT_REGEXEQ_OP:
		case OID_BPCHAR_REGEXEQ_OP:
		case OID_NAME_REGEXEQ_OP:
			/* the right-hand const is type text for all of these */
			pstatus = pattern_fixed_prefix(patt, Pattern_Type_Regex,
										   &prefix, &rest);
			result = prefix_quals(leftop, opclass, prefix, pstatus);
			break;

		case OID_TEXT_ICREGEXEQ_OP:
		case OID_BPCHAR_ICREGEXEQ_OP:
		case OID_NAME_ICREGEXEQ_OP:
			/* the right-hand const is type text for all of these */
			pstatus = pattern_fixed_prefix(patt, Pattern_Type_Regex_IC,
										   &prefix, &rest);
			result = prefix_quals(leftop, opclass, prefix, pstatus);
			break;

		case OID_INET_SUB_OP:
		case OID_INET_SUBEQ_OP:
		case OID_CIDR_SUB_OP:
		case OID_CIDR_SUBEQ_OP:
			result = network_prefix_quals(leftop, expr_op, opclass,
										  patt->constvalue);
			break;

		default:
			result = list_make1(rinfo);
			break;
	}

	return result;
}

/*
 * Given a fixed prefix that all the "leftop" values must have,
 * generate suitable indexqual condition(s).  opclass is the index
 * operator class; we use it to deduce the appropriate comparison
 * operators and operand datatypes.
 */
static List *
prefix_quals(Node *leftop, Oid opclass,
			 Const *prefix_const, Pattern_Prefix_Status pstatus)
{
	List	   *result;
	Oid			datatype;
	Oid			oproid;
	Expr	   *expr;
	Const	   *greaterstr;

	Assert(pstatus != Pattern_Prefix_None);

	switch (opclass)
	{
		case TEXT_BTREE_OPS_OID:
		case TEXT_PATTERN_BTREE_OPS_OID:
			datatype = TEXTOID;
			break;

		case VARCHAR_BTREE_OPS_OID:
		case VARCHAR_PATTERN_BTREE_OPS_OID:
			datatype = VARCHAROID;
			break;

		case BPCHAR_BTREE_OPS_OID:
		case BPCHAR_PATTERN_BTREE_OPS_OID:
			datatype = BPCHAROID;
			break;

		case NAME_BTREE_OPS_OID:
		case NAME_PATTERN_BTREE_OPS_OID:
			datatype = NAMEOID;
			break;

		case BYTEA_BTREE_OPS_OID:
			datatype = BYTEAOID;
			break;

		default:
			/* shouldn't get here */
			elog(ERROR, "unexpected opclass: %u", opclass);
			return NIL;
	}

	/*
	 * If necessary, coerce the prefix constant to the right type. The given
	 * prefix constant is either text or bytea type.
	 */
	if (prefix_const->consttype != datatype)
	{
		char	   *prefix;

		switch (prefix_const->consttype)
		{
			case TEXTOID:
				prefix = DatumGetCString(DirectFunctionCall1(textout,
												  prefix_const->constvalue));
				break;
			case BYTEAOID:
				prefix = DatumGetCString(DirectFunctionCall1(byteaout,
												  prefix_const->constvalue));
				break;
			default:
				elog(ERROR, "unexpected const type: %u",
					 prefix_const->consttype);
				return NIL;
		}
		prefix_const = string_to_const(prefix, datatype);
		pfree(prefix);
	}

	/*
	 * If we found an exact-match pattern, generate an "=" indexqual.
	 */
	if (pstatus == Pattern_Prefix_Exact)
	{
		oproid = get_opclass_member(opclass, InvalidOid,
									BTEqualStrategyNumber);
		if (oproid == InvalidOid)
			elog(ERROR, "no = operator for opclass %u", opclass);
		expr = make_opclause(oproid, BOOLOID, false,
							 (Expr *) leftop, (Expr *) prefix_const);
		result = list_make1(make_restrictinfo(expr, true, false, NULL));
		return result;
	}

	/*
	 * Otherwise, we have a nonempty required prefix of the values.
	 *
	 * We can always say "x >= prefix".
	 */
	oproid = get_opclass_member(opclass, InvalidOid,
								BTGreaterEqualStrategyNumber);
	if (oproid == InvalidOid)
		elog(ERROR, "no >= operator for opclass %u", opclass);
	expr = make_opclause(oproid, BOOLOID, false,
						 (Expr *) leftop, (Expr *) prefix_const);
	result = list_make1(make_restrictinfo(expr, true, false, NULL));

	/*-------
	 * If we can create a string larger than the prefix, we can say
	 * "x < greaterstr".
	 *-------
	 */
	greaterstr = make_greater_string(prefix_const);
	if (greaterstr)
	{
		oproid = get_opclass_member(opclass, InvalidOid,
									BTLessStrategyNumber);
		if (oproid == InvalidOid)
			elog(ERROR, "no < operator for opclass %u", opclass);
		expr = make_opclause(oproid, BOOLOID, false,
							 (Expr *) leftop, (Expr *) greaterstr);
		result = lappend(result, make_restrictinfo(expr, true, false, NULL));
	}

	return result;
}

/*
 * Given a leftop and a rightop, and a inet-class sup/sub operator,
 * generate suitable indexqual condition(s).  expr_op is the original
 * operator, and opclass is the index opclass.
 */
static List *
network_prefix_quals(Node *leftop, Oid expr_op, Oid opclass, Datum rightop)
{
	bool		is_eq;
	Oid			datatype;
	Oid			opr1oid;
	Oid			opr2oid;
	Datum		opr1right;
	Datum		opr2right;
	List	   *result;
	Expr	   *expr;

	switch (expr_op)
	{
		case OID_INET_SUB_OP:
			datatype = INETOID;
			is_eq = false;
			break;
		case OID_INET_SUBEQ_OP:
			datatype = INETOID;
			is_eq = true;
			break;
		case OID_CIDR_SUB_OP:
			datatype = CIDROID;
			is_eq = false;
			break;
		case OID_CIDR_SUBEQ_OP:
			datatype = CIDROID;
			is_eq = true;
			break;
		default:
			elog(ERROR, "unexpected operator: %u", expr_op);
			return NIL;
	}

	/*
	 * create clause "key >= network_scan_first( rightop )", or ">" if the
	 * operator disallows equality.
	 */
	if (is_eq)
	{
		opr1oid = get_opclass_member(opclass, InvalidOid,
									 BTGreaterEqualStrategyNumber);
		if (opr1oid == InvalidOid)
			elog(ERROR, "no >= operator for opclass %u", opclass);
	}
	else
	{
		opr1oid = get_opclass_member(opclass, InvalidOid,
									 BTGreaterStrategyNumber);
		if (opr1oid == InvalidOid)
			elog(ERROR, "no > operator for opclass %u", opclass);
	}

	opr1right = network_scan_first(rightop);

	expr = make_opclause(opr1oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1, opr1right,
											false, false));
	result = list_make1(make_restrictinfo(expr, true, false, NULL));

	/* create clause "key <= network_scan_last( rightop )" */

	opr2oid = get_opclass_member(opclass, InvalidOid,
								 BTLessEqualStrategyNumber);
	if (opr2oid == InvalidOid)
		elog(ERROR, "no <= operator for opclass %u", opclass);

	opr2right = network_scan_last(rightop);

	expr = make_opclause(opr2oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1, opr2right,
											false, false));
	result = lappend(result, make_restrictinfo(expr, true, false, NULL));

	return result;
}

/*
 * Handy subroutines for match_special_index_operator() and friends.
 */

/*
 * Generate a Datum of the appropriate type from a C string.
 * Note that all of the supported types are pass-by-ref, so the
 * returned value should be pfree'd if no longer needed.
 */
static Datum
string_to_datum(const char *str, Oid datatype)
{
	/*
	 * We cheat a little by assuming that textin() will do for bpchar and
	 * varchar constants too...
	 */
	if (datatype == NAMEOID)
		return DirectFunctionCall1(namein, CStringGetDatum(str));
	else if (datatype == BYTEAOID)
		return DirectFunctionCall1(byteain, CStringGetDatum(str));
	else
		return DirectFunctionCall1(textin, CStringGetDatum(str));
}

/*
 * Generate a Const node of the appropriate type from a C string.
 */
static Const *
string_to_const(const char *str, Oid datatype)
{
	Datum		conval = string_to_datum(str, datatype);

	return makeConst(datatype, ((datatype == NAMEOID) ? NAMEDATALEN : -1),
					 conval, false, false);
}
