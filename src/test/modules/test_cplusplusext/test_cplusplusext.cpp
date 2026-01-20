/*--------------------------------------------------------------------------
 *
 * test_cplusplusext.cpp
 *		Test that PostgreSQL headers compile with a C++ compiler.
 *
 * This file is compiled with a C++ compiler to verify that PostgreSQL
 * headers remain compatible with C++ extensions.
 *
 * Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_cplusplusext/test_cplusplusext.cpp
 *
 * -------------------------------------------------------------------------
 */

extern "C" {
#include "postgres.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_cplusplus_add);
}

/*
 * Simple function that returns the sum of two integers.  This verifies that
 * C++ extension modules can be loaded and called correctly at runtime.
 */
extern "C" Datum
test_cplusplus_add(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int32		b = PG_GETARG_INT32(1);

	PG_RETURN_INT32(a + b);
}
