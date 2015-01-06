/*
 * brin_minmax.c
 *		Implementation of Min/Max opclass for BRIN
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_minmax.c
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/brin_internal.h"
#include "access/brin_tuple.h"
#include "access/skey.h"
#include "catalog/pg_type.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * Procedure numbers must not collide with BRIN_PROCNUM defines in
 * brin_internal.h.  Note we only need inequality functions.
 */
#define		MINMAX_NUM_PROCNUMS		4	/* # support procs we need */
#define		PROCNUM_LESS			11
#define		PROCNUM_LESSEQUAL		12
#define		PROCNUM_GREATEREQUAL	13
#define		PROCNUM_GREATER			14

/*
 * Subtract this from procnum to obtain index in MinmaxOpaque arrays
 * (Must be equal to minimum of private procnums)
 */
#define		PROCNUM_BASE			11

typedef struct MinmaxOpaque
{
	FmgrInfo	operators[MINMAX_NUM_PROCNUMS];
	bool		inited[MINMAX_NUM_PROCNUMS];
} MinmaxOpaque;

static FmgrInfo *minmax_get_procinfo(BrinDesc *bdesc, uint16 attno,
					uint16 procnum);


Datum
brin_minmax_opcinfo(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);
	BrinOpcInfo *result;

	/*
	 * opaque->operators is initialized lazily, as indicated by 'inited' which
	 * is initialized to all false by palloc0.
	 */

	result = palloc0(MAXALIGN(SizeofBrinOpcInfo(2)) +
					 sizeof(MinmaxOpaque));
	result->oi_nstored = 2;
	result->oi_opaque = (MinmaxOpaque *)
		MAXALIGN((char *) result + SizeofBrinOpcInfo(2));
	result->oi_typids[0] = typoid;
	result->oi_typids[1] = typoid;

	PG_RETURN_POINTER(result);
}

/*
 * Examine the given index tuple (which contains partial status of a certain
 * page range) by comparing it to the given value that comes from another heap
 * tuple.  If the new value is outside the min/max range specified by the
 * existing tuple values, update the index tuple and return true.  Otherwise,
 * return false and do not modify in this case.
 */
Datum
brin_minmax_add_value(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	Datum		newval = PG_GETARG_DATUM(2);
	bool		isnull = PG_GETARG_DATUM(3);
	Oid			colloid = PG_GET_COLLATION();
	FmgrInfo   *cmpFn;
	Datum		compar;
	bool		updated = false;
	Form_pg_attribute attr;
	AttrNumber	attno;

	/*
	 * If the new value is null, we record that we saw it if it's the first
	 * one; otherwise, there's nothing to do.
	 */
	if (isnull)
	{
		if (column->bv_hasnulls)
			PG_RETURN_BOOL(false);

		column->bv_hasnulls = true;
		PG_RETURN_BOOL(true);
	}

	attno = column->bv_attno;
	attr = bdesc->bd_tupdesc->attrs[attno - 1];

	/*
	 * If the recorded value is null, store the new value (which we know to be
	 * not null) as both minimum and maximum, and we're done.
	 */
	if (column->bv_allnulls)
	{
		column->bv_values[0] = datumCopy(newval, attr->attbyval, attr->attlen);
		column->bv_values[1] = datumCopy(newval, attr->attbyval, attr->attlen);
		column->bv_allnulls = false;
		PG_RETURN_BOOL(true);
	}

	/*
	 * Otherwise, need to compare the new value with the existing boundaries
	 * and update them accordingly.  First check if it's less than the
	 * existing minimum.
	 */
	cmpFn = minmax_get_procinfo(bdesc, attno, PROCNUM_LESS);
	compar = FunctionCall2Coll(cmpFn, colloid, newval, column->bv_values[0]);
	if (DatumGetBool(compar))
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(column->bv_values[0]));
		column->bv_values[0] = datumCopy(newval, attr->attbyval, attr->attlen);
		updated = true;
	}

	/*
	 * And now compare it to the existing maximum.
	 */
	cmpFn = minmax_get_procinfo(bdesc, attno, PROCNUM_GREATER);
	compar = FunctionCall2Coll(cmpFn, colloid, newval, column->bv_values[1]);
	if (DatumGetBool(compar))
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(column->bv_values[1]));
		column->bv_values[1] = datumCopy(newval, attr->attbyval, attr->attlen);
		updated = true;
	}

	PG_RETURN_BOOL(updated);
}

/*
 * Given an index tuple corresponding to a certain page range and a scan key,
 * return whether the scan key is consistent with the index tuple's min/max
 * values.  Return true if so, false otherwise.
 */
Datum
brin_minmax_consistent(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(2);
	Oid			colloid = PG_GET_COLLATION();
	AttrNumber	attno;
	Datum		value;
	Datum		matches;

	Assert(key->sk_attno == column->bv_attno);

	/* handle IS NULL/IS NOT NULL tests */
	if (key->sk_flags & SK_ISNULL)
	{
		if (key->sk_flags & SK_SEARCHNULL)
		{
			if (column->bv_allnulls || column->bv_hasnulls)
				PG_RETURN_BOOL(true);
			PG_RETURN_BOOL(false);
		}

		/*
		 * For IS NOT NULL, we can only skip ranges that are known to have
		 * only nulls.
		 */
		Assert(key->sk_flags & SK_SEARCHNOTNULL);
		PG_RETURN_BOOL(!column->bv_allnulls);
	}

	/* if the range is all empty, it cannot possibly be consistent */
	if (column->bv_allnulls)
		PG_RETURN_BOOL(false);

	attno = key->sk_attno;
	value = key->sk_argument;
	switch (key->sk_strategy)
	{
		case BTLessStrategyNumber:
			matches = FunctionCall2Coll(minmax_get_procinfo(bdesc, attno,
															PROCNUM_LESS),
										colloid, column->bv_values[0], value);
			break;
		case BTLessEqualStrategyNumber:
			matches = FunctionCall2Coll(minmax_get_procinfo(bdesc, attno,
														  PROCNUM_LESSEQUAL),
										colloid, column->bv_values[0], value);
			break;
		case BTEqualStrategyNumber:

			/*
			 * In the equality case (WHERE col = someval), we want to return
			 * the current page range if the minimum value in the range <=
			 * scan key, and the maximum value >= scan key.
			 */
			matches = FunctionCall2Coll(minmax_get_procinfo(bdesc, attno,
														  PROCNUM_LESSEQUAL),
										colloid, column->bv_values[0], value);
			if (!DatumGetBool(matches))
				break;
			/* max() >= scankey */
			matches = FunctionCall2Coll(minmax_get_procinfo(bdesc, attno,
													   PROCNUM_GREATEREQUAL),
										colloid, column->bv_values[1], value);
			break;
		case BTGreaterEqualStrategyNumber:
			matches = FunctionCall2Coll(minmax_get_procinfo(bdesc, attno,
													   PROCNUM_GREATEREQUAL),
										colloid, column->bv_values[1], value);
			break;
		case BTGreaterStrategyNumber:
			matches = FunctionCall2Coll(minmax_get_procinfo(bdesc, attno,
															PROCNUM_GREATER),
										colloid, column->bv_values[1], value);
			break;
		default:
			/* shouldn't happen */
			elog(ERROR, "invalid strategy number %d", key->sk_strategy);
			matches = 0;
			break;
	}

	PG_RETURN_DATUM(matches);
}

/*
 * Given two BrinValues, update the first of them as a union of the summary
 * values contained in both.  The second one is untouched.
 */
Datum
brin_minmax_union(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *col_a = (BrinValues *) PG_GETARG_POINTER(1);
	BrinValues *col_b = (BrinValues *) PG_GETARG_POINTER(2);
	Oid			colloid = PG_GET_COLLATION();
	AttrNumber	attno;
	Form_pg_attribute attr;
	bool		needsadj;

	Assert(col_a->bv_attno == col_b->bv_attno);

	/* If there are no values in B, there's nothing to do */
	if (col_b->bv_allnulls)
		PG_RETURN_VOID();

	attno = col_a->bv_attno;
	attr = bdesc->bd_tupdesc->attrs[attno - 1];

	/* Adjust "hasnulls" */
	if (col_b->bv_hasnulls && !col_a->bv_hasnulls)
		col_a->bv_hasnulls = true;

	/*
	 * Adjust "allnulls".  If B has values but A doesn't, just copy the values
	 * from B into A, and we're done.  (We cannot run the operators in this
	 * case, because values in A might contain garbage.)
	 */
	if (!col_b->bv_allnulls && col_a->bv_allnulls)
	{
		col_a->bv_allnulls = false;
		col_a->bv_values[0] = datumCopy(col_b->bv_values[0],
										attr->attbyval, attr->attlen);
		col_a->bv_values[1] = datumCopy(col_b->bv_values[1],
										attr->attbyval, attr->attlen);
		PG_RETURN_VOID();
	}

	/* Adjust minimum, if B's min is less than A's min */
	needsadj = FunctionCall2Coll(minmax_get_procinfo(bdesc, attno,
													 PROCNUM_LESS),
						  colloid, col_b->bv_values[0], col_a->bv_values[0]);
	if (needsadj)
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(col_a->bv_values[0]));
		col_a->bv_values[0] = datumCopy(col_b->bv_values[0],
										attr->attbyval, attr->attlen);
	}

	/* Adjust maximum, if B's max is greater than A's max */
	needsadj = FunctionCall2Coll(minmax_get_procinfo(bdesc, attno,
													 PROCNUM_GREATER),
						  colloid, col_b->bv_values[1], col_a->bv_values[1]);
	if (needsadj)
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(col_a->bv_values[1]));
		col_a->bv_values[1] = datumCopy(col_b->bv_values[1],
										attr->attbyval, attr->attlen);
	}

	PG_RETURN_VOID();
}

/*
 * Return the procedure corresponding to the given function support number.
 */
static FmgrInfo *
minmax_get_procinfo(BrinDesc *bdesc, uint16 attno, uint16 procnum)
{
	MinmaxOpaque *opaque;
	uint16		basenum = procnum - PROCNUM_BASE;

	opaque = (MinmaxOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * We cache these in the opaque struct, to avoid repetitive syscache
	 * lookups.
	 */
	if (!opaque->inited[basenum])
	{
		fmgr_info_copy(&opaque->operators[basenum],
					   index_getprocinfo(bdesc->bd_index, attno, procnum),
					   bdesc->bd_context);
		opaque->inited[basenum] = true;
	}

	return &opaque->operators[basenum];
}
