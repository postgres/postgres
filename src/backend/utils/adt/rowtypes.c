/*-------------------------------------------------------------------------
 *
 * rowtypes.c
 *	  I/O and comparison functions for generic composite types.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/rowtypes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/detoast.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


/*
 * structure to cache metadata needed for record I/O
 */
typedef struct ColumnIOData
{
	Oid			column_type;
	Oid			typiofunc;
	Oid			typioparam;
	bool		typisvarlena;
	FmgrInfo	proc;
} ColumnIOData;

typedef struct RecordIOData
{
	Oid			record_type;
	int32		record_typmod;
	int			ncolumns;
	ColumnIOData columns[FLEXIBLE_ARRAY_MEMBER];
} RecordIOData;

/*
 * structure to cache metadata needed for record comparison
 */
typedef struct ColumnCompareData
{
	TypeCacheEntry *typentry;	/* has everything we need, actually */
} ColumnCompareData;

typedef struct RecordCompareData
{
	int			ncolumns;		/* allocated length of columns[] */
	Oid			record1_type;
	int32		record1_typmod;
	Oid			record2_type;
	int32		record2_typmod;
	ColumnCompareData columns[FLEXIBLE_ARRAY_MEMBER];
} RecordCompareData;


/*
 * record_in		- input routine for any composite type.
 */
Datum
record_in(PG_FUNCTION_ARGS)
{
	char	   *string = PG_GETARG_CSTRING(0);
	Oid			tupType = PG_GETARG_OID(1);
	int32		tupTypmod = PG_GETARG_INT32(2);
	HeapTupleHeader result;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	RecordIOData *my_extra;
	bool		needComma = false;
	int			ncolumns;
	int			i;
	char	   *ptr;
	Datum	   *values;
	bool	   *nulls;
	StringInfoData buf;

	check_stack_depth();		/* recurses for record-type columns */

	/*
	 * Give a friendly error message if we did not get enough info to identify
	 * the target record type.  (lookup_rowtype_tupdesc would fail anyway, but
	 * with a non-user-friendly message.)  In ordinary SQL usage, we'll get -1
	 * for typmod, since composite types and RECORD have no type modifiers at
	 * the SQL level, and thus must fail for RECORD.  However some callers can
	 * supply a valid typmod, and then we can do something useful for RECORD.
	 */
	if (tupType == RECORDOID && tupTypmod < 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("input of anonymous composite types is not implemented")));

	/*
	 * This comes from the composite type's pg_type.oid and stores system oids
	 * in user tables, specifically DatumTupleFields. This oid must be
	 * preserved by binary upgrades.
	 */
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/*
	 * We arrange to look up the needed I/O info just once per series of
	 * calls, assuming the record type doesn't change underneath us.
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordIOData, columns) +
							   ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	/*
	 * Scan the string.  We use "buf" to accumulate the de-quoted data for
	 * each column, which is then fed to the appropriate input converter.
	 */
	ptr = string;
	/* Allow leading whitespace */
	while (*ptr && isspace((unsigned char) *ptr))
		ptr++;
	if (*ptr++ != '(')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed record literal: \"%s\"", string),
				 errdetail("Missing left parenthesis.")));

	initStringInfo(&buf);

	for (i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = att->atttypid;
		char	   *column_data;

		/* Ignore dropped columns in datatype, but fill with nulls */
		if (att->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

		if (needComma)
		{
			/* Skip comma that separates prior field from this one */
			if (*ptr == ',')
				ptr++;
			else
				/* *ptr must be ')' */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed record literal: \"%s\"", string),
						 errdetail("Too few columns.")));
		}

		/* Check for null: completely empty input means null */
		if (*ptr == ',' || *ptr == ')')
		{
			column_data = NULL;
			nulls[i] = true;
		}
		else
		{
			/* Extract string for this column */
			bool		inquote = false;

			resetStringInfo(&buf);
			while (inquote || !(*ptr == ',' || *ptr == ')'))
			{
				char		ch = *ptr++;

				if (ch == '\0')
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed record literal: \"%s\"",
									string),
							 errdetail("Unexpected end of input.")));
				if (ch == '\\')
				{
					if (*ptr == '\0')
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed record literal: \"%s\"",
										string),
								 errdetail("Unexpected end of input.")));
					appendStringInfoChar(&buf, *ptr++);
				}
				else if (ch == '"')
				{
					if (!inquote)
						inquote = true;
					else if (*ptr == '"')
					{
						/* doubled quote within quote sequence */
						appendStringInfoChar(&buf, *ptr++);
					}
					else
						inquote = false;
				}
				else
					appendStringInfoChar(&buf, ch);
			}

			column_data = buf.data;
			nulls[i] = false;
		}

		/*
		 * Convert the column value
		 */
		if (column_info->column_type != column_type)
		{
			getTypeInputInfo(column_type,
							 &column_info->typiofunc,
							 &column_info->typioparam);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}

		values[i] = InputFunctionCall(&column_info->proc,
									  column_data,
									  column_info->typioparam,
									  att->atttypmod);

		/*
		 * Prep for next column
		 */
		needComma = true;
	}

	if (*ptr++ != ')')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed record literal: \"%s\"", string),
				 errdetail("Too many columns.")));
	/* Allow trailing whitespace */
	while (*ptr && isspace((unsigned char) *ptr))
		ptr++;
	if (*ptr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed record literal: \"%s\"", string),
				 errdetail("Junk after right parenthesis.")));

	tuple = heap_form_tuple(tupdesc, values, nulls);

	/*
	 * We cannot return tuple->t_data because heap_form_tuple allocates it as
	 * part of a larger chunk, and our caller may expect to be able to pfree
	 * our result.  So must copy the info into a new palloc chunk.
	 */
	result = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy(result, tuple->t_data, tuple->t_len);

	heap_freetuple(tuple);
	pfree(buf.data);
	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	PG_RETURN_HEAPTUPLEHEADER(result);
}

/*
 * record_out		- output routine for any composite type.
 */
Datum
record_out(PG_FUNCTION_ARGS)
{
	HeapTupleHeader rec = PG_GETARG_HEAPTUPLEHEADER(0);
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	RecordIOData *my_extra;
	bool		needComma = false;
	int			ncolumns;
	int			i;
	Datum	   *values;
	bool	   *nulls;
	StringInfoData buf;

	check_stack_depth();		/* recurses for record-type columns */

	/* Extract type info from the tuple itself */
	tupType = HeapTupleHeaderGetTypeId(rec);
	tupTypmod = HeapTupleHeaderGetTypMod(rec);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/* Build a temporary HeapTuple control structure */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = rec;

	/*
	 * We arrange to look up the needed I/O info just once per series of
	 * calls, assuming the record type doesn't change underneath us.
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordIOData, columns) +
							   ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	/* Break down the tuple into fields */
	heap_deform_tuple(&tuple, tupdesc, values, nulls);

	/* And build the result string */
	initStringInfo(&buf);

	appendStringInfoChar(&buf, '(');

	for (i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = att->atttypid;
		Datum		attr;
		char	   *value;
		char	   *tmp;
		bool		nq;

		/* Ignore dropped columns in datatype */
		if (att->attisdropped)
			continue;

		if (needComma)
			appendStringInfoChar(&buf, ',');
		needComma = true;

		if (nulls[i])
		{
			/* emit nothing... */
			continue;
		}

		/*
		 * Convert the column value to text
		 */
		if (column_info->column_type != column_type)
		{
			getTypeOutputInfo(column_type,
							  &column_info->typiofunc,
							  &column_info->typisvarlena);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}

		attr = values[i];
		value = OutputFunctionCall(&column_info->proc, attr);

		/* Detect whether we need double quotes for this value */
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

	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	PG_RETURN_CSTRING(buf.data);
}

/*
 * record_recv		- binary input routine for any composite type.
 */
Datum
record_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			tupType = PG_GETARG_OID(1);
	int32		tupTypmod = PG_GETARG_INT32(2);
	HeapTupleHeader result;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	RecordIOData *my_extra;
	int			ncolumns;
	int			usercols;
	int			validcols;
	int			i;
	Datum	   *values;
	bool	   *nulls;

	check_stack_depth();		/* recurses for record-type columns */

	/*
	 * Give a friendly error message if we did not get enough info to identify
	 * the target record type.  (lookup_rowtype_tupdesc would fail anyway, but
	 * with a non-user-friendly message.)  In ordinary SQL usage, we'll get -1
	 * for typmod, since composite types and RECORD have no type modifiers at
	 * the SQL level, and thus must fail for RECORD.  However some callers can
	 * supply a valid typmod, and then we can do something useful for RECORD.
	 */
	if (tupType == RECORDOID && tupTypmod < 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("input of anonymous composite types is not implemented")));

	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/*
	 * We arrange to look up the needed I/O info just once per series of
	 * calls, assuming the record type doesn't change underneath us.
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordIOData, columns) +
							   ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	/* Fetch number of columns user thinks it has */
	usercols = pq_getmsgint(buf, 4);

	/* Need to scan to count nondeleted columns */
	validcols = 0;
	for (i = 0; i < ncolumns; i++)
	{
		if (!TupleDescAttr(tupdesc, i)->attisdropped)
			validcols++;
	}
	if (usercols != validcols)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("wrong number of columns: %d, expected %d",
						usercols, validcols)));

	/* Process each column */
	for (i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = att->atttypid;
		Oid			coltypoid;
		int			itemlen;
		StringInfoData item_buf;
		StringInfo	bufptr;
		char		csave;

		/* Ignore dropped columns in datatype, but fill with nulls */
		if (att->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

		/* Check column type recorded in the data */
		coltypoid = pq_getmsgint(buf, sizeof(Oid));

		/*
		 * From a security standpoint, it doesn't matter whether the input's
		 * column type matches what we expect: the column type's receive
		 * function has to be robust enough to cope with invalid data.
		 * However, from a user-friendliness standpoint, it's nicer to
		 * complain about type mismatches than to throw "improper binary
		 * format" errors.  But there's a problem: only built-in types have
		 * OIDs that are stable enough to believe that a mismatch is a real
		 * issue.  So complain only if both OIDs are in the built-in range.
		 * Otherwise, carry on with the column type we "should" be getting.
		 */
		if (coltypoid != column_type &&
			coltypoid < FirstGenbkiObjectId &&
			column_type < FirstGenbkiObjectId)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("binary data has type %u (%s) instead of expected %u (%s) in record column %d",
							coltypoid,
							format_type_extended(coltypoid, -1,
												 FORMAT_TYPE_ALLOW_INVALID),
							column_type,
							format_type_extended(column_type, -1,
												 FORMAT_TYPE_ALLOW_INVALID),
							i + 1)));

		/* Get and check the item length */
		itemlen = pq_getmsgint(buf, 4);
		if (itemlen < -1 || itemlen > (buf->len - buf->cursor))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("insufficient data left in message")));

		if (itemlen == -1)
		{
			/* -1 length means NULL */
			bufptr = NULL;
			nulls[i] = true;
			csave = 0;			/* keep compiler quiet */
		}
		else
		{
			/*
			 * Rather than copying data around, we just set up a phony
			 * StringInfo pointing to the correct portion of the input buffer.
			 * We assume we can scribble on the input buffer so as to maintain
			 * the convention that StringInfos have a trailing null.
			 */
			item_buf.data = &buf->data[buf->cursor];
			item_buf.maxlen = itemlen + 1;
			item_buf.len = itemlen;
			item_buf.cursor = 0;

			buf->cursor += itemlen;

			csave = buf->data[buf->cursor];
			buf->data[buf->cursor] = '\0';

			bufptr = &item_buf;
			nulls[i] = false;
		}

		/* Now call the column's receiveproc */
		if (column_info->column_type != column_type)
		{
			getTypeBinaryInputInfo(column_type,
								   &column_info->typiofunc,
								   &column_info->typioparam);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}

		values[i] = ReceiveFunctionCall(&column_info->proc,
										bufptr,
										column_info->typioparam,
										att->atttypmod);

		if (bufptr)
		{
			/* Trouble if it didn't eat the whole buffer */
			if (item_buf.cursor != itemlen)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
						 errmsg("improper binary format in record column %d",
								i + 1)));

			buf->data[buf->cursor] = csave;
		}
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);

	/*
	 * We cannot return tuple->t_data because heap_form_tuple allocates it as
	 * part of a larger chunk, and our caller may expect to be able to pfree
	 * our result.  So must copy the info into a new palloc chunk.
	 */
	result = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy(result, tuple->t_data, tuple->t_len);

	heap_freetuple(tuple);
	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	PG_RETURN_HEAPTUPLEHEADER(result);
}

/*
 * record_send		- binary output routine for any composite type.
 */
Datum
record_send(PG_FUNCTION_ARGS)
{
	HeapTupleHeader rec = PG_GETARG_HEAPTUPLEHEADER(0);
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	RecordIOData *my_extra;
	int			ncolumns;
	int			validcols;
	int			i;
	Datum	   *values;
	bool	   *nulls;
	StringInfoData buf;

	check_stack_depth();		/* recurses for record-type columns */

	/* Extract type info from the tuple itself */
	tupType = HeapTupleHeaderGetTypeId(rec);
	tupTypmod = HeapTupleHeaderGetTypMod(rec);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/* Build a temporary HeapTuple control structure */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = rec;

	/*
	 * We arrange to look up the needed I/O info just once per series of
	 * calls, assuming the record type doesn't change underneath us.
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordIOData, columns) +
							   ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	/* Break down the tuple into fields */
	heap_deform_tuple(&tuple, tupdesc, values, nulls);

	/* And build the result string */
	pq_begintypsend(&buf);

	/* Need to scan to count nondeleted columns */
	validcols = 0;
	for (i = 0; i < ncolumns; i++)
	{
		if (!TupleDescAttr(tupdesc, i)->attisdropped)
			validcols++;
	}
	pq_sendint32(&buf, validcols);

	for (i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = att->atttypid;
		Datum		attr;
		bytea	   *outputbytes;

		/* Ignore dropped columns in datatype */
		if (att->attisdropped)
			continue;

		pq_sendint32(&buf, column_type);

		if (nulls[i])
		{
			/* emit -1 data length to signify a NULL */
			pq_sendint32(&buf, -1);
			continue;
		}

		/*
		 * Convert the column value to binary
		 */
		if (column_info->column_type != column_type)
		{
			getTypeBinaryOutputInfo(column_type,
									&column_info->typiofunc,
									&column_info->typisvarlena);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}

		attr = values[i];
		outputbytes = SendFunctionCall(&column_info->proc, attr);
		pq_sendint32(&buf, VARSIZE(outputbytes) - VARHDRSZ);
		pq_sendbytes(&buf, VARDATA(outputbytes),
					 VARSIZE(outputbytes) - VARHDRSZ);
	}

	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 * record_cmp()
 * Internal comparison function for records.
 *
 * Returns -1, 0 or 1
 *
 * Do not assume that the two inputs are exactly the same record type;
 * for instance we might be comparing an anonymous ROW() construct against a
 * named composite type.  We will compare as long as they have the same number
 * of non-dropped columns of the same types.
 */
static int
record_cmp(FunctionCallInfo fcinfo)
{
	HeapTupleHeader record1 = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleHeader record2 = PG_GETARG_HEAPTUPLEHEADER(1);
	int			result = 0;
	Oid			tupType1;
	Oid			tupType2;
	int32		tupTypmod1;
	int32		tupTypmod2;
	TupleDesc	tupdesc1;
	TupleDesc	tupdesc2;
	HeapTupleData tuple1;
	HeapTupleData tuple2;
	int			ncolumns1;
	int			ncolumns2;
	RecordCompareData *my_extra;
	int			ncols;
	Datum	   *values1;
	Datum	   *values2;
	bool	   *nulls1;
	bool	   *nulls2;
	int			i1;
	int			i2;
	int			j;

	check_stack_depth();		/* recurses for record-type columns */

	/* Extract type info from the tuples */
	tupType1 = HeapTupleHeaderGetTypeId(record1);
	tupTypmod1 = HeapTupleHeaderGetTypMod(record1);
	tupdesc1 = lookup_rowtype_tupdesc(tupType1, tupTypmod1);
	ncolumns1 = tupdesc1->natts;
	tupType2 = HeapTupleHeaderGetTypeId(record2);
	tupTypmod2 = HeapTupleHeaderGetTypMod(record2);
	tupdesc2 = lookup_rowtype_tupdesc(tupType2, tupTypmod2);
	ncolumns2 = tupdesc2->natts;

	/* Build temporary HeapTuple control structures */
	tuple1.t_len = HeapTupleHeaderGetDatumLength(record1);
	ItemPointerSetInvalid(&(tuple1.t_self));
	tuple1.t_tableOid = InvalidOid;
	tuple1.t_data = record1;
	tuple2.t_len = HeapTupleHeaderGetDatumLength(record2);
	ItemPointerSetInvalid(&(tuple2.t_self));
	tuple2.t_tableOid = InvalidOid;
	tuple2.t_data = record2;

	/*
	 * We arrange to look up the needed comparison info just once per series
	 * of calls, assuming the record types don't change underneath us.
	 */
	ncols = Max(ncolumns1, ncolumns2);
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncols)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncols * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncols;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
		my_extra->record2_type = InvalidOid;
		my_extra->record2_typmod = 0;
	}

	if (my_extra->record1_type != tupType1 ||
		my_extra->record1_typmod != tupTypmod1 ||
		my_extra->record2_type != tupType2 ||
		my_extra->record2_typmod != tupTypmod2)
	{
		MemSet(my_extra->columns, 0, ncols * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType1;
		my_extra->record1_typmod = tupTypmod1;
		my_extra->record2_type = tupType2;
		my_extra->record2_typmod = tupTypmod2;
	}

	/* Break down the tuples into fields */
	values1 = (Datum *) palloc(ncolumns1 * sizeof(Datum));
	nulls1 = (bool *) palloc(ncolumns1 * sizeof(bool));
	heap_deform_tuple(&tuple1, tupdesc1, values1, nulls1);
	values2 = (Datum *) palloc(ncolumns2 * sizeof(Datum));
	nulls2 = (bool *) palloc(ncolumns2 * sizeof(bool));
	heap_deform_tuple(&tuple2, tupdesc2, values2, nulls2);

	/*
	 * Scan corresponding columns, allowing for dropped columns in different
	 * places in the two rows.  i1 and i2 are physical column indexes, j is
	 * the logical column index.
	 */
	i1 = i2 = j = 0;
	while (i1 < ncolumns1 || i2 < ncolumns2)
	{
		Form_pg_attribute att1;
		Form_pg_attribute att2;
		TypeCacheEntry *typentry;
		Oid			collation;

		/*
		 * Skip dropped columns
		 */
		if (i1 < ncolumns1 && TupleDescAttr(tupdesc1, i1)->attisdropped)
		{
			i1++;
			continue;
		}
		if (i2 < ncolumns2 && TupleDescAttr(tupdesc2, i2)->attisdropped)
		{
			i2++;
			continue;
		}
		if (i1 >= ncolumns1 || i2 >= ncolumns2)
			break;				/* we'll deal with mismatch below loop */

		att1 = TupleDescAttr(tupdesc1, i1);
		att2 = TupleDescAttr(tupdesc2, i2);

		/*
		 * Have two matching columns, they must be same type
		 */
		if (att1->atttypid != att2->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare dissimilar column types %s and %s at record column %d",
							format_type_be(att1->atttypid),
							format_type_be(att2->atttypid),
							j + 1)));

		/*
		 * If they're not same collation, we don't complain here, but the
		 * comparison function might.
		 */
		collation = att1->attcollation;
		if (collation != att2->attcollation)
			collation = InvalidOid;

		/*
		 * Lookup the comparison function if not done already
		 */
		typentry = my_extra->columns[j].typentry;
		if (typentry == NULL ||
			typentry->type_id != att1->atttypid)
		{
			typentry = lookup_type_cache(att1->atttypid,
										 TYPECACHE_CMP_PROC_FINFO);
			if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify a comparison function for type %s",
								format_type_be(typentry->type_id))));
			my_extra->columns[j].typentry = typentry;
		}

		/*
		 * We consider two NULLs equal; NULL > not-NULL.
		 */
		if (!nulls1[i1] || !nulls2[i2])
		{
			LOCAL_FCINFO(locfcinfo, 2);
			int32		cmpresult;

			if (nulls1[i1])
			{
				/* arg1 is greater than arg2 */
				result = 1;
				break;
			}
			if (nulls2[i2])
			{
				/* arg1 is less than arg2 */
				result = -1;
				break;
			}

			/* Compare the pair of elements */
			InitFunctionCallInfoData(*locfcinfo, &typentry->cmp_proc_finfo, 2,
									 collation, NULL, NULL);
			locfcinfo->args[0].value = values1[i1];
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = values2[i2];
			locfcinfo->args[1].isnull = false;
			cmpresult = DatumGetInt32(FunctionCallInvoke(locfcinfo));

			/* We don't expect comparison support functions to return null */
			Assert(!locfcinfo->isnull);

			if (cmpresult < 0)
			{
				/* arg1 is less than arg2 */
				result = -1;
				break;
			}
			else if (cmpresult > 0)
			{
				/* arg1 is greater than arg2 */
				result = 1;
				break;
			}
		}

		/* equal, so continue to next column */
		i1++, i2++, j++;
	}

	/*
	 * If we didn't break out of the loop early, check for column count
	 * mismatch.  (We do not report such mismatch if we found unequal column
	 * values; is that a feature or a bug?)
	 */
	if (result == 0)
	{
		if (i1 != ncolumns1 || i2 != ncolumns2)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare record types with different numbers of columns")));
	}

	pfree(values1);
	pfree(nulls1);
	pfree(values2);
	pfree(nulls2);
	ReleaseTupleDesc(tupdesc1);
	ReleaseTupleDesc(tupdesc2);

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(record1, 0);
	PG_FREE_IF_COPY(record2, 1);

	return result;
}

/*
 * record_eq :
 *		  compares two records for equality
 * result :
 *		  returns true if the records are equal, false otherwise.
 *
 * Note: we do not use record_cmp here, since equality may be meaningful in
 * datatypes that don't have a total ordering (and hence no btree support).
 */
Datum
record_eq(PG_FUNCTION_ARGS)
{
	HeapTupleHeader record1 = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleHeader record2 = PG_GETARG_HEAPTUPLEHEADER(1);
	bool		result = true;
	Oid			tupType1;
	Oid			tupType2;
	int32		tupTypmod1;
	int32		tupTypmod2;
	TupleDesc	tupdesc1;
	TupleDesc	tupdesc2;
	HeapTupleData tuple1;
	HeapTupleData tuple2;
	int			ncolumns1;
	int			ncolumns2;
	RecordCompareData *my_extra;
	int			ncols;
	Datum	   *values1;
	Datum	   *values2;
	bool	   *nulls1;
	bool	   *nulls2;
	int			i1;
	int			i2;
	int			j;

	check_stack_depth();		/* recurses for record-type columns */

	/* Extract type info from the tuples */
	tupType1 = HeapTupleHeaderGetTypeId(record1);
	tupTypmod1 = HeapTupleHeaderGetTypMod(record1);
	tupdesc1 = lookup_rowtype_tupdesc(tupType1, tupTypmod1);
	ncolumns1 = tupdesc1->natts;
	tupType2 = HeapTupleHeaderGetTypeId(record2);
	tupTypmod2 = HeapTupleHeaderGetTypMod(record2);
	tupdesc2 = lookup_rowtype_tupdesc(tupType2, tupTypmod2);
	ncolumns2 = tupdesc2->natts;

	/* Build temporary HeapTuple control structures */
	tuple1.t_len = HeapTupleHeaderGetDatumLength(record1);
	ItemPointerSetInvalid(&(tuple1.t_self));
	tuple1.t_tableOid = InvalidOid;
	tuple1.t_data = record1;
	tuple2.t_len = HeapTupleHeaderGetDatumLength(record2);
	ItemPointerSetInvalid(&(tuple2.t_self));
	tuple2.t_tableOid = InvalidOid;
	tuple2.t_data = record2;

	/*
	 * We arrange to look up the needed comparison info just once per series
	 * of calls, assuming the record types don't change underneath us.
	 */
	ncols = Max(ncolumns1, ncolumns2);
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncols)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncols * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncols;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
		my_extra->record2_type = InvalidOid;
		my_extra->record2_typmod = 0;
	}

	if (my_extra->record1_type != tupType1 ||
		my_extra->record1_typmod != tupTypmod1 ||
		my_extra->record2_type != tupType2 ||
		my_extra->record2_typmod != tupTypmod2)
	{
		MemSet(my_extra->columns, 0, ncols * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType1;
		my_extra->record1_typmod = tupTypmod1;
		my_extra->record2_type = tupType2;
		my_extra->record2_typmod = tupTypmod2;
	}

	/* Break down the tuples into fields */
	values1 = (Datum *) palloc(ncolumns1 * sizeof(Datum));
	nulls1 = (bool *) palloc(ncolumns1 * sizeof(bool));
	heap_deform_tuple(&tuple1, tupdesc1, values1, nulls1);
	values2 = (Datum *) palloc(ncolumns2 * sizeof(Datum));
	nulls2 = (bool *) palloc(ncolumns2 * sizeof(bool));
	heap_deform_tuple(&tuple2, tupdesc2, values2, nulls2);

	/*
	 * Scan corresponding columns, allowing for dropped columns in different
	 * places in the two rows.  i1 and i2 are physical column indexes, j is
	 * the logical column index.
	 */
	i1 = i2 = j = 0;
	while (i1 < ncolumns1 || i2 < ncolumns2)
	{
		LOCAL_FCINFO(locfcinfo, 2);
		Form_pg_attribute att1;
		Form_pg_attribute att2;
		TypeCacheEntry *typentry;
		Oid			collation;
		bool		oprresult;

		/*
		 * Skip dropped columns
		 */
		if (i1 < ncolumns1 && TupleDescAttr(tupdesc1, i1)->attisdropped)
		{
			i1++;
			continue;
		}
		if (i2 < ncolumns2 && TupleDescAttr(tupdesc2, i2)->attisdropped)
		{
			i2++;
			continue;
		}
		if (i1 >= ncolumns1 || i2 >= ncolumns2)
			break;				/* we'll deal with mismatch below loop */

		att1 = TupleDescAttr(tupdesc1, i1);
		att2 = TupleDescAttr(tupdesc2, i2);

		/*
		 * Have two matching columns, they must be same type
		 */
		if (att1->atttypid != att2->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare dissimilar column types %s and %s at record column %d",
							format_type_be(att1->atttypid),
							format_type_be(att2->atttypid),
							j + 1)));

		/*
		 * If they're not same collation, we don't complain here, but the
		 * equality function might.
		 */
		collation = att1->attcollation;
		if (collation != att2->attcollation)
			collation = InvalidOid;

		/*
		 * Lookup the equality function if not done already
		 */
		typentry = my_extra->columns[j].typentry;
		if (typentry == NULL ||
			typentry->type_id != att1->atttypid)
		{
			typentry = lookup_type_cache(att1->atttypid,
										 TYPECACHE_EQ_OPR_FINFO);
			if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify an equality operator for type %s",
								format_type_be(typentry->type_id))));
			my_extra->columns[j].typentry = typentry;
		}

		/*
		 * We consider two NULLs equal; NULL > not-NULL.
		 */
		if (!nulls1[i1] || !nulls2[i2])
		{
			if (nulls1[i1] || nulls2[i2])
			{
				result = false;
				break;
			}

			/* Compare the pair of elements */
			InitFunctionCallInfoData(*locfcinfo, &typentry->eq_opr_finfo, 2,
									 collation, NULL, NULL);
			locfcinfo->args[0].value = values1[i1];
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = values2[i2];
			locfcinfo->args[1].isnull = false;
			oprresult = DatumGetBool(FunctionCallInvoke(locfcinfo));
			if (locfcinfo->isnull || !oprresult)
			{
				result = false;
				break;
			}
		}

		/* equal, so continue to next column */
		i1++, i2++, j++;
	}

	/*
	 * If we didn't break out of the loop early, check for column count
	 * mismatch.  (We do not report such mismatch if we found unequal column
	 * values; is that a feature or a bug?)
	 */
	if (result)
	{
		if (i1 != ncolumns1 || i2 != ncolumns2)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare record types with different numbers of columns")));
	}

	pfree(values1);
	pfree(nulls1);
	pfree(values2);
	pfree(nulls2);
	ReleaseTupleDesc(tupdesc1);
	ReleaseTupleDesc(tupdesc2);

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(record1, 0);
	PG_FREE_IF_COPY(record2, 1);

	PG_RETURN_BOOL(result);
}

Datum
record_ne(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(!DatumGetBool(record_eq(fcinfo)));
}

Datum
record_lt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_cmp(fcinfo) < 0);
}

Datum
record_gt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_cmp(fcinfo) > 0);
}

Datum
record_le(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_cmp(fcinfo) <= 0);
}

Datum
record_ge(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_cmp(fcinfo) >= 0);
}

Datum
btrecordcmp(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(record_cmp(fcinfo));
}


/*
 * record_image_cmp :
 * Internal byte-oriented comparison function for records.
 *
 * Returns -1, 0 or 1
 *
 * Note: The normal concepts of "equality" do not apply here; different
 * representation of values considered to be equal are not considered to be
 * identical.  As an example, for the citext type 'A' and 'a' are equal, but
 * they are not identical.
 */
static int
record_image_cmp(FunctionCallInfo fcinfo)
{
	HeapTupleHeader record1 = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleHeader record2 = PG_GETARG_HEAPTUPLEHEADER(1);
	int			result = 0;
	Oid			tupType1;
	Oid			tupType2;
	int32		tupTypmod1;
	int32		tupTypmod2;
	TupleDesc	tupdesc1;
	TupleDesc	tupdesc2;
	HeapTupleData tuple1;
	HeapTupleData tuple2;
	int			ncolumns1;
	int			ncolumns2;
	RecordCompareData *my_extra;
	int			ncols;
	Datum	   *values1;
	Datum	   *values2;
	bool	   *nulls1;
	bool	   *nulls2;
	int			i1;
	int			i2;
	int			j;

	/* Extract type info from the tuples */
	tupType1 = HeapTupleHeaderGetTypeId(record1);
	tupTypmod1 = HeapTupleHeaderGetTypMod(record1);
	tupdesc1 = lookup_rowtype_tupdesc(tupType1, tupTypmod1);
	ncolumns1 = tupdesc1->natts;
	tupType2 = HeapTupleHeaderGetTypeId(record2);
	tupTypmod2 = HeapTupleHeaderGetTypMod(record2);
	tupdesc2 = lookup_rowtype_tupdesc(tupType2, tupTypmod2);
	ncolumns2 = tupdesc2->natts;

	/* Build temporary HeapTuple control structures */
	tuple1.t_len = HeapTupleHeaderGetDatumLength(record1);
	ItemPointerSetInvalid(&(tuple1.t_self));
	tuple1.t_tableOid = InvalidOid;
	tuple1.t_data = record1;
	tuple2.t_len = HeapTupleHeaderGetDatumLength(record2);
	ItemPointerSetInvalid(&(tuple2.t_self));
	tuple2.t_tableOid = InvalidOid;
	tuple2.t_data = record2;

	/*
	 * We arrange to look up the needed comparison info just once per series
	 * of calls, assuming the record types don't change underneath us.
	 */
	ncols = Max(ncolumns1, ncolumns2);
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncols)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncols * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncols;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
		my_extra->record2_type = InvalidOid;
		my_extra->record2_typmod = 0;
	}

	if (my_extra->record1_type != tupType1 ||
		my_extra->record1_typmod != tupTypmod1 ||
		my_extra->record2_type != tupType2 ||
		my_extra->record2_typmod != tupTypmod2)
	{
		MemSet(my_extra->columns, 0, ncols * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType1;
		my_extra->record1_typmod = tupTypmod1;
		my_extra->record2_type = tupType2;
		my_extra->record2_typmod = tupTypmod2;
	}

	/* Break down the tuples into fields */
	values1 = (Datum *) palloc(ncolumns1 * sizeof(Datum));
	nulls1 = (bool *) palloc(ncolumns1 * sizeof(bool));
	heap_deform_tuple(&tuple1, tupdesc1, values1, nulls1);
	values2 = (Datum *) palloc(ncolumns2 * sizeof(Datum));
	nulls2 = (bool *) palloc(ncolumns2 * sizeof(bool));
	heap_deform_tuple(&tuple2, tupdesc2, values2, nulls2);

	/*
	 * Scan corresponding columns, allowing for dropped columns in different
	 * places in the two rows.  i1 and i2 are physical column indexes, j is
	 * the logical column index.
	 */
	i1 = i2 = j = 0;
	while (i1 < ncolumns1 || i2 < ncolumns2)
	{
		Form_pg_attribute att1;
		Form_pg_attribute att2;

		/*
		 * Skip dropped columns
		 */
		if (i1 < ncolumns1 && TupleDescAttr(tupdesc1, i1)->attisdropped)
		{
			i1++;
			continue;
		}
		if (i2 < ncolumns2 && TupleDescAttr(tupdesc2, i2)->attisdropped)
		{
			i2++;
			continue;
		}
		if (i1 >= ncolumns1 || i2 >= ncolumns2)
			break;				/* we'll deal with mismatch below loop */

		att1 = TupleDescAttr(tupdesc1, i1);
		att2 = TupleDescAttr(tupdesc2, i2);

		/*
		 * Have two matching columns, they must be same type
		 */
		if (att1->atttypid != att2->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare dissimilar column types %s and %s at record column %d",
							format_type_be(att1->atttypid),
							format_type_be(att2->atttypid),
							j + 1)));

		/*
		 * The same type should have the same length (or both should be
		 * variable).
		 */
		Assert(att1->attlen == att2->attlen);

		/*
		 * We consider two NULLs equal; NULL > not-NULL.
		 */
		if (!nulls1[i1] || !nulls2[i2])
		{
			int			cmpresult = 0;

			if (nulls1[i1])
			{
				/* arg1 is greater than arg2 */
				result = 1;
				break;
			}
			if (nulls2[i2])
			{
				/* arg1 is less than arg2 */
				result = -1;
				break;
			}

			/* Compare the pair of elements */
			if (att1->attbyval)
			{
				if (values1[i1] != values2[i2])
					cmpresult = (values1[i1] < values2[i2]) ? -1 : 1;
			}
			else if (att1->attlen > 0)
			{
				cmpresult = memcmp(DatumGetPointer(values1[i1]),
								   DatumGetPointer(values2[i2]),
								   att1->attlen);
			}
			else if (att1->attlen == -1)
			{
				Size		len1,
							len2;
				struct varlena *arg1val;
				struct varlena *arg2val;

				len1 = toast_raw_datum_size(values1[i1]);
				len2 = toast_raw_datum_size(values2[i2]);
				arg1val = PG_DETOAST_DATUM_PACKED(values1[i1]);
				arg2val = PG_DETOAST_DATUM_PACKED(values2[i2]);

				cmpresult = memcmp(VARDATA_ANY(arg1val),
								   VARDATA_ANY(arg2val),
								   Min(len1, len2) - VARHDRSZ);
				if ((cmpresult == 0) && (len1 != len2))
					cmpresult = (len1 < len2) ? -1 : 1;

				if ((Pointer) arg1val != (Pointer) values1[i1])
					pfree(arg1val);
				if ((Pointer) arg2val != (Pointer) values2[i2])
					pfree(arg2val);
			}
			else
				elog(ERROR, "unexpected attlen: %d", att1->attlen);

			if (cmpresult < 0)
			{
				/* arg1 is less than arg2 */
				result = -1;
				break;
			}
			else if (cmpresult > 0)
			{
				/* arg1 is greater than arg2 */
				result = 1;
				break;
			}
		}

		/* equal, so continue to next column */
		i1++, i2++, j++;
	}

	/*
	 * If we didn't break out of the loop early, check for column count
	 * mismatch.  (We do not report such mismatch if we found unequal column
	 * values; is that a feature or a bug?)
	 */
	if (result == 0)
	{
		if (i1 != ncolumns1 || i2 != ncolumns2)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare record types with different numbers of columns")));
	}

	pfree(values1);
	pfree(nulls1);
	pfree(values2);
	pfree(nulls2);
	ReleaseTupleDesc(tupdesc1);
	ReleaseTupleDesc(tupdesc2);

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(record1, 0);
	PG_FREE_IF_COPY(record2, 1);

	return result;
}

/*
 * record_image_eq :
 *		  compares two records for identical contents, based on byte images
 * result :
 *		  returns true if the records are identical, false otherwise.
 *
 * Note: we do not use record_image_cmp here, since we can avoid
 * de-toasting for unequal lengths this way.
 */
Datum
record_image_eq(PG_FUNCTION_ARGS)
{
	HeapTupleHeader record1 = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleHeader record2 = PG_GETARG_HEAPTUPLEHEADER(1);
	bool		result = true;
	Oid			tupType1;
	Oid			tupType2;
	int32		tupTypmod1;
	int32		tupTypmod2;
	TupleDesc	tupdesc1;
	TupleDesc	tupdesc2;
	HeapTupleData tuple1;
	HeapTupleData tuple2;
	int			ncolumns1;
	int			ncolumns2;
	RecordCompareData *my_extra;
	int			ncols;
	Datum	   *values1;
	Datum	   *values2;
	bool	   *nulls1;
	bool	   *nulls2;
	int			i1;
	int			i2;
	int			j;

	/* Extract type info from the tuples */
	tupType1 = HeapTupleHeaderGetTypeId(record1);
	tupTypmod1 = HeapTupleHeaderGetTypMod(record1);
	tupdesc1 = lookup_rowtype_tupdesc(tupType1, tupTypmod1);
	ncolumns1 = tupdesc1->natts;
	tupType2 = HeapTupleHeaderGetTypeId(record2);
	tupTypmod2 = HeapTupleHeaderGetTypMod(record2);
	tupdesc2 = lookup_rowtype_tupdesc(tupType2, tupTypmod2);
	ncolumns2 = tupdesc2->natts;

	/* Build temporary HeapTuple control structures */
	tuple1.t_len = HeapTupleHeaderGetDatumLength(record1);
	ItemPointerSetInvalid(&(tuple1.t_self));
	tuple1.t_tableOid = InvalidOid;
	tuple1.t_data = record1;
	tuple2.t_len = HeapTupleHeaderGetDatumLength(record2);
	ItemPointerSetInvalid(&(tuple2.t_self));
	tuple2.t_tableOid = InvalidOid;
	tuple2.t_data = record2;

	/*
	 * We arrange to look up the needed comparison info just once per series
	 * of calls, assuming the record types don't change underneath us.
	 */
	ncols = Max(ncolumns1, ncolumns2);
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncols)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncols * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncols;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
		my_extra->record2_type = InvalidOid;
		my_extra->record2_typmod = 0;
	}

	if (my_extra->record1_type != tupType1 ||
		my_extra->record1_typmod != tupTypmod1 ||
		my_extra->record2_type != tupType2 ||
		my_extra->record2_typmod != tupTypmod2)
	{
		MemSet(my_extra->columns, 0, ncols * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType1;
		my_extra->record1_typmod = tupTypmod1;
		my_extra->record2_type = tupType2;
		my_extra->record2_typmod = tupTypmod2;
	}

	/* Break down the tuples into fields */
	values1 = (Datum *) palloc(ncolumns1 * sizeof(Datum));
	nulls1 = (bool *) palloc(ncolumns1 * sizeof(bool));
	heap_deform_tuple(&tuple1, tupdesc1, values1, nulls1);
	values2 = (Datum *) palloc(ncolumns2 * sizeof(Datum));
	nulls2 = (bool *) palloc(ncolumns2 * sizeof(bool));
	heap_deform_tuple(&tuple2, tupdesc2, values2, nulls2);

	/*
	 * Scan corresponding columns, allowing for dropped columns in different
	 * places in the two rows.  i1 and i2 are physical column indexes, j is
	 * the logical column index.
	 */
	i1 = i2 = j = 0;
	while (i1 < ncolumns1 || i2 < ncolumns2)
	{
		Form_pg_attribute att1;
		Form_pg_attribute att2;

		/*
		 * Skip dropped columns
		 */
		if (i1 < ncolumns1 && TupleDescAttr(tupdesc1, i1)->attisdropped)
		{
			i1++;
			continue;
		}
		if (i2 < ncolumns2 && TupleDescAttr(tupdesc2, i2)->attisdropped)
		{
			i2++;
			continue;
		}
		if (i1 >= ncolumns1 || i2 >= ncolumns2)
			break;				/* we'll deal with mismatch below loop */

		att1 = TupleDescAttr(tupdesc1, i1);
		att2 = TupleDescAttr(tupdesc2, i2);

		/*
		 * Have two matching columns, they must be same type
		 */
		if (att1->atttypid != att2->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare dissimilar column types %s and %s at record column %d",
							format_type_be(att1->atttypid),
							format_type_be(att2->atttypid),
							j + 1)));

		/*
		 * We consider two NULLs equal; NULL > not-NULL.
		 */
		if (!nulls1[i1] || !nulls2[i2])
		{
			if (nulls1[i1] || nulls2[i2])
			{
				result = false;
				break;
			}

			/* Compare the pair of elements */
			result = datum_image_eq(values1[i1], values2[i2], att1->attbyval, att2->attlen);
			if (!result)
				break;
		}

		/* equal, so continue to next column */
		i1++, i2++, j++;
	}

	/*
	 * If we didn't break out of the loop early, check for column count
	 * mismatch.  (We do not report such mismatch if we found unequal column
	 * values; is that a feature or a bug?)
	 */
	if (result)
	{
		if (i1 != ncolumns1 || i2 != ncolumns2)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare record types with different numbers of columns")));
	}

	pfree(values1);
	pfree(nulls1);
	pfree(values2);
	pfree(nulls2);
	ReleaseTupleDesc(tupdesc1);
	ReleaseTupleDesc(tupdesc2);

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(record1, 0);
	PG_FREE_IF_COPY(record2, 1);

	PG_RETURN_BOOL(result);
}

Datum
record_image_ne(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(!DatumGetBool(record_image_eq(fcinfo)));
}

Datum
record_image_lt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_image_cmp(fcinfo) < 0);
}

Datum
record_image_gt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_image_cmp(fcinfo) > 0);
}

Datum
record_image_le(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_image_cmp(fcinfo) <= 0);
}

Datum
record_image_ge(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_image_cmp(fcinfo) >= 0);
}

Datum
btrecordimagecmp(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(record_image_cmp(fcinfo));
}


/*
 * Row type hash functions
 */

Datum
hash_record(PG_FUNCTION_ARGS)
{
	HeapTupleHeader record = PG_GETARG_HEAPTUPLEHEADER(0);
	uint32		result = 0;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	int			ncolumns;
	RecordCompareData *my_extra;
	Datum	   *values;
	bool	   *nulls;

	check_stack_depth();		/* recurses for record-type columns */

	/* Extract type info from tuple */
	tupType = HeapTupleHeaderGetTypeId(record);
	tupTypmod = HeapTupleHeaderGetTypMod(record);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/* Build temporary HeapTuple control structure */
	tuple.t_len = HeapTupleHeaderGetDatumLength(record);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = record;

	/*
	 * We arrange to look up the needed hashing info just once per series
	 * of calls, assuming the record type doesn't change underneath us.
	 */
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncolumns * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncolumns;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
	}

	if (my_extra->record1_type != tupType ||
		my_extra->record1_typmod != tupTypmod)
	{
		MemSet(my_extra->columns, 0, ncolumns * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType;
		my_extra->record1_typmod = tupTypmod;
	}

	/* Break down the tuple into fields */
	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));
	heap_deform_tuple(&tuple, tupdesc, values, nulls);

	for (int i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att;
		TypeCacheEntry *typentry;
		uint32		element_hash;

		att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		/*
		 * Lookup the hash function if not done already
		 */
		typentry = my_extra->columns[i].typentry;
		if (typentry == NULL ||
			typentry->type_id != att->atttypid)
		{
			typentry = lookup_type_cache(att->atttypid,
										 TYPECACHE_HASH_PROC_FINFO);
			if (!OidIsValid(typentry->hash_proc_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify a hash function for type %s",
								format_type_be(typentry->type_id))));
			my_extra->columns[i].typentry = typentry;
		}

		/* Compute hash of element */
		if (nulls[i])
		{
			element_hash = 0;
		}
		else
		{
			LOCAL_FCINFO(locfcinfo, 1);

			InitFunctionCallInfoData(*locfcinfo, &typentry->hash_proc_finfo, 1,
									 att->attcollation, NULL, NULL);
			locfcinfo->args[0].value = values[i];
			locfcinfo->args[0].isnull = false;
			element_hash = DatumGetUInt32(FunctionCallInvoke(locfcinfo));

			/* We don't expect hash support functions to return null */
			Assert(!locfcinfo->isnull);
		}

		/* see hash_array() */
		result = (result << 5) - result + element_hash;
	}

	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(record, 0);

	PG_RETURN_UINT32(result);
}

Datum
hash_record_extended(PG_FUNCTION_ARGS)
{
	HeapTupleHeader record = PG_GETARG_HEAPTUPLEHEADER(0);
	uint64		seed = PG_GETARG_INT64(1);
	uint64		result = 0;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	int			ncolumns;
	RecordCompareData *my_extra;
	Datum	   *values;
	bool	   *nulls;

	check_stack_depth();		/* recurses for record-type columns */

	/* Extract type info from tuple */
	tupType = HeapTupleHeaderGetTypeId(record);
	tupTypmod = HeapTupleHeaderGetTypMod(record);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/* Build temporary HeapTuple control structure */
	tuple.t_len = HeapTupleHeaderGetDatumLength(record);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = record;

	/*
	 * We arrange to look up the needed hashing info just once per series
	 * of calls, assuming the record type doesn't change underneath us.
	 */
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncolumns * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncolumns;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
	}

	if (my_extra->record1_type != tupType ||
		my_extra->record1_typmod != tupTypmod)
	{
		MemSet(my_extra->columns, 0, ncolumns * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType;
		my_extra->record1_typmod = tupTypmod;
	}

	/* Break down the tuple into fields */
	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));
	heap_deform_tuple(&tuple, tupdesc, values, nulls);

	for (int i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att;
		TypeCacheEntry *typentry;
		uint64		element_hash;

		att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		/*
		 * Lookup the hash function if not done already
		 */
		typentry = my_extra->columns[i].typentry;
		if (typentry == NULL ||
			typentry->type_id != att->atttypid)
		{
			typentry = lookup_type_cache(att->atttypid,
										 TYPECACHE_HASH_EXTENDED_PROC_FINFO);
			if (!OidIsValid(typentry->hash_extended_proc_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify an extended hash function for type %s",
								format_type_be(typentry->type_id))));
			my_extra->columns[i].typentry = typentry;
		}

		/* Compute hash of element */
		if (nulls[i])
		{
			element_hash = 0;
		}
		else
		{
			LOCAL_FCINFO(locfcinfo, 2);

			InitFunctionCallInfoData(*locfcinfo, &typentry->hash_extended_proc_finfo, 2,
									 att->attcollation, NULL, NULL);
			locfcinfo->args[0].value = values[i];
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = Int64GetDatum(seed);
			locfcinfo->args[0].isnull = false;
			element_hash = DatumGetUInt64(FunctionCallInvoke(locfcinfo));

			/* We don't expect hash support functions to return null */
			Assert(!locfcinfo->isnull);
		}

		/* see hash_array_extended() */
		result = (result << 5) - result + element_hash;
	}

	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(record, 0);

	PG_RETURN_UINT64(result);
}
