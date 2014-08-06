/*-------------------------------------------------------------------------
 *
 * sortsupport.c
 *	  Support routines for accelerated sorting.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/sortsupport.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/* See sortsupport.h */
#define SORTSUPPORT_INCLUDE_DEFINITIONS

#include "access/nbtree.h"
#include "fmgr.h"
#include "utils/lsyscache.h"
#include "utils/sortsupport.h"


/* Info needed to use an old-style comparison function as a sort comparator */
typedef struct
{
	FunctionCallInfoData fcinfo;	/* reusable callinfo structure */
	FmgrInfo	flinfo;			/* lookup data for comparison function */
} SortShimExtra;


/*
 * Shim function for calling an old-style comparator
 *
 * This is essentially an inlined version of FunctionCall2Coll(), except
 * we assume that the FunctionCallInfoData was already mostly set up by
 * PrepareSortSupportComparisonShim.
 */
static int
comparison_shim(Datum x, Datum y, SortSupport ssup)
{
	SortShimExtra *extra = (SortShimExtra *) ssup->ssup_extra;
	Datum		result;

	extra->fcinfo.arg[0] = x;
	extra->fcinfo.arg[1] = y;

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
												 sizeof(SortShimExtra));

	/* Lookup the comparison function */
	fmgr_info_cxt(cmpFunc, &extra->flinfo, ssup->ssup_cxt);

	/* We can initialize the callinfo just once and re-use it */
	InitFunctionCallInfoData(extra->fcinfo, &extra->flinfo, 2,
							 ssup->ssup_collation, NULL, NULL);
	extra->fcinfo.argnull[0] = false;
	extra->fcinfo.argnull[1] = false;

	ssup->ssup_extra = extra;
	ssup->comparator = comparison_shim;
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
	Oid			sortSupportFunction;
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

	/* Look for a sort support function */
	sortSupportFunction = get_opfamily_proc(opfamily, opcintype, opcintype,
											BTSORTSUPPORT_PROC);
	if (OidIsValid(sortSupportFunction))
	{
		/*
		 * The sort support function can provide a comparator, but it can
		 * also choose not to so (e.g. based on the selected collation).
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
