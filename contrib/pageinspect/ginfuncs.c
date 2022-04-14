/*
 * ginfuncs.c
 *		Functions to investigate the content of GIN indexes
 *
 * Copyright (c) 2014-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pageinspect/ginfuncs.c
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/gin_private.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pageinspect.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#define DatumGetItemPointer(X)	 ((ItemPointer) DatumGetPointer(X))
#define ItemPointerGetDatum(X)	 PointerGetDatum(X)


PG_FUNCTION_INFO_V1(gin_metapage_info);
PG_FUNCTION_INFO_V1(gin_page_opaque_info);
PG_FUNCTION_INFO_V1(gin_leafpage_items);


Datum
gin_metapage_info(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	TupleDesc	tupdesc;
	Page		page;
	GinPageOpaque opaq;
	GinMetaPageData *metadata;
	HeapTuple	resultTuple;
	Datum		values[10];
	bool		nulls[10];

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	page = get_page_from_raw(raw_page);

	if (PageIsNew(page))
		PG_RETURN_NULL();

	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GinPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input page is not a valid GIN metapage"),
				 errdetail("Expected special size %d, got %d.",
						   (int) MAXALIGN(sizeof(GinPageOpaqueData)),
						   (int) PageGetSpecialSize(page))));

	opaq = (GinPageOpaque) PageGetSpecialPointer(page);
	if (opaq->flags != GIN_META)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input page is not a GIN metapage"),
				 errdetail("Flags %04X, expected %04X",
						   opaq->flags, GIN_META)));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	metadata = GinPageGetMeta(page);

	memset(nulls, 0, sizeof(nulls));

	values[0] = Int64GetDatum(metadata->head);
	values[1] = Int64GetDatum(metadata->tail);
	values[2] = Int32GetDatum(metadata->tailFreeSize);
	values[3] = Int64GetDatum(metadata->nPendingPages);
	values[4] = Int64GetDatum(metadata->nPendingHeapTuples);

	/* statistics, updated by VACUUM */
	values[5] = Int64GetDatum(metadata->nTotalPages);
	values[6] = Int64GetDatum(metadata->nEntryPages);
	values[7] = Int64GetDatum(metadata->nDataPages);
	values[8] = Int64GetDatum(metadata->nEntries);

	values[9] = Int32GetDatum(metadata->ginVersion);

	/* Build and return the result tuple. */
	resultTuple = heap_form_tuple(tupdesc, values, nulls);

	return HeapTupleGetDatum(resultTuple);
}


Datum
gin_page_opaque_info(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	TupleDesc	tupdesc;
	Page		page;
	GinPageOpaque opaq;
	HeapTuple	resultTuple;
	Datum		values[3];
	bool		nulls[3];
	Datum		flags[16];
	int			nflags = 0;
	uint16		flagbits;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	page = get_page_from_raw(raw_page);

	if (PageIsNew(page))
		PG_RETURN_NULL();

	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GinPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input page is not a valid GIN data leaf page"),
				 errdetail("Expected special size %d, got %d.",
						   (int) MAXALIGN(sizeof(GinPageOpaqueData)),
						   (int) PageGetSpecialSize(page))));

	opaq = (GinPageOpaque) PageGetSpecialPointer(page);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Convert the flags bitmask to an array of human-readable names */
	flagbits = opaq->flags;
	if (flagbits & GIN_DATA)
		flags[nflags++] = CStringGetTextDatum("data");
	if (flagbits & GIN_LEAF)
		flags[nflags++] = CStringGetTextDatum("leaf");
	if (flagbits & GIN_DELETED)
		flags[nflags++] = CStringGetTextDatum("deleted");
	if (flagbits & GIN_META)
		flags[nflags++] = CStringGetTextDatum("meta");
	if (flagbits & GIN_LIST)
		flags[nflags++] = CStringGetTextDatum("list");
	if (flagbits & GIN_LIST_FULLROW)
		flags[nflags++] = CStringGetTextDatum("list_fullrow");
	if (flagbits & GIN_INCOMPLETE_SPLIT)
		flags[nflags++] = CStringGetTextDatum("incomplete_split");
	if (flagbits & GIN_COMPRESSED)
		flags[nflags++] = CStringGetTextDatum("compressed");
	flagbits &= ~(GIN_DATA | GIN_LEAF | GIN_DELETED | GIN_META | GIN_LIST |
				  GIN_LIST_FULLROW | GIN_INCOMPLETE_SPLIT | GIN_COMPRESSED);
	if (flagbits)
	{
		/* any flags we don't recognize are printed in hex */
		flags[nflags++] = DirectFunctionCall1(to_hex32, Int32GetDatum(flagbits));
	}

	memset(nulls, 0, sizeof(nulls));

	values[0] = Int64GetDatum(opaq->rightlink);
	values[1] = Int32GetDatum(opaq->maxoff);
	values[2] = PointerGetDatum(construct_array(flags, nflags,
												TEXTOID,
												-1, false, TYPALIGN_INT));

	/* Build and return the result tuple. */
	resultTuple = heap_form_tuple(tupdesc, values, nulls);

	return HeapTupleGetDatum(resultTuple);
}

typedef struct gin_leafpage_items_state
{
	TupleDesc	tupd;
	GinPostingList *seg;
	GinPostingList *lastseg;
} gin_leafpage_items_state;

Datum
gin_leafpage_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	FuncCallContext *fctx;
	gin_leafpage_items_state *inter_call_data;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext mctx;
		Page		page;
		GinPageOpaque opaq;

		fctx = SRF_FIRSTCALL_INIT();
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		page = get_page_from_raw(raw_page);

		if (PageIsNew(page))
		{
			MemoryContextSwitchTo(mctx);
			PG_RETURN_NULL();
		}

		if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GinPageOpaqueData)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("input page is not a valid GIN data leaf page"),
					 errdetail("Expected special size %d, got %d.",
							   (int) MAXALIGN(sizeof(GinPageOpaqueData)),
							   (int) PageGetSpecialSize(page))));

		opaq = (GinPageOpaque) PageGetSpecialPointer(page);
		if (opaq->flags != (GIN_DATA | GIN_LEAF | GIN_COMPRESSED))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("input page is not a compressed GIN data leaf page"),
					 errdetail("Flags %04X, expected %04X",
							   opaq->flags,
							   (GIN_DATA | GIN_LEAF | GIN_COMPRESSED))));

		inter_call_data = palloc(sizeof(gin_leafpage_items_state));

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		inter_call_data->tupd = tupdesc;

		inter_call_data->seg = GinDataLeafPageGetPostingList(page);
		inter_call_data->lastseg = (GinPostingList *)
			(((char *) inter_call_data->seg) +
			 GinDataLeafPageGetPostingListSize(page));

		fctx->user_fctx = inter_call_data;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	inter_call_data = fctx->user_fctx;

	if (inter_call_data->seg != inter_call_data->lastseg)
	{
		GinPostingList *cur = inter_call_data->seg;
		HeapTuple	resultTuple;
		Datum		result;
		Datum		values[3];
		bool		nulls[3];
		int			ndecoded,
					i;
		ItemPointer tids;
		Datum	   *tids_datum;

		memset(nulls, 0, sizeof(nulls));

		values[0] = ItemPointerGetDatum(&cur->first);
		values[1] = UInt16GetDatum(cur->nbytes);

		/* build an array of decoded item pointers */
		tids = ginPostingListDecode(cur, &ndecoded);
		tids_datum = (Datum *) palloc(ndecoded * sizeof(Datum));
		for (i = 0; i < ndecoded; i++)
			tids_datum[i] = ItemPointerGetDatum(&tids[i]);
		values[2] = PointerGetDatum(construct_array(tids_datum,
													ndecoded,
													TIDOID,
													sizeof(ItemPointerData),
													false, TYPALIGN_SHORT));
		pfree(tids_datum);
		pfree(tids);

		/* Build and return the result tuple. */
		resultTuple = heap_form_tuple(inter_call_data->tupd, values, nulls);
		result = HeapTupleGetDatum(resultTuple);

		inter_call_data->seg = GinNextPostingListSegment(cur);

		SRF_RETURN_NEXT(fctx, result);
	}

	SRF_RETURN_DONE(fctx);
}
