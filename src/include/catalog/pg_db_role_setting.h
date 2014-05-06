/*-------------------------------------------------------------------------
 *
 * pg_db_role_setting.h
 *	definition of configuration settings
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_db_role_setting.h,v 1.4 2010/02/26 02:01:21 momjian Exp $
 *
 * NOTES
 *		the genbki.pl script reads this file and generates .bki
 *		information from the DATA() statements.
 *
 *		XXX do NOT break up DATA() statements into multiple lines!
 *			the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DB_ROLE_SETTING_H
#define PG_DB_ROLE_SETTING_H

#include "catalog/genbki.h"
#include "nodes/parsenodes.h"
#include "utils/guc.h"
#include "utils/relcache.h"

/* ----------------
 *		pg_db_role_setting definition.  cpp turns this into
 *		typedef struct FormData_pg_db_role_setting
 * ----------------
 */
#define DbRoleSettingRelationId 2964

CATALOG(pg_db_role_setting,2964) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	Oid			setdatabase;	/* database */
	Oid			setrole;		/* role */
	text		setconfig[1];	/* GUC settings to apply at login */
} FormData_pg_db_role_setting;

typedef FormData_pg_db_role_setting *Form_pg_db_role_setting;

/* ----------------
 *		compiler constants for pg_db_role_setting
 * ----------------
 */
#define Natts_pg_db_role_setting				3
#define Anum_pg_db_role_setting_setdatabase		1
#define Anum_pg_db_role_setting_setrole			2
#define Anum_pg_db_role_setting_setconfig		3

/* ----------------
 *		initial contents of pg_db_role_setting are NOTHING
 * ----------------
 */

/*
 * prototypes for functions in pg_db_role_setting.h
 */
extern void AlterSetting(Oid databaseid, Oid roleid, VariableSetStmt *setstmt);
extern void DropSetting(Oid databaseid, Oid roleid);
extern void ApplySetting(Oid databaseid, Oid roleid, Relation relsetting,
			 GucSource source);

#endif   /* PG_DB_ROLE_SETTING_H */
