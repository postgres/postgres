/*-------------------------------------------------------------------------
 *
 * quote.c
 *	  Functions for quoting identifiers and literals
 *
 * Portions Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/quote.c,v 1.17 2005/10/15 02:49:29 momjian Exp $
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
	text	   *t = PG_GETARG_TEXT_P(0);
	text	   *result;
	const char *qstr;
	char	   *str;
	int			len;

	/* We have to convert to a C string to use quote_identifier */
	len = VARSIZE(t) - VARHDRSZ;
	str = (char *) palloc(len + 1);
	memcpy(str, VARDATA(t), len);
	str[len] = '\0';

	qstr = quote_identifier(str);

	len = strlen(qstr);
	result = (text *) palloc(len + VARHDRSZ);
	VARATT_SIZEP(result) = len + VARHDRSZ;
	memcpy(VARDATA(result), qstr, len);

	PG_RETURN_TEXT_P(result);
}

/*
 * quote_literal -
 *	  returns a properly quoted literal
 */
Datum
quote_literal(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_P(0);
	text	   *result;
	char	   *cp1;
	char	   *cp2;
	int			len;

	len = VARSIZE(t) - VARHDRSZ;
	/* We make a worst-case result area; wasting a little space is OK */
	result = (text *) palloc(len * 2 + 3 + VARHDRSZ);

	cp1 = VARDATA(t);
	cp2 = VARDATA(result);

	for (; len-- > 0; cp1++)
		if (*cp1 == '\\')
		{
			*cp2++ = ESCAPE_STRING_SYNTAX;
			break;
		}

	len = VARSIZE(t) - VARHDRSZ;
	cp1 = VARDATA(t);
	*cp2++ = '\'';
	while (len-- > 0)
	{
		if (SQL_STR_DOUBLE(*cp1))
			*cp2++ = *cp1;
		*cp2++ = *cp1++;
	}
	*cp2++ = '\'';

	VARATT_SIZEP(result) = cp2 - ((char *) result);

	PG_RETURN_TEXT_P(result);
}
