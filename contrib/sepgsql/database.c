/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/database.c
 *
 * Routines corresponding to database objects
 *
 * Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/dependency.h"
#include "catalog/pg_database.h"
#include "commands/seclabel.h"
#include "sepgsql.h"

void
sepgsql_database_post_create(Oid databaseId)
{
	char   *scontext = sepgsql_get_client_label();
	char   *tcontext;
	char   *ncontext;
	ObjectAddress	object;

	/*
	 * Compute a default security label of the newly created database
	 * based on a pair of security label of client and source database.
	 *
	 * XXX - Right now, this logic uses "template1" as its source, because
	 * here is no way to know the Oid of source database.
	 */
	object.classId = DatabaseRelationId;
	object.objectId = TemplateDbOid;
	object.objectSubId = 0;
	tcontext = GetSecurityLabel(&object, SEPGSQL_LABEL_TAG);

	ncontext = sepgsql_compute_create(scontext, tcontext,
									  SEPG_CLASS_DB_DATABASE);

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
 * sepgsql_database_relabel
 *
 * It checks privileges to relabel the supplied database with the `seclabel'
 */
void
sepgsql_database_relabel(Oid databaseId, const char *seclabel)
{
	ObjectAddress	object;
	char		   *audit_name;

	object.classId = DatabaseRelationId;
	object.objectId = databaseId;
	object.objectSubId = 0;
	audit_name = getObjectDescription(&object);

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
