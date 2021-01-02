/*-------------------------------------------------------------------------
 *
 * quote.c
 *	  Functions for quoting identifiers and literals
 *
 * Portions Copyright (c) 2000-2021, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/quote.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/builtins.h"


/*
 * quote_ident -
 *	  returns a properly quoted identifier
 */
Datum
quote_ident(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	const char *qstr;
	char	   *str;

	str = text_to_cstring(t);
	qstr = quote_identifier(str);
	PG_RETURN_TEXT_P(cstring_to_text(qstr));
}

/*
 * quote_literal_internal -
 *	  helper function for quote_literal and quote_literal_cstr
 *
 * NOTE: think not to make this function's behavior change with
 * standard_conforming_strings.  We don't know where the result
 * literal will be used, and so we must generate a result that
 * will work with either setting.  Take a look at what dblink
 * uses this for before thinking you know better.
 */
static size_t
quote_literal_internal(char *dst, const char *src, size_t len)
{
	const char *s;
	char	   *savedst = dst;

	for (s = src; s < src + len; s++)
	{
		if (*s == '\\')
		{
			*dst++ = ESCAPE_STRING_SYNTAX;
			break;
		}
	}

	*dst++ = '\'';
	while (len-- > 0)
	{
		if (SQL_STR_DOUBLE(*src, true))
			*dst++ = *src;
		*dst++ = *src++;
	}
	*dst++ = '\'';

	return dst - savedst;
}

/*
 * quote_literal -
 *	  returns a properly quoted literal
 */
Datum
quote_literal(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	text	   *result;
	char	   *cp1;
	char	   *cp2;
	int			len;

	len = VARSIZE_ANY_EXHDR(t);
	/* We make a worst-case result area; wasting a little space is OK */
	result = (text *) palloc(len * 2 + 3 + VARHDRSZ);

	cp1 = VARDATA_ANY(t);
	cp2 = VARDATA(result);

	SET_VARSIZE(result, VARHDRSZ + quote_literal_internal(cp2, cp1, len));

	PG_RETURN_TEXT_P(result);
}

/*
 * quote_literal_cstr -
 *	  returns a properly quoted literal
 */
char *
quote_literal_cstr(const char *rawstr)
{
	char	   *result;
	int			len;
	int			newlen;

	len = strlen(rawstr);
	/* We make a worst-case result area; wasting a little space is OK */
	result = palloc(len * 2 + 3 + 1);

	newlen = quote_literal_internal(result, rawstr, len);
	result[newlen] = '\0';

	return result;
}

/*
 * quote_nullable -
 *	  Returns a properly quoted literal, with null values returned
 *	  as the text string 'NULL'.
 */
Datum
quote_nullable(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_TEXT_P(cstring_to_text("NULL"));
	else
		PG_RETURN_DATUM(DirectFunctionCall1(quote_literal,
											PG_GETARG_DATUM(0)));
}
