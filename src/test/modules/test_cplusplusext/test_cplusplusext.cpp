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
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_cplusplus_add);
}

StaticAssertDecl(sizeof(int32) == 4, "int32 should be 4 bytes");

/*
 * Simple function that returns the sum of two integers.  This verifies that
 * C++ extension modules can be loaded and called correctly at runtime.
 */
extern "C" Datum
test_cplusplus_add(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int32		b = PG_GETARG_INT32(1);
	RangeTblRef *node = makeNode(RangeTblRef);
	List	   *list = list_make1(node);

	foreach_ptr(RangeTblRef, rtr, list)
	{
		(void) rtr;
	}

	foreach_node(RangeTblRef, rtr, list)
	{
		(void) rtr;
	}

	StaticAssertStmt(sizeof(int32) == 4, "int32 should be 4 bytes");
	(void) StaticAssertExpr(sizeof(int64) == 8, "int64 should be 8 bytes");

	list_free(list);
	pfree(node);

	PG_RETURN_INT32(a + b);
}
