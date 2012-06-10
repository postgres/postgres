/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/schema.c
 *
 * Routines corresponding to schema objects
 *
 * Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/seclabel.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/tqual.h"

#include "sepgsql.h"

/*
 * sepgsql_schema_post_create
 *
 * This routine assigns a default security label on a newly defined
 * schema.
 */
void
sepgsql_schema_post_create(Oid namespaceId)
{
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	tuple;
	char	   *tcontext;
	char	   *ncontext;
	char		audit_name[NAMEDATALEN + 20];
	ObjectAddress object;
	Form_pg_namespace nspForm;

	/*
	 * Compute a default security label when we create a new schema object
	 * under the working database.
	 *
	 * XXX - uncoming version of libselinux supports to take object name to
	 * handle special treatment on default security label; such as special
	 * label on "pg_temp" schema.
	 */
	rel = heap_open(NamespaceRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(namespaceId));

	sscan = systable_beginscan(rel, NamespaceOidIndexId, true,
							   SnapshotSelf, 1, &skey);
	tuple = systable_getnext(sscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "catalog lookup failed for namespace %u", namespaceId);

	nspForm = (Form_pg_namespace) GETSTRUCT(tuple);

	tcontext = sepgsql_get_label(DatabaseRelationId, MyDatabaseId, 0);
	ncontext = sepgsql_compute_create(sepgsql_get_client_label(),
									  tcontext,
									  SEPG_CLASS_DB_SCHEMA);

	/*
	 * check db_schema:{create}
	 */
	snprintf(audit_name, sizeof(audit_name),
			 "schema %s", NameStr(nspForm->nspname));
	sepgsql_avc_check_perms_label(ncontext,
								  SEPG_CLASS_DB_SCHEMA,
								  SEPG_DB_SCHEMA__CREATE,
								  audit_name,
								  true);
	systable_endscan(sscan);
	heap_close(rel, AccessShareLock);

	/*
	 * Assign the default security label on a new procedure
	 */
	object.classId = NamespaceRelationId;
	object.objectId = namespaceId;
	object.objectSubId = 0;
	SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, ncontext);

	pfree(ncontext);
	pfree(tcontext);
}

/*
 * sepgsql_schema_drop
 *
 * It checks privileges to drop the supplied schema object.
 */
void
sepgsql_schema_drop(Oid namespaceId)
{
	ObjectAddress object;
	char	   *audit_name;

	/*
	 * check db_schema:{drop} permission
	 */
	object.classId = NamespaceRelationId;
	object.objectId = namespaceId;
	object.objectSubId = 0;
	audit_name = getObjectDescription(&object);

	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_SCHEMA,
							SEPG_DB_SCHEMA__DROP,
							audit_name,
							true);
	pfree(audit_name);
}

/*
 * sepgsql_schema_relabel
 *
 * It checks privileges to relabel the supplied schema
 * by the `seclabel'.
 */
void
sepgsql_schema_relabel(Oid namespaceId, const char *seclabel)
{
	ObjectAddress object;
	char	   *audit_name;

	object.classId = NamespaceRelationId;
	object.objectId = namespaceId;
	object.objectSubId = 0;
	audit_name = getObjectDescription(&object);

	/*
	 * check db_schema:{setattr relabelfrom} permission
	 */
	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_SCHEMA,
							SEPG_DB_SCHEMA__SETATTR |
							SEPG_DB_SCHEMA__RELABELFROM,
							audit_name,
							true);

	/*
	 * check db_schema:{relabelto} permission
	 */
	sepgsql_avc_check_perms_label(seclabel,
								  SEPG_CLASS_DB_SCHEMA,
								  SEPG_DB_SCHEMA__RELABELTO,
								  audit_name,
								  true);
	pfree(audit_name);
}
