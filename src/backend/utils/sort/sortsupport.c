/*-------------------------------------------------------------------------
 *
 * sortsupport.c
 *	  Support routines for accelerated sorting.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/sortsupport.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

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
 * sortsupport.h defines inline versions of these functions if allowed by the
 * compiler; in which case the definitions below are skipped.
 */
#ifndef USE_INLINE

/*
 * Apply a sort comparator function and return a 3-way comparison result.
 * This takes care of handling reverse-sort and NULLs-ordering properly.
 */
int
ApplySortComparator(Datum datum1, bool isNull1,
					Datum datum2, bool isNull2,
					SortSupport ssup)
{
	int			compare;

	if (isNull1)
	{
		if (isNull2)
			compare = 0;		/* NULL "=" NULL */
		else if (ssup->ssup_nulls_first)
			compare = -1;		/* NULL "<" NOT_NULL */
		else
			compare = 1;		/* NULL ">" NOT_NULL */
	}
	else if (isNull2)
	{
		if (ssup->ssup_nulls_first)
			compare = 1;		/* NOT_NULL ">" NULL */
		else
			compare = -1;		/* NOT_NULL "<" NULL */
	}
	else
	{
		compare = (*ssup->comparator) (datum1, datum2, ssup);
		if (ssup->ssup_reverse)
			compare = -compare;
	}

	return compare;
}
#endif   /* ! USE_INLINE */

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
	Oid			sortFunction;
	bool		issupport;

	if (!get_sort_function_for_ordering_op(orderingOp,
										   &sortFunction,
										   &issupport,
										   &ssup->ssup_reverse))
		elog(ERROR, "operator %u is not a valid ordering operator",
			 orderingOp);

	if (issupport)
	{
		/* The sort support function should provide a comparator */
		OidFunctionCall1(sortFunction, PointerGetDatum(ssup));
		Assert(ssup->comparator != NULL);
	}
	else
	{
		/* We'll use a shim to call the old-style btree comparator */
		PrepareSortSupportComparisonShim(sortFunction, ssup);
	}
}
