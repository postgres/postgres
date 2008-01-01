/*-------------------------------------------------------------------------
 *
 * bool.c
 *	  Functions for the built-in type "bool".
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/bool.c,v 1.42 2008/01/01 19:45:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "libpq/pqformat.h"
#include "utils/builtins.h"

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		boolin			- converts "t" or "f" to 1 or 0
 *
 * Check explicitly for "true/false" and TRUE/FALSE, 1/0, YES/NO.
 * Reject other values. - thomas 1997-10-05
 *
 * In the switch statement, check the most-used possibilities first.
 */
Datum
boolin(PG_FUNCTION_ARGS)
{
	const char *in_str = PG_GETARG_CSTRING(0);
	const char *str;
	size_t		len;

	/*
	 * Skip leading and trailing whitespace
	 */
	str = in_str;
	while (isspace((unsigned char) *str))
		str++;

	len = strlen(str);
	while (len > 0 && isspace((unsigned char) str[len - 1]))
		len--;

	switch (*str)
	{
		case 't':
		case 'T':
			if (pg_strncasecmp(str, "true", len) == 0)
				PG_RETURN_BOOL(true);
			break;

		case 'f':
		case 'F':
			if (pg_strncasecmp(str, "false", len) == 0)
				PG_RETURN_BOOL(false);
			break;

		case 'y':
		case 'Y':
			if (pg_strncasecmp(str, "yes", len) == 0)
				PG_RETURN_BOOL(true);
			break;

		case '1':
			if (pg_strncasecmp(str, "1", len) == 0)
				PG_RETURN_BOOL(true);
			break;

		case 'n':
		case 'N':
			if (pg_strncasecmp(str, "no", len) == 0)
				PG_RETURN_BOOL(false);
			break;

		case '0':
			if (pg_strncasecmp(str, "0", len) == 0)
				PG_RETURN_BOOL(false);
			break;

		default:
			break;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
		   errmsg("invalid input syntax for type boolean: \"%s\"", in_str)));

	/* not reached */
	PG_RETURN_BOOL(false);
}

/*
 *		boolout			- converts 1 or 0 to "t" or "f"
 */
Datum
boolout(PG_FUNCTION_ARGS)
{
	bool		b = PG_GETARG_BOOL(0);
	char	   *result = (char *) palloc(2);

	result[0] = (b) ? 't' : 'f';
	result[1] = '\0';
	PG_RETURN_CSTRING(result);
}

/*
 *		boolrecv			- converts external binary format to bool
 *
 * The external representation is one byte.  Any nonzero value is taken
 * as "true".
 */
Datum
boolrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int			ext;

	ext = pq_getmsgbyte(buf);
	PG_RETURN_BOOL((ext != 0) ? true : false);
}

/*
 *		boolsend			- converts bool to binary format
 */
Datum
boolsend(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, arg1 ? 1 : 0);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 *		booltext			- cast function for bool => text
 *
 * We need this because it's different from the behavior of boolout();
 * this function follows the SQL-spec result (except for producing lower case)
 */
Datum
booltext(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	char	   *str;

	if (arg1)
		str = "true";
	else
		str = "false";

	PG_RETURN_DATUM(DirectFunctionCall1(textin, CStringGetDatum(str)));
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

Datum
booleq(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
boolne(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
boollt(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
boolgt(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
boolle(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
boolge(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

/*
 * Per SQL92, istrue() and isfalse() should return false, not NULL,
 * when presented a NULL input (since NULL is our implementation of
 * UNKNOWN).  Conversely isnottrue() and isnotfalse() should return true.
 * Therefore, these routines are all declared not-strict in pg_proc
 * and must do their own checking for null inputs.
 *
 * Note we don't need isunknown() and isnotunknown() functions, since
 * nullvalue() and nonnullvalue() will serve.
 */

Datum
istrue(PG_FUNCTION_ARGS)
{
	bool		b;

	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(false);

	b = PG_GETARG_BOOL(0);

	PG_RETURN_BOOL(b);
}

Datum
isfalse(PG_FUNCTION_ARGS)
{
	bool		b;

	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(false);

	b = PG_GETARG_BOOL(0);

	PG_RETURN_BOOL(!b);
}

Datum
isnottrue(PG_FUNCTION_ARGS)
{
	bool		b;

	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(true);

	b = PG_GETARG_BOOL(0);

	PG_RETURN_BOOL(!b);
}

Datum
isnotfalse(PG_FUNCTION_ARGS)
{
	bool		b;

	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(true);

	b = PG_GETARG_BOOL(0);

	PG_RETURN_BOOL(b);
}

/*
 * boolean-and and boolean-or aggregates.
 */

/* function for standard EVERY aggregate implementation conforming to SQL 2003.
 * must be strict. It is also named bool_and for homogeneity.
 */
Datum
booland_statefunc(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(PG_GETARG_BOOL(0) && PG_GETARG_BOOL(1));
}

/* function for standard ANY/SOME aggregate conforming to SQL 2003.
 * must be strict. The name of the aggregate is bool_or. See the doc.
 */
Datum
boolor_statefunc(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(PG_GETARG_BOOL(0) || PG_GETARG_BOOL(1));
}
