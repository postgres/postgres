/*-------------------------------------------------------------------------
 *
 * command.c--
 *	  random postgres portal and utility support code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/command.c,v 1.35 1998/12/18 09:10:18 vadim Exp $
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
#include "access/relscan.h"
#include "catalog/indexing.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/pg_type.h"
#include "commands/command.h"
#include "executor/execdefs.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "optimizer/prep.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/mcxt.h"
#include "utils/portal.h"
#include "utils/syscache.h"
#include "miscadmin.h"

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
	PortalExecutorHeapMemory = (MemoryContext)
		PortalGetHeapMemory(portal);

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

	AssertState(context ==
			 (MemoryContext) PortalGetHeapMemory(GetPortalByName(NULL)));

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
	PortalExecutorHeapMemory = (MemoryContext)
		PortalGetHeapMemory(portal);

	ExecutorRun(queryDesc, PortalGetState(portal), feature, count);

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
	PortalDestroy(&portal);
}

/* ----------------
 *		PerformAddAttribute
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
PerformAddAttribute(char *relationName,
					char *userName,
					bool inherits,
					ColumnDef *colDef)
{
	Relation	rel,
				attrdesc;
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
	if (IsSystemRelationName(relationName))
		elog(ERROR, "PerformAddAttribute: class \"%s\" is a system catalog",
			 relationName);
#ifndef NO_SECURITY
	if (!pg_ownercheck(userName, relationName, RELNAME))
		elog(ERROR, "PerformAddAttribute: you do not own class \"%s\"",
			 relationName);
#endif

	/*
	 * we can't add a not null attribute
	 */
	if (colDef->is_not_null)
		elog(ERROR, "Can't add a NOT NULL attribute to an existing relation");
	if (colDef->defval)
		elog(ERROR, "ADD ATTRIBUTE: DEFAULT not yet implemented");

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
			Oid			myrelid,
						childrelid;
			List	   *child,
					   *children;

			rel = heap_openr(relationName);
			if (!RelationIsValid(rel))
			{
				elog(ERROR, "PerformAddAttribute: unknown relation: \"%s\"",
					 relationName);
			}
			myrelid = RelationGetRelid(rel);
			heap_close(rel);

			/* this routine is actually in the planner */
			children = find_all_inheritors(lconsi(myrelid, NIL), NIL);

			/*
			 * find_all_inheritors does the recursive search of the
			 * inheritance hierarchy, so all we have to do is process all
			 * of the relids in the list that it returns.
			 */
			foreach(child, children)
			{
				childrelid = lfirsti(child);
				if (childrelid == myrelid)
					continue;
				rel = heap_open(childrelid);
				if (!RelationIsValid(rel))
				{
					elog(ERROR, "PerformAddAttribute: can't find catalog entry for inheriting class with oid %d",
						 childrelid);
				}
				PerformAddAttribute((rel->rd_rel->relname).data,
									userName, false, colDef);
				heap_close(rel);
			}
		}
	}

	rel = heap_openr(RelationRelationName);

	reltup = SearchSysCacheTupleCopy(RELNAME,
									 PointerGetDatum(relationName),
									 0, 0, 0);

	if (!HeapTupleIsValid(reltup))
	{
		heap_close(rel);
		elog(ERROR, "PerformAddAttribute: relation \"%s\" not found",
			 relationName);
	}

	/*
	 * XXX is the following check sufficient?
	 */
	if (((Form_pg_class) GETSTRUCT(reltup))->relkind == RELKIND_INDEX)
	{
		elog(ERROR, "PerformAddAttribute: index relation \"%s\" not changed",
			 relationName);
		return;
	}

	minattnum = ((Form_pg_class) GETSTRUCT(reltup))->relnatts;
	maxatts = minattnum + 1;
	if (maxatts > MaxHeapAttributeNumber)
	{
		pfree(reltup);
		heap_close(rel);
		elog(ERROR, "PerformAddAttribute: relations limited to %d attributes",
			 MaxHeapAttributeNumber);
	}

	attrdesc = heap_openr(AttributeRelationName);

	Assert(attrdesc);
	Assert(RelationGetForm(attrdesc));

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
		Form_pg_type form;
		char	   *typename;
		int			attnelems;

		tup = SearchSysCacheTuple(ATTNAME,
								  ObjectIdGetDatum(reltup->t_data->t_oid),
								  PointerGetDatum(colDef->colname),
								  0, 0);

		if (HeapTupleIsValid(tup))
		{
			heap_close(attrdesc);
			heap_close(rel);
			elog(ERROR, "PerformAddAttribute: attribute \"%s\" already exists in class \"%s\"",
				 colDef->colname, relationName);
		}

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

		typeTuple = SearchSysCacheTuple(TYPNAME,
										PointerGetDatum(typename),
										0, 0, 0);
		form = (Form_pg_type) GETSTRUCT(typeTuple);

		if (!HeapTupleIsValid(typeTuple))
			elog(ERROR, "Add: type \"%s\" nonexistent", typename);
		namestrcpy(&(attribute->attname), colDef->colname);
		attribute->atttypid = typeTuple->t_data->t_oid;
		attribute->attlen = form->typlen;
		attributeD.attdisbursion = 0;
		attribute->attcacheoff = -1;
		attribute->atttypmod = colDef->typename->typmod;
		attribute->attnum = i;
		attribute->attbyval = form->typbyval;
		attribute->attnelems = attnelems;
		attribute->attisset = (bool) (form->typtype == 'c');
		attribute->attalign = form->typalign;
		attribute->attnotnull = false;
		attribute->atthasdef = (colDef->defval != NULL);

		heap_insert(attrdesc, attributeTuple);
		if (hasindex)
			CatalogIndexInsert(idescs,
							   Num_pg_attr_indices,
							   attrdesc,
							   attributeTuple);
	}

	if (hasindex)
		CatalogCloseIndices(Num_pg_attr_indices, idescs);
	heap_close(attrdesc);

	((Form_pg_class) GETSTRUCT(reltup))->relnatts = maxatts;
	heap_replace(rel, &reltup->t_self, reltup, NULL);

	/* keep catalog indices current */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
	CatalogIndexInsert(ridescs, Num_pg_class_indices, rel, reltup);
	CatalogCloseIndices(Num_pg_class_indices, ridescs);

	pfree(reltup);
	heap_close(rel);
}

void
LockTableCommand(LockStmt *lockstmt)
{
	Relation	rel;
	int			aclresult;

	rel = heap_openr(lockstmt->relname);
	if (rel == NULL)
		elog(ERROR, "LOCK TABLE: relation %s can't be openned", lockstmt->relname);

	if (lockstmt->mode == AccessShareLock)
		aclresult = pg_aclcheck(lockstmt->relname, GetPgUserName(), ACL_RD);
	else
		aclresult = pg_aclcheck(lockstmt->relname, GetPgUserName(), ACL_WR);

	if (aclresult != ACLCHECK_OK)
		elog(ERROR, "LOCK TABLE: permission denied");

	LockRelation(rel, lockstmt->mode);

}
