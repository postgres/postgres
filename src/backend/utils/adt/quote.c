/*-------------------------------------------------------------------------
 *
 * quote.c
 *	  Functions for quoting identifiers and literals
 *
 * Portions Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/quote.c,v 1.11 2003/08/04 23:59:39 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "mb/pg_wchar.h"
#include "utils/builtins.h"


static bool quote_ident_required(text *iptr);
static text *do_quote_ident(text *iptr);
static text *do_quote_literal(text *iptr);


/*
 * quote_ident -
 *	  returns a properly quoted identifier
 */
Datum
quote_ident(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_P(0);
	text	   *result;

	if (quote_ident_required(t))
		result = do_quote_ident(t);
	else
	{
		result = (text *) palloc(VARSIZE(t));
		memcpy(result, t, VARSIZE(t));
	}

	PG_FREE_IF_COPY(t, 0);

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

	result = do_quote_literal(t);

	PG_FREE_IF_COPY(t, 0);

	PG_RETURN_TEXT_P(result);
}

/*
 * Check if a given identifier needs quoting
 */
static bool
quote_ident_required(text *iptr)
{
	char	   *cp;
	char	   *ep;

	cp = VARDATA(iptr);
	ep = VARDATA(iptr) + VARSIZE(iptr) - VARHDRSZ;

	if (cp >= ep)
		return true;

	if (pg_mblen(cp) != 1)
		return true;
	if (!(*cp == '_' || (*cp >= 'a' && *cp <= 'z')))
		return true;

	while ((++cp) < ep)
	{
		if (pg_mblen(cp) != 1)
			return true;

		if (*cp >= 'a' && *cp <= 'z')
			continue;
		if (*cp >= '0' && *cp <= '9')
			continue;
		if (*cp == '_')
			continue;

		return true;
	}

	return false;
}

/*
 * Return a properly quoted identifier
 */
static text *
do_quote_ident(text *iptr)
{
	text	   *result;
	char	   *cp1;
	char	   *cp2;
	int			len;
	int			wl;

	len = VARSIZE(iptr) - VARHDRSZ;
	result = (text *) palloc(len * 2 + VARHDRSZ + 2);

	cp1 = VARDATA(iptr);
	cp2 = VARDATA(result);

	*cp2++ = '"';
	while (len > 0)
	{
		if ((wl = pg_mblen(cp1)) != 1)
		{
			len -= wl;

			while (wl-- > 0)
				*cp2++ = *cp1++;
			continue;
		}

		if (*cp1 == '"')
			*cp2++ = '"';
		*cp2++ = *cp1++;

		len--;
	}
	*cp2++ = '"';

	VARATT_SIZEP(result) = cp2 - ((char *) result);

	return result;
}

/*
 * Return a properly quoted literal value
 */
static text *
do_quote_literal(text *lptr)
{
	text	   *result;
	char	   *cp1;
	char	   *cp2;
	int			len;
	int			wl;

	len = VARSIZE(lptr) - VARHDRSZ;
	result = (text *) palloc(len * 2 + VARHDRSZ + 2);

	cp1 = VARDATA(lptr);
	cp2 = VARDATA(result);

	*cp2++ = '\'';
	while (len > 0)
	{
		if ((wl = pg_mblen(cp1)) != 1)
		{
			len -= wl;

			while (wl-- > 0)
				*cp2++ = *cp1++;
			continue;
		}

		if (*cp1 == '\'')
			*cp2++ = '\'';
		if (*cp1 == '\\')
			*cp2++ = '\\';
		*cp2++ = *cp1++;

		len--;
	}
	*cp2++ = '\'';

	VARATT_SIZEP(result) = cp2 - ((char *) result);

	return result;
}
