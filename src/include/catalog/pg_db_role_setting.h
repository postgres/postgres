/*-------------------------------------------------------------------------
 *
 * pg_db_role_setting.h
 *	  definition of the system catalog for per-database/per-user
 *	  configuration settings (pg_db_role_setting)
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_db_role_setting.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DB_ROLE_SETTING_H
#define PG_DB_ROLE_SETTING_H

#include "catalog/genbki.h"
#include "catalog/pg_db_role_setting_d.h"	/* IWYU pragma: export */

#include "utils/guc.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/* ----------------
 *		pg_db_role_setting definition.  cpp turns this into
 *		typedef struct FormData_pg_db_role_setting
 * ----------------
 */
CATALOG(pg_db_role_setting,2964,DbRoleSettingRelationId) BKI_SHARED_RELATION
{
	/* database, or 0 for a role-specific setting */
	Oid			setdatabase BKI_LOOKUP_OPT(pg_database);

	/* role, or 0 for a database-specific setting */
	Oid			setrole BKI_LOOKUP_OPT(pg_authid);

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		setconfig[1];	/* GUC settings to apply at login */
#endif
} FormData_pg_db_role_setting;

typedef FormData_pg_db_role_setting * Form_pg_db_role_setting;

DECLARE_TOAST_WITH_MACRO(pg_db_role_setting, 2966, 2967, PgDbRoleSettingToastTable, PgDbRoleSettingToastIndex);

DECLARE_UNIQUE_INDEX_PKEY(pg_db_role_setting_databaseid_rol_index, 2965, DbRoleSettingDatidRolidIndexId, pg_db_role_setting, btree(setdatabase oid_ops, setrole oid_ops));

/*
 * prototypes for functions in pg_db_role_setting.c
 */
extern void AlterSetting(Oid databaseid, Oid roleid, VariableSetStmt *setstmt);
extern void DropSetting(Oid databaseid, Oid roleid);
extern void ApplySetting(Snapshot snapshot, Oid databaseid, Oid roleid,
						 Relation relsetting, GucSource source);

#endif							/* PG_DB_ROLE_SETTING_H */
