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
static void calc_proba_full_overlap(TypeCacheEntry *typcache, RangeBound binlow1, RangeBound binlow2,
                                    RangeBound binup1, RangeBound binup2, double * probabilities, bool has_subdiff);
static void calc_proba_part_overlap(TypeCacheEntry *typcache, RangeBound binlow1, RangeBound binlow2,
                                    RangeBound binup1, RangeBound binup2, double * probabilities, bool has_subdiff);
static void swapprobabilities(double * probabilities);

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
                        samplerows1,
                        samplerows2,
                        i;
    RangeBound          *hist1_lower,
                        *hist2_lower,
                        upper;
    int                 *begin1,
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
                        empty,
                        has_subdiff;

    get_join_variables(root, args, sjinfo,
                       &vardata1, &vardata2, &join_is_reversed);

    typcache = range_get_typcache(fcinfo, vardata1.vartype);
    opfuncoid = get_opcode(operator);
    has_subdiff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);

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
            !get_attstatsslot(&sslot2_hist, vardata2.statsTuple,
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

    for (i = 0; i < nhist1-1; i++)
    {
        range_deserialize(typcache, DatumGetRangeTypeP(sslot1_hist.values[i]), &hist1_lower[i], &upper, &empty);
        /* The histogram should not contain any empty ranges */
        if (empty)
            elog(ERROR, "bounds histogram contains an empty range");
    }
    /* I take the last upper bound as last lower bound to make sure everything is encompassed */
    range_deserialize(typcache, DatumGetRangeTypeP(sslot1_hist.values[nhist1-1]), &upper, &hist1_lower[nhist1-1], &empty);

    for (i = 0; i < nhist2-1; i++)
    {
        range_deserialize(typcache, DatumGetRangeTypeP(sslot2_hist.values[i]), &hist2_lower[i], &upper, &empty);
        /* The histogram should not contain any empty ranges */
        if (empty)
            elog(ERROR, "bounds histogram contains an empty range");
    }
    /* I take the last upper bound as last lower bound to make sure everything is encompassed */
    range_deserialize(typcache, DatumGetRangeTypeP(sslot2_hist.values[nhist2-1]), &upper, &hist2_lower[nhist2-1], &empty);

    nvals1 = sslot1_val.nvalues;  // Notice that this is equal to: (nhist1-1)*4
    nvals2 = sslot2_val.nvalues;
    nvals1 = nvals1/4;  // This will then be equal to the number of bins or equivalently (nhist1-1)
    nvals2 = nvals2/4;
    begin1 = (int *) palloc(sizeof(int) * nvals1);
    end1 = (int *) palloc(sizeof(int) * nvals1);
    continue1 = (int *) palloc(sizeof(int) * nvals1);
    type4_1 = (int *) palloc(sizeof(int) * nvals1);
    begin2 = (int *) palloc(sizeof(int) * nvals2);
    end2 = (int *) palloc(sizeof(int) * nvals1);
    continue2 = (int *) palloc(sizeof(int) * nvals1);
    type4_2 = (int *) palloc(sizeof(int) * nvals1);

    for (i = 0; i < nvals1; i++)
    {
        begin1[i] = (int) DatumGetFloat8(sslot1_val.values[i*4]);
        end1[i] = (int) DatumGetFloat8(sslot1_val.values[i*4+1]);
        continue1[i] = (int) DatumGetFloat8(sslot1_val.values[i*4+2]);
        type4_1[i] = (int) DatumGetFloat8(sslot1_val.values[i*4+3]);
    }

    for (i = 0; i < nvals2; i++)
    {
        begin2[i] = (int) DatumGetFloat8(sslot2_val.values[i*4]);
        end2[i] = (int) DatumGetFloat8(sslot2_val.values[i*4+1]);
        continue2[i] = (int) DatumGetFloat8(sslot2_val.values[i*4+2]);
        type4_2[i] = (int) DatumGetFloat8(sslot2_val.values[i*4+3]);
    }

    int hist1_idx = 0, hist2_idx = 0;  // Corresponds to the bin number we are analysing for each histogram
    int type1_1, type2_1, type3_1, type1_2, type2_2, type3_2;
    int res;
    double sum_overlaps;
    double probabilities[16];
    double count[16];

    /* Find the first overlapping bins of the histogram */
    while (range_cmp_bounds(typcache, &(hist1_lower[hist1_idx+1]), &(hist2_lower[hist2_idx])) <= 0)
    {
        hist1_idx++;
    }
    while (range_cmp_bounds(typcache, &(hist2_lower[hist2_idx+1]), &(hist1_lower[hist1_idx])) <= 0)
    {
        hist2_idx++;
    }

    sum_overlaps = 0;
    do
    {
        /* Compute the number of type 1/2/3 for each bin */
        type2_1 = end1[hist1_idx] - type4_1[hist1_idx];
        type3_1 = begin1[hist1_idx] - type4_1[hist1_idx];
        type1_1 = continue1[hist1_idx] - type2_1;

        type2_2 = end2[hist2_idx] - type4_2[hist2_idx];
        type3_2 = begin2[hist2_idx] - type4_2[hist2_idx];
        type1_2 = continue2[hist2_idx] - type2_2;

        /* Boolean that tells if i can subdiff; is false if a bound is infinite or if has_subdiff is false */
        empty = !(hist1_lower[hist1_idx].infinite || hist1_lower[hist1_idx+1].infinite || hist2_lower[hist2_idx].infinite
                    || hist2_lower[hist2_idx+1].infinite) && has_subdiff;

        /* Case : There is entire overlap */
        if((range_cmp_bounds(typcache, &(hist1_lower[hist1_idx]), &(hist2_lower[hist2_idx])) <= 0 &&
            range_cmp_bounds(typcache, &(hist1_lower[hist1_idx+1]), &(hist2_lower[hist2_idx+1])) >= 0) ||
           (range_cmp_bounds(typcache, &(hist2_lower[hist2_idx]), &(hist1_lower[hist1_idx])) <= 0 &&
            range_cmp_bounds(typcache, &(hist2_lower[hist2_idx+1]), &(hist1_lower[hist1_idx+1])) >= 0))
        {
            calc_proba_full_overlap(typcache, hist1_lower[hist1_idx], hist2_lower[hist2_idx],
                                    hist1_lower[hist1_idx+1], hist2_lower[hist2_idx+1],
                                    probabilities, empty);
        }
        /* Case : There is partial overlap */
        else
        {
            fflush(stdout);
            calc_proba_part_overlap(typcache, hist1_lower[hist1_idx], hist2_lower[hist2_idx],
                                    hist1_lower[hist1_idx+1], hist2_lower[hist2_idx+1],
                                    probabilities, empty);
        }
        /*
         * Could maybe add a case where bounds are equal ? rn, equal bounds is considered as entire overlap.
         * Note that this would only be useful in a case where there is no diff function for the RangeBounds.
         * If there is a diff function, then the S1 probability will rightfully be set to zero.
         */

        /* Count the number of resulting overlaps */
        count[0] = probabilities[0] * type1_1 * type1_2; //1x1
        count[1] = probabilities[1] * type1_1 * type2_2; //1x2
        count[2] = probabilities[2] * type1_1 * type3_2; //1x3
        count[3] = probabilities[3] * type1_1 * type4_2[hist2_idx]; //1x4
        count[4] = probabilities[4] * type2_1 * type1_2; //2x1
        count[5] = probabilities[5] * type2_1 * type2_2; //2x2
        count[6] = probabilities[6] * type2_1 * type3_2; //2x3
        count[7] = probabilities[7] * type2_1 * type4_2[hist2_idx]; //2x4
        count[8] = probabilities[8] * type3_1 * type1_2; //3x1
        count[9] = probabilities[9] * type3_1 * type2_2; //3x2
        count[10] = probabilities[10] * type3_1 * type3_2; //3x3
        count[11] = probabilities[11] * type3_1 * type4_2[hist2_idx]; //3x4
        count[12] = probabilities[12] * type4_1[hist1_idx] * type1_2; //4x1
        count[13] = probabilities[13] * type4_1[hist1_idx] * type2_2; //4x2
        count[14] = probabilities[14] * type4_1[hist1_idx] * type3_2; //4x3
        count[15] = probabilities[15] * type4_1[hist1_idx] * type4_2[hist2_idx]; //4x4

        for (i = 0; i < 16; i++)
        {
            sum_overlaps += count[i];
        }
        printf("idx : %d, %d\n", hist1_idx, hist2_idx);
        printf("%f - %f - %f - %f - %f - %f - %f - %f - %f - %f - %f - %f - %f - %f - %f - %f\n",
        probabilities[0], probabilities[1], probabilities[2], probabilities[3], probabilities[4], probabilities[5],
        probabilities[6], probabilities[7], probabilities[8], probabilities[9], probabilities[10], probabilities[11],
        probabilities[12], probabilities[13], probabilities[14], probabilities[15]);
        printf("Guess total nb rows currently : %f\n", sum_overlaps);
        fflush(stdout);

        /* Go to next bin */
        res = range_cmp_bounds(typcache, &(hist1_lower[hist1_idx+1]), &(hist2_lower[hist2_idx+1]));
        if (res == 0)
        {
            hist1_idx++;
            hist2_idx++;
        }
        else if (res > 0)
        {
            hist2_idx++;
        }
        else
        {
            hist1_idx++;
        }
    } while (hist1_idx < nhist1-1 && hist2_idx < nhist2-1);

    samplerows1 = 0;
    samplerows2 = 0;
    for (i = 0; i < nhist1-1; i++) {
        samplerows1 += begin1[i];
    }
    for (i = 0; i < nhist2-1; i++) {
        samplerows2 += begin2[i];
    }
    
    printf("Guessed nb of rows : %f\n", sum_overlaps);
    printf("Maxnb rows : %d\n", samplerows1*samplerows2);
    printf("Selectivity : %f\n", sum_overlaps / (samplerows1*samplerows2));
    fflush(stdout);

    selec = sum_overlaps / (samplerows1 * samplerows2);

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

static void
calc_proba_full_overlap(TypeCacheEntry *typcache, RangeBound binlow1, RangeBound binlow2,
                        RangeBound binup1, RangeBound binup2, double * probabilities, bool has_subdiff)
{
    double S1, S2, S3, size;
    bool swapped;
    fflush(stdout);
    /* Swap values to have the lowest one in binlow1 or the highest in binup1 of the two lows are equal */
    swapped = range_cmp_bounds(typcache, &binlow1, &binlow2) > 0 || range_cmp_bounds(typcache, &binup1, &binup2) < 0;
    if (swapped)
    {
        RangeBound temp;
        temp = binlow1;
        binlow1 = binlow2;
        binlow2 = temp;
        temp = binup1;
        binup1 = binup2;
        binup2 = temp;
    }
    if (has_subdiff)
    {
        size = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
                                                typcache->rng_collation, 
                                                binup1.val, binlow1.val));
        S1 = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
                                                typcache->rng_collation, 
                                                binlow2.val, binlow1.val));
        S2 = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
                                                typcache->rng_collation, 
                                                binup2.val, binlow2.val));
        S3 = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
                                                typcache->rng_collation, 
                                                binup1.val, binup2.val));
        S1 = S1/size;
        S2 = S2/size;
        S3 = S3/size;
    }
    else  /* Take default values */
    {
        S1 = 0.33;
        S2 = 0.34;
        S3 = 0.33;
    }

    probabilities[0] = 0.0; // 1x1
    probabilities[1] = 0.0; // 1x2
    probabilities[2] = 1.0; // 1x3
    probabilities[3] = 1.0; // 1x4
    probabilities[4] = 0.0; // 2x1
    probabilities[5] = 0.0; // 2x2
    probabilities[6] = (1.0/2.0)*S2; // 2x3
    probabilities[7] = (2.0/3.0)*S2; // 2x4
    probabilities[8] = S2; // 3x1
    probabilities[9] = (1.0/2.0)*S2; // 3x2
    probabilities[10] = S2; // 3x3
    probabilities[11] = (2.0/3.0)*S2; // 3x4
    probabilities[12] = S2*S2 + 2*S2*S3; // 4x1 
    probabilities[13] = (2.0/3.0)*S2*S2 + S2*S3; // 4x2
    probabilities[14] = 1.0 - S1*S1 - S3*S3 - (1.0/3.0)*S2*S2 - S1*S2; // 4x3
    probabilities[15] = (4.0/3.0)*S1*S2 + 2*S1*S3 + (2.0/3.0)*S2*S2 + (4.0/3.0)*S1*S3; // 4x4

    if (swapped)
    {
        swapprobabilities(probabilities);
    }
}

static void
calc_proba_part_overlap(TypeCacheEntry *typcache, RangeBound binlow1, RangeBound binlow2,
                        RangeBound binup1, RangeBound binup2, double * probabilities, bool has_subdiff)
{
    double S1, S2_1, S2_2, S3, size_1, size_2;
    bool swapped;
    /* Swap values to have the lowest one in binlow1 */
    swapped = range_cmp_bounds(typcache, &binlow1, &binlow2) > 0;
    if (swapped)
    {
        RangeBound temp;
        temp = binlow1;
        binlow1 = binlow2;
        binlow2 = temp;
        temp = binup1;
        binup1 = binup2;
        binup2 = temp;
    }
    if (has_subdiff) {
        size_1 = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
                                                typcache->rng_collation, 
                                                binup1.val, binlow1.val));
        size_2 = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
                                                typcache->rng_collation, 
                                                binup2.val, binlow2.val));
        S1 = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
                                                typcache->rng_collation, 
                                                binlow2.val, binlow1.val));
        S3 = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
                                                typcache->rng_collation, 
                                                binup2.val, binup1.val));
        S1 = S1/size_1; // Is in fact the probability S1_1, but I know S1_2 is zero
        S2_1 = 1.0 - S1;
        S3 = S3/size_2; // Is in fact the probability S3_2, but I know S3_1 is zero
        S2_2 = 1.0 - S3;
    }
    else /* Take default values */
    {
        S1 = 0.5;
        S3 = 0.5;
        S2_1 = 0.5;
        S2_2 = 0.5;
    }

    probabilities[0] = 0; // 1x1
    probabilities[1] = 0; // 1x2
    probabilities[2] = S2_2; // 1x3
    probabilities[3] = 1.0 - S3*S3; // 1x4
    probabilities[4] = 0; // 2x1
    probabilities[5] = 0; // 2x2
    probabilities[6] = (1.0/2.0)*S2_1*S2_2; // 2x3
    probabilities[7] = (2.0/3.0)*S2_1*S2_2*S2_2 + S2_1*S2_2*S3; // 2x4
    probabilities[8] = 0.0; // 3x1
    probabilities[9] = (1.0/2.0)*S2_1*S2_2 + S2_1*S3; // 3x2
    probabilities[10] = S2_2; // 3x3
    probabilities[11] = (2.0/3)*S2_2*S2_2 + 2.0*S2_2*S3; // 3x4
    probabilities[12] = S2_1*S2_1; // 4x1 
    probabilities[13] = (2.0/3.0)*S2_1*S2_1; // 4x2
    probabilities[14] = (2.0/3.0)*S2_1*S2_1; // 4x3
    probabilities[15] = (4.0/3.0)*S1*S2_1*S2_2*S2_2 + (2.0/3)*S2_1*S2_1*S2_2*S2_2 +
                        2.0*S1*S2_1*S2_2*S3 + (4.0/3.0)*S2_1*S2_1*S2_2*S3; // 4x4

    if (swapped)
    {
        swapprobabilities(probabilities);
    }
}

static void
swapprobabilities(double * probabilities)
{
    float temp;
    /* swap 2x1 and 1x2 */
    temp = probabilities[1];
    probabilities[1] = probabilities[4];
    probabilities[4] = temp;
    /* swap 3x1 and 1x3 */
    temp = probabilities[2];
    probabilities[2] = probabilities[8];
    probabilities[8] = temp;
    /* swap 4x1 and 1x4 */
    temp = probabilities[3];
    probabilities[3] = probabilities[12];
    probabilities[12] = temp;
    /* swap 2x3 and 3x2 */
    temp = probabilities[6];
    probabilities[6] = probabilities[9];
    probabilities[9] = temp;
    /* swap 4x2 and 4x2 */
    temp = probabilities[7];
    probabilities[7] = probabilities[13];
    probabilities[13] = temp;
    /* swap 3x4 and 4x3 */
    temp = probabilities[11];
    probabilities[11] = probabilities[14];
    probabilities[14] = temp;
}