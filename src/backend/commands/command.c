/*-------------------------------------------------------------------------
 *
 * command.c--
 *    random postgres portal and utility support code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/Attic/command.c,v 1.10 1997/08/19 04:43:27 vadim Exp $
 *
 * NOTES
 *    The PortalExecutorHeapMemory crap needs to be eliminated
 *    by designing a better executor / portal processing memory
 *    interface.
 *	
 *    The PerformAddAttribute() code, like most of the relation
 *    manipulating code in the commands/ directory, should go
 *    someplace closer to the lib/catalog code.
 *	
 *-------------------------------------------------------------------------
 */
#include <postgres.h>

#include <access/relscan.h>
#include <utils/portal.h>  
#include <commands/command.h>
#include <utils/mcxt.h>
#include <executor/executor.h>
#include <executor/execdefs.h>
#include <catalog/indexing.h>
#include <utils/syscache.h>
#include <catalog/catalog.h>
#include <access/heapam.h>  
#include <utils/array.h>
#include <utils/acl.h>
#include <optimizer/prep.h>
#include <catalog/catname.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <utils/builtins.h>

/* ----------------
 * 	PortalExecutorHeapMemory stuff
 *
 *	This is where the XXXSuperDuperHacky code was. -cim 3/15/90
 * ----------------
 */
MemoryContext PortalExecutorHeapMemory  = NULL;

/* --------------------------------
 * 	PortalCleanup
 * --------------------------------
 */
void
PortalCleanup(Portal portal)
{
    MemoryContext	context;
    
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
 * 	PerformPortalFetch
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
    QueryDesc		*queryDesc;
    MemoryContext	context;
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    if (name == NULL) {
	elog(NOTICE, "PerformPortalFetch: blank portal unsupported");
	return;
    }
    
    /* ----------------
     *	get the portal from the portal name
     * ----------------
     */
    portal = GetPortalByName(name);
    if (! PortalIsValid(portal)) {
	elog(NOTICE, "PerformPortalFetch: portal \"%s\" not found",
	     name);
	return;
    }
    
    /* ----------------
     * 	switch into the portal context
     * ----------------
     */
    context= MemoryContextSwitchTo((MemoryContext)PortalGetHeapMemory(portal));
    
    AssertState(context ==
		(MemoryContext)PortalGetHeapMemory(GetPortalByName(NULL)));
    
    /* ----------------
     *  setup "feature" to tell the executor what direction and
     *  how many tuples to fetch.
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
    BeginCommand(name,
		 queryDesc->operation,
		 portal->attinfo,/* QueryDescGetTypeInfo(queryDesc), */
		 false,	/* portal fetches don't end up in relations */
		 false,	/* this is a portal fetch, not a "retrieve portal" */
		 tag,
		 dest);
    
    /* ----------------
     *	execute the portal fetch operation
     * ----------------
     */
    PortalExecutorHeapMemory = (MemoryContext)
	PortalGetHeapMemory(portal);
    
    ExecutorRun(queryDesc, PortalGetState(portal), feature, count);
    
    /* ----------------
     * Note: the "end-of-command" tag is returned by higher-level
     *	     utility code
     *
     * Return blank portal for now.
     * Otherwise, this named portal will be cleaned.
     * Note: portals will only be supported within a BEGIN...END
     * block in the near future.  Later, someone will fix it to
     * do what is possible across transaction boundries.
     * ----------------
     */
    MemoryContextSwitchTo(
	(MemoryContext)PortalGetHeapMemory(GetPortalByName(NULL)));
}

/* --------------------------------
 * 	PerformPortalClose
 * --------------------------------
 */
void
PerformPortalClose(char *name, CommandDest dest)
{
    Portal	portal;
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    if (name == NULL) {
	elog(NOTICE, "PerformPortalClose: blank portal unsupported");
	return;
    }
    
    /* ----------------
     *	get the portal from the portal name
     * ----------------
     */
    portal = GetPortalByName(name);
    if (! PortalIsValid(portal)) {
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
 *	PerformAddAttribute
 *
 *	adds an additional attribute to a relation
 *
 *	Adds attribute field(s) to a relation.  Each new attribute
 *	is given attnums in sequential order and is added to the
 *	ATTRIBUTE relation.  If the AMI fails, defunct tuples will
 *	remain in the ATTRIBUTE relation for later vacuuming.
 *	Later, there may be some reserved attribute names???
 *
 *	(If needed, can instead use elog to handle exceptions.)
 *
 *	Note:
 *		Initial idea of ordering the tuple attributes so that all
 *	the variable length domains occured last was scratched.  Doing
 *	so would not speed access too much (in general) and would create
 *	many complications in formtuple, amgetattr, and addattribute.
 *
 *	scan attribute catalog for name conflict (within rel)
 *	scan type catalog for absence of data type (if not arg)
 *	create attnum magically???
 *	create attribute tuple
 *	insert attribute in attribute catalog
 *	modify reldesc
 *	create new relation tuple
 *	insert new relation in relation catalog
 *	delete original relation from relation catalog
 * ----------------
 */
void
PerformAddAttribute(char *relationName,
		    char *userName,
		    bool inherits,
		    ColumnDef *colDef)
{	
    Relation		relrdesc, attrdesc;
    HeapScanDesc	attsdesc;
    HeapTuple		reltup;
    HeapTuple		attributeTuple;
    AttributeTupleForm	attribute;
    FormData_pg_attribute attributeD;
    int			i;
    int			minattnum, maxatts;
    HeapTuple		tup;
    ScanKeyData		key[2];
    ItemPointerData	oldTID;
    Relation		idescs[Num_pg_attr_indices];
    Relation		ridescs[Num_pg_class_indices];
    bool		hasindex;
    
    /*
     * permissions checking.  this would normally be done in utility.c,
     * but this particular routine is recursive.
     *
     * normally, only the owner of a class can change its schema.
     */
    if (IsSystemRelationName(relationName))
	elog(WARN, "PerformAddAttribute: class \"%s\" is a system catalog",
	     relationName);
#ifndef NO_SECURITY
    if (!pg_ownercheck(userName, relationName, RELNAME))
	elog(WARN, "PerformAddAttribute: you do not own class \"%s\"",
	     relationName);
#endif
    /*   
     * we can't add a not null attribute
     */
    if (colDef->is_not_null)
        elog(WARN,"Can't add a not null attribute to a existent relation");
    /*
     * if the first element in the 'schema' list is a "*" then we are
     * supposed to add this attribute to all classes that inherit from
     * 'relationName' (as well as to 'relationName').
     *
     * any permissions or problems with duplicate attributes will cause
     * the whole transaction to abort, which is what we want -- all or
     * nothing.
     */
    if (colDef != NULL) {
	if (inherits) {
	    Oid myrelid, childrelid;
	    List *child, *children;
	    
	    relrdesc = heap_openr(relationName);
	    if (!RelationIsValid(relrdesc)) {
		elog(WARN, "PerformAddAttribute: unknown relation: \"%s\"",
		     relationName);
	    }
	    myrelid = relrdesc->rd_id;
	    heap_close(relrdesc);
	    
	    /* this routine is actually in the planner */
	    children = find_all_inheritors(lconsi(myrelid,NIL), NIL);

	    /*
	     * find_all_inheritors does the recursive search of the
	     * inheritance hierarchy, so all we have to do is process
	     * all of the relids in the list that it returns.
	     */
	    foreach (child, children) {
		childrelid = lfirsti(child);
		if (childrelid == myrelid)
		    continue;
		relrdesc = heap_open(childrelid);
		if (!RelationIsValid(relrdesc)) {
		    elog(WARN, "PerformAddAttribute: can't find catalog entry for inheriting class with oid %d",
			 childrelid);
		}
		PerformAddAttribute((relrdesc->rd_rel->relname).data,
				    userName, false, colDef);
		heap_close(relrdesc);
	    }
	}
    }
    
    relrdesc = heap_openr(RelationRelationName);
    reltup = ClassNameIndexScan(relrdesc, relationName);
    
    if (!PointerIsValid(reltup)) {
	heap_close(relrdesc);
	elog(WARN, "PerformAddAttribute: relation \"%s\" not found",
	     relationName);
    }
    /*
     * XXX is the following check sufficient?
     */
    if (((Form_pg_class) GETSTRUCT(reltup))->relkind == RELKIND_INDEX) {
	elog(WARN, "PerformAddAttribute: index relation \"%s\" not changed",
	     relationName);
	return;
    }
    
    minattnum = ((Form_pg_class) GETSTRUCT(reltup))->relnatts;
    maxatts = minattnum + 1;
    if (maxatts > MaxHeapAttributeNumber) {
	pfree(reltup);				/* XXX temp */
	heap_close(relrdesc);			/* XXX temp */
	elog(WARN, "PerformAddAttribute: relations limited to %d attributes",
	     MaxHeapAttributeNumber);
	return;
    }
    
    attrdesc = heap_openr(AttributeRelationName);
    
    Assert(attrdesc);
    Assert(RelationGetRelationTupleForm(attrdesc));
    
    /*
     * Open all (if any) pg_attribute indices
     */
    hasindex = RelationGetRelationTupleForm(attrdesc)->relhasindex;
    if (hasindex)
	CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);
    
    ScanKeyEntryInitialize(&key[0], 
			   (bits16) NULL,
			   (AttrNumber) Anum_pg_attribute_attrelid,
			   (RegProcedure)ObjectIdEqualRegProcedure,
			   (Datum) reltup->t_oid);
    
    ScanKeyEntryInitialize(&key[1],
			   (bits16) NULL,
			   (AttrNumber) Anum_pg_attribute_attname,
                           (RegProcedure)NameEqualRegProcedure,
			   (Datum) NULL);
    
    attributeD.attrelid = reltup->t_oid;
    attributeD.attdefrel = InvalidOid;	/* XXX temporary */
    attributeD.attnvals = 0;		/* XXX temporary */
    attributeD.atttyparg = InvalidOid;	/* XXX temporary */
    attributeD.attbound = 0;		/* XXX temporary */
    attributeD.attcanindex = 0;		/* XXX need this info */
    attributeD.attproc = InvalidOid;	/* XXX tempoirary */
    attributeD.attcacheoff = -1;
    
    attributeTuple = heap_addheader(Natts_pg_attribute,
				    sizeof attributeD,
				    (char *)&attributeD);
    
    attribute = (AttributeTupleForm)GETSTRUCT(attributeTuple);
    
    i = 1 + minattnum;

    {
	HeapTuple	typeTuple;
	TypeTupleForm	form;
	char *p;
	int attnelems;
	
	/*
	 * XXX use syscache here as an optimization
	 */
	key[1].sk_argument = (Datum)colDef->colname;
 	attsdesc = heap_beginscan(attrdesc, 0, NowTimeQual, 2, key); 


	tup = heap_getnext(attsdesc, 0, (Buffer *) NULL);
	if (HeapTupleIsValid(tup)) {
	    pfree(reltup);			/* XXX temp */
	    heap_endscan(attsdesc);		/* XXX temp */
	    heap_close(attrdesc);		/* XXX temp */
	    heap_close(relrdesc);		/* XXX temp */
	    elog(WARN, "PerformAddAttribute: attribute \"%s\" already exists in class \"%s\"",
		 key[1].sk_argument,
		 relationName);
	    return;
	}
	heap_endscan(attsdesc);
	
	/*
	 * check to see if it is an array attribute.
	 */
	
	p = colDef->typename->name;
	
	if (colDef->typename->arrayBounds)
	    {
		attnelems = length(colDef->typename->arrayBounds);
		p = makeArrayTypeName(colDef->typename->name);
	    }
	else
	    attnelems = 0;
	
	typeTuple = SearchSysCacheTuple(TYPNAME, 
					PointerGetDatum(p),
					0,0,0);
	form = (TypeTupleForm)GETSTRUCT(typeTuple);
	
	if (!HeapTupleIsValid(typeTuple)) {
	    elog(WARN, "Add: type \"%s\" nonexistent", p);
	}
	namestrcpy(&(attribute->attname), (char*) key[1].sk_argument);
	attribute->atttypid = typeTuple->t_oid;
	if (colDef->typename->typlen > 0)
	    attribute->attlen = colDef->typename->typlen;
	else	/* bpchar, varchar, text */
	    attribute->attlen = form->typlen;
	attribute->attnum = i;
	attribute->attbyval = form->typbyval;
	attribute->attnelems = attnelems;
	attribute->attcacheoff = -1;
	attribute->attisset = (bool) (form->typtype == 'c');
	attribute->attalign = form->typalign;
        attribute->attnotnull = false;
	
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
    oldTID = reltup->t_ctid;
    heap_replace(relrdesc, &oldTID, reltup);
    
    /* keep catalog indices current */
    CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
    CatalogIndexInsert(ridescs, Num_pg_class_indices, relrdesc, reltup);
    CatalogCloseIndices(Num_pg_class_indices, ridescs);
    
    pfree(reltup);
    heap_close(relrdesc);
}
