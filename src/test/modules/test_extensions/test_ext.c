/*
 * test_ext.c
 *
 * Dummy C extension for testing extension_control_path with pg_upgrade
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 */
#include "postgres.h"

#include "fmgr.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_ext);

Datum
test_ext(PG_FUNCTION_ARGS)
{
	ereport(NOTICE,
			(errmsg("running successful")));
	PG_RETURN_VOID();
}
