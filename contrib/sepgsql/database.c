/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/database.c
 *
 * Routines corresponding to database objects
 *
 * Copyright (c) 2010-2021, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "commands/seclabel.h"
#include "sepgsql.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/snapmgr.h"

/*
 * sepgsql_database_post_create
 *
 * This routine assigns a default security label on a newly defined
 * database, and check permission needed for its creation.
 */
void
sepgsql_database_post_create(Oid databaseId, const char *dtemplate)
{
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	tuple;
	char	   *tcontext;
	char	   *ncontext;
	ObjectAddress object;
	Form_pg_database datForm;
	StringInfoData audit_name;

	/*
	 * Oid of the source database is not saved in pg_database catalog, so we
	 * collect its identifier using contextual information. If NULL, its
	 * default is "template1" according to createdb().
	 */
	if (!dtemplate)
		dtemplate = "template1";

	object.classId = DatabaseRelationId;
	object.objectId = get_database_oid(dtemplate, false);
	object.objectSubId = 0;

	tcontext = sepgsql_get_label(object.classId,
								 object.objectId,
								 object.objectSubId);

	/*
	 * check db_database:{getattr} permission
	 */
	initStringInfo(&audit_name);
	appendStringInfoString(&audit_name, quote_identifier(dtemplate));
	sepgsql_avc_check_perms_label(tcontext,
								  SEPG_CLASS_DB_DATABASE,
								  SEPG_DB_DATABASE__GETATTR,
								  audit_name.data,
								  true);

	/*
	 * Compute a default security label of the newly created database based on
	 * a pair of security label of client and source database.
	 *
	 * XXX - upcoming version of libselinux supports to take object name to
	 * handle special treatment on default security label.
	 */
	rel = table_open(DatabaseRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				Anum_pg_database_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(databaseId));

	sscan = systable_beginscan(rel, DatabaseOidIndexId, true,
							   SnapshotSelf, 1, &skey);
	tuple = systable_getnext(sscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for database %u", databaseId);

	datForm = (Form_pg_database) GETSTRUCT(tuple);

	ncontext = sepgsql_compute_create(sepgsql_get_client_label(),
									  tcontext,
									  SEPG_CLASS_DB_DATABASE,
									  NameStr(datForm->datname));

	/*
	 * check db_database:{create} permission
	 */
	resetStringInfo(&audit_name);
	appendStringInfoString(&audit_name,
						   quote_identifier(NameStr(datForm->datname)));
	sepgsql_avc_check_perms_label(ncontext,
								  SEPG_CLASS_DB_DATABASE,
								  SEPG_DB_DATABASE__CREATE,
								  audit_name.data,
								  true);

	systable_endscan(sscan);
	table_close(rel, AccessShareLock);

	/*
	 * Assign the default security label on the new database
	 */
	object.classId = DatabaseRelationId;
	object.objectId = databaseId;
	object.objectSubId = 0;

	SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, ncontext);

	pfree(ncontext);
	pfree(tcontext);
}

/*
 * sepgsql_database_drop
 *
 * It checks privileges to drop the supplied database
 */
void
sepgsql_database_drop(Oid databaseId)
{
	ObjectAddress object;
	char	   *audit_name;

	/*
	 * check db_database:{drop} permission
	 */
	object.classId = DatabaseRelationId;
	object.objectId = databaseId;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object, false);

	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_DATABASE,
							SEPG_DB_DATABASE__DROP,
							audit_name,
							true);
	pfree(audit_name);
}

/*
 * sepgsql_database_post_alter
 *
 * It checks privileges to alter the supplied database
 */
void
sepgsql_database_setattr(Oid databaseId)
{
	ObjectAddress object;
	char	   *audit_name;

	/*
	 * check db_database:{setattr} permission
	 */
	object.classId = DatabaseRelationId;
	object.objectId = databaseId;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object, false);

	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_DATABASE,
							SEPG_DB_DATABASE__SETATTR,
							audit_name,
							true);
	pfree(audit_name);
}

/*
 * sepgsql_database_relabel
 *
 * It checks privileges to relabel the supplied database with the `seclabel'
 */
void
sepgsql_database_relabel(Oid databaseId, const char *seclabel)
{
	ObjectAddress object;
	char	   *audit_name;

	object.classId = DatabaseRelationId;
	object.objectId = databaseId;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object, false);

	/*
	 * check db_database:{setattr relabelfrom} permission
	 */
	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_DATABASE,
							SEPG_DB_DATABASE__SETATTR |
							SEPG_DB_DATABASE__RELABELFROM,
							audit_name,
							true);

	/*
	 * check db_database:{relabelto} permission
	 */
	sepgsql_avc_check_perms_label(seclabel,
								  SEPG_CLASS_DB_DATABASE,
								  SEPG_DB_DATABASE__RELABELTO,
								  audit_name,
								  true);
	pfree(audit_name);
}
