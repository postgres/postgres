/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/schema.c
 *
 * Routines corresponding to schema objects
 *
 * Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/dependency.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/seclabel.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"

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
	char	   *scontext;
	char	   *tcontext;
	char	   *ncontext;
	ObjectAddress object;

	/*
	 * Compute a default security label when we create a new schema object
	 * under the working database.
	 */
	scontext = sepgsql_get_client_label();
	tcontext = sepgsql_get_label(DatabaseRelationId, MyDatabaseId, 0);
	ncontext = sepgsql_compute_create(scontext, tcontext,
									  SEPG_CLASS_DB_SCHEMA);

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
 * sepgsql_schema_relabel
 *
 * It checks privileges to relabel the supplied schema
 * by the `seclabel'.
 */
void
sepgsql_schema_relabel(Oid namespaceId, const char *seclabel)
{
	ObjectAddress	object;
	char		   *audit_name;

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
