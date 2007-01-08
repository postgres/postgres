/*-------------------------------------------------------------------------
 *
 * costsize.c
 *	  Routines to compute (and set) relation sizes and path costs
 *
 * Path costs are measured in arbitrary units established by these basic
 * parameters:
 *
 *	seq_page_cost		Cost of a sequential page fetch
 *	random_page_cost	Cost of a non-sequential page fetch
 *	cpu_tuple_cost		Cost of typical CPU time to process a tuple
 *	cpu_index_tuple_cost  Cost of typical CPU time to process an index tuple
 *	cpu_operator_cost	Cost of CPU time to execute an operator or function
 *
 * We expect that the kernel will typically do some amount of read-ahead
 * optimization; this in conjunction with seek costs means that seq_page_cost
 * is normally considerably less than random_page_cost.  (However, if the
 * database is fully cached in RAM, it is reasonable to set them equal.)
 *
 * We also use a rough estimate "effective_cache_size" of the number of
 * disk pages in Postgres + OS-level disk cache.  (We can't simply use
 * NBuffers for this purpose because that would ignore the effects of
 * the kernel's disk cache.)
 *
 * Obviously, taking constants for these values is an oversimplification,
 * but it's tough enough to get any useful estimates even at this level of
 * detail.	Note that all of these parameters are user-settable, in case
 * the default values are drastically off for a particular platform.
 *
 * We compute two separate costs for each path:
 *		total_cost: total estimated cost to fetch all tuples
 *		startup_cost: cost that is expended before first tuple is fetched
 * In some scenarios, such as when there is a LIMIT or we are implementing
 * an EXISTS(...) sub-select, it is not necessary to fetch all tuples of the
 * path's result.  A caller can estimate the cost of fetching a partial
 * result by interpolating between startup_cost and total_cost.  In detail:
 *		actual_cost = startup_cost +
 *			(total_cost - startup_cost) * tuples_to_fetch / path->parent->rows;
 * Note that a base relation's rows count (and, by extension, plan_rows for
 * plan nodes below the LIMIT node) are set without regard to any LIMIT, so
 * that this equation works properly.  (Also, these routines guarantee not to
 * set the rows count to zero, so there will be no zero divide.)  The LIMIT is
 * applied as a top-level plan node.
 *
 * For largely historical reasons, most of the routines in this module use
 * the passed result Path only to store their startup_cost and total_cost
 * results into.  All the input data they need is passed as separate
 * parameters, even though much of it could be extracted from the Path.
 * An exception is made for the cost_XXXjoin() routines, which expect all
 * the non-cost fields of the passed XXXPath to be filled in.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/costsize.c,v 1.169.2.2 2007/01/08 16:09:31 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "executor/nodeHash.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/tuplesort.h"


#define LOG2(x)  (log(x) / 0.693147180559945)

/*
 * Some Paths return less than the nominal number of rows of their parent
 * relations; join nodes need to do this to get the correct input count:
 */
#define PATH_ROWS(path) \
	(IsA(path, UniquePath) ? \
	 ((UniquePath *) (path))->rows : \
	 (path)->parent->rows)


double		seq_page_cost = DEFAULT_SEQ_PAGE_COST;
double		random_page_cost = DEFAULT_RANDOM_PAGE_COST;
double		cpu_tuple_cost = DEFAULT_CPU_TUPLE_COST;
double		cpu_index_tuple_cost = DEFAULT_CPU_INDEX_TUPLE_COST;
double		cpu_operator_cost = DEFAULT_CPU_OPERATOR_COST;

int			effective_cache_size = DEFAULT_EFFECTIVE_CACHE_SIZE;

Cost		disable_cost = 100000000.0;

bool		enable_seqscan = true;
bool		enable_indexscan = true;
bool		enable_bitmapscan = true;
bool		enable_tidscan = true;
bool		enable_sort = true;
bool		enable_hashagg = true;
bool		enable_nestloop = true;
bool		enable_mergejoin = true;
bool		enable_hashjoin = true;


static bool cost_qual_eval_walker(Node *node, QualCost *total);
static Selectivity approx_selectivity(PlannerInfo *root, List *quals,
				   JoinType jointype);
static Selectivity join_in_selectivity(JoinPath *path, PlannerInfo *root);
static void set_rel_width(PlannerInfo *root, RelOptInfo *rel);
static double relation_byte_size(double tuples, int width);
static double page_size(double tuples, int width);


/*
 * clamp_row_est
 *		Force a row-count estimate to a sane value.
 */
double
clamp_row_est(double nrows)
{
	/*
	 * Force estimate to be at least one row, to make explain output look
	 * better and to avoid possible divide-by-zero when interpolating costs.
	 * Make it an integer, too.
	 */
	if (nrows <= 1.0)
		nrows = 1.0;
	else
		nrows = rint(nrows);

	return nrows;
}


/*
 * cost_seqscan
 *	  Determines and returns the cost of scanning a relation sequentially.
 */
void
cost_seqscan(Path *path, PlannerInfo *root,
			 RelOptInfo *baserel)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;

	/* Should only be applied to base relations */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	if (!enable_seqscan)
		startup_cost += disable_cost;

	/*
	 * disk costs
	 */
	run_cost += seq_page_cost * baserel->pages;

	/* CPU costs */
	startup_cost += baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_index
 *	  Determines and returns the cost of scanning a relation using an index.
 *
 * 'index' is the index to be used
 * 'indexQuals' is the list of applicable qual clauses (implicit AND semantics)
 * 'outer_rel' is the outer relation when we are considering using the index
 *		scan as the inside of a nestloop join (hence, some of the indexQuals
 *		are join clauses, and we should expect repeated scans of the index);
 *		NULL for a plain index scan
 *
 * cost_index() takes an IndexPath not just a Path, because it sets a few
 * additional fields of the IndexPath besides startup_cost and total_cost.
 * These fields are needed if the IndexPath is used in a BitmapIndexScan.
 *
 * NOTE: 'indexQuals' must contain only clauses usable as index restrictions.
 * Any additional quals evaluated as qpquals may reduce the number of returned
 * tuples, but they won't reduce the number of tuples we have to fetch from
 * the table, so they don't reduce the scan cost.
 *
 * NOTE: as of 8.0, indexQuals is a list of RestrictInfo nodes, where formerly
 * it was a list of bare clause expressions.
 */
void
cost_index(IndexPath *path, PlannerInfo *root,
		   IndexOptInfo *index,
		   List *indexQuals,
		   RelOptInfo *outer_rel)
{
	RelOptInfo *baserel = index->rel;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		indexStartupCost;
	Cost		indexTotalCost;
	Selectivity indexSelectivity;
	double		indexCorrelation,
				csquared;
	Cost		min_IO_cost,
				max_IO_cost;
	Cost		cpu_per_tuple;
	double		tuples_fetched;
	double		pages_fetched;

	/* Should only be applied to base relations */
	Assert(IsA(baserel, RelOptInfo) &&
		   IsA(index, IndexOptInfo));
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	if (!enable_indexscan)
		startup_cost += disable_cost;

	/*
	 * Call index-access-method-specific code to estimate the processing cost
	 * for scanning the index, as well as the selectivity of the index (ie,
	 * the fraction of main-table tuples we will have to retrieve) and its
	 * correlation to the main-table tuple order.
	 */
	OidFunctionCall8(index->amcostestimate,
					 PointerGetDatum(root),
					 PointerGetDatum(index),
					 PointerGetDatum(indexQuals),
					 PointerGetDatum(outer_rel),
					 PointerGetDatum(&indexStartupCost),
					 PointerGetDatum(&indexTotalCost),
					 PointerGetDatum(&indexSelectivity),
					 PointerGetDatum(&indexCorrelation));

	/*
	 * Save amcostestimate's results for possible use in bitmap scan planning.
	 * We don't bother to save indexStartupCost or indexCorrelation, because a
	 * bitmap scan doesn't care about either.
	 */
	path->indextotalcost = indexTotalCost;
	path->indexselectivity = indexSelectivity;

	/* all costs for touching index itself included here */
	startup_cost += indexStartupCost;
	run_cost += indexTotalCost - indexStartupCost;

	/* estimate number of main-table tuples fetched */
	tuples_fetched = clamp_row_est(indexSelectivity * baserel->tuples);

	/*----------
	 * Estimate number of main-table pages fetched, and compute I/O cost.
	 *
	 * When the index ordering is uncorrelated with the table ordering,
	 * we use an approximation proposed by Mackert and Lohman (see
	 * index_pages_fetched() for details) to compute the number of pages
	 * fetched, and then charge random_page_cost per page fetched.
	 *
	 * When the index ordering is exactly correlated with the table ordering
	 * (just after a CLUSTER, for example), the number of pages fetched should
	 * be exactly selectivity * table_size.  What's more, all but the first
	 * will be sequential fetches, not the random fetches that occur in the
	 * uncorrelated case.  So if the number of pages is more than 1, we
	 * ought to charge
	 *		random_page_cost + (pages_fetched - 1) * seq_page_cost
	 * For partially-correlated indexes, we ought to charge somewhere between
	 * these two estimates.  We currently interpolate linearly between the
	 * estimates based on the correlation squared (XXX is that appropriate?).
	 *----------
	 */
	if (outer_rel != NULL && outer_rel->rows > 1)
	{
		/*
		 * For repeated indexscans, the appropriate estimate for the
		 * uncorrelated case is to scale up the number of tuples fetched in
		 * the Mackert and Lohman formula by the number of scans, so that we
		 * estimate the number of pages fetched by all the scans; then
		 * pro-rate the costs for one scan.  In this case we assume all the
		 * fetches are random accesses.
		 */
		double		num_scans = outer_rel->rows;

		pages_fetched = index_pages_fetched(tuples_fetched * num_scans,
											baserel->pages,
											(double) index->pages,
											root);

		max_IO_cost = (pages_fetched * random_page_cost) / num_scans;

		/*
		 * In the perfectly correlated case, the number of pages touched
		 * by each scan is selectivity * table_size, and we can use the
		 * Mackert and Lohman formula at the page level to estimate how
		 * much work is saved by caching across scans.  We still assume
		 * all the fetches are random, though, which is an overestimate
		 * that's hard to correct for without double-counting the cache
		 * effects.  (But in most cases where such a plan is actually
		 * interesting, only one page would get fetched per scan anyway,
		 * so it shouldn't matter much.)
		 */
		pages_fetched = ceil(indexSelectivity * (double) baserel->pages);

		pages_fetched = index_pages_fetched(pages_fetched * num_scans,
											baserel->pages,
											(double) index->pages,
											root);

		min_IO_cost = (pages_fetched * random_page_cost) / num_scans;
	}
	else
	{
		/*
		 * Normal case: apply the Mackert and Lohman formula, and then
		 * interpolate between that and the correlation-derived result.
		 */
		pages_fetched = index_pages_fetched(tuples_fetched,
											baserel->pages,
											(double) index->pages,
											root);

		/* max_IO_cost is for the perfectly uncorrelated case (csquared=0) */
		max_IO_cost = pages_fetched * random_page_cost;

		/* min_IO_cost is for the perfectly correlated case (csquared=1) */
		pages_fetched = ceil(indexSelectivity * (double) baserel->pages);
		min_IO_cost = random_page_cost;
		if (pages_fetched > 1)
			min_IO_cost += (pages_fetched - 1) * seq_page_cost;
	}

	/*
	 * Now interpolate based on estimated index order correlation to get
	 * total disk I/O cost for main table accesses.
	 */
	csquared = indexCorrelation * indexCorrelation;

	run_cost += max_IO_cost + csquared * (min_IO_cost - max_IO_cost);

	/*
	 * Estimate CPU costs per tuple.
	 *
	 * Normally the indexquals will be removed from the list of restriction
	 * clauses that we have to evaluate as qpquals, so we should subtract
	 * their costs from baserestrictcost.  But if we are doing a join then
	 * some of the indexquals are join clauses and shouldn't be subtracted.
	 * Rather than work out exactly how much to subtract, we don't subtract
	 * anything.
	 */
	startup_cost += baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;

	if (outer_rel == NULL)
	{
		QualCost	index_qual_cost;

		cost_qual_eval(&index_qual_cost, indexQuals);
		/* any startup cost still has to be paid ... */
		cpu_per_tuple -= index_qual_cost.per_tuple;
	}

	run_cost += cpu_per_tuple * tuples_fetched;

	path->path.startup_cost = startup_cost;
	path->path.total_cost = startup_cost + run_cost;
}

/*
 * index_pages_fetched
 *	  Estimate the number of pages actually fetched after accounting for
 *	  cache effects.
 *
 * We use an approximation proposed by Mackert and Lohman, "Index Scans
 * Using a Finite LRU Buffer: A Validated I/O Model", ACM Transactions
 * on Database Systems, Vol. 14, No. 3, September 1989, Pages 401-424.
 * The Mackert and Lohman approximation is that the number of pages
 * fetched is
 *	PF =
 *		min(2TNs/(2T+Ns), T)			when T <= b
 *		2TNs/(2T+Ns)					when T > b and Ns <= 2Tb/(2T-b)
 *		b + (Ns - 2Tb/(2T-b))*(T-b)/T	when T > b and Ns > 2Tb/(2T-b)
 * where
 *		T = # pages in table
 *		N = # tuples in table
 *		s = selectivity = fraction of table to be scanned
 *		b = # buffer pages available (we include kernel space here)
 *
 * We assume that effective_cache_size is the total number of buffer pages
 * available for the whole query, and pro-rate that space across all the
 * tables in the query and the index currently under consideration.  (This
 * ignores space needed for other indexes used by the query, but since we
 * don't know which indexes will get used, we can't estimate that very well;
 * and in any case counting all the tables may well be an overestimate, since
 * depending on the join plan not all the tables may be scanned concurrently.)
 *
 * The product Ns is the number of tuples fetched; we pass in that
 * product rather than calculating it here.  "pages" is the number of pages
 * in the object under consideration (either an index or a table).
 * "index_pages" is the amount to add to the total table space, which was
 * computed for us by query_planner.
 *
 * Caller is expected to have ensured that tuples_fetched is greater than zero
 * and rounded to integer (see clamp_row_est).	The result will likewise be
 * greater than zero and integral.
 */
double
index_pages_fetched(double tuples_fetched, BlockNumber pages,
					double index_pages, PlannerInfo *root)
{
	double		pages_fetched;
	double		total_pages;
	double		T,
				b;

	/* T is # pages in table, but don't allow it to be zero */
	T = (pages > 1) ? (double) pages : 1.0;

	/* Compute number of pages assumed to be competing for cache space */
	total_pages = root->total_table_pages + index_pages;
	total_pages = Max(total_pages, 1.0);
	Assert(T <= total_pages);

	/* b is pro-rated share of effective_cache_size */
	b = (double) effective_cache_size *T / total_pages;

	/* force it positive and integral */
	if (b <= 1.0)
		b = 1.0;
	else
		b = ceil(b);

	/* This part is the Mackert and Lohman formula */
	if (T <= b)
	{
		pages_fetched =
			(2.0 * T * tuples_fetched) / (2.0 * T + tuples_fetched);
		if (pages_fetched >= T)
			pages_fetched = T;
		else
			pages_fetched = ceil(pages_fetched);
	}
	else
	{
		double		lim;

		lim = (2.0 * T * b) / (2.0 * T - b);
		if (tuples_fetched <= lim)
		{
			pages_fetched =
				(2.0 * T * tuples_fetched) / (2.0 * T + tuples_fetched);
		}
		else
		{
			pages_fetched =
				b + (tuples_fetched - lim) * (T - b) / T;
		}
		pages_fetched = ceil(pages_fetched);
	}
	return pages_fetched;
}

/*
 * get_indexpath_pages
 *		Determine the total size of the indexes used in a bitmap index path.
 *
 * Note: if the same index is used more than once in a bitmap tree, we will
 * count it multiple times, which perhaps is the wrong thing ... but it's
 * not completely clear, and detecting duplicates is difficult, so ignore it
 * for now.
 */
static double
get_indexpath_pages(Path *bitmapqual)
{
	double		result = 0;
	ListCell   *l;

	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;

		foreach(l, apath->bitmapquals)
		{
			result += get_indexpath_pages((Path *) lfirst(l));
		}
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;

		foreach(l, opath->bitmapquals)
		{
			result += get_indexpath_pages((Path *) lfirst(l));
		}
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;

		result = (double) ipath->indexinfo->pages;
	}
	else
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));

	return result;
}

/*
 * cost_bitmap_heap_scan
 *	  Determines and returns the cost of scanning a relation using a bitmap
 *	  index-then-heap plan.
 *
 * 'baserel' is the relation to be scanned
 * 'bitmapqual' is a tree of IndexPaths, BitmapAndPaths, and BitmapOrPaths
 * 'outer_rel' is the outer relation when we are considering using the bitmap
 *		scan as the inside of a nestloop join (hence, some of the indexQuals
 *		are join clauses, and we should expect repeated scans of the table);
 *		NULL for a plain bitmap scan
 *
 * Note: if this is a join inner path, the component IndexPaths in bitmapqual
 * should have been costed accordingly.
 */
void
cost_bitmap_heap_scan(Path *path, PlannerInfo *root, RelOptInfo *baserel,
					  Path *bitmapqual, RelOptInfo *outer_rel)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		indexTotalCost;
	Selectivity indexSelectivity;
	Cost		cpu_per_tuple;
	Cost		cost_per_page;
	double		tuples_fetched;
	double		pages_fetched;
	double		T;

	/* Should only be applied to base relations */
	Assert(IsA(baserel, RelOptInfo));
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	if (!enable_bitmapscan)
		startup_cost += disable_cost;

	/*
	 * Fetch total cost of obtaining the bitmap, as well as its total
	 * selectivity.
	 */
	cost_bitmap_tree_node(bitmapqual, &indexTotalCost, &indexSelectivity);

	startup_cost += indexTotalCost;

	/*
	 * Estimate number of main-table pages fetched.
	 */
	tuples_fetched = clamp_row_est(indexSelectivity * baserel->tuples);

	T = (baserel->pages > 1) ? (double) baserel->pages : 1.0;

	if (outer_rel != NULL && outer_rel->rows > 1)
	{
		/*
		 * For repeated bitmap scans, scale up the number of tuples fetched in
		 * the Mackert and Lohman formula by the number of scans, so that we
		 * estimate the number of pages fetched by all the scans. Then
		 * pro-rate for one scan.
		 */
		double		num_scans = outer_rel->rows;

		pages_fetched = index_pages_fetched(tuples_fetched * num_scans,
											baserel->pages,
											get_indexpath_pages(bitmapqual),
											root);
		pages_fetched /= num_scans;
	}
	else
	{
		/*
		 * For a single scan, the number of heap pages that need to be fetched
		 * is the same as the Mackert and Lohman formula for the case T <= b
		 * (ie, no re-reads needed).
		 */
		pages_fetched = (2.0 * T * tuples_fetched) / (2.0 * T + tuples_fetched);
	}
	if (pages_fetched >= T)
		pages_fetched = T;
	else
		pages_fetched = ceil(pages_fetched);

	/*
	 * For small numbers of pages we should charge random_page_cost apiece,
	 * while if nearly all the table's pages are being read, it's more
	 * appropriate to charge seq_page_cost apiece.	The effect is nonlinear,
	 * too. For lack of a better idea, interpolate like this to determine the
	 * cost per page.
	 */
	if (pages_fetched >= 2.0)
		cost_per_page = random_page_cost -
			(random_page_cost - seq_page_cost) * sqrt(pages_fetched / T);
	else
		cost_per_page = random_page_cost;

	run_cost += pages_fetched * cost_per_page;

	/*
	 * Estimate CPU costs per tuple.
	 *
	 * Often the indexquals don't need to be rechecked at each tuple ... but
	 * not always, especially not if there are enough tuples involved that the
	 * bitmaps become lossy.  For the moment, just assume they will be
	 * rechecked always.
	 */
	startup_cost += baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;

	run_cost += cpu_per_tuple * tuples_fetched;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_bitmap_tree_node
 *		Extract cost and selectivity from a bitmap tree node (index/and/or)
 */
void
cost_bitmap_tree_node(Path *path, Cost *cost, Selectivity *selec)
{
	if (IsA(path, IndexPath))
	{
		*cost = ((IndexPath *) path)->indextotalcost;
		*selec = ((IndexPath *) path)->indexselectivity;
		/*
		 * Charge a small amount per retrieved tuple to reflect the costs of
		 * manipulating the bitmap.  This is mostly to make sure that a bitmap
		 * scan doesn't look to be the same cost as an indexscan to retrieve
		 * a single tuple.
		 */
		*cost += 0.1 * cpu_operator_cost * ((IndexPath *) path)->rows;
	}
	else if (IsA(path, BitmapAndPath))
	{
		*cost = path->total_cost;
		*selec = ((BitmapAndPath *) path)->bitmapselectivity;
	}
	else if (IsA(path, BitmapOrPath))
	{
		*cost = path->total_cost;
		*selec = ((BitmapOrPath *) path)->bitmapselectivity;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d", nodeTag(path));
		*cost = *selec = 0;		/* keep compiler quiet */
	}
}

/*
 * cost_bitmap_and_node
 *		Estimate the cost of a BitmapAnd node
 *
 * Note that this considers only the costs of index scanning and bitmap
 * creation, not the eventual heap access.	In that sense the object isn't
 * truly a Path, but it has enough path-like properties (costs in particular)
 * to warrant treating it as one.
 */
void
cost_bitmap_and_node(BitmapAndPath *path, PlannerInfo *root)
{
	Cost		totalCost;
	Selectivity selec;
	ListCell   *l;

	/*
	 * We estimate AND selectivity on the assumption that the inputs are
	 * independent.  This is probably often wrong, but we don't have the info
	 * to do better.
	 *
	 * The runtime cost of the BitmapAnd itself is estimated at 100x
	 * cpu_operator_cost for each tbm_intersect needed.  Probably too small,
	 * definitely too simplistic?
	 */
	totalCost = 0.0;
	selec = 1.0;
	foreach(l, path->bitmapquals)
	{
		Path	   *subpath = (Path *) lfirst(l);
		Cost		subCost;
		Selectivity subselec;

		cost_bitmap_tree_node(subpath, &subCost, &subselec);

		selec *= subselec;

		totalCost += subCost;
		if (l != list_head(path->bitmapquals))
			totalCost += 100.0 * cpu_operator_cost;
	}
	path->bitmapselectivity = selec;
	path->path.startup_cost = totalCost;
	path->path.total_cost = totalCost;
}

/*
 * cost_bitmap_or_node
 *		Estimate the cost of a BitmapOr node
 *
 * See comments for cost_bitmap_and_node.
 */
void
cost_bitmap_or_node(BitmapOrPath *path, PlannerInfo *root)
{
	Cost		totalCost;
	Selectivity selec;
	ListCell   *l;

	/*
	 * We estimate OR selectivity on the assumption that the inputs are
	 * non-overlapping, since that's often the case in "x IN (list)" type
	 * situations.	Of course, we clamp to 1.0 at the end.
	 *
	 * The runtime cost of the BitmapOr itself is estimated at 100x
	 * cpu_operator_cost for each tbm_union needed.  Probably too small,
	 * definitely too simplistic?  We are aware that the tbm_unions are
	 * optimized out when the inputs are BitmapIndexScans.
	 */
	totalCost = 0.0;
	selec = 0.0;
	foreach(l, path->bitmapquals)
	{
		Path	   *subpath = (Path *) lfirst(l);
		Cost		subCost;
		Selectivity subselec;

		cost_bitmap_tree_node(subpath, &subCost, &subselec);

		selec += subselec;

		totalCost += subCost;
		if (l != list_head(path->bitmapquals) &&
			!IsA(subpath, IndexPath))
			totalCost += 100.0 * cpu_operator_cost;
	}
	path->bitmapselectivity = Min(selec, 1.0);
	path->path.startup_cost = totalCost;
	path->path.total_cost = totalCost;
}

/*
 * cost_tidscan
 *	  Determines and returns the cost of scanning a relation using TIDs.
 */
void
cost_tidscan(Path *path, PlannerInfo *root,
			 RelOptInfo *baserel, List *tidquals)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;
	int			ntuples;
	ListCell   *l;

	/* Should only be applied to base relations */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	if (!enable_tidscan)
		startup_cost += disable_cost;

	/* Count how many tuples we expect to retrieve */
	ntuples = 0;
	foreach(l, tidquals)
	{
		if (IsA(lfirst(l), ScalarArrayOpExpr))
		{
			/* Each element of the array yields 1 tuple */
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) lfirst(l);
			Node	   *arraynode = (Node *) lsecond(saop->args);

			ntuples += estimate_array_length(arraynode);
		}
		else
		{
			/* It's just CTID = something, count 1 tuple */
			ntuples++;
		}
	}

	/* disk costs --- assume each tuple on a different page */
	run_cost += random_page_cost * ntuples;

	/* CPU costs */
	startup_cost += baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * ntuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_subqueryscan
 *	  Determines and returns the cost of scanning a subquery RTE.
 */
void
cost_subqueryscan(Path *path, RelOptInfo *baserel)
{
	Cost		startup_cost;
	Cost		run_cost;
	Cost		cpu_per_tuple;

	/* Should only be applied to base relations that are subqueries */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_SUBQUERY);

	/*
	 * Cost of path is cost of evaluating the subplan, plus cost of evaluating
	 * any restriction clauses that will be attached to the SubqueryScan node,
	 * plus cpu_tuple_cost to account for selection and projection overhead.
	 */
	path->startup_cost = baserel->subplan->startup_cost;
	path->total_cost = baserel->subplan->total_cost;

	startup_cost = baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
	run_cost = cpu_per_tuple * baserel->tuples;

	path->startup_cost += startup_cost;
	path->total_cost += startup_cost + run_cost;
}

/*
 * cost_functionscan
 *	  Determines and returns the cost of scanning a function RTE.
 */
void
cost_functionscan(Path *path, PlannerInfo *root, RelOptInfo *baserel)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;

	/* Should only be applied to base relations that are functions */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_FUNCTION);

	/*
	 * For now, estimate function's cost at one operator eval per function
	 * call.  Someday we should revive the function cost estimate columns in
	 * pg_proc...
	 */
	cpu_per_tuple = cpu_operator_cost;

	/* Add scanning CPU costs */
	startup_cost += baserel->baserestrictcost.startup;
	cpu_per_tuple += cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_valuesscan
 *	  Determines and returns the cost of scanning a VALUES RTE.
 */
void
cost_valuesscan(Path *path, PlannerInfo *root, RelOptInfo *baserel)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;

	/* Should only be applied to base relations that are values lists */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_VALUES);

	/*
	 * For now, estimate list evaluation cost at one operator eval per list
	 * (probably pretty bogus, but is it worth being smarter?)
	 */
	cpu_per_tuple = cpu_operator_cost;

	/* Add scanning CPU costs */
	startup_cost += baserel->baserestrictcost.startup;
	cpu_per_tuple += cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_sort
 *	  Determines and returns the cost of sorting a relation, including
 *	  the cost of reading the input data.
 *
 * If the total volume of data to sort is less than work_mem, we will do
 * an in-memory sort, which requires no I/O and about t*log2(t) tuple
 * comparisons for t tuples.
 *
 * If the total volume exceeds work_mem, we switch to a tape-style merge
 * algorithm.  There will still be about t*log2(t) tuple comparisons in
 * total, but we will also need to write and read each tuple once per
 * merge pass.	We expect about ceil(logM(r)) merge passes where r is the
 * number of initial runs formed and M is the merge order used by tuplesort.c.
 * Since the average initial run should be about twice work_mem, we have
 *		disk traffic = 2 * relsize * ceil(logM(p / (2*work_mem)))
 *		cpu = comparison_cost * t * log2(t)
 *
 * The disk traffic is assumed to be 3/4ths sequential and 1/4th random
 * accesses (XXX can't we refine that guess?)
 *
 * We charge two operator evals per tuple comparison, which should be in
 * the right ballpark in most cases.
 *
 * 'pathkeys' is a list of sort keys
 * 'input_cost' is the total cost for reading the input data
 * 'tuples' is the number of tuples in the relation
 * 'width' is the average tuple width in bytes
 *
 * NOTE: some callers currently pass NIL for pathkeys because they
 * can't conveniently supply the sort keys.  Since this routine doesn't
 * currently do anything with pathkeys anyway, that doesn't matter...
 * but if it ever does, it should react gracefully to lack of key data.
 * (Actually, the thing we'd most likely be interested in is just the number
 * of sort keys, which all callers *could* supply.)
 */
void
cost_sort(Path *path, PlannerInfo *root,
		  List *pathkeys, Cost input_cost, double tuples, int width)
{
	Cost		startup_cost = input_cost;
	Cost		run_cost = 0;
	double		nbytes = relation_byte_size(tuples, width);
	long		work_mem_bytes = work_mem * 1024L;

	if (!enable_sort)
		startup_cost += disable_cost;

	/*
	 * We want to be sure the cost of a sort is never estimated as zero, even
	 * if passed-in tuple count is zero.  Besides, mustn't do log(0)...
	 */
	if (tuples < 2.0)
		tuples = 2.0;

	/*
	 * CPU costs
	 *
	 * Assume about two operator evals per tuple comparison and N log2 N
	 * comparisons
	 */
	startup_cost += 2.0 * cpu_operator_cost * tuples * LOG2(tuples);

	/* disk costs */
	if (nbytes > work_mem_bytes)
	{
		double		npages = ceil(nbytes / BLCKSZ);
		double		nruns = (nbytes / work_mem_bytes) * 0.5;
		double		mergeorder = tuplesort_merge_order(work_mem_bytes);
		double		log_runs;
		double		npageaccesses;

		/* Compute logM(r) as log(r) / log(M) */
		if (nruns > mergeorder)
			log_runs = ceil(log(nruns) / log(mergeorder));
		else
			log_runs = 1.0;
		npageaccesses = 2.0 * npages * log_runs;
		/* Assume 3/4ths of accesses are sequential, 1/4th are not */
		startup_cost += npageaccesses *
			(seq_page_cost * 0.75 + random_page_cost * 0.25);
	}

	/*
	 * Also charge a small amount (arbitrarily set equal to operator cost) per
	 * extracted tuple.
	 */
	run_cost += cpu_operator_cost * tuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_material
 *	  Determines and returns the cost of materializing a relation, including
 *	  the cost of reading the input data.
 *
 * If the total volume of data to materialize exceeds work_mem, we will need
 * to write it to disk, so the cost is much higher in that case.
 */
void
cost_material(Path *path,
			  Cost input_cost, double tuples, int width)
{
	Cost		startup_cost = input_cost;
	Cost		run_cost = 0;
	double		nbytes = relation_byte_size(tuples, width);
	long		work_mem_bytes = work_mem * 1024L;

	/* disk costs */
	if (nbytes > work_mem_bytes)
	{
		double		npages = ceil(nbytes / BLCKSZ);

		/* We'll write during startup and read during retrieval */
		startup_cost += seq_page_cost * npages;
		run_cost += seq_page_cost * npages;
	}

	/*
	 * Charge a very small amount per inserted tuple, to reflect bookkeeping
	 * costs.  We use cpu_tuple_cost/10 for this.  This is needed to break the
	 * tie that would otherwise exist between nestloop with A outer,
	 * materialized B inner and nestloop with B outer, materialized A inner.
	 * The extra cost ensures we'll prefer materializing the smaller rel.
	 */
	startup_cost += cpu_tuple_cost * 0.1 * tuples;

	/*
	 * Also charge a small amount per extracted tuple.	We use cpu_tuple_cost
	 * so that it doesn't appear worthwhile to materialize a bare seqscan.
	 */
	run_cost += cpu_tuple_cost * tuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_agg
 *		Determines and returns the cost of performing an Agg plan node,
 *		including the cost of its input.
 *
 * Note: when aggstrategy == AGG_SORTED, caller must ensure that input costs
 * are for appropriately-sorted input.
 */
void
cost_agg(Path *path, PlannerInfo *root,
		 AggStrategy aggstrategy, int numAggs,
		 int numGroupCols, double numGroups,
		 Cost input_startup_cost, Cost input_total_cost,
		 double input_tuples)
{
	Cost		startup_cost;
	Cost		total_cost;

	/*
	 * We charge one cpu_operator_cost per aggregate function per input tuple,
	 * and another one per output tuple (corresponding to transfn and finalfn
	 * calls respectively).  If we are grouping, we charge an additional
	 * cpu_operator_cost per grouping column per input tuple for grouping
	 * comparisons.
	 *
	 * We will produce a single output tuple if not grouping, and a tuple per
	 * group otherwise.  We charge cpu_tuple_cost for each output tuple.
	 *
	 * Note: in this cost model, AGG_SORTED and AGG_HASHED have exactly the
	 * same total CPU cost, but AGG_SORTED has lower startup cost.	If the
	 * input path is already sorted appropriately, AGG_SORTED should be
	 * preferred (since it has no risk of memory overflow).  This will happen
	 * as long as the computed total costs are indeed exactly equal --- but if
	 * there's roundoff error we might do the wrong thing.  So be sure that
	 * the computations below form the same intermediate values in the same
	 * order.
	 */
	if (aggstrategy == AGG_PLAIN)
	{
		startup_cost = input_total_cost;
		startup_cost += cpu_operator_cost * (input_tuples + 1) * numAggs;
		/* we aren't grouping */
		total_cost = startup_cost + cpu_tuple_cost;
	}
	else if (aggstrategy == AGG_SORTED)
	{
		/* Here we are able to deliver output on-the-fly */
		startup_cost = input_startup_cost;
		total_cost = input_total_cost;
		/* calcs phrased this way to match HASHED case, see note above */
		total_cost += cpu_operator_cost * input_tuples * numGroupCols;
		total_cost += cpu_operator_cost * input_tuples * numAggs;
		total_cost += cpu_operator_cost * numGroups * numAggs;
		total_cost += cpu_tuple_cost * numGroups;
	}
	else
	{
		/* must be AGG_HASHED */
		startup_cost = input_total_cost;
		startup_cost += cpu_operator_cost * input_tuples * numGroupCols;
		startup_cost += cpu_operator_cost * input_tuples * numAggs;
		total_cost = startup_cost;
		total_cost += cpu_operator_cost * numGroups * numAggs;
		total_cost += cpu_tuple_cost * numGroups;
	}

	path->startup_cost = startup_cost;
	path->total_cost = total_cost;
}

/*
 * cost_group
 *		Determines and returns the cost of performing a Group plan node,
 *		including the cost of its input.
 *
 * Note: caller must ensure that input costs are for appropriately-sorted
 * input.
 */
void
cost_group(Path *path, PlannerInfo *root,
		   int numGroupCols, double numGroups,
		   Cost input_startup_cost, Cost input_total_cost,
		   double input_tuples)
{
	Cost		startup_cost;
	Cost		total_cost;

	startup_cost = input_startup_cost;
	total_cost = input_total_cost;

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We assume
	 * all columns get compared at most of the tuples.
	 */
	total_cost += cpu_operator_cost * input_tuples * numGroupCols;

	path->startup_cost = startup_cost;
	path->total_cost = total_cost;
}

/*
 * If a nestloop's inner path is an indexscan, be sure to use its estimated
 * output row count, which may be lower than the restriction-clause-only row
 * count of its parent.  (We don't include this case in the PATH_ROWS macro
 * because it applies *only* to a nestloop's inner relation.)  We have to
 * be prepared to recurse through Append nodes in case of an appendrel.
 */
static double
nestloop_inner_path_rows(Path *path)
{
	double		result;

	if (IsA(path, IndexPath))
		result = ((IndexPath *) path)->rows;
	else if (IsA(path, BitmapHeapPath))
		result = ((BitmapHeapPath *) path)->rows;
	else if (IsA(path, AppendPath))
	{
		ListCell   *l;

		result = 0;
		foreach(l, ((AppendPath *) path)->subpaths)
		{
			result += nestloop_inner_path_rows((Path *) lfirst(l));
		}
	}
	else
		result = PATH_ROWS(path);

	return result;
}

/*
 * cost_nestloop
 *	  Determines and returns the cost of joining two relations using the
 *	  nested loop algorithm.
 *
 * 'path' is already filled in except for the cost fields
 */
void
cost_nestloop(NestPath *path, PlannerInfo *root)
{
	Path	   *outer_path = path->outerjoinpath;
	Path	   *inner_path = path->innerjoinpath;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;
	QualCost	restrict_qual_cost;
	double		outer_path_rows = PATH_ROWS(outer_path);
	double		inner_path_rows = nestloop_inner_path_rows(inner_path);
	double		ntuples;
	Selectivity joininfactor;

	if (!enable_nestloop)
		startup_cost += disable_cost;

	/*
	 * If we're doing JOIN_IN then we will stop scanning inner tuples for an
	 * outer tuple as soon as we have one match.  Account for the effects of
	 * this by scaling down the cost estimates in proportion to the JOIN_IN
	 * selectivity.  (This assumes that all the quals attached to the join are
	 * IN quals, which should be true.)
	 */
	joininfactor = join_in_selectivity(path, root);

	/* cost of source data */

	/*
	 * NOTE: clearly, we must pay both outer and inner paths' startup_cost
	 * before we can start returning tuples, so the join's startup cost is
	 * their sum.  What's not so clear is whether the inner path's
	 * startup_cost must be paid again on each rescan of the inner path. This
	 * is not true if the inner path is materialized or is a hashjoin, but
	 * probably is true otherwise.
	 */
	startup_cost += outer_path->startup_cost + inner_path->startup_cost;
	run_cost += outer_path->total_cost - outer_path->startup_cost;
	if (IsA(inner_path, MaterialPath) ||
		IsA(inner_path, HashPath))
	{
		/* charge only run cost for each iteration of inner path */
	}
	else
	{
		/*
		 * charge startup cost for each iteration of inner path, except we
		 * already charged the first startup_cost in our own startup
		 */
		run_cost += (outer_path_rows - 1) * inner_path->startup_cost;
	}
	run_cost += outer_path_rows *
		(inner_path->total_cost - inner_path->startup_cost) * joininfactor;

	/*
	 * Compute number of tuples processed (not number emitted!)
	 */
	ntuples = outer_path_rows * inner_path_rows * joininfactor;

	/* CPU costs */
	cost_qual_eval(&restrict_qual_cost, path->joinrestrictinfo);
	startup_cost += restrict_qual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + restrict_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * ntuples;

	path->path.startup_cost = startup_cost;
	path->path.total_cost = startup_cost + run_cost;
}

/*
 * cost_mergejoin
 *	  Determines and returns the cost of joining two relations using the
 *	  merge join algorithm.
 *
 * 'path' is already filled in except for the cost fields
 *
 * Notes: path's mergeclauses should be a subset of the joinrestrictinfo list;
 * outersortkeys and innersortkeys are lists of the keys to be used
 * to sort the outer and inner relations, or NIL if no explicit
 * sort is needed because the source path is already ordered.
 */
void
cost_mergejoin(MergePath *path, PlannerInfo *root)
{
	Path	   *outer_path = path->jpath.outerjoinpath;
	Path	   *inner_path = path->jpath.innerjoinpath;
	List	   *mergeclauses = path->path_mergeclauses;
	List	   *outersortkeys = path->outersortkeys;
	List	   *innersortkeys = path->innersortkeys;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;
	Selectivity merge_selec;
	QualCost	merge_qual_cost;
	QualCost	qp_qual_cost;
	RestrictInfo *firstclause;
	double		outer_path_rows = PATH_ROWS(outer_path);
	double		inner_path_rows = PATH_ROWS(inner_path);
	double		outer_rows,
				inner_rows;
	double		mergejointuples,
				rescannedtuples;
	double		rescanratio;
	Selectivity outerscansel,
				innerscansel;
	Selectivity joininfactor;
	Path		sort_path;		/* dummy for result of cost_sort */

	if (!enable_mergejoin)
		startup_cost += disable_cost;

	/*
	 * Compute cost and selectivity of the mergequals and qpquals (other
	 * restriction clauses) separately.  We use approx_selectivity here for
	 * speed --- in most cases, any errors won't affect the result much.
	 *
	 * Note: it's probably bogus to use the normal selectivity calculation
	 * here when either the outer or inner path is a UniquePath.
	 */
	merge_selec = approx_selectivity(root, mergeclauses,
									 path->jpath.jointype);
	cost_qual_eval(&merge_qual_cost, mergeclauses);
	cost_qual_eval(&qp_qual_cost, path->jpath.joinrestrictinfo);
	qp_qual_cost.startup -= merge_qual_cost.startup;
	qp_qual_cost.per_tuple -= merge_qual_cost.per_tuple;

	/* approx # tuples passing the merge quals */
	mergejointuples = clamp_row_est(merge_selec * outer_path_rows * inner_path_rows);

	/*
	 * When there are equal merge keys in the outer relation, the mergejoin
	 * must rescan any matching tuples in the inner relation. This means
	 * re-fetching inner tuples.  Our cost model for this is that a re-fetch
	 * costs the same as an original fetch, which is probably an overestimate;
	 * but on the other hand we ignore the bookkeeping costs of mark/restore.
	 * Not clear if it's worth developing a more refined model.
	 *
	 * The number of re-fetches can be estimated approximately as size of
	 * merge join output minus size of inner relation.	Assume that the
	 * distinct key values are 1, 2, ..., and denote the number of values of
	 * each key in the outer relation as m1, m2, ...; in the inner relation,
	 * n1, n2, ... Then we have
	 *
	 * size of join = m1 * n1 + m2 * n2 + ...
	 *
	 * number of rescanned tuples = (m1 - 1) * n1 + (m2 - 1) * n2 + ... = m1 *
	 * n1 + m2 * n2 + ... - (n1 + n2 + ...) = size of join - size of inner
	 * relation
	 *
	 * This equation works correctly for outer tuples having no inner match
	 * (nk = 0), but not for inner tuples having no outer match (mk = 0); we
	 * are effectively subtracting those from the number of rescanned tuples,
	 * when we should not.	Can we do better without expensive selectivity
	 * computations?
	 */
	if (IsA(outer_path, UniquePath))
		rescannedtuples = 0;
	else
	{
		rescannedtuples = mergejointuples - inner_path_rows;
		/* Must clamp because of possible underestimate */
		if (rescannedtuples < 0)
			rescannedtuples = 0;
	}
	/* We'll inflate inner run cost this much to account for rescanning */
	rescanratio = 1.0 + (rescannedtuples / inner_path_rows);

	/*
	 * A merge join will stop as soon as it exhausts either input stream
	 * (unless it's an outer join, in which case the outer side has to be
	 * scanned all the way anyway).  Estimate fraction of the left and right
	 * inputs that will actually need to be scanned. We use only the first
	 * (most significant) merge clause for this purpose.
	 *
	 * Since this calculation is somewhat expensive, and will be the same for
	 * all mergejoin paths associated with the merge clause, we cache the
	 * results in the RestrictInfo node.
	 */
	if (mergeclauses && path->jpath.jointype != JOIN_FULL)
	{
		firstclause = (RestrictInfo *) linitial(mergeclauses);
		if (firstclause->left_mergescansel < 0) /* not computed yet? */
			mergejoinscansel(root, (Node *) firstclause->clause,
							 &firstclause->left_mergescansel,
							 &firstclause->right_mergescansel);

		if (bms_is_subset(firstclause->left_relids, outer_path->parent->relids))
		{
			/* left side of clause is outer */
			outerscansel = firstclause->left_mergescansel;
			innerscansel = firstclause->right_mergescansel;
		}
		else
		{
			/* left side of clause is inner */
			outerscansel = firstclause->right_mergescansel;
			innerscansel = firstclause->left_mergescansel;
		}
		if (path->jpath.jointype == JOIN_LEFT)
			outerscansel = 1.0;
		else if (path->jpath.jointype == JOIN_RIGHT)
			innerscansel = 1.0;
	}
	else
	{
		/* cope with clauseless or full mergejoin */
		outerscansel = innerscansel = 1.0;
	}

	/* convert selectivity to row count; must scan at least one row */
	outer_rows = clamp_row_est(outer_path_rows * outerscansel);
	inner_rows = clamp_row_est(inner_path_rows * innerscansel);

	/*
	 * Readjust scan selectivities to account for above rounding.  This is
	 * normally an insignificant effect, but when there are only a few rows in
	 * the inputs, failing to do this makes for a large percentage error.
	 */
	outerscansel = outer_rows / outer_path_rows;
	innerscansel = inner_rows / inner_path_rows;

	/* cost of source data */

	if (outersortkeys)			/* do we need to sort outer? */
	{
		cost_sort(&sort_path,
				  root,
				  outersortkeys,
				  outer_path->total_cost,
				  outer_path_rows,
				  outer_path->parent->width);
		startup_cost += sort_path.startup_cost;
		run_cost += (sort_path.total_cost - sort_path.startup_cost)
			* outerscansel;
	}
	else
	{
		startup_cost += outer_path->startup_cost;
		run_cost += (outer_path->total_cost - outer_path->startup_cost)
			* outerscansel;
	}

	if (innersortkeys)			/* do we need to sort inner? */
	{
		cost_sort(&sort_path,
				  root,
				  innersortkeys,
				  inner_path->total_cost,
				  inner_path_rows,
				  inner_path->parent->width);
		startup_cost += sort_path.startup_cost;
		run_cost += (sort_path.total_cost - sort_path.startup_cost)
			* innerscansel * rescanratio;
	}
	else
	{
		startup_cost += inner_path->startup_cost;
		run_cost += (inner_path->total_cost - inner_path->startup_cost)
			* innerscansel * rescanratio;
	}

	/* CPU costs */

	/*
	 * If we're doing JOIN_IN then we will stop outputting inner tuples for an
	 * outer tuple as soon as we have one match.  Account for the effects of
	 * this by scaling down the cost estimates in proportion to the expected
	 * output size.  (This assumes that all the quals attached to the join are
	 * IN quals, which should be true.)
	 */
	joininfactor = join_in_selectivity(&path->jpath, root);

	/*
	 * The number of tuple comparisons needed is approximately number of outer
	 * rows plus number of inner rows plus number of rescanned tuples (can we
	 * refine this?).  At each one, we need to evaluate the mergejoin quals.
	 * NOTE: JOIN_IN mode does not save any work here, so do NOT include
	 * joininfactor.
	 */
	startup_cost += merge_qual_cost.startup;
	run_cost += merge_qual_cost.per_tuple *
		(outer_rows + inner_rows * rescanratio);

	/*
	 * For each tuple that gets through the mergejoin proper, we charge
	 * cpu_tuple_cost plus the cost of evaluating additional restriction
	 * clauses that are to be applied at the join.	(This is pessimistic since
	 * not all of the quals may get evaluated at each tuple.)  This work is
	 * skipped in JOIN_IN mode, so apply the factor.
	 */
	startup_cost += qp_qual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qp_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * mergejointuples * joininfactor;

	path->jpath.path.startup_cost = startup_cost;
	path->jpath.path.total_cost = startup_cost + run_cost;
}

/*
 * cost_hashjoin
 *	  Determines and returns the cost of joining two relations using the
 *	  hash join algorithm.
 *
 * 'path' is already filled in except for the cost fields
 *
 * Note: path's hashclauses should be a subset of the joinrestrictinfo list
 */
void
cost_hashjoin(HashPath *path, PlannerInfo *root)
{
	Path	   *outer_path = path->jpath.outerjoinpath;
	Path	   *inner_path = path->jpath.innerjoinpath;
	List	   *hashclauses = path->path_hashclauses;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;
	Selectivity hash_selec;
	QualCost	hash_qual_cost;
	QualCost	qp_qual_cost;
	double		hashjointuples;
	double		outer_path_rows = PATH_ROWS(outer_path);
	double		inner_path_rows = PATH_ROWS(inner_path);
	int			num_hashclauses = list_length(hashclauses);
	int			numbuckets;
	int			numbatches;
	double		virtualbuckets;
	Selectivity innerbucketsize;
	Selectivity joininfactor;
	ListCell   *hcl;

	if (!enable_hashjoin)
		startup_cost += disable_cost;

	/*
	 * Compute cost and selectivity of the hashquals and qpquals (other
	 * restriction clauses) separately.  We use approx_selectivity here for
	 * speed --- in most cases, any errors won't affect the result much.
	 *
	 * Note: it's probably bogus to use the normal selectivity calculation
	 * here when either the outer or inner path is a UniquePath.
	 */
	hash_selec = approx_selectivity(root, hashclauses,
									path->jpath.jointype);
	cost_qual_eval(&hash_qual_cost, hashclauses);
	cost_qual_eval(&qp_qual_cost, path->jpath.joinrestrictinfo);
	qp_qual_cost.startup -= hash_qual_cost.startup;
	qp_qual_cost.per_tuple -= hash_qual_cost.per_tuple;

	/* approx # tuples passing the hash quals */
	hashjointuples = clamp_row_est(hash_selec * outer_path_rows * inner_path_rows);

	/* cost of source data */
	startup_cost += outer_path->startup_cost;
	run_cost += outer_path->total_cost - outer_path->startup_cost;
	startup_cost += inner_path->total_cost;

	/*
	 * Cost of computing hash function: must do it once per input tuple. We
	 * charge one cpu_operator_cost for each column's hash function.  Also,
	 * tack on one cpu_tuple_cost per inner row, to model the costs of
	 * inserting the row into the hashtable.
	 *
	 * XXX when a hashclause is more complex than a single operator, we really
	 * should charge the extra eval costs of the left or right side, as
	 * appropriate, here.  This seems more work than it's worth at the moment.
	 */
	startup_cost += (cpu_operator_cost * num_hashclauses + cpu_tuple_cost)
		* inner_path_rows;
	run_cost += cpu_operator_cost * num_hashclauses * outer_path_rows;

	/* Get hash table size that executor would use for inner relation */
	ExecChooseHashTableSize(inner_path_rows,
							inner_path->parent->width,
							&numbuckets,
							&numbatches);
	virtualbuckets = (double) numbuckets *(double) numbatches;

	/*
	 * Determine bucketsize fraction for inner relation.  We use the smallest
	 * bucketsize estimated for any individual hashclause; this is undoubtedly
	 * conservative.
	 *
	 * BUT: if inner relation has been unique-ified, we can assume it's good
	 * for hashing.  This is important both because it's the right answer, and
	 * because we avoid contaminating the cache with a value that's wrong for
	 * non-unique-ified paths.
	 */
	if (IsA(inner_path, UniquePath))
		innerbucketsize = 1.0 / virtualbuckets;
	else
	{
		innerbucketsize = 1.0;
		foreach(hcl, hashclauses)
		{
			RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(hcl);
			Selectivity thisbucketsize;

			Assert(IsA(restrictinfo, RestrictInfo));

			/*
			 * First we have to figure out which side of the hashjoin clause
			 * is the inner side.
			 *
			 * Since we tend to visit the same clauses over and over when
			 * planning a large query, we cache the bucketsize estimate in the
			 * RestrictInfo node to avoid repeated lookups of statistics.
			 */
			if (bms_is_subset(restrictinfo->right_relids,
							  inner_path->parent->relids))
			{
				/* righthand side is inner */
				thisbucketsize = restrictinfo->right_bucketsize;
				if (thisbucketsize < 0)
				{
					/* not cached yet */
					thisbucketsize =
						estimate_hash_bucketsize(root,
										   get_rightop(restrictinfo->clause),
												 virtualbuckets);
					restrictinfo->right_bucketsize = thisbucketsize;
				}
			}
			else
			{
				Assert(bms_is_subset(restrictinfo->left_relids,
									 inner_path->parent->relids));
				/* lefthand side is inner */
				thisbucketsize = restrictinfo->left_bucketsize;
				if (thisbucketsize < 0)
				{
					/* not cached yet */
					thisbucketsize =
						estimate_hash_bucketsize(root,
											get_leftop(restrictinfo->clause),
												 virtualbuckets);
					restrictinfo->left_bucketsize = thisbucketsize;
				}
			}

			if (innerbucketsize > thisbucketsize)
				innerbucketsize = thisbucketsize;
		}
	}

	/*
	 * If inner relation is too big then we will need to "batch" the join,
	 * which implies writing and reading most of the tuples to disk an extra
	 * time.  Charge seq_page_cost per page, since the I/O should be nice and
	 * sequential.  Writing the inner rel counts as startup cost,
	 * all the rest as run cost.
	 */
	if (numbatches > 1)
	{
		double		outerpages = page_size(outer_path_rows,
										   outer_path->parent->width);
		double		innerpages = page_size(inner_path_rows,
										   inner_path->parent->width);

		startup_cost += seq_page_cost * innerpages;
		run_cost += seq_page_cost * (innerpages + 2 * outerpages);
	}

	/* CPU costs */

	/*
	 * If we're doing JOIN_IN then we will stop comparing inner tuples to an
	 * outer tuple as soon as we have one match.  Account for the effects of
	 * this by scaling down the cost estimates in proportion to the expected
	 * output size.  (This assumes that all the quals attached to the join are
	 * IN quals, which should be true.)
	 */
	joininfactor = join_in_selectivity(&path->jpath, root);

	/*
	 * The number of tuple comparisons needed is the number of outer tuples
	 * times the typical number of tuples in a hash bucket, which is the inner
	 * relation size times its bucketsize fraction.  At each one, we need to
	 * evaluate the hashjoin quals.  But actually, charging the full qual eval
	 * cost at each tuple is pessimistic, since we don't evaluate the quals
	 * unless the hash values match exactly.  For lack of a better idea, halve
	 * the cost estimate to allow for that.
	 */
	startup_cost += hash_qual_cost.startup;
	run_cost += hash_qual_cost.per_tuple *
		outer_path_rows * clamp_row_est(inner_path_rows * innerbucketsize) *
		joininfactor * 0.5;

	/*
	 * For each tuple that gets through the hashjoin proper, we charge
	 * cpu_tuple_cost plus the cost of evaluating additional restriction
	 * clauses that are to be applied at the join.	(This is pessimistic since
	 * not all of the quals may get evaluated at each tuple.)
	 */
	startup_cost += qp_qual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qp_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * hashjointuples * joininfactor;

	path->jpath.path.startup_cost = startup_cost;
	path->jpath.path.total_cost = startup_cost + run_cost;
}


/*
 * cost_qual_eval
 *		Estimate the CPU costs of evaluating a WHERE clause.
 *		The input can be either an implicitly-ANDed list of boolean
 *		expressions, or a list of RestrictInfo nodes.
 *		The result includes both a one-time (startup) component,
 *		and a per-evaluation component.
 */
void
cost_qual_eval(QualCost *cost, List *quals)
{
	ListCell   *l;

	cost->startup = 0;
	cost->per_tuple = 0;

	/* We don't charge any cost for the implicit ANDing at top level ... */

	foreach(l, quals)
	{
		Node	   *qual = (Node *) lfirst(l);

		/*
		 * RestrictInfo nodes contain an eval_cost field reserved for this
		 * routine's use, so that it's not necessary to evaluate the qual
		 * clause's cost more than once.  If the clause's cost hasn't been
		 * computed yet, the field's startup value will contain -1.
		 *
		 * If the RestrictInfo is marked pseudoconstant, it will be tested
		 * only once, so treat its cost as all startup cost.
		 */
		if (qual && IsA(qual, RestrictInfo))
		{
			RestrictInfo *rinfo = (RestrictInfo *) qual;

			if (rinfo->eval_cost.startup < 0)
			{
				rinfo->eval_cost.startup = 0;
				rinfo->eval_cost.per_tuple = 0;
				cost_qual_eval_walker((Node *) rinfo->clause,
									  &rinfo->eval_cost);
				if (rinfo->pseudoconstant)
				{
					/* count one execution during startup */
					rinfo->eval_cost.startup += rinfo->eval_cost.per_tuple;
					rinfo->eval_cost.per_tuple = 0;
				}
			}
			cost->startup += rinfo->eval_cost.startup;
			cost->per_tuple += rinfo->eval_cost.per_tuple;
		}
		else
		{
			/* If it's a bare expression, must always do it the hard way */
			cost_qual_eval_walker(qual, cost);
		}
	}
}

static bool
cost_qual_eval_walker(Node *node, QualCost *total)
{
	if (node == NULL)
		return false;

	/*
	 * Our basic strategy is to charge one cpu_operator_cost for each operator
	 * or function node in the given tree.	Vars and Consts are charged zero,
	 * and so are boolean operators (AND, OR, NOT). Simplistic, but a lot
	 * better than no model at all.
	 *
	 * Should we try to account for the possibility of short-circuit
	 * evaluation of AND/OR?
	 */
	if (IsA(node, FuncExpr) ||
		IsA(node, OpExpr) ||
		IsA(node, DistinctExpr) ||
		IsA(node, NullIfExpr))
		total->per_tuple += cpu_operator_cost;
	else if (IsA(node, ScalarArrayOpExpr))
	{
		/*
		 * Estimate that the operator will be applied to about half of the
		 * array elements before the answer is determined.
		 */
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;
		Node	   *arraynode = (Node *) lsecond(saop->args);

		total->per_tuple +=
			cpu_operator_cost * estimate_array_length(arraynode) * 0.5;
	}
	else if (IsA(node, RowCompareExpr))
	{
		/* Conservatively assume we will check all the columns */
		RowCompareExpr *rcexpr = (RowCompareExpr *) node;

		total->per_tuple += cpu_operator_cost * list_length(rcexpr->opnos);
	}
	else if (IsA(node, SubLink))
	{
		/* This routine should not be applied to un-planned expressions */
		elog(ERROR, "cannot handle unplanned sub-select");
	}
	else if (IsA(node, SubPlan))
	{
		/*
		 * A subplan node in an expression typically indicates that the
		 * subplan will be executed on each evaluation, so charge accordingly.
		 * (Sub-selects that can be executed as InitPlans have already been
		 * removed from the expression.)
		 *
		 * An exception occurs when we have decided we can implement the
		 * subplan by hashing.
		 */
		SubPlan    *subplan = (SubPlan *) node;
		Plan	   *plan = subplan->plan;

		if (subplan->useHashTable)
		{
			/*
			 * If we are using a hash table for the subquery outputs, then the
			 * cost of evaluating the query is a one-time cost. We charge one
			 * cpu_operator_cost per tuple for the work of loading the
			 * hashtable, too.
			 */
			total->startup += plan->total_cost +
				cpu_operator_cost * plan->plan_rows;

			/*
			 * The per-tuple costs include the cost of evaluating the lefthand
			 * expressions, plus the cost of probing the hashtable. Recursion
			 * into the testexpr will handle the lefthand expressions
			 * properly, and will count one cpu_operator_cost for each
			 * comparison operator.  That is probably too low for the probing
			 * cost, but it's hard to make a better estimate, so live with it
			 * for now.
			 */
		}
		else
		{
			/*
			 * Otherwise we will be rescanning the subplan output on each
			 * evaluation.	We need to estimate how much of the output we will
			 * actually need to scan.  NOTE: this logic should agree with the
			 * estimates used by make_subplan() in plan/subselect.c.
			 */
			Cost		plan_run_cost = plan->total_cost - plan->startup_cost;

			if (subplan->subLinkType == EXISTS_SUBLINK)
			{
				/* we only need to fetch 1 tuple */
				total->per_tuple += plan_run_cost / plan->plan_rows;
			}
			else if (subplan->subLinkType == ALL_SUBLINK ||
					 subplan->subLinkType == ANY_SUBLINK)
			{
				/* assume we need 50% of the tuples */
				total->per_tuple += 0.50 * plan_run_cost;
				/* also charge a cpu_operator_cost per row examined */
				total->per_tuple += 0.50 * plan->plan_rows * cpu_operator_cost;
			}
			else
			{
				/* assume we need all tuples */
				total->per_tuple += plan_run_cost;
			}

			/*
			 * Also account for subplan's startup cost. If the subplan is
			 * uncorrelated or undirect correlated, AND its topmost node is a
			 * Sort or Material node, assume that we'll only need to pay its
			 * startup cost once; otherwise assume we pay the startup cost
			 * every time.
			 */
			if (subplan->parParam == NIL &&
				(IsA(plan, Sort) ||
				 IsA(plan, Material)))
				total->startup += plan->startup_cost;
			else
				total->per_tuple += plan->startup_cost;
		}
	}

	return expression_tree_walker(node, cost_qual_eval_walker,
								  (void *) total);
}


/*
 * approx_selectivity
 *		Quick-and-dirty estimation of clause selectivities.
 *		The input can be either an implicitly-ANDed list of boolean
 *		expressions, or a list of RestrictInfo nodes (typically the latter).
 *
 * This is quick-and-dirty because we bypass clauselist_selectivity, and
 * simply multiply the independent clause selectivities together.  Now
 * clauselist_selectivity often can't do any better than that anyhow, but
 * for some situations (such as range constraints) it is smarter.  However,
 * we can't effectively cache the results of clauselist_selectivity, whereas
 * the individual clause selectivities can be and are cached.
 *
 * Since we are only using the results to estimate how many potential
 * output tuples are generated and passed through qpqual checking, it
 * seems OK to live with the approximation.
 */
static Selectivity
approx_selectivity(PlannerInfo *root, List *quals, JoinType jointype)
{
	Selectivity total = 1.0;
	ListCell   *l;

	foreach(l, quals)
	{
		Node	   *qual = (Node *) lfirst(l);

		/* Note that clause_selectivity will be able to cache its result */
		total *= clause_selectivity(root, qual, 0, jointype);
	}
	return total;
}


/*
 * set_baserel_size_estimates
 *		Set the size estimates for the given base relation.
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already.
 *
 * We set the following fields of the rel node:
 *	rows: the estimated number of output tuples (after applying
 *		  restriction clauses).
 *	width: the estimated average output tuple width in bytes.
 *	baserestrictcost: estimated cost of evaluating baserestrictinfo clauses.
 */
void
set_baserel_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	double		nrows;

	/* Should only be applied to base relations */
	Assert(rel->relid > 0);

	nrows = rel->tuples *
		clauselist_selectivity(root,
							   rel->baserestrictinfo,
							   0,
							   JOIN_INNER);

	rel->rows = clamp_row_est(nrows);

	cost_qual_eval(&rel->baserestrictcost, rel->baserestrictinfo);

	set_rel_width(root, rel);
}

/*
 * set_joinrel_size_estimates
 *		Set the size estimates for the given join relation.
 *
 * The rel's targetlist must have been constructed already, and a
 * restriction clause list that matches the given component rels must
 * be provided.
 *
 * Since there is more than one way to make a joinrel for more than two
 * base relations, the results we get here could depend on which component
 * rel pair is provided.  In theory we should get the same answers no matter
 * which pair is provided; in practice, since the selectivity estimation
 * routines don't handle all cases equally well, we might not.  But there's
 * not much to be done about it.  (Would it make sense to repeat the
 * calculations for each pair of input rels that's encountered, and somehow
 * average the results?  Probably way more trouble than it's worth.)
 *
 * It's important that the results for symmetric JoinTypes be symmetric,
 * eg, (rel1, rel2, JOIN_LEFT) should produce the same result as (rel2,
 * rel1, JOIN_RIGHT).  Also, JOIN_IN should produce the same result as
 * JOIN_UNIQUE_INNER, likewise JOIN_REVERSE_IN == JOIN_UNIQUE_OUTER.
 *
 * We set only the rows field here.  The width field was already set by
 * build_joinrel_tlist, and baserestrictcost is not used for join rels.
 */
void
set_joinrel_size_estimates(PlannerInfo *root, RelOptInfo *rel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel,
						   JoinType jointype,
						   List *restrictlist)
{
	Selectivity jselec;
	Selectivity pselec;
	double		nrows;
	UniquePath *upath;

	/*
	 * Compute joinclause selectivity.	Note that we are only considering
	 * clauses that become restriction clauses at this join level; we are not
	 * double-counting them because they were not considered in estimating the
	 * sizes of the component rels.
	 *
	 * For an outer join, we have to distinguish the selectivity of the
	 * join's own clauses (JOIN/ON conditions) from any clauses that were
	 * "pushed down".  For inner joins we just count them all as joinclauses.
	 */
	if (IS_OUTER_JOIN(jointype))
	{
		List	   *joinquals = NIL;
		List	   *pushedquals = NIL;
		ListCell   *l;

		/* Grovel through the clauses to separate into two lists */
		foreach(l, restrictlist)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			Assert(IsA(rinfo, RestrictInfo));
			if (rinfo->is_pushed_down)
				pushedquals = lappend(pushedquals, rinfo);
			else
				joinquals = lappend(joinquals, rinfo);
		}

		/* Get the separate selectivities */
		jselec = clauselist_selectivity(root,
										joinquals,
										0,
										jointype);
		pselec = clauselist_selectivity(root,
										pushedquals,
										0,
										jointype);

		/* Avoid leaking a lot of ListCells */
		list_free(joinquals);
		list_free(pushedquals);
	}
	else
	{
		jselec = clauselist_selectivity(root,
										restrictlist,
										0,
										jointype);
		pselec = 0.0;			/* not used, keep compiler quiet */
	}

	/*
	 * Basically, we multiply size of Cartesian product by selectivity.
	 *
	 * If we are doing an outer join, take that into account: the joinqual
	 * selectivity has to be clamped using the knowledge that the output must
	 * be at least as large as the non-nullable input.  However, any
	 * pushed-down quals are applied after the outer join, so their
	 * selectivity applies fully.
	 *
	 * For JOIN_IN and variants, the Cartesian product is figured with respect
	 * to a unique-ified input, and then we can clamp to the size of the other
	 * input.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			nrows = outer_rel->rows * inner_rel->rows * jselec;
			break;
		case JOIN_LEFT:
			nrows = outer_rel->rows * inner_rel->rows * jselec;
			if (nrows < outer_rel->rows)
				nrows = outer_rel->rows;
			nrows *= pselec;
			break;
		case JOIN_RIGHT:
			nrows = outer_rel->rows * inner_rel->rows * jselec;
			if (nrows < inner_rel->rows)
				nrows = inner_rel->rows;
			nrows *= pselec;
			break;
		case JOIN_FULL:
			nrows = outer_rel->rows * inner_rel->rows * jselec;
			if (nrows < outer_rel->rows)
				nrows = outer_rel->rows;
			if (nrows < inner_rel->rows)
				nrows = inner_rel->rows;
			nrows *= pselec;
			break;
		case JOIN_IN:
		case JOIN_UNIQUE_INNER:
			upath = create_unique_path(root, inner_rel,
									   inner_rel->cheapest_total_path);
			nrows = outer_rel->rows * upath->rows * jselec;
			if (nrows > outer_rel->rows)
				nrows = outer_rel->rows;
			break;
		case JOIN_REVERSE_IN:
		case JOIN_UNIQUE_OUTER:
			upath = create_unique_path(root, outer_rel,
									   outer_rel->cheapest_total_path);
			nrows = upath->rows * inner_rel->rows * jselec;
			if (nrows > inner_rel->rows)
				nrows = inner_rel->rows;
			break;
		default:
			elog(ERROR, "unrecognized join type: %d", (int) jointype);
			nrows = 0;			/* keep compiler quiet */
			break;
	}

	rel->rows = clamp_row_est(nrows);
}

/*
 * join_in_selectivity
 *	  Determines the factor by which a JOIN_IN join's result is expected
 *	  to be smaller than an ordinary inner join.
 *
 * 'path' is already filled in except for the cost fields
 */
static Selectivity
join_in_selectivity(JoinPath *path, PlannerInfo *root)
{
	RelOptInfo *innerrel;
	UniquePath *innerunique;
	Selectivity selec;
	double		nrows;

	/* Return 1.0 whenever it's not JOIN_IN */
	if (path->jointype != JOIN_IN)
		return 1.0;

	/*
	 * Return 1.0 if the inner side is already known unique.  The case where
	 * the inner path is already a UniquePath probably cannot happen in
	 * current usage, but check it anyway for completeness.  The interesting
	 * case is where we've determined the inner relation itself is unique,
	 * which we can check by looking at the rows estimate for its UniquePath.
	 */
	if (IsA(path->innerjoinpath, UniquePath))
		return 1.0;
	innerrel = path->innerjoinpath->parent;
	innerunique = create_unique_path(root,
									 innerrel,
									 innerrel->cheapest_total_path);
	if (innerunique->rows >= innerrel->rows)
		return 1.0;

	/*
	 * Compute same result set_joinrel_size_estimates would compute for
	 * JOIN_INNER.	Note that we use the input rels' absolute size estimates,
	 * not PATH_ROWS() which might be less; if we used PATH_ROWS() we'd be
	 * double-counting the effects of any join clauses used in input scans.
	 */
	selec = clauselist_selectivity(root,
								   path->joinrestrictinfo,
								   0,
								   JOIN_INNER);
	nrows = path->outerjoinpath->parent->rows * innerrel->rows * selec;

	nrows = clamp_row_est(nrows);

	/* See if it's larger than the actual JOIN_IN size estimate */
	if (nrows > path->path.parent->rows)
		return path->path.parent->rows / nrows;
	else
		return 1.0;
}

/*
 * set_function_size_estimates
 *		Set the size estimates for a base relation that is a function call.
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already.
 *
 * We set the same fields as set_baserel_size_estimates.
 */
void
set_function_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;

	/* Should only be applied to base relations that are functions */
	Assert(rel->relid > 0);
	rte = rt_fetch(rel->relid, root->parse->rtable);
	Assert(rte->rtekind == RTE_FUNCTION);

	/*
	 * Estimate number of rows the function itself will return.
	 *
	 * XXX no idea how to do this yet; but we can at least check whether
	 * function returns set or not...
	 */
	if (expression_returns_set(rte->funcexpr))
		rel->tuples = 1000;
	else
		rel->tuples = 1;

	/* Now estimate number of output rows, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_values_size_estimates
 *		Set the size estimates for a base relation that is a values list.
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already.
 *
 * We set the same fields as set_baserel_size_estimates.
 */
void
set_values_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;

	/* Should only be applied to base relations that are values lists */
	Assert(rel->relid > 0);
	rte = rt_fetch(rel->relid, root->parse->rtable);
	Assert(rte->rtekind == RTE_VALUES);

	/*
	 * Estimate number of rows the values list will return. We know this
	 * precisely based on the list length (well, barring set-returning
	 * functions in list items, but that's a refinement not catered for
	 * anywhere else either).
	 */
	rel->tuples = list_length(rte->values_lists);

	/* Now estimate number of output rows, etc */
	set_baserel_size_estimates(root, rel);
}


/*
 * set_rel_width
 *		Set the estimated output width of a base relation.
 *
 * NB: this works best on plain relations because it prefers to look at
 * real Vars.  It will fail to make use of pg_statistic info when applied
 * to a subquery relation, even if the subquery outputs are simple vars
 * that we could have gotten info for.	Is it worth trying to be smarter
 * about subqueries?
 *
 * The per-attribute width estimates are cached for possible re-use while
 * building join relations.
 */
static void
set_rel_width(PlannerInfo *root, RelOptInfo *rel)
{
	int32		tuple_width = 0;
	ListCell   *tllist;

	foreach(tllist, rel->reltargetlist)
	{
		Var		   *var = (Var *) lfirst(tllist);
		int			ndx;
		Oid			relid;
		int32		item_width;

		/* For now, punt on whole-row child Vars */
		if (!IsA(var, Var))
		{
			tuple_width += 32;	/* arbitrary */
			continue;
		}

		ndx = var->varattno - rel->min_attr;

		/*
		 * The width probably hasn't been cached yet, but may as well check
		 */
		if (rel->attr_widths[ndx] > 0)
		{
			tuple_width += rel->attr_widths[ndx];
			continue;
		}

		relid = getrelid(var->varno, root->parse->rtable);
		if (relid != InvalidOid)
		{
			item_width = get_attavgwidth(relid, var->varattno);
			if (item_width > 0)
			{
				rel->attr_widths[ndx] = item_width;
				tuple_width += item_width;
				continue;
			}
		}

		/*
		 * Not a plain relation, or can't find statistics for it. Estimate
		 * using just the type info.
		 */
		item_width = get_typavgwidth(var->vartype, var->vartypmod);
		Assert(item_width > 0);
		rel->attr_widths[ndx] = item_width;
		tuple_width += item_width;
	}
	Assert(tuple_width >= 0);
	rel->width = tuple_width;
}

/*
 * relation_byte_size
 *	  Estimate the storage space in bytes for a given number of tuples
 *	  of a given width (size in bytes).
 */
static double
relation_byte_size(double tuples, int width)
{
	return tuples * (MAXALIGN(width) + MAXALIGN(sizeof(HeapTupleHeaderData)));
}

/*
 * page_size
 *	  Returns an estimate of the number of pages covered by a given
 *	  number of tuples of a given width (size in bytes).
 */
static double
page_size(double tuples, int width)
{
	return ceil(relation_byte_size(tuples, width) / BLCKSZ);
}
