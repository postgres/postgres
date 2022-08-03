/*--------------------------------------------------------------------------
 *
 * test_lfind.c
 *		Test correctness of optimized linear search functions.
 *
 * Copyright (c) 2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_lfind/test_lfind.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "port/pg_lfind.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_lfind);

Datum
test_lfind(PG_FUNCTION_ARGS)
{
#define TEST_ARRAY_SIZE 135
	uint32		test_array[TEST_ARRAY_SIZE] = {0};

	test_array[8] = 1;
	test_array[64] = 2;
	test_array[TEST_ARRAY_SIZE - 1] = 3;

	if (pg_lfind32(1, test_array, 4))
		elog(ERROR, "pg_lfind32() found nonexistent element");
	if (!pg_lfind32(1, test_array, TEST_ARRAY_SIZE))
		elog(ERROR, "pg_lfind32() did not find existing element");

	if (pg_lfind32(2, test_array, 32))
		elog(ERROR, "pg_lfind32() found nonexistent element");
	if (!pg_lfind32(2, test_array, TEST_ARRAY_SIZE))
		elog(ERROR, "pg_lfind32() did not find existing element");

	if (pg_lfind32(3, test_array, 96))
		elog(ERROR, "pg_lfind32() found nonexistent element");
	if (!pg_lfind32(3, test_array, TEST_ARRAY_SIZE))
		elog(ERROR, "pg_lfind32() did not find existing element");

	if (pg_lfind32(4, test_array, TEST_ARRAY_SIZE))
		elog(ERROR, "pg_lfind32() found nonexistent element");

	PG_RETURN_VOID();
}
