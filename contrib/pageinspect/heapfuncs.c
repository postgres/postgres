/*-------------------------------------------------------------------------
 *
 * heapfuncs.c
 *	  Functions to investigate heap pages
 *
 * We check the input to these functions for corrupt pointers etc. that
 * might cause crashes, but at the same time we try to print out as much
 * information as possible, even if it's nonsense. That's because if a
 * page is corrupt, we don't know why and how exactly it is corrupt, so we
 * let the user to judge it.
 *
 * These functions are restricted to superusers for the fear of introducing
 * security holes if the input checking isn't as water-tight as it should.
 * You'd need to be superuser to obtain a raw page image anyway, so
 * there's hardly any use case for using these without superuser-rights
 * anyway.
 *
 * Copyright (c) 2007-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/contrib/pageinspect/heapfuncs.c,v 1.6 2009/01/01 17:23:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "miscadmin.h"

Datum		heap_page_items(PG_FUNCTION_ARGS);


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
}	heap_page_items_state;

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
		Datum		values[13];
		bool		nulls[13];
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
			lp_len >= sizeof(HeapTupleHeader) &&
			lp_offset == MAXALIGN(lp_offset) &&
			lp_offset + lp_len <= raw_page_size)
		{
			HeapTupleHeader tuphdr;
			int			bits_len;

			/* Extract information from the tuple header */

			tuphdr = (HeapTupleHeader) PageGetItem(page, id);

			values[4] = UInt32GetDatum(HeapTupleHeaderGetXmin(tuphdr));
			values[5] = UInt32GetDatum(HeapTupleHeaderGetXmax(tuphdr));
			values[6] = UInt32GetDatum(HeapTupleHeaderGetRawCommandId(tuphdr)); /* shared with xvac */
			values[7] = PointerGetDatum(&tuphdr->t_ctid);
			values[8] = UInt16GetDatum(tuphdr->t_infomask2);
			values[9] = UInt16GetDatum(tuphdr->t_infomask);
			values[10] = UInt8GetDatum(tuphdr->t_hoff);

			/*
			 * We already checked that the item as is completely within the
			 * raw page passed to us, with the length given in the line
			 * pointer.. Let's check that t_hoff doesn't point over lp_len,
			 * before using it to access t_bits and oid.
			 */
			if (tuphdr->t_hoff >= sizeof(HeapTupleHeader) &&
				tuphdr->t_hoff <= lp_len)
			{
				if (tuphdr->t_infomask & HEAP_HASNULL)
				{
					bits_len = tuphdr->t_hoff -
						(((char *) tuphdr->t_bits) -((char *) tuphdr));

					values[11] = CStringGetTextDatum(
						bits_to_text(tuphdr->t_bits, bits_len * 8));
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

			for (i = 4; i <= 12; i++)
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
