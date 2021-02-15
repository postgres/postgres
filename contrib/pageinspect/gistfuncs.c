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

Datum
gist_page_items_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bool		randomAccess;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext oldcontext;
	Page		page;
	OffsetNumber offset;
	OffsetNumber maxoff = InvalidOffsetNumber;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* The tupdesc and tuplestore must be created in ecxt_per_query_memory */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	randomAccess = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;
	tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	page = get_page_from_raw(raw_page);

	/* Avoid bogus PageGetMaxOffsetNumber() call with deleted pages */
	if (GistPageIsDeleted(page))
		elog(NOTICE, "page is deleted");
	else
		maxoff = PageGetMaxOffsetNumber(page);

	for (offset = FirstOffsetNumber;
		 offset <= maxoff;
		 offset++)
	{
		Datum		values[5];
		bool		nulls[5];
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
		values[3] = BoolGetDatum(ItemIdIsDead(id));
		values[4] = PointerGetDatum(tuple_bytea);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	return (Datum) 0;
}

Datum
gist_page_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	Oid			indexRelid = PG_GETARG_OID(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bool		randomAccess;
	Relation	indexRel;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext oldcontext;
	Page		page;
	OffsetNumber offset;
	OffsetNumber maxoff = InvalidOffsetNumber;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* The tupdesc and tuplestore must be created in ecxt_per_query_memory */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	randomAccess = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;
	tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* Open the relation */
	indexRel = index_open(indexRelid, AccessShareLock);

	page = get_page_from_raw(raw_page);

	/* Avoid bogus PageGetMaxOffsetNumber() call with deleted pages */
	if (GistPageIsDeleted(page))
		elog(NOTICE, "page is deleted");
	else
		maxoff = PageGetMaxOffsetNumber(page);

	for (offset = FirstOffsetNumber;
		 offset <= maxoff;
		 offset++)
	{
		Datum		values[5];
		bool		nulls[5];
		ItemId		id;
		IndexTuple	itup;
		Datum		itup_values[INDEX_MAX_KEYS];
		bool		itup_isnull[INDEX_MAX_KEYS];
		char	   *key_desc;

		id = PageGetItemId(page, offset);

		if (!ItemIdIsValid(id))
			elog(ERROR, "invalid ItemId");

		itup = (IndexTuple) PageGetItem(page, id);

		index_deform_tuple(itup, RelationGetDescr(indexRel),
						   itup_values, itup_isnull);

		memset(nulls, 0, sizeof(nulls));

		values[0] = DatumGetInt16(offset);
		values[1] = ItemPointerGetDatum(&itup->t_tid);
		values[2] = Int32GetDatum((int) IndexTupleSize(itup));
		values[3] = BoolGetDatum(ItemIdIsDead(id));

		key_desc = BuildIndexValueDescription(indexRel, itup_values, itup_isnull);
		if (key_desc)
			values[4] = CStringGetTextDatum(key_desc);
		else
		{
			values[4] = (Datum) 0;
			nulls[4] = true;
		}

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	relation_close(indexRel, AccessShareLock);

	return (Datum) 0;
}
