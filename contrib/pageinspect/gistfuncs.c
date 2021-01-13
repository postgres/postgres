/*
 * gistfuncs.c
 *		Functions to investigate the content of GiST indexes
 *
 * Copyright (c) 2014-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pageinspect/gistfuncs.c
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/gist_private.h"
#include "access/htup.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pageinspect.h"
#include "storage/itemptr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/pg_lsn.h"
#include "utils/varlena.h"

PG_FUNCTION_INFO_V1(gist_page_opaque_info);
PG_FUNCTION_INFO_V1(gist_page_items);
PG_FUNCTION_INFO_V1(gist_page_items_bytea);

#define ItemPointerGetDatum(X)	 PointerGetDatum(X)


Datum
gist_page_opaque_info(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	TupleDesc	tupdesc;
	Page		page;
	GISTPageOpaque opaq;
	HeapTuple	resultTuple;
	Datum		values[4];
	bool		nulls[4];
	Datum		flags[16];
	int			nflags = 0;
	uint16		flagbits;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	page = get_page_from_raw(raw_page);

	opaq = (GISTPageOpaque) PageGetSpecialPointer(page);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Convert the flags bitmask to an array of human-readable names */
	flagbits = opaq->flags;
	if (flagbits & F_LEAF)
		flags[nflags++] = CStringGetTextDatum("leaf");
	if (flagbits & F_DELETED)
		flags[nflags++] = CStringGetTextDatum("deleted");
	if (flagbits & F_TUPLES_DELETED)
		flags[nflags++] = CStringGetTextDatum("tuples_deleted");
	if (flagbits & F_FOLLOW_RIGHT)
		flags[nflags++] = CStringGetTextDatum("follow_right");
	if (flagbits & F_HAS_GARBAGE)
		flags[nflags++] = CStringGetTextDatum("has_garbage");
	flagbits &= ~(F_LEAF | F_DELETED | F_TUPLES_DELETED | F_FOLLOW_RIGHT | F_HAS_GARBAGE);
	if (flagbits)
	{
		/* any flags we don't recognize are printed in hex */
		flags[nflags++] = DirectFunctionCall1(to_hex32, Int32GetDatum(flagbits));
	}

	memset(nulls, 0, sizeof(nulls));

	values[0] = LSNGetDatum(PageGetLSN(page));
	values[1] = LSNGetDatum(GistPageGetNSN(page));
	values[2] = Int64GetDatum(opaq->rightlink);
	values[3] = PointerGetDatum(construct_array(flags, nflags,
												TEXTOID,
												-1, false, TYPALIGN_INT));

	/* Build and return the result tuple. */
	resultTuple = heap_form_tuple(tupdesc, values, nulls);

	return HeapTupleGetDatum(resultTuple);
}

typedef struct gist_page_items_state
{
	Page		page;
	TupleDesc	tupd;
	OffsetNumber offset;
	Relation	rel;
} gist_page_items_state;

Datum
gist_page_items_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	FuncCallContext *fctx;
	gist_page_items_state *inter_call_data;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext mctx;
		Page		page;

		fctx = SRF_FIRSTCALL_INIT();
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		page = get_page_from_raw(raw_page);

		inter_call_data = palloc(sizeof(gist_page_items_state));

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		if (GistPageIsDeleted(page))
			elog(NOTICE, "page is deleted");

		inter_call_data->page = page;
		inter_call_data->tupd = tupdesc;
		inter_call_data->offset = FirstOffsetNumber;

		fctx->max_calls = PageGetMaxOffsetNumber(page);
		fctx->user_fctx = inter_call_data;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	inter_call_data = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		Page		page = inter_call_data->page;
		OffsetNumber offset = inter_call_data->offset;
		HeapTuple	resultTuple;
		Datum		result;
		Datum		values[4];
		bool		nulls[4];
		ItemId		id;
		IndexTuple	itup;
		bytea	   *tuple_bytea;
		int			tuple_len;

		id = PageGetItemId(page, offset);

		if (!ItemIdIsValid(id))
			elog(ERROR, "invalid ItemId");

		itup = (IndexTuple) PageGetItem(page, id);
		tuple_len = IndexTupleSize(itup);

		memset(nulls, 0, sizeof(nulls));

		values[0] = DatumGetInt16(offset);
		values[1] = ItemPointerGetDatum(&itup->t_tid);
		values[2] = Int32GetDatum((int) IndexTupleSize(itup));

		tuple_bytea = (bytea *) palloc(tuple_len + VARHDRSZ);
		SET_VARSIZE(tuple_bytea, tuple_len + VARHDRSZ);
		memcpy(VARDATA(tuple_bytea), itup, tuple_len);
		values[3] = PointerGetDatum(tuple_bytea);

		/* Build and return the result tuple. */
		resultTuple = heap_form_tuple(inter_call_data->tupd, values, nulls);
		result = HeapTupleGetDatum(resultTuple);

		inter_call_data->offset++;
		SRF_RETURN_NEXT(fctx, result);
	}

	SRF_RETURN_DONE(fctx);
}

Datum
gist_page_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	Oid			indexRelid = PG_GETARG_OID(1);
	FuncCallContext *fctx;
	gist_page_items_state *inter_call_data;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	if (SRF_IS_FIRSTCALL())
	{
		Relation	indexRel;
		TupleDesc	tupdesc;
		MemoryContext mctx;
		Page		page;

		fctx = SRF_FIRSTCALL_INIT();
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		page = get_page_from_raw(raw_page);

		inter_call_data = palloc(sizeof(gist_page_items_state));

		/* Open the relation */
		indexRel = index_open(indexRelid, AccessShareLock);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		if (GistPageIsDeleted(page))
			elog(NOTICE, "page is deleted");

		inter_call_data->page = page;
		inter_call_data->tupd = tupdesc;
		inter_call_data->offset = FirstOffsetNumber;
		inter_call_data->rel = indexRel;

		fctx->max_calls = PageGetMaxOffsetNumber(page);
		fctx->user_fctx = inter_call_data;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	inter_call_data = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		Page		page = inter_call_data->page;
		OffsetNumber offset = inter_call_data->offset;
		HeapTuple	resultTuple;
		Datum		result;
		Datum		values[4];
		bool		nulls[4];
		ItemId		id;
		IndexTuple	itup;
		Datum		itup_values[INDEX_MAX_KEYS];
		bool		itup_isnull[INDEX_MAX_KEYS];
		char	   *key_desc;

		id = PageGetItemId(page, offset);

		if (!ItemIdIsValid(id))
			elog(ERROR, "invalid ItemId");

		itup = (IndexTuple) PageGetItem(page, id);

		index_deform_tuple(itup, RelationGetDescr(inter_call_data->rel),
						   itup_values, itup_isnull);

		key_desc = BuildIndexValueDescription(inter_call_data->rel, itup_values,
											  itup_isnull);

		memset(nulls, 0, sizeof(nulls));

		values[0] = DatumGetInt16(offset);
		values[1] = ItemPointerGetDatum(&itup->t_tid);
		values[2] = Int32GetDatum((int) IndexTupleSize(itup));
		values[3] = CStringGetTextDatum(key_desc);

		/* Build and return the result tuple. */
		resultTuple = heap_form_tuple(inter_call_data->tupd, values, nulls);
		result = HeapTupleGetDatum(resultTuple);

		inter_call_data->offset++;
		SRF_RETURN_NEXT(fctx, result);
	}

	relation_close(inter_call_data->rel, AccessShareLock);

	SRF_RETURN_DONE(fctx);
}
