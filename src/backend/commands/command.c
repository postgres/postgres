/*-------------------------------------------------------------------------
 *
 * command.c
 *	  random postgres portal and utility support code
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/command.c,v 1.153 2002/02/14 15:24:06 tgl Exp $
 *
 * NOTES
 *	  The PerformAddAttribute() code, like most of the relation
 *	  manipulating code in the commands/ directory, should go
 *	  someplace closer to the lib/catalog code.
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
#include "catalog/pg_attrdef.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "commands/command.h"
#include "commands/trigger.h"
#include "commands/defrem.h"	/* For add constraint unique, primary */
#include "executor/execdefs.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "parser/parse.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/analyze.h"		/* For add constraint unique, primary */
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/relcache.h"
#include "utils/temprel.h"


static void drop_default(Oid relid, int16 attnum);
static bool needs_toast_table(Relation rel);


/* --------------------------------
 *		PortalCleanup
 * --------------------------------
 */
void
PortalCleanup(Portal portal)
{
	MemoryContext oldcontext;

	/*
	 * sanity checks
	 */
	AssertArg(PortalIsValid(portal));
	AssertArg(portal->cleanup == PortalCleanup);

	/*
	 * set proper portal-executor context before calling ExecMain.
	 */
	oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	/*
	 * tell the executor to shutdown the query
	 */
	ExecutorEnd(PortalGetQueryDesc(portal), PortalGetState(portal));

	/*
	 * switch back to previous context
	 */
	MemoryContextSwitchTo(oldcontext);
}

/* --------------------------------
 *		PerformPortalFetch
 * --------------------------------
 */
void
PerformPortalFetch(char *name,
				   bool forward,
				   int count,
				   char *tag,
				   CommandDest dest)
{
	Portal		portal;
	QueryDesc  *queryDesc;
	EState	   *estate;
	MemoryContext oldcontext;
	CommandId	savedId;
	bool		temp_desc = false;

	/*
	 * sanity checks
	 */
	if (name == NULL)
	{
		elog(NOTICE, "PerformPortalFetch: missing portal name");
		return;
	}

	/*
	 * get the portal from the portal name
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		elog(NOTICE, "PerformPortalFetch: portal \"%s\" not found",
			 name);
		return;
	}

	/*
	 * switch into the portal context
	 */
	oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	/*
	 * If the requested destination is not the same as the query's
	 * original destination, make a temporary QueryDesc with the proper
	 * destination.  This supports MOVE, for example, which will pass in
	 * dest = None.
	 *
	 * EXCEPTION: if the query's original dest is RemoteInternal (ie, it's a
	 * binary cursor) and the request is Remote, we do NOT override the
	 * original dest.  This is necessary since a FETCH command will pass
	 * dest = Remote, not knowing whether the cursor is binary or not.
	 */
	queryDesc = PortalGetQueryDesc(portal);
	estate = PortalGetState(portal);

	if (dest != queryDesc->dest &&
		!(queryDesc->dest == RemoteInternal && dest == Remote))
	{
		QueryDesc  *qdesc = (QueryDesc *) palloc(sizeof(QueryDesc));

		memcpy(qdesc, queryDesc, sizeof(QueryDesc));
		qdesc->dest = dest;
		queryDesc = qdesc;
		temp_desc = true;
	}

	/*
	 * Tell the destination to prepare to receive some tuples.
	 */
	BeginCommand(name,
				 queryDesc->operation,
				 PortalGetTupleDesc(portal),
				 false,			/* portal fetches don't end up in
								 * relations */
				 false,			/* this is a portal fetch, not a "retrieve
								 * portal" */
				 tag,
				 queryDesc->dest);

	/*
	 * Restore the scanCommandId that was current when the cursor was
	 * opened.  This ensures that we see the same tuples throughout the
	 * execution of the cursor.
	 */
	savedId = GetScanCommandId();
	SetScanCommandId(PortalGetCommandId(portal));

	/*
	 * Determine which direction to go in, and check to see if we're
	 * already at the end of the available tuples in that direction.  If
	 * so, do nothing.	(This check exists because not all plan node types
	 * are robust about being called again if they've already returned
	 * NULL once.)	If it's OK to do the fetch, call the executor.  Then,
	 * update the atStart/atEnd state depending on the number of tuples
	 * that were retrieved.
	 */
	if (forward)
	{
		if (!portal->atEnd)
		{
			ExecutorRun(queryDesc, estate, EXEC_FOR, (long) count);

			/*
			 * I use CMD_UPDATE, because no CMD_MOVE or the like exists,
			 * and I would like to provide the same kind of info as
			 * CMD_UPDATE
			 */
			UpdateCommandInfo(CMD_UPDATE, 0, estate->es_processed);
			if (estate->es_processed > 0)
				portal->atStart = false;		/* OK to back up now */
			if (count <= 0 || (int) estate->es_processed < count)
				portal->atEnd = true;	/* we retrieved 'em all */
		}
	}
	else
	{
		if (!portal->atStart)
		{
			ExecutorRun(queryDesc, estate, EXEC_BACK, (long) count);

			/*
			 * I use CMD_UPDATE, because no CMD_MOVE or the like exists,
			 * and I would like to provide the same kind of info as
			 * CMD_UPDATE
			 */
			UpdateCommandInfo(CMD_UPDATE, 0, estate->es_processed);
			if (estate->es_processed > 0)
				portal->atEnd = false;	/* OK to go forward now */
			if (count <= 0 || (int) estate->es_processed < count)
				portal->atStart = true; /* we retrieved 'em all */
		}
	}

	/*
	 * Restore outer command ID.
	 */
	SetScanCommandId(savedId);

	/*
	 * Clean up and switch back to old context.
	 */
	if (temp_desc)
		pfree(queryDesc);

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Note: the "end-of-command" tag is returned by higher-level utility
	 * code
	 */
}

/* --------------------------------
 *		PerformPortalClose
 * --------------------------------
 */
void
PerformPortalClose(char *name, CommandDest dest)
{
	Portal		portal;

	/*
	 * sanity checks
	 */
	if (name == NULL)
	{
		elog(NOTICE, "PerformPortalClose: missing portal name");
		return;
	}

	/*
	 * get the portal from the portal name
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		elog(NOTICE, "PerformPortalClose: portal \"%s\" not found",
			 name);
		return;
	}

	/*
	 * Note: PortalCleanup is called as a side-effect
	 */
	PortalDrop(portal);
}

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
AlterTableAddColumn(const char *relationName,
					bool inherits,
					ColumnDef *colDef)
{
	Relation	rel,
				attrdesc;
	Oid			myrelid;
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
	char	   *typename;
	int			attndims;

	/*
	 * permissions checking.  this would normally be done in utility.c,
	 * but this particular routine is recursive.
	 *
	 * normally, only the owner of a class can change its schema.
	 */
	if (!allowSystemTableMods && IsSystemRelationName(relationName))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 relationName);
	if (!pg_ownercheck(GetUserId(), relationName, RELNAME))
		elog(ERROR, "ALTER TABLE: permission denied");

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	rel = heap_openr(relationName, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 relationName);

	myrelid = RelationGetRelid(rel);
	heap_close(rel, NoLock);	/* close rel but keep lock! */

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
			char	   *childrelname;

			if (childrelid == myrelid)
				continue;
			rel = heap_open(childrelid, AccessExclusiveLock);
			childrelname = pstrdup(RelationGetRelationName(rel));
			heap_close(rel, AccessExclusiveLock);

			AlterTableAddColumn(childrelname, false, colDef);

			pfree(childrelname);
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
			 "\n\tAdd the column, then use ALTER TABLE ADD CONSTRAINT.");


	rel = heap_openr(RelationRelationName, RowExclusiveLock);

	reltup = SearchSysCache(RELNAME,
							PointerGetDatum(relationName),
							0, 0, 0);

	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "ALTER TABLE: relation \"%s\" not found",
			 relationName);

	if (SearchSysCacheExists(ATTNAME,
							 ObjectIdGetDatum(reltup->t_data->t_oid),
							 PointerGetDatum(colDef->colname),
							 0, 0))
		elog(ERROR, "ALTER TABLE: column name \"%s\" already exists in table \"%s\"",
			 colDef->colname, relationName);

	minattnum = ((Form_pg_class) GETSTRUCT(reltup))->relnatts;
	maxatts = minattnum + 1;
	if (maxatts > MaxHeapAttributeNumber)
		elog(ERROR, "ALTER TABLE: relations limited to %d columns",
			 MaxHeapAttributeNumber);
	i = minattnum + 1;

	attrdesc = heap_openr(AttributeRelationName, RowExclusiveLock);

	if (colDef->typename->arrayBounds)
	{
		attndims = length(colDef->typename->arrayBounds);
		typename = makeArrayTypeName(colDef->typename->name);
	}
	else
	{
		attndims = 0;
		typename = colDef->typename->name;
	}

	typeTuple = SearchSysCache(TYPENAME,
							   PointerGetDatum(typename),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "ALTER TABLE: type \"%s\" does not exist", typename);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);

	attributeTuple = heap_addheader(Natts_pg_attribute,
									ATTRIBUTE_TUPLE_SIZE,
									(void *) &attributeD);

	attribute = (Form_pg_attribute) GETSTRUCT(attributeTuple);

	attribute->attrelid = reltup->t_data->t_oid;
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

	heap_close(attrdesc, NoLock);

	/*
	 * Update number of attributes in pg_class tuple
	 */
	newreltup = heap_copytuple(reltup);

	((Form_pg_class) GETSTRUCT(newreltup))->relnatts = maxatts;
	simple_heap_update(rel, &newreltup->t_self, newreltup);

	/* keep catalog indices current */
	if (RelationGetForm(rel)->relhasindex)
	{
		Relation	ridescs[Num_pg_class_indices];

		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
		CatalogIndexInsert(ridescs, Num_pg_class_indices, rel, newreltup);
		CatalogCloseIndices(Num_pg_class_indices, ridescs);
	}

	heap_freetuple(newreltup);
	ReleaseSysCache(reltup);

	heap_close(rel, NoLock);

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
		rel = heap_openr(relationName, AccessExclusiveLock);
		AddRelationRawConstraints(rel, NIL, colDef->constraints);
		heap_close(rel, NoLock);
	}

	/*
	 * Automatically create the secondary relation for TOAST if it
	 * formerly had no such but now has toastable attributes.
	 */
	AlterTableCreateToastTable(relationName, true);
}


/*
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT
 */
void
AlterTableAlterColumnDefault(const char *relationName,
							 bool inh, const char *colName,
							 Node *newDefault)
{
	Relation	rel;
	HeapTuple	tuple;
	int16		attnum;
	Oid			myrelid;

	if (!allowSystemTableMods && IsSystemRelationName(relationName))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 relationName);
#ifndef NO_SECURITY
	if (!pg_ownercheck(GetUserId(), relationName, RELNAME))
		elog(ERROR, "ALTER TABLE: permission denied");
#endif

	rel = heap_openr(relationName, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 relationName);

	myrelid = RelationGetRelid(rel);
	heap_close(rel, NoLock);

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
			rel = heap_open(childrelid, AccessExclusiveLock);
			AlterTableAlterColumnDefault(RelationGetRelationName(rel),
										 false, colName, newDefault);
			heap_close(rel, AccessExclusiveLock);
		}
	}

	/* -= now do the thing on this relation =- */

	/* reopen the business */
	rel = heap_openr(relationName, AccessExclusiveLock);

	/*
	 * get the number of the attribute
	 */
	tuple = SearchSysCache(ATTNAME,
						   ObjectIdGetDatum(myrelid),
						   PointerGetDatum(colName),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ALTER TABLE: relation \"%s\" has no column \"%s\"",
			 relationName, colName);

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
		ScanKeyData scankeys[3];
		HeapScanDesc scan;

		attr_rel = heap_openr(AttributeRelationName, RowExclusiveLock);
		ScanKeyEntryInitialize(&scankeys[0], 0x0,
							   Anum_pg_attribute_attrelid, F_OIDEQ,
							   ObjectIdGetDatum(myrelid));
		ScanKeyEntryInitialize(&scankeys[1], 0x0,
							   Anum_pg_attribute_attnum, F_INT2EQ,
							   Int16GetDatum(attnum));
		ScanKeyEntryInitialize(&scankeys[2], 0x0,
							   Anum_pg_attribute_atthasdef, F_BOOLEQ,
							   BoolGetDatum(true));

		scan = heap_beginscan(attr_rel, false, SnapshotNow, 3, scankeys);
		AssertState(scan != NULL);

		if (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
		{
			HeapTuple	newtuple;
			Relation	irelations[Num_pg_attr_indices];

			/* update to false */
			newtuple = heap_copytuple(tuple);
			((Form_pg_attribute) GETSTRUCT(newtuple))->atthasdef = FALSE;
			simple_heap_update(attr_rel, &tuple->t_self, newtuple);

			/* keep the system catalog indices current */
			CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, irelations);
			CatalogIndexInsert(irelations, Num_pg_attr_indices, attr_rel, newtuple);
			CatalogCloseIndices(Num_pg_attr_indices, irelations);

			/* get rid of actual default definition */
			drop_default(myrelid, attnum);
		}

		heap_endscan(scan);
		heap_close(attr_rel, NoLock);
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
 * ALTER TABLE ALTER COLUMN SET STATISTICS
 */
void
AlterTableAlterColumnStatistics(const char *relationName,
								bool inh, const char *colName,
								Node *statsTarget)
{
	Relation	rel;
	Oid			myrelid;
	int			newtarget;
	Relation	attrelation;
	HeapTuple	tuple;

	/* we allow this on system tables */
#ifndef NO_SECURITY
	if (!pg_ownercheck(GetUserId(), relationName, RELNAME))
		elog(ERROR, "ALTER TABLE: permission denied");
#endif

	rel = heap_openr(relationName, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 relationName);

	myrelid = RelationGetRelid(rel);
	heap_close(rel, NoLock);	/* close rel, but keep lock! */

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
			rel = heap_open(childrelid, AccessExclusiveLock);
			AlterTableAlterColumnStatistics(RelationGetRelationName(rel),
											false, colName, statsTarget);
			heap_close(rel, AccessExclusiveLock);
		}
	}

	/* -= now do the thing on this relation =- */

	Assert(IsA(statsTarget, Integer));
	newtarget = intVal(statsTarget);

	/* Limit target to sane range (should we raise an error instead?) */
	if (newtarget < 0)
		newtarget = 0;
	else if (newtarget > 1000)
		newtarget = 1000;

	attrelation = heap_openr(AttributeRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(ATTNAME,
							   ObjectIdGetDatum(myrelid),
							   PointerGetDatum(colName),
							   0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ALTER TABLE: relation \"%s\" has no column \"%s\"",
			 relationName, colName);

	if (((Form_pg_attribute) GETSTRUCT(tuple))->attnum < 0)
		elog(ERROR, "ALTER TABLE: cannot change system attribute \"%s\"",
			 colName);

	((Form_pg_attribute) GETSTRUCT(tuple))->attstattarget = newtarget;

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
}


#ifdef	_DROP_COLUMN_HACK__
/*
 *	ALTER TABLE DROP COLUMN trial implementation
 *
 */

/*
 *	system table scan(index scan/sequential scan)
 */
typedef struct SysScanDescData
{
	Relation	heap_rel;
	Relation	irel;
	HeapScanDesc scan;
	IndexScanDesc iscan;
	HeapTupleData tuple;
	Buffer		buffer;
}	SysScanDescData, *SysScanDesc;

static void *
systable_beginscan(Relation rel, const char *indexRelname, int nkeys, ScanKey entry)
{
	bool		hasindex = (rel->rd_rel->relhasindex && !IsIgnoringSystemIndexes());
	SysScanDesc sysscan;

	sysscan = (SysScanDesc) palloc(sizeof(SysScanDescData));
	sysscan->heap_rel = rel;
	sysscan->irel = (Relation) NULL;
	sysscan->tuple.t_datamcxt = NULL;
	sysscan->tuple.t_data = NULL;
	sysscan->buffer = InvalidBuffer;
	if (hasindex)
	{
		sysscan->irel = index_openr((char *) indexRelname);
		sysscan->iscan = index_beginscan(sysscan->irel, false, nkeys, entry);
	}
	else
		sysscan->scan = heap_beginscan(rel, false, SnapshotNow, nkeys, entry);
	return (void *) sysscan;
}

static void
systable_endscan(void *scan)
{
	SysScanDesc sysscan = (SysScanDesc) scan;

	if (sysscan->irel)
	{
		if (BufferIsValid(sysscan->buffer))
			ReleaseBuffer(sysscan->buffer);
		index_endscan(sysscan->iscan);
		index_close(sysscan->irel);
	}
	else
		heap_endscan(sysscan->scan);
	pfree(scan);
}

static HeapTuple
systable_getnext(void *scan)
{
	SysScanDesc sysscan = (SysScanDesc) scan;
	HeapTuple	htup = (HeapTuple) NULL;
	RetrieveIndexResult indexRes;

	if (sysscan->irel)
	{
		if (BufferIsValid(sysscan->buffer))
		{
			ReleaseBuffer(sysscan->buffer);
			sysscan->buffer = InvalidBuffer;
		}
		while (indexRes = index_getnext(sysscan->iscan, ForwardScanDirection), indexRes != NULL)
		{
			sysscan->tuple.t_self = indexRes->heap_iptr;
			heap_fetch(sysscan->heap_rel, SnapshotNow, &sysscan->tuple, &(sysscan->buffer));
			pfree(indexRes);
			if (sysscan->tuple.t_data != NULL)
			{
				htup = &sysscan->tuple;
				break;
			}
		}
	}
	else
		htup = heap_getnext(sysscan->scan, 0);
	return htup;
}

/*
 *	find a specified attribute in a node entry
 */
static bool
find_attribute_walker(Node *node, int *attnump)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0 && var->varno == 1 &&
			var->varattno == *attnump)
			return true;
	}
	return expression_tree_walker(node, find_attribute_walker,
								  (void *) attnump);
}

static bool
find_attribute_in_node(Node *node, int attnum)
{
	return find_attribute_walker(node, &attnum);
}

/*
 *	Remove/check references for the column
 */
static bool
RemoveColumnReferences(Oid reloid, int attnum, bool checkonly, HeapTuple reltup)
{
	Relation	indexRelation,
				rcrel;
	ScanKeyData entry;
	HeapScanDesc scan;
	void	   *sysscan;
	HeapTuple	htup,
				indexTuple;
	Form_pg_index index;
	Form_pg_class pgcform = (Form_pg_class) NULL;
	int			i;
	bool		checkok = true;


	if (!checkonly)
		pgcform = (Form_pg_class) GETSTRUCT(reltup);

	/*
	 * Remove/check constraints here
	 */
	ScanKeyEntryInitialize(&entry, (bits16) 0x0, Anum_pg_relcheck_rcrelid,
					   (RegProcedure) F_OIDEQ, ObjectIdGetDatum(reloid));
	rcrel = heap_openr(RelCheckRelationName, RowExclusiveLock);
	sysscan = systable_beginscan(rcrel, RelCheckIndex, 1, &entry);

	while (HeapTupleIsValid(htup = systable_getnext(sysscan)))
	{
		Form_pg_relcheck relcheck;
		char	   *ccbin;
		Node	   *node;

		relcheck = (Form_pg_relcheck) GETSTRUCT(htup);
		ccbin = DatumGetCString(DirectFunctionCall1(textout,
									 PointerGetDatum(&relcheck->rcbin)));
		node = stringToNode(ccbin);
		pfree(ccbin);
		if (find_attribute_in_node(node, attnum))
		{
			if (checkonly)
			{
				checkok = false;
				elog(ERROR, "target column is used in a constraint");
			}
			else
			{
				simple_heap_delete(rcrel, &htup->t_self);
				pgcform->relchecks--;
			}
		}
	}
	systable_endscan(sysscan);
	heap_close(rcrel, NoLock);

	/*
	 * What to do with triggers/rules/views/procedues ?
	 */

	/*
	 * Remove/check indexes
	 */
	indexRelation = heap_openr(IndexRelationName, RowExclusiveLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_index_indrelid, F_OIDEQ,
						   ObjectIdGetDatum(reloid));
	scan = heap_beginscan(indexRelation, false, SnapshotNow, 1, &entry);
	while (HeapTupleIsValid(indexTuple = heap_getnext(scan, 0)))
	{
		index = (Form_pg_index) GETSTRUCT(indexTuple);
		for (i = 0; i < INDEX_MAX_KEYS; i++)
		{
			if (index->indkey[i] == InvalidAttrNumber)
				break;
			else if (index->indkey[i] == attnum)
			{
				if (checkonly)
				{
					checkok = false;
					elog(ERROR, "target column is used in an index");
				}
				else
				{
					htup = SearchSysCache(RELOID,
									 ObjectIdGetDatum(index->indexrelid),
										  0, 0, 0);
					RemoveIndex(NameStr(((Form_pg_class) GETSTRUCT(htup))->relname));
					ReleaseSysCache(htup);
				}
				break;
			}
		}
	}
	heap_endscan(scan);
	heap_close(indexRelation, NoLock);

	return checkok;
}
#endif   /* _DROP_COLUMN_HACK__ */

/*
 * ALTER TABLE DROP COLUMN
 */
void
AlterTableDropColumn(const char *relationName,
					 bool inh, const char *colName,
					 int behavior)
{
#ifdef	_DROP_COLUMN_HACK__
	Relation	rel,
				attrdesc;
	Oid			myrelid;
	HeapTuple	reltup;
	HeapTupleData classtuple;
	Buffer		buffer;
	Form_pg_attribute attribute;
	HeapTuple	tup;
	Relation	idescs[Num_pg_attr_indices];
	int			attnum;
	bool		hasindex;
	char		dropColname[32];

	if (inh)
		elog(ERROR, "ALTER TABLE / DROP COLUMN with inherit option is not supported yet");

	/*
	 * permissions checking.  this would normally be done in utility.c,
	 * but this particular routine is recursive.
	 *
	 * normally, only the owner of a class can change its schema.
	 */
	if (!allowSystemTableMods && IsSystemRelationName(relationName))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 relationName);
#ifndef NO_SECURITY
	if (!pg_ownercheck(GetUserId(), relationName, RELNAME))
		elog(ERROR, "ALTER TABLE: permission denied");
#endif

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	rel = heap_openr(relationName, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 relationName);

	myrelid = RelationGetRelid(rel);
	heap_close(rel, NoLock);	/* close rel but keep lock! */

	/*
	 * What to do when rel has inheritors ?
	 */
	if (length(find_all_inheritors(myrelid)) > 1)
		elog(ERROR, "ALTER TABLE: cannot drop a column on table that is inherited from");

	/*
	 * lock the pg_class tuple for update
	 */
	rel = heap_openr(RelationRelationName, RowExclusiveLock);
	reltup = SearchSysCache(RELNAME,
							PointerGetDatum(relationName),
							0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "ALTER TABLE: relation \"%s\" not found",
			 relationName);
	classtuple.t_self = reltup->t_self;
	ReleaseSysCache(reltup);

	switch (heap_mark4update(rel, &classtuple, &buffer))
	{
		case HeapTupleSelfUpdated:
		case HeapTupleMayBeUpdated:
			break;
		default:
			elog(ERROR, "couldn't lock pg_class tuple");
	}
	reltup = heap_copytuple(&classtuple);
	ReleaseBuffer(buffer);

	attrdesc = heap_openr(AttributeRelationName, RowExclusiveLock);

	/*
	 * Get the target pg_attribute tuple and make a modifiable copy
	 */
	tup = SearchSysCacheCopy(ATTNAME,
							 ObjectIdGetDatum(reltup->t_data->t_oid),
							 PointerGetDatum(colName),
							 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "ALTER TABLE: column name \"%s\" doesn't exist in table \"%s\"",
			 colName, relationName);

	attribute = (Form_pg_attribute) GETSTRUCT(tup);
	attnum = attribute->attnum;
	if (attnum <= 0)
		elog(ERROR, "ALTER TABLE: column name \"%s\" was already dropped",
			 colName);

	/*
	 * Check constraints/indices etc here
	 */
	if (behavior != CASCADE)
	{
		if (!RemoveColumnReferences(myrelid, attnum, true, NULL))
			elog(ERROR, "the column is referenced");
	}

	/*
	 * change the target pg_attribute tuple
	 */
	sprintf(dropColname, "*already Dropped*%d", attnum);
	namestrcpy(&(attribute->attname), dropColname);
	ATTRIBUTE_DROP_COLUMN(attribute);

	simple_heap_update(attrdesc, &tup->t_self, tup);
	hasindex = (!IsIgnoringSystemIndexes() && RelationGetForm(attrdesc)->relhasindex);
	if (hasindex)
	{
		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_attr_indices,
						   attrdesc, tup);
		CatalogCloseIndices(Num_pg_attr_indices, idescs);
	}
	heap_close(attrdesc, NoLock);
	heap_freetuple(tup);

	/* delete comment for this attribute only */
	CreateComments(RelationGetRelid(rel), RelOid_pg_class,
				   (int32) attnum, NULL);

	/* delete attrdef */
	drop_default(myrelid, attnum);

	/*
	 * Remove objects which reference this column
	 */
	if (behavior == CASCADE)
	{
		Relation	ridescs[Num_pg_class_indices];

		RemoveColumnReferences(myrelid, attnum, false, reltup);
		/* update pg_class tuple */
		simple_heap_update(rel, &reltup->t_self, reltup);
		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
		CatalogIndexInsert(ridescs, Num_pg_class_indices, rel, reltup);
		CatalogCloseIndices(Num_pg_class_indices, ridescs);
	}

	heap_freetuple(reltup);
	heap_close(rel, NoLock);
#else
	elog(ERROR, "ALTER TABLE / DROP COLUMN is not implemented");
#endif   /* _DROP_COLUMN_HACK__ */
}



/*
 * ALTER TABLE ADD CONSTRAINT
 */
void
AlterTableAddConstraint(char *relationName,
						bool inh, List *newConstraints)
{
	Relation	rel;
	Oid			myrelid;
	List	   *listptr;

	if (!allowSystemTableMods && IsSystemRelationName(relationName))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 relationName);
#ifndef NO_SECURITY
	if (!pg_ownercheck(GetUserId(), relationName, RELNAME))
		elog(ERROR, "ALTER TABLE: permission denied");
#endif

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	rel = heap_openr(relationName, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 relationName);

	myrelid = RelationGetRelid(rel);

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
			char	   *childrelname;
			Relation	childrel;

			if (childrelid == myrelid)
				continue;
			childrel = heap_open(childrelid, AccessExclusiveLock);
			childrelname = pstrdup(RelationGetRelationName(childrel));
			heap_close(childrel, AccessExclusiveLock);
			AlterTableAddConstraint(childrelname, false, newConstraints);
			pfree(childrelname);
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
								rte = addRangeTableEntry(pstate, relationName, NULL,
														 false, true);
								addRTEtoQuery(pstate, rte, true, true);

								/*
								 * Convert the A_EXPR in raw_expr into an
								 * EXPR
								 */
								expr = transformExpr(pstate, constr->raw_expr,
													 EXPR_COLUMN_FIRST);

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
										 relationName);

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

					if (is_temp_rel_name(fkconstraint->pktable_name) &&
						!is_temp_rel_name(relationName))
						elog(ERROR, "ALTER TABLE / ADD CONSTRAINT: Unable to reference temporary table from permanent table constraint.");

					/*
					 * Grab an exclusive lock on the pk table, so that
					 * someone doesn't delete rows out from under us.
					 */

					pkrel = heap_openr(fkconstraint->pktable_name, AccessExclusiveLock);
					if (pkrel->rd_rel->relkind != RELKIND_RELATION)
						elog(ERROR, "referenced table \"%s\" not a relation",
							 fkconstraint->pktable_name);
					heap_close(pkrel, NoLock);

					/*
					 * First we check for limited correctness of the
					 * constraint.
					 *
					 * NOTE: we assume parser has already checked for
					 * existence of an appropriate unique index on the
					 * referenced relation, and that the column datatypes
					 * are comparable.
					 *
					 * Scan through each tuple, calling the RI_FKey_Match_Ins
					 * (insert trigger) as if that tuple had just been
					 * inserted.  If any of those fail, it should
					 * elog(ERROR) and that's that.
					 */

					trig.tgoid = 0;
					if (fkconstraint->constr_name)
						trig.tgname = fkconstraint->constr_name;
					else
						trig.tgname = "<unknown>";
					trig.tgfoid = 0;
					trig.tgtype = 0;
					trig.tgenabled = TRUE;
					trig.tgisconstraint = TRUE;
					trig.tginitdeferred = FALSE;
					trig.tgdeferrable = FALSE;

					trig.tgargs = (char **) palloc(
					 sizeof(char *) * (4 + length(fkconstraint->fk_attrs)
									   + length(fkconstraint->pk_attrs)));

					if (fkconstraint->constr_name)
						trig.tgargs[0] = fkconstraint->constr_name;
					else
						trig.tgargs[0] = "<unknown>";
					trig.tgargs[1] = (char *) relationName;
					trig.tgargs[2] = fkconstraint->pktable_name;
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
AlterTableDropConstraint(const char *relationName,
						 bool inh, const char *constrName,
						 int behavior)
{
	Relation	rel;
	int			deleted;

	if (!allowSystemTableMods && IsSystemRelationName(relationName))
		elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
			 relationName);
#ifndef NO_SECURITY
	if (!pg_ownercheck(GetUserId(), relationName, RELNAME))
		elog(ERROR, "ALTER TABLE: permission denied");
#endif

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

	rel = heap_openr(relationName, AccessExclusiveLock);

	/* Disallow DROP CONSTRAINT on views, indexes, sequences, etc */
	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 relationName);

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
AlterTableOwner(const char *relationName, const char *newOwnerName)
{
	Relation	class_rel;
	HeapTuple	tuple;
	int32		newOwnerSysid;
	Relation	idescs[Num_pg_class_indices];

	/*
	 * first check that we are a superuser
	 */
	if (!superuser())
		elog(ERROR, "ALTER TABLE: permission denied");

	/*
	 * look up the new owner in pg_shadow and get the sysid
	 */
	newOwnerSysid = get_usesysid(newOwnerName);

	/*
	 * find the table's entry in pg_class and make a modifiable copy
	 */
	class_rel = heap_openr(RelationRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(RELNAME,
							   PointerGetDatum(relationName),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ALTER TABLE: relation \"%s\" not found",
			 relationName);

	switch (((Form_pg_class) GETSTRUCT(tuple))->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_INDEX:
		case RELKIND_VIEW:
		case RELKIND_SEQUENCE:
			/* ok to change owner */
			break;
		default:
			elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table, index, view, or sequence",
				 relationName);
	}

	/*
	 * modify the table's entry and write to the heap
	 */
	((Form_pg_class) GETSTRUCT(tuple))->relowner = newOwnerSysid;

	simple_heap_update(class_rel, &tuple->t_self, tuple);

	/* Keep the catalog indices up to date */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
	CatalogIndexInsert(idescs, Num_pg_class_indices, class_rel, tuple);
	CatalogCloseIndices(Num_pg_class_indices, idescs);

	/*
	 * unlock everything and return
	 */
	heap_freetuple(tuple);
	heap_close(class_rel, NoLock);
}


/*
 * ALTER TABLE CREATE TOAST TABLE
 */
void
AlterTableCreateToastTable(const char *relationName, bool silent)
{
	Relation	rel;
	Oid			myrelid;
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
	 * permissions checking.  XXX exactly what is appropriate here?
	 */
#ifndef NO_SECURITY
	if (!pg_ownercheck(GetUserId(), relationName, RELNAME))
		elog(ERROR, "ALTER TABLE: permission denied");
#endif

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	rel = heap_openr(relationName, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 relationName);

	myrelid = RelationGetRelid(rel);

	/*
	 * lock the pg_class tuple for update (is that really needed?)
	 */
	class_rel = heap_openr(RelationRelationName, RowExclusiveLock);

	reltup = SearchSysCache(RELNAME,
							PointerGetDatum(relationName),
							0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "ALTER TABLE: relation \"%s\" not found",
			 relationName);
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
			 relationName);
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
			 relationName);
	}

	/*
	 * Create the toast table and its index
	 */
	sprintf(toast_relname, "pg_toast_%u", myrelid);
	sprintf(toast_idxname, "pg_toast_%u_idx", myrelid);

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
	 * Note: the toast relation is considered a "normal" relation even if
	 * its master relation is a temp table.  There cannot be any naming
	 * collision, and the toast rel will be destroyed when its master is,
	 * so there's no need to handle the toast rel as temp.
	 */
	toast_relid = heap_create_with_catalog(toast_relname, tupdesc,
										   RELKIND_TOASTVALUE, false,
										   false, true);

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

	toast_idxid = index_create(toast_relname, toast_idxname, indexInfo,
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

/*
 * LOCK TABLE
 */
void
LockTableCommand(LockStmt *lockstmt)
{
	List	   *p;

	/*
	 * Iterate over the list and open, lock, and close the relations one
	 * at a time
	 */

	foreach(p, lockstmt->rellist)
	{
		char	   *relname = strVal(lfirst(p));
		int			aclresult;
		Relation	rel;

		if (lockstmt->mode == AccessShareLock)
			aclresult = pg_aclcheck(relname, GetUserId(),
									ACL_SELECT);
		else
			aclresult = pg_aclcheck(relname, GetUserId(),
									ACL_UPDATE | ACL_DELETE);

		if (aclresult != ACLCHECK_OK)
			elog(ERROR, "LOCK TABLE: permission denied");

		rel = relation_openr(relname, lockstmt->mode);

		/* Currently, we only allow plain tables to be locked */
		if (rel->rd_rel->relkind != RELKIND_RELATION)
			elog(ERROR, "LOCK TABLE: %s is not a table",
				 relname);

		relation_close(rel, NoLock);	/* close rel, keep lock */
	}
}
