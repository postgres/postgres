/*-------------------------------------------------------------------------
 *
 * costsize.c--
 *	  Routines to compute (and set) relation sizes and path costs
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/costsize.c,v 1.21 1998/06/15 19:28:39 momjian Exp $
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

#include "nodes/relation.h"
#include "optimizer/cost.h"
#include "optimizer/internal.h"
#include "optimizer/keys.h"
#include "optimizer/tlist.h"
#include "utils/lsyscache.h"

extern int	NBuffers;

static int	compute_attribute_width(TargetEntry *tlistentry);
static double base_log(double x, double b);
static int	compute_targetlist_width(List *targetlist);

int			_disable_cost_ = 30000000;

bool		_enable_seqscan_ = true;
bool		_enable_indexscan_ = true;
bool		_enable_sort_ = true;
bool		_enable_hash_ = true;
bool		_enable_nestloop_ = true;
bool		_enable_mergesort_ = true;
bool		_enable_hashjoin_ = true;

Cost		_cpu_page_wight_ = _CPU_PAGE_WEIGHT_;
Cost		_cpu_index_page_wight_ = _CPU_INDEX_PAGE_WEIGHT_;

/*
 * cost_seqscan--
 *	  Determines and returns the cost of scanning a relation sequentially.
 *	  If the relation is a temporary to be materialized from a query
 *	  embedded within a data field (determined by 'relid' containing an
 *	  attribute reference), then a predetermined constant is returned (we
 *	  have NO IDEA how big the result of a POSTQUEL procedure is going to
 *	  be).
 *
 *		disk = p
 *		cpu = *CPU-PAGE-WEIGHT* * t
 *
 * 'relid' is the relid of the relation to be scanned
 * 'relpages' is the number of pages in the relation to be scanned
 *		(as determined from the system catalogs)
 * 'reltuples' is the number of tuples in the relation to be scanned
 *
 * Returns a flonum.
 *
 */
Cost
cost_seqscan(int relid, int relpages, int reltuples)
{
	Cost		temp = 0;

	if (!_enable_seqscan_)
		temp += _disable_cost_;

	if (relid < 0)
	{

		/*
		 * cost of sequentially scanning a materialized temporary relation
		 */
		temp += _TEMP_SCAN_COST_;
	}
	else
	{
		temp += relpages;
		temp += _cpu_page_wight_ * reltuples;
	}
	Assert(temp >= 0);
	return (temp);
}


/*
 * cost_index--
 *	  Determines and returns the cost of scanning a relation using an index.
 *
 *		disk = expected-index-pages + expected-data-pages
 *		cpu = *CPU-PAGE-WEIGHT* *
 *				(expected-index-tuples + expected-data-tuples)
 *
 * 'indexid' is the index OID
 * 'expected-indexpages' is the number of index pages examined in the scan
 * 'selec' is the selectivity of the index
 * 'relpages' is the number of pages in the main relation
 * 'reltuples' is the number of tuples in the main relation
 * 'indexpages' is the number of pages in the index relation
 * 'indextuples' is the number of tuples in the index relation
 *
 * Returns a flonum.
 *
 */
Cost
cost_index(Oid indexid,
		   int expected_indexpages,
		   Cost selec,
		   int relpages,
		   int reltuples,
		   int indexpages,
		   int indextuples,
		   bool is_injoin)
{
	Cost		temp;
	double		temp2;

	temp = (Cost) 0;

	if (!_enable_indexscan_ && !is_injoin)
		temp += _disable_cost_;

	/* expected index relation pages */
	temp += expected_indexpages;

	/* expected base relation pages */
	temp2 = (reltuples == 0) ? (double) 0 : (double) relpages / reltuples;
	temp2 = temp2 * (double) selec *indextuples;

	temp += Min(relpages, (int) ceil(temp2));

	/* per index tuples */
	temp = temp + (_cpu_index_page_wight_ * selec * indextuples);

	/* per heap tuples */
	temp = temp + (_cpu_page_wight_ * selec * reltuples);

	Assert(temp >= 0);
	return (temp);
}

/*
 * cost_sort--
 *	  Determines and returns the cost of sorting a relation by considering
 *	  1. the cost of doing an external sort:	XXX this is probably too low
 *				disk = (p lg p)
 *				cpu = *CPU-PAGE-WEIGHT* * (t lg t)
 *	  2. the cost of reading the sort result into memory (another seqscan)
 *		 unless 'noread' is set
 *
 * 'keys' is a list of sort keys
 * 'tuples' is the number of tuples in the relation
 * 'width' is the average tuple width in bytes
 * 'noread' is a flag indicating that the sort result can remain on disk
 *				(i.e., the sort result is the result relation)
 *
 * Returns a flonum.
 *
 */
Cost
cost_sort(List *keys, int tuples, int width, bool noread)
{
	Cost		temp = 0;
	int			npages = page_size(tuples, width);
	Cost		pages = (Cost) npages;
	Cost		numTuples = tuples;

	if (!_enable_sort_)
		temp += _disable_cost_;
	if (tuples == 0 || keys == NULL)
	{
		Assert(temp >= 0);
		return (temp);
	}
	temp += pages * base_log((double) pages, (double) 2.0);

	/*
	 * could be base_log(pages, NBuffers), but we are only doing 2-way
	 * merges
	 */
	temp += _cpu_page_wight_ *
		numTuples * base_log((double) pages, (double) 2.0);

	if (!noread)
		temp = temp + cost_seqscan(_TEMP_RELATION_ID_, npages, tuples);
	Assert(temp >= 0);

	return (temp);
}


/*
 * cost_result--
 *	  Determines and returns the cost of writing a relation of 'tuples'
 *	  tuples of 'width' bytes out to a result relation.
 *
 * Returns a flonum.
 *
 */
#ifdef NOT_USED
Cost
cost_result(int tuples, int width)
{
	Cost		temp = 0;

	temp = temp + page_size(tuples, width);
	temp = temp + _cpu_page_wight_ * tuples;
	Assert(temp >= 0);
	return (temp);
}

#endif

/*
 * cost_nestloop--
 *	  Determines and returns the cost of joining two relations using the
 *	  nested loop algorithm.
 *
 * 'outercost' is the (disk+cpu) cost of scanning the outer relation
 * 'innercost' is the (disk+cpu) cost of scanning the inner relation
 * 'outertuples' is the number of tuples in the outer relation
 *
 * Returns a flonum.
 *
 */
Cost
cost_nestloop(Cost outercost,
			  Cost innercost,
			  int outertuples,
			  int innertuples,
			  int outerpages,
			  bool is_indexjoin)
{
	Cost		temp = 0;

	if (!_enable_nestloop_)
		temp += _disable_cost_;
	temp += outercost;
	temp += outertuples * innercost;
	Assert(temp >= 0);

	return (temp);
}

/*
 * cost_mergesort--
 *	  'outercost' and 'innercost' are the (disk+cpu) costs of scanning the
 *				outer and inner relations
 *	  'outersortkeys' and 'innersortkeys' are lists of the keys to be used
 *				to sort the outer and inner relations
 *	  'outertuples' and 'innertuples' are the number of tuples in the outer
 *				and inner relations
 *	  'outerwidth' and 'innerwidth' are the (typical) widths (in bytes)
 *				of the tuples of the outer and inner relations
 *
 * Returns a flonum.
 *
 */
Cost
cost_mergesort(Cost outercost,
			   Cost innercost,
			   List *outersortkeys,
			   List *innersortkeys,
			   int outersize,
			   int innersize,
			   int outerwidth,
			   int innerwidth)
{
	Cost		temp = 0;

	if (!_enable_mergesort_)
		temp += _disable_cost_;

	temp += outercost;
	temp += innercost;
	temp += cost_sort(outersortkeys, outersize, outerwidth, false);
	temp += cost_sort(innersortkeys, innersize, innerwidth, false);
	temp += _cpu_page_wight_ * (outersize + innersize);
	Assert(temp >= 0);

	return (temp);
}

/*
 * cost_hashjoin--				XXX HASH
 *	  'outercost' and 'innercost' are the (disk+cpu) costs of scanning the
 *				outer and inner relations
 *	  'outerkeys' and 'innerkeys' are lists of the keys to be used
 *				to hash the outer and inner relations
 *	  'outersize' and 'innersize' are the number of tuples in the outer
 *				and inner relations
 *	  'outerwidth' and 'innerwidth' are the (typical) widths (in bytes)
 *				of the tuples of the outer and inner relations
 *
 * Returns a flonum.
 */
Cost
cost_hashjoin(Cost outercost,
			  Cost innercost,
			  List *outerkeys,
			  List *innerkeys,
			  int outersize,
			  int innersize,
			  int outerwidth,
			  int innerwidth)
{
	Cost		temp = 0;
	int			outerpages = page_size(outersize, outerwidth);
	int			innerpages = page_size(innersize, innerwidth);
	int			nrun = ceil((double) outerpages / (double) NBuffers);

	if (outerpages < innerpages)
		return _disable_cost_;
	if (!_enable_hashjoin_)
		temp += _disable_cost_;

	/*
	 * temp += outercost + (nrun + 1) * innercost;
	 *
	 * the innercost shouldn't be used it.  Instead the cost of hashing the
	 * innerpath should be used
	 *
	 * ASSUME innercost is 1 for now -- a horrible hack - jolly temp +=
	 * outercost + (nrun + 1);
	 *
	 * But we must add innercost to result.		- vadim 04/24/97
	 */
	temp += outercost + innercost + (nrun + 1);

	temp += _cpu_page_wight_ * (outersize + nrun * innersize);
	Assert(temp >= 0);

	return (temp);
}

/*
 * compute-rel-size--
 *	  Computes the size of each relation in 'rel-list' (after applying
 *	  restrictions), by multiplying the selectivity of each restriction
 *	  by the original size of the relation.
 *
 *	  Sets the 'size' field for each relation entry with this computed size.
 *
 * Returns the size.
 */
int
compute_rel_size(Rel *rel)
{
	Cost		temp;
	int			temp1;

	temp = rel->tuples * product_selec(rel->clauseinfo);
	Assert(temp >= 0);
	if (temp >= (MAXINT - 1))
		temp1 = MAXINT;
	else
		temp1 = ceil((double) temp);
	Assert(temp1 >= 0);
	Assert(temp1 <= MAXINT);
	return (temp1);
}

/*
 * compute-rel-width--
 *	  Computes the width in bytes of a tuple from 'rel'.
 *
 * Returns the width of the tuple as a fixnum.
 */
int
compute_rel_width(Rel *rel)
{
	return (compute_targetlist_width(get_actual_tlist(rel->targetlist)));
}

/*
 * compute-targetlist-width--
 *	  Computes the width in bytes of a tuple made from 'targetlist'.
 *
 * Returns the width of the tuple as a fixnum.
 */
static int
compute_targetlist_width(List *targetlist)
{
	List	   *temp_tl;
	int			tuple_width = 0;

	foreach(temp_tl, targetlist)
	{
		tuple_width = tuple_width +
			compute_attribute_width(lfirst(temp_tl));
	}
	return (tuple_width);
}

/*
 * compute-attribute-width--
 *	  Given a target list entry, find the size in bytes of the attribute.
 *
 *	  If a field is variable-length, it is assumed to be at least the size
 *	  of a TID field.
 *
 * Returns the width of the attribute as a fixnum.
 */
static int
compute_attribute_width(TargetEntry *tlistentry)
{
	int			width = get_typlen(tlistentry->resdom->restype);

	if (width < 0)
		return (_DEFAULT_ATTRIBUTE_WIDTH_);
	else
		return (width);
}

/*
 * compute-joinrel-size--
 *	  Computes the size of the join relation 'joinrel'.
 *
 * Returns a fixnum.
 */
int
compute_joinrel_size(JoinPath *joinpath)
{
	Cost		temp = 1.0;
	int			temp1 = 0;

	temp *= ((Path *) joinpath->outerjoinpath)->parent->size;
	temp *= ((Path *) joinpath->innerjoinpath)->parent->size;

	temp = temp * product_selec(joinpath->pathclauseinfo);
	if (temp >= (MAXINT - 1))
		temp1 = MAXINT;
	else
	{

		/*
		 * should be ceil here, we don't want joinrel size's of one, do
		 * we?
		 */
		temp1 = ceil((double) temp);
	}
	Assert(temp1 >= 0);

	return (temp1);
}

/*
 * page-size--
 *	  Returns an estimate of the number of pages covered by a given
 *	  number of tuples of a given width (size in bytes).
 */
int
page_size(int tuples, int width)
{
	int			temp = 0;

	temp = ceil((double) (tuples * (width + sizeof(HeapTupleData)))
				/ BLCKSZ);
	Assert(temp >= 0);
	return (temp);
}

static double
base_log(double x, double b)
{
	return (log(x) / log(b));
}
