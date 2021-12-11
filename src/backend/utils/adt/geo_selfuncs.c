/*-------------------------------------------------------------------------
 *
 * geo_selfuncs.c
 *	  Selectivity routines registered in the operator catalog in the
 *	  "oprrest" and "oprjoin" attributes.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/geo_selfuncs.c
 *
 *	XXX These are totally bogus.  Perhaps someone will make them do
 *	something reasonable, someday.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/builtins.h"
#include "utils/geo_decls.h"
#include "access/htup_details.h"
#include "catalog/pg_statistic.h"
#include "nodes/pg_list.h"
#include "optimizer/pathnode.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/selfuncs.h"
#include "utils/rangetypes.h"


/*
 *	Selectivity functions for geometric operators.  These are bogus -- unless
 *	we know the actual key distribution in the index, we can't make a good
 *	prediction of the selectivity of these operators.
 *
 *	Note: the values used here may look unreasonably small.  Perhaps they
 *	are.  For now, we want to make sure that the optimizer will make use
 *	of a geometric index if one is available, so the selectivity had better
 *	be fairly small.
 *
 *	In general, GiST needs to search multiple subtrees in order to guarantee
 *	that all occurrences of the same key have been found.  Because of this,
 *	the estimated cost for scanning the index ought to be higher than the
 *	output selectivity would indicate.  gistcostestimate(), over in selfuncs.c,
 *	ought to be adjusted accordingly --- but until we can generate somewhat
 *	realistic numbers here, it hardly matters...
 */


/*
 * Selectivity for operators that depend on area, such as "overlap".
 */

Datum
areasel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.005);
}

Datum
areajoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.005);
}

/*
 *	positionsel
 *
 * How likely is a box to be strictly left of (right of, above, below)
 * a given box?
 */

Datum
positionsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.1);
}

Datum
positionjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.1);
}

/*
 *	contsel -- How likely is a box to contain (be contained by) a given box?
 *
 * This is a tighter constraint than "overlap", so produce a smaller
 * estimate than areasel does.
 */

Datum
contsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.001);
}

Datum
contjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.001);
}

HistSlot *construct_hist_slots(float8 *hist_bins, float8 *slots_values, int slots_count) {
    HistSlot *hist = (HistSlot *) palloc(sizeof(HistSlot) * slots_count);
    for(int i = 0; i < slots_count; i++) {
        hist[i].lower = hist_bins[i];
        hist[i].upper = hist_bins[i + 1];
        hist[i].value = slots_values[i];
    }

    return hist;
}


/*
 * Range Overlaps Join Selectivity.
 */
Datum
rangeoverlapsjoinsel(PG_FUNCTION_ARGS)
{
    PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
    Oid         operator = PG_GETARG_OID(1);
    List       *args = (List *) PG_GETARG_POINTER(2);
    JoinType    jointype = (JoinType) PG_GETARG_INT16(3);
    SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
    Oid         collation = PG_GET_COLLATION();

    double      selec = 0.005;

    VariableStatData vardata1;
    VariableStatData vardata2;
    Oid         opfuncoid;
    
    AttStatsSlot sslot1;
    AttStatsSlot sslot2;
    
    int         bins_count;
    int         slots_count;

    HistSlot *hist;

    Form_pg_statistic stats1 = NULL;
    TypeCacheEntry *typcache = NULL;
    bool        join_is_reversed;
    bool        empty;

    get_join_variables(root, args, sjinfo,
                       &vardata1, &vardata2, &join_is_reversed);

    typcache = range_get_typcache(fcinfo, vardata1.vartype);
    opfuncoid = get_opcode(operator);

    memset(&sslot1, 0, sizeof(sslot1));

    /* Can't use the histogram with insecure range support functions */
    if (!statistic_proc_security_check(&vardata1, opfuncoid))
        PG_RETURN_FLOAT8((float8) selec);

    if (HeapTupleIsValid(vardata1.statsTuple))
    {
        stats1 = (Form_pg_statistic) GETSTRUCT(vardata1.statsTuple);

        if (!get_attstatsslot(&sslot1, vardata1.statsTuple,
                             STATISTIC_KIND_BINS_HISTOGRAM,
                             InvalidOid, ATTSTATSSLOT_VALUES))
        {
            ReleaseVariableStats(vardata1);
            ReleaseVariableStats(vardata2);
            PG_RETURN_FLOAT8((float8) selec);
        }

        if (!get_attstatsslot(&sslot2, vardata1.statsTuple,
                             STATISTIC_KIND_BINS_VALUES_HISTOGRAM,
                             InvalidOid, ATTSTATSSLOT_VALUES))
        {
            ReleaseVariableStats(vardata1);
            ReleaseVariableStats(vardata2);
            PG_RETURN_FLOAT8((float8) selec);
        }
    }

    bins_count = sslot1.nvalues;  //number of bins
    slots_count = sslot2.nvalues;  //number of slots (#bins - 1)

    float8 *hist_bins = (float8 *) palloc(sizeof(float8) * bins_count);
    float8 *slots_values = (float8 *) palloc(sizeof(float8) * slots_count);

    for(int i = 0; i < bins_count; i++) {
        hist_bins[i] = DatumGetFloat8(sslot1.values[i]);
        // printf("hist_bins[%d]: %f\n", i, hist_bins[i]);
    }

    for(int i = 0; i < slots_count; i++) {
        slots_values[i] = DatumGetFloat8(sslot2.values[i]);
        // printf("slots_bins[%d]: %f\n", i, slots_values[i]);
    }

    hist = construct_hist_slots(hist_bins, slots_values, slots_count);
    for(int i = 0; i < slots_count; i++) {
        printf("slot[%d]: %f -> %f : %f \n", i, hist[i].lower, hist[i].upper, hist[i].value);
    }

    fflush(stdout);

    free_attstatsslot(&sslot1);
    free_attstatsslot(&sslot2);

    ReleaseVariableStats(vardata1);
    ReleaseVariableStats(vardata2);

    CLAMP_PROBABILITY(selec);
    PG_RETURN_FLOAT8((float8) selec);
}