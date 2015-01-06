/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/relation.c
 *
 * Routines corresponding to relation/attribute objects
 *
 * Copyright (c) 2010-2015, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/indexing.h"
#include "catalog/dependency.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "commands/seclabel.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tqual.h"

#include "sepgsql.h"

static void sepgsql_index_modify(Oid indexOid);

/*
 * sepgsql_attribute_post_create
 *
 * This routine assigns a default security label on a newly defined
 * column, using ALTER TABLE ... ADD COLUMN.
 * Note that this routine is not invoked in the case of CREATE TABLE,
 * although it also defines columns in addition to table.
 */
void
sepgsql_attribute_post_create(Oid relOid, AttrNumber attnum)
{
	Relation	rel;
	ScanKeyData skey[2];
	SysScanDesc sscan;
	HeapTuple	tuple;
	char	   *scontext;
	char	   *tcontext;
	char	   *ncontext;
	ObjectAddress object;
	Form_pg_attribute attForm;
	StringInfoData audit_name;

	/*
	 * Only attributes within regular relation have individual security
	 * labels.
	 */
	if (get_rel_relkind(relOid) != RELKIND_RELATION)
		return;

	/*
	 * Compute a default security label of the new column underlying the
	 * specified relation, and check permission to create it.
	 */
	rel = heap_open(AttributeRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relOid));
	ScanKeyInit(&skey[1],
				Anum_pg_attribute_attnum,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));

	sscan = systable_beginscan(rel, AttributeRelidNumIndexId, true,
							   SnapshotSelf, 2, &skey[0]);

	tuple = systable_getnext(sscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "catalog lookup failed for column %d of relation %u",
			 attnum, relOid);

	attForm = (Form_pg_attribute) GETSTRUCT(tuple);

	scontext = sepgsql_get_client_label();
	tcontext = sepgsql_get_label(RelationRelationId, relOid, 0);
	ncontext = sepgsql_compute_create(scontext, tcontext,
									  SEPG_CLASS_DB_COLUMN,
									  NameStr(attForm->attname));

	/*
	 * check db_column:{create} permission
	 */
	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = 0;

	initStringInfo(&audit_name);
	appendStringInfo(&audit_name, "%s.%s",
					 getObjectIdentity(&object),
					 quote_identifier(NameStr(attForm->attname)));
	sepgsql_avc_check_perms_label(ncontext,
								  SEPG_CLASS_DB_COLUMN,
								  SEPG_DB_COLUMN__CREATE,
								  audit_name.data,
								  true);

	/*
	 * Assign the default security label on a new procedure
	 */
	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = attnum;
	SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, ncontext);

	systable_endscan(sscan);
	heap_close(rel, AccessShareLock);

	pfree(tcontext);
	pfree(ncontext);
}

/*
 * sepgsql_attribute_drop
 *
 * It checks privileges to drop the supplied column.
 */
void
sepgsql_attribute_drop(Oid relOid, AttrNumber attnum)
{
	ObjectAddress object;
	char	   *audit_name;

	if (get_rel_relkind(relOid) != RELKIND_RELATION)
		return;

	/*
	 * check db_column:{drop} permission
	 */
	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = attnum;
	audit_name = getObjectIdentity(&object);

	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_COLUMN,
							SEPG_DB_COLUMN__DROP,
							audit_name,
							true);
	pfree(audit_name);
}

/*
 * sepgsql_attribute_relabel
 *
 * It checks privileges to relabel the supplied column
 * by the `seclabel'.
 */
void
sepgsql_attribute_relabel(Oid relOid, AttrNumber attnum,
						  const char *seclabel)
{
	ObjectAddress object;
	char	   *audit_name;

	if (get_rel_relkind(relOid) != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot set security label on non-regular columns")));

	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = attnum;
	audit_name = getObjectIdentity(&object);

	/*
	 * check db_column:{setattr relabelfrom} permission
	 */
	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_COLUMN,
							SEPG_DB_COLUMN__SETATTR |
							SEPG_DB_COLUMN__RELABELFROM,
							audit_name,
							true);

	/*
	 * check db_column:{relabelto} permission
	 */
	sepgsql_avc_check_perms_label(seclabel,
								  SEPG_CLASS_DB_COLUMN,
								  SEPG_DB_PROCEDURE__RELABELTO,
								  audit_name,
								  true);
	pfree(audit_name);
}

/*
 * sepgsql_attribute_setattr
 *
 * It checks privileges to alter the supplied column.
 */
void
sepgsql_attribute_setattr(Oid relOid, AttrNumber attnum)
{
	ObjectAddress object;
	char	   *audit_name;

	if (get_rel_relkind(relOid) != RELKIND_RELATION)
		return;

	/*
	 * check db_column:{setattr} permission
	 */
	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = attnum;
	audit_name = getObjectIdentity(&object);

	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_COLUMN,
							SEPG_DB_COLUMN__SETATTR,
							audit_name,
							true);
	pfree(audit_name);
}

/*
 * sepgsql_relation_post_create
 *
 * The post creation hook of relation/attribute
 */
void
sepgsql_relation_post_create(Oid relOid)
{
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	tuple;
	Form_pg_class classForm;
	ObjectAddress object;
	uint16		tclass;
	char	   *scontext;		/* subject */
	char	   *tcontext;		/* schema */
	char	   *rcontext;		/* relation */
	char	   *ccontext;		/* column */
	char	   *nsp_name;
	StringInfoData audit_name;

	/*
	 * Fetch catalog record of the new relation. Because pg_class entry is not
	 * visible right now, we need to scan the catalog using SnapshotSelf.
	 */
	rel = heap_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relOid));

	sscan = systable_beginscan(rel, ClassOidIndexId, true,
							   SnapshotSelf, 1, &skey);

	tuple = systable_getnext(sscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "catalog lookup failed for relation %u", relOid);

	classForm = (Form_pg_class) GETSTRUCT(tuple);

	/* ignore indexes on toast tables */
	if (classForm->relkind == RELKIND_INDEX &&
		classForm->relnamespace == PG_TOAST_NAMESPACE)
		goto out;

	/*
	 * check db_schema:{add_name} permission of the namespace
	 */
	object.classId = NamespaceRelationId;
	object.objectId = classForm->relnamespace;
	object.objectSubId = 0;
	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_SCHEMA,
							SEPG_DB_SCHEMA__ADD_NAME,
							getObjectIdentity(&object),
							true);

	switch (classForm->relkind)
	{
		case RELKIND_RELATION:
			tclass = SEPG_CLASS_DB_TABLE;
			break;
		case RELKIND_SEQUENCE:
			tclass = SEPG_CLASS_DB_SEQUENCE;
			break;
		case RELKIND_VIEW:
			tclass = SEPG_CLASS_DB_VIEW;
			break;
		case RELKIND_INDEX:
			/* deal with indexes specially; no need for tclass */
			sepgsql_index_modify(relOid);
			goto out;
		default:
			/* ignore other relkinds */
			goto out;
	}

	/*
	 * Compute a default security label when we create a new relation object
	 * under the specified namespace.
	 */
	scontext = sepgsql_get_client_label();
	tcontext = sepgsql_get_label(NamespaceRelationId,
								 classForm->relnamespace, 0);
	rcontext = sepgsql_compute_create(scontext, tcontext, tclass,
									  NameStr(classForm->relname));

	/*
	 * check db_xxx:{create} permission
	 */
	nsp_name = get_namespace_name(classForm->relnamespace);
	initStringInfo(&audit_name);
	appendStringInfo(&audit_name, "%s.%s",
					 quote_identifier(nsp_name),
					 quote_identifier(NameStr(classForm->relname)));
	sepgsql_avc_check_perms_label(rcontext,
								  tclass,
								  SEPG_DB_DATABASE__CREATE,
								  audit_name.data,
								  true);

	/*
	 * Assign the default security label on the new relation
	 */
	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = 0;
	SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, rcontext);

	/*
	 * We also assigns a default security label on columns of the new regular
	 * tables.
	 */
	if (classForm->relkind == RELKIND_RELATION)
	{
		Relation	arel;
		ScanKeyData akey;
		SysScanDesc ascan;
		HeapTuple	atup;
		Form_pg_attribute attForm;

		arel = heap_open(AttributeRelationId, AccessShareLock);

		ScanKeyInit(&akey,
					Anum_pg_attribute_attrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relOid));

		ascan = systable_beginscan(arel, AttributeRelidNumIndexId, true,
								   SnapshotSelf, 1, &akey);

		while (HeapTupleIsValid(atup = systable_getnext(ascan)))
		{
			attForm = (Form_pg_attribute) GETSTRUCT(atup);

			resetStringInfo(&audit_name);
			appendStringInfo(&audit_name, "%s.%s.%s",
							 quote_identifier(nsp_name),
							 quote_identifier(NameStr(classForm->relname)),
							 quote_identifier(NameStr(attForm->attname)));

			ccontext = sepgsql_compute_create(scontext,
											  rcontext,
											  SEPG_CLASS_DB_COLUMN,
											  NameStr(attForm->attname));

			/*
			 * check db_column:{create} permission
			 */
			sepgsql_avc_check_perms_label(ccontext,
										  SEPG_CLASS_DB_COLUMN,
										  SEPG_DB_COLUMN__CREATE,
										  audit_name.data,
										  true);

			object.classId = RelationRelationId;
			object.objectId = relOid;
			object.objectSubId = attForm->attnum;
			SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, ccontext);

			pfree(ccontext);
		}
		systable_endscan(ascan);
		heap_close(arel, AccessShareLock);
	}
	pfree(rcontext);

out:
	systable_endscan(sscan);
	heap_close(rel, AccessShareLock);
}

/*
 * sepgsql_relation_drop
 *
 * It checks privileges to drop the supplied relation.
 */
void
sepgsql_relation_drop(Oid relOid)
{
	ObjectAddress object;
	char	   *audit_name;
	uint16_t	tclass;
	char		relkind;

	relkind = get_rel_relkind(relOid);
	switch (relkind)
	{
		case RELKIND_RELATION:
			tclass = SEPG_CLASS_DB_TABLE;
			break;
		case RELKIND_SEQUENCE:
			tclass = SEPG_CLASS_DB_SEQUENCE;
			break;
		case RELKIND_VIEW:
			tclass = SEPG_CLASS_DB_VIEW;
			break;
		case RELKIND_INDEX:
			/* ignore indexes on toast tables */
			if (get_rel_namespace(relOid) == PG_TOAST_NAMESPACE)
				return;
			/* other indexes are handled specially below; no need for tclass */
			break;
		default:
			/* ignore other relkinds */
			return;
	}

	/*
	 * check db_schema:{remove_name} permission
	 */
	object.classId = NamespaceRelationId;
	object.objectId = get_rel_namespace(relOid);
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object);

	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_SCHEMA,
							SEPG_DB_SCHEMA__REMOVE_NAME,
							audit_name,
							true);
	pfree(audit_name);

	/* deal with indexes specially */
	if (relkind == RELKIND_INDEX)
	{
		sepgsql_index_modify(relOid);
		return;
	}

	/*
	 * check db_table/sequence/view:{drop} permission
	 */
	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object);

	sepgsql_avc_check_perms(&object,
							tclass,
							SEPG_DB_TABLE__DROP,
							audit_name,
							true);
	pfree(audit_name);

	/*
	 * check db_column:{drop} permission
	 */
	if (relkind == RELKIND_RELATION)
	{
		Form_pg_attribute attForm;
		CatCList   *attrList;
		HeapTuple	atttup;
		int			i;

		attrList = SearchSysCacheList1(ATTNUM, ObjectIdGetDatum(relOid));
		for (i = 0; i < attrList->n_members; i++)
		{
			atttup = &attrList->members[i]->tuple;
			attForm = (Form_pg_attribute) GETSTRUCT(atttup);

			if (attForm->attisdropped)
				continue;

			object.classId = RelationRelationId;
			object.objectId = relOid;
			object.objectSubId = attForm->attnum;
			audit_name = getObjectIdentity(&object);

			sepgsql_avc_check_perms(&object,
									SEPG_CLASS_DB_COLUMN,
									SEPG_DB_COLUMN__DROP,
									audit_name,
									true);
			pfree(audit_name);
		}
		ReleaseCatCacheList(attrList);
	}
}

/*
 * sepgsql_relation_relabel
 *
 * It checks privileges to relabel the supplied relation by the `seclabel'.
 */
void
sepgsql_relation_relabel(Oid relOid, const char *seclabel)
{
	ObjectAddress object;
	char	   *audit_name;
	char		relkind;
	uint16_t	tclass = 0;

	relkind = get_rel_relkind(relOid);
	if (relkind == RELKIND_RELATION)
		tclass = SEPG_CLASS_DB_TABLE;
	else if (relkind == RELKIND_SEQUENCE)
		tclass = SEPG_CLASS_DB_SEQUENCE;
	else if (relkind == RELKIND_VIEW)
		tclass = SEPG_CLASS_DB_VIEW;
	else
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot set security labels on relations except "
						"for tables, sequences or views")));

	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object);

	/*
	 * check db_xxx:{setattr relabelfrom} permission
	 */
	sepgsql_avc_check_perms(&object,
							tclass,
							SEPG_DB_TABLE__SETATTR |
							SEPG_DB_TABLE__RELABELFROM,
							audit_name,
							true);

	/*
	 * check db_xxx:{relabelto} permission
	 */
	sepgsql_avc_check_perms_label(seclabel,
								  tclass,
								  SEPG_DB_TABLE__RELABELTO,
								  audit_name,
								  true);
	pfree(audit_name);
}

/*
 * sepgsql_relation_setattr
 *
 * It checks privileges to set attribute of the supplied relation
 */
void
sepgsql_relation_setattr(Oid relOid)
{
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	oldtup;
	HeapTuple	newtup;
	Form_pg_class oldform;
	Form_pg_class newform;
	ObjectAddress object;
	char	   *audit_name;
	uint16_t	tclass;

	switch (get_rel_relkind(relOid))
	{
		case RELKIND_RELATION:
			tclass = SEPG_CLASS_DB_TABLE;
			break;
		case RELKIND_SEQUENCE:
			tclass = SEPG_CLASS_DB_SEQUENCE;
			break;
		case RELKIND_VIEW:
			tclass = SEPG_CLASS_DB_VIEW;
			break;
		case RELKIND_INDEX:
			/* deal with indexes specially */
			sepgsql_index_modify(relOid);
			return;
		default:
			/* other relkinds don't need additional work */
			return;
	}

	/*
	 * Fetch newer catalog
	 */
	rel = heap_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relOid));

	sscan = systable_beginscan(rel, ClassOidIndexId, true,
							   SnapshotSelf, 1, &skey);

	newtup = systable_getnext(sscan);
	if (!HeapTupleIsValid(newtup))
		elog(ERROR, "catalog lookup failed for relation %u", relOid);
	newform = (Form_pg_class) GETSTRUCT(newtup);

	/*
	 * Fetch older catalog
	 */
	oldtup = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(oldtup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	oldform = (Form_pg_class) GETSTRUCT(oldtup);

	/*
	 * Does this ALTER command takes operation to namespace?
	 */
	if (newform->relnamespace != oldform->relnamespace)
	{
		sepgsql_schema_remove_name(oldform->relnamespace);
		sepgsql_schema_add_name(newform->relnamespace);
	}
	if (strcmp(NameStr(newform->relname), NameStr(oldform->relname)) != 0)
		sepgsql_schema_rename(oldform->relnamespace);

	/*
	 * XXX - In the future version, db_tuple:{use} of system catalog entry
	 * shall be checked, if tablespace configuration is changed.
	 */

	/*
	 * check db_xxx:{setattr} permission
	 */
	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object);

	sepgsql_avc_check_perms(&object,
							tclass,
							SEPG_DB_TABLE__SETATTR,
							audit_name,
							true);
	pfree(audit_name);

	ReleaseSysCache(oldtup);
	systable_endscan(sscan);
	heap_close(rel, AccessShareLock);
}

/*
 * sepgsql_relation_setattr_extra
 *
 * It checks permission of the relation being referenced by extra attributes,
 * such as pg_index entries. Like core PostgreSQL, sepgsql also does not deal
 * with such entries as individual "objects", thus, modification of these
 * entries shall be considered as setting an attribute of the underlying
 * relation.
 */
static void
sepgsql_relation_setattr_extra(Relation catalog,
							   Oid catindex_id,
							   Oid extra_oid,
							   AttrNumber anum_relation_id,
							   AttrNumber anum_extra_id)
{
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;

	ScanKeyInit(&skey, anum_extra_id,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extra_oid));

	sscan = systable_beginscan(catalog, catindex_id, true,
							   SnapshotSelf, 1, &skey);
	tuple = systable_getnext(sscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "catalog lookup failed for object %u in catalog \"%s\"",
			 extra_oid, RelationGetRelationName(catalog));

	datum = heap_getattr(tuple, anum_relation_id,
						 RelationGetDescr(catalog), &isnull);
	Assert(!isnull);

	sepgsql_relation_setattr(DatumGetObjectId(datum));

	systable_endscan(sscan);
}

/*
 * sepgsql_index_modify
 *		Handle index create, update, drop
 *
 * Unlike other relation kinds, indexes do not have their own security labels,
 * so instead of doing checks directly, treat them as extra attributes of their
 * owning tables; so check 'setattr' permissions on the table.
 */
static void
sepgsql_index_modify(Oid indexOid)
{
	Relation	catalog = heap_open(IndexRelationId, AccessShareLock);

	/* check db_table:{setattr} permission of the table being indexed */
	sepgsql_relation_setattr_extra(catalog,
								   IndexRelidIndexId,
								   indexOid,
								   Anum_pg_index_indrelid,
								   Anum_pg_index_indexrelid);
	heap_close(catalog, AccessShareLock);
}
