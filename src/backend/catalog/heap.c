/*-------------------------------------------------------------------------
 *
 * heap.c--
 *	  code to create and destroy POSTGRES heap relations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/heap.c,v 1.51 1998/06/15 19:28:07 momjian Exp $
 *
 * INTERFACE ROUTINES
 *		heap_create()			- Create an uncataloged heap relation
 *		heap_create_with_catalog() - Create a cataloged relation
 *		heap_destroy_with_catalog()	- Removes named relation from catalogs
 *
 * NOTES
 *	  this code taken from access/heap/create.c, which contains
 *	  the old heap_create_with_catalogr, amcreate, and amdestroy.
 *	  those routines will soon call these routines using the function
 *	  manager,
 *	  just like the poorly named "NewXXX" routines do.	The
 *	  "New" routines are all going to die soon, once and for all!
 *		-cim 1/13/91
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_ipl.h"
#include "catalog/pg_relcheck.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/plannodes.h"
#include "optimizer/tlist.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteRemove.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/mcxt.h"
#include "utils/relcache.h"
#include "utils/tqual.h"

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

static void
AddPgRelationTuple(Relation pg_class_desc,
				 Relation new_rel_desc, Oid new_rel_oid, unsigned natts);
static void AddToTempRelList(Relation r);
static void DeletePgAttributeTuples(Relation rdesc);
static void DeletePgRelationTuple(Relation rdesc);
static void DeletePgTypeTuple(Relation rdesc);
static int	RelationAlreadyExists(Relation pg_class_desc, char relname[]);
static void RelationRemoveIndexes(Relation relation);
static void RelationRemoveInheritance(Relation relation);
static void RemoveFromTempRelList(Relation r);
static void addNewRelationType(char *typeName, Oid new_rel_oid);
static void StoreConstraints(Relation rel);
static void RemoveConstraints(Relation rel);


/* ----------------------------------------------------------------
 *				XXX UGLY HARD CODED BADNESS FOLLOWS XXX
 *
 *		these should all be moved to someplace in the lib/catalog
 *		module, if not obliterated first.
 * ----------------------------------------------------------------
 */


/*
 * Note:
 *		Should the executor special case these attributes in the future?
 *		Advantage:	consume 1/2 the space in the ATTRIBUTE relation.
 *		Disadvantage:  having rules to compute values in these tuples may
 *				be more difficult if not impossible.
 */

static FormData_pg_attribute a1 = {
	0xffffffff, {"ctid"}, 27l, 0l, sizeof(ItemPointerData),
	SelfItemPointerAttributeNumber, 0, -1, -1, '\0', '\0', 'i', '\0', '\0'
};

static FormData_pg_attribute a2 = {
	0xffffffff, {"oid"}, 26l, 0l, sizeof(Oid),
	ObjectIdAttributeNumber, 0, -1, -1, '\001', '\0', 'i', '\0', '\0'
};

static FormData_pg_attribute a3 = {
	0xffffffff, {"xmin"}, 28l, 0l, sizeof(TransactionId),
	MinTransactionIdAttributeNumber, 0, -1, -1, '\0', '\0', 'i', '\0', '\0'
};

static FormData_pg_attribute a4 = {
	0xffffffff, {"cmin"}, 29l, 0l, sizeof(CommandId),
	MinCommandIdAttributeNumber, 0, -1, -1, '\001', '\0', 'i', '\0', '\0'
};

static FormData_pg_attribute a5 = {
	0xffffffff, {"xmax"}, 28l, 0l, sizeof(TransactionId),
	MaxTransactionIdAttributeNumber, 0, -1, -1, '\0', '\0', 'i', '\0', '\0'
};

static FormData_pg_attribute a6 = {
	0xffffffff, {"cmax"}, 29l, 0l, sizeof(CommandId),
	MaxCommandIdAttributeNumber, 0, -1, -1, '\001', '\0', 'i', '\0', '\0'
};

static AttributeTupleForm HeapAtt[] =
{&a1, &a2, &a3, &a4, &a5, &a6};

/* ----------------------------------------------------------------
 *				XXX END OF UGLY HARD CODED BADNESS XXX
 * ----------------------------------------------------------------
 */

/* the tempRelList holds
   the list of temporary uncatalogued relations that are created.
   these relations should be destroyed at the end of transactions
*/
typedef struct tempRelList
{
	Relation   *rels;			/* array of relation descriptors */
	int			num;			/* number of temporary relations */
	int			size;			/* size of space allocated for the rels
								 * array */
} TempRelList;

#define TEMP_REL_LIST_SIZE	32

static TempRelList *tempRels = NULL;


/* ----------------------------------------------------------------
 *		heap_create		- Create an uncataloged heap relation
 *
 *		Fields relpages, reltuples, reltuples, relkeys, relhistory,
 *		relisindexed, and relkind of rdesc->rd_rel are initialized
 *		to all zeros, as are rd_last and rd_hook.  Rd_refcnt is set to 1.
 *
 *		Remove the system relation specific code to elsewhere eventually.
 *
 *		Eventually, must place information about this temporary relation
 *		into the transaction context block.
 *
 *
 * if heap_create is called with "" as the name, then heap_create will create
 * a temporary name "temp_$RELOID" for the relation
 * ----------------------------------------------------------------
 */
Relation
heap_create(char *name,
			TupleDesc tupDesc)
{
	unsigned	i;
	Oid			relid;
	Relation	rdesc;
	int			len;
	bool		nailme = false;
	char	   *relname = name;
	char		tempname[40];
	int			isTemp = 0;
	int			natts = tupDesc->natts;

/*	  AttributeTupleForm *att = tupDesc->attrs; */

	extern GlobalMemory CacheCxt;
	MemoryContext oldcxt;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	AssertArg(natts > 0);

	if (IsSystemRelationName(relname) && IsNormalProcessingMode())
	{
		elog(ERROR,
		 "Illegal class name: %s -- pg_ is reserved for system catalogs",
			 relname);
	}

	/* ----------------
	 *	switch to the cache context so that we don't lose
	 *	allocations at the end of this transaction, I guess.
	 *	-cim 6/14/90
	 * ----------------
	 */
	if (!CacheCxt)
		CacheCxt = CreateGlobalMemory("Cache");

	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

	/* ----------------
	 *	real ugly stuff to assign the proper relid in the relation
	 *	descriptor follows.
	 * ----------------
	 */
	if (!strcmp(RelationRelationName, relname))
	{
		relid = RelOid_pg_class;
		nailme = true;
	}
	else if (!strcmp(AttributeRelationName, relname))
	{
		relid = RelOid_pg_attribute;
		nailme = true;
	}
	else if (!strcmp(ProcedureRelationName, relname))
	{
		relid = RelOid_pg_proc;
		nailme = true;
	}
	else if (!strcmp(TypeRelationName, relname))
	{
		relid = RelOid_pg_type;
		nailme = true;
	}
	else
	{
		relid = newoid();

		if (name[0] == '\0')
		{
			sprintf(tempname, "temp_%d", relid);
			relname = tempname;
			isTemp = 1;
		}
	}

	/* ----------------
	 *	allocate a new relation descriptor.
	 *
	 *	XXX the length computation may be incorrect, handle elsewhere
	 * ----------------
	 */
	len = sizeof(RelationData);

	rdesc = (Relation) palloc(len);
	MemSet((char *) rdesc, 0, len);

	/* ----------
	   create a new tuple descriptor from the one passed in
	*/
	rdesc->rd_att = CreateTupleDescCopyConstr(tupDesc);

	/* ----------------
	 *	initialize the fields of our new relation descriptor
	 * ----------------
	 */

	/* ----------------
	 *	nail the reldesc if this is a bootstrap create reln and
	 *	we may need it in the cache later on in the bootstrap
	 *	process so we don't ever want it kicked out.  e.g. pg_attribute!!!
	 * ----------------
	 */
	if (nailme)
		rdesc->rd_isnailed = true;

	RelationSetReferenceCount(rdesc, 1);

	rdesc->rd_rel = (Form_pg_class) palloc(sizeof *rdesc->rd_rel);

	MemSet((char *) rdesc->rd_rel, 0,
		   sizeof *rdesc->rd_rel);
	namestrcpy(&(rdesc->rd_rel->relname), relname);
	rdesc->rd_rel->relkind = RELKIND_UNCATALOGED;
	rdesc->rd_rel->relnatts = natts;
	if (tupDesc->constr)
		rdesc->rd_rel->relchecks = tupDesc->constr->num_check;

	for (i = 0; i < natts; i++)
		rdesc->rd_att->attrs[i]->attrelid = relid;

	rdesc->rd_id = relid;

	if (nailme)
	{
		/* for system relations, set the reltype field here */
		rdesc->rd_rel->reltype = relid;
	}

	/* ----------------
	 *	remember if this is a temp relation
	 * ----------------
	 */

	rdesc->rd_istemp = isTemp;

	/* ----------------
	 *	have the storage manager create the relation.
	 * ----------------
	 */

	rdesc->rd_tmpunlinked = TRUE;		/* change once table is created */
	rdesc->rd_fd = (File) smgrcreate(DEFAULT_SMGR, rdesc);
	rdesc->rd_tmpunlinked = FALSE;

	RelationRegisterRelation(rdesc);

	MemoryContextSwitchTo(oldcxt);

	/*
	 * add all temporary relations to the tempRels list so they can be
	 * properly disposed of at the end of transaction
	 */
	if (isTemp)
		AddToTempRelList(rdesc);

	return (rdesc);
}


/* ----------------------------------------------------------------
 *		heap_create_with_catalog		- Create a cataloged relation
 *
 *		this is done in 6 steps:
 *
 *		1) CheckAttributeNames() is used to make certain the tuple
 *		   descriptor contains a valid set of attribute names
 *
 *		2) pg_class is opened and RelationAlreadyExists()
 *		   preforms a scan to ensure that no relation with the
 *		   same name already exists.
 *
 *		3) heap_create_with_catalogr() is called to create the new relation
 *		   on disk.
 *
 *		4) TypeDefine() is called to define a new type corresponding
 *		   to the new relation.
 *
 *		5) AddNewAttributeTuples() is called to register the
 *		   new relation's schema in pg_attribute.
 *
 *		6) AddPgRelationTuple() is called to register the
 *		   relation itself in the catalogs.
 *
 *		7) StoreConstraints is called ()		- vadim 08/22/97
 *
 *		8) the relations are closed and the new relation's oid
 *		   is returned.
 *
 * old comments:
 *		A new relation is inserted into the RELATION relation
 *		with the specified attribute(s) (newly inserted into
 *		the ATTRIBUTE relation).  How does concurrency control
 *		work?  Is it automatic now?  Expects the caller to have
 *		attname, atttypid, atttyparg, attproc, and attlen domains filled.
 *		Create fills the attnum domains sequentually from zero,
 *		fills the attdisbursion domains with zeros, and fills the
 *		attrelid fields with the relid.
 *
 *		scan relation catalog for name conflict
 *		scan type catalog for typids (if not arg)
 *		create and insert attribute(s) into attribute catalog
 *		create new relation
 *		insert new relation into attribute catalog
 *
 *		Should coordinate with heap_create_with_catalogr(). Either
 *		it should not be called or there should be a way to prevent
 *		the relation from being removed at the end of the
 *		transaction if it is successful ('u'/'r' may be enough).
 *		Also, if the transaction does not commit, then the
 *		relation should be removed.
 *
 *		XXX amcreate ignores "off" when inserting (for now).
 *		XXX amcreate (like the other utilities) needs to understand indexes.
 *
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		CheckAttributeNames
 *
 *		this is used to make certain the tuple descriptor contains a
 *		valid set of attribute names.  a problem simply generates
 *		elog(ERROR) which aborts the current transaction.
 * --------------------------------
 */
static void
CheckAttributeNames(TupleDesc tupdesc)
{
	unsigned	i;
	unsigned	j;
	int			natts = tupdesc->natts;

	/* ----------------
	 *	first check for collision with system attribute names
	 * ----------------
	 *
	 *	 also, warn user if attribute to be created has
	 *	 an unknown typid  (usually as a result of a 'retrieve into'
	 *	  - jolly
	 */
	for (i = 0; i < natts; i += 1)
	{
		for (j = 0; j < sizeof HeapAtt / sizeof HeapAtt[0]; j += 1)
		{
			if (nameeq(&(HeapAtt[j]->attname),
					   &(tupdesc->attrs[i]->attname)))
			{
				elog(ERROR,
					 "create: system attribute named \"%s\"",
					 HeapAtt[j]->attname.data);
			}
		}
		if (tupdesc->attrs[i]->atttypid == UNKNOWNOID)
		{
			elog(NOTICE,
				 "create: attribute named \"%s\" has an unknown type",
				 tupdesc->attrs[i]->attname.data);
		}
	}

	/* ----------------
	 *	next check for repeated attribute names
	 * ----------------
	 */
	for (i = 1; i < natts; i += 1)
	{
		for (j = 0; j < i; j += 1)
		{
			if (nameeq(&(tupdesc->attrs[j]->attname),
					   &(tupdesc->attrs[i]->attname)))
			{
				elog(ERROR,
					 "create: repeated attribute \"%s\"",
					 tupdesc->attrs[j]->attname.data);
			}
		}
	}
}

/* --------------------------------
 *		RelationAlreadyExists
 *
 *		this preforms a scan of pg_class to ensure that
 *		no relation with the same name already exists.	The caller
 *		has to open pg_class and pass an open descriptor.
 * --------------------------------
 */
static int
RelationAlreadyExists(Relation pg_class_desc, char relname[])
{
	ScanKeyData key;
	HeapScanDesc pg_class_scan;
	HeapTuple	tup;

	/*
	 * If this is not bootstrap (initdb) time, use the catalog index on
	 * pg_class.
	 */

	if (!IsBootstrapProcessingMode())
	{
		tup = ClassNameIndexScan(pg_class_desc, relname);
		if (HeapTupleIsValid(tup))
		{
			pfree(tup);
			return ((int) true);
		}
		else
			return ((int) false);
	}

	/* ----------------
	 *	At bootstrap time, we have to do this the hard way.  Form the
	 *	scan key.
	 * ----------------
	 */
	ScanKeyEntryInitialize(&key,
						   0,
						   (AttrNumber) Anum_pg_class_relname,
						   (RegProcedure) F_NAMEEQ,
						   (Datum) relname);

	/* ----------------
	 *	begin the scan
	 * ----------------
	 */
	pg_class_scan = heap_beginscan(pg_class_desc,
								   0,
								   false,
								   1,
								   &key);

	/* ----------------
	 *	get a tuple.  if the tuple is NULL then it means we
	 *	didn't find an existing relation.
	 * ----------------
	 */
	tup = heap_getnext(pg_class_scan, 0, (Buffer *) NULL);

	/* ----------------
	 *	end the scan and return existance of relation.
	 * ----------------
	 */
	heap_endscan(pg_class_scan);

	return
		(PointerIsValid(tup) == true);
}

/* --------------------------------
 *		AddNewAttributeTuples
 *
 *		this registers the new relation's schema by adding
 *		tuples to pg_attribute.
 * --------------------------------
 */
static void
AddNewAttributeTuples(Oid new_rel_oid,
					  TupleDesc tupdesc)
{
	AttributeTupleForm *dpp;
	unsigned	i;
	HeapTuple	tup;
	Relation	rdesc;
	bool		hasindex;
	Relation	idescs[Num_pg_attr_indices];
	int			natts = tupdesc->natts;

	/* ----------------
	 *	open pg_attribute
	 * ----------------
	 */
	rdesc = heap_openr(AttributeRelationName);

	/* -----------------
	 * Check if we have any indices defined on pg_attribute.
	 * -----------------
	 */
	Assert(rdesc);
	Assert(rdesc->rd_rel);
	hasindex = RelationGetRelationTupleForm(rdesc)->relhasindex;
	if (hasindex)
		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);

	/* ----------------
	 *	initialize tuple descriptor.  Note we use setheapoverride()
	 *	so that we can see the effects of our TypeDefine() done
	 *	previously.
	 * ----------------
	 */
	setheapoverride(true);
	fillatt(tupdesc);
	setheapoverride(false);

	/* ----------------
	 *	first we add the user attributes..
	 * ----------------
	 */
	dpp = tupdesc->attrs;
	for (i = 0; i < natts; i++)
	{
		(*dpp)->attrelid = new_rel_oid;
		(*dpp)->attdisbursion = 0;

		tup = heap_addheader(Natts_pg_attribute,
							 ATTRIBUTE_TUPLE_SIZE,
							 (char *) *dpp);

		heap_insert(rdesc, tup);
		if (hasindex)
			CatalogIndexInsert(idescs, Num_pg_attr_indices, rdesc, tup);

		pfree(tup);
		dpp++;
	}

	/* ----------------
	 *	next we add the system attributes..
	 * ----------------
	 */
	dpp = HeapAtt;
	for (i = 0; i < -1 - FirstLowInvalidHeapAttributeNumber; i++)
	{
		(*dpp)->attrelid = new_rel_oid;
		/* (*dpp)->attdisbursion = 0;	   unneeded */

		tup = heap_addheader(Natts_pg_attribute,
							 ATTRIBUTE_TUPLE_SIZE,
							 (char *) *dpp);

		heap_insert(rdesc, tup);

		if (hasindex)
			CatalogIndexInsert(idescs, Num_pg_attr_indices, rdesc, tup);

		pfree(tup);
		dpp++;
	}

	heap_close(rdesc);

	/*
	 * close pg_attribute indices
	 */
	if (hasindex)
		CatalogCloseIndices(Num_pg_attr_indices, idescs);
}

/* --------------------------------
 *		AddPgRelationTuple
 *
 *		this registers the new relation in the catalogs by
 *		adding a tuple to pg_class.
 * --------------------------------
 */
static void
AddPgRelationTuple(Relation pg_class_desc,
				   Relation new_rel_desc,
				   Oid new_rel_oid,
				   unsigned natts)
{
	Form_pg_class new_rel_reltup;
	HeapTuple	tup;
	Relation	idescs[Num_pg_class_indices];
	bool		isBootstrap;
	extern bool ItsSequenceCreation;	/* It's hack, I know... - vadim
										 * 03/28/97		*/

	/* ----------------
	 *	first we munge some of the information in our
	 *	uncataloged relation's relation descriptor.
	 * ----------------
	 */
	new_rel_reltup = new_rel_desc->rd_rel;

	/* CHECK should get new_rel_oid first via an insert then use XXX */
	/* new_rel_reltup->reltuples = 1; *//* XXX */

	new_rel_reltup->relowner = GetUserId();
	if (ItsSequenceCreation)
		new_rel_reltup->relkind = RELKIND_SEQUENCE;
	else
		new_rel_reltup->relkind = RELKIND_RELATION;
	new_rel_reltup->relnatts = natts;

	/* ----------------
	 *	now form a tuple to add to pg_class
	 *	XXX Natts_pg_class_fixed is a hack - see pg_class.h
	 * ----------------
	 */
	tup = heap_addheader(Natts_pg_class_fixed,
						 CLASS_TUPLE_SIZE,
						 (char *) new_rel_reltup);
	tup->t_oid = new_rel_oid;

	/* ----------------
	 *	finally insert the new tuple and free it.
	 *
	 *	Note: I have no idea why we do a
	 *			SetProcessingMode(BootstrapProcessing);
	 *		  here -cim 6/14/90
	 * ----------------
	 */
	isBootstrap = IsBootstrapProcessingMode() ? true : false;

	SetProcessingMode(BootstrapProcessing);

	heap_insert(pg_class_desc, tup);

	if (!isBootstrap)
	{

		/*
		 * First, open the catalog indices and insert index tuples for the
		 * new relation.
		 */

		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_class_indices, pg_class_desc, tup);
		CatalogCloseIndices(Num_pg_class_indices, idescs);

		/* now restore processing mode */
		SetProcessingMode(NormalProcessing);
	}

	pfree(tup);
}


/* --------------------------------
 *		addNewRelationType -
 *
 *		define a complex type corresponding to the new relation
 * --------------------------------
 */
static void
addNewRelationType(char *typeName, Oid new_rel_oid)
{
	Oid			new_type_oid;

	/*
	 * The sizes are set to oid size because it makes implementing sets
	 * MUCH easier, and no one (we hope) uses these fields to figure out
	 * how much space to allocate for the type. An oid is the type used
	 * for a set definition.  When a user requests a set, what they
	 * actually get is the oid of a tuple in the pg_proc catalog, so the
	 * size of the "set" is the size of an oid. Similarly, byval being
	 * true makes sets much easier, and it isn't used by anything else.
	 * Note the assumption that OIDs are the same size as int4s.
	 */
	new_type_oid = TypeCreate(typeName, /* type name */
							  new_rel_oid,		/* relation oid */
							  typeLen(typeidType(OIDOID)),		/* internal size */
							  typeLen(typeidType(OIDOID)),		/* external size */
							  'c',		/* type-type (catalog) */
							  ',',		/* default array delimiter */
							  "int4in", /* input procedure */
							  "int4out",		/* output procedure */
							  "int4in", /* receive procedure */
							  "int4out",		/* send procedure */
							  NULL,		/* array element type - irrelevent */
							  "-",		/* default type value */
							  (bool) 1, /* passed by value */
							  'i');		/* default alignment */
}

/* --------------------------------
 *		heap_create_with_catalog
 *
 *		creates a new cataloged relation.  see comments above.
 * --------------------------------
 */
Oid
heap_create_with_catalog(char relname[],
						 TupleDesc tupdesc)
{
	Relation	pg_class_desc;
	Relation	new_rel_desc;
	Oid			new_rel_oid;

	int			natts = tupdesc->natts;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	AssertState(IsNormalProcessingMode() || IsBootstrapProcessingMode());
	if (natts == 0 || natts > MaxHeapAttributeNumber)
		elog(ERROR, "amcreate: from 1 to %d attributes must be specified",
			 MaxHeapAttributeNumber);

	CheckAttributeNames(tupdesc);

	/* ----------------
	 *	open pg_class and see that the relation doesn't
	 *	already exist.
	 * ----------------
	 */
	pg_class_desc = heap_openr(RelationRelationName);

	if (RelationAlreadyExists(pg_class_desc, relname))
	{
		heap_close(pg_class_desc);
		elog(ERROR, "amcreate: %s relation already exists", relname);
	}

	/* ----------------
	 *	ok, relation does not already exist so now we
	 *	create an uncataloged relation and pull its relation oid
	 *	from the newly formed relation descriptor.
	 *
	 *	Note: The call to heap_create() does all the "real" work
	 *	of creating the disk file for the relation.
	 * ----------------
	 */
	new_rel_desc = heap_create(relname, tupdesc);
	new_rel_oid = new_rel_desc->rd_att->attrs[0]->attrelid;

	/* ----------------
	 *	since defining a relation also defines a complex type,
	 *	we add a new system type corresponding to the new relation.
	 * ----------------
	 */
	addNewRelationType(relname, new_rel_oid);

	/* ----------------
	 *	now add tuples to pg_attribute for the attributes in
	 *	our new relation.
	 * ----------------
	 */
	AddNewAttributeTuples(new_rel_oid, tupdesc);

	/* ----------------
	 *	now update the information in pg_class.
	 * ----------------
	 */
	AddPgRelationTuple(pg_class_desc,
					   new_rel_desc,
					   new_rel_oid,
					   natts);

	StoreConstraints(new_rel_desc);

	/* ----------------
	 *	ok, the relation has been cataloged, so close our relations
	 *	and return the oid of the newly created relation.
	 *
	 *	SOMEDAY: fill the STATISTIC relation properly.
	 * ----------------
	 */
	heap_close(new_rel_desc);
	heap_close(pg_class_desc);

	return new_rel_oid;
}


/* ----------------------------------------------------------------
 *		heap_destroy_with_catalog	- removes all record of named relation from catalogs
 *
 *		1)	open relation, check for existence, etc.
 *		2)	remove inheritance information
 *		3)	remove indexes
 *		4)	remove pg_class tuple
 *		5)	remove pg_attribute tuples
 *		6)	remove pg_type tuples
 *		7)	RemoveConstraints ()
 *		8)	unlink relation
 *
 * old comments
 *		Except for vital relations, removes relation from
 *		relation catalog, and related attributes from
 *		attribute catalog (needed?).  (Anything else?)
 *
 *		get proper relation from relation catalog (if not arg)
 *		check if relation is vital (strcmp()/reltype?)
 *		scan attribute catalog deleting attributes of reldesc
 *				(necessary?)
 *		delete relation from relation catalog
 *		(How are the tuples of the relation discarded?)
 *
 *		XXX Must fix to work with indexes.
 *		There may be a better order for doing things.
 *		Problems with destroying a deleted database--cannot create
 *		a struct reldesc without having an open file descriptor.
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		RelationRemoveInheritance
 *
 *		Note: for now, we cause an exception if relation is a
 *		superclass.  Someday, we may want to allow this and merge
 *		the type info into subclass procedures....	this seems like
 *		lots of work.
 * --------------------------------
 */
static void
RelationRemoveInheritance(Relation relation)
{
	Relation	catalogRelation;
	HeapTuple	tuple;
	HeapScanDesc scan;
	ScanKeyData entry;

	/* ----------------
	 *	open pg_inherits
	 * ----------------
	 */
	catalogRelation = heap_openr(InheritsRelationName);

	/* ----------------
	 *	form a scan key for the subclasses of this class
	 *	and begin scanning
	 * ----------------
	 */
	ScanKeyEntryInitialize(&entry, 0x0, Anum_pg_inherits_inhparent,
						   F_OIDEQ,
					  ObjectIdGetDatum(RelationGetRelationId(relation)));

	scan = heap_beginscan(catalogRelation,
						  false,
						  false,
						  1,
						  &entry);

	/* ----------------
	 *	if any subclasses exist, then we disallow the deletion.
	 * ----------------
	 */
	tuple = heap_getnext(scan, 0, (Buffer *) NULL);
	if (HeapTupleIsValid(tuple))
	{
		heap_endscan(scan);
		heap_close(catalogRelation);

		elog(ERROR, "relation <%d> inherits \"%s\"",
			 ((InheritsTupleForm) GETSTRUCT(tuple))->inhrel,
			 RelationGetRelationName(relation));
	}

	/* ----------------
	 *	If we get here, it means the relation has no subclasses
	 *	so we can trash it.  First we remove dead INHERITS tuples.
	 * ----------------
	 */
	entry.sk_attno = Anum_pg_inherits_inhrel;

	scan = heap_beginscan(catalogRelation,
						  false,
						  false,
						  1,
						  &entry);

	for (;;)
	{
		tuple = heap_getnext(scan, 0, (Buffer *) NULL);
		if (!HeapTupleIsValid(tuple))
			break;
		heap_delete(catalogRelation, &tuple->t_ctid);
	}

	heap_endscan(scan);
	heap_close(catalogRelation);

	/* ----------------
	 *	now remove dead IPL tuples
	 * ----------------
	 */
	catalogRelation =
		heap_openr(InheritancePrecidenceListRelationName);

	entry.sk_attno = Anum_pg_ipl_iplrel;

	scan = heap_beginscan(catalogRelation,
						  false,
						  false,
						  1,
						  &entry);

	for (;;)
	{
		tuple = heap_getnext(scan, 0, (Buffer *) NULL);
		if (!HeapTupleIsValid(tuple))
			break;
		heap_delete(catalogRelation, &tuple->t_ctid);
	}

	heap_endscan(scan);
	heap_close(catalogRelation);
}

/* --------------------------------
 *		RelationRemoveIndexes
 *
 * --------------------------------
 */
static void
RelationRemoveIndexes(Relation relation)
{
	Relation	indexRelation;
	HeapTuple	tuple;
	HeapScanDesc scan;
	ScanKeyData entry;

	indexRelation = heap_openr(IndexRelationName);

	ScanKeyEntryInitialize(&entry, 0x0, Anum_pg_index_indrelid,
						   F_OIDEQ,
					  ObjectIdGetDatum(RelationGetRelationId(relation)));

	scan = heap_beginscan(indexRelation,
						  false,
						  false,
						  1,
						  &entry);

	for (;;)
	{
		tuple = heap_getnext(scan, 0, (Buffer *) NULL);
		if (!HeapTupleIsValid(tuple))
			break;

		index_destroy(((IndexTupleForm) GETSTRUCT(tuple))->indexrelid);
	}

	heap_endscan(scan);
	heap_close(indexRelation);
}

/* --------------------------------
 *		DeletePgRelationTuple
 *
 * --------------------------------
 */
static void
DeletePgRelationTuple(Relation rdesc)
{
	Relation	pg_class_desc;
	HeapScanDesc pg_class_scan;
	ScanKeyData key;
	HeapTuple	tup;

	/* ----------------
	 *	open pg_class
	 * ----------------
	 */
	pg_class_desc = heap_openr(RelationRelationName);

	/* ----------------
	 *	create a scan key to locate the relation oid of the
	 *	relation to delete
	 * ----------------
	 */
	ScanKeyEntryInitialize(&key, 0, ObjectIdAttributeNumber,
						   F_INT4EQ, rdesc->rd_att->attrs[0]->attrelid);

	pg_class_scan = heap_beginscan(pg_class_desc,
								   0,
								   false,
								   1,
								   &key);

	/* ----------------
	 *	use heap_getnext() to fetch the pg_class tuple.  If this
	 *	tuple is not valid then something's wrong.
	 * ----------------
	 */
	tup = heap_getnext(pg_class_scan, 0, (Buffer *) NULL);

	if (!PointerIsValid(tup))
	{
		heap_endscan(pg_class_scan);
		heap_close(pg_class_desc);
		elog(ERROR, "DeletePgRelationTuple: %s relation nonexistent",
			 &rdesc->rd_rel->relname);
	}

	/* ----------------
	 *	delete the relation tuple from pg_class, and finish up.
	 * ----------------
	 */
	heap_endscan(pg_class_scan);
	heap_delete(pg_class_desc, &tup->t_ctid);

	heap_close(pg_class_desc);
}

/* --------------------------------
 *		DeletePgAttributeTuples
 *
 * --------------------------------
 */
static void
DeletePgAttributeTuples(Relation rdesc)
{
	Relation	pg_attribute_desc;
	HeapScanDesc pg_attribute_scan;
	ScanKeyData key;
	HeapTuple	tup;

	/* ----------------
	 *	open pg_attribute
	 * ----------------
	 */
	pg_attribute_desc = heap_openr(AttributeRelationName);

	/* ----------------
	 *	create a scan key to locate the attribute tuples to delete
	 *	and begin the scan.
	 * ----------------
	 */
	ScanKeyEntryInitialize(&key, 0, Anum_pg_attribute_attrelid,
						   F_INT4EQ, rdesc->rd_att->attrs[0]->attrelid);

	/* -----------------
	 * Get a write lock _before_ getting the read lock in the scan
	 * ----------------
	 */
	RelationSetLockForWrite(pg_attribute_desc);

	pg_attribute_scan = heap_beginscan(pg_attribute_desc,
									   0,
									   false,
									   1,
									   &key);

	/* ----------------
	 *	use heap_getnext() / amdelete() until all attribute tuples
	 *	have been deleted.
	 * ----------------
	 */
	while (tup = heap_getnext(pg_attribute_scan, 0, (Buffer *) NULL),
		   PointerIsValid(tup))
	{

		heap_delete(pg_attribute_desc, &tup->t_ctid);
	}

	/* ----------------
	 *	finish up.
	 * ----------------
	 */
	heap_endscan(pg_attribute_scan);

	/* ----------------
	 * Release the write lock
	 * ----------------
	 */
	RelationUnsetLockForWrite(pg_attribute_desc);
	heap_close(pg_attribute_desc);
}


/* --------------------------------
 *		DeletePgTypeTuple
 *
 *		If the user attempts to destroy a relation and there
 *		exists attributes in other relations of type
 *		"relation we are deleting", then we have to do something
 *		special.  presently we disallow the destroy.
 * --------------------------------
 */
static void
DeletePgTypeTuple(Relation rdesc)
{
	Relation	pg_type_desc;
	HeapScanDesc pg_type_scan;
	Relation	pg_attribute_desc;
	HeapScanDesc pg_attribute_scan;
	ScanKeyData key;
	ScanKeyData attkey;
	HeapTuple	tup;
	HeapTuple	atttup;
	Oid			typoid;

	/* ----------------
	 *	open pg_type
	 * ----------------
	 */
	pg_type_desc = heap_openr(TypeRelationName);

	/* ----------------
	 *	create a scan key to locate the type tuple corresponding
	 *	to this relation.
	 * ----------------
	 */
	ScanKeyEntryInitialize(&key, 0, Anum_pg_type_typrelid, F_INT4EQ,
						   rdesc->rd_att->attrs[0]->attrelid);

	pg_type_scan = heap_beginscan(pg_type_desc,
								  0,
								  false,
								  1,
								  &key);

	/* ----------------
	 *	use heap_getnext() to fetch the pg_type tuple.	If this
	 *	tuple is not valid then something's wrong.
	 * ----------------
	 */
	tup = heap_getnext(pg_type_scan, 0, (Buffer *) NULL);

	if (!PointerIsValid(tup))
	{
		heap_endscan(pg_type_scan);
		heap_close(pg_type_desc);
		elog(ERROR, "DeletePgTypeTuple: %s type nonexistent",
			 &rdesc->rd_rel->relname);
	}

	/* ----------------
	 *	now scan pg_attribute.	if any other relations have
	 *	attributes of the type of the relation we are deleteing
	 *	then we have to disallow the deletion.	should talk to
	 *	stonebraker about this.  -cim 6/19/90
	 * ----------------
	 */
	typoid = tup->t_oid;

	pg_attribute_desc = heap_openr(AttributeRelationName);

	ScanKeyEntryInitialize(&attkey,
						   0, Anum_pg_attribute_atttypid, F_INT4EQ,
						   typoid);

	pg_attribute_scan = heap_beginscan(pg_attribute_desc,
									   0,
									   false,
									   1,
									   &attkey);

	/* ----------------
	 *	try and get a pg_attribute tuple.  if we succeed it means
	 *	we cant delete the relation because something depends on
	 *	the schema.
	 * ----------------
	 */
	atttup = heap_getnext(pg_attribute_scan, 0, (Buffer *) NULL);

	if (PointerIsValid(atttup))
	{
		Oid			relid = ((AttributeTupleForm) GETSTRUCT(atttup))->attrelid;

		heap_endscan(pg_type_scan);
		heap_close(pg_type_desc);
		heap_endscan(pg_attribute_scan);
		heap_close(pg_attribute_desc);

		elog(ERROR, "DeletePgTypeTuple: att of type %s exists in relation %d",
			 &rdesc->rd_rel->relname, relid);
	}
	heap_endscan(pg_attribute_scan);
	heap_close(pg_attribute_desc);

	/* ----------------
	 *	Ok, it's safe so we delete the relation tuple
	 *	from pg_type and finish up.  But first end the scan so that
	 *	we release the read lock on pg_type.  -mer 13 Aug 1991
	 * ----------------
	 */
	heap_endscan(pg_type_scan);
	heap_delete(pg_type_desc, &tup->t_ctid);

	heap_close(pg_type_desc);
}

/* --------------------------------
 *		heap_destroy_with_catalog
 *
 * --------------------------------
 */
void
heap_destroy_with_catalog(char *relname)
{
	Relation	rdesc;
	Oid			rid;

	/* ----------------
	 *	first open the relation.  if the relation does exist,
	 *	heap_openr() returns NULL.
	 * ----------------
	 */
	rdesc = heap_openr(relname);
	if (rdesc == NULL)
		elog(ERROR, "Relation %s Does Not Exist!", relname);

	RelationSetLockForWrite(rdesc);
	rid = rdesc->rd_id;

	/* ----------------
	 *	prevent deletion of system relations
	 * ----------------
	 */
	if (IsSystemRelationName(RelationGetRelationName(rdesc)->data))
		elog(ERROR, "amdestroy: cannot destroy %s relation",
			 &rdesc->rd_rel->relname);

	/* ----------------
	 *	remove inheritance information
	 * ----------------
	 */
	RelationRemoveInheritance(rdesc);

	/* ----------------
	 *	remove indexes if necessary
	 * ----------------
	 */
	if (rdesc->rd_rel->relhasindex)
		RelationRemoveIndexes(rdesc);

	/* ----------------
	 *	remove rules if necessary
	 * ----------------
	 */
	if (rdesc->rd_rules != NULL)
		RelationRemoveRules(rid);

	/* triggers */
	if (rdesc->rd_rel->reltriggers > 0)
		RelationRemoveTriggers(rdesc);

	/* ----------------
	 *	delete attribute tuples
	 * ----------------
	 */
	DeletePgAttributeTuples(rdesc);

	/* ----------------
	 *	delete type tuple.	here we want to see the effects
	 *	of the deletions we just did, so we use setheapoverride().
	 * ----------------
	 */
	setheapoverride(true);
	DeletePgTypeTuple(rdesc);
	setheapoverride(false);

	/* ----------------
	 *	delete relation tuple
	 * ----------------
	 */
	DeletePgRelationTuple(rdesc);

	/*
	 * release dirty buffers of this relation
	 */
	ReleaseRelationBuffers(rdesc);

	/* ----------------
	 *	flush the relation from the relcache
	 * ----------------
	 * Does nothing!!! Flushing moved below.	- vadim 06/04/97
	RelationIdInvalidateRelationCacheByRelationId(rdesc->rd_id);
	 */

	RemoveConstraints(rdesc);

	/* ----------------
	 *	unlink the relation and finish up.
	 * ----------------
	 */
	if (!(rdesc->rd_istemp) || !(rdesc->rd_tmpunlinked))
		smgrunlink(DEFAULT_SMGR, rdesc);

	rdesc->rd_tmpunlinked = TRUE;

	RelationUnsetLockForWrite(rdesc);

	heap_close(rdesc);

	/* ok - flush the relation from the relcache */
	RelationForgetRelation(rid);
}

/*
 * heap_destroy
 *	  destroy and close temporary relations
 *
 */

void
heap_destroy(Relation rdesc)
{
	ReleaseRelationBuffers(rdesc);
	if (!(rdesc->rd_istemp) || !(rdesc->rd_tmpunlinked))
		smgrunlink(DEFAULT_SMGR, rdesc);
	rdesc->rd_tmpunlinked = TRUE;
	heap_close(rdesc);
	RemoveFromTempRelList(rdesc);
}


/**************************************************************
  functions to deal with the list of temporary relations
**************************************************************/

/* --------------
   InitTempRellist():

   initialize temporary relations list
   the tempRelList is a list of temporary relations that
   are created in the course of the transactions
   they need to be destroyed properly at the end of the transactions

   MODIFIES the global variable tempRels

 >> NOTE <<

   malloc is used instead of palloc because we KNOW when we are
   going to free these things.	Keeps us away from the memory context
   hairyness

*/
void
InitTempRelList(void)
{
	if (tempRels)
	{
		free(tempRels->rels);
		free(tempRels);
	}

	tempRels = (TempRelList *) malloc(sizeof(TempRelList));
	tempRels->size = TEMP_REL_LIST_SIZE;
	tempRels->rels = (Relation *) malloc(sizeof(Relation) * tempRels->size);
	MemSet(tempRels->rels, 0, sizeof(Relation) * tempRels->size);
	tempRels->num = 0;
}

/*
   removes a relation from the TempRelList

   MODIFIES the global variable tempRels
	  we don't really remove it, just mark it as NULL
	  and DestroyTempRels will look for NULLs
*/
static void
RemoveFromTempRelList(Relation r)
{
	int			i;

	if (!tempRels)
		return;

	for (i = 0; i < tempRels->num; i++)
	{
		if (tempRels->rels[i] == r)
		{
			tempRels->rels[i] = NULL;
			break;
		}
	}
}

/*
   add a temporary relation to the TempRelList

   MODIFIES the global variable tempRels
*/
static void
AddToTempRelList(Relation r)
{
	if (!tempRels)
		return;

	if (tempRels->num == tempRels->size)
	{
		tempRels->size += TEMP_REL_LIST_SIZE;
		tempRels->rels = realloc(tempRels->rels,
								 sizeof(Relation) * tempRels->size);
	}
	tempRels->rels[tempRels->num] = r;
	tempRels->num++;
}

/*
   go through the tempRels list and destroy each of the relations
*/
void
DestroyTempRels(void)
{
	int			i;
	Relation	rdesc;

	if (!tempRels)
		return;

	for (i = 0; i < tempRels->num; i++)
	{
		rdesc = tempRels->rels[i];
		/* rdesc may be NULL if it has been removed from the list already */
		if (rdesc)
			heap_destroy(rdesc);
	}
	free(tempRels->rels);
	free(tempRels);
	tempRels = NULL;
}


static void
StoreAttrDefault(Relation rel, AttrDefault *attrdef)
{
	char		str[MAX_PARSE_BUFFER];
	char		cast[2 * NAMEDATALEN] = {0};
	AttributeTupleForm atp = rel->rd_att->attrs[attrdef->adnum - 1];
	QueryTreeList *queryTree_list;
	Query	   *query;
	List	   *planTree_list;
	TargetEntry *te;
	Resdom	   *resdom;
	Node	   *expr;
	char	   *adbin;
	MemoryContext oldcxt;
	Relation	adrel;
	Relation	idescs[Num_pg_attrdef_indices];
	HeapTuple	tuple;
	Datum		values[4];
	char		nulls[4] = {' ', ' ', ' ', ' '};
	extern GlobalMemory CacheCxt;

start:;
	sprintf(str, "select %s%s from %.*s", attrdef->adsrc, cast,
			NAMEDATALEN, rel->rd_rel->relname.data);
	setheapoverride(true);
	planTree_list = (List *) pg_parse_and_plan(str, NULL, 0, &queryTree_list, None);
	setheapoverride(false);
	query = (Query *) (queryTree_list->qtrees[0]);

	if (length(query->rtable) > 1 ||
		flatten_tlist(query->targetList) != NIL)
		elog(ERROR, "DEFAULT: cannot use attribute(s)");
	te = (TargetEntry *) lfirst(query->targetList);
	resdom = te->resdom;
	expr = te->expr;

	if (IsA(expr, Const))
	{
		if (((Const *) expr)->consttype != atp->atttypid)
		{
			if (*cast != 0)
				elog(ERROR, "DEFAULT: const type mismatched");
			sprintf(cast, ":: %s", typeidTypeName(atp->atttypid));
			goto start;
		}
	}
	else if (exprType(expr) != atp->atttypid)
		elog(ERROR, "DEFAULT: type mismatched");

	adbin = nodeToString(expr);
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);
	attrdef->adbin = (char *) palloc(strlen(adbin) + 1);
	strcpy(attrdef->adbin, adbin);
	(void) MemoryContextSwitchTo(oldcxt);
	pfree(adbin);

	values[Anum_pg_attrdef_adrelid - 1] = rel->rd_id;
	values[Anum_pg_attrdef_adnum - 1] = attrdef->adnum;
	values[Anum_pg_attrdef_adbin - 1] = PointerGetDatum(textin(attrdef->adbin));
	values[Anum_pg_attrdef_adsrc - 1] = PointerGetDatum(textin(attrdef->adsrc));
	adrel = heap_openr(AttrDefaultRelationName);
	tuple = heap_formtuple(adrel->rd_att, values, nulls);
	CatalogOpenIndices(Num_pg_attrdef_indices, Name_pg_attrdef_indices, idescs);
	heap_insert(adrel, tuple);
	CatalogIndexInsert(idescs, Num_pg_attrdef_indices, adrel, tuple);
	CatalogCloseIndices(Num_pg_attrdef_indices, idescs);
	heap_close(adrel);

	pfree(DatumGetPointer(values[Anum_pg_attrdef_adbin - 1]));
	pfree(DatumGetPointer(values[Anum_pg_attrdef_adsrc - 1]));
	pfree(tuple);

}

static void
StoreRelCheck(Relation rel, ConstrCheck *check)
{
	char		str[MAX_PARSE_BUFFER];
	QueryTreeList *queryTree_list;
	Query	   *query;
	List	   *planTree_list;
	Plan	   *plan;
	List	   *qual;
	char	   *ccbin;
	MemoryContext oldcxt;
	Relation	rcrel;
	Relation	idescs[Num_pg_relcheck_indices];
	HeapTuple	tuple;
	Datum		values[4];
	char		nulls[4] = {' ', ' ', ' ', ' '};
	extern GlobalMemory CacheCxt;

	sprintf(str, "select 1 from %.*s where %s",
			NAMEDATALEN, rel->rd_rel->relname.data, check->ccsrc);
	setheapoverride(true);
	planTree_list = (List *) pg_parse_and_plan(str, NULL, 0, &queryTree_list, None);
	setheapoverride(false);
	query = (Query *) (queryTree_list->qtrees[0]);

	if (length(query->rtable) > 1)
		elog(ERROR, "CHECK: only relation %.*s can be referenced",
			 NAMEDATALEN, rel->rd_rel->relname.data);

	plan = (Plan *) lfirst(planTree_list);
	qual = plan->qual;

	ccbin = nodeToString(qual);
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);
	check->ccbin = (char *) palloc(strlen(ccbin) + 1);
	strcpy(check->ccbin, ccbin);
	(void) MemoryContextSwitchTo(oldcxt);
	pfree(ccbin);

	values[Anum_pg_relcheck_rcrelid - 1] = rel->rd_id;
	values[Anum_pg_relcheck_rcname - 1] = PointerGetDatum(namein(check->ccname));
	values[Anum_pg_relcheck_rcbin - 1] = PointerGetDatum(textin(check->ccbin));
	values[Anum_pg_relcheck_rcsrc - 1] = PointerGetDatum(textin(check->ccsrc));
	rcrel = heap_openr(RelCheckRelationName);
	tuple = heap_formtuple(rcrel->rd_att, values, nulls);
	CatalogOpenIndices(Num_pg_relcheck_indices, Name_pg_relcheck_indices, idescs);
	heap_insert(rcrel, tuple);
	CatalogIndexInsert(idescs, Num_pg_relcheck_indices, rcrel, tuple);
	CatalogCloseIndices(Num_pg_relcheck_indices, idescs);
	heap_close(rcrel);

	pfree(DatumGetPointer(values[Anum_pg_relcheck_rcname - 1]));
	pfree(DatumGetPointer(values[Anum_pg_relcheck_rcbin - 1]));
	pfree(DatumGetPointer(values[Anum_pg_relcheck_rcsrc - 1]));
	pfree(tuple);

	return;
}

static void
StoreConstraints(Relation rel)
{
	TupleConstr *constr = rel->rd_att->constr;
	int			i;

	if (!constr)
		return;

	if (constr->num_defval > 0)
	{
		for (i = 0; i < constr->num_defval; i++)
			StoreAttrDefault(rel, &(constr->defval[i]));
	}

	if (constr->num_check > 0)
	{
		for (i = 0; i < constr->num_check; i++)
			StoreRelCheck(rel, &(constr->check[i]));
	}

	return;
}

static void
RemoveAttrDefault(Relation rel)
{
	Relation	adrel;
	HeapScanDesc adscan;
	ScanKeyData key;
	HeapTuple	tup;

	adrel = heap_openr(AttrDefaultRelationName);

	ScanKeyEntryInitialize(&key, 0, Anum_pg_attrdef_adrelid,
						   F_OIDEQ, rel->rd_id);

	RelationSetLockForWrite(adrel);

	adscan = heap_beginscan(adrel, 0, false, 1, &key);

	while (tup = heap_getnext(adscan, 0, (Buffer *) NULL), PointerIsValid(tup))
		heap_delete(adrel, &tup->t_ctid);

	heap_endscan(adscan);

	RelationUnsetLockForWrite(adrel);
	heap_close(adrel);

}

static void
RemoveRelCheck(Relation rel)
{
	Relation	rcrel;
	HeapScanDesc rcscan;
	ScanKeyData key;
	HeapTuple	tup;

	rcrel = heap_openr(RelCheckRelationName);

	ScanKeyEntryInitialize(&key, 0, Anum_pg_relcheck_rcrelid,
						   F_OIDEQ, rel->rd_id);

	RelationSetLockForWrite(rcrel);

	rcscan = heap_beginscan(rcrel, 0, false, 1, &key);

	while (tup = heap_getnext(rcscan, 0, (Buffer *) NULL), PointerIsValid(tup))
		heap_delete(rcrel, &tup->t_ctid);

	heap_endscan(rcscan);

	RelationUnsetLockForWrite(rcrel);
	heap_close(rcrel);

}

static void
RemoveConstraints(Relation rel)
{
	TupleConstr *constr = rel->rd_att->constr;

	if (!constr)
		return;

	if (constr->num_defval > 0)
		RemoveAttrDefault(rel);

	if (constr->num_check > 0)
		RemoveRelCheck(rel);

	return;
}
