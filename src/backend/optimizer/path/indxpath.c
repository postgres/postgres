/*-------------------------------------------------------------------------
 *
 * indxpath.c
 *	  Routines to determine which indexes are usable for scanning a
 *	  given relation, and create Paths accordingly.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/indxpath.c,v 1.212.2.4 2007/11/07 22:37:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/skey.h"
#include "catalog/pg_am.h"
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
#include "optimizer/var.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/selfuncs.h"


/*
 * DoneMatchingIndexKeys() - MACRO
 */
#define DoneMatchingIndexKeys(classes)	(classes[0] == InvalidOid)

#define IsBooleanOpclass(opclass) \
	((opclass) == BOOL_BTREE_OPS_OID || (opclass) == BOOL_HASH_OPS_OID)


/* Per-path data used within choose_bitmap_and() */
typedef struct
{
	Path	   *path;			/* IndexPath, BitmapAndPath, or BitmapOrPath */
	List	   *quals;			/* the WHERE clauses it uses */
	List	   *preds;			/* predicates of its partial index(es) */
	Bitmapset  *clauseids;		/* quals+preds represented as a bitmapset */
} PathClauseUsage;


static List *find_usable_indexes(PlannerInfo *root, RelOptInfo *rel,
					List *clauses, List *outer_clauses,
					bool istoplevel, RelOptInfo *outer_rel,
					SaOpControl saop_control);
static List *find_saop_paths(PlannerInfo *root, RelOptInfo *rel,
				List *clauses, List *outer_clauses,
				bool istoplevel, RelOptInfo *outer_rel);
static Path *choose_bitmap_and(PlannerInfo *root, RelOptInfo *rel,
				  List *paths, RelOptInfo *outer_rel);
static int	path_usage_comparator(const void *a, const void *b);
static Cost bitmap_scan_cost_est(PlannerInfo *root, RelOptInfo *rel,
					 Path *ipath, RelOptInfo *outer_rel);
static Cost bitmap_and_cost_est(PlannerInfo *root, RelOptInfo *rel,
					List *paths, RelOptInfo *outer_rel);
static PathClauseUsage *classify_index_clause_usage(Path *path,
													List **clauselist);
static void find_indexpath_quals(Path *bitmapqual, List **quals, List **preds);
static int	find_list_position(Node *node, List **nodelist);
static bool match_clause_to_indexcol(IndexOptInfo *index,
						 int indexcol, Oid opclass,
						 RestrictInfo *rinfo,
						 Relids outer_relids,
						 SaOpControl saop_control);
static bool is_indexable_operator(Oid expr_op, Oid opclass,
					  bool indexkey_on_left);
static bool match_rowcompare_to_indexcol(IndexOptInfo *index,
							 int indexcol,
							 Oid opclass,
							 RowCompareExpr *clause,
							 Relids outer_relids);
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
static List *expand_indexqual_opclause(RestrictInfo *rinfo, Oid opclass);
static RestrictInfo *expand_indexqual_rowcompare(RestrictInfo *rinfo,
							IndexOptInfo *index,
							int indexcol);
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
 * Note: check_partial_indexes() must have been run previously for this rel.
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
									 true, NULL, SAOP_FORBID);

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
										  NULL);
	bitindexpaths = list_concat(bitindexpaths, indexpaths);

	/*
	 * Likewise, generate paths using ScalarArrayOpExpr clauses; these can't
	 * be simple indexscans but they can be used in bitmap scans.
	 */
	indexpaths = find_saop_paths(root, rel,
								 rel->baserestrictinfo, NIL,
								 true, NULL);
	bitindexpaths = list_concat(bitindexpaths, indexpaths);

	/*
	 * If we found anything usable, generate a BitmapHeapPath for the most
	 * promising combination of bitmap index paths.
	 */
	if (bitindexpaths != NIL)
	{
		Path	   *bitmapqual;
		BitmapHeapPath *bpath;

		bitmapqual = choose_bitmap_and(root, rel, bitindexpaths, NULL);
		bpath = create_bitmap_heap_path(root, rel, bitmapqual, NULL);
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
 * 'outer_rel' is the outer side of the join if forming an inner indexscan
 *		(so some of the given clauses are join clauses); NULL if not
 * 'saop_control' indicates whether ScalarArrayOpExpr clauses can be used
 *
 * Note: check_partial_indexes() must have been run previously.
 *----------
 */
static List *
find_usable_indexes(PlannerInfo *root, RelOptInfo *rel,
					List *clauses, List *outer_clauses,
					bool istoplevel, RelOptInfo *outer_rel,
					SaOpControl saop_control)
{
	Relids		outer_relids = outer_rel ? outer_rel->relids : NULL;
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
		 * clauses was used (and, if saop_control is SAOP_REQUIRE, it has to
		 * have been a ScalarArrayOpExpr clause).
		 */
		restrictclauses = group_clauses_by_indexkey(index,
													clauses,
													outer_clauses,
													outer_relids,
													saop_control,
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
		if (index_is_ordered && istoplevel && outer_rel == NULL)
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
									  outer_rel);
			result = lappend(result, ipath);
		}

		/*
		 * 4. If the index is ordered, and there is a requested query ordering
		 * that we failed to match, consider variant ways of achieving the
		 * ordering.  Again, this is only interesting at top level.
		 */
		if (index_is_ordered && istoplevel && outer_rel == NULL &&
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
										  outer_rel);
				result = lappend(result, ipath);
			}
		}
	}

	return result;
}


/*
 * find_saop_paths
 *		Find all the potential indexpaths that make use of ScalarArrayOpExpr
 *		clauses.  The executor only supports these in bitmap scans, not
 *		plain indexscans, so we need to segregate them from the normal case.
 *		Otherwise, same API as find_usable_indexes().
 *		Returns a list of IndexPaths.
 */
static List *
find_saop_paths(PlannerInfo *root, RelOptInfo *rel,
				List *clauses, List *outer_clauses,
				bool istoplevel, RelOptInfo *outer_rel)
{
	bool		have_saop = false;
	ListCell   *l;

	/*
	 * Since find_usable_indexes is relatively expensive, don't bother to run
	 * it unless there are some top-level ScalarArrayOpExpr clauses.
	 */
	foreach(l, clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		Assert(IsA(rinfo, RestrictInfo));
		if (IsA(rinfo->clause, ScalarArrayOpExpr))
		{
			have_saop = true;
			break;
		}
	}
	if (!have_saop)
		return NIL;

	return find_usable_indexes(root, rel,
							   clauses, outer_clauses,
							   istoplevel, outer_rel,
							   SAOP_REQUIRE);
}


/*
 * generate_bitmap_or_paths
 *		Look through the list of clauses to find OR clauses, and generate
 *		a BitmapOrPath for each one we can handle that way.  Return a list
 *		of the generated BitmapOrPaths.
 *
 * outer_clauses is a list of additional clauses that can be assumed true
 * for the purpose of generating indexquals, but are not to be searched for
 * ORs.  (See find_usable_indexes() for motivation.)  outer_rel is the outer
 * side when we are considering a nestloop inner indexpath.
 */
List *
generate_bitmap_or_paths(PlannerInfo *root, RelOptInfo *rel,
						 List *clauses, List *outer_clauses,
						 RelOptInfo *outer_rel)
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
											  outer_rel,
											  SAOP_ALLOW);
				/* Recurse in case there are sub-ORs */
				indlist = list_concat(indlist,
									  generate_bitmap_or_paths(root, rel,
															   andargs,
															   all_clauses,
															   outer_rel));
			}
			else
			{
				Assert(IsA(orarg, RestrictInfo));
				Assert(!restriction_is_or_clause((RestrictInfo *) orarg));
				indlist = find_usable_indexes(root, rel,
											  list_make1(orarg),
											  all_clauses,
											  false,
											  outer_rel,
											  SAOP_ALLOW);
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
			bitmapqual = choose_bitmap_and(root, rel, indlist, outer_rel);
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
choose_bitmap_and(PlannerInfo *root, RelOptInfo *rel,
				  List *paths, RelOptInfo *outer_rel)
{
	int			npaths = list_length(paths);
	PathClauseUsage **pathinfoarray;
	PathClauseUsage *pathinfo;
	List	   *clauselist;
	List	   *bestpaths = NIL;
	Cost		bestcost = 0;
	int			i, j;
	ListCell   *l;

	Assert(npaths > 0);			/* else caller error */
	if (npaths == 1)
		return (Path *) linitial(paths);		/* easy case */

	/*
	 * In theory we should consider every nonempty subset of the given paths.
	 * In practice that seems like overkill, given the crude nature of the
	 * estimates, not to mention the possible effects of higher-level AND and
	 * OR clauses.  Moreover, it's completely impractical if there are a large
	 * number of paths, since the work would grow as O(2^N).
	 *
	 * As a heuristic, we first check for paths using exactly the same
	 * sets of WHERE clauses + index predicate conditions, and reject all
	 * but the cheapest-to-scan in any such group.  This primarily gets rid
	 * of indexes that include the interesting columns but also irrelevant
	 * columns.  (In situations where the DBA has gone overboard on creating
	 * variant indexes, this can make for a very large reduction in the number
	 * of paths considered further.)
	 *
	 * We then sort the surviving paths with the cheapest-to-scan first,
	 * and for each path, consider using that path alone as the basis for
	 * a bitmap scan.  Then we consider bitmap AND scans formed from that
	 * path plus each subsequent (higher-cost) path, adding on a subsequent
	 * path if it results in a reduction in the estimated total scan cost.
	 * This means we consider about O(N^2) rather than O(2^N) path
	 * combinations, which is quite tolerable, especially given than N is
	 * usually reasonably small because of the prefiltering step.  The
	 * cheapest of these is returned.
	 *
	 * We will only consider AND combinations in which no two indexes use
	 * the same WHERE clause.  This is a bit of a kluge: it's needed because
	 * costsize.c and clausesel.c aren't very smart about redundant clauses.
	 * They will usually double-count the redundant clauses, producing a
	 * too-small selectivity that makes a redundant AND step look like it
	 * reduces the total cost.  Perhaps someday that code will be smarter and
	 * we can remove this limitation.  (But note that this also defends
	 * against flat-out duplicate input paths, which can happen because
	 * best_inner_indexscan will find the same OR join clauses that
	 * create_or_index_quals has pulled OR restriction clauses out of.)
	 *
	 * For the same reason, we reject AND combinations in which an index
	 * predicate clause duplicates another clause.  Here we find it necessary
	 * to be even stricter: we'll reject a partial index if any of its
	 * predicate clauses are implied by the set of WHERE clauses and predicate
	 * clauses used so far.  This covers cases such as a condition "x = 42"
	 * used with a plain index, followed by a clauseless scan of a partial
	 * index "WHERE x >= 40 AND x < 50".  The partial index has been accepted
	 * only because "x = 42" was present, and so allowing it would partially
	 * double-count selectivity.  (We could use predicate_implied_by on
	 * regular qual clauses too, to have a more intelligent, but much more
	 * expensive, check for redundancy --- but in most cases simple equality
	 * seems to suffice.)
	 */

	/*
	 * Extract clause usage info and detect any paths that use exactly
	 * the same set of clauses; keep only the cheapest-to-scan of any such
	 * groups.  The surviving paths are put into an array for qsort'ing.
	 */
	pathinfoarray = (PathClauseUsage **)
		palloc(npaths * sizeof(PathClauseUsage *));
	clauselist = NIL;
	npaths = 0;
	foreach(l, paths)
	{
		Path   *ipath = (Path *) lfirst(l);

		pathinfo = classify_index_clause_usage(ipath, &clauselist);
		for (i = 0; i < npaths; i++)
		{
			if (bms_equal(pathinfo->clauseids, pathinfoarray[i]->clauseids))
				break;
		}
		if (i < npaths)
		{
			/* duplicate clauseids, keep the cheaper one */
			Cost		ncost;
			Cost		ocost;
			Selectivity nselec;
			Selectivity oselec;

			cost_bitmap_tree_node(pathinfo->path, &ncost, &nselec);
			cost_bitmap_tree_node(pathinfoarray[i]->path, &ocost, &oselec);
			if (ncost < ocost)
				pathinfoarray[i] = pathinfo;
		}
		else
		{
			/* not duplicate clauseids, add to array */
			pathinfoarray[npaths++] = pathinfo;
		}
	}

	/* If only one surviving path, we're done */
	if (npaths == 1)
		return pathinfoarray[0]->path;

	/* Sort the surviving paths by index access cost */
	qsort(pathinfoarray, npaths, sizeof(PathClauseUsage *),
		  path_usage_comparator);

	/*
	 * For each surviving index, consider it as an "AND group leader", and
	 * see whether adding on any of the later indexes results in an AND path
	 * with cheaper total cost than before.  Then take the cheapest AND group.
	 */
	for (i = 0; i < npaths; i++)
	{
		Cost		costsofar;
		List	   *qualsofar;
		Bitmapset  *clauseidsofar;
		ListCell   *lastcell;

		pathinfo = pathinfoarray[i];
		paths = list_make1(pathinfo->path);
		costsofar = bitmap_scan_cost_est(root, rel, pathinfo->path, outer_rel);
		qualsofar = list_concat(list_copy(pathinfo->quals),
								list_copy(pathinfo->preds));
		clauseidsofar = bms_copy(pathinfo->clauseids);
		lastcell = list_head(paths);	/* for quick deletions */

		for (j = i+1; j < npaths; j++)
		{
			Cost		newcost;

			pathinfo = pathinfoarray[j];
			/* Check for redundancy */
			if (bms_overlap(pathinfo->clauseids, clauseidsofar))
				continue;			/* consider it redundant */
			if (pathinfo->preds)
			{
				bool	redundant = false;

				/* we check each predicate clause separately */
				foreach(l, pathinfo->preds)
				{
					Node	   *np = (Node *) lfirst(l);

					if (predicate_implied_by(list_make1(np), qualsofar))
					{
						redundant = true;
						break;		/* out of inner foreach loop */
					}
				}
				if (redundant)
					continue;
			}
			/* tentatively add new path to paths, so we can estimate cost */
			paths = lappend(paths, pathinfo->path);
			newcost = bitmap_and_cost_est(root, rel, paths, outer_rel);
			if (newcost < costsofar)
			{
				/* keep new path in paths, update subsidiary variables */
				costsofar = newcost;
				qualsofar = list_concat(qualsofar,
										list_copy(pathinfo->quals));
				qualsofar = list_concat(qualsofar,
										list_copy(pathinfo->preds));
				clauseidsofar = bms_add_members(clauseidsofar,
												pathinfo->clauseids);
				lastcell = lnext(lastcell);
			}
			else
			{
				/* reject new path, remove it from paths list */
				paths = list_delete_cell(paths, lnext(lastcell), lastcell);
			}
			Assert(lnext(lastcell) == NULL);
		}

		/* Keep the cheapest AND-group (or singleton) */
		if (i == 0 || costsofar < bestcost)
		{
			bestpaths = paths;
			bestcost = costsofar;
		}

		/* some easy cleanup (we don't try real hard though) */
		list_free(qualsofar);
	}

	if (list_length(bestpaths) == 1)
		return (Path *) linitial(bestpaths);		/* no need for AND */
	return (Path *) create_bitmap_and_path(root, rel, bestpaths);
}

/* qsort comparator to sort in increasing index access cost order */
static int
path_usage_comparator(const void *a, const void *b)
{
	PathClauseUsage *pa = *(PathClauseUsage *const *) a;
	PathClauseUsage *pb = *(PathClauseUsage *const *) b;
	Cost		acost;
	Cost		bcost;
	Selectivity aselec;
	Selectivity bselec;

	cost_bitmap_tree_node(pa->path, &acost, &aselec);
	cost_bitmap_tree_node(pb->path, &bcost, &bselec);

	/*
	 * If costs are the same, sort by selectivity.
	 */
	if (acost < bcost)
		return -1;
	if (acost > bcost)
		return 1;

	if (aselec < bselec)
		return -1;
	if (aselec > bselec)
		return 1;

	return 0;
}

/*
 * Estimate the cost of actually executing a bitmap scan with a single
 * index path (no BitmapAnd, at least not at this level).
 */
static Cost
bitmap_scan_cost_est(PlannerInfo *root, RelOptInfo *rel,
					 Path *ipath, RelOptInfo *outer_rel)
{
	Path		bpath;

	cost_bitmap_heap_scan(&bpath, root, rel, ipath, outer_rel);

	return bpath.total_cost;
}

/*
 * Estimate the cost of actually executing a BitmapAnd scan with the given
 * inputs.
 */
static Cost
bitmap_and_cost_est(PlannerInfo *root, RelOptInfo *rel,
					List *paths, RelOptInfo *outer_rel)
{
	BitmapAndPath apath;
	Path		bpath;

	/* Set up a dummy BitmapAndPath */
	apath.path.type = T_BitmapAndPath;
	apath.path.parent = rel;
	apath.bitmapquals = paths;
	cost_bitmap_and_node(&apath, root);

	/* Now we can do cost_bitmap_heap_scan */
	cost_bitmap_heap_scan(&bpath, root, rel, (Path *) &apath, outer_rel);

	return bpath.total_cost;
}


/*
 * classify_index_clause_usage
 *		Construct a PathClauseUsage struct describing the WHERE clauses and
 *		index predicate clauses used by the given indexscan path.
 *		We consider two clauses the same if they are equal().
 *
 * At some point we might want to migrate this info into the Path data
 * structure proper, but for the moment it's only needed within
 * choose_bitmap_and().
 *
 * *clauselist is used and expanded as needed to identify all the distinct
 * clauses seen across successive calls.  Caller must initialize it to NIL
 * before first call of a set.
 */
static PathClauseUsage *
classify_index_clause_usage(Path *path, List **clauselist)
{
	PathClauseUsage *result;
	Bitmapset  *clauseids;
	ListCell   *lc;

	result = (PathClauseUsage *) palloc(sizeof(PathClauseUsage));
	result->path = path;

	/* Recursively find the quals and preds used by the path */
	result->quals = NIL;
	result->preds = NIL;
	find_indexpath_quals(path, &result->quals, &result->preds);

	/* Build up a bitmapset representing the quals and preds */
	clauseids = NULL;
	foreach(lc, result->quals)
	{
		Node   *node = (Node *) lfirst(lc);

		clauseids = bms_add_member(clauseids,
								   find_list_position(node, clauselist));
	}
	foreach(lc, result->preds)
	{
		Node   *node = (Node *) lfirst(lc);

		clauseids = bms_add_member(clauseids,
								   find_list_position(node, clauselist));
	}
	result->clauseids = clauseids;

	return result;
}


/*
 * find_indexpath_quals
 *
 * Given the Path structure for a plain or bitmap indexscan, extract lists
 * of all the indexquals and index predicate conditions used in the Path.
 * These are appended to the initial contents of *quals and *preds (hence
 * caller should initialize those to NIL).
 *
 * This is sort of a simplified version of make_restrictinfo_from_bitmapqual;
 * here, we are not trying to produce an accurate representation of the AND/OR
 * semantics of the Path, but just find out all the base conditions used.
 *
 * The result lists contain pointers to the expressions used in the Path,
 * but all the list cells are freshly built, so it's safe to destructively
 * modify the lists (eg, by concat'ing with other lists).
 */
static void
find_indexpath_quals(Path *bitmapqual, List **quals, List **preds)
{
	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;
		ListCell   *l;

		foreach(l, apath->bitmapquals)
		{
			find_indexpath_quals((Path *) lfirst(l), quals, preds);
		}
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;
		ListCell   *l;

		foreach(l, opath->bitmapquals)
		{
			find_indexpath_quals((Path *) lfirst(l), quals, preds);
		}
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;

		*quals = list_concat(*quals, get_actual_clauses(ipath->indexclauses));
		*preds = list_concat(*preds, list_copy(ipath->indexinfo->indpred));
	}
	else
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));
}


/*
 * find_list_position
 *		Return the given node's position (counting from 0) in the given
 *		list of nodes.  If it's not equal() to any existing list member,
 *		add it at the end, and return that position.
 */
static int
find_list_position(Node *node, List **nodelist)
{
	int			i;
	ListCell   *lc;

	i = 0;
	foreach(lc, *nodelist)
	{
		Node   *oldnode = (Node *) lfirst(lc);

		if (equal(node, oldnode))
			return i;
		i++;
	}

	*nodelist = lappend(*nodelist, node);

	return i;
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
 * 'saop_control' indicates whether ScalarArrayOpExpr clauses can be used.
 * When it's SAOP_REQUIRE, *found_clause is set TRUE only if we used at least
 * one ScalarArrayOpExpr from the current clauses list.
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
						  SaOpControl saop_control,
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
										 outer_relids,
										 saop_control))
			{
				clausegroup = list_append_unique_ptr(clausegroup, rinfo);
				if (saop_control != SAOP_REQUIRE ||
					IsA(rinfo->clause, ScalarArrayOpExpr))
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
										 outer_relids,
										 saop_control))
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
 *	  It is also possible to match RowCompareExpr clauses to indexes (but
 *	  currently, only btree indexes handle this).  In this routine we will
 *	  report a match if the first column of the row comparison matches the
 *	  target index column.	This is sufficient to guarantee that some index
 *	  condition can be constructed from the RowCompareExpr --- whether the
 *	  remaining columns match the index too is considered in
 *	  expand_indexqual_rowcompare().
 *
 *	  It is also possible to match ScalarArrayOpExpr clauses to indexes, when
 *	  the clause is of the form "indexkey op ANY (arrayconst)".  Since the
 *	  executor can only handle these in the context of bitmap index scans,
 *	  our caller specifies whether to allow these or not.
 *
 *	  For boolean indexes, it is also possible to match the clause directly
 *	  to the indexkey; or perhaps the clause is (NOT indexkey).
 *
 * 'index' is the index of interest.
 * 'indexcol' is a column number of 'index' (counting from 0).
 * 'opclass' is the corresponding operator class.
 * 'rinfo' is the clause to be tested (as a RestrictInfo node).
 * 'saop_control' indicates whether ScalarArrayOpExpr clauses can be used.
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
						 Relids outer_relids,
						 SaOpControl saop_control)
{
	Expr	   *clause = rinfo->clause;
	Node	   *leftop,
			   *rightop;
	Relids		left_relids;
	Relids		right_relids;
	Oid			expr_op;
	bool		plain_op;

	/*
	 * Never match pseudoconstants to indexes.	(Normally this could not
	 * happen anyway, since a pseudoconstant clause couldn't contain a Var,
	 * but what if someone builds an expression index on a constant? It's not
	 * totally unreasonable to do so with a partial index, either.)
	 */
	if (rinfo->pseudoconstant)
		return false;

	/* First check for boolean-index cases. */
	if (IsBooleanOpclass(opclass))
	{
		if (match_boolean_index_clause((Node *) clause, indexcol, index))
			return true;
	}

	/*
	 * Clause must be a binary opclause, or possibly a ScalarArrayOpExpr
	 * (which is always binary, by definition).  Or it could be a
	 * RowCompareExpr, which we pass off to match_rowcompare_to_indexcol().
	 */
	if (is_opclause(clause))
	{
		leftop = get_leftop(clause);
		rightop = get_rightop(clause);
		if (!leftop || !rightop)
			return false;
		left_relids = rinfo->left_relids;
		right_relids = rinfo->right_relids;
		expr_op = ((OpExpr *) clause)->opno;
		plain_op = true;
	}
	else if (saop_control != SAOP_FORBID &&
			 clause && IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

		/* We only accept ANY clauses, not ALL */
		if (!saop->useOr)
			return false;
		leftop = (Node *) linitial(saop->args);
		rightop = (Node *) lsecond(saop->args);
		left_relids = NULL;		/* not actually needed */
		right_relids = pull_varnos(rightop);
		expr_op = saop->opno;
		plain_op = false;
	}
	else if (clause && IsA(clause, RowCompareExpr))
	{
		return match_rowcompare_to_indexcol(index, indexcol, opclass,
											(RowCompareExpr *) clause,
											outer_relids);
	}
	else
		return false;

	/*
	 * Check for clauses of the form: (indexkey operator constant) or
	 * (constant operator indexkey).  See above notes about const-ness.
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		bms_is_subset(right_relids, outer_relids) &&
		!contain_volatile_functions(rightop))
	{
		if (is_indexable_operator(expr_op, opclass, true))
			return true;

		/*
		 * If we didn't find a member of the index's opclass, see whether it
		 * is a "special" indexable operator.
		 */
		if (plain_op &&
			match_special_index_operator(clause, opclass, true))
			return true;
		return false;
	}

	if (plain_op &&
		match_index_to_operand(rightop, indexcol, index) &&
		bms_is_subset(left_relids, outer_relids) &&
		!contain_volatile_functions(leftop))
	{
		if (is_indexable_operator(expr_op, opclass, false))
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
 * is_indexable_operator
 *	  Does the operator match the specified index opclass?
 *
 * If the indexkey is on the right, what we actually want to know
 * is whether the operator has a commutator operator that matches
 * the opclass.
 */
static bool
is_indexable_operator(Oid expr_op, Oid opclass, bool indexkey_on_left)
{
	/* Get the commuted operator if necessary */
	if (!indexkey_on_left)
	{
		expr_op = get_commutator(expr_op);
		if (expr_op == InvalidOid)
			return false;
	}

	/* OK if the (commuted) operator is a member of the index's opclass */
	return op_in_opclass(expr_op, opclass);
}

/*
 * match_rowcompare_to_indexcol()
 *	  Handles the RowCompareExpr case for match_clause_to_indexcol(),
 *	  which see for comments.
 */
static bool
match_rowcompare_to_indexcol(IndexOptInfo *index,
							 int indexcol,
							 Oid opclass,
							 RowCompareExpr *clause,
							 Relids outer_relids)
{
	Node	   *leftop,
			   *rightop;
	Oid			expr_op;

	/* Forget it if we're not dealing with a btree index */
	if (index->relam != BTREE_AM_OID)
		return false;

	/*
	 * We could do the matching on the basis of insisting that the opclass
	 * shown in the RowCompareExpr be the same as the index column's opclass,
	 * but that does not work well for cross-type comparisons (the opclass
	 * could be for the other datatype).  Also it would fail to handle indexes
	 * using reverse-sort opclasses.  Instead, match if the operator listed in
	 * the RowCompareExpr is the < <= > or >= member of the index opclass
	 * (after commutation, if the indexkey is on the right).
	 */
	leftop = (Node *) linitial(clause->largs);
	rightop = (Node *) linitial(clause->rargs);
	expr_op = linitial_oid(clause->opnos);

	/*
	 * These syntactic tests are the same as in match_clause_to_indexcol()
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		bms_is_subset(pull_varnos(rightop), outer_relids) &&
		!contain_volatile_functions(rightop))
	{
		/* OK, indexkey is on left */
	}
	else if (match_index_to_operand(rightop, indexcol, index) &&
			 bms_is_subset(pull_varnos(leftop), outer_relids) &&
			 !contain_volatile_functions(leftop))
	{
		/* indexkey is on right, so commute the operator */
		expr_op = get_commutator(expr_op);
		if (expr_op == InvalidOid)
			return false;
	}
	else
		return false;

	/* We're good if the operator is the right type of opclass member */
	switch (get_op_opclass_strategy(expr_op, opclass))
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			return true;
	}

	return false;
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
										 outer_relids,
										 SAOP_ALLOW))
				return true;

			indexcol++;
			classes++;
		} while (!DoneMatchingIndexKeys(classes));
	}

	return false;
}

/*
 * best_inner_indexscan
 *	  Finds the best available inner indexscans for a nestloop join
 *	  with the given rel on the inside and the given outer_rel outside.
 *
 * *cheapest_startup gets the path with least startup cost
 * *cheapest_total gets the path with least total cost (often the same path)
 * Both are set to NULL if there are no possible inner indexscans.
 *
 * We ignore ordering considerations, since a nestloop's inner scan's order
 * is uninteresting.  Hence startup cost and total cost are the only figures
 * of merit to consider.
 *
 * Note: create_index_paths() must have been run previously for this rel,
 * else the results will always be NULL.
 */
void
best_inner_indexscan(PlannerInfo *root, RelOptInfo *rel,
					 RelOptInfo *outer_rel, JoinType jointype,
					 Path **cheapest_startup, Path **cheapest_total)
{
	Relids		outer_relids;
	bool		isouterjoin;
	List	   *clause_list;
	List	   *indexpaths;
	List	   *bitindexpaths;
	ListCell   *l;
	InnerIndexscanInfo *info;
	MemoryContext oldcontext;

	/* Initialize results for failure returns */
	*cheapest_startup = *cheapest_total = NULL;

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
			return;
	}

	/*
	 * If there are no indexable joinclauses for this rel, exit quickly.
	 */
	if (bms_is_empty(rel->index_outer_relids))
		return;

	/*
	 * Otherwise, we have to do path selection in the memory context of the
	 * given rel, so that any created path can be safely attached to the rel's
	 * cache of best inner paths.  (This is not currently an issue for normal
	 * planning, but it is an issue for GEQO planning.)
	 */
	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

	/*
	 * Intersect the given outer relids with index_outer_relids to find the
	 * set of outer relids actually relevant for this rel. If there are none,
	 * again we can fail immediately.
	 */
	outer_relids = bms_intersect(rel->index_outer_relids, outer_rel->relids);
	if (bms_is_empty(outer_relids))
	{
		bms_free(outer_relids);
		MemoryContextSwitchTo(oldcontext);
		return;
	}

	/*
	 * Look to see if we already computed the result for this set of relevant
	 * outerrels.  (We include the isouterjoin status in the cache lookup key
	 * for safety.	In practice I suspect this is not necessary because it
	 * should always be the same for a given combination of rels.)
	 *
	 * NOTE: because we cache on outer_relids rather than outer_rel->relids,
	 * we will report the same paths and hence path cost for joins with
	 * different sets of irrelevant rels on the outside.  Now that cost_index
	 * is sensitive to outer_rel->rows, this is not really right.  However the
	 * error is probably not large.  Is it worth establishing a separate cache
	 * entry for each distinct outer_rel->relids set to get this right?
	 */
	foreach(l, rel->index_inner_paths)
	{
		info = (InnerIndexscanInfo *) lfirst(l);
		if (bms_equal(info->other_relids, outer_relids) &&
			info->isouterjoin == isouterjoin)
		{
			bms_free(outer_relids);
			MemoryContextSwitchTo(oldcontext);
			*cheapest_startup = info->cheapest_startup_innerpath;
			*cheapest_total = info->cheapest_total_innerpath;
			return;
		}
	}

	/*
	 * Find all the relevant restriction and join clauses.
	 *
	 * Note: because we include restriction clauses, we will find indexscans
	 * that could be plain indexscans, ie, they don't require the join context
	 * at all.	This may seem redundant, but we need to include those scans in
	 * the input given to choose_bitmap_and() to be sure we find optimal AND
	 * combinations of join and non-join scans.  Also, even if the "best inner
	 * indexscan" is just a plain indexscan, it will have a different cost
	 * estimate because of cache effects.
	 */
	clause_list = find_clauses_for_join(root, rel, outer_relids, isouterjoin);

	/*
	 * Find all the index paths that are usable for this join, except for
	 * stuff involving OR and ScalarArrayOpExpr clauses.
	 */
	indexpaths = find_usable_indexes(root, rel,
									 clause_list, NIL,
									 false, outer_rel,
									 SAOP_FORBID);

	/*
	 * Generate BitmapOrPaths for any suitable OR-clauses present in the
	 * clause list.
	 */
	bitindexpaths = generate_bitmap_or_paths(root, rel,
											 clause_list, NIL,
											 outer_rel);

	/*
	 * Likewise, generate paths using ScalarArrayOpExpr clauses; these can't
	 * be simple indexscans but they can be used in bitmap scans.
	 */
	bitindexpaths = list_concat(bitindexpaths,
								find_saop_paths(root, rel,
												clause_list, NIL,
												false, outer_rel));

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

		bitmapqual = choose_bitmap_and(root, rel, bitindexpaths, outer_rel);
		bpath = create_bitmap_heap_path(root, rel, bitmapqual, outer_rel);
		indexpaths = lappend(indexpaths, bpath);
	}

	/*
	 * Now choose the cheapest members of indexpaths.
	 */
	if (indexpaths != NIL)
	{
		*cheapest_startup = *cheapest_total = (Path *) linitial(indexpaths);

		for_each_cell(l, lnext(list_head(indexpaths)))
		{
			Path	   *path = (Path *) lfirst(l);

			if (compare_path_costs(path, *cheapest_startup, STARTUP_COST) < 0)
				*cheapest_startup = path;
			if (compare_path_costs(path, *cheapest_total, TOTAL_COST) < 0)
				*cheapest_total = path;
		}
	}

	/* Cache the results --- whether positive or negative */
	info = makeNode(InnerIndexscanInfo);
	info->other_relids = outer_relids;
	info->isouterjoin = isouterjoin;
	info->cheapest_startup_innerpath = *cheapest_startup;
	info->cheapest_total_innerpath = *cheapest_total;
	rel->index_inner_paths = lcons(info, rel->index_inner_paths);

	MemoryContextSwitchTo(oldcontext);
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
													outer_relids,
													rel->relids,
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

			/* Otherwise, ignore if not a binary opclause */
			if (!is_opclause(clause) || list_length(clause->args) != 2)
				continue;

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
			isIndexable = (opclass == INET_BTREE_OPS_OID ||
						   opclass == CIDR_BTREE_OPS_OID);
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
 *	  will know what to do with.  RowCompare clauses are simplified if
 *	  necessary to create a clause that is fully checkable by the index.
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
			Expr	   *clause = rinfo->clause;

			/* First check for boolean cases */
			if (IsBooleanOpclass(curClass))
			{
				Expr	   *boolqual;

				boolqual = expand_boolean_index_clause((Node *) clause,
													   indexcol,
													   index);
				if (boolqual)
				{
					resultquals = lappend(resultquals,
										  make_restrictinfo(boolqual,
															true,
															false,
															false,
															NULL));
					continue;
				}
			}

			/*
			 * Else it must be an opclause (usual case), ScalarArrayOp, or
			 * RowCompare
			 */
			if (is_opclause(clause))
			{
				resultquals = list_concat(resultquals,
										  expand_indexqual_opclause(rinfo,
																  curClass));
			}
			else if (IsA(clause, ScalarArrayOpExpr))
			{
				/* no extra work at this time */
				resultquals = lappend(resultquals, rinfo);
			}
			else if (IsA(clause, RowCompareExpr))
			{
				resultquals = lappend(resultquals,
									  expand_indexqual_rowcompare(rinfo,
																  index,
																  indexcol));
			}
			else
				elog(ERROR, "unsupported indexqual type: %d",
					 (int) nodeTag(clause));
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
 * expand_indexqual_opclause --- expand a single indexqual condition
 *		that is an operator clause
 *
 * The input is a single RestrictInfo, the output a list of RestrictInfos
 */
static List *
expand_indexqual_opclause(RestrictInfo *rinfo, Oid opclass)
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
 * expand_indexqual_rowcompare --- expand a single indexqual condition
 *		that is a RowCompareExpr
 *
 * It's already known that the first column of the row comparison matches
 * the specified column of the index.  We can use additional columns of the
 * row comparison as index qualifications, so long as they match the index
 * in the "same direction", ie, the indexkeys are all on the same side of the
 * clause and the operators are all the same-type members of the opclasses.
 * If all the columns of the RowCompareExpr match in this way, we just use it
 * as-is.  Otherwise, we build a shortened RowCompareExpr (if more than one
 * column matches) or a simple OpExpr (if the first-column match is all
 * there is).  In these cases the modified clause is always "<=" or ">="
 * even when the original was "<" or ">" --- this is necessary to match all
 * the rows that could match the original.	(We are essentially building a
 * lossy version of the row comparison when we do this.)
 */
static RestrictInfo *
expand_indexqual_rowcompare(RestrictInfo *rinfo,
							IndexOptInfo *index,
							int indexcol)
{
	RowCompareExpr *clause = (RowCompareExpr *) rinfo->clause;
	bool		var_on_left;
	int			op_strategy;
	Oid			op_subtype;
	bool		op_recheck;
	int			matching_cols;
	Oid			expr_op;
	List	   *opclasses;
	List	   *subtypes;
	List	   *new_ops;
	ListCell   *largs_cell;
	ListCell   *rargs_cell;
	ListCell   *opnos_cell;

	/* We have to figure out (again) how the first col matches */
	var_on_left = match_index_to_operand((Node *) linitial(clause->largs),
										 indexcol, index);
	Assert(var_on_left ||
		   match_index_to_operand((Node *) linitial(clause->rargs),
								  indexcol, index));
	expr_op = linitial_oid(clause->opnos);
	if (!var_on_left)
		expr_op = get_commutator(expr_op);
	get_op_opclass_properties(expr_op, index->classlist[indexcol],
							  &op_strategy, &op_subtype, &op_recheck);
	/* Build lists of the opclasses and operator subtypes in case needed */
	opclasses = list_make1_oid(index->classlist[indexcol]);
	subtypes = list_make1_oid(op_subtype);

	/*
	 * See how many of the remaining columns match some index column in the
	 * same way.  A note about rel membership tests: we assume that the clause
	 * as a whole is already known to use only Vars from the indexed relation
	 * and possibly some acceptable outer relations. So the "other" side of
	 * any potential index condition is OK as long as it doesn't use Vars from
	 * the indexed relation.
	 */
	matching_cols = 1;
	largs_cell = lnext(list_head(clause->largs));
	rargs_cell = lnext(list_head(clause->rargs));
	opnos_cell = lnext(list_head(clause->opnos));

	while (largs_cell != NULL)
	{
		Node	   *varop;
		Node	   *constop;
		int			i;

		expr_op = lfirst_oid(opnos_cell);
		if (var_on_left)
		{
			varop = (Node *) lfirst(largs_cell);
			constop = (Node *) lfirst(rargs_cell);
		}
		else
		{
			varop = (Node *) lfirst(rargs_cell);
			constop = (Node *) lfirst(largs_cell);
			/* indexkey is on right, so commute the operator */
			expr_op = get_commutator(expr_op);
			if (expr_op == InvalidOid)
				break;			/* operator is not usable */
		}
		if (bms_is_member(index->rel->relid, pull_varnos(constop)))
			break;				/* no good, Var on wrong side */
		if (contain_volatile_functions(constop))
			break;				/* no good, volatile comparison value */

		/*
		 * The Var side can match any column of the index.	If the user does
		 * something weird like having multiple identical index columns, we
		 * insist the match be on the first such column, to avoid confusing
		 * the executor.
		 */
		for (i = 0; i < index->ncolumns; i++)
		{
			if (match_index_to_operand(varop, i, index))
				break;
		}
		if (i >= index->ncolumns)
			break;				/* no match found */

		/* Now, do we have the right operator for this column? */
		if (get_op_opclass_strategy(expr_op, index->classlist[i])
			!= op_strategy)
			break;

		/* Add opclass and subtype to lists */
		get_op_opclass_properties(expr_op, index->classlist[i],
								  &op_strategy, &op_subtype, &op_recheck);
		opclasses = lappend_oid(opclasses, index->classlist[i]);
		subtypes = lappend_oid(subtypes, op_subtype);

		/* This column matches, keep scanning */
		matching_cols++;
		largs_cell = lnext(largs_cell);
		rargs_cell = lnext(rargs_cell);
		opnos_cell = lnext(opnos_cell);
	}

	/* Return clause as-is if it's all usable as index quals */
	if (matching_cols == list_length(clause->opnos))
		return rinfo;

	/*
	 * We have to generate a subset rowcompare (possibly just one OpExpr). The
	 * painful part of this is changing < to <= or > to >=, so deal with that
	 * first.
	 */
	if (op_strategy == BTLessEqualStrategyNumber ||
		op_strategy == BTGreaterEqualStrategyNumber)
	{
		/* easy, just use the same operators */
		new_ops = list_truncate(list_copy(clause->opnos), matching_cols);
	}
	else
	{
		ListCell   *opclasses_cell;
		ListCell   *subtypes_cell;

		if (op_strategy == BTLessStrategyNumber)
			op_strategy = BTLessEqualStrategyNumber;
		else if (op_strategy == BTGreaterStrategyNumber)
			op_strategy = BTGreaterEqualStrategyNumber;
		else
			elog(ERROR, "unexpected strategy number %d", op_strategy);
		new_ops = NIL;
		forboth(opclasses_cell, opclasses, subtypes_cell, subtypes)
		{
			expr_op = get_opclass_member(lfirst_oid(opclasses_cell),
										 lfirst_oid(subtypes_cell),
										 op_strategy);
			if (!OidIsValid(expr_op))	/* should not happen */
				elog(ERROR, "could not find member %d of opclass %u",
					 op_strategy, lfirst_oid(opclasses_cell));
			if (!var_on_left)
			{
				expr_op = get_commutator(expr_op);
				if (!OidIsValid(expr_op))		/* should not happen */
					elog(ERROR, "could not find commutator of member %d of opclass %u",
						 op_strategy, lfirst_oid(opclasses_cell));
			}
			new_ops = lappend_oid(new_ops, expr_op);
		}
	}

	/* If we have more than one matching col, create a subset rowcompare */
	if (matching_cols > 1)
	{
		RowCompareExpr *rc = makeNode(RowCompareExpr);

		if (var_on_left)
			rc->rctype = (RowCompareType) op_strategy;
		else
			rc->rctype = (op_strategy == BTLessEqualStrategyNumber) ?
				ROWCOMPARE_GE : ROWCOMPARE_LE;
		rc->opnos = new_ops;
		rc->opclasses = list_truncate(list_copy(clause->opclasses),
									  matching_cols);
		rc->largs = list_truncate((List *) copyObject(clause->largs),
								  matching_cols);
		rc->rargs = list_truncate((List *) copyObject(clause->rargs),
								  matching_cols);
		return make_restrictinfo((Expr *) rc, true, false, false, NULL);
	}
	else
	{
		Expr	   *opexpr;

		opexpr = make_opclause(linitial_oid(new_ops), BOOLOID, false,
							   copyObject(linitial(clause->largs)),
							   copyObject(linitial(clause->rargs)));
		return make_restrictinfo(opexpr, true, false, false, NULL);
	}
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
	FmgrInfo	ltproc;
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
		result = list_make1(make_restrictinfo(expr, true, false, false, NULL));
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
	result = list_make1(make_restrictinfo(expr, true, false, false, NULL));

	/*-------
	 * If we can create a string larger than the prefix, we can say
	 * "x < greaterstr".
	 *-------
	 */
	oproid = get_opclass_member(opclass, InvalidOid,
								BTLessStrategyNumber);
	if (oproid == InvalidOid)
		elog(ERROR, "no < operator for opclass %u", opclass);
	fmgr_info(get_opcode(oproid), &ltproc);
	greaterstr = make_greater_string(prefix_const, &ltproc);
	if (greaterstr)
	{
		expr = make_opclause(oproid, BOOLOID, false,
							 (Expr *) leftop, (Expr *) greaterstr);
		result = lappend(result,
						 make_restrictinfo(expr, true, false, false, NULL));
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
	result = list_make1(make_restrictinfo(expr, true, false, false, NULL));

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
	result = lappend(result,
					 make_restrictinfo(expr, true, false, false, NULL));

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
