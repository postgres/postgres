/* src/tutorial/funcs.c */

/******************************************************************************
  These are user-defined functions that can be bound to a Postgres backend
  and called by Postgres to execute SQL functions of the same name.

  The calling format for these functions is defined by the CREATE FUNCTION
  SQL statement that binds them to the backend.
*****************************************************************************/

#include "postgres.h"			/* general Postgres declarations */

#include "executor/executor.h"	/* for GetAttributeByName() */
#include "utils/fmgrprotos.h"	/* for text_starts_with() */
#include "utils/geo_decls.h"	/* for point type */

PG_MODULE_MAGIC;


/* By Value */

PG_FUNCTION_INFO_V1(add_one);

Datum
add_one(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);

	PG_RETURN_INT32(arg + 1);
}

/* By Reference, Fixed Length */

PG_FUNCTION_INFO_V1(add_one_float8);

Datum
add_one_float8(PG_FUNCTION_ARGS)
{
	/* The macros for FLOAT8 hide its pass-by-reference nature */
	float8		arg = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(arg + 1.0);
}

PG_FUNCTION_INFO_V1(makepoint);

Datum
makepoint(PG_FUNCTION_ARGS)
{
	Point	   *pointx = PG_GETARG_POINT_P(0);
	Point	   *pointy = PG_GETARG_POINT_P(1);
	Point	   *new_point = (Point *) palloc(sizeof(Point));

	new_point->x = pointx->x;
	new_point->y = pointy->y;

	PG_RETURN_POINT_P(new_point);
}

/* By Reference, Variable Length */

PG_FUNCTION_INFO_V1(copytext);

Datum
copytext(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);

	/*
	 * VARSIZE_ANY_EXHDR is the size of the struct in bytes, minus the
	 * VARHDRSZ or VARHDRSZ_SHORT of its header.  Construct the copy with a
	 * full-length header.
	 */
	text	   *new_t = (text *) palloc(VARSIZE_ANY_EXHDR(t) + VARHDRSZ);

	SET_VARSIZE(new_t, VARSIZE_ANY_EXHDR(t) + VARHDRSZ);

	/*
	 * VARDATA is a pointer to the data region of the new struct.  The source
	 * could be a short datum, so retrieve its data through VARDATA_ANY.
	 */
	memcpy(VARDATA(new_t),		/* destination */
		   VARDATA_ANY(t),		/* source */
		   VARSIZE_ANY_EXHDR(t));	/* how many bytes */
	PG_RETURN_TEXT_P(new_t);
}

PG_FUNCTION_INFO_V1(concat_text);

Datum
concat_text(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int32		arg1_size = VARSIZE_ANY_EXHDR(arg1);
	int32		arg2_size = VARSIZE_ANY_EXHDR(arg2);
	int32		new_text_size = arg1_size + arg2_size + VARHDRSZ;
	text	   *new_text = (text *) palloc(new_text_size);

	SET_VARSIZE(new_text, new_text_size);
	memcpy(VARDATA(new_text), VARDATA_ANY(arg1), arg1_size);
	memcpy(VARDATA(new_text) + arg1_size, VARDATA_ANY(arg2), arg2_size);
	PG_RETURN_TEXT_P(new_text);
}

/* A wrapper around starts_with(text, text) */

PG_FUNCTION_INFO_V1(t_starts_with);

Datum
t_starts_with(PG_FUNCTION_ARGS)
{
	text	   *t1 = PG_GETARG_TEXT_PP(0);
	text	   *t2 = PG_GETARG_TEXT_PP(1);
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	result = DatumGetBool(DirectFunctionCall2Coll(text_starts_with,
												  collid,
												  PointerGetDatum(t1),
												  PointerGetDatum(t2)));
	PG_RETURN_BOOL(result);
}

/* Composite types */

PG_FUNCTION_INFO_V1(c_overpaid);

Datum
c_overpaid(PG_FUNCTION_ARGS)
{
	HeapTupleHeader t = PG_GETARG_HEAPTUPLEHEADER(0);
	int32		limit = PG_GETARG_INT32(1);
	bool		isnull;
	int32		salary;

	salary = DatumGetInt32(GetAttributeByName(t, "salary", &isnull));
	if (isnull)
		PG_RETURN_BOOL(false);

	/*
	 * Alternatively, we might prefer to do PG_RETURN_NULL() for null salary
	 */

	PG_RETURN_BOOL(salary > limit);
}
