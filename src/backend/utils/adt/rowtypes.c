/*-------------------------------------------------------------------------
 *
 * rowtypes.c
 *	  I/O functions for generic composite types.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/rowtypes.c,v 1.2 2004/06/06 04:50:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/heapam.h"
#include "access/htup.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
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
	FmgrInfo	proc;
} ColumnIOData;

typedef struct RecordIOData
{
	Oid			record_type;
	int32		record_typmod;
	int			ncolumns;
	ColumnIOData columns[1];	/* VARIABLE LENGTH ARRAY */
} RecordIOData;


/*
 * record_in		- input routine for any composite type.
 */
Datum
record_in(PG_FUNCTION_ARGS)
{
	char	   *string = PG_GETARG_CSTRING(0);
	Oid			tupType = PG_GETARG_OID(1);
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	RecordIOData *my_extra;
	int			ncolumns;
	int			i;
	char	   *ptr;
	Datum	   *values;
	char	   *nulls;
	StringInfoData buf;

	/*
	 * Use the passed type unless it's RECORD; we can't support input
	 * of anonymous types, mainly because there's no good way to figure
	 * out which anonymous type is wanted.  Note that for RECORD,
	 * what we'll probably actually get is RECORD's typelem, ie, zero.
	 */
	if (tupType == InvalidOid || tupType == RECORDOID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("input of anonymous composite types is not implemented")));
	tupdesc = lookup_rowtype_tupdesc(tupType, -1);
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
							   sizeof(RecordIOData) - sizeof(ColumnIOData)
							   + ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = -1;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != -1)
	{
		MemSet(my_extra, 0,
			   sizeof(RecordIOData) - sizeof(ColumnIOData)
			   + ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = -1;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (char *) palloc(ncolumns * sizeof(char));

	/*
	 * Scan the string.
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
		ColumnIOData *column_info = &my_extra->columns[i];

		/* Check for null */
		if (*ptr == ',' || *ptr == ')')
		{
			values[i] = (Datum) 0;
			nulls[i] = 'n';
		}
		else
		{
			/* Extract string for this column */
			bool	inquote = false;

			buf.len = 0;
			buf.data[0] = '\0';
			while (inquote || !(*ptr == ',' || *ptr == ')'))
			{
				char ch = *ptr++;

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
				else if (ch == '\"')
				{
					if (!inquote)
						inquote = true;
					else if (*ptr == '\"')
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

			/*
			 * Convert the column value
			 */
			if (column_info->column_type != tupdesc->attrs[i]->atttypid)
			{
				getTypeInputInfo(tupdesc->attrs[i]->atttypid,
								 &column_info->typiofunc,
								 &column_info->typioparam);
				fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
							  fcinfo->flinfo->fn_mcxt);
				column_info->column_type = tupdesc->attrs[i]->atttypid;
			}

			values[i] = FunctionCall3(&column_info->proc,
									  CStringGetDatum(buf.data),
									  ObjectIdGetDatum(column_info->typioparam),
									  Int32GetDatum(tupdesc->attrs[i]->atttypmod));
			nulls[i] = ' ';
		}

		/*
		 * Prep for next column
		 */
		if (*ptr == ',')
		{
			if (i == ncolumns-1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed record literal: \"%s\"", string),
						 errdetail("Too many columns.")));
			ptr++;
		}
		else
		{
			/* *ptr must be ')' */
			if (i < ncolumns-1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed record literal: \"%s\"", string),
						 errdetail("Too few columns.")));
		}
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

	tuple = heap_formtuple(tupdesc, values, nulls);

	pfree(buf.data);
	pfree(values);
	pfree(nulls);

	PG_RETURN_HEAPTUPLEHEADER(tuple->t_data);
}

/*
 * record_out		- output routine for any composite type.
 */
Datum
record_out(PG_FUNCTION_ARGS)
{
	HeapTupleHeader rec = PG_GETARG_HEAPTUPLEHEADER(0);
	Oid			tupType = PG_GETARG_OID(1);
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	RecordIOData *my_extra;
	int			ncolumns;
	int			i;
	Datum	   *values;
	char	   *nulls;
	StringInfoData buf;

	/*
	 * Use the passed type unless it's RECORD; in that case, we'd better
	 * get the type info out of the datum itself.  Note that for RECORD,
	 * what we'll probably actually get is RECORD's typelem, ie, zero.
	 */
	if (tupType == InvalidOid || tupType == RECORDOID)
	{
		tupType = HeapTupleHeaderGetTypeId(rec);
		tupTypmod = HeapTupleHeaderGetTypMod(rec);
	}
	else
		tupTypmod = -1;
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
							   sizeof(RecordIOData) - sizeof(ColumnIOData)
							   + ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = -1;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   sizeof(RecordIOData) - sizeof(ColumnIOData)
			   + ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	/* Break down the tuple into fields */
	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (char *) palloc(ncolumns * sizeof(char));
	heap_deformtuple(&tuple, tupdesc, values, nulls);

	/* And build the result string */
	initStringInfo(&buf);

	appendStringInfoChar(&buf, '(');

	for (i = 0; i < ncolumns; i++)
	{
		ColumnIOData *column_info = &my_extra->columns[i];
		char	*value;
		char	*tmp;
		bool	nq;

		if (i > 0)
			appendStringInfoChar(&buf, ',');

		if (nulls[i] == 'n')
		{
			/* emit nothing... */
			continue;
		}

		/*
		 * Convert the column value
		 */
		if (column_info->column_type != tupdesc->attrs[i]->atttypid)
		{
			bool	typIsVarlena;

			getTypeOutputInfo(tupdesc->attrs[i]->atttypid,
							  &column_info->typiofunc,
							  &column_info->typioparam,
							  &typIsVarlena);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = tupdesc->attrs[i]->atttypid;
		}

		value = DatumGetCString(FunctionCall3(&column_info->proc,
											  values[i],
											  ObjectIdGetDatum(column_info->typioparam),
											  Int32GetDatum(tupdesc->attrs[i]->atttypmod)));

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

		if (nq)
			appendStringInfoChar(&buf, '"');
		for (tmp = value; *tmp; tmp++)
		{
			char		ch = *tmp;

			if (ch == '"' || ch == '\\')
				appendStringInfoChar(&buf, '\\');
			appendStringInfoChar(&buf, ch);
		}
		if (nq)
			appendStringInfoChar(&buf, '"');
	}

	appendStringInfoChar(&buf, ')');

	pfree(values);
	pfree(nulls);

	PG_RETURN_CSTRING(buf.data);
}

/*
 * record_recv		- binary input routine for any composite type.
 */
Datum
record_recv(PG_FUNCTION_ARGS)
{
	/* Need to decide on external format before we can write this */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("input of composite types not implemented yet")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * record_send		- binary output routine for any composite type.
 */
Datum
record_send(PG_FUNCTION_ARGS)
{
	/* Need to decide on external format before we can write this */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("output of composite types not implemented yet")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}
