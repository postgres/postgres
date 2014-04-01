/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/relation.c
 *
 * Routines corresponding to relation/attribute objects
 *
 * Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/indexing.h"
#include "catalog/dependency.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "commands/seclabel.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/tqual.h"

#include "sepgsql.h"

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
	char	   *scontext = sepgsql_get_client_label();
	char	   *tcontext;
	char	   *ncontext;
	ObjectAddress object;

	/*
	 * Only attributes within regular relation have individual security
	 * labels.
	 */
	if (get_rel_relkind(relOid) != RELKIND_RELATION)
		return;

	/*
	 * Compute a default security label when we create a new procedure object
	 * under the specified namespace.
	 */
	scontext = sepgsql_get_client_label();
	tcontext = sepgsql_get_label(RelationRelationId, relOid, 0);
	ncontext = sepgsql_compute_create(scontext, tcontext,
									  SEPG_CLASS_DB_COLUMN);

	/*
	 * Assign the default security label on a new procedure
	 */
	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = attnum;
	SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, ncontext);

	pfree(tcontext);
	pfree(ncontext);
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
	char	   *scontext = sepgsql_get_client_label();
	char	   *tcontext;
	char	   *audit_name;
	ObjectAddress object;

	if (get_rel_relkind(relOid) != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot set security label on non-regular columns")));

	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = attnum;
	audit_name = getObjectDescription(&object);

	/*
	 * check db_column:{setattr relabelfrom} permission
	 */
	tcontext = sepgsql_get_label(RelationRelationId, relOid, attnum);
	sepgsql_check_perms(scontext,
						tcontext,
						SEPG_CLASS_DB_COLUMN,
						SEPG_DB_COLUMN__SETATTR |
						SEPG_DB_COLUMN__RELABELFROM,
						audit_name,
						true);

	/*
	 * check db_column:{relabelto} permission
	 */
	sepgsql_check_perms(scontext,
						seclabel,
						SEPG_CLASS_DB_COLUMN,
						SEPG_DB_PROCEDURE__RELABELTO,
						audit_name,
						true);

	pfree(tcontext);
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

	if (classForm->relkind == RELKIND_RELATION)
		tclass = SEPG_CLASS_DB_TABLE;
	else if (classForm->relkind == RELKIND_SEQUENCE)
		tclass = SEPG_CLASS_DB_SEQUENCE;
	else if (classForm->relkind == RELKIND_VIEW)
		tclass = SEPG_CLASS_DB_VIEW;
	else
		goto out;				/* No need to assign individual labels */

	/*
	 * Compute a default security label when we create a new relation object
	 * under the specified namespace.
	 */
	scontext = sepgsql_get_client_label();
	tcontext = sepgsql_get_label(NamespaceRelationId,
								 classForm->relnamespace, 0);
	rcontext = sepgsql_compute_create(scontext, tcontext, tclass);

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
		AttrNumber	index;

		ccontext = sepgsql_compute_create(scontext, rcontext,
										  SEPG_CLASS_DB_COLUMN);
		for (index = FirstLowInvalidHeapAttributeNumber + 1;
			 index <= classForm->relnatts;
			 index++)
		{
			if (index == InvalidAttrNumber)
				continue;

			if (index == ObjectIdAttributeNumber && !classForm->relhasoids)
				continue;

			object.classId = RelationRelationId;
			object.objectId = relOid;
			object.objectSubId = index;
			SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, ccontext);
		}
		pfree(ccontext);
	}
	pfree(rcontext);
out:
	systable_endscan(sscan);
	heap_close(rel, AccessShareLock);
}

/*
 * sepgsql_relation_relabel
 *
 * It checks privileges to relabel the supplied relation by the `seclabel'.
 */
void
sepgsql_relation_relabel(Oid relOid, const char *seclabel)
{
	char	   *scontext = sepgsql_get_client_label();
	char	   *tcontext;
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

	audit_name = getObjectDescriptionOids(RelationRelationId, relOid);

	/*
	 * check db_xxx:{setattr relabelfrom} permission
	 */
	tcontext = sepgsql_get_label(RelationRelationId, relOid, 0);

	sepgsql_check_perms(scontext,
						tcontext,
						tclass,
						SEPG_DB_TABLE__SETATTR |
						SEPG_DB_TABLE__RELABELFROM,
						audit_name,
						true);

	/*
	 * check db_xxx:{relabelto} permission
	 */
	sepgsql_check_perms(scontext,
						seclabel,
						tclass,
						SEPG_DB_TABLE__RELABELTO,
						audit_name,
						true);

	pfree(tcontext);
	pfree(audit_name);
}
