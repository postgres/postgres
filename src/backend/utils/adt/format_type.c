/*-------------------------------------------------------------------------
 *
 * format_type.c
 *	  Display type names "nicely".
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/format_type.c,v 1.44 2006/07/14 14:52:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "mb/pg_wchar.h"

#define MAX_INT32_LEN 11
#define _textin(str) DirectFunctionCall1(textin, CStringGetDatum(str))

static char *format_type_internal(Oid type_oid, int32 typemod,
					 bool typemod_given, bool allow_invalid);
static char *
psnprintf(size_t len, const char *fmt,...)
/* This lets gcc check the format string for consistency. */
__attribute__((format(printf, 2, 3)));


/*
 * SQL function: format_type(type_oid, typemod)
 *
 * `type_oid' is from pg_type.oid, `typemod' is from
 * pg_attribute.atttypmod. This function will get the type name and
 * format it and the modifier to canonical SQL format, if the type is
 * a standard type. Otherwise you just get pg_type.typname back,
 * double quoted if it contains funny characters or matches a keyword.
 *
 * If typemod is NULL then we are formatting a type name in a context where
 * no typemod is available, eg a function argument or result type.	This
 * yields a slightly different result from specifying typemod = -1 in some
 * cases.  Given typemod = -1 we feel compelled to produce an output that
 * the parser will interpret as having typemod -1, so that pg_dump will
 * produce CREATE TABLE commands that recreate the original state.	But
 * given NULL typemod, we assume that the parser's interpretation of
 * typemod doesn't matter, and so we are willing to output a slightly
 * "prettier" representation of the same type.	For example, type = bpchar
 * and typemod = NULL gets you "character", whereas typemod = -1 gets you
 * "bpchar" --- the former will be interpreted as character(1) by the
 * parser, which does not yield typemod -1.
 *
 * XXX encoding a meaning in typemod = NULL is ugly; it'd have been
 * cleaner to make two functions of one and two arguments respectively.
 * Not worth changing it now, however.
 */
Datum
format_type(PG_FUNCTION_ARGS)
{
	Oid			type_oid;
	int32		typemod;
	char	   *result;

	/* Since this function is not strict, we must test for null args */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	type_oid = PG_GETARG_OID(0);

	if (PG_ARGISNULL(1))
		result = format_type_internal(type_oid, -1, false, true);
	else
	{
		typemod = PG_GETARG_INT32(1);
		result = format_type_internal(type_oid, typemod, true, true);
	}

	PG_RETURN_DATUM(_textin(result));
}

/*
 * This version is for use within the backend in error messages, etc.
 * One difference is that it will fail for an invalid type.
 *
 * The result is always a palloc'd string.
 */
char *
format_type_be(Oid type_oid)
{
	return format_type_internal(type_oid, -1, false, false);
}

/*
 * This version allows a nondefault typemod to be specified.
 */
char *
format_type_with_typemod(Oid type_oid, int32 typemod)
{
	return format_type_internal(type_oid, typemod, true, false);
}



static char *
format_type_internal(Oid type_oid, int32 typemod,
					 bool typemod_given, bool allow_invalid)
{
	bool		with_typemod = typemod_given && (typemod >= 0);
	HeapTuple	tuple;
	Form_pg_type typeform;
	Oid			array_base_type;
	bool		is_array;
	char	   *buf;

	if (type_oid == InvalidOid && allow_invalid)
		return pstrdup("-");

	tuple = SearchSysCache(TYPEOID,
						   ObjectIdGetDatum(type_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		if (allow_invalid)
			return pstrdup("???");
		else
			elog(ERROR, "cache lookup failed for type %u", type_oid);
	}
	typeform = (Form_pg_type) GETSTRUCT(tuple);

	/*
	 * Check if it's an array (and not a domain --- we don't want to show the
	 * substructure of a domain type).	Fixed-length array types such as
	 * "name" shouldn't get deconstructed either.  As of Postgres 8.1, rather
	 * than checking typlen we check the toast property, and don't deconstruct
	 * "plain storage" array types --- this is because we don't want to show
	 * oidvector as oid[].
	 */
	array_base_type = typeform->typelem;

	if (array_base_type != InvalidOid &&
		typeform->typstorage != 'p' &&
		typeform->typtype != 'd')
	{
		/* Switch our attention to the array element type */
		ReleaseSysCache(tuple);
		tuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(array_base_type),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
		{
			if (allow_invalid)
				return pstrdup("???[]");
			else
				elog(ERROR, "cache lookup failed for type %u", type_oid);
		}
		typeform = (Form_pg_type) GETSTRUCT(tuple);
		type_oid = array_base_type;
		is_array = true;
	}
	else
		is_array = false;

	/*
	 * See if we want to special-case the output for certain built-in types.
	 * Note that these special cases should all correspond to special
	 * productions in gram.y, to ensure that the type name will be taken as a
	 * system type, not a user type of the same name.
	 *
	 * If we do not provide a special-case output here, the type name will be
	 * handled the same way as a user type name --- in particular, it will be
	 * double-quoted if it matches any lexer keyword.  This behavior is
	 * essential for some cases, such as types "bit" and "char".
	 */
	buf = NULL;					/* flag for no special case */

	switch (type_oid)
	{
		case BITOID:
			if (with_typemod)
				buf = psnprintf(5 + MAX_INT32_LEN + 1, "bit(%d)",
								(int) typemod);
			else if (typemod_given)
			{
				/*
				 * bit with typmod -1 is not the same as BIT, which means
				 * BIT(1) per SQL spec.  Report it as the quoted typename so
				 * that parser will not assign a bogus typmod.
				 */
			}
			else
				buf = pstrdup("bit");
			break;

		case BOOLOID:
			buf = pstrdup("boolean");
			break;

		case BPCHAROID:
			if (with_typemod)
				buf = psnprintf(11 + MAX_INT32_LEN + 1, "character(%d)",
								(int) (typemod - VARHDRSZ));
			else if (typemod_given)
			{
				/*
				 * bpchar with typmod -1 is not the same as CHARACTER, which
				 * means CHARACTER(1) per SQL spec.  Report it as bpchar so
				 * that parser will not assign a bogus typmod.
				 */
			}
			else
				buf = pstrdup("character");
			break;

		case FLOAT4OID:
			buf = pstrdup("real");
			break;

		case FLOAT8OID:
			buf = pstrdup("double precision");
			break;

		case INT2OID:
			buf = pstrdup("smallint");
			break;

		case INT4OID:
			buf = pstrdup("integer");
			break;

		case INT8OID:
			buf = pstrdup("bigint");
			break;

		case NUMERICOID:
			if (with_typemod)
				buf = psnprintf(10 + 2 * MAX_INT32_LEN + 1, "numeric(%d,%d)",
								((typemod - VARHDRSZ) >> 16) & 0xffff,
								(typemod - VARHDRSZ) & 0xffff);
			else
				buf = pstrdup("numeric");
			break;

		case INTERVALOID:
			if (with_typemod)
			{
				int			fields = INTERVAL_RANGE(typemod);
				int			precision = INTERVAL_PRECISION(typemod);
				const char *fieldstr;

				switch (fields)
				{
					case INTERVAL_MASK(YEAR):
						fieldstr = " year";
						break;
					case INTERVAL_MASK(MONTH):
						fieldstr = " month";
						break;
					case INTERVAL_MASK(DAY):
						fieldstr = " day";
						break;
					case INTERVAL_MASK(HOUR):
						fieldstr = " hour";
						break;
					case INTERVAL_MASK(MINUTE):
						fieldstr = " minute";
						break;
					case INTERVAL_MASK(SECOND):
						fieldstr = " second";
						break;
						case INTERVAL_MASK(YEAR)
					| INTERVAL_MASK(MONTH):
						fieldstr = " year to month";
						break;
						case INTERVAL_MASK(DAY)
					| INTERVAL_MASK(HOUR):
						fieldstr = " day to hour";
						break;
						case INTERVAL_MASK(DAY)
							| INTERVAL_MASK(HOUR)
					| INTERVAL_MASK(MINUTE):
						fieldstr = " day to minute";
						break;
						case INTERVAL_MASK(DAY)
							| INTERVAL_MASK(HOUR)
							| INTERVAL_MASK(MINUTE)
					| INTERVAL_MASK(SECOND):
						fieldstr = " day to second";
						break;
						case INTERVAL_MASK(HOUR)
					| INTERVAL_MASK(MINUTE):
						fieldstr = " hour to minute";
						break;
						case INTERVAL_MASK(HOUR)
							| INTERVAL_MASK(MINUTE)
					| INTERVAL_MASK(SECOND):
						fieldstr = " hour to second";
						break;
						case INTERVAL_MASK(MINUTE)
					| INTERVAL_MASK(SECOND):
						fieldstr = " minute to second";
						break;
					case INTERVAL_FULL_RANGE:
						fieldstr = "";
						break;
					default:
						elog(ERROR, "invalid INTERVAL typmod: 0x%x", typemod);
						fieldstr = "";
						break;
				}
				if (precision != INTERVAL_FULL_PRECISION)
					buf = psnprintf(100, "interval(%d)%s",
									precision, fieldstr);
				else
					buf = psnprintf(100, "interval%s",
									fieldstr);
			}
			else
				buf = pstrdup("interval");
			break;

		case TIMEOID:
			if (with_typemod)
				buf = psnprintf(50, "time(%d) without time zone",
								typemod);
			else
				buf = pstrdup("time without time zone");
			break;

		case TIMETZOID:
			if (with_typemod)
				buf = psnprintf(50, "time(%d) with time zone",
								typemod);
			else
				buf = pstrdup("time with time zone");
			break;

		case TIMESTAMPOID:
			if (with_typemod)
				buf = psnprintf(50, "timestamp(%d) without time zone",
								typemod);
			else
				buf = pstrdup("timestamp without time zone");
			break;

		case TIMESTAMPTZOID:
			if (with_typemod)
				buf = psnprintf(50, "timestamp(%d) with time zone",
								typemod);
			else
				buf = pstrdup("timestamp with time zone");
			break;

		case VARBITOID:
			if (with_typemod)
				buf = psnprintf(13 + MAX_INT32_LEN + 1, "bit varying(%d)",
								(int) typemod);
			else
				buf = pstrdup("bit varying");
			break;

		case VARCHAROID:
			if (with_typemod)
				buf = psnprintf(19 + MAX_INT32_LEN + 1,
								"character varying(%d)",
								(int) (typemod - VARHDRSZ));
			else
				buf = pstrdup("character varying");
			break;
	}

	if (buf == NULL)
	{
		/*
		 * Default handling: report the name as it appears in the catalog.
		 * Here, we must qualify the name if it is not visible in the search
		 * path, and we must double-quote it if it's not a standard identifier
		 * or if it matches any keyword.
		 */
		char	   *nspname;
		char	   *typname;

		if (TypeIsVisible(type_oid))
			nspname = NULL;
		else
			nspname = get_namespace_name(typeform->typnamespace);

		typname = NameStr(typeform->typname);

		buf = quote_qualified_identifier(nspname, typname);
	}

	if (is_array)
		buf = psnprintf(strlen(buf) + 3, "%s[]", buf);

	ReleaseSysCache(tuple);

	return buf;
}


/*
 * type_maximum_size --- determine maximum width of a variable-width column
 *
 * If the max width is indeterminate, return -1.  In particular, we return
 * -1 for any type not known to this routine.  We assume the caller has
 * already determined that the type is a variable-width type, so it's not
 * necessary to look up the type's pg_type tuple here.
 *
 * This may appear unrelated to format_type(), but in fact the two routines
 * share knowledge of the encoding of typmod for different types, so it's
 * convenient to keep them together.
 */
int32
type_maximum_size(Oid type_oid, int32 typemod)
{
	if (typemod < 0)
		return -1;

	switch (type_oid)
	{
		case BPCHAROID:
		case VARCHAROID:
			/* typemod includes varlena header */

			/* typemod is in characters not bytes */
			return (typemod - VARHDRSZ) *
				pg_encoding_max_length(GetDatabaseEncoding())
				+ VARHDRSZ;

		case NUMERICOID:
			/* precision (ie, max # of digits) is in upper bits of typmod */
			if (typemod > VARHDRSZ)
			{
				int			precision = ((typemod - VARHDRSZ) >> 16) & 0xffff;

				/* Numeric stores 2 decimal digits/byte, plus header */
				return (precision + 1) / 2 + NUMERIC_HDRSZ;
			}
			break;

		case VARBITOID:
		case BITOID:
			/* typemod is the (max) number of bits */
			return (typemod + (BITS_PER_BYTE - 1)) / BITS_PER_BYTE
				+ 2 * sizeof(int32);
	}

	/* Unknown type, or unlimited-width type such as 'text' */
	return -1;
}


/*
 * oidvectortypes			- converts a vector of type OIDs to "typname" list
 */
Datum
oidvectortypes(PG_FUNCTION_ARGS)
{
	oidvector  *oidArray = (oidvector *) PG_GETARG_POINTER(0);
	char	   *result;
	int			numargs = oidArray->dim1;
	int			num;
	size_t		total;
	size_t		left;

	total = 20 * numargs + 1;
	result = palloc(total);
	result[0] = '\0';
	left = total - 1;

	for (num = 0; num < numargs; num++)
	{
		char	   *typename = format_type_internal(oidArray->values[num], -1,
													false, true);
		size_t		slen = strlen(typename);

		if (left < (slen + 2))
		{
			total += slen + 2;
			result = repalloc(result, total);
			left += slen + 2;
		}

		if (num > 0)
		{
			strcat(result, ", ");
			left -= 2;
		}
		strcat(result, typename);
		left -= slen;
	}

	PG_RETURN_DATUM(_textin(result));
}


/* snprintf into a palloc'd string */
static char *
psnprintf(size_t len, const char *fmt,...)
{
	va_list		ap;
	char	   *buf;

	buf = palloc(len);

	va_start(ap, fmt);
	vsnprintf(buf, len, fmt, ap);
	va_end(ap);

	return buf;
}
