/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/proc.c
 *
 * Routines corresponding to procedure objects
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/seclabel.h"
#include "lib/stringinfo.h"
#include "sepgsql.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

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
	char	   *nsp_name;
	char	   *scontext;
	char	   *tcontext;
	char	   *ncontext;
	uint32		required;
	int			i;
	StringInfoData audit_name;
	ObjectAddress object;
	Form_pg_proc proForm;

	/*
	 * Fetch namespace of the new procedure. Because pg_proc entry is not
	 * visible right now, we need to scan the catalog using SnapshotSelf.
	 */
	rel = table_open(ProcedureRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				Anum_pg_proc_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(functionId));

	sscan = systable_beginscan(rel, ProcedureOidIndexId, true,
							   SnapshotSelf, 1, &skey);

	tuple = systable_getnext(sscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for function %u", functionId);

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
							getObjectIdentity(&object, false),
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
									  SEPG_CLASS_DB_PROCEDURE,
									  NameStr(proForm->proname));

	/*
	 * check db_procedure:{create (install)} permission
	 */
	initStringInfo(&audit_name);
	nsp_name = get_namespace_name(proForm->pronamespace);
	appendStringInfo(&audit_name, "%s(",
					 quote_qualified_identifier(nsp_name, NameStr(proForm->proname)));
	for (i = 0; i < proForm->pronargs; i++)
	{
		if (i > 0)
			appendStringInfoChar(&audit_name, ',');

		object.classId = TypeRelationId;
		object.objectId = proForm->proargtypes.values[i];
		object.objectSubId = 0;
		appendStringInfoString(&audit_name, getObjectIdentity(&object, false));
	}
	appendStringInfoChar(&audit_name, ')');

	required = SEPG_DB_PROCEDURE__CREATE;
	if (proForm->proleakproof)
		required |= SEPG_DB_PROCEDURE__INSTALL;

	sepgsql_avc_check_perms_label(ncontext,
								  SEPG_CLASS_DB_PROCEDURE,
								  required,
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
	table_close(rel, AccessShareLock);

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
	audit_name = getObjectIdentity(&object, false);

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
	audit_name = getObjectIdentity(&object, false);

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
	audit_name = getObjectIdentity(&object, false);

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

/*
 * sepgsql_proc_setattr
 *
 * It checks privileges to alter the supplied function.
 */
void
sepgsql_proc_setattr(Oid functionId)
{
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	oldtup;
	HeapTuple	newtup;
	Form_pg_proc oldform;
	Form_pg_proc newform;
	uint32		required;
	ObjectAddress object;
	char	   *audit_name;

	/*
	 * Fetch newer catalog
	 */
	rel = table_open(ProcedureRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				Anum_pg_proc_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(functionId));

	sscan = systable_beginscan(rel, ProcedureOidIndexId, true,
							   SnapshotSelf, 1, &skey);
	newtup = systable_getnext(sscan);
	if (!HeapTupleIsValid(newtup))
		elog(ERROR, "could not find tuple for function %u", functionId);
	newform = (Form_pg_proc) GETSTRUCT(newtup);

	/*
	 * Fetch older catalog
	 */
	oldtup = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionId));
	if (!HeapTupleIsValid(oldtup))
		elog(ERROR, "cache lookup failed for function %u", functionId);
	oldform = (Form_pg_proc) GETSTRUCT(oldtup);

	/*
	 * Does this ALTER command takes operation to namespace?
	 */
	if (newform->pronamespace != oldform->pronamespace)
	{
		sepgsql_schema_remove_name(oldform->pronamespace);
		sepgsql_schema_add_name(oldform->pronamespace);
	}
	if (strcmp(NameStr(newform->proname), NameStr(oldform->proname)) != 0)
		sepgsql_schema_rename(oldform->pronamespace);

	/*
	 * check db_procedure:{setattr (install)} permission
	 */
	required = SEPG_DB_PROCEDURE__SETATTR;
	if (!oldform->proleakproof && newform->proleakproof)
		required |= SEPG_DB_PROCEDURE__INSTALL;

	object.classId = ProcedureRelationId;
	object.objectId = functionId;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object, false);

	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_PROCEDURE,
							required,
							audit_name,
							true);
	/* cleanups */
	pfree(audit_name);

	ReleaseSysCache(oldtup);
	systable_endscan(sscan);
	table_close(rel, AccessShareLock);
}

/*
 * sepgsql_proc_execute
 *
 * It checks privileges to execute the supplied function
 */
void
sepgsql_proc_execute(Oid functionId)
{
	ObjectAddress object;
	char	   *audit_name;

	/*
	 * check db_procedure:{execute} permission
	 */
	object.classId = ProcedureRelationId;
	object.objectId = functionId;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object, false);
	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_PROCEDURE,
							SEPG_DB_PROCEDURE__EXECUTE,
							audit_name,
							true);
	pfree(audit_name);
}
