/*-------------------------------------------------------------------------
 *
 * oid.c
 *	  Functions for the built-in type Oid ... also oidvector.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "common/int.h"
#include "libpq/pqformat.h"
#include "nodes/miscnodes.h"
#include "nodes/value.h"
#include "utils/array.h"
#include "utils/builtins.h"


#define OidVectorSize(n)	(offsetof(oidvector, values) + (n) * sizeof(Oid))


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

Datum
oidin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	Oid			result;

	result = uint32in_subr(s, NULL, "oid", fcinfo->context);
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
	pq_sendint32(&buf, arg1);
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
	Node	   *escontext = fcinfo->context;
	oidvector  *result;
	int			nalloc;
	int			n;

	nalloc = 32;				/* arbitrary initial size guess */
	result = (oidvector *) palloc0(OidVectorSize(nalloc));

	for (n = 0;; n++)
	{
		while (*oidString && isspace((unsigned char) *oidString))
			oidString++;
		if (*oidString == '\0')
			break;

		if (n >= nalloc)
		{
			nalloc *= 2;
			result = (oidvector *) repalloc(result, OidVectorSize(nalloc));
		}

		result->values[n] = uint32in_subr(oidString, &oidString,
										  "oid", escontext);
		if (SOFT_ERROR_OCCURRED(escontext))
			PG_RETURN_NULL();
	}

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
	LOCAL_FCINFO(locfcinfo, 3);
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	oidvector  *result;

	/*
	 * Normally one would call array_recv() using DirectFunctionCall3, but
	 * that does not work since array_recv wants to cache some data using
	 * fcinfo->flinfo->fn_extra.  So we need to pass it our own flinfo
	 * parameter.
	 */
	InitFunctionCallInfoData(*locfcinfo, fcinfo->flinfo, 3,
							 InvalidOid, NULL, NULL);

	locfcinfo->args[0].value = PointerGetDatum(buf);
	locfcinfo->args[0].isnull = false;
	locfcinfo->args[1].value = ObjectIdGetDatum(OIDOID);
	locfcinfo->args[1].isnull = false;
	locfcinfo->args[2].value = Int32GetDatum(-1);
	locfcinfo->args[2].isnull = false;

	result = (oidvector *) DatumGetPointer(array_recv(locfcinfo));

	Assert(!locfcinfo->isnull);

	/* sanity checks: oidvector must be 1-D, 0-based, no nulls */
	if (ARR_NDIM(result) != 1 ||
		ARR_HASNULL(result) ||
		ARR_ELEMTYPE(result) != OIDOID ||
		ARR_LBOUND(result)[0] != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid oidvector data")));

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
 *		oidparse				- get OID from ICONST/FCONST node
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
			return uint32in_subr(castNode(Float, node)->fval, NULL,
								 "oid", NULL);
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
	}
	return InvalidOid;			/* keep compiler quiet */
}

/* qsort comparison function for Oids */
int
oid_cmp(const void *p1, const void *p2)
{
	Oid			v1 = *((const Oid *) p1);
	Oid			v2 = *((const Oid *) p2);

	return pg_cmp_u32(v1, v2);
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
