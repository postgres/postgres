/*
 * This file contains some public functions
 * usable for both the backend and the frontend.
 * Tatsuo Ishii
 * $Id: common.c,v 1.12 2001/02/11 01:59:22 ishii Exp $
 */
#include "postgres.h"

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#endif

#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"

#ifndef FRONTEND
/*
 * convert encoding char to encoding symbol value.
 * case is ignored.
 * if there's no valid encoding, returns -1
 */
int
pg_char_to_encoding(const char *s)
{
	pg_encoding_conv_tbl *p = pg_conv_tbl;

	if (!s)
		return (-1);

	for (; p->encoding >= 0; p++)
	{
		if (!strcasecmp(s, p->name))
			break;
	}
	return (p->encoding);
}

/* Same, as an fmgr-callable function */
Datum
PG_char_to_encoding(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);

	PG_RETURN_INT32(pg_char_to_encoding(NameStr(*s)));
}

/*
 * check to see if encoding name is valid
 */
int
pg_valid_client_encoding(const char *name)
{
	return (pg_char_to_encoding(name));
}

/*
 * find encoding table entry by encoding
 */
pg_encoding_conv_tbl *
pg_get_encent_by_encoding(int encoding)
{
	pg_encoding_conv_tbl *p = pg_conv_tbl;

	for (; p->encoding >= 0; p++)
	{
		if (p->encoding == encoding)
			return (p);
	}
	return (0);
}

/*
 * convert encoding symbol to encoding char.
 * if there's no valid encoding symbol, returns ""
 */
const char *
pg_encoding_to_char(int encoding)
{
	pg_encoding_conv_tbl *p = pg_get_encent_by_encoding(encoding);

	if (!p)
		return ("");
	return (p->name);
}

/* Same, as an fmgr-callable function */
Datum
PG_encoding_to_char(PG_FUNCTION_ARGS)
{
	int32		encoding = PG_GETARG_INT32(0);

	PG_RETURN_NAME(pg_encoding_to_char(encoding));
}

#endif
