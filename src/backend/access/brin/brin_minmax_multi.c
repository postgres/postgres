/*
 * brin_minmax_multi.c
 *		Implementation of Multi Min/Max opclass for BRIN
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * Implements a variant of minmax opclass, where the summary is composed of
 * multiple smaller intervals. This allows us to handle outliers, which
 * usually make the simple minmax opclass inefficient.
 *
 * Consider for example page range with simple minmax interval [1000,2000],
 * and assume a new row gets inserted into the range with value 1000000.
 * Due to that the interval gets [1000,1000000]. I.e. the minmax interval
 * got 1000x wider and won't be useful to eliminate scan keys between 2001
 * and 1000000.
 *
 * With minmax-multi opclass, we may have [1000,2000] interval initially,
 * but after adding the new row we start tracking it as two interval:
 *
 *   [1000,2000] and [1000000,1000000]
 *
 * This allows us to still eliminate the page range when the scan keys hit
 * the gap between 2000 and 1000000, making it useful in cases when the
 * simple minmax opclass gets inefficient.
 *
 * The number of intervals tracked per page range is somewhat flexible.
 * What is restricted is the number of values per page range, and the limit
 * is currently 32 (see values_per_range reloption). Collapsed intervals
 * (with equal minimum and maximum value) are stored as a single value,
 * while regular intervals require two values.
 *
 * When the number of values gets too high (by adding new values to the
 * summary), we merge some of the intervals to free space for more values.
 * This is done in a greedy way - we simply pick the two closest intervals,
 * merge them, and repeat this until the number of values to store gets
 * sufficiently low (below 50% of maximum values), but that is mostly
 * arbitrary threshold and may be changed easily).
 *
 * To pick the closest intervals we use the "distance" support procedure,
 * which measures space between two ranges (i.e. the length of an interval).
 * The computed value may be an approximation - in the worst case we will
 * merge two ranges that are slightly less optimal at that step, but the
 * index should still produce correct results.
 *
 * The compactions (reducing the number of values) is fairly expensive, as
 * it requires calling the distance functions, sorting etc. So when building
 * the summary, we use a significantly larger buffer, and only enforce the
 * exact limit at the very end. This improves performance, and it also helps
 * with building better ranges (due to the greedy approach).
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_minmax_multi.c
 */
#include "postgres.h"

/* needed for PGSQL_AF_INET */
#include <sys/socket.h>

#include "access/genam.h"
#include "access/brin.h"
#include "access/brin_internal.h"
#include "access/brin_tuple.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/float.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

/*
 * Additional SQL level support functions
 *
 * Procedure numbers must not use values reserved for BRIN itself; see
 * brin_internal.h.
 */
#define		MINMAX_MAX_PROCNUMS		1	/* maximum support procs we need */
#define		PROCNUM_DISTANCE		11	/* required, distance between values */

/*
 * Subtract this from procnum to obtain index in MinmaxMultiOpaque arrays
 * (Must be equal to minimum of private procnums).
 */
#define		PROCNUM_BASE			11

/*
 * Sizing the insert buffer - we use 10x the number of values specified
 * in the reloption, but we cap it to 8192 not to get too large. When
 * the buffer gets full, we reduce the number of values by half.
 */
#define		MINMAX_BUFFER_FACTOR			10
#define		MINMAX_BUFFER_MIN				256
#define		MINMAX_BUFFER_MAX				8192
#define		MINMAX_BUFFER_LOAD_FACTOR		0.5

typedef struct MinmaxMultiOpaque
{
	FmgrInfo	extra_procinfos[MINMAX_MAX_PROCNUMS];
	bool		extra_proc_missing[MINMAX_MAX_PROCNUMS];
	Oid			cached_subtype;
	FmgrInfo	strategy_procinfos[BTMaxStrategyNumber];
} MinmaxMultiOpaque;

/*
 * Storage type for BRIN's minmax reloptions
 */
typedef struct MinMaxMultiOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			valuesPerRange; /* number of values per range */
} MinMaxMultiOptions;

#define MINMAX_MULTI_DEFAULT_VALUES_PER_PAGE		32

#define MinMaxMultiGetValuesPerRange(opts) \
		((opts) && (((MinMaxMultiOptions *) (opts))->valuesPerRange != 0) ? \
		 ((MinMaxMultiOptions *) (opts))->valuesPerRange : \
		 MINMAX_MULTI_DEFAULT_VALUES_PER_PAGE)

#define SAMESIGN(a,b) (((a) < 0) == ((b) < 0))

/*
 * The summary of minmax-multi indexes has two representations - Ranges for
 * convenient processing, and SerializedRanges for storage in bytea value.
 *
 * The Ranges struct stores the boundary values in a single array, but we
 * treat regular and single-point ranges differently to save space. For
 * regular ranges (with different boundary values) we have to store both
 * the lower and upper bound of the range, while for "single-point ranges"
 * we only need to store a single value.
 *
 * The 'values' array stores boundary values for regular ranges first (there
 * are 2*nranges values to store), and then the nvalues boundary values for
 * single-point ranges. That is, we have (2*nranges + nvalues) boundary
 * values in the array.
 *
 * +-------------------------+----------------------------------+
 * | ranges (2 * nranges of) | single point values (nvalues of) |
 * +-------------------------+----------------------------------+
 *
 * This allows us to quickly add new values, and store outliers without
 * having to widen any of the existing range values.
 *
 * 'nsorted' denotes how many of 'nvalues' in the values[] array are sorted.
 * When nsorted == nvalues, all single point values are sorted.
 *
 * We never store more than maxvalues values (as set by values_per_range
 * reloption). If needed we merge some of the ranges.
 *
 * To minimize palloc overhead, we always allocate the full array with
 * space for maxvalues elements. This should be fine as long as the
 * maxvalues is reasonably small (64 seems fine), which is the case
 * thanks to values_per_range reloption being limited to 256.
 */
typedef struct Ranges
{
	/* Cache information that we need quite often. */
	Oid			typid;
	Oid			colloid;
	AttrNumber	attno;
	FmgrInfo   *cmp;

	/* (2*nranges + nvalues) <= maxvalues */
	int			nranges;		/* number of ranges in the values[] array */
	int			nsorted;		/* number of nvalues which are sorted */
	int			nvalues;		/* number of point values in values[] array */
	int			maxvalues;		/* number of elements in the values[] array */

	/*
	 * We simply add the values into a large buffer, without any expensive
	 * steps (sorting, deduplication, ...). The buffer is a multiple of the
	 * target number of values, so the compaction happens less often,
	 * amortizing the costs. We keep the actual target and compact to the
	 * requested number of values at the very end, before serializing to
	 * on-disk representation.
	 */
	/* requested number of values */
	int			target_maxvalues;

	/* values stored for this range - either raw values, or ranges */
	Datum		values[FLEXIBLE_ARRAY_MEMBER];
} Ranges;

/*
 * On-disk the summary is stored as a bytea value, with a simple header
 * with basic metadata, followed by the boundary values. It has a varlena
 * header, so can be treated as varlena directly.
 *
 * See brin_range_serialize/brin_range_deserialize for serialization details.
 */
typedef struct SerializedRanges
{
	/* varlena header (do not touch directly!) */
	int32		vl_len_;

	/* type of values stored in the data array */
	Oid			typid;

	/* (2*nranges + nvalues) <= maxvalues */
	int			nranges;		/* number of ranges in the array (stored) */
	int			nvalues;		/* number of values in the data array (all) */
	int			maxvalues;		/* maximum number of values (reloption) */

	/* contains the actual data */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} SerializedRanges;

static SerializedRanges *brin_range_serialize(Ranges *range);

static Ranges *brin_range_deserialize(int maxvalues,
									  SerializedRanges *serialized);


/*
 * Used to represent ranges expanded to make merging and combining easier.
 *
 * Each expanded range is essentially an interval, represented by min/max
 * values, along with a flag whether it's a collapsed range (in which case
 * the min and max values are equal). We have the flag to handle by-ref
 * data types - we can't simply compare the datums, and this saves some
 * calls to the type-specific comparator function.
 */
typedef struct ExpandedRange
{
	Datum		minval;			/* lower boundary */
	Datum		maxval;			/* upper boundary */
	bool		collapsed;		/* true if minval==maxval */
} ExpandedRange;

/*
 * Represents a distance between two ranges (identified by index into
 * an array of extended ranges).
 */
typedef struct DistanceValue
{
	int			index;
	double		value;
} DistanceValue;


/* Cache for support and strategy procedures. */

static FmgrInfo *minmax_multi_get_procinfo(BrinDesc *bdesc, uint16 attno,
										   uint16 procnum);

static FmgrInfo *minmax_multi_get_strategy_procinfo(BrinDesc *bdesc,
													uint16 attno, Oid subtype,
													uint16 strategynum);

typedef struct compare_context
{
	FmgrInfo   *cmpFn;
	Oid			colloid;
} compare_context;

static int	compare_values(const void *a, const void *b, void *arg);


#ifdef USE_ASSERT_CHECKING
/*
 * Check that the order of the array values is correct, using the cmp
 * function (which should be BTLessStrategyNumber).
 */
static void
AssertArrayOrder(FmgrInfo *cmp, Oid colloid, Datum *values, int nvalues)
{
	int			i;
	Datum		lt;

	for (i = 0; i < (nvalues - 1); i++)
	{
		lt = FunctionCall2Coll(cmp, colloid, values[i], values[i + 1]);
		Assert(DatumGetBool(lt));
	}
}
#endif

/*
 * Comprehensive check of the Ranges structure.
 */
static void
AssertCheckRanges(Ranges *ranges, FmgrInfo *cmpFn, Oid colloid)
{
#ifdef USE_ASSERT_CHECKING
	int			i;

	/* some basic sanity checks */
	Assert(ranges->nranges >= 0);
	Assert(ranges->nsorted >= 0);
	Assert(ranges->nvalues >= ranges->nsorted);
	Assert(ranges->maxvalues >= 2 * ranges->nranges + ranges->nvalues);
	Assert(ranges->typid != InvalidOid);

	/*
	 * First the ranges - there are 2*nranges boundary values, and the values
	 * have to be strictly ordered (equal values would mean the range is
	 * collapsed, and should be stored as a point). This also guarantees that
	 * the ranges do not overlap.
	 */
	AssertArrayOrder(cmpFn, colloid, ranges->values, 2 * ranges->nranges);

	/* then the single-point ranges (with nvalues boundary values ) */
	AssertArrayOrder(cmpFn, colloid, &ranges->values[2 * ranges->nranges],
					 ranges->nsorted);

	/*
	 * Check that none of the values are not covered by ranges (both sorted
	 * and unsorted)
	 */
	if (ranges->nranges > 0)
	{
		for (i = 0; i < ranges->nvalues; i++)
		{
			Datum		compar;
			int			start,
						end;
			Datum		minvalue = ranges->values[0];
			Datum		maxvalue = ranges->values[2 * ranges->nranges - 1];
			Datum		value = ranges->values[2 * ranges->nranges + i];

			compar = FunctionCall2Coll(cmpFn, colloid, value, minvalue);

			/*
			 * If the value is smaller than the lower bound in the first range
			 * then it cannot possibly be in any of the ranges.
			 */
			if (DatumGetBool(compar))
				continue;

			compar = FunctionCall2Coll(cmpFn, colloid, maxvalue, value);

			/*
			 * Likewise, if the value is larger than the upper bound of the
			 * final range, then it cannot possibly be inside any of the
			 * ranges.
			 */
			if (DatumGetBool(compar))
				continue;

			/* bsearch the ranges to see if 'value' fits within any of them */
			start = 0;			/* first range */
			end = ranges->nranges - 1;	/* last range */
			while (true)
			{
				int			midpoint = (start + end) / 2;

				/* this means we ran out of ranges in the last step */
				if (start > end)
					break;

				/* copy the min/max values from the ranges */
				minvalue = ranges->values[2 * midpoint];
				maxvalue = ranges->values[2 * midpoint + 1];

				/*
				 * Is the value smaller than the minval? If yes, we'll recurse
				 * to the left side of range array.
				 */
				compar = FunctionCall2Coll(cmpFn, colloid, value, minvalue);

				/* smaller than the smallest value in this range */
				if (DatumGetBool(compar))
				{
					end = (midpoint - 1);
					continue;
				}

				/*
				 * Is the value greater than the minval? If yes, we'll recurse
				 * to the right side of range array.
				 */
				compar = FunctionCall2Coll(cmpFn, colloid, maxvalue, value);

				/* larger than the largest value in this range */
				if (DatumGetBool(compar))
				{
					start = (midpoint + 1);
					continue;
				}

				/* hey, we found a matching range */
				Assert(false);
			}
		}
	}

	/* and values in the unsorted part must not be in the sorted part */
	if (ranges->nsorted > 0)
	{
		compare_context cxt;

		cxt.colloid = ranges->colloid;
		cxt.cmpFn = ranges->cmp;

		for (i = ranges->nsorted; i < ranges->nvalues; i++)
		{
			Datum		value = ranges->values[2 * ranges->nranges + i];

			Assert(bsearch_arg(&value, &ranges->values[2 * ranges->nranges],
							   ranges->nsorted, sizeof(Datum),
							   compare_values, (void *) &cxt) == NULL);
		}
	}
#endif
}

/*
 * Check that the expanded ranges (built when reducing the number of ranges
 * by combining some of them) are correctly sorted and do not overlap.
 */
static void
AssertCheckExpandedRanges(BrinDesc *bdesc, Oid colloid, AttrNumber attno,
						  Form_pg_attribute attr, ExpandedRange *ranges,
						  int nranges)
{
#ifdef USE_ASSERT_CHECKING
	int			i;
	FmgrInfo   *eq;
	FmgrInfo   *lt;

	eq = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
											BTEqualStrategyNumber);

	lt = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
											BTLessStrategyNumber);

	/*
	 * Each range independently should be valid, i.e. that for the boundary
	 * values (lower <= upper).
	 */
	for (i = 0; i < nranges; i++)
	{
		Datum		r;
		Datum		minval = ranges[i].minval;
		Datum		maxval = ranges[i].maxval;

		if (ranges[i].collapsed)	/* collapsed: minval == maxval */
			r = FunctionCall2Coll(eq, colloid, minval, maxval);
		else					/* non-collapsed: minval < maxval */
			r = FunctionCall2Coll(lt, colloid, minval, maxval);

		Assert(DatumGetBool(r));
	}

	/*
	 * And the ranges should be ordered and must not overlap, i.e. upper <
	 * lower for boundaries of consecutive ranges.
	 */
	for (i = 0; i < nranges - 1; i++)
	{
		Datum		r;
		Datum		maxval = ranges[i].maxval;
		Datum		minval = ranges[i + 1].minval;

		r = FunctionCall2Coll(lt, colloid, maxval, minval);

		Assert(DatumGetBool(r));
	}
#endif
}


/*
 * minmax_multi_init
 * 		Initialize the deserialized range list, allocate all the memory.
 *
 * This is only in-memory representation of the ranges, so we allocate
 * enough space for the maximum number of values (so as not to have to do
 * repallocs as the ranges grow).
 */
static Ranges *
minmax_multi_init(int maxvalues)
{
	Size		len;
	Ranges	   *ranges;

	Assert(maxvalues > 0);

	len = offsetof(Ranges, values); /* fixed header */
	len += maxvalues * sizeof(Datum);	/* Datum values */

	ranges = (Ranges *) palloc0(len);

	ranges->maxvalues = maxvalues;

	return ranges;
}


/*
 * range_deduplicate_values
 *		Deduplicate the part with values in the simple points.
 *
 * This is meant to be a cheaper way of reducing the size of the ranges. It
 * does not touch the ranges, and only sorts the other values - it does not
 * call the distance functions, which may be quite expensive, etc.
 *
 * We do know the values are not duplicate with the ranges, because we check
 * that before adding a new value. Same for the sorted part of values.
 */
static void
range_deduplicate_values(Ranges *range)
{
	int			i,
				n;
	int			start;
	compare_context cxt;

	/*
	 * If there are no unsorted values, we're done (this probably can't
	 * happen, as we're adding values to unsorted part).
	 */
	if (range->nsorted == range->nvalues)
		return;

	/* sort the values */
	cxt.colloid = range->colloid;
	cxt.cmpFn = range->cmp;

	/* the values start right after the ranges (which are always sorted) */
	start = 2 * range->nranges;

	/*
	 * XXX This might do a merge sort, to leverage that the first part of the
	 * array is already sorted. If the sorted part is large, it might be quite
	 * a bit faster.
	 */
	qsort_arg(&range->values[start],
			  range->nvalues, sizeof(Datum),
			  compare_values, &cxt);

	n = 1;
	for (i = 1; i < range->nvalues; i++)
	{
		/* same as preceding value, so store it */
		if (compare_values(&range->values[start + i - 1],
						   &range->values[start + i],
						   (void *) &cxt) == 0)
			continue;

		range->values[start + n] = range->values[start + i];

		n++;
	}

	/* now all the values are sorted */
	range->nvalues = n;
	range->nsorted = n;

	AssertCheckRanges(range, range->cmp, range->colloid);
}


/*
 * brin_range_serialize
 *	  Serialize the in-memory representation into a compact varlena value.
 *
 * Simply copy the header and then also the individual values, as stored
 * in the in-memory value array.
 */
static SerializedRanges *
brin_range_serialize(Ranges *range)
{
	Size		len;
	int			nvalues;
	SerializedRanges *serialized;
	Oid			typid;
	int			typlen;
	bool		typbyval;

	char	   *ptr;

	/* simple sanity checks */
	Assert(range->nranges >= 0);
	Assert(range->nsorted >= 0);
	Assert(range->nvalues >= 0);
	Assert(range->maxvalues > 0);
	Assert(range->target_maxvalues > 0);

	/* at this point the range should be compacted to the target size */
	Assert(2 * range->nranges + range->nvalues <= range->target_maxvalues);

	Assert(range->target_maxvalues <= range->maxvalues);

	/* range boundaries are always sorted */
	Assert(range->nvalues >= range->nsorted);

	/* deduplicate values, if there's unsorted part */
	range_deduplicate_values(range);

	/* see how many Datum values we actually have */
	nvalues = 2 * range->nranges + range->nvalues;

	typid = range->typid;
	typbyval = get_typbyval(typid);
	typlen = get_typlen(typid);

	/* header is always needed */
	len = offsetof(SerializedRanges, data);

	/*
	 * The space needed depends on data type - for fixed-length data types
	 * (by-value and some by-reference) it's pretty simple, just multiply
	 * (attlen * nvalues) and we're done. For variable-length by-reference
	 * types we need to actually walk all the values and sum the lengths.
	 */
	if (typlen == -1)			/* varlena */
	{
		int			i;

		for (i = 0; i < nvalues; i++)
		{
			len += VARSIZE_ANY(range->values[i]);
		}
	}
	else if (typlen == -2)		/* cstring */
	{
		int			i;

		for (i = 0; i < nvalues; i++)
		{
			/* don't forget to include the null terminator ;-) */
			len += strlen(DatumGetCString(range->values[i])) + 1;
		}
	}
	else						/* fixed-length types (even by-reference) */
	{
		Assert(typlen > 0);
		len += nvalues * typlen;
	}

	/*
	 * Allocate the serialized object, copy the basic information. The
	 * serialized object is a varlena, so update the header.
	 */
	serialized = (SerializedRanges *) palloc0(len);
	SET_VARSIZE(serialized, len);

	serialized->typid = typid;
	serialized->nranges = range->nranges;
	serialized->nvalues = range->nvalues;
	serialized->maxvalues = range->target_maxvalues;

	/*
	 * And now copy also the boundary values (like the length calculation this
	 * depends on the particular data type).
	 */
	ptr = serialized->data;		/* start of the serialized data */

	for (int i = 0; i < nvalues; i++)
	{
		if (typbyval)			/* simple by-value data types */
		{
			Datum		tmp;

			/*
			 * For byval types, we need to copy just the significant bytes -
			 * we can't use memcpy directly, as that assumes little-endian
			 * behavior.  store_att_byval does almost what we need, but it
			 * requires a properly aligned buffer - the output buffer does not
			 * guarantee that. So we simply use a local Datum variable (which
			 * guarantees proper alignment), and then copy the value from it.
			 */
			store_att_byval(&tmp, range->values[i], typlen);

			memcpy(ptr, &tmp, typlen);
			ptr += typlen;
		}
		else if (typlen > 0)	/* fixed-length by-ref types */
		{
			memcpy(ptr, DatumGetPointer(range->values[i]), typlen);
			ptr += typlen;
		}
		else if (typlen == -1)	/* varlena */
		{
			int			tmp = VARSIZE_ANY(DatumGetPointer(range->values[i]));

			memcpy(ptr, DatumGetPointer(range->values[i]), tmp);
			ptr += tmp;
		}
		else if (typlen == -2)	/* cstring */
		{
			int			tmp = strlen(DatumGetCString(range->values[i])) + 1;

			memcpy(ptr, DatumGetCString(range->values[i]), tmp);
			ptr += tmp;
		}

		/* make sure we haven't overflown the buffer end */
		Assert(ptr <= ((char *) serialized + len));
	}

	/* exact size */
	Assert(ptr == ((char *) serialized + len));

	return serialized;
}

/*
 * brin_range_deserialize
 *	  Serialize the in-memory representation into a compact varlena value.
 *
 * Simply copy the header and then also the individual values, as stored
 * in the in-memory value array.
 */
static Ranges *
brin_range_deserialize(int maxvalues, SerializedRanges *serialized)
{
	int			i,
				nvalues;
	char	   *ptr,
			   *dataptr;
	bool		typbyval;
	int			typlen;
	Size		datalen;

	Ranges	   *range;

	Assert(serialized->nranges >= 0);
	Assert(serialized->nvalues >= 0);
	Assert(serialized->maxvalues > 0);

	nvalues = 2 * serialized->nranges + serialized->nvalues;

	Assert(nvalues <= serialized->maxvalues);
	Assert(serialized->maxvalues <= maxvalues);

	range = minmax_multi_init(maxvalues);

	/* copy the header info */
	range->nranges = serialized->nranges;
	range->nvalues = serialized->nvalues;
	range->nsorted = serialized->nvalues;
	range->maxvalues = maxvalues;
	range->target_maxvalues = serialized->maxvalues;

	range->typid = serialized->typid;

	typbyval = get_typbyval(serialized->typid);
	typlen = get_typlen(serialized->typid);

	/*
	 * And now deconstruct the values into Datum array. We have to copy the
	 * data because the serialized representation ignores alignment, and we
	 * don't want to rely on it being kept around anyway.
	 */
	ptr = serialized->data;

	/*
	 * We don't want to allocate many pieces, so we just allocate everything
	 * in one chunk. How much space will we need?
	 *
	 * XXX We don't need to copy simple by-value data types.
	 */
	datalen = 0;
	dataptr = NULL;
	for (i = 0; (i < nvalues) && (!typbyval); i++)
	{
		if (typlen > 0)			/* fixed-length by-ref types */
			datalen += MAXALIGN(typlen);
		else if (typlen == -1)	/* varlena */
		{
			datalen += MAXALIGN(VARSIZE_ANY(ptr));
			ptr += VARSIZE_ANY(ptr);
		}
		else if (typlen == -2)	/* cstring */
		{
			Size		slen = strlen(ptr) + 1;

			datalen += MAXALIGN(slen);
			ptr += slen;
		}
	}

	if (datalen > 0)
		dataptr = palloc(datalen);

	/*
	 * Restore the source pointer (might have been modified when calculating
	 * the space we need to allocate).
	 */
	ptr = serialized->data;

	for (i = 0; i < nvalues; i++)
	{
		if (typbyval)			/* simple by-value data types */
		{
			Datum		v = 0;

			memcpy(&v, ptr, typlen);

			range->values[i] = fetch_att(&v, true, typlen);
			ptr += typlen;
		}
		else if (typlen > 0)	/* fixed-length by-ref types */
		{
			range->values[i] = PointerGetDatum(dataptr);

			memcpy(dataptr, ptr, typlen);
			dataptr += MAXALIGN(typlen);

			ptr += typlen;
		}
		else if (typlen == -1)	/* varlena */
		{
			range->values[i] = PointerGetDatum(dataptr);

			memcpy(dataptr, ptr, VARSIZE_ANY(ptr));
			dataptr += MAXALIGN(VARSIZE_ANY(ptr));
			ptr += VARSIZE_ANY(ptr);
		}
		else if (typlen == -2)	/* cstring */
		{
			Size		slen = strlen(ptr) + 1;

			range->values[i] = PointerGetDatum(dataptr);

			memcpy(dataptr, ptr, slen);
			dataptr += MAXALIGN(slen);
			ptr += slen;
		}

		/* make sure we haven't overflown the buffer end */
		Assert(ptr <= ((char *) serialized + VARSIZE_ANY(serialized)));
	}

	/* should have consumed the whole input value exactly */
	Assert(ptr == ((char *) serialized + VARSIZE_ANY(serialized)));

	/* return the deserialized value */
	return range;
}

/*
 * compare_expanded_ranges
 *	  Compare the expanded ranges - first by minimum, then by maximum.
 *
 * We do guarantee that ranges in a single Ranges object do not overlap, so it
 * may seem strange that we don't order just by minimum. But when merging two
 * Ranges (which happens in the union function), the ranges may in fact
 * overlap. So we do compare both.
 */
static int
compare_expanded_ranges(const void *a, const void *b, void *arg)
{
	ExpandedRange *ra = (ExpandedRange *) a;
	ExpandedRange *rb = (ExpandedRange *) b;
	Datum		r;

	compare_context *cxt = (compare_context *) arg;

	/* first compare minvals */
	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, ra->minval, rb->minval);

	if (DatumGetBool(r))
		return -1;

	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, rb->minval, ra->minval);

	if (DatumGetBool(r))
		return 1;

	/* then compare maxvals */
	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, ra->maxval, rb->maxval);

	if (DatumGetBool(r))
		return -1;

	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, rb->maxval, ra->maxval);

	if (DatumGetBool(r))
		return 1;

	return 0;
}

/*
 * compare_values
 *	  Compare the values.
 */
static int
compare_values(const void *a, const void *b, void *arg)
{
	Datum	   *da = (Datum *) a;
	Datum	   *db = (Datum *) b;
	Datum		r;

	compare_context *cxt = (compare_context *) arg;

	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, *da, *db);

	if (DatumGetBool(r))
		return -1;

	r = FunctionCall2Coll(cxt->cmpFn, cxt->colloid, *db, *da);

	if (DatumGetBool(r))
		return 1;

	return 0;
}

/*
 * Check if the new value matches one of the existing ranges.
 */
static bool
has_matching_range(BrinDesc *bdesc, Oid colloid, Ranges *ranges,
				   Datum newval, AttrNumber attno, Oid typid)
{
	Datum		compar;

	Datum		minvalue;
	Datum		maxvalue;

	FmgrInfo   *cmpLessFn;
	FmgrInfo   *cmpGreaterFn;

	/* binary search on ranges */
	int			start,
				end;

	if (ranges->nranges == 0)
		return false;

	minvalue = ranges->values[0];
	maxvalue = ranges->values[2 * ranges->nranges - 1];

	/*
	 * Otherwise, need to compare the new value with boundaries of all the
	 * ranges. First check if it's less than the absolute minimum, which is
	 * the first value in the array.
	 */
	cmpLessFn = minmax_multi_get_strategy_procinfo(bdesc, attno, typid,
												   BTLessStrategyNumber);
	compar = FunctionCall2Coll(cmpLessFn, colloid, newval, minvalue);

	/* smaller than the smallest value in the range list */
	if (DatumGetBool(compar))
		return false;

	/*
	 * And now compare it to the existing maximum (last value in the data
	 * array). But only if we haven't already ruled out a possible match in
	 * the minvalue check.
	 */
	cmpGreaterFn = minmax_multi_get_strategy_procinfo(bdesc, attno, typid,
													  BTGreaterStrategyNumber);
	compar = FunctionCall2Coll(cmpGreaterFn, colloid, newval, maxvalue);

	if (DatumGetBool(compar))
		return false;

	/*
	 * So we know it's in the general min/max, the question is whether it
	 * falls in one of the ranges or gaps. We'll do a binary search on
	 * individual ranges - for each range we check equality (value falls into
	 * the range), and then check ranges either above or below the current
	 * range.
	 */
	start = 0;					/* first range */
	end = (ranges->nranges - 1);	/* last range */
	while (true)
	{
		int			midpoint = (start + end) / 2;

		/* this means we ran out of ranges in the last step */
		if (start > end)
			return false;

		/* copy the min/max values from the ranges */
		minvalue = ranges->values[2 * midpoint];
		maxvalue = ranges->values[2 * midpoint + 1];

		/*
		 * Is the value smaller than the minval? If yes, we'll recurse to the
		 * left side of range array.
		 */
		compar = FunctionCall2Coll(cmpLessFn, colloid, newval, minvalue);

		/* smaller than the smallest value in this range */
		if (DatumGetBool(compar))
		{
			end = (midpoint - 1);
			continue;
		}

		/*
		 * Is the value greater than the minval? If yes, we'll recurse to the
		 * right side of range array.
		 */
		compar = FunctionCall2Coll(cmpGreaterFn, colloid, newval, maxvalue);

		/* larger than the largest value in this range */
		if (DatumGetBool(compar))
		{
			start = (midpoint + 1);
			continue;
		}

		/* hey, we found a matching range */
		return true;
	}

	return false;
}


/*
 * range_contains_value
 * 		See if the new value is already contained in the range list.
 *
 * We first inspect the list of intervals. We use a small trick - we check
 * the value against min/max of the whole range (min of the first interval,
 * max of the last one) first, and only inspect the individual intervals if
 * this passes.
 *
 * If the value matches none of the intervals, we check the exact values.
 * We simply loop through them and invoke equality operator on them.
 *
 * The last parameter (full) determines whether we need to search all the
 * values, including the unsorted part. With full=false, the unsorted part
 * is not searched, which may produce false negatives and duplicate values
 * (in the unsorted part only), but when we're building the range that's
 * fine - we'll deduplicate before serialization, and it can only happen
 * if there already are unsorted values (so it was already modified).
 *
 * Serialized ranges don't have any unsorted values, so this can't cause
 * false negatives during querying.
 */
static bool
range_contains_value(BrinDesc *bdesc, Oid colloid,
					 AttrNumber attno, Form_pg_attribute attr,
					 Ranges *ranges, Datum newval, bool full)
{
	int			i;
	FmgrInfo   *cmpEqualFn;
	Oid			typid = attr->atttypid;

	/*
	 * First inspect the ranges, if there are any. We first check the whole
	 * range, and only when there's still a chance of getting a match we
	 * inspect the individual ranges.
	 */
	if (has_matching_range(bdesc, colloid, ranges, newval, attno, typid))
		return true;

	cmpEqualFn = minmax_multi_get_strategy_procinfo(bdesc, attno, typid,
													BTEqualStrategyNumber);

	/*
	 * There is no matching range, so let's inspect the sorted values.
	 *
	 * We do a sequential search for small numbers of values, and binary
	 * search once we have more than 16 values. This threshold is somewhat
	 * arbitrary, as it depends on how expensive the comparison function is.
	 *
	 * XXX If we use the threshold here, maybe we should do the same thing in
	 * has_matching_range? Or maybe we should do the bin search all the time?
	 *
	 * XXX We could use the same optimization as for ranges, to check if the
	 * value is between min/max, to maybe rule out all sorted values without
	 * having to inspect all of them.
	 */
	if (ranges->nsorted >= 16)
	{
		compare_context cxt;

		cxt.colloid = ranges->colloid;
		cxt.cmpFn = ranges->cmp;

		if (bsearch_arg(&newval, &ranges->values[2 * ranges->nranges],
						ranges->nsorted, sizeof(Datum),
						compare_values, (void *) &cxt) != NULL)
			return true;
	}
	else
	{
		for (i = 2 * ranges->nranges; i < 2 * ranges->nranges + ranges->nsorted; i++)
		{
			Datum		compar;

			compar = FunctionCall2Coll(cmpEqualFn, colloid, newval, ranges->values[i]);

			/* found an exact match */
			if (DatumGetBool(compar))
				return true;
		}
	}

	/* If not asked to inspect the unsorted part, we're done. */
	if (!full)
		return false;

	/* Inspect the unsorted part. */
	for (i = 2 * ranges->nranges + ranges->nsorted; i < 2 * ranges->nranges + ranges->nvalues; i++)
	{
		Datum		compar;

		compar = FunctionCall2Coll(cmpEqualFn, colloid, newval, ranges->values[i]);

		/* found an exact match */
		if (DatumGetBool(compar))
			return true;
	}

	/* the value is not covered by this BRIN tuple */
	return false;
}

/*
 * Expand ranges from Ranges into ExpandedRange array. This expects the
 * eranges to be pre-allocated and with the correct size - there needs to be
 * (nranges + nvalues) elements.
 *
 * The order of expanded ranges is arbitrary. We do expand the ranges first,
 * and this part is sorted. But then we expand the values, and this part may
 * be unsorted.
 */
static void
fill_expanded_ranges(ExpandedRange *eranges, int neranges, Ranges *ranges)
{
	int			idx;
	int			i;

	/* Check that the output array has the right size. */
	Assert(neranges == (ranges->nranges + ranges->nvalues));

	idx = 0;
	for (i = 0; i < ranges->nranges; i++)
	{
		eranges[idx].minval = ranges->values[2 * i];
		eranges[idx].maxval = ranges->values[2 * i + 1];
		eranges[idx].collapsed = false;
		idx++;

		Assert(idx <= neranges);
	}

	for (i = 0; i < ranges->nvalues; i++)
	{
		eranges[idx].minval = ranges->values[2 * ranges->nranges + i];
		eranges[idx].maxval = ranges->values[2 * ranges->nranges + i];
		eranges[idx].collapsed = true;
		idx++;

		Assert(idx <= neranges);
	}

	/* Did we produce the expected number of elements? */
	Assert(idx == neranges);

	return;
}

/*
 * Sort and deduplicate expanded ranges.
 *
 * The ranges may be deduplicated - we're simply appending values, without
 * checking for duplicates etc. So maybe the deduplication will reduce the
 * number of ranges enough, and we won't have to compute the distances etc.
 *
 * Returns the number of expanded ranges.
 */
static int
sort_expanded_ranges(FmgrInfo *cmp, Oid colloid,
					 ExpandedRange *eranges, int neranges)
{
	int			n;
	int			i;
	compare_context cxt;

	Assert(neranges > 0);

	/* sort the values */
	cxt.colloid = colloid;
	cxt.cmpFn = cmp;

	/*
	 * XXX We do qsort on all the values, but we could also leverage the fact
	 * that some of the input data is already sorted (all the ranges and maybe
	 * some of the points) and do merge sort.
	 */
	qsort_arg(eranges, neranges, sizeof(ExpandedRange),
			  compare_expanded_ranges, &cxt);

	/*
	 * Deduplicate the ranges - simply compare each range to the preceding
	 * one, and skip the duplicate ones.
	 */
	n = 1;
	for (i = 1; i < neranges; i++)
	{
		/* if the current range is equal to the preceding one, do nothing */
		if (!compare_expanded_ranges(&eranges[i - 1], &eranges[i], (void *) &cxt))
			continue;

		/* otherwise, copy it to n-th place (if not already there) */
		if (i != n)
			memcpy(&eranges[n], &eranges[i], sizeof(ExpandedRange));

		n++;
	}

	Assert((n > 0) && (n <= neranges));

	return n;
}

/*
 * When combining multiple Range values (in union function), some of the
 * ranges may overlap. We simply merge the overlapping ranges to fix that.
 *
 * XXX This assumes the expanded ranges were previously sorted (by minval
 * and then maxval). We leverage this when detecting overlap.
 */
static int
merge_overlapping_ranges(FmgrInfo *cmp, Oid colloid,
						 ExpandedRange *eranges, int neranges)
{
	int			idx;

	/* Merge ranges (idx) and (idx+1) if they overlap. */
	idx = 0;
	while (idx < (neranges - 1))
	{
		Datum		r;

		/*
		 * comparing [?,maxval] vs. [minval,?] - the ranges overlap if (minval
		 * < maxval)
		 */
		r = FunctionCall2Coll(cmp, colloid,
							  eranges[idx].maxval,
							  eranges[idx + 1].minval);

		/*
		 * Nope, maxval < minval, so no overlap. And we know the ranges are
		 * ordered, so there are no more overlaps, because all the remaining
		 * ranges have greater or equal minval.
		 */
		if (DatumGetBool(r))
		{
			/* proceed to the next range */
			idx += 1;
			continue;
		}

		/*
		 * So ranges 'idx' and 'idx+1' do overlap, but we don't know if
		 * 'idx+1' is contained in 'idx', or if they overlap only partially.
		 * So compare the upper bounds and keep the larger one.
		 */
		r = FunctionCall2Coll(cmp, colloid,
							  eranges[idx].maxval,
							  eranges[idx + 1].maxval);

		if (DatumGetBool(r))
			eranges[idx].maxval = eranges[idx + 1].maxval;

		/*
		 * The range certainly is no longer collapsed (irrespectively of the
		 * previous state).
		 */
		eranges[idx].collapsed = false;

		/*
		 * Now get rid of the (idx+1) range entirely by shifting the remaining
		 * ranges by 1. There are neranges elements, and we need to move
		 * elements from (idx+2). That means the number of elements to move is
		 * [ncranges - (idx+2)].
		 */
		memmove(&eranges[idx + 1], &eranges[idx + 2],
				(neranges - (idx + 2)) * sizeof(ExpandedRange));

		/*
		 * Decrease the number of ranges, and repeat (with the same range, as
		 * it might overlap with additional ranges thanks to the merge).
		 */
		neranges--;
	}

	return neranges;
}

/*
 * Simple comparator for distance values, comparing the double value.
 * This is intentionally sorting the distances in descending order, i.e.
 * the longer gaps will be at the front.
 */
static int
compare_distances(const void *a, const void *b)
{
	DistanceValue *da = (DistanceValue *) a;
	DistanceValue *db = (DistanceValue *) b;

	if (da->value < db->value)
		return 1;
	else if (da->value > db->value)
		return -1;

	return 0;
}

/*
 * Given an array of expanded ranges, compute size of the gaps between each
 * range.  For neranges there are (neranges-1) gaps.
 *
 * We simply call the "distance" function to compute the (max-min) for pairs
 * of consecutive ranges. The function may be fairly expensive, so we do that
 * just once (and then use it to pick as many ranges to merge as possible).
 *
 * See reduce_expanded_ranges for details.
 */
static DistanceValue *
build_distances(FmgrInfo *distanceFn, Oid colloid,
				ExpandedRange *eranges, int neranges)
{
	int			i;
	int			ndistances;
	DistanceValue *distances;

	Assert(neranges > 0);

	/* If there's only a single range, there's no distance to calculate. */
	if (neranges == 1)
		return NULL;

	ndistances = (neranges - 1);
	distances = (DistanceValue *) palloc0(sizeof(DistanceValue) * ndistances);

	/*
	 * Walk through the ranges once and compute the distance between the
	 * ranges so that we can sort them once.
	 */
	for (i = 0; i < ndistances; i++)
	{
		Datum		a1,
					a2,
					r;

		a1 = eranges[i].maxval;
		a2 = eranges[i + 1].minval;

		/* compute length of the gap (between max/min) */
		r = FunctionCall2Coll(distanceFn, colloid, a1, a2);

		/* remember the index of the gap the distance is for */
		distances[i].index = i;
		distances[i].value = DatumGetFloat8(r);
	}

	/*
	 * Sort the distances in descending order, so that the longest gaps are at
	 * the front.
	 */
	pg_qsort(distances, ndistances, sizeof(DistanceValue), compare_distances);

	return distances;
}

/*
 * Builds expanded ranges for the existing ranges (and single-point ranges),
 * and also the new value (which did not fit into the array).  This expanded
 * representation makes the processing a bit easier, as it allows handling
 * ranges and points the same way.
 *
 * We sort and deduplicate the expanded ranges - this is necessary, because
 * the points may be unsorted. And moreover the two parts (ranges and
 * points) are sorted on their own.
 */
static ExpandedRange *
build_expanded_ranges(FmgrInfo *cmp, Oid colloid, Ranges *ranges,
					  int *nranges)
{
	int			neranges;
	ExpandedRange *eranges;

	/* both ranges and points are expanded into a separate element */
	neranges = ranges->nranges + ranges->nvalues;

	eranges = (ExpandedRange *) palloc0(neranges * sizeof(ExpandedRange));

	/* fill the expanded ranges */
	fill_expanded_ranges(eranges, neranges, ranges);

	/* sort and deduplicate the expanded ranges */
	neranges = sort_expanded_ranges(cmp, colloid, eranges, neranges);

	/* remember how many ranges we built */
	*nranges = neranges;

	return eranges;
}

#ifdef USE_ASSERT_CHECKING
/*
 * Counts boundary values needed to store the ranges. Each single-point
 * range is stored using a single value, each regular range needs two.
 */
static int
count_values(ExpandedRange *cranges, int ncranges)
{
	int			i;
	int			count;

	count = 0;
	for (i = 0; i < ncranges; i++)
	{
		if (cranges[i].collapsed)
			count += 1;
		else
			count += 2;
	}

	return count;
}
#endif

/*
 * reduce_expanded_ranges
 *		reduce the ranges until the number of values is low enough
 *
 * Combines ranges until the number of boundary values drops below the
 * threshold specified by max_values. This happens by merging enough
 * ranges by the distance between them.
 *
 * Returns the number of result ranges.
 *
 * We simply use the global min/max and then add boundaries for enough
 * largest gaps. Each gap adds 2 values, so we simply use (target/2-1)
 * distances. Then we simply sort all the values - each two values are
 * a boundary of a range (possibly collapsed).
 *
 * XXX Some of the ranges may be collapsed (i.e. the min/max values are
 * equal), but we ignore that for now. We could repeat the process,
 * adding a couple more gaps recursively.
 *
 * XXX The ranges to merge are selected solely using the distance. But
 * that may not be the best strategy, for example when multiple gaps
 * are of equal (or very similar) length.
 *
 * Consider for example points 1, 2, 3, .., 64, which have gaps of the
 * same length 1 of course. In that case, we tend to pick the first
 * gap of that length, which leads to this:
 *
 *    step 1:  [1, 2], 3, 4, 5, .., 64
 *    step 2:  [1, 3], 4, 5,    .., 64
 *    step 3:  [1, 4], 5,       .., 64
 *    ...
 *
 * So in the end we'll have one "large" range and multiple small points.
 * That may be fine, but it seems a bit strange and non-optimal. Maybe
 * we should consider other things when picking ranges to merge - e.g.
 * length of the ranges? Or perhaps randomize the choice of ranges, with
 * probability inversely proportional to the distance (the gap lengths
 * may be very close, but not exactly the same).
 *
 * XXX Or maybe we could just handle this by using random value as a
 * tie-break, or by adding random noise to the actual distance.
 */
static int
reduce_expanded_ranges(ExpandedRange *eranges, int neranges,
					   DistanceValue *distances, int max_values,
					   FmgrInfo *cmp, Oid colloid)
{
	int			i;
	int			nvalues;
	Datum	   *values;

	compare_context cxt;

	/* total number of gaps between ranges */
	int			ndistances = (neranges - 1);

	/* number of gaps to keep */
	int			keep = (max_values / 2 - 1);

	/*
	 * Maybe we have a sufficiently low number of ranges already?
	 *
	 * XXX This should happen before we actually do the expensive stuff like
	 * sorting, so maybe this should be just an assert.
	 */
	if (keep >= ndistances)
		return neranges;

	/* sort the values */
	cxt.colloid = colloid;
	cxt.cmpFn = cmp;

	/* allocate space for the boundary values */
	nvalues = 0;
	values = (Datum *) palloc(sizeof(Datum) * max_values);

	/* add the global min/max values, from the first/last range */
	values[nvalues++] = eranges[0].minval;
	values[nvalues++] = eranges[neranges - 1].maxval;

	/* add boundary values for enough gaps */
	for (i = 0; i < keep; i++)
	{
		/* index of the gap between (index) and (index+1) ranges */
		int			index = distances[i].index;

		Assert((index >= 0) && ((index + 1) < neranges));

		/* add max from the preceding range, minval from the next one */
		values[nvalues++] = eranges[index].maxval;
		values[nvalues++] = eranges[index + 1].minval;

		Assert(nvalues <= max_values);
	}

	/* We should have an even number of range values. */
	Assert(nvalues % 2 == 0);

	/*
	 * Sort the values using the comparator function, and form ranges from the
	 * sorted result.
	 */
	qsort_arg(values, nvalues, sizeof(Datum),
			  compare_values, &cxt);

	/* We have nvalues boundary values, which means nvalues/2 ranges. */
	for (i = 0; i < (nvalues / 2); i++)
	{
		eranges[i].minval = values[2 * i];
		eranges[i].maxval = values[2 * i + 1];

		/* if the boundary values are the same, it's a collapsed range */
		eranges[i].collapsed = (compare_values(&values[2 * i],
											   &values[2 * i + 1],
											   &cxt) == 0);
	}

	return (nvalues / 2);
}

/*
 * Store the boundary values from ExpandedRanges back into 'ranges' (using
 * only the minimal number of values needed).
 */
static void
store_expanded_ranges(Ranges *ranges, ExpandedRange *eranges, int neranges)
{
	int			i;
	int			idx = 0;

	/* first copy in the regular ranges */
	ranges->nranges = 0;
	for (i = 0; i < neranges; i++)
	{
		if (!eranges[i].collapsed)
		{
			ranges->values[idx++] = eranges[i].minval;
			ranges->values[idx++] = eranges[i].maxval;
			ranges->nranges++;
		}
	}

	/* now copy in the collapsed ones */
	ranges->nvalues = 0;
	for (i = 0; i < neranges; i++)
	{
		if (eranges[i].collapsed)
		{
			ranges->values[idx++] = eranges[i].minval;
			ranges->nvalues++;
		}
	}

	/* all the values are sorted */
	ranges->nsorted = ranges->nvalues;

	Assert(count_values(eranges, neranges) == 2 * ranges->nranges + ranges->nvalues);
	Assert(2 * ranges->nranges + ranges->nvalues <= ranges->maxvalues);
}


/*
 * Consider freeing space in the ranges. Checks if there's space for at least
 * one new value, and performs compaction if needed.
 *
 * Returns true if the value was actually modified.
 */
static bool
ensure_free_space_in_buffer(BrinDesc *bdesc, Oid colloid,
							AttrNumber attno, Form_pg_attribute attr,
							Ranges *range)
{
	MemoryContext ctx;
	MemoryContext oldctx;

	FmgrInfo   *cmpFn,
			   *distanceFn;

	/* expanded ranges */
	ExpandedRange *eranges;
	int			neranges;
	DistanceValue *distances;

	/*
	 * If there is free space in the buffer, we're done without having to
	 * modify anything.
	 */
	if (2 * range->nranges + range->nvalues < range->maxvalues)
		return false;

	/* we'll certainly need the comparator, so just look it up now */
	cmpFn = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
											   BTLessStrategyNumber);

	/* deduplicate values, if there's an unsorted part */
	range_deduplicate_values(range);

	/*
	 * Did we reduce enough free space by just the deduplication?
	 *
	 * We don't simply check against range->maxvalues again. The deduplication
	 * might have freed very little space (e.g. just one value), forcing us to
	 * do deduplication very often. In that case, it's better to do the
	 * compaction and reduce more space.
	 */
	if (2 * range->nranges + range->nvalues <= range->maxvalues * MINMAX_BUFFER_LOAD_FACTOR)
		return true;

	/*
	 * We need to combine some of the existing ranges, to reduce the number of
	 * values we have to store.
	 *
	 * The distanceFn calls (which may internally call e.g. numeric_le) may
	 * allocate quite a bit of memory, and we must not leak it (we might have
	 * to do this repeatedly, even for a single BRIN page range). Otherwise
	 * we'd have problems e.g. when building new indexes. So we use a memory
	 * context and make sure we free the memory at the end (so if we call the
	 * distance function many times, it might be an issue, but meh).
	 */
	ctx = AllocSetContextCreate(CurrentMemoryContext,
								"minmax-multi context",
								ALLOCSET_DEFAULT_SIZES);

	oldctx = MemoryContextSwitchTo(ctx);

	/* build the expanded ranges */
	eranges = build_expanded_ranges(cmpFn, colloid, range, &neranges);

	/* and we'll also need the 'distance' procedure */
	distanceFn = minmax_multi_get_procinfo(bdesc, attno, PROCNUM_DISTANCE);

	/* build array of gap distances and sort them in ascending order */
	distances = build_distances(distanceFn, colloid, eranges, neranges);

	/*
	 * Combine ranges until we release at least 50% of the space. This
	 * threshold is somewhat arbitrary, perhaps needs tuning. We must not use
	 * too low or high value.
	 */
	neranges = reduce_expanded_ranges(eranges, neranges, distances,
									  range->maxvalues * MINMAX_BUFFER_LOAD_FACTOR,
									  cmpFn, colloid);

	/* Make sure we've sufficiently reduced the number of ranges. */
	Assert(count_values(eranges, neranges) <= range->maxvalues * MINMAX_BUFFER_LOAD_FACTOR);

	/* decompose the expanded ranges into regular ranges and single values */
	store_expanded_ranges(range, eranges, neranges);

	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(ctx);

	/* Did we break the ranges somehow? */
	AssertCheckRanges(range, cmpFn, colloid);

	return true;
}

/*
 * range_add_value
 * 		Add the new value to the minmax-multi range.
 */
static bool
range_add_value(BrinDesc *bdesc, Oid colloid,
				AttrNumber attno, Form_pg_attribute attr,
				Ranges *ranges, Datum newval)
{
	FmgrInfo   *cmpFn;
	bool		modified = false;

	/* we'll certainly need the comparator, so just look it up now */
	cmpFn = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
											   BTLessStrategyNumber);

	/* comprehensive checks of the input ranges */
	AssertCheckRanges(ranges, cmpFn, colloid);

	/*
	 * Make sure there's enough free space in the buffer. We only trigger this
	 * when the buffer is full, which means it had to be modified as we size
	 * it to be larger than what is stored on disk.
	 *
	 * This needs to happen before we check if the value is contained in the
	 * range, because the value might be in the unsorted part, and we don't
	 * check that in range_contains_value. The deduplication would then move
	 * it to the sorted part, and we'd add the value too, which violates the
	 * rule that we never have duplicates with the ranges or sorted values.
	 *
	 * We might also deduplicate and recheck if the value is contained, but
	 * that seems like overkill. We'd need to deduplicate anyway, so why not
	 * do it now.
	 */
	modified = ensure_free_space_in_buffer(bdesc, colloid,
										   attno, attr, ranges);

	/*
	 * Bail out if the value already is covered by the range.
	 *
	 * We could also add values until we hit values_per_range, and then do the
	 * deduplication in a batch, hoping for better efficiency. But that would
	 * mean we actually modify the range every time, which means having to
	 * serialize the value, which does palloc, walks the values, copies them,
	 * etc. Not exactly cheap.
	 *
	 * So instead we do the check, which should be fairly cheap - assuming the
	 * comparator function is not very expensive.
	 *
	 * This also implies the values array can't contain duplicate values.
	 */
	if (range_contains_value(bdesc, colloid, attno, attr, ranges, newval, false))
		return modified;

	/* Make a copy of the value, if needed. */
	newval = datumCopy(newval, attr->attbyval, attr->attlen);

	/*
	 * If there's space in the values array, copy it in and we're done.
	 *
	 * We do want to keep the values sorted (to speed up searches), so we do a
	 * simple insertion sort. We could do something more elaborate, e.g. by
	 * sorting the values only now and then, but for small counts (e.g. when
	 * maxvalues is 64) this should be fine.
	 */
	ranges->values[2 * ranges->nranges + ranges->nvalues] = newval;
	ranges->nvalues++;

	/* If we added the first value, we can consider it as sorted. */
	if (ranges->nvalues == 1)
		ranges->nsorted = 1;

	/*
	 * Check we haven't broken the ordering of boundary values (checks both
	 * parts, but that doesn't hurt).
	 */
	AssertCheckRanges(ranges, cmpFn, colloid);

	/* Check the range contains the value we just added. */
	Assert(range_contains_value(bdesc, colloid, attno, attr, ranges, newval, true));

	/* yep, we've modified the range */
	return true;
}

/*
 * Generate range representation of data collected during "batch mode".
 * This is similar to reduce_expanded_ranges, except that we can't assume
 * the values are sorted and there may be duplicate values.
 */
static void
compactify_ranges(BrinDesc *bdesc, Ranges *ranges, int max_values)
{
	FmgrInfo   *cmpFn,
			   *distanceFn;

	/* expanded ranges */
	ExpandedRange *eranges;
	int			neranges;
	DistanceValue *distances;

	MemoryContext ctx;
	MemoryContext oldctx;

	/*
	 * Do we need to actually compactify anything?
	 *
	 * There are two reasons why compaction may be needed - firstly, there may
	 * be too many values, or some of the values may be unsorted.
	 */
	if ((ranges->nranges * 2 + ranges->nvalues <= max_values) &&
		(ranges->nsorted == ranges->nvalues))
		return;

	/* we'll certainly need the comparator, so just look it up now */
	cmpFn = minmax_multi_get_strategy_procinfo(bdesc, ranges->attno, ranges->typid,
											   BTLessStrategyNumber);

	/* and we'll also need the 'distance' procedure */
	distanceFn = minmax_multi_get_procinfo(bdesc, ranges->attno, PROCNUM_DISTANCE);

	/*
	 * The distanceFn calls (which may internally call e.g. numeric_le) may
	 * allocate quite a bit of memory, and we must not leak it. Otherwise,
	 * we'd have problems e.g. when building indexes. So we create a local
	 * memory context and make sure we free the memory before leaving this
	 * function (not after every call).
	 */
	ctx = AllocSetContextCreate(CurrentMemoryContext,
								"minmax-multi context",
								ALLOCSET_DEFAULT_SIZES);

	oldctx = MemoryContextSwitchTo(ctx);

	/* build the expanded ranges */
	eranges = build_expanded_ranges(cmpFn, ranges->colloid, ranges, &neranges);

	/* build array of gap distances and sort them in ascending order */
	distances = build_distances(distanceFn, ranges->colloid,
								eranges, neranges);

	/*
	 * Combine ranges until we get below max_values. We don't use any scale
	 * factor, because this is used during serialization, and we don't expect
	 * more tuples to be inserted anytime soon.
	 */
	neranges = reduce_expanded_ranges(eranges, neranges, distances,
									  max_values, cmpFn, ranges->colloid);

	Assert(count_values(eranges, neranges) <= max_values);

	/* transform back into regular ranges and single values */
	store_expanded_ranges(ranges, eranges, neranges);

	/* check all the range invariants */
	AssertCheckRanges(ranges, cmpFn, ranges->colloid);

	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(ctx);
}

Datum
brin_minmax_multi_opcinfo(PG_FUNCTION_ARGS)
{
	BrinOpcInfo *result;

	/*
	 * opaque->strategy_procinfos is initialized lazily; here it is set to
	 * all-uninitialized by palloc0 which sets fn_oid to InvalidOid.
	 */

	result = palloc0(MAXALIGN(SizeofBrinOpcInfo(1)) +
					 sizeof(MinmaxMultiOpaque));
	result->oi_nstored = 1;
	result->oi_regular_nulls = true;
	result->oi_opaque = (MinmaxMultiOpaque *)
		MAXALIGN((char *) result + SizeofBrinOpcInfo(1));
	result->oi_typcache[0] = lookup_type_cache(PG_BRIN_MINMAX_MULTI_SUMMARYOID, 0);

	PG_RETURN_POINTER(result);
}

/*
 * Compute the distance between two float4 values (plain subtraction).
 */
Datum
brin_minmax_multi_distance_float4(PG_FUNCTION_ARGS)
{
	float		a1 = PG_GETARG_FLOAT4(0);
	float		a2 = PG_GETARG_FLOAT4(1);

	/* if both values are NaN, then we consider them the same */
	if (isnan(a1) && isnan(a2))
		PG_RETURN_FLOAT8(0.0);

	/* if one value is NaN, use infinite distance */
	if (isnan(a1) || isnan(a2))
		PG_RETURN_FLOAT8(get_float8_infinity());

	/*
	 * We know the values are range boundaries, but the range may be collapsed
	 * (i.e. single points), with equal values.
	 */
	Assert(a1 <= a2);

	PG_RETURN_FLOAT8((double) a2 - (double) a1);
}

/*
 * Compute the distance between two float8 values (plain subtraction).
 */
Datum
brin_minmax_multi_distance_float8(PG_FUNCTION_ARGS)
{
	double		a1 = PG_GETARG_FLOAT8(0);
	double		a2 = PG_GETARG_FLOAT8(1);

	/* if both values are NaN, then we consider them the same */
	if (isnan(a1) && isnan(a2))
		PG_RETURN_FLOAT8(0.0);

	/* if one value is NaN, use infinite distance */
	if (isnan(a1) || isnan(a2))
		PG_RETURN_FLOAT8(get_float8_infinity());

	/*
	 * We know the values are range boundaries, but the range may be collapsed
	 * (i.e. single points), with equal values.
	 */
	Assert(a1 <= a2);

	PG_RETURN_FLOAT8(a2 - a1);
}

/*
 * Compute the distance between two int2 values (plain subtraction).
 */
Datum
brin_minmax_multi_distance_int2(PG_FUNCTION_ARGS)
{
	int16		a1 = PG_GETARG_INT16(0);
	int16		a2 = PG_GETARG_INT16(1);

	/*
	 * We know the values are range boundaries, but the range may be collapsed
	 * (i.e. single points), with equal values.
	 */
	Assert(a1 <= a2);

	PG_RETURN_FLOAT8((double) a2 - (double) a1);
}

/*
 * Compute the distance between two int4 values (plain subtraction).
 */
Datum
brin_minmax_multi_distance_int4(PG_FUNCTION_ARGS)
{
	int32		a1 = PG_GETARG_INT32(0);
	int32		a2 = PG_GETARG_INT32(1);

	/*
	 * We know the values are range boundaries, but the range may be collapsed
	 * (i.e. single points), with equal values.
	 */
	Assert(a1 <= a2);

	PG_RETURN_FLOAT8((double) a2 - (double) a1);
}

/*
 * Compute the distance between two int8 values (plain subtraction).
 */
Datum
brin_minmax_multi_distance_int8(PG_FUNCTION_ARGS)
{
	int64		a1 = PG_GETARG_INT64(0);
	int64		a2 = PG_GETARG_INT64(1);

	/*
	 * We know the values are range boundaries, but the range may be collapsed
	 * (i.e. single points), with equal values.
	 */
	Assert(a1 <= a2);

	PG_RETURN_FLOAT8((double) a2 - (double) a1);
}

/*
 * Compute the distance between two tid values (by mapping them to float8 and
 * then subtracting them).
 */
Datum
brin_minmax_multi_distance_tid(PG_FUNCTION_ARGS)
{
	double		da1,
				da2;

	ItemPointer pa1 = (ItemPointer) PG_GETARG_DATUM(0);
	ItemPointer pa2 = (ItemPointer) PG_GETARG_DATUM(1);

	/*
	 * We know the values are range boundaries, but the range may be collapsed
	 * (i.e. single points), with equal values.
	 */
	Assert(ItemPointerCompare(pa1, pa2) <= 0);

	/*
	 * We use the no-check variants here, because user-supplied values may
	 * have (ip_posid == 0). See ItemPointerCompare.
	 */
	da1 = ItemPointerGetBlockNumberNoCheck(pa1) * MaxHeapTuplesPerPage +
		ItemPointerGetOffsetNumberNoCheck(pa1);

	da2 = ItemPointerGetBlockNumberNoCheck(pa2) * MaxHeapTuplesPerPage +
		ItemPointerGetOffsetNumberNoCheck(pa2);

	PG_RETURN_FLOAT8(da2 - da1);
}

/*
 * Compute the distance between two numeric values (plain subtraction).
 */
Datum
brin_minmax_multi_distance_numeric(PG_FUNCTION_ARGS)
{
	Datum		d;
	Datum		a1 = PG_GETARG_DATUM(0);
	Datum		a2 = PG_GETARG_DATUM(1);

	/*
	 * We know the values are range boundaries, but the range may be collapsed
	 * (i.e. single points), with equal values.
	 */
	Assert(DatumGetBool(DirectFunctionCall2(numeric_le, a1, a2)));

	d = DirectFunctionCall2(numeric_sub, a2, a1);	/* a2 - a1 */

	PG_RETURN_FLOAT8(DirectFunctionCall1(numeric_float8, d));
}

/*
 * Compute the approximate distance between two UUID values.
 *
 * XXX We do not need a perfectly accurate value, so we approximate the
 * deltas (which would have to be 128-bit integers) with a 64-bit float.
 * The small inaccuracies do not matter in practice, in the worst case
 * we'll decide to merge ranges that are not the closest ones.
 */
Datum
brin_minmax_multi_distance_uuid(PG_FUNCTION_ARGS)
{
	int			i;
	float8		delta = 0;

	Datum		a1 = PG_GETARG_DATUM(0);
	Datum		a2 = PG_GETARG_DATUM(1);

	pg_uuid_t  *u1 = DatumGetUUIDP(a1);
	pg_uuid_t  *u2 = DatumGetUUIDP(a2);

	/*
	 * We know the values are range boundaries, but the range may be collapsed
	 * (i.e. single points), with equal values.
	 */
	Assert(DatumGetBool(DirectFunctionCall2(uuid_le, a1, a2)));

	/* compute approximate delta as a double precision value */
	for (i = UUID_LEN - 1; i >= 0; i--)
	{
		delta += (int) u2->data[i] - (int) u1->data[i];
		delta /= 256;
	}

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * Compute the approximate distance between two dates.
 */
Datum
brin_minmax_multi_distance_date(PG_FUNCTION_ARGS)
{
	float8		delta = 0;
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	delta = (float8) dateVal2 - (float8) dateVal1;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * Compute the approximate distance between two time (without tz) values.
 *
 * TimeADT is just an int64, so we simply subtract the values directly.
 */
Datum
brin_minmax_multi_distance_time(PG_FUNCTION_ARGS)
{
	float8		delta = 0;

	TimeADT		ta = PG_GETARG_TIMEADT(0);
	TimeADT		tb = PG_GETARG_TIMEADT(1);

	delta = (tb - ta);

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * Compute the approximate distance between two timetz values.
 *
 * Simply subtracts the TimeADT (int64) values embedded in TimeTzADT.
 */
Datum
brin_minmax_multi_distance_timetz(PG_FUNCTION_ARGS)
{
	float8		delta = 0;

	TimeTzADT  *ta = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *tb = PG_GETARG_TIMETZADT_P(1);

	delta = (tb->time - ta->time) + (tb->zone - ta->zone) * USECS_PER_SEC;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * Compute the distance between two timestamp values.
 */
Datum
brin_minmax_multi_distance_timestamp(PG_FUNCTION_ARGS)
{
	float8		delta = 0;

	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	delta = (float8) dt2 - (float8) dt1;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * Compute the distance between two interval values.
 */
Datum
brin_minmax_multi_distance_interval(PG_FUNCTION_ARGS)
{
	float8		delta = 0;

	Interval   *ia = PG_GETARG_INTERVAL_P(0);
	Interval   *ib = PG_GETARG_INTERVAL_P(1);

	int64		dayfraction;
	int64		days;

	/*
	 * Delta is (fractional) number of days between the intervals. Assume
	 * months have 30 days for consistency with interval_cmp_internal. We
	 * don't need to be exact, in the worst case we'll build a bit less
	 * efficient ranges. But we should not contradict interval_cmp.
	 */
	dayfraction = (ib->time % USECS_PER_DAY) - (ia->time % USECS_PER_DAY);
	days = (ib->time / USECS_PER_DAY) - (ia->time / USECS_PER_DAY);
	days += (int64) ib->day - (int64) ia->day;
	days += ((int64) ib->month - (int64) ia->month) * INT64CONST(30);

	/* convert to double precision */
	delta = (double) days + dayfraction / (double) USECS_PER_DAY;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * Compute the distance between two pg_lsn values.
 *
 * LSN is just an int64 encoding position in the stream, so just subtract
 * those int64 values directly.
 */
Datum
brin_minmax_multi_distance_pg_lsn(PG_FUNCTION_ARGS)
{
	float8		delta = 0;

	XLogRecPtr	lsna = PG_GETARG_LSN(0);
	XLogRecPtr	lsnb = PG_GETARG_LSN(1);

	delta = (lsnb - lsna);

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * Compute the distance between two macaddr values.
 *
 * mac addresses are treated as 6 unsigned chars, so do the same thing we
 * already do for UUID values.
 */
Datum
brin_minmax_multi_distance_macaddr(PG_FUNCTION_ARGS)
{
	float8		delta;

	macaddr    *a = PG_GETARG_MACADDR_P(0);
	macaddr    *b = PG_GETARG_MACADDR_P(1);

	delta = ((float8) b->f - (float8) a->f);
	delta /= 256;

	delta += ((float8) b->e - (float8) a->e);
	delta /= 256;

	delta += ((float8) b->d - (float8) a->d);
	delta /= 256;

	delta += ((float8) b->c - (float8) a->c);
	delta /= 256;

	delta += ((float8) b->b - (float8) a->b);
	delta /= 256;

	delta += ((float8) b->a - (float8) a->a);
	delta /= 256;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * Compute the distance between two macaddr8 values.
 *
 * macaddr8 addresses are 8 unsigned chars, so do the same thing we
 * already do for UUID values.
 */
Datum
brin_minmax_multi_distance_macaddr8(PG_FUNCTION_ARGS)
{
	float8		delta;

	macaddr8   *a = PG_GETARG_MACADDR8_P(0);
	macaddr8   *b = PG_GETARG_MACADDR8_P(1);

	delta = ((float8) b->h - (float8) a->h);
	delta /= 256;

	delta += ((float8) b->g - (float8) a->g);
	delta /= 256;

	delta += ((float8) b->f - (float8) a->f);
	delta /= 256;

	delta += ((float8) b->e - (float8) a->e);
	delta /= 256;

	delta += ((float8) b->d - (float8) a->d);
	delta /= 256;

	delta += ((float8) b->c - (float8) a->c);
	delta /= 256;

	delta += ((float8) b->b - (float8) a->b);
	delta /= 256;

	delta += ((float8) b->a - (float8) a->a);
	delta /= 256;

	Assert(delta >= 0);

	PG_RETURN_FLOAT8(delta);
}

/*
 * Compute the distance between two inet values.
 *
 * The distance is defined as the difference between 32-bit/128-bit values,
 * depending on the IP version. The distance is computed by subtracting
 * the bytes and normalizing it to [0,1] range for each IP family.
 * Addresses from different families are considered to be in maximum
 * distance, which is 1.0.
 *
 * XXX Does this need to consider the mask (bits)?  For now, it's ignored.
 */
Datum
brin_minmax_multi_distance_inet(PG_FUNCTION_ARGS)
{
	float8		delta;
	int			i;
	int			len;
	unsigned char *addra,
			   *addrb;

	inet	   *ipa = PG_GETARG_INET_PP(0);
	inet	   *ipb = PG_GETARG_INET_PP(1);

	int			lena,
				lenb;

	/*
	 * If the addresses are from different families, consider them to be in
	 * maximal possible distance (which is 1.0).
	 */
	if (ip_family(ipa) != ip_family(ipb))
		PG_RETURN_FLOAT8(1.0);

	addra = (unsigned char *) palloc(ip_addrsize(ipa));
	memcpy(addra, ip_addr(ipa), ip_addrsize(ipa));

	addrb = (unsigned char *) palloc(ip_addrsize(ipb));
	memcpy(addrb, ip_addr(ipb), ip_addrsize(ipb));

	/*
	 * The length is calculated from the mask length, because we sort the
	 * addresses by first address in the range, so A.B.C.D/24 < A.B.C.1 (the
	 * first range starts at A.B.C.0, which is before A.B.C.1). We don't want
	 * to produce a negative delta in this case, so we just cut the extra
	 * bytes.
	 *
	 * XXX Maybe this should be a bit more careful and cut the bits, not just
	 * whole bytes.
	 */
	lena = ip_bits(ipa);
	lenb = ip_bits(ipb);

	len = ip_addrsize(ipa);

	/* apply the network mask to both addresses */
	for (i = 0; i < len; i++)
	{
		unsigned char mask;
		int			nbits;

		nbits = Max(0, lena - (i * 8));
		if (nbits < 8)
		{
			mask = (0xFF << (8 - nbits));
			addra[i] = (addra[i] & mask);
		}

		nbits = Max(0, lenb - (i * 8));
		if (nbits < 8)
		{
			mask = (0xFF << (8 - nbits));
			addrb[i] = (addrb[i] & mask);
		}
	}

	/* Calculate the difference between the addresses. */
	delta = 0;
	for (i = len - 1; i >= 0; i--)
	{
		unsigned char a = addra[i];
		unsigned char b = addrb[i];

		delta += (float8) b - (float8) a;
		delta /= 256;
	}

	Assert((delta >= 0) && (delta <= 1));

	pfree(addra);
	pfree(addrb);

	PG_RETURN_FLOAT8(delta);
}

static void
brin_minmax_multi_serialize(BrinDesc *bdesc, Datum src, Datum *dst)
{
	Ranges	   *ranges = (Ranges *) DatumGetPointer(src);
	SerializedRanges *s;

	/*
	 * In batch mode, we need to compress the accumulated values to the
	 * actually requested number of values/ranges.
	 */
	compactify_ranges(bdesc, ranges, ranges->target_maxvalues);

	/* At this point everything has to be fully sorted. */
	Assert(ranges->nsorted == ranges->nvalues);

	s = brin_range_serialize(ranges);
	dst[0] = PointerGetDatum(s);
}

static int
brin_minmax_multi_get_values(BrinDesc *bdesc, MinMaxMultiOptions *opts)
{
	return MinMaxMultiGetValuesPerRange(opts);
}

/*
 * Examine the given index tuple (which contains the partial status of a
 * certain page range) by comparing it to the given value that comes from
 * another heap tuple.  If the new value is outside the min/max range
 * specified by the existing tuple values, update the index tuple and return
 * true.  Otherwise, return false and do not modify in this case.
 */
Datum
brin_minmax_multi_add_value(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	Datum		newval = PG_GETARG_DATUM(2);
	bool		isnull PG_USED_FOR_ASSERTS_ONLY = PG_GETARG_DATUM(3);
	MinMaxMultiOptions *opts = (MinMaxMultiOptions *) PG_GET_OPCLASS_OPTIONS();
	Oid			colloid = PG_GET_COLLATION();
	bool		modified = false;
	Form_pg_attribute attr;
	AttrNumber	attno;
	Ranges	   *ranges;
	SerializedRanges *serialized = NULL;

	Assert(!isnull);

	attno = column->bv_attno;
	attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);

	/* use the already deserialized value, if possible */
	ranges = (Ranges *) DatumGetPointer(column->bv_mem_value);

	/*
	 * If this is the first non-null value, we need to initialize the range
	 * list. Otherwise, just extract the existing range list from BrinValues.
	 *
	 * When starting with an empty range, we assume this is a batch mode and
	 * we use a larger buffer. The buffer size is derived from the BRIN range
	 * size, number of rows per page, with some sensible min/max values. A
	 * small buffer would be bad for performance, but a large buffer might
	 * require a lot of memory (because of keeping all the values).
	 */
	if (column->bv_allnulls)
	{
		MemoryContext oldctx;

		int			target_maxvalues;
		int			maxvalues;
		BlockNumber pagesPerRange = BrinGetPagesPerRange(bdesc->bd_index);

		/* what was specified as a reloption? */
		target_maxvalues = brin_minmax_multi_get_values(bdesc, opts);

		/*
		 * Determine the insert buffer size - we use 10x the target, capped to
		 * the maximum number of values in the heap range. This is more than
		 * enough, considering the actual number of rows per page is likely
		 * much lower, but meh.
		 */
		maxvalues = Min(target_maxvalues * MINMAX_BUFFER_FACTOR,
						MaxHeapTuplesPerPage * pagesPerRange);

		/* but always at least the original value */
		maxvalues = Max(maxvalues, target_maxvalues);

		/* always cap by MIN/MAX */
		maxvalues = Max(maxvalues, MINMAX_BUFFER_MIN);
		maxvalues = Min(maxvalues, MINMAX_BUFFER_MAX);

		oldctx = MemoryContextSwitchTo(column->bv_context);
		ranges = minmax_multi_init(maxvalues);
		ranges->attno = attno;
		ranges->colloid = colloid;
		ranges->typid = attr->atttypid;
		ranges->target_maxvalues = target_maxvalues;

		/* we'll certainly need the comparator, so just look it up now */
		ranges->cmp = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
														 BTLessStrategyNumber);

		MemoryContextSwitchTo(oldctx);

		column->bv_allnulls = false;
		modified = true;

		column->bv_mem_value = PointerGetDatum(ranges);
		column->bv_serialize = brin_minmax_multi_serialize;
	}
	else if (!ranges)
	{
		MemoryContext oldctx;

		int			maxvalues;
		BlockNumber pagesPerRange = BrinGetPagesPerRange(bdesc->bd_index);

		oldctx = MemoryContextSwitchTo(column->bv_context);

		serialized = (SerializedRanges *) PG_DETOAST_DATUM(column->bv_values[0]);

		/*
		 * Determine the insert buffer size - we use 10x the target, capped to
		 * the maximum number of values in the heap range. This is more than
		 * enough, considering the actual number of rows per page is likely
		 * much lower, but meh.
		 */
		maxvalues = Min(serialized->maxvalues * MINMAX_BUFFER_FACTOR,
						MaxHeapTuplesPerPage * pagesPerRange);

		/* but always at least the original value */
		maxvalues = Max(maxvalues, serialized->maxvalues);

		/* always cap by MIN/MAX */
		maxvalues = Max(maxvalues, MINMAX_BUFFER_MIN);
		maxvalues = Min(maxvalues, MINMAX_BUFFER_MAX);

		ranges = brin_range_deserialize(maxvalues, serialized);

		ranges->attno = attno;
		ranges->colloid = colloid;
		ranges->typid = attr->atttypid;

		/* we'll certainly need the comparator, so just look it up now */
		ranges->cmp = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
														 BTLessStrategyNumber);

		column->bv_mem_value = PointerGetDatum(ranges);
		column->bv_serialize = brin_minmax_multi_serialize;

		MemoryContextSwitchTo(oldctx);
	}

	/*
	 * Try to add the new value to the range. We need to update the modified
	 * flag, so that we serialize the updated summary later.
	 */
	modified |= range_add_value(bdesc, colloid, attno, attr, ranges, newval);


	PG_RETURN_BOOL(modified);
}

/*
 * Given an index tuple corresponding to a certain page range and a scan key,
 * return whether the scan key is consistent with the index tuple's min/max
 * values.  Return true if so, false otherwise.
 */
Datum
brin_minmax_multi_consistent(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	ScanKey    *keys = (ScanKey *) PG_GETARG_POINTER(2);
	int			nkeys = PG_GETARG_INT32(3);

	Oid			colloid = PG_GET_COLLATION(),
				subtype;
	AttrNumber	attno;
	Datum		value;
	FmgrInfo   *finfo;
	SerializedRanges *serialized;
	Ranges	   *ranges;
	int			keyno;
	int			rangeno;
	int			i;

	attno = column->bv_attno;

	serialized = (SerializedRanges *) PG_DETOAST_DATUM(column->bv_values[0]);
	ranges = brin_range_deserialize(serialized->maxvalues, serialized);

	/* inspect the ranges, and for each one evaluate the scan keys */
	for (rangeno = 0; rangeno < ranges->nranges; rangeno++)
	{
		Datum		minval = ranges->values[2 * rangeno];
		Datum		maxval = ranges->values[2 * rangeno + 1];

		/* assume the range is matching, and we'll try to prove otherwise */
		bool		matching = true;

		for (keyno = 0; keyno < nkeys; keyno++)
		{
			Datum		matches;
			ScanKey		key = keys[keyno];

			/* NULL keys are handled and filtered-out in bringetbitmap */
			Assert(!(key->sk_flags & SK_ISNULL));

			attno = key->sk_attno;
			subtype = key->sk_subtype;
			value = key->sk_argument;
			switch (key->sk_strategy)
			{
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
					finfo = minmax_multi_get_strategy_procinfo(bdesc, attno, subtype,
															   key->sk_strategy);
					/* first value from the array */
					matches = FunctionCall2Coll(finfo, colloid, minval, value);
					break;

				case BTEqualStrategyNumber:
					{
						Datum		compar;
						FmgrInfo   *cmpFn;

						/* by default this range does not match */
						matches = false;

						/*
						 * Otherwise, need to compare the new value with
						 * boundaries of all the ranges. First check if it's
						 * less than the absolute minimum, which is the first
						 * value in the array.
						 */
						cmpFn = minmax_multi_get_strategy_procinfo(bdesc, attno, subtype,
																   BTGreaterStrategyNumber);
						compar = FunctionCall2Coll(cmpFn, colloid, minval, value);

						/* smaller than the smallest value in this range */
						if (DatumGetBool(compar))
							break;

						cmpFn = minmax_multi_get_strategy_procinfo(bdesc, attno, subtype,
																   BTLessStrategyNumber);
						compar = FunctionCall2Coll(cmpFn, colloid, maxval, value);

						/* larger than the largest value in this range */
						if (DatumGetBool(compar))
							break;

						/*
						 * We haven't managed to eliminate this range, so
						 * consider it matching.
						 */
						matches = true;

						break;
					}
				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:
					finfo = minmax_multi_get_strategy_procinfo(bdesc, attno, subtype,
															   key->sk_strategy);
					/* last value from the array */
					matches = FunctionCall2Coll(finfo, colloid, maxval, value);
					break;

				default:
					/* shouldn't happen */
					elog(ERROR, "invalid strategy number %d", key->sk_strategy);
					matches = 0;
					break;
			}

			/* the range has to match all the scan keys */
			matching &= DatumGetBool(matches);

			/* once we find a non-matching key, we're done */
			if (!matching)
				break;
		}

		/*
		 * have we found a range matching all scan keys? if yes, we're done
		 */
		if (matching)
			PG_RETURN_DATUM(BoolGetDatum(true));
	}

	/*
	 * And now inspect the values. We don't bother with doing a binary search
	 * here, because we're dealing with serialized / fully compacted ranges,
	 * so there should be only very few values.
	 */
	for (i = 0; i < ranges->nvalues; i++)
	{
		Datum		val = ranges->values[2 * ranges->nranges + i];

		/* assume the range is matching, and we'll try to prove otherwise */
		bool		matching = true;

		for (keyno = 0; keyno < nkeys; keyno++)
		{
			Datum		matches;
			ScanKey		key = keys[keyno];

			/* we've already dealt with NULL keys at the beginning */
			if (key->sk_flags & SK_ISNULL)
				continue;

			attno = key->sk_attno;
			subtype = key->sk_subtype;
			value = key->sk_argument;
			switch (key->sk_strategy)
			{
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
				case BTEqualStrategyNumber:
				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:

					finfo = minmax_multi_get_strategy_procinfo(bdesc, attno, subtype,
															   key->sk_strategy);
					matches = FunctionCall2Coll(finfo, colloid, val, value);
					break;

				default:
					/* shouldn't happen */
					elog(ERROR, "invalid strategy number %d", key->sk_strategy);
					matches = 0;
					break;
			}

			/* the range has to match all the scan keys */
			matching &= DatumGetBool(matches);

			/* once we find a non-matching key, we're done */
			if (!matching)
				break;
		}

		/* have we found a range matching all scan keys? if yes, we're done */
		if (matching)
			PG_RETURN_DATUM(BoolGetDatum(true));
	}

	PG_RETURN_DATUM(BoolGetDatum(false));
}

/*
 * Given two BrinValues, update the first of them as a union of the summary
 * values contained in both.  The second one is untouched.
 */
Datum
brin_minmax_multi_union(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *col_a = (BrinValues *) PG_GETARG_POINTER(1);
	BrinValues *col_b = (BrinValues *) PG_GETARG_POINTER(2);

	Oid			colloid = PG_GET_COLLATION();
	SerializedRanges *serialized_a;
	SerializedRanges *serialized_b;
	Ranges	   *ranges_a;
	Ranges	   *ranges_b;
	AttrNumber	attno;
	Form_pg_attribute attr;
	ExpandedRange *eranges;
	int			neranges;
	FmgrInfo   *cmpFn,
			   *distanceFn;
	DistanceValue *distances;
	MemoryContext ctx;
	MemoryContext oldctx;

	Assert(col_a->bv_attno == col_b->bv_attno);
	Assert(!col_a->bv_allnulls && !col_b->bv_allnulls);

	attno = col_a->bv_attno;
	attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);

	serialized_a = (SerializedRanges *) PG_DETOAST_DATUM(col_a->bv_values[0]);
	serialized_b = (SerializedRanges *) PG_DETOAST_DATUM(col_b->bv_values[0]);

	ranges_a = brin_range_deserialize(serialized_a->maxvalues, serialized_a);
	ranges_b = brin_range_deserialize(serialized_b->maxvalues, serialized_b);

	/* make sure neither of the ranges is NULL */
	Assert(ranges_a && ranges_b);

	neranges = (ranges_a->nranges + ranges_a->nvalues) +
		(ranges_b->nranges + ranges_b->nvalues);

	/*
	 * The distanceFn calls (which may internally call e.g. numeric_le) may
	 * allocate quite a bit of memory, and we must not leak it. Otherwise,
	 * we'd have problems e.g. when building indexes. So we create a local
	 * memory context and make sure we free the memory before leaving this
	 * function (not after every call).
	 */
	ctx = AllocSetContextCreate(CurrentMemoryContext,
								"minmax-multi context",
								ALLOCSET_DEFAULT_SIZES);

	oldctx = MemoryContextSwitchTo(ctx);

	/* allocate and fill */
	eranges = (ExpandedRange *) palloc0(neranges * sizeof(ExpandedRange));

	/* fill the expanded ranges with entries for the first range */
	fill_expanded_ranges(eranges, ranges_a->nranges + ranges_a->nvalues,
						 ranges_a);

	/* and now add combine ranges for the second range */
	fill_expanded_ranges(&eranges[ranges_a->nranges + ranges_a->nvalues],
						 ranges_b->nranges + ranges_b->nvalues,
						 ranges_b);

	cmpFn = minmax_multi_get_strategy_procinfo(bdesc, attno, attr->atttypid,
											   BTLessStrategyNumber);

	/* sort the expanded ranges */
	neranges = sort_expanded_ranges(cmpFn, colloid, eranges, neranges);

	/*
	 * We've loaded two different lists of expanded ranges, so some of them
	 * may be overlapping. So walk through them and merge them.
	 */
	neranges = merge_overlapping_ranges(cmpFn, colloid, eranges, neranges);

	/* check that the combine ranges are correct (no overlaps, ordering) */
	AssertCheckExpandedRanges(bdesc, colloid, attno, attr, eranges, neranges);

	/*
	 * If needed, reduce some of the ranges.
	 *
	 * XXX This may be fairly expensive, so maybe we should do it only when
	 * it's actually needed (when we have too many ranges).
	 */

	/* build array of gap distances and sort them in ascending order */
	distanceFn = minmax_multi_get_procinfo(bdesc, attno, PROCNUM_DISTANCE);
	distances = build_distances(distanceFn, colloid, eranges, neranges);

	/*
	 * See how many values would be needed to store the current ranges, and if
	 * needed combine as many of them to get below the threshold. The
	 * collapsed ranges will be stored as a single value.
	 *
	 * XXX This does not apply the load factor, as we don't expect to add more
	 * values to the range, so we prefer to keep as many ranges as possible.
	 *
	 * XXX Can the maxvalues be different in the two ranges? Perhaps we should
	 * use maximum of those?
	 */
	neranges = reduce_expanded_ranges(eranges, neranges, distances,
									  ranges_a->maxvalues,
									  cmpFn, colloid);

	/* update the first range summary */
	store_expanded_ranges(ranges_a, eranges, neranges);

	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(ctx);

	/* cleanup and update the serialized value */
	pfree(serialized_a);
	col_a->bv_values[0] = PointerGetDatum(brin_range_serialize(ranges_a));

	PG_RETURN_VOID();
}

/*
 * Cache and return minmax multi opclass support procedure
 *
 * Return the procedure corresponding to the given function support number
 * or null if it does not exist.
 */
static FmgrInfo *
minmax_multi_get_procinfo(BrinDesc *bdesc, uint16 attno, uint16 procnum)
{
	MinmaxMultiOpaque *opaque;
	uint16		basenum = procnum - PROCNUM_BASE;

	/*
	 * We cache these in the opaque struct, to avoid repetitive syscache
	 * lookups.
	 */
	opaque = (MinmaxMultiOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * If we already searched for this proc and didn't find it, don't bother
	 * searching again.
	 */
	if (opaque->extra_proc_missing[basenum])
		return NULL;

	if (opaque->extra_procinfos[basenum].fn_oid == InvalidOid)
	{
		if (RegProcedureIsValid(index_getprocid(bdesc->bd_index, attno,
												procnum)))
		{
			fmgr_info_copy(&opaque->extra_procinfos[basenum],
						   index_getprocinfo(bdesc->bd_index, attno, procnum),
						   bdesc->bd_context);
		}
		else
		{
			opaque->extra_proc_missing[basenum] = true;
			return NULL;
		}
	}

	return &opaque->extra_procinfos[basenum];
}

/*
 * Cache and return the procedure for the given strategy.
 *
 * Note: this function mirrors minmax_multi_get_strategy_procinfo; see notes
 * there.  If changes are made here, see that function too.
 */
static FmgrInfo *
minmax_multi_get_strategy_procinfo(BrinDesc *bdesc, uint16 attno, Oid subtype,
								   uint16 strategynum)
{
	MinmaxMultiOpaque *opaque;

	Assert(strategynum >= 1 &&
		   strategynum <= BTMaxStrategyNumber);

	opaque = (MinmaxMultiOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * We cache the procedures for the previous subtype in the opaque struct,
	 * to avoid repetitive syscache lookups.  If the subtype changed,
	 * invalidate all the cached entries.
	 */
	if (opaque->cached_subtype != subtype)
	{
		uint16		i;

		for (i = 1; i <= BTMaxStrategyNumber; i++)
			opaque->strategy_procinfos[i - 1].fn_oid = InvalidOid;
		opaque->cached_subtype = subtype;
	}

	if (opaque->strategy_procinfos[strategynum - 1].fn_oid == InvalidOid)
	{
		Form_pg_attribute attr;
		HeapTuple	tuple;
		Oid			opfamily,
					oprid;

		opfamily = bdesc->bd_index->rd_opfamily[attno - 1];
		attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);
		tuple = SearchSysCache4(AMOPSTRATEGY, ObjectIdGetDatum(opfamily),
								ObjectIdGetDatum(attr->atttypid),
								ObjectIdGetDatum(subtype),
								Int16GetDatum(strategynum));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 strategynum, attr->atttypid, subtype, opfamily);

		oprid = DatumGetObjectId(SysCacheGetAttrNotNull(AMOPSTRATEGY, tuple,
														Anum_pg_amop_amopopr));
		ReleaseSysCache(tuple);
		Assert(RegProcedureIsValid(oprid));

		fmgr_info_cxt(get_opcode(oprid),
					  &opaque->strategy_procinfos[strategynum - 1],
					  bdesc->bd_context);
	}

	return &opaque->strategy_procinfos[strategynum - 1];
}

Datum
brin_minmax_multi_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(MinMaxMultiOptions));

	add_local_int_reloption(relopts, "values_per_range", "desc",
							MINMAX_MULTI_DEFAULT_VALUES_PER_PAGE, 8, 256,
							offsetof(MinMaxMultiOptions, valuesPerRange));

	PG_RETURN_VOID();
}

/*
 * brin_minmax_multi_summary_in
 *		- input routine for type brin_minmax_multi_summary.
 *
 * brin_minmax_multi_summary is only used internally to represent summaries
 * in BRIN minmax-multi indexes, so it has no operations of its own, and we
 * disallow input too.
 */
Datum
brin_minmax_multi_summary_in(PG_FUNCTION_ARGS)
{
	/*
	 * brin_minmax_multi_summary stores the data in binary form and parsing
	 * text input is not needed, so disallow this.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "brin_minmax_multi_summary")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}


/*
 * brin_minmax_multi_summary_out
 *		- output routine for type brin_minmax_multi_summary.
 *
 * BRIN minmax-multi summaries are serialized into a bytea value, but we
 * want to output something nicer humans can understand.
 */
Datum
brin_minmax_multi_summary_out(PG_FUNCTION_ARGS)
{
	int			i;
	int			idx;
	SerializedRanges *ranges;
	Ranges	   *ranges_deserialized;
	StringInfoData str;
	bool		isvarlena;
	Oid			outfunc;
	FmgrInfo	fmgrinfo;
	ArrayBuildState *astate_values = NULL;

	initStringInfo(&str);
	appendStringInfoChar(&str, '{');

	/*
	 * Detoast to get value with full 4B header (can't be stored in a toast
	 * table, but can use 1B header).
	 */
	ranges = (SerializedRanges *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));

	/* lookup output func for the type */
	getTypeOutputInfo(ranges->typid, &outfunc, &isvarlena);
	fmgr_info(outfunc, &fmgrinfo);

	/* deserialize the range info easy-to-process pieces */
	ranges_deserialized = brin_range_deserialize(ranges->maxvalues, ranges);

	appendStringInfo(&str, "nranges: %d  nvalues: %d  maxvalues: %d",
					 ranges_deserialized->nranges,
					 ranges_deserialized->nvalues,
					 ranges_deserialized->maxvalues);

	/* serialize ranges */
	idx = 0;
	for (i = 0; i < ranges_deserialized->nranges; i++)
	{
		char	   *a,
				   *b;
		text	   *c;
		StringInfoData buf;

		initStringInfo(&buf);

		a = OutputFunctionCall(&fmgrinfo, ranges_deserialized->values[idx++]);
		b = OutputFunctionCall(&fmgrinfo, ranges_deserialized->values[idx++]);

		appendStringInfo(&buf, "%s ... %s", a, b);

		c = cstring_to_text_with_len(buf.data, buf.len);

		astate_values = accumArrayResult(astate_values,
										 PointerGetDatum(c),
										 false,
										 TEXTOID,
										 CurrentMemoryContext);
	}

	if (ranges_deserialized->nranges > 0)
	{
		Oid			typoutput;
		bool		typIsVarlena;
		Datum		val;
		char	   *extval;

		getTypeOutputInfo(ANYARRAYOID, &typoutput, &typIsVarlena);

		val = makeArrayResult(astate_values, CurrentMemoryContext);

		extval = OidOutputFunctionCall(typoutput, val);

		appendStringInfo(&str, " ranges: %s", extval);
	}

	/* serialize individual values */
	astate_values = NULL;

	for (i = 0; i < ranges_deserialized->nvalues; i++)
	{
		Datum		a;
		text	   *b;

		a = FunctionCall1(&fmgrinfo, ranges_deserialized->values[idx++]);
		b = cstring_to_text(DatumGetCString(a));

		astate_values = accumArrayResult(astate_values,
										 PointerGetDatum(b),
										 false,
										 TEXTOID,
										 CurrentMemoryContext);
	}

	if (ranges_deserialized->nvalues > 0)
	{
		Oid			typoutput;
		bool		typIsVarlena;
		Datum		val;
		char	   *extval;

		getTypeOutputInfo(ANYARRAYOID, &typoutput, &typIsVarlena);

		val = makeArrayResult(astate_values, CurrentMemoryContext);

		extval = OidOutputFunctionCall(typoutput, val);

		appendStringInfo(&str, " values: %s", extval);
	}


	appendStringInfoChar(&str, '}');

	PG_RETURN_CSTRING(str.data);
}

/*
 * brin_minmax_multi_summary_recv
 *		- binary input routine for type brin_minmax_multi_summary.
 */
Datum
brin_minmax_multi_summary_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "brin_minmax_multi_summary")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * brin_minmax_multi_summary_send
 *		- binary output routine for type brin_minmax_multi_summary.
 *
 * BRIN minmax-multi summaries are serialized in a bytea value (although
 * the type is named differently), so let's just send that.
 */
Datum
brin_minmax_multi_summary_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}
