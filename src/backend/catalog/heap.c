/*-------------------------------------------------------------------------
 *
 * heap.c
 *	  code to create and destroy POSTGRES heap relations
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/heap.c
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
 *	  just like the poorly named "NewXXX" routines do.  The
 *	  "New" routines are all going to die soon, once and for all!
 *		-cim 1/13/91
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/tablecmds.h"
#include "commands/typecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* Potentially set by pg_upgrade_support functions */
Oid			binary_upgrade_next_heap_pg_class_oid = InvalidOid;
Oid			binary_upgrade_next_toast_pg_class_oid = InvalidOid;

static void AddNewRelationTuple(Relation pg_class_desc,
								Relation new_rel_desc,
								Oid new_rel_oid,
								Oid new_type_oid,
								Oid reloftype,
								Oid relowner,
								char relkind,
								TransactionId relfrozenxid,
								TransactionId relminmxid,
								Datum relacl,
								Datum reloptions);
static ObjectAddress AddNewRelationType(const char *typeName,
										Oid typeNamespace,
										Oid new_rel_oid,
										char new_rel_kind,
										Oid ownerid,
										Oid new_row_type,
										Oid new_array_type);
static void RelationRemoveInheritance(Oid relid);
static Oid	StoreRelCheck(Relation rel, const char *ccname, Node *expr,
						  bool is_validated, bool is_local, int inhcount,
						  bool is_no_inherit, bool is_internal);
static void StoreConstraints(Relation rel, List *cooked_constraints,
							 bool is_internal);
static bool MergeWithExistingConstraint(Relation rel, const char *ccname, Node *expr,
										bool allow_merge, bool is_local,
										bool is_initially_valid,
										bool is_no_inherit);
static void SetRelationNumChecks(Relation rel, int numchecks);
static Node *cookConstraint(ParseState *pstate,
							Node *raw_constraint,
							char *relname);


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

/*
 * The initializers below do not include trailing variable length fields,
 * but that's OK - we're never going to reference anything beyond the
 * fixed-size portion of the structure anyway.
 */

static const FormData_pg_attribute a1 = {
	.attname = {"ctid"},
	.atttypid = TIDOID,
	.attlen = sizeof(ItemPointerData),
	.attnum = SelfItemPointerAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = false,
	.attstorage = TYPSTORAGE_PLAIN,
	.attalign = TYPALIGN_SHORT,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a2 = {
	.attname = {"xmin"},
	.atttypid = XIDOID,
	.attlen = sizeof(TransactionId),
	.attnum = MinTransactionIdAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = true,
	.attstorage = TYPSTORAGE_PLAIN,
	.attalign = TYPALIGN_INT,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a3 = {
	.attname = {"cmin"},
	.atttypid = CIDOID,
	.attlen = sizeof(CommandId),
	.attnum = MinCommandIdAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = true,
	.attstorage = TYPSTORAGE_PLAIN,
	.attalign = TYPALIGN_INT,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a4 = {
	.attname = {"xmax"},
	.atttypid = XIDOID,
	.attlen = sizeof(TransactionId),
	.attnum = MaxTransactionIdAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = true,
	.attstorage = TYPSTORAGE_PLAIN,
	.attalign = TYPALIGN_INT,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a5 = {
	.attname = {"cmax"},
	.atttypid = CIDOID,
	.attlen = sizeof(CommandId),
	.attnum = MaxCommandIdAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = true,
	.attstorage = TYPSTORAGE_PLAIN,
	.attalign = TYPALIGN_INT,
	.attnotnull = true,
	.attislocal = true,
};

/*
 * We decided to call this attribute "tableoid" rather than say
 * "classoid" on the basis that in the future there may be more than one
 * table of a particular class/type. In any case table is still the word
 * used in SQL.
 */
static const FormData_pg_attribute a6 = {
	.attname = {"tableoid"},
	.atttypid = OIDOID,
	.attlen = sizeof(Oid),
	.attnum = TableOidAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = true,
	.attstorage = TYPSTORAGE_PLAIN,
	.attalign = TYPALIGN_INT,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute *SysAtt[] = {&a1, &a2, &a3, &a4, &a5, &a6};

/*
 * This function returns a Form_pg_attribute pointer for a system attribute.
 * Note that we elog if the presented attno is invalid, which would only
 * happen if there's a problem upstream.
 */
const FormData_pg_attribute *
SystemAttributeDefinition(AttrNumber attno)
{
	if (attno >= 0 || attno < -(int) lengthof(SysAtt))
		elog(ERROR, "invalid system attribute number %d", attno);
	return SysAtt[-attno - 1];
}

/*
 * If the given name is a system attribute name, return a Form_pg_attribute
 * pointer for a prototype definition.  If not, return NULL.
 */
const FormData_pg_attribute *
SystemAttributeByName(const char *attname)
{
	int			j;

	for (j = 0; j < (int) lengthof(SysAtt); j++)
	{
		const FormData_pg_attribute *att = SysAtt[j];

		if (strcmp(NameStr(att->attname), attname) == 0)
			return att;
	}

	return NULL;
}


/* ----------------------------------------------------------------
 *				XXX END OF UGLY HARD CODED BADNESS XXX
 * ---------------------------------------------------------------- */


/* ----------------------------------------------------------------
 *		heap_create		- Create an uncataloged heap relation
 *
 *		Note API change: the caller must now always provide the OID
 *		to use for the relation.  The relfilenode may (and, normally,
 *		should) be left unspecified.
 *
 *		rel->rd_rel is initialized by RelationBuildLocalRelation,
 *		and is mostly zeroes at return.
 * ----------------------------------------------------------------
 */
Relation
heap_create(const char *relname,
			Oid relnamespace,
			Oid reltablespace,
			Oid relid,
			Oid relfilenode,
			Oid accessmtd,
			TupleDesc tupDesc,
			char relkind,
			char relpersistence,
			bool shared_relation,
			bool mapped_relation,
			bool allow_system_table_mods,
			TransactionId *relfrozenxid,
			MultiXactId *relminmxid)
{
	bool		create_storage;
	Relation	rel;

	/* The caller must have provided an OID for the relation. */
	Assert(OidIsValid(relid));

	/*
	 * Don't allow creating relations in pg_catalog directly, even though it
	 * is allowed to move user defined relations there. Semantics with search
	 * paths including pg_catalog are too confusing for now.
	 *
	 * But allow creating indexes on relations in pg_catalog even if
	 * allow_system_table_mods = off, upper layers already guarantee it's on a
	 * user defined relation, not a system one.
	 */
	if (!allow_system_table_mods &&
		((IsCatalogNamespace(relnamespace) && relkind != RELKIND_INDEX) ||
		 IsToastNamespace(relnamespace)) &&
		IsNormalProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create \"%s.%s\"",
						get_namespace_name(relnamespace), relname),
				 errdetail("System catalog modifications are currently disallowed.")));

	*relfrozenxid = InvalidTransactionId;
	*relminmxid = InvalidMultiXactId;

	/* Handle reltablespace for specific relkinds. */
	switch (relkind)
	{
		case RELKIND_VIEW:
		case RELKIND_COMPOSITE_TYPE:
		case RELKIND_FOREIGN_TABLE:

			/*
			 * Force reltablespace to zero if the relation has no physical
			 * storage.  This is mainly just for cleanliness' sake.
			 *
			 * Partitioned tables and indexes don't have physical storage
			 * either, but we want to keep their tablespace settings so that
			 * their children can inherit it.
			 */
			reltablespace = InvalidOid;
			break;

		case RELKIND_SEQUENCE:

			/*
			 * Force reltablespace to zero for sequences, since we don't
			 * support moving them around into different tablespaces.
			 */
			reltablespace = InvalidOid;
			break;
		default:
			break;
	}

	/*
	 * Decide whether to create storage. If caller passed a valid relfilenode,
	 * storage is already created, so don't do it here.  Also don't create it
	 * for relkinds without physical storage.
	 */
	if (!RELKIND_HAS_STORAGE(relkind) || OidIsValid(relfilenode))
		create_storage = false;
	else
	{
		create_storage = true;
		relfilenode = relid;
	}

	/*
	 * Never allow a pg_class entry to explicitly specify the database's
	 * default tablespace in reltablespace; force it to zero instead. This
	 * ensures that if the database is cloned with a different default
	 * tablespace, the pg_class entry will still match where CREATE DATABASE
	 * will put the physically copied relation.
	 *
	 * Yes, this is a bit of a hack.
	 */
	if (reltablespace == MyDatabaseTableSpace)
		reltablespace = InvalidOid;

	/*
	 * build the relcache entry.
	 */
	rel = RelationBuildLocalRelation(relname,
									 relnamespace,
									 tupDesc,
									 relid,
									 accessmtd,
									 relfilenode,
									 reltablespace,
									 shared_relation,
									 mapped_relation,
									 relpersistence,
									 relkind);

	/*
	 * Have the storage manager create the relation's disk file, if needed.
	 *
	 * For relations the callback creates both the main and the init fork, for
	 * indexes only the main fork is created. The other forks will be created
	 * on demand.
	 */
	if (create_storage)
	{
		switch (rel->rd_rel->relkind)
		{
			case RELKIND_VIEW:
			case RELKIND_COMPOSITE_TYPE:
			case RELKIND_FOREIGN_TABLE:
			case RELKIND_PARTITIONED_TABLE:
			case RELKIND_PARTITIONED_INDEX:
				Assert(false);
				break;

			case RELKIND_INDEX:
			case RELKIND_SEQUENCE:
				RelationCreateStorage(rel->rd_node, relpersistence);
				break;

			case RELKIND_RELATION:
			case RELKIND_TOASTVALUE:
			case RELKIND_MATVIEW:
				table_relation_set_new_filenode(rel, &rel->rd_node,
												relpersistence,
												relfrozenxid, relminmxid);
				break;
		}
	}

	/*
	 * If a tablespace is specified, removal of that tablespace is normally
	 * protected by the existence of a physical file; but for relations with
	 * no files, add a pg_shdepend entry to account for that.
	 */
	if (!create_storage && reltablespace != InvalidOid)
		recordDependencyOnTablespace(RelationRelationId, relid,
									 reltablespace);

	return rel;
}

/* ----------------------------------------------------------------
 *		heap_create_with_catalog		- Create a cataloged relation
 *
 *		this is done in multiple steps:
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
 *		4) TypeCreate() is called to define a new type corresponding
 *		   to the new relation.
 *
 *		5) AddNewRelationTuple() is called to register the
 *		   relation in pg_class.
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
 *
 *		relkind is the relkind of the relation to be created.
 *		flags controls which datatypes are allowed, cf CheckAttributeType.
 * --------------------------------
 */
void
CheckAttributeNamesTypes(TupleDesc tupdesc, char relkind,
						 int flags)
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
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (SystemAttributeByName(NameStr(attr->attname)) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column name \"%s\" conflicts with a system column name",
								NameStr(attr->attname))));
		}
	}

	/*
	 * next check for repeated attribute names
	 */
	for (i = 1; i < natts; i++)
	{
		for (j = 0; j < i; j++)
		{
			if (strcmp(NameStr(TupleDescAttr(tupdesc, j)->attname),
					   NameStr(TupleDescAttr(tupdesc, i)->attname)) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column name \"%s\" specified more than once",
								NameStr(TupleDescAttr(tupdesc, j)->attname))));
		}
	}

	/*
	 * next check the attribute types
	 */
	for (i = 0; i < natts; i++)
	{
		CheckAttributeType(NameStr(TupleDescAttr(tupdesc, i)->attname),
						   TupleDescAttr(tupdesc, i)->atttypid,
						   TupleDescAttr(tupdesc, i)->attcollation,
						   NIL, /* assume we're creating a new rowtype */
						   flags);
	}
}

/* --------------------------------
 *		CheckAttributeType
 *
 *		Verify that the proposed datatype of an attribute is legal.
 *		This is needed mainly because there are types (and pseudo-types)
 *		in the catalogs that we do not support as elements of real tuples.
 *		We also check some other properties required of a table column.
 *
 * If the attribute is being proposed for addition to an existing table or
 * composite type, pass a one-element list of the rowtype OID as
 * containing_rowtypes.  When checking a to-be-created rowtype, it's
 * sufficient to pass NIL, because there could not be any recursive reference
 * to a not-yet-existing rowtype.
 *
 * flags is a bitmask controlling which datatypes we allow.  For the most
 * part, pseudo-types are disallowed as attribute types, but there are some
 * exceptions: ANYARRAYOID, RECORDOID, and RECORDARRAYOID can be allowed
 * in some cases.  (This works because values of those type classes are
 * self-identifying to some extent.  However, RECORDOID and RECORDARRAYOID
 * are reliably identifiable only within a session, since the identity info
 * may use a typmod that is only locally assigned.  The caller is expected
 * to know whether these cases are safe.)
 *
 * flags can also control the phrasing of the error messages.  If
 * CHKATYPE_IS_PARTKEY is specified, "attname" should be a partition key
 * column number as text, not a real column name.
 * --------------------------------
 */
void
CheckAttributeType(const char *attname,
				   Oid atttypid, Oid attcollation,
				   List *containing_rowtypes,
				   int flags)
{
	char		att_typtype = get_typtype(atttypid);
	Oid			att_typelem;

	if (att_typtype == TYPTYPE_PSEUDO)
	{
		/*
		 * We disallow pseudo-type columns, with the exception of ANYARRAY,
		 * RECORD, and RECORD[] when the caller says that those are OK.
		 *
		 * We don't need to worry about recursive containment for RECORD and
		 * RECORD[] because (a) no named composite type should be allowed to
		 * contain those, and (b) two "anonymous" record types couldn't be
		 * considered to be the same type, so infinite recursion isn't
		 * possible.
		 */
		if (!((atttypid == ANYARRAYOID && (flags & CHKATYPE_ANYARRAY)) ||
			  (atttypid == RECORDOID && (flags & CHKATYPE_ANYRECORD)) ||
			  (atttypid == RECORDARRAYOID && (flags & CHKATYPE_ANYRECORD))))
		{
			if (flags & CHKATYPE_IS_PARTKEY)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				/* translator: first %s is an integer not a name */
						 errmsg("partition key column %s has pseudo-type %s",
								attname, format_type_be(atttypid))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("column \"%s\" has pseudo-type %s",
								attname, format_type_be(atttypid))));
		}
	}
	else if (att_typtype == TYPTYPE_DOMAIN)
	{
		/*
		 * If it's a domain, recurse to check its base type.
		 */
		CheckAttributeType(attname, getBaseType(atttypid), attcollation,
						   containing_rowtypes,
						   flags);
	}
	else if (att_typtype == TYPTYPE_COMPOSITE)
	{
		/*
		 * For a composite type, recurse into its attributes.
		 */
		Relation	relation;
		TupleDesc	tupdesc;
		int			i;

		/*
		 * Check for self-containment.  Eventually we might be able to allow
		 * this (just return without complaint, if so) but it's not clear how
		 * many other places would require anti-recursion defenses before it
		 * would be safe to allow tables to contain their own rowtype.
		 */
		if (list_member_oid(containing_rowtypes, atttypid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("composite type %s cannot be made a member of itself",
							format_type_be(atttypid))));

		containing_rowtypes = lappend_oid(containing_rowtypes, atttypid);

		relation = relation_open(get_typ_typrelid(atttypid), AccessShareLock);

		tupdesc = RelationGetDescr(relation);

		for (i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (attr->attisdropped)
				continue;
			CheckAttributeType(NameStr(attr->attname),
							   attr->atttypid, attr->attcollation,
							   containing_rowtypes,
							   flags & ~CHKATYPE_IS_PARTKEY);
		}

		relation_close(relation, AccessShareLock);

		containing_rowtypes = list_delete_last(containing_rowtypes);
	}
	else if (att_typtype == TYPTYPE_RANGE)
	{
		/*
		 * If it's a range, recurse to check its subtype.
		 */
		CheckAttributeType(attname, get_range_subtype(atttypid),
						   get_range_collation(atttypid),
						   containing_rowtypes,
						   flags);
	}
	else if (OidIsValid((att_typelem = get_element_type(atttypid))))
	{
		/*
		 * Must recurse into array types, too, in case they are composite.
		 */
		CheckAttributeType(attname, att_typelem, attcollation,
						   containing_rowtypes,
						   flags);
	}

	/*
	 * This might not be strictly invalid per SQL standard, but it is pretty
	 * useless, and it cannot be dumped, so we must disallow it.
	 */
	if (!OidIsValid(attcollation) && type_is_collatable(atttypid))
	{
		if (flags & CHKATYPE_IS_PARTKEY)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
			/* translator: first %s is an integer not a name */
					 errmsg("no collation was derived for partition key column %s with collatable type %s",
							attname, format_type_be(atttypid)),
					 errhint("Use the COLLATE clause to set the collation explicitly.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("no collation was derived for column \"%s\" with collatable type %s",
							attname, format_type_be(atttypid)),
					 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}
}

/*
 * InsertPgAttributeTuple
 *		Construct and insert a new tuple in pg_attribute.
 *
 * Caller has already opened and locked pg_attribute.  new_attribute is the
 * attribute to insert.  attcacheoff is always initialized to -1, attacl and
 * attoptions are always initialized to NULL.
 *
 * indstate is the index state for CatalogTupleInsertWithInfo.  It can be
 * passed as NULL, in which case we'll fetch the necessary info.  (Don't do
 * this when inserting multiple attributes, because it's a tad more
 * expensive.)
 */
void
InsertPgAttributeTuple(Relation pg_attribute_rel,
					   Form_pg_attribute new_attribute,
					   Datum attoptions,
					   CatalogIndexState indstate)
{
	Datum		values[Natts_pg_attribute];
	bool		nulls[Natts_pg_attribute];
	HeapTuple	tup;

	/* This is a tad tedious, but way cleaner than what we used to do... */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_attribute_attrelid - 1] = ObjectIdGetDatum(new_attribute->attrelid);
	values[Anum_pg_attribute_attname - 1] = NameGetDatum(&new_attribute->attname);
	values[Anum_pg_attribute_atttypid - 1] = ObjectIdGetDatum(new_attribute->atttypid);
	values[Anum_pg_attribute_attstattarget - 1] = Int32GetDatum(new_attribute->attstattarget);
	values[Anum_pg_attribute_attlen - 1] = Int16GetDatum(new_attribute->attlen);
	values[Anum_pg_attribute_attnum - 1] = Int16GetDatum(new_attribute->attnum);
	values[Anum_pg_attribute_attndims - 1] = Int32GetDatum(new_attribute->attndims);
	values[Anum_pg_attribute_attcacheoff - 1] = Int32GetDatum(-1);
	values[Anum_pg_attribute_atttypmod - 1] = Int32GetDatum(new_attribute->atttypmod);
	values[Anum_pg_attribute_attbyval - 1] = BoolGetDatum(new_attribute->attbyval);
	values[Anum_pg_attribute_attstorage - 1] = CharGetDatum(new_attribute->attstorage);
	values[Anum_pg_attribute_attalign - 1] = CharGetDatum(new_attribute->attalign);
	values[Anum_pg_attribute_attnotnull - 1] = BoolGetDatum(new_attribute->attnotnull);
	values[Anum_pg_attribute_atthasdef - 1] = BoolGetDatum(new_attribute->atthasdef);
	values[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(new_attribute->atthasmissing);
	values[Anum_pg_attribute_attidentity - 1] = CharGetDatum(new_attribute->attidentity);
	values[Anum_pg_attribute_attgenerated - 1] = CharGetDatum(new_attribute->attgenerated);
	values[Anum_pg_attribute_attisdropped - 1] = BoolGetDatum(new_attribute->attisdropped);
	values[Anum_pg_attribute_attislocal - 1] = BoolGetDatum(new_attribute->attislocal);
	values[Anum_pg_attribute_attinhcount - 1] = Int32GetDatum(new_attribute->attinhcount);
	values[Anum_pg_attribute_attcollation - 1] = ObjectIdGetDatum(new_attribute->attcollation);
	values[Anum_pg_attribute_attoptions - 1] = attoptions;

	/* start out with empty permissions and empty options */
	nulls[Anum_pg_attribute_attacl - 1] = true;
	nulls[Anum_pg_attribute_attoptions - 1] = attoptions == (Datum) 0;
	nulls[Anum_pg_attribute_attfdwoptions - 1] = true;
	nulls[Anum_pg_attribute_attmissingval - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(pg_attribute_rel), values, nulls);

	/* finally insert the new tuple, update the indexes, and clean up */
	if (indstate != NULL)
		CatalogTupleInsertWithInfo(pg_attribute_rel, tup, indstate);
	else
		CatalogTupleInsert(pg_attribute_rel, tup);

	heap_freetuple(tup);
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
	Form_pg_attribute attr;
	int			i;
	Relation	rel;
	CatalogIndexState indstate;
	int			natts = tupdesc->natts;
	ObjectAddress myself,
				referenced;

	/*
	 * open pg_attribute and its indexes.
	 */
	rel = table_open(AttributeRelationId, RowExclusiveLock);

	indstate = CatalogOpenIndexes(rel);

	/*
	 * First we add the user attributes.  This is also a convenient place to
	 * add dependencies on their datatypes and collations.
	 */
	for (i = 0; i < natts; i++)
	{
		attr = TupleDescAttr(tupdesc, i);
		/* Fill in the correct relation OID */
		attr->attrelid = new_rel_oid;
		/* Make sure this is OK, too */
		attr->attstattarget = -1;

		InsertPgAttributeTuple(rel, attr, (Datum) 0, indstate);

		/* Add dependency info */
		myself.classId = RelationRelationId;
		myself.objectId = new_rel_oid;
		myself.objectSubId = i + 1;
		referenced.classId = TypeRelationId;
		referenced.objectId = attr->atttypid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

		/* The default collation is pinned, so don't bother recording it */
		if (OidIsValid(attr->attcollation) &&
			attr->attcollation != DEFAULT_COLLATION_OID)
		{
			referenced.classId = CollationRelationId;
			referenced.objectId = attr->attcollation;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	/*
	 * Next we add the system attributes.  Skip OID if rel has no OIDs. Skip
	 * all for a view or type relation.  We don't bother with making datatype
	 * dependencies here, since presumably all these types are pinned.
	 */
	if (relkind != RELKIND_VIEW && relkind != RELKIND_COMPOSITE_TYPE)
	{
		for (i = 0; i < (int) lengthof(SysAtt); i++)
		{
			FormData_pg_attribute attStruct;

			memcpy(&attStruct, SysAtt[i], sizeof(FormData_pg_attribute));

			/* Fill in the correct relation OID in the copied tuple */
			attStruct.attrelid = new_rel_oid;

			InsertPgAttributeTuple(rel, &attStruct, (Datum) 0, indstate);
		}
	}

	/*
	 * clean up
	 */
	CatalogCloseIndexes(indstate);

	table_close(rel, RowExclusiveLock);
}

/* --------------------------------
 *		InsertPgClassTuple
 *
 *		Construct and insert a new tuple in pg_class.
 *
 * Caller has already opened and locked pg_class.
 * Tuple data is taken from new_rel_desc->rd_rel, except for the
 * variable-width fields which are not present in a cached reldesc.
 * relacl and reloptions are passed in Datum form (to avoid having
 * to reference the data types in heap.h).  Pass (Datum) 0 to set them
 * to NULL.
 * --------------------------------
 */
void
InsertPgClassTuple(Relation pg_class_desc,
				   Relation new_rel_desc,
				   Oid new_rel_oid,
				   Datum relacl,
				   Datum reloptions)
{
	Form_pg_class rd_rel = new_rel_desc->rd_rel;
	Datum		values[Natts_pg_class];
	bool		nulls[Natts_pg_class];
	HeapTuple	tup;

	/* This is a tad tedious, but way cleaner than what we used to do... */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_class_oid - 1] = ObjectIdGetDatum(new_rel_oid);
	values[Anum_pg_class_relname - 1] = NameGetDatum(&rd_rel->relname);
	values[Anum_pg_class_relnamespace - 1] = ObjectIdGetDatum(rd_rel->relnamespace);
	values[Anum_pg_class_reltype - 1] = ObjectIdGetDatum(rd_rel->reltype);
	values[Anum_pg_class_reloftype - 1] = ObjectIdGetDatum(rd_rel->reloftype);
	values[Anum_pg_class_relowner - 1] = ObjectIdGetDatum(rd_rel->relowner);
	values[Anum_pg_class_relam - 1] = ObjectIdGetDatum(rd_rel->relam);
	values[Anum_pg_class_relfilenode - 1] = ObjectIdGetDatum(rd_rel->relfilenode);
	values[Anum_pg_class_reltablespace - 1] = ObjectIdGetDatum(rd_rel->reltablespace);
	values[Anum_pg_class_relpages - 1] = Int32GetDatum(rd_rel->relpages);
	values[Anum_pg_class_reltuples - 1] = Float4GetDatum(rd_rel->reltuples);
	values[Anum_pg_class_relallvisible - 1] = Int32GetDatum(rd_rel->relallvisible);
	values[Anum_pg_class_reltoastrelid - 1] = ObjectIdGetDatum(rd_rel->reltoastrelid);
	values[Anum_pg_class_relhasindex - 1] = BoolGetDatum(rd_rel->relhasindex);
	values[Anum_pg_class_relisshared - 1] = BoolGetDatum(rd_rel->relisshared);
	values[Anum_pg_class_relpersistence - 1] = CharGetDatum(rd_rel->relpersistence);
	values[Anum_pg_class_relkind - 1] = CharGetDatum(rd_rel->relkind);
	values[Anum_pg_class_relnatts - 1] = Int16GetDatum(rd_rel->relnatts);
	values[Anum_pg_class_relchecks - 1] = Int16GetDatum(rd_rel->relchecks);
	values[Anum_pg_class_relhasrules - 1] = BoolGetDatum(rd_rel->relhasrules);
	values[Anum_pg_class_relhastriggers - 1] = BoolGetDatum(rd_rel->relhastriggers);
	values[Anum_pg_class_relrowsecurity - 1] = BoolGetDatum(rd_rel->relrowsecurity);
	values[Anum_pg_class_relforcerowsecurity - 1] = BoolGetDatum(rd_rel->relforcerowsecurity);
	values[Anum_pg_class_relhassubclass - 1] = BoolGetDatum(rd_rel->relhassubclass);
	values[Anum_pg_class_relispopulated - 1] = BoolGetDatum(rd_rel->relispopulated);
	values[Anum_pg_class_relreplident - 1] = CharGetDatum(rd_rel->relreplident);
	values[Anum_pg_class_relispartition - 1] = BoolGetDatum(rd_rel->relispartition);
	values[Anum_pg_class_relrewrite - 1] = ObjectIdGetDatum(rd_rel->relrewrite);
	values[Anum_pg_class_relfrozenxid - 1] = TransactionIdGetDatum(rd_rel->relfrozenxid);
	values[Anum_pg_class_relminmxid - 1] = MultiXactIdGetDatum(rd_rel->relminmxid);
	if (relacl != (Datum) 0)
		values[Anum_pg_class_relacl - 1] = relacl;
	else
		nulls[Anum_pg_class_relacl - 1] = true;
	if (reloptions != (Datum) 0)
		values[Anum_pg_class_reloptions - 1] = reloptions;
	else
		nulls[Anum_pg_class_reloptions - 1] = true;

	/* relpartbound is set by updating this tuple, if necessary */
	nulls[Anum_pg_class_relpartbound - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(pg_class_desc), values, nulls);

	/* finally insert the new tuple, update the indexes, and clean up */
	CatalogTupleInsert(pg_class_desc, tup);

	heap_freetuple(tup);
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
					Oid reloftype,
					Oid relowner,
					char relkind,
					TransactionId relfrozenxid,
					TransactionId relminmxid,
					Datum relacl,
					Datum reloptions)
{
	Form_pg_class new_rel_reltup;

	/*
	 * first we update some of the information in our uncataloged relation's
	 * relation descriptor.
	 */
	new_rel_reltup = new_rel_desc->rd_rel;

	switch (relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
		case RELKIND_INDEX:
		case RELKIND_TOASTVALUE:
			/* The relation is real, but as yet empty */
			new_rel_reltup->relpages = 0;
			new_rel_reltup->reltuples = 0;
			new_rel_reltup->relallvisible = 0;
			break;
		case RELKIND_SEQUENCE:
			/* Sequences always have a known size */
			new_rel_reltup->relpages = 1;
			new_rel_reltup->reltuples = 1;
			new_rel_reltup->relallvisible = 0;
			break;
		default:
			/* Views, etc, have no disk storage */
			new_rel_reltup->relpages = 0;
			new_rel_reltup->reltuples = 0;
			new_rel_reltup->relallvisible = 0;
			break;
	}

	new_rel_reltup->relfrozenxid = relfrozenxid;
	new_rel_reltup->relminmxid = relminmxid;
	new_rel_reltup->relowner = relowner;
	new_rel_reltup->reltype = new_type_oid;
	new_rel_reltup->reloftype = reloftype;

	/* relispartition is always set by updating this tuple later */
	new_rel_reltup->relispartition = false;

	new_rel_desc->rd_att->tdtypeid = new_type_oid;

	/* Now build and insert the tuple */
	InsertPgClassTuple(pg_class_desc, new_rel_desc, new_rel_oid,
					   relacl, reloptions);
}


/* --------------------------------
 *		AddNewRelationType -
 *
 *		define a composite type corresponding to the new relation
 * --------------------------------
 */
static ObjectAddress
AddNewRelationType(const char *typeName,
				   Oid typeNamespace,
				   Oid new_rel_oid,
				   char new_rel_kind,
				   Oid ownerid,
				   Oid new_row_type,
				   Oid new_array_type)
{
	return
		TypeCreate(new_row_type,	/* optional predetermined OID */
				   typeName,	/* type name */
				   typeNamespace,	/* type namespace */
				   new_rel_oid, /* relation oid */
				   new_rel_kind,	/* relation kind */
				   ownerid,		/* owner's ID */
				   -1,			/* internal size (varlena) */
				   TYPTYPE_COMPOSITE,	/* type-type (composite) */
				   TYPCATEGORY_COMPOSITE,	/* type-category (ditto) */
				   false,		/* composite types are never preferred */
				   DEFAULT_TYPDELIM,	/* default array delimiter */
				   F_RECORD_IN, /* input procedure */
				   F_RECORD_OUT,	/* output procedure */
				   F_RECORD_RECV,	/* receive procedure */
				   F_RECORD_SEND,	/* send procedure */
				   InvalidOid,	/* typmodin procedure - none */
				   InvalidOid,	/* typmodout procedure - none */
				   InvalidOid,	/* analyze procedure - default */
				   InvalidOid,	/* array element type - irrelevant */
				   false,		/* this is not an array type */
				   new_array_type,	/* array type if any */
				   InvalidOid,	/* domain base type - irrelevant */
				   NULL,		/* default value - none */
				   NULL,		/* default binary representation */
				   false,		/* passed by reference */
				   TYPALIGN_DOUBLE, /* alignment - must be the largest! */
				   TYPSTORAGE_EXTENDED, /* fully TOASTable */
				   -1,			/* typmod */
				   0,			/* array dimensions for typBaseType */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* rowtypes never have a collation */
}

/* --------------------------------
 *		heap_create_with_catalog
 *
 *		creates a new cataloged relation.  see comments above.
 *
 * Arguments:
 *	relname: name to give to new rel
 *	relnamespace: OID of namespace it goes in
 *	reltablespace: OID of tablespace it goes in
 *	relid: OID to assign to new rel, or InvalidOid to select a new OID
 *	reltypeid: OID to assign to rel's rowtype, or InvalidOid to select one
 *	reloftypeid: if a typed table, OID of underlying type; else InvalidOid
 *	ownerid: OID of new rel's owner
 *	tupdesc: tuple descriptor (source of column definitions)
 *	cooked_constraints: list of precooked check constraints and defaults
 *	relkind: relkind for new rel
 *	relpersistence: rel's persistence status (permanent, temp, or unlogged)
 *	shared_relation: true if it's to be a shared relation
 *	mapped_relation: true if the relation will use the relfilenode map
 *	oncommit: ON COMMIT marking (only relevant if it's a temp table)
 *	reloptions: reloptions in Datum form, or (Datum) 0 if none
 *	use_user_acl: true if should look for user-defined default permissions;
 *		if false, relacl is always set NULL
 *	allow_system_table_mods: true to allow creation in system namespaces
 *	is_internal: is this a system-generated catalog?
 *
 * Output parameters:
 *	typaddress: if not null, gets the object address of the new pg_type entry
 *
 * Returns the OID of the new relation
 * --------------------------------
 */
Oid
heap_create_with_catalog(const char *relname,
						 Oid relnamespace,
						 Oid reltablespace,
						 Oid relid,
						 Oid reltypeid,
						 Oid reloftypeid,
						 Oid ownerid,
						 Oid accessmtd,
						 TupleDesc tupdesc,
						 List *cooked_constraints,
						 char relkind,
						 char relpersistence,
						 bool shared_relation,
						 bool mapped_relation,
						 OnCommitAction oncommit,
						 Datum reloptions,
						 bool use_user_acl,
						 bool allow_system_table_mods,
						 bool is_internal,
						 Oid relrewrite,
						 ObjectAddress *typaddress)
{
	Relation	pg_class_desc;
	Relation	new_rel_desc;
	Acl		   *relacl;
	Oid			existing_relid;
	Oid			old_type_oid;
	Oid			new_type_oid;
	ObjectAddress new_type_addr;
	Oid			new_array_oid = InvalidOid;
	TransactionId relfrozenxid;
	MultiXactId relminmxid;

	pg_class_desc = table_open(RelationRelationId, RowExclusiveLock);

	/*
	 * sanity checks
	 */
	Assert(IsNormalProcessingMode() || IsBootstrapProcessingMode());

	/*
	 * Validate proposed tupdesc for the desired relkind.  If
	 * allow_system_table_mods is on, allow ANYARRAY to be used; this is a
	 * hack to allow creating pg_statistic and cloning it during VACUUM FULL.
	 */
	CheckAttributeNamesTypes(tupdesc, relkind,
							 allow_system_table_mods ? CHKATYPE_ANYARRAY : 0);

	/*
	 * This would fail later on anyway, if the relation already exists.  But
	 * by catching it here we can emit a nicer error message.
	 */
	existing_relid = get_relname_relid(relname, relnamespace);
	if (existing_relid != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists", relname)));

	/*
	 * Since we are going to create a rowtype as well, also check for
	 * collision with an existing type name.  If there is one and it's an
	 * autogenerated array, we can rename it out of the way; otherwise we can
	 * at least give a good error message.
	 */
	old_type_oid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								   CStringGetDatum(relname),
								   ObjectIdGetDatum(relnamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, relname, relnamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", relname),
					 errhint("A relation has an associated type of the same name, "
							 "so you must use a name that doesn't conflict "
							 "with any existing type.")));
	}

	/*
	 * Shared relations must be in pg_global (last-ditch check)
	 */
	if (shared_relation && reltablespace != GLOBALTABLESPACE_OID)
		elog(ERROR, "shared relations must be placed in pg_global tablespace");

	/*
	 * Allocate an OID for the relation, unless we were told what to use.
	 *
	 * The OID will be the relfilenode as well, so make sure it doesn't
	 * collide with either pg_class OIDs or existing physical files.
	 */
	if (!OidIsValid(relid))
	{
		/* Use binary-upgrade override for pg_class.oid/relfilenode? */
		if (IsBinaryUpgrade &&
			(relkind == RELKIND_RELATION || relkind == RELKIND_SEQUENCE ||
			 relkind == RELKIND_VIEW || relkind == RELKIND_MATVIEW ||
			 relkind == RELKIND_COMPOSITE_TYPE || relkind == RELKIND_FOREIGN_TABLE ||
			 relkind == RELKIND_PARTITIONED_TABLE))
		{
			if (!OidIsValid(binary_upgrade_next_heap_pg_class_oid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("pg_class heap OID value not set when in binary upgrade mode")));

			relid = binary_upgrade_next_heap_pg_class_oid;
			binary_upgrade_next_heap_pg_class_oid = InvalidOid;
		}
		/* There might be no TOAST table, so we have to test for it. */
		else if (IsBinaryUpgrade &&
				 OidIsValid(binary_upgrade_next_toast_pg_class_oid) &&
				 relkind == RELKIND_TOASTVALUE)
		{
			relid = binary_upgrade_next_toast_pg_class_oid;
			binary_upgrade_next_toast_pg_class_oid = InvalidOid;
		}
		else
			relid = GetNewRelFileNode(reltablespace, pg_class_desc,
									  relpersistence);
	}

	/*
	 * Determine the relation's initial permissions.
	 */
	if (use_user_acl)
	{
		switch (relkind)
		{
			case RELKIND_RELATION:
			case RELKIND_VIEW:
			case RELKIND_MATVIEW:
			case RELKIND_FOREIGN_TABLE:
			case RELKIND_PARTITIONED_TABLE:
				relacl = get_user_default_acl(OBJECT_TABLE, ownerid,
											  relnamespace);
				break;
			case RELKIND_SEQUENCE:
				relacl = get_user_default_acl(OBJECT_SEQUENCE, ownerid,
											  relnamespace);
				break;
			default:
				relacl = NULL;
				break;
		}
	}
	else
		relacl = NULL;

	/*
	 * Create the relcache entry (mostly dummy at this point) and the physical
	 * disk file.  (If we fail further down, it's the smgr's responsibility to
	 * remove the disk file again.)
	 */
	new_rel_desc = heap_create(relname,
							   relnamespace,
							   reltablespace,
							   relid,
							   InvalidOid,
							   accessmtd,
							   tupdesc,
							   relkind,
							   relpersistence,
							   shared_relation,
							   mapped_relation,
							   allow_system_table_mods,
							   &relfrozenxid,
							   &relminmxid);

	Assert(relid == RelationGetRelid(new_rel_desc));

	new_rel_desc->rd_rel->relrewrite = relrewrite;

	/*
	 * Decide whether to create an array type over the relation's rowtype. We
	 * do not create any array types for system catalogs (ie, those made
	 * during initdb). We do not create them where the use of a relation as
	 * such is an implementation detail: toast tables, sequences and indexes.
	 */
	if (IsUnderPostmaster && (relkind == RELKIND_RELATION ||
							  relkind == RELKIND_VIEW ||
							  relkind == RELKIND_MATVIEW ||
							  relkind == RELKIND_FOREIGN_TABLE ||
							  relkind == RELKIND_COMPOSITE_TYPE ||
							  relkind == RELKIND_PARTITIONED_TABLE))
		new_array_oid = AssignTypeArrayOid();

	/*
	 * Since defining a relation also defines a complex type, we add a new
	 * system type corresponding to the new relation.  The OID of the type can
	 * be preselected by the caller, but if reltypeid is InvalidOid, we'll
	 * generate a new OID for it.
	 *
	 * NOTE: we could get a unique-index failure here, in case someone else is
	 * creating the same type name in parallel but hadn't committed yet when
	 * we checked for a duplicate name above.
	 */
	new_type_addr = AddNewRelationType(relname,
									   relnamespace,
									   relid,
									   relkind,
									   ownerid,
									   reltypeid,
									   new_array_oid);
	new_type_oid = new_type_addr.objectId;
	if (typaddress)
		*typaddress = new_type_addr;

	/*
	 * Now make the array type if wanted.
	 */
	if (OidIsValid(new_array_oid))
	{
		char	   *relarrayname;

		relarrayname = makeArrayTypeName(relname, relnamespace);

		TypeCreate(new_array_oid,	/* force the type's OID to this */
				   relarrayname,	/* Array type name */
				   relnamespace,	/* Same namespace as parent */
				   InvalidOid,	/* Not composite, no relationOid */
				   0,			/* relkind, also N/A here */
				   ownerid,		/* owner's ID */
				   -1,			/* Internal size (varlena) */
				   TYPTYPE_BASE,	/* Not composite - typelem is */
				   TYPCATEGORY_ARRAY,	/* type-category (array) */
				   false,		/* array types are never preferred */
				   DEFAULT_TYPDELIM,	/* default array delimiter */
				   F_ARRAY_IN,	/* array input proc */
				   F_ARRAY_OUT, /* array output proc */
				   F_ARRAY_RECV,	/* array recv (bin) proc */
				   F_ARRAY_SEND,	/* array send (bin) proc */
				   InvalidOid,	/* typmodin procedure - none */
				   InvalidOid,	/* typmodout procedure - none */
				   F_ARRAY_TYPANALYZE,	/* array analyze procedure */
				   new_type_oid,	/* array element type - the rowtype */
				   true,		/* yes, this is an array type */
				   InvalidOid,	/* this has no array type */
				   InvalidOid,	/* domain base type - irrelevant */
				   NULL,		/* default value - none */
				   NULL,		/* default binary representation */
				   false,		/* passed by reference */
				   TYPALIGN_DOUBLE, /* alignment - must be the largest! */
				   TYPSTORAGE_EXTENDED, /* fully TOASTable */
				   -1,			/* typmod */
				   0,			/* array dimensions for typBaseType */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* rowtypes never have a collation */

		pfree(relarrayname);
	}

	/*
	 * now create an entry in pg_class for the relation.
	 *
	 * NOTE: we could get a unique-index failure here, in case someone else is
	 * creating the same relation name in parallel but hadn't committed yet
	 * when we checked for a duplicate name above.
	 */
	AddNewRelationTuple(pg_class_desc,
						new_rel_desc,
						relid,
						new_type_oid,
						reloftypeid,
						ownerid,
						relkind,
						relfrozenxid,
						relminmxid,
						PointerGetDatum(relacl),
						reloptions);

	/*
	 * now add tuples to pg_attribute for the attributes in our new relation.
	 */
	AddNewAttributeTuples(relid, new_rel_desc->rd_att, relkind);

	/*
	 * Make a dependency link to force the relation to be deleted if its
	 * namespace is.  Also make a dependency link to its owner, as well as
	 * dependencies for any roles mentioned in the default ACL.
	 *
	 * For composite types, these dependencies are tracked for the pg_type
	 * entry, so we needn't record them here.  Likewise, TOAST tables don't
	 * need a namespace dependency (they live in a pinned namespace) nor an
	 * owner dependency (they depend indirectly through the parent table), nor
	 * should they have any ACL entries.  The same applies for extension
	 * dependencies.
	 *
	 * Also, skip this in bootstrap mode, since we don't make dependencies
	 * while bootstrapping.
	 */
	if (relkind != RELKIND_COMPOSITE_TYPE &&
		relkind != RELKIND_TOASTVALUE &&
		!IsBootstrapProcessingMode())
	{
		ObjectAddress myself,
					referenced;

		myself.classId = RelationRelationId;
		myself.objectId = relid;
		myself.objectSubId = 0;

		referenced.classId = NamespaceRelationId;
		referenced.objectId = relnamespace;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

		recordDependencyOnOwner(RelationRelationId, relid, ownerid);

		recordDependencyOnNewAcl(RelationRelationId, relid, 0, ownerid, relacl);

		recordDependencyOnCurrentExtension(&myself, false);

		if (reloftypeid)
		{
			referenced.classId = TypeRelationId;
			referenced.objectId = reloftypeid;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}

		/*
		 * Make a dependency link to force the relation to be deleted if its
		 * access method is. Do this only for relation and materialized views.
		 *
		 * No need to add an explicit dependency for the toast table, as the
		 * main table depends on it.
		 */
		if (relkind == RELKIND_RELATION ||
			relkind == RELKIND_MATVIEW)
		{
			referenced.classId = AccessMethodRelationId;
			referenced.objectId = accessmtd;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	/* Post creation hook for new relation */
	InvokeObjectPostCreateHookArg(RelationRelationId, relid, 0, is_internal);

	/*
	 * Store any supplied constraints and defaults.
	 *
	 * NB: this may do a CommandCounterIncrement and rebuild the relcache
	 * entry, so the relation must be valid and self-consistent at this point.
	 * In particular, there are not yet constraints and defaults anywhere.
	 */
	StoreConstraints(new_rel_desc, cooked_constraints, is_internal);

	/*
	 * If there's a special on-commit action, remember it
	 */
	if (oncommit != ONCOMMIT_NOOP)
		register_on_commit_action(relid, oncommit);

	/*
	 * ok, the relation has been cataloged, so close our relations and return
	 * the OID of the newly created relation.
	 */
	table_close(new_rel_desc, NoLock);	/* do not unlock till end of xact */
	table_close(pg_class_desc, RowExclusiveLock);

	return relid;
}

/*
 *		RelationRemoveInheritance
 *
 * Formerly, this routine checked for child relations and aborted the
 * deletion if any were found.  Now we rely on the dependency mechanism
 * to check for or delete child relations.  By the time we get here,
 * there are no children and we need only remove any pg_inherits rows
 * linking this relation to its parent(s).
 */
static void
RelationRemoveInheritance(Oid relid)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;

	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);

	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId, true,
							  NULL, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(catalogRelation, &tuple->t_self);

	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);
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
	pg_class_desc = table_open(RelationRelationId, RowExclusiveLock);

	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	/* delete the relation tuple from pg_class, and finish up */
	CatalogTupleDelete(pg_class_desc, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(pg_class_desc, RowExclusiveLock);
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
	attrel = table_open(AttributeRelationId, RowExclusiveLock);

	/* Use the index to scan only attributes of the target relation */
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(attrel, AttributeRelidNumIndexId, true,
							  NULL, 1, key);

	/* Delete all the matching tuples */
	while ((atttup = systable_getnext(scan)) != NULL)
		CatalogTupleDelete(attrel, &atttup->t_self);

	/* Clean up after the scan */
	systable_endscan(scan);
	table_close(attrel, RowExclusiveLock);
}

/*
 *		DeleteSystemAttributeTuples
 *
 * Remove pg_attribute rows for system columns of the given relid.
 *
 * Note: this is only used when converting a table to a view.  Views don't
 * have system columns, so we should remove them from pg_attribute.
 */
void
DeleteSystemAttributeTuples(Oid relid)
{
	Relation	attrel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	atttup;

	/* Grab an appropriate lock on the pg_attribute relation */
	attrel = table_open(AttributeRelationId, RowExclusiveLock);

	/* Use the index to scan only system attributes of the target relation */
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_attribute_attnum,
				BTLessEqualStrategyNumber, F_INT2LE,
				Int16GetDatum(0));

	scan = systable_beginscan(attrel, AttributeRelidNumIndexId, true,
							  NULL, 2, key);

	/* Delete all the matching tuples */
	while ((atttup = systable_getnext(scan)) != NULL)
		CatalogTupleDelete(attrel, &atttup->t_self);

	/* Clean up after the scan */
	systable_endscan(scan);
	table_close(attrel, RowExclusiveLock);
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
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.  (In the simple case where we are directly
	 * dropping this column, ATExecDropColumn already did this ... but when
	 * cascading from a drop of some other object, we may not have any lock.)
	 */
	rel = relation_open(relid, AccessExclusiveLock);

	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy2(ATTNUM,
								ObjectIdGetDatum(relid),
								Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tuple))	/* shouldn't happen */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	attStruct = (Form_pg_attribute) GETSTRUCT(tuple);

	if (attnum < 0)
	{
		/* System attribute (probably OID) ... just delete the row */

		CatalogTupleDelete(attr_rel, &tuple->t_self);
	}
	else
	{
		/* Dropping user attributes is lots harder */

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

		/* Unset this so no one tries to look up the generation expression */
		attStruct->attgenerated = '\0';

		/*
		 * Change the column name to something that isn't likely to conflict
		 */
		snprintf(newattname, sizeof(newattname),
				 "........pg.dropped.%d........", attnum);
		namestrcpy(&(attStruct->attname), newattname);

		/* clear the missing value if any */
		if (attStruct->atthasmissing)
		{
			Datum		valuesAtt[Natts_pg_attribute];
			bool		nullsAtt[Natts_pg_attribute];
			bool		replacesAtt[Natts_pg_attribute];

			/* update the tuple - set atthasmissing and attmissingval */
			MemSet(valuesAtt, 0, sizeof(valuesAtt));
			MemSet(nullsAtt, false, sizeof(nullsAtt));
			MemSet(replacesAtt, false, sizeof(replacesAtt));

			valuesAtt[Anum_pg_attribute_atthasmissing - 1] =
				BoolGetDatum(false);
			replacesAtt[Anum_pg_attribute_atthasmissing - 1] = true;
			valuesAtt[Anum_pg_attribute_attmissingval - 1] = (Datum) 0;
			nullsAtt[Anum_pg_attribute_attmissingval - 1] = true;
			replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;

			tuple = heap_modify_tuple(tuple, RelationGetDescr(attr_rel),
									  valuesAtt, nullsAtt, replacesAtt);
		}

		CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);
	}

	/*
	 * Because updating the pg_attribute row will trigger a relcache flush for
	 * the target relation, we need not do anything else to notify other
	 * backends of the change.
	 */

	table_close(attr_rel, RowExclusiveLock);

	if (attnum > 0)
		RemoveStatistics(relid, attnum);

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
				  DropBehavior behavior, bool complain, bool internal)
{
	Relation	attrdef_rel;
	ScanKeyData scankeys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;

	attrdef_rel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	ScanKeyInit(&scankeys[0],
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&scankeys[1],
				Anum_pg_attrdef_adnum,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));

	scan = systable_beginscan(attrdef_rel, AttrDefaultIndexId, true,
							  NULL, 2, scankeys);

	/* There should be at most one matching tuple, but we loop anyway */
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ObjectAddress object;
		Form_pg_attrdef attrtuple = (Form_pg_attrdef) GETSTRUCT(tuple);

		object.classId = AttrDefaultRelationId;
		object.objectId = attrtuple->oid;
		object.objectSubId = 0;

		performDeletion(&object, behavior,
						internal ? PERFORM_DELETION_INTERNAL : 0);

		found = true;
	}

	systable_endscan(scan);
	table_close(attrdef_rel, RowExclusiveLock);

	if (complain && !found)
		elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
			 relid, attnum);
}

/*
 *		RemoveAttrDefaultById
 *
 * Remove a pg_attrdef entry specified by OID.  This is the guts of
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
	attrdef_rel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	/* Find the pg_attrdef tuple */
	ScanKeyInit(&scankeys[0],
				Anum_pg_attrdef_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attrdefId));

	scan = systable_beginscan(attrdef_rel, AttrDefaultOidIndexId, true,
							  NULL, 1, scankeys);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for attrdef %u", attrdefId);

	myrelid = ((Form_pg_attrdef) GETSTRUCT(tuple))->adrelid;
	myattnum = ((Form_pg_attrdef) GETSTRUCT(tuple))->adnum;

	/* Get an exclusive lock on the relation owning the attribute */
	myrel = relation_open(myrelid, AccessExclusiveLock);

	/* Now we can delete the pg_attrdef row */
	CatalogTupleDelete(attrdef_rel, &tuple->t_self);

	systable_endscan(scan);
	table_close(attrdef_rel, RowExclusiveLock);

	/* Fix the pg_attribute row */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy2(ATTNUM,
								ObjectIdGetDatum(myrelid),
								Int16GetDatum(myattnum));
	if (!HeapTupleIsValid(tuple))	/* shouldn't happen */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 myattnum, myrelid);

	((Form_pg_attribute) GETSTRUCT(tuple))->atthasdef = false;

	CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

	/*
	 * Our update of the pg_attribute row will force a relcache rebuild, so
	 * there's nothing else to do here.
	 */
	table_close(attr_rel, RowExclusiveLock);

	/* Keep lock on attribute's rel until end of xact */
	relation_close(myrel, NoLock);
}

/*
 * heap_drop_with_catalog	- removes specified relation from catalogs
 *
 * Note that this routine is not responsible for dropping objects that are
 * linked to the pg_class entry via dependencies (for example, indexes and
 * constraints).  Those are deleted by the dependency-tracing logic in
 * dependency.c before control gets here.  In general, therefore, this routine
 * should never be called directly; go through performDeletion() instead.
 */
void
heap_drop_with_catalog(Oid relid)
{
	Relation	rel;
	HeapTuple	tuple;
	Oid			parentOid = InvalidOid,
				defaultPartOid = InvalidOid;

	/*
	 * To drop a partition safely, we must grab exclusive lock on its parent,
	 * because another backend might be about to execute a query on the parent
	 * table.  If it relies on previously cached partition descriptor, then it
	 * could attempt to access the just-dropped relation as its partition. We
	 * must therefore take a table lock strong enough to prevent all queries
	 * on the table from proceeding until we commit and send out a
	 * shared-cache-inval notice that will make them update their partition
	 * descriptors.
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	if (((Form_pg_class) GETSTRUCT(tuple))->relispartition)
	{
		parentOid = get_partition_parent(relid);
		LockRelationOid(parentOid, AccessExclusiveLock);

		/*
		 * If this is not the default partition, dropping it will change the
		 * default partition's partition constraint, so we must lock it.
		 */
		defaultPartOid = get_default_partition_oid(parentOid);
		if (OidIsValid(defaultPartOid) && relid != defaultPartOid)
			LockRelationOid(defaultPartOid, AccessExclusiveLock);
	}

	ReleaseSysCache(tuple);

	/*
	 * Open and lock the relation.
	 */
	rel = relation_open(relid, AccessExclusiveLock);

	/*
	 * There can no longer be anyone *else* touching the relation, but we
	 * might still have open queries or cursors, or pending trigger events, in
	 * our own session.
	 */
	CheckTableNotInUse(rel, "DROP TABLE");

	/*
	 * This effectively deletes all rows in the table, and may be done in a
	 * serializable transaction.  In that case we must record a rw-conflict in
	 * to this transaction from each transaction holding a predicate lock on
	 * the table.
	 */
	CheckTableForSerializableConflictIn(rel);

	/*
	 * Delete pg_foreign_table tuple first.
	 */
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		Relation	rel;
		HeapTuple	tuple;

		rel = table_open(ForeignTableRelationId, RowExclusiveLock);

		tuple = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for foreign table %u", relid);

		CatalogTupleDelete(rel, &tuple->t_self);

		ReleaseSysCache(tuple);
		table_close(rel, RowExclusiveLock);
	}

	/*
	 * If a partitioned table, delete the pg_partitioned_table tuple.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		RemovePartitionKeyByRelId(relid);

	/*
	 * If the relation being dropped is the default partition itself,
	 * invalidate its entry in pg_partitioned_table.
	 */
	if (relid == defaultPartOid)
		update_default_partition_oid(parentOid, InvalidOid);

	/*
	 * Schedule unlinking of the relation's physical files at commit.
	 */
	if (rel->rd_rel->relkind != RELKIND_VIEW &&
		rel->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
		rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		RelationDropStorage(rel);
	}

	/*
	 * Close relcache entry, but *keep* AccessExclusiveLock on the relation
	 * until transaction commit.  This ensures no one else will try to do
	 * something with the doomed relation.
	 */
	relation_close(rel, NoLock);

	/*
	 * Remove any associated relation synchronization states.
	 */
	RemoveSubscriptionRel(InvalidOid, relid);

	/*
	 * Forget any ON COMMIT action for the rel
	 */
	remove_on_commit_action(relid);

	/*
	 * Flush the relation from the relcache.  We want to do this before
	 * starting to remove catalog entries, just to be certain that no relcache
	 * entry rebuild will happen partway through.  (That should not really
	 * matter, since we don't do CommandCounterIncrement here, but let's be
	 * safe.)
	 */
	RelationForgetRelation(relid);

	/*
	 * remove inheritance information
	 */
	RelationRemoveInheritance(relid);

	/*
	 * delete statistics
	 */
	RemoveStatistics(relid, 0);

	/*
	 * delete attribute tuples
	 */
	DeleteAttributeTuples(relid);

	/*
	 * delete relation tuple
	 */
	DeleteRelationTuple(relid);

	if (OidIsValid(parentOid))
	{
		/*
		 * If this is not the default partition, the partition constraint of
		 * the default partition has changed to include the portion of the key
		 * space previously covered by the dropped partition.
		 */
		if (OidIsValid(defaultPartOid) && relid != defaultPartOid)
			CacheInvalidateRelcacheByRelid(defaultPartOid);

		/*
		 * Invalidate the parent's relcache so that the partition is no longer
		 * included in its partition descriptor.
		 */
		CacheInvalidateRelcacheByRelid(parentOid);
		/* keep the lock */
	}
}


/*
 * RelationClearMissing
 *
 * Set atthasmissing and attmissingval to false/null for all attributes
 * where they are currently set. This can be safely and usefully done if
 * the table is rewritten (e.g. by VACUUM FULL or CLUSTER) where we know there
 * are no rows left with less than a full complement of attributes.
 *
 * The caller must have an AccessExclusive lock on the relation.
 */
void
RelationClearMissing(Relation rel)
{
	Relation	attr_rel;
	Oid			relid = RelationGetRelid(rel);
	int			natts = RelationGetNumberOfAttributes(rel);
	int			attnum;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];
	Form_pg_attribute attrtuple;
	HeapTuple	tuple,
				newtuple;

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	repl_val[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(false);
	repl_null[Anum_pg_attribute_attmissingval - 1] = true;

	repl_repl[Anum_pg_attribute_atthasmissing - 1] = true;
	repl_repl[Anum_pg_attribute_attmissingval - 1] = true;


	/* Get a lock on pg_attribute */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	/* process each non-system attribute, including any dropped columns */
	for (attnum = 1; attnum <= natts; attnum++)
	{
		tuple = SearchSysCache2(ATTNUM,
								ObjectIdGetDatum(relid),
								Int16GetDatum(attnum));
		if (!HeapTupleIsValid(tuple))	/* shouldn't happen */
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 attnum, relid);

		attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

		/* ignore any where atthasmissing is not true */
		if (attrtuple->atthasmissing)
		{
			newtuple = heap_modify_tuple(tuple, RelationGetDescr(attr_rel),
										 repl_val, repl_null, repl_repl);

			CatalogTupleUpdate(attr_rel, &newtuple->t_self, newtuple);

			heap_freetuple(newtuple);
		}

		ReleaseSysCache(tuple);
	}

	/*
	 * Our update of the pg_attribute rows will force a relcache rebuild, so
	 * there's nothing else to do here.
	 */
	table_close(attr_rel, RowExclusiveLock);
}

/*
 * SetAttrMissing
 *
 * Set the missing value of a single attribute. This should only be used by
 * binary upgrade. Takes an AccessExclusive lock on the relation owning the
 * attribute.
 */
void
SetAttrMissing(Oid relid, char *attname, char *value)
{
	Datum		valuesAtt[Natts_pg_attribute];
	bool		nullsAtt[Natts_pg_attribute];
	bool		replacesAtt[Natts_pg_attribute];
	Datum		missingval;
	Form_pg_attribute attStruct;
	Relation	attrrel,
				tablerel;
	HeapTuple	atttup,
				newtup;

	/* lock the table the attribute belongs to */
	tablerel = table_open(relid, AccessExclusiveLock);

	/* Don't do anything unless it's a plain table */
	if (tablerel->rd_rel->relkind != RELKIND_RELATION)
	{
		table_close(tablerel, AccessExclusiveLock);
		return;
	}

	/* Lock the attribute row and get the data */
	attrrel = table_open(AttributeRelationId, RowExclusiveLock);
	atttup = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "cache lookup failed for attribute %s of relation %u",
			 attname, relid);
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);

	/* get an array value from the value string */
	missingval = OidFunctionCall3(F_ARRAY_IN,
								  CStringGetDatum(value),
								  ObjectIdGetDatum(attStruct->atttypid),
								  Int32GetDatum(attStruct->atttypmod));

	/* update the tuple - set atthasmissing and attmissingval */
	MemSet(valuesAtt, 0, sizeof(valuesAtt));
	MemSet(nullsAtt, false, sizeof(nullsAtt));
	MemSet(replacesAtt, false, sizeof(replacesAtt));

	valuesAtt[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(true);
	replacesAtt[Anum_pg_attribute_atthasmissing - 1] = true;
	valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
	replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;

	newtup = heap_modify_tuple(atttup, RelationGetDescr(attrrel),
							   valuesAtt, nullsAtt, replacesAtt);
	CatalogTupleUpdate(attrrel, &newtup->t_self, newtup);

	/* clean up */
	ReleaseSysCache(atttup);
	table_close(attrrel, RowExclusiveLock);
	table_close(tablerel, AccessExclusiveLock);
}

/*
 * Store a default expression for column attnum of relation rel.
 *
 * Returns the OID of the new pg_attrdef tuple.
 *
 * add_column_mode must be true if we are storing the default for a new
 * attribute, and false if it's for an already existing attribute. The reason
 * for this is that the missing value must never be updated after it is set,
 * which can only be when a column is added to the table. Otherwise we would
 * in effect be changing existing tuples.
 */
Oid
StoreAttrDefault(Relation rel, AttrNumber attnum,
				 Node *expr, bool is_internal, bool add_column_mode)
{
	char	   *adbin;
	Relation	adrel;
	HeapTuple	tuple;
	Datum		values[4];
	static bool nulls[4] = {false, false, false, false};
	Relation	attrrel;
	HeapTuple	atttup;
	Form_pg_attribute attStruct;
	char		attgenerated;
	Oid			attrdefOid;
	ObjectAddress colobject,
				defobject;

	adrel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	/*
	 * Flatten expression to string form for storage.
	 */
	adbin = nodeToString(expr);

	/*
	 * Make the pg_attrdef entry.
	 */
	attrdefOid = GetNewOidWithIndex(adrel, AttrDefaultOidIndexId,
									Anum_pg_attrdef_oid);
	values[Anum_pg_attrdef_oid - 1] = ObjectIdGetDatum(attrdefOid);
	values[Anum_pg_attrdef_adrelid - 1] = RelationGetRelid(rel);
	values[Anum_pg_attrdef_adnum - 1] = attnum;
	values[Anum_pg_attrdef_adbin - 1] = CStringGetTextDatum(adbin);

	tuple = heap_form_tuple(adrel->rd_att, values, nulls);
	CatalogTupleInsert(adrel, tuple);

	defobject.classId = AttrDefaultRelationId;
	defobject.objectId = attrdefOid;
	defobject.objectSubId = 0;

	table_close(adrel, RowExclusiveLock);

	/* now can free some of the stuff allocated above */
	pfree(DatumGetPointer(values[Anum_pg_attrdef_adbin - 1]));
	heap_freetuple(tuple);
	pfree(adbin);

	/*
	 * Update the pg_attribute entry for the column to show that a default
	 * exists.
	 */
	attrrel = table_open(AttributeRelationId, RowExclusiveLock);
	atttup = SearchSysCacheCopy2(ATTNUM,
								 ObjectIdGetDatum(RelationGetRelid(rel)),
								 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, RelationGetRelid(rel));
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);
	attgenerated = attStruct->attgenerated;
	if (!attStruct->atthasdef)
	{
		Form_pg_attribute defAttStruct;

		ExprState  *exprState;
		Expr	   *expr2 = (Expr *) expr;
		EState	   *estate = NULL;
		ExprContext *econtext;
		Datum		valuesAtt[Natts_pg_attribute];
		bool		nullsAtt[Natts_pg_attribute];
		bool		replacesAtt[Natts_pg_attribute];
		Datum		missingval = (Datum) 0;
		bool		missingIsNull = true;

		MemSet(valuesAtt, 0, sizeof(valuesAtt));
		MemSet(nullsAtt, false, sizeof(nullsAtt));
		MemSet(replacesAtt, false, sizeof(replacesAtt));
		valuesAtt[Anum_pg_attribute_atthasdef - 1] = true;
		replacesAtt[Anum_pg_attribute_atthasdef - 1] = true;

		if (rel->rd_rel->relkind == RELKIND_RELATION  && add_column_mode &&
			!attgenerated)
		{
			expr2 = expression_planner(expr2);
			estate = CreateExecutorState();
			exprState = ExecPrepareExpr(expr2, estate);
			econtext = GetPerTupleExprContext(estate);

			missingval = ExecEvalExpr(exprState, econtext,
									  &missingIsNull);

			FreeExecutorState(estate);

			defAttStruct = TupleDescAttr(rel->rd_att, attnum - 1);

			if (missingIsNull)
			{
				/* if the default evaluates to NULL, just store a NULL array */
				missingval = (Datum) 0;
			}
			else
			{
				/* otherwise make a one-element array of the value */
				missingval = PointerGetDatum(construct_array(&missingval,
															 1,
															 defAttStruct->atttypid,
															 defAttStruct->attlen,
															 defAttStruct->attbyval,
															 defAttStruct->attalign));
			}

			valuesAtt[Anum_pg_attribute_atthasmissing - 1] = !missingIsNull;
			replacesAtt[Anum_pg_attribute_atthasmissing - 1] = true;
			valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
			replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;
			nullsAtt[Anum_pg_attribute_attmissingval - 1] = missingIsNull;
		}
		atttup = heap_modify_tuple(atttup, RelationGetDescr(attrrel),
								   valuesAtt, nullsAtt, replacesAtt);

		CatalogTupleUpdate(attrrel, &atttup->t_self, atttup);

		if (!missingIsNull)
			pfree(DatumGetPointer(missingval));

	}
	table_close(attrrel, RowExclusiveLock);
	heap_freetuple(atttup);

	/*
	 * Make a dependency so that the pg_attrdef entry goes away if the column
	 * (or whole table) is deleted.
	 */
	colobject.classId = RelationRelationId;
	colobject.objectId = RelationGetRelid(rel);
	colobject.objectSubId = attnum;

	recordDependencyOn(&defobject, &colobject, DEPENDENCY_AUTO);

	/*
	 * Record dependencies on objects used in the expression, too.
	 */
	if (attgenerated)
	{
		/*
		 * Generated column: Dropping anything that the generation expression
		 * refers to automatically drops the generated column.
		 */
		recordDependencyOnSingleRelExpr(&colobject, expr, RelationGetRelid(rel),
										DEPENDENCY_AUTO,
										DEPENDENCY_AUTO, false);
	}
	else
	{
		/*
		 * Normal default: Dropping anything that the default refers to
		 * requires CASCADE and drops the default only.
		 */
		recordDependencyOnSingleRelExpr(&defobject, expr, RelationGetRelid(rel),
										DEPENDENCY_NORMAL,
										DEPENDENCY_NORMAL, false);
	}

	/*
	 * Post creation hook for attribute defaults.
	 *
	 * XXX. ALTER TABLE ALTER COLUMN SET/DROP DEFAULT is implemented with a
	 * couple of deletion/creation of the attribute's default entry, so the
	 * callee should check existence of an older version of this entry if it
	 * needs to distinguish.
	 */
	InvokeObjectPostCreateHookArg(AttrDefaultRelationId,
								  RelationGetRelid(rel), attnum, is_internal);

	return attrdefOid;
}

/*
 * Store a check-constraint expression for the given relation.
 *
 * Caller is responsible for updating the count of constraints
 * in the pg_class entry for the relation.
 *
 * The OID of the new constraint is returned.
 */
static Oid
StoreRelCheck(Relation rel, const char *ccname, Node *expr,
			  bool is_validated, bool is_local, int inhcount,
			  bool is_no_inherit, bool is_internal)
{
	char	   *ccbin;
	List	   *varList;
	int			keycount;
	int16	   *attNos;
	Oid			constrOid;

	/*
	 * Flatten expression to string form for storage.
	 */
	ccbin = nodeToString(expr);

	/*
	 * Find columns of rel that are used in expr
	 *
	 * NB: pull_var_clause is okay here only because we don't allow subselects
	 * in check constraints; it would fail to examine the contents of
	 * subselects.
	 */
	varList = pull_var_clause(expr, 0);
	keycount = list_length(varList);

	if (keycount > 0)
	{
		ListCell   *vl;
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
	 * Partitioned tables do not contain any rows themselves, so a NO INHERIT
	 * constraint makes no sense.
	 */
	if (is_no_inherit &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot add NO INHERIT constraint to partitioned table \"%s\"",
						RelationGetRelationName(rel))));

	/*
	 * Create the Check Constraint
	 */
	constrOid =
		CreateConstraintEntry(ccname,	/* Constraint Name */
							  RelationGetNamespace(rel),	/* namespace */
							  CONSTRAINT_CHECK, /* Constraint Type */
							  false,	/* Is Deferrable */
							  false,	/* Is Deferred */
							  is_validated,
							  InvalidOid,	/* no parent constraint */
							  RelationGetRelid(rel),	/* relation */
							  attNos,	/* attrs in the constraint */
							  keycount, /* # key attrs in the constraint */
							  keycount, /* # total attrs in the constraint */
							  InvalidOid,	/* not a domain constraint */
							  InvalidOid,	/* no associated index */
							  InvalidOid,	/* Foreign key fields */
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  0,
							  ' ',
							  ' ',
							  ' ',
							  NULL, /* not an exclusion constraint */
							  expr, /* Tree form of check constraint */
							  ccbin,	/* Binary form of check constraint */
							  is_local, /* conislocal */
							  inhcount, /* coninhcount */
							  is_no_inherit,	/* connoinherit */
							  is_internal); /* internally constructed? */

	pfree(ccbin);

	return constrOid;
}

/*
 * Store defaults and constraints (passed as a list of CookedConstraint).
 *
 * Each CookedConstraint struct is modified to store the new catalog tuple OID.
 *
 * NOTE: only pre-cooked expressions will be passed this way, which is to
 * say expressions inherited from an existing relation.  Newly parsed
 * expressions can be added later, by direct calls to StoreAttrDefault
 * and StoreRelCheck (see AddRelationNewConstraints()).
 */
static void
StoreConstraints(Relation rel, List *cooked_constraints, bool is_internal)
{
	int			numchecks = 0;
	ListCell   *lc;

	if (cooked_constraints == NIL)
		return;					/* nothing to do */

	/*
	 * Deparsing of constraint expressions will fail unless the just-created
	 * pg_attribute tuples for this relation are made visible.  So, bump the
	 * command counter.  CAUTION: this will cause a relcache entry rebuild.
	 */
	CommandCounterIncrement();

	foreach(lc, cooked_constraints)
	{
		CookedConstraint *con = (CookedConstraint *) lfirst(lc);

		switch (con->contype)
		{
			case CONSTR_DEFAULT:
				con->conoid = StoreAttrDefault(rel, con->attnum, con->expr,
											   is_internal, false);
				break;
			case CONSTR_CHECK:
				con->conoid =
					StoreRelCheck(rel, con->name, con->expr,
								  !con->skip_validation, con->is_local,
								  con->inhcount, con->is_no_inherit,
								  is_internal);
				numchecks++;
				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}

	if (numchecks > 0)
		SetRelationNumChecks(rel, numchecks);
}

/*
 * AddRelationNewConstraints
 *
 * Add new column default expressions and/or constraint check expressions
 * to an existing relation.  This is defined to do both for efficiency in
 * DefineRelation, but of course you can do just one or the other by passing
 * empty lists.
 *
 * rel: relation to be modified
 * newColDefaults: list of RawColumnDefault structures
 * newConstraints: list of Constraint nodes
 * allow_merge: true if check constraints may be merged with existing ones
 * is_local: true if definition is local, false if it's inherited
 * is_internal: true if result of some internal process, not a user request
 *
 * All entries in newColDefaults will be processed.  Entries in newConstraints
 * will be processed only if they are CONSTR_CHECK type.
 *
 * Returns a list of CookedConstraint nodes that shows the cooked form of
 * the default and constraint expressions added to the relation.
 *
 * NB: caller should have opened rel with AccessExclusiveLock, and should
 * hold that lock till end of transaction.  Also, we assume the caller has
 * done a CommandCounterIncrement if necessary to make the relation's catalog
 * tuples visible.
 */
List *
AddRelationNewConstraints(Relation rel,
						  List *newColDefaults,
						  List *newConstraints,
						  bool allow_merge,
						  bool is_local,
						  bool is_internal,
						  const char *queryString)
{
	List	   *cookedConstraints = NIL;
	TupleDesc	tupleDesc;
	TupleConstr *oldconstr;
	int			numoldchecks;
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	int			numchecks;
	List	   *checknames;
	ListCell   *cell;
	Node	   *expr;
	CookedConstraint *cooked;

	/*
	 * Get info about existing constraints.
	 */
	tupleDesc = RelationGetDescr(rel);
	oldconstr = tupleDesc->constr;
	if (oldconstr)
		numoldchecks = oldconstr->num_check;
	else
		numoldchecks = 0;

	/*
	 * Create a dummy ParseState and insert the target relation as its sole
	 * rangetable entry.  We need a ParseState for transformExpr.
	 */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	nsitem = addRangeTableEntryForRelation(pstate,
										   rel,
										   AccessShareLock,
										   NULL,
										   false,
										   true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	/*
	 * Process column default expressions.
	 */
	foreach(cell, newColDefaults)
	{
		RawColumnDefault *colDef = (RawColumnDefault *) lfirst(cell);
		Form_pg_attribute atp = TupleDescAttr(rel->rd_att, colDef->attnum - 1);
		Oid			defOid;

		expr = cookDefault(pstate, colDef->raw_default,
						   atp->atttypid, atp->atttypmod,
						   NameStr(atp->attname),
						   atp->attgenerated);

		/*
		 * If the expression is just a NULL constant, we do not bother to make
		 * an explicit pg_attrdef entry, since the default behavior is
		 * equivalent.  This applies to column defaults, but not for
		 * generation expressions.
		 *
		 * Note a nonobvious property of this test: if the column is of a
		 * domain type, what we'll get is not a bare null Const but a
		 * CoerceToDomain expr, so we will not discard the default.  This is
		 * critical because the column default needs to be retained to
		 * override any default that the domain might have.
		 */
		if (expr == NULL ||
			(!colDef->generated &&
			 IsA(expr, Const) &&
			 castNode(Const, expr)->constisnull))
			continue;

		/* If the DEFAULT is volatile we cannot use a missing value */
		if (colDef->missingMode && contain_volatile_functions((Node *) expr))
			colDef->missingMode = false;

		defOid = StoreAttrDefault(rel, colDef->attnum, expr, is_internal,
								  colDef->missingMode);

		cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
		cooked->contype = CONSTR_DEFAULT;
		cooked->conoid = defOid;
		cooked->name = NULL;
		cooked->attnum = colDef->attnum;
		cooked->expr = expr;
		cooked->skip_validation = false;
		cooked->is_local = is_local;
		cooked->inhcount = is_local ? 0 : 1;
		cooked->is_no_inherit = false;
		cookedConstraints = lappend(cookedConstraints, cooked);
	}

	/*
	 * Process constraint expressions.
	 */
	numchecks = numoldchecks;
	checknames = NIL;
	foreach(cell, newConstraints)
	{
		Constraint *cdef = (Constraint *) lfirst(cell);
		char	   *ccname;
		Oid			constrOid;

		if (cdef->contype != CONSTR_CHECK)
			continue;

		if (cdef->raw_expr != NULL)
		{
			Assert(cdef->cooked_expr == NULL);

			/*
			 * Transform raw parsetree to executable expression, and verify
			 * it's valid as a CHECK constraint.
			 */
			expr = cookConstraint(pstate, cdef->raw_expr,
								  RelationGetRelationName(rel));
		}
		else
		{
			Assert(cdef->cooked_expr != NULL);

			/*
			 * Here, we assume the parser will only pass us valid CHECK
			 * expressions, so we do no particular checking.
			 */
			expr = stringToNode(cdef->cooked_expr);
		}

		/*
		 * Check name uniqueness, or generate a name if none was given.
		 */
		if (cdef->conname != NULL)
		{
			ListCell   *cell2;

			ccname = cdef->conname;
			/* Check against other new constraints */
			/* Needed because we don't do CommandCounterIncrement in loop */
			foreach(cell2, checknames)
			{
				if (strcmp((char *) lfirst(cell2), ccname) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("check constraint \"%s\" already exists",
									ccname)));
			}

			/* save name for future checks */
			checknames = lappend(checknames, ccname);

			/*
			 * Check against pre-existing constraints.  If we are allowed to
			 * merge with an existing constraint, there's no more to do here.
			 * (We omit the duplicate constraint from the result, which is
			 * what ATAddCheckConstraint wants.)
			 */
			if (MergeWithExistingConstraint(rel, ccname, expr,
											allow_merge, is_local,
											cdef->initially_valid,
											cdef->is_no_inherit))
				continue;
		}
		else
		{
			/*
			 * When generating a name, we want to create "tab_col_check" for a
			 * column constraint and "tab_check" for a table constraint.  We
			 * no longer have any info about the syntactic positioning of the
			 * constraint phrase, so we approximate this by seeing whether the
			 * expression references more than one column.  (If the user
			 * played by the rules, the result is the same...)
			 *
			 * Note: pull_var_clause() doesn't descend into sublinks, but we
			 * eliminated those above; and anyway this only needs to be an
			 * approximate answer.
			 */
			List	   *vars;
			char	   *colname;

			vars = pull_var_clause(expr, 0);

			/* eliminate duplicates */
			vars = list_union(NIL, vars);

			if (list_length(vars) == 1)
				colname = get_attname(RelationGetRelid(rel),
									  ((Var *) linitial(vars))->varattno,
									  true);
			else
				colname = NULL;

			ccname = ChooseConstraintName(RelationGetRelationName(rel),
										  colname,
										  "check",
										  RelationGetNamespace(rel),
										  checknames);

			/* save name for future checks */
			checknames = lappend(checknames, ccname);
		}

		/*
		 * OK, store it.
		 */
		constrOid =
			StoreRelCheck(rel, ccname, expr, cdef->initially_valid, is_local,
						  is_local ? 0 : 1, cdef->is_no_inherit, is_internal);

		numchecks++;

		cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
		cooked->contype = CONSTR_CHECK;
		cooked->conoid = constrOid;
		cooked->name = ccname;
		cooked->attnum = 0;
		cooked->expr = expr;
		cooked->skip_validation = cdef->skip_validation;
		cooked->is_local = is_local;
		cooked->inhcount = is_local ? 0 : 1;
		cooked->is_no_inherit = cdef->is_no_inherit;
		cookedConstraints = lappend(cookedConstraints, cooked);
	}

	/*
	 * Update the count of constraints in the relation's pg_class tuple. We do
	 * this even if there was no change, in order to ensure that an SI update
	 * message is sent out for the pg_class tuple, which will force other
	 * backends to rebuild their relcache entries for the rel. (This is
	 * critical if we added defaults but not constraints.)
	 */
	SetRelationNumChecks(rel, numchecks);

	return cookedConstraints;
}

/*
 * Check for a pre-existing check constraint that conflicts with a proposed
 * new one, and either adjust its conislocal/coninhcount settings or throw
 * error as needed.
 *
 * Returns true if merged (constraint is a duplicate), or false if it's
 * got a so-far-unique name, or throws error if conflict.
 *
 * XXX See MergeConstraintsIntoExisting too if you change this code.
 */
static bool
MergeWithExistingConstraint(Relation rel, const char *ccname, Node *expr,
							bool allow_merge, bool is_local,
							bool is_initially_valid,
							bool is_no_inherit)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[3];
	HeapTuple	tup;

	/* Search for a pg_constraint entry with same name and relation */
	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	found = false;

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(ccname));

	conscan = systable_beginscan(conDesc, ConstraintRelidTypidNameIndexId, true,
								 NULL, 3, skey);

	/* There can be at most one matching row */
	if (HeapTupleIsValid(tup = systable_getnext(conscan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

		/* Found it.  Conflicts if not identical check constraint */
		if (con->contype == CONSTRAINT_CHECK)
		{
			Datum		val;
			bool		isnull;

			val = fastgetattr(tup,
							  Anum_pg_constraint_conbin,
							  conDesc->rd_att, &isnull);
			if (isnull)
				elog(ERROR, "null conbin for rel %s",
					 RelationGetRelationName(rel));
			if (equal(expr, stringToNode(TextDatumGetCString(val))))
				found = true;
		}

		/*
		 * If the existing constraint is purely inherited (no local
		 * definition) then interpret addition of a local constraint as a
		 * legal merge.  This allows ALTER ADD CONSTRAINT on parent and child
		 * tables to be given in either order with same end state.  However if
		 * the relation is a partition, all inherited constraints are always
		 * non-local, including those that were merged.
		 */
		if (is_local && !con->conislocal && !rel->rd_rel->relispartition)
			allow_merge = true;

		if (!found || !allow_merge)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("constraint \"%s\" for relation \"%s\" already exists",
							ccname, RelationGetRelationName(rel))));

		/* If the child constraint is "no inherit" then cannot merge */
		if (con->connoinherit)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("constraint \"%s\" conflicts with non-inherited constraint on relation \"%s\"",
							ccname, RelationGetRelationName(rel))));

		/*
		 * Must not change an existing inherited constraint to "no inherit"
		 * status.  That's because inherited constraints should be able to
		 * propagate to lower-level children.
		 */
		if (con->coninhcount > 0 && is_no_inherit)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("constraint \"%s\" conflicts with inherited constraint on relation \"%s\"",
							ccname, RelationGetRelationName(rel))));

		/*
		 * If the child constraint is "not valid" then cannot merge with a
		 * valid parent constraint.
		 */
		if (is_initially_valid && !con->convalidated)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("constraint \"%s\" conflicts with NOT VALID constraint on relation \"%s\"",
							ccname, RelationGetRelationName(rel))));

		/* OK to update the tuple */
		ereport(NOTICE,
				(errmsg("merging constraint \"%s\" with inherited definition",
						ccname)));

		tup = heap_copytuple(tup);
		con = (Form_pg_constraint) GETSTRUCT(tup);

		/*
		 * In case of partitions, an inherited constraint must be inherited
		 * only once since it cannot have multiple parents and it is never
		 * considered local.
		 */
		if (rel->rd_rel->relispartition)
		{
			con->coninhcount = 1;
			con->conislocal = false;
		}
		else
		{
			if (is_local)
				con->conislocal = true;
			else
				con->coninhcount++;
		}

		if (is_no_inherit)
		{
			Assert(is_local);
			con->connoinherit = true;
		}

		CatalogTupleUpdate(conDesc, &tup->t_self, tup);
	}

	systable_endscan(conscan);
	table_close(conDesc, RowExclusiveLock);

	return found;
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

	relrel = table_open(RelationRelationId, RowExclusiveLock);
	reltup = SearchSysCacheCopy1(RELOID,
								 ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));
	relStruct = (Form_pg_class) GETSTRUCT(reltup);

	if (relStruct->relchecks != numchecks)
	{
		relStruct->relchecks = numchecks;

		CatalogTupleUpdate(relrel, &reltup->t_self, reltup);
	}
	else
	{
		/* Skip the disk update, but force relcache inval anyway */
		CacheInvalidateRelcache(rel);
	}

	heap_freetuple(reltup);
	table_close(relrel, RowExclusiveLock);
}

/*
 * Check for references to generated columns
 */
static bool
check_nested_generated_walker(Node *node, void *context)
{
	ParseState *pstate = context;

	if (node == NULL)
		return false;
	else if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Oid			relid;
		AttrNumber	attnum;

		relid = rt_fetch(var->varno, pstate->p_rtable)->relid;
		if (!OidIsValid(relid))
			return false;		/* XXX shouldn't we raise an error? */

		attnum = var->varattno;

		if (attnum > 0 && get_attgenerated(relid, attnum))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot use generated column \"%s\" in column generation expression",
							get_attname(relid, attnum, false)),
					 errdetail("A generated column cannot reference another generated column."),
					 parser_errposition(pstate, var->location)));
		/* A whole-row Var is necessarily self-referential, so forbid it */
		if (attnum == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot use whole-row variable in column generation expression"),
					 errdetail("This would cause the generated column to depend on its own value."),
					 parser_errposition(pstate, var->location)));
		/* System columns were already checked in the parser */

		return false;
	}
	else
		return expression_tree_walker(node, check_nested_generated_walker,
									  (void *) context);
}

static void
check_nested_generated(ParseState *pstate, Node *node)
{
	check_nested_generated_walker(node, pstate);
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
			const char *attname,
			char attgenerated)
{
	Node	   *expr;

	Assert(raw_default != NULL);

	/*
	 * Transform raw parsetree to executable expression.
	 */
	expr = transformExpr(pstate, raw_default, attgenerated ? EXPR_KIND_GENERATED_COLUMN : EXPR_KIND_COLUMN_DEFAULT);

	if (attgenerated)
	{
		check_nested_generated(pstate, expr);

		if (contain_mutable_functions(expr))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("generation expression is not immutable")));
	}
	else
	{
		/*
		 * For a default expression, transformExpr() should have rejected
		 * column references.
		 */
		Assert(!contain_var_clause(expr));
	}

	/*
	 * Coerce the expression to the correct type and typmod, if given. This
	 * should match the parser's processing of non-defaulted expressions ---
	 * see transformAssignedExpr().
	 */
	if (OidIsValid(atttypid))
	{
		Oid			type_id = exprType(expr);

		expr = coerce_to_target_type(pstate, expr, type_id,
									 atttypid, atttypmod,
									 COERCION_ASSIGNMENT,
									 COERCE_IMPLICIT_CAST,
									 -1);
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

	/*
	 * Finally, take care of collations in the finished expression.
	 */
	assign_expr_collations(pstate, expr);

	return expr;
}

/*
 * Take a raw CHECK constraint expression and convert it to a cooked format
 * ready for storage.
 *
 * Parse state must be set up to recognize any vars that might appear
 * in the expression.
 */
static Node *
cookConstraint(ParseState *pstate,
			   Node *raw_constraint,
			   char *relname)
{
	Node	   *expr;

	/*
	 * Transform raw parsetree to executable expression.
	 */
	expr = transformExpr(pstate, raw_constraint, EXPR_KIND_CHECK_CONSTRAINT);

	/*
	 * Make sure it yields a boolean result.
	 */
	expr = coerce_to_boolean(pstate, expr, "CHECK");

	/*
	 * Take care of collations.
	 */
	assign_expr_collations(pstate, expr);

	/*
	 * Make sure no outside relations are referred to (this is probably dead
	 * code now that add_missing_from is history).
	 */
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("only table \"%s\" can be referenced in check constraint",
						relname)));

	return expr;
}

/*
 * CopyStatistics --- copy entries in pg_statistic from one rel to another
 */
void
CopyStatistics(Oid fromrelid, Oid torelid)
{
	HeapTuple	tup;
	SysScanDesc scan;
	ScanKeyData key[1];
	Relation	statrel;

	statrel = table_open(StatisticRelationId, RowExclusiveLock);

	/* Now search for stat records */
	ScanKeyInit(&key[0],
				Anum_pg_statistic_starelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(fromrelid));

	scan = systable_beginscan(statrel, StatisticRelidAttnumInhIndexId,
							  true, NULL, 1, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_statistic statform;

		/* make a modifiable copy */
		tup = heap_copytuple(tup);
		statform = (Form_pg_statistic) GETSTRUCT(tup);

		/* update the copy of the tuple and insert it */
		statform->starelid = torelid;
		CatalogTupleInsert(statrel, tup);

		heap_freetuple(tup);
	}

	systable_endscan(scan);

	table_close(statrel, RowExclusiveLock);
}

/*
 * RemoveStatistics --- remove entries in pg_statistic for a rel or column
 *
 * If attnum is zero, remove all entries for rel; else remove only the one(s)
 * for that column.
 */
void
RemoveStatistics(Oid relid, AttrNumber attnum)
{
	Relation	pgstatistic;
	SysScanDesc scan;
	ScanKeyData key[2];
	int			nkeys;
	HeapTuple	tuple;

	pgstatistic = table_open(StatisticRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_statistic_starelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	if (attnum == 0)
		nkeys = 1;
	else
	{
		ScanKeyInit(&key[1],
					Anum_pg_statistic_staattnum,
					BTEqualStrategyNumber, F_INT2EQ,
					Int16GetDatum(attnum));
		nkeys = 2;
	}

	scan = systable_beginscan(pgstatistic, StatisticRelidAttnumInhIndexId, true,
							  NULL, nkeys, key);

	/* we must loop even when attnum != 0, in case of inherited stats */
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(pgstatistic, &tuple->t_self);

	systable_endscan(scan);

	table_close(pgstatistic, RowExclusiveLock);
}


/*
 * RelationTruncateIndexes - truncate all indexes associated
 * with the heap relation to zero tuples.
 *
 * The routine will truncate and then reconstruct the indexes on
 * the specified relation.  Caller must hold exclusive lock on rel.
 */
static void
RelationTruncateIndexes(Relation heapRelation)
{
	ListCell   *indlist;

	/* Ask the relcache to produce a list of the indexes of the rel */
	foreach(indlist, RelationGetIndexList(heapRelation))
	{
		Oid			indexId = lfirst_oid(indlist);
		Relation	currentIndex;
		IndexInfo  *indexInfo;

		/* Open the index relation; use exclusive lock, just to be sure */
		currentIndex = index_open(indexId, AccessExclusiveLock);

		/*
		 * Fetch info needed for index_build.  Since we know there are no
		 * tuples that actually need indexing, we can use a dummy IndexInfo.
		 * This is slightly cheaper to build, but the real point is to avoid
		 * possibly running user-defined code in index expressions or
		 * predicates.  We might be getting invoked during ON COMMIT
		 * processing, and we don't want to run any such code then.
		 */
		indexInfo = BuildDummyIndexInfo(currentIndex);

		/*
		 * Now truncate the actual file (and discard buffers).
		 */
		RelationTruncate(currentIndex, 0);

		/* Initialize the index and rebuild */
		/* Note: we do not need to re-establish pkey setting */
		index_build(heapRelation, currentIndex, indexInfo, true, false);

		/* We're done with this index */
		index_close(currentIndex, NoLock);
	}
}

/*
 *	 heap_truncate
 *
 *	 This routine deletes all data within all the specified relations.
 *
 * This is not transaction-safe!  There is another, transaction-safe
 * implementation in commands/tablecmds.c.  We now use this only for
 * ON COMMIT truncation of temporary tables, where it doesn't matter.
 */
void
heap_truncate(List *relids)
{
	List	   *relations = NIL;
	ListCell   *cell;

	/* Open relations for processing, and grab exclusive access on each */
	foreach(cell, relids)
	{
		Oid			rid = lfirst_oid(cell);
		Relation	rel;

		rel = table_open(rid, AccessExclusiveLock);
		relations = lappend(relations, rel);
	}

	/* Don't allow truncate on tables that are referenced by foreign keys */
	heap_truncate_check_FKs(relations, true);

	/* OK to do it */
	foreach(cell, relations)
	{
		Relation	rel = lfirst(cell);

		/* Truncate the relation */
		heap_truncate_one_rel(rel);

		/* Close the relation, but keep exclusive lock on it until commit */
		table_close(rel, NoLock);
	}
}

/*
 *	 heap_truncate_one_rel
 *
 *	 This routine deletes all data within the specified relation.
 *
 * This is not transaction-safe, because the truncation is done immediately
 * and cannot be rolled back later.  Caller is responsible for having
 * checked permissions etc, and must have obtained AccessExclusiveLock.
 */
void
heap_truncate_one_rel(Relation rel)
{
	Oid			toastrelid;

	/*
	 * Truncate the relation.  Partitioned tables have no storage, so there is
	 * nothing to do for them here.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return;

	/* Truncate the underlying relation */
	table_relation_nontransactional_truncate(rel);

	/* If the relation has indexes, truncate the indexes too */
	RelationTruncateIndexes(rel);

	/* If there is a toast table, truncate that too */
	toastrelid = rel->rd_rel->reltoastrelid;
	if (OidIsValid(toastrelid))
	{
		Relation	toastrel = table_open(toastrelid, AccessExclusiveLock);

		table_relation_nontransactional_truncate(toastrel);
		RelationTruncateIndexes(toastrel);
		/* keep the lock... */
		table_close(toastrel, NoLock);
	}
}

/*
 * heap_truncate_check_FKs
 *		Check for foreign keys referencing a list of relations that
 *		are to be truncated, and raise error if there are any
 *
 * We disallow such FKs (except self-referential ones) since the whole point
 * of TRUNCATE is to not scan the individual rows to be thrown away.
 *
 * This is split out so it can be shared by both implementations of truncate.
 * Caller should already hold a suitable lock on the relations.
 *
 * tempTables is only used to select an appropriate error message.
 */
void
heap_truncate_check_FKs(List *relations, bool tempTables)
{
	List	   *oids = NIL;
	List	   *dependents;
	ListCell   *cell;

	/*
	 * Build a list of OIDs of the interesting relations.
	 *
	 * If a relation has no triggers, then it can neither have FKs nor be
	 * referenced by a FK from another table, so we can ignore it.  For
	 * partitioned tables, FKs have no triggers, so we must include them
	 * anyway.
	 */
	foreach(cell, relations)
	{
		Relation	rel = lfirst(cell);

		if (rel->rd_rel->relhastriggers ||
			rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			oids = lappend_oid(oids, RelationGetRelid(rel));
	}

	/*
	 * Fast path: if no relation has triggers, none has FKs either.
	 */
	if (oids == NIL)
		return;

	/*
	 * Otherwise, must scan pg_constraint.  We make one pass with all the
	 * relations considered; if this finds nothing, then all is well.
	 */
	dependents = heap_truncate_find_FKs(oids);
	if (dependents == NIL)
		return;

	/*
	 * Otherwise we repeat the scan once per relation to identify a particular
	 * pair of relations to complain about.  This is pretty slow, but
	 * performance shouldn't matter much in a failure path.  The reason for
	 * doing things this way is to ensure that the message produced is not
	 * dependent on chance row locations within pg_constraint.
	 */
	foreach(cell, oids)
	{
		Oid			relid = lfirst_oid(cell);
		ListCell   *cell2;

		dependents = heap_truncate_find_FKs(list_make1_oid(relid));

		foreach(cell2, dependents)
		{
			Oid			relid2 = lfirst_oid(cell2);

			if (!list_member_oid(oids, relid2))
			{
				char	   *relname = get_rel_name(relid);
				char	   *relname2 = get_rel_name(relid2);

				if (tempTables)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("unsupported ON COMMIT and foreign key combination"),
							 errdetail("Table \"%s\" references \"%s\", but they do not have the same ON COMMIT setting.",
									   relname2, relname)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot truncate a table referenced in a foreign key constraint"),
							 errdetail("Table \"%s\" references \"%s\".",
									   relname2, relname),
							 errhint("Truncate table \"%s\" at the same time, "
									 "or use TRUNCATE ... CASCADE.",
									 relname2)));
			}
		}
	}
}

/*
 * heap_truncate_find_FKs
 *		Find relations having foreign keys referencing any of the given rels
 *
 * Input and result are both lists of relation OIDs.  The result contains
 * no duplicates, does *not* include any rels that were already in the input
 * list, and is sorted in OID order.  (The last property is enforced mainly
 * to guarantee consistent behavior in the regression tests; we don't want
 * behavior to change depending on chance locations of rows in pg_constraint.)
 *
 * Note: caller should already have appropriate lock on all rels mentioned
 * in relationIds.  Since adding or dropping an FK requires exclusive lock
 * on both rels, this ensures that the answer will be stable.
 */
List *
heap_truncate_find_FKs(List *relationIds)
{
	List	   *result = NIL;
	List	   *oids = list_copy(relationIds);
	List	   *parent_cons;
	ListCell   *cell;
	ScanKeyData key;
	Relation	fkeyRel;
	SysScanDesc fkeyScan;
	HeapTuple	tuple;
	bool		restart;

	oids = list_copy(relationIds);

	/*
	 * Must scan pg_constraint.  Right now, it is a seqscan because there is
	 * no available index on confrelid.
	 */
	fkeyRel = table_open(ConstraintRelationId, AccessShareLock);

restart:
	restart = false;
	parent_cons = NIL;

	fkeyScan = systable_beginscan(fkeyRel, InvalidOid, false,
								  NULL, 0, NULL);

	while (HeapTupleIsValid(tuple = systable_getnext(fkeyScan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

		/* Not a foreign key */
		if (con->contype != CONSTRAINT_FOREIGN)
			continue;

		/* Not referencing one of our list of tables */
		if (!list_member_oid(oids, con->confrelid))
			continue;

		/*
		 * If this constraint has a parent constraint which we have not seen
		 * yet, keep track of it for the second loop, below.  Tracking parent
		 * constraints allows us to climb up to the top-level level constraint
		 * and look for all possible relations referencing the partitioned
		 * table.
		 */
		if (OidIsValid(con->conparentid) &&
			!list_member_oid(parent_cons, con->conparentid))
			parent_cons = lappend_oid(parent_cons, con->conparentid);

		/*
		 * Add referencer to result, unless present in input list.  (Don't
		 * worry about dupes: we'll fix that below).
		 */
		if (!list_member_oid(relationIds, con->conrelid))
			result = lappend_oid(result, con->conrelid);
	}

	systable_endscan(fkeyScan);

	/*
	 * Process each parent constraint we found to add the list of referenced
	 * relations by them to the oids list.  If we do add any new such
	 * relations, redo the first loop above.  Also, if we see that the parent
	 * constraint in turn has a parent, add that so that we process all
	 * relations in a single additional pass.
	 */
	foreach(cell, parent_cons)
	{
		Oid			parent = lfirst_oid(cell);

		ScanKeyInit(&key,
					Anum_pg_constraint_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(parent));

		fkeyScan = systable_beginscan(fkeyRel, ConstraintOidIndexId,
									  true, NULL, 1, &key);

		tuple = systable_getnext(fkeyScan);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

			/*
			 * pg_constraint rows always appear for partitioned hierarchies
			 * this way: on the each side of the constraint, one row appears
			 * for each partition that points to the top-most table on the
			 * other side.
			 *
			 * Because of this arrangement, we can correctly catch all
			 * relevant relations by adding to 'parent_cons' all rows with
			 * valid conparentid, and to the 'oids' list all rows with a zero
			 * conparentid.  If any oids are added to 'oids', redo the first
			 * loop above by setting 'restart'.
			 */
			if (OidIsValid(con->conparentid))
				parent_cons = list_append_unique_oid(parent_cons,
													 con->conparentid);
			else if (!list_member_oid(oids, con->confrelid))
			{
				oids = lappend_oid(oids, con->confrelid);
				restart = true;
			}
		}

		systable_endscan(fkeyScan);
	}

	list_free(parent_cons);
	if (restart)
		goto restart;

	table_close(fkeyRel, AccessShareLock);
	list_free(oids);

	/* Now sort and de-duplicate the result list */
	list_sort(result, list_oid_cmp);
	list_deduplicate_oid(result);

	return result;
}

/*
 * StorePartitionKey
 *		Store information about the partition key rel into the catalog
 */
void
StorePartitionKey(Relation rel,
				  char strategy,
				  int16 partnatts,
				  AttrNumber *partattrs,
				  List *partexprs,
				  Oid *partopclass,
				  Oid *partcollation)
{
	int			i;
	int2vector *partattrs_vec;
	oidvector  *partopclass_vec;
	oidvector  *partcollation_vec;
	Datum		partexprDatum;
	Relation	pg_partitioned_table;
	HeapTuple	tuple;
	Datum		values[Natts_pg_partitioned_table];
	bool		nulls[Natts_pg_partitioned_table];
	ObjectAddress myself;
	ObjectAddress referenced;

	Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/* Copy the partition attribute numbers, opclass OIDs into arrays */
	partattrs_vec = buildint2vector(partattrs, partnatts);
	partopclass_vec = buildoidvector(partopclass, partnatts);
	partcollation_vec = buildoidvector(partcollation, partnatts);

	/* Convert the expressions (if any) to a text datum */
	if (partexprs)
	{
		char	   *exprString;

		exprString = nodeToString(partexprs);
		partexprDatum = CStringGetTextDatum(exprString);
		pfree(exprString);
	}
	else
		partexprDatum = (Datum) 0;

	pg_partitioned_table = table_open(PartitionedRelationId, RowExclusiveLock);

	MemSet(nulls, false, sizeof(nulls));

	/* Only this can ever be NULL */
	if (!partexprDatum)
		nulls[Anum_pg_partitioned_table_partexprs - 1] = true;

	values[Anum_pg_partitioned_table_partrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_partitioned_table_partstrat - 1] = CharGetDatum(strategy);
	values[Anum_pg_partitioned_table_partnatts - 1] = Int16GetDatum(partnatts);
	values[Anum_pg_partitioned_table_partdefid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_partitioned_table_partattrs - 1] = PointerGetDatum(partattrs_vec);
	values[Anum_pg_partitioned_table_partclass - 1] = PointerGetDatum(partopclass_vec);
	values[Anum_pg_partitioned_table_partcollation - 1] = PointerGetDatum(partcollation_vec);
	values[Anum_pg_partitioned_table_partexprs - 1] = partexprDatum;

	tuple = heap_form_tuple(RelationGetDescr(pg_partitioned_table), values, nulls);

	CatalogTupleInsert(pg_partitioned_table, tuple);
	table_close(pg_partitioned_table, RowExclusiveLock);

	/* Mark this relation as dependent on a few things as follows */
	myself.classId = RelationRelationId;
	myself.objectId = RelationGetRelid(rel);
	myself.objectSubId = 0;

	/* Operator class and collation per key column */
	for (i = 0; i < partnatts; i++)
	{
		referenced.classId = OperatorClassRelationId;
		referenced.objectId = partopclass[i];
		referenced.objectSubId = 0;

		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

		/* The default collation is pinned, so don't bother recording it */
		if (OidIsValid(partcollation[i]) &&
			partcollation[i] != DEFAULT_COLLATION_OID)
		{
			referenced.classId = CollationRelationId;
			referenced.objectId = partcollation[i];
			referenced.objectSubId = 0;

			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	/*
	 * The partitioning columns are made internally dependent on the table,
	 * because we cannot drop any of them without dropping the whole table.
	 * (ATExecDropColumn independently enforces that, but it's not bulletproof
	 * so we need the dependencies too.)
	 */
	for (i = 0; i < partnatts; i++)
	{
		if (partattrs[i] == 0)
			continue;			/* ignore expressions here */

		referenced.classId = RelationRelationId;
		referenced.objectId = RelationGetRelid(rel);
		referenced.objectSubId = partattrs[i];

		recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
	}

	/*
	 * Also consider anything mentioned in partition expressions.  External
	 * references (e.g. functions) get NORMAL dependencies.  Table columns
	 * mentioned in the expressions are handled the same as plain partitioning
	 * columns, i.e. they become internally dependent on the whole table.
	 */
	if (partexprs)
		recordDependencyOnSingleRelExpr(&myself,
										(Node *) partexprs,
										RelationGetRelid(rel),
										DEPENDENCY_NORMAL,
										DEPENDENCY_INTERNAL,
										true /* reverse the self-deps */ );

	/*
	 * We must invalidate the relcache so that the next
	 * CommandCounterIncrement() will cause the same to be rebuilt using the
	 * information in just created catalog entry.
	 */
	CacheInvalidateRelcache(rel);
}

/*
 *	RemovePartitionKeyByRelId
 *		Remove pg_partitioned_table entry for a relation
 */
void
RemovePartitionKeyByRelId(Oid relid)
{
	Relation	rel;
	HeapTuple	tuple;

	rel = table_open(PartitionedRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for partition key of relation %u",
			 relid);

	CatalogTupleDelete(rel, &tuple->t_self);

	ReleaseSysCache(tuple);
	table_close(rel, RowExclusiveLock);
}

/*
 * StorePartitionBound
 *		Update pg_class tuple of rel to store the partition bound and set
 *		relispartition to true
 *
 * If this is the default partition, also update the default partition OID in
 * pg_partitioned_table.
 *
 * Also, invalidate the parent's relcache, so that the next rebuild will load
 * the new partition's info into its partition descriptor.  If there is a
 * default partition, we must invalidate its relcache entry as well.
 */
void
StorePartitionBound(Relation rel, Relation parent, PartitionBoundSpec *bound)
{
	Relation	classRel;
	HeapTuple	tuple,
				newtuple;
	Datum		new_val[Natts_pg_class];
	bool		new_null[Natts_pg_class],
				new_repl[Natts_pg_class];
	Oid			defaultPartOid;

	/* Update pg_class tuple */
	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));

#ifdef USE_ASSERT_CHECKING
	{
		Form_pg_class classForm;
		bool		isnull;

		classForm = (Form_pg_class) GETSTRUCT(tuple);
		Assert(!classForm->relispartition);
		(void) SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relpartbound,
							   &isnull);
		Assert(isnull);
	}
#endif

	/* Fill in relpartbound value */
	memset(new_val, 0, sizeof(new_val));
	memset(new_null, false, sizeof(new_null));
	memset(new_repl, false, sizeof(new_repl));
	new_val[Anum_pg_class_relpartbound - 1] = CStringGetTextDatum(nodeToString(bound));
	new_null[Anum_pg_class_relpartbound - 1] = false;
	new_repl[Anum_pg_class_relpartbound - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(classRel),
								 new_val, new_null, new_repl);
	/* Also set the flag */
	((Form_pg_class) GETSTRUCT(newtuple))->relispartition = true;
	CatalogTupleUpdate(classRel, &newtuple->t_self, newtuple);
	heap_freetuple(newtuple);
	table_close(classRel, RowExclusiveLock);

	/*
	 * If we're storing bounds for the default partition, update
	 * pg_partitioned_table too.
	 */
	if (bound->is_default)
		update_default_partition_oid(RelationGetRelid(parent),
									 RelationGetRelid(rel));

	/* Make these updates visible */
	CommandCounterIncrement();

	/*
	 * The partition constraint for the default partition depends on the
	 * partition bounds of every other partition, so we must invalidate the
	 * relcache entry for that partition every time a partition is added or
	 * removed.
	 */
	defaultPartOid = get_default_oid_from_partdesc(RelationGetPartitionDesc(parent));
	if (OidIsValid(defaultPartOid))
		CacheInvalidateRelcacheByRelid(defaultPartOid);

	CacheInvalidateRelcache(parent);
}
