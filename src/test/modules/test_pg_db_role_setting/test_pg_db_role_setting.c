/*--------------------------------------------------------------------------
 *
 * test_pg_db_role_setting.c
 *		Code for testing mandatory access control (MAC) using object access hooks.
 *
 * Copyright (c) 2022-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_pg_db_role_setting/test_pg_db_role_setting.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/guc.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(load_test_pg_db_role_setting);

static char *superuser_param;
static char *user_param;

/*
 * Module load callback
 */
void
_PG_init(void)
{
	DefineCustomStringVariable("test_pg_db_role_setting.superuser_param",
							   "Sample superuser parameter.",
							   NULL,
							   &superuser_param,
							   "superuser_param_value",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("test_pg_db_role_setting.user_param",
							   "Sample user parameter.",
							   NULL,
							   &user_param,
							   "user_param_value",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);
}

/*
 * Empty function, which is used just to trigger load of this module.
 */
Datum
load_test_pg_db_role_setting(PG_FUNCTION_ARGS)
{
	PG_RETURN_VOID();
}
