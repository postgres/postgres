/*-------------------------------------------------------------------------
 *
 * heap.c
 *	  code to create and destroy POSTGRES heap relations
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/heap.c,v 1.253 2003/09/25 06:57:57 petere Exp $
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
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/var.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
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
					char relkind);
static void AddNewRelationType(const char *typeName,
				   Oid typeNamespace,
				   Oid new_rel_oid,
				   char new_rel_kind,
				   Oid new_type_oid);
static void RelationRemoveInheritance(Relation relation);
static void StoreAttrDefault(Relation rel, AttrNumber attnum, char *adbin);
static void StoreRelCheck(Relation rel, char *ccname, char *ccbin);
static void StoreConstraints(Relation rel, TupleDesc tupdesc);
static void SetRelationNumChecks(Relation rel, int numchecks);
static void RemoveStatistics(Relation rel, AttrNumber attnum);


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
	false, 'p', false, 'i', true, false, false, true, 0
};

static FormData_pg_attribute a2 = {
	0, {"oid"}, OIDOID, 0, sizeof(Oid),
	ObjectIdAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', true, false, false, true, 0
};

static FormData_pg_attribute a3 = {
	0, {"xmin"}, XIDOID, 0, sizeof(TransactionId),
	MinTransactionIdAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', true, false, false, true, 0
};

static FormData_pg_attribute a4 = {
	0, {"cmin"}, CIDOID, 0, sizeof(CommandId),
	MinCommandIdAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', true, false, false, true, 0
};

static FormData_pg_attribute a5 = {
	0, {"xmax"}, XIDOID, 0, sizeof(TransactionId),
	MaxTransactionIdAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', true, false, false, true, 0
};

static FormData_pg_attribute a6 = {
	0, {"cmax"}, CIDOID, 0, sizeof(CommandId),
	MaxCommandIdAttributeNumber, 0, -1, -1,
	true, 'p', false, 'i', true, false, false, true, 0
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
	true, 'p', false, 'i', true, false, false, true, 0
};

static Form_pg_attribute SysAtt[] = {&a1, &a2, &a3, &a4, &a5, &a6, &a7};

/*
 * This function returns a Form_pg_attribute pointer for a system attribute.
 * Note that we elog if the presented attno is invalid, which would only
 * happen if there's a problem upstream.
 */
Form_pg_attribute
SystemAttributeDefinition(AttrNumber attno, bool relhasoids)
{
	if (attno >= 0 || attno < -(int) lengthof(SysAtt))
		elog(ERROR, "invalid system attribute number %d", attno);
	if (attno == ObjectIdAttributeNumber && !relhasoids)
		elog(ERROR, "invalid system attribute number %d", attno);
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
 * else caller must call heap_storage_create later (or not at all,
 * if the relation doesn't need physical storage).
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
	RelFileNode rnode;
	Relation	rel;

	/*
	 * sanity checks
	 */
	if (!allow_system_table_mods &&
	(IsSystemNamespace(relnamespace) || IsToastNamespace(relnamespace)) &&
		IsNormalProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create \"%s.%s\"",
						get_namespace_name(relnamespace), relname),
				 errdetail("System catalog modifications are currently disallowed.")));

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
			relid = RelOid_pg_shadow;
		else if (strcmp(GroupRelationName, relname) == 0)
			relid = RelOid_pg_group;
		else if (strcmp(DatabaseRelationName, relname) == 0)
			relid = RelOid_pg_database;
		else
			relid = newoid();
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
	 * have the storage manager create the relation's disk file, if
	 * wanted.
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
 *		1) CheckAttributeNamesTypes() is used to make certain the tuple
 *		   descriptor contains a valid set of attribute names and types
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
 *		CheckAttributeNamesTypes
 *
 *		this is used to make certain the tuple descriptor contains a
 *		valid set of attribute names and datatypes.  a problem simply
 *		generates ereport(ERROR) which aborts the current transaction.
 * --------------------------------
 */
void
CheckAttributeNamesTypes(TupleDesc tupdesc, char relkind)
{
	int			i;
	int			j;
	int			natts = tupdesc->natts;

	/* Sanity check on column count */
	if (natts < 0 || natts > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	/*
	 * first check for collision with system attribute names
	 *
	 * Skip this for a view or type relation, since those don't have system
	 * attributes.
	 */
	if (relkind != RELKIND_VIEW && relkind != RELKIND_COMPOSITE_TYPE)
	{
		for (i = 0; i < natts; i++)
		{
			if (SystemAttributeByName(NameStr(tupdesc->attrs[i]->attname),
									  tupdesc->tdhasoid) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column name \"%s\" conflicts with a system column name",
								NameStr(tupdesc->attrs[i]->attname))));
		}
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
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column name \"%s\" is duplicated",
								NameStr(tupdesc->attrs[j]->attname))));
		}
	}

	/*
	 * next check the attribute types
	 */
	for (i = 0; i < natts; i++)
	{
		CheckAttributeType(NameStr(tupdesc->attrs[i]->attname),
						   tupdesc->attrs[i]->atttypid);
	}
}

/* --------------------------------
 *		CheckAttributeType
 *
 *		Verify that the proposed datatype of an attribute is legal.
 *		This is needed because there are types (and pseudo-types)
 *		in the catalogs that we do not support as elements of real tuples.
 * --------------------------------
 */
void
CheckAttributeType(const char *attname, Oid atttypid)
{
	char		att_typtype = get_typtype(atttypid);

	/*
	 * Warn user, but don't fail, if column to be created has UNKNOWN type
	 * (usually as a result of a 'retrieve into' - jolly)
	 *
	 * Refuse any attempt to create a pseudo-type column or one that uses a
	 * standalone composite type.  (Eventually we should probably refuse
	 * all references to complex types, but for now there's still some
	 * Berkeley-derived code that thinks it can do this...)
	 */
	if (atttypid == UNKNOWNOID)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("column \"%s\" has type \"unknown\"", attname),
				 errdetail("Proceeding with relation creation anyway.")));
	else if (att_typtype == 'p')
	{
		/* Special hack for pg_statistic: allow ANYARRAY during initdb */
		if (atttypid != ANYARRAYOID || IsUnderPostmaster)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column \"%s\" has pseudo-type %s",
							attname, format_type_be(atttypid))));
	}
	else if (att_typtype == 'c')
	{
		Oid			typrelid = get_typ_typrelid(atttypid);

		if (get_rel_relkind(typrelid) == RELKIND_COMPOSITE_TYPE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column \"%s\" has composite type %s",
							attname, format_type_be(atttypid))));
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
					  char relkind)
{
	Form_pg_attribute *dpp;
	int			i;
	HeapTuple	tup;
	Relation	rel;
	CatalogIndexState indstate;
	int			natts = tupdesc->natts;
	ObjectAddress myself,
				referenced;

	/*
	 * open pg_attribute and its indexes.
	 */
	rel = heap_openr(AttributeRelationName, RowExclusiveLock);

	indstate = CatalogOpenIndexes(rel);

	/*
	 * First we add the user attributes.  This is also a convenient place
	 * to add dependencies on their datatypes.
	 */
	dpp = tupdesc->attrs;
	for (i = 0; i < natts; i++)
	{
		/* Fill in the correct relation OID */
		(*dpp)->attrelid = new_rel_oid;
		/* Make sure these are OK, too */
		(*dpp)->attstattarget = -1;
		(*dpp)->attcacheoff = -1;

		tup = heap_addheader(Natts_pg_attribute,
							 false,
							 ATTRIBUTE_TUPLE_SIZE,
							 (void *) *dpp);

		simple_heap_insert(rel, tup);

		CatalogIndexInsert(indstate, tup);

		heap_freetuple(tup);

		myself.classId = RelOid_pg_class;
		myself.objectId = new_rel_oid;
		myself.objectSubId = i + 1;
		referenced.classId = RelOid_pg_type;
		referenced.objectId = (*dpp)->atttypid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

		dpp++;
	}

	/*
	 * Next we add the system attributes.  Skip OID if rel has no OIDs.
	 * Skip all for a view or type relation.  We don't bother with making
	 * datatype dependencies here, since presumably all these types are
	 * pinned.
	 */
	if (relkind != RELKIND_VIEW && relkind != RELKIND_COMPOSITE_TYPE)
	{
		dpp = SysAtt;
		for (i = 0; i < -1 - FirstLowInvalidHeapAttributeNumber; i++)
		{
			if (tupdesc->tdhasoid ||
				(*dpp)->attnum != ObjectIdAttributeNumber)
			{
				Form_pg_attribute attStruct;

				tup = heap_addheader(Natts_pg_attribute,
									 false,
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

				CatalogIndexInsert(indstate, tup);

				heap_freetuple(tup);
			}
			dpp++;
		}
	}

	/*
	 * clean up
	 */
	CatalogCloseIndexes(indstate);

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
					char relkind)
{
	Form_pg_class new_rel_reltup;
	HeapTuple	tup;

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

	/* ----------------
	 *	now form a tuple to add to pg_class
	 *	XXX Natts_pg_class_fixed is a hack - see pg_class.h
	 * ----------------
	 */
	tup = heap_addheader(Natts_pg_class_fixed,
						 true,
						 CLASS_TUPLE_SIZE,
						 (void *) new_rel_reltup);

	/* force tuple to have the desired OID */
	HeapTupleSetOid(tup, new_rel_oid);

	/*
	 * finally insert the new tuple, update the indexes, and clean up.
	 */
	simple_heap_insert(pg_class_desc, tup);

	CatalogUpdateIndexes(pg_class_desc, tup);

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
				   char new_rel_kind,
				   Oid new_type_oid)
{
	/*
	 * We set the I/O procedures of a complex type to record_in and
	 * record_out, so that a user will get an error message not a weird
	 * number if he tries to SELECT a complex type.
	 *
	 * OLD and probably obsolete comments:
	 *
	 * The sizes are set to oid size because it makes implementing sets MUCH
	 * easier, and no one (we hope) uses these fields to figure out how
	 * much space to allocate for the type. An oid is the type used for a
	 * set definition.	When a user requests a set, what they actually get
	 * is the oid of a tuple in the pg_proc catalog, so the size of the
	 * "set" is the size of an oid. Similarly, byval being true makes sets
	 * much easier, and it isn't used by anything else.
	 */
	TypeCreate(typeName,		/* type name */
			   typeNamespace,	/* type namespace */
			   new_type_oid,	/* preassigned oid for type */
			   new_rel_oid,		/* relation oid */
			   new_rel_kind,	/* relation kind */
			   sizeof(Oid),		/* internal size */
			   'c',				/* type-type (complex) */
			   ',',				/* default array delimiter */
			   F_RECORD_IN,		/* input procedure */
			   F_RECORD_OUT,	/* output procedure */
			   F_RECORD_RECV,	/* receive procedure */
			   F_RECORD_SEND,	/* send procedure */
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
						 OnCommitAction oncommit,
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

	CheckAttributeNamesTypes(tupdesc, relkind);

	if (get_relname_relid(relname, relnamespace))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists", relname)));

	/*
	 * Create the relcache entry (mostly dummy at this point) and the
	 * physical disk file.	(If we fail further down, it's the smgr's
	 * responsibility to remove the disk file again.)
	 *
	 * NB: create a physical file only if it's not a view or type relation.
	 */
	new_rel_desc = heap_create(relname,
							   relnamespace,
							   tupdesc,
							   shared_relation,
							   (relkind != RELKIND_VIEW &&
								relkind != RELKIND_COMPOSITE_TYPE),
							   allow_system_table_mods);

	/* Fetch the relation OID assigned by heap_create */
	new_rel_oid = RelationGetRelid(new_rel_desc);

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
						relkind);

	/*
	 * since defining a relation also defines a complex type, we add a new
	 * system type corresponding to the new relation.
	 *
	 * NOTE: we could get a unique-index failure here, in case the same name
	 * has already been used for a type.
	 */
	AddNewRelationType(relname,
					   relnamespace,
					   new_rel_oid,
					   relkind,
					   new_type_oid);

	/*
	 * now add tuples to pg_attribute for the attributes in our new
	 * relation.
	 */
	AddNewAttributeTuples(new_rel_oid, new_rel_desc->rd_att, relkind);

	/*
	 * make a dependency link to force the relation to be deleted if its
	 * namespace is.  Skip this in bootstrap mode, since we don't make
	 * dependencies while bootstrapping.
	 */
	if (!IsBootstrapProcessingMode())
	{
		ObjectAddress myself,
					referenced;

		myself.classId = RelOid_pg_class;
		myself.objectId = new_rel_oid;
		myself.objectSubId = 0;
		referenced.classId = get_system_catalog_relid(NamespaceRelationName);
		referenced.objectId = relnamespace;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/*
	 * store constraints and defaults passed in the tupdesc, if any.
	 *
	 * NB: this may do a CommandCounterIncrement and rebuild the relcache
	 * entry, so the relation must be valid and self-consistent at this
	 * point. In particular, there are not yet constraints and defaults
	 * anywhere.
	 */
	StoreConstraints(new_rel_desc, tupdesc);

	/*
	 * If there's a special on-commit action, remember it
	 */
	if (oncommit != ONCOMMIT_NOOP)
		register_on_commit_action(new_rel_oid, oncommit);

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
 * deletion if any were found.	Now we rely on the dependency mechanism
 * to check for or delete child relations.	By the time we get here,
 * there are no children and we need only remove any pg_inherits rows
 * linking this relation to its parent(s).
 */
static void
RelationRemoveInheritance(Relation relation)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;

	catalogRelation = heap_openr(InheritsRelationName, RowExclusiveLock);

	ScanKeyEntryInitialize(&key, 0x0,
						   Anum_pg_inherits_inhrelid, F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));

	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndex, true,
							  SnapshotNow, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		simple_heap_delete(catalogRelation, &tuple->t_self);

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
		elog(ERROR, "cache lookup failed for relation %u", relid);

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
	SysScanDesc scan;
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
		simple_heap_delete(attrel, &atttup->t_self);

	/* Clean up after the scan */
	systable_endscan(scan);
	heap_close(attrel, RowExclusiveLock);
}

/*
 *		RemoveAttributeById
 *
 * This is the guts of ALTER TABLE DROP COLUMN: actually mark the attribute
 * deleted in pg_attribute.  We also remove pg_statistic entries for it.
 * (Everything else needed, such as getting rid of any pg_attrdef entry,
 * is handled by dependency.c.)
 */
void
RemoveAttributeById(Oid relid, AttrNumber attnum)
{
	Relation	rel;
	Relation	attr_rel;
	HeapTuple	tuple;
	Form_pg_attribute attStruct;
	char		newattname[NAMEDATALEN];

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.  (In the simple case where we are
	 * directly dropping this column, AlterTableDropColumn already did
	 * this ... but when cascading from a drop of some other object, we
	 * may not have any lock.)
	 */
	rel = relation_open(relid, AccessExclusiveLock);

	attr_rel = heap_openr(AttributeRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(ATTNUM,
							   ObjectIdGetDatum(relid),
							   Int16GetDatum(attnum),
							   0, 0);
	if (!HeapTupleIsValid(tuple))		/* shouldn't happen */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	attStruct = (Form_pg_attribute) GETSTRUCT(tuple);

	/* Mark the attribute as dropped */
	attStruct->attisdropped = true;

	/*
	 * Set the type OID to invalid.  A dropped attribute's type link
	 * cannot be relied on (once the attribute is dropped, the type might
	 * be too). Fortunately we do not need the type row --- the only
	 * really essential information is the type's typlen and typalign,
	 * which are preserved in the attribute's attlen and attalign.  We set
	 * atttypid to zero here as a means of catching code that incorrectly
	 * expects it to be valid.
	 */
	attStruct->atttypid = InvalidOid;

	/* Remove any NOT NULL constraint the column may have */
	attStruct->attnotnull = false;

	/* We don't want to keep stats for it anymore */
	attStruct->attstattarget = 0;

	/* Change the column name to something that isn't likely to conflict */
	snprintf(newattname, sizeof(newattname),
			 "........pg.dropped.%d........", attnum);
	namestrcpy(&(attStruct->attname), newattname);

	simple_heap_update(attr_rel, &tuple->t_self, tuple);

	/* keep the system catalog indexes current */
	CatalogUpdateIndexes(attr_rel, tuple);

	/*
	 * Because updating the pg_attribute row will trigger a relcache flush
	 * for the target relation, we need not do anything else to notify
	 * other backends of the change.
	 */

	heap_close(attr_rel, RowExclusiveLock);

	RemoveStatistics(rel, attnum);

	relation_close(rel, NoLock);
}

/*
 *		RemoveAttrDefault
 *
 * If the specified relation/attribute has a default, remove it.
 * (If no default, raise error if complain is true, else return quietly.)
 */
void
RemoveAttrDefault(Oid relid, AttrNumber attnum,
				  DropBehavior behavior, bool complain)
{
	Relation	attrdef_rel;
	ScanKeyData scankeys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;

	attrdef_rel = heap_openr(AttrDefaultRelationName, RowExclusiveLock);

	ScanKeyEntryInitialize(&scankeys[0], 0x0,
						   Anum_pg_attrdef_adrelid, F_OIDEQ,
						   ObjectIdGetDatum(relid));
	ScanKeyEntryInitialize(&scankeys[1], 0x0,
						   Anum_pg_attrdef_adnum, F_INT2EQ,
						   Int16GetDatum(attnum));

	scan = systable_beginscan(attrdef_rel, AttrDefaultIndex, true,
							  SnapshotNow, 2, scankeys);

	/* There should be at most one matching tuple, but we loop anyway */
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ObjectAddress object;

		object.classId = RelationGetRelid(attrdef_rel);
		object.objectId = HeapTupleGetOid(tuple);
		object.objectSubId = 0;

		performDeletion(&object, behavior);

		found = true;
	}

	systable_endscan(scan);
	heap_close(attrdef_rel, RowExclusiveLock);

	if (complain && !found)
		elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
			 relid, attnum);
}

/*
 *		RemoveAttrDefaultById
 *
 * Remove a pg_attrdef entry specified by OID.	This is the guts of
 * attribute-default removal.  Note it should be called via performDeletion,
 * not directly.
 */
void
RemoveAttrDefaultById(Oid attrdefId)
{
	Relation	attrdef_rel;
	Relation	attr_rel;
	Relation	myrel;
	ScanKeyData scankeys[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			myrelid;
	AttrNumber	myattnum;

	/* Grab an appropriate lock on the pg_attrdef relation */
	attrdef_rel = heap_openr(AttrDefaultRelationName, RowExclusiveLock);

	/* Find the pg_attrdef tuple */
	ScanKeyEntryInitialize(&scankeys[0], 0x0,
						   ObjectIdAttributeNumber, F_OIDEQ,
						   ObjectIdGetDatum(attrdefId));

	scan = systable_beginscan(attrdef_rel, AttrDefaultOidIndex, true,
							  SnapshotNow, 1, scankeys);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for attrdef %u", attrdefId);

	myrelid = ((Form_pg_attrdef) GETSTRUCT(tuple))->adrelid;
	myattnum = ((Form_pg_attrdef) GETSTRUCT(tuple))->adnum;

	/* Get an exclusive lock on the relation owning the attribute */
	myrel = relation_open(myrelid, AccessExclusiveLock);

	/* Now we can delete the pg_attrdef row */
	simple_heap_delete(attrdef_rel, &tuple->t_self);

	systable_endscan(scan);
	heap_close(attrdef_rel, RowExclusiveLock);

	/* Fix the pg_attribute row */
	attr_rel = heap_openr(AttributeRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(ATTNUM,
							   ObjectIdGetDatum(myrelid),
							   Int16GetDatum(myattnum),
							   0, 0);
	if (!HeapTupleIsValid(tuple))		/* shouldn't happen */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 myattnum, myrelid);

	((Form_pg_attribute) GETSTRUCT(tuple))->atthasdef = false;

	simple_heap_update(attr_rel, &tuple->t_self, tuple);

	/* keep the system catalog indexes current */
	CatalogUpdateIndexes(attr_rel, tuple);

	/*
	 * Our update of the pg_attribute row will force a relcache rebuild,
	 * so there's nothing else to do here.
	 */
	heap_close(attr_rel, RowExclusiveLock);

	/* Keep lock on attribute's rel until end of xact */
	relation_close(myrel, NoLock);
}

/* ----------------------------------------------------------------
 *		heap_drop_with_catalog	- removes specified relation from catalogs
 *
 *		1)	open relation, acquire exclusive lock.
 *		2)	flush relation buffers from bufmgr
 *		3)	remove inheritance information
 *		4)	remove pg_statistic tuples
 *		5)	remove pg_attribute tuples
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
	rel = relation_open(rid, AccessExclusiveLock);

	/*
	 * Release all buffers that belong to this relation, after writing any
	 * that are dirty
	 */
	i = FlushRelationBuffers(rel, (BlockNumber) 0);
	if (i < 0)
		elog(ERROR, "FlushRelationBuffers returned %d", i);

	/*
	 * remove inheritance information
	 */
	RelationRemoveInheritance(rel);

	/*
	 * delete statistics
	 */
	RemoveStatistics(rel, 0);

	/*
	 * delete attribute tuples
	 */
	DeleteAttributeTuples(RelationGetRelid(rel));

	/*
	 * delete relation tuple
	 */
	DeleteRelationTuple(RelationGetRelid(rel));

	/*
	 * forget any ON COMMIT action for the rel
	 */
	remove_on_commit_action(rid);

	/*
	 * unlink the relation's physical file and finish up.
	 */
	if (rel->rd_rel->relkind != RELKIND_VIEW &&
		rel->rd_rel->relkind != RELKIND_COMPOSITE_TYPE)
		smgrunlink(DEFAULT_SMGR, rel);

	/*
	 * Close relcache entry, but *keep* AccessExclusiveLock on the
	 * relation until transaction commit.  This ensures no one else will
	 * try to do something with the doomed relation.
	 */
	relation_close(rel, NoLock);

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
	HeapTuple	tuple;
	Datum		values[4];
	static char nulls[4] = {' ', ' ', ' ', ' '};
	Relation	attrrel;
	HeapTuple	atttup;
	Form_pg_attribute attStruct;
	Oid			attrdefOid;
	ObjectAddress colobject,
				defobject;

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
							   false, false);

	/*
	 * Make the pg_attrdef entry.
	 */
	values[Anum_pg_attrdef_adrelid - 1] = RelationGetRelid(rel);
	values[Anum_pg_attrdef_adnum - 1] = attnum;
	values[Anum_pg_attrdef_adbin - 1] = DirectFunctionCall1(textin,
												 CStringGetDatum(adbin));
	values[Anum_pg_attrdef_adsrc - 1] = DirectFunctionCall1(textin,
												 CStringGetDatum(adsrc));

	adrel = heap_openr(AttrDefaultRelationName, RowExclusiveLock);

	tuple = heap_formtuple(adrel->rd_att, values, nulls);
	attrdefOid = simple_heap_insert(adrel, tuple);

	CatalogUpdateIndexes(adrel, tuple);

	defobject.classId = RelationGetRelid(adrel);
	defobject.objectId = attrdefOid;
	defobject.objectSubId = 0;

	heap_close(adrel, RowExclusiveLock);

	/* now can free some of the stuff allocated above */
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
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, RelationGetRelid(rel));
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);
	if (!attStruct->atthasdef)
	{
		attStruct->atthasdef = true;
		simple_heap_update(attrrel, &atttup->t_self, atttup);
		/* keep catalog indexes current */
		CatalogUpdateIndexes(attrrel, atttup);
	}
	heap_close(attrrel, RowExclusiveLock);
	heap_freetuple(atttup);

	/*
	 * Make a dependency so that the pg_attrdef entry goes away if the
	 * column (or whole table) is deleted.
	 */
	colobject.classId = RelOid_pg_class;
	colobject.objectId = RelationGetRelid(rel);
	colobject.objectSubId = attnum;

	recordDependencyOn(&defobject, &colobject, DEPENDENCY_AUTO);

	/*
	 * Record dependencies on objects used in the expression, too.
	 */
	recordDependencyOnExpr(&defobject, expr, NIL, DEPENDENCY_NORMAL);
}

/*
 * Store a check-constraint expression for the given relation.
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
							   false, false);

	/*
	 * Find columns of rel that are used in ccbin
	 *
	 * NB: pull_var_clause is okay here only because we don't allow
	 * subselects in check constraints; it would fail to examine the
	 * contents of subselects.
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
			Var		   *var = (Var *) lfirst(vl);
			int			j;

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
	CreateConstraintEntry(ccname,		/* Constraint Name */
						  RelationGetNamespace(rel),	/* namespace */
						  CONSTRAINT_CHECK,		/* Constraint Type */
						  false,	/* Is Deferrable */
						  false,	/* Is Deferred */
						  RelationGetRelid(rel),		/* relation */
						  attNos,		/* attrs in the constraint */
						  keycount,		/* # attrs in the constraint */
						  InvalidOid,	/* not a domain constraint */
						  InvalidOid,	/* Foreign key fields */
						  NULL,
						  0,
						  ' ',
						  ' ',
						  ' ',
						  InvalidOid,	/* no associated index */
						  expr, /* Tree form check constraint */
						  ccbin,	/* Binary form check constraint */
						  ccsrc);		/* Source form check constraint */

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
	 * visible.  So, bump the command counter.	CAUTION: this will cause a
	 * relcache entry rebuild.
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
			if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
									 RelationGetRelid(rel),
									 RelationGetNamespace(rel),
									 ccname))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("constraint \"%s\" for relation \"%s\" already exists",
								ccname, RelationGetRelationName(rel))));
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
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("check constraint \"%s\" already exists",
								ccname)));
			}
		}
		else
		{
			bool		success;

			do
			{
				List	   *listptr2;

				/*
				 * Generate a name that does not conflict with
				 * pre-existing constraints, nor with any auto-generated
				 * names so far.
				 */
				ccname = GenerateConstraintName(CONSTRAINT_RELATION,
												RelationGetRelid(rel),
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
		expr = coerce_to_boolean(pstate, expr, "CHECK");

		/*
		 * Make sure no outside relations are referred to.
		 */
		if (length(pstate->p_rtable) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("only table \"%s\" can be referenced in check constraint",
							relname)));

		/*
		 * No subplans or aggregates, either...
		 */
		if (pstate->p_hasSubLinks)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				   errmsg("cannot use subquery in check constraint")));
		if (pstate->p_hasAggs)
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("cannot use aggregate function in check constraint")));

		/*
		 * Constraints are evaluated with execQual, which expects an
		 * implicit-AND list, so convert expression to implicit-AND form.
		 * (We could go so far as to convert to CNF, but that's probably
		 * overkill...)
		 */
		expr = (Node *) make_ands_implicit((Expr *) expr);

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

	relrel = heap_openr(RelationRelationName, RowExclusiveLock);
	reltup = SearchSysCacheCopy(RELOID,
								ObjectIdGetDatum(RelationGetRelid(rel)),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));
	relStruct = (Form_pg_class) GETSTRUCT(reltup);

	if (relStruct->relchecks != numchecks)
	{
		relStruct->relchecks = numchecks;

		simple_heap_update(relrel, &reltup->t_self, reltup);

		/* keep catalog indexes current */
		CatalogUpdateIndexes(relrel, reltup);
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
 * If atttypid is not InvalidOid, coerce the expression to the specified
 * type (and typmod atttypmod).   attname is only needed in this case:
 * it is used in the error message, if any.
 */
Node *
cookDefault(ParseState *pstate,
			Node *raw_default,
			Oid atttypid,
			int32 atttypmod,
			char *attname)
{
	Node	   *expr;

	Assert(raw_default != NULL);

	/*
	 * Transform raw parsetree to executable expression.
	 */
	expr = transformExpr(pstate, raw_default);

	/*
	 * Make sure default expr does not refer to any vars.
	 */
	if (contain_var_clause(expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
			  errmsg("cannot use column references in default expression")));

	/*
	 * It can't return a set either.
	 */
	if (expression_returns_set(expr))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("default expression must not return a set")));

	/*
	 * No subplans or aggregates, either...
	 */
	if (pstate->p_hasSubLinks)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use subquery in default expression")));
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate function in default expression")));

	/*
	 * Coerce the expression to the correct type and typmod, if given.
	 * This should match the parser's processing of non-defaulted
	 * expressions --- see updateTargetListEntry().
	 */
	if (OidIsValid(atttypid))
	{
		Oid			type_id = exprType(expr);

		expr = coerce_to_target_type(pstate, expr, type_id,
									 atttypid, atttypmod,
									 COERCION_ASSIGNMENT,
									 COERCE_IMPLICIT_CAST);
		if (expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is of type %s"
							" but default expression is of type %s",
							attname,
							format_type_be(atttypid),
							format_type_be(type_id)),
			errhint("You will need to rewrite or cast the expression.")));
	}

	return expr;
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
	SysScanDesc conscan;
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
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(contup);

		if (strcmp(NameStr(con->conname), constrName) == 0)
		{
			ObjectAddress conobj;

			conobj.classId = RelationGetRelid(conrel);
			conobj.objectId = HeapTupleGetOid(contup);
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


/*
 * RemoveStatistics --- remove entries in pg_statistic for a rel or column
 *
 * If attnum is zero, remove all entries for rel; else remove only the one
 * for that column.
 */
static void
RemoveStatistics(Relation rel, AttrNumber attnum)
{
	Relation	pgstatistic;
	SysScanDesc scan;
	ScanKeyData key[2];
	int			nkeys;
	HeapTuple	tuple;

	pgstatistic = heap_openr(StatisticRelationName, RowExclusiveLock);

	ScanKeyEntryInitialize(&key[0], 0x0,
						   Anum_pg_statistic_starelid, F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(rel)));

	if (attnum == 0)
		nkeys = 1;
	else
	{
		ScanKeyEntryInitialize(&key[1], 0x0,
							   Anum_pg_statistic_staattnum, F_INT2EQ,
							   Int16GetDatum(attnum));
		nkeys = 2;
	}

	scan = systable_beginscan(pgstatistic, StatisticRelidAttnumIndex, true,
							  SnapshotNow, nkeys, key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		simple_heap_delete(pgstatistic, &tuple->t_self);

	systable_endscan(scan);

	heap_close(pgstatistic, RowExclusiveLock);
}


/*
 * RelationTruncateIndexes - truncate all
 * indexes associated with the heap relation to zero tuples.
 *
 * The routine will truncate and then reconstruct the indexes on
 * the relation specified by the heapId parameter.
 */
static void
RelationTruncateIndexes(Oid heapId)
{
	Relation	heapRelation;
	List	   *indlist;

	/*
	 * Open the heap rel.  We need grab no lock because we assume
	 * heap_truncate is holding an exclusive lock on the heap rel.
	 */
	heapRelation = heap_open(heapId, NoLock);

	/* Ask the relcache to produce a list of the indexes of the rel */
	foreach(indlist, RelationGetIndexList(heapRelation))
	{
		Oid			indexId = lfirsto(indlist);
		Relation	currentIndex;
		IndexInfo  *indexInfo;

		/* Open the index relation */
		currentIndex = index_open(indexId);

		/* Obtain exclusive lock on it, just to be sure */
		LockRelation(currentIndex, AccessExclusiveLock);

		/* Fetch info needed for index_build */
		indexInfo = BuildIndexInfo(currentIndex);

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
		 * not give up the locks we hold on them).	We're done with this
		 * index, but we must re-open the heap rel.
		 */
		heapRelation = heap_open(heapId, NoLock);
	}

	/* Finish by closing the heap rel again */
	heap_close(heapRelation, NoLock);
}

/*
 *	 heap_truncate
 *
 *	 This routine deletes all data within the specified relation.
 *
 * This is not transaction-safe!  There is another, transaction-safe
 * implementation in commands/cluster.c.  We now use this only for
 * ON COMMIT truncation of temporary tables, where it doesn't matter.
 */
void
heap_truncate(Oid rid)
{
	Relation	rel;
	Oid			toastrelid;

	/* Open relation for processing, and grab exclusive access on it. */
	rel = heap_open(rid, AccessExclusiveLock);

	/* Don't allow truncate on tables that are referenced by foreign keys */
	heap_truncate_check_FKs(rel);

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

	/* If it has a toast table, recursively truncate that too */
	toastrelid = rel->rd_rel->reltoastrelid;
	if (OidIsValid(toastrelid))
		heap_truncate(toastrelid);

	/*
	 * Close the relation, but keep exclusive lock on it until commit.
	 */
	heap_close(rel, NoLock);
}

/*
 * heap_truncate_check_FKs
 *		Check for foreign keys referencing a relation that's to be truncated
 *
 * We disallow such FKs (except self-referential ones) since the whole point
 * of TRUNCATE is to not scan the individual rows to be thrown away.
 *
 * This is split out so it can be shared by both implementations of truncate.
 * Caller should already hold a suitable lock on the relation.
 */
void
heap_truncate_check_FKs(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	ScanKeyData key;
	Relation	fkeyRel;
	SysScanDesc fkeyScan;
	HeapTuple	tuple;

	/*
	 * Fast path: if the relation has no triggers, it surely has no FKs
	 * either.
	 */
	if (rel->rd_rel->reltriggers == 0)
		return;

	/*
	 * Otherwise, must scan pg_constraint.  Right now, this is a seqscan
	 * because there is no available index on confrelid.
	 */
	fkeyRel = heap_openr(ConstraintRelationName, AccessShareLock);

	ScanKeyEntryInitialize(&key, 0,
						   Anum_pg_constraint_confrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(relid));

	fkeyScan = systable_beginscan(fkeyRel, NULL, false,
								  SnapshotNow, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(fkeyScan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

		if (con->contype == CONSTRAINT_FOREIGN && con->conrelid != relid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot truncate a table referenced in a foreign key constraint"),
					 errdetail("Table \"%s\" references \"%s\" via foreign key constraint \"%s\".",
							   get_rel_name(con->conrelid),
							   RelationGetRelationName(rel),
							   NameStr(con->conname))));
	}

	systable_endscan(fkeyScan);
	heap_close(fkeyRel, AccessShareLock);
}
