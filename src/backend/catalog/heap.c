/*-------------------------------------------------------------------------
 *
 * heap.c
 *	  code to create and destroy POSTGRES heap relations
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/heap.c,v 1.206 2002/07/14 21:08:08 tgl Exp $
 *
 *
 * INTERFACE ROUTINES
 *		heap_create()			- Create an uncataloged heap relation
 *		heap_create_with_catalog() - Create a cataloged relation
 *		heap_drop_with_catalog() - Removes named relation from catalogs
 *
 * NOTES
 *	  this code taken from access/heap/create.c, which contains
 *	  the old heap_create_with_catalog, amcreate, and amdestroy.
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
#include "access/genam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/var.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "rewrite/rewriteRemove.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


static void AddNewRelationTuple(Relation pg_class_desc,
					Relation new_rel_desc,
					Oid new_rel_oid, Oid new_type_oid,
					char relkind, bool relhasoids);
static void AddNewRelationType(const char *typeName,
							   Oid typeNamespace,
							   Oid new_rel_oid,
							   Oid new_type_oid);
static void RelationRemoveInheritance(Relation relation);
static void StoreAttrDefault(Relation rel, AttrNumber attnum, char *adbin);
static void StoreRelCheck(Relation rel, char *ccname, char *ccbin);
static void StoreConstraints(Relation rel, TupleDesc tupdesc);
static void SetRelationNumChecks(Relation rel, int numchecks);
static void RemoveDefaults(Relation rel);
static void RemoveStatistics(Relation rel);


/* ----------------------------------------------------------------
 *				XXX UGLY HARD CODED BADNESS FOLLOWS XXX
 *
 *		these should all be moved to someplace in the lib/catalog
 *		module, if not obliterated first.
 * ----------------------------------------------------------------
 */


/*
 * Note:
 *		Should the system special case these attributes in the future?
 *		Advantage:	consume much less space in the ATTRIBUTE relation.
 *		Disadvantage:  special cases will be all over the place.
 */

static FormData_pg_attribute a1 = {
	0, {"ctid"}, TIDOID, 0, sizeof(ItemPointerData),
	SelfItemPointerAttributeNumber, 0, -1, -1,
	false, 'p', false, 'i', false, false
};

static FormData_pg_attribute a2 = {
	0, {"oid"}, OIDOID, 0, sizeof(Oid),
	ObjectIdAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', false, false
};

static FormData_pg_attribute a3 = {
	0, {"xmin"}, XIDOID, 0, sizeof(TransactionId),
	MinTransactionIdAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', false, false
};

static FormData_pg_attribute a4 = {
	0, {"cmin"}, CIDOID, 0, sizeof(CommandId),
	MinCommandIdAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', false, false
};

static FormData_pg_attribute a5 = {
	0, {"xmax"}, XIDOID, 0, sizeof(TransactionId),
	MaxTransactionIdAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', false, false
};

static FormData_pg_attribute a6 = {
	0, {"cmax"}, CIDOID, 0, sizeof(CommandId),
	MaxCommandIdAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', false, false
};

/*
 * We decided to call this attribute "tableoid" rather than say
 * "classoid" on the basis that in the future there may be more than one
 * table of a particular class/type. In any case table is still the word
 * used in SQL.
 */
static FormData_pg_attribute a7 = {
	0, {"tableoid"}, OIDOID, 0, sizeof(Oid),
	TableOidAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', false, false
};

static Form_pg_attribute SysAtt[] = {&a1, &a2, &a3, &a4, &a5, &a6, &a7};

/*
 * This function returns a Form_pg_attribute pointer for a system attribute.
 * Note that we elog if the presented attno is invalid.
 */
Form_pg_attribute
SystemAttributeDefinition(AttrNumber attno, bool relhasoids)
{
	if (attno >= 0 || attno < -(int) lengthof(SysAtt))
		elog(ERROR, "SystemAttributeDefinition: invalid attribute number %d",
			 attno);
	if (attno == ObjectIdAttributeNumber && !relhasoids)
		elog(ERROR, "SystemAttributeDefinition: invalid attribute number %d",
			 attno);
	return SysAtt[-attno - 1];
}

/*
 * If the given name is a system attribute name, return a Form_pg_attribute
 * pointer for a prototype definition.	If not, return NULL.
 */
Form_pg_attribute
SystemAttributeByName(const char *attname, bool relhasoids)
{
	int			j;

	for (j = 0; j < (int) lengthof(SysAtt); j++)
	{
		Form_pg_attribute att = SysAtt[j];

		if (relhasoids || att->attnum != ObjectIdAttributeNumber)
		{
			if (strcmp(NameStr(att->attname), attname) == 0)
				return att;
		}
	}

	return NULL;
}


/* ----------------------------------------------------------------
 *				XXX END OF UGLY HARD CODED BADNESS XXX
 * ---------------------------------------------------------------- */


/* ----------------------------------------------------------------
 *		heap_create		- Create an uncataloged heap relation
 *
 *		rel->rd_rel is initialized by RelationBuildLocalRelation,
 *		and is mostly zeroes at return.
 *
 *		Remove the system relation specific code to elsewhere eventually.
 *
 * If storage_create is TRUE then heap_storage_create is called here,
 * else caller must call heap_storage_create later.
 * ----------------------------------------------------------------
 */
Relation
heap_create(const char *relname,
			Oid relnamespace,
			TupleDesc tupDesc,
			bool shared_relation,
			bool storage_create,
			bool allow_system_table_mods)
{
	Oid			relid;
	Oid			dbid = shared_relation ? InvalidOid : MyDatabaseId;
	bool		nailme = false;
	RelFileNode	rnode;
	Relation	rel;

	/*
	 * sanity checks
	 */
	if (!allow_system_table_mods &&
		(IsSystemNamespace(relnamespace) || IsToastNamespace(relnamespace)) &&
		IsNormalProcessingMode())
		elog(ERROR, "cannot create %s.%s: "
			 "system catalog modifications are currently disallowed",
			 get_namespace_name(relnamespace), relname);

	/*
	 * Real ugly stuff to assign the proper relid in the relation
	 * descriptor follows.	Note that only "bootstrapped" relations whose
	 * OIDs are hard-coded in pg_class.h should be listed here.  We also
	 * have to recognize those rels that must be nailed in cache.
	 */
	if (IsSystemNamespace(relnamespace))
	{
		if (strcmp(TypeRelationName, relname) == 0)
		{
			nailme = true;
			relid = RelOid_pg_type;
		}
		else if (strcmp(AttributeRelationName, relname) == 0)
		{
			nailme = true;
			relid = RelOid_pg_attribute;
		}
		else if (strcmp(ProcedureRelationName, relname) == 0)
		{
			nailme = true;
			relid = RelOid_pg_proc;
		}
		else if (strcmp(RelationRelationName, relname) == 0)
		{
			nailme = true;
			relid = RelOid_pg_class;
		}
		else if (strcmp(ShadowRelationName, relname) == 0)
		{
			relid = RelOid_pg_shadow;
		}
		else if (strcmp(GroupRelationName, relname) == 0)
		{
			relid = RelOid_pg_group;
		}
		else if (strcmp(DatabaseRelationName, relname) == 0)
		{
			relid = RelOid_pg_database;
		}
		else
		{
			relid = newoid();
		}
	}
	else
		relid = newoid();

	/*
	 * For now, the physical identifier of the relation is the same as the
	 * logical identifier.
	 */
	rnode.tblNode = dbid;
	rnode.relNode = relid;

	/*
	 * build the relcache entry.
	 */
	rel = RelationBuildLocalRelation(relname,
									 relnamespace,
									 tupDesc,
									 relid, dbid,
									 rnode,
									 nailme);

	/*
	 * have the storage manager create the relation.
	 */
	if (storage_create)
		heap_storage_create(rel);

	return rel;
}

void
heap_storage_create(Relation rel)
{
	Assert(rel->rd_fd < 0);
	rel->rd_fd = smgrcreate(DEFAULT_SMGR, rel);
	Assert(rel->rd_fd >= 0);
}

/* ----------------------------------------------------------------
 *		heap_create_with_catalog		- Create a cataloged relation
 *
 *		this is done in 6 steps:
 *
 *		1) CheckAttributeNames() is used to make certain the tuple
 *		   descriptor contains a valid set of attribute names
 *
 *		2) pg_class is opened and get_relname_relid()
 *		   performs a scan to ensure that no relation with the
 *		   same name already exists.
 *
 *		3) heap_create() is called to create the new relation on disk.
 *
 *		4) AddNewRelationTuple() is called to register the
 *		   relation in pg_class.
 *
 *		5) TypeCreate() is called to define a new type corresponding
 *		   to the new relation.
 *
 *		6) AddNewAttributeTuples() is called to register the
 *		   new relation's schema in pg_attribute.
 *
 *		7) StoreConstraints is called ()		- vadim 08/22/97
 *
 *		8) the relations are closed and the new relation's oid
 *		   is returned.
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
CheckAttributeNames(TupleDesc tupdesc, bool relhasoids, char relkind)
{
	int			i;
	int			j;
	int			natts = tupdesc->natts;

	/*
	 * first check for collision with system attribute names
	 *
	 * Skip this for a view, since it doesn't have system attributes.
	 */
	if (relkind != RELKIND_VIEW)
	{
		for (i = 0; i < natts; i++)
		{
			if (SystemAttributeByName(NameStr(tupdesc->attrs[i]->attname),
									  relhasoids) != NULL)
				elog(ERROR, "name of column \"%s\" conflicts with an existing system column",
					 NameStr(tupdesc->attrs[i]->attname));
		}
	}

	/*
	 * also, warn user if attribute to be created has an unknown typid
	 * (usually as a result of a 'retrieve into' - jolly
	 */
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->atttypid == UNKNOWNOID)
			elog(WARNING, "Attribute '%s' has an unknown type"
				 "\n\tProceeding with relation creation anyway",
				 NameStr(tupdesc->attrs[i]->attname));
	}

	/*
	 * next check for repeated attribute names
	 */
	for (i = 1; i < natts; i++)
	{
		for (j = 0; j < i; j++)
		{
			if (strcmp(NameStr(tupdesc->attrs[j]->attname),
					   NameStr(tupdesc->attrs[i]->attname)) == 0)
				elog(ERROR, "column name \"%s\" is duplicated",
					 NameStr(tupdesc->attrs[j]->attname));
		}
	}
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
					  TupleDesc tupdesc,
					  bool relhasoids,
					  char relkind)
{
	Form_pg_attribute *dpp;
	int			i;
	HeapTuple	tup;
	Relation	rel;
	bool		hasindex;
	Relation	idescs[Num_pg_attr_indices];
	int			natts = tupdesc->natts;

	/*
	 * open pg_attribute
	 */
	rel = heap_openr(AttributeRelationName, RowExclusiveLock);

	/*
	 * Check if we have any indices defined on pg_attribute.
	 */
	hasindex = RelationGetForm(rel)->relhasindex;
	if (hasindex)
		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);

	/*
	 * first we add the user attributes..
	 */
	dpp = tupdesc->attrs;
	for (i = 0; i < natts; i++)
	{
		/* Fill in the correct relation OID */
		(*dpp)->attrelid = new_rel_oid;
		/* Make sure these are OK, too */
		(*dpp)->attstattarget = DEFAULT_ATTSTATTARGET;
		(*dpp)->attcacheoff = -1;

		tup = heap_addheader(Natts_pg_attribute,
							 ATTRIBUTE_TUPLE_SIZE,
							 (void *) *dpp);

		simple_heap_insert(rel, tup);

		if (hasindex)
			CatalogIndexInsert(idescs, Num_pg_attr_indices, rel, tup);

		heap_freetuple(tup);
		dpp++;
	}

	/*
	 * next we add the system attributes.  Skip OID if rel has no OIDs.
	 */
	if (relkind != RELKIND_VIEW)
	{
		dpp = SysAtt;
		for (i = 0; i < -1 - FirstLowInvalidHeapAttributeNumber; i++)
		{
			if (relhasoids || (*dpp)->attnum != ObjectIdAttributeNumber)
			{
				Form_pg_attribute attStruct;

				tup = heap_addheader(Natts_pg_attribute,
									 ATTRIBUTE_TUPLE_SIZE,
									 (void *) *dpp);

				/* Fill in the correct relation OID in the copied tuple */
				attStruct = (Form_pg_attribute) GETSTRUCT(tup);
				attStruct->attrelid = new_rel_oid;

				/*
			 	 * Unneeded since they should be OK in the constant data
			 	 * anyway
			 	 */
				/* attStruct->attstattarget = 0; */
				/* attStruct->attcacheoff = -1; */

				simple_heap_insert(rel, tup);

				if (hasindex)
					CatalogIndexInsert(idescs, Num_pg_attr_indices, rel, tup);

				heap_freetuple(tup);
			}
			dpp++;
		}
	}

	/*
	 * close pg_attribute indices
	 */
	if (hasindex)
		CatalogCloseIndices(Num_pg_attr_indices, idescs);

	heap_close(rel, RowExclusiveLock);
}

/* --------------------------------
 *		AddNewRelationTuple
 *
 *		this registers the new relation in the catalogs by
 *		adding a tuple to pg_class.
 * --------------------------------
 */
static void
AddNewRelationTuple(Relation pg_class_desc,
					Relation new_rel_desc,
					Oid new_rel_oid,
					Oid new_type_oid,
					char relkind,
					bool relhasoids)
{
	Form_pg_class new_rel_reltup;
	HeapTuple	tup;
	Relation	idescs[Num_pg_class_indices];

	/*
	 * first we update some of the information in our uncataloged
	 * relation's relation descriptor.
	 */
	new_rel_reltup = new_rel_desc->rd_rel;

	/*
	 * Here we insert bogus estimates of the size of the new relation. In
	 * reality, of course, the new relation has 0 tuples and pages, and if
	 * we were tracking these statistics accurately then we'd set the
	 * fields that way.  But at present the stats will be updated only by
	 * VACUUM or CREATE INDEX, and the user might insert a lot of tuples
	 * before he gets around to doing either of those.	So, instead of
	 * saying the relation is empty, we insert guesstimates.  The point is
	 * to keep the optimizer from making really stupid choices on
	 * never-yet-vacuumed tables; so the estimates need only be large
	 * enough to discourage the optimizer from using nested-loop plans.
	 * With this hack, nested-loop plans will be preferred only after the
	 * table has been proven to be small by VACUUM or CREATE INDEX.
	 * Maintaining the stats on-the-fly would solve the problem more
	 * cleanly, but the overhead of that would likely cost more than it'd
	 * save. (NOTE: CREATE INDEX inserts the same bogus estimates if it
	 * finds the relation has 0 rows and pages. See index.c.)
	 */
	switch (relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_INDEX:
		case RELKIND_TOASTVALUE:
			new_rel_reltup->relpages = 10;		/* bogus estimates */
			new_rel_reltup->reltuples = 1000;
			break;
		case RELKIND_SEQUENCE:
			new_rel_reltup->relpages = 1;
			new_rel_reltup->reltuples = 1;
			break;
		default:				/* views, etc */
			new_rel_reltup->relpages = 0;
			new_rel_reltup->reltuples = 0;
			break;
	}

	new_rel_reltup->relowner = GetUserId();
	new_rel_reltup->reltype = new_type_oid;
	new_rel_reltup->relkind = relkind;
	new_rel_reltup->relhasoids = relhasoids;

	/* ----------------
	 *	now form a tuple to add to pg_class
	 *	XXX Natts_pg_class_fixed is a hack - see pg_class.h
	 * ----------------
	 */
	tup = heap_addheader(Natts_pg_class_fixed,
						 CLASS_TUPLE_SIZE,
						 (void *) new_rel_reltup);

	/* force tuple to have the desired OID */
	tup->t_data->t_oid = new_rel_oid;

	/*
	 * finally insert the new tuple and free it.
	 */
	simple_heap_insert(pg_class_desc, tup);

	if (!IsIgnoringSystemIndexes())
	{
		/*
		 * First, open the catalog indices and insert index tuples for the
		 * new relation.
		 */
		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_class_indices, pg_class_desc, tup);
		CatalogCloseIndices(Num_pg_class_indices, idescs);
	}

	heap_freetuple(tup);
}


/* --------------------------------
 *		AddNewRelationType -
 *
 *		define a complex type corresponding to the new relation
 * --------------------------------
 */
static void
AddNewRelationType(const char *typeName,
				   Oid typeNamespace,
				   Oid new_rel_oid,
				   Oid new_type_oid)
{
	/*
	 * The sizes are set to oid size because it makes implementing sets
	 * MUCH easier, and no one (we hope) uses these fields to figure out
	 * how much space to allocate for the type. An oid is the type used
	 * for a set definition.  When a user requests a set, what they
	 * actually get is the oid of a tuple in the pg_proc catalog, so the
	 * size of the "set" is the size of an oid. Similarly, byval being
	 * true makes sets much easier, and it isn't used by anything else.
	 */
	TypeCreate(typeName,		/* type name */
			   typeNamespace,	/* type namespace */
			   new_type_oid,	/* preassigned oid for type */
			   new_rel_oid,		/* relation oid */
			   sizeof(Oid),		/* internal size */
			   -1,				/* external size */
			   'c',				/* type-type (complex) */
			   ',',				/* default array delimiter */
			   F_OIDIN,			/* input procedure */
			   F_OIDOUT,		/* output procedure */
			   F_OIDIN,			/* receive procedure */
			   F_OIDOUT,		/* send procedure */
			   InvalidOid,		/* array element type - irrelevant */
			   InvalidOid,		/* domain base type - irrelevant */
			   NULL,			/* default type value - none */
			   NULL,			/* default type binary representation */
			   true,			/* passed by value */
			   'i',				/* default alignment - same as for OID */
			   'p',				/* Not TOASTable */
			   -1,				/* typmod */
			   0,				/* array dimensions for typBaseType */
			   false);			/* Type NOT NULL */
}

/* --------------------------------
 *		heap_create_with_catalog
 *
 *		creates a new cataloged relation.  see comments above.
 * --------------------------------
 */
Oid
heap_create_with_catalog(const char *relname,
						 Oid relnamespace,
						 TupleDesc tupdesc,
						 char relkind,
						 bool shared_relation,
						 bool relhasoids,
						 bool allow_system_table_mods)
{
	Relation	pg_class_desc;
	Relation	new_rel_desc;
	Oid			new_rel_oid;
	Oid			new_type_oid;

	/*
	 * sanity checks
	 */
	Assert(IsNormalProcessingMode() || IsBootstrapProcessingMode());
	if (tupdesc->natts <= 0 || tupdesc->natts > MaxHeapAttributeNumber)
		elog(ERROR, "Number of columns is out of range (1 to %d)",
			 MaxHeapAttributeNumber);

	CheckAttributeNames(tupdesc, relhasoids, relkind);

	if (get_relname_relid(relname, relnamespace))
		elog(ERROR, "Relation '%s' already exists", relname);

	/*
	 * Tell heap_create not to create a physical file; we'll do that below
	 * after all our catalog updates are done.	(This isn't really
	 * necessary anymore, but we may as well avoid the cycles of creating
	 * and deleting the file in case we fail.)
	 */
	new_rel_desc = heap_create(relname,
							   relnamespace,
							   tupdesc,
							   shared_relation,
							   false,
							   allow_system_table_mods);

	/* Fetch the relation OID assigned by heap_create */
	new_rel_oid = new_rel_desc->rd_att->attrs[0]->attrelid;

	/* Assign an OID for the relation's tuple type */
	new_type_oid = newoid();

	/*
	 * now create an entry in pg_class for the relation.
	 *
	 * NOTE: we could get a unique-index failure here, in case someone else
	 * is creating the same relation name in parallel but hadn't committed
	 * yet when we checked for a duplicate name above.
	 */
	pg_class_desc = heap_openr(RelationRelationName, RowExclusiveLock);

	AddNewRelationTuple(pg_class_desc,
						new_rel_desc,
						new_rel_oid,
						new_type_oid,
						relkind,
						relhasoids);

	/*
	 * since defining a relation also defines a complex type, we add a new
	 * system type corresponding to the new relation.
	 *
	 * NOTE: we could get a unique-index failure here, in case the same name
	 * has already been used for a type.
	 */
	AddNewRelationType(relname, relnamespace, new_rel_oid, new_type_oid);

	/*
	 * now add tuples to pg_attribute for the attributes in our new
	 * relation.
	 */
	AddNewAttributeTuples(new_rel_oid, new_rel_desc->rd_att,
						  relhasoids, relkind);

	/*
	 * store constraints and defaults passed in the tupdesc, if any.
	 *
	 * NB: this may do a CommandCounterIncrement and rebuild the relcache
	 * entry, so the relation must be valid and self-consistent at this point.
	 * In particular, there are not yet constraints and defaults anywhere.
	 */
	StoreConstraints(new_rel_desc, tupdesc);

	/*
	 * We create the disk file for this relation here
	 */
	if (relkind != RELKIND_VIEW)
		heap_storage_create(new_rel_desc);

	/*
	 * ok, the relation has been cataloged, so close our relations and
	 * return the oid of the newly created relation.
	 */
	heap_close(new_rel_desc, NoLock);	/* do not unlock till end of xact */
	heap_close(pg_class_desc, RowExclusiveLock);

	return new_rel_oid;
}


/*
 *		RelationRemoveInheritance
 *
 * Formerly, this routine checked for child relations and aborted the
 * deletion if any were found.  Now we rely on the dependency mechanism
 * to check for or delete child relations.  By the time we get here,
 * there are no children and we need only remove the pg_inherits rows.
 */
static void
RelationRemoveInheritance(Relation relation)
{
	Relation	catalogRelation;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData entry;

	catalogRelation = heap_openr(InheritsRelationName, RowExclusiveLock);

	ScanKeyEntryInitialize(&entry, 0x0,
						   Anum_pg_inherits_inhrelid, F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));

	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndex, true,
							  SnapshotNow, 1, &entry);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		simple_heap_delete(catalogRelation, &tuple->t_self);
	}

	systable_endscan(scan);
	heap_close(catalogRelation, RowExclusiveLock);
}

/*
 *		DeleteRelationTuple
 *
 * Remove pg_class row for the given relid.
 *
 * Note: this is shared by relation deletion and index deletion.  It's
 * not intended for use anyplace else.
 */
void
DeleteRelationTuple(Oid relid)
{
	Relation	pg_class_desc;
	HeapTuple	tup;

	/* Grab an appropriate lock on the pg_class relation */
	pg_class_desc = heap_openr(RelationRelationName, RowExclusiveLock);

	tup = SearchSysCache(RELOID,
						 ObjectIdGetDatum(relid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "DeleteRelationTuple: cache lookup failed for relation %u",
			 relid);

	/* delete the relation tuple from pg_class, and finish up */
	simple_heap_delete(pg_class_desc, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(pg_class_desc, RowExclusiveLock);
}

/*
 *		DeleteAttributeTuples
 *
 * Remove pg_attribute rows for the given relid.
 *
 * Note: this is shared by relation deletion and index deletion.  It's
 * not intended for use anyplace else.
 */
void
DeleteAttributeTuples(Oid relid)
{
	Relation	attrel;
	SysScanDesc	scan;
	ScanKeyData key[1];
	HeapTuple	atttup;

	/* Grab an appropriate lock on the pg_attribute relation */
	attrel = heap_openr(AttributeRelationName, RowExclusiveLock);

	/* Use the index to scan only attributes of the target relation */
	ScanKeyEntryInitialize(&key[0], 0x0,
						   Anum_pg_attribute_attrelid, F_OIDEQ,
						   ObjectIdGetDatum(relid));

	scan = systable_beginscan(attrel, AttributeRelidNumIndex, true,
							  SnapshotNow, 1, key);

	/* Delete all the matching tuples */
	while ((atttup = systable_getnext(scan)) != NULL)
	{
		simple_heap_delete(attrel, &atttup->t_self);
	}

	/* Clean up after the scan */
	systable_endscan(scan);
	heap_close(attrel, RowExclusiveLock);
}

/* ----------------------------------------------------------------
 *		heap_drop_with_catalog	- removes specified relation from catalogs
 *
 *		1)	open relation, acquire exclusive lock.
 *		2)	flush relation buffers from bufmgr
 *		3)	remove inheritance information
 *		4)	remove pg_statistic tuples
 *		5)	remove pg_attribute tuples and related items
 *		6)	remove pg_class tuple
 *		7)	unlink relation file
 *
 * Note that this routine is not responsible for dropping objects that are
 * linked to the pg_class entry via dependencies (for example, indexes and
 * constraints).  Those are deleted by the dependency-tracing logic in
 * dependency.c before control gets here.  In general, therefore, this routine
 * should never be called directly; go through performDeletion() instead.
 * ----------------------------------------------------------------
 */
void
heap_drop_with_catalog(Oid rid)
{
	Relation	rel;
	int			i;

	/*
	 * Open and lock the relation.
	 */
	rel = heap_open(rid, AccessExclusiveLock);

	/*
	 * Release all buffers that belong to this relation, after writing any
	 * that are dirty
	 */
	i = FlushRelationBuffers(rel, (BlockNumber) 0);
	if (i < 0)
		elog(ERROR, "heap_drop_with_catalog: FlushRelationBuffers returned %d",
			 i);

	/*
	 * remove inheritance information
	 */
	RelationRemoveInheritance(rel);

	/*
	 * delete statistics
	 */
	RemoveStatistics(rel);

	/*
	 * delete attribute tuples and associated defaults
	 */
	DeleteAttributeTuples(RelationGetRelid(rel));

	RemoveDefaults(rel);

	/*
	 * delete relation tuple
	 */
	DeleteRelationTuple(RelationGetRelid(rel));

	/*
	 * unlink the relation's physical file and finish up.
	 */
	if (rel->rd_rel->relkind != RELKIND_VIEW)
		smgrunlink(DEFAULT_SMGR, rel);

	/*
	 * Close relcache entry, but *keep* AccessExclusiveLock on the
	 * relation until transaction commit.  This ensures no one else will
	 * try to do something with the doomed relation.
	 */
	heap_close(rel, NoLock);

	/*
	 * flush the relation from the relcache
	 */
	RelationForgetRelation(rid);
}


/*
 * Store a default expression for column attnum of relation rel.
 * The expression must be presented as a nodeToString() string.
 */
static void
StoreAttrDefault(Relation rel, AttrNumber attnum, char *adbin)
{
	Node	   *expr;
	char	   *adsrc;
	Relation	adrel;
	Relation	idescs[Num_pg_attrdef_indices];
	HeapTuple	tuple;
	Datum		values[4];
	static char nulls[4] = {' ', ' ', ' ', ' '};
	Relation	attrrel;
	Relation	attridescs[Num_pg_attr_indices];
	HeapTuple	atttup;
	Form_pg_attribute attStruct;

	/*
	 * Need to construct source equivalent of given node-string.
	 */
	expr = stringToNode(adbin);

	/*
	 * deparse it
	 */
	adsrc = deparse_expression(expr,
						deparse_context_for(RelationGetRelationName(rel),
											RelationGetRelid(rel)),
							   false);

	values[Anum_pg_attrdef_adrelid - 1] = RelationGetRelid(rel);
	values[Anum_pg_attrdef_adnum - 1] = attnum;
	values[Anum_pg_attrdef_adbin - 1] = DirectFunctionCall1(textin,
												 CStringGetDatum(adbin));
	values[Anum_pg_attrdef_adsrc - 1] = DirectFunctionCall1(textin,
												 CStringGetDatum(adsrc));
	adrel = heap_openr(AttrDefaultRelationName, RowExclusiveLock);
	tuple = heap_formtuple(adrel->rd_att, values, nulls);
	simple_heap_insert(adrel, tuple);
	CatalogOpenIndices(Num_pg_attrdef_indices, Name_pg_attrdef_indices,
					   idescs);
	CatalogIndexInsert(idescs, Num_pg_attrdef_indices, adrel, tuple);
	CatalogCloseIndices(Num_pg_attrdef_indices, idescs);
	heap_close(adrel, RowExclusiveLock);

	pfree(DatumGetPointer(values[Anum_pg_attrdef_adbin - 1]));
	pfree(DatumGetPointer(values[Anum_pg_attrdef_adsrc - 1]));
	heap_freetuple(tuple);
	pfree(adsrc);

	/*
	 * Update the pg_attribute entry for the column to show that a default
	 * exists.
	 */
	attrrel = heap_openr(AttributeRelationName, RowExclusiveLock);
	atttup = SearchSysCacheCopy(ATTNUM,
								ObjectIdGetDatum(RelationGetRelid(rel)),
								Int16GetDatum(attnum),
								0, 0);
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "cache lookup of attribute %d in relation %u failed",
			 attnum, RelationGetRelid(rel));
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);
	if (!attStruct->atthasdef)
	{
		attStruct->atthasdef = true;
		simple_heap_update(attrrel, &atttup->t_self, atttup);
		/* keep catalog indices current */
		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices,
						   attridescs);
		CatalogIndexInsert(attridescs, Num_pg_attr_indices, attrrel, atttup);
		CatalogCloseIndices(Num_pg_attr_indices, attridescs);
	}
	heap_close(attrrel, RowExclusiveLock);
	heap_freetuple(atttup);
}

/*
 * Store a constraint expression for the given relation.
 * The expression must be presented as a nodeToString() string.
 *
 * Caller is responsible for updating the count of constraints
 * in the pg_class entry for the relation.
 */
static void
StoreRelCheck(Relation rel, char *ccname, char *ccbin)
{
	Node	   *expr;
	char	   *ccsrc;
	List	   *varList;
	int			keycount;
	int16	   *attNos;

	/*
	 * Convert condition to a normal boolean expression tree.
	 */
	expr = stringToNode(ccbin);
	expr = (Node *) make_ands_explicit((List *) expr);

	/*
	 * deparse it
	 */
	ccsrc = deparse_expression(expr,
						deparse_context_for(RelationGetRelationName(rel),
											RelationGetRelid(rel)),
							   false);

	/*
	 * Find columns of rel that are used in ccbin
	 */
	varList = pull_var_clause(expr, false);
	keycount = length(varList);

	if (keycount > 0)
	{
		List	   *vl;
		int			i = 0;

		attNos = (int16 *) palloc(keycount * sizeof(int16));
		foreach(vl, varList)
		{
			Var	   *var = (Var *) lfirst(vl);
			int		j;

			for (j = 0; j < i; j++)
				if (attNos[j] == var->varattno)
					break;
			if (j == i)
				attNos[i++] = var->varattno;
		}
		keycount = i;
	}
	else
		attNos = NULL;

	/*
	 * Create the Check Constraint
	 */
	CreateConstraintEntry(ccname, 	/* Constraint Name */
						  RelationGetNamespace(rel), /* namespace */
						  CONSTRAINT_CHECK, /* Constraint Type */
						  false, 	/* Is Deferrable */
						  false,	/* Is Deferred */
						  RelationGetRelid(rel), /* relation */
						  attNos,	/* List of attributes in the constraint */
						  keycount,	/* # attributes in the constraint */
						  InvalidOid, /* not a domain constraint */
						  InvalidOid, /* Foreign key fields */
						  NULL,
						  0,
						  ' ',
						  ' ',
						  ' ',
						  ccbin,	/* Binary form check constraint */
						  ccsrc);	/* Source form check constraint */

	pfree(ccsrc);
}

/*
 * Store defaults and constraints passed in via the tuple constraint struct.
 *
 * NOTE: only pre-cooked expressions will be passed this way, which is to
 * say expressions inherited from an existing relation.  Newly parsed
 * expressions can be added later, by direct calls to StoreAttrDefault
 * and StoreRelCheck (see AddRelationRawConstraints()).
 */
static void
StoreConstraints(Relation rel, TupleDesc tupdesc)
{
	TupleConstr *constr = tupdesc->constr;
	int			i;

	if (!constr)
		return;					/* nothing to do */

	/*
	 * Deparsing of constraint expressions will fail unless the
	 * just-created pg_attribute tuples for this relation are made
	 * visible.  So, bump the command counter.  CAUTION: this will
	 * cause a relcache entry rebuild.
	 */
	CommandCounterIncrement();

	for (i = 0; i < constr->num_defval; i++)
		StoreAttrDefault(rel, constr->defval[i].adnum,
						 constr->defval[i].adbin);

	for (i = 0; i < constr->num_check; i++)
		StoreRelCheck(rel, constr->check[i].ccname,
					  constr->check[i].ccbin);

	if (constr->num_check > 0)
		SetRelationNumChecks(rel, constr->num_check);
}

/*
 * AddRelationRawConstraints
 *
 * Add raw (not-yet-transformed) column default expressions and/or constraint
 * check expressions to an existing relation.  This is defined to do both
 * for efficiency in DefineRelation, but of course you can do just one or
 * the other by passing empty lists.
 *
 * rel: relation to be modified
 * rawColDefaults: list of RawColumnDefault structures
 * rawConstraints: list of Constraint nodes
 *
 * All entries in rawColDefaults will be processed.  Entries in rawConstraints
 * will be processed only if they are CONSTR_CHECK type and contain a "raw"
 * expression.
 *
 * NB: caller should have opened rel with AccessExclusiveLock, and should
 * hold that lock till end of transaction.	Also, we assume the caller has
 * done a CommandCounterIncrement if necessary to make the relation's catalog
 * tuples visible.
 */
void
AddRelationRawConstraints(Relation rel,
						  List *rawColDefaults,
						  List *rawConstraints)
{
	char	   *relname = RelationGetRelationName(rel);
	TupleDesc	tupleDesc;
	TupleConstr *oldconstr;
	int			numoldchecks;
	ConstrCheck *oldchecks;
	ParseState *pstate;
	RangeTblEntry *rte;
	int			numchecks;
	int			constr_name_ctr = 0;
	List	   *listptr;
	Node	   *expr;

	/*
	 * Get info about existing constraints.
	 */
	tupleDesc = RelationGetDescr(rel);
	oldconstr = tupleDesc->constr;
	if (oldconstr)
	{
		numoldchecks = oldconstr->num_check;
		oldchecks = oldconstr->check;
	}
	else
	{
		numoldchecks = 0;
		oldchecks = NULL;
	}

	/*
	 * Create a dummy ParseState and insert the target relation as its
	 * sole rangetable entry.  We need a ParseState for transformExpr.
	 */
	pstate = make_parsestate(NULL);
	rte = addRangeTableEntryForRelation(pstate,
										RelationGetRelid(rel),
										makeAlias(relname, NIL),
										false,
										true);
	addRTEtoQuery(pstate, rte, true, true);

	/*
	 * Process column default expressions.
	 */
	foreach(listptr, rawColDefaults)
	{
		RawColumnDefault *colDef = (RawColumnDefault *) lfirst(listptr);
		Form_pg_attribute atp = rel->rd_att->attrs[colDef->attnum - 1];

		expr = cookDefault(pstate, colDef->raw_default,
						   atp->atttypid, atp->atttypmod,
						   NameStr(atp->attname));
		StoreAttrDefault(rel, colDef->attnum, nodeToString(expr));
	}

	/*
	 * Process constraint expressions.
	 */
	numchecks = numoldchecks;
	foreach(listptr, rawConstraints)
	{
		Constraint *cdef = (Constraint *) lfirst(listptr);
		char	   *ccname;

		if (cdef->contype != CONSTR_CHECK || cdef->raw_expr == NULL)
			continue;
		Assert(cdef->cooked_expr == NULL);

		/* Check name uniqueness, or generate a new name */
		if (cdef->name != NULL)
		{
			List	   *listptr2;

			ccname = cdef->name;
			/* Check against pre-existing constraints */
			if (ConstraintNameIsUsed(RelationGetRelid(rel),
									 RelationGetNamespace(rel),
									 ccname))
				elog(ERROR, "constraint \"%s\" already exists for relation \"%s\"",
					 ccname, RelationGetRelationName(rel));
			/* Check against other new constraints */
			/* Needed because we don't do CommandCounterIncrement in loop */
			foreach(listptr2, rawConstraints)
			{
				Constraint *cdef2 = (Constraint *) lfirst(listptr2);

				if (cdef2 == cdef ||
					cdef2->contype != CONSTR_CHECK ||
					cdef2->raw_expr == NULL ||
					cdef2->name == NULL)
					continue;
				if (strcmp(cdef2->name, ccname) == 0)
					elog(ERROR, "Duplicate CHECK constraint name: '%s'",
						 ccname);
			}
		}
		else
		{
			bool		success;

			do
			{
				List	   *listptr2;

				/*
				 * Generate a name that does not conflict with pre-existing
				 * constraints, nor with any auto-generated names so far.
				 */
				ccname = GenerateConstraintName(RelationGetRelid(rel),
												RelationGetNamespace(rel),
												&constr_name_ctr);
				/*
				 * Check against other new constraints, in case the user
				 * has specified a name that looks like an auto-generated
				 * name.
				 */
				success = true;
				foreach(listptr2, rawConstraints)
				{
					Constraint *cdef2 = (Constraint *) lfirst(listptr2);

					if (cdef2 == cdef ||
						cdef2->contype != CONSTR_CHECK ||
						cdef2->raw_expr == NULL ||
						cdef2->name == NULL)
						continue;
					if (strcmp(cdef2->name, ccname) == 0)
					{
						success = false;
						break;
					}
				}
			} while (!success);
		}

		/*
		 * Transform raw parsetree to executable expression.
		 */
		expr = transformExpr(pstate, cdef->raw_expr);

		/*
		 * Make sure it yields a boolean result.
		 */
		expr = coerce_to_boolean(expr, "CHECK");

		/*
		 * Make sure no outside relations are referred to.
		 */
		if (length(pstate->p_rtable) != 1)
			elog(ERROR, "Only relation \"%s\" can be referenced in CHECK constraint expression",
				 relname);

		/*
		 * No subplans or aggregates, either...
		 */
		if (contain_subplans(expr))
			elog(ERROR, "cannot use subselect in CHECK constraint expression");
		if (contain_agg_clause(expr))
			elog(ERROR, "cannot use aggregate function in CHECK constraint expression");

		/*
		 * Might as well try to reduce any constant expressions.
		 */
		expr = eval_const_expressions(expr);

		/*
		 * Constraints are evaluated with execQual, which expects an
		 * implicit-AND list, so convert expression to implicit-AND form.
		 * (We could go so far as to convert to CNF, but that's probably
		 * overkill...)
		 */
		expr = (Node *) make_ands_implicit((Expr *) expr);

		/*
		 * Must fix opids in operator clauses.
		 */
		fix_opids(expr);

		/*
		 * OK, store it.
		 */
		StoreRelCheck(rel, ccname, nodeToString(expr));

		numchecks++;
	}

	/*
	 * Update the count of constraints in the relation's pg_class tuple.
	 * We do this even if there was no change, in order to ensure that an
	 * SI update message is sent out for the pg_class tuple, which will
	 * force other backends to rebuild their relcache entries for the rel.
	 * (This is critical if we added defaults but not constraints.)
	 */
	SetRelationNumChecks(rel, numchecks);
}

/*
 * Update the count of constraints in the relation's pg_class tuple.
 *
 * Caller had better hold exclusive lock on the relation.
 *
 * An important side effect is that a SI update message will be sent out for
 * the pg_class tuple, which will force other backends to rebuild their
 * relcache entries for the rel.  Also, this backend will rebuild its
 * own relcache entry at the next CommandCounterIncrement.
 */
static void
SetRelationNumChecks(Relation rel, int numchecks)
{
	Relation	relrel;
	HeapTuple	reltup;
	Form_pg_class relStruct;
	Relation	relidescs[Num_pg_class_indices];

	relrel = heap_openr(RelationRelationName, RowExclusiveLock);
	reltup = SearchSysCacheCopy(RELOID,
								ObjectIdGetDatum(RelationGetRelid(rel)),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup of relation %u failed",
			 RelationGetRelid(rel));
	relStruct = (Form_pg_class) GETSTRUCT(reltup);

	if (relStruct->relchecks != numchecks)
	{
		relStruct->relchecks = numchecks;

		simple_heap_update(relrel, &reltup->t_self, reltup);

		/* keep catalog indices current */
		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices,
						   relidescs);
		CatalogIndexInsert(relidescs, Num_pg_class_indices, relrel, reltup);
		CatalogCloseIndices(Num_pg_class_indices, relidescs);
	}
	else
	{
		/* Skip the disk update, but force relcache inval anyway */
		CacheInvalidateRelcache(RelationGetRelid(rel));
	}

	heap_freetuple(reltup);
	heap_close(relrel, RowExclusiveLock);
}

/*
 * Take a raw default and convert it to a cooked format ready for
 * storage.
 *
 * Parse state should be set up to recognize any vars that might appear
 * in the expression.  (Even though we plan to reject vars, it's more
 * user-friendly to give the correct error message than "unknown var".)
 *
 * If atttypid is not InvalidOid, check that the expression is coercible
 * to the specified type.  atttypmod is needed in this case, and attname
 * is used in the error message if any.
 */
Node *
cookDefault(ParseState *pstate,
			Node *raw_default,
			Oid atttypid,
			int32 atttypmod,
			char *attname)
{
	Node		*expr;

	Assert(raw_default != NULL);

	/*
	 * Transform raw parsetree to executable expression.
	 */
	expr = transformExpr(pstate, raw_default);

	/*
	 * Make sure default expr does not refer to any vars.
	 */
	if (contain_var_clause(expr))
		elog(ERROR, "cannot use column references in DEFAULT clause");

	/*
	 * It can't return a set either.
	 */
	if (expression_returns_set(expr))
		elog(ERROR, "DEFAULT clause must not return a set");

	/*
	 * No subplans or aggregates, either...
	 */
	if (contain_subplans(expr))
		elog(ERROR, "cannot use subselects in DEFAULT clause");
	if (contain_agg_clause(expr))
		elog(ERROR, "cannot use aggregate functions in DEFAULT clause");

	/*
	 * Check that it will be possible to coerce the expression to the
	 * column's type.  We store the expression without coercion,
	 * however, to avoid premature coercion in cases like
	 *
	 * CREATE TABLE tbl (fld timestamp DEFAULT 'now'::text);
	 *
	 * NB: this should match the code in optimizer/prep/preptlist.c that
	 * will actually do the coercion, to ensure we don't accept an
	 * unusable default expression.
	 */
	if (OidIsValid(atttypid))
	{
		Oid		type_id = exprType(expr);

		if (type_id != atttypid)
		{
			if (CoerceTargetExpr(pstate, expr, type_id,
								 atttypid, atttypmod, false) == NULL)
				elog(ERROR, "Column \"%s\" is of type %s"
					 " but default expression is of type %s"
					 "\n\tYou will need to rewrite or cast the expression",
					 attname,
					 format_type_be(atttypid),
					 format_type_be(type_id));
		}
	}

	/*
	 * Might as well try to reduce any constant expressions.
	 */
	expr = eval_const_expressions(expr);

	/*
	 * Must fix opids, in case any operators remain...
	 */
	fix_opids(expr);

	return(expr);
}


static void
RemoveAttrDefaults(Relation rel)
{
	Relation	adrel;
	HeapScanDesc adscan;
	ScanKeyData key;
	HeapTuple	tup;

	adrel = heap_openr(AttrDefaultRelationName, RowExclusiveLock);

	ScanKeyEntryInitialize(&key, 0, Anum_pg_attrdef_adrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(rel)));

	adscan = heap_beginscan(adrel, SnapshotNow, 1, &key);

	while ((tup = heap_getnext(adscan, ForwardScanDirection)) != NULL)
		simple_heap_delete(adrel, &tup->t_self);

	heap_endscan(adscan);
	heap_close(adrel, RowExclusiveLock);
}

/*
 * Removes all constraints on a relation that match the given name.
 *
 * It is the responsibility of the calling function to acquire a suitable
 * lock on the relation.
 *
 * Returns: The number of constraints removed.
 */
int
RemoveRelConstraints(Relation rel, const char *constrName,
					 DropBehavior behavior)
{
	int			ndeleted = 0;
	Relation	conrel;
	SysScanDesc	conscan;
	ScanKeyData key[1];
	HeapTuple	contup;

	/* Grab an appropriate lock on the pg_constraint relation */
	conrel = heap_openr(ConstraintRelationName, RowExclusiveLock);

	/* Use the index to scan only constraints of the target relation */
	ScanKeyEntryInitialize(&key[0], 0x0,
						   Anum_pg_constraint_conrelid, F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(rel)));

	conscan = systable_beginscan(conrel, ConstraintRelidIndex, true,
								 SnapshotNow, 1, key);

	/*
	 * Scan over the result set, removing any matching entries.
	 */
	while ((contup = systable_getnext(conscan)) != NULL)
	{
		Form_pg_constraint	con = (Form_pg_constraint) GETSTRUCT(contup);

		if (strcmp(NameStr(con->conname), constrName) == 0)
		{
			ObjectAddress	conobj;

			conobj.classId = RelationGetRelid(conrel);
			conobj.objectId = contup->t_data->t_oid;
			conobj.objectSubId = 0;

			performDeletion(&conobj, behavior);

			ndeleted++;
		}
	}

	/* Clean up after the scan */
	systable_endscan(conscan);
	heap_close(conrel, RowExclusiveLock);

	return ndeleted;
}

static void
RemoveDefaults(Relation rel)
{
	TupleConstr *constr = rel->rd_att->constr;

	/*
	 * We can skip looking at pg_attrdef if there are no defaults recorded
	 * in the Relation.
	 */
	if (constr && constr->num_defval > 0)
		RemoveAttrDefaults(rel);
}

static void
RemoveStatistics(Relation rel)
{
	Relation	pgstatistic;
	HeapScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;

	pgstatistic = heap_openr(StatisticRelationName, RowExclusiveLock);

	ScanKeyEntryInitialize(&key, 0x0, Anum_pg_statistic_starelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = heap_beginscan(pgstatistic, SnapshotNow, 1, &key);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		simple_heap_delete(pgstatistic, &tuple->t_self);

	heap_endscan(scan);
	heap_close(pgstatistic, RowExclusiveLock);
}


/*
 * RelationTruncateIndexes - truncate all
 * indices associated with the heap relation to zero tuples.
 *
 * The routine will truncate and then reconstruct the indices on
 * the relation specified by the heapId parameter.
 */
static void
RelationTruncateIndexes(Oid heapId)
{
	Relation	indexRelation;
	ScanKeyData entry;
	SysScanDesc	scan;
	HeapTuple	indexTuple;

	/* Scan pg_index to find indexes on specified heap */
	indexRelation = heap_openr(IndexRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0,
						   Anum_pg_index_indrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(heapId));
	scan = systable_beginscan(indexRelation, IndexIndrelidIndex, true,
							  SnapshotNow, 1, &entry);

	while (HeapTupleIsValid(indexTuple = systable_getnext(scan)))
	{
		Form_pg_index indexform = (Form_pg_index) GETSTRUCT(indexTuple);
		Oid			indexId;
		IndexInfo  *indexInfo;
		Relation	heapRelation,
					currentIndex;

		/*
		 * For each index, fetch info needed for index_build
		 */
		indexId = indexform->indexrelid;
		indexInfo = BuildIndexInfo(indexform);

		/*
		 * We have to re-open the heap rel each time through this loop
		 * because index_build will close it again.  We need grab no lock,
		 * however, because we assume heap_truncate is holding an
		 * exclusive lock on the heap rel.
		 */
		heapRelation = heap_open(heapId, NoLock);

		/* Open the index relation */
		currentIndex = index_open(indexId);

		/* Obtain exclusive lock on it, just to be sure */
		LockRelation(currentIndex, AccessExclusiveLock);

		/*
		 * Drop any buffers associated with this index. If they're dirty,
		 * they're just dropped without bothering to flush to disk.
		 */
		DropRelationBuffers(currentIndex);

		/* Now truncate the actual data and set blocks to zero */
		smgrtruncate(DEFAULT_SMGR, currentIndex, 0);
		currentIndex->rd_nblocks = 0;
		currentIndex->rd_targblock = InvalidBlockNumber;

		/* Initialize the index and rebuild */
		index_build(heapRelation, currentIndex, indexInfo);

		/*
		 * index_build will close both the heap and index relations (but
		 * not give up the locks we hold on them).
		 */
	}

	/* Complete the scan and close pg_index */
	systable_endscan(scan);
	heap_close(indexRelation, AccessShareLock);
}

/*
 *	 heap_truncate
 *
 *	 This routine is used to truncate the data from the
 *	 storage manager of any data within the relation handed
 *	 to this routine.
 */
void
heap_truncate(Oid rid)
{
	Relation	rel;

	/* Open relation for processing, and grab exclusive access on it. */

	rel = heap_open(rid, AccessExclusiveLock);

	/*
	 * TRUNCATE TABLE within a transaction block is dangerous, because if
	 * the transaction is later rolled back we have no way to undo
	 * truncation of the relation's physical file.  Disallow it except for
	 * a rel created in the current xact (which would be deleted on abort,
	 * anyway).
	 */
	if (IsTransactionBlock() && !rel->rd_myxactonly)
		elog(ERROR, "TRUNCATE TABLE cannot run inside a transaction block");

	/*
	 * Release any buffers associated with this relation.  If they're
	 * dirty, they're just dropped without bothering to flush to disk.
	 */
	DropRelationBuffers(rel);

	/* Now truncate the actual data and set blocks to zero */
	smgrtruncate(DEFAULT_SMGR, rel, 0);
	rel->rd_nblocks = 0;
	rel->rd_targblock = InvalidBlockNumber;

	/* If this relation has indexes, truncate the indexes too */
	RelationTruncateIndexes(rid);

	/*
	 * Close the relation, but keep exclusive lock on it until commit.
	 */
	heap_close(rel, NoLock);
}
