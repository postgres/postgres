/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/dml.c
 *
 * Routines to handle DML permission checks
 *
 * Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tupdesc.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/dependency.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits_fn.h"
#include "commands/seclabel.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "nodes/bitmapset.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "sepgsql.h"

/*
 * fixup_whole_row_references
 *
 * When user reference a whole of row, it is equivalent to reference to
 * all the user columns (not system columns). So, we need to fix up the
 * given bitmapset, if it contains a whole of the row reference.
 */
static Bitmapset *
fixup_whole_row_references(Oid relOid, Bitmapset *columns)
{
	Bitmapset  *result;
	HeapTuple	tuple;
	AttrNumber	natts;
	AttrNumber	attno;
	int			index;

	/* if no whole of row references, do not anything */
	index = InvalidAttrNumber - FirstLowInvalidHeapAttributeNumber;
	if (!bms_is_member(index, columns))
		return columns;

	/* obtain number of attributes */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	natts = ((Form_pg_class) GETSTRUCT(tuple))->relnatts;
	ReleaseSysCache(tuple);

	/* fix up the given columns */
	result = bms_copy(columns);
	result = bms_del_member(result, index);

	for (attno = 1; attno <= natts; attno++)
	{
		tuple = SearchSysCache2(ATTNUM,
								ObjectIdGetDatum(relOid),
								Int16GetDatum(attno));
		if (!HeapTupleIsValid(tuple))
			continue;

		if (((Form_pg_attribute) GETSTRUCT(tuple))->attisdropped)
			continue;

		index = attno - FirstLowInvalidHeapAttributeNumber;

		result = bms_add_member(result, index);

		ReleaseSysCache(tuple);
	}
	return result;
}

/*
 * fixup_inherited_columns
 *
 * When user is querying on a table with children, it implicitly accesses
 * child tables also. So, we also need to check security label of child
 * tables and columns, but here is no guarantee attribute numbers are
 * same between the parent ans children.
 * It returns a bitmapset which contains attribute number of the child
 * table based on the given bitmapset of the parent.
 */
static Bitmapset *
fixup_inherited_columns(Oid parentId, Oid childId, Bitmapset *columns)
{
	Bitmapset  *result = NULL;
	int			index;

	/*
	 * obviously, no need to do anything here
	 */
	if (parentId == childId)
		return columns;

	index = -1;
	while ((index = bms_next_member(columns, index)) >= 0)
	{
		/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
		AttrNumber	attno = index + FirstLowInvalidHeapAttributeNumber;
		char	   *attname;

		/*
		 * whole-row-reference shall be fixed-up later
		 */
		if (attno == InvalidAttrNumber)
		{
			result = bms_add_member(result, index);
			continue;
		}

		attname = get_attname(parentId, attno);
		if (!attname)
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 attno, parentId);
		attno = get_attnum(childId, attname);
		if (attno == InvalidAttrNumber)
			elog(ERROR, "cache lookup failed for attribute %s of relation %u",
				 attname, childId);

		result = bms_add_member(result,
								attno - FirstLowInvalidHeapAttributeNumber);

		pfree(attname);
	}

	return result;
}

/*
 * check_relation_privileges
 *
 * It actually checks required permissions on a certain relation
 * and its columns.
 */
static bool
check_relation_privileges(Oid relOid,
						  Bitmapset *selected,
						  Bitmapset *inserted,
						  Bitmapset *updated,
						  uint32 required,
						  bool abort_on_violation)
{
	ObjectAddress object;
	char	   *audit_name;
	Bitmapset  *columns;
	int			index;
	char		relkind = get_rel_relkind(relOid);
	bool		result = true;

	/*
	 * Hardwired Policies: SE-PostgreSQL enforces - clients cannot modify
	 * system catalogs using DMLs - clients cannot reference/modify toast
	 * relations using DMLs
	 */
	if (sepgsql_getenforce() > 0)
	{
		Oid			relnamespace = get_rel_namespace(relOid);

		if (IsSystemNamespace(relnamespace) &&
			(required & (SEPG_DB_TABLE__UPDATE |
						 SEPG_DB_TABLE__INSERT |
						 SEPG_DB_TABLE__DELETE)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("SELinux: hardwired security policy violation")));

		if (relkind == RELKIND_TOASTVALUE)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("SELinux: hardwired security policy violation")));
	}

	/*
	 * Check permissions on the relation
	 */
	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object);
	switch (relkind)
	{
		case RELKIND_RELATION:
			result = sepgsql_avc_check_perms(&object,
											 SEPG_CLASS_DB_TABLE,
											 required,
											 audit_name,
											 abort_on_violation);
			break;

		case RELKIND_SEQUENCE:
			Assert((required & ~SEPG_DB_TABLE__SELECT) == 0);

			if (required & SEPG_DB_TABLE__SELECT)
				result = sepgsql_avc_check_perms(&object,
												 SEPG_CLASS_DB_SEQUENCE,
												 SEPG_DB_SEQUENCE__GET_VALUE,
												 audit_name,
												 abort_on_violation);
			break;

		case RELKIND_VIEW:
			result = sepgsql_avc_check_perms(&object,
											 SEPG_CLASS_DB_VIEW,
											 SEPG_DB_VIEW__EXPAND,
											 audit_name,
											 abort_on_violation);
			break;

		default:
			/* nothing to be checked */
			break;
	}
	pfree(audit_name);

	/*
	 * Only columns owned by relations shall be checked
	 */
	if (relkind != RELKIND_RELATION)
		return true;

	/*
	 * Check permissions on the columns
	 */
	selected = fixup_whole_row_references(relOid, selected);
	inserted = fixup_whole_row_references(relOid, inserted);
	updated = fixup_whole_row_references(relOid, updated);
	columns = bms_union(selected, bms_union(inserted, updated));

	while ((index = bms_first_member(columns)) >= 0)
	{
		AttrNumber	attnum;
		uint32		column_perms = 0;

		if (bms_is_member(index, selected))
			column_perms |= SEPG_DB_COLUMN__SELECT;
		if (bms_is_member(index, inserted))
		{
			if (required & SEPG_DB_TABLE__INSERT)
				column_perms |= SEPG_DB_COLUMN__INSERT;
		}
		if (bms_is_member(index, updated))
		{
			if (required & SEPG_DB_TABLE__UPDATE)
				column_perms |= SEPG_DB_COLUMN__UPDATE;
		}
		if (column_perms == 0)
			continue;

		/* obtain column's permission */
		attnum = index + FirstLowInvalidHeapAttributeNumber;

		object.classId = RelationRelationId;
		object.objectId = relOid;
		object.objectSubId = attnum;
		audit_name = getObjectDescription(&object);

		result = sepgsql_avc_check_perms(&object,
										 SEPG_CLASS_DB_COLUMN,
										 column_perms,
										 audit_name,
										 abort_on_violation);
		pfree(audit_name);

		if (!result)
			return result;
	}
	return true;
}

/*
 * sepgsql_dml_privileges
 *
 * Entrypoint of the DML permission checks
 */
bool
sepgsql_dml_privileges(List *rangeTabls, bool abort_on_violation)
{
	ListCell   *lr;

	foreach(lr, rangeTabls)
	{
		RangeTblEntry *rte = lfirst(lr);
		uint32		required = 0;
		List	   *tableIds;
		ListCell   *li;

		/*
		 * Only regular relations shall be checked
		 */
		if (rte->rtekind != RTE_RELATION)
			continue;

		/*
		 * Find out required permissions
		 */
		if (rte->requiredPerms & ACL_SELECT)
			required |= SEPG_DB_TABLE__SELECT;
		if (rte->requiredPerms & ACL_INSERT)
			required |= SEPG_DB_TABLE__INSERT;
		if (rte->requiredPerms & ACL_UPDATE)
		{
			if (!bms_is_empty(rte->updatedCols))
				required |= SEPG_DB_TABLE__UPDATE;
			else
				required |= SEPG_DB_TABLE__LOCK;
		}
		if (rte->requiredPerms & ACL_DELETE)
			required |= SEPG_DB_TABLE__DELETE;

		/*
		 * Skip, if nothing to be checked
		 */
		if (required == 0)
			continue;

		/*
		 * If this RangeTblEntry is also supposed to reference inherited
		 * tables, we need to check security label of the child tables. So, we
		 * expand rte->relid into list of OIDs of inheritance hierarchy, then
		 * checker routine will be invoked for each relations.
		 */
		if (!rte->inh)
			tableIds = list_make1_oid(rte->relid);
		else
			tableIds = find_all_inheritors(rte->relid, NoLock, NULL);

		foreach(li, tableIds)
		{
			Oid			tableOid = lfirst_oid(li);
			Bitmapset  *selectedCols;
			Bitmapset  *insertedCols;
			Bitmapset  *updatedCols;

			/*
			 * child table has different attribute numbers, so we need to fix
			 * up them.
			 */
			selectedCols = fixup_inherited_columns(rte->relid, tableOid,
												   rte->selectedCols);
			insertedCols = fixup_inherited_columns(rte->relid, tableOid,
												   rte->insertedCols);
			updatedCols = fixup_inherited_columns(rte->relid, tableOid,
												  rte->updatedCols);

			/*
			 * check permissions on individual tables
			 */
			if (!check_relation_privileges(tableOid,
										   selectedCols,
										   insertedCols,
										   updatedCols,
										   required, abort_on_violation))
				return false;
		}
		list_free(tableIds);
	}
	return true;
}
