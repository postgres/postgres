/*-------------------------------------------------------------------------
 *
 * costsize.c
 *	  Routines to compute (and set) relation sizes and path costs
 *
 * Path costs are measured in units of disk accesses: one page fetch
 * has cost 1.  The other primitive unit is the CPU time required to
 * process one tuple, which we set at "cpu_page_weight" of a page
 * fetch.  Obviously, the CPU time per tuple depends on the query
 * involved, but the relative CPU and disk speeds of a given platform
 * are so variable that we are lucky if we can get useful numbers
 * at all.  cpu_page_weight is user-settable, in case a particular
 * user is clueful enough to have a better-than-default estimate
 * of the ratio for his platform.  There is also cpu_index_page_weight,
 * the cost to process a tuple of an index during an index scan.
 *
 * 
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/costsize.c,v 1.49 2000/01/23 02:06:59 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#ifndef MAXINT
#define MAXINT		  INT_MAX
#endif
#else
#ifdef HAVE_VALUES_H
#include <values.h>
#endif
#endif

#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/internal.h"
#include "optimizer/tlist.h"
#include "utils/lsyscache.h"


Cost		cpu_page_weight = CPU_PAGE_WEIGHT;
Cost		cpu_index_page_weight = CPU_INDEX_PAGE_WEIGHT;

Cost		disable_cost = 100000000.0;

bool		enable_seqscan = true;
bool		enable_indexscan = true;
bool		enable_tidscan = true;
bool		enable_sort = true;
bool		enable_nestloop = true;
bool		enable_mergejoin = true;
bool		enable_hashjoin = true;


static void set_rel_width(Query *root, RelOptInfo *rel);
static int	compute_attribute_width(TargetEntry *tlistentry);
static double relation_byte_size(double tuples, int width);
static double page_size(double tuples, int width);
static double base_log(double x, double b);


/*
 * cost_seqscan
 *	  Determines and returns the cost of scanning a relation sequentially.
 *	  If the relation is a temporary to be materialized from a query
 *	  embedded within a data field (determined by 'relid' containing an
 *	  attribute reference), then a predetermined constant is returned (we
 *	  have NO IDEA how big the result of a POSTQUEL procedure is going to
 *	  be).
 *
 *		disk = p
 *		cpu = CPU-PAGE-WEIGHT * t
 */
Cost
cost_seqscan(RelOptInfo *baserel)
{
	Cost		temp = 0;

	/* Should only be applied to base relations */
	Assert(length(baserel->relids) == 1);

	if (!enable_seqscan)
		temp += disable_cost;

	if (lfirsti(baserel->relids) < 0)
	{
		/*
		 * cost of sequentially scanning a materialized temporary relation
		 */
		temp += _NONAME_SCAN_COST_;
	}
	else
	{
		temp += baserel->pages;
		temp += cpu_page_weight * baserel->tuples;
	}

	Assert(temp >= 0);
	return temp;
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
 *		of a nestloop join.
 *
 * NOTE: 'indexQuals' must contain only clauses usable as index restrictions.
 * Any additional quals evaluated as qpquals may reduce the number of returned
 * tuples, but they won't reduce the number of tuples we have to fetch from
 * the table, so they don't reduce the scan cost.
 */
Cost
cost_index(Query *root,
		   RelOptInfo *baserel,
		   IndexOptInfo *index,
		   List *indexQuals,
		   bool is_injoin)
{
	Cost		temp = 0;
	Cost		indexAccessCost;
	Selectivity	indexSelectivity;
	double		reltuples;
	double		relpages;

	/* Should only be applied to base relations */
	Assert(IsA(baserel, RelOptInfo) && IsA(index, IndexOptInfo));
	Assert(length(baserel->relids) == 1);

	if (!enable_indexscan && !is_injoin)
		temp += disable_cost;

	/*
	 * Call index-access-method-specific code to estimate the processing
	 * cost for scanning the index, as well as the selectivity of the index
	 * (ie, the fraction of main-table tuples we will have to retrieve).
	 */
	fmgr(index->amcostestimate, root, baserel, index, indexQuals,
		 &indexAccessCost, &indexSelectivity);

	/* all costs for touching index itself included here */
	temp += indexAccessCost;

	/*--------------------
	 * Estimate number of main-table tuples and pages touched.
	 *
	 * Worst case is that each tuple the index tells us to fetch comes
	 * from a different base-rel page, in which case the I/O cost would be
	 * 'reltuples' pages.  In practice we can expect the number of page
	 * fetches to be reduced by the buffer cache, because more than one
	 * tuple can be retrieved per page fetched.  Currently, we estimate
	 * the number of pages to be retrieved as
	 *			MIN(reltuples, relpages)
	 * This amounts to assuming that the buffer cache is perfectly efficient
	 * and never ends up reading the same page twice within one scan, which
	 * of course is too optimistic.  On the other hand, we are assuming that
	 * the target tuples are perfectly uniformly distributed across the
	 * relation's pages, which is too pessimistic --- any nonuniformity of
	 * distribution will reduce the number of pages we have to fetch.
	 * So, we guess-and-hope that these sources of error will more or less
	 * balance out.
	 *
	 * XXX need to add a penalty for nonsequential page fetches.
	 *
	 * XXX if the relation has recently been "clustered" using this index,
	 * then in fact the target tuples will be highly nonuniformly distributed,
	 * and we will be seriously overestimating the scan cost!  Currently we
	 * have no way to know whether the relation has been clustered, nor how
	 * much it's been modified since the last clustering, so we ignore this
	 * effect.  Would be nice to do better someday.
	 *--------------------
	 */

	reltuples = indexSelectivity * baserel->tuples;

	relpages = reltuples;
	if (baserel->pages > 0 && baserel->pages < relpages)
		relpages = baserel->pages;

	/* disk costs for main table */
	temp += relpages;

	/* CPU costs for heap tuples */
	temp += cpu_page_weight * reltuples;

	Assert(temp >= 0);
	return temp;
}

/*
 * cost_tidscan
 *	  Determines and returns the cost of scanning a relation using tid-s.
 *
 *		disk = number of tids
 *		cpu = CPU-PAGE-WEIGHT * number_of_tids
 */
Cost
cost_tidscan(RelOptInfo *baserel, List *tideval)
{
	Cost	temp = 0;

	if (!enable_tidscan)
		temp += disable_cost;

	temp += (1.0 + cpu_page_weight) * length(tideval);

	return temp;
}
 
/*
 * cost_sort
 *	  Determines and returns the cost of sorting a relation.
 *
 * If the total volume of data to sort is less than SortMem, we will do
 * an in-memory sort, which requires no I/O and about t*log2(t) tuple
 * comparisons for t tuples.  We use cpu_index_page_weight as the cost
 * of a tuple comparison (is this reasonable, or do we need another
 * basic parameter?).
 *
 * If the total volume exceeds SortMem, we switch to a tape-style merge
 * algorithm.  There will still be about t*log2(t) tuple comparisons in
 * total, but we will also need to write and read each tuple once per
 * merge pass.  We expect about ceil(log6(r)) merge passes where r is the
 * number of initial runs formed (log6 because tuplesort.c uses six-tape
 * merging).  Since the average initial run should be about twice SortMem,
 * we have
 *		disk = 2 * p * ceil(log6(p / (2*SortMem)))
 *		cpu = CPU-INDEX-PAGE-WEIGHT * t * log2(t)
 *
 * 'pathkeys' is a list of sort keys
 * 'tuples' is the number of tuples in the relation
 * 'width' is the average tuple width in bytes
 *
 * NOTE: some callers currently pass NIL for pathkeys because they
 * can't conveniently supply the sort keys.  Since this routine doesn't
 * currently do anything with pathkeys anyway, that doesn't matter...
 * but if it ever does, it should react gracefully to lack of key data.
 */
Cost
cost_sort(List *pathkeys, double tuples, int width)
{
	Cost		temp = 0;
	double		nbytes = relation_byte_size(tuples, width);
	long		sortmembytes = SortMem * 1024L;

	if (!enable_sort)
		temp += disable_cost;

	/*
	 * We want to be sure the cost of a sort is never estimated as zero,
	 * even if passed-in tuple count is zero.  Besides, mustn't do
	 * log(0)...
	 */
	if (tuples < 2.0)
		tuples = 2.0;

	temp += cpu_index_page_weight * tuples * base_log(tuples, 2.0);

	if (nbytes > sortmembytes)
	{
		double		npages = ceil(nbytes / BLCKSZ);
		double		nruns = nbytes / (sortmembytes * 2);
		double		log_runs = ceil(base_log(nruns, 6.0));

		if (log_runs < 1.0)
			log_runs = 1.0;
		temp += 2 * npages * log_runs;
	}

	Assert(temp > 0);
	return temp;
}


/*
 * cost_result
 *	  Determines and returns the cost of writing a relation of 'tuples'
 *	  tuples of 'width' bytes out to a result relation.
 */
#ifdef NOT_USED
Cost
cost_result(double tuples, int width)
{
	Cost		temp = 0;

	temp += page_size(tuples, width);
	temp += cpu_page_weight * tuples;
	Assert(temp >= 0);
	return temp;
}

#endif

/*
 * cost_nestloop
 *	  Determines and returns the cost of joining two relations using the
 *	  nested loop algorithm.
 *
 * 'outer_path' is the path for the outer relation
 * 'inner_path' is the path for the inner relation
 * 'is_indexjoin' is true if we are using an indexscan for the inner relation
 */
Cost
cost_nestloop(Path *outer_path,
			  Path *inner_path,
			  bool is_indexjoin)
{
	Cost		temp = 0;

	if (!enable_nestloop)
		temp += disable_cost;

	temp += outer_path->path_cost;
	temp += outer_path->parent->rows * inner_path->path_cost;

	Assert(temp >= 0);
	return temp;
}

/*
 * cost_mergejoin
 *	  Determines and returns the cost of joining two relations using the
 *	  merge join algorithm.
 *
 * 'outer_path' is the path for the outer relation
 * 'inner_path' is the path for the inner relation
 * 'outersortkeys' and 'innersortkeys' are lists of the keys to be used
 *				to sort the outer and inner relations, or NIL if no explicit
 *				sort is needed because the source path is already ordered
 */
Cost
cost_mergejoin(Path *outer_path,
			   Path *inner_path,
			   List *outersortkeys,
			   List *innersortkeys)
{
	Cost		temp = 0;

	if (!enable_mergejoin)
		temp += disable_cost;

	/* cost of source data */
	temp += outer_path->path_cost + inner_path->path_cost;

	if (outersortkeys)			/* do we need to sort? */
		temp += cost_sort(outersortkeys,
						  outer_path->parent->rows,
						  outer_path->parent->width);

	if (innersortkeys)			/* do we need to sort? */
		temp += cost_sort(innersortkeys,
						  inner_path->parent->rows,
						  inner_path->parent->width);

	/*
	 * Estimate the number of tuples to be processed in the mergejoin itself
	 * as one per tuple in the two source relations.  This could be a drastic
	 * underestimate if there are many equal-keyed tuples in either relation,
	 * but we have no good way of estimating that...
	 */
	temp += cpu_page_weight * (outer_path->parent->rows +
							   inner_path->parent->rows);

	Assert(temp >= 0);
	return temp;
}

/*
 * cost_hashjoin
 *	  Determines and returns the cost of joining two relations using the
 *	  hash join algorithm.
 *
 * 'outer_path' is the path for the outer relation
 * 'inner_path' is the path for the inner relation
 * 'innerdisbursion' is an estimate of the disbursion statistic
 *				for the inner hash key.
 */
Cost
cost_hashjoin(Path *outer_path,
			  Path *inner_path,
			  Selectivity innerdisbursion)
{
	Cost		temp = 0;
	double		outerbytes = relation_byte_size(outer_path->parent->rows,
												outer_path->parent->width);
	double		innerbytes = relation_byte_size(inner_path->parent->rows,
												inner_path->parent->width);
	long		hashtablebytes = SortMem * 1024L;

	if (!enable_hashjoin)
		temp += disable_cost;

	/* cost of source data */
	temp += outer_path->path_cost + inner_path->path_cost;

	/* cost of computing hash function: must do it once per tuple */
	temp += cpu_page_weight * (outer_path->parent->rows +
							   inner_path->parent->rows);

	/* the number of tuple comparisons needed is the number of outer
	 * tuples times the typical hash bucket size, which we estimate
	 * conservatively as the inner disbursion times the inner tuple
	 * count.  The cost per comparison is set at cpu_index_page_weight;
	 * is that reasonable, or do we need another basic parameter?
	 */
	temp += cpu_index_page_weight * outer_path->parent->rows *
		(inner_path->parent->rows * innerdisbursion);

	/*
	 * if inner relation is too big then we will need to "batch" the join,
	 * which implies writing and reading most of the tuples to disk an
	 * extra time.  Charge one cost unit per page of I/O.
	 */
	if (innerbytes > hashtablebytes)
		temp += 2 * (page_size(outer_path->parent->rows,
							   outer_path->parent->width) +
					 page_size(inner_path->parent->rows,
							   inner_path->parent->width));

	/*
	 * Bias against putting larger relation on inside.  We don't want
	 * an absolute prohibition, though, since larger relation might have
	 * better disbursion --- and we can't trust the size estimates
	 * unreservedly, anyway.
	 */
	if (innerbytes > outerbytes)
		temp *= 1.1;			/* is this an OK fudge factor? */

	Assert(temp >= 0);
	return temp;
}

/*
 * set_rel_rows_width
 *		Set the 'rows' and 'width' estimates for the given base relation.
 *
 * 'rows' is the estimated number of output tuples (after applying
 * restriction clauses).
 * 'width' is the estimated average output tuple width in bytes.
 */
void
set_rel_rows_width(Query *root, RelOptInfo *rel)
{
	/* Should only be applied to base relations */
	Assert(length(rel->relids) == 1);

	rel->rows = rel->tuples *
		restrictlist_selectivity(root,
								 rel->restrictinfo,
								 lfirsti(rel->relids));
	Assert(rel->rows >= 0);

	set_rel_width(root, rel);
}

/*
 * set_joinrel_rows_width
 *		Set the 'rows' and 'width' estimates for the given join relation.
 */
void
set_joinrel_rows_width(Query *root, RelOptInfo *rel,
					   JoinPath *joinpath)
{
	double		temp;

	/* cartesian product */
	temp = joinpath->outerjoinpath->parent->rows *
		joinpath->innerjoinpath->parent->rows;

	/* apply join restrictivity */
	temp *= restrictlist_selectivity(root,
									 joinpath->path.parent->restrictinfo,
									 0);

	Assert(temp >= 0);
	rel->rows = temp;

	set_rel_width(root, rel);
}

/*
 * set_rel_width
 *		Set the estimated output width of the relation.
 */
static void
set_rel_width(Query *root, RelOptInfo *rel)
{
	int			tuple_width = 0;
	List	   *tle;

	foreach(tle, rel->targetlist)
		tuple_width += compute_attribute_width((TargetEntry *) lfirst(tle));
	Assert(tuple_width >= 0);
	rel->width = tuple_width;
}

/*
 * compute_attribute_width
 *	  Given a target list entry, find the size in bytes of the attribute.
 *
 *	  If a field is variable-length, we make a default assumption.  Would be
 *	  better if VACUUM recorded some stats about the average field width...
 */
static int
compute_attribute_width(TargetEntry *tlistentry)
{
	int			width = get_typlen(tlistentry->resdom->restype);

	if (width < 0)
		return _DEFAULT_ATTRIBUTE_WIDTH_;
	else
		return width;
}

/*
 * relation_byte_size
 *	  Estimate the storage space in bytes for a given number of tuples
 *	  of a given width (size in bytes).
 */
static double
relation_byte_size(double tuples, int width)
{
	return tuples * ((double) (width + sizeof(HeapTupleData)));
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

static double
base_log(double x, double b)
{
	return log(x) / log(b);
}
