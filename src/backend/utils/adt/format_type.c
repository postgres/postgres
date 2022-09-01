/*-------------------------------------------------------------------------
 *
 * format_type.c
 *	  Display type names "nicely".
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/format_type.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/syscache.h"

static char *printTypmod(const char *typname, int32 typmod, Oid typmodout);


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
 * no typemod is available, eg a function argument or result type.  This
 * yields a slightly different result from specifying typemod = -1 in some
 * cases.  Given typemod = -1 we feel compelled to produce an output that
 * the parser will interpret as having typemod -1, so that pg_dump will
 * produce CREATE TABLE commands that recreate the original state.  But
 * given NULL typemod, we assume that the parser's interpretation of
 * typemod doesn't matter, and so we are willing to output a slightly
 * "prettier" representation of the same type.  For example, type = bpchar
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
	bits16		flags = FORMAT_TYPE_ALLOW_INVALID;

	/* Since this function is not strict, we must test for null args */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	type_oid = PG_GETARG_OID(0);

	if (PG_ARGISNULL(1))
		typemod = -1;
	else
	{
		typemod = PG_GETARG_INT32(1);
		flags |= FORMAT_TYPE_TYPEMOD_GIVEN;
	}

	result = format_type_extended(type_oid, typemod, flags);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * format_type_extended
 *		Generate a possibly-qualified type name.
 *
 * The default behavior is to only qualify if the type is not in the search
 * path, to ignore the given typmod, and to raise an error if a non-existent
 * type_oid is given.
 *
 * The following bits in 'flags' modify the behavior:
 * - FORMAT_TYPE_TYPEMOD_GIVEN
 *			include the typmod in the output (typmod could still be -1 though)
 * - FORMAT_TYPE_ALLOW_INVALID
 *			if the type OID is invalid or unknown, return ??? or such instead
 *			of failing
 * - FORMAT_TYPE_INVALID_AS_NULL
 *			if the type OID is invalid or unknown, return NULL instead of ???
 *			or such
 * - FORMAT_TYPE_FORCE_QUALIFY
 *			always schema-qualify type names, regardless of search_path
 *
 * Note that TYPEMOD_GIVEN is not interchangeable with "typemod == -1";
 * see the comments above for format_type().
 *
 * Returns a palloc'd string, or NULL.
 */
char *
format_type_extended(Oid type_oid, int32 typemod, bits16 flags)
{
	HeapTuple	tuple;
	Form_pg_type typeform;
	Oid			array_base_type;
	bool		is_array;
	char	   *buf;
	bool		with_typemod;

	if (type_oid == InvalidOid)
	{
		if ((flags & FORMAT_TYPE_INVALID_AS_NULL) != 0)
			return NULL;
		else if ((flags & FORMAT_TYPE_ALLOW_INVALID) != 0)
			return pstrdup("-");
	}

	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tuple))
	{
		if ((flags & FORMAT_TYPE_INVALID_AS_NULL) != 0)
			return NULL;
		else if ((flags & FORMAT_TYPE_ALLOW_INVALID) != 0)
			return pstrdup("???");
		else
			elog(ERROR, "cache lookup failed for type %u", type_oid);
	}
	typeform = (Form_pg_type) GETSTRUCT(tuple);

	/*
	 * Check if it's a "true" array type.  Pseudo-array types such as "name"
	 * shouldn't get deconstructed.  Also check the toast property, and don't
	 * deconstruct "plain storage" array types --- this is because we don't
	 * want to show oidvector as oid[].
	 */
	array_base_type = typeform->typelem;

	if (IsTrueArrayType(typeform) &&
		typeform->typstorage != TYPSTORAGE_PLAIN)
	{
		/* Switch our attention to the array element type */
		ReleaseSysCache(tuple);
		tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(array_base_type));
		if (!HeapTupleIsValid(tuple))
		{
			if ((flags & FORMAT_TYPE_INVALID_AS_NULL) != 0)
				return NULL;
			else if ((flags & FORMAT_TYPE_ALLOW_INVALID) != 0)
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

	with_typemod = (flags & FORMAT_TYPE_TYPEMOD_GIVEN) != 0 && (typemod >= 0);

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
				buf = printTypmod("bit", typemod, typeform->typmodout);
			else if ((flags & FORMAT_TYPE_TYPEMOD_GIVEN) != 0)
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
				buf = printTypmod("character", typemod, typeform->typmodout);
			else if ((flags & FORMAT_TYPE_TYPEMOD_GIVEN) != 0)
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
				buf = printTypmod("numeric", typemod, typeform->typmodout);
			else
				buf = pstrdup("numeric");
			break;

		case INTERVALOID:
			if (with_typemod)
				buf = printTypmod("interval", typemod, typeform->typmodout);
			else
				buf = pstrdup("interval");
			break;

		case TIMEOID:
			if (with_typemod)
				buf = printTypmod("time", typemod, typeform->typmodout);
			else
				buf = pstrdup("time without time zone");
			break;

		case TIMETZOID:
			if (with_typemod)
				buf = printTypmod("time", typemod, typeform->typmodout);
			else
				buf = pstrdup("time with time zone");
			break;

		case TIMESTAMPOID:
			if (with_typemod)
				buf = printTypmod("timestamp", typemod, typeform->typmodout);
			else
				buf = pstrdup("timestamp without time zone");
			break;

		case TIMESTAMPTZOID:
			if (with_typemod)
				buf = printTypmod("timestamp", typemod, typeform->typmodout);
			else
				buf = pstrdup("timestamp with time zone");
			break;

		case VARBITOID:
			if (with_typemod)
				buf = printTypmod("bit varying", typemod, typeform->typmodout);
			else
				buf = pstrdup("bit varying");
			break;

		case VARCHAROID:
			if (with_typemod)
				buf = printTypmod("character varying", typemod, typeform->typmodout);
			else
				buf = pstrdup("character varying");
			break;
	}

	if (buf == NULL)
	{
		/*
		 * Default handling: report the name as it appears in the catalog.
		 * Here, we must qualify the name if it is not visible in the search
		 * path or if caller requests it; and we must double-quote it if it's
		 * not a standard identifier or if it matches any keyword.
		 */
		char	   *nspname;
		char	   *typname;

		if ((flags & FORMAT_TYPE_FORCE_QUALIFY) == 0 &&
			TypeIsVisible(type_oid))
			nspname = NULL;
		else
			nspname = get_namespace_name_or_temp(typeform->typnamespace);

		typname = NameStr(typeform->typname);

		buf = quote_qualified_identifier(nspname, typname);

		if (with_typemod)
			buf = printTypmod(buf, typemod, typeform->typmodout);
	}

	if (is_array)
		buf = psprintf("%s[]", buf);

	ReleaseSysCache(tuple);

	return buf;
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
	return format_type_extended(type_oid, -1, 0);
}

/*
 * This version returns a name that is always qualified (unless it's one
 * of the SQL-keyword type names, such as TIMESTAMP WITH TIME ZONE).
 */
char *
format_type_be_qualified(Oid type_oid)
{
	return format_type_extended(type_oid, -1, FORMAT_TYPE_FORCE_QUALIFY);
}

/*
 * This version allows a nondefault typemod to be specified.
 */
char *
format_type_with_typemod(Oid type_oid, int32 typemod)
{
	return format_type_extended(type_oid, typemod, FORMAT_TYPE_TYPEMOD_GIVEN);
}

/*
 * Add typmod decoration to the basic type name
 */
static char *
printTypmod(const char *typname, int32 typmod, Oid typmodout)
{
	char	   *res;

	/* Shouldn't be called if typmod is -1 */
	Assert(typmod >= 0);

	if (typmodout == InvalidOid)
	{
		/* Default behavior: just print the integer typmod with parens */
		res = psprintf("%s(%d)", typname, (int) typmod);
	}
	else
	{
		/* Use the type-specific typmodout procedure */
		char	   *tmstr;

		tmstr = DatumGetCString(OidFunctionCall1(typmodout,
												 Int32GetDatum(typmod)));
		res = psprintf("%s%s", typname, tmstr);
	}

	return res;
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
 * convenient to keep them together.  (XXX now that most of this knowledge
 * has been pushed out of format_type into the typmodout functions, it's
 * interesting to wonder if it's worth trying to factor this code too...)
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
			return numeric_maximum_size(typemod);

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
		char	   *typename = format_type_extended(oidArray->values[num], -1,
													FORMAT_TYPE_ALLOW_INVALID);
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

	PG_RETURN_TEXT_P(cstring_to_text(result));
}
