/*-------------------------------------------------------------------------
 *
 * oid.c
 *	  Functions for the built-in type Oid.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/oid.c,v 1.37 2000/07/03 23:09:52 wieck Exp $
 *
 *-------------------------------------------------------------------------
 */


#include <ctype.h>
#include "postgres.h"
#include "utils/builtins.h"

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		oidvectorin			- converts "num num ..." to internal form
 *
 *		Note:
 *				Fills any unsupplied positions with InvalidOid.
 */
Datum
oidvectorin(PG_FUNCTION_ARGS)
{
	char	   *oidString = PG_GETARG_CSTRING(0);
	Oid		   *result;
	int			slot;

	result = (Oid *) palloc(sizeof(Oid[INDEX_MAX_KEYS]));

	for (slot = 0; *oidString && slot < INDEX_MAX_KEYS; slot++)
	{
		if (sscanf(oidString, "%u", &result[slot]) != 1)
			break;
		while (*oidString && isspace((int) *oidString))
			oidString++;
		while (*oidString && !isspace((int) *oidString))
			oidString++;
	}
	while (*oidString && isspace((int) *oidString))
		oidString++;
	if (*oidString)
		elog(ERROR, "oidvector value has too many values");
	while (slot < INDEX_MAX_KEYS)
		result[slot++] = InvalidOid;

	PG_RETURN_POINTER(result);
}

/*
 *		oidvectorout - converts internal form to "num num ..."
 */
Datum
oidvectorout(PG_FUNCTION_ARGS)
{
	Oid		   *oidArray = (Oid *) PG_GETARG_POINTER(0);
	int			num,
				maxnum;
	char	   *rp;
	char	   *result;

	/* find last non-zero value in vector */
	for (maxnum = INDEX_MAX_KEYS - 1; maxnum >= 0; maxnum--)
		if (oidArray[maxnum] != 0)
			break;

	/* assumes sign, 10 digits, ' ' */
	rp = result = (char *) palloc((maxnum + 1) * 12 + 1);
	for (num = 0; num <= maxnum; num++)
	{
		if (num != 0)
			*rp++ = ' ';
		ltoa(oidArray[num], rp);
		while (*++rp != '\0')
			;
	}
	*rp = '\0';
	PG_RETURN_CSTRING(result);
}

Datum
oidin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);

	/* XXX should use an unsigned-int conversion here */
	return DirectFunctionCall1(int4in, CStringGetDatum(s));
}

Datum
oidout(PG_FUNCTION_ARGS)
{
	Oid			o = PG_GETARG_OID(0);

	/* XXX should use an unsigned-int conversion here */
	return DirectFunctionCall1(int4out, ObjectIdGetDatum(o));
}

/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

Datum
oideq(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
oidne(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
oidvectoreq(PG_FUNCTION_ARGS)
{
	Oid			*arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid			*arg2 = (Oid *) PG_GETARG_POINTER(1);

	PG_RETURN_BOOL(memcmp(arg1, arg2, INDEX_MAX_KEYS * sizeof(Oid)) == 0);
}

Datum
oidvectorne(PG_FUNCTION_ARGS)
{
	Oid			*arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid			*arg2 = (Oid *) PG_GETARG_POINTER(1);

	PG_RETURN_BOOL(memcmp(arg1, arg2, INDEX_MAX_KEYS * sizeof(Oid)) != 0);
}

Datum
oidvectorlt(PG_FUNCTION_ARGS)
{
	Oid			*arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid			*arg2 = (Oid *) PG_GETARG_POINTER(1);
	int			i;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
		if (arg1[i] != arg2[i])
			PG_RETURN_BOOL(arg1[i] < arg2[i]);
	PG_RETURN_BOOL(false);
}

Datum
oidvectorle(PG_FUNCTION_ARGS)
{
	Oid			*arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid			*arg2 = (Oid *) PG_GETARG_POINTER(1);
	int			i;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
		if (arg1[i] != arg2[i])
			PG_RETURN_BOOL(arg1[i] <= arg2[i]);
	PG_RETURN_BOOL(true);
}

Datum
oidvectorge(PG_FUNCTION_ARGS)
{
	Oid			*arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid			*arg2 = (Oid *) PG_GETARG_POINTER(1);
	int			i;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
		if (arg1[i] != arg2[i])
			PG_RETURN_BOOL(arg1[i] >= arg2[i]);
	PG_RETURN_BOOL(true);
}

Datum
oidvectorgt(PG_FUNCTION_ARGS)
{
	Oid			*arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid			*arg2 = (Oid *) PG_GETARG_POINTER(1);
	int			i;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
		if (arg1[i] != arg2[i])
			PG_RETURN_BOOL(arg1[i] > arg2[i]);
	PG_RETURN_BOOL(false);
}

Datum
oideqint4(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	int32		arg2 = PG_GETARG_INT32(1);

	/* oid is unsigned, but int4 is signed */
	PG_RETURN_BOOL(arg2 >= 0 && arg1 == arg2);
}

Datum
int4eqoid(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	Oid			arg2 = PG_GETARG_OID(1);

	/* oid is unsigned, but int4 is signed */
	PG_RETURN_BOOL(arg1 >= 0 && arg1 == arg2);
}

Datum
oid_text(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	text	   *result;
	int			len;
	char	   *str;

	str = DatumGetCString(DirectFunctionCall1(oidout,
											  ObjectIdGetDatum(oid)));
	len = strlen(str) + VARHDRSZ;

	result = (text *) palloc(len);

	VARATT_SIZEP(result) = len;
	memcpy(VARDATA(result), str, (len - VARHDRSZ));
	pfree(str);

	PG_RETURN_TEXT_P(result);
}

Datum
text_oid(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	Oid			result;
	int			len;
	char	   *str;

	len = (VARSIZE(string) - VARHDRSZ);

	str = palloc(len + 1);
	memcpy(str, VARDATA(string), len);
	*(str + len) = '\0';

	result = DatumGetObjectId(DirectFunctionCall1(oidin,
												  CStringGetDatum(str)));
	pfree(str);

	PG_RETURN_OID(result);
}
