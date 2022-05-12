/*-------------------------------------------------------------------------
 *
 * multirangetypes.c
 *	  I/O functions, operators, and support functions for multirange types.
 *
 * The stored (serialized) format of a multirange value is:
 *
 *	12 bytes: MultirangeType struct including varlena header, multirange
 *			  type's OID and the number of ranges in the multirange.
 *	4 * (rangesCount - 1) bytes: 32-bit items pointing to the each range
 *								 in the multirange starting from
 *								 the second one.
 *	1 * rangesCount bytes : 8-bit flags for each range in the multirange
 *	The rest of the multirange are range bound values pointed by multirange
 *	items.
 *
 *	Majority of items contain lengths of corresponding range bound values.
 *	Thanks to that items are typically low numbers.  This makes multiranges
 *	compression-friendly.  Every MULTIRANGE_ITEM_OFFSET_STRIDE item contains
 *	an offset of the corresponding range bound values.  That allows fast lookups
 *	for a particular range index.  Offsets are counted starting from the end of
 *	flags aligned to the bound type.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/multirangetypes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/multirangetypes.h"
#include "utils/array.h"
#include "utils/memutils.h"

/* fn_extra cache entry for one of the range I/O functions */
typedef struct MultirangeIOData
{
	TypeCacheEntry *typcache;	/* multirange type's typcache entry */
	FmgrInfo	typioproc;		/* range type's I/O proc */
	Oid			typioparam;		/* range type's I/O parameter */
} MultirangeIOData;

typedef enum
{
	MULTIRANGE_BEFORE_RANGE,
	MULTIRANGE_IN_RANGE,
	MULTIRANGE_IN_RANGE_ESCAPED,
	MULTIRANGE_IN_RANGE_QUOTED,
	MULTIRANGE_IN_RANGE_QUOTED_ESCAPED,
	MULTIRANGE_AFTER_RANGE,
	MULTIRANGE_FINISHED,
} MultirangeParseState;

/*
 * Macros for accessing past MultirangeType parts of multirange: items, flags
 * and boundaries.
 */
#define MultirangeGetItemsPtr(mr) ((uint32 *) ((Pointer) (mr) + \
	sizeof(MultirangeType)))
#define MultirangeGetFlagsPtr(mr) ((uint8 *) ((Pointer) (mr) + \
	sizeof(MultirangeType) + ((mr)->rangeCount - 1) * sizeof(uint32)))
#define MultirangeGetBoundariesPtr(mr, align) ((Pointer) (mr) + \
	att_align_nominal(sizeof(MultirangeType) + \
		((mr)->rangeCount - 1) * sizeof(uint32) + \
		(mr)->rangeCount * sizeof(uint8), (align)))

#define MULTIRANGE_ITEM_OFF_BIT 0x80000000
#define MULTIRANGE_ITEM_GET_OFFLEN(item) ((item) & 0x7FFFFFFF)
#define MULTIRANGE_ITEM_HAS_OFF(item) ((item) & MULTIRANGE_ITEM_OFF_BIT)
#define MULTIRANGE_ITEM_OFFSET_STRIDE 4

typedef int (*multirange_bsearch_comparison) (TypeCacheEntry *typcache,
											  RangeBound *lower,
											  RangeBound *upper,
											  void *key,
											  bool *match);

static MultirangeIOData *get_multirange_io_data(FunctionCallInfo fcinfo,
												Oid mltrngtypid,
												IOFuncSelector func);
static int32 multirange_canonicalize(TypeCacheEntry *rangetyp,
									 int32 input_range_count,
									 RangeType **ranges);

/*
 *----------------------------------------------------------
 * I/O FUNCTIONS
 *----------------------------------------------------------
 */

/*
 * Converts string to multirange.
 *
 * We expect curly brackets to bound the list, with zero or more ranges
 * separated by commas.  We accept whitespace anywhere: before/after our
 * brackets and around the commas.  Ranges can be the empty literal or some
 * stuff inside parens/brackets.  Mostly we delegate parsing the individual
 * range contents to range_in, but we have to detect quoting and
 * backslash-escaping which can happen for range bounds.  Backslashes can
 * escape something inside or outside a quoted string, and a quoted string
 * can escape quote marks with either backslashes or double double-quotes.
 */
Datum
multirange_in(PG_FUNCTION_ARGS)
{
	char	   *input_str = PG_GETARG_CSTRING(0);
	Oid			mltrngtypoid = PG_GETARG_OID(1);
	Oid			typmod = PG_GETARG_INT32(2);
	TypeCacheEntry *rangetyp;
	int32		ranges_seen = 0;
	int32		range_count = 0;
	int32		range_capacity = 8;
	RangeType  *range;
	RangeType **ranges = palloc(range_capacity * sizeof(RangeType *));
	MultirangeIOData *cache;
	MultirangeType *ret;
	MultirangeParseState parse_state;
	const char *ptr = input_str;
	const char *range_str_begin = NULL;
	int32		range_str_len;
	char	   *range_str;

	cache = get_multirange_io_data(fcinfo, mltrngtypoid, IOFunc_input);
	rangetyp = cache->typcache->rngtype;

	/* consume whitespace */
	while (*ptr != '\0' && isspace((unsigned char) *ptr))
		ptr++;

	if (*ptr == '{')
		ptr++;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed multirange literal: \"%s\"",
						input_str),
				 errdetail("Missing left brace.")));

	/* consume ranges */
	parse_state = MULTIRANGE_BEFORE_RANGE;
	for (; parse_state != MULTIRANGE_FINISHED; ptr++)
	{
		char		ch = *ptr;

		if (ch == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed multirange literal: \"%s\"",
							input_str),
					 errdetail("Unexpected end of input.")));

		/* skip whitespace */
		if (isspace((unsigned char) ch))
			continue;

		switch (parse_state)
		{
			case MULTIRANGE_BEFORE_RANGE:
				if (ch == '[' || ch == '(')
				{
					range_str_begin = ptr;
					parse_state = MULTIRANGE_IN_RANGE;
				}
				else if (ch == '}' && ranges_seen == 0)
					parse_state = MULTIRANGE_FINISHED;
				else if (pg_strncasecmp(ptr, RANGE_EMPTY_LITERAL,
										strlen(RANGE_EMPTY_LITERAL)) == 0)
				{
					ranges_seen++;
					/* nothing to do with an empty range */
					ptr += strlen(RANGE_EMPTY_LITERAL) - 1;
					parse_state = MULTIRANGE_AFTER_RANGE;
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed multirange literal: \"%s\"",
									input_str),
							 errdetail("Expected range start.")));
				break;
			case MULTIRANGE_IN_RANGE:
				if (ch == ']' || ch == ')')
				{
					range_str_len = ptr - range_str_begin + 1;
					range_str = pnstrdup(range_str_begin, range_str_len);
					if (range_capacity == range_count)
					{
						range_capacity *= 2;
						ranges = (RangeType **)
							repalloc(ranges, range_capacity * sizeof(RangeType *));
					}
					ranges_seen++;
					range = DatumGetRangeTypeP(InputFunctionCall(&cache->typioproc,
																 range_str,
																 cache->typioparam,
																 typmod));
					if (!RangeIsEmpty(range))
						ranges[range_count++] = range;
					parse_state = MULTIRANGE_AFTER_RANGE;
				}
				else
				{
					if (ch == '"')
						parse_state = MULTIRANGE_IN_RANGE_QUOTED;
					else if (ch == '\\')
						parse_state = MULTIRANGE_IN_RANGE_ESCAPED;

					/*
					 * We will include this character into range_str once we
					 * find the end of the range value.
					 */
				}
				break;
			case MULTIRANGE_IN_RANGE_ESCAPED:

				/*
				 * We will include this character into range_str once we find
				 * the end of the range value.
				 */
				parse_state = MULTIRANGE_IN_RANGE;
				break;
			case MULTIRANGE_IN_RANGE_QUOTED:
				if (ch == '"')
					if (*(ptr + 1) == '"')
					{
						/* two quote marks means an escaped quote mark */
						ptr++;
					}
					else
						parse_state = MULTIRANGE_IN_RANGE;
				else if (ch == '\\')
					parse_state = MULTIRANGE_IN_RANGE_QUOTED_ESCAPED;

				/*
				 * We will include this character into range_str once we find
				 * the end of the range value.
				 */
				break;
			case MULTIRANGE_AFTER_RANGE:
				if (ch == ',')
					parse_state = MULTIRANGE_BEFORE_RANGE;
				else if (ch == '}')
					parse_state = MULTIRANGE_FINISHED;
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed multirange literal: \"%s\"",
									input_str),
							 errdetail("Expected comma or end of multirange.")));
				break;
			case MULTIRANGE_IN_RANGE_QUOTED_ESCAPED:

				/*
				 * We will include this character into range_str once we find
				 * the end of the range value.
				 */
				parse_state = MULTIRANGE_IN_RANGE_QUOTED;
				break;
			default:
				elog(ERROR, "unknown parse state: %d", parse_state);
		}
	}

	/* consume whitespace */
	while (*ptr != '\0' && isspace((unsigned char) *ptr))
		ptr++;

	if (*ptr != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed multirange literal: \"%s\"",
						input_str),
				 errdetail("Junk after closing right brace.")));

	ret = make_multirange(mltrngtypoid, rangetyp, range_count, ranges);
	PG_RETURN_MULTIRANGE_P(ret);
}

Datum
multirange_out(PG_FUNCTION_ARGS)
{
	MultirangeType *multirange = PG_GETARG_MULTIRANGE_P(0);
	Oid			mltrngtypoid = MultirangeTypeGetOid(multirange);
	MultirangeIOData *cache;
	StringInfoData buf;
	RangeType  *range;
	char	   *rangeStr;
	int32		range_count;
	int32		i;
	RangeType **ranges;

	cache = get_multirange_io_data(fcinfo, mltrngtypoid, IOFunc_output);

	initStringInfo(&buf);

	appendStringInfoChar(&buf, '{');

	multirange_deserialize(cache->typcache->rngtype, multirange, &range_count, &ranges);
	for (i = 0; i < range_count; i++)
	{
		if (i > 0)
			appendStringInfoChar(&buf, ',');
		range = ranges[i];
		rangeStr = OutputFunctionCall(&cache->typioproc, RangeTypePGetDatum(range));
		appendStringInfoString(&buf, rangeStr);
	}

	appendStringInfoChar(&buf, '}');

	PG_RETURN_CSTRING(buf.data);
}

/*
 * Binary representation: First a int32-sized count of ranges, followed by
 * ranges in their native binary representation.
 */
Datum
multirange_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			mltrngtypoid = PG_GETARG_OID(1);
	int32		typmod = PG_GETARG_INT32(2);
	MultirangeIOData *cache;
	uint32		range_count;
	RangeType **ranges;
	MultirangeType *ret;
	StringInfoData tmpbuf;

	cache = get_multirange_io_data(fcinfo, mltrngtypoid, IOFunc_receive);

	range_count = pq_getmsgint(buf, 4);
	ranges = palloc(range_count * sizeof(RangeType *));

	initStringInfo(&tmpbuf);
	for (int i = 0; i < range_count; i++)
	{
		uint32		range_len = pq_getmsgint(buf, 4);
		const char *range_data = pq_getmsgbytes(buf, range_len);

		resetStringInfo(&tmpbuf);
		appendBinaryStringInfo(&tmpbuf, range_data, range_len);

		ranges[i] = DatumGetRangeTypeP(ReceiveFunctionCall(&cache->typioproc,
														   &tmpbuf,
														   cache->typioparam,
														   typmod));
	}
	pfree(tmpbuf.data);

	pq_getmsgend(buf);

	ret = make_multirange(mltrngtypoid, cache->typcache->rngtype,
						  range_count, ranges);
	PG_RETURN_MULTIRANGE_P(ret);
}

Datum
multirange_send(PG_FUNCTION_ARGS)
{
	MultirangeType *multirange = PG_GETARG_MULTIRANGE_P(0);
	Oid			mltrngtypoid = MultirangeTypeGetOid(multirange);
	StringInfo	buf = makeStringInfo();
	RangeType **ranges;
	int32		range_count;
	MultirangeIOData *cache;

	cache = get_multirange_io_data(fcinfo, mltrngtypoid, IOFunc_send);

	/* construct output */
	pq_begintypsend(buf);

	pq_sendint32(buf, multirange->rangeCount);

	multirange_deserialize(cache->typcache->rngtype, multirange, &range_count, &ranges);
	for (int i = 0; i < range_count; i++)
	{
		Datum		range;

		range = RangeTypePGetDatum(ranges[i]);
		range = PointerGetDatum(SendFunctionCall(&cache->typioproc, range));

		pq_sendint32(buf, VARSIZE(range) - VARHDRSZ);
		pq_sendbytes(buf, VARDATA(range), VARSIZE(range) - VARHDRSZ);
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(buf));
}

/*
 * get_multirange_io_data: get cached information needed for multirange type I/O
 *
 * The multirange I/O functions need a bit more cached info than other multirange
 * functions, so they store a MultirangeIOData struct in fn_extra, not just a
 * pointer to a type cache entry.
 */
static MultirangeIOData *
get_multirange_io_data(FunctionCallInfo fcinfo, Oid mltrngtypid, IOFuncSelector func)
{
	MultirangeIOData *cache = (MultirangeIOData *) fcinfo->flinfo->fn_extra;

	if (cache == NULL || cache->typcache->type_id != mltrngtypid)
	{
		Oid			typiofunc;
		int16		typlen;
		bool		typbyval;
		char		typalign;
		char		typdelim;

		cache = (MultirangeIOData *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
														sizeof(MultirangeIOData));
		cache->typcache = lookup_type_cache(mltrngtypid, TYPECACHE_MULTIRANGE_INFO);
		if (cache->typcache->rngtype == NULL)
			elog(ERROR, "type %u is not a multirange type", mltrngtypid);

		/* get_type_io_data does more than we need, but is convenient */
		get_type_io_data(cache->typcache->rngtype->type_id,
						 func,
						 &typlen,
						 &typbyval,
						 &typalign,
						 &typdelim,
						 &cache->typioparam,
						 &typiofunc);

		if (!OidIsValid(typiofunc))
		{
			/* this could only happen for receive or send */
			if (func == IOFunc_receive)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("no binary input function available for type %s",
								format_type_be(cache->typcache->rngtype->type_id))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("no binary output function available for type %s",
								format_type_be(cache->typcache->rngtype->type_id))));
		}
		fmgr_info_cxt(typiofunc, &cache->typioproc,
					  fcinfo->flinfo->fn_mcxt);

		fcinfo->flinfo->fn_extra = (void *) cache;
	}

	return cache;
}

/*
 * Converts a list of arbitrary ranges into a list that is sorted and merged.
 * Changes the contents of `ranges`.
 *
 * Returns the number of slots actually used, which may be less than
 * input_range_count but never more.
 *
 * We assume that no input ranges are null, but empties are okay.
 */
static int32
multirange_canonicalize(TypeCacheEntry *rangetyp, int32 input_range_count,
						RangeType **ranges)
{
	RangeType  *lastRange = NULL;
	RangeType  *currentRange;
	int32		i;
	int32		output_range_count = 0;

	/* Sort the ranges so we can find the ones that overlap/meet. */
	qsort_arg(ranges, input_range_count, sizeof(RangeType *), range_compare,
			  rangetyp);

	/* Now merge where possible: */
	for (i = 0; i < input_range_count; i++)
	{
		currentRange = ranges[i];
		if (RangeIsEmpty(currentRange))
			continue;

		if (lastRange == NULL)
		{
			ranges[output_range_count++] = lastRange = currentRange;
			continue;
		}

		/*
		 * range_adjacent_internal gives true if *either* A meets B or B meets
		 * A, which is not quite want we want, but we rely on the sorting
		 * above to rule out B meets A ever happening.
		 */
		if (range_adjacent_internal(rangetyp, lastRange, currentRange))
		{
			/* The two ranges touch (without overlap), so merge them: */
			ranges[output_range_count - 1] = lastRange =
				range_union_internal(rangetyp, lastRange, currentRange, false);
		}
		else if (range_before_internal(rangetyp, lastRange, currentRange))
		{
			/* There's a gap, so make a new entry: */
			lastRange = ranges[output_range_count] = currentRange;
			output_range_count++;
		}
		else
		{
			/* They must overlap, so merge them: */
			ranges[output_range_count - 1] = lastRange =
				range_union_internal(rangetyp, lastRange, currentRange, true);
		}
	}

	return output_range_count;
}

/*
 *----------------------------------------------------------
 * SUPPORT FUNCTIONS
 *
 *	 These functions aren't in pg_proc, but are useful for
 *	 defining new generic multirange functions in C.
 *----------------------------------------------------------
 */

/*
 * multirange_get_typcache: get cached information about a multirange type
 *
 * This is for use by multirange-related functions that follow the convention
 * of using the fn_extra field as a pointer to the type cache entry for
 * the multirange type.  Functions that need to cache more information than
 * that must fend for themselves.
 */
TypeCacheEntry *
multirange_get_typcache(FunctionCallInfo fcinfo, Oid mltrngtypid)
{
	TypeCacheEntry *typcache = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;

	if (typcache == NULL ||
		typcache->type_id != mltrngtypid)
	{
		typcache = lookup_type_cache(mltrngtypid, TYPECACHE_MULTIRANGE_INFO);
		if (typcache->rngtype == NULL)
			elog(ERROR, "type %u is not a multirange type", mltrngtypid);
		fcinfo->flinfo->fn_extra = (void *) typcache;
	}

	return typcache;
}


/*
 * Estimate size occupied by serialized multirange.
 */
static Size
multirange_size_estimate(TypeCacheEntry *rangetyp, int32 range_count,
						 RangeType **ranges)
{
	char		elemalign = rangetyp->rngelemtype->typalign;
	Size		size;
	int32		i;

	/*
	 * Count space for MultirangeType struct, items and flags.
	 */
	size = att_align_nominal(sizeof(MultirangeType) +
							 Max(range_count - 1, 0) * sizeof(uint32) +
							 range_count * sizeof(uint8), elemalign);

	/* Count space for range bounds */
	for (i = 0; i < range_count; i++)
		size += att_align_nominal(VARSIZE(ranges[i]) -
								  sizeof(RangeType) -
								  sizeof(char), elemalign);

	return size;
}

/*
 * Write multirange data into pre-allocated space.
 */
static void
write_multirange_data(MultirangeType *multirange, TypeCacheEntry *rangetyp,
					  int32 range_count, RangeType **ranges)
{
	uint32	   *items;
	uint32		prev_offset = 0;
	uint8	   *flags;
	int32		i;
	Pointer		begin,
				ptr;
	char		elemalign = rangetyp->rngelemtype->typalign;

	items = MultirangeGetItemsPtr(multirange);
	flags = MultirangeGetFlagsPtr(multirange);
	ptr = begin = MultirangeGetBoundariesPtr(multirange, elemalign);
	for (i = 0; i < range_count; i++)
	{
		uint32		len;

		if (i > 0)
		{
			/*
			 * Every range, except the first one, has an item.  Every
			 * MULTIRANGE_ITEM_OFFSET_STRIDE item contains an offset, others
			 * contain lengths.
			 */
			items[i - 1] = ptr - begin;
			if ((i % MULTIRANGE_ITEM_OFFSET_STRIDE) != 0)
				items[i - 1] -= prev_offset;
			else
				items[i - 1] |= MULTIRANGE_ITEM_OFF_BIT;
			prev_offset = ptr - begin;
		}
		flags[i] = *((Pointer) ranges[i] + VARSIZE(ranges[i]) - sizeof(char));
		len = VARSIZE(ranges[i]) - sizeof(RangeType) - sizeof(char);
		memcpy(ptr, (Pointer) (ranges[i] + 1), len);
		ptr += att_align_nominal(len, elemalign);
	}
}


/*
 * This serializes the multirange from a list of non-null ranges.  It also
 * sorts the ranges and merges any that touch.  The ranges should already be
 * detoasted, and there should be no NULLs.  This should be used by most
 * callers.
 *
 * Note that we may change the `ranges` parameter (the pointers, but not
 * any already-existing RangeType contents).
 */
MultirangeType *
make_multirange(Oid mltrngtypoid, TypeCacheEntry *rangetyp, int32 range_count,
				RangeType **ranges)
{
	MultirangeType *multirange;
	Size		size;

	/* Sort and merge input ranges. */
	range_count = multirange_canonicalize(rangetyp, range_count, ranges);

	/* Note: zero-fill is required here, just as in heap tuples */
	size = multirange_size_estimate(rangetyp, range_count, ranges);
	multirange = palloc0(size);
	SET_VARSIZE(multirange, size);

	/* Now fill in the datum */
	multirange->multirangetypid = mltrngtypoid;
	multirange->rangeCount = range_count;

	write_multirange_data(multirange, rangetyp, range_count, ranges);

	return multirange;
}

/*
 * Get offset of bounds values of the i'th range in the multirange.
 */
static uint32
multirange_get_bounds_offset(const MultirangeType *multirange, int32 i)
{
	uint32	   *items = MultirangeGetItemsPtr(multirange);
	uint32		offset = 0;

	/*
	 * Summarize lengths till we meet an offset.
	 */
	while (i > 0)
	{
		offset += MULTIRANGE_ITEM_GET_OFFLEN(items[i - 1]);
		if (MULTIRANGE_ITEM_HAS_OFF(items[i - 1]))
			break;
		i--;
	}
	return offset;
}

/*
 * Fetch the i'th range from the multirange.
 */
RangeType *
multirange_get_range(TypeCacheEntry *rangetyp,
					 const MultirangeType *multirange, int i)
{
	uint32		offset;
	uint8		flags;
	Pointer		begin,
				ptr;
	int16		typlen = rangetyp->rngelemtype->typlen;
	char		typalign = rangetyp->rngelemtype->typalign;
	uint32		len;
	RangeType  *range;

	Assert(i < multirange->rangeCount);

	offset = multirange_get_bounds_offset(multirange, i);
	flags = MultirangeGetFlagsPtr(multirange)[i];
	ptr = begin = MultirangeGetBoundariesPtr(multirange, typalign) + offset;

	/*
	 * Calculate the size of bound values.  In principle, we could get offset
	 * of the next range bound values and calculate accordingly.  But range
	 * bound values are aligned, so we have to walk the values to get the
	 * exact size.
	 */
	if (RANGE_HAS_LBOUND(flags))
		ptr = (Pointer) att_addlength_pointer(ptr, typlen, ptr);
	if (RANGE_HAS_UBOUND(flags))
	{
		ptr = (Pointer) att_align_pointer(ptr, typalign, typlen, ptr);
		ptr = (Pointer) att_addlength_pointer(ptr, typlen, ptr);
	}
	len = (ptr - begin) + sizeof(RangeType) + sizeof(uint8);

	range = palloc0(len);
	SET_VARSIZE(range, len);
	range->rangetypid = rangetyp->type_id;

	memcpy(range + 1, begin, ptr - begin);
	*((uint8 *) (range + 1) + (ptr - begin)) = flags;

	return range;
}

/*
 * Fetch bounds from the i'th range of the multirange.  This is the shortcut for
 * doing the same thing as multirange_get_range() + range_deserialize(), but
 * performing fewer operations.
 */
void
multirange_get_bounds(TypeCacheEntry *rangetyp,
					  const MultirangeType *multirange,
					  uint32 i, RangeBound *lower, RangeBound *upper)
{
	uint32		offset;
	uint8		flags;
	Pointer		ptr;
	int16		typlen = rangetyp->rngelemtype->typlen;
	char		typalign = rangetyp->rngelemtype->typalign;
	bool		typbyval = rangetyp->rngelemtype->typbyval;
	Datum		lbound;
	Datum		ubound;

	Assert(i < multirange->rangeCount);

	offset = multirange_get_bounds_offset(multirange, i);
	flags = MultirangeGetFlagsPtr(multirange)[i];
	ptr = MultirangeGetBoundariesPtr(multirange, typalign) + offset;

	/* multirange can't contain empty ranges */
	Assert((flags & RANGE_EMPTY) == 0);

	/* fetch lower bound, if any */
	if (RANGE_HAS_LBOUND(flags))
	{
		/* att_align_pointer cannot be necessary here */
		lbound = fetch_att(ptr, typbyval, typlen);
		ptr = (Pointer) att_addlength_pointer(ptr, typlen, ptr);
	}
	else
		lbound = (Datum) 0;

	/* fetch upper bound, if any */
	if (RANGE_HAS_UBOUND(flags))
	{
		ptr = (Pointer) att_align_pointer(ptr, typalign, typlen, ptr);
		ubound = fetch_att(ptr, typbyval, typlen);
		/* no need for att_addlength_pointer */
	}
	else
		ubound = (Datum) 0;

	/* emit results */
	lower->val = lbound;
	lower->infinite = (flags & RANGE_LB_INF) != 0;
	lower->inclusive = (flags & RANGE_LB_INC) != 0;
	lower->lower = true;

	upper->val = ubound;
	upper->infinite = (flags & RANGE_UB_INF) != 0;
	upper->inclusive = (flags & RANGE_UB_INC) != 0;
	upper->lower = false;
}

/*
 * Construct union range from the multirange.
 */
RangeType *
multirange_get_union_range(TypeCacheEntry *rangetyp,
						   const MultirangeType *mr)
{
	RangeBound	lower,
				upper,
				tmp;

	if (MultirangeIsEmpty(mr))
		return make_empty_range(rangetyp);

	multirange_get_bounds(rangetyp, mr, 0, &lower, &tmp);
	multirange_get_bounds(rangetyp, mr, mr->rangeCount - 1, &tmp, &upper);

	return make_range(rangetyp, &lower, &upper, false);
}


/*
 * multirange_deserialize: deconstruct a multirange value
 *
 * NB: the given multirange object must be fully detoasted; it cannot have a
 * short varlena header.
 */
void
multirange_deserialize(TypeCacheEntry *rangetyp,
					   const MultirangeType *multirange, int32 *range_count,
					   RangeType ***ranges)
{
	*range_count = multirange->rangeCount;

	/* Convert each ShortRangeType into a RangeType */
	if (*range_count > 0)
	{
		int			i;

		*ranges = palloc(*range_count * sizeof(RangeType *));
		for (i = 0; i < *range_count; i++)
			(*ranges)[i] = multirange_get_range(rangetyp, multirange, i);
	}
	else
	{
		*ranges = NULL;
	}
}

MultirangeType *
make_empty_multirange(Oid mltrngtypoid, TypeCacheEntry *rangetyp)
{
	return make_multirange(mltrngtypoid, rangetyp, 0, NULL);
}

/*
 * Similar to range_overlaps_internal(), but takes range bounds instead of
 * ranges as arguments.
 */
static bool
range_bounds_overlaps(TypeCacheEntry *typcache,
					  RangeBound *lower1, RangeBound *upper1,
					  RangeBound *lower2, RangeBound *upper2)
{
	if (range_cmp_bounds(typcache, lower1, lower2) >= 0 &&
		range_cmp_bounds(typcache, lower1, upper2) <= 0)
		return true;

	if (range_cmp_bounds(typcache, lower2, lower1) >= 0 &&
		range_cmp_bounds(typcache, lower2, upper1) <= 0)
		return true;

	return false;
}

/*
 * Similar to range_contains_internal(), but takes range bounds instead of
 * ranges as arguments.
 */
static bool
range_bounds_contains(TypeCacheEntry *typcache,
					  RangeBound *lower1, RangeBound *upper1,
					  RangeBound *lower2, RangeBound *upper2)
{
	if (range_cmp_bounds(typcache, lower1, lower2) <= 0 &&
		range_cmp_bounds(typcache, upper1, upper2) >= 0)
		return true;

	return false;
}

/*
 * Check if the given key matches any range in multirange using binary search.
 * If the required range isn't found, that counts as a mismatch.  When the
 * required range is found, the comparison function can still report this as
 * either match or mismatch.  For instance, if we search for containment, we can
 * found a range, which is overlapping but not containing the key range, and
 * that would count as a mismatch.
 */
static bool
multirange_bsearch_match(TypeCacheEntry *typcache, const MultirangeType *mr,
						 void *key, multirange_bsearch_comparison cmp_func)
{
	uint32		l,
				u,
				idx;
	int			comparison;
	bool		match = false;

	l = 0;
	u = mr->rangeCount;
	while (l < u)
	{
		RangeBound	lower,
					upper;

		idx = (l + u) / 2;
		multirange_get_bounds(typcache, mr, idx, &lower, &upper);
		comparison = (*cmp_func) (typcache, &lower, &upper, key, &match);

		if (comparison < 0)
			u = idx;
		else if (comparison > 0)
			l = idx + 1;
		else
			return match;
	}

	return false;
}

/*
 *----------------------------------------------------------
 * GENERIC FUNCTIONS
 *----------------------------------------------------------
 */

/*
 * Construct multirange value from zero or more ranges.  Since this is a
 * variadic function we get passed an array.  The array must contain ranges
 * that match our return value, and there must be no NULLs.
 */
Datum
multirange_constructor2(PG_FUNCTION_ARGS)
{
	Oid			mltrngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	Oid			rngtypid;
	TypeCacheEntry *typcache;
	TypeCacheEntry *rangetyp;
	ArrayType  *rangeArray;
	int			range_count;
	Datum	   *elements;
	bool	   *nulls;
	RangeType **ranges;
	int			dims;
	int			i;

	typcache = multirange_get_typcache(fcinfo, mltrngtypid);
	rangetyp = typcache->rngtype;

	/*
	 * A no-arg invocation should call multirange_constructor0 instead, but
	 * returning an empty range is what that does.
	 */

	if (PG_NARGS() == 0)
		PG_RETURN_MULTIRANGE_P(make_multirange(mltrngtypid, rangetyp, 0, NULL));

	/*
	 * This check should be guaranteed by our signature, but let's do it just
	 * in case.
	 */

	if (PG_ARGISNULL(0))
		elog(ERROR,
			 "multirange values cannot contain null members");

	rangeArray = PG_GETARG_ARRAYTYPE_P(0);

	dims = ARR_NDIM(rangeArray);
	if (dims > 1)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("multiranges cannot be constructed from multidimensional arrays")));

	rngtypid = ARR_ELEMTYPE(rangeArray);
	if (rngtypid != rangetyp->type_id)
		elog(ERROR, "type %u does not match constructor type", rngtypid);

	/*
	 * Be careful: we can still be called with zero ranges, like this:
	 * `int4multirange(variadic '{}'::int4range[])
	 */
	if (dims == 0)
	{
		range_count = 0;
		ranges = NULL;
	}
	else
	{
		deconstruct_array(rangeArray, rngtypid, rangetyp->typlen, rangetyp->typbyval,
						  rangetyp->typalign, &elements, &nulls, &range_count);

		ranges = palloc0(range_count * sizeof(RangeType *));
		for (i = 0; i < range_count; i++)
		{
			if (nulls[i])
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("multirange values cannot contain null members")));

			/* make_multirange will do its own copy */
			ranges[i] = DatumGetRangeTypeP(elements[i]);
		}
	}

	PG_RETURN_MULTIRANGE_P(make_multirange(mltrngtypid, rangetyp, range_count, ranges));
}

/*
 * Construct multirange value from a single range.  It'd be nice if we could
 * just use multirange_constructor2 for this case, but we need a non-variadic
 * single-arg function to let us define a CAST from a range to its multirange.
 */
Datum
multirange_constructor1(PG_FUNCTION_ARGS)
{
	Oid			mltrngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	Oid			rngtypid;
	TypeCacheEntry *typcache;
	TypeCacheEntry *rangetyp;
	RangeType  *range;

	typcache = multirange_get_typcache(fcinfo, mltrngtypid);
	rangetyp = typcache->rngtype;

	/*
	 * This check should be guaranteed by our signature, but let's do it just
	 * in case.
	 */

	if (PG_ARGISNULL(0))
		elog(ERROR,
			 "multirange values cannot contain null members");

	range = PG_GETARG_RANGE_P(0);

	/* Make sure the range type matches. */
	rngtypid = RangeTypeGetOid(range);
	if (rngtypid != rangetyp->type_id)
		elog(ERROR, "type %u does not match constructor type", rngtypid);

	PG_RETURN_MULTIRANGE_P(make_multirange(mltrngtypid, rangetyp, 1, &range));
}

/*
 * Constructor just like multirange_constructor1, but opr_sanity gets angry
 * if the same internal function handles multiple functions with different arg
 * counts.
 */
Datum
multirange_constructor0(PG_FUNCTION_ARGS)
{
	Oid			mltrngtypid;
	TypeCacheEntry *typcache;
	TypeCacheEntry *rangetyp;

	/* This should always be called without arguments */
	if (PG_NARGS() != 0)
		elog(ERROR,
			 "niladic multirange constructor must not receive arguments");

	mltrngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	typcache = multirange_get_typcache(fcinfo, mltrngtypid);
	rangetyp = typcache->rngtype;

	PG_RETURN_MULTIRANGE_P(make_multirange(mltrngtypid, rangetyp, 0, NULL));
}


/* multirange, multirange -> multirange type functions */

/* multirange union */
Datum
multirange_union(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;
	int32		range_count1;
	int32		range_count2;
	int32		range_count3;
	RangeType **ranges1;
	RangeType **ranges2;
	RangeType **ranges3;

	if (MultirangeIsEmpty(mr1))
		PG_RETURN_MULTIRANGE_P(mr2);
	if (MultirangeIsEmpty(mr2))
		PG_RETURN_MULTIRANGE_P(mr1);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	multirange_deserialize(typcache->rngtype, mr1, &range_count1, &ranges1);
	multirange_deserialize(typcache->rngtype, mr2, &range_count2, &ranges2);

	range_count3 = range_count1 + range_count2;
	ranges3 = palloc0(range_count3 * sizeof(RangeType *));
	memcpy(ranges3, ranges1, range_count1 * sizeof(RangeType *));
	memcpy(ranges3 + range_count1, ranges2, range_count2 * sizeof(RangeType *));
	PG_RETURN_MULTIRANGE_P(make_multirange(typcache->type_id, typcache->rngtype,
										   range_count3, ranges3));
}

/* multirange minus */
Datum
multirange_minus(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	Oid			mltrngtypoid = MultirangeTypeGetOid(mr1);
	TypeCacheEntry *typcache;
	TypeCacheEntry *rangetyp;
	int32		range_count1;
	int32		range_count2;
	RangeType **ranges1;
	RangeType **ranges2;

	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);
	rangetyp = typcache->rngtype;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		PG_RETURN_MULTIRANGE_P(mr1);

	multirange_deserialize(typcache->rngtype, mr1, &range_count1, &ranges1);
	multirange_deserialize(typcache->rngtype, mr2, &range_count2, &ranges2);

	PG_RETURN_MULTIRANGE_P(multirange_minus_internal(mltrngtypoid,
													 rangetyp,
													 range_count1,
													 ranges1,
													 range_count2,
													 ranges2));
}

MultirangeType *
multirange_minus_internal(Oid mltrngtypoid, TypeCacheEntry *rangetyp,
						  int32 range_count1, RangeType **ranges1,
						  int32 range_count2, RangeType **ranges2)
{
	RangeType  *r1;
	RangeType  *r2;
	RangeType **ranges3;
	int32		range_count3;
	int32		i1;
	int32		i2;

	/*
	 * Worst case: every range in ranges1 makes a different cut to some range
	 * in ranges2.
	 */
	ranges3 = palloc0((range_count1 + range_count2) * sizeof(RangeType *));
	range_count3 = 0;

	/*
	 * For each range in mr1, keep subtracting until it's gone or the ranges
	 * in mr2 have passed it. After a subtraction we assign what's left back
	 * to r1. The parallel progress through mr1 and mr2 is similar to
	 * multirange_overlaps_multirange_internal.
	 */
	r2 = ranges2[0];
	for (i1 = 0, i2 = 0; i1 < range_count1; i1++)
	{
		r1 = ranges1[i1];

		/* Discard r2s while r2 << r1 */
		while (r2 != NULL && range_before_internal(rangetyp, r2, r1))
		{
			r2 = ++i2 >= range_count2 ? NULL : ranges2[i2];
		}

		while (r2 != NULL)
		{
			if (range_split_internal(rangetyp, r1, r2, &ranges3[range_count3], &r1))
			{
				/*
				 * If r2 takes a bite out of the middle of r1, we need two
				 * outputs
				 */
				range_count3++;
				r2 = ++i2 >= range_count2 ? NULL : ranges2[i2];
			}
			else if (range_overlaps_internal(rangetyp, r1, r2))
			{
				/*
				 * If r2 overlaps r1, replace r1 with r1 - r2.
				 */
				r1 = range_minus_internal(rangetyp, r1, r2);

				/*
				 * If r2 goes past r1, then we need to stay with it, in case
				 * it hits future r1s. Otherwise we need to keep r1, in case
				 * future r2s hit it. Since we already subtracted, there's no
				 * point in using the overright/overleft calls.
				 */
				if (RangeIsEmpty(r1) || range_before_internal(rangetyp, r1, r2))
					break;
				else
					r2 = ++i2 >= range_count2 ? NULL : ranges2[i2];
			}
			else
			{
				/*
				 * This and all future r2s are past r1, so keep them. Also
				 * assign whatever is left of r1 to the result.
				 */
				break;
			}
		}

		/*
		 * Nothing else can remove anything from r1, so keep it. Even if r1 is
		 * empty here, make_multirange will remove it.
		 */
		ranges3[range_count3++] = r1;
	}

	return make_multirange(mltrngtypoid, rangetyp, range_count3, ranges3);
}

/* multirange intersection */
Datum
multirange_intersect(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	Oid			mltrngtypoid = MultirangeTypeGetOid(mr1);
	TypeCacheEntry *typcache;
	TypeCacheEntry *rangetyp;
	int32		range_count1;
	int32		range_count2;
	RangeType **ranges1;
	RangeType **ranges2;

	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);
	rangetyp = typcache->rngtype;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		PG_RETURN_MULTIRANGE_P(make_empty_multirange(mltrngtypoid, rangetyp));

	multirange_deserialize(rangetyp, mr1, &range_count1, &ranges1);
	multirange_deserialize(rangetyp, mr2, &range_count2, &ranges2);

	PG_RETURN_MULTIRANGE_P(multirange_intersect_internal(mltrngtypoid,
														 rangetyp,
														 range_count1,
														 ranges1,
														 range_count2,
														 ranges2));
}

MultirangeType *
multirange_intersect_internal(Oid mltrngtypoid, TypeCacheEntry *rangetyp,
							  int32 range_count1, RangeType **ranges1,
							  int32 range_count2, RangeType **ranges2)
{
	RangeType  *r1;
	RangeType  *r2;
	RangeType **ranges3;
	int32		range_count3;
	int32		i1;
	int32		i2;

	if (range_count1 == 0 || range_count2 == 0)
		return make_multirange(mltrngtypoid, rangetyp, 0, NULL);

	/*-----------------------------------------------
	 * Worst case is a stitching pattern like this:
	 *
	 * mr1: --- --- --- ---
	 * mr2:   --- --- ---
	 * mr3:   - - - - - -
	 *
	 * That seems to be range_count1 + range_count2 - 1,
	 * but one extra won't hurt.
	 *-----------------------------------------------
	 */
	ranges3 = palloc0((range_count1 + range_count2) * sizeof(RangeType *));
	range_count3 = 0;

	/*
	 * For each range in mr1, keep intersecting until the ranges in mr2 have
	 * passed it. The parallel progress through mr1 and mr2 is similar to
	 * multirange_minus_multirange_internal, but we don't have to assign back
	 * to r1.
	 */
	r2 = ranges2[0];
	for (i1 = 0, i2 = 0; i1 < range_count1; i1++)
	{
		r1 = ranges1[i1];

		/* Discard r2s while r2 << r1 */
		while (r2 != NULL && range_before_internal(rangetyp, r2, r1))
		{
			r2 = ++i2 >= range_count2 ? NULL : ranges2[i2];
		}

		while (r2 != NULL)
		{
			if (range_overlaps_internal(rangetyp, r1, r2))
			{
				/* Keep the overlapping part */
				ranges3[range_count3++] = range_intersect_internal(rangetyp, r1, r2);

				/* If we "used up" all of r2, go to the next one... */
				if (range_overleft_internal(rangetyp, r2, r1))
					r2 = ++i2 >= range_count2 ? NULL : ranges2[i2];

				/* ...otherwise go to the next r1 */
				else
					break;
			}
			else
				/* We're past r1, so move to the next one */
				break;
		}

		/* If we're out of r2s, there can be no more intersections */
		if (r2 == NULL)
			break;
	}

	return make_multirange(mltrngtypoid, rangetyp, range_count3, ranges3);
}

/*
 * range_agg_transfn: combine adjacent/overlapping ranges.
 *
 * All we do here is gather the input ranges into an array
 * so that the finalfn can sort and combine them.
 */
Datum
range_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggContext;
	Oid			rngtypoid;
	ArrayBuildState *state;

	if (!AggCheckCallContext(fcinfo, &aggContext))
		elog(ERROR, "range_agg_transfn called in non-aggregate context");

	rngtypoid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	if (!type_is_range(rngtypoid))
		elog(ERROR, "range_agg must be called with a range");

	if (PG_ARGISNULL(0))
		state = initArrayResult(rngtypoid, aggContext, false);
	else
		state = (ArrayBuildState *) PG_GETARG_POINTER(0);

	/* skip NULLs */
	if (!PG_ARGISNULL(1))
		accumArrayResult(state, PG_GETARG_DATUM(1), false, rngtypoid, aggContext);

	PG_RETURN_POINTER(state);
}

/*
 * range_agg_finalfn: use our internal array to merge touching ranges.
 *
 * Shared by range_agg_finalfn(anyrange) and
 * multirange_agg_finalfn(anymultirange).
 */
Datum
range_agg_finalfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggContext;
	Oid			mltrngtypoid;
	TypeCacheEntry *typcache;
	ArrayBuildState *state;
	int32		range_count;
	RangeType **ranges;
	int			i;

	if (!AggCheckCallContext(fcinfo, &aggContext))
		elog(ERROR, "range_agg_finalfn called in non-aggregate context");

	state = PG_ARGISNULL(0) ? NULL : (ArrayBuildState *) PG_GETARG_POINTER(0);
	if (state == NULL)
		/* This shouldn't be possible, but just in case.... */
		PG_RETURN_NULL();

	/* Also return NULL if we had zero inputs, like other aggregates */
	range_count = state->nelems;
	if (range_count == 0)
		PG_RETURN_NULL();

	mltrngtypoid = get_fn_expr_rettype(fcinfo->flinfo);
	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);

	ranges = palloc0(range_count * sizeof(RangeType *));
	for (i = 0; i < range_count; i++)
		ranges[i] = DatumGetRangeTypeP(state->dvalues[i]);

	PG_RETURN_MULTIRANGE_P(make_multirange(mltrngtypoid, typcache->rngtype, range_count, ranges));
}

/*
 * multirange_agg_transfn: combine adjacent/overlapping multiranges.
 *
 * All we do here is gather the input multiranges' ranges into an array so
 * that the finalfn can sort and combine them.
 */
Datum
multirange_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggContext;
	Oid			mltrngtypoid;
	TypeCacheEntry *typcache;
	TypeCacheEntry *rngtypcache;
	ArrayBuildState *state;

	if (!AggCheckCallContext(fcinfo, &aggContext))
		elog(ERROR, "multirange_agg_transfn called in non-aggregate context");

	mltrngtypoid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	if (!type_is_multirange(mltrngtypoid))
		elog(ERROR, "range_agg must be called with a multirange");

	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);
	rngtypcache = typcache->rngtype;

	if (PG_ARGISNULL(0))
		state = initArrayResult(rngtypcache->type_id, aggContext, false);
	else
		state = (ArrayBuildState *) PG_GETARG_POINTER(0);

	/* skip NULLs */
	if (!PG_ARGISNULL(1))
	{
		MultirangeType *current;
		int32		range_count;
		RangeType **ranges;

		current = PG_GETARG_MULTIRANGE_P(1);
		multirange_deserialize(rngtypcache, current, &range_count, &ranges);
		if (range_count == 0)
		{
			/*
			 * Add an empty range so we get an empty result (not a null
			 * result).
			 */
			accumArrayResult(state,
							 RangeTypePGetDatum(make_empty_range(rngtypcache)),
							 false, rngtypcache->type_id, aggContext);
		}
		else
		{
			for (int32 i = 0; i < range_count; i++)
				accumArrayResult(state, RangeTypePGetDatum(ranges[i]), false, rngtypcache->type_id, aggContext);
		}
	}

	PG_RETURN_POINTER(state);
}

Datum
multirange_intersect_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggContext;
	Oid			mltrngtypoid;
	TypeCacheEntry *typcache;
	MultirangeType *result;
	MultirangeType *current;
	int32		range_count1;
	int32		range_count2;
	RangeType **ranges1;
	RangeType **ranges2;

	if (!AggCheckCallContext(fcinfo, &aggContext))
		elog(ERROR, "multirange_intersect_agg_transfn called in non-aggregate context");

	mltrngtypoid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	if (!type_is_multirange(mltrngtypoid))
		elog(ERROR, "range_intersect_agg must be called with a multirange");

	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);

	/* strictness ensures these are non-null */
	result = PG_GETARG_MULTIRANGE_P(0);
	current = PG_GETARG_MULTIRANGE_P(1);

	multirange_deserialize(typcache->rngtype, result, &range_count1, &ranges1);
	multirange_deserialize(typcache->rngtype, current, &range_count2, &ranges2);

	result = multirange_intersect_internal(mltrngtypoid,
										   typcache->rngtype,
										   range_count1,
										   ranges1,
										   range_count2,
										   ranges2);
	PG_RETURN_RANGE_P(result);
}


/* multirange -> element type functions */

/* extract lower bound value */
Datum
multirange_lower(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_NULL();

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	multirange_get_bounds(typcache->rngtype, mr, 0,
						  &lower, &upper);

	if (!lower.infinite)
		PG_RETURN_DATUM(lower.val);
	else
		PG_RETURN_NULL();
}

/* extract upper bound value */
Datum
multirange_upper(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_NULL();

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	multirange_get_bounds(typcache->rngtype, mr, mr->rangeCount - 1,
						  &lower, &upper);

	if (!upper.infinite)
		PG_RETURN_DATUM(upper.val);
	else
		PG_RETURN_NULL();
}


/* multirange -> bool functions */

/* is multirange empty? */
Datum
multirange_empty(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);

	PG_RETURN_BOOL(MultirangeIsEmpty(mr));
}

/* is lower bound inclusive? */
Datum
multirange_lower_inc(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	multirange_get_bounds(typcache->rngtype, mr, 0,
						  &lower, &upper);

	PG_RETURN_BOOL(lower.inclusive);
}

/* is upper bound inclusive? */
Datum
multirange_upper_inc(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	multirange_get_bounds(typcache->rngtype, mr, mr->rangeCount - 1,
						  &lower, &upper);

	PG_RETURN_BOOL(upper.inclusive);
}

/* is lower bound infinite? */
Datum
multirange_lower_inf(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	multirange_get_bounds(typcache->rngtype, mr, 0,
						  &lower, &upper);

	PG_RETURN_BOOL(lower.infinite);
}

/* is upper bound infinite? */
Datum
multirange_upper_inf(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	multirange_get_bounds(typcache->rngtype, mr, mr->rangeCount - 1,
						  &lower, &upper);

	PG_RETURN_BOOL(upper.infinite);
}



/* multirange, element -> bool functions */

/* contains? */
Datum
multirange_contains_elem(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	Datum		val = PG_GETARG_DATUM(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(multirange_contains_elem_internal(typcache->rngtype, mr, val));
}

/* contained by? */
Datum
elem_contained_by_multirange(PG_FUNCTION_ARGS)
{
	Datum		val = PG_GETARG_DATUM(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(multirange_contains_elem_internal(typcache->rngtype, mr, val));
}

/*
 * Comparison function for checking if any range of multirange contains given
 * key element using binary search.
 */
static int
multirange_elem_bsearch_comparison(TypeCacheEntry *typcache,
								   RangeBound *lower, RangeBound *upper,
								   void *key, bool *match)
{
	Datum		val = *((Datum *) key);
	int			cmp;

	if (!lower->infinite)
	{
		cmp = DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo,
											  typcache->rng_collation,
											  lower->val, val));
		if (cmp > 0 || (cmp == 0 && !lower->inclusive))
			return -1;
	}

	if (!upper->infinite)
	{
		cmp = DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo,
											  typcache->rng_collation,
											  upper->val, val));
		if (cmp < 0 || (cmp == 0 && !upper->inclusive))
			return 1;
	}

	*match = true;
	return 0;
}

/*
 * Test whether multirange mr contains a specific element value.
 */
bool
multirange_contains_elem_internal(TypeCacheEntry *rangetyp,
								  const MultirangeType *mr, Datum val)
{
	if (MultirangeIsEmpty(mr))
		return false;

	return multirange_bsearch_match(rangetyp, mr, &val,
									multirange_elem_bsearch_comparison);
}

/* multirange, range -> bool functions */

/* contains? */
Datum
multirange_contains_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(multirange_contains_range_internal(typcache->rngtype, mr, r));
}

Datum
range_contains_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_contains_multirange_internal(typcache->rngtype, r, mr));
}

/* contained by? */
Datum
range_contained_by_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(multirange_contains_range_internal(typcache->rngtype, mr, r));
}

Datum
multirange_contained_by_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_contains_multirange_internal(typcache->rngtype, r, mr));
}

/*
 * Comparison function for checking if any range of multirange contains given
 * key range using binary search.
 */
static int
multirange_range_contains_bsearch_comparison(TypeCacheEntry *typcache,
											 RangeBound *lower, RangeBound *upper,
											 void *key, bool *match)
{
	RangeBound *keyLower = (RangeBound *) key;
	RangeBound *keyUpper = (RangeBound *) key + 1;

	/* Check if key range is strictly in the left or in the right */
	if (range_cmp_bounds(typcache, keyUpper, lower) < 0)
		return -1;
	if (range_cmp_bounds(typcache, keyLower, upper) > 0)
		return 1;

	/*
	 * At this point we found overlapping range.  But we have to check if it
	 * really contains the key range.  Anyway, we have to stop our search
	 * here, because multirange contains only non-overlapping ranges.
	 */
	*match = range_bounds_contains(typcache, lower, upper, keyLower, keyUpper);

	return 0;
}

/*
 * Test whether multirange mr contains a specific range r.
 */
bool
multirange_contains_range_internal(TypeCacheEntry *rangetyp,
								   const MultirangeType *mr,
								   const RangeType *r)
{
	RangeBound	bounds[2];
	bool		empty;

	/*
	 * Every multirange contains an infinite number of empty ranges, even an
	 * empty one.
	 */
	if (RangeIsEmpty(r))
		return true;

	if (MultirangeIsEmpty(mr))
		return false;

	range_deserialize(rangetyp, r, &bounds[0], &bounds[1], &empty);
	Assert(!empty);

	return multirange_bsearch_match(rangetyp, mr, bounds,
									multirange_range_contains_bsearch_comparison);
}

/*
 * Test whether range r contains a multirange mr.
 */
bool
range_contains_multirange_internal(TypeCacheEntry *rangetyp,
								   const RangeType *r,
								   const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2,
				tmp;
	bool		empty;

	/*
	 * Every range contains an infinite number of empty multiranges, even an
	 * empty one.
	 */
	if (MultirangeIsEmpty(mr))
		return true;

	if (RangeIsEmpty(r))
		return false;

	/* Range contains multirange iff it contains its union range. */
	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);
	multirange_get_bounds(rangetyp, mr, 0, &lower2, &tmp);
	multirange_get_bounds(rangetyp, mr, mr->rangeCount - 1, &tmp, &upper2);

	return range_bounds_contains(rangetyp, &lower1, &upper1, &lower2, &upper2);
}


/* multirange, multirange -> bool functions */

/* equality (internal version) */
bool
multirange_eq_internal(TypeCacheEntry *rangetyp,
					   const MultirangeType *mr1,
					   const MultirangeType *mr2)
{
	int32		range_count_1;
	int32		range_count_2;
	int32		i;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	/* Different types should be prevented by ANYMULTIRANGE matching rules */
	if (MultirangeTypeGetOid(mr1) != MultirangeTypeGetOid(mr2))
		elog(ERROR, "multirange types do not match");

	range_count_1 = mr1->rangeCount;
	range_count_2 = mr2->rangeCount;

	if (range_count_1 != range_count_2)
		return false;

	for (i = 0; i < range_count_1; i++)
	{
		multirange_get_bounds(rangetyp, mr1, i, &lower1, &upper1);
		multirange_get_bounds(rangetyp, mr2, i, &lower2, &upper2);

		if (range_cmp_bounds(rangetyp, &lower1, &lower2) != 0 ||
			range_cmp_bounds(rangetyp, &upper1, &upper2) != 0)
			return false;
	}

	return true;
}

/* equality */
Datum
multirange_eq(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_eq_internal(typcache->rngtype, mr1, mr2));
}

/* inequality (internal version) */
bool
multirange_ne_internal(TypeCacheEntry *rangetyp,
					   const MultirangeType *mr1,
					   const MultirangeType *mr2)
{
	return (!multirange_eq_internal(rangetyp, mr1, mr2));
}

/* inequality */
Datum
multirange_ne(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_ne_internal(typcache->rngtype, mr1, mr2));
}

/* overlaps? */
Datum
range_overlaps_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_overlaps_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_overlaps_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_overlaps_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_overlaps_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_overlaps_multirange_internal(typcache->rngtype, mr1, mr2));
}

/*
 * Comparison function for checking if any range of multirange overlaps given
 * key range using binary search.
 */
static int
multirange_range_overlaps_bsearch_comparison(TypeCacheEntry *typcache,
											 RangeBound *lower, RangeBound *upper,
											 void *key, bool *match)
{
	RangeBound *keyLower = (RangeBound *) key;
	RangeBound *keyUpper = (RangeBound *) key + 1;

	if (range_cmp_bounds(typcache, keyUpper, lower) < 0)
		return -1;
	if (range_cmp_bounds(typcache, keyLower, upper) > 0)
		return 1;

	*match = true;
	return 0;
}

bool
range_overlaps_multirange_internal(TypeCacheEntry *rangetyp,
								   const RangeType *r,
								   const MultirangeType *mr)
{
	RangeBound	bounds[2];
	bool		empty;

	/*
	 * Empties never overlap, even with empties. (This seems strange since
	 * they *do* contain each other, but we want to follow how ranges work.)
	 */
	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return false;

	range_deserialize(rangetyp, r, &bounds[0], &bounds[1], &empty);
	Assert(!empty);

	return multirange_bsearch_match(rangetyp, mr, bounds,
									multirange_range_overlaps_bsearch_comparison);
}

bool
multirange_overlaps_multirange_internal(TypeCacheEntry *rangetyp,
										const MultirangeType *mr1,
										const MultirangeType *mr2)
{
	int32		range_count1;
	int32		range_count2;
	int32		i1;
	int32		i2;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	/*
	 * Empties never overlap, even with empties. (This seems strange since
	 * they *do* contain each other, but we want to follow how ranges work.)
	 */
	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		return false;

	range_count1 = mr1->rangeCount;
	range_count2 = mr2->rangeCount;

	/*
	 * Every range in mr1 gets a chance to overlap with the ranges in mr2, but
	 * we can use their ordering to avoid O(n^2). This is similar to
	 * range_overlaps_multirange where r1 : r2 :: mrr : r, but there if we
	 * don't find an overlap with r we're done, and here if we don't find an
	 * overlap with r2 we try the next r2.
	 */
	i1 = 0;
	multirange_get_bounds(rangetyp, mr1, i1, &lower1, &upper1);
	for (i1 = 0, i2 = 0; i2 < range_count2; i2++)
	{
		multirange_get_bounds(rangetyp, mr2, i2, &lower2, &upper2);

		/* Discard r1s while r1 << r2 */
		while (range_cmp_bounds(rangetyp, &upper1, &lower2) < 0)
		{
			if (++i1 >= range_count1)
				return false;
			multirange_get_bounds(rangetyp, mr1, i1, &lower1, &upper1);
		}

		/*
		 * If r1 && r2, we're done, otherwise we failed to find an overlap for
		 * r2, so go to the next one.
		 */
		if (range_bounds_overlaps(rangetyp, &lower1, &upper1, &lower2, &upper2))
			return true;
	}

	/* We looked through all of mr2 without finding an overlap */
	return false;
}

/* does not extend to right of? */
bool
range_overleft_multirange_internal(TypeCacheEntry *rangetyp,
								   const RangeType *r,
								   const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);


	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);
	multirange_get_bounds(rangetyp, mr, mr->rangeCount - 1,
						  &lower2, &upper2);

	PG_RETURN_BOOL(range_cmp_bounds(rangetyp, &upper1, &upper2) <= 0);
}

Datum
range_overleft_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_overleft_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_overleft_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;

	if (MultirangeIsEmpty(mr) || RangeIsEmpty(r))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	multirange_get_bounds(typcache->rngtype, mr, mr->rangeCount - 1,
						  &lower1, &upper1);
	range_deserialize(typcache->rngtype, r, &lower2, &upper2, &empty);
	Assert(!empty);

	PG_RETURN_BOOL(range_cmp_bounds(typcache->rngtype, &upper1, &upper2) <= 0);
}

Datum
multirange_overleft_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	multirange_get_bounds(typcache->rngtype, mr1, mr1->rangeCount - 1,
						  &lower1, &upper1);
	multirange_get_bounds(typcache->rngtype, mr2, mr2->rangeCount - 1,
						  &lower2, &upper2);

	PG_RETURN_BOOL(range_cmp_bounds(typcache->rngtype, &upper1, &upper2) <= 0);
}

/* does not extend to left of? */
bool
range_overright_multirange_internal(TypeCacheEntry *rangetyp,
									const RangeType *r,
									const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);

	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);
	multirange_get_bounds(rangetyp, mr, 0, &lower2, &upper2);

	return (range_cmp_bounds(rangetyp, &lower1, &lower2) >= 0);
}

Datum
range_overright_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_overright_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_overright_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;

	if (MultirangeIsEmpty(mr) || RangeIsEmpty(r))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	multirange_get_bounds(typcache->rngtype, mr, 0, &lower1, &upper1);
	range_deserialize(typcache->rngtype, r, &lower2, &upper2, &empty);
	Assert(!empty);

	PG_RETURN_BOOL(range_cmp_bounds(typcache->rngtype, &lower1, &lower2) >= 0);
}

Datum
multirange_overright_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	multirange_get_bounds(typcache->rngtype, mr1, 0, &lower1, &upper1);
	multirange_get_bounds(typcache->rngtype, mr2, 0, &lower2, &upper2);

	PG_RETURN_BOOL(range_cmp_bounds(typcache->rngtype, &lower1, &lower2) >= 0);
}

/* contains? */
Datum
multirange_contains_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_contains_multirange_internal(typcache->rngtype, mr1, mr2));
}

/* contained by? */
Datum
multirange_contained_by_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_contains_multirange_internal(typcache->rngtype, mr2, mr1));
}

/*
 * Test whether multirange mr1 contains every range from another multirange mr2.
 */
bool
multirange_contains_multirange_internal(TypeCacheEntry *rangetyp,
										const MultirangeType *mr1,
										const MultirangeType *mr2)
{
	int32		range_count1 = mr1->rangeCount;
	int32		range_count2 = mr2->rangeCount;
	int			i1,
				i2;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	/*
	 * We follow the same logic for empties as ranges: - an empty multirange
	 * contains an empty range/multirange. - an empty multirange can't contain
	 * any other range/multirange. - an empty multirange is contained by any
	 * other range/multirange.
	 */

	if (range_count2 == 0)
		return true;
	if (range_count1 == 0)
		return false;

	/*
	 * Every range in mr2 must be contained by some range in mr1. To avoid
	 * O(n^2) we walk through both ranges in tandem.
	 */
	i1 = 0;
	multirange_get_bounds(rangetyp, mr1, i1, &lower1, &upper1);
	for (i2 = 0; i2 < range_count2; i2++)
	{
		multirange_get_bounds(rangetyp, mr2, i2, &lower2, &upper2);

		/* Discard r1s while r1 << r2 */
		while (range_cmp_bounds(rangetyp, &upper1, &lower2) < 0)
		{
			if (++i1 >= range_count1)
				return false;
			multirange_get_bounds(rangetyp, mr1, i1, &lower1, &upper1);
		}

		/*
		 * If r1 @> r2, go to the next r2, otherwise return false (since every
		 * r1[n] and r1[n+1] must have a gap). Note this will give weird
		 * answers if you don't canonicalize, e.g. with a custom
		 * int2multirange {[1,1], [2,2]} there is a "gap". But that is
		 * consistent with other range operators, e.g. '[1,1]'::int2range -|-
		 * '[2,2]'::int2range is false.
		 */
		if (!range_bounds_contains(rangetyp, &lower1, &upper1,
								   &lower2, &upper2))
			return false;
	}

	/* All ranges in mr2 are satisfied */
	return true;
}

/* strictly left of? */
Datum
range_before_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_before_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_before_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_after_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_before_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_before_multirange_internal(typcache->rngtype, mr1, mr2));
}

/* strictly right of? */
Datum
range_after_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_after_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_after_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_before_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_after_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_before_multirange_internal(typcache->rngtype, mr2, mr1));
}

/* strictly left of? (internal version) */
bool
range_before_multirange_internal(TypeCacheEntry *rangetyp,
								 const RangeType *r,
								 const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return false;

	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);

	multirange_get_bounds(rangetyp, mr, 0, &lower2, &upper2);

	return (range_cmp_bounds(rangetyp, &upper1, &lower2) < 0);
}

bool
multirange_before_multirange_internal(TypeCacheEntry *rangetyp,
									  const MultirangeType *mr1,
									  const MultirangeType *mr2)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		return false;

	multirange_get_bounds(rangetyp, mr1, mr1->rangeCount - 1,
						  &lower1, &upper1);
	multirange_get_bounds(rangetyp, mr2, 0,
						  &lower2, &upper2);

	return (range_cmp_bounds(rangetyp, &upper1, &lower2) < 0);
}

/* strictly right of? (internal version) */
bool
range_after_multirange_internal(TypeCacheEntry *rangetyp,
								const RangeType *r,
								const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;
	int32		range_count;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return false;

	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);

	range_count = mr->rangeCount;
	multirange_get_bounds(rangetyp, mr, range_count - 1,
						  &lower2, &upper2);

	return (range_cmp_bounds(rangetyp, &lower1, &upper2) > 0);
}

bool
range_adjacent_multirange_internal(TypeCacheEntry *rangetyp,
								   const RangeType *r,
								   const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;
	int32		range_count;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return false;

	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);

	range_count = mr->rangeCount;
	multirange_get_bounds(rangetyp, mr, 0,
						  &lower2, &upper2);

	if (bounds_adjacent(rangetyp, upper1, lower2))
		return true;

	if (range_count > 1)
		multirange_get_bounds(rangetyp, mr, range_count - 1,
							  &lower2, &upper2);

	if (bounds_adjacent(rangetyp, upper2, lower1))
		return true;

	return false;
}

/* adjacent to? */
Datum
range_adjacent_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_adjacent_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_adjacent_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return false;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_adjacent_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_adjacent_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;
	int32		range_count1;
	int32		range_count2;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		return false;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	range_count1 = mr1->rangeCount;
	range_count2 = mr2->rangeCount;
	multirange_get_bounds(typcache->rngtype, mr1, range_count1 - 1,
						  &lower1, &upper1);
	multirange_get_bounds(typcache->rngtype, mr2, 0,
						  &lower2, &upper2);
	if (bounds_adjacent(typcache->rngtype, upper1, lower2))
		PG_RETURN_BOOL(true);

	if (range_count1 > 1)
		multirange_get_bounds(typcache->rngtype, mr1, 0,
							  &lower1, &upper1);
	if (range_count2 > 1)
		multirange_get_bounds(typcache->rngtype, mr2, range_count2 - 1,
							  &lower2, &upper2);
	if (bounds_adjacent(typcache->rngtype, upper2, lower1))
		PG_RETURN_BOOL(true);
	PG_RETURN_BOOL(false);
}

/* Btree support */

/* btree comparator */
Datum
multirange_cmp(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	int32		range_count_1;
	int32		range_count_2;
	int32		range_count_max;
	int32		i;
	TypeCacheEntry *typcache;
	int			cmp = 0;		/* If both are empty we'll use this. */

	/* Different types should be prevented by ANYMULTIRANGE matching rules */
	if (MultirangeTypeGetOid(mr1) != MultirangeTypeGetOid(mr2))
		elog(ERROR, "multirange types do not match");

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	range_count_1 = mr1->rangeCount;
	range_count_2 = mr2->rangeCount;

	/* Loop over source data */
	range_count_max = Max(range_count_1, range_count_2);
	for (i = 0; i < range_count_max; i++)
	{
		RangeBound	lower1,
					upper1,
					lower2,
					upper2;

		/*
		 * If one multirange is shorter, it's as if it had empty ranges at the
		 * end to extend its length. An empty range compares earlier than any
		 * other range, so the shorter multirange comes before the longer.
		 * This is the same behavior as in other types, e.g. in strings 'aaa'
		 * < 'aaaaaa'.
		 */
		if (i >= range_count_1)
		{
			cmp = -1;
			break;
		}
		if (i >= range_count_2)
		{
			cmp = 1;
			break;
		}

		multirange_get_bounds(typcache->rngtype, mr1, i, &lower1, &upper1);
		multirange_get_bounds(typcache->rngtype, mr2, i, &lower2, &upper2);

		cmp = range_cmp_bounds(typcache->rngtype, &lower1, &lower2);
		if (cmp == 0)
			cmp = range_cmp_bounds(typcache->rngtype, &upper1, &upper2);
		if (cmp != 0)
			break;
	}

	PG_FREE_IF_COPY(mr1, 0);
	PG_FREE_IF_COPY(mr2, 1);

	PG_RETURN_INT32(cmp);
}

/* inequality operators using the multirange_cmp function */
Datum
multirange_lt(PG_FUNCTION_ARGS)
{
	int			cmp = multirange_cmp(fcinfo);

	PG_RETURN_BOOL(cmp < 0);
}

Datum
multirange_le(PG_FUNCTION_ARGS)
{
	int			cmp = multirange_cmp(fcinfo);

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
multirange_ge(PG_FUNCTION_ARGS)
{
	int			cmp = multirange_cmp(fcinfo);

	PG_RETURN_BOOL(cmp >= 0);
}

Datum
multirange_gt(PG_FUNCTION_ARGS)
{
	int			cmp = multirange_cmp(fcinfo);

	PG_RETURN_BOOL(cmp > 0);
}

/* multirange -> range functions */

/* Find the smallest range that includes everything in the multirange */
Datum
range_merge_from_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	Oid			mltrngtypoid = MultirangeTypeGetOid(mr);
	TypeCacheEntry *typcache;
	RangeType  *result;

	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);

	if (MultirangeIsEmpty(mr))
	{
		result = make_empty_range(typcache->rngtype);
	}
	else if (mr->rangeCount == 1)
	{
		result = multirange_get_range(typcache->rngtype, mr, 0);
	}
	else
	{
		RangeBound	firstLower,
					firstUpper,
					lastLower,
					lastUpper;

		multirange_get_bounds(typcache->rngtype, mr, 0,
							  &firstLower, &firstUpper);
		multirange_get_bounds(typcache->rngtype, mr, mr->rangeCount - 1,
							  &lastLower, &lastUpper);

		result = make_range(typcache->rngtype, &firstLower, &lastUpper, false);
	}

	PG_RETURN_RANGE_P(result);
}

/* Turn multirange into a set of ranges */
Datum
multirange_unnest(PG_FUNCTION_ARGS)
{
	typedef struct
	{
		MultirangeType *mr;
		TypeCacheEntry *typcache;
		int			index;
	} multirange_unnest_fctx;

	FuncCallContext *funcctx;
	multirange_unnest_fctx *fctx;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MultirangeType *mr;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * Get the multirange value and detoast if needed.  We can't do this
		 * earlier because if we have to detoast, we want the detoasted copy
		 * to be in multi_call_memory_ctx, so it will go away when we're done
		 * and not before.  (If no detoast happens, we assume the originally
		 * passed multirange will stick around till then.)
		 */
		mr = PG_GETARG_MULTIRANGE_P(0);

		/* allocate memory for user context */
		fctx = (multirange_unnest_fctx *) palloc(sizeof(multirange_unnest_fctx));

		/* initialize state */
		fctx->mr = mr;
		fctx->index = 0;
		fctx->typcache = lookup_type_cache(MultirangeTypeGetOid(mr),
										   TYPECACHE_MULTIRANGE_INFO);

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	if (fctx->index < fctx->mr->rangeCount)
	{
		RangeType  *range;

		range = multirange_get_range(fctx->typcache->rngtype,
									 fctx->mr,
									 fctx->index);
		fctx->index++;

		SRF_RETURN_NEXT(funcctx, RangeTypePGetDatum(range));
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

/* Hash support */

/* hash a multirange value */
Datum
hash_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	uint32		result = 1;
	TypeCacheEntry *typcache,
			   *scache;
	int32		range_count,
				i;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	scache = typcache->rngtype->rngelemtype;
	if (!OidIsValid(scache->hash_proc_finfo.fn_oid))
	{
		scache = lookup_type_cache(scache->type_id,
								   TYPECACHE_HASH_PROC_FINFO);
		if (!OidIsValid(scache->hash_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s",
							format_type_be(scache->type_id))));
	}

	range_count = mr->rangeCount;
	for (i = 0; i < range_count; i++)
	{
		RangeBound	lower,
					upper;
		uint8		flags = MultirangeGetFlagsPtr(mr)[i];
		uint32		lower_hash;
		uint32		upper_hash;
		uint32		range_hash;

		multirange_get_bounds(typcache->rngtype, mr, i, &lower, &upper);

		if (RANGE_HAS_LBOUND(flags))
			lower_hash = DatumGetUInt32(FunctionCall1Coll(&scache->hash_proc_finfo,
														  typcache->rngtype->rng_collation,
														  lower.val));
		else
			lower_hash = 0;

		if (RANGE_HAS_UBOUND(flags))
			upper_hash = DatumGetUInt32(FunctionCall1Coll(&scache->hash_proc_finfo,
														  typcache->rngtype->rng_collation,
														  upper.val));
		else
			upper_hash = 0;

		/* Merge hashes of flags and bounds */
		range_hash = hash_uint32((uint32) flags);
		range_hash ^= lower_hash;
		range_hash = pg_rotate_left32(range_hash, 1);
		range_hash ^= upper_hash;

		/*
		 * Use the same approach as hash_array to combine the individual
		 * elements' hash values:
		 */
		result = (result << 5) - result + range_hash;
	}

	PG_FREE_IF_COPY(mr, 0);

	PG_RETURN_UINT32(result);
}

/*
 * Returns 64-bit value by hashing a value to a 64-bit value, with a seed.
 * Otherwise, similar to hash_multirange.
 */
Datum
hash_multirange_extended(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	Datum		seed = PG_GETARG_DATUM(1);
	uint64		result = 1;
	TypeCacheEntry *typcache,
			   *scache;
	int32		range_count,
				i;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	scache = typcache->rngtype->rngelemtype;
	if (!OidIsValid(scache->hash_extended_proc_finfo.fn_oid))
	{
		scache = lookup_type_cache(scache->type_id,
								   TYPECACHE_HASH_EXTENDED_PROC_FINFO);
		if (!OidIsValid(scache->hash_extended_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s",
							format_type_be(scache->type_id))));
	}

	range_count = mr->rangeCount;
	for (i = 0; i < range_count; i++)
	{
		RangeBound	lower,
					upper;
		uint8		flags = MultirangeGetFlagsPtr(mr)[i];
		uint64		lower_hash;
		uint64		upper_hash;
		uint64		range_hash;

		multirange_get_bounds(typcache->rngtype, mr, i, &lower, &upper);

		if (RANGE_HAS_LBOUND(flags))
			lower_hash = DatumGetUInt64(FunctionCall2Coll(&scache->hash_extended_proc_finfo,
														  typcache->rngtype->rng_collation,
														  lower.val,
														  seed));
		else
			lower_hash = 0;

		if (RANGE_HAS_UBOUND(flags))
			upper_hash = DatumGetUInt64(FunctionCall2Coll(&scache->hash_extended_proc_finfo,
														  typcache->rngtype->rng_collation,
														  upper.val,
														  seed));
		else
			upper_hash = 0;

		/* Merge hashes of flags and bounds */
		range_hash = DatumGetUInt64(hash_uint32_extended((uint32) flags,
														 DatumGetInt64(seed)));
		range_hash ^= lower_hash;
		range_hash = ROTATE_HIGH_AND_LOW_32BITS(range_hash);
		range_hash ^= upper_hash;

		/*
		 * Use the same approach as hash_array to combine the individual
		 * elements' hash values:
		 */
		result = (result << 5) - result + range_hash;
	}

	PG_FREE_IF_COPY(mr, 0);

	PG_RETURN_UINT64(result);
}
