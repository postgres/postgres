/*-------------------------------------------------------------------------
 *
 * command.c
 *	  random postgres portal and utility support code
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/command.c,v 1.72 2000/05/28 17:55:54 tgl Exp $
 *
 * NOTES
 *	  The PortalExecutorHeapMemory crap needs to be eliminated
 *	  by designing a better executor / portal processing memory
 *	  interface.
 *
 *	  The PerformAddAttribute() code, like most of the relation
 *	  manipulating code in the commands/ directory, should go
 *	  someplace closer to the lib/catalog code.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/skey.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_type.h"
#include "commands/command.h"
#include "commands/rename.h"
#include "executor/execdefs.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "catalog/heap.h"
#include "miscadmin.h"
#include "optimizer/prep.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"
#include "utils/temprel.h"
#include "commands/trigger.h"
#ifdef	_DROP_COLUMN_HACK__
#include "catalog/pg_index.h"
#include "catalog/pg_relcheck.h"
#include "commands/defrem.h"
#include "commands/comment.h"
#include "access/genam.h"
#include "optimizer/clauses.h"
#include "../parser/parse.h"
#endif	 /* _DROP_COLUMN_HACK__ */

/* ----------------
 *		PortalExecutorHeapMemory stuff
 *
 *		This is where the XXXSuperDuperHacky code was. -cim 3/15/90
 * ----------------
 */
MemoryContext PortalExecutorHeapMemory = NULL;

/* --------------------------------
 *		PortalCleanup
 * --------------------------------
 */
void
PortalCleanup(Portal portal)
{
	MemoryContext context;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	AssertArg(PortalIsValid(portal));
	AssertArg(portal->cleanup == PortalCleanup);

	/* ----------------
	 *	set proper portal-executor context before calling ExecMain.
	 * ----------------
	 */
	context = MemoryContextSwitchTo((MemoryContext) PortalGetHeapMemory(portal));
	PortalExecutorHeapMemory = (MemoryContext) PortalGetHeapMemory(portal);

	/* ----------------
	 *	tell the executor to shutdown the query
	 * ----------------
	 */
	ExecutorEnd(PortalGetQueryDesc(portal), PortalGetState(portal));

	/* ----------------
	 *	switch back to previous context
	 * ----------------
	 */
	MemoryContextSwitchTo(context);
	PortalExecutorHeapMemory = (MemoryContext) NULL;
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
	int			feature;
	QueryDesc  *queryDesc;
	MemoryContext context;
	Const		limcount;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	if (name == NULL)
	{
		elog(NOTICE, "PerformPortalFetch: blank portal unsupported");
		return;
	}

	/* ----------------
	 *	Create a const node from the given count value
	 * ----------------
	 */
	memset(&limcount, 0, sizeof(limcount));
	limcount.type = T_Const;
	limcount.consttype = INT4OID;
	limcount.constlen = sizeof(int4);
	limcount.constvalue = (Datum) count;
	limcount.constisnull = FALSE;
	limcount.constbyval = TRUE;
	limcount.constisset = FALSE;
	limcount.constiscast = FALSE;


	/* ----------------
	 *	get the portal from the portal name
	 * ----------------
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		elog(NOTICE, "PerformPortalFetch: portal \"%s\" not found",
			 name);
		return;
	}

	/* ----------------
	 *	switch into the portal context
	 * ----------------
	 */
	context = MemoryContextSwitchTo((MemoryContext) PortalGetHeapMemory(portal));

	AssertState(context == (MemoryContext) PortalGetHeapMemory(GetPortalByName(NULL)));

	/* ----------------
	 *	setup "feature" to tell the executor what direction and
	 *	how many tuples to fetch.
	 * ----------------
	 */
	if (forward)
		feature = EXEC_FOR;
	else
		feature = EXEC_BACK;

	/* ----------------
	 *	tell the destination to prepare to recieve some tuples
	 * ----------------
	 */
	queryDesc = PortalGetQueryDesc(portal);

	if (dest == None)			/* MOVE */
	{
		QueryDesc  *qdesc = (QueryDesc *) palloc(sizeof(QueryDesc));

		memcpy(qdesc, queryDesc, sizeof(QueryDesc));
		qdesc->dest = dest;
		queryDesc = qdesc;
	}

	BeginCommand(name,
				 queryDesc->operation,
				 portal->attinfo,		/* QueryDescGetTypeInfo(queryDesc),
										 * */
				 false,			/* portal fetches don't end up in
								 * relations */
				 false,			/* this is a portal fetch, not a "retrieve
								 * portal" */
				 tag,
				 dest);

	/* ----------------
	 *	execute the portal fetch operation
	 * ----------------
	 */
	PortalExecutorHeapMemory = (MemoryContext) PortalGetHeapMemory(portal);

	ExecutorRun(queryDesc, PortalGetState(portal), feature,
				(Node *) NULL, (Node *) &limcount);

	if (dest == None)			/* MOVE */
		pfree(queryDesc);

	/* ----------------
	 * Note: the "end-of-command" tag is returned by higher-level
	 *		 utility code
	 *
	 * Return blank portal for now.
	 * Otherwise, this named portal will be cleaned.
	 * Note: portals will only be supported within a BEGIN...END
	 * block in the near future.  Later, someone will fix it to
	 * do what is possible across transaction boundries.
	 * ----------------
	 */
	MemoryContextSwitchTo(
			 (MemoryContext) PortalGetHeapMemory(GetPortalByName(NULL)));
}

/* --------------------------------
 *		PerformPortalClose
 * --------------------------------
 */
void
PerformPortalClose(char *name, CommandDest dest)
{
	Portal		portal;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	if (name == NULL)
	{
		elog(NOTICE, "PerformPortalClose: blank portal unsupported");
		return;
	}

	/* ----------------
	 *	get the portal from the portal name
	 * ----------------
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		elog(NOTICE, "PerformPortalClose: portal \"%s\" not found",
			 name);
		return;
	}

	/* ----------------
	 *	Note: PortalCleanup is called as a side-effect
	 * ----------------
	 */
	PortalDrop(&portal);
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
 *		many complications in formtuple, amgetattr, and addattribute.
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
	HeapTuple	attributeTuple;
	Form_pg_attribute attribute;
	FormData_pg_attribute attributeD;
	int			i;
	int			minattnum,
				maxatts;
	HeapTuple	tup;
	Relation	idescs[Num_pg_attr_indices];
	Relation	ridescs[Num_pg_class_indices];
	bool		hasindex;

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
	if (!pg_ownercheck(UserName, relationName, RELNAME))
		elog(ERROR, "ALTER TABLE: permission denied");
#endif

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	rel = heap_openr(relationName, AccessExclusiveLock);
	myrelid = RelationGetRelid(rel);
	heap_close(rel, NoLock);	/* close rel but keep lock! */

	/*
	 * we can't add a not null attribute
	 */
	if (colDef->is_not_null)
		elog(ERROR, "Can't add a NOT NULL attribute to an existing relation");

	if (colDef->raw_default || colDef->cooked_default)
		elog(ERROR, "Adding columns with defaults is not implemented.");


	/*
	 * if the first element in the 'schema' list is a "*" then we are
	 * supposed to add this attribute to all classes that inherit from
	 * 'relationName' (as well as to 'relationName').
	 *
	 * any permissions or problems with duplicate attributes will cause the
	 * whole transaction to abort, which is what we want -- all or
	 * nothing.
	 */
	if (colDef != NULL)
	{
		if (inherits)
		{
			List	   *child,
					   *children;

			/* this routine is actually in the planner */
			children = find_all_inheritors(myrelid);

			/*
			 * find_all_inheritors does the recursive search of the
			 * inheritance hierarchy, so all we have to do is process all
			 * of the relids in the list that it returns.
			 */
			foreach(child, children)
			{
				Oid			childrelid = lfirsti(child);

				if (childrelid == myrelid)
					continue;
				rel = heap_open(childrelid, AccessExclusiveLock);
				AlterTableAddColumn(RelationGetRelationName(rel),
									false, colDef);
				heap_close(rel, AccessExclusiveLock);
			}
		}
	}

	rel = heap_openr(RelationRelationName, RowExclusiveLock);

	reltup = SearchSysCacheTupleCopy(RELNAME,
									 PointerGetDatum(relationName),
									 0, 0, 0);

	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "ALTER TABLE: relation \"%s\" not found",
			 relationName);

	/*
	 * XXX is the following check sufficient?
	 */
	if (((Form_pg_class) GETSTRUCT(reltup))->relkind != RELKIND_RELATION)
	{
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 relationName);
	}

	minattnum = ((Form_pg_class) GETSTRUCT(reltup))->relnatts;
	maxatts = minattnum + 1;
	if (maxatts > MaxHeapAttributeNumber)
		elog(ERROR, "ALTER TABLE: relations limited to %d columns",
			 MaxHeapAttributeNumber);

	attrdesc = heap_openr(AttributeRelationName, RowExclusiveLock);

	/*
	 * Open all (if any) pg_attribute indices
	 */
	hasindex = RelationGetForm(attrdesc)->relhasindex;
	if (hasindex)
		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);

	attributeD.attrelid = reltup->t_data->t_oid;

	attributeTuple = heap_addheader(Natts_pg_attribute,
									sizeof attributeD,
									(char *) &attributeD);

	attribute = (Form_pg_attribute) GETSTRUCT(attributeTuple);

	i = 1 + minattnum;

	{
		HeapTuple	typeTuple;
		Form_pg_type tform;
		char	   *typename;
		int			attnelems;

		tup = SearchSysCacheTuple(ATTNAME,
								  ObjectIdGetDatum(reltup->t_data->t_oid),
								  PointerGetDatum(colDef->colname),
								  0, 0);

		if (HeapTupleIsValid(tup))
			elog(ERROR, "ALTER TABLE: column name \"%s\" already exists in table \"%s\"",
				 colDef->colname, relationName);

		/*
		 * check to see if it is an array attribute.
		 */

		typename = colDef->typename->name;

		if (colDef->typename->arrayBounds)
		{
			attnelems = length(colDef->typename->arrayBounds);
			typename = makeArrayTypeName(colDef->typename->name);
		}
		else
			attnelems = 0;

		typeTuple = SearchSysCacheTuple(TYPENAME,
										PointerGetDatum(typename),
										0, 0, 0);
		tform = (Form_pg_type) GETSTRUCT(typeTuple);

		if (!HeapTupleIsValid(typeTuple))
			elog(ERROR, "ALTER TABLE: type \"%s\" does not exist", typename);
		namestrcpy(&(attribute->attname), colDef->colname);
		attribute->atttypid = typeTuple->t_data->t_oid;
		attribute->attlen = tform->typlen;
		attribute->attdisbursion = 0;
		attribute->attcacheoff = -1;
		attribute->atttypmod = colDef->typename->typmod;
		attribute->attnum = i;
		attribute->attbyval = tform->typbyval;
		attribute->attnelems = attnelems;
		attribute->attisset = (bool) (tform->typtype == 'c');
		attribute->attstorage = 'p';
		attribute->attalign = tform->typalign;
		attribute->attnotnull = false;
		attribute->atthasdef = (colDef->raw_default != NULL ||
								colDef->cooked_default != NULL);

		heap_insert(attrdesc, attributeTuple);
		if (hasindex)
			CatalogIndexInsert(idescs,
							   Num_pg_attr_indices,
							   attrdesc,
							   attributeTuple);
	}

	if (hasindex)
		CatalogCloseIndices(Num_pg_attr_indices, idescs);

	heap_close(attrdesc, RowExclusiveLock);

	((Form_pg_class) GETSTRUCT(reltup))->relnatts = maxatts;
	heap_update(rel, &reltup->t_self, reltup, NULL);

	/* keep catalog indices current */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
	CatalogIndexInsert(ridescs, Num_pg_class_indices, rel, reltup);
	CatalogCloseIndices(Num_pg_class_indices, ridescs);

	heap_freetuple(reltup);

	heap_close(rel, NoLock);
}



static void drop_default(Oid relid, int16 attnum);


/*
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT
 */
void
AlterTableAlterColumn(const char *relationName,
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
	if (!pg_ownercheck(UserName, relationName, RELNAME))
		elog(ERROR, "ALTER TABLE: permission denied");
#endif

	rel = heap_openr(relationName, AccessExclusiveLock);
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
			AlterTableAlterColumn(RelationGetRelationName(rel),
								  false, colName, newDefault);
			heap_close(rel, AccessExclusiveLock);
		}
	}

	/* -= now do the thing on this relation =- */

	/* reopen the business */
	rel = heap_openr((char *) relationName, AccessExclusiveLock);

	/*
	 * get the number of the attribute
	 */
	tuple = SearchSysCacheTuple(ATTNAME,
								ObjectIdGetDatum(myrelid),
								NameGetDatum(namein((char *) colName)),
								0, 0);

	if (!HeapTupleIsValid(tuple))
	{
		heap_close(rel, AccessExclusiveLock);
		elog(ERROR, "ALTER TABLE: relation \"%s\" has no column \"%s\"",
			 relationName, colName);
	}

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;

	if (newDefault)				/* SET DEFAULT */
	{
		List	   *rawDefaults = NIL;
		RawColumnDefault *rawEnt;

		/* Get rid of the old one first */
		drop_default(myrelid, attnum);

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = newDefault;
		rawDefaults = lappend(rawDefaults, rawEnt);

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 */
		AddRelationRawConstraints(rel, rawDefaults, NIL);
	}

	else
/* DROP DEFAULT */
	{
		Relation	attr_rel;
		ScanKeyData scankeys[3];
		HeapScanDesc scan;
		HeapTuple	tuple;

		attr_rel = heap_openr(AttributeRelationName, AccessExclusiveLock);
		ScanKeyEntryInitialize(&scankeys[0], 0x0,
							   Anum_pg_attribute_attrelid, F_OIDEQ,
							   ObjectIdGetDatum(myrelid));
		ScanKeyEntryInitialize(&scankeys[1], 0x0,
							   Anum_pg_attribute_attnum, F_INT2EQ,
							   Int16GetDatum(attnum));
		ScanKeyEntryInitialize(&scankeys[2], 0x0,
							   Anum_pg_attribute_atthasdef, F_BOOLEQ,
							   Int32GetDatum(TRUE));

		scan = heap_beginscan(attr_rel, false, SnapshotNow, 3, scankeys);
		AssertState(scan != NULL);

		if (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
		{
			HeapTuple	newtuple;
			Relation	irelations[Num_pg_attr_indices];

			/* update to false */
			newtuple = heap_copytuple(tuple);
			((Form_pg_attribute) GETSTRUCT(newtuple))->atthasdef = FALSE;
			heap_update(attr_rel, &tuple->t_self, newtuple, NULL);

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

	attrdef_rel = heap_openr(AttrDefaultRelationName, AccessExclusiveLock);
	ScanKeyEntryInitialize(&scankeys[0], 0x0,
						   Anum_pg_attrdef_adrelid, F_OIDEQ,
						   ObjectIdGetDatum(relid));
	ScanKeyEntryInitialize(&scankeys[1], 0x0,
						   Anum_pg_attrdef_adnum, F_INT2EQ,
						   Int16GetDatum(attnum));

	scan = heap_beginscan(attrdef_rel, false, SnapshotNow, 2, scankeys);
	AssertState(scan != NULL);

	if (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
		heap_delete(attrdef_rel, &tuple->t_self, NULL);

	heap_endscan(scan);

	heap_close(attrdef_rel, NoLock);
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
}			SysScanDescData, *SysScanDesc;

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
find_attribute_walker(Node *node, int attnum)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0 && var->varno == 1 &&
			var->varattno == attnum)
			return true;
	}
	return expression_tree_walker(node, find_attribute_walker, (void *) attnum);
}
static bool
find_attribute_in_node(Node *node, int attnum)
{
	return expression_tree_walker(node, find_attribute_walker, (void *) attnum);
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
	Form_pg_relcheck relcheck;
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
		char	   *ccbin;
		Node	   *node;

		relcheck = (Form_pg_relcheck) GETSTRUCT(htup);
		ccbin = textout(&relcheck->rcbin);
		if (!ccbin)
			continue;
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
				heap_delete(rcrel, &htup->t_self, NULL);
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
					htup = SearchSysCacheTuple(RELOID,
									 ObjectIdGetDatum(index->indexrelid),
											   0, 0, 0);
					RemoveIndex(NameStr(((Form_pg_class) GETSTRUCT(htup))->relname));
				}
				break;
			}
		}
	}
	heap_endscan(scan);
	heap_close(indexRelation, NoLock);

	return checkok;
}

#endif	 /* _DROP_COLUMN_HACK__ */

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
				attrdesc,
				adrel;
	Oid			myrelid,
				attoid;
	HeapTuple	reltup;
	HeapTupleData classtuple;
	Buffer		buffer;
	Form_pg_attribute attribute;
	HeapTuple	tup;
	Relation	idescs[Num_pg_attr_indices];
	int			attnum;
	bool		hasindex;
	char		dropColname[32];
	void	   *sysscan;
	ScanKeyData scankeys[2];

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
	if (!pg_ownercheck(UserName, relationName, RELNAME))
		elog(ERROR, "ALTER TABLE: permission denied");
#endif

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	rel = heap_openr(relationName, AccessExclusiveLock);
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
	reltup = SearchSysCacheTuple(RELNAME, PointerGetDatum(relationName),
								 0, 0, 0);

	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "ALTER TABLE: relation \"%s\" not found",
			 relationName);
	rel = heap_openr(RelationRelationName, RowExclusiveLock);
	classtuple.t_self = reltup->t_self;
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

	/*
	 * XXX is the following check sufficient?
	 */
	if (((Form_pg_class) GETSTRUCT(reltup))->relkind != RELKIND_RELATION)
	{
		elog(ERROR, "ALTER TABLE: relation \"%s\" is not a table",
			 relationName);
	}

	attrdesc = heap_openr(AttributeRelationName, RowExclusiveLock);

	/*
	 * Get the target pg_attribute tuple
	 */
	tup = SearchSysCacheTupleCopy(ATTNAME,
								  ObjectIdGetDatum(reltup->t_data->t_oid),
								  PointerGetDatum(colName), 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "ALTER TABLE: column name \"%s\" doesn't exist in table \"%s\"",
			 colName, relationName);

	attribute = (Form_pg_attribute) GETSTRUCT(tup);
	if (attribute->attnum <= 0)
		elog(ERROR, "ALTER TABLE: column name \"%s\" was already dropped", colName);
	attnum = attribute->attnum;
	attoid = tup->t_data->t_oid;

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

	heap_update(attrdesc, &tup->t_self, tup, NULL);
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

	/* delete comments */
	DeleteComments(attoid);
	/* delete attrdef */
	adrel = heap_openr(AttrDefaultRelationName, RowExclusiveLock);
	ScanKeyEntryInitialize(&scankeys[0], 0x0, Anum_pg_attrdef_adrelid,
						   F_OIDEQ, ObjectIdGetDatum(myrelid));

	/*
	 * Oops pg_attrdef doesn't have (adrelid,adnum) index
	 * ScanKeyEntryInitialize(&scankeys[1], 0x0, Anum_pg_attrdef_adnum,
	 * F_INT2EQ, Int16GetDatum(attnum)); sysscan =
	 * systable_beginscan(adrel, AttrDefaultIndex, 2, scankeys);
	 */
	sysscan = systable_beginscan(adrel, AttrDefaultIndex, 1, scankeys);
	while (HeapTupleIsValid(tup = systable_getnext(sysscan)))
	{
		if (((Form_pg_attrdef) GETSTRUCT(tup))->adnum == attnum)
		{
			heap_delete(adrel, &tup->t_self, NULL);
			break;
		}
	}
	systable_endscan(sysscan);
	heap_close(adrel, NoLock);

	/*
	 * Remove objects which reference this column
	 */
	if (behavior == CASCADE)
	{
		Relation	ridescs[Num_pg_class_indices];

		RemoveColumnReferences(myrelid, attnum, false, reltup);
		/* update pg_class tuple */
		heap_update(rel, &reltup->t_self, reltup, NULL);
		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
		CatalogIndexInsert(ridescs, Num_pg_class_indices, rel, reltup);
		CatalogCloseIndices(Num_pg_class_indices, ridescs);
	}

	heap_freetuple(reltup);
	heap_close(rel, NoLock);
#else
				elog(ERROR, "ALTER TABLE / DROP COLUMN is not implemented");
#endif	 /* _DROP_COLUMN_HACK__ */
}



/*
 * ALTER TABLE ADD CONSTRAINT
 */
void
AlterTableAddConstraint(const char *relationName,
						bool inh, Node *newConstraint)
{
	if (newConstraint == NULL)
		elog(ERROR, "ALTER TABLE / ADD CONSTRAINT passed invalid constraint.");

	switch (nodeTag(newConstraint))
	{
		case T_Constraint:
			elog(ERROR, "ALTER TABLE / ADD CONSTRAINT is not implemented");
		case T_FkConstraint:
			{
				FkConstraint *fkconstraint = (FkConstraint *) newConstraint;
				Relation	rel;
				HeapScanDesc scan;
				HeapTuple	tuple;
				Trigger		trig;
				List	   *list;
				int			count;

				/*
				 * Grab an exclusive lock on the pk table, so that someone
				 * doesn't delete rows out from under us.
				 */

				rel = heap_openr(fkconstraint->pktable_name, AccessExclusiveLock);
				heap_close(rel, NoLock);

				/*
				 * Grab an exclusive lock on the fk table, and then scan
				 * through each tuple, calling the RI_FKey_Match_Ins
				 * (insert trigger) as if that tuple had just been
				 * inserted.  If any of those fail, it should elog(ERROR)
				 * and that's that.
				 */
				rel = heap_openr(relationName, AccessExclusiveLock);
				trig.tgoid = 0;
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

				trig.tgargs[0] = "<unnamed>";
				trig.tgargs[1] = (char *) relationName;
				trig.tgargs[2] = fkconstraint->pktable_name;
				trig.tgargs[3] = fkconstraint->match_type;
				count = 4;
				foreach(list, fkconstraint->fk_attrs)
				{
					Ident	   *fk_at = lfirst(list);

					trig.tgargs[count++] = fk_at->name;
				}
				foreach(list, fkconstraint->pk_attrs)
				{
					Ident	   *pk_at = lfirst(list);

					trig.tgargs[count++] = pk_at->name;
				}
				trig.tgnargs = count;

				scan = heap_beginscan(rel, false, SnapshotNow, 0, NULL);
				AssertState(scan != NULL);

				while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
				{
					TriggerData newtrigdata;

					newtrigdata.tg_event = TRIGGER_EVENT_INSERT | TRIGGER_EVENT_ROW;
					newtrigdata.tg_relation = rel;
					newtrigdata.tg_trigtuple = tuple;
					newtrigdata.tg_newtuple = NULL;
					newtrigdata.tg_trigger = &trig;

					CurrentTriggerData = &newtrigdata;

					RI_FKey_check_ins(NULL);

					/* Make a call to the check function */
				}
				heap_endscan(scan);
				heap_close(rel, NoLock);		/* close rel but keep
												 * lock! */

				pfree(trig.tgargs);
			}
			break;
		default:
			elog(ERROR, "ALTER TABLE / ADD CONSTRAINT unable to determine type of constraint passed");
	}
}



/*
 * ALTER TABLE DROP CONSTRAINT
 */
void
AlterTableDropConstraint(const char *relationName,
						 bool inh, const char *constrName,
						 int behavior)
{
	elog(ERROR, "ALTER TABLE / DROP CONSTRAINT is not implemented");
}



/*
 *
 * LOCK TABLE
 *
 */
void
LockTableCommand(LockStmt *lockstmt)
{
	Relation	rel;
	int			aclresult;

	rel = heap_openr(lockstmt->relname, NoLock);
	if (!RelationIsValid(rel))
		elog(ERROR, "Relation '%s' does not exist", lockstmt->relname);

	if (lockstmt->mode == AccessShareLock)
		aclresult = pg_aclcheck(lockstmt->relname, GetPgUserName(), ACL_RD);
	else
		aclresult = pg_aclcheck(lockstmt->relname, GetPgUserName(), ACL_WR);

	if (aclresult != ACLCHECK_OK)
		elog(ERROR, "LOCK TABLE: permission denied");

	LockRelation(rel, lockstmt->mode);

	heap_close(rel, NoLock);	/* close rel, keep lock */
}
