/*-------------------------------------------------------------------------
 *
 * oid.c
 *	  Functions for the built-in type Oid ... also oidvector.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/oid.c,v 1.52 2003/09/25 06:58:04 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "libpq/pqformat.h"
#include "utils/builtins.h"


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

static Oid
oidin_subr(const char *funcname, const char *s, char **endloc)
{
	unsigned long cvt;
	char	   *endptr;
	Oid			result;

	errno = 0;
	cvt = strtoul(s, &endptr, 10);

	/*
	 * strtoul() normally only sets ERANGE.  On some systems it also may
	 * set EINVAL, which simply means it couldn't parse the input string.
	 * This is handled by the second "if" consistent across platforms.
	 * Note that for historical reasons we accept an empty string as
	 * meaning 0.
	 */
	if (errno && errno != ERANGE && errno != EINVAL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type \"oid\": \"%s\"",
						s)));
	if (endptr == s && *endptr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type \"oid\": \"%s\"",
						s)));

	if (errno == ERANGE)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for type \"oid\"", s)));

	if (endloc)
	{
		/* caller wants to deal with rest of string */
		*endloc = endptr;
	}
	else
	{
		/* allow only whitespace after number */
		while (*endptr && isspace((unsigned char) *endptr))
			endptr++;
		if (*endptr)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type \"oid\": \"%s\"",
							s)));
	}

	result = (Oid) cvt;

	/*
	 * Cope with possibility that unsigned long is wider than Oid, in
	 * which case strtoul will not raise an error for some values that are
	 * out of the range of Oid.
	 *
	 * For backwards compatibility, we want to accept inputs that are given
	 * with a minus sign, so allow the input value if it matches after
	 * either signed or unsigned extension to long.
	 *
	 * To ensure consistent results on 32-bit and 64-bit platforms, make sure
	 * the error message is the same as if strtoul() had returned ERANGE.
	 */
#if OID_MAX != ULONG_MAX
	if (cvt != (unsigned long) result &&
		cvt != (unsigned long) ((int) result))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for type \"oid\"", s)));
#endif

	return result;
}

Datum
oidin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	Oid			result;

	result = oidin_subr("oidin", s, NULL);
	PG_RETURN_OID(result);
}

Datum
oidout(PG_FUNCTION_ARGS)
{
	Oid			o = PG_GETARG_OID(0);
	char	   *result = (char *) palloc(12);

	snprintf(result, 12, "%u", o);
	PG_RETURN_CSTRING(result);
}

/*
 *		oidrecv			- converts external binary format to oid
 */
Datum
oidrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_OID((Oid) pq_getmsgint(buf, sizeof(Oid)));
}

/*
 *		oidsend			- converts oid to binary format
 */
Datum
oidsend(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint(&buf, arg1, sizeof(Oid));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


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
	Oid		   *result = (Oid *) palloc(sizeof(Oid[INDEX_MAX_KEYS]));
	int			slot;

	for (slot = 0; slot < INDEX_MAX_KEYS; slot++)
	{
		while (*oidString && isspace((unsigned char) *oidString))
			oidString++;
		if (*oidString == '\0')
			break;
		result[slot] = oidin_subr("oidvectorin", oidString, &oidString);
	}
	while (*oidString && isspace((unsigned char) *oidString))
		oidString++;
	if (*oidString)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("oidvector has too many elements")));
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

/*
 *		oidvectorrecv			- converts external binary format to oidvector
 */
Datum
oidvectorrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid		   *result = (Oid *) palloc(sizeof(Oid[INDEX_MAX_KEYS]));
	int			slot;

	for (slot = 0; slot < INDEX_MAX_KEYS; slot++)
		result[slot] = (Oid) pq_getmsgint(buf, sizeof(Oid));
	PG_RETURN_POINTER(result);
}

/*
 *		oidvectorsend			- converts oidvector to binary format
 */
Datum
oidvectorsend(PG_FUNCTION_ARGS)
{
	Oid		   *oidArray = (Oid *) PG_GETARG_POINTER(0);
	StringInfoData buf;
	int			slot;

	pq_begintypsend(&buf);
	for (slot = 0; slot < INDEX_MAX_KEYS; slot++)
		pq_sendint(&buf, oidArray[slot], sizeof(Oid));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
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
oidlarger(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_OID((arg1 > arg2) ? arg1 : arg2);
}

Datum
oidsmaller(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_OID((arg1 < arg2) ? arg1 : arg2);
}

Datum
oidvectoreq(PG_FUNCTION_ARGS)
{
	Oid		   *arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid		   *arg2 = (Oid *) PG_GETARG_POINTER(1);

	PG_RETURN_BOOL(memcmp(arg1, arg2, INDEX_MAX_KEYS * sizeof(Oid)) == 0);
}

Datum
oidvectorne(PG_FUNCTION_ARGS)
{
	Oid		   *arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid		   *arg2 = (Oid *) PG_GETARG_POINTER(1);

	PG_RETURN_BOOL(memcmp(arg1, arg2, INDEX_MAX_KEYS * sizeof(Oid)) != 0);
}

Datum
oidvectorlt(PG_FUNCTION_ARGS)
{
	Oid		   *arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid		   *arg2 = (Oid *) PG_GETARG_POINTER(1);
	int			i;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
		if (arg1[i] != arg2[i])
			PG_RETURN_BOOL(arg1[i] < arg2[i]);
	PG_RETURN_BOOL(false);
}

Datum
oidvectorle(PG_FUNCTION_ARGS)
{
	Oid		   *arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid		   *arg2 = (Oid *) PG_GETARG_POINTER(1);
	int			i;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
		if (arg1[i] != arg2[i])
			PG_RETURN_BOOL(arg1[i] <= arg2[i]);
	PG_RETURN_BOOL(true);
}

Datum
oidvectorge(PG_FUNCTION_ARGS)
{
	Oid		   *arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid		   *arg2 = (Oid *) PG_GETARG_POINTER(1);
	int			i;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
		if (arg1[i] != arg2[i])
			PG_RETURN_BOOL(arg1[i] >= arg2[i]);
	PG_RETURN_BOOL(true);
}

Datum
oidvectorgt(PG_FUNCTION_ARGS)
{
	Oid		   *arg1 = (Oid *) PG_GETARG_POINTER(0);
	Oid		   *arg2 = (Oid *) PG_GETARG_POINTER(1);
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

	result = oidin_subr("text_oid", str, NULL);

	pfree(str);

	PG_RETURN_OID(result);
}
