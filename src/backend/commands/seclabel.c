/* -------------------------------------------------------------------------
 *
 * seclabel.c
 *	  routines to support security label feature.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_shseclabel.h"
#include "commands/seclabel.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"

typedef struct
{
	const char *provider_name;
	check_object_relabel_type hook;
} LabelProvider;

static List *label_provider_list = NIL;

/*
 * ExecSecLabelStmt --
 *
 * Apply a security label to a database object.
 */
Oid
ExecSecLabelStmt(SecLabelStmt *stmt)
{
	LabelProvider *provider = NULL;
	ObjectAddress address;
	Relation	relation;
	ListCell   *lc;

	/*
	 * Find the named label provider, or if none specified, check whether
	 * there's exactly one, and if so use it.
	 */
	if (stmt->provider == NULL)
	{
		if (label_provider_list == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("no security label providers have been loaded")));
		if (lnext(list_head(label_provider_list)) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("must specify provider when multiple security label providers have been loaded")));
		provider = (LabelProvider *) linitial(label_provider_list);
	}
	else
	{
		foreach(lc, label_provider_list)
		{
			LabelProvider *lp = lfirst(lc);

			if (strcmp(stmt->provider, lp->provider_name) == 0)
			{
				provider = lp;
				break;
			}
		}
		if (provider == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("security label provider \"%s\" is not loaded",
							stmt->provider)));
	}

	/*
	 * Translate the parser representation which identifies this object into
	 * an ObjectAddress. get_object_address() will throw an error if the
	 * object does not exist, and will also acquire a lock on the target to
	 * guard against concurrent modifications.
	 */
	address = get_object_address(stmt->objtype, stmt->objname, stmt->objargs,
								 &relation, ShareUpdateExclusiveLock, false);

	/* Require ownership of the target object. */
	check_object_ownership(GetUserId(), stmt->objtype, address,
						   stmt->objname, stmt->objargs, relation);

	/* Perform other integrity checks as needed. */
	switch (stmt->objtype)
	{
		case OBJECT_COLUMN:

			/*
			 * Allow security labels only on columns of tables, views,
			 * materialized views, composite types, and foreign tables (which
			 * are the only relkinds for which pg_dump will dump labels).
			 */
			if (relation->rd_rel->relkind != RELKIND_RELATION &&
				relation->rd_rel->relkind != RELKIND_VIEW &&
				relation->rd_rel->relkind != RELKIND_MATVIEW &&
				relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
				relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a table, view, materialized view, composite type, or foreign table",
								RelationGetRelationName(relation))));
			break;
		default:
			break;
	}

	/* Provider gets control here, may throw ERROR to veto new label. */
	(*provider->hook) (&address, stmt->label);

	/* Apply new label. */
	SetSecurityLabel(&address, provider->provider_name, stmt->label);

	/*
	 * If get_object_address() opened the relation for us, we close it to keep
	 * the reference count correct - but we retain any locks acquired by
	 * get_object_address() until commit time, to guard against concurrent
	 * activity.
	 */
	if (relation != NULL)
		relation_close(relation, NoLock);

	return address.objectId;
}

/*
 * GetSharedSecurityLabel returns the security label for a shared object for
 * a given provider, or NULL if there is no such label.
 */
static char *
GetSharedSecurityLabel(const ObjectAddress *object, const char *provider)
{
	Relation	pg_shseclabel;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	char	   *seclabel = NULL;

	ScanKeyInit(&keys[0],
				Anum_pg_shseclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_shseclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_shseclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_shseclabel = heap_open(SharedSecLabelRelationId, AccessShareLock);

	scan = systable_beginscan(pg_shseclabel, SharedSecLabelObjectIndexId, true,
							  SnapshotNow, 3, keys);

	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		datum = heap_getattr(tuple, Anum_pg_shseclabel_label,
							 RelationGetDescr(pg_shseclabel), &isnull);
		if (!isnull)
			seclabel = TextDatumGetCString(datum);
	}
	systable_endscan(scan);

	heap_close(pg_shseclabel, AccessShareLock);

	return seclabel;
}

/*
 * GetSecurityLabel returns the security label for a shared or database object
 * for a given provider, or NULL if there is no such label.
 */
char *
GetSecurityLabel(const ObjectAddress *object, const char *provider)
{
	Relation	pg_seclabel;
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	char	   *seclabel = NULL;

	/* Shared objects have their own security label catalog. */
	if (IsSharedRelation(object->classId))
		return GetSharedSecurityLabel(object, provider);

	/* Must be an unshared object, so examine pg_seclabel. */
	ScanKeyInit(&keys[0],
				Anum_pg_seclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_seclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_seclabel_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(object->objectSubId));
	ScanKeyInit(&keys[3],
				Anum_pg_seclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_seclabel = heap_open(SecLabelRelationId, AccessShareLock);

	scan = systable_beginscan(pg_seclabel, SecLabelObjectIndexId, true,
							  SnapshotNow, 4, keys);

	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		datum = heap_getattr(tuple, Anum_pg_seclabel_label,
							 RelationGetDescr(pg_seclabel), &isnull);
		if (!isnull)
			seclabel = TextDatumGetCString(datum);
	}
	systable_endscan(scan);

	heap_close(pg_seclabel, AccessShareLock);

	return seclabel;
}

/*
 * SetSharedSecurityLabel is a helper function of SetSecurityLabel to
 * handle shared database objects.
 */
static void
SetSharedSecurityLabel(const ObjectAddress *object,
					   const char *provider, const char *label)
{
	Relation	pg_shseclabel;
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	oldtup;
	HeapTuple	newtup = NULL;
	Datum		values[Natts_pg_shseclabel];
	bool		nulls[Natts_pg_shseclabel];
	bool		replaces[Natts_pg_shseclabel];

	/* Prepare to form or update a tuple, if necessary. */
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));
	values[Anum_pg_shseclabel_objoid - 1] = ObjectIdGetDatum(object->objectId);
	values[Anum_pg_shseclabel_classoid - 1] = ObjectIdGetDatum(object->classId);
	values[Anum_pg_shseclabel_provider - 1] = CStringGetTextDatum(provider);
	if (label != NULL)
		values[Anum_pg_shseclabel_label - 1] = CStringGetTextDatum(label);

	/* Use the index to search for a matching old tuple */
	ScanKeyInit(&keys[0],
				Anum_pg_shseclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_shseclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_shseclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_shseclabel = heap_open(SharedSecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_shseclabel, SharedSecLabelObjectIndexId, true,
							  SnapshotNow, 3, keys);

	oldtup = systable_getnext(scan);
	if (HeapTupleIsValid(oldtup))
	{
		if (label == NULL)
			simple_heap_delete(pg_shseclabel, &oldtup->t_self);
		else
		{
			replaces[Anum_pg_shseclabel_label - 1] = true;
			newtup = heap_modify_tuple(oldtup, RelationGetDescr(pg_shseclabel),
									   values, nulls, replaces);
			simple_heap_update(pg_shseclabel, &oldtup->t_self, newtup);
		}
	}
	systable_endscan(scan);

	/* If we didn't find an old tuple, insert a new one */
	if (newtup == NULL && label != NULL)
	{
		newtup = heap_form_tuple(RelationGetDescr(pg_shseclabel),
								 values, nulls);
		simple_heap_insert(pg_shseclabel, newtup);
	}

	/* Update indexes, if necessary */
	if (newtup != NULL)
	{
		CatalogUpdateIndexes(pg_shseclabel, newtup);
		heap_freetuple(newtup);
	}

	heap_close(pg_shseclabel, RowExclusiveLock);
}

/*
 * SetSecurityLabel attempts to set the security label for the specified
 * provider on the specified object to the given value.  NULL means that any
 * any existing label should be deleted.
 */
void
SetSecurityLabel(const ObjectAddress *object,
				 const char *provider, const char *label)
{
	Relation	pg_seclabel;
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	oldtup;
	HeapTuple	newtup = NULL;
	Datum		values[Natts_pg_seclabel];
	bool		nulls[Natts_pg_seclabel];
	bool		replaces[Natts_pg_seclabel];

	/* Shared objects have their own security label catalog. */
	if (IsSharedRelation(object->classId))
	{
		SetSharedSecurityLabel(object, provider, label);
		return;
	}

	/* Prepare to form or update a tuple, if necessary. */
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));
	values[Anum_pg_seclabel_objoid - 1] = ObjectIdGetDatum(object->objectId);
	values[Anum_pg_seclabel_classoid - 1] = ObjectIdGetDatum(object->classId);
	values[Anum_pg_seclabel_objsubid - 1] = Int32GetDatum(object->objectSubId);
	values[Anum_pg_seclabel_provider - 1] = CStringGetTextDatum(provider);
	if (label != NULL)
		values[Anum_pg_seclabel_label - 1] = CStringGetTextDatum(label);

	/* Use the index to search for a matching old tuple */
	ScanKeyInit(&keys[0],
				Anum_pg_seclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_seclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_seclabel_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(object->objectSubId));
	ScanKeyInit(&keys[3],
				Anum_pg_seclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_seclabel = heap_open(SecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_seclabel, SecLabelObjectIndexId, true,
							  SnapshotNow, 4, keys);

	oldtup = systable_getnext(scan);
	if (HeapTupleIsValid(oldtup))
	{
		if (label == NULL)
			simple_heap_delete(pg_seclabel, &oldtup->t_self);
		else
		{
			replaces[Anum_pg_seclabel_label - 1] = true;
			newtup = heap_modify_tuple(oldtup, RelationGetDescr(pg_seclabel),
									   values, nulls, replaces);
			simple_heap_update(pg_seclabel, &oldtup->t_self, newtup);
		}
	}
	systable_endscan(scan);

	/* If we didn't find an old tuple, insert a new one */
	if (newtup == NULL && label != NULL)
	{
		newtup = heap_form_tuple(RelationGetDescr(pg_seclabel),
								 values, nulls);
		simple_heap_insert(pg_seclabel, newtup);
	}

	/* Update indexes, if necessary */
	if (newtup != NULL)
	{
		CatalogUpdateIndexes(pg_seclabel, newtup);
		heap_freetuple(newtup);
	}

	heap_close(pg_seclabel, RowExclusiveLock);
}

/*
 * DeleteSharedSecurityLabel is a helper function of DeleteSecurityLabel
 * to handle shared database objects.
 */
void
DeleteSharedSecurityLabel(Oid objectId, Oid classId)
{
	Relation	pg_shseclabel;
	ScanKeyData skey[2];
	SysScanDesc scan;
	HeapTuple	oldtup;

	ScanKeyInit(&skey[0],
				Anum_pg_shseclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));
	ScanKeyInit(&skey[1],
				Anum_pg_shseclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));

	pg_shseclabel = heap_open(SharedSecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_shseclabel, SharedSecLabelObjectIndexId, true,
							  SnapshotNow, 2, skey);
	while (HeapTupleIsValid(oldtup = systable_getnext(scan)))
		simple_heap_delete(pg_shseclabel, &oldtup->t_self);
	systable_endscan(scan);

	heap_close(pg_shseclabel, RowExclusiveLock);
}

/*
 * DeleteSecurityLabel removes all security labels for an object (and any
 * sub-objects, if applicable).
 */
void
DeleteSecurityLabel(const ObjectAddress *object)
{
	Relation	pg_seclabel;
	ScanKeyData skey[3];
	SysScanDesc scan;
	HeapTuple	oldtup;
	int			nkeys;

	/* Shared objects have their own security label catalog. */
	if (IsSharedRelation(object->classId))
	{
		Assert(object->objectSubId == 0);
		DeleteSharedSecurityLabel(object->objectId, object->classId);
		return;
	}

	ScanKeyInit(&skey[0],
				Anum_pg_seclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&skey[1],
				Anum_pg_seclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	if (object->objectSubId != 0)
	{
		ScanKeyInit(&skey[2],
					Anum_pg_seclabel_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	pg_seclabel = heap_open(SecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_seclabel, SecLabelObjectIndexId, true,
							  SnapshotNow, nkeys, skey);
	while (HeapTupleIsValid(oldtup = systable_getnext(scan)))
		simple_heap_delete(pg_seclabel, &oldtup->t_self);
	systable_endscan(scan);

	heap_close(pg_seclabel, RowExclusiveLock);
}

void
register_label_provider(const char *provider_name, check_object_relabel_type hook)
{
	LabelProvider *provider;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	provider = palloc(sizeof(LabelProvider));
	provider->provider_name = pstrdup(provider_name);
	provider->hook = hook;
	label_provider_list = lappend(label_provider_list, provider);
	MemoryContextSwitchTo(oldcxt);
}
