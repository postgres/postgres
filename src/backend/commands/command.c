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
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/command.c,v 1.174 2002/04/12 20:38:20 tgl Exp $
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
#include "catalog/namespace.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_relcheck.h"
#include "catalog/pg_type.h"
#include "commands/command.h"
#include "commands/trigger.h"
#include "executor/execdefs.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "parser/analyze.h"
#include "parser/parse.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/relcache.h"


static void drop_default(Oid relid, int16 attnum);
static bool needs_toast_table(Relation rel);
static void CheckTupleType(Form_pg_class tuple_class);


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


/*
 * PerformPortalFetch
 *
 *	name: name of portal
 *	forward: forward or backward fetch?
 *	count: # of tuples to fetch (0 implies all)
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
PerformPortalFetch(char *name,
				   bool forward,
				   int count,
				   CommandDest dest,
				   char *completionTag)
{
	Portal		portal;
	QueryDesc  *queryDesc;
	EState	   *estate;
	MemoryContext oldcontext;
	ScanDirection direction;
	CommandId	savedId;
	bool		temp_desc = false;

	/* initialize completion status in case of early exit */
	if (completionTag)
		strcpy(completionTag, (dest == None) ? "MOVE 0" : "FETCH 0");

	/*
	 * sanity checks
	 */
	if (name == NULL)
	{
		elog(WARNING, "PerformPortalFetch: missing portal name");
		return;
	}

	/*
	 * get the portal from the portal name
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		elog(WARNING, "PerformPortalFetch: portal \"%s\" not found",
			 name);
		return;
	}

	/*
	 * switch into the portal context
	 */
	oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	queryDesc = PortalGetQueryDesc(portal);
	estate = PortalGetState(portal);

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
	 * Restore the scanCommandId that was current when the cursor was
	 * opened.  This ensures that we see the same tuples throughout the
	 * execution of the cursor.
	 */
	savedId = GetScanCommandId();
	SetScanCommandId(PortalGetCommandId(portal));

	/*
	 * Determine which direction to go in, and check to see if we're
	 * already at the end of the available tuples in that direction.  If
	 * so, set the direction to NoMovement to avoid trying to fetch any
	 * tuples.  (This check exists because not all plan node types
	 * are robust about being called again if they've already returned
	 * NULL once.)  Then call the executor (we must not skip this, because
	 * the destination needs to see a setup and shutdown even if no tuples
	 * are available).  Finally, update the atStart/atEnd state depending
	 * on the number of tuples that were retrieved.
	 */
	if (forward)
	{
		if (portal->atEnd)
			direction = NoMovementScanDirection;
		else
			direction = ForwardScanDirection;

		ExecutorRun(queryDesc, estate, direction, (long) count);

		if (estate->es_processed > 0)
			portal->atStart = false; /* OK to back up now */
		if (count <= 0 || (int) estate->es_processed < count)
			portal->atEnd = true;	/* we retrieved 'em all */
	}
	else
	{
		if (portal->atStart)
			direction = NoMovementScanDirection;
		else
			direction = BackwardScanDirection;

		ExecutorRun(queryDesc, estate, direction, (long) count);

		if (estate->es_processed > 0)
			portal->atEnd = false;	/* OK to go forward now */
		if (count <= 0 || (int) estate->es_processed < count)
			portal->atStart = true; /* we retrieved 'em all */
	}

	/* Return command status if wanted */
	if (completionTag)
		snprintf(completionTag, COMPLETION_TAG_BUFSIZE, "%s %u",
				 (dest == None) ? "MOVE" : "FETCH",
				 estate->es_processed);

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
		elog(WARNING, "PerformPortalClose: missing portal name");
		return;
	}

	/*
	 * get the portal from the portal name
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		elog(WARNING, "PerformPortalClose: portal \"%s\" not found",
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
	sprintf(toast_idxname, "pg_toast_%u_idx", relOid);

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

	foreach(p, lockstmt->relations)
	{
		RangeVar   *relation = lfirst(p);
		Oid			reloid;
		int32		aclresult;
		Relation	rel;

		/*
		 * We don't want to open the relation until we've checked privilege.
		 * So, manually get the relation OID.
		 */
		reloid = RangeVarGetRelid(relation, false);

		if (lockstmt->mode == AccessShareLock)
			aclresult = pg_class_aclcheck(reloid, GetUserId(),
										  ACL_SELECT);
		else
			aclresult = pg_class_aclcheck(reloid, GetUserId(),
										  ACL_UPDATE | ACL_DELETE);

		if (aclresult != ACLCHECK_OK)
			elog(ERROR, "LOCK TABLE: permission denied");

		rel = relation_open(reloid, lockstmt->mode);

		/* Currently, we only allow plain tables to be locked */
		if (rel->rd_rel->relkind != RELKIND_RELATION)
			elog(ERROR, "LOCK TABLE: %s is not a table",
				 relation->relname);

		relation_close(rel, NoLock);	/* close rel, keep lock */
	}
}


/*
 * CREATE SCHEMA
 */
void
CreateSchemaCommand(CreateSchemaStmt *stmt)
{
	const char *schemaName = stmt->schemaname;
	const char *authId = stmt->authid;
	List	   *parsetree_list;
	List	   *parsetree_item;
	const char *owner_name;
	Oid			owner_userid;
	Oid			saved_userid;

	saved_userid = GetUserId();

	if (!authId)
	{
		owner_userid = saved_userid;
		owner_name = GetUserName(owner_userid);
	}
	else if (superuser())
	{
		owner_name = authId;
		/* The following will error out if user does not exist */
		owner_userid = get_usesysid(owner_name);
		/*
		 * Set the current user to the requested authorization so
		 * that objects created in the statement have the requested
		 * owner.  (This will revert to session user on error or at
		 * the end of this routine.)
		 */
		SetUserId(owner_userid);
	}
	else /* not superuser */
	{
		owner_userid = saved_userid;
		owner_name = GetUserName(owner_userid);
		if (strcmp(authId, owner_name) != 0)
			elog(ERROR, "CREATE SCHEMA: permission denied"
				 "\n\t\"%s\" is not a superuser, so cannot create a schema for \"%s\"",
				 owner_name, authId);
	}

	if (!allowSystemTableMods && IsReservedName(schemaName))
		elog(ERROR, "CREATE SCHEMA: Illegal schema name: \"%s\" -- pg_ is reserved for system schemas",
			 schemaName);

	/* Create the schema's namespace */
	NamespaceCreate(schemaName, owner_userid);

	/* Let commands in the schema-element-list know about the schema */
	CommandCounterIncrement();

	/*
	 * Examine the list of commands embedded in the CREATE SCHEMA command,
	 * and reorganize them into a sequentially executable order with no
	 * forward references.  Note that the result is still a list of raw
	 * parsetrees in need of parse analysis --- we cannot, in general,
	 * run analyze.c on one statement until we have actually executed the
	 * prior ones.
	 */
	parsetree_list = analyzeCreateSchemaStmt(stmt);

	/*
	 * Analyze and execute each command contained in the CREATE SCHEMA
	 */
	foreach(parsetree_item, parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(parsetree_item);
		List	   *querytree_list,
				   *querytree_item;

		querytree_list = parse_analyze(parsetree, NULL);

		foreach(querytree_item, querytree_list)
		{
			Query	   *querytree = (Query *) lfirst(querytree_item);

			/* schemas should contain only utility stmts */
			Assert(querytree->commandType == CMD_UTILITY);
			/* do this step */
			ProcessUtility(querytree->utilityStmt, None, NULL);
			/* make sure later steps can see the object created here */
			CommandCounterIncrement();
		}
	}

	/* Reset current user */
	SetUserId(saved_userid);
}
