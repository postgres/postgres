/* $Header: /cvsroot/pgsql/src/backend/utils/adt/format_type.c,v 1.1 2000/07/07 19:24:37 petere Exp $ */

#include "postgres.h"

#include <ctype.h>
#include <stdarg.h>

#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

#define streq(a, b) (strcmp((a), (b))==0)
#define MAX_INT32_LEN 11


static char *
psnprintf(size_t len, const char * fmt, ...)
{
	va_list ap;
	char * buf;

	buf = palloc(len);

	va_start(ap, fmt);
	vsnprintf(buf, len, fmt, ap);
	va_end(ap);

	return buf;
}


#define _textin(str) DirectFunctionCall1(textin, CStringGetDatum(str))


/*
 * SQL function: format_type(type_oid, typemod)
 *
 * `type_oid' is from pg_type.oid, `typemod' is from
 * pg_attribute.atttypmod. This function will get the type name and
 * format it and the modifier to canonical SQL format, if the type is
 * a standard type. Otherwise you just get pg_type.typname back,
 * double quoted if it contains funny characters.
 *
 * If typemod is null (in the SQL sense) then you won't get any
 * "..(x)" type qualifiers. The result is not technically correct,
 * because the various types interpret missing type modifiers
 * differently, but it can be used as a convenient way to format
 * system catalogs, e.g., pg_aggregate, in psql.
 */
Datum
format_type(PG_FUNCTION_ARGS)
{
	Oid type_oid;
	bool with_typemod;
	int32 typemod = 0;
	char * buf;
	char * name;
	Oid array_base_type;
	int16 typlen;
	bool is_array;
	HeapTuple tuple;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	type_oid = DatumGetObjectId(PG_GETARG_DATUM(0));

	with_typemod = !PG_ARGISNULL(1);
	if (with_typemod)
		typemod = PG_GETARG_INT32(1);

	tuple = SearchSysCacheTuple(TYPEOID, ObjectIdGetDatum(type_oid),
								0, 0, 0);

	if (!HeapTupleIsValid(tuple))
		PG_RETURN_TEXT_P(_textin("???"));

	array_base_type = ((Form_pg_type) GETSTRUCT(tuple))->typelem;
	typlen = ((Form_pg_type) GETSTRUCT(tuple))->typlen;
	if (array_base_type != 0 && typlen < 0)
	{
		tuple = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(array_base_type),
									0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			PG_RETURN_TEXT_P(_textin("???[]"));
		is_array = true;
	}
	else
		is_array = false;
	

	name =  NameStr(((Form_pg_type) GETSTRUCT(tuple))->typname);

	if (streq(name, "bit"))
	{
		if (with_typemod)
			buf = psnprintf(5 + MAX_INT32_LEN + 1, "bit(%d)", (int) typemod - 4);
		else
			buf = pstrdup("bit");
	}

	else if (streq(name, "bool"))
		buf = pstrdup("boolean");

	else if (streq(name, "bpchar"))
	{
		if (with_typemod)
			buf = psnprintf(11 + MAX_INT32_LEN + 1, "character(%d)", (int) typemod - 4);
		else
			buf = pstrdup("character");
	}

	/* This char type is the single-byte version. You have to
	 * double-quote it to get at it in the parser. */
	else if (streq(name, "char"))
		buf = pstrdup("\"char\"");
#if 0
	/* The parser has these backwards, so leave as is for now. */
	else if (streq(name, "float4"))
		buf = pstrdup("real");

	else if (streq(name, "float8"))
		buf = pstrdup("double precision");
#endif
	else if (streq(name, "int2"))
		buf = pstrdup("smallint");

	else if (streq(name, "int4"))
		buf = pstrdup("integer");

	else if (streq(name, "int8"))
		buf = pstrdup("bigint");

	else if (streq(name, "numeric"))
	{
		if (with_typemod)
			buf = psnprintf(10 + 2 * MAX_INT32_LEN + 1, "numeric(%d,%d)",
							((typemod - VARHDRSZ) >> 16) & 0xffff,
							(typemod - VARHDRSZ) & 0xffff);
		else
			buf = pstrdup("numeric");
	}

	else if (streq(name, "timetz"))
		buf = pstrdup("time with time zone");

	else if (streq(name, "varbit"))
	{
		if (with_typemod)
			buf = psnprintf(13 + MAX_INT32_LEN + 1, "bit varying(%d)", (int) typemod - 4);
		else
			buf = pstrdup("bit varying");
	}

	else if (streq(name, "varchar"))
	{
		if (with_typemod)
			buf = psnprintf(19 + MAX_INT32_LEN + 1, "character varying(%d)", (int) typemod - 4);
		else
			buf = pstrdup("character varying");
	}

	else
	{
		if (strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_") != strlen(name)
			|| isdigit((int) name[0]))
			buf = psnprintf(strlen(name) + 3, "\"%s\"", name);
		else
			buf = name;
	}

	if (is_array)
	{
		char * buf2 = psnprintf(strlen(buf) + 3, "%s[]", buf);
		buf = buf2;
	}

	PG_RETURN_TEXT_P(_textin(buf));
}
