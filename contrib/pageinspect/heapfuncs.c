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
 * Copyright (c) 2007-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pageinspect/heapfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"


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
					 errmsg("illegal character '%c' in t_bits string", str[off])));

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
				 (errmsg("must be superuser to use raw page functions"))));

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
			HeapTupleHeader		tuphdr;
			bytea			   *tuple_data_bytea;
			int					tuple_data_len;

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
					int	bits_len =
						((tuphdr->t_infomask2 & HEAP_NATTS_MASK) / 8 + 1) * 8;

					values[11] = CStringGetTextDatum(
								 bits_to_text(tuphdr->t_bits, bits_len));
				}
				else
					nulls[11] = true;

				if (tuphdr->t_infomask & HEAP_HASOID)
					values[12] = HeapTupleHeaderGetOid(tuphdr);
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
 * elements accordindly. This is a reimplementation of nocachegetattr()
 * in heaptuple.c simplified for educational purposes.
 */
static Datum
tuple_data_split_internal(Oid relid, char *tupdata,
				 uint16 tupdata_len, uint16 t_infomask,
				 uint16 t_infomask2, bits8 *t_bits,
				 bool do_detoast)
{
	ArrayBuildState	   *raw_attrs;
	int 				nattrs;
	int					i;
	int					off = 0;
	Relation			rel;
	TupleDesc			tupdesc;

	/* Get tuple descriptor from relation OID */
	rel = relation_open(relid, NoLock);
	tupdesc = CreateTupleDescCopyConstr(rel->rd_att);
	relation_close(rel, NoLock);

	raw_attrs = initArrayResult(BYTEAOID, CurrentMemoryContext, false);
	nattrs = tupdesc->natts;

	if (nattrs < (t_infomask2 & HEAP_NATTS_MASK))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("number of attributes in tuple header is greater than number of attributes in tuple descriptor")));

	for (i = 0; i < nattrs; i++)
	{
		Form_pg_attribute	attr;
		bool				is_null;
		bytea			   *attr_data = NULL;

		attr = tupdesc->attrs[i];
		is_null = (t_infomask & HEAP_HASNULL) && att_isnull(i, t_bits);

		/*
		 * Tuple header can specify less attributes than tuple descriptor
		 * as ALTER TABLE ADD COLUMN without DEFAULT keyword does not
		 * actually change tuples in pages, so attributes with numbers greater
		 * than (t_infomask2 & HEAP_NATTS_MASK) should be treated as NULL.
		 */
		if (i >= (t_infomask2 & HEAP_NATTS_MASK))
			is_null = true;

		if (!is_null)
		{
			int		len;

			if (attr->attlen == -1)
			{
				off = att_align_pointer(off, tupdesc->attrs[i]->attalign, -1,
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
				off = att_align_nominal(off, tupdesc->attrs[i]->attalign);
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

			off = att_addlength_pointer(off, tupdesc->attrs[i]->attlen,
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
	Oid				relid;
	bytea		   *raw_data;
	uint16			t_infomask;
	uint16			t_infomask2;
	char		   *t_bits_str;
	bool			do_detoast = false;
	bits8		   *t_bits = NULL;
	Datum			res;

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
		int		bits_str_len;
		int		bits_len;

		bits_len = (t_infomask2 & HEAP_NATTS_MASK) / 8 + 1;
		if (!t_bits_str)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("argument of t_bits is null, but it is expected to be null and %i character long",
							bits_len * 8)));

		bits_str_len = strlen(t_bits_str);
		if ((bits_str_len % 8) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("length of t_bits is not a multiple of eight")));

		if (bits_len * 8 != bits_str_len)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("unexpected length of t_bits %u, expected %i",
							bits_str_len, bits_len * 8)));

		/* do the conversion */
		t_bits = text_to_bits(t_bits_str, bits_str_len);
	}
	else
	{
		if (t_bits_str)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("t_bits string is expected to be NULL, but instead it is %lu bytes length",
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
