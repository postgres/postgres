/*-------------------------------------------------------------------------
 *
 * char.c
 *	  Functions for the built-in type "char".
 *	  Functions for the built-in type "cid" (what's that doing here?)
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/char.c,v 1.30 2001/01/24 19:43:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/builtins.h"

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		charin			- converts "x" to 'x'
 */
Datum
charin(PG_FUNCTION_ARGS)
{
	char	   *ch = PG_GETARG_CSTRING(0);

	PG_RETURN_CHAR(ch[0]);
}

/*
 *		charout			- converts 'x' to "x"
 */
Datum
charout(PG_FUNCTION_ARGS)
{
	char		ch = PG_GETARG_CHAR(0);
	char	   *result = (char *) palloc(2);

	result[0] = ch;
	result[1] = '\0';
	PG_RETURN_CSTRING(result);
}

/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

/*
 * NOTE: comparisons are done as though char is unsigned (uint8).
 * Arithmetic is done as though char is signed (int8).
 *
 * You wanted consistency?
 */

Datum
chareq(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
charne(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
charlt(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 < (uint8) arg2);
}

Datum
charle(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 <= (uint8) arg2);
}

Datum
chargt(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 > (uint8) arg2);
}

Datum
charge(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_BOOL((uint8) arg1 >= (uint8) arg2);
}

Datum
charpl(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_CHAR((int8) arg1 + (int8) arg2);
}

Datum
charmi(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_CHAR((int8) arg1 - (int8) arg2);
}

Datum
charmul(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_CHAR((int8) arg1 * (int8) arg2);
}

Datum
chardiv(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	char		arg2 = PG_GETARG_CHAR(1);

	PG_RETURN_CHAR((int8) arg1 / (int8) arg2);
}

Datum
text_char(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_P(0);

	/* XXX what if arg1 has length zero? */
	PG_RETURN_CHAR(*(VARDATA(arg1)));
}

Datum
char_text(PG_FUNCTION_ARGS)
{
	char		arg1 = PG_GETARG_CHAR(0);
	text	   *result = palloc(VARHDRSZ + 1);

	VARATT_SIZEP(result) = VARHDRSZ + 1;
	*(VARDATA(result)) = arg1;

	PG_RETURN_TEXT_P(result);
}


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		cidin	- converts CommandId to internal representation.
 */
Datum
cidin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	CommandId	c;

	c = atoi(s);

	/* XXX assume that CommandId is 32 bits... */
	PG_RETURN_INT32((int32) c);
}

/*
 *		cidout	- converts a cid to external representation.
 */
Datum
cidout(PG_FUNCTION_ARGS)
{
	/* XXX assume that CommandId is 32 bits... */
	CommandId	c = PG_GETARG_INT32(0);
	char	   *result = (char *) palloc(16);

	sprintf(result, "%u", (unsigned int) c);
	PG_RETURN_CSTRING(result);
}

/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

Datum
cideq(PG_FUNCTION_ARGS)
{
	/* XXX assume that CommandId is 32 bits... */
	CommandId	arg1 = PG_GETARG_INT32(0);
	CommandId	arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 == arg2);
}
