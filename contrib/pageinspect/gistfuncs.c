/*
 * gistfuncs.c
 *		Functions to investigate the content of GiST indexes
 *
 * Copyright (c) 2014-2022, PostgreSQL Global Development Group
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
#include "catalog/pg_am_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pageinspect.h"
#include "storage/itemptr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/varlena.h"

PG_FUNCTION_INFO_V1(gist_page_opaque_info);
PG_FUNCTION_INFO_V1(gist_page_items);
PG_FUNCTION_INFO_V1(gist_page_items_bytea);

#define IS_GIST(r) ((r)->rd_rel->relam == GIST_AM_OID)

#define ItemPointerGetDatum(X)	 PointerGetDatum(X)


static Page verify_gist_page(bytea *raw_page);

/*
 * Verify that the given bytea contains a GIST page or die in the attempt.
 * A pointer to the page is returned.
 */
static Page
verify_gist_page(bytea *raw_page)
{
	Page		page = get_page_from_raw(raw_page);
	GISTPageOpaque opaq;

	if (PageIsNew(page))
		return page;

	/* verify the special space has the expected size */
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GISTPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input page is not a valid %s page", "GiST"),
				 errdetail("Expected special size %d, got %d.",
						   (int) MAXALIGN(sizeof(GISTPageOpaqueData)),
						   (int) PageGetSpecialSize(page))));

	opaq = GistPageGetOpaque(page);
	if (opaq->gist_page_id != GIST_PAGE_ID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input page is not a valid %s page", "GiST"),
				 errdetail("Expected %08x, got %08x.",
						   GIST_PAGE_ID,
						   opaq->gist_page_id)));

	return page;
}

Datum
gist_page_opaque_info(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	TupleDesc	tupdesc;
	Page		page;
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

	page = verify_gist_page(raw_page);

	if (PageIsNew(page))
		PG_RETURN_NULL();

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Convert the flags bitmask to an array of human-readable names */
	flagbits = GistPageGetOpaque(page)->flags;
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
	values[2] = Int64GetDatum(GistPageGetOpaque(page)->rightlink);
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
	Page		page;
	OffsetNumber offset;
	OffsetNumber maxoff = InvalidOffsetNumber;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	InitMaterializedSRF(fcinfo, 0);

	page = verify_gist_page(raw_page);

	if (PageIsNew(page))
		PG_RETURN_NULL();

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

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

Datum
gist_page_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	Oid			indexRelid = PG_GETARG_OID(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	indexRel;
	TupleDesc	tupdesc;
	Page		page;
	uint16		flagbits;
	bits16		printflags = 0;
	OffsetNumber offset;
	OffsetNumber maxoff = InvalidOffsetNumber;
	char	   *index_columns;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	InitMaterializedSRF(fcinfo, 0);

	/* Open the relation */
	indexRel = index_open(indexRelid, AccessShareLock);

	if (!IS_GIST(indexRel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a %s index",
						RelationGetRelationName(indexRel), "GiST")));

	page = verify_gist_page(raw_page);

	if (PageIsNew(page))
	{
		index_close(indexRel, AccessShareLock);
		PG_RETURN_NULL();
	}

	flagbits = GistPageGetOpaque(page)->flags;

	/*
	 * Included attributes are added when dealing with leaf pages, discarded
	 * for non-leaf pages as these include only data for key attributes.
	 */
	printflags |= RULE_INDEXDEF_PRETTY;
	if (flagbits & F_LEAF)
	{
		tupdesc = RelationGetDescr(indexRel);
	}
	else
	{
		tupdesc = CreateTupleDescCopy(RelationGetDescr(indexRel));
		tupdesc->natts = IndexRelationGetNumberOfKeyAttributes(indexRel);
		printflags |= RULE_INDEXDEF_KEYS_ONLY;
	}

	index_columns = pg_get_indexdef_columns_extended(indexRelid,
													 printflags);

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
		StringInfoData buf;
		int			i;

		id = PageGetItemId(page, offset);

		if (!ItemIdIsValid(id))
			elog(ERROR, "invalid ItemId");

		itup = (IndexTuple) PageGetItem(page, id);

		index_deform_tuple(itup, tupdesc,
						   itup_values, itup_isnull);

		memset(nulls, 0, sizeof(nulls));

		values[0] = DatumGetInt16(offset);
		values[1] = ItemPointerGetDatum(&itup->t_tid);
		values[2] = Int32GetDatum((int) IndexTupleSize(itup));
		values[3] = BoolGetDatum(ItemIdIsDead(id));

		if (index_columns)
		{
			initStringInfo(&buf);
			appendStringInfo(&buf, "(%s)=(", index_columns);

			/* Most of this is copied from record_out(). */
			for (i = 0; i < tupdesc->natts; i++)
			{
				char	   *value;
				char	   *tmp;
				bool		nq = false;

				if (itup_isnull[i])
					value = "null";
				else
				{
					Oid			foutoid;
					bool		typisvarlena;
					Oid			typoid;

					typoid = tupdesc->attrs[i].atttypid;
					getTypeOutputInfo(typoid, &foutoid, &typisvarlena);
					value = OidOutputFunctionCall(foutoid, itup_values[i]);
				}

				if (i == IndexRelationGetNumberOfKeyAttributes(indexRel))
					appendStringInfoString(&buf, ") INCLUDE (");
				else if (i > 0)
					appendStringInfoString(&buf, ", ");

				/* Check whether we need double quotes for this value */
				nq = (value[0] == '\0');	/* force quotes for empty string */
				for (tmp = value; *tmp; tmp++)
				{
					char		ch = *tmp;

					if (ch == '"' || ch == '\\' ||
						ch == '(' || ch == ')' || ch == ',' ||
						isspace((unsigned char) ch))
					{
						nq = true;
						break;
					}
				}

				/* And emit the string */
				if (nq)
					appendStringInfoCharMacro(&buf, '"');
				for (tmp = value; *tmp; tmp++)
				{
					char		ch = *tmp;

					if (ch == '"' || ch == '\\')
						appendStringInfoCharMacro(&buf, ch);
					appendStringInfoCharMacro(&buf, ch);
				}
				if (nq)
					appendStringInfoCharMacro(&buf, '"');
			}

			appendStringInfoChar(&buf, ')');

			values[4] = CStringGetTextDatum(buf.data);
			nulls[4] = false;
		}
		else
		{
			values[4] = (Datum) 0;
			nulls[4] = true;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	relation_close(indexRel, AccessShareLock);

	return (Datum) 0;
}
