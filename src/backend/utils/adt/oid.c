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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/oid.c,v 1.41 2000/12/03 20:45:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <errno.h>

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
		while (*oidString && isspace((unsigned char) *oidString))
			oidString++;
		while (*oidString && isdigit((unsigned char) *oidString))
			oidString++;
	}
	while (*oidString && isspace((unsigned char) *oidString))
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
		sprintf(rp, "%u", oidArray[num]);
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
	unsigned long cvt;
	char	   *endptr;
	Oid			result;

	errno = 0;

	cvt = strtoul(s, &endptr, 10);

	/*
	 * strtoul() normally only sets ERANGE.  On some systems it also
	 * may set EINVAL, which simply means it couldn't parse the
	 * input string.  This is handled by the second "if" consistent
	 * across platforms.
	 */
	if (errno && errno != EINVAL)
		elog(ERROR, "oidin: error reading \"%s\": %m", s);
	if (endptr && *endptr)
		elog(ERROR, "oidin: error in \"%s\": can't parse \"%s\"", s, endptr);

	/*
	 * Cope with possibility that unsigned long is wider than Oid.
	 */
	result = (Oid) cvt;
	if ((unsigned long) result != cvt)
		elog(ERROR, "oidin: error reading \"%s\": value too large", s);

	return ObjectIdGetDatum(result);
}

Datum
oidout(PG_FUNCTION_ARGS)
{
	Oid			o = PG_GETARG_OID(0);
	char	   *result = (char *) palloc(12);

	snprintf(result, 12, "%u", o);
	PG_RETURN_CSTRING(result);
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
oidlt(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
oidle(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
oidge(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

Datum
oidgt(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 > arg2);
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
