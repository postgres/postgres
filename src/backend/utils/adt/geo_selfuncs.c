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

/* PROJECT_ULB */
static double calc_areasel(TypeCacheEntry *typcache, VariableStatData *vardata,
						   const RangeType *constval, Oid operator);
static double calc_areajoinsel(TypeCacheEntry *typcache, VariableStatData *vardata,
						       const RangeType *constval, Oid operator);
static double calc_positionsel(TypeCacheEntry *typcache, VariableStatData *vardata,
						  const RangeType *constval, Oid operator);
// static double default_geo_range_selectivity(Oid operator);

/*
 * Selectivity for operators that depend on area, such as "overlap".
 */

Datum
areasel(PG_FUNCTION_ARGS)
{
    // TODO
	PG_RETURN_FLOAT8(0.005);
}

Datum
areajoinsel(PG_FUNCTION_ARGS)
{
    PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
    Oid         operator = PG_GETARG_OID(1);
    List       *args = (List *) PG_GETARG_POINTER(2);
    JoinType    jointype = (JoinType) PG_GETARG_INT16(3);
    SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
    Oid         collation = PG_GET_COLLATION();

    double      selec = 0.01;

    VariableStatData    vardata1,
                        vardata2;
    Oid                 opfuncoid;
    AttStatsSlot        sslot1_hist,
                        sslot1_val,
                        sslot2_hist,
                        sslot2_val;
    int                 nhist1,
                        nhist2,
                        nvals1,
                        nvals2,
                        i;
    RangeBound          *hist1_lower,
                        *hist2_lower,
                        upper;
    float8              *begin1,
                        *end1,
                        *continue1,
                        *type4_1,
                        *begin2,
                        *end2,
                        *continue2,
                        *type4_2;
    Form_pg_statistic   stats1 = NULL;
    TypeCacheEntry      *typcache = NULL;
    bool                join_is_reversed,
                        empty;

    get_join_variables(root, args, sjinfo,
                       &vardata1, &vardata2, &join_is_reversed);

    typcache = range_get_typcache(fcinfo, vardata1.vartype);
    opfuncoid = get_opcode(operator);

    memset(&sslot1_hist, 0, sizeof(sslot1_hist));
    memset(&sslot1_val, 0, sizeof(sslot1_val));
    memset(&sslot2_hist, 0, sizeof(sslot2_hist));
    memset(&sslot2_val, 0, sizeof(sslot2_val));

    /* Can't use the histogram with insecure range support functions */
    if (!statistic_proc_security_check(&vardata1, opfuncoid))
        PG_RETURN_FLOAT8((float8) selec);

    if (HeapTupleIsValid(vardata1.statsTuple))
    {
        stats1 = (Form_pg_statistic) GETSTRUCT(vardata1.statsTuple);
        /* Try to get fraction of empty ranges */
        if (!get_attstatsslot(&sslot1_hist, vardata1.statsTuple,
                             STATISTIC_KIND_BOUNDS_HISTOGRAM,
                             InvalidOid, ATTSTATSSLOT_VALUES) ||
            !get_attstatsslot(&sslot1_val, vardata1.statsTuple,
                             STATISTIC_KIND_RANGE_TYPE_HISTOGRAM,
                             InvalidOid, ATTSTATSSLOT_VALUES) ||
            !get_attstatsslot(&sslot2_val, vardata2.statsTuple,
                             STATISTIC_KIND_BOUNDS_HISTOGRAM,
                             InvalidOid, ATTSTATSSLOT_VALUES) ||
            !get_attstatsslot(&sslot2_val, vardata2.statsTuple,
                             STATISTIC_KIND_RANGE_TYPE_HISTOGRAM,
                             InvalidOid, ATTSTATSSLOT_VALUES))
        {
            ReleaseVariableStats(vardata1);
            ReleaseVariableStats(vardata2);
            PG_RETURN_FLOAT8((float8) selec);
        }
    }

    nhist1 = sslot1_hist.nvalues;
    nhist2 = sslot2_hist.nvalues;
    hist1_lower = (RangeBound *) palloc(sizeof(RangeBound) * nhist1);
    hist2_lower = (RangeBound *) palloc(sizeof(RangeBound) * nhist2);

    for (i = 0; i < nhist1; i++)
    {
        range_deserialize(typcache, DatumGetRangeTypeP(sslot1_hist.values[i]), &hist1_lower[i], &upper, &empty);
        /* The histogram should not contain any empty ranges */
        if (empty)
            elog(ERROR, "bounds histogram contains an empty range");
    }

    for (i = 0; i < nhist2; i++)
    {
        range_deserialize(typcache, DatumGetRangeTypeP(sslot2_hist.values[i]), &hist2_lower[i], &upper, &empty);
        /* The histogram should not contain any empty ranges */
        if (empty)
            elog(ERROR, "bounds histogram contains an empty range");
    }

    nvals1 = sslot1_val.nvalues;  // Notice that this is equal to: (nhist1-1)*4
    nvals2 = sslot2_val.nvalues;
    nvals1 = nvals1/4;  // This will then be equal to the number of bins or equivalently (nhist1-1)
    nvals2 = nvals2/4;
    begin1 = (float8 *) palloc(sizeof(float8) * nvals1);
    end1 = (float8 *) palloc(sizeof(float8) * nvals1);
    continue1 = (float8 *) palloc(sizeof(float8) * nvals1);
    type4_1 = (float8 *) palloc(sizeof(float8) * nvals1);
    begin2 = (float8 *) palloc(sizeof(float8) * nvals2);
    end2 = (float8 *) palloc(sizeof(float8) * nvals1);
    continue2 = (float8 *) palloc(sizeof(float8) * nvals1);
    type4_2 = (float8 *) palloc(sizeof(float8) * nvals1);

    for (i = 0; i < nvals1; i++)
    {
        begin1[i] = DatumGetFloat8(sslot1_val.values[i*4]);
        end1[i] = DatumGetFloat8(sslot1_val.values[i*4+1]);
        continue1[i] = DatumGetFloat8(sslot1_val.values[i*4+2]);
        type4_1[i] = DatumGetFloat8(sslot1_val.values[i*4+3]);
    }

    for (i = 0; i < nvals2; i++)
    {
        begin2[i] = DatumGetFloat8(sslot2_val.values[i*4]);
        end2[i] = DatumGetFloat8(sslot2_val.values[i*4+1]);
        continue2[i] = DatumGetFloat8(sslot2_val.values[i*4+2]);
        type4_2[i] = DatumGetFloat8(sslot2_val.values[i*4+3]);
    }

    for (i = 0; i < nvals1; i++)
    {
        printf("%f - %f - %f - %f\n", begin1[i], end1[i], continue1[i], type4_1[i]);
        printf("%f - %f - %f - %f\n\n", begin2[i], end2[i], continue2[i], type4_2[i]);
    }

    printf("hist_lower = [");
    for (i = 0; i < nhist1; i++)
    {
        printf("%d", DatumGetInt16(hist1_lower[i].val));
        if (i < nhist1 - 1)
            printf(", ");
    }
    printf("]\n");

    fflush(stdout);

    pfree(hist1_lower);
    pfree(hist2_lower);
    pfree(begin1);
    pfree(begin2);
    pfree(end1);
    pfree(end2);
    pfree(continue1);
    pfree(continue2);
    pfree(type4_1);
    pfree(type4_2);

    free_attstatsslot(&sslot1_hist);
    free_attstatsslot(&sslot2_hist);
    free_attstatsslot(&sslot1_val);
    free_attstatsslot(&sslot2_val);

    ReleaseVariableStats(vardata1);
    ReleaseVariableStats(vardata2);

    CLAMP_PROBABILITY(selec);
    PG_RETURN_FLOAT8((float8) selec);
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
    // TODO
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
