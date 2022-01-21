/*-------------------------------------------------------------------------
 *
 * heapfuncs.c
 *	  Functions to investigate heap pages
 *
 * We check the input to these functions for corrupt pointers etc. that
 * might cause crashes, but at the same time we try to print out as much
 * information as possible, even if it's nonsense. That's because if a
 * page is corrupt, we don't know why and how exactly it is corrupt, so we
 * let the user judge it.
 *
 * These functions are restricted to superusers for the fear of introducing
 * security holes if the input checking isn't as water-tight as it should be.
 * You'd need to be superuser to obtain a raw page image anyway, so
 * there's hardly any use case for using these without superuser-rights
 * anyway.
 *
 * Copyright (c) 2007-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pageinspect/heapfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pageinspect.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"

/*
 * It's not supported to create tuples with oids anymore, but when pg_upgrade
 * was used to upgrade from an older version, tuples might still have an
 * oid. Seems worthwhile to display that.
 */
#define HeapTupleHeaderGetOidOld(tup) \
( \
	((tup)->t_infomask & HEAP_HASOID_OLD) ? \
	   *((Oid *) ((char *)(tup) + (tup)->t_hoff - sizeof(Oid))) \
	: \
		InvalidOid \
)


/*
 * bits_to_text
 *
 * Converts a bits8-array of 'len' bits to a human-readable
 * c-string representation.
 */
static char *
bits_to_text(bits8 *bits, int len)
{
	int			i;
	char	   *str;

	str = palloc(len + 1);

	for (i = 0; i < len; i++)
		str[i] = (bits[(i / 8)] & (1 << (i % 8))) ? '1' : '0';

	str[i] = '\0';

	return str;
}


/*
 * text_to_bits
 *
 * Converts a c-string representation of bits into a bits8-array. This is
 * the reverse operation of previous routine.
 */
static bits8 *
text_to_bits(char *str, int len)
{
	bits8	   *bits;
	int			off = 0;
	char		byte = 0;

	bits = palloc(len + 1);

	while (off < len)
	{
		if (off % 8 == 0)
			byte = 0;

		if ((str[off] == '0') || (str[off] == '1'))
			byte = byte | ((str[off] - '0') << off % 8);
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid character \"%.*s\" in t_bits string",
							pg_mblen(str + off), str + off)));

		if (off % 8 == 7)
			bits[off / 8] = byte;

		off++;
	}

	return bits;
}

/*
 * heap_page_items
 *
 * Allows inspection of line pointers and tuple headers of a heap page.
 */
PG_FUNCTION_INFO_V1(heap_page_items);

typedef struct heap_page_items_state
{
	TupleDesc	tupd;
	Page		page;
	uint16		offset;
} heap_page_items_state;

Datum
heap_page_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	heap_page_items_state *inter_call_data = NULL;
	FuncCallContext *fctx;
	int			raw_page_size;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	raw_page_size = VARSIZE(raw_page) - VARHDRSZ;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext mctx;

		if (raw_page_size < SizeOfPageHeaderData)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("input page too small (%d bytes)", raw_page_size)));

		fctx = SRF_FIRSTCALL_INIT();
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		inter_call_data = palloc(sizeof(heap_page_items_state));

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		inter_call_data->tupd = tupdesc;

		inter_call_data->offset = FirstOffsetNumber;
		inter_call_data->page = VARDATA(raw_page);

		fctx->max_calls = PageGetMaxOffsetNumber(inter_call_data->page);
		fctx->user_fctx = inter_call_data;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	inter_call_data = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		Page		page = inter_call_data->page;
		HeapTuple	resultTuple;
		Datum		result;
		ItemId		id;
		Datum		values[14];
		bool		nulls[14];
		uint16		lp_offset;
		uint16		lp_flags;
		uint16		lp_len;

		memset(nulls, 0, sizeof(nulls));

		/* Extract information from the line pointer */

		id = PageGetItemId(page, inter_call_data->offset);

		lp_offset = ItemIdGetOffset(id);
		lp_flags = ItemIdGetFlags(id);
		lp_len = ItemIdGetLength(id);

		values[0] = UInt16GetDatum(inter_call_data->offset);
		values[1] = UInt16GetDatum(lp_offset);
		values[2] = UInt16GetDatum(lp_flags);
		values[3] = UInt16GetDatum(lp_len);

		/*
		 * We do just enough validity checking to make sure we don't reference
		 * data outside the page passed to us. The page could be corrupt in
		 * many other ways, but at least we won't crash.
		 */
		if (ItemIdHasStorage(id) &&
			lp_len >= MinHeapTupleSize &&
			lp_offset == MAXALIGN(lp_offset) &&
			lp_offset + lp_len <= raw_page_size)
		{
			HeapTupleHeader tuphdr;
			bytea	   *tuple_data_bytea;
			int			tuple_data_len;

			/* Extract information from the tuple header */

			tuphdr = (HeapTupleHeader) PageGetItem(page, id);

			values[4] = UInt32GetDatum(HeapTupleHeaderGetRawXmin(tuphdr));
			values[5] = UInt32GetDatum(HeapTupleHeaderGetRawXmax(tuphdr));
			/* shared with xvac */
			values[6] = UInt32GetDatum(HeapTupleHeaderGetRawCommandId(tuphdr));
			values[7] = PointerGetDatum(&tuphdr->t_ctid);
			values[8] = UInt32GetDatum(tuphdr->t_infomask2);
			values[9] = UInt32GetDatum(tuphdr->t_infomask);
			values[10] = UInt8GetDatum(tuphdr->t_hoff);

			/* Copy raw tuple data into bytea attribute */
			tuple_data_len = lp_len - tuphdr->t_hoff;
			tuple_data_bytea = (bytea *) palloc(tuple_data_len + VARHDRSZ);
			SET_VARSIZE(tuple_data_bytea, tuple_data_len + VARHDRSZ);
			memcpy(VARDATA(tuple_data_bytea), (char *) tuphdr + tuphdr->t_hoff,
				   tuple_data_len);
			values[13] = PointerGetDatum(tuple_data_bytea);

			/*
			 * We already checked that the item is completely within the raw
			 * page passed to us, with the length given in the line pointer.
			 * Let's check that t_hoff doesn't point over lp_len, before using
			 * it to access t_bits and oid.
			 */
			if (tuphdr->t_hoff >= SizeofHeapTupleHeader &&
				tuphdr->t_hoff <= lp_len &&
				tuphdr->t_hoff == MAXALIGN(tuphdr->t_hoff))
			{
				if (tuphdr->t_infomask & HEAP_HASNULL)
				{
					int			bits_len;

					bits_len =
						BITMAPLEN(HeapTupleHeaderGetNatts(tuphdr)) * BITS_PER_BYTE;
					values[11] = CStringGetTextDatum(bits_to_text(tuphdr->t_bits, bits_len));
				}
				else
					nulls[11] = true;

				if (tuphdr->t_infomask & HEAP_HASOID_OLD)
					values[12] = HeapTupleHeaderGetOidOld(tuphdr);
				else
					nulls[12] = true;
			}
			else
			{
				nulls[11] = true;
				nulls[12] = true;
			}
		}
		else
		{
			/*
			 * The line pointer is not used, or it's invalid. Set the rest of
			 * the fields to NULL
			 */
			int			i;

			for (i = 4; i <= 13; i++)
				nulls[i] = true;
		}

		/* Build and return the result tuple. */
		resultTuple = heap_form_tuple(inter_call_data->tupd, values, nulls);
		result = HeapTupleGetDatum(resultTuple);

		inter_call_data->offset++;

		SRF_RETURN_NEXT(fctx, result);
	}
	else
		SRF_RETURN_DONE(fctx);
}

/*
 * tuple_data_split_internal
 *
 * Split raw tuple data taken directly from a page into an array of bytea
 * elements. This routine does a lookup on NULL values and creates array
 * elements accordingly. This is a reimplementation of nocachegetattr()
 * in heaptuple.c simplified for educational purposes.
 */
static Datum
tuple_data_split_internal(Oid relid, char *tupdata,
						  uint16 tupdata_len, uint16 t_infomask,
						  uint16 t_infomask2, bits8 *t_bits,
						  bool do_detoast)
{
	ArrayBuildState *raw_attrs;
	int			nattrs;
	int			i;
	int			off = 0;
	Relation	rel;
	TupleDesc	tupdesc;

	/* Get tuple descriptor from relation OID */
	rel = relation_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	raw_attrs = initArrayResult(BYTEAOID, CurrentMemoryContext, false);
	nattrs = tupdesc->natts;

	if (rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("only heap AM is supported")));

	if (nattrs < (t_infomask2 & HEAP_NATTS_MASK))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("number of attributes in tuple header is greater than number of attributes in tuple descriptor")));

	for (i = 0; i < nattrs; i++)
	{
		Form_pg_attribute attr;
		bool		is_null;
		bytea	   *attr_data = NULL;

		attr = TupleDescAttr(tupdesc, i);

		/*
		 * Tuple header can specify fewer attributes than tuple descriptor as
		 * ALTER TABLE ADD COLUMN without DEFAULT keyword does not actually
		 * change tuples in pages, so attributes with numbers greater than
		 * (t_infomask2 & HEAP_NATTS_MASK) should be treated as NULL.
		 */
		if (i >= (t_infomask2 & HEAP_NATTS_MASK))
			is_null = true;
		else
			is_null = (t_infomask & HEAP_HASNULL) && att_isnull(i, t_bits);

		if (!is_null)
		{
			int			len;

			if (attr->attlen == -1)
			{
				off = att_align_pointer(off, attr->attalign, -1,
										tupdata + off);

				/*
				 * As VARSIZE_ANY throws an exception if it can't properly
				 * detect the type of external storage in macros VARTAG_SIZE,
				 * this check is repeated to have a nicer error handling.
				 */
				if (VARATT_IS_EXTERNAL(tupdata + off) &&
					!VARATT_IS_EXTERNAL_ONDISK(tupdata + off) &&
					!VARATT_IS_EXTERNAL_INDIRECT(tupdata + off))
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("first byte of varlena attribute is incorrect for attribute %d", i)));

				len = VARSIZE_ANY(tupdata + off);
			}
			else
			{
				off = att_align_nominal(off, attr->attalign);
				len = attr->attlen;
			}

			if (tupdata_len < off + len)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("unexpected end of tuple data")));

			if (attr->attlen == -1 && do_detoast)
				attr_data = DatumGetByteaPCopy(tupdata + off);
			else
			{
				attr_data = (bytea *) palloc(len + VARHDRSZ);
				SET_VARSIZE(attr_data, len + VARHDRSZ);
				memcpy(VARDATA(attr_data), tupdata + off, len);
			}

			off = att_addlength_pointer(off, attr->attlen,
										tupdata + off);
		}

		raw_attrs = accumArrayResult(raw_attrs, PointerGetDatum(attr_data),
									 is_null, BYTEAOID, CurrentMemoryContext);
		if (attr_data)
			pfree(attr_data);
	}

	if (tupdata_len != off)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("end of tuple reached without looking at all its data")));

	relation_close(rel, AccessShareLock);

	return makeArrayResult(raw_attrs, CurrentMemoryContext);
}

/*
 * tuple_data_split
 *
 * Split raw tuple data taken directly from page into distinct elements
 * taking into account null values.
 */
PG_FUNCTION_INFO_V1(tuple_data_split);

Datum
tuple_data_split(PG_FUNCTION_ARGS)
{
	Oid			relid;
	bytea	   *raw_data;
	uint16		t_infomask;
	uint16		t_infomask2;
	char	   *t_bits_str;
	bool		do_detoast = false;
	bits8	   *t_bits = NULL;
	Datum		res;

	relid = PG_GETARG_OID(0);
	raw_data = PG_ARGISNULL(1) ? NULL : PG_GETARG_BYTEA_P(1);
	t_infomask = PG_GETARG_INT16(2);
	t_infomask2 = PG_GETARG_INT16(3);
	t_bits_str = PG_ARGISNULL(4) ? NULL :
		text_to_cstring(PG_GETARG_TEXT_PP(4));

	if (PG_NARGS() >= 6)
		do_detoast = PG_GETARG_BOOL(5);

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	if (!raw_data)
		PG_RETURN_NULL();

	/*
	 * Convert t_bits string back to the bits8 array as represented in the
	 * tuple header.
	 */
	if (t_infomask & HEAP_HASNULL)
	{
		size_t		bits_str_len;
		size_t		bits_len;

		bits_len = BITMAPLEN(t_infomask2 & HEAP_NATTS_MASK) * BITS_PER_BYTE;
		if (!t_bits_str)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("t_bits string must not be NULL")));

		bits_str_len = strlen(t_bits_str);
		if (bits_len != bits_str_len)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("unexpected length of t_bits string: %zu, expected %zu",
							bits_str_len, bits_len)));

		/* do the conversion */
		t_bits = text_to_bits(t_bits_str, bits_str_len);
	}
	else
	{
		if (t_bits_str)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("t_bits string is expected to be NULL, but instead it is %zu bytes long",
							strlen(t_bits_str))));
	}

	/* Split tuple data */
	res = tuple_data_split_internal(relid, (char *) raw_data + VARHDRSZ,
									VARSIZE(raw_data) - VARHDRSZ,
									t_infomask, t_infomask2, t_bits,
									do_detoast);

	if (t_bits)
		pfree(t_bits);

	PG_RETURN_ARRAYTYPE_P(res);
}

/*
 * heap_tuple_infomask_flags
 *
 * Decode into a human-readable format t_infomask and t_infomask2 associated
 * to a tuple.  All the flags are described in access/htup_details.h.
 */
PG_FUNCTION_INFO_V1(heap_tuple_infomask_flags);

Datum
heap_tuple_infomask_flags(PG_FUNCTION_ARGS)
{
#define HEAP_TUPLE_INFOMASK_COLS 2
	Datum		values[HEAP_TUPLE_INFOMASK_COLS];
	bool		nulls[HEAP_TUPLE_INFOMASK_COLS];
	uint16		t_infomask = PG_GETARG_INT16(0);
	uint16		t_infomask2 = PG_GETARG_INT16(1);
	int			cnt = 0;
	ArrayType  *a;
	int			bitcnt;
	Datum	   *flags;
	TupleDesc	tupdesc;
	HeapTuple	tuple;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	bitcnt = pg_popcount((const char *) &t_infomask, sizeof(uint16)) +
		pg_popcount((const char *) &t_infomask2, sizeof(uint16));

	/* Initialize values and NULL flags arrays */
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* If no flags, return a set of empty arrays */
	if (bitcnt <= 0)
	{
		values[0] = PointerGetDatum(construct_empty_array(TEXTOID));
		values[1] = PointerGetDatum(construct_empty_array(TEXTOID));
		tuple = heap_form_tuple(tupdesc, values, nulls);
		PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
	}

	/* build set of raw flags */
	flags = (Datum *) palloc0(sizeof(Datum) * bitcnt);

	/* decode t_infomask */
	if ((t_infomask & HEAP_HASNULL) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_HASNULL");
	if ((t_infomask & HEAP_HASVARWIDTH) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_HASVARWIDTH");
	if ((t_infomask & HEAP_HASEXTERNAL) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_HASEXTERNAL");
	if ((t_infomask & HEAP_HASOID_OLD) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_HASOID_OLD");
	if ((t_infomask & HEAP_XMAX_KEYSHR_LOCK) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_XMAX_KEYSHR_LOCK");
	if ((t_infomask & HEAP_COMBOCID) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_COMBOCID");
	if ((t_infomask & HEAP_XMAX_EXCL_LOCK) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_XMAX_EXCL_LOCK");
	if ((t_infomask & HEAP_XMAX_LOCK_ONLY) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_XMAX_LOCK_ONLY");
	if ((t_infomask & HEAP_XMIN_COMMITTED) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_XMIN_COMMITTED");
	if ((t_infomask & HEAP_XMIN_INVALID) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_XMIN_INVALID");
	if ((t_infomask & HEAP_XMAX_COMMITTED) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_XMAX_COMMITTED");
	if ((t_infomask & HEAP_XMAX_INVALID) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_XMAX_INVALID");
	if ((t_infomask & HEAP_XMAX_IS_MULTI) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_XMAX_IS_MULTI");
	if ((t_infomask & HEAP_UPDATED) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_UPDATED");
	if ((t_infomask & HEAP_MOVED_OFF) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_MOVED_OFF");
	if ((t_infomask & HEAP_MOVED_IN) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_MOVED_IN");

	/* decode t_infomask2 */
	if ((t_infomask2 & HEAP_KEYS_UPDATED) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_KEYS_UPDATED");
	if ((t_infomask2 & HEAP_HOT_UPDATED) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_HOT_UPDATED");
	if ((t_infomask2 & HEAP_ONLY_TUPLE) != 0)
		flags[cnt++] = CStringGetTextDatum("HEAP_ONLY_TUPLE");

	/* build value */
	Assert(cnt <= bitcnt);
	a = construct_array(flags, cnt, TEXTOID, -1, false, TYPALIGN_INT);
	values[0] = PointerGetDatum(a);

	/*
	 * Build set of combined flags.  Use the same array as previously, this
	 * keeps the code simple.
	 */
	cnt = 0;
	MemSet(flags, 0, sizeof(Datum) * bitcnt);

	/* decode combined masks of t_infomask */
	if ((t_infomask & HEAP_XMAX_SHR_LOCK) == HEAP_XMAX_SHR_LOCK)
		flags[cnt++] = CStringGetTextDatum("HEAP_XMAX_SHR_LOCK");
	if ((t_infomask & HEAP_XMIN_FROZEN) == HEAP_XMIN_FROZEN)
		flags[cnt++] = CStringGetTextDatum("HEAP_XMIN_FROZEN");
	if ((t_infomask & HEAP_MOVED) == HEAP_MOVED)
		flags[cnt++] = CStringGetTextDatum("HEAP_MOVED");

	/* Build an empty array if there are no combined flags */
	if (cnt == 0)
		a = construct_empty_array(TEXTOID);
	else
		a = construct_array(flags, cnt, TEXTOID, -1, false, TYPALIGN_INT);
	pfree(flags);
	values[1] = PointerGetDatum(a);

	/* Returns the record as Datum */
	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
