/*-------------------------------------------------------------------------
 *
 * tablecmds.c
 *	  Commands for altering table structures and settings
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/tablecmds.c,v 1.2 2002/04/15 23:45:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/tuptoaster.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "parser/parse.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteSupport.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/relcache.h"


static void drop_default(Oid relid, int16 attnum);
static bool needs_toast_table(Relation rel);
static void CheckTupleType(Form_pg_class tuple_class);

static List *MergeAttributes(List *schema, List *supers, bool istemp,
				List **supOids, List **supconstr, bool *supHasOids);
static bool change_varattnos_of_a_node(Node *node, const AttrNumber *newattno);
static void StoreCatalogInheritance(Oid relationId, List *supers);
static int	findAttrByName(const char *attributeName, List *schema);
static void setRelhassubclassInRelation(Oid relationId, bool relhassubclass);
static List *MergeDomainAttributes(List *schema);

/* Used by attribute and relation renaming routines: */

#define RI_TRIGGER_PK	1		/* is a trigger on the PK relation */
#define RI_TRIGGER_FK	2		/* is a trigger on the FK relation */
#define RI_TRIGGER_NONE 0		/* is not an RI trigger function */

static int	ri_trigger_type(Oid tgfoid);
static void update_ri_trigger_args(Oid relid,
					   const char *oldname,
					   const char *newname,
					   bool fk_scan,
					   bool update_relname);


/* ----------------
 *		AlterTableAddColumn
 *		(formerly known as PerformAddAttribute)
 *
 *		adds an additional attribute to a relation
 *
 *		Adds attribute field(s) to a relation.	Each new attribute
 *		is given attnums in sequential order and is added to the
 *		ATTRIBUTE relation.  If the AMI fails, defunct tuples will
 *		remain in the ATTRIBUTE relation for later vacuuming.
 *		Later, there may be some reserved attribute names???
 *
 *		(If needed, can instead use elog to handle exceptions.)
 *
 *		Note:
 *				Initial idea of ordering the tuple attributes so that all
 *		the variable length domains occured last was scratched.  Doing
 *		so would not speed access too much (in general) and would create
 *		many complications in formtuple, heap_getattr, and addattribute.
 *
 *		scan attribute catalog for name conflict (within rel)
 *		scan type catalog for absence of data type (if not arg)
 *		create attnum magically???
 *		create attribute tuple
 *		insert attribute in attribute catalog
 *		modify reldesc
 *		create new relation tuple
 *		insert new relation in relation catalog
 *		delete original relation from relation catalog
 * ----------------
 */
void
AlterTableAddColumn(Oid myrelid,
					bool inherits,
					ColumnDef *colDef)
{
	Relation	rel,
				pgclass,
				attrdesc;
	HeapTuple	reltup;
	HeapTuple	newreltup;
	HeapTuple	attributeTuple;
	Form_pg_attribute attribute;
	FormData_pg_attribute attributeD;
	int			i;
	int			minattnum,
				maxatts;
	HeapTuple	typeTuple;
	Form_pg_type tform;
	int			attndims;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	rel = heap_open(myrelid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 RelationGetRelationName(rel));

	/*
	 * permissions checking.  this would normally be done in utility.c,
	 * but this particular routine is recursive.
	 *
	 * normally, only the owner of a class can change its schema.
	 */
	if (!allowSystemTableMods
		&& IsSystemRelation(rel))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 RelationGetRelationName(rel));
	if (!pg_class_ownercheck(myrelid, GetUserId()))
		elog(ERROR, "ALTER TABLE: \"%s\": permission denied",
			 RelationGetRelationName(rel));

	/*
	 * Recurse to add the column to child classes, if requested.
	 *
	 * any permissions or problems with duplicate attributes will cause the
	 * whole transaction to abort, which is what we want -- all or
	 * nothing.
	 */
	if (inherits)
	{
		List	   *child,
				   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(myrelid);

		/*
		 * find_all_inheritors does the recursive search of the
		 * inheritance hierarchy, so all we have to do is process all of
		 * the relids in the list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirsti(child);

			if (childrelid == myrelid)
				continue;

			AlterTableAddColumn(childrelid, false, colDef);
		}
	}

	/*
	 * OK, get on with it...
	 *
	 * Implementation restrictions: because we don't touch the table rows,
	 * the new column values will initially appear to be NULLs.  (This
	 * happens because the heap tuple access routines always check for
	 * attnum > # of attributes in tuple, and return NULL if so.)
	 * Therefore we can't support a DEFAULT value in SQL92-compliant
	 * fashion, and we also can't allow a NOT NULL constraint.
	 *
	 * We do allow CHECK constraints, even though these theoretically could
	 * fail for NULL rows (eg, CHECK (newcol IS NOT NULL)).
	 */
	if (colDef->raw_default || colDef->cooked_default)
		elog(ERROR, "Adding columns with defaults is not implemented."
			 "\n\tAdd the column, then use ALTER TABLE SET DEFAULT.");

	if (colDef->is_not_null)
		elog(ERROR, "Adding NOT NULL columns is not implemented."
			 "\n\tAdd the column, then use ALTER TABLE ... SET NOT NULL.");

	pgclass = heap_openr(RelationRelationName, RowExclusiveLock);

	reltup = SearchSysCache(RELOID,
							ObjectIdGetDatum(myrelid),
							0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "ALTER TABLE: relation \"%s\" not found",
			 RelationGetRelationName(rel));

	if (SearchSysCacheExists(ATTNAME,
							 ObjectIdGetDatum(myrelid),
							 PointerGetDatum(colDef->colname),
							 0, 0))
		elog(ERROR, "ALTER TABLE: column name \"%s\" already exists in table \"%s\"",
			 colDef->colname, RelationGetRelationName(rel));

	minattnum = ((Form_pg_class) GETSTRUCT(reltup))->relnatts;
	maxatts = minattnum + 1;
	if (maxatts > MaxHeapAttributeNumber)
		elog(ERROR, "ALTER TABLE: relations limited to %d columns",
			 MaxHeapAttributeNumber);
	i = minattnum + 1;

	attrdesc = heap_openr(AttributeRelationName, RowExclusiveLock);

	if (colDef->typename->arrayBounds)
		attndims = length(colDef->typename->arrayBounds);
	else
		attndims = 0;

	typeTuple = typenameType(colDef->typename);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);

	attributeTuple = heap_addheader(Natts_pg_attribute,
									ATTRIBUTE_TUPLE_SIZE,
									(void *) &attributeD);

	attribute = (Form_pg_attribute) GETSTRUCT(attributeTuple);

	attribute->attrelid = myrelid;
	namestrcpy(&(attribute->attname), colDef->colname);
	attribute->atttypid = typeTuple->t_data->t_oid;
	attribute->attstattarget = DEFAULT_ATTSTATTARGET;
	attribute->attlen = tform->typlen;
	attribute->attcacheoff = -1;
	attribute->atttypmod = colDef->typename->typmod;
	attribute->attnum = i;
	attribute->attbyval = tform->typbyval;
	attribute->attndims = attndims;
	attribute->attisset = (bool) (tform->typtype == 'c');
	attribute->attstorage = tform->typstorage;
	attribute->attalign = tform->typalign;
	attribute->attnotnull = colDef->is_not_null;
	attribute->atthasdef = (colDef->raw_default != NULL ||
							colDef->cooked_default != NULL);

	ReleaseSysCache(typeTuple);

	heap_insert(attrdesc, attributeTuple);

	/* Update indexes on pg_attribute */
	if (RelationGetForm(attrdesc)->relhasindex)
	{
		Relation	idescs[Num_pg_attr_indices];

		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_attr_indices, attrdesc, attributeTuple);
		CatalogCloseIndices(Num_pg_attr_indices, idescs);
	}

	heap_close(attrdesc, RowExclusiveLock);

	/*
	 * Update number of attributes in pg_class tuple
	 */
	newreltup = heap_copytuple(reltup);

	((Form_pg_class) GETSTRUCT(newreltup))->relnatts = maxatts;
	simple_heap_update(pgclass, &newreltup->t_self, newreltup);

	/* keep catalog indices current */
	if (RelationGetForm(pgclass)->relhasindex)
	{
		Relation	ridescs[Num_pg_class_indices];

		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
		CatalogIndexInsert(ridescs, Num_pg_class_indices, pgclass, newreltup);
		CatalogCloseIndices(Num_pg_class_indices, ridescs);
	}

	heap_freetuple(newreltup);
	ReleaseSysCache(reltup);

	heap_close(pgclass, NoLock);

	heap_close(rel, NoLock);	/* close rel but keep lock! */

	/*
	 * Make our catalog updates visible for subsequent steps.
	 */
	CommandCounterIncrement();

	/*
	 * Add any CHECK constraints attached to the new column.
	 *
	 * To do this we must re-open the rel so that its new attr list gets
	 * loaded into the relcache.
	 */
	if (colDef->constraints != NIL)
	{
		rel = heap_open(myrelid, AccessExclusiveLock);
		AddRelationRawConstraints(rel, NIL, colDef->constraints);
		heap_close(rel, NoLock);
	}

	/*
	 * Automatically create the secondary relation for TOAST if it
	 * formerly had no such but now has toastable attributes.
	 */
	AlterTableCreateToastTable(myrelid, true);
}

/*
 * ALTER TABLE ALTER COLUMN DROP NOT NULL
 */
void
AlterTableAlterColumnDropNotNull(Oid myrelid,
								 bool inh, const char *colName)
{
	Relation	rel;
	HeapTuple	tuple;
	AttrNumber	attnum;
	Relation	attr_rel;
	List	   	*indexoidlist;
	List	   	*indexoidscan;

	rel = heap_open(myrelid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 RelationGetRelationName(rel));

	if (!allowSystemTableMods
		&& IsSystemRelation(rel))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 RelationGetRelationName(rel));

	if (!pg_class_ownercheck(myrelid, GetUserId()))
		elog(ERROR, "ALTER TABLE: \"%s\": permission denied",
			 RelationGetRelationName(rel));

	/*
	 * Propagate to children if desired
	 */
	if (inh)
	{
		List	   *child,
				   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(myrelid);

		/*
		 * find_all_inheritors does the recursive search of the
		 * inheritance hierarchy, so all we have to do is process all of
		 * the relids in the list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirsti(child);

			if (childrelid == myrelid)
				continue;
			AlterTableAlterColumnDropNotNull(childrelid,
											 false, colName);
		}
	}

	/* -= now do the thing on this relation =- */

	/*
	 * get the number of the attribute
	 */
	tuple = SearchSysCache(ATTNAME,
						   ObjectIdGetDatum(myrelid),
						   PointerGetDatum(colName),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ALTER TABLE: relation \"%s\" has no column \"%s\"",
			 RelationGetRelationName(rel), colName);

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;
	ReleaseSysCache(tuple);

	/* Prevent them from altering a system attribute */
	if (attnum < 0)
		elog(ERROR, "ALTER TABLE: Cannot alter system attribute \"%s\"",
			 colName);

	/*
	 * Check that the attribute is not in a primary key
	 */

	/* Loop over all indices on the relation */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid		indexoid = lfirsti(indexoidscan);
		HeapTuple	indexTuple;
		Form_pg_index 	indexStruct;
		int		i;

		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexoid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "ALTER TABLE: Index %u not found",
				 indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/* If the index is not a primary key, skip the check */
		if (indexStruct->indisprimary)
		{
			/*
			 * Loop over each attribute in the primary key and
			 * see if it matches the to-be-altered attribute
			 */
			for (i = 0; i < INDEX_MAX_KEYS &&
					 indexStruct->indkey[i] != InvalidAttrNumber; i++)
			{
				if (indexStruct->indkey[i] == attnum)
					elog(ERROR, "ALTER TABLE: Attribute \"%s\" is in a primary key", colName);
			}
		}

		ReleaseSysCache(indexTuple);
	}

	freeList(indexoidlist);

	/*
	 * Okay, actually perform the catalog change
	 */
	attr_rel = heap_openr(AttributeRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(ATTNAME,
							   ObjectIdGetDatum(myrelid),
							   PointerGetDatum(colName),
							   0, 0);
	if (!HeapTupleIsValid(tuple)) /* shouldn't happen */
		elog(ERROR, "ALTER TABLE: relation \"%s\" has no column \"%s\"",
			 RelationGetRelationName(rel), colName);

	((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull = FALSE;

	simple_heap_update(attr_rel, &tuple->t_self, tuple);

	/* keep the system catalog indices current */
	if (RelationGetForm(attr_rel)->relhasindex)
	{
		Relation	idescs[Num_pg_attr_indices];

		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_attr_indices, attr_rel, tuple);
		CatalogCloseIndices(Num_pg_attr_indices, idescs);
	}

	heap_close(attr_rel, RowExclusiveLock);

	heap_close(rel, NoLock);
}

/*
 * ALTER TABLE ALTER COLUMN SET NOT NULL
 */
void
AlterTableAlterColumnSetNotNull(Oid myrelid,
								bool inh, const char *colName)
{
	Relation	rel;
	HeapTuple	tuple;
	AttrNumber	attnum;
	Relation	attr_rel;
	HeapScanDesc 	scan;
	TupleDesc	tupdesc;

	rel = heap_open(myrelid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 RelationGetRelationName(rel));

	if (!allowSystemTableMods
		&& IsSystemRelation(rel))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 RelationGetRelationName(rel));

	if (!pg_class_ownercheck(myrelid, GetUserId()))
		elog(ERROR, "ALTER TABLE: \"%s\": permission denied",
			 RelationGetRelationName(rel));

	/*
	 * Propagate to children if desired
	 */
	if (inh)
	{
		List	   *child,
				   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(myrelid);

		/*
		 * find_all_inheritors does the recursive search of the
		 * inheritance hierarchy, so all we have to do is process all of
		 * the relids in the list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirsti(child);

			if (childrelid == myrelid)
				continue;
			AlterTableAlterColumnSetNotNull(childrelid,
											false, colName);
		}
	}

	/* -= now do the thing on this relation =- */

	/*
	 * get the number of the attribute
	 */
	tuple = SearchSysCache(ATTNAME,
						   ObjectIdGetDatum(myrelid),
						   PointerGetDatum(colName),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ALTER TABLE: relation \"%s\" has no column \"%s\"",
			 RelationGetRelationName(rel), colName);

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;
	ReleaseSysCache(tuple);

	/* Prevent them from altering a system attribute */
	if (attnum < 0)
		elog(ERROR, "ALTER TABLE: Cannot alter system attribute \"%s\"",
			 colName);

	/*
	 * Perform a scan to ensure that there are no NULL
	 * values already in the relation
	 */
	tupdesc = RelationGetDescr(rel);

	scan = heap_beginscan(rel, false, SnapshotNow, 0, NULL);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		Datum 		d;
		bool		isnull;

		d = heap_getattr(tuple, attnum, tupdesc, &isnull);

		if (isnull)
			elog(ERROR, "ALTER TABLE: Attribute \"%s\" contains NULL values",
				 colName);
	}

	heap_endscan(scan);

	/*
	 * Okay, actually perform the catalog change
	 */
	attr_rel = heap_openr(AttributeRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(ATTNAME,
							   ObjectIdGetDatum(myrelid),
							   PointerGetDatum(colName),
							   0, 0);
	if (!HeapTupleIsValid(tuple)) /* shouldn't happen */
		elog(ERROR, "ALTER TABLE: relation \"%s\" has no column \"%s\"",
			 RelationGetRelationName(rel), colName);

	((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull = TRUE;

	simple_heap_update(attr_rel, &tuple->t_self, tuple);

	/* keep the system catalog indices current */
	if (RelationGetForm(attr_rel)->relhasindex)
	{
		Relation	idescs[Num_pg_attr_indices];

		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_attr_indices, attr_rel, tuple);
		CatalogCloseIndices(Num_pg_attr_indices, idescs);
	}

	heap_close(attr_rel, RowExclusiveLock);

	heap_close(rel, NoLock);
}


/*
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT
 */
void
AlterTableAlterColumnDefault(Oid myrelid,
							 bool inh, const char *colName,
							 Node *newDefault)
{
	Relation	rel;
	HeapTuple	tuple;
	AttrNumber	attnum;

	rel = heap_open(myrelid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 RelationGetRelationName(rel));

	if (!allowSystemTableMods
		&& IsSystemRelation(rel))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 RelationGetRelationName(rel));

	if (!pg_class_ownercheck(myrelid, GetUserId()))
		elog(ERROR, "ALTER TABLE: \"%s\": permission denied",
			 RelationGetRelationName(rel));

	/*
	 * Propagate to children if desired
	 */
	if (inh)
	{
		List	   *child,
				   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(myrelid);

		/*
		 * find_all_inheritors does the recursive search of the
		 * inheritance hierarchy, so all we have to do is process all of
		 * the relids in the list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirsti(child);

			if (childrelid == myrelid)
				continue;
			AlterTableAlterColumnDefault(childrelid,
										 false, colName, newDefault);
		}
	}

	/* -= now do the thing on this relation =- */

	/*
	 * get the number of the attribute
	 */
	tuple = SearchSysCache(ATTNAME,
						   ObjectIdGetDatum(myrelid),
						   PointerGetDatum(colName),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ALTER TABLE: relation \"%s\" has no column \"%s\"",
			 RelationGetRelationName(rel), colName);

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;
	ReleaseSysCache(tuple);

	if (newDefault)
	{
		/* SET DEFAULT */
		RawColumnDefault *rawEnt;

		/* Get rid of the old one first */
		drop_default(myrelid, attnum);

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = newDefault;

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 */
		AddRelationRawConstraints(rel, makeList1(rawEnt), NIL);
	}
	else
	{
		/* DROP DEFAULT */
		Relation	attr_rel;

		/* Fix the pg_attribute row */
		attr_rel = heap_openr(AttributeRelationName, RowExclusiveLock);

		tuple = SearchSysCacheCopy(ATTNAME,
								   ObjectIdGetDatum(myrelid),
								   PointerGetDatum(colName),
								   0, 0);
		if (!HeapTupleIsValid(tuple)) /* shouldn't happen */
			elog(ERROR, "ALTER TABLE: relation \"%s\" has no column \"%s\"",
				 RelationGetRelationName(rel), colName);

		((Form_pg_attribute) GETSTRUCT(tuple))->atthasdef = FALSE;

		simple_heap_update(attr_rel, &tuple->t_self, tuple);

		/* keep the system catalog indices current */
		if (RelationGetForm(attr_rel)->relhasindex)
		{
			Relation	idescs[Num_pg_attr_indices];

			CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);
			CatalogIndexInsert(idescs, Num_pg_attr_indices, attr_rel, tuple);
			CatalogCloseIndices(Num_pg_attr_indices, idescs);
		}

		heap_close(attr_rel, RowExclusiveLock);

		/* get rid of actual default definition in pg_attrdef */
		drop_default(myrelid, attnum);
	}

	heap_close(rel, NoLock);
}


static void
drop_default(Oid relid, int16 attnum)
{
	ScanKeyData scankeys[2];
	HeapScanDesc scan;
	Relation	attrdef_rel;
	HeapTuple	tuple;

	attrdef_rel = heap_openr(AttrDefaultRelationName, RowExclusiveLock);
	ScanKeyEntryInitialize(&scankeys[0], 0x0,
						   Anum_pg_attrdef_adrelid, F_OIDEQ,
						   ObjectIdGetDatum(relid));
	ScanKeyEntryInitialize(&scankeys[1], 0x0,
						   Anum_pg_attrdef_adnum, F_INT2EQ,
						   Int16GetDatum(attnum));

	scan = heap_beginscan(attrdef_rel, false, SnapshotNow, 2, scankeys);

	if (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
		simple_heap_delete(attrdef_rel, &tuple->t_self);

	heap_endscan(scan);

	heap_close(attrdef_rel, NoLock);
}


/*
 * ALTER TABLE ALTER COLUMN SET STATISTICS / STORAGE
 */
void
AlterTableAlterColumnFlags(Oid myrelid,
						   bool inh, const char *colName,
						   Node *flagValue, const char *flagType)
{
	Relation	rel;
	int			newtarget = 1;
	char        newstorage = 'x';
	char        *storagemode;
	Relation	attrelation;
	HeapTuple	tuple;

	rel = heap_open(myrelid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 RelationGetRelationName(rel));

	/*
	 * we allow statistics case for system tables
	 */
	if (*flagType != 'S' && !allowSystemTableMods && IsSystemRelation(rel))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 RelationGetRelationName(rel));

	if (!pg_class_ownercheck(myrelid, GetUserId()))
		elog(ERROR, "ALTER TABLE: \"%s\": permission denied",
			 RelationGetRelationName(rel));

	/*
	 * Check the supplied parameters before anything else
	 */
	if (*flagType == 'S')
	{
		/* STATISTICS */
		Assert(IsA(flagValue, Integer));
		newtarget = intVal(flagValue);

		/*
		 * Limit target to sane range (should we raise an error instead?)
		 */
		if (newtarget < 0)
			newtarget = 0;
		else if (newtarget > 1000)
			newtarget = 1000;
	}
	else if (*flagType == 'M')
	{
		/* STORAGE */
		Assert(IsA(flagValue, Value));

		storagemode = strVal(flagValue);
		if (strcasecmp(storagemode, "plain") == 0)
			newstorage = 'p';
		else if (strcasecmp(storagemode, "external") == 0)
			newstorage = 'e';
		else if (strcasecmp(storagemode, "extended") == 0)
			newstorage = 'x';
		else if (strcasecmp(storagemode, "main") == 0)
			newstorage = 'm';
		else
			elog(ERROR, "ALTER TABLE: \"%s\" storage not recognized",
				 storagemode);
	}
	else
	{
		elog(ERROR, "ALTER TABLE: Invalid column flag: %c",
			 (int) *flagType);
	}

	/*
	 * Propagate to children if desired
	 */
	if (inh)
	{
		List	   *child,
				   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(myrelid);

		/*
		 * find_all_inheritors does the recursive search of the
		 * inheritance hierarchy, so all we have to do is process all of
		 * the relids in the list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirsti(child);

			if (childrelid == myrelid)
				continue;
			AlterTableAlterColumnFlags(childrelid,
									   false, colName, flagValue, flagType);
		}
	}

	/* -= now do the thing on this relation =- */

	attrelation = heap_openr(AttributeRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(ATTNAME,
							   ObjectIdGetDatum(myrelid),
							   PointerGetDatum(colName),
							   0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ALTER TABLE: relation \"%s\" has no column \"%s\"",
			 RelationGetRelationName(rel), colName);

	if (((Form_pg_attribute) GETSTRUCT(tuple))->attnum < 0)
		elog(ERROR, "ALTER TABLE: cannot change system attribute \"%s\"",
			 colName);
	/*
	 * Now change the appropriate field
	 */
	if (*flagType == 'S')
		((Form_pg_attribute) GETSTRUCT(tuple))->attstattarget = newtarget;
	else
	{
		if ((newstorage == 'p') ||
			(((Form_pg_attribute) GETSTRUCT(tuple))->attlen == -1))
			((Form_pg_attribute) GETSTRUCT(tuple))->attstorage = newstorage;
		else
		{
			elog(ERROR,
				 "ALTER TABLE: Fixed-length columns can only have storage \"plain\"");
		}
	}
	simple_heap_update(attrelation, &tuple->t_self, tuple);

	/* keep system catalog indices current */
	{
		Relation	irelations[Num_pg_attr_indices];

		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, irelations);
		CatalogIndexInsert(irelations, Num_pg_attr_indices, attrelation, tuple);
		CatalogCloseIndices(Num_pg_attr_indices, irelations);
	}

	heap_freetuple(tuple);
	heap_close(attrelation, NoLock);
	heap_close(rel, NoLock);	/* close rel, but keep lock! */
}



/*
 * ALTER TABLE DROP COLUMN
 */
void
AlterTableDropColumn(Oid myrelid,
					 bool inh, const char *colName,
					 int behavior)
{
	elog(ERROR, "ALTER TABLE / DROP COLUMN is not implemented");
}



/*
 * ALTER TABLE ADD CONSTRAINT
 */
void
AlterTableAddConstraint(Oid myrelid,
						bool inh, List *newConstraints)
{
	Relation	rel;
	List	   *listptr;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	rel = heap_open(myrelid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 RelationGetRelationName(rel));

	if (!allowSystemTableMods
		&& IsSystemRelation(rel))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 RelationGetRelationName(rel));

	if (!pg_class_ownercheck(myrelid, GetUserId()))
		elog(ERROR, "ALTER TABLE: \"%s\": permission denied",
			 RelationGetRelationName(rel));

	if (inh)
	{
		List	   *child,
				   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(myrelid);

		/*
		 * find_all_inheritors does the recursive search of the
		 * inheritance hierarchy, so all we have to do is process all of
		 * the relids in the list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirsti(child);

			if (childrelid == myrelid)
				continue;
			AlterTableAddConstraint(childrelid, false, newConstraints);
		}
	}

	foreach(listptr, newConstraints)
	{
		Node	   *newConstraint = lfirst(listptr);

		switch (nodeTag(newConstraint))
		{
			case T_Constraint:
				{
					Constraint *constr = (Constraint *) newConstraint;

					/*
					 * Currently, we only expect to see CONSTR_CHECK nodes
					 * arriving here (see the preprocessing done in
					 * parser/analyze.c).  Use a switch anyway to make it
					 * easier to add more code later.
					 */
					switch (constr->contype)
					{
						case CONSTR_CHECK:
							{
								ParseState *pstate;
								bool		successful = true;
								HeapScanDesc scan;
								ExprContext *econtext;
								TupleTableSlot *slot;
								HeapTuple	tuple;
								RangeTblEntry *rte;
								List	   *qual;
								Node	   *expr;
								char	   *name;

								if (constr->name)
									name = constr->name;
								else
									name = "<unnamed>";

								/*
								 * We need to make a parse state and range
								 * table to allow us to transformExpr and
								 * fix_opids to get a version of the
								 * expression we can pass to ExecQual
								 */
								pstate = make_parsestate(NULL);
								rte = addRangeTableEntryForRelation(pstate,
																	myrelid,
											makeAlias(RelationGetRelationName(rel), NIL),
																	false,
																	true);
								addRTEtoQuery(pstate, rte, true, true);

								/*
								 * Convert the A_EXPR in raw_expr into an
								 * EXPR
								 */
								expr = transformExpr(pstate, constr->raw_expr);

								/*
								 * Make sure it yields a boolean result.
								 */
								if (exprType(expr) != BOOLOID)
									elog(ERROR, "CHECK '%s' does not yield boolean result",
										 name);

								/*
								 * Make sure no outside relations are
								 * referred to.
								 */
								if (length(pstate->p_rtable) != 1)
									elog(ERROR, "Only relation '%s' can be referenced in CHECK",
										 RelationGetRelationName(rel));

								/*
								 * Might as well try to reduce any
								 * constant expressions.
								 */
								expr = eval_const_expressions(expr);

								/* And fix the opids */
								fix_opids(expr);

								qual = makeList1(expr);

								/* Make tuple slot to hold tuples */
								slot = MakeTupleTableSlot();
								ExecSetSlotDescriptor(slot, RelationGetDescr(rel), false);
								/* Make an expression context for ExecQual */
								econtext = MakeExprContext(slot, CurrentMemoryContext);

								/*
								 * Scan through the rows now, checking the
								 * expression at each row.
								 */
								scan = heap_beginscan(rel, false, SnapshotNow, 0, NULL);

								while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
								{
									ExecStoreTuple(tuple, slot, InvalidBuffer, false);
									if (!ExecQual(qual, econtext, true))
									{
										successful = false;
										break;
									}
									ResetExprContext(econtext);
								}

								heap_endscan(scan);

								FreeExprContext(econtext);
								pfree(slot);

								if (!successful)
									elog(ERROR, "AlterTableAddConstraint: rejected due to CHECK constraint %s", name);

								/*
								 * Call AddRelationRawConstraints to do
								 * the real adding -- It duplicates some
								 * of the above, but does not check the
								 * validity of the constraint against
								 * tuples already in the table.
								 */
								AddRelationRawConstraints(rel, NIL,
													  makeList1(constr));

								break;
							}
						default:
							elog(ERROR, "ALTER TABLE / ADD CONSTRAINT is not implemented for that constraint type.");
					}
					break;
				}
			case T_FkConstraint:
				{
					FkConstraint *fkconstraint = (FkConstraint *) newConstraint;
					Relation	pkrel;
					HeapScanDesc scan;
					HeapTuple	tuple;
					Trigger		trig;
					List	   *list;
					int			count;

					/*
					 * Grab an exclusive lock on the pk table, so that
					 * someone doesn't delete rows out from under us.
					 *
					 * XXX wouldn't a lesser lock be sufficient?
					 */
					pkrel = heap_openrv(fkconstraint->pktable,
										AccessExclusiveLock);

					/*
					 * Validity checks
					 */
					if (pkrel->rd_rel->relkind != RELKIND_RELATION)
						elog(ERROR, "referenced table \"%s\" not a relation",
							 fkconstraint->pktable->relname);

					if (isTempNamespace(RelationGetNamespace(pkrel)) &&
						!isTempNamespace(RelationGetNamespace(rel)))
						elog(ERROR, "ALTER TABLE / ADD CONSTRAINT: Unable to reference temporary table from permanent table constraint.");

					/*
					 * First we check for limited correctness of the
					 * constraint.
					 *
					 * NOTE: we assume parser has already checked for
					 * existence of an appropriate unique index on the
					 * referenced relation, and that the column datatypes
					 * are comparable.
					 *
					 * Scan through each tuple, calling RI_FKey_check_ins
					 * (insert trigger) as if that tuple had just been
					 * inserted.  If any of those fail, it should
					 * elog(ERROR) and that's that.
					 */
					MemSet(&trig, 0, sizeof(trig));
					trig.tgoid = InvalidOid;
					if (fkconstraint->constr_name)
						trig.tgname = fkconstraint->constr_name;
					else
						trig.tgname = "<unknown>";
					trig.tgenabled = TRUE;
					trig.tgisconstraint = TRUE;
					trig.tgconstrrelid = RelationGetRelid(pkrel);
					trig.tgdeferrable = FALSE;
					trig.tginitdeferred = FALSE;

					trig.tgargs = (char **) palloc(
					 sizeof(char *) * (4 + length(fkconstraint->fk_attrs)
									   + length(fkconstraint->pk_attrs)));

					trig.tgargs[0] = trig.tgname;
					trig.tgargs[1] = RelationGetRelationName(rel);
					trig.tgargs[2] = RelationGetRelationName(pkrel);
					trig.tgargs[3] = fkconstraint->match_type;
					count = 4;
					foreach(list, fkconstraint->fk_attrs)
					{
						Ident	   *fk_at = lfirst(list);

						trig.tgargs[count] = fk_at->name;
						count += 2;
					}
					count = 5;
					foreach(list, fkconstraint->pk_attrs)
					{
						Ident	   *pk_at = lfirst(list);

						trig.tgargs[count] = pk_at->name;
						count += 2;
					}
					trig.tgnargs = count - 1;

					scan = heap_beginscan(rel, false, SnapshotNow, 0, NULL);

					while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
					{
						/* Make a call to the check function */

						/*
						 * No parameters are passed, but we do set a
						 * context
						 */
						FunctionCallInfoData fcinfo;
						TriggerData trigdata;

						MemSet(&fcinfo, 0, sizeof(fcinfo));

						/*
						 * We assume RI_FKey_check_ins won't look at
						 * flinfo...
						 */

						trigdata.type = T_TriggerData;
						trigdata.tg_event = TRIGGER_EVENT_INSERT | TRIGGER_EVENT_ROW;
						trigdata.tg_relation = rel;
						trigdata.tg_trigtuple = tuple;
						trigdata.tg_newtuple = NULL;
						trigdata.tg_trigger = &trig;

						fcinfo.context = (Node *) &trigdata;

						RI_FKey_check_ins(&fcinfo);
					}
					heap_endscan(scan);

					pfree(trig.tgargs);

					heap_close(pkrel, NoLock);

					break;
				}
			default:
				elog(ERROR, "ALTER TABLE / ADD CONSTRAINT unable to determine type of constraint passed");
		}
	}

	/* Close rel, but keep lock till commit */
	heap_close(rel, NoLock);
}



/*
 * ALTER TABLE DROP CONSTRAINT
 * Note: It is legal to remove a constraint with name "" as it is possible
 * to add a constraint with name "".
 * Christopher Kings-Lynne
 */
void
AlterTableDropConstraint(Oid myrelid,
						 bool inh, const char *constrName,
						 int behavior)
{
	Relation	rel;
	int			deleted;

	/*
	 * We don't support CASCADE yet  - in fact, RESTRICT doesn't work to
	 * the spec either!
	 */
	if (behavior == CASCADE)
		elog(ERROR, "ALTER TABLE / DROP CONSTRAINT does not support the CASCADE keyword");

	/*
	 * Acquire an exclusive lock on the target relation for the duration
	 * of the operation.
	 */
	rel = heap_open(myrelid, AccessExclusiveLock);

	/* Disallow DROP CONSTRAINT on views, indexes, sequences, etc */
	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 RelationGetRelationName(rel));

	if (!allowSystemTableMods
		&& IsSystemRelation(rel))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 RelationGetRelationName(rel));

	if (!pg_class_ownercheck(myrelid, GetUserId()))
		elog(ERROR, "ALTER TABLE: \"%s\": permission denied",
			 RelationGetRelationName(rel));

	/*
	 * Since all we have is the name of the constraint, we have to look
	 * through all catalogs that could possibly contain a constraint for
	 * this relation. We also keep a count of the number of constraints
	 * removed.
	 */

	deleted = 0;

	/*
	 * First, we remove all CHECK constraints with the given name
	 */

	deleted += RemoveCheckConstraint(rel, constrName, inh);

	/*
	 * Now we remove NULL, UNIQUE, PRIMARY KEY and FOREIGN KEY
	 * constraints.
	 *
	 * Unimplemented.
	 */

	/* Close the target relation */
	heap_close(rel, NoLock);

	/* If zero constraints deleted, complain */
	if (deleted == 0)
		elog(ERROR, "ALTER TABLE / DROP CONSTRAINT: %s does not exist",
			 constrName);
	/* Otherwise if more than one constraint deleted, notify */
	else if (deleted > 1)
		elog(NOTICE, "Multiple constraints dropped");
}

/*
 * ALTER TABLE OWNER
 */
void
AlterTableOwner(Oid relationOid, int32 newOwnerSysId)
{
	Relation		target_rel;
	Relation		class_rel;
	HeapTuple		tuple;
	Relation		idescs[Num_pg_class_indices];
	Form_pg_class	tuple_class;

	/* Get exclusive lock till end of transaction on the target table */
	target_rel = heap_open(relationOid, AccessExclusiveLock);

	/* Get its pg_class tuple, too */
	class_rel = heap_openr(RelationRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(relationOid),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ALTER TABLE: relation %u not found", relationOid);
	tuple_class = (Form_pg_class) GETSTRUCT(tuple);

	/* Can we change the ownership of this tuple? */
	CheckTupleType(tuple_class);

	/*
	 * Okay, this is a valid tuple: change its ownership and
	 * write to the heap.
	 */
	tuple_class->relowner = newOwnerSysId;
	simple_heap_update(class_rel, &tuple->t_self, tuple);

	/* Keep the catalog indices up to date */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
	CatalogIndexInsert(idescs, Num_pg_class_indices, class_rel, tuple);
	CatalogCloseIndices(Num_pg_class_indices, idescs);

	/*
	 * If we are operating on a table, also change the ownership of any
	 * indexes that belong to the table, as well as the table's toast
	 * table (if it has one)
	 */
	if (tuple_class->relkind == RELKIND_RELATION ||
		tuple_class->relkind == RELKIND_TOASTVALUE)
	{
		List *index_oid_list, *i;

		/* Find all the indexes belonging to this relation */
		index_oid_list = RelationGetIndexList(target_rel);

		/* For each index, recursively change its ownership */
		foreach(i, index_oid_list)
		{
			AlterTableOwner(lfirsti(i), newOwnerSysId);
		}

		freeList(index_oid_list);
	}

	if (tuple_class->relkind == RELKIND_RELATION)
	{
		/* If it has a toast table, recurse to change its ownership */
		if (tuple_class->reltoastrelid != InvalidOid)
		{
			AlterTableOwner(tuple_class->reltoastrelid, newOwnerSysId);
		}
	}

	heap_freetuple(tuple);
	heap_close(class_rel, RowExclusiveLock);
	heap_close(target_rel, NoLock);
}

static void
CheckTupleType(Form_pg_class tuple_class)
{
	switch (tuple_class->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_INDEX:
		case RELKIND_VIEW:
		case RELKIND_SEQUENCE:
		case RELKIND_TOASTVALUE:
			/* ok to change owner */
			break;
		default:
			elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table, TOAST table, index, view, or sequence",
				 NameStr(tuple_class->relname));
	}
}

/*
 * ALTER TABLE CREATE TOAST TABLE
 */
void
AlterTableCreateToastTable(Oid relOid, bool silent)
{
	Relation	rel;
	HeapTuple	reltup;
	HeapTupleData classtuple;
	TupleDesc	tupdesc;
	Relation	class_rel;
	Buffer		buffer;
	Relation	ridescs[Num_pg_class_indices];
	Oid			toast_relid;
	Oid			toast_idxid;
	char		toast_relname[NAMEDATALEN + 1];
	char		toast_idxname[NAMEDATALEN + 1];
	IndexInfo  *indexInfo;
	Oid			classObjectId[2];

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	rel = heap_open(relOid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 RelationGetRelationName(rel));

	if (!pg_class_ownercheck(relOid, GetUserId()))
		elog(ERROR, "ALTER TABLE: \"%s\": permission denied",
			 RelationGetRelationName(rel));

	/*
	 * lock the pg_class tuple for update (is that really needed?)
	 */
	class_rel = heap_openr(RelationRelationName, RowExclusiveLock);

	reltup = SearchSysCache(RELOID,
							ObjectIdGetDatum(relOid),
							0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "ALTER TABLE: relation \"%s\" not found",
			 RelationGetRelationName(rel));
	classtuple.t_self = reltup->t_self;
	ReleaseSysCache(reltup);

	switch (heap_mark4update(class_rel, &classtuple, &buffer))
	{
		case HeapTupleSelfUpdated:
		case HeapTupleMayBeUpdated:
			break;
		default:
			elog(ERROR, "couldn't lock pg_class tuple");
	}
	reltup = heap_copytuple(&classtuple);
	ReleaseBuffer(buffer);

	/*
	 * Is it already toasted?
	 */
	if (((Form_pg_class) GETSTRUCT(reltup))->reltoastrelid != InvalidOid)
	{
		if (silent)
		{
			heap_close(rel, NoLock);
			heap_close(class_rel, NoLock);
			heap_freetuple(reltup);
			return;
		}

		elog(ERROR, "ALTER TABLE: relation \"%s\" already has a toast table",
			 RelationGetRelationName(rel));
	}

	/*
	 * Check to see whether the table actually needs a TOAST table.
	 */
	if (!needs_toast_table(rel))
	{
		if (silent)
		{
			heap_close(rel, NoLock);
			heap_close(class_rel, NoLock);
			heap_freetuple(reltup);
			return;
		}

		elog(ERROR, "ALTER TABLE: relation \"%s\" does not need a toast table",
			 RelationGetRelationName(rel));
	}

	/*
	 * Create the toast table and its index
	 */
	sprintf(toast_relname, "pg_toast_%u", relOid);
	sprintf(toast_idxname, "pg_toast_%u_index", relOid);

	/* this is pretty painful...  need a tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(3);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1,
					   "chunk_id",
					   OIDOID,
					   -1, 0, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2,
					   "chunk_seq",
					   INT4OID,
					   -1, 0, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3,
					   "chunk_data",
					   BYTEAOID,
					   -1, 0, false);

	/*
	 * Ensure that the toast table doesn't itself get toasted, or we'll be
	 * toast :-(.  This is essential for chunk_data because type bytea is
	 * toastable; hit the other two just to be sure.
	 */
	tupdesc->attrs[0]->attstorage = 'p';
	tupdesc->attrs[1]->attstorage = 'p';
	tupdesc->attrs[2]->attstorage = 'p';

	/*
	 * Note: the toast relation is placed in the regular pg_toast namespace
	 * even if its master relation is a temp table.  There cannot be any
	 * naming collision, and the toast rel will be destroyed when its master
	 * is, so there's no need to handle the toast rel as temp.
	 */
	toast_relid = heap_create_with_catalog(toast_relname,
										   PG_TOAST_NAMESPACE,
										   tupdesc,
										   RELKIND_TOASTVALUE,
										   false,
										   true);

	/* make the toast relation visible, else index creation will fail */
	CommandCounterIncrement();

	/*
	 * Create unique index on chunk_id, chunk_seq.
	 *
	 * NOTE: the tuple toaster could actually function with a single-column
	 * index on chunk_id only.	However, it couldn't be unique then.  We
	 * want it to be unique as a check against the possibility of
	 * duplicate TOAST chunk OIDs.	Too, the index might be a little more
	 * efficient this way, since btree isn't all that happy with large
	 * numbers of equal keys.
	 */

	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_NumIndexAttrs = 2;
	indexInfo->ii_NumKeyAttrs = 2;
	indexInfo->ii_KeyAttrNumbers[0] = 1;
	indexInfo->ii_KeyAttrNumbers[1] = 2;
	indexInfo->ii_Predicate = NIL;
	indexInfo->ii_FuncOid = InvalidOid;
	indexInfo->ii_Unique = true;

	classObjectId[0] = OID_BTREE_OPS_OID;
	classObjectId[1] = INT4_BTREE_OPS_OID;

	toast_idxid = index_create(toast_relid, toast_idxname, indexInfo,
							   BTREE_AM_OID, classObjectId,
							   true, true);

	/*
	 * Update toast rel's pg_class entry to show that it has an index. The
	 * index OID is stored into the reltoastidxid field for easy access by
	 * the tuple toaster.
	 */
	setRelhasindex(toast_relid, true, true, toast_idxid);

	/*
	 * Store the toast table's OID in the parent relation's tuple
	 */
	((Form_pg_class) GETSTRUCT(reltup))->reltoastrelid = toast_relid;
	simple_heap_update(class_rel, &reltup->t_self, reltup);

	/*
	 * Keep catalog indices current
	 */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
	CatalogIndexInsert(ridescs, Num_pg_class_indices, class_rel, reltup);
	CatalogCloseIndices(Num_pg_class_indices, ridescs);

	heap_freetuple(reltup);

	/*
	 * Close relations and make changes visible
	 */
	heap_close(class_rel, NoLock);
	heap_close(rel, NoLock);

	CommandCounterIncrement();
}

/*
 * Check to see whether the table needs a TOAST table.	It does only if
 * (1) there are any toastable attributes, and (2) the maximum length
 * of a tuple could exceed TOAST_TUPLE_THRESHOLD.  (We don't want to
 * create a toast table for something like "f1 varchar(20)".)
 */
static bool
needs_toast_table(Relation rel)
{
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc;
	Form_pg_attribute *att;
	int32		tuple_length;
	int			i;

	tupdesc = rel->rd_att;
	att = tupdesc->attrs;

	for (i = 0; i < tupdesc->natts; i++)
	{
		data_length = att_align(data_length, att[i]->attlen, att[i]->attalign);
		if (att[i]->attlen >= 0)
		{
			/* Fixed-length types are never toastable */
			data_length += att[i]->attlen;
		}
		else
		{
			int32		maxlen = type_maximum_size(att[i]->atttypid,
												   att[i]->atttypmod);

			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att[i]->attstorage != 'p')
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
		return false;			/* nothing to toast? */
	if (maxlength_unknown)
		return true;			/* any unlimited-length attrs? */
	tuple_length = MAXALIGN(offsetof(HeapTupleHeaderData, t_bits) +
							BITMAPLEN(tupdesc->natts)) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
}


/* ----------------------------------------------------------------
 *		DefineRelation
 *				Creates a new relation.
 *
 * If successful, returns the OID of the new relation.
 * ----------------------------------------------------------------
 */
Oid
DefineRelation(CreateStmt *stmt, char relkind)
{
	char	   *relname = palloc(NAMEDATALEN);
	Oid			namespaceId;
	List	   *schema = stmt->tableElts;
	int			numberOfAttributes;
	Oid			relationId;
	Relation	rel;
	TupleDesc	descriptor;
	List	   *inheritOids;
	List	   *old_constraints;
	bool		parentHasOids;
	List	   *rawDefaults;
	List	   *listptr;
	int			i;
	AttrNumber	attnum;

	/*
	 * Truncate relname to appropriate length (probably a waste of time,
	 * as parser should have done this already).
	 */
	StrNCpy(relname, (stmt->relation)->relname, NAMEDATALEN);

	/*
	 * Look up the namespace in which we are supposed to create the
	 * relation.
	 */
	namespaceId = RangeVarGetCreationNamespace(stmt->relation);

	/*
	 * Merge domain attributes into the known columns before processing table
	 * inheritance.  Otherwise we risk adding double constraints to a
	 * domain-type column that's inherited.
	 */
	schema = MergeDomainAttributes(schema);

	/*
	 * Look up inheritance ancestors and generate relation schema,
	 * including inherited attributes.
	 */
	schema = MergeAttributes(schema, stmt->inhRelations,
							 stmt->relation->istemp,
							 &inheritOids, &old_constraints, &parentHasOids);

	numberOfAttributes = length(schema);
	if (numberOfAttributes <= 0)
		elog(ERROR, "DefineRelation: please inherit from a relation or define an attribute");

	/*
	 * Create a relation descriptor from the relation schema and create
	 * the relation.  Note that in this stage only inherited (pre-cooked)
	 * defaults and constraints will be included into the new relation.
	 * (BuildDescForRelation takes care of the inherited defaults, but we
	 * have to copy inherited constraints here.)
	 */
	descriptor = BuildDescForRelation(schema);

	if (old_constraints != NIL)
	{
		ConstrCheck *check = (ConstrCheck *) palloc(length(old_constraints) *
													sizeof(ConstrCheck));
		int			ncheck = 0;

		foreach(listptr, old_constraints)
		{
			Constraint *cdef = (Constraint *) lfirst(listptr);

			if (cdef->contype != CONSTR_CHECK)
				continue;

			if (cdef->name != NULL)
			{
				for (i = 0; i < ncheck; i++)
				{
					if (strcmp(check[i].ccname, cdef->name) == 0)
						elog(ERROR, "Duplicate CHECK constraint name: '%s'",
							 cdef->name);
				}
				check[ncheck].ccname = cdef->name;
			}
			else
			{
				check[ncheck].ccname = (char *) palloc(NAMEDATALEN);
				snprintf(check[ncheck].ccname, NAMEDATALEN, "$%d", ncheck + 1);
			}
			Assert(cdef->raw_expr == NULL && cdef->cooked_expr != NULL);
			check[ncheck].ccbin = pstrdup(cdef->cooked_expr);
			ncheck++;
		}
		if (ncheck > 0)
		{
			if (descriptor->constr == NULL)
			{
				descriptor->constr = (TupleConstr *) palloc(sizeof(TupleConstr));
				descriptor->constr->defval = NULL;
				descriptor->constr->num_defval = 0;
				descriptor->constr->has_not_null = false;
			}
			descriptor->constr->num_check = ncheck;
			descriptor->constr->check = check;
		}
	}

	relationId = heap_create_with_catalog(relname,
										  namespaceId,
										  descriptor,
										  relkind,
										  stmt->hasoids || parentHasOids,
										  allowSystemTableMods);

	StoreCatalogInheritance(relationId, inheritOids);

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 */
	CommandCounterIncrement();

	/*
	 * Open the new relation and acquire exclusive lock on it.	This isn't
	 * really necessary for locking out other backends (since they can't
	 * see the new rel anyway until we commit), but it keeps the lock
	 * manager from complaining about deadlock risks.
	 */
	rel = heap_open(relationId, AccessExclusiveLock);

	/*
	 * Now add any newly specified column default values and CHECK
	 * constraints to the new relation.  These are passed to us in the
	 * form of raw parsetrees; we need to transform them to executable
	 * expression trees before they can be added. The most convenient way
	 * to do that is to apply the parser's transformExpr routine, but
	 * transformExpr doesn't work unless we have a pre-existing relation.
	 * So, the transformation has to be postponed to this final step of
	 * CREATE TABLE.
	 *
	 * First, scan schema to find new column defaults.
	 */
	rawDefaults = NIL;
	attnum = 0;

	foreach(listptr, schema)
	{
		ColumnDef  *colDef = lfirst(listptr);
		RawColumnDefault *rawEnt;

		attnum++;

		if (colDef->raw_default == NULL)
			continue;
		Assert(colDef->cooked_default == NULL);

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = colDef->raw_default;
		rawDefaults = lappend(rawDefaults, rawEnt);
	}

	/*
	 * Parse and add the defaults/constraints, if any.
	 */
	if (rawDefaults || stmt->constraints)
		AddRelationRawConstraints(rel, rawDefaults, stmt->constraints);

	/*
	 * Clean up.  We keep lock on new relation (although it shouldn't be
	 * visible to anyone else anyway, until commit).
	 */
	heap_close(rel, NoLock);

	return relationId;
}

/*
 * RemoveRelation
 *		Deletes a relation.
 *
 * Exceptions:
 *		BadArg if name is invalid.
 *
 * Note:
 *		If the relation has indices defined on it, then the index relations
 * themselves will be destroyed, too.
 */
void
RemoveRelation(const RangeVar *relation)
{
	Oid			relOid;

	relOid = RangeVarGetRelid(relation, false);
	heap_drop_with_catalog(relOid, allowSystemTableMods);
}

/*
 * TruncateRelation
 *				  Removes all the rows from a relation
 *
 * Exceptions:
 *				  BadArg if name is invalid
 *
 * Note:
 *				  Rows are removed, indices are truncated and reconstructed.
 */
void
TruncateRelation(const RangeVar *relation)
{
	Relation	rel;
	Oid			relid;

	/* Grab exclusive lock in preparation for truncate */
	rel = heap_openrv(relation, AccessExclusiveLock);
	relid = RelationGetRelid(rel);

	if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
		elog(ERROR, "TRUNCATE cannot be used on sequences. '%s' is a sequence",
			 RelationGetRelationName(rel));

	if (rel->rd_rel->relkind == RELKIND_VIEW)
		elog(ERROR, "TRUNCATE cannot be used on views. '%s' is a view",
			 RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		elog(ERROR, "TRUNCATE cannot be used on system tables. '%s' is a system table",
			 RelationGetRelationName(rel));

	if (!pg_class_ownercheck(relid, GetUserId()))
		elog(ERROR, "you do not own relation \"%s\"",
			 RelationGetRelationName(rel));

	/* Keep the lock until transaction commit */
	heap_close(rel, NoLock);

	heap_truncate(relid);
}


/*
 * MergeDomainAttributes
 *      Returns a new table schema with the constraints, types, and other
 *      attributes of domains resolved for fields using a domain as
 *      their type.
 */
static List *
MergeDomainAttributes(List *schema)
{
	List	   *entry;

	/*
	 * Loop through the table elements supplied. These should
	 * never include inherited domains else they'll be
	 * double (or more) processed.
	 */
	foreach(entry, schema)
	{
		ColumnDef  *coldef = lfirst(entry);
		HeapTuple  tuple;
		Form_pg_type typeTup;

		tuple = typenameType(coldef->typename);
		typeTup = (Form_pg_type) GETSTRUCT(tuple);

		if (typeTup->typtype == 'd')
		{
			/* Force the column to have the correct typmod. */
			coldef->typename->typmod = typeTup->typtypmod;
			/* XXX more to do here? */
		}

		/* Enforce type NOT NULL || column definition NOT NULL -> NOT NULL */
		/* Currently only used for domains, but could be valid for all */
		coldef->is_not_null |= typeTup->typnotnull;

		ReleaseSysCache(tuple);
	}

	return schema;
}

/*----------
 * MergeAttributes
 *		Returns new schema given initial schema and superclasses.
 *
 * Input arguments:
 * 'schema' is the column/attribute definition for the table. (It's a list
 *		of ColumnDef's.) It is destructively changed.
 * 'supers' is a list of names (as RangeVar nodes) of parent relations.
 * 'istemp' is TRUE if we are creating a temp relation.
 *
 * Output arguments:
 * 'supOids' receives an integer list of the OIDs of the parent relations.
 * 'supconstr' receives a list of constraints belonging to the parents,
 *		updated as necessary to be valid for the child.
 * 'supHasOids' is set TRUE if any parent has OIDs, else it is set FALSE.
 *
 * Return value:
 * Completed schema list.
 *
 * Notes:
 *	  The order in which the attributes are inherited is very important.
 *	  Intuitively, the inherited attributes should come first. If a table
 *	  inherits from multiple parents, the order of those attributes are
 *	  according to the order of the parents specified in CREATE TABLE.
 *
 *	  Here's an example:
 *
 *		create table person (name text, age int4, location point);
 *		create table emp (salary int4, manager text) inherits(person);
 *		create table student (gpa float8) inherits (person);
 *		create table stud_emp (percent int4) inherits (emp, student);
 *
 *	  The order of the attributes of stud_emp is:
 *
 *							person {1:name, 2:age, 3:location}
 *							/	 \
 *			   {6:gpa}	student   emp {4:salary, 5:manager}
 *							\	 /
 *						   stud_emp {7:percent}
 *
 *	   If the same attribute name appears multiple times, then it appears
 *	   in the result table in the proper location for its first appearance.
 *
 *	   Constraints (including NOT NULL constraints) for the child table
 *	   are the union of all relevant constraints, from both the child schema
 *	   and parent tables.
 *
 *	   The default value for a child column is defined as:
 *		(1) If the child schema specifies a default, that value is used.
 *		(2) If neither the child nor any parent specifies a default, then
 *			the column will not have a default.
 *		(3) If conflicting defaults are inherited from different parents
 *			(and not overridden by the child), an error is raised.
 *		(4) Otherwise the inherited default is used.
 *		Rule (3) is new in Postgres 7.1; in earlier releases you got a
 *		rather arbitrary choice of which parent default to use.
 *----------
 */
static List *
MergeAttributes(List *schema, List *supers, bool istemp,
				List **supOids, List **supconstr, bool *supHasOids)
{
	List	   *entry;
	List	   *inhSchema = NIL;
	List	   *parentOids = NIL;
	List	   *constraints = NIL;
	bool		parentHasOids = false;
	bool		have_bogus_defaults = false;
	char	   *bogus_marker = "Bogus!";		/* marks conflicting
												 * defaults */
	int			child_attno;

	/*
	 * Check for duplicate names in the explicit list of attributes.
	 *
	 * Although we might consider merging such entries in the same way that
	 * we handle name conflicts for inherited attributes, it seems to make
	 * more sense to assume such conflicts are errors.
	 */
	foreach(entry, schema)
	{
		ColumnDef  *coldef = lfirst(entry);
		List	   *rest;

		foreach(rest, lnext(entry))
		{
			ColumnDef  *restdef = lfirst(rest);

			if (strcmp(coldef->colname, restdef->colname) == 0)
				elog(ERROR, "CREATE TABLE: attribute \"%s\" duplicated",
					 coldef->colname);
		}
	}

	/*
	 * Scan the parents left-to-right, and merge their attributes to form
	 * a list of inherited attributes (inhSchema).	Also check to see if
	 * we need to inherit an OID column.
	 */
	child_attno = 0;
	foreach(entry, supers)
	{
		RangeVar   *parent = (RangeVar *) lfirst(entry);
		Relation	relation;
		TupleDesc	tupleDesc;
		TupleConstr *constr;
		AttrNumber *newattno;
		AttrNumber	parent_attno;

		relation = heap_openrv(parent, AccessShareLock);

		if (relation->rd_rel->relkind != RELKIND_RELATION)
			elog(ERROR, "CREATE TABLE: inherited relation \"%s\" is not a table",
				 parent->relname);
		/* Permanent rels cannot inherit from temporary ones */
		if (!istemp && isTempNamespace(RelationGetNamespace(relation)))
			elog(ERROR, "CREATE TABLE: cannot inherit from temp relation \"%s\"",
				 parent->relname);

		/*
		 * We should have an UNDER permission flag for this, but for now,
		 * demand that creator of a child table own the parent.
		 */
		if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
			elog(ERROR, "you do not own table \"%s\"",
				 parent->relname);

		/*
		 * Reject duplications in the list of parents.
		 */
		if (intMember(RelationGetRelid(relation), parentOids))
			elog(ERROR, "CREATE TABLE: inherited relation \"%s\" duplicated",
				 parent->relname);

		parentOids = lappendi(parentOids, RelationGetRelid(relation));
		setRelhassubclassInRelation(RelationGetRelid(relation), true);

		parentHasOids |= relation->rd_rel->relhasoids;

		tupleDesc = RelationGetDescr(relation);
		constr = tupleDesc->constr;

		/*
		 * newattno[] will contain the child-table attribute numbers for
		 * the attributes of this parent table.  (They are not the same
		 * for parents after the first one.)
		 */
		newattno = (AttrNumber *) palloc(tupleDesc->natts * sizeof(AttrNumber));

		for (parent_attno = 1; parent_attno <= tupleDesc->natts;
			 parent_attno++)
		{
			Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
			char	   *attributeName = NameStr(attribute->attname);
			int			exist_attno;
			ColumnDef  *def;
			TypeName   *typename;

			/*
			 * Does it conflict with some previously inherited column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				/*
				 * Yes, try to merge the two column definitions. They must
				 * have the same type and typmod.
				 */
				elog(NOTICE, "CREATE TABLE: merging multiple inherited definitions of attribute \"%s\"",
					 attributeName);
				def = (ColumnDef *) nth(exist_attno - 1, inhSchema);
				if (typenameTypeId(def->typename) != attribute->atttypid ||
					def->typename->typmod != attribute->atttypmod)
					elog(ERROR, "CREATE TABLE: inherited attribute \"%s\" type conflict (%s and %s)",
						 attributeName,
						 TypeNameToString(def->typename),
						 typeidTypeName(attribute->atttypid));
				/* Merge of NOT NULL constraints = OR 'em together */
				def->is_not_null |= attribute->attnotnull;
				/* Default and other constraints are handled below */
				newattno[parent_attno - 1] = exist_attno;
			}
			else
			{
				/*
				 * No, create a new inherited column
				 */
				def = makeNode(ColumnDef);
				def->colname = pstrdup(attributeName);
				typename = makeNode(TypeName);
				typename->typeid = attribute->atttypid;
				typename->typmod = attribute->atttypmod;
				def->typename = typename;
				def->is_not_null = attribute->attnotnull;
				def->raw_default = NULL;
				def->cooked_default = NULL;
				def->constraints = NIL;
				inhSchema = lappend(inhSchema, def);
				newattno[parent_attno - 1] = ++child_attno;
			}

			/*
			 * Copy default if any
			 */
			if (attribute->atthasdef)
			{
				char	   *this_default = NULL;
				AttrDefault *attrdef;
				int			i;

				/* Find default in constraint structure */
				Assert(constr != NULL);
				attrdef = constr->defval;
				for (i = 0; i < constr->num_defval; i++)
				{
					if (attrdef[i].adnum == parent_attno)
					{
						this_default = attrdef[i].adbin;
						break;
					}
				}
				Assert(this_default != NULL);

				/*
				 * If default expr could contain any vars, we'd need to
				 * fix 'em, but it can't; so default is ready to apply to
				 * child.
				 *
				 * If we already had a default from some prior parent, check
				 * to see if they are the same.  If so, no problem; if
				 * not, mark the column as having a bogus default. Below,
				 * we will complain if the bogus default isn't overridden
				 * by the child schema.
				 */
				Assert(def->raw_default == NULL);
				if (def->cooked_default == NULL)
					def->cooked_default = pstrdup(this_default);
				else if (strcmp(def->cooked_default, this_default) != 0)
				{
					def->cooked_default = bogus_marker;
					have_bogus_defaults = true;
				}
			}
		}

		/*
		 * Now copy the constraints of this parent, adjusting attnos using
		 * the completed newattno[] map
		 */
		if (constr && constr->num_check > 0)
		{
			ConstrCheck *check = constr->check;
			int			i;

			for (i = 0; i < constr->num_check; i++)
			{
				Constraint *cdef = makeNode(Constraint);
				Node	   *expr;

				cdef->contype = CONSTR_CHECK;
				if (check[i].ccname[0] == '$')
					cdef->name = NULL;
				else
					cdef->name = pstrdup(check[i].ccname);
				cdef->raw_expr = NULL;
				/* adjust varattnos of ccbin here */
				expr = stringToNode(check[i].ccbin);
				change_varattnos_of_a_node(expr, newattno);
				cdef->cooked_expr = nodeToString(expr);
				constraints = lappend(constraints, cdef);
			}
		}

		pfree(newattno);

		/*
		 * Close the parent rel, but keep our AccessShareLock on it until
		 * xact commit.  That will prevent someone else from deleting or
		 * ALTERing the parent before the child is committed.
		 */
		heap_close(relation, NoLock);
	}

	/*
	 * If we had no inherited attributes, the result schema is just the
	 * explicitly declared columns.  Otherwise, we need to merge the
	 * declared columns into the inherited schema list.
	 */
	if (inhSchema != NIL)
	{
		foreach(entry, schema)
		{
			ColumnDef  *newdef = lfirst(entry);
			char	   *attributeName = newdef->colname;
			int			exist_attno;

			/*
			 * Does it conflict with some previously inherited column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				ColumnDef  *def;

				/*
				 * Yes, try to merge the two column definitions. They must
				 * have the same type and typmod.
				 */
				elog(NOTICE, "CREATE TABLE: merging attribute \"%s\" with inherited definition",
					 attributeName);
				def = (ColumnDef *) nth(exist_attno - 1, inhSchema);
				if (typenameTypeId(def->typename) != typenameTypeId(newdef->typename) ||
					def->typename->typmod != newdef->typename->typmod)
					elog(ERROR, "CREATE TABLE: attribute \"%s\" type conflict (%s and %s)",
						 attributeName,
						 TypeNameToString(def->typename),
						 TypeNameToString(newdef->typename));
				/* Merge of NOT NULL constraints = OR 'em together */
				def->is_not_null |= newdef->is_not_null;
				/* If new def has a default, override previous default */
				if (newdef->raw_default != NULL)
				{
					def->raw_default = newdef->raw_default;
					def->cooked_default = newdef->cooked_default;
				}
			}
			else
			{
				/*
				 * No, attach new column to result schema
				 */
				inhSchema = lappend(inhSchema, newdef);
			}
		}

		schema = inhSchema;
	}

	/*
	 * If we found any conflicting parent default values, check to make
	 * sure they were overridden by the child.
	 */
	if (have_bogus_defaults)
	{
		foreach(entry, schema)
		{
			ColumnDef  *def = lfirst(entry);

			if (def->cooked_default == bogus_marker)
				elog(ERROR, "CREATE TABLE: attribute \"%s\" inherits conflicting default values"
					 "\n\tTo resolve the conflict, specify a default explicitly",
					 def->colname);
		}
	}

	*supOids = parentOids;
	*supconstr = constraints;
	*supHasOids = parentHasOids;
	return schema;
}

/*
 * complementary static functions for MergeAttributes().
 *
 * Varattnos of pg_relcheck.rcbin must be rewritten when subclasses inherit
 * constraints from parent classes, since the inherited attributes could
 * be given different column numbers in multiple-inheritance cases.
 *
 * Note that the passed node tree is modified in place!
 */
static bool
change_varattnos_walker(Node *node, const AttrNumber *newattno)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0 && var->varno == 1 &&
			var->varattno > 0)
		{
			/*
			 * ??? the following may be a problem when the node is
			 * multiply referenced though stringToNode() doesn't create
			 * such a node currently.
			 */
			Assert(newattno[var->varattno - 1] > 0);
			var->varattno = newattno[var->varattno - 1];
		}
		return false;
	}
	return expression_tree_walker(node, change_varattnos_walker,
								  (void *) newattno);
}

static bool
change_varattnos_of_a_node(Node *node, const AttrNumber *newattno)
{
	return change_varattnos_walker(node, newattno);
}

/*
 * StoreCatalogInheritance
 *		Updates the system catalogs with proper inheritance information.
 *
 * supers is an integer list of the OIDs of the new relation's direct
 * ancestors.  NB: it is destructively changed to include indirect ancestors.
 */
static void
StoreCatalogInheritance(Oid relationId, List *supers)
{
	Relation	relation;
	TupleDesc	desc;
	int16		seqNumber;
	List	   *entry;
	HeapTuple	tuple;

	/*
	 * sanity checks
	 */
	AssertArg(OidIsValid(relationId));

	if (supers == NIL)
		return;

	/*
	 * Catalog INHERITS information using direct ancestors only.
	 */
	relation = heap_openr(InheritsRelationName, RowExclusiveLock);
	desc = RelationGetDescr(relation);

	seqNumber = 1;
	foreach(entry, supers)
	{
		Oid			entryOid = lfirsti(entry);
		Datum		datum[Natts_pg_inherits];
		char		nullarr[Natts_pg_inherits];

		datum[0] = ObjectIdGetDatum(relationId);		/* inhrel */
		datum[1] = ObjectIdGetDatum(entryOid);	/* inhparent */
		datum[2] = Int16GetDatum(seqNumber);	/* inhseqno */

		nullarr[0] = ' ';
		nullarr[1] = ' ';
		nullarr[2] = ' ';

		tuple = heap_formtuple(desc, datum, nullarr);

		heap_insert(relation, tuple);

		if (RelationGetForm(relation)->relhasindex)
		{
			Relation	idescs[Num_pg_inherits_indices];

			CatalogOpenIndices(Num_pg_inherits_indices, Name_pg_inherits_indices, idescs);
			CatalogIndexInsert(idescs, Num_pg_inherits_indices, relation, tuple);
			CatalogCloseIndices(Num_pg_inherits_indices, idescs);
		}

		heap_freetuple(tuple);

		seqNumber += 1;
	}

	heap_close(relation, RowExclusiveLock);

	/* ----------------
	 * Expand supers list to include indirect ancestors as well.
	 *
	 * Algorithm:
	 *	0. begin with list of direct superclasses.
	 *	1. append after each relationId, its superclasses, recursively.
	 *	2. remove all but last of duplicates.
	 * ----------------
	 */

	/*
	 * 1. append after each relationId, its superclasses, recursively.
	 */
	foreach(entry, supers)
	{
		HeapTuple	tuple;
		Oid			id;
		int16		number;
		List	   *next;
		List	   *current;

		id = (Oid) lfirsti(entry);
		current = entry;
		next = lnext(entry);

		for (number = 1;; number += 1)
		{
			tuple = SearchSysCache(INHRELID,
								   ObjectIdGetDatum(id),
								   Int16GetDatum(number),
								   0, 0);
			if (!HeapTupleIsValid(tuple))
				break;

			lnext(current) = lconsi(((Form_pg_inherits)
									 GETSTRUCT(tuple))->inhparent,
									NIL);

			ReleaseSysCache(tuple);

			current = lnext(current);
		}
		lnext(current) = next;
	}

	/*
	 * 2. remove all but last of duplicates.
	 */
	foreach(entry, supers)
	{
		Oid			thisone;
		bool		found;
		List	   *rest;

again:
		thisone = lfirsti(entry);
		found = false;
		foreach(rest, lnext(entry))
		{
			if (thisone == lfirsti(rest))
			{
				found = true;
				break;
			}
		}
		if (found)
		{
			/*
			 * found a later duplicate, so remove this entry.
			 */
			lfirsti(entry) = lfirsti(lnext(entry));
			lnext(entry) = lnext(lnext(entry));

			goto again;
		}
	}
}

/*
 * Look for an existing schema entry with the given name.
 *
 * Returns the index (starting with 1) if attribute already exists in schema,
 * 0 if it doesn't.
 */
static int
findAttrByName(const char *attributeName, List *schema)
{
	List	   *s;
	int			i = 0;

	foreach(s, schema)
	{
		ColumnDef  *def = lfirst(s);

		++i;
		if (strcmp(attributeName, def->colname) == 0)
			return i;
	}
	return 0;
}

/*
 * Update a relation's pg_class.relhassubclass entry to the given value
 */
static void
setRelhassubclassInRelation(Oid relationId, bool relhassubclass)
{
	Relation	relationRelation;
	HeapTuple	tuple;
	Relation	idescs[Num_pg_class_indices];

	/*
	 * Fetch a modifiable copy of the tuple, modify it, update pg_class.
	 */
	relationRelation = heap_openr(RelationRelationName, RowExclusiveLock);
	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(relationId),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "setRelhassubclassInRelation: cache lookup failed for relation %u", relationId);

	((Form_pg_class) GETSTRUCT(tuple))->relhassubclass = relhassubclass;
	simple_heap_update(relationRelation, &tuple->t_self, tuple);

	/* keep the catalog indices up to date */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
	CatalogIndexInsert(idescs, Num_pg_class_indices, relationRelation, tuple);
	CatalogCloseIndices(Num_pg_class_indices, idescs);

	heap_freetuple(tuple);
	heap_close(relationRelation, RowExclusiveLock);
}


/*
 *		renameatt		- changes the name of a attribute in a relation
 *
 *		Attname attribute is changed in attribute catalog.
 *		No record of the previous attname is kept (correct?).
 *
 *		get proper relrelation from relation catalog (if not arg)
 *		scan attribute catalog
 *				for name conflict (within rel)
 *				for original attribute (if not arg)
 *		modify attname in attribute tuple
 *		insert modified attribute in attribute catalog
 *		delete original attribute from attribute catalog
 */
void
renameatt(Oid relid,
		  const char *oldattname,
		  const char *newattname,
		  bool recurse)
{
	Relation	targetrelation;
	Relation	attrelation;
	HeapTuple	atttup;
	List	   *indexoidlist;
	List	   *indexoidscan;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	targetrelation = heap_open(relid, AccessExclusiveLock);

	/*
	 * permissions checking.  this would normally be done in utility.c,
	 * but this particular routine is recursive.
	 *
	 * normally, only the owner of a class can change its schema.
	 */
	if (!allowSystemTableMods 
		&& IsSystemRelation(targetrelation))
		elog(ERROR, "renameatt: class \"%s\" is a system catalog",
			 RelationGetRelationName(targetrelation));
	if (!pg_class_ownercheck(relid, GetUserId()))
		elog(ERROR, "renameatt: you do not own class \"%s\"",
			 RelationGetRelationName(targetrelation));

	/*
	 * if the 'recurse' flag is set then we are supposed to rename this
	 * attribute in all classes that inherit from 'relname' (as well as in
	 * 'relname').
	 *
	 * any permissions or problems with duplicate attributes will cause the
	 * whole transaction to abort, which is what we want -- all or
	 * nothing.
	 */
	if (recurse)
	{
		List	   *child,
				   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(relid);

		/*
		 * find_all_inheritors does the recursive search of the
		 * inheritance hierarchy, so all we have to do is process all of
		 * the relids in the list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirsti(child);

			if (childrelid == relid)
				continue;
			/* note we need not recurse again! */
			renameatt(childrelid, oldattname, newattname, false);
		}
	}

	attrelation = heap_openr(AttributeRelationName, RowExclusiveLock);

	atttup = SearchSysCacheCopy(ATTNAME,
								ObjectIdGetDatum(relid),
								PointerGetDatum(oldattname),
								0, 0);
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "renameatt: attribute \"%s\" does not exist", oldattname);

	if (((Form_pg_attribute) GETSTRUCT(atttup))->attnum < 0)
		elog(ERROR, "renameatt: system attribute \"%s\" not renamed", oldattname);

	/* should not already exist */
	if (SearchSysCacheExists(ATTNAME,
							 ObjectIdGetDatum(relid),
							 PointerGetDatum(newattname),
							 0, 0))
		elog(ERROR, "renameatt: attribute \"%s\" exists", newattname);

	StrNCpy(NameStr(((Form_pg_attribute) GETSTRUCT(atttup))->attname),
			newattname, NAMEDATALEN);

	simple_heap_update(attrelation, &atttup->t_self, atttup);

	/* keep system catalog indices current */
	{
		Relation	irelations[Num_pg_attr_indices];

		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, irelations);
		CatalogIndexInsert(irelations, Num_pg_attr_indices, attrelation, atttup);
		CatalogCloseIndices(Num_pg_attr_indices, irelations);
	}

	heap_freetuple(atttup);

	/*
	 * Update column names of indexes that refer to the column being
	 * renamed.
	 */
	indexoidlist = RelationGetIndexList(targetrelation);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirsti(indexoidscan);
		HeapTuple	indextup;

		/*
		 * First check to see if index is a functional index. If so, its
		 * column name is a function name and shouldn't be renamed here.
		 */
		indextup = SearchSysCache(INDEXRELID,
								  ObjectIdGetDatum(indexoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(indextup))
			elog(ERROR, "renameatt: can't find index id %u", indexoid);
		if (OidIsValid(((Form_pg_index) GETSTRUCT(indextup))->indproc))
		{
			ReleaseSysCache(indextup);
			continue;
		}
		ReleaseSysCache(indextup);

		/*
		 * Okay, look to see if any column name of the index matches the
		 * old attribute name.
		 */
		atttup = SearchSysCacheCopy(ATTNAME,
									ObjectIdGetDatum(indexoid),
									PointerGetDatum(oldattname),
									0, 0);
		if (!HeapTupleIsValid(atttup))
			continue;			/* Nope, so ignore it */

		/*
		 * Update the (copied) attribute tuple.
		 */
		StrNCpy(NameStr(((Form_pg_attribute) GETSTRUCT(atttup))->attname),
				newattname, NAMEDATALEN);

		simple_heap_update(attrelation, &atttup->t_self, atttup);

		/* keep system catalog indices current */
		{
			Relation	irelations[Num_pg_attr_indices];

			CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, irelations);
			CatalogIndexInsert(irelations, Num_pg_attr_indices, attrelation, atttup);
			CatalogCloseIndices(Num_pg_attr_indices, irelations);
		}
		heap_freetuple(atttup);
	}

	freeList(indexoidlist);

	heap_close(attrelation, RowExclusiveLock);

	/*
	 * Update att name in any RI triggers associated with the relation.
	 */
	if (targetrelation->rd_rel->reltriggers > 0)
	{
		/* update tgargs column reference where att is primary key */
		update_ri_trigger_args(RelationGetRelid(targetrelation),
							   oldattname, newattname,
							   false, false);
		/* update tgargs column reference where att is foreign key */
		update_ri_trigger_args(RelationGetRelid(targetrelation),
							   oldattname, newattname,
							   true, false);
	}

	heap_close(targetrelation, NoLock); /* close rel but keep lock! */
}

/*
 *		renamerel		- change the name of a relation
 *
 *		XXX - When renaming sequences, we don't bother to modify the
 *			  sequence name that is stored within the sequence itself
 *			  (this would cause problems with MVCC). In the future,
 *			  the sequence name should probably be removed from the
 *			  sequence, AFAIK there's no need for it to be there.
 */
void
renamerel(Oid relid, const char *newrelname)
{
	Relation	targetrelation;
	Relation	relrelation;	/* for RELATION relation */
	HeapTuple	reltup;
	Oid			namespaceId;
	char		relkind;
	bool		relhastriggers;
	Relation	irelations[Num_pg_class_indices];

	/*
	 * Grab an exclusive lock on the target table or index, which we will
	 * NOT release until end of transaction.
	 */
	targetrelation = relation_open(relid, AccessExclusiveLock);

	namespaceId = RelationGetNamespace(targetrelation);

	/* Validity checks */
	if (!allowSystemTableMods &&
		IsSystemRelation(targetrelation))
		elog(ERROR, "renamerel: system relation \"%s\" may not be renamed",
			 RelationGetRelationName(targetrelation));

	relkind = targetrelation->rd_rel->relkind;
	relhastriggers = (targetrelation->rd_rel->reltriggers > 0);

	/*
	 * Find relation's pg_class tuple, and make sure newrelname isn't in
	 * use.
	 */
	relrelation = heap_openr(RelationRelationName, RowExclusiveLock);

	reltup = SearchSysCacheCopy(RELOID,
								PointerGetDatum(relid),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "renamerel: relation \"%s\" does not exist",
			 RelationGetRelationName(targetrelation));

	if (get_relname_relid(newrelname, namespaceId) != InvalidOid)
		elog(ERROR, "renamerel: relation \"%s\" exists", newrelname);

	/*
	 * Update pg_class tuple with new relname.	(Scribbling on reltup is
	 * OK because it's a copy...)
	 */
	StrNCpy(NameStr(((Form_pg_class) GETSTRUCT(reltup))->relname),
			newrelname, NAMEDATALEN);

	simple_heap_update(relrelation, &reltup->t_self, reltup);

	/* keep the system catalog indices current */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, irelations);
	CatalogIndexInsert(irelations, Num_pg_class_indices, relrelation, reltup);
	CatalogCloseIndices(Num_pg_class_indices, irelations);

	heap_close(relrelation, NoLock);
	heap_freetuple(reltup);

	/*
	 * Also rename the associated type, if any.
	 */
	if (relkind != RELKIND_INDEX)
		TypeRename(RelationGetRelationName(targetrelation), namespaceId,
				   newrelname);

	/*
	 * If it's a view, must also rename the associated ON SELECT rule.
	 */
	if (relkind == RELKIND_VIEW)
	{
		char	   *oldrulename,
				   *newrulename;

		oldrulename = MakeRetrieveViewRuleName(RelationGetRelationName(targetrelation));
		newrulename = MakeRetrieveViewRuleName(newrelname);
		RenameRewriteRule(oldrulename, newrulename);
	}

	/*
	 * Update rel name in any RI triggers associated with the relation.
	 */
	if (relhastriggers)
	{
		/* update tgargs where relname is primary key */
		update_ri_trigger_args(relid,
							   RelationGetRelationName(targetrelation),
							   newrelname,
							   false, true);
		/* update tgargs where relname is foreign key */
		update_ri_trigger_args(relid,
							   RelationGetRelationName(targetrelation),
							   newrelname,
							   true, true);
	}

	/*
	 * Close rel, but keep exclusive lock!
	 */
	relation_close(targetrelation, NoLock);
}

/*
 * Given a trigger function OID, determine whether it is an RI trigger,
 * and if so whether it is attached to PK or FK relation.
 *
 * XXX this probably doesn't belong here; should be exported by
 * ri_triggers.c
 */
static int
ri_trigger_type(Oid tgfoid)
{
	switch (tgfoid)
	{
		case F_RI_FKEY_CASCADE_DEL:
		case F_RI_FKEY_CASCADE_UPD:
		case F_RI_FKEY_RESTRICT_DEL:
		case F_RI_FKEY_RESTRICT_UPD:
		case F_RI_FKEY_SETNULL_DEL:
		case F_RI_FKEY_SETNULL_UPD:
		case F_RI_FKEY_SETDEFAULT_DEL:
		case F_RI_FKEY_SETDEFAULT_UPD:
		case F_RI_FKEY_NOACTION_DEL:
		case F_RI_FKEY_NOACTION_UPD:
			return RI_TRIGGER_PK;

		case F_RI_FKEY_CHECK_INS:
		case F_RI_FKEY_CHECK_UPD:
			return RI_TRIGGER_FK;
	}

	return RI_TRIGGER_NONE;
}

/*
 * Scan pg_trigger for RI triggers that are on the specified relation
 * (if fk_scan is false) or have it as the tgconstrrel (if fk_scan
 * is true).  Update RI trigger args fields matching oldname to contain
 * newname instead.  If update_relname is true, examine the relname
 * fields; otherwise examine the attname fields.
 */
static void
update_ri_trigger_args(Oid relid,
					   const char *oldname,
					   const char *newname,
					   bool fk_scan,
					   bool update_relname)
{
	Relation	tgrel;
	Relation	irel;
	ScanKeyData skey[1];
	IndexScanDesc idxtgscan;
	RetrieveIndexResult idxres;
	Datum		values[Natts_pg_trigger];
	char		nulls[Natts_pg_trigger];
	char		replaces[Natts_pg_trigger];

	tgrel = heap_openr(TriggerRelationName, RowExclusiveLock);
	if (fk_scan)
		irel = index_openr(TriggerConstrRelidIndex);
	else
		irel = index_openr(TriggerRelidIndex);

	ScanKeyEntryInitialize(&skey[0], 0x0,
						   1,	/* always column 1 of index */
						   F_OIDEQ,
						   ObjectIdGetDatum(relid));
	idxtgscan = index_beginscan(irel, false, 1, skey);

	while ((idxres = index_getnext(idxtgscan, ForwardScanDirection)) != NULL)
	{
		HeapTupleData tupledata;
		Buffer		buffer;
		HeapTuple	tuple;
		Form_pg_trigger pg_trigger;
		bytea	   *val;
		bytea	   *newtgargs;
		bool		isnull;
		int			tg_type;
		bool		examine_pk;
		bool		changed;
		int			tgnargs;
		int			i;
		int			newlen;
		const char *arga[RI_MAX_ARGUMENTS];
		const char *argp;

		tupledata.t_self = idxres->heap_iptr;
		heap_fetch(tgrel, SnapshotNow, &tupledata, &buffer, idxtgscan);
		pfree(idxres);
		if (!tupledata.t_data)
			continue;
		tuple = &tupledata;
		pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);
		tg_type = ri_trigger_type(pg_trigger->tgfoid);
		if (tg_type == RI_TRIGGER_NONE)
		{
			/* Not an RI trigger, forget it */
			ReleaseBuffer(buffer);
			continue;
		}

		/*
		 * It is an RI trigger, so parse the tgargs bytea.
		 *
		 * NB: we assume the field will never be compressed or moved out of
		 * line; so does trigger.c ...
		 */
		tgnargs = pg_trigger->tgnargs;
		val = (bytea *) fastgetattr(tuple,
									Anum_pg_trigger_tgargs,
									tgrel->rd_att, &isnull);
		if (isnull || tgnargs < RI_FIRST_ATTNAME_ARGNO ||
			tgnargs > RI_MAX_ARGUMENTS)
		{
			/* This probably shouldn't happen, but ignore busted triggers */
			ReleaseBuffer(buffer);
			continue;
		}
		argp = (const char *) VARDATA(val);
		for (i = 0; i < tgnargs; i++)
		{
			arga[i] = argp;
			argp += strlen(argp) + 1;
		}

		/*
		 * Figure out which item(s) to look at.  If the trigger is
		 * primary-key type and attached to my rel, I should look at the
		 * PK fields; if it is foreign-key type and attached to my rel, I
		 * should look at the FK fields.  But the opposite rule holds when
		 * examining triggers found by tgconstrrel search.
		 */
		examine_pk = (tg_type == RI_TRIGGER_PK) == (!fk_scan);

		changed = false;
		if (update_relname)
		{
			/* Change the relname if needed */
			i = examine_pk ? RI_PK_RELNAME_ARGNO : RI_FK_RELNAME_ARGNO;
			if (strcmp(arga[i], oldname) == 0)
			{
				arga[i] = newname;
				changed = true;
			}
		}
		else
		{
			/* Change attname(s) if needed */
			i = examine_pk ? RI_FIRST_ATTNAME_ARGNO + RI_KEYPAIR_PK_IDX :
				RI_FIRST_ATTNAME_ARGNO + RI_KEYPAIR_FK_IDX;
			for (; i < tgnargs; i += 2)
			{
				if (strcmp(arga[i], oldname) == 0)
				{
					arga[i] = newname;
					changed = true;
				}
			}
		}

		if (!changed)
		{
			/* Don't need to update this tuple */
			ReleaseBuffer(buffer);
			continue;
		}

		/*
		 * Construct modified tgargs bytea.
		 */
		newlen = VARHDRSZ;
		for (i = 0; i < tgnargs; i++)
			newlen += strlen(arga[i]) + 1;
		newtgargs = (bytea *) palloc(newlen);
		VARATT_SIZEP(newtgargs) = newlen;
		newlen = VARHDRSZ;
		for (i = 0; i < tgnargs; i++)
		{
			strcpy(((char *) newtgargs) + newlen, arga[i]);
			newlen += strlen(arga[i]) + 1;
		}

		/*
		 * Build modified tuple.
		 */
		for (i = 0; i < Natts_pg_trigger; i++)
		{
			values[i] = (Datum) 0;
			replaces[i] = ' ';
			nulls[i] = ' ';
		}
		values[Anum_pg_trigger_tgargs - 1] = PointerGetDatum(newtgargs);
		replaces[Anum_pg_trigger_tgargs - 1] = 'r';

		tuple = heap_modifytuple(tuple, tgrel, values, nulls, replaces);

		/*
		 * Now we can release hold on original tuple.
		 */
		ReleaseBuffer(buffer);

		/*
		 * Update pg_trigger and its indexes
		 */
		simple_heap_update(tgrel, &tuple->t_self, tuple);

		{
			Relation	irelations[Num_pg_attr_indices];

			CatalogOpenIndices(Num_pg_trigger_indices, Name_pg_trigger_indices, irelations);
			CatalogIndexInsert(irelations, Num_pg_trigger_indices, tgrel, tuple);
			CatalogCloseIndices(Num_pg_trigger_indices, irelations);
		}

		/* free up our scratch memory */
		pfree(newtgargs);
		heap_freetuple(tuple);
	}

	index_endscan(idxtgscan);
	index_close(irel);

	heap_close(tgrel, RowExclusiveLock);

	/*
	 * Increment cmd counter to make updates visible; this is needed in
	 * case the same tuple has to be updated again by next pass (can
	 * happen in case of a self-referential FK relationship).
	 */
	CommandCounterIncrement();
}
