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
#include "catalog/pg_namespace.h"
#include "commands/seclabel.h"
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
	char	   *scontext = sepgsql_get_client_label();
	char	   *tcontext;
	char	   *ncontext;
	ObjectAddress object;

	/*
	 * FIXME: Right now, we assume pg_database object has a fixed security
	 * label, because pg_seclabel does not support to store label of shared
	 * database objects.
	 */
	tcontext = "system_u:object_r:sepgsql_db_t:s0";

	/*
	 * Compute a default security label when we create a new schema object
	 * under the working database.
	 */
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
	char	   *scontext = sepgsql_get_client_label();
	char	   *tcontext;
	char	   *audit_name;

	audit_name = getObjectDescriptionOids(NamespaceRelationId, namespaceId);

	/*
	 * check db_schema:{setattr relabelfrom} permission
	 */
	tcontext = sepgsql_get_label(NamespaceRelationId, namespaceId, 0);

	sepgsql_check_perms(scontext,
						tcontext,
						SEPG_CLASS_DB_SCHEMA,
						SEPG_DB_SCHEMA__SETATTR |
						SEPG_DB_SCHEMA__RELABELFROM,
						audit_name,
						true);

	/*
	 * check db_schema:{relabelto} permission
	 */
	sepgsql_check_perms(scontext,
						seclabel,
						SEPG_CLASS_DB_SCHEMA,
						SEPG_DB_SCHEMA__RELABELTO,
						audit_name,
						true);

	pfree(tcontext);
	pfree(audit_name);
}
