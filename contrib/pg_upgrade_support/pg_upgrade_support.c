/*
 *	pg_upgrade_sysoids.c
 *
 *	server-side functions to set backend global variables
 *	to control oid and relfilenode assignment
 *
 *	Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *	contrib/pg_upgrade_support/pg_upgrade_support.c
 */

#include "postgres.h"

#include "fmgr.h"
#include "catalog/dependency.h"
#include "catalog/pg_class.h"

/* THIS IS USED ONLY FOR PG >= 9.0 */

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

extern PGDLLIMPORT Oid binary_upgrade_next_pg_type_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_pg_type_array_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_pg_type_toast_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_heap_relfilenode;
extern PGDLLIMPORT Oid binary_upgrade_next_toast_relfilenode;
extern PGDLLIMPORT Oid binary_upgrade_next_index_relfilenode;
extern PGDLLIMPORT Oid binary_upgrade_next_pg_enum_oid;

Datum		set_next_pg_type_oid(PG_FUNCTION_ARGS);
Datum		set_next_pg_type_array_oid(PG_FUNCTION_ARGS);
Datum		set_next_pg_type_toast_oid(PG_FUNCTION_ARGS);
Datum		set_next_heap_relfilenode(PG_FUNCTION_ARGS);
Datum		set_next_toast_relfilenode(PG_FUNCTION_ARGS);
Datum		set_next_index_relfilenode(PG_FUNCTION_ARGS);
Datum		set_next_pg_enum_oid(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(set_next_pg_type_oid);
PG_FUNCTION_INFO_V1(set_next_pg_type_array_oid);
PG_FUNCTION_INFO_V1(set_next_pg_type_toast_oid);
PG_FUNCTION_INFO_V1(set_next_heap_relfilenode);
PG_FUNCTION_INFO_V1(set_next_toast_relfilenode);
PG_FUNCTION_INFO_V1(set_next_index_relfilenode);
PG_FUNCTION_INFO_V1(set_next_pg_enum_oid);

Datum
set_next_pg_type_oid(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);

	binary_upgrade_next_pg_type_oid = typoid;

	PG_RETURN_VOID();
}

Datum
set_next_pg_type_array_oid(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);

	binary_upgrade_next_pg_type_array_oid = typoid;

	PG_RETURN_VOID();
}

Datum
set_next_pg_type_toast_oid(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);

	binary_upgrade_next_pg_type_toast_oid = typoid;

	PG_RETURN_VOID();
}

Datum
set_next_heap_relfilenode(PG_FUNCTION_ARGS)
{
	Oid			relfilenode = PG_GETARG_OID(0);

	binary_upgrade_next_heap_relfilenode = relfilenode;

	PG_RETURN_VOID();
}

Datum
set_next_toast_relfilenode(PG_FUNCTION_ARGS)
{
	Oid			relfilenode = PG_GETARG_OID(0);

	binary_upgrade_next_toast_relfilenode = relfilenode;

	PG_RETURN_VOID();
}

Datum
set_next_index_relfilenode(PG_FUNCTION_ARGS)
{
	Oid			relfilenode = PG_GETARG_OID(0);

	binary_upgrade_next_index_relfilenode = relfilenode;

	PG_RETURN_VOID();
}

Datum
set_next_pg_enum_oid(PG_FUNCTION_ARGS)
{
	Oid			enumoid = PG_GETARG_OID(0);

	binary_upgrade_next_pg_enum_oid = enumoid;

	PG_RETURN_VOID();
}
