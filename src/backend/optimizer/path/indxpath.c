/*-------------------------------------------------------------------------
 *
 * indxpath.c
 *	  Routines to determine which indexes are usable for scanning a
 *	  given relation, and create Paths accordingly.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/indxpath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/stratnum.h"
#include "access/sysattr.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"


/* XXX see PartCollMatchesExprColl */
#define IndexCollMatchesExprColl(idxcollation, exprcollation) \
	((idxcollation) == InvalidOid || (idxcollation) == (exprcollation))

/* Whether we are looking for plain indexscan, bitmap scan, or either */
typedef enum
{
	ST_INDEXSCAN,				/* must support amgettuple */
	ST_BITMAPSCAN,				/* must support amgetbitmap */
	ST_ANYSCAN,					/* either is okay */
} ScanTypeControl;

/* Data structure for collecting qual clauses that match an index */
typedef struct
{
	bool		nonempty;		/* True if lists are not all empty */
	/* Lists of IndexClause nodes, one list per index column */
	List	   *indexclauses[INDEX_MAX_KEYS];
} IndexClauseSet;

/* Per-path data used within choose_bitmap_and() */
typedef struct
{
	Path	   *path;			/* IndexPath, BitmapAndPath, or BitmapOrPath */
	List	   *quals;			/* the WHERE clauses it uses */
	List	   *preds;			/* predicates of its partial index(es) */
	Bitmapset  *clauseids;		/* quals+preds represented as a bitmapset */
	bool		unclassifiable; /* has too many quals+preds to process? */
} PathClauseUsage;

/* Callback argument for ec_member_matches_indexcol */
typedef struct
{
	IndexOptInfo *index;		/* index we're considering */
	int			indexcol;		/* index column we want to match to */
} ec_member_matches_arg;


static void consider_index_join_clauses(PlannerInfo *root, RelOptInfo *rel,
										IndexOptInfo *index,
										IndexClauseSet *rclauseset,
										IndexClauseSet *jclauseset,
										IndexClauseSet *eclauseset,
										List **bitindexpaths);
static void consider_index_join_outer_rels(PlannerInfo *root, RelOptInfo *rel,
										   IndexOptInfo *index,
										   IndexClauseSet *rclauseset,
										   IndexClauseSet *jclauseset,
										   IndexClauseSet *eclauseset,
										   List **bitindexpaths,
										   List *indexjoinclauses,
										   int considered_clauses,
										   List **considered_relids);
static void get_join_index_paths(PlannerInfo *root, RelOptInfo *rel,
								 IndexOptInfo *index,
								 IndexClauseSet *rclauseset,
								 IndexClauseSet *jclauseset,
								 IndexClauseSet *eclauseset,
								 List **bitindexpaths,
								 Relids relids,
								 List **considered_relids);
static bool eclass_already_used(EquivalenceClass *parent_ec, Relids oldrelids,
								List *indexjoinclauses);
static void get_index_paths(PlannerInfo *root, RelOptInfo *rel,
							IndexOptInfo *index, IndexClauseSet *clauses,
							List **bitindexpaths);
static List *build_index_paths(PlannerInfo *root, RelOptInfo *rel,
							   IndexOptInfo *index, IndexClauseSet *clauses,
							   bool useful_predicate,
							   ScanTypeControl scantype,
							   bool *skip_nonnative_saop);
static List *build_paths_for_OR(PlannerInfo *root, RelOptInfo *rel,
								List *clauses, List *other_clauses);
static List *generate_bitmap_or_paths(PlannerInfo *root, RelOptInfo *rel,
									  List *clauses, List *other_clauses);
static Path *choose_bitmap_and(PlannerInfo *root, RelOptInfo *rel,
							   List *paths);
static int	path_usage_comparator(const void *a, const void *b);
static Cost bitmap_scan_cost_est(PlannerInfo *root, RelOptInfo *rel,
								 Path *ipath);
static Cost bitmap_and_cost_est(PlannerInfo *root, RelOptInfo *rel,
								List *paths);
static PathClauseUsage *classify_index_clause_usage(Path *path,
													List **clauselist);
static void find_indexpath_quals(Path *bitmapqual, List **quals, List **preds);
static int	find_list_position(Node *node, List **nodelist);
static bool check_index_only(RelOptInfo *rel, IndexOptInfo *index);
static double get_loop_count(PlannerInfo *root, Index cur_relid, Relids outer_relids);
static double adjust_rowcount_for_semijoins(PlannerInfo *root,
											Index cur_relid,
											Index outer_relid,
											double rowcount);
static double approximate_joinrel_size(PlannerInfo *root, Relids relids);
static void match_restriction_clauses_to_index(PlannerInfo *root,
											   IndexOptInfo *index,
											   IndexClauseSet *clauseset);
static void match_join_clauses_to_index(PlannerInfo *root,
										RelOptInfo *rel, IndexOptInfo *index,
										IndexClauseSet *clauseset,
										List **joinorclauses);
static void match_eclass_clauses_to_index(PlannerInfo *root,
										  IndexOptInfo *index,
										  IndexClauseSet *clauseset);
static void match_clauses_to_index(PlannerInfo *root,
								   List *clauses,
								   IndexOptInfo *index,
								   IndexClauseSet *clauseset);
static void match_clause_to_index(PlannerInfo *root,
								  RestrictInfo *rinfo,
								  IndexOptInfo *index,
								  IndexClauseSet *clauseset);
static IndexClause *match_clause_to_indexcol(PlannerInfo *root,
											 RestrictInfo *rinfo,
											 int indexcol,
											 IndexOptInfo *index);
static bool IsBooleanOpfamily(Oid opfamily);
static IndexClause *match_boolean_index_clause(PlannerInfo *root,
											   RestrictInfo *rinfo,
											   int indexcol, IndexOptInfo *index);
static IndexClause *match_opclause_to_indexcol(PlannerInfo *root,
											   RestrictInfo *rinfo,
											   int indexcol,
											   IndexOptInfo *index);
static IndexClause *match_funcclause_to_indexcol(PlannerInfo *root,
												 RestrictInfo *rinfo,
												 int indexcol,
												 IndexOptInfo *index);
static IndexClause *get_index_clause_from_support(PlannerInfo *root,
												  RestrictInfo *rinfo,
												  Oid funcid,
												  int indexarg,
												  int indexcol,
												  IndexOptInfo *index);
static IndexClause *match_saopclause_to_indexcol(PlannerInfo *root,
												 RestrictInfo *rinfo,
												 int indexcol,
												 IndexOptInfo *index);
static IndexClause *match_rowcompare_to_indexcol(PlannerInfo *root,
												 RestrictInfo *rinfo,
												 int indexcol,
												 IndexOptInfo *index);
static IndexClause *match_orclause_to_indexcol(PlannerInfo *root,
											   RestrictInfo *rinfo,
											   int indexcol,
											   IndexOptInfo *index);
static IndexClause *expand_indexqual_rowcompare(PlannerInfo *root,
												RestrictInfo *rinfo,
												int indexcol,
												IndexOptInfo *index,
												Oid expr_op,
												bool var_on_left);
static void match_pathkeys_to_index(IndexOptInfo *index, List *pathkeys,
									List **orderby_clauses_p,
									List **clause_columns_p);
static Expr *match_clause_to_ordering_op(IndexOptInfo *index,
										 int indexcol, Expr *clause, Oid pk_opfamily);
static bool ec_member_matches_indexcol(PlannerInfo *root, RelOptInfo *rel,
									   EquivalenceClass *ec, EquivalenceMember *em,
									   void *arg);


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
 * so it can be applied in any context.  A "parameterized" index scan uses
 * join clauses (plus restriction clauses, if available) in its indexqual.
 * When joining such a scan to one of the relations supplying the other
 * variables used in its indexqual, the parameterized scan must appear as
 * the inner relation of a nestloop join; it can't be used on the outer side,
 * nor in a merge or hash join.  In that context, values for the other rels'
 * attributes are available and fixed during any one scan of the indexpath.
 *
 * An IndexPath is generated and submitted to add_path() for each plain or
 * parameterized index scan this routine deems potentially interesting for
 * the current query.
 *
 * 'rel' is the relation for which we want to generate index paths
 *
 * Note: check_index_predicates() must have been run previously for this rel.
 *
 * Note: in cases involving LATERAL references in the relation's tlist, it's
 * possible that rel->lateral_relids is nonempty.  Currently, we include
 * lateral_relids into the parameterization reported for each path, but don't
 * take it into account otherwise.  The fact that any such rels *must* be
 * available as parameter sources perhaps should influence our choices of
 * index quals ... but for now, it doesn't seem worth troubling over.
 * In particular, comments below about "unparameterized" paths should be read
 * as meaning "unparameterized so far as the indexquals are concerned".
 */
void
create_index_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *indexpaths;
	List	   *bitindexpaths;
	List	   *bitjoinpaths;
	List	   *joinorclauses;
	IndexClauseSet rclauseset;
	IndexClauseSet jclauseset;
	IndexClauseSet eclauseset;
	ListCell   *lc;

	/* Skip the whole mess if no indexes */
	if (rel->indexlist == NIL)
		return;

	/* Bitmap paths are collected and then dealt with at the end */
	bitindexpaths = bitjoinpaths = joinorclauses = NIL;

	/* Examine each index in turn */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);

		/* Protect limited-size array in IndexClauseSets */
		Assert(index->nkeycolumns <= INDEX_MAX_KEYS);

		/*
		 * Ignore partial indexes that do not match the query.
		 * (generate_bitmap_or_paths() might be able to do something with
		 * them, but that's of no concern here.)
		 */
		if (index->indpred != NIL && !index->predOK)
			continue;

		/*
		 * Identify the restriction clauses that can match the index.
		 */
		MemSet(&rclauseset, 0, sizeof(rclauseset));
		match_restriction_clauses_to_index(root, index, &rclauseset);

		/*
		 * Build index paths from the restriction clauses.  These will be
		 * non-parameterized paths.  Plain paths go directly to add_path(),
		 * bitmap paths are added to bitindexpaths to be handled below.
		 */
		get_index_paths(root, rel, index, &rclauseset,
						&bitindexpaths);

		/*
		 * Identify the join clauses that can match the index.  For the moment
		 * we keep them separate from the restriction clauses.  Note that this
		 * step finds only "loose" join clauses that have not been merged into
		 * EquivalenceClasses.  Also, collect join OR clauses for later.
		 */
		MemSet(&jclauseset, 0, sizeof(jclauseset));
		match_join_clauses_to_index(root, rel, index,
									&jclauseset, &joinorclauses);

		/*
		 * Look for EquivalenceClasses that can generate joinclauses matching
		 * the index.
		 */
		MemSet(&eclauseset, 0, sizeof(eclauseset));
		match_eclass_clauses_to_index(root, index,
									  &eclauseset);

		/*
		 * If we found any plain or eclass join clauses, build parameterized
		 * index paths using them.
		 */
		if (jclauseset.nonempty || eclauseset.nonempty)
			consider_index_join_clauses(root, rel, index,
										&rclauseset,
										&jclauseset,
										&eclauseset,
										&bitjoinpaths);
	}

	/*
	 * Generate BitmapOrPaths for any suitable OR-clauses present in the
	 * restriction list.  Add these to bitindexpaths.
	 */
	indexpaths = generate_bitmap_or_paths(root, rel,
										  rel->baserestrictinfo, NIL);
	bitindexpaths = list_concat(bitindexpaths, indexpaths);

	/*
	 * Likewise, generate BitmapOrPaths for any suitable OR-clauses present in
	 * the joinclause list.  Add these to bitjoinpaths.
	 */
	indexpaths = generate_bitmap_or_paths(root, rel,
										  joinorclauses, rel->baserestrictinfo);
	bitjoinpaths = list_concat(bitjoinpaths, indexpaths);

	/*
	 * If we found anything usable, generate a BitmapHeapPath for the most
	 * promising combination of restriction bitmap index paths.  Note there
	 * will be only one such path no matter how many indexes exist.  This
	 * should be sufficient since there's basically only one figure of merit
	 * (total cost) for such a path.
	 */
	if (bitindexpaths != NIL)
	{
		Path	   *bitmapqual;
		BitmapHeapPath *bpath;

		bitmapqual = choose_bitmap_and(root, rel, bitindexpaths);
		bpath = create_bitmap_heap_path(root, rel, bitmapqual,
										rel->lateral_relids, 1.0, 0);
		add_path(rel, (Path *) bpath);

		/* create a partial bitmap heap path */
		if (rel->consider_parallel && rel->lateral_relids == NULL)
			create_partial_bitmap_paths(root, rel, bitmapqual);
	}

	/*
	 * Likewise, if we found anything usable, generate BitmapHeapPaths for the
	 * most promising combinations of join bitmap index paths.  Our strategy
	 * is to generate one such path for each distinct parameterization seen
	 * among the available bitmap index paths.  This may look pretty
	 * expensive, but usually there won't be very many distinct
	 * parameterizations.  (This logic is quite similar to that in
	 * consider_index_join_clauses, but we're working with whole paths not
	 * individual clauses.)
	 */
	if (bitjoinpaths != NIL)
	{
		List	   *all_path_outers;

		/* Identify each distinct parameterization seen in bitjoinpaths */
		all_path_outers = NIL;
		foreach(lc, bitjoinpaths)
		{
			Path	   *path = (Path *) lfirst(lc);
			Relids		required_outer = PATH_REQ_OUTER(path);

			all_path_outers = list_append_unique(all_path_outers,
												 required_outer);
		}

		/* Now, for each distinct parameterization set ... */
		foreach(lc, all_path_outers)
		{
			Relids		max_outers = (Relids) lfirst(lc);
			List	   *this_path_set;
			Path	   *bitmapqual;
			Relids		required_outer;
			double		loop_count;
			BitmapHeapPath *bpath;
			ListCell   *lcp;

			/* Identify all the bitmap join paths needing no more than that */
			this_path_set = NIL;
			foreach(lcp, bitjoinpaths)
			{
				Path	   *path = (Path *) lfirst(lcp);

				if (bms_is_subset(PATH_REQ_OUTER(path), max_outers))
					this_path_set = lappend(this_path_set, path);
			}

			/*
			 * Add in restriction bitmap paths, since they can be used
			 * together with any join paths.
			 */
			this_path_set = list_concat(this_path_set, bitindexpaths);

			/* Select best AND combination for this parameterization */
			bitmapqual = choose_bitmap_and(root, rel, this_path_set);

			/* And push that path into the mix */
			required_outer = PATH_REQ_OUTER(bitmapqual);
			loop_count = get_loop_count(root, rel->relid, required_outer);
			bpath = create_bitmap_heap_path(root, rel, bitmapqual,
											required_outer, loop_count, 0);
			add_path(rel, (Path *) bpath);
		}
	}
}

/*
 * consider_index_join_clauses
 *	  Given sets of join clauses for an index, decide which parameterized
 *	  index paths to build.
 *
 * Plain indexpaths are sent directly to add_path, while potential
 * bitmap indexpaths are added to *bitindexpaths for later processing.
 *
 * 'rel' is the index's heap relation
 * 'index' is the index for which we want to generate paths
 * 'rclauseset' is the collection of indexable restriction clauses
 * 'jclauseset' is the collection of indexable simple join clauses
 * 'eclauseset' is the collection of indexable clauses from EquivalenceClasses
 * '*bitindexpaths' is the list to add bitmap paths to
 */
static void
consider_index_join_clauses(PlannerInfo *root, RelOptInfo *rel,
							IndexOptInfo *index,
							IndexClauseSet *rclauseset,
							IndexClauseSet *jclauseset,
							IndexClauseSet *eclauseset,
							List **bitindexpaths)
{
	int			considered_clauses = 0;
	List	   *considered_relids = NIL;
	int			indexcol;

	/*
	 * The strategy here is to identify every potentially useful set of outer
	 * rels that can provide indexable join clauses.  For each such set,
	 * select all the join clauses available from those outer rels, add on all
	 * the indexable restriction clauses, and generate plain and/or bitmap
	 * index paths for that set of clauses.  This is based on the assumption
	 * that it's always better to apply a clause as an indexqual than as a
	 * filter (qpqual); which is where an available clause would end up being
	 * applied if we omit it from the indexquals.
	 *
	 * This looks expensive, but in most practical cases there won't be very
	 * many distinct sets of outer rels to consider.  As a safety valve when
	 * that's not true, we use a heuristic: limit the number of outer rel sets
	 * considered to a multiple of the number of clauses considered.  (We'll
	 * always consider using each individual join clause, though.)
	 *
	 * For simplicity in selecting relevant clauses, we represent each set of
	 * outer rels as a maximum set of clause_relids --- that is, the indexed
	 * relation itself is also included in the relids set.  considered_relids
	 * lists all relids sets we've already tried.
	 */
	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		/* Consider each applicable simple join clause */
		considered_clauses += list_length(jclauseset->indexclauses[indexcol]);
		consider_index_join_outer_rels(root, rel, index,
									   rclauseset, jclauseset, eclauseset,
									   bitindexpaths,
									   jclauseset->indexclauses[indexcol],
									   considered_clauses,
									   &considered_relids);
		/* Consider each applicable eclass join clause */
		considered_clauses += list_length(eclauseset->indexclauses[indexcol]);
		consider_index_join_outer_rels(root, rel, index,
									   rclauseset, jclauseset, eclauseset,
									   bitindexpaths,
									   eclauseset->indexclauses[indexcol],
									   considered_clauses,
									   &considered_relids);
	}
}

/*
 * consider_index_join_outer_rels
 *	  Generate parameterized paths based on clause relids in the clause list.
 *
 * Workhorse for consider_index_join_clauses; see notes therein for rationale.
 *
 * 'rel', 'index', 'rclauseset', 'jclauseset', 'eclauseset', and
 *		'bitindexpaths' as above
 * 'indexjoinclauses' is a list of IndexClauses for join clauses
 * 'considered_clauses' is the total number of clauses considered (so far)
 * '*considered_relids' is a list of all relids sets already considered
 */
static void
consider_index_join_outer_rels(PlannerInfo *root, RelOptInfo *rel,
							   IndexOptInfo *index,
							   IndexClauseSet *rclauseset,
							   IndexClauseSet *jclauseset,
							   IndexClauseSet *eclauseset,
							   List **bitindexpaths,
							   List *indexjoinclauses,
							   int considered_clauses,
							   List **considered_relids)
{
	ListCell   *lc;

	/* Examine relids of each joinclause in the given list */
	foreach(lc, indexjoinclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);
		Relids		clause_relids = iclause->rinfo->clause_relids;
		EquivalenceClass *parent_ec = iclause->rinfo->parent_ec;
		int			num_considered_relids;

		/* If we already tried its relids set, no need to do so again */
		if (list_member(*considered_relids, clause_relids))
			continue;

		/*
		 * Generate the union of this clause's relids set with each
		 * previously-tried set.  This ensures we try this clause along with
		 * every interesting subset of previous clauses.  However, to avoid
		 * exponential growth of planning time when there are many clauses,
		 * limit the number of relid sets accepted to 10 * considered_clauses.
		 *
		 * Note: get_join_index_paths appends entries to *considered_relids,
		 * but we do not need to visit such newly-added entries within this
		 * loop, so we don't use foreach() here.  No real harm would be done
		 * if we did visit them, since the subset check would reject them; but
		 * it would waste some cycles.
		 */
		num_considered_relids = list_length(*considered_relids);
		for (int pos = 0; pos < num_considered_relids; pos++)
		{
			Relids		oldrelids = (Relids) list_nth(*considered_relids, pos);

			/*
			 * If either is a subset of the other, no new set is possible.
			 * This isn't a complete test for redundancy, but it's easy and
			 * cheap.  get_join_index_paths will check more carefully if we
			 * already generated the same relids set.
			 */
			if (bms_subset_compare(clause_relids, oldrelids) != BMS_DIFFERENT)
				continue;

			/*
			 * If this clause was derived from an equivalence class, the
			 * clause list may contain other clauses derived from the same
			 * eclass.  We should not consider that combining this clause with
			 * one of those clauses generates a usefully different
			 * parameterization; so skip if any clause derived from the same
			 * eclass would already have been included when using oldrelids.
			 */
			if (parent_ec &&
				eclass_already_used(parent_ec, oldrelids,
									indexjoinclauses))
				continue;

			/*
			 * If the number of relid sets considered exceeds our heuristic
			 * limit, stop considering combinations of clauses.  We'll still
			 * consider the current clause alone, though (below this loop).
			 */
			if (list_length(*considered_relids) >= 10 * considered_clauses)
				break;

			/* OK, try the union set */
			get_join_index_paths(root, rel, index,
								 rclauseset, jclauseset, eclauseset,
								 bitindexpaths,
								 bms_union(clause_relids, oldrelids),
								 considered_relids);
		}

		/* Also try this set of relids by itself */
		get_join_index_paths(root, rel, index,
							 rclauseset, jclauseset, eclauseset,
							 bitindexpaths,
							 clause_relids,
							 considered_relids);
	}
}

/*
 * get_join_index_paths
 *	  Generate index paths using clauses from the specified outer relations.
 *	  In addition to generating paths, relids is added to *considered_relids
 *	  if not already present.
 *
 * Workhorse for consider_index_join_clauses; see notes therein for rationale.
 *
 * 'rel', 'index', 'rclauseset', 'jclauseset', 'eclauseset',
 *		'bitindexpaths', 'considered_relids' as above
 * 'relids' is the current set of relids to consider (the target rel plus
 *		one or more outer rels)
 */
static void
get_join_index_paths(PlannerInfo *root, RelOptInfo *rel,
					 IndexOptInfo *index,
					 IndexClauseSet *rclauseset,
					 IndexClauseSet *jclauseset,
					 IndexClauseSet *eclauseset,
					 List **bitindexpaths,
					 Relids relids,
					 List **considered_relids)
{
	IndexClauseSet clauseset;
	int			indexcol;

	/* If we already considered this relids set, don't repeat the work */
	if (list_member(*considered_relids, relids))
		return;

	/* Identify indexclauses usable with this relids set */
	MemSet(&clauseset, 0, sizeof(clauseset));

	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		ListCell   *lc;

		/* First find applicable simple join clauses */
		foreach(lc, jclauseset->indexclauses[indexcol])
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);

			if (bms_is_subset(iclause->rinfo->clause_relids, relids))
				clauseset.indexclauses[indexcol] =
					lappend(clauseset.indexclauses[indexcol], iclause);
		}

		/*
		 * Add applicable eclass join clauses.  The clauses generated for each
		 * column are redundant (cf generate_implied_equalities_for_column),
		 * so we need at most one.  This is the only exception to the general
		 * rule of using all available index clauses.
		 */
		foreach(lc, eclauseset->indexclauses[indexcol])
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);

			if (bms_is_subset(iclause->rinfo->clause_relids, relids))
			{
				clauseset.indexclauses[indexcol] =
					lappend(clauseset.indexclauses[indexcol], iclause);
				break;
			}
		}

		/* Add restriction clauses */
		clauseset.indexclauses[indexcol] =
			list_concat(clauseset.indexclauses[indexcol],
						rclauseset->indexclauses[indexcol]);

		if (clauseset.indexclauses[indexcol] != NIL)
			clauseset.nonempty = true;
	}

	/* We should have found something, else caller passed silly relids */
	Assert(clauseset.nonempty);

	/* Build index path(s) using the collected set of clauses */
	get_index_paths(root, rel, index, &clauseset, bitindexpaths);

	/*
	 * Remember we considered paths for this set of relids.
	 */
	*considered_relids = lappend(*considered_relids, relids);
}

/*
 * eclass_already_used
 *		True if any join clause usable with oldrelids was generated from
 *		the specified equivalence class.
 */
static bool
eclass_already_used(EquivalenceClass *parent_ec, Relids oldrelids,
					List *indexjoinclauses)
{
	ListCell   *lc;

	foreach(lc, indexjoinclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);
		RestrictInfo *rinfo = iclause->rinfo;

		if (rinfo->parent_ec == parent_ec &&
			bms_is_subset(rinfo->clause_relids, oldrelids))
			return true;
	}
	return false;
}


/*
 * get_index_paths
 *	  Given an index and a set of index clauses for it, construct IndexPaths.
 *
 * Plain indexpaths are sent directly to add_path, while potential
 * bitmap indexpaths are added to *bitindexpaths for later processing.
 *
 * This is a fairly simple frontend to build_index_paths().  Its reason for
 * existence is mainly to handle ScalarArrayOpExpr quals properly.  If the
 * index AM supports them natively, we should just include them in simple
 * index paths.  If not, we should exclude them while building simple index
 * paths, and then make a separate attempt to include them in bitmap paths.
 */
static void
get_index_paths(PlannerInfo *root, RelOptInfo *rel,
				IndexOptInfo *index, IndexClauseSet *clauses,
				List **bitindexpaths)
{
	List	   *indexpaths;
	bool		skip_nonnative_saop = false;
	ListCell   *lc;

	/*
	 * Build simple index paths using the clauses.  Allow ScalarArrayOpExpr
	 * clauses only if the index AM supports them natively.
	 */
	indexpaths = build_index_paths(root, rel,
								   index, clauses,
								   index->predOK,
								   ST_ANYSCAN,
								   &skip_nonnative_saop);

	/*
	 * Submit all the ones that can form plain IndexScan plans to add_path. (A
	 * plain IndexPath can represent either a plain IndexScan or an
	 * IndexOnlyScan, but for our purposes here that distinction does not
	 * matter.  However, some of the indexes might support only bitmap scans,
	 * and those we mustn't submit to add_path here.)
	 *
	 * Also, pick out the ones that are usable as bitmap scans.  For that, we
	 * must discard indexes that don't support bitmap scans, and we also are
	 * only interested in paths that have some selectivity; we should discard
	 * anything that was generated solely for ordering purposes.
	 */
	foreach(lc, indexpaths)
	{
		IndexPath  *ipath = (IndexPath *) lfirst(lc);

		if (index->amhasgettuple)
			add_path(rel, (Path *) ipath);

		if (index->amhasgetbitmap &&
			(ipath->path.pathkeys == NIL ||
			 ipath->indexselectivity < 1.0))
			*bitindexpaths = lappend(*bitindexpaths, ipath);
	}

	/*
	 * If there were ScalarArrayOpExpr clauses that the index can't handle
	 * natively, generate bitmap scan paths relying on executor-managed
	 * ScalarArrayOpExpr.
	 */
	if (skip_nonnative_saop)
	{
		indexpaths = build_index_paths(root, rel,
									   index, clauses,
									   false,
									   ST_BITMAPSCAN,
									   NULL);
		*bitindexpaths = list_concat(*bitindexpaths, indexpaths);
	}
}

/*
 * build_index_paths
 *	  Given an index and a set of index clauses for it, construct zero
 *	  or more IndexPaths. It also constructs zero or more partial IndexPaths.
 *
 * We return a list of paths because (1) this routine checks some cases
 * that should cause us to not generate any IndexPath, and (2) in some
 * cases we want to consider both a forward and a backward scan, so as
 * to obtain both sort orders.  Note that the paths are just returned
 * to the caller and not immediately fed to add_path().
 *
 * At top level, useful_predicate should be exactly the index's predOK flag
 * (ie, true if it has a predicate that was proven from the restriction
 * clauses).  When working on an arm of an OR clause, useful_predicate
 * should be true if the predicate required the current OR list to be proven.
 * Note that this routine should never be called at all if the index has an
 * unprovable predicate.
 *
 * scantype indicates whether we want to create plain indexscans, bitmap
 * indexscans, or both.  When it's ST_BITMAPSCAN, we will not consider
 * index ordering while deciding if a Path is worth generating.
 *
 * If skip_nonnative_saop is non-NULL, we ignore ScalarArrayOpExpr clauses
 * unless the index AM supports them directly, and we set *skip_nonnative_saop
 * to true if we found any such clauses (caller must initialize the variable
 * to false).  If it's NULL, we do not ignore ScalarArrayOpExpr clauses.
 *
 * 'rel' is the index's heap relation
 * 'index' is the index for which we want to generate paths
 * 'clauses' is the collection of indexable clauses (IndexClause nodes)
 * 'useful_predicate' indicates whether the index has a useful predicate
 * 'scantype' indicates whether we need plain or bitmap scan support
 * 'skip_nonnative_saop' indicates whether to accept SAOP if index AM doesn't
 */
static List *
build_index_paths(PlannerInfo *root, RelOptInfo *rel,
				  IndexOptInfo *index, IndexClauseSet *clauses,
				  bool useful_predicate,
				  ScanTypeControl scantype,
				  bool *skip_nonnative_saop)
{
	List	   *result = NIL;
	IndexPath  *ipath;
	List	   *index_clauses;
	Relids		outer_relids;
	double		loop_count;
	List	   *orderbyclauses;
	List	   *orderbyclausecols;
	List	   *index_pathkeys;
	List	   *useful_pathkeys;
	bool		pathkeys_possibly_useful;
	bool		index_is_ordered;
	bool		index_only_scan;
	int			indexcol;

	Assert(skip_nonnative_saop != NULL || scantype == ST_BITMAPSCAN);

	/*
	 * Check that index supports the desired scan type(s)
	 */
	switch (scantype)
	{
		case ST_INDEXSCAN:
			if (!index->amhasgettuple)
				return NIL;
			break;
		case ST_BITMAPSCAN:
			if (!index->amhasgetbitmap)
				return NIL;
			break;
		case ST_ANYSCAN:
			/* either or both are OK */
			break;
	}

	/*
	 * 1. Combine the per-column IndexClause lists into an overall list.
	 *
	 * In the resulting list, clauses are ordered by index key, so that the
	 * column numbers form a nondecreasing sequence.  (This order is depended
	 * on by btree and possibly other places.)  The list can be empty, if the
	 * index AM allows that.
	 *
	 * We also build a Relids set showing which outer rels are required by the
	 * selected clauses.  Any lateral_relids are included in that, but not
	 * otherwise accounted for.
	 */
	index_clauses = NIL;
	outer_relids = bms_copy(rel->lateral_relids);
	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		ListCell   *lc;

		foreach(lc, clauses->indexclauses[indexcol])
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);
			RestrictInfo *rinfo = iclause->rinfo;

			if (skip_nonnative_saop && !index->amsearcharray &&
				IsA(rinfo->clause, ScalarArrayOpExpr))
			{
				/*
				 * Caller asked us to generate IndexPaths that omit any
				 * ScalarArrayOpExpr clauses when the underlying index AM
				 * lacks native support.
				 *
				 * We must omit this clause (and tell caller about it).
				 */
				*skip_nonnative_saop = true;
				continue;
			}

			/* OK to include this clause */
			index_clauses = lappend(index_clauses, iclause);
			outer_relids = bms_add_members(outer_relids,
										   rinfo->clause_relids);
		}

		/*
		 * If no clauses match the first index column, check for amoptionalkey
		 * restriction.  We can't generate a scan over an index with
		 * amoptionalkey = false unless there's at least one index clause.
		 * (When working on columns after the first, this test cannot fail. It
		 * is always okay for columns after the first to not have any
		 * clauses.)
		 */
		if (index_clauses == NIL && !index->amoptionalkey)
			return NIL;
	}

	/* We do not want the index's rel itself listed in outer_relids */
	outer_relids = bms_del_member(outer_relids, rel->relid);

	/* Compute loop_count for cost estimation purposes */
	loop_count = get_loop_count(root, rel->relid, outer_relids);

	/*
	 * 2. Compute pathkeys describing index's ordering, if any, then see how
	 * many of them are actually useful for this query.  This is not relevant
	 * if we are only trying to build bitmap indexscans.
	 */
	pathkeys_possibly_useful = (scantype != ST_BITMAPSCAN &&
								has_useful_pathkeys(root, rel));
	index_is_ordered = (index->sortopfamily != NULL);
	if (index_is_ordered && pathkeys_possibly_useful)
	{
		index_pathkeys = build_index_pathkeys(root, index,
											  ForwardScanDirection);
		useful_pathkeys = truncate_useless_pathkeys(root, rel,
													index_pathkeys);
		orderbyclauses = NIL;
		orderbyclausecols = NIL;
	}
	else if (index->amcanorderbyop && pathkeys_possibly_useful)
	{
		/*
		 * See if we can generate ordering operators for query_pathkeys or at
		 * least some prefix thereof.  Matching to just a prefix of the
		 * query_pathkeys will allow an incremental sort to be considered on
		 * the index's partially sorted results.
		 */
		match_pathkeys_to_index(index, root->query_pathkeys,
								&orderbyclauses,
								&orderbyclausecols);
		if (list_length(root->query_pathkeys) == list_length(orderbyclauses))
			useful_pathkeys = root->query_pathkeys;
		else
			useful_pathkeys = list_copy_head(root->query_pathkeys,
											 list_length(orderbyclauses));
	}
	else
	{
		useful_pathkeys = NIL;
		orderbyclauses = NIL;
		orderbyclausecols = NIL;
	}

	/*
	 * 3. Check if an index-only scan is possible.  If we're not building
	 * plain indexscans, this isn't relevant since bitmap scans don't support
	 * index data retrieval anyway.
	 */
	index_only_scan = (scantype != ST_BITMAPSCAN &&
					   check_index_only(rel, index));

	/*
	 * 4. Generate an indexscan path if there are relevant restriction clauses
	 * in the current clauses, OR the index ordering is potentially useful for
	 * later merging or final output ordering, OR the index has a useful
	 * predicate, OR an index-only scan is possible.
	 */
	if (index_clauses != NIL || useful_pathkeys != NIL || useful_predicate ||
		index_only_scan)
	{
		ipath = create_index_path(root, index,
								  index_clauses,
								  orderbyclauses,
								  orderbyclausecols,
								  useful_pathkeys,
								  ForwardScanDirection,
								  index_only_scan,
								  outer_relids,
								  loop_count,
								  false);
		result = lappend(result, ipath);

		/*
		 * If appropriate, consider parallel index scan.  We don't allow
		 * parallel index scan for bitmap index scans.
		 */
		if (index->amcanparallel &&
			rel->consider_parallel && outer_relids == NULL &&
			scantype != ST_BITMAPSCAN)
		{
			ipath = create_index_path(root, index,
									  index_clauses,
									  orderbyclauses,
									  orderbyclausecols,
									  useful_pathkeys,
									  ForwardScanDirection,
									  index_only_scan,
									  outer_relids,
									  loop_count,
									  true);

			/*
			 * if, after costing the path, we find that it's not worth using
			 * parallel workers, just free it.
			 */
			if (ipath->path.parallel_workers > 0)
				add_partial_path(rel, (Path *) ipath);
			else
				pfree(ipath);
		}
	}

	/*
	 * 5. If the index is ordered, a backwards scan might be interesting.
	 */
	if (index_is_ordered && pathkeys_possibly_useful)
	{
		index_pathkeys = build_index_pathkeys(root, index,
											  BackwardScanDirection);
		useful_pathkeys = truncate_useless_pathkeys(root, rel,
													index_pathkeys);
		if (useful_pathkeys != NIL)
		{
			ipath = create_index_path(root, index,
									  index_clauses,
									  NIL,
									  NIL,
									  useful_pathkeys,
									  BackwardScanDirection,
									  index_only_scan,
									  outer_relids,
									  loop_count,
									  false);
			result = lappend(result, ipath);

			/* If appropriate, consider parallel index scan */
			if (index->amcanparallel &&
				rel->consider_parallel && outer_relids == NULL &&
				scantype != ST_BITMAPSCAN)
			{
				ipath = create_index_path(root, index,
										  index_clauses,
										  NIL,
										  NIL,
										  useful_pathkeys,
										  BackwardScanDirection,
										  index_only_scan,
										  outer_relids,
										  loop_count,
										  true);

				/*
				 * if, after costing the path, we find that it's not worth
				 * using parallel workers, just free it.
				 */
				if (ipath->path.parallel_workers > 0)
					add_partial_path(rel, (Path *) ipath);
				else
					pfree(ipath);
			}
		}
	}

	return result;
}

/*
 * build_paths_for_OR
 *	  Given a list of restriction clauses from one arm of an OR clause,
 *	  construct all matching IndexPaths for the relation.
 *
 * Here we must scan all indexes of the relation, since a bitmap OR tree
 * can use multiple indexes.
 *
 * The caller actually supplies two lists of restriction clauses: some
 * "current" ones and some "other" ones.  Both lists can be used freely
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
 * 'rel' is the relation for which we want to generate index paths
 * 'clauses' is the current list of clauses (RestrictInfo nodes)
 * 'other_clauses' is the list of additional upper-level clauses
 */
static List *
build_paths_for_OR(PlannerInfo *root, RelOptInfo *rel,
				   List *clauses, List *other_clauses)
{
	List	   *result = NIL;
	List	   *all_clauses = NIL;	/* not computed till needed */
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		IndexClauseSet clauseset;
		List	   *indexpaths;
		bool		useful_predicate;

		/* Ignore index if it doesn't support bitmap scans */
		if (!index->amhasgetbitmap)
			continue;

		/*
		 * Ignore partial indexes that do not match the query.  If a partial
		 * index is marked predOK then we know it's OK.  Otherwise, we have to
		 * test whether the added clauses are sufficient to imply the
		 * predicate. If so, we can use the index in the current context.
		 *
		 * We set useful_predicate to true iff the predicate was proven using
		 * the current set of clauses.  This is needed to prevent matching a
		 * predOK index to an arm of an OR, which would be a legal but
		 * pointlessly inefficient plan.  (A better plan will be generated by
		 * just scanning the predOK index alone, no OR.)
		 */
		useful_predicate = false;
		if (index->indpred != NIL)
		{
			if (index->predOK)
			{
				/* Usable, but don't set useful_predicate */
			}
			else
			{
				/* Form all_clauses if not done already */
				if (all_clauses == NIL)
					all_clauses = list_concat_copy(clauses, other_clauses);

				if (!predicate_implied_by(index->indpred, all_clauses, false))
					continue;	/* can't use it at all */

				if (!predicate_implied_by(index->indpred, other_clauses, false))
					useful_predicate = true;
			}
		}

		/*
		 * Identify the restriction clauses that can match the index.
		 */
		MemSet(&clauseset, 0, sizeof(clauseset));
		match_clauses_to_index(root, clauses, index, &clauseset);

		/*
		 * If no matches so far, and the index predicate isn't useful, we
		 * don't want it.
		 */
		if (!clauseset.nonempty && !useful_predicate)
			continue;

		/*
		 * Add "other" restriction clauses to the clauseset.
		 */
		match_clauses_to_index(root, other_clauses, index, &clauseset);

		/*
		 * Construct paths if possible.
		 */
		indexpaths = build_index_paths(root, rel,
									   index, &clauseset,
									   useful_predicate,
									   ST_BITMAPSCAN,
									   NULL);
		result = list_concat(result, indexpaths);
	}

	return result;
}

/*
 * Utility structure used to group similar OR-clause arguments in
 * group_similar_or_args().  It represents information about the OR-clause
 * argument and its matching index key.
 */
typedef struct
{
	int			indexnum;		/* index of the matching index, or -1 if no
								 * matching index */
	int			colnum;			/* index of the matching column, or -1 if no
								 * matching index */
	Oid			opno;			/* OID of the OpClause operator, or InvalidOid
								 * if not an OpExpr */
	Oid			inputcollid;	/* OID of the OpClause input collation */
	int			argindex;		/* index of the clause in the list of
								 * arguments */
	int			groupindex;		/* value of argindex for the fist clause in
								 * the group of similar clauses */
} OrArgIndexMatch;

/*
 * Comparison function for OrArgIndexMatch which provides sort order placing
 * similar OR-clause arguments together.
 */
static int
or_arg_index_match_cmp(const void *a, const void *b)
{
	const OrArgIndexMatch *match_a = (const OrArgIndexMatch *) a;
	const OrArgIndexMatch *match_b = (const OrArgIndexMatch *) b;

	if (match_a->indexnum < match_b->indexnum)
		return -1;
	else if (match_a->indexnum > match_b->indexnum)
		return 1;

	if (match_a->colnum < match_b->colnum)
		return -1;
	else if (match_a->colnum > match_b->colnum)
		return 1;

	if (match_a->opno < match_b->opno)
		return -1;
	else if (match_a->opno > match_b->opno)
		return 1;

	if (match_a->inputcollid < match_b->inputcollid)
		return -1;
	else if (match_a->inputcollid > match_b->inputcollid)
		return 1;

	if (match_a->argindex < match_b->argindex)
		return -1;
	else if (match_a->argindex > match_b->argindex)
		return 1;

	return 0;
}

/*
 * Another comparison function for OrArgIndexMatch.  It sorts groups together
 * using groupindex.  The group items are then sorted by argindex.
 */
static int
or_arg_index_match_cmp_group(const void *a, const void *b)
{
	const OrArgIndexMatch *match_a = (const OrArgIndexMatch *) a;
	const OrArgIndexMatch *match_b = (const OrArgIndexMatch *) b;

	if (match_a->groupindex < match_b->groupindex)
		return -1;
	else if (match_a->groupindex > match_b->groupindex)
		return 1;

	if (match_a->argindex < match_b->argindex)
		return -1;
	else if (match_a->argindex > match_b->argindex)
		return 1;

	return 0;
}

/*
 * group_similar_or_args
 *		Transform incoming OR-restrictinfo into a list of sub-restrictinfos,
 *		each of them containing a subset of similar OR-clause arguments from
 *		the source rinfo.
 *
 * Similar OR-clause arguments are of the form "indexkey op constant" having
 * the same indexkey, operator, and collation.  Constant may comprise either
 * Const or Param.  It may be employed later, during the
 * match_clause_to_indexcol() to transform the whole OR-sub-rinfo to an SAOP
 * clause.
 *
 * Returns the processed list of OR-clause arguments.
 */
static List *
group_similar_or_args(PlannerInfo *root, RelOptInfo *rel, RestrictInfo *rinfo)
{
	int			n;
	int			i;
	int			group_start;
	OrArgIndexMatch *matches;
	bool		matched = false;
	ListCell   *lc;
	ListCell   *lc2;
	List	   *orargs;
	List	   *result = NIL;
	Index		relid = rel->relid;

	Assert(IsA(rinfo->orclause, BoolExpr));
	orargs = ((BoolExpr *) rinfo->orclause)->args;
	n = list_length(orargs);

	/*
	 * To avoid N^2 behavior, take utility pass along the list of OR-clause
	 * arguments.  For each argument, fill the OrArgIndexMatch structure,
	 * which will be used to sort these arguments at the next step.
	 */
	i = -1;
	matches = (OrArgIndexMatch *) palloc(sizeof(OrArgIndexMatch) * n);
	foreach(lc, orargs)
	{
		Node	   *arg = lfirst(lc);
		RestrictInfo *argrinfo;
		OpExpr	   *clause;
		Oid			opno;
		Node	   *leftop,
				   *rightop;
		Node	   *nonConstExpr;
		int			indexnum;
		int			colnum;

		i++;
		matches[i].argindex = i;
		matches[i].groupindex = i;
		matches[i].indexnum = -1;
		matches[i].colnum = -1;
		matches[i].opno = InvalidOid;
		matches[i].inputcollid = InvalidOid;

		if (!IsA(arg, RestrictInfo))
			continue;

		argrinfo = castNode(RestrictInfo, arg);

		/* Only operator clauses can match  */
		if (!IsA(argrinfo->clause, OpExpr))
			continue;

		clause = (OpExpr *) argrinfo->clause;
		opno = clause->opno;

		/* Only binary operators can match  */
		if (list_length(clause->args) != 2)
			continue;

		/*
		 * Ignore any RelabelType node above the operands.  This is needed to
		 * be able to apply indexscanning in binary-compatible-operator cases.
		 * Note: we can assume there is at most one RelabelType node;
		 * eval_const_expressions() will have simplified if more than one.
		 */
		leftop = get_leftop(clause);
		if (IsA(leftop, RelabelType))
			leftop = (Node *) ((RelabelType *) leftop)->arg;

		rightop = get_rightop(clause);
		if (IsA(rightop, RelabelType))
			rightop = (Node *) ((RelabelType *) rightop)->arg;

		/*
		 * Check for clauses of the form: (indexkey operator constant) or
		 * (constant operator indexkey).  But we don't know a particular index
		 * yet.  Therefore, we try to distinguish the potential index key and
		 * constant first, then search for a matching index key among all
		 * indexes.
		 */
		if (bms_is_member(relid, argrinfo->right_relids) &&
			!bms_is_member(relid, argrinfo->left_relids) &&
			!contain_volatile_functions(leftop))
		{
			opno = get_commutator(opno);

			if (!OidIsValid(opno))
			{
				/* commutator doesn't exist, we can't reverse the order */
				continue;
			}
			nonConstExpr = rightop;
		}
		else if (bms_is_member(relid, argrinfo->left_relids) &&
				 !bms_is_member(relid, argrinfo->right_relids) &&
				 !contain_volatile_functions(rightop))
		{
			nonConstExpr = leftop;
		}
		else
		{
			continue;
		}

		/*
		 * Match non-constant part to the index key.  It's possible that a
		 * single non-constant part matches multiple index keys.  It's OK, we
		 * just stop with first matching index key.  Given that this choice is
		 * determined the same for every clause, we will group similar clauses
		 * together anyway.
		 */
		indexnum = 0;
		foreach(lc2, rel->indexlist)
		{
			IndexOptInfo *index = (IndexOptInfo *) lfirst(lc2);

			/*
			 * Ignore index if it doesn't support bitmap scans or SAOP
			 * clauses.
			 */
			if (!index->amhasgetbitmap || !index->amsearcharray)
				continue;

			for (colnum = 0; colnum < index->nkeycolumns; colnum++)
			{
				if (match_index_to_operand(nonConstExpr, colnum, index))
				{
					matches[i].indexnum = indexnum;
					matches[i].colnum = colnum;
					matches[i].opno = opno;
					matches[i].inputcollid = clause->inputcollid;
					matched = true;
					break;
				}
			}

			/*
			 * Stop looping through the indexes, if we managed to match
			 * nonConstExpr to any index column.
			 */
			if (matches[i].indexnum >= 0)
				break;
			indexnum++;
		}
	}

	/*
	 * Fast-path check: if no clause is matching to the index column, we can
	 * just give up at this stage and return the clause list as-is.
	 */
	if (!matched)
	{
		pfree(matches);
		return orargs;
	}

	/*
	 * Sort clauses to make similar clauses go together.  But at the same
	 * time, we would like to change the order of clauses as little as
	 * possible.  To do so, we reorder each group of similar clauses so that
	 * the first item of the group stays in place, and all the other items are
	 * moved after it.  So, if there are no similar clauses, the order of
	 * clauses stays the same.  When there are some groups, required
	 * reordering happens while the rest of the clauses remain in their
	 * places.  That is achieved by assigning a 'groupindex' to each clause:
	 * the number of the first item in the group in the original clause list.
	 */
	qsort(matches, n, sizeof(OrArgIndexMatch), or_arg_index_match_cmp);

	/* Assign groupindex to the sorted clauses */
	for (i = 1; i < n; i++)
	{
		/*
		 * When two clauses are similar and should belong to the same group,
		 * copy the 'groupindex' from the previous clause.  Given we are
		 * considering clauses in direct order, all the clauses would have a
		 * 'groupindex' equal to the 'groupindex' of the first clause in the
		 * group.
		 */
		if (matches[i].indexnum == matches[i - 1].indexnum &&
			matches[i].colnum == matches[i - 1].colnum &&
			matches[i].opno == matches[i - 1].opno &&
			matches[i].inputcollid == matches[i - 1].inputcollid &&
			matches[i].indexnum != -1)
			matches[i].groupindex = matches[i - 1].groupindex;
	}

	/* Re-sort clauses first by groupindex then by argindex */
	qsort(matches, n, sizeof(OrArgIndexMatch), or_arg_index_match_cmp_group);

	/*
	 * Group similar clauses into single sub-restrictinfo. Side effect: the
	 * resulting list of restrictions will be sorted by indexnum and colnum.
	 */
	group_start = 0;
	for (i = 1; i <= n; i++)
	{
		/* Check if it's a group boundary */
		if (group_start >= 0 &&
			(i == n ||
			 matches[i].indexnum != matches[group_start].indexnum ||
			 matches[i].colnum != matches[group_start].colnum ||
			 matches[i].opno != matches[group_start].opno ||
			 matches[i].inputcollid != matches[group_start].inputcollid ||
			 matches[i].indexnum == -1))
		{
			/*
			 * One clause in group: add it "as is" to the upper-level OR.
			 */
			if (i - group_start == 1)
			{
				result = lappend(result,
								 list_nth(orargs,
										  matches[group_start].argindex));
			}
			else
			{
				/*
				 * Two or more clauses in a group: create a nested OR.
				 */
				List	   *args = NIL;
				List	   *rargs = NIL;
				RestrictInfo *subrinfo;
				int			j;

				Assert(i - group_start >= 2);

				/* Construct the list of nested OR arguments */
				for (j = group_start; j < i; j++)
				{
					Node	   *arg = list_nth(orargs, matches[j].argindex);

					rargs = lappend(rargs, arg);
					if (IsA(arg, RestrictInfo))
						args = lappend(args, ((RestrictInfo *) arg)->clause);
					else
						args = lappend(args, arg);
				}

				/* Construct the nested OR and wrap it with RestrictInfo */
				subrinfo = make_plain_restrictinfo(root,
												   make_orclause(args),
												   make_orclause(rargs),
												   rinfo->is_pushed_down,
												   rinfo->has_clone,
												   rinfo->is_clone,
												   rinfo->pseudoconstant,
												   rinfo->security_level,
												   rinfo->required_relids,
												   rinfo->incompatible_relids,
												   rinfo->outer_relids);
				result = lappend(result, subrinfo);
			}

			group_start = i;
		}
	}
	pfree(matches);
	return result;
}

/*
 * make_bitmap_paths_for_or_group
 *		Generate bitmap paths for a group of similar OR-clause arguments
 *		produced by group_similar_or_args().
 *
 * This function considers two cases: (1) matching a group of clauses to
 * the index as a whole, and (2) matching the individual clauses one-by-one.
 * (1) typically comprises an optimal solution.  If not, (2) typically
 * comprises fair alternative.
 *
 * Ideally, we could consider all arbitrary splits of arguments into
 * subgroups, but that could lead to unacceptable computational complexity.
 * This is why we only consider two cases of above.
 */
static List *
make_bitmap_paths_for_or_group(PlannerInfo *root, RelOptInfo *rel,
							   RestrictInfo *ri, List *other_clauses)
{
	List	   *jointlist = NIL;
	List	   *splitlist = NIL;
	ListCell   *lc;
	List	   *orargs;
	List	   *args = ((BoolExpr *) ri->orclause)->args;
	Cost		jointcost = 0.0,
				splitcost = 0.0;
	Path	   *bitmapqual;
	List	   *indlist;

	/*
	 * First, try to match the whole group to the one index.
	 */
	orargs = list_make1(ri);
	indlist = build_paths_for_OR(root, rel,
								 orargs,
								 other_clauses);
	if (indlist != NIL)
	{
		bitmapqual = choose_bitmap_and(root, rel, indlist);
		jointcost = bitmapqual->total_cost;
		jointlist = list_make1(bitmapqual);
	}

	/*
	 * If we manage to find a bitmap scan, which uses the group of OR-clause
	 * arguments as a whole, we can skip matching OR-clause arguments
	 * one-by-one as long as there are no other clauses, which can bring more
	 * efficiency to one-by-one case.
	 */
	if (jointlist != NIL && other_clauses == NIL)
		return jointlist;

	/*
	 * Also try to match all containing clauses one-by-one.
	 */
	foreach(lc, args)
	{
		orargs = list_make1(lfirst(lc));

		indlist = build_paths_for_OR(root, rel,
									 orargs,
									 other_clauses);

		if (indlist == NIL)
		{
			splitlist = NIL;
			break;
		}

		bitmapqual = choose_bitmap_and(root, rel, indlist);
		splitcost += bitmapqual->total_cost;
		splitlist = lappend(splitlist, bitmapqual);
	}

	/*
	 * Pick the best option.
	 */
	if (splitlist == NIL)
		return jointlist;
	else if (jointlist == NIL)
		return splitlist;
	else
		return (jointcost < splitcost) ? jointlist : splitlist;
}


/*
 * generate_bitmap_or_paths
 *		Look through the list of clauses to find OR clauses, and generate
 *		a BitmapOrPath for each one we can handle that way.  Return a list
 *		of the generated BitmapOrPaths.
 *
 * other_clauses is a list of additional clauses that can be assumed true
 * for the purpose of generating indexquals, but are not to be searched for
 * ORs.  (See build_paths_for_OR() for motivation.)
 */
static List *
generate_bitmap_or_paths(PlannerInfo *root, RelOptInfo *rel,
						 List *clauses, List *other_clauses)
{
	List	   *result = NIL;
	List	   *all_clauses;
	ListCell   *lc;

	/*
	 * We can use both the current and other clauses as context for
	 * build_paths_for_OR; no need to remove ORs from the lists.
	 */
	all_clauses = list_concat_copy(clauses, other_clauses);

	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		List	   *pathlist;
		Path	   *bitmapqual;
		ListCell   *j;
		List	   *groupedArgs;
		List	   *inner_other_clauses = NIL;

		/* Ignore RestrictInfos that aren't ORs */
		if (!restriction_is_or_clause(rinfo))
			continue;

		/*
		 * We must be able to match at least one index to each of the arms of
		 * the OR, else we can't use it.
		 */
		pathlist = NIL;

		/*
		 * Group the similar OR-clause arguments into dedicated RestrictInfos,
		 * because each of those RestrictInfos has a chance to match the index
		 * as a whole.
		 */
		groupedArgs = group_similar_or_args(root, rel, rinfo);

		if (groupedArgs != ((BoolExpr *) rinfo->orclause)->args)
		{
			/*
			 * Some parts of the rinfo were probably grouped.  In this case,
			 * we have a set of sub-rinfos that together are an exact
			 * duplicate of rinfo.  Thus, we need to remove the rinfo from
			 * other clauses. match_clauses_to_index detects duplicated
			 * iclauses by comparing pointers to original rinfos that would be
			 * different.  So, we must delete rinfo to avoid de-facto
			 * duplicated clauses in the index clauses list.
			 */
			inner_other_clauses = list_delete(list_copy(all_clauses), rinfo);
		}

		foreach(j, groupedArgs)
		{
			Node	   *orarg = (Node *) lfirst(j);
			List	   *indlist;

			/* OR arguments should be ANDs or sub-RestrictInfos */
			if (is_andclause(orarg))
			{
				List	   *andargs = ((BoolExpr *) orarg)->args;

				indlist = build_paths_for_OR(root, rel,
											 andargs,
											 all_clauses);

				/* Recurse in case there are sub-ORs */
				indlist = list_concat(indlist,
									  generate_bitmap_or_paths(root, rel,
															   andargs,
															   all_clauses));
			}
			else if (restriction_is_or_clause(castNode(RestrictInfo, orarg)))
			{
				RestrictInfo *ri = castNode(RestrictInfo, orarg);

				/*
				 * Generate bitmap paths for the group of similar OR-clause
				 * arguments.
				 */
				indlist = make_bitmap_paths_for_or_group(root,
														 rel, ri,
														 inner_other_clauses);

				if (indlist == NIL)
				{
					pathlist = NIL;
					break;
				}
				else
				{
					pathlist = list_concat(pathlist, indlist);
					continue;
				}
			}
			else
			{
				RestrictInfo *ri = castNode(RestrictInfo, orarg);
				List	   *orargs;

				orargs = list_make1(ri);

				indlist = build_paths_for_OR(root, rel,
											 orargs,
											 all_clauses);
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

		if (inner_other_clauses != NIL)
			list_free(inner_other_clauses);

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
 * given path set.  We want to choose a good tradeoff between selectivity
 * and cost of computing the bitmap.
 *
 * The result is either a single one of the inputs, or a BitmapAndPath
 * combining multiple inputs.
 */
static Path *
choose_bitmap_and(PlannerInfo *root, RelOptInfo *rel, List *paths)
{
	int			npaths = list_length(paths);
	PathClauseUsage **pathinfoarray;
	PathClauseUsage *pathinfo;
	List	   *clauselist;
	List	   *bestpaths = NIL;
	Cost		bestcost = 0;
	int			i,
				j;
	ListCell   *l;

	Assert(npaths > 0);			/* else caller error */
	if (npaths == 1)
		return (Path *) linitial(paths);	/* easy case */

	/*
	 * In theory we should consider every nonempty subset of the given paths.
	 * In practice that seems like overkill, given the crude nature of the
	 * estimates, not to mention the possible effects of higher-level AND and
	 * OR clauses.  Moreover, it's completely impractical if there are a large
	 * number of paths, since the work would grow as O(2^N).
	 *
	 * As a heuristic, we first check for paths using exactly the same sets of
	 * WHERE clauses + index predicate conditions, and reject all but the
	 * cheapest-to-scan in any such group.  This primarily gets rid of indexes
	 * that include the interesting columns but also irrelevant columns.  (In
	 * situations where the DBA has gone overboard on creating variant
	 * indexes, this can make for a very large reduction in the number of
	 * paths considered further.)
	 *
	 * We then sort the surviving paths with the cheapest-to-scan first, and
	 * for each path, consider using that path alone as the basis for a bitmap
	 * scan.  Then we consider bitmap AND scans formed from that path plus
	 * each subsequent (higher-cost) path, adding on a subsequent path if it
	 * results in a reduction in the estimated total scan cost. This means we
	 * consider about O(N^2) rather than O(2^N) path combinations, which is
	 * quite tolerable, especially given than N is usually reasonably small
	 * because of the prefiltering step.  The cheapest of these is returned.
	 *
	 * We will only consider AND combinations in which no two indexes use the
	 * same WHERE clause.  This is a bit of a kluge: it's needed because
	 * costsize.c and clausesel.c aren't very smart about redundant clauses.
	 * They will usually double-count the redundant clauses, producing a
	 * too-small selectivity that makes a redundant AND step look like it
	 * reduces the total cost.  Perhaps someday that code will be smarter and
	 * we can remove this limitation.  (But note that this also defends
	 * against flat-out duplicate input paths, which can happen because
	 * match_join_clauses_to_index will find the same OR join clauses that
	 * extract_restriction_or_clauses has pulled OR restriction clauses out
	 * of.)
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
	 * Extract clause usage info and detect any paths that use exactly the
	 * same set of clauses; keep only the cheapest-to-scan of any such groups.
	 * The surviving paths are put into an array for qsort'ing.
	 */
	pathinfoarray = (PathClauseUsage **)
		palloc(npaths * sizeof(PathClauseUsage *));
	clauselist = NIL;
	npaths = 0;
	foreach(l, paths)
	{
		Path	   *ipath = (Path *) lfirst(l);

		pathinfo = classify_index_clause_usage(ipath, &clauselist);

		/* If it's unclassifiable, treat it as distinct from all others */
		if (pathinfo->unclassifiable)
		{
			pathinfoarray[npaths++] = pathinfo;
			continue;
		}

		for (i = 0; i < npaths; i++)
		{
			if (!pathinfoarray[i]->unclassifiable &&
				bms_equal(pathinfo->clauseids, pathinfoarray[i]->clauseids))
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
	 * For each surviving index, consider it as an "AND group leader", and see
	 * whether adding on any of the later indexes results in an AND path with
	 * cheaper total cost than before.  Then take the cheapest AND group.
	 *
	 * Note: paths that are either clauseless or unclassifiable will have
	 * empty clauseids, so that they will not be rejected by the clauseids
	 * filter here, nor will they cause later paths to be rejected by it.
	 */
	for (i = 0; i < npaths; i++)
	{
		Cost		costsofar;
		List	   *qualsofar;
		Bitmapset  *clauseidsofar;

		pathinfo = pathinfoarray[i];
		paths = list_make1(pathinfo->path);
		costsofar = bitmap_scan_cost_est(root, rel, pathinfo->path);
		qualsofar = list_concat_copy(pathinfo->quals, pathinfo->preds);
		clauseidsofar = bms_copy(pathinfo->clauseids);

		for (j = i + 1; j < npaths; j++)
		{
			Cost		newcost;

			pathinfo = pathinfoarray[j];
			/* Check for redundancy */
			if (bms_overlap(pathinfo->clauseids, clauseidsofar))
				continue;		/* consider it redundant */
			if (pathinfo->preds)
			{
				bool		redundant = false;

				/* we check each predicate clause separately */
				foreach(l, pathinfo->preds)
				{
					Node	   *np = (Node *) lfirst(l);

					if (predicate_implied_by(list_make1(np), qualsofar, false))
					{
						redundant = true;
						break;	/* out of inner foreach loop */
					}
				}
				if (redundant)
					continue;
			}
			/* tentatively add new path to paths, so we can estimate cost */
			paths = lappend(paths, pathinfo->path);
			newcost = bitmap_and_cost_est(root, rel, paths);
			if (newcost < costsofar)
			{
				/* keep new path in paths, update subsidiary variables */
				costsofar = newcost;
				qualsofar = list_concat(qualsofar, pathinfo->quals);
				qualsofar = list_concat(qualsofar, pathinfo->preds);
				clauseidsofar = bms_add_members(clauseidsofar,
												pathinfo->clauseids);
			}
			else
			{
				/* reject new path, remove it from paths list */
				paths = list_truncate(paths, list_length(paths) - 1);
			}
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
		return (Path *) linitial(bestpaths);	/* no need for AND */
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
 * index path (which could be a BitmapAnd or BitmapOr node).
 */
static Cost
bitmap_scan_cost_est(PlannerInfo *root, RelOptInfo *rel, Path *ipath)
{
	BitmapHeapPath bpath;

	/* Set up a dummy BitmapHeapPath */
	bpath.path.type = T_BitmapHeapPath;
	bpath.path.pathtype = T_BitmapHeapScan;
	bpath.path.parent = rel;
	bpath.path.pathtarget = rel->reltarget;
	bpath.path.param_info = ipath->param_info;
	bpath.path.pathkeys = NIL;
	bpath.bitmapqual = ipath;

	/*
	 * Check the cost of temporary path without considering parallelism.
	 * Parallel bitmap heap path will be considered at later stage.
	 */
	bpath.path.parallel_workers = 0;

	/* Now we can do cost_bitmap_heap_scan */
	cost_bitmap_heap_scan(&bpath.path, root, rel,
						  bpath.path.param_info,
						  ipath,
						  get_loop_count(root, rel->relid,
										 PATH_REQ_OUTER(ipath)));

	return bpath.path.total_cost;
}

/*
 * Estimate the cost of actually executing a BitmapAnd scan with the given
 * inputs.
 */
static Cost
bitmap_and_cost_est(PlannerInfo *root, RelOptInfo *rel, List *paths)
{
	BitmapAndPath *apath;

	/*
	 * Might as well build a real BitmapAndPath here, as the work is slightly
	 * too complicated to be worth repeating just to save one palloc.
	 */
	apath = create_bitmap_and_path(root, rel, paths);

	return bitmap_scan_cost_est(root, rel, (Path *) apath);
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

	/*
	 * Some machine-generated queries have outlandish numbers of qual clauses.
	 * To avoid getting into O(N^2) behavior even in this preliminary
	 * classification step, we want to limit the number of entries we can
	 * accumulate in *clauselist.  Treat any path with more than 100 quals +
	 * preds as unclassifiable, which will cause calling code to consider it
	 * distinct from all other paths.
	 */
	if (list_length(result->quals) + list_length(result->preds) > 100)
	{
		result->clauseids = NULL;
		result->unclassifiable = true;
		return result;
	}

	/* Build up a bitmapset representing the quals and preds */
	clauseids = NULL;
	foreach(lc, result->quals)
	{
		Node	   *node = (Node *) lfirst(lc);

		clauseids = bms_add_member(clauseids,
								   find_list_position(node, clauselist));
	}
	foreach(lc, result->preds)
	{
		Node	   *node = (Node *) lfirst(lc);

		clauseids = bms_add_member(clauseids,
								   find_list_position(node, clauselist));
	}
	result->clauseids = clauseids;
	result->unclassifiable = false;

	return result;
}


/*
 * find_indexpath_quals
 *
 * Given the Path structure for a plain or bitmap indexscan, extract lists
 * of all the index clauses and index predicate conditions used in the Path.
 * These are appended to the initial contents of *quals and *preds (hence
 * caller should initialize those to NIL).
 *
 * Note we are not trying to produce an accurate representation of the AND/OR
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
		ListCell   *l;

		foreach(l, ipath->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(l);

			*quals = lappend(*quals, iclause->rinfo->clause);
		}
		*preds = list_concat(*preds, ipath->indexinfo->indpred);
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
		Node	   *oldnode = (Node *) lfirst(lc);

		if (equal(node, oldnode))
			return i;
		i++;
	}

	*nodelist = lappend(*nodelist, node);

	return i;
}


/*
 * check_index_only
 *		Determine whether an index-only scan is possible for this index.
 */
static bool
check_index_only(RelOptInfo *rel, IndexOptInfo *index)
{
	bool		result;
	Bitmapset  *attrs_used = NULL;
	Bitmapset  *index_canreturn_attrs = NULL;
	ListCell   *lc;
	int			i;

	/* Index-only scans must be enabled */
	if (!enable_indexonlyscan)
		return false;

	/*
	 * Check that all needed attributes of the relation are available from the
	 * index.
	 */

	/*
	 * First, identify all the attributes needed for joins or final output.
	 * Note: we must look at rel's targetlist, not the attr_needed data,
	 * because attr_needed isn't computed for inheritance child rels.
	 */
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs_used);

	/*
	 * Add all the attributes used by restriction clauses; but consider only
	 * those clauses not implied by the index predicate, since ones that are
	 * so implied don't need to be checked explicitly in the plan.
	 *
	 * Note: attributes used only in index quals would not be needed at
	 * runtime either, if we are certain that the index is not lossy.  However
	 * it'd be complicated to account for that accurately, and it doesn't
	 * matter in most cases, since we'd conclude that such attributes are
	 * available from the index anyway.
	 */
	foreach(lc, index->indrestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, rel->relid, &attrs_used);
	}

	/*
	 * Construct a bitmapset of columns that the index can return back in an
	 * index-only scan.
	 */
	for (i = 0; i < index->ncolumns; i++)
	{
		int			attno = index->indexkeys[i];

		/*
		 * For the moment, we just ignore index expressions.  It might be nice
		 * to do something with them, later.
		 */
		if (attno == 0)
			continue;

		if (index->canreturn[i])
			index_canreturn_attrs =
				bms_add_member(index_canreturn_attrs,
							   attno - FirstLowInvalidHeapAttributeNumber);
	}

	/* Do we have all the necessary attributes? */
	result = bms_is_subset(attrs_used, index_canreturn_attrs);

	bms_free(attrs_used);
	bms_free(index_canreturn_attrs);

	return result;
}

/*
 * get_loop_count
 *		Choose the loop count estimate to use for costing a parameterized path
 *		with the given set of outer relids.
 *
 * Since we produce parameterized paths before we've begun to generate join
 * relations, it's impossible to predict exactly how many times a parameterized
 * path will be iterated; we don't know the size of the relation that will be
 * on the outside of the nestloop.  However, we should try to account for
 * multiple iterations somehow in costing the path.  The heuristic embodied
 * here is to use the rowcount of the smallest other base relation needed in
 * the join clauses used by the path.  (We could alternatively consider the
 * largest one, but that seems too optimistic.)  This is of course the right
 * answer for single-other-relation cases, and it seems like a reasonable
 * zero-order approximation for multiway-join cases.
 *
 * In addition, we check to see if the other side of each join clause is on
 * the inside of some semijoin that the current relation is on the outside of.
 * If so, the only way that a parameterized path could be used is if the
 * semijoin RHS has been unique-ified, so we should use the number of unique
 * RHS rows rather than using the relation's raw rowcount.
 *
 * Note: for this to work, allpaths.c must establish all baserel size
 * estimates before it begins to compute paths, or at least before it
 * calls create_index_paths().
 */
static double
get_loop_count(PlannerInfo *root, Index cur_relid, Relids outer_relids)
{
	double		result;
	int			outer_relid;

	/* For a non-parameterized path, just return 1.0 quickly */
	if (outer_relids == NULL)
		return 1.0;

	result = 0.0;
	outer_relid = -1;
	while ((outer_relid = bms_next_member(outer_relids, outer_relid)) >= 0)
	{
		RelOptInfo *outer_rel;
		double		rowcount;

		/* Paranoia: ignore bogus relid indexes */
		if (outer_relid >= root->simple_rel_array_size)
			continue;
		outer_rel = root->simple_rel_array[outer_relid];
		if (outer_rel == NULL)
			continue;
		Assert(outer_rel->relid == outer_relid);	/* sanity check on array */

		/* Other relation could be proven empty, if so ignore */
		if (IS_DUMMY_REL(outer_rel))
			continue;

		/* Otherwise, rel's rows estimate should be valid by now */
		Assert(outer_rel->rows > 0);

		/* Check to see if rel is on the inside of any semijoins */
		rowcount = adjust_rowcount_for_semijoins(root,
												 cur_relid,
												 outer_relid,
												 outer_rel->rows);

		/* Remember smallest row count estimate among the outer rels */
		if (result == 0.0 || result > rowcount)
			result = rowcount;
	}
	/* Return 1.0 if we found no valid relations (shouldn't happen) */
	return (result > 0.0) ? result : 1.0;
}

/*
 * Check to see if outer_relid is on the inside of any semijoin that cur_relid
 * is on the outside of.  If so, replace rowcount with the estimated number of
 * unique rows from the semijoin RHS (assuming that's smaller, which it might
 * not be).  The estimate is crude but it's the best we can do at this stage
 * of the proceedings.
 */
static double
adjust_rowcount_for_semijoins(PlannerInfo *root,
							  Index cur_relid,
							  Index outer_relid,
							  double rowcount)
{
	ListCell   *lc;

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->jointype == JOIN_SEMI &&
			bms_is_member(cur_relid, sjinfo->syn_lefthand) &&
			bms_is_member(outer_relid, sjinfo->syn_righthand))
		{
			/* Estimate number of unique-ified rows */
			double		nraw;
			double		nunique;

			nraw = approximate_joinrel_size(root, sjinfo->syn_righthand);
			nunique = estimate_num_groups(root,
										  sjinfo->semi_rhs_exprs,
										  nraw,
										  NULL,
										  NULL);
			if (rowcount > nunique)
				rowcount = nunique;
		}
	}
	return rowcount;
}

/*
 * Make an approximate estimate of the size of a joinrel.
 *
 * We don't have enough info at this point to get a good estimate, so we
 * just multiply the base relation sizes together.  Fortunately, this is
 * the right answer anyway for the most common case with a single relation
 * on the RHS of a semijoin.  Also, estimate_num_groups() has only a weak
 * dependency on its input_rows argument (it basically uses it as a clamp).
 * So we might be able to get a fairly decent end result even with a severe
 * overestimate of the RHS's raw size.
 */
static double
approximate_joinrel_size(PlannerInfo *root, Relids relids)
{
	double		rowcount = 1.0;
	int			relid;

	relid = -1;
	while ((relid = bms_next_member(relids, relid)) >= 0)
	{
		RelOptInfo *rel;

		/* Paranoia: ignore bogus relid indexes */
		if (relid >= root->simple_rel_array_size)
			continue;
		rel = root->simple_rel_array[relid];
		if (rel == NULL)
			continue;
		Assert(rel->relid == relid);	/* sanity check on array */

		/* Relation could be proven empty, if so ignore */
		if (IS_DUMMY_REL(rel))
			continue;

		/* Otherwise, rel's rows estimate should be valid by now */
		Assert(rel->rows > 0);

		/* Accumulate product */
		rowcount *= rel->rows;
	}
	return rowcount;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK QUERY CLAUSES  ----
 ****************************************************************************/

/*
 * match_restriction_clauses_to_index
 *	  Identify restriction clauses for the rel that match the index.
 *	  Matching clauses are added to *clauseset.
 */
static void
match_restriction_clauses_to_index(PlannerInfo *root,
								   IndexOptInfo *index,
								   IndexClauseSet *clauseset)
{
	/* We can ignore clauses that are implied by the index predicate */
	match_clauses_to_index(root, index->indrestrictinfo, index, clauseset);
}

/*
 * match_join_clauses_to_index
 *	  Identify join clauses for the rel that match the index.
 *	  Matching clauses are added to *clauseset.
 *	  Also, add any potentially usable join OR clauses to *joinorclauses.
 *	  They also might be processed by match_clause_to_index() as a whole.
 */
static void
match_join_clauses_to_index(PlannerInfo *root,
							RelOptInfo *rel, IndexOptInfo *index,
							IndexClauseSet *clauseset,
							List **joinorclauses)
{
	ListCell   *lc;

	/* Scan the rel's join clauses */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/* Check if clause can be moved to this rel */
		if (!join_clause_is_movable_to(rinfo, rel))
			continue;

		/*
		 * Potentially usable, so see if it matches the index or is an OR. Use
		 * list_append_unique_ptr() here to avoid possible duplicates when
		 * processing the same clauses with different indexes.
		 */
		if (restriction_is_or_clause(rinfo))
			*joinorclauses = list_append_unique_ptr(*joinorclauses, rinfo);

		match_clause_to_index(root, rinfo, index, clauseset);
	}
}

/*
 * match_eclass_clauses_to_index
 *	  Identify EquivalenceClass join clauses for the rel that match the index.
 *	  Matching clauses are added to *clauseset.
 */
static void
match_eclass_clauses_to_index(PlannerInfo *root, IndexOptInfo *index,
							  IndexClauseSet *clauseset)
{
	int			indexcol;

	/* No work if rel is not in any such ECs */
	if (!index->rel->has_eclass_joins)
		return;

	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		ec_member_matches_arg arg;
		List	   *clauses;

		/* Generate clauses, skipping any that join to lateral_referencers */
		arg.index = index;
		arg.indexcol = indexcol;
		clauses = generate_implied_equalities_for_column(root,
														 index->rel,
														 ec_member_matches_indexcol,
														 &arg,
														 index->rel->lateral_referencers);

		/*
		 * We have to check whether the results actually do match the index,
		 * since for non-btree indexes the EC's equality operators might not
		 * be in the index opclass (cf ec_member_matches_indexcol).
		 */
		match_clauses_to_index(root, clauses, index, clauseset);
	}
}

/*
 * match_clauses_to_index
 *	  Perform match_clause_to_index() for each clause in a list.
 *	  Matching clauses are added to *clauseset.
 */
static void
match_clauses_to_index(PlannerInfo *root,
					   List *clauses,
					   IndexOptInfo *index,
					   IndexClauseSet *clauseset)
{
	ListCell   *lc;

	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		match_clause_to_index(root, rinfo, index, clauseset);
	}
}

/*
 * match_clause_to_index
 *	  Test whether a qual clause can be used with an index.
 *
 * If the clause is usable, add an IndexClause entry for it to the appropriate
 * list in *clauseset.  (*clauseset must be initialized to zeroes before first
 * call.)
 *
 * Note: in some circumstances we may find the same RestrictInfos coming from
 * multiple places.  Defend against redundant outputs by refusing to add a
 * clause twice (pointer equality should be a good enough check for this).
 *
 * Note: it's possible that a badly-defined index could have multiple matching
 * columns.  We always select the first match if so; this avoids scenarios
 * wherein we get an inflated idea of the index's selectivity by using the
 * same clause multiple times with different index columns.
 */
static void
match_clause_to_index(PlannerInfo *root,
					  RestrictInfo *rinfo,
					  IndexOptInfo *index,
					  IndexClauseSet *clauseset)
{
	int			indexcol;

	/*
	 * Never match pseudoconstants to indexes.  (Normally a match could not
	 * happen anyway, since a pseudoconstant clause couldn't contain a Var,
	 * but what if someone builds an expression index on a constant? It's not
	 * totally unreasonable to do so with a partial index, either.)
	 */
	if (rinfo->pseudoconstant)
		return;

	/*
	 * If clause can't be used as an indexqual because it must wait till after
	 * some lower-security-level restriction clause, reject it.
	 */
	if (!restriction_is_securely_promotable(rinfo, index->rel))
		return;

	/* OK, check each index key column for a match */
	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		IndexClause *iclause;
		ListCell   *lc;

		/* Ignore duplicates */
		foreach(lc, clauseset->indexclauses[indexcol])
		{
			iclause = (IndexClause *) lfirst(lc);

			if (iclause->rinfo == rinfo)
				return;
		}

		/* OK, try to match the clause to the index column */
		iclause = match_clause_to_indexcol(root,
										   rinfo,
										   indexcol,
										   index);
		if (iclause)
		{
			/* Success, so record it */
			clauseset->indexclauses[indexcol] =
				lappend(clauseset->indexclauses[indexcol], iclause);
			clauseset->nonempty = true;
			return;
		}
	}
}

/*
 * match_clause_to_indexcol()
 *	  Determine whether a restriction clause matches a column of an index,
 *	  and if so, build an IndexClause node describing the details.
 *
 *	  To match an index normally, an operator clause:
 *
 *	  (1)  must be in the form (indexkey op const) or (const op indexkey);
 *		   and
 *	  (2)  must contain an operator which is in the index's operator family
 *		   for this column; and
 *	  (3)  must match the collation of the index, if collation is relevant.
 *
 *	  Our definition of "const" is exceedingly liberal: we allow anything that
 *	  doesn't involve a volatile function or a Var of the index's relation.
 *	  In particular, Vars belonging to other relations of the query are
 *	  accepted here, since a clause of that form can be used in a
 *	  parameterized indexscan.  It's the responsibility of higher code levels
 *	  to manage restriction and join clauses appropriately.
 *
 *	  Note: we do need to check for Vars of the index's relation on the
 *	  "const" side of the clause, since clauses like (a.f1 OP (b.f2 OP a.f3))
 *	  are not processable by a parameterized indexscan on a.f1, whereas
 *	  something like (a.f1 OP (b.f2 OP c.f3)) is.
 *
 *	  Presently, the executor can only deal with indexquals that have the
 *	  indexkey on the left, so we can only use clauses that have the indexkey
 *	  on the right if we can commute the clause to put the key on the left.
 *	  We handle that by generating an IndexClause with the correctly-commuted
 *	  opclause as a derived indexqual.
 *
 *	  If the index has a collation, the clause must have the same collation.
 *	  For collation-less indexes, we assume it doesn't matter; this is
 *	  necessary for cases like "hstore ? text", wherein hstore's operators
 *	  don't care about collation but the clause will get marked with a
 *	  collation anyway because of the text argument.  (This logic is
 *	  embodied in the macro IndexCollMatchesExprColl.)
 *
 *	  It is also possible to match RowCompareExpr clauses to indexes (but
 *	  currently, only btree indexes handle this).
 *
 *	  It is also possible to match ScalarArrayOpExpr clauses to indexes, when
 *	  the clause is of the form "indexkey op ANY (arrayconst)".
 *
 *	  It is also possible to match a list of OR clauses if it might be
 *	  transformed into a single ScalarArrayOpExpr clause.  On success,
 *	  the returning index clause will contain a transformed clause.
 *
 *	  For boolean indexes, it is also possible to match the clause directly
 *	  to the indexkey; or perhaps the clause is (NOT indexkey).
 *
 *	  And, last but not least, some operators and functions can be processed
 *	  to derive (typically lossy) indexquals from a clause that isn't in
 *	  itself indexable.  If we see that any operand of an OpExpr or FuncExpr
 *	  matches the index key, and the function has a planner support function
 *	  attached to it, we'll invoke the support function to see if such an
 *	  indexqual can be built.
 *
 * 'rinfo' is the clause to be tested (as a RestrictInfo node).
 * 'indexcol' is a column number of 'index' (counting from 0).
 * 'index' is the index of interest.
 *
 * Returns an IndexClause if the clause can be used with this index key,
 * or NULL if not.
 *
 * NOTE:  This routine always returns NULL if the clause is an AND clause.
 * Higher-level routines deal with OR and AND clauses. OR clause can be
 * matched as a whole by match_orclause_to_indexcol() though.
 */
static IndexClause *
match_clause_to_indexcol(PlannerInfo *root,
						 RestrictInfo *rinfo,
						 int indexcol,
						 IndexOptInfo *index)
{
	IndexClause *iclause;
	Expr	   *clause = rinfo->clause;
	Oid			opfamily;

	Assert(indexcol < index->nkeycolumns);

	/*
	 * Historically this code has coped with NULL clauses.  That's probably
	 * not possible anymore, but we might as well continue to cope.
	 */
	if (clause == NULL)
		return NULL;

	/* First check for boolean-index cases. */
	opfamily = index->opfamily[indexcol];
	if (IsBooleanOpfamily(opfamily))
	{
		iclause = match_boolean_index_clause(root, rinfo, indexcol, index);
		if (iclause)
			return iclause;
	}

	/*
	 * Clause must be an opclause, funcclause, ScalarArrayOpExpr,
	 * RowCompareExpr, or OR-clause that could be converted to SAOP.  Or, if
	 * the index supports it, we can handle IS NULL/NOT NULL clauses.
	 */
	if (IsA(clause, OpExpr))
	{
		return match_opclause_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (IsA(clause, FuncExpr))
	{
		return match_funcclause_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		return match_saopclause_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		return match_rowcompare_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (restriction_is_or_clause(rinfo))
	{
		return match_orclause_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (index->amsearchnulls && IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		if (!nt->argisrow &&
			match_index_to_operand((Node *) nt->arg, indexcol, index))
		{
			iclause = makeNode(IndexClause);
			iclause->rinfo = rinfo;
			iclause->indexquals = list_make1(rinfo);
			iclause->lossy = false;
			iclause->indexcol = indexcol;
			iclause->indexcols = NIL;
			return iclause;
		}
	}

	return NULL;
}

/*
 * IsBooleanOpfamily
 *	  Detect whether an opfamily supports boolean equality as an operator.
 *
 * If the opfamily OID is in the range of built-in objects, we can rely
 * on hard-wired knowledge of which built-in opfamilies support this.
 * For extension opfamilies, there's no choice but to do a catcache lookup.
 */
static bool
IsBooleanOpfamily(Oid opfamily)
{
	if (opfamily < FirstNormalObjectId)
		return IsBuiltinBooleanOpfamily(opfamily);
	else
		return op_in_opfamily(BooleanEqualOperator, opfamily);
}

/*
 * match_boolean_index_clause
 *	  Recognize restriction clauses that can be matched to a boolean index.
 *
 * The idea here is that, for an index on a boolean column that supports the
 * BooleanEqualOperator, we can transform a plain reference to the indexkey
 * into "indexkey = true", or "NOT indexkey" into "indexkey = false", etc,
 * so as to make the expression indexable using the index's "=" operator.
 * Since Postgres 8.1, we must do this because constant simplification does
 * the reverse transformation; without this code there'd be no way to use
 * such an index at all.
 *
 * This should be called only when IsBooleanOpfamily() recognizes the
 * index's operator family.  We check to see if the clause matches the
 * index's key, and if so, build a suitable IndexClause.
 */
static IndexClause *
match_boolean_index_clause(PlannerInfo *root,
						   RestrictInfo *rinfo,
						   int indexcol,
						   IndexOptInfo *index)
{
	Node	   *clause = (Node *) rinfo->clause;
	Expr	   *op = NULL;

	/* Direct match? */
	if (match_index_to_operand(clause, indexcol, index))
	{
		/* convert to indexkey = TRUE */
		op = make_opclause(BooleanEqualOperator, BOOLOID, false,
						   (Expr *) clause,
						   (Expr *) makeBoolConst(true, false),
						   InvalidOid, InvalidOid);
	}
	/* NOT clause? */
	else if (is_notclause(clause))
	{
		Node	   *arg = (Node *) get_notclausearg((Expr *) clause);

		if (match_index_to_operand(arg, indexcol, index))
		{
			/* convert to indexkey = FALSE */
			op = make_opclause(BooleanEqualOperator, BOOLOID, false,
							   (Expr *) arg,
							   (Expr *) makeBoolConst(false, false),
							   InvalidOid, InvalidOid);
		}
	}

	/*
	 * Since we only consider clauses at top level of WHERE, we can convert
	 * indexkey IS TRUE and indexkey IS FALSE to index searches as well.  The
	 * different meaning for NULL isn't important.
	 */
	else if (clause && IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;
		Node	   *arg = (Node *) btest->arg;

		if (btest->booltesttype == IS_TRUE &&
			match_index_to_operand(arg, indexcol, index))
		{
			/* convert to indexkey = TRUE */
			op = make_opclause(BooleanEqualOperator, BOOLOID, false,
							   (Expr *) arg,
							   (Expr *) makeBoolConst(true, false),
							   InvalidOid, InvalidOid);
		}
		else if (btest->booltesttype == IS_FALSE &&
				 match_index_to_operand(arg, indexcol, index))
		{
			/* convert to indexkey = FALSE */
			op = make_opclause(BooleanEqualOperator, BOOLOID, false,
							   (Expr *) arg,
							   (Expr *) makeBoolConst(false, false),
							   InvalidOid, InvalidOid);
		}
	}

	/*
	 * If we successfully made an operator clause from the given qual, we must
	 * wrap it in an IndexClause.  It's not lossy.
	 */
	if (op)
	{
		IndexClause *iclause = makeNode(IndexClause);

		iclause->rinfo = rinfo;
		iclause->indexquals = list_make1(make_simple_restrictinfo(root, op));
		iclause->lossy = false;
		iclause->indexcol = indexcol;
		iclause->indexcols = NIL;
		return iclause;
	}

	return NULL;
}

/*
 * match_opclause_to_indexcol()
 *	  Handles the OpExpr case for match_clause_to_indexcol(),
 *	  which see for comments.
 */
static IndexClause *
match_opclause_to_indexcol(PlannerInfo *root,
						   RestrictInfo *rinfo,
						   int indexcol,
						   IndexOptInfo *index)
{
	IndexClause *iclause;
	OpExpr	   *clause = (OpExpr *) rinfo->clause;
	Node	   *leftop,
			   *rightop;
	Oid			expr_op;
	Oid			expr_coll;
	Index		index_relid;
	Oid			opfamily;
	Oid			idxcollation;

	/*
	 * Only binary operators need apply.  (In theory, a planner support
	 * function could do something with a unary operator, but it seems
	 * unlikely to be worth the cycles to check.)
	 */
	if (list_length(clause->args) != 2)
		return NULL;

	leftop = (Node *) linitial(clause->args);
	rightop = (Node *) lsecond(clause->args);
	expr_op = clause->opno;
	expr_coll = clause->inputcollid;

	index_relid = index->rel->relid;
	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * Check for clauses of the form: (indexkey operator constant) or
	 * (constant operator indexkey).  See match_clause_to_indexcol's notes
	 * about const-ness.
	 *
	 * Note that we don't ask the support function about clauses that don't
	 * have one of these forms.  Again, in principle it might be possible to
	 * do something, but it seems unlikely to be worth the cycles to check.
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, rinfo->right_relids) &&
		!contain_volatile_functions(rightop))
	{
		if (IndexCollMatchesExprColl(idxcollation, expr_coll) &&
			op_in_opfamily(expr_op, opfamily))
		{
			iclause = makeNode(IndexClause);
			iclause->rinfo = rinfo;
			iclause->indexquals = list_make1(rinfo);
			iclause->lossy = false;
			iclause->indexcol = indexcol;
			iclause->indexcols = NIL;
			return iclause;
		}

		/*
		 * If we didn't find a member of the index's opfamily, try the support
		 * function for the operator's underlying function.
		 */
		set_opfuncid(clause);	/* make sure we have opfuncid */
		return get_index_clause_from_support(root,
											 rinfo,
											 clause->opfuncid,
											 0, /* indexarg on left */
											 indexcol,
											 index);
	}

	if (match_index_to_operand(rightop, indexcol, index) &&
		!bms_is_member(index_relid, rinfo->left_relids) &&
		!contain_volatile_functions(leftop))
	{
		if (IndexCollMatchesExprColl(idxcollation, expr_coll))
		{
			Oid			comm_op = get_commutator(expr_op);

			if (OidIsValid(comm_op) &&
				op_in_opfamily(comm_op, opfamily))
			{
				RestrictInfo *commrinfo;

				/* Build a commuted OpExpr and RestrictInfo */
				commrinfo = commute_restrictinfo(rinfo, comm_op);

				/* Make an IndexClause showing that as a derived qual */
				iclause = makeNode(IndexClause);
				iclause->rinfo = rinfo;
				iclause->indexquals = list_make1(commrinfo);
				iclause->lossy = false;
				iclause->indexcol = indexcol;
				iclause->indexcols = NIL;
				return iclause;
			}
		}

		/*
		 * If we didn't find a member of the index's opfamily, try the support
		 * function for the operator's underlying function.
		 */
		set_opfuncid(clause);	/* make sure we have opfuncid */
		return get_index_clause_from_support(root,
											 rinfo,
											 clause->opfuncid,
											 1, /* indexarg on right */
											 indexcol,
											 index);
	}

	return NULL;
}

/*
 * match_funcclause_to_indexcol()
 *	  Handles the FuncExpr case for match_clause_to_indexcol(),
 *	  which see for comments.
 */
static IndexClause *
match_funcclause_to_indexcol(PlannerInfo *root,
							 RestrictInfo *rinfo,
							 int indexcol,
							 IndexOptInfo *index)
{
	FuncExpr   *clause = (FuncExpr *) rinfo->clause;
	int			indexarg;
	ListCell   *lc;

	/*
	 * We have no built-in intelligence about function clauses, but if there's
	 * a planner support function, it might be able to do something.  But, to
	 * cut down on wasted planning cycles, only call the support function if
	 * at least one argument matches the target index column.
	 *
	 * Note that we don't insist on the other arguments being pseudoconstants;
	 * the support function has to check that.  This is to allow cases where
	 * only some of the other arguments need to be included in the indexqual.
	 */
	indexarg = 0;
	foreach(lc, clause->args)
	{
		Node	   *op = (Node *) lfirst(lc);

		if (match_index_to_operand(op, indexcol, index))
		{
			return get_index_clause_from_support(root,
												 rinfo,
												 clause->funcid,
												 indexarg,
												 indexcol,
												 index);
		}

		indexarg++;
	}

	return NULL;
}

/*
 * get_index_clause_from_support()
 *		If the function has a planner support function, try to construct
 *		an IndexClause using indexquals created by the support function.
 */
static IndexClause *
get_index_clause_from_support(PlannerInfo *root,
							  RestrictInfo *rinfo,
							  Oid funcid,
							  int indexarg,
							  int indexcol,
							  IndexOptInfo *index)
{
	Oid			prosupport = get_func_support(funcid);
	SupportRequestIndexCondition req;
	List	   *sresult;

	if (!OidIsValid(prosupport))
		return NULL;

	req.type = T_SupportRequestIndexCondition;
	req.root = root;
	req.funcid = funcid;
	req.node = (Node *) rinfo->clause;
	req.indexarg = indexarg;
	req.index = index;
	req.indexcol = indexcol;
	req.opfamily = index->opfamily[indexcol];
	req.indexcollation = index->indexcollations[indexcol];

	req.lossy = true;			/* default assumption */

	sresult = (List *)
		DatumGetPointer(OidFunctionCall1(prosupport,
										 PointerGetDatum(&req)));

	if (sresult != NIL)
	{
		IndexClause *iclause = makeNode(IndexClause);
		List	   *indexquals = NIL;
		ListCell   *lc;

		/*
		 * The support function API says it should just give back bare
		 * clauses, so here we must wrap each one in a RestrictInfo.
		 */
		foreach(lc, sresult)
		{
			Expr	   *clause = (Expr *) lfirst(lc);

			indexquals = lappend(indexquals,
								 make_simple_restrictinfo(root, clause));
		}

		iclause->rinfo = rinfo;
		iclause->indexquals = indexquals;
		iclause->lossy = req.lossy;
		iclause->indexcol = indexcol;
		iclause->indexcols = NIL;

		return iclause;
	}

	return NULL;
}

/*
 * match_saopclause_to_indexcol()
 *	  Handles the ScalarArrayOpExpr case for match_clause_to_indexcol(),
 *	  which see for comments.
 */
static IndexClause *
match_saopclause_to_indexcol(PlannerInfo *root,
							 RestrictInfo *rinfo,
							 int indexcol,
							 IndexOptInfo *index)
{
	ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) rinfo->clause;
	Node	   *leftop,
			   *rightop;
	Relids		right_relids;
	Oid			expr_op;
	Oid			expr_coll;
	Index		index_relid;
	Oid			opfamily;
	Oid			idxcollation;

	/* We only accept ANY clauses, not ALL */
	if (!saop->useOr)
		return NULL;
	leftop = (Node *) linitial(saop->args);
	rightop = (Node *) lsecond(saop->args);
	right_relids = pull_varnos(root, rightop);
	expr_op = saop->opno;
	expr_coll = saop->inputcollid;

	index_relid = index->rel->relid;
	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * We must have indexkey on the left and a pseudo-constant array argument.
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, right_relids) &&
		!contain_volatile_functions(rightop))
	{
		if (IndexCollMatchesExprColl(idxcollation, expr_coll) &&
			op_in_opfamily(expr_op, opfamily))
		{
			IndexClause *iclause = makeNode(IndexClause);

			iclause->rinfo = rinfo;
			iclause->indexquals = list_make1(rinfo);
			iclause->lossy = false;
			iclause->indexcol = indexcol;
			iclause->indexcols = NIL;
			return iclause;
		}

		/*
		 * We do not currently ask support functions about ScalarArrayOpExprs,
		 * though in principle we could.
		 */
	}

	return NULL;
}

/*
 * match_rowcompare_to_indexcol()
 *	  Handles the RowCompareExpr case for match_clause_to_indexcol(),
 *	  which see for comments.
 *
 * In this routine we check whether the first column of the row comparison
 * matches the target index column.  This is sufficient to guarantee that some
 * index condition can be constructed from the RowCompareExpr --- the rest
 * is handled by expand_indexqual_rowcompare().
 */
static IndexClause *
match_rowcompare_to_indexcol(PlannerInfo *root,
							 RestrictInfo *rinfo,
							 int indexcol,
							 IndexOptInfo *index)
{
	RowCompareExpr *clause = (RowCompareExpr *) rinfo->clause;
	Index		index_relid;
	Oid			opfamily;
	Oid			idxcollation;
	Node	   *leftop,
			   *rightop;
	bool		var_on_left;
	Oid			expr_op;
	Oid			expr_coll;

	/* Forget it if we're not dealing with a btree index */
	if (index->relam != BTREE_AM_OID)
		return NULL;

	index_relid = index->rel->relid;
	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * We could do the matching on the basis of insisting that the opfamily
	 * shown in the RowCompareExpr be the same as the index column's opfamily,
	 * but that could fail in the presence of reverse-sort opfamilies: it'd be
	 * a matter of chance whether RowCompareExpr had picked the forward or
	 * reverse-sort family.  So look only at the operator, and match if it is
	 * a member of the index's opfamily (after commutation, if the indexkey is
	 * on the right).  We'll worry later about whether any additional
	 * operators are matchable to the index.
	 */
	leftop = (Node *) linitial(clause->largs);
	rightop = (Node *) linitial(clause->rargs);
	expr_op = linitial_oid(clause->opnos);
	expr_coll = linitial_oid(clause->inputcollids);

	/* Collations must match, if relevant */
	if (!IndexCollMatchesExprColl(idxcollation, expr_coll))
		return NULL;

	/*
	 * These syntactic tests are the same as in match_opclause_to_indexcol()
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, pull_varnos(root, rightop)) &&
		!contain_volatile_functions(rightop))
	{
		/* OK, indexkey is on left */
		var_on_left = true;
	}
	else if (match_index_to_operand(rightop, indexcol, index) &&
			 !bms_is_member(index_relid, pull_varnos(root, leftop)) &&
			 !contain_volatile_functions(leftop))
	{
		/* indexkey is on right, so commute the operator */
		expr_op = get_commutator(expr_op);
		if (expr_op == InvalidOid)
			return NULL;
		var_on_left = false;
	}
	else
		return NULL;

	/* We're good if the operator is the right type of opfamily member */
	switch (get_op_opfamily_strategy(expr_op, opfamily))
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			return expand_indexqual_rowcompare(root,
											   rinfo,
											   indexcol,
											   index,
											   expr_op,
											   var_on_left);
	}

	return NULL;
}

/*
 * match_orclause_to_indexcol()
 *	  Handles the OR-expr case for match_clause_to_indexcol() in the case
 *	  when it could be transformed to ScalarArrayOpExpr.
 *
 * In this routine, we attempt to transform a list of OR-clause args into a
 * single SAOP expression matching the target index column.  On success,
 * return an IndexClause, containing the transformed expression or NULL,
 * if failed.
 */
static IndexClause *
match_orclause_to_indexcol(PlannerInfo *root,
						   RestrictInfo *rinfo,
						   int indexcol,
						   IndexOptInfo *index)
{
	ListCell   *lc;
	BoolExpr   *orclause = (BoolExpr *) rinfo->orclause;
	Node	   *indexExpr = NULL;
	List	   *consts = NIL;
	ScalarArrayOpExpr *saopexpr = NULL;
	Oid			matchOpno = InvalidOid;
	IndexClause *iclause;
	Oid			consttype = InvalidOid;
	Oid			arraytype = InvalidOid;
	Oid			inputcollid = InvalidOid;
	bool		firstTime = true;
	bool		haveNonConst = false;
	Index		indexRelid = index->rel->relid;

	Assert(IsA(orclause, BoolExpr));
	Assert(orclause->boolop == OR_EXPR);

	/* Ignore index if it doesn't support SAOP clauses */
	if (!index->amsearcharray)
		return NULL;

	/*
	 * Try to convert a list of OR-clauses to a single SAOP expression. Each
	 * OR entry must be in the form: (indexkey operator constant) or (constant
	 * operator indexkey).  Operators of all the entries must match.  To be
	 * effective, give up on the first non-matching entry.  Exit is
	 * implemented as a break from the loop, which is catched afterwards.
	 */
	foreach(lc, orclause->args)
	{
		RestrictInfo *subRinfo;
		OpExpr	   *subClause;
		Oid			opno;
		Node	   *leftop,
				   *rightop;
		Node	   *constExpr;

		if (!IsA(lfirst(lc), RestrictInfo))
			break;

		subRinfo = (RestrictInfo *) lfirst(lc);

		/* Only operator clauses can match  */
		if (!IsA(subRinfo->clause, OpExpr))
			break;

		subClause = (OpExpr *) subRinfo->clause;
		opno = subClause->opno;

		/* Only binary operators can match  */
		if (list_length(subClause->args) != 2)
			break;

		/*
		 * The parameters below must match between sub-rinfo and its parent as
		 * make_restrictinfo() fills them with the same values, and further
		 * modifications are also the same for the whole subtree.  However,
		 * still make a sanity check.
		 */
		Assert(subRinfo->is_pushed_down == rinfo->is_pushed_down);
		Assert(subRinfo->is_clone == rinfo->is_clone);
		Assert(subRinfo->security_level == rinfo->security_level);
		Assert(bms_equal(subRinfo->incompatible_relids, rinfo->incompatible_relids));
		Assert(bms_equal(subRinfo->outer_relids, rinfo->outer_relids));

		/*
		 * Also, check that required_relids in sub-rinfo is subset of parent's
		 * required_relids.
		 */
		Assert(bms_is_subset(subRinfo->required_relids, rinfo->required_relids));

		/* Only the operator returning a boolean suit the transformation. */
		if (get_op_rettype(opno) != BOOLOID)
			break;

		/*
		 * Check for clauses of the form: (indexkey operator constant) or
		 * (constant operator indexkey).  See match_clause_to_indexcol's notes
		 * about const-ness.
		 */
		leftop = (Node *) linitial(subClause->args);
		rightop = (Node *) lsecond(subClause->args);
		if (match_index_to_operand(leftop, indexcol, index) &&
			!bms_is_member(indexRelid, subRinfo->right_relids) &&
			!contain_volatile_functions(rightop))
		{
			indexExpr = leftop;
			constExpr = rightop;
		}
		else if (match_index_to_operand(rightop, indexcol, index) &&
				 !bms_is_member(indexRelid, subRinfo->left_relids) &&
				 !contain_volatile_functions(leftop))
		{
			opno = get_commutator(opno);
			if (!OidIsValid(opno))
			{
				/* commutator doesn't exist, we can't reverse the order */
				break;
			}
			indexExpr = rightop;
			constExpr = leftop;
		}
		else
		{
			break;
		}

		/*
		 * Ignore any RelabelType node above the operands.  This is needed to
		 * be able to apply indexscanning in binary-compatible-operator cases.
		 * Note: we can assume there is at most one RelabelType node;
		 * eval_const_expressions() will have simplified if more than one.
		 */
		if (IsA(constExpr, RelabelType))
			constExpr = (Node *) ((RelabelType *) constExpr)->arg;
		if (IsA(indexExpr, RelabelType))
			indexExpr = (Node *) ((RelabelType *) indexExpr)->arg;

		/* Forbid transformation for composite types, records. */
		if (type_is_rowtype(exprType(constExpr)) ||
			type_is_rowtype(exprType(indexExpr)))
			break;

		/*
		 * Save information about the operator, type, and collation for the
		 * first matching qual.  Then, check that subsequent quals match the
		 * first.
		 */
		if (firstTime)
		{
			matchOpno = opno;
			consttype = exprType(constExpr);
			arraytype = get_array_type(consttype);
			inputcollid = subClause->inputcollid;

			/*
			 * Check that the operator is presented in the opfamily and that
			 * the expression collation matches the index collation.  Also,
			 * there must be an array type to construct an array later.
			 */
			if (!IndexCollMatchesExprColl(index->indexcollations[indexcol], inputcollid) ||
				!op_in_opfamily(matchOpno, index->opfamily[indexcol]) ||
				!OidIsValid(arraytype))
				break;
			firstTime = false;
		}
		else
		{
			if (opno != matchOpno ||
				inputcollid != subClause->inputcollid ||
				consttype != exprType(constExpr))
				break;
		}

		/*
		 * Check if our list of constants in match_clause_to_indexcol's
		 * understanding of const-ness have something other than Const.
		 */
		if (!IsA(constExpr, Const))
			haveNonConst = true;
		consts = lappend(consts, constExpr);
	}

	/*
	 * Catch the break from the loop above.  Normally, a foreach() loop ends
	 * up with a NULL list cell.  A non-NULL list cell indicates a break from
	 * the foreach() loop.  Free the consts list and return NULL then.
	 */
	if (lc != NULL)
	{
		list_free(consts);
		return NULL;
	}

	saopexpr = make_SAOP_expr(matchOpno, indexExpr, consttype, inputcollid,
							  inputcollid, consts, haveNonConst);

	/*
	 * Finally, build an IndexClause based on the SAOP node.  Use
	 * make_simple_restrictinfo() to get RestrictInfo with clean selectivity
	 * estimations, because they may differ from the estimation made for an OR
	 * clause.  Although it is not a lossy expression, keep the original rinfo
	 * in iclause->rinfo as prescribed.
	 */
	iclause = makeNode(IndexClause);
	iclause->rinfo = rinfo;
	iclause->indexquals = list_make1(make_simple_restrictinfo(root,
															  &saopexpr->xpr));
	iclause->lossy = false;
	iclause->indexcol = indexcol;
	iclause->indexcols = NIL;
	return iclause;
}

/*
 * expand_indexqual_rowcompare --- expand a single indexqual condition
 *		that is a RowCompareExpr
 *
 * It's already known that the first column of the row comparison matches
 * the specified column of the index.  We can use additional columns of the
 * row comparison as index qualifications, so long as they match the index
 * in the "same direction", ie, the indexkeys are all on the same side of the
 * clause and the operators are all the same-type members of the opfamilies.
 *
 * If all the columns of the RowCompareExpr match in this way, we just use it
 * as-is, except for possibly commuting it to put the indexkeys on the left.
 *
 * Otherwise, we build a shortened RowCompareExpr (if more than one
 * column matches) or a simple OpExpr (if the first-column match is all
 * there is).  In these cases the modified clause is always "<=" or ">="
 * even when the original was "<" or ">" --- this is necessary to match all
 * the rows that could match the original.  (We are building a lossy version
 * of the row comparison when we do this, so we set lossy = true.)
 *
 * Note: this is really just the last half of match_rowcompare_to_indexcol,
 * but we split it out for comprehensibility.
 */
static IndexClause *
expand_indexqual_rowcompare(PlannerInfo *root,
							RestrictInfo *rinfo,
							int indexcol,
							IndexOptInfo *index,
							Oid expr_op,
							bool var_on_left)
{
	IndexClause *iclause = makeNode(IndexClause);
	RowCompareExpr *clause = (RowCompareExpr *) rinfo->clause;
	int			op_strategy;
	Oid			op_lefttype;
	Oid			op_righttype;
	int			matching_cols;
	List	   *expr_ops;
	List	   *opfamilies;
	List	   *lefttypes;
	List	   *righttypes;
	List	   *new_ops;
	List	   *var_args;
	List	   *non_var_args;

	iclause->rinfo = rinfo;
	iclause->indexcol = indexcol;

	if (var_on_left)
	{
		var_args = clause->largs;
		non_var_args = clause->rargs;
	}
	else
	{
		var_args = clause->rargs;
		non_var_args = clause->largs;
	}

	get_op_opfamily_properties(expr_op, index->opfamily[indexcol], false,
							   &op_strategy,
							   &op_lefttype,
							   &op_righttype);

	/* Initialize returned list of which index columns are used */
	iclause->indexcols = list_make1_int(indexcol);

	/* Build lists of ops, opfamilies and operator datatypes in case needed */
	expr_ops = list_make1_oid(expr_op);
	opfamilies = list_make1_oid(index->opfamily[indexcol]);
	lefttypes = list_make1_oid(op_lefttype);
	righttypes = list_make1_oid(op_righttype);

	/*
	 * See how many of the remaining columns match some index column in the
	 * same way.  As in match_clause_to_indexcol(), the "other" side of any
	 * potential index condition is OK as long as it doesn't use Vars from the
	 * indexed relation.
	 */
	matching_cols = 1;

	while (matching_cols < list_length(var_args))
	{
		Node	   *varop = (Node *) list_nth(var_args, matching_cols);
		Node	   *constop = (Node *) list_nth(non_var_args, matching_cols);
		int			i;

		expr_op = list_nth_oid(clause->opnos, matching_cols);
		if (!var_on_left)
		{
			/* indexkey is on right, so commute the operator */
			expr_op = get_commutator(expr_op);
			if (expr_op == InvalidOid)
				break;			/* operator is not usable */
		}
		if (bms_is_member(index->rel->relid, pull_varnos(root, constop)))
			break;				/* no good, Var on wrong side */
		if (contain_volatile_functions(constop))
			break;				/* no good, volatile comparison value */

		/*
		 * The Var side can match any key column of the index.
		 */
		for (i = 0; i < index->nkeycolumns; i++)
		{
			if (match_index_to_operand(varop, i, index) &&
				get_op_opfamily_strategy(expr_op,
										 index->opfamily[i]) == op_strategy &&
				IndexCollMatchesExprColl(index->indexcollations[i],
										 list_nth_oid(clause->inputcollids,
													  matching_cols)))
				break;
		}
		if (i >= index->nkeycolumns)
			break;				/* no match found */

		/* Add column number to returned list */
		iclause->indexcols = lappend_int(iclause->indexcols, i);

		/* Add operator info to lists */
		get_op_opfamily_properties(expr_op, index->opfamily[i], false,
								   &op_strategy,
								   &op_lefttype,
								   &op_righttype);
		expr_ops = lappend_oid(expr_ops, expr_op);
		opfamilies = lappend_oid(opfamilies, index->opfamily[i]);
		lefttypes = lappend_oid(lefttypes, op_lefttype);
		righttypes = lappend_oid(righttypes, op_righttype);

		/* This column matches, keep scanning */
		matching_cols++;
	}

	/* Result is non-lossy if all columns are usable as index quals */
	iclause->lossy = (matching_cols != list_length(clause->opnos));

	/*
	 * We can use rinfo->clause as-is if we have var on left and it's all
	 * usable as index quals.
	 */
	if (var_on_left && !iclause->lossy)
		iclause->indexquals = list_make1(rinfo);
	else
	{
		/*
		 * We have to generate a modified rowcompare (possibly just one
		 * OpExpr).  The painful part of this is changing < to <= or > to >=,
		 * so deal with that first.
		 */
		if (!iclause->lossy)
		{
			/* very easy, just use the commuted operators */
			new_ops = expr_ops;
		}
		else if (op_strategy == BTLessEqualStrategyNumber ||
				 op_strategy == BTGreaterEqualStrategyNumber)
		{
			/* easy, just use the same (possibly commuted) operators */
			new_ops = list_truncate(expr_ops, matching_cols);
		}
		else
		{
			ListCell   *opfamilies_cell;
			ListCell   *lefttypes_cell;
			ListCell   *righttypes_cell;

			if (op_strategy == BTLessStrategyNumber)
				op_strategy = BTLessEqualStrategyNumber;
			else if (op_strategy == BTGreaterStrategyNumber)
				op_strategy = BTGreaterEqualStrategyNumber;
			else
				elog(ERROR, "unexpected strategy number %d", op_strategy);
			new_ops = NIL;
			forthree(opfamilies_cell, opfamilies,
					 lefttypes_cell, lefttypes,
					 righttypes_cell, righttypes)
			{
				Oid			opfam = lfirst_oid(opfamilies_cell);
				Oid			lefttype = lfirst_oid(lefttypes_cell);
				Oid			righttype = lfirst_oid(righttypes_cell);

				expr_op = get_opfamily_member(opfam, lefttype, righttype,
											  op_strategy);
				if (!OidIsValid(expr_op))	/* should not happen */
					elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
						 op_strategy, lefttype, righttype, opfam);
				new_ops = lappend_oid(new_ops, expr_op);
			}
		}

		/* If we have more than one matching col, create a subset rowcompare */
		if (matching_cols > 1)
		{
			RowCompareExpr *rc = makeNode(RowCompareExpr);

			rc->cmptype = (CompareType) op_strategy;
			rc->opnos = new_ops;
			rc->opfamilies = list_copy_head(clause->opfamilies,
											matching_cols);
			rc->inputcollids = list_copy_head(clause->inputcollids,
											  matching_cols);
			rc->largs = list_copy_head(var_args, matching_cols);
			rc->rargs = list_copy_head(non_var_args, matching_cols);
			iclause->indexquals = list_make1(make_simple_restrictinfo(root,
																	  (Expr *) rc));
		}
		else
		{
			Expr	   *op;

			/* We don't report an index column list in this case */
			iclause->indexcols = NIL;

			op = make_opclause(linitial_oid(new_ops), BOOLOID, false,
							   copyObject(linitial(var_args)),
							   copyObject(linitial(non_var_args)),
							   InvalidOid,
							   linitial_oid(clause->inputcollids));
			iclause->indexquals = list_make1(make_simple_restrictinfo(root, op));
		}
	}

	return iclause;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK ORDERING OPERATORS	----
 ****************************************************************************/

/*
 * match_pathkeys_to_index
 *		For the given 'index' and 'pathkeys', output a list of suitable ORDER
 *		BY expressions, each of the form "indexedcol operator pseudoconstant",
 *		along with an integer list of the index column numbers (zero based)
 *		that each clause would be used with.
 *
 * This attempts to find an ORDER BY and index column number for all items in
 * the pathkey list, however, if we're unable to match any given pathkey to an
 * index column, we return just the ones matched by the function so far.  This
 * allows callers who are interested in partial matches to get them.  Callers
 * can determine a partial match vs a full match by checking the outputted
 * list lengths.  A full match will have one item in the output lists for each
 * item in the given 'pathkeys' list.
 */
static void
match_pathkeys_to_index(IndexOptInfo *index, List *pathkeys,
						List **orderby_clauses_p,
						List **clause_columns_p)
{
	ListCell   *lc1;

	*orderby_clauses_p = NIL;	/* set default results */
	*clause_columns_p = NIL;

	/* Only indexes with the amcanorderbyop property are interesting here */
	if (!index->amcanorderbyop)
		return;

	foreach(lc1, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc1);
		bool		found = false;
		EquivalenceMemberIterator it;
		EquivalenceMember *member;


		/* Pathkey must request default sort order for the target opfamily */
		if (pathkey->pk_cmptype != COMPARE_LT || pathkey->pk_nulls_first)
			return;

		/* If eclass is volatile, no hope of using an indexscan */
		if (pathkey->pk_eclass->ec_has_volatile)
			return;

		/*
		 * Try to match eclass member expression(s) to index.  Note that child
		 * EC members are considered, but only when they belong to the target
		 * relation.  (Unlike regular members, the same expression could be a
		 * child member of more than one EC.  Therefore, the same index could
		 * be considered to match more than one pathkey list, which is OK
		 * here.  See also get_eclass_for_sort_expr.)
		 */
		setup_eclass_member_iterator(&it, pathkey->pk_eclass,
									 index->rel->relids);
		while ((member = eclass_member_iterator_next(&it)) != NULL)
		{
			int			indexcol;

			/* No possibility of match if it references other relations */
			if (!bms_equal(member->em_relids, index->rel->relids))
				continue;

			/*
			 * We allow any column of the index to match each pathkey; they
			 * don't have to match left-to-right as you might expect.  This is
			 * correct for GiST, and it doesn't matter for SP-GiST because
			 * that doesn't handle multiple columns anyway, and no other
			 * existing AMs support amcanorderbyop.  We might need different
			 * logic in future for other implementations.
			 */
			for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
			{
				Expr	   *expr;

				expr = match_clause_to_ordering_op(index,
												   indexcol,
												   member->em_expr,
												   pathkey->pk_opfamily);
				if (expr)
				{
					*orderby_clauses_p = lappend(*orderby_clauses_p, expr);
					*clause_columns_p = lappend_int(*clause_columns_p, indexcol);
					found = true;
					break;
				}
			}

			if (found)			/* don't want to look at remaining members */
				break;
		}

		/*
		 * Return the matches found so far when this pathkey couldn't be
		 * matched to the index.
		 */
		if (!found)
			return;
	}
}

/*
 * match_clause_to_ordering_op
 *	  Determines whether an ordering operator expression matches an
 *	  index column.
 *
 *	  This is similar to, but simpler than, match_clause_to_indexcol.
 *	  We only care about simple OpExpr cases.  The input is a bare
 *	  expression that is being ordered by, which must be of the form
 *	  (indexkey op const) or (const op indexkey) where op is an ordering
 *	  operator for the column's opfamily.
 *
 * 'index' is the index of interest.
 * 'indexcol' is a column number of 'index' (counting from 0).
 * 'clause' is the ordering expression to be tested.
 * 'pk_opfamily' is the btree opfamily describing the required sort order.
 *
 * Note that we currently do not consider the collation of the ordering
 * operator's result.  In practical cases the result type will be numeric
 * and thus have no collation, and it's not very clear what to match to
 * if it did have a collation.  The index's collation should match the
 * ordering operator's input collation, not its result.
 *
 * If successful, return 'clause' as-is if the indexkey is on the left,
 * otherwise a commuted copy of 'clause'.  If no match, return NULL.
 */
static Expr *
match_clause_to_ordering_op(IndexOptInfo *index,
							int indexcol,
							Expr *clause,
							Oid pk_opfamily)
{
	Oid			opfamily;
	Oid			idxcollation;
	Node	   *leftop,
			   *rightop;
	Oid			expr_op;
	Oid			expr_coll;
	Oid			sortfamily;
	bool		commuted;

	Assert(indexcol < index->nkeycolumns);

	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * Clause must be a binary opclause.
	 */
	if (!is_opclause(clause))
		return NULL;
	leftop = get_leftop(clause);
	rightop = get_rightop(clause);
	if (!leftop || !rightop)
		return NULL;
	expr_op = ((OpExpr *) clause)->opno;
	expr_coll = ((OpExpr *) clause)->inputcollid;

	/*
	 * We can forget the whole thing right away if wrong collation.
	 */
	if (!IndexCollMatchesExprColl(idxcollation, expr_coll))
		return NULL;

	/*
	 * Check for clauses of the form: (indexkey operator constant) or
	 * (constant operator indexkey).
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!contain_var_clause(rightop) &&
		!contain_volatile_functions(rightop))
	{
		commuted = false;
	}
	else if (match_index_to_operand(rightop, indexcol, index) &&
			 !contain_var_clause(leftop) &&
			 !contain_volatile_functions(leftop))
	{
		/* Might match, but we need a commuted operator */
		expr_op = get_commutator(expr_op);
		if (expr_op == InvalidOid)
			return NULL;
		commuted = true;
	}
	else
		return NULL;

	/*
	 * Is the (commuted) operator an ordering operator for the opfamily? And
	 * if so, does it yield the right sorting semantics?
	 */
	sortfamily = get_op_opfamily_sortfamily(expr_op, opfamily);
	if (sortfamily != pk_opfamily)
		return NULL;

	/* We have a match.  Return clause or a commuted version thereof. */
	if (commuted)
	{
		OpExpr	   *newclause = makeNode(OpExpr);

		/* flat-copy all the fields of clause */
		memcpy(newclause, clause, sizeof(OpExpr));

		/* commute it */
		newclause->opno = expr_op;
		newclause->opfuncid = InvalidOid;
		newclause->args = list_make2(rightop, leftop);

		clause = (Expr *) newclause;
	}

	return clause;
}


/****************************************************************************
 *				----  ROUTINES TO DO PARTIAL INDEX PREDICATE TESTS	----
 ****************************************************************************/

/*
 * check_index_predicates
 *		Set the predicate-derived IndexOptInfo fields for each index
 *		of the specified relation.
 *
 * predOK is set true if the index is partial and its predicate is satisfied
 * for this query, ie the query's WHERE clauses imply the predicate.
 *
 * indrestrictinfo is set to the relation's baserestrictinfo list less any
 * conditions that are implied by the index's predicate.  (Obviously, for a
 * non-partial index, this is the same as baserestrictinfo.)  Such conditions
 * can be dropped from the plan when using the index, in certain cases.
 *
 * At one time it was possible for this to get re-run after adding more
 * restrictions to the rel, thus possibly letting us prove more indexes OK.
 * That doesn't happen any more (at least not in the core code's usage),
 * but this code still supports it in case extensions want to mess with the
 * baserestrictinfo list.  We assume that adding more restrictions can't make
 * an index not predOK.  We must recompute indrestrictinfo each time, though,
 * to make sure any newly-added restrictions get into it if needed.
 */
void
check_index_predicates(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *clauselist;
	bool		have_partial;
	bool		is_target_rel;
	Relids		otherrels;
	ListCell   *lc;

	/* Indexes are available only on base or "other" member relations. */
	Assert(IS_SIMPLE_REL(rel));

	/*
	 * Initialize the indrestrictinfo lists to be identical to
	 * baserestrictinfo, and check whether there are any partial indexes.  If
	 * not, this is all we need to do.
	 */
	have_partial = false;
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);

		index->indrestrictinfo = rel->baserestrictinfo;
		if (index->indpred)
			have_partial = true;
	}
	if (!have_partial)
		return;

	/*
	 * Construct a list of clauses that we can assume true for the purpose of
	 * proving the index(es) usable.  Restriction clauses for the rel are
	 * always usable, and so are any join clauses that are "movable to" this
	 * rel.  Also, we can consider any EC-derivable join clauses (which must
	 * be "movable to" this rel, by definition).
	 */
	clauselist = list_copy(rel->baserestrictinfo);

	/* Scan the rel's join clauses */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/* Check if clause can be moved to this rel */
		if (!join_clause_is_movable_to(rinfo, rel))
			continue;

		clauselist = lappend(clauselist, rinfo);
	}

	/*
	 * Add on any equivalence-derivable join clauses.  Computing the correct
	 * relid sets for generate_join_implied_equalities is slightly tricky
	 * because the rel could be a child rel rather than a true baserel, and in
	 * that case we must subtract its parents' relid(s) from all_query_rels.
	 * Additionally, we mustn't consider clauses that are only computable
	 * after outer joins that can null the rel.
	 */
	if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		otherrels = bms_difference(root->all_query_rels,
								   find_childrel_parents(root, rel));
	else
		otherrels = bms_difference(root->all_query_rels, rel->relids);
	otherrels = bms_del_members(otherrels, rel->nulling_relids);

	if (!bms_is_empty(otherrels))
		clauselist =
			list_concat(clauselist,
						generate_join_implied_equalities(root,
														 bms_union(rel->relids,
																   otherrels),
														 otherrels,
														 rel,
														 NULL));

	/*
	 * Normally we remove quals that are implied by a partial index's
	 * predicate from indrestrictinfo, indicating that they need not be
	 * checked explicitly by an indexscan plan using this index.  However, if
	 * the rel is a target relation of UPDATE/DELETE/MERGE/SELECT FOR UPDATE,
	 * we cannot remove such quals from the plan, because they need to be in
	 * the plan so that they will be properly rechecked by EvalPlanQual
	 * testing.  Some day we might want to remove such quals from the main
	 * plan anyway and pass them through to EvalPlanQual via a side channel;
	 * but for now, we just don't remove implied quals at all for target
	 * relations.
	 */
	is_target_rel = (bms_is_member(rel->relid, root->all_result_relids) ||
					 get_plan_rowmark(root->rowMarks, rel->relid) != NULL);

	/*
	 * Now try to prove each index predicate true, and compute the
	 * indrestrictinfo lists for partial indexes.  Note that we compute the
	 * indrestrictinfo list even for non-predOK indexes; this might seem
	 * wasteful, but we may be able to use such indexes in OR clauses, cf
	 * generate_bitmap_or_paths().
	 */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		ListCell   *lcr;

		if (index->indpred == NIL)
			continue;			/* ignore non-partial indexes here */

		if (!index->predOK)		/* don't repeat work if already proven OK */
			index->predOK = predicate_implied_by(index->indpred, clauselist,
												 false);

		/* If rel is an update target, leave indrestrictinfo as set above */
		if (is_target_rel)
			continue;

		/* Else compute indrestrictinfo as the non-implied quals */
		index->indrestrictinfo = NIL;
		foreach(lcr, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lcr);

			/* predicate_implied_by() assumes first arg is immutable */
			if (contain_mutable_functions((Node *) rinfo->clause) ||
				!predicate_implied_by(list_make1(rinfo->clause),
									  index->indpred, false))
				index->indrestrictinfo = lappend(index->indrestrictinfo, rinfo);
		}
	}
}

/****************************************************************************
 *				----  ROUTINES TO CHECK EXTERNALLY-VISIBLE CONDITIONS  ----
 ****************************************************************************/

/*
 * ec_member_matches_indexcol
 *	  Test whether an EquivalenceClass member matches an index column.
 *
 * This is a callback for use by generate_implied_equalities_for_column.
 */
static bool
ec_member_matches_indexcol(PlannerInfo *root, RelOptInfo *rel,
						   EquivalenceClass *ec, EquivalenceMember *em,
						   void *arg)
{
	IndexOptInfo *index = ((ec_member_matches_arg *) arg)->index;
	int			indexcol = ((ec_member_matches_arg *) arg)->indexcol;
	Oid			curFamily;
	Oid			curCollation;

	Assert(indexcol < index->nkeycolumns);

	curFamily = index->opfamily[indexcol];
	curCollation = index->indexcollations[indexcol];

	/*
	 * If it's a btree index, we can reject it if its opfamily isn't
	 * compatible with the EC, since no clause generated from the EC could be
	 * used with the index.  For non-btree indexes, we can't easily tell
	 * whether clauses generated from the EC could be used with the index, so
	 * don't check the opfamily.  This might mean we return "true" for a
	 * useless EC, so we have to recheck the results of
	 * generate_implied_equalities_for_column; see
	 * match_eclass_clauses_to_index.
	 */
	if (index->relam == BTREE_AM_OID &&
		!list_member_oid(ec->ec_opfamilies, curFamily))
		return false;

	/* We insist on collation match for all index types, though */
	if (!IndexCollMatchesExprColl(curCollation, ec->ec_collation))
		return false;

	return match_index_to_operand((Node *) em->em_expr, indexcol, index);
}

/*
 * relation_has_unique_index_for
 *	  Determine whether the relation provably has at most one row satisfying
 *	  a set of equality conditions, because the conditions constrain all
 *	  columns of some unique index.
 *
 * The conditions can be represented in either or both of two ways:
 * 1. A list of RestrictInfo nodes, where the caller has already determined
 * that each condition is a mergejoinable equality with an expression in
 * this relation on one side, and an expression not involving this relation
 * on the other.  The transient outer_is_left flag is used to identify which
 * side we should look at: left side if outer_is_left is false, right side
 * if it is true.
 * 2. A list of expressions in this relation, and a corresponding list of
 * equality operators. The caller must have already checked that the operators
 * represent equality.  (Note: the operators could be cross-type; the
 * expressions should correspond to their RHS inputs.)
 *
 * The caller need only supply equality conditions arising from joins;
 * this routine automatically adds in any usable baserestrictinfo clauses.
 * (Note that the passed-in restrictlist will be destructively modified!)
 */
bool
relation_has_unique_index_for(PlannerInfo *root, RelOptInfo *rel,
							  List *restrictlist,
							  List *exprlist, List *oprlist)
{
	return relation_has_unique_index_ext(root, rel, restrictlist,
										 exprlist, oprlist, NULL);
}

/*
 * relation_has_unique_index_ext
 *	  Same as relation_has_unique_index_for(), but supports extra_clauses
 *	  parameter.  If extra_clauses isn't NULL, return baserestrictinfo clauses
 *	  which were used to derive uniqueness.
 */
bool
relation_has_unique_index_ext(PlannerInfo *root, RelOptInfo *rel,
							  List *restrictlist,
							  List *exprlist, List *oprlist,
							  List **extra_clauses)
{
	ListCell   *ic;

	Assert(list_length(exprlist) == list_length(oprlist));

	/* Short-circuit if no indexes... */
	if (rel->indexlist == NIL)
		return false;

	/*
	 * Examine the rel's restriction clauses for usable var = const clauses
	 * that we can add to the restrictlist.
	 */
	foreach(ic, rel->baserestrictinfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(ic);

		/*
		 * Note: can_join won't be set for a restriction clause, but
		 * mergeopfamilies will be if it has a mergejoinable operator and
		 * doesn't contain volatile functions.
		 */
		if (restrictinfo->mergeopfamilies == NIL)
			continue;			/* not mergejoinable */

		/*
		 * The clause certainly doesn't refer to anything but the given rel.
		 * If either side is pseudoconstant then we can use it.
		 */
		if (bms_is_empty(restrictinfo->left_relids))
		{
			/* righthand side is inner */
			restrictinfo->outer_is_left = true;
		}
		else if (bms_is_empty(restrictinfo->right_relids))
		{
			/* lefthand side is inner */
			restrictinfo->outer_is_left = false;
		}
		else
			continue;

		/* OK, add to list */
		restrictlist = lappend(restrictlist, restrictinfo);
	}

	/* Short-circuit the easy case */
	if (restrictlist == NIL && exprlist == NIL)
		return false;

	/* Examine each index of the relation ... */
	foreach(ic, rel->indexlist)
	{
		IndexOptInfo *ind = (IndexOptInfo *) lfirst(ic);
		int			c;
		List	   *exprs = NIL;

		/*
		 * If the index is not unique, or not immediately enforced, or if it's
		 * a partial index, it's useless here.  We're unable to make use of
		 * predOK partial unique indexes due to the fact that
		 * check_index_predicates() also makes use of join predicates to
		 * determine if the partial index is usable. Here we need proofs that
		 * hold true before any joins are evaluated.
		 */
		if (!ind->unique || !ind->immediate || ind->indpred != NIL)
			continue;

		/*
		 * Try to find each index column in the lists of conditions.  This is
		 * O(N^2) or worse, but we expect all the lists to be short.
		 */
		for (c = 0; c < ind->nkeycolumns; c++)
		{
			bool		matched = false;
			ListCell   *lc;
			ListCell   *lc2;

			foreach(lc, restrictlist)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
				Node	   *rexpr;

				/*
				 * The condition's equality operator must be a member of the
				 * index opfamily, else it is not asserting the right kind of
				 * equality behavior for this index.  We check this first
				 * since it's probably cheaper than match_index_to_operand().
				 */
				if (!list_member_oid(rinfo->mergeopfamilies, ind->opfamily[c]))
					continue;

				/*
				 * XXX at some point we may need to check collations here too.
				 * For the moment we assume all collations reduce to the same
				 * notion of equality.
				 */

				/* OK, see if the condition operand matches the index key */
				if (rinfo->outer_is_left)
					rexpr = get_rightop(rinfo->clause);
				else
					rexpr = get_leftop(rinfo->clause);

				if (match_index_to_operand(rexpr, c, ind))
				{
					matched = true; /* column is unique */

					if (bms_membership(rinfo->clause_relids) == BMS_SINGLETON)
					{
						MemoryContext oldMemCtx =
							MemoryContextSwitchTo(root->planner_cxt);

						/*
						 * Add filter clause into a list allowing caller to
						 * know if uniqueness have made not only by join
						 * clauses.
						 */
						Assert(bms_is_empty(rinfo->left_relids) ||
							   bms_is_empty(rinfo->right_relids));
						if (extra_clauses)
							exprs = lappend(exprs, rinfo);
						MemoryContextSwitchTo(oldMemCtx);
					}

					break;
				}
			}

			if (matched)
				continue;

			forboth(lc, exprlist, lc2, oprlist)
			{
				Node	   *expr = (Node *) lfirst(lc);
				Oid			opr = lfirst_oid(lc2);

				/* See if the expression matches the index key */
				if (!match_index_to_operand(expr, c, ind))
					continue;

				/*
				 * The equality operator must be a member of the index
				 * opfamily, else it is not asserting the right kind of
				 * equality behavior for this index.  We assume the caller
				 * determined it is an equality operator, so we don't need to
				 * check any more tightly than this.
				 */
				if (!op_in_opfamily(opr, ind->opfamily[c]))
					continue;

				/*
				 * XXX at some point we may need to check collations here too.
				 * For the moment we assume all collations reduce to the same
				 * notion of equality.
				 */

				matched = true; /* column is unique */
				break;
			}

			if (!matched)
				break;			/* no match; this index doesn't help us */
		}

		/* Matched all key columns of this index? */
		if (c == ind->nkeycolumns)
		{
			if (extra_clauses)
				*extra_clauses = exprs;
			return true;
		}
	}

	return false;
}

/*
 * indexcol_is_bool_constant_for_query
 *
 * If an index column is constrained to have a constant value by the query's
 * WHERE conditions, then it's irrelevant for sort-order considerations.
 * Usually that means we have a restriction clause WHERE indexcol = constant,
 * which gets turned into an EquivalenceClass containing a constant, which
 * is recognized as redundant by build_index_pathkeys().  But if the index
 * column is a boolean variable (or expression), then we are not going to
 * see WHERE indexcol = constant, because expression preprocessing will have
 * simplified that to "WHERE indexcol" or "WHERE NOT indexcol".  So we are not
 * going to have a matching EquivalenceClass (unless the query also contains
 * "ORDER BY indexcol").  To allow such cases to work the same as they would
 * for non-boolean values, this function is provided to detect whether the
 * specified index column matches a boolean restriction clause.
 */
bool
indexcol_is_bool_constant_for_query(PlannerInfo *root,
									IndexOptInfo *index,
									int indexcol)
{
	ListCell   *lc;

	/* If the index isn't boolean, we can't possibly get a match */
	if (!IsBooleanOpfamily(index->opfamily[indexcol]))
		return false;

	/* Check each restriction clause for the index's rel */
	foreach(lc, index->rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * As in match_clause_to_indexcol, never match pseudoconstants to
		 * indexes.  (It might be semantically okay to do so here, but the
		 * odds of getting a match are negligible, so don't waste the cycles.)
		 */
		if (rinfo->pseudoconstant)
			continue;

		/* See if we can match the clause's expression to the index column */
		if (match_boolean_index_clause(root, rinfo, indexcol, index))
			return true;
	}

	return false;
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
 *
 * Note that we aren't interested in collations here; the caller must check
 * for a collation match, if it's dealing with an operator where that matters.
 *
 * This is exported for use in selfuncs.c.
 */
bool
match_index_to_operand(Node *operand,
					   int indexcol,
					   IndexOptInfo *index)
{
	int			indkey;

	/*
	 * Ignore any RelabelType node above the operand.   This is needed to be
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
			indkey == ((Var *) operand)->varattno &&
			((Var *) operand)->varnullingrels == NULL)
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
				indexpr_item = lnext(index->indexprs, indexpr_item);
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

/*
 * is_pseudo_constant_for_index()
 *	  Test whether the given expression can be used as an indexscan
 *	  comparison value.
 *
 * An indexscan comparison value must not contain any volatile functions,
 * and it can't contain any Vars of the index's own table.  Vars of
 * other tables are okay, though; in that case we'd be producing an
 * indexqual usable in a parameterized indexscan.  This is, therefore,
 * a weaker condition than is_pseudo_constant_clause().
 *
 * This function is exported for use by planner support functions,
 * which will have available the IndexOptInfo, but not any RestrictInfo
 * infrastructure.  It is making the same test made by functions above
 * such as match_opclause_to_indexcol(), but those rely where possible
 * on RestrictInfo information about variable membership.
 *
 * expr: the nodetree to be checked
 * index: the index of interest
 */
bool
is_pseudo_constant_for_index(PlannerInfo *root, Node *expr, IndexOptInfo *index)
{
	/* pull_varnos is cheaper than volatility check, so do that first */
	if (bms_is_member(index->rel->relid, pull_varnos(root, expr)))
		return false;			/* no good, contains Var of table */
	if (contain_volatile_functions(expr))
		return false;			/* no good, volatile comparison value */
	return true;
}
