/*
 *	pg_upgrade_sysoids.c
 *
 *	server-side functions to set backend global variables
 *	to control oid and relfilenode assignment
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade_support/pg_upgrade_support.c,v 1.5 2010/07/06 19:18:55 momjian Exp $
 */

#include "postgres.h"

#include "fmgr.h"
#include "catalog/dependency.h"
#include "catalog/pg_class.h"

/* THIS IS USED ONLY FOR PG >= 9.0 */

/*
 * Cannot include "catalog/pg_enum.h" here because we might
 * not be compiling against PG 9.0.
 */
extern void EnumValuesCreate(Oid enumTypeOid, List *vals,
				 Oid binary_upgrade_next_pg_enum_oid);

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

extern PGDLLIMPORT Oid binary_upgrade_next_pg_type_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_pg_type_array_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_pg_type_toast_oid;
extern PGDLLIMPORT Oid binary_upgrade_next_heap_relfilenode;
extern PGDLLIMPORT Oid binary_upgrade_next_toast_relfilenode;
extern PGDLLIMPORT Oid binary_upgrade_next_index_relfilenode;

Datum		set_next_pg_type_oid(PG_FUNCTION_ARGS);
Datum		set_next_pg_type_array_oid(PG_FUNCTION_ARGS);
Datum		set_next_pg_type_toast_oid(PG_FUNCTION_ARGS);
Datum		set_next_heap_relfilenode(PG_FUNCTION_ARGS);
Datum		set_next_toast_relfilenode(PG_FUNCTION_ARGS);
Datum		set_next_index_relfilenode(PG_FUNCTION_ARGS);
Datum		add_pg_enum_label(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(set_next_pg_type_oid);
PG_FUNCTION_INFO_V1(set_next_pg_type_array_oid);
PG_FUNCTION_INFO_V1(set_next_pg_type_toast_oid);
PG_FUNCTION_INFO_V1(set_next_heap_relfilenode);
PG_FUNCTION_INFO_V1(set_next_toast_relfilenode);
PG_FUNCTION_INFO_V1(set_next_index_relfilenode);
PG_FUNCTION_INFO_V1(add_pg_enum_label);

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
add_pg_enum_label(PG_FUNCTION_ARGS)
{
	Oid			enumoid = PG_GETARG_OID(0);
	Oid			typoid = PG_GETARG_OID(1);
	Name		label = PG_GETARG_NAME(2);

	EnumValuesCreate(typoid, list_make1(makeString(NameStr(*label))),
					 enumoid);

	PG_RETURN_VOID();
}
