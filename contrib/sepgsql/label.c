/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/label.c
 *
 * Routines to support SELinux labels (security context)
 *
 * Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/genam.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "commands/dbcommands.h"
#include "commands/seclabel.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tqual.h"

#include "sepgsql.h"

#include <selinux/label.h>

/*
 * client_label
 *
 * security label of the client process
 */
static char *client_label = NULL;

char *
sepgsql_get_client_label(void)
{
	return client_label;
}

char *
sepgsql_set_client_label(char *new_label)
{
	char	   *old_label = client_label;

	client_label = new_label;

	return old_label;
}

/*
 * sepgsql_get_label
 *
 * It returns a security context of the specified database object.
 * If unlabeled or incorrectly labeled, the system "unlabeled" label
 * shall be returned.
 */
char *
sepgsql_get_label(Oid classId, Oid objectId, int32 subId)
{
	ObjectAddress object;
	char	   *label;

	object.classId = classId;
	object.objectId = objectId;
	object.objectSubId = subId;

	label = GetSecurityLabel(&object, SEPGSQL_LABEL_TAG);
	if (!label || security_check_context_raw((security_context_t) label))
	{
		security_context_t unlabeled;

		if (security_get_initial_context_raw("unlabeled", &unlabeled) < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
			   errmsg("SELinux: failed to get initial security label: %m")));
		PG_TRY();
		{
			label = pstrdup(unlabeled);
		}
		PG_CATCH();
		{
			freecon(unlabeled);
			PG_RE_THROW();
		}
		PG_END_TRY();

		freecon(unlabeled);
	}
	return label;
}

/*
 * sepgsql_object_relabel
 *
 * An entrypoint of SECURITY LABEL statement
 */
void
sepgsql_object_relabel(const ObjectAddress *object, const char *seclabel)
{
	/*
	 * validate format of the supplied security label, if it is security
	 * context of selinux.
	 */
	if (seclabel &&
		security_check_context_raw((security_context_t) seclabel) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
			   errmsg("SELinux: invalid security label: \"%s\"", seclabel)));

	/*
	 * Do actual permission checks for each object classes
	 */
	switch (object->classId)
	{
		case NamespaceRelationId:
			sepgsql_schema_relabel(object->objectId, seclabel);
			break;
		case RelationRelationId:
			if (object->objectSubId == 0)
				sepgsql_relation_relabel(object->objectId,
										 seclabel);
			else
				sepgsql_attribute_relabel(object->objectId,
										  object->objectSubId,
										  seclabel);
			break;
		case ProcedureRelationId:
			sepgsql_proc_relabel(object->objectId, seclabel);
			break;

		default:
			elog(ERROR, "unsupported object type: %u", object->classId);
			break;
	}
}

/*
 * TEXT sepgsql_getcon(VOID)
 *
 * It returns the security label of the client.
 */
PG_FUNCTION_INFO_V1(sepgsql_getcon);
Datum
sepgsql_getcon(PG_FUNCTION_ARGS)
{
	char	   *client_label;

	if (!sepgsql_is_enabled())
		PG_RETURN_NULL();

	client_label = sepgsql_get_client_label();

	PG_RETURN_TEXT_P(cstring_to_text(client_label));
}

/*
 * TEXT sepgsql_mcstrans_in(TEXT)
 *
 * It translate the given qualified MLS/MCS range into raw format
 * when mcstrans daemon is working.
 */
PG_FUNCTION_INFO_V1(sepgsql_mcstrans_in);
Datum
sepgsql_mcstrans_in(PG_FUNCTION_ARGS)
{
	text	   *label = PG_GETARG_TEXT_P(0);
	char	   *raw_label;
	char	   *result;

	if (!sepgsql_is_enabled())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sepgsql is not enabled")));

	if (selinux_trans_to_raw_context(text_to_cstring(label),
									 &raw_label) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: could not translate security label: %m")));

	PG_TRY();
	{
		result = pstrdup(raw_label);
	}
	PG_CATCH();
	{
		freecon(raw_label);
		PG_RE_THROW();
	}
	PG_END_TRY();
	freecon(raw_label);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * TEXT sepgsql_mcstrans_out(TEXT)
 *
 * It translate the given raw MLS/MCS range into qualified format
 * when mcstrans daemon is working.
 */
PG_FUNCTION_INFO_V1(sepgsql_mcstrans_out);
Datum
sepgsql_mcstrans_out(PG_FUNCTION_ARGS)
{
	text	   *label = PG_GETARG_TEXT_P(0);
	char	   *qual_label;
	char	   *result;

	if (!sepgsql_is_enabled())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sepgsql is not currently enabled")));

	if (selinux_raw_to_trans_context(text_to_cstring(label),
									 &qual_label) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: could not translate security label: %m")));

	PG_TRY();
	{
		result = pstrdup(qual_label);
	}
	PG_CATCH();
	{
		freecon(qual_label);
		PG_RE_THROW();
	}
	PG_END_TRY();
	freecon(qual_label);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * quote_object_names
 *
 * It tries to quote the supplied identifiers
 */
static char *
quote_object_name(const char *src1, const char *src2,
				  const char *src3, const char *src4)
{
	StringInfoData result;
	const char *temp;

	initStringInfo(&result);

	if (src1)
	{
		temp = quote_identifier(src1);
		appendStringInfo(&result, "%s", temp);
		if (src1 != temp)
			pfree((void *) temp);
	}
	if (src2)
	{
		temp = quote_identifier(src2);
		appendStringInfo(&result, ".%s", temp);
		if (src2 != temp)
			pfree((void *) temp);
	}
	if (src3)
	{
		temp = quote_identifier(src3);
		appendStringInfo(&result, ".%s", temp);
		if (src3 != temp)
			pfree((void *) temp);
	}
	if (src4)
	{
		temp = quote_identifier(src4);
		appendStringInfo(&result, ".%s", temp);
		if (src4 != temp)
			pfree((void *) temp);
	}
	return result.data;
}

/*
 * exec_object_restorecon
 *
 * This routine is a helper called by sepgsql_restorecon; it set up
 * initial security labels of database objects within the supplied
 * catalog OID.
 */
static void
exec_object_restorecon(struct selabel_handle * sehnd, Oid catalogId)
{
	Relation	rel;
	SysScanDesc sscan;
	HeapTuple	tuple;
	char	   *database_name = get_database_name(MyDatabaseId);
	char	   *namespace_name;
	Oid			namespace_id;
	char	   *relation_name;

	/*
	 * Open the target catalog. We don't want to allow writable accesses by
	 * other session during initial labeling.
	 */
	rel = heap_open(catalogId, AccessShareLock);

	sscan = systable_beginscan(rel, InvalidOid, false,
							   SnapshotNow, 0, NULL);
	while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		Form_pg_namespace nspForm;
		Form_pg_class relForm;
		Form_pg_attribute attForm;
		Form_pg_proc proForm;
		char	   *objname;
		int			objtype = 1234;
		ObjectAddress object;
		security_context_t context;

		/*
		 * The way to determine object name depends on object classes. So, any
		 * branches set up `objtype', `objname' and `object' here.
		 */
		switch (catalogId)
		{
			case NamespaceRelationId:
				nspForm = (Form_pg_namespace) GETSTRUCT(tuple);

				objtype = SELABEL_DB_SCHEMA;

				objname = quote_object_name(database_name,
											NameStr(nspForm->nspname),
											NULL, NULL);

				object.classId = NamespaceRelationId;
				object.objectId = HeapTupleGetOid(tuple);
				object.objectSubId = 0;
				break;

			case RelationRelationId:
				relForm = (Form_pg_class) GETSTRUCT(tuple);

				if (relForm->relkind == RELKIND_RELATION)
					objtype = SELABEL_DB_TABLE;
				else if (relForm->relkind == RELKIND_SEQUENCE)
					objtype = SELABEL_DB_SEQUENCE;
				else if (relForm->relkind == RELKIND_VIEW)
					objtype = SELABEL_DB_VIEW;
				else
					continue;	/* no need to assign security label */

				namespace_name = get_namespace_name(relForm->relnamespace);
				objname = quote_object_name(database_name,
											namespace_name,
											NameStr(relForm->relname),
											NULL);
				pfree(namespace_name);

				object.classId = RelationRelationId;
				object.objectId = HeapTupleGetOid(tuple);
				object.objectSubId = 0;
				break;

			case AttributeRelationId:
				attForm = (Form_pg_attribute) GETSTRUCT(tuple);

				if (get_rel_relkind(attForm->attrelid) != RELKIND_RELATION)
					continue;	/* no need to assign security label */

				objtype = SELABEL_DB_COLUMN;

				namespace_id = get_rel_namespace(attForm->attrelid);
				namespace_name = get_namespace_name(namespace_id);
				relation_name = get_rel_name(attForm->attrelid);
				objname = quote_object_name(database_name,
											namespace_name,
											relation_name,
											NameStr(attForm->attname));
				pfree(namespace_name);
				pfree(relation_name);

				object.classId = RelationRelationId;
				object.objectId = attForm->attrelid;
				object.objectSubId = attForm->attnum;
				break;

			case ProcedureRelationId:
				proForm = (Form_pg_proc) GETSTRUCT(tuple);

				objtype = SELABEL_DB_PROCEDURE;

				namespace_name = get_namespace_name(proForm->pronamespace);
				objname = quote_object_name(database_name,
											namespace_name,
											NameStr(proForm->proname),
											NULL);
				pfree(namespace_name);

				object.classId = ProcedureRelationId;
				object.objectId = HeapTupleGetOid(tuple);
				object.objectSubId = 0;
				break;

			default:
				elog(ERROR, "unexpected catalog id: %u", catalogId);
				objname = NULL; /* for compiler quiet */
				break;
		}

		if (selabel_lookup_raw(sehnd, &context, objname, objtype) == 0)
		{
			PG_TRY();
			{
				/*
				 * Check SELinux permission to relabel the fetched object,
				 * then do the actual relabeling.
				 */
				sepgsql_object_relabel(&object, context);

				SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, context);
			}
			PG_CATCH();
			{
				freecon(context);
				PG_RE_THROW();
			}
			PG_END_TRY();
			freecon(context);
		}
		else if (errno == ENOENT)
			ereport(WARNING,
					(errmsg("SELinux: no initial label assigned for %s (type=%d), skipping",
							objname, objtype)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SELinux: could not determine initial security label for %s (type=%d): %m", objname, objtype)));

		pfree(objname);
	}
	systable_endscan(sscan);

	heap_close(rel, NoLock);
}

/*
 * BOOL sepgsql_restorecon(TEXT specfile)
 *
 * This function tries to assign initial security labels on all the object
 * within the current database, according to the system setting.
 * It is typically invoked by sepgsql-install script just after initdb, to
 * assign initial security labels.
 *
 * If @specfile is not NULL, it uses explicitly specified specfile, instead
 * of the system default.
 */
PG_FUNCTION_INFO_V1(sepgsql_restorecon);
Datum
sepgsql_restorecon(PG_FUNCTION_ARGS)
{
	struct selabel_handle *sehnd;
	struct selinux_opt seopts;

	/*
	 * SELinux has to be enabled on the running platform.
	 */
	if (!sepgsql_is_enabled())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sepgsql is not currently enabled")));

	/*
	 * Check DAC permission. Only superuser can set up initial security
	 * labels, like root-user in filesystems
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		  errmsg("SELinux: must be superuser to restore initial contexts")));

	/*
	 * Open selabel_lookup(3) stuff. It provides a set of mapping between an
	 * initial security label and object class/name due to the system setting.
	 */
	if (PG_ARGISNULL(0))
	{
		seopts.type = SELABEL_OPT_UNUSED;
		seopts.value = NULL;
	}
	else
	{
		seopts.type = SELABEL_OPT_PATH;
		seopts.value = TextDatumGetCString(PG_GETARG_DATUM(0));
	}
	sehnd = selabel_open(SELABEL_CTX_DB, &seopts, 1);
	if (!sehnd)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
			   errmsg("SELinux: failed to initialize labeling handle: %m")));
	PG_TRY();
	{
		/*
		 * Right now, we have no support labeling on the shared database
		 * objects, such as database, role, or tablespace.
		 */
		exec_object_restorecon(sehnd, NamespaceRelationId);
		exec_object_restorecon(sehnd, RelationRelationId);
		exec_object_restorecon(sehnd, AttributeRelationId);
		exec_object_restorecon(sehnd, ProcedureRelationId);
	}
	PG_CATCH();
	{
		selabel_close(sehnd);
		PG_RE_THROW();
	}
	PG_END_TRY();

	selabel_close(sehnd);

	PG_RETURN_BOOL(true);
}
