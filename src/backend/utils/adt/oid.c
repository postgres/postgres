/*-------------------------------------------------------------------------
 *
 * oid.c
 *	  Functions for the built-in type Oid ... also oidvector.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/oid.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>

#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/builtins.h"


#define OidVectorSize(n)	(offsetof(oidvector, values) + (n) * sizeof(Oid))


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

static Oid
oidin_subr(const char *s, char **endloc)
{
	unsigned long cvt;
	char	   *endptr;
	Oid			result;

	if (*s == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type oid: \"%s\"",
						s)));

	errno = 0;
	cvt = strtoul(s, &endptr, 10);

	/*
	 * strtoul() normally only sets ERANGE.  On some systems it also may set
	 * EINVAL, which simply means it couldn't parse the input string. This is
	 * handled by the second "if" consistent across platforms.
	 */
	if (errno && errno != ERANGE && errno != EINVAL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type oid: \"%s\"",
						s)));

	if (endptr == s && *s != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type oid: \"%s\"",
						s)));

	if (errno == ERANGE)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for type oid", s)));

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
					 errmsg("invalid input syntax for type oid: \"%s\"",
							s)));
	}

	result = (Oid) cvt;

	/*
	 * Cope with possibility that unsigned long is wider than Oid, in which
	 * case strtoul will not raise an error for some values that are out of
	 * the range of Oid.
	 *
	 * For backwards compatibility, we want to accept inputs that are given
	 * with a minus sign, so allow the input value if it matches after either
	 * signed or unsigned extension to long.
	 *
	 * To ensure consistent results on 32-bit and 64-bit platforms, make sure
	 * the error message is the same as if strtoul() had returned ERANGE.
	 */
#if OID_MAX != ULONG_MAX
	if (cvt != (unsigned long) result &&
		cvt != (unsigned long) ((int) result))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for type oid", s)));
#endif

	return result;
}

Datum
oidin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	Oid			result;

	result = oidin_subr(s, NULL);
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
 * construct oidvector given a raw array of Oids
 *
 * If oids is NULL then caller must fill values[] afterward
 */
oidvector *
buildoidvector(const Oid *oids, int n)
{
	oidvector  *result;

	result = (oidvector *) palloc0(OidVectorSize(n));

	if (n > 0 && oids)
		memcpy(result->values, oids, n * sizeof(Oid));

	/*
	 * Attach standard array header.  For historical reasons, we set the index
	 * lower bound to 0 not 1.
	 */
	SET_VARSIZE(result, OidVectorSize(n));
	result->ndim = 1;
	result->dataoffset = 0;		/* never any nulls */
	result->elemtype = OIDOID;
	result->dim1 = n;
	result->lbound1 = 0;

	return result;
}

/*
 *		oidvectorin			- converts "num num ..." to internal form
 */
Datum
oidvectorin(PG_FUNCTION_ARGS)
{
	char	   *oidString = PG_GETARG_CSTRING(0);
	oidvector  *result;
	int			n;

	result = (oidvector *) palloc0(OidVectorSize(FUNC_MAX_ARGS));

	for (n = 0; n < FUNC_MAX_ARGS; n++)
	{
		while (*oidString && isspace((unsigned char) *oidString))
			oidString++;
		if (*oidString == '\0')
			break;
		result->values[n] = oidin_subr(oidString, &oidString);
	}
	while (*oidString && isspace((unsigned char) *oidString))
		oidString++;
	if (*oidString)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("oidvector has too many elements")));

	SET_VARSIZE(result, OidVectorSize(n));
	result->ndim = 1;
	result->dataoffset = 0;		/* never any nulls */
	result->elemtype = OIDOID;
	result->dim1 = n;
	result->lbound1 = 0;

	PG_RETURN_POINTER(result);
}

/*
 *		oidvectorout - converts internal form to "num num ..."
 */
Datum
oidvectorout(PG_FUNCTION_ARGS)
{
	oidvector  *oidArray = (oidvector *) PG_GETARG_POINTER(0);
	int			num,
				nnums = oidArray->dim1;
	char	   *rp;
	char	   *result;

	/* assumes sign, 10 digits, ' ' */
	rp = result = (char *) palloc(nnums * 12 + 1);
	for (num = 0; num < nnums; num++)
	{
		if (num != 0)
			*rp++ = ' ';
		sprintf(rp, "%u", oidArray->values[num]);
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
	FunctionCallInfoData locfcinfo;
	oidvector  *result;

	/*
	 * Normally one would call array_recv() using DirectFunctionCall3, but
	 * that does not work since array_recv wants to cache some data using
	 * fcinfo->flinfo->fn_extra.  So we need to pass it our own flinfo
	 * parameter.
	 */
	InitFunctionCallInfoData(locfcinfo, fcinfo->flinfo, 3,
							 InvalidOid, NULL, NULL);

	locfcinfo.arg[0] = PointerGetDatum(buf);
	locfcinfo.arg[1] = ObjectIdGetDatum(OIDOID);
	locfcinfo.arg[2] = Int32GetDatum(-1);
	locfcinfo.argnull[0] = false;
	locfcinfo.argnull[1] = false;
	locfcinfo.argnull[2] = false;

	result = (oidvector *) DatumGetPointer(array_recv(&locfcinfo));

	Assert(!locfcinfo.isnull);

	/* sanity checks: oidvector must be 1-D, 0-based, no nulls */
	if (ARR_NDIM(result) != 1 ||
		ARR_HASNULL(result) ||
		ARR_ELEMTYPE(result) != OIDOID ||
		ARR_LBOUND(result)[0] != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid oidvector data")));

	/* check length for consistency with oidvectorin() */
	if (ARR_DIMS(result)[0] > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("oidvector has too many elements")));

	PG_RETURN_POINTER(result);
}

/*
 *		oidvectorsend			- converts oidvector to binary format
 */
Datum
oidvectorsend(PG_FUNCTION_ARGS)
{
	return array_send(fcinfo);
}

/*
 *		oidparse				- get OID from IConst/FConst node
 */
Oid
oidparse(Node *node)
{
	switch (nodeTag(node))
	{
		case T_Integer:
			return intVal(node);
		case T_Float:

			/*
			 * Values too large for int4 will be represented as Float
			 * constants by the lexer.  Accept these if they are valid OID
			 * strings.
			 */
			return oidin_subr(strVal(node), NULL);
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
	}
	return InvalidOid;			/* keep compiler quiet */
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
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp == 0);
}

Datum
oidvectorne(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp != 0);
}

Datum
oidvectorlt(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp < 0);
}

Datum
oidvectorle(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
oidvectorge(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp >= 0);
}

Datum
oidvectorgt(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp > 0);
}
