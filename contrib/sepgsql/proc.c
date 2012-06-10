/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/proc.c
 *
 * Routines corresponding to procedure objects
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
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "commands/seclabel.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/tqual.h"

#include "sepgsql.h"

/*
 * sepgsql_proc_post_create
 *
 * This routine assigns a default security label on a newly defined
 * procedure.
 */
void
sepgsql_proc_post_create(Oid functionId)
{
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	tuple;
	char	   *scontext;
	char	   *tcontext;
	char	   *ncontext;
	int			i;
	StringInfoData audit_name;
	ObjectAddress object;
	Form_pg_proc proForm;

	/*
	 * Fetch namespace of the new procedure. Because pg_proc entry is not
	 * visible right now, we need to scan the catalog using SnapshotSelf.
	 */
	rel = heap_open(ProcedureRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(functionId));

	sscan = systable_beginscan(rel, ProcedureOidIndexId, true,
							   SnapshotSelf, 1, &skey);

	tuple = systable_getnext(sscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "catalog lookup failed for proc %u", functionId);

	proForm = (Form_pg_proc) GETSTRUCT(tuple);

	/*
	 * check db_schema:{add_name} permission of the namespace
	 */
	object.classId = NamespaceRelationId;
	object.objectId = proForm->pronamespace;
	object.objectSubId = 0;
	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_SCHEMA,
							SEPG_DB_SCHEMA__ADD_NAME,
							getObjectDescription(&object),
							true);

	/*
	 * XXX - db_language:{implement} also should be checked here
	 */


	/*
	 * Compute a default security label when we create a new procedure object
	 * under the specified namespace.
	 */
	scontext = sepgsql_get_client_label();
	tcontext = sepgsql_get_label(NamespaceRelationId,
								 proForm->pronamespace, 0);
	ncontext = sepgsql_compute_create(scontext, tcontext,
									  SEPG_CLASS_DB_PROCEDURE);

	/*
	 * check db_procedure:{create} permission
	 */
	initStringInfo(&audit_name);
	appendStringInfo(&audit_name, "function %s(", NameStr(proForm->proname));
	for (i = 0; i < proForm->pronargs; i++)
	{
		Oid			typeoid = proForm->proargtypes.values[i];

		if (i > 0)
			appendStringInfoChar(&audit_name, ',');
		appendStringInfoString(&audit_name, format_type_be(typeoid));
	}
	appendStringInfoChar(&audit_name, ')');

	sepgsql_avc_check_perms_label(ncontext,
								  SEPG_CLASS_DB_PROCEDURE,
								  SEPG_DB_PROCEDURE__CREATE,
								  audit_name.data,
								  true);

	/*
	 * Assign the default security label on a new procedure
	 */
	object.classId = ProcedureRelationId;
	object.objectId = functionId;
	object.objectSubId = 0;
	SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, ncontext);

	/*
	 * Cleanup
	 */
	systable_endscan(sscan);
	heap_close(rel, AccessShareLock);

	pfree(audit_name.data);
	pfree(tcontext);
	pfree(ncontext);
}

/*
 * sepgsql_proc_drop
 *
 * It checks privileges to drop the supplied function.
 */
void
sepgsql_proc_drop(Oid functionId)
{
	ObjectAddress object;
	char	   *audit_name;

	/*
	 * check db_schema:{remove_name} permission
	 */
	object.classId = NamespaceRelationId;
	object.objectId = get_func_namespace(functionId);
	object.objectSubId = 0;
	audit_name = getObjectDescription(&object);

	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_SCHEMA,
							SEPG_DB_SCHEMA__REMOVE_NAME,
							audit_name,
							true);
	pfree(audit_name);

	/*
	 * check db_procedure:{drop} permission
	 */
	object.classId = ProcedureRelationId;
	object.objectId = functionId;
	object.objectSubId = 0;
	audit_name = getObjectDescription(&object);

	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_PROCEDURE,
							SEPG_DB_PROCEDURE__DROP,
							audit_name,
							true);
	pfree(audit_name);
}

/*
 * sepgsql_proc_relabel
 *
 * It checks privileges to relabel the supplied function
 * by the `seclabel'.
 */
void
sepgsql_proc_relabel(Oid functionId, const char *seclabel)
{
	ObjectAddress object;
	char	   *audit_name;

	object.classId = ProcedureRelationId;
	object.objectId = functionId;
	object.objectSubId = 0;
	audit_name = getObjectDescription(&object);

	/*
	 * check db_procedure:{setattr relabelfrom} permission
	 */
	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_PROCEDURE,
							SEPG_DB_PROCEDURE__SETATTR |
							SEPG_DB_PROCEDURE__RELABELFROM,
							audit_name,
							true);

	/*
	 * check db_procedure:{relabelto} permission
	 */
	sepgsql_avc_check_perms_label(seclabel,
								  SEPG_CLASS_DB_PROCEDURE,
								  SEPG_DB_PROCEDURE__RELABELTO,
								  audit_name,
								  true);
	pfree(audit_name);
}
