/*-------------------------------------------------------------------------
 *
 * sortsupport.c
 *	  Support routines for accelerated sorting.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/sortsupport.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gist.h"
#include "access/nbtree.h"
#include "catalog/pg_am.h"
#include "fmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/sortsupport.h"


/* Info needed to use an old-style comparison function as a sort comparator */
typedef struct
{
	FmgrInfo	flinfo;			/* lookup data for comparison function */
	FunctionCallInfoBaseData fcinfo;	/* reusable callinfo structure */
} SortShimExtra;

#define SizeForSortShimExtra(nargs) (offsetof(SortShimExtra, fcinfo) + SizeForFunctionCallInfo(nargs))

/*
 * Shim function for calling an old-style comparator
 *
 * This is essentially an inlined version of FunctionCall2Coll(), except
 * we assume that the FunctionCallInfoBaseData was already mostly set up by
 * PrepareSortSupportComparisonShim.
 */
static int
comparison_shim(Datum x, Datum y, SortSupport ssup)
{
	SortShimExtra *extra = (SortShimExtra *) ssup->ssup_extra;
	Datum		result;

	extra->fcinfo.args[0].value = x;
	extra->fcinfo.args[1].value = y;

	/* just for paranoia's sake, we reset isnull each time */
	extra->fcinfo.isnull = false;

	result = FunctionCallInvoke(&extra->fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (extra->fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", extra->flinfo.fn_oid);

	return result;
}

/*
 * Set up a shim function to allow use of an old-style btree comparison
 * function as if it were a sort support comparator.
 */
void
PrepareSortSupportComparisonShim(Oid cmpFunc, SortSupport ssup)
{
	SortShimExtra *extra;

	extra = (SortShimExtra *) MemoryContextAlloc(ssup->ssup_cxt,
												 SizeForSortShimExtra(2));

	/* Lookup the comparison function */
	fmgr_info_cxt(cmpFunc, &extra->flinfo, ssup->ssup_cxt);

	/* We can initialize the callinfo just once and re-use it */
	InitFunctionCallInfoData(extra->fcinfo, &extra->flinfo, 2,
							 ssup->ssup_collation, NULL, NULL);
	extra->fcinfo.args[0].isnull = false;
	extra->fcinfo.args[1].isnull = false;

	ssup->ssup_extra = extra;
	ssup->comparator = comparison_shim;
}

/*
 * Look up and call sortsupport function to setup SortSupport comparator;
 * or if no such function exists or it declines to set up the appropriate
 * state, prepare a suitable shim.
 */
static void
FinishSortSupportFunction(Oid opfamily, Oid opcintype, SortSupport ssup)
{
	Oid			sortSupportFunction;

	/* Look for a sort support function */
	sortSupportFunction = get_opfamily_proc(opfamily, opcintype, opcintype,
											BTSORTSUPPORT_PROC);
	if (OidIsValid(sortSupportFunction))
	{
		/*
		 * The sort support function can provide a comparator, but it can also
		 * choose not to so (e.g. based on the selected collation).
		 */
		OidFunctionCall1(sortSupportFunction, PointerGetDatum(ssup));
	}

	if (ssup->comparator == NULL)
	{
		Oid			sortFunction;

		sortFunction = get_opfamily_proc(opfamily, opcintype, opcintype,
										 BTORDER_PROC);

		if (!OidIsValid(sortFunction))
			elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
				 BTORDER_PROC, opcintype, opcintype, opfamily);

		/* We'll use a shim to call the old-style btree comparator */
		PrepareSortSupportComparisonShim(sortFunction, ssup);
	}
}

/*
 * Fill in SortSupport given an ordering operator (btree "<" or ">" operator).
 *
 * Caller must previously have zeroed the SortSupportData structure and then
 * filled in ssup_cxt, ssup_collation, and ssup_nulls_first.  This will fill
 * in ssup_reverse as well as the comparator function pointer.
 */
void
PrepareSortSupportFromOrderingOp(Oid orderingOp, SortSupport ssup)
{
	Oid			opfamily;
	Oid			opcintype;
	int16		strategy;

	Assert(ssup->comparator == NULL);

	/* Find the operator in pg_amop */
	if (!get_ordering_op_properties(orderingOp, &opfamily, &opcintype,
									&strategy))
		elog(ERROR, "operator %u is not a valid ordering operator",
			 orderingOp);
	ssup->ssup_reverse = (strategy == BTGreaterStrategyNumber);

	FinishSortSupportFunction(opfamily, opcintype, ssup);
}

/*
 * Fill in SortSupport given an index relation, attribute, and strategy.
 *
 * Caller must previously have zeroed the SortSupportData structure and then
 * filled in ssup_cxt, ssup_attno, ssup_collation, and ssup_nulls_first.  This
 * will fill in ssup_reverse (based on the supplied strategy), as well as the
 * comparator function pointer.
 */
void
PrepareSortSupportFromIndexRel(Relation indexRel, int16 strategy,
							   SortSupport ssup)
{
	Oid			opfamily = indexRel->rd_opfamily[ssup->ssup_attno - 1];
	Oid			opcintype = indexRel->rd_opcintype[ssup->ssup_attno - 1];

	Assert(ssup->comparator == NULL);

	if (indexRel->rd_rel->relam != BTREE_AM_OID)
		elog(ERROR, "unexpected non-btree AM: %u", indexRel->rd_rel->relam);
	if (strategy != BTGreaterStrategyNumber &&
		strategy != BTLessStrategyNumber)
		elog(ERROR, "unexpected sort support strategy: %d", strategy);
	ssup->ssup_reverse = (strategy == BTGreaterStrategyNumber);

	FinishSortSupportFunction(opfamily, opcintype, ssup);
}

/*
 * Fill in SortSupport given a GiST index relation
 *
 * Caller must previously have zeroed the SortSupportData structure and then
 * filled in ssup_cxt, ssup_attno, ssup_collation, and ssup_nulls_first.  This
 * will fill in ssup_reverse (always false for GiST index build), as well as
 * the comparator function pointer.
 */
void
PrepareSortSupportFromGistIndexRel(Relation indexRel, SortSupport ssup)
{
	Oid			opfamily = indexRel->rd_opfamily[ssup->ssup_attno - 1];
	Oid			opcintype = indexRel->rd_opcintype[ssup->ssup_attno - 1];
	Oid			sortSupportFunction;

	Assert(ssup->comparator == NULL);

	if (indexRel->rd_rel->relam != GIST_AM_OID)
		elog(ERROR, "unexpected non-gist AM: %u", indexRel->rd_rel->relam);
	ssup->ssup_reverse = false;

	/*
	 * Look up the sort support function. This is simpler than for B-tree
	 * indexes because we don't support the old-style btree comparators.
	 */
	sortSupportFunction = get_opfamily_proc(opfamily, opcintype, opcintype,
											GIST_SORTSUPPORT_PROC);
	if (!OidIsValid(sortSupportFunction))
		elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
			 GIST_SORTSUPPORT_PROC, opcintype, opcintype, opfamily);
	OidFunctionCall1(sortSupportFunction, PointerGetDatum(ssup));
}
