/*-------------------------------------------------------------------------
 *
 * costsize.c
 *	  Routines to compute (and set) relation sizes and path costs
 *
 * Path costs are measured in units of disk accesses: one sequential page
 * fetch has cost 1.  All else is scaled relative to a page fetch, using
 * the scaling parameters
 *
 *	random_page_cost	Cost of a non-sequential page fetch
 *	cpu_tuple_cost		Cost of typical CPU time to process a tuple
 *	cpu_index_tuple_cost  Cost of typical CPU time to process an index tuple
 *	cpu_operator_cost	Cost of CPU time to process a typical WHERE operator
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
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/costsize.c,v 1.115.2.3 2005/04/04 01:43:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic.h"
#include "executor/nodeHash.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "parser/parsetree.h"
#include "utils/selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


#define LOG2(x)  (log(x) / 0.693147180559945)
#define LOG6(x)  (log(x) / 1.79175946922805)

/*
 * Some Paths return less than the nominal number of rows of their parent
 * relations; join nodes need to do this to get the correct input count:
 */
#define PATH_ROWS(path) \
	(IsA(path, UniquePath) ? \
	 ((UniquePath *) (path))->rows : \
	 (path)->parent->rows)


double		effective_cache_size = DEFAULT_EFFECTIVE_CACHE_SIZE;
double		random_page_cost = DEFAULT_RANDOM_PAGE_COST;
double		cpu_tuple_cost = DEFAULT_CPU_TUPLE_COST;
double		cpu_index_tuple_cost = DEFAULT_CPU_INDEX_TUPLE_COST;
double		cpu_operator_cost = DEFAULT_CPU_OPERATOR_COST;

Cost		disable_cost = 100000000.0;

bool		enable_seqscan = true;
bool		enable_indexscan = true;
bool		enable_tidscan = true;
bool		enable_sort = true;
bool		enable_hashagg = true;
bool		enable_nestloop = true;
bool		enable_mergejoin = true;
bool		enable_hashjoin = true;


static Selectivity estimate_hash_bucketsize(Query *root, Var *var,
						 int nbuckets);
static bool cost_qual_eval_walker(Node *node, QualCost *total);
static Selectivity approx_selectivity(Query *root, List *quals,
				   JoinType jointype);
static void set_rel_width(Query *root, RelOptInfo *rel);
static double relation_byte_size(double tuples, int width);
static double page_size(double tuples, int width);


/*
 * cost_seqscan
 *	  Determines and returns the cost of scanning a relation sequentially.
 */
void
cost_seqscan(Path *path, Query *root,
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
	 *
	 * The cost of reading a page sequentially is 1.0, by definition. Note
	 * that the Unix kernel will typically do some amount of read-ahead
	 * optimization, so that this cost is less than the true cost of
	 * reading a page from disk.  We ignore that issue here, but must take
	 * it into account when estimating the cost of non-sequential
	 * accesses!
	 */
	run_cost += baserel->pages; /* sequential fetches with cost 1.0 */

	/* CPU costs */
	startup_cost += baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_nonsequential_access
 *	  Estimate the cost of accessing one page at random from a relation
 *	  (or sort temp file) of the given size in pages.
 *
 * The simplistic model that the cost is random_page_cost is what we want
 * to use for large relations; but for small ones that is a serious
 * overestimate because of the effects of caching.	This routine tries to
 * account for that.
 *
 * Unfortunately we don't have any good way of estimating the effective cache
 * size we are working with --- we know that Postgres itself has NBuffers
 * internal buffers, but the size of the kernel's disk cache is uncertain,
 * and how much of it we get to use is even less certain.  We punt the problem
 * for now by assuming we are given an effective_cache_size parameter.
 *
 * Given a guesstimated cache size, we estimate the actual I/O cost per page
 * with the entirely ad-hoc equations:
 *	if relpages >= effective_cache_size:
 *		random_page_cost * (1 - (effective_cache_size/relpages)/2)
 *	if relpages < effective_cache_size:
 *		1 + (random_page_cost/2-1) * (relpages/effective_cache_size) ** 2
 * These give the right asymptotic behavior (=> 1.0 as relpages becomes
 * small, => random_page_cost as it becomes large) and meet in the middle
 * with the estimate that the cache is about 50% effective for a relation
 * of the same size as effective_cache_size.  (XXX this is probably all
 * wrong, but I haven't been able to find any theory about how effective
 * a disk cache should be presumed to be.)
 */
static Cost
cost_nonsequential_access(double relpages)
{
	double		relsize;

	/* don't crash on bad input data */
	if (relpages <= 0.0 || effective_cache_size <= 0.0)
		return random_page_cost;

	relsize = relpages / effective_cache_size;

	if (relsize >= 1.0)
		return random_page_cost * (1.0 - 0.5 / relsize);
	else
		return 1.0 + (random_page_cost * 0.5 - 1.0) * relsize * relsize;
}

/*
 * cost_index
 *	  Determines and returns the cost of scanning a relation using an index.
 *
 *	  NOTE: an indexscan plan node can actually represent several passes,
 *	  but here we consider the cost of just one pass.
 *
 * 'root' is the query root
 * 'baserel' is the base relation the index is for
 * 'index' is the index to be used
 * 'indexQuals' is the list of applicable qual clauses (implicit AND semantics)
 * 'is_injoin' is T if we are considering using the index scan as the inside
 *		of a nestloop join (hence, some of the indexQuals are join clauses)
 *
 * NOTE: 'indexQuals' must contain only clauses usable as index restrictions.
 * Any additional quals evaluated as qpquals may reduce the number of returned
 * tuples, but they won't reduce the number of tuples we have to fetch from
 * the table, so they don't reduce the scan cost.
 */
void
cost_index(Path *path, Query *root,
		   RelOptInfo *baserel,
		   IndexOptInfo *index,
		   List *indexQuals,
		   bool is_injoin)
{
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
	double		T,
				b;

	/* Should only be applied to base relations */
	Assert(IsA(baserel, RelOptInfo) &&
		   IsA(index, IndexOptInfo));
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	if (!enable_indexscan)
		startup_cost += disable_cost;

	/*
	 * Call index-access-method-specific code to estimate the processing
	 * cost for scanning the index, as well as the selectivity of the
	 * index (ie, the fraction of main-table tuples we will have to
	 * retrieve) and its correlation to the main-table tuple order.
	 */
	OidFunctionCall8(index->amcostestimate,
					 PointerGetDatum(root),
					 PointerGetDatum(baserel),
					 PointerGetDatum(index),
					 PointerGetDatum(indexQuals),
					 PointerGetDatum(&indexStartupCost),
					 PointerGetDatum(&indexTotalCost),
					 PointerGetDatum(&indexSelectivity),
					 PointerGetDatum(&indexCorrelation));

	/* all costs for touching index itself included here */
	startup_cost += indexStartupCost;
	run_cost += indexTotalCost - indexStartupCost;

	/*----------
	 * Estimate number of main-table tuples and pages fetched.
	 *
	 * When the index ordering is uncorrelated with the table ordering,
	 * we use an approximation proposed by Mackert and Lohman, "Index Scans
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
	 * When the index ordering is exactly correlated with the table ordering
	 * (just after a CLUSTER, for example), the number of pages fetched should
	 * be just sT.	What's more, these will be sequential fetches, not the
	 * random fetches that occur in the uncorrelated case.	So, depending on
	 * the extent of correlation, we should estimate the actual I/O cost
	 * somewhere between s * T * 1.0 and PF * random_cost.	We currently
	 * interpolate linearly between these two endpoints based on the
	 * correlation squared (XXX is that appropriate?).
	 *
	 * In any case the number of tuples fetched is Ns.
	 *----------
	 */

	tuples_fetched = indexSelectivity * baserel->tuples;
	/* Don't believe estimates less than 1... */
	if (tuples_fetched < 1.0)
		tuples_fetched = 1.0;

	/* This part is the Mackert and Lohman formula */

	T = (baserel->pages > 1) ? (double) baserel->pages : 1.0;
	b = (effective_cache_size > 1) ? effective_cache_size : 1.0;

	if (T <= b)
	{
		pages_fetched =
			(2.0 * T * tuples_fetched) / (2.0 * T + tuples_fetched);
		if (pages_fetched > T)
			pages_fetched = T;
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
	}

	/*
	 * min_IO_cost corresponds to the perfectly correlated case
	 * (csquared=1), max_IO_cost to the perfectly uncorrelated case
	 * (csquared=0).  Note that we just charge random_page_cost per page
	 * in the uncorrelated case, rather than using
	 * cost_nonsequential_access, since we've already accounted for
	 * caching effects by using the Mackert model.
	 */
	min_IO_cost = ceil(indexSelectivity * T);
	max_IO_cost = pages_fetched * random_page_cost;

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
	 * some of the indexquals are join clauses and shouldn't be
	 * subtracted. Rather than work out exactly how much to subtract, we
	 * don't subtract anything.
	 *
	 * XXX For a lossy index, not all the quals will be removed and so we
	 * really shouldn't subtract their costs; but detecting that seems
	 * more expensive than it's worth.
	 */
	startup_cost += baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;

	if (!is_injoin)
	{
		QualCost	index_qual_cost;

		cost_qual_eval(&index_qual_cost, indexQuals);
		cpu_per_tuple -= index_qual_cost.per_tuple;
	}

	run_cost += cpu_per_tuple * tuples_fetched;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_tidscan
 *	  Determines and returns the cost of scanning a relation using TIDs.
 */
void
cost_tidscan(Path *path, Query *root,
			 RelOptInfo *baserel, List *tideval)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;
	int			ntuples = length(tideval);

	/* Should only be applied to base relations */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	if (!enable_tidscan)
		startup_cost += disable_cost;

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
	 * Cost of path is cost of evaluating the subplan, plus cost of
	 * evaluating any restriction clauses that will be attached to the
	 * SubqueryScan node, plus cpu_tuple_cost to account for selection and
	 * projection overhead.
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
cost_functionscan(Path *path, Query *root, RelOptInfo *baserel)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;

	/* Should only be applied to base relations that are functions */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_FUNCTION);

	/*
	 * For now, estimate function's cost at one operator eval per function
	 * call.  Someday we should revive the function cost estimate columns
	 * in pg_proc...
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
 * If the total volume of data to sort is less than SortMem, we will do
 * an in-memory sort, which requires no I/O and about t*log2(t) tuple
 * comparisons for t tuples.
 *
 * If the total volume exceeds SortMem, we switch to a tape-style merge
 * algorithm.  There will still be about t*log2(t) tuple comparisons in
 * total, but we will also need to write and read each tuple once per
 * merge pass.	We expect about ceil(log6(r)) merge passes where r is the
 * number of initial runs formed (log6 because tuplesort.c uses six-tape
 * merging).  Since the average initial run should be about twice SortMem,
 * we have
 *		disk traffic = 2 * relsize * ceil(log6(p / (2*SortMem)))
 *		cpu = comparison_cost * t * log2(t)
 *
 * The disk traffic is assumed to be half sequential and half random
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
cost_sort(Path *path, Query *root,
		  List *pathkeys, Cost input_cost, double tuples, int width)
{
	Cost		startup_cost = input_cost;
	Cost		run_cost = 0;
	double		nbytes = relation_byte_size(tuples, width);
	long		sortmembytes = SortMem * 1024L;

	if (!enable_sort)
		startup_cost += disable_cost;

	/*
	 * We want to be sure the cost of a sort is never estimated as zero,
	 * even if passed-in tuple count is zero.  Besides, mustn't do
	 * log(0)...
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
	if (nbytes > sortmembytes)
	{
		double		npages = ceil(nbytes / BLCKSZ);
		double		nruns = nbytes / (sortmembytes * 2);
		double		log_runs = ceil(LOG6(nruns));
		double		npageaccesses;

		if (log_runs < 1.0)
			log_runs = 1.0;
		npageaccesses = 2.0 * npages * log_runs;
		/* Assume half are sequential (cost 1), half are not */
		startup_cost += npageaccesses *
			(1.0 + cost_nonsequential_access(npages)) * 0.5;
	}

	/*
	 * Also charge a small amount (arbitrarily set equal to operator cost)
	 * per extracted tuple.
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
 * If the total volume of data to materialize exceeds SortMem, we will need
 * to write it to disk, so the cost is much higher in that case.
 */
void
cost_material(Path *path,
			  Cost input_cost, double tuples, int width)
{
	Cost		startup_cost = input_cost;
	Cost		run_cost = 0;
	double		nbytes = relation_byte_size(tuples, width);
	long		sortmembytes = SortMem * 1024L;

	/* disk costs */
	if (nbytes > sortmembytes)
	{
		double		npages = ceil(nbytes / BLCKSZ);

		/* We'll write during startup and read during retrieval */
		startup_cost += npages;
		run_cost += npages;
	}

	/*
	 * Also charge a small amount per extracted tuple.	We use
	 * cpu_tuple_cost so that it doesn't appear worthwhile to materialize
	 * a bare seqscan.
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
cost_agg(Path *path, Query *root,
		 AggStrategy aggstrategy, int numAggs,
		 int numGroupCols, double numGroups,
		 Cost input_startup_cost, Cost input_total_cost,
		 double input_tuples)
{
	Cost		startup_cost;
	Cost		total_cost;

	/*
	 * We charge one cpu_operator_cost per aggregate function per input
	 * tuple, and another one per output tuple (corresponding to transfn
	 * and finalfn calls respectively).  If we are grouping, we charge an
	 * additional cpu_operator_cost per grouping column per input tuple
	 * for grouping comparisons.
	 *
	 * We will produce a single output tuple if not grouping, and a tuple per
	 * group otherwise.
	 *
	 * Note: in this cost model, AGG_SORTED and AGG_HASHED have exactly the
	 * same total CPU cost, but AGG_SORTED has lower startup cost.	If the
	 * input path is already sorted appropriately, AGG_SORTED should be
	 * preferred (since it has no risk of memory overflow).  This will
	 * happen as long as the computed total costs are indeed exactly equal
	 * --- but if there's roundoff error we might do the wrong thing.  So
	 * be sure that the computations below form the same intermediate
	 * values in the same order.
	 */
	if (aggstrategy == AGG_PLAIN)
	{
		startup_cost = input_total_cost;
		startup_cost += cpu_operator_cost * (input_tuples + 1) * numAggs;
		/* we aren't grouping */
		total_cost = startup_cost;
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
	}
	else
	{
		/* must be AGG_HASHED */
		startup_cost = input_total_cost;
		startup_cost += cpu_operator_cost * input_tuples * numGroupCols;
		startup_cost += cpu_operator_cost * input_tuples * numAggs;
		total_cost = startup_cost;
		total_cost += cpu_operator_cost * numGroups * numAggs;
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
cost_group(Path *path, Query *root,
		   int numGroupCols, double numGroups,
		   Cost input_startup_cost, Cost input_total_cost,
		   double input_tuples)
{
	Cost		startup_cost;
	Cost		total_cost;

	startup_cost = input_startup_cost;
	total_cost = input_total_cost;

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We
	 * assume all columns get compared at most of the tuples.
	 */
	total_cost += cpu_operator_cost * input_tuples * numGroupCols;

	path->startup_cost = startup_cost;
	path->total_cost = total_cost;
}

/*
 * cost_nestloop
 *	  Determines and returns the cost of joining two relations using the
 *	  nested loop algorithm.
 *
 * 'path' is already filled in except for the cost fields
 */
void
cost_nestloop(NestPath *path, Query *root)
{
	Path	   *outer_path = path->outerjoinpath;
	Path	   *inner_path = path->innerjoinpath;
	List	   *restrictlist = path->joinrestrictinfo;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;
	QualCost	restrict_qual_cost;
	double		outer_path_rows = PATH_ROWS(outer_path);
	double		inner_path_rows = PATH_ROWS(inner_path);
	double		ntuples;
	Selectivity joininfactor;

	if (!enable_nestloop)
		startup_cost += disable_cost;

	/*
	 * If we're doing JOIN_IN then we will stop scanning inner tuples for
	 * an outer tuple as soon as we have one match.  Account for the
	 * effects of this by scaling down the cost estimates in proportion to
	 * the expected output size.  (This assumes that all the quals
	 * attached to the join are IN quals, which should be true.)
	 *
	 * Note: it's probably bogus to use the normal selectivity calculation
	 * here when either the outer or inner path is a UniquePath.
	 */
	if (path->jointype == JOIN_IN)
	{
		Selectivity qual_selec = approx_selectivity(root, restrictlist,
													path->jointype);
		double		qptuples;

		qptuples = ceil(qual_selec * outer_path_rows * inner_path_rows);
		if (qptuples > path->path.parent->rows)
			joininfactor = path->path.parent->rows / qptuples;
		else
			joininfactor = 1.0;
	}
	else
		joininfactor = 1.0;

	/* cost of source data */

	/*
	 * NOTE: clearly, we must pay both outer and inner paths' startup_cost
	 * before we can start returning tuples, so the join's startup cost is
	 * their sum.  What's not so clear is whether the inner path's
	 * startup_cost must be paid again on each rescan of the inner path.
	 * This is not true if the inner path is materialized or is a
	 * hashjoin, but probably is true otherwise.
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
	 * Compute number of tuples processed (not number emitted!). If inner
	 * path is an indexscan, be sure to use its estimated output row
	 * count, which may be lower than the restriction-clause-only row
	 * count of its parent.  (We don't include this case in the PATH_ROWS
	 * macro because it applies *only* to a nestloop's inner relation.)
	 * Note: it is correct to use the unadjusted inner_path_rows in the
	 * above calculation for joininfactor, since otherwise we'd be
	 * double-counting the selectivity of the join clause being used for
	 * the index.
	 */
	if (IsA(inner_path, IndexPath))
		inner_path_rows = ((IndexPath *) inner_path)->rows;

	ntuples = inner_path_rows * outer_path_rows;

	/* CPU costs */
	cost_qual_eval(&restrict_qual_cost, restrictlist);
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
cost_mergejoin(MergePath *path, Query *root)
{
	Path	   *outer_path = path->jpath.outerjoinpath;
	Path	   *inner_path = path->jpath.innerjoinpath;
	List	   *restrictlist = path->jpath.joinrestrictinfo;
	List	   *mergeclauses = path->path_mergeclauses;
	List	   *outersortkeys = path->outersortkeys;
	List	   *innersortkeys = path->innersortkeys;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;
	Selectivity merge_selec;
	Selectivity qp_selec;
	QualCost	merge_qual_cost;
	QualCost	qp_qual_cost;
	RestrictInfo *firstclause;
	List	   *qpquals;
	double		outer_path_rows = PATH_ROWS(outer_path);
	double		inner_path_rows = PATH_ROWS(inner_path);
	double		outer_rows,
				inner_rows;
	double		mergejointuples,
				rescannedtuples;
	double		qptuples;
	double		rescanratio;
	Selectivity outerscansel,
				innerscansel;
	Selectivity joininfactor;
	Path		sort_path;		/* dummy for result of cost_sort */

	if (!enable_mergejoin)
		startup_cost += disable_cost;

	/*
	 * Compute cost and selectivity of the mergequals and qpquals (other
	 * restriction clauses) separately.  We use approx_selectivity here
	 * for speed --- in most cases, any errors won't affect the result
	 * much.
	 *
	 * Note: it's probably bogus to use the normal selectivity calculation
	 * here when either the outer or inner path is a UniquePath.
	 */
	merge_selec = approx_selectivity(root, mergeclauses,
									 path->jpath.jointype);
	cost_qual_eval(&merge_qual_cost, mergeclauses);
	qpquals = set_ptrDifference(restrictlist, mergeclauses);
	qp_selec = approx_selectivity(root, qpquals,
								  path->jpath.jointype);
	cost_qual_eval(&qp_qual_cost, qpquals);
	freeList(qpquals);

	/* approx # tuples passing the merge quals */
	mergejointuples = ceil(merge_selec * outer_path_rows * inner_path_rows);
	/* approx # tuples passing qpquals as well */
	qptuples = ceil(mergejointuples * qp_selec);

	/*
	 * When there are equal merge keys in the outer relation, the
	 * mergejoin must rescan any matching tuples in the inner relation.
	 * This means re-fetching inner tuples.  Our cost model for this is
	 * that a re-fetch costs the same as an original fetch, which is
	 * probably an overestimate; but on the other hand we ignore the
	 * bookkeeping costs of mark/restore. Not clear if it's worth
	 * developing a more refined model.
	 *
	 * The number of re-fetches can be estimated approximately as size of
	 * merge join output minus size of inner relation.	Assume that the
	 * distinct key values are 1, 2, ..., and denote the number of values
	 * of each key in the outer relation as m1, m2, ...; in the inner
	 * relation, n1, n2, ...  Then we have
	 *
	 * size of join = m1 * n1 + m2 * n2 + ...
	 *
	 * number of rescanned tuples = (m1 - 1) * n1 + (m2 - 1) * n2 + ... = m1 *
	 * n1 + m2 * n2 + ... - (n1 + n2 + ...) = size of join - size of inner
	 * relation
	 *
	 * This equation works correctly for outer tuples having no inner match
	 * (nk = 0), but not for inner tuples having no outer match (mk = 0);
	 * we are effectively subtracting those from the number of rescanned
	 * tuples, when we should not.	Can we do better without expensive
	 * selectivity computations?
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
		firstclause = (RestrictInfo *) lfirst(mergeclauses);
		if (firstclause->left_mergescansel < 0)	/* not computed yet? */
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

	outer_rows = ceil(outer_path_rows * outerscansel);
	if (outer_rows < 1)
		outer_rows = 1;
	inner_rows = ceil(inner_path_rows * innerscansel);
	if (inner_rows < 1)
		inner_rows = 1;

	/*
	 * Readjust scan selectivities to account for above rounding.  This is
	 * normally an insignificant effect, but when there are only a few
	 * rows in the inputs, failing to do this makes for a large percentage
	 * error.
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
	 * If we're doing JOIN_IN then we will stop outputting inner tuples
	 * for an outer tuple as soon as we have one match.  Account for the
	 * effects of this by scaling down the cost estimates in proportion to
	 * the expected output size.  (This assumes that all the quals
	 * attached to the join are IN quals, which should be true.)
	 */
	if (path->jpath.jointype == JOIN_IN &&
		qptuples > path->jpath.path.parent->rows)
		joininfactor = path->jpath.path.parent->rows / qptuples;
	else
		joininfactor = 1.0;

	/*
	 * The number of tuple comparisons needed is approximately number of
	 * outer rows plus number of inner rows plus number of rescanned
	 * tuples (can we refine this?).  At each one, we need to evaluate the
	 * mergejoin quals.  NOTE: JOIN_IN mode does not save any work here,
	 * so do NOT include joininfactor.
	 */
	startup_cost += merge_qual_cost.startup;
	run_cost += merge_qual_cost.per_tuple *
		(outer_rows + inner_rows * rescanratio);

	/*
	 * For each tuple that gets through the mergejoin proper, we charge
	 * cpu_tuple_cost plus the cost of evaluating additional restriction
	 * clauses that are to be applied at the join.	(This is pessimistic
	 * since not all of the quals may get evaluated at each tuple.)  This
	 * work is skipped in JOIN_IN mode, so apply the factor.
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
cost_hashjoin(HashPath *path, Query *root)
{
	Path	   *outer_path = path->jpath.outerjoinpath;
	Path	   *inner_path = path->jpath.innerjoinpath;
	List	   *restrictlist = path->jpath.joinrestrictinfo;
	List	   *hashclauses = path->path_hashclauses;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;
	Selectivity hash_selec;
	Selectivity qp_selec;
	QualCost	hash_qual_cost;
	QualCost	qp_qual_cost;
	double		hashjointuples;
	double		qptuples;
	double		outer_path_rows = PATH_ROWS(outer_path);
	double		inner_path_rows = PATH_ROWS(inner_path);
	double		outerbytes = relation_byte_size(outer_path_rows,
											  outer_path->parent->width);
	double		innerbytes = relation_byte_size(inner_path_rows,
											  inner_path->parent->width);
	int			num_hashclauses = length(hashclauses);
	int			virtualbuckets;
	int			physicalbuckets;
	int			numbatches;
	Selectivity innerbucketsize;
	Selectivity joininfactor;
	List	   *hcl;
	List	   *qpquals;

	if (!enable_hashjoin)
		startup_cost += disable_cost;

	/*
	 * Compute cost and selectivity of the hashquals and qpquals (other
	 * restriction clauses) separately.  We use approx_selectivity here
	 * for speed --- in most cases, any errors won't affect the result
	 * much.
	 *
	 * Note: it's probably bogus to use the normal selectivity calculation
	 * here when either the outer or inner path is a UniquePath.
	 */
	hash_selec = approx_selectivity(root, hashclauses,
									path->jpath.jointype);
	cost_qual_eval(&hash_qual_cost, hashclauses);
	qpquals = set_ptrDifference(restrictlist, hashclauses);
	qp_selec = approx_selectivity(root, qpquals,
								  path->jpath.jointype);
	cost_qual_eval(&qp_qual_cost, qpquals);
	freeList(qpquals);

	/* approx # tuples passing the hash quals */
	hashjointuples = ceil(hash_selec * outer_path_rows * inner_path_rows);
	/* approx # tuples passing qpquals as well */
	qptuples = ceil(hashjointuples * qp_selec);

	/* cost of source data */
	startup_cost += outer_path->startup_cost;
	run_cost += outer_path->total_cost - outer_path->startup_cost;
	startup_cost += inner_path->total_cost;

	/*
	 * Cost of computing hash function: must do it once per input tuple.
	 * We charge one cpu_operator_cost for each column's hash function.
	 *
	 * XXX when a hashclause is more complex than a single operator, we
	 * really should charge the extra eval costs of the left or right
	 * side, as appropriate, here.	This seems more work than it's worth
	 * at the moment.
	 */
	startup_cost += cpu_operator_cost * num_hashclauses * inner_path_rows;
	run_cost += cpu_operator_cost * num_hashclauses * outer_path_rows;

	/* Get hash table size that executor would use for inner relation */
	ExecChooseHashTableSize(inner_path_rows,
							inner_path->parent->width,
							&virtualbuckets,
							&physicalbuckets,
							&numbatches);

	/*
	 * Determine bucketsize fraction for inner relation.  We use the
	 * smallest bucketsize estimated for any individual hashclause; this
	 * is undoubtedly conservative.
	 *
	 * BUT: if inner relation has been unique-ified, we can assume it's good
	 * for hashing.  This is important both because it's the right answer,
	 * and because we avoid contaminating the cache with a value that's
	 * wrong for non-unique-ified paths.
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
			 * First we have to figure out which side of the hashjoin
			 * clause is the inner side.
			 *
			 * Since we tend to visit the same clauses over and over when
			 * planning a large query, we cache the bucketsize estimate in
			 * the RestrictInfo node to avoid repeated lookups of
			 * statistics.
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
							   (Var *) get_rightop(restrictinfo->clause),
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
								(Var *) get_leftop(restrictinfo->clause),
												 virtualbuckets);
					restrictinfo->left_bucketsize = thisbucketsize;
				}
			}

			if (innerbucketsize > thisbucketsize)
				innerbucketsize = thisbucketsize;
		}
	}

	/*
	 * if inner relation is too big then we will need to "batch" the join,
	 * which implies writing and reading most of the tuples to disk an
	 * extra time.	Charge one cost unit per page of I/O (correct since it
	 * should be nice and sequential...).  Writing the inner rel counts as
	 * startup cost, all the rest as run cost.
	 */
	if (numbatches)
	{
		double		outerpages = page_size(outer_path_rows,
										   outer_path->parent->width);
		double		innerpages = page_size(inner_path_rows,
										   inner_path->parent->width);

		startup_cost += innerpages;
		run_cost += innerpages + 2 * outerpages;
	}

	/* CPU costs */

	/*
	 * If we're doing JOIN_IN then we will stop comparing inner tuples to
	 * an outer tuple as soon as we have one match.  Account for the
	 * effects of this by scaling down the cost estimates in proportion to
	 * the expected output size.  (This assumes that all the quals
	 * attached to the join are IN quals, which should be true.)
	 */
	if (path->jpath.jointype == JOIN_IN &&
		qptuples > path->jpath.path.parent->rows)
		joininfactor = path->jpath.path.parent->rows / qptuples;
	else
		joininfactor = 1.0;

	/*
	 * The number of tuple comparisons needed is the number of outer
	 * tuples times the typical number of tuples in a hash bucket, which
	 * is the inner relation size times its bucketsize fraction.  At each
	 * one, we need to evaluate the hashjoin quals.
	 */
	startup_cost += hash_qual_cost.startup;
	run_cost += hash_qual_cost.per_tuple *
		outer_path_rows * ceil(inner_path_rows * innerbucketsize) *
		joininfactor;

	/*
	 * For each tuple that gets through the hashjoin proper, we charge
	 * cpu_tuple_cost plus the cost of evaluating additional restriction
	 * clauses that are to be applied at the join.	(This is pessimistic
	 * since not all of the quals may get evaluated at each tuple.)
	 */
	startup_cost += qp_qual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qp_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * hashjointuples * joininfactor;

	/*
	 * Bias against putting larger relation on inside.	We don't want an
	 * absolute prohibition, though, since larger relation might have
	 * better bucketsize --- and we can't trust the size estimates
	 * unreservedly, anyway.  Instead, inflate the run cost by the square
	 * root of the size ratio.	(Why square root?  No real good reason,
	 * but it seems reasonable...)
	 *
	 * Note: before 7.4 we implemented this by inflating startup cost; but if
	 * there's a disable_cost component in the input paths' startup cost,
	 * that unfairly penalizes the hash.  Probably it'd be better to keep
	 * track of disable penalty separately from cost.
	 */
	if (innerbytes > outerbytes && outerbytes > 0)
		run_cost *= sqrt(innerbytes / outerbytes);

	path->jpath.path.startup_cost = startup_cost;
	path->jpath.path.total_cost = startup_cost + run_cost;
}

/*
 * Estimate hash bucketsize fraction (ie, number of entries in a bucket
 * divided by total tuples in relation) if the specified Var is used
 * as a hash key.
 *
 * XXX This is really pretty bogus since we're effectively assuming that the
 * distribution of hash keys will be the same after applying restriction
 * clauses as it was in the underlying relation.  However, we are not nearly
 * smart enough to figure out how the restrict clauses might change the
 * distribution, so this will have to do for now.
 *
 * We are passed the number of buckets the executor will use for the given
 * input relation.	If the data were perfectly distributed, with the same
 * number of tuples going into each available bucket, then the bucketsize
 * fraction would be 1/nbuckets.  But this happy state of affairs will occur
 * only if (a) there are at least nbuckets distinct data values, and (b)
 * we have a not-too-skewed data distribution.	Otherwise the buckets will
 * be nonuniformly occupied.  If the other relation in the join has a key
 * distribution similar to this one's, then the most-loaded buckets are
 * exactly those that will be probed most often.  Therefore, the "average"
 * bucket size for costing purposes should really be taken as something close
 * to the "worst case" bucket size.  We try to estimate this by adjusting the
 * fraction if there are too few distinct data values, and then scaling up
 * by the ratio of the most common value's frequency to the average frequency.
 *
 * If no statistics are available, use a default estimate of 0.1.  This will
 * discourage use of a hash rather strongly if the inner relation is large,
 * which is what we want.  We do not want to hash unless we know that the
 * inner rel is well-dispersed (or the alternatives seem much worse).
 */
static Selectivity
estimate_hash_bucketsize(Query *root, Var *var, int nbuckets)
{
	Oid			relid;
	RelOptInfo *rel;
	HeapTuple	tuple;
	Form_pg_statistic stats;
	double		estfract,
				ndistinct,
				mcvfreq,
				avgfreq;
	float4	   *numbers;
	int			nnumbers;

	/* Ignore any binary-compatible relabeling */
	if (var && IsA(var, RelabelType))
		var = (Var *) ((RelabelType *) var)->arg;

	/*
	 * Lookup info about var's relation and attribute; if none available,
	 * return default estimate.
	 */
	if (var == NULL || !IsA(var, Var))
		return 0.1;

	relid = getrelid(var->varno, root->rtable);
	if (relid == InvalidOid)
		return 0.1;

	rel = find_base_rel(root, var->varno);

	if (rel->tuples <= 0.0 || rel->rows <= 0.0)
		return 0.1;				/* ensure we can divide below */

	tuple = SearchSysCache(STATRELATT,
						   ObjectIdGetDatum(relid),
						   Int16GetDatum(var->varattno),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		/*
		 * If the attribute is known unique because of an index,
		 * we can treat it as well-distributed.
		 */
		if (has_unique_index(rel, var->varattno))
			return 1.0 / (double) nbuckets;

		/*
		 * Perhaps the Var is a system attribute; if so, it will have no
		 * entry in pg_statistic, but we may be able to guess something
		 * about its distribution anyway.
		 */
		switch (var->varattno)
		{
			case ObjectIdAttributeNumber:
			case SelfItemPointerAttributeNumber:
				/* these are unique, so buckets should be well-distributed */
				return 1.0 / (double) nbuckets;
			case TableOidAttributeNumber:
				/* hashing this is a terrible idea... */
				return 1.0;
		}
		return 0.1;
	}
	stats = (Form_pg_statistic) GETSTRUCT(tuple);

	/*
	 * Obtain number of distinct data values in raw relation.
	 */
	ndistinct = stats->stadistinct;
	if (ndistinct < 0.0)
		ndistinct = -ndistinct * rel->tuples;

	if (ndistinct <= 0.0)		/* ensure we can divide */
	{
		ReleaseSysCache(tuple);
		return 0.1;
	}

	/* Also compute avg freq of all distinct data values in raw relation */
	avgfreq = (1.0 - stats->stanullfrac) / ndistinct;

	/*
	 * Adjust ndistinct to account for restriction clauses.  Observe we
	 * are assuming that the data distribution is affected uniformly by
	 * the restriction clauses!
	 *
	 * XXX Possibly better way, but much more expensive: multiply by
	 * selectivity of rel's restriction clauses that mention the target
	 * Var.
	 */
	ndistinct *= rel->rows / rel->tuples;

	/*
	 * Initial estimate of bucketsize fraction is 1/nbuckets as long as
	 * the number of buckets is less than the expected number of distinct
	 * values; otherwise it is 1/ndistinct.
	 */
	if (ndistinct > (double) nbuckets)
		estfract = 1.0 / (double) nbuckets;
	else
		estfract = 1.0 / ndistinct;

	/*
	 * Look up the frequency of the most common value, if available.
	 */
	mcvfreq = 0.0;

	if (get_attstatsslot(tuple, var->vartype, var->vartypmod,
						 STATISTIC_KIND_MCV, InvalidOid,
						 NULL, NULL, &numbers, &nnumbers))
	{
		/*
		 * The first MCV stat is for the most common value.
		 */
		if (nnumbers > 0)
			mcvfreq = numbers[0];
		free_attstatsslot(var->vartype, NULL, 0,
						  numbers, nnumbers);
	}

	/*
	 * Adjust estimated bucketsize upward to account for skewed
	 * distribution.
	 */
	if (avgfreq > 0.0 && mcvfreq > avgfreq)
		estfract *= mcvfreq / avgfreq;

	/*
	 * Clamp bucketsize to sane range (the above adjustment could easily
	 * produce an out-of-range result).  We set the lower bound a little
	 * above zero, since zero isn't a very sane result.
	 */
	if (estfract < 1.0e-6)
		estfract = 1.0e-6;
	else if (estfract > 1.0)
		estfract = 1.0;

	ReleaseSysCache(tuple);

	return (Selectivity) estfract;
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
	List	   *l;

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
		 */
		if (qual && IsA(qual, RestrictInfo))
		{
			RestrictInfo *restrictinfo = (RestrictInfo *) qual;

			if (restrictinfo->eval_cost.startup < 0)
			{
				restrictinfo->eval_cost.startup = 0;
				restrictinfo->eval_cost.per_tuple = 0;
				cost_qual_eval_walker((Node *) restrictinfo->clause,
									  &restrictinfo->eval_cost);
			}
			cost->startup += restrictinfo->eval_cost.startup;
			cost->per_tuple += restrictinfo->eval_cost.per_tuple;
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
	 * Our basic strategy is to charge one cpu_operator_cost for each
	 * operator or function node in the given tree.  Vars and Consts are
	 * charged zero, and so are boolean operators (AND, OR, NOT).
	 * Simplistic, but a lot better than no model at all.
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
		/* should charge more than 1 op cost, but how many? */
		total->per_tuple += cpu_operator_cost * 10;
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
		 * subplan will be executed on each evaluation, so charge
		 * accordingly. (Sub-selects that can be executed as InitPlans
		 * have already been removed from the expression.)
		 *
		 * An exception occurs when we have decided we can implement the
		 * subplan by hashing.
		 *
		 */
		SubPlan    *subplan = (SubPlan *) node;
		Plan	   *plan = subplan->plan;

		if (subplan->useHashTable)
		{
			/*
			 * If we are using a hash table for the subquery outputs, then
			 * the cost of evaluating the query is a one-time cost. We
			 * charge one cpu_operator_cost per tuple for the work of
			 * loading the hashtable, too.
			 */
			total->startup += plan->total_cost +
				cpu_operator_cost * plan->plan_rows;

			/*
			 * The per-tuple costs include the cost of evaluating the
			 * lefthand expressions, plus the cost of probing the
			 * hashtable. Recursion into the exprs list will handle the
			 * lefthand expressions properly, and will count one
			 * cpu_operator_cost for each comparison operator.	That is
			 * probably too low for the probing cost, but it's hard to
			 * make a better estimate, so live with it for now.
			 */
		}
		else
		{
			/*
			 * Otherwise we will be rescanning the subplan output on each
			 * evaluation.	We need to estimate how much of the output we
			 * will actually need to scan.	NOTE: this logic should agree
			 * with the estimates used by make_subplan() in
			 * plan/subselect.c.
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
			 * uncorrelated or undirect correlated, AND its topmost node
			 * is a Sort or Material node, assume that we'll only need to
			 * pay its startup cost once; otherwise assume we pay the
			 * startup cost every time.
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
 * The "quick" part comes from caching the selectivity estimates so we can
 * avoid recomputing them later.  (Since the same clauses are typically
 * examined over and over in different possible join trees, this makes a
 * big difference.)
 *
 * The "dirty" part comes from the fact that the selectivities of multiple
 * clauses are estimated independently and multiplied together.  Now
 * clauselist_selectivity often can't do any better than that anyhow, but
 * for some situations (such as range constraints) it is smarter.
 *
 * Since we are only using the results to estimate how many potential
 * output tuples are generated and passed through qpqual checking, it
 * seems OK to live with the approximation.
 */
static Selectivity
approx_selectivity(Query *root, List *quals, JoinType jointype)
{
	Selectivity total = 1.0;
	List	   *l;

	foreach(l, quals)
	{
		Node	   *qual = (Node *) lfirst(l);
		Selectivity selec;

		/*
		 * RestrictInfo nodes contain a this_selec field reserved for this
		 * routine's use, so that it's not necessary to evaluate the qual
		 * clause's selectivity more than once.  If the clause's
		 * selectivity hasn't been computed yet, the field will contain
		 * -1.
		 */
		if (qual && IsA(qual, RestrictInfo))
		{
			RestrictInfo *restrictinfo = (RestrictInfo *) qual;

			if (restrictinfo->this_selec < 0)
				restrictinfo->this_selec =
					clause_selectivity(root,
									   (Node *) restrictinfo->clause,
									   0,
									   jointype);
			selec = restrictinfo->this_selec;
		}
		else
		{
			/* If it's a bare expression, must always do it the hard way */
			selec = clause_selectivity(root, qual, 0, jointype);
		}
		total *= selec;
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
set_baserel_size_estimates(Query *root, RelOptInfo *rel)
{
	double		temp;

	/* Should only be applied to base relations */
	Assert(rel->relid > 0);

	temp = rel->tuples *
		restrictlist_selectivity(root,
								 rel->baserestrictinfo,
								 rel->relid,
								 JOIN_INNER);

	/*
	 * Force estimate to be at least one row, to make explain output look
	 * better and to avoid possible divide-by-zero when interpolating
	 * cost.  Make it an integer, too.
	 */
	if (temp < 1.0)
		temp = 1.0;
	else
		temp = ceil(temp);

	rel->rows = temp;

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
 * We set the same relnode fields as set_baserel_size_estimates() does.
 */
void
set_joinrel_size_estimates(Query *root, RelOptInfo *rel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel,
						   JoinType jointype,
						   List *restrictlist)
{
	Selectivity selec;
	double		temp;
	UniquePath *upath;

	/*
	 * Compute joinclause selectivity.	Note that we are only considering
	 * clauses that become restriction clauses at this join level; we are
	 * not double-counting them because they were not considered in
	 * estimating the sizes of the component rels.
	 */
	selec = restrictlist_selectivity(root,
									 restrictlist,
									 0,
									 jointype);

	/*
	 * Basically, we multiply size of Cartesian product by selectivity.
	 *
	 * If we are doing an outer join, take that into account: the output must
	 * be at least as large as the non-nullable input.	(Is there any
	 * chance of being even smarter?)
	 *
	 * For JOIN_IN and variants, the Cartesian product is figured with
	 * respect to a unique-ified input, and then we can clamp to the size
	 * of the other input.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			temp = outer_rel->rows * inner_rel->rows * selec;
			break;
		case JOIN_LEFT:
			temp = outer_rel->rows * inner_rel->rows * selec;
			if (temp < outer_rel->rows)
				temp = outer_rel->rows;
			break;
		case JOIN_RIGHT:
			temp = outer_rel->rows * inner_rel->rows * selec;
			if (temp < inner_rel->rows)
				temp = inner_rel->rows;
			break;
		case JOIN_FULL:
			temp = outer_rel->rows * inner_rel->rows * selec;
			if (temp < outer_rel->rows)
				temp = outer_rel->rows;
			if (temp < inner_rel->rows)
				temp = inner_rel->rows;
			break;
		case JOIN_IN:
		case JOIN_UNIQUE_INNER:
			upath = create_unique_path(root, inner_rel,
									   inner_rel->cheapest_total_path);
			temp = outer_rel->rows * upath->rows * selec;
			if (temp > outer_rel->rows)
				temp = outer_rel->rows;
			break;
		case JOIN_REVERSE_IN:
		case JOIN_UNIQUE_OUTER:
			upath = create_unique_path(root, outer_rel,
									   outer_rel->cheapest_total_path);
			temp = upath->rows * inner_rel->rows * selec;
			if (temp > inner_rel->rows)
				temp = inner_rel->rows;
			break;
		default:
			elog(ERROR, "unrecognized join type: %d", (int) jointype);
			temp = 0;			/* keep compiler quiet */
			break;
	}

	/*
	 * Force estimate to be at least one row, to make explain output look
	 * better and to avoid possible divide-by-zero when interpolating
	 * cost.  Make it an integer, too.
	 */
	if (temp < 1.0)
		temp = 1.0;
	else
		temp = ceil(temp);

	rel->rows = temp;

	/*
	 * We need not compute the output width here, because
	 * build_joinrel_tlist already did.
	 */
}

/*
 * set_function_size_estimates
 *		Set the size estimates for a base relation that is a function call.
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
set_function_size_estimates(Query *root, RelOptInfo *rel)
{
	double		temp;

	/* Should only be applied to base relations that are functions */
	Assert(rel->relid > 0);
	Assert(rel->rtekind == RTE_FUNCTION);

	/*
	 * Estimate number of rows the function itself will return.
	 *
	 * XXX no idea how to do this yet; but should at least check whether
	 * function returns set or not...
	 */
	rel->tuples = 1000;

	/* Now estimate number of output rows */
	temp = rel->tuples *
		restrictlist_selectivity(root,
								 rel->baserestrictinfo,
								 rel->relid,
								 JOIN_INNER);

	/*
	 * Force estimate to be at least one row, to make explain output look
	 * better and to avoid possible divide-by-zero when interpolating
	 * cost.  Make it an integer, too.
	 */
	if (temp < 1.0)
		temp = 1.0;
	else
		temp = ceil(temp);

	rel->rows = temp;

	cost_qual_eval(&rel->baserestrictcost, rel->baserestrictinfo);

	set_rel_width(root, rel);
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
set_rel_width(Query *root, RelOptInfo *rel)
{
	int32		tuple_width = 0;
	List	   *tllist;

	foreach(tllist, FastListValue(&rel->reltargetlist))
	{
		Var		   *var = (Var *) lfirst(tllist);
		int			ndx = var->varattno - rel->min_attr;
		Oid			relid;
		int32		item_width;

		Assert(IsA(var, Var));

		/*
		 * The width probably hasn't been cached yet, but may as well
		 * check
		 */
		if (rel->attr_widths[ndx] > 0)
		{
			tuple_width += rel->attr_widths[ndx];
			continue;
		}

		relid = getrelid(var->varno, root->rtable);
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
	return tuples * (MAXALIGN(width) + MAXALIGN(sizeof(HeapTupleData)));
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
