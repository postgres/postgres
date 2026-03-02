/*--------------------------------------------------------------------------
 *
 * test_custom_types.c
 *		Test module for a set of functions for custom types.
 *
 * The custom type used in this module is similar to int4 for simplicity,
 * except that it is able to use various typanalyze functions to enforce
 * check patterns with ANALYZE.
 *
 * Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_custom_types/test_custom_types.c
 *
 *--------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "commands/vacuum.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/* Function declarations */
PG_FUNCTION_INFO_V1(int_custom_in);
PG_FUNCTION_INFO_V1(int_custom_out);
PG_FUNCTION_INFO_V1(int_custom_typanalyze_false);
PG_FUNCTION_INFO_V1(int_custom_typanalyze_invalid);
PG_FUNCTION_INFO_V1(int_custom_eq);
PG_FUNCTION_INFO_V1(int_custom_ne);
PG_FUNCTION_INFO_V1(int_custom_lt);
PG_FUNCTION_INFO_V1(int_custom_le);
PG_FUNCTION_INFO_V1(int_custom_gt);
PG_FUNCTION_INFO_V1(int_custom_ge);
PG_FUNCTION_INFO_V1(int_custom_cmp);

/*
 * int_custom_in - input function for int_custom type
 *
 * Converts a string to a int_custom (which is just an int32 internally).
 */
Datum
int_custom_in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);

	PG_RETURN_INT32(pg_strtoint32_safe(num, fcinfo->context));
}

/*
 * int_custom_out - output function for int_custom type
 *
 * Converts a int_custom to a string.
 */
Datum
int_custom_out(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	char	   *result = (char *) palloc(12);	/* sign, 10 digits, '\0' */

	pg_ltoa(arg1, result);
	PG_RETURN_CSTRING(result);
}

/*
 * int_custom_typanalyze_false - typanalyze function that returns false
 *
 * This function returns false, to simulate a type that cannot be analyzed.
 */
Datum
int_custom_typanalyze_false(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(false);
}

/*
 * Callback used to compute invalid statistics.
 */
static void
int_custom_invalid_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
						 int samplerows, double totalrows)
{
	/* We are not valid, and do not want to be. */
	stats->stats_valid = false;
}

/*
 * int_custom_typanalyze_invalid
 *
 * This function sets some invalid stats data, letting the caller know that
 * we are safe for an analyze, returning true.
 */
Datum
int_custom_typanalyze_invalid(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *) PG_GETARG_POINTER(0);

	/* If the attstattarget column is negative, use the default value */
	if (stats->attstattarget < 0)
		stats->attstattarget = default_statistics_target;

	/* Buggy number, no need to care as long as it is positive */
	stats->minrows = 300;

	/* Set callback to compute some invalid stats */
	stats->compute_stats = int_custom_invalid_stats;

	PG_RETURN_BOOL(true);
}

/*
 * Comparison functions for int_custom type
 */
Datum
int_custom_eq(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
int_custom_ne(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
int_custom_lt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
int_custom_le(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
int_custom_gt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
int_custom_ge(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

Datum
int_custom_cmp(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	if (arg1 < arg2)
		PG_RETURN_INT32(-1);
	else if (arg1 > arg2)
		PG_RETURN_INT32(1);
	else
		PG_RETURN_INT32(0);
}
