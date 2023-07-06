/*-------------------------------------------------------------------------
 *
 * index.c
 *	  code to create and destroy POSTGRES index relations
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/index.c
 *
 *
 * INTERFACE ROUTINES
 *		index_create()			- Create a cataloged index relation
 *		index_drop()			- Removes index relation from catalogs
 *		BuildIndexInfo()		- Prepare to insert index tuples
 *		FormIndexDatum()		- Construct datum vector for one index tuple
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "bootstrap/bootstrap.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_description.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/event_trigger.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parser.h"
#include "pgstat.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"

/* Potentially set by pg_upgrade_support functions */
Oid			binary_upgrade_next_index_pg_class_oid = InvalidOid;

/*
 * Pointer-free representation of variables used when reindexing system
 * catalogs; we use this to propagate those values to parallel workers.
 */
typedef struct
{
	Oid			currentlyReindexedHeap;
	Oid			currentlyReindexedIndex;
	int			numPendingReindexedIndexes;
	Oid			pendingReindexedIndexes[FLEXIBLE_ARRAY_MEMBER];
} SerializedReindexState;

/* non-export function prototypes */
static bool relationHasPrimaryKey(Relation rel);
static TupleDesc ConstructTupleDescriptor(Relation heapRelation,
										  IndexInfo *indexInfo,
										  List *indexColNames,
										  Oid accessMethodObjectId,
										  Oid *collationObjectId,
										  Oid *classObjectId);
static void InitializeAttributeOids(Relation indexRelation,
									int numatts, Oid indexoid);
static void AppendAttributeTuples(Relation indexRelation, int numatts,
								  Datum *attopts);
static void UpdateIndexRelation(Oid indexoid, Oid heapoid,
								Oid parentIndexId,
								IndexInfo *indexInfo,
								Oid *collationOids,
								Oid *classOids,
								int16 *coloptions,
								bool primary,
								bool isexclusion,
								bool immediate,
								bool isvalid,
								bool isready);
static void index_update_stats(Relation rel,
							   bool hasindex,
							   double reltuples);
static void IndexCheckExclusion(Relation heapRelation,
								Relation indexRelation,
								IndexInfo *indexInfo);
static bool validate_index_callback(ItemPointer itemptr, void *opaque);
static bool ReindexIsCurrentlyProcessingIndex(Oid indexOid);
static void SetReindexProcessing(Oid heapOid, Oid indexOid);
static void ResetReindexProcessing(void);
static void SetReindexPending(List *indexes);
static void RemoveReindexPending(Oid indexOid);


/*
 * relationHasPrimaryKey
 *		See whether an existing relation has a primary key.
 *
 * Caller must have suitable lock on the relation.
 *
 * Note: we intentionally do not check indisvalid here; that's because this
 * is used to enforce the rule that there can be only one indisprimary index,
 * and we want that to be true even if said index is invalid.
 */
static bool
relationHasPrimaryKey(Relation rel)
{
	bool		result = false;
	List	   *indexoidlist;
	ListCell   *indexoidscan;

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache until we find one marked primary key
	 * (hopefully there isn't more than one such).
	 */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indexTuple;

		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))	/* should not happen */
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		result = ((Form_pg_index) GETSTRUCT(indexTuple))->indisprimary;
		ReleaseSysCache(indexTuple);
		if (result)
			break;
	}

	list_free(indexoidlist);

	return result;
}

/*
 * index_check_primary_key
 *		Apply special checks needed before creating a PRIMARY KEY index
 *
 * This processing used to be in DefineIndex(), but has been split out
 * so that it can be applied during ALTER TABLE ADD PRIMARY KEY USING INDEX.
 *
 * We check for a pre-existing primary key, and that all columns of the index
 * are simple column references (not expressions), and that all those
 * columns are marked NOT NULL.  If not, fail.
 *
 * We used to automatically change unmarked columns to NOT NULL here by doing
 * our own local ALTER TABLE command.  But that doesn't work well if we're
 * executing one subcommand of an ALTER TABLE: the operations may not get
 * performed in the right order overall.  Now we expect that the parser
 * inserted any required ALTER TABLE SET NOT NULL operations before trying
 * to create a primary-key index.
 *
 * Caller had better have at least ShareLock on the table, else the not-null
 * checking isn't trustworthy.
 */
void
index_check_primary_key(Relation heapRel,
						IndexInfo *indexInfo,
						bool is_alter_table,
						IndexStmt *stmt)
{
	int			i;

	/*
	 * If ALTER TABLE or CREATE TABLE .. PARTITION OF, check that there isn't
	 * already a PRIMARY KEY.  In CREATE TABLE for an ordinary relation, we
	 * have faith that the parser rejected multiple pkey clauses; and CREATE
	 * INDEX doesn't have a way to say PRIMARY KEY, so it's no problem either.
	 */
	if ((is_alter_table || heapRel->rd_rel->relispartition) &&
		relationHasPrimaryKey(heapRel))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("multiple primary keys for table \"%s\" are not allowed",
						RelationGetRelationName(heapRel))));
	}

	/*
	 * Check that all of the attributes in a primary key are marked as not
	 * null.  (We don't really expect to see that; it'd mean the parser messed
	 * up.  But it seems wise to check anyway.)
	 */
	for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
	{
		AttrNumber	attnum = indexInfo->ii_IndexAttrNumbers[i];
		HeapTuple	atttuple;
		Form_pg_attribute attform;

		if (attnum == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("primary keys cannot be expressions")));

		/* System attributes are never null, so no need to check */
		if (attnum < 0)
			continue;

		atttuple = SearchSysCache2(ATTNUM,
								   ObjectIdGetDatum(RelationGetRelid(heapRel)),
								   Int16GetDatum(attnum));
		if (!HeapTupleIsValid(atttuple))
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 attnum, RelationGetRelid(heapRel));
		attform = (Form_pg_attribute) GETSTRUCT(atttuple);

		if (!attform->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("primary key column \"%s\" is not marked NOT NULL",
							NameStr(attform->attname))));

		ReleaseSysCache(atttuple);
	}
}

/*
 *		ConstructTupleDescriptor
 *
 * Build an index tuple descriptor for a new index
 */
static TupleDesc
ConstructTupleDescriptor(Relation heapRelation,
						 IndexInfo *indexInfo,
						 List *indexColNames,
						 Oid accessMethodObjectId,
						 Oid *collationObjectId,
						 Oid *classObjectId)
{
	int			numatts = indexInfo->ii_NumIndexAttrs;
	int			numkeyatts = indexInfo->ii_NumIndexKeyAttrs;
	ListCell   *colnames_item = list_head(indexColNames);
	ListCell   *indexpr_item = list_head(indexInfo->ii_Expressions);
	IndexAmRoutine *amroutine;
	TupleDesc	heapTupDesc;
	TupleDesc	indexTupDesc;
	int			natts;			/* #atts in heap rel --- for error checks */
	int			i;

	/* We need access to the index AM's API struct */
	amroutine = GetIndexAmRoutineByAmId(accessMethodObjectId, false);

	/* ... and to the table's tuple descriptor */
	heapTupDesc = RelationGetDescr(heapRelation);
	natts = RelationGetForm(heapRelation)->relnatts;

	/*
	 * allocate the new tuple descriptor
	 */
	indexTupDesc = CreateTemplateTupleDesc(numatts);

	/*
	 * Fill in the pg_attribute row.
	 */
	for (i = 0; i < numatts; i++)
	{
		AttrNumber	atnum = indexInfo->ii_IndexAttrNumbers[i];
		Form_pg_attribute to = TupleDescAttr(indexTupDesc, i);
		HeapTuple	tuple;
		Form_pg_type typeTup;
		Form_pg_opclass opclassTup;
		Oid			keyType;

		MemSet(to, 0, ATTRIBUTE_FIXED_PART_SIZE);
		to->attnum = i + 1;
		to->attstattarget = -1;
		to->attcacheoff = -1;
		to->attislocal = true;
		to->attcollation = (i < numkeyatts) ?
			collationObjectId[i] : InvalidOid;

		/*
		 * Set the attribute name as specified by caller.
		 */
		if (colnames_item == NULL)	/* shouldn't happen */
			elog(ERROR, "too few entries in colnames list");
		namestrcpy(&to->attname, (const char *) lfirst(colnames_item));
		colnames_item = lnext(indexColNames, colnames_item);

		/*
		 * For simple index columns, we copy some pg_attribute fields from the
		 * parent relation.  For expressions we have to look at the expression
		 * result.
		 */
		if (atnum != 0)
		{
			/* Simple index column */
			const FormData_pg_attribute *from;

			Assert(atnum > 0);	/* should've been caught above */

			if (atnum > natts)	/* safety check */
				elog(ERROR, "invalid column number %d", atnum);
			from = TupleDescAttr(heapTupDesc,
								 AttrNumberGetAttrOffset(atnum));

			to->atttypid = from->atttypid;
			to->attlen = from->attlen;
			to->attndims = from->attndims;
			to->atttypmod = from->atttypmod;
			to->attbyval = from->attbyval;
			to->attstorage = from->attstorage;
			to->attalign = from->attalign;
		}
		else
		{
			/* Expressional index */
			Node	   *indexkey;

			if (indexpr_item == NULL)	/* shouldn't happen */
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(indexInfo->ii_Expressions, indexpr_item);

			/*
			 * Lookup the expression type in pg_type for the type length etc.
			 */
			keyType = exprType(indexkey);
			tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(keyType));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for type %u", keyType);
			typeTup = (Form_pg_type) GETSTRUCT(tuple);

			/*
			 * Assign some of the attributes values. Leave the rest.
			 */
			to->atttypid = keyType;
			to->attlen = typeTup->typlen;
			to->attbyval = typeTup->typbyval;
			to->attstorage = typeTup->typstorage;
			to->attalign = typeTup->typalign;
			to->atttypmod = exprTypmod(indexkey);

			ReleaseSysCache(tuple);

			/*
			 * Make sure the expression yields a type that's safe to store in
			 * an index.  We need this defense because we have index opclasses
			 * for pseudo-types such as "record", and the actually stored type
			 * had better be safe; eg, a named composite type is okay, an
			 * anonymous record type is not.  The test is the same as for
			 * whether a table column is of a safe type (which is why we
			 * needn't check for the non-expression case).
			 */
			CheckAttributeType(NameStr(to->attname),
							   to->atttypid, to->attcollation,
							   NIL, 0);
		}

		/*
		 * We do not yet have the correct relation OID for the index, so just
		 * set it invalid for now.  InitializeAttributeOids() will fix it
		 * later.
		 */
		to->attrelid = InvalidOid;

		/*
		 * Check the opclass and index AM to see if either provides a keytype
		 * (overriding the attribute type).  Opclass (if exists) takes
		 * precedence.
		 */
		keyType = amroutine->amkeytype;

		if (i < indexInfo->ii_NumIndexKeyAttrs)
		{
			tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(classObjectId[i]));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for opclass %u",
					 classObjectId[i]);
			opclassTup = (Form_pg_opclass) GETSTRUCT(tuple);
			if (OidIsValid(opclassTup->opckeytype))
				keyType = opclassTup->opckeytype;

			/*
			 * If keytype is specified as ANYELEMENT, and opcintype is
			 * ANYARRAY, then the attribute type must be an array (else it'd
			 * not have matched this opclass); use its element type.
			 *
			 * We could also allow ANYCOMPATIBLE/ANYCOMPATIBLEARRAY here, but
			 * there seems no need to do so; there's no reason to declare an
			 * opclass as taking ANYCOMPATIBLEARRAY rather than ANYARRAY.
			 */
			if (keyType == ANYELEMENTOID && opclassTup->opcintype == ANYARRAYOID)
			{
				keyType = get_base_element_type(to->atttypid);
				if (!OidIsValid(keyType))
					elog(ERROR, "could not get element type of array type %u",
						 to->atttypid);
			}

			ReleaseSysCache(tuple);
		}

		/*
		 * If a key type different from the heap value is specified, update
		 * the type-related fields in the index tupdesc.
		 */
		if (OidIsValid(keyType) && keyType != to->atttypid)
		{
			tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(keyType));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for type %u", keyType);
			typeTup = (Form_pg_type) GETSTRUCT(tuple);

			to->atttypid = keyType;
			to->atttypmod = -1;
			to->attlen = typeTup->typlen;
			to->attbyval = typeTup->typbyval;
			to->attalign = typeTup->typalign;
			to->attstorage = typeTup->typstorage;

			ReleaseSysCache(tuple);
		}
	}

	pfree(amroutine);

	return indexTupDesc;
}

/* ----------------------------------------------------------------
 *		InitializeAttributeOids
 * ----------------------------------------------------------------
 */
static void
InitializeAttributeOids(Relation indexRelation,
						int numatts,
						Oid indexoid)
{
	TupleDesc	tupleDescriptor;
	int			i;

	tupleDescriptor = RelationGetDescr(indexRelation);

	for (i = 0; i < numatts; i += 1)
		TupleDescAttr(tupleDescriptor, i)->attrelid = indexoid;
}

/* ----------------------------------------------------------------
 *		AppendAttributeTuples
 * ----------------------------------------------------------------
 */
static void
AppendAttributeTuples(Relation indexRelation, int numatts, Datum *attopts)
{
	Relation	pg_attribute;
	CatalogIndexState indstate;
	TupleDesc	indexTupDesc;
	int			i;

	/*
	 * open the attribute relation and its indexes
	 */
	pg_attribute = table_open(AttributeRelationId, RowExclusiveLock);

	indstate = CatalogOpenIndexes(pg_attribute);

	/*
	 * insert data from new index's tupdesc into pg_attribute
	 */
	indexTupDesc = RelationGetDescr(indexRelation);

	for (i = 0; i < numatts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(indexTupDesc, i);
		Datum		attoptions = attopts ? attopts[i] : (Datum) 0;

		Assert(attr->attnum == i + 1);

		InsertPgAttributeTuple(pg_attribute, attr, attoptions, indstate);
	}

	CatalogCloseIndexes(indstate);

	table_close(pg_attribute, RowExclusiveLock);
}

/* ----------------------------------------------------------------
 *		UpdateIndexRelation
 *
 * Construct and insert a new entry in the pg_index catalog
 * ----------------------------------------------------------------
 */
static void
UpdateIndexRelation(Oid indexoid,
					Oid heapoid,
					Oid parentIndexId,
					IndexInfo *indexInfo,
					Oid *collationOids,
					Oid *classOids,
					int16 *coloptions,
					bool primary,
					bool isexclusion,
					bool immediate,
					bool isvalid,
					bool isready)
{
	int2vector *indkey;
	oidvector  *indcollation;
	oidvector  *indclass;
	int2vector *indoption;
	Datum		exprsDatum;
	Datum		predDatum;
	Datum		values[Natts_pg_index];
	bool		nulls[Natts_pg_index];
	Relation	pg_index;
	HeapTuple	tuple;
	int			i;

	/*
	 * Copy the index key, opclass, and indoption info into arrays (should we
	 * make the caller pass them like this to start with?)
	 */
	indkey = buildint2vector(NULL, indexInfo->ii_NumIndexAttrs);
	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
		indkey->values[i] = indexInfo->ii_IndexAttrNumbers[i];
	indcollation = buildoidvector(collationOids, indexInfo->ii_NumIndexKeyAttrs);
	indclass = buildoidvector(classOids, indexInfo->ii_NumIndexKeyAttrs);
	indoption = buildint2vector(coloptions, indexInfo->ii_NumIndexKeyAttrs);

	/*
	 * Convert the index expressions (if any) to a text datum
	 */
	if (indexInfo->ii_Expressions != NIL)
	{
		char	   *exprsString;

		exprsString = nodeToString(indexInfo->ii_Expressions);
		exprsDatum = CStringGetTextDatum(exprsString);
		pfree(exprsString);
	}
	else
		exprsDatum = (Datum) 0;

	/*
	 * Convert the index predicate (if any) to a text datum.  Note we convert
	 * implicit-AND format to normal explicit-AND for storage.
	 */
	if (indexInfo->ii_Predicate != NIL)
	{
		char	   *predString;

		predString = nodeToString(make_ands_explicit(indexInfo->ii_Predicate));
		predDatum = CStringGetTextDatum(predString);
		pfree(predString);
	}
	else
		predDatum = (Datum) 0;


	/*
	 * open the system catalog index relation
	 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);

	/*
	 * Build a pg_index tuple
	 */
	MemSet(nulls, false, sizeof(nulls));

	values[Anum_pg_index_indexrelid - 1] = ObjectIdGetDatum(indexoid);
	values[Anum_pg_index_indrelid - 1] = ObjectIdGetDatum(heapoid);
	values[Anum_pg_index_indnatts - 1] = Int16GetDatum(indexInfo->ii_NumIndexAttrs);
	values[Anum_pg_index_indnkeyatts - 1] = Int16GetDatum(indexInfo->ii_NumIndexKeyAttrs);
	values[Anum_pg_index_indisunique - 1] = BoolGetDatum(indexInfo->ii_Unique);
	values[Anum_pg_index_indisprimary - 1] = BoolGetDatum(primary);
	values[Anum_pg_index_indisexclusion - 1] = BoolGetDatum(isexclusion);
	values[Anum_pg_index_indimmediate - 1] = BoolGetDatum(immediate);
	values[Anum_pg_index_indisclustered - 1] = BoolGetDatum(false);
	values[Anum_pg_index_indisvalid - 1] = BoolGetDatum(isvalid);
	values[Anum_pg_index_indcheckxmin - 1] = BoolGetDatum(false);
	values[Anum_pg_index_indisready - 1] = BoolGetDatum(isready);
	values[Anum_pg_index_indislive - 1] = BoolGetDatum(true);
	values[Anum_pg_index_indisreplident - 1] = BoolGetDatum(false);
	values[Anum_pg_index_indkey - 1] = PointerGetDatum(indkey);
	values[Anum_pg_index_indcollation - 1] = PointerGetDatum(indcollation);
	values[Anum_pg_index_indclass - 1] = PointerGetDatum(indclass);
	values[Anum_pg_index_indoption - 1] = PointerGetDatum(indoption);
	values[Anum_pg_index_indexprs - 1] = exprsDatum;
	if (exprsDatum == (Datum) 0)
		nulls[Anum_pg_index_indexprs - 1] = true;
	values[Anum_pg_index_indpred - 1] = predDatum;
	if (predDatum == (Datum) 0)
		nulls[Anum_pg_index_indpred - 1] = true;

	tuple = heap_form_tuple(RelationGetDescr(pg_index), values, nulls);

	/*
	 * insert the tuple into the pg_index catalog
	 */
	CatalogTupleInsert(pg_index, tuple);

	/*
	 * close the relation and free the tuple
	 */
	table_close(pg_index, RowExclusiveLock);
	heap_freetuple(tuple);
}


/*
 * index_create
 *
 * heapRelation: table to build index on (suitably locked by caller)
 * indexRelationName: what it say
 * indexRelationId: normally, pass InvalidOid to let this routine
 *		generate an OID for the index.  During bootstrap this may be
 *		nonzero to specify a preselected OID.
 * parentIndexRelid: if creating an index partition, the OID of the
 *		parent index; otherwise InvalidOid.
 * parentConstraintId: if creating a constraint on a partition, the OID
 *		of the constraint in the parent; otherwise InvalidOid.
 * relFileNode: normally, pass InvalidOid to get new storage.  May be
 *		nonzero to attach an existing valid build.
 * indexInfo: same info executor uses to insert into the index
 * indexColNames: column names to use for index (List of char *)
 * accessMethodObjectId: OID of index AM to use
 * tableSpaceId: OID of tablespace to use
 * collationObjectId: array of collation OIDs, one per index column
 * classObjectId: array of index opclass OIDs, one per index column
 * coloptions: array of per-index-column indoption settings
 * reloptions: AM-specific options
 * flags: bitmask that can include any combination of these bits:
 *		INDEX_CREATE_IS_PRIMARY
 *			the index is a primary key
 *		INDEX_CREATE_ADD_CONSTRAINT:
 *			invoke index_constraint_create also
 *		INDEX_CREATE_SKIP_BUILD:
 *			skip the index_build() step for the moment; caller must do it
 *			later (typically via reindex_index())
 *		INDEX_CREATE_CONCURRENT:
 *			do not lock the table against writers.  The index will be
 *			marked "invalid" and the caller must take additional steps
 *			to fix it up.
 *		INDEX_CREATE_IF_NOT_EXISTS:
 *			do not throw an error if a relation with the same name
 *			already exists.
 *		INDEX_CREATE_PARTITIONED:
 *			create a partitioned index (table must be partitioned)
 * constr_flags: flags passed to index_constraint_create
 *		(only if INDEX_CREATE_ADD_CONSTRAINT is set)
 * allow_system_table_mods: allow table to be a system catalog
 * is_internal: if true, post creation hook for new index
 * constraintId: if not NULL, receives OID of created constraint
 *
 * Returns the OID of the created index.
 */
Oid
index_create(Relation heapRelation,
			 const char *indexRelationName,
			 Oid indexRelationId,
			 Oid parentIndexRelid,
			 Oid parentConstraintId,
			 Oid relFileNode,
			 IndexInfo *indexInfo,
			 List *indexColNames,
			 Oid accessMethodObjectId,
			 Oid tableSpaceId,
			 Oid *collationObjectId,
			 Oid *classObjectId,
			 int16 *coloptions,
			 Datum reloptions,
			 bits16 flags,
			 bits16 constr_flags,
			 bool allow_system_table_mods,
			 bool is_internal,
			 Oid *constraintId)
{
	Oid			heapRelationId = RelationGetRelid(heapRelation);
	Relation	pg_class;
	Relation	indexRelation;
	TupleDesc	indexTupDesc;
	bool		shared_relation;
	bool		mapped_relation;
	bool		is_exclusion;
	Oid			namespaceId;
	int			i;
	char		relpersistence;
	bool		isprimary = (flags & INDEX_CREATE_IS_PRIMARY) != 0;
	bool		invalid = (flags & INDEX_CREATE_INVALID) != 0;
	bool		concurrent = (flags & INDEX_CREATE_CONCURRENT) != 0;
	bool		partitioned = (flags & INDEX_CREATE_PARTITIONED) != 0;
	char		relkind;
	TransactionId relfrozenxid;
	MultiXactId relminmxid;

	/* constraint flags can only be set when a constraint is requested */
	Assert((constr_flags == 0) ||
		   ((flags & INDEX_CREATE_ADD_CONSTRAINT) != 0));
	/* partitioned indexes must never be "built" by themselves */
	Assert(!partitioned || (flags & INDEX_CREATE_SKIP_BUILD));

	relkind = partitioned ? RELKIND_PARTITIONED_INDEX : RELKIND_INDEX;
	is_exclusion = (indexInfo->ii_ExclusionOps != NULL);

	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	/*
	 * The index will be in the same namespace as its parent table, and is
	 * shared across databases if and only if the parent is.  Likewise, it
	 * will use the relfilenode map if and only if the parent does; and it
	 * inherits the parent's relpersistence.
	 */
	namespaceId = RelationGetNamespace(heapRelation);
	shared_relation = heapRelation->rd_rel->relisshared;
	mapped_relation = RelationIsMapped(heapRelation);
	relpersistence = heapRelation->rd_rel->relpersistence;

	/*
	 * check parameters
	 */
	if (indexInfo->ii_NumIndexAttrs < 1)
		elog(ERROR, "must index at least one column");

	if (!allow_system_table_mods &&
		IsSystemRelation(heapRelation) &&
		IsNormalProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user-defined indexes on system catalog tables are not supported")));

	/*
	 * Btree text_pattern_ops uses text_eq as the equality operator, which is
	 * fine as long as the collation is deterministic; text_eq then reduces to
	 * bitwise equality and so it is semantically compatible with the other
	 * operators and functions in that opclass.  But with a nondeterministic
	 * collation, text_eq could yield results that are incompatible with the
	 * actual behavior of the index (which is determined by the opclass's
	 * comparison function).  We prevent such problems by refusing creation of
	 * an index with that opclass and a nondeterministic collation.
	 *
	 * The same applies to varchar_pattern_ops and bpchar_pattern_ops.  If we
	 * find more cases, we might decide to create a real mechanism for marking
	 * opclasses as incompatible with nondeterminism; but for now, this small
	 * hack suffices.
	 *
	 * Another solution is to use a special operator, not text_eq, as the
	 * equality opclass member; but that is undesirable because it would
	 * prevent index usage in many queries that work fine today.
	 */
	for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
	{
		Oid			collation = collationObjectId[i];
		Oid			opclass = classObjectId[i];

		if (collation)
		{
			if ((opclass == TEXT_BTREE_PATTERN_OPS_OID ||
				 opclass == VARCHAR_BTREE_PATTERN_OPS_OID ||
				 opclass == BPCHAR_BTREE_PATTERN_OPS_OID) &&
				!get_collation_isdeterministic(collation))
			{
				HeapTuple	classtup;

				classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
				if (!HeapTupleIsValid(classtup))
					elog(ERROR, "cache lookup failed for operator class %u", opclass);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("nondeterministic collations are not supported for operator class \"%s\"",
								NameStr(((Form_pg_opclass) GETSTRUCT(classtup))->opcname))));
				ReleaseSysCache(classtup);
			}
		}
	}

	/*
	 * Concurrent index build on a system catalog is unsafe because we tend to
	 * release locks before committing in catalogs.
	 */
	if (concurrent &&
		IsCatalogRelation(heapRelation))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("concurrent index creation on system catalog tables is not supported")));

	/*
	 * This case is currently not supported.  There's no way to ask for it in
	 * the grammar with CREATE INDEX, but it can happen with REINDEX.
	 */
	if (concurrent && is_exclusion)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("concurrent index creation for exclusion constraints is not supported")));

	/*
	 * We cannot allow indexing a shared relation after initdb (because
	 * there's no way to make the entry in other databases' pg_class).
	 */
	if (shared_relation && !IsBootstrapProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("shared indexes cannot be created after initdb")));

	/*
	 * Shared relations must be in pg_global, too (last-ditch check)
	 */
	if (shared_relation && tableSpaceId != GLOBALTABLESPACE_OID)
		elog(ERROR, "shared relations must be placed in pg_global tablespace");

	/*
	 * Check for duplicate name (both as to the index, and as to the
	 * associated constraint if any).  Such cases would fail on the relevant
	 * catalogs' unique indexes anyway, but we prefer to give a friendlier
	 * error message.
	 */
	if (get_relname_relid(indexRelationName, namespaceId))
	{
		if ((flags & INDEX_CREATE_IF_NOT_EXISTS) != 0)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" already exists, skipping",
							indexRelationName)));
			table_close(pg_class, RowExclusiveLock);
			return InvalidOid;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists",
						indexRelationName)));
	}

	if ((flags & INDEX_CREATE_ADD_CONSTRAINT) != 0 &&
		ConstraintNameIsUsed(CONSTRAINT_RELATION, heapRelationId,
							 indexRelationName))
	{
		/*
		 * INDEX_CREATE_IF_NOT_EXISTS does not apply here, since the
		 * conflicting constraint is not an index.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("constraint \"%s\" for relation \"%s\" already exists",
						indexRelationName, RelationGetRelationName(heapRelation))));
	}

	/*
	 * construct tuple descriptor for index tuples
	 */
	indexTupDesc = ConstructTupleDescriptor(heapRelation,
											indexInfo,
											indexColNames,
											accessMethodObjectId,
											collationObjectId,
											classObjectId);

	/*
	 * Allocate an OID for the index, unless we were told what to use.
	 *
	 * The OID will be the relfilenode as well, so make sure it doesn't
	 * collide with either pg_class OIDs or existing physical files.
	 */
	if (!OidIsValid(indexRelationId))
	{
		/* Use binary-upgrade override for pg_class.oid/relfilenode? */
		if (IsBinaryUpgrade)
		{
			if (!OidIsValid(binary_upgrade_next_index_pg_class_oid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("pg_class index OID value not set when in binary upgrade mode")));

			indexRelationId = binary_upgrade_next_index_pg_class_oid;
			binary_upgrade_next_index_pg_class_oid = InvalidOid;
		}
		else
		{
			indexRelationId =
				GetNewRelFileNode(tableSpaceId, pg_class, relpersistence);
		}
	}

	/*
	 * create the index relation's relcache entry and, if necessary, the
	 * physical disk file. (If we fail further down, it's the smgr's
	 * responsibility to remove the disk file again, if any.)
	 */
	indexRelation = heap_create(indexRelationName,
								namespaceId,
								tableSpaceId,
								indexRelationId,
								relFileNode,
								accessMethodObjectId,
								indexTupDesc,
								relkind,
								relpersistence,
								shared_relation,
								mapped_relation,
								allow_system_table_mods,
								&relfrozenxid,
								&relminmxid);

	Assert(relfrozenxid == InvalidTransactionId);
	Assert(relminmxid == InvalidMultiXactId);
	Assert(indexRelationId == RelationGetRelid(indexRelation));

	/*
	 * Obtain exclusive lock on it.  Although no other transactions can see it
	 * until we commit, this prevents deadlock-risk complaints from lock
	 * manager in cases such as CLUSTER.
	 */
	LockRelation(indexRelation, AccessExclusiveLock);

	/*
	 * Fill in fields of the index's pg_class entry that are not set correctly
	 * by heap_create.
	 *
	 * XXX should have a cleaner way to create cataloged indexes
	 */
	indexRelation->rd_rel->relowner = heapRelation->rd_rel->relowner;
	indexRelation->rd_rel->relam = accessMethodObjectId;
	indexRelation->rd_rel->relispartition = OidIsValid(parentIndexRelid);

	/*
	 * store index's pg_class entry
	 */
	InsertPgClassTuple(pg_class, indexRelation,
					   RelationGetRelid(indexRelation),
					   (Datum) 0,
					   reloptions);

	/* done with pg_class */
	table_close(pg_class, RowExclusiveLock);

	/*
	 * now update the object id's of all the attribute tuple forms in the
	 * index relation's tuple descriptor
	 */
	InitializeAttributeOids(indexRelation,
							indexInfo->ii_NumIndexAttrs,
							indexRelationId);

	/*
	 * append ATTRIBUTE tuples for the index
	 */
	AppendAttributeTuples(indexRelation, indexInfo->ii_NumIndexAttrs,
						  indexInfo->ii_OpclassOptions);

	/* ----------------
	 *	  update pg_index
	 *	  (append INDEX tuple)
	 *
	 *	  Note that this stows away a representation of "predicate".
	 *	  (Or, could define a rule to maintain the predicate) --Nels, Feb '92
	 * ----------------
	 */
	UpdateIndexRelation(indexRelationId, heapRelationId, parentIndexRelid,
						indexInfo,
						collationObjectId, classObjectId, coloptions,
						isprimary, is_exclusion,
						(constr_flags & INDEX_CONSTR_CREATE_DEFERRABLE) == 0,
						!concurrent && !invalid,
						!concurrent);

	/*
	 * Register relcache invalidation on the indexes' heap relation, to
	 * maintain consistency of its index list
	 */
	CacheInvalidateRelcache(heapRelation);

	/* update pg_inherits and the parent's relhassubclass, if needed */
	if (OidIsValid(parentIndexRelid))
	{
		StoreSingleInheritance(indexRelationId, parentIndexRelid, 1);
		SetRelationHasSubclass(parentIndexRelid, true);
	}

	/*
	 * Register constraint and dependencies for the index.
	 *
	 * If the index is from a CONSTRAINT clause, construct a pg_constraint
	 * entry.  The index will be linked to the constraint, which in turn is
	 * linked to the table.  If it's not a CONSTRAINT, we need to make a
	 * dependency directly on the table.
	 *
	 * We don't need a dependency on the namespace, because there'll be an
	 * indirect dependency via our parent table.
	 *
	 * During bootstrap we can't register any dependencies, and we don't try
	 * to make a constraint either.
	 */
	if (!IsBootstrapProcessingMode())
	{
		ObjectAddress myself,
					referenced;

		myself.classId = RelationRelationId;
		myself.objectId = indexRelationId;
		myself.objectSubId = 0;

		if ((flags & INDEX_CREATE_ADD_CONSTRAINT) != 0)
		{
			char		constraintType;
			ObjectAddress localaddr;

			if (isprimary)
				constraintType = CONSTRAINT_PRIMARY;
			else if (indexInfo->ii_Unique)
				constraintType = CONSTRAINT_UNIQUE;
			else if (is_exclusion)
				constraintType = CONSTRAINT_EXCLUSION;
			else
			{
				elog(ERROR, "constraint must be PRIMARY, UNIQUE or EXCLUDE");
				constraintType = 0; /* keep compiler quiet */
			}

			localaddr = index_constraint_create(heapRelation,
												indexRelationId,
												parentConstraintId,
												indexInfo,
												indexRelationName,
												constraintType,
												constr_flags,
												allow_system_table_mods,
												is_internal);
			if (constraintId)
				*constraintId = localaddr.objectId;
		}
		else
		{
			bool		have_simple_col = false;

			/* Create auto dependencies on simply-referenced columns */
			for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
			{
				if (indexInfo->ii_IndexAttrNumbers[i] != 0)
				{
					referenced.classId = RelationRelationId;
					referenced.objectId = heapRelationId;
					referenced.objectSubId = indexInfo->ii_IndexAttrNumbers[i];

					recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

					have_simple_col = true;
				}
			}

			/*
			 * If there are no simply-referenced columns, give the index an
			 * auto dependency on the whole table.  In most cases, this will
			 * be redundant, but it might not be if the index expressions and
			 * predicate contain no Vars or only whole-row Vars.
			 */
			if (!have_simple_col)
			{
				referenced.classId = RelationRelationId;
				referenced.objectId = heapRelationId;
				referenced.objectSubId = 0;

				recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
			}
		}

		/*
		 * If this is an index partition, create partition dependencies on
		 * both the parent index and the table.  (Note: these must be *in
		 * addition to*, not instead of, all other dependencies.  Otherwise
		 * we'll be short some dependencies after DETACH PARTITION.)
		 */
		if (OidIsValid(parentIndexRelid))
		{
			referenced.classId = RelationRelationId;
			referenced.objectId = parentIndexRelid;
			referenced.objectSubId = 0;

			recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_PRI);

			referenced.classId = RelationRelationId;
			referenced.objectId = heapRelationId;
			referenced.objectSubId = 0;

			recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_SEC);
		}

		/* Store dependency on collations */
		/* The default collation is pinned, so don't bother recording it */
		for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
		{
			if (OidIsValid(collationObjectId[i]) &&
				collationObjectId[i] != DEFAULT_COLLATION_OID)
			{
				referenced.classId = CollationRelationId;
				referenced.objectId = collationObjectId[i];
				referenced.objectSubId = 0;

				recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
			}
		}

		/* Store dependency on operator classes */
		for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
		{
			referenced.classId = OperatorClassRelationId;
			referenced.objectId = classObjectId[i];
			referenced.objectSubId = 0;

			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}

		/* Store dependencies on anything mentioned in index expressions */
		if (indexInfo->ii_Expressions)
		{
			recordDependencyOnSingleRelExpr(&myself,
											(Node *) indexInfo->ii_Expressions,
											heapRelationId,
											DEPENDENCY_NORMAL,
											DEPENDENCY_AUTO, false);
		}

		/* Store dependencies on anything mentioned in predicate */
		if (indexInfo->ii_Predicate)
		{
			recordDependencyOnSingleRelExpr(&myself,
											(Node *) indexInfo->ii_Predicate,
											heapRelationId,
											DEPENDENCY_NORMAL,
											DEPENDENCY_AUTO, false);
		}
	}
	else
	{
		/* Bootstrap mode - assert we weren't asked for constraint support */
		Assert((flags & INDEX_CREATE_ADD_CONSTRAINT) == 0);
	}

	/* Post creation hook for new index */
	InvokeObjectPostCreateHookArg(RelationRelationId,
								  indexRelationId, 0, is_internal);

	/*
	 * Advance the command counter so that we can see the newly-entered
	 * catalog tuples for the index.
	 */
	CommandCounterIncrement();

	/*
	 * In bootstrap mode, we have to fill in the index strategy structure with
	 * information from the catalogs.  If we aren't bootstrapping, then the
	 * relcache entry has already been rebuilt thanks to sinval update during
	 * CommandCounterIncrement.
	 */
	if (IsBootstrapProcessingMode())
		RelationInitIndexAccessInfo(indexRelation);
	else
		Assert(indexRelation->rd_indexcxt != NULL);

	indexRelation->rd_index->indnkeyatts = indexInfo->ii_NumIndexKeyAttrs;

	/* Validate opclass-specific options */
	if (indexInfo->ii_OpclassOptions)
		for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
			(void) index_opclass_options(indexRelation, i + 1,
										 indexInfo->ii_OpclassOptions[i],
										 true);

	/*
	 * If this is bootstrap (initdb) time, then we don't actually fill in the
	 * index yet.  We'll be creating more indexes and classes later, so we
	 * delay filling them in until just before we're done with bootstrapping.
	 * Similarly, if the caller specified to skip the build then filling the
	 * index is delayed till later (ALTER TABLE can save work in some cases
	 * with this).  Otherwise, we call the AM routine that constructs the
	 * index.
	 */
	if (IsBootstrapProcessingMode())
	{
		index_register(heapRelationId, indexRelationId, indexInfo);
	}
	else if ((flags & INDEX_CREATE_SKIP_BUILD) != 0)
	{
		/*
		 * Caller is responsible for filling the index later on.  However,
		 * we'd better make sure that the heap relation is correctly marked as
		 * having an index.
		 */
		index_update_stats(heapRelation,
						   true,
						   -1.0);
		/* Make the above update visible */
		CommandCounterIncrement();
	}
	else
	{
		index_build(heapRelation, indexRelation, indexInfo, false, true);
	}

	/*
	 * Close the index; but we keep the lock that we acquired above until end
	 * of transaction.  Closing the heap is caller's responsibility.
	 */
	index_close(indexRelation, NoLock);

	return indexRelationId;
}

/*
 * index_concurrently_create_copy
 *
 * Create concurrently an index based on the definition of the one provided by
 * caller.  The index is inserted into catalogs and needs to be built later
 * on.  This is called during concurrent reindex processing.
 */
Oid
index_concurrently_create_copy(Relation heapRelation, Oid oldIndexId, const char *newName)
{
	Relation	indexRelation;
	IndexInfo  *oldInfo,
			   *newInfo;
	Oid			newIndexId = InvalidOid;
	HeapTuple	indexTuple,
				classTuple;
	Datum		indclassDatum,
				colOptionDatum,
				optionDatum;
	oidvector  *indclass;
	int2vector *indcoloptions;
	bool		isnull;
	List	   *indexColNames = NIL;
	List	   *indexExprs = NIL;
	List	   *indexPreds = NIL;

	indexRelation = index_open(oldIndexId, RowExclusiveLock);

	/* The new index needs some information from the old index */
	oldInfo = BuildIndexInfo(indexRelation);

	/*
	 * Concurrent build of an index with exclusion constraints is not
	 * supported.
	 */
	if (oldInfo->ii_ExclusionOps != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("concurrent index creation for exclusion constraints is not supported")));

	/* Get the array of class and column options IDs from index info */
	indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(oldIndexId));
	if (!HeapTupleIsValid(indexTuple))
		elog(ERROR, "cache lookup failed for index %u", oldIndexId);
	indclassDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
									Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	colOptionDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
									 Anum_pg_index_indoption, &isnull);
	Assert(!isnull);
	indcoloptions = (int2vector *) DatumGetPointer(colOptionDatum);

	/* Fetch options of index if any */
	classTuple = SearchSysCache1(RELOID, oldIndexId);
	if (!HeapTupleIsValid(classTuple))
		elog(ERROR, "cache lookup failed for relation %u", oldIndexId);
	optionDatum = SysCacheGetAttr(RELOID, classTuple,
								  Anum_pg_class_reloptions, &isnull);

	/*
	 * Fetch the list of expressions and predicates directly from the
	 * catalogs.  This cannot rely on the information from IndexInfo of the
	 * old index as these have been flattened for the planner.
	 */
	if (oldInfo->ii_Expressions != NIL)
	{
		Datum		exprDatum;
		char	   *exprString;

		exprDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
									Anum_pg_index_indexprs, &isnull);
		Assert(!isnull);
		exprString = TextDatumGetCString(exprDatum);
		indexExprs = (List *) stringToNode(exprString);
		pfree(exprString);
	}
	if (oldInfo->ii_Predicate != NIL)
	{
		Datum		predDatum;
		char	   *predString;

		predDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
									Anum_pg_index_indpred, &isnull);
		Assert(!isnull);
		predString = TextDatumGetCString(predDatum);
		indexPreds = (List *) stringToNode(predString);

		/* Also convert to implicit-AND format */
		indexPreds = make_ands_implicit((Expr *) indexPreds);
		pfree(predString);
	}

	/*
	 * Build the index information for the new index.  Note that rebuild of
	 * indexes with exclusion constraints is not supported, hence there is no
	 * need to fill all the ii_Exclusion* fields.
	 */
	newInfo = makeIndexInfo(oldInfo->ii_NumIndexAttrs,
							oldInfo->ii_NumIndexKeyAttrs,
							oldInfo->ii_Am,
							indexExprs,
							indexPreds,
							oldInfo->ii_Unique,
							false,	/* not ready for inserts */
							true);

	/*
	 * Extract the list of column names and the column numbers for the new
	 * index information.  All this information will be used for the index
	 * creation.
	 */
	for (int i = 0; i < oldInfo->ii_NumIndexAttrs; i++)
	{
		TupleDesc	indexTupDesc = RelationGetDescr(indexRelation);
		Form_pg_attribute att = TupleDescAttr(indexTupDesc, i);

		indexColNames = lappend(indexColNames, NameStr(att->attname));
		newInfo->ii_IndexAttrNumbers[i] = oldInfo->ii_IndexAttrNumbers[i];
	}

	/* Extract opclass parameters for each attribute, if any */
	if (oldInfo->ii_OpclassOptions != NULL)
	{
		newInfo->ii_OpclassOptions = palloc0(sizeof(Datum) *
											 newInfo->ii_NumIndexAttrs);
		for (int i = 0; i < newInfo->ii_NumIndexAttrs; i++)
			newInfo->ii_OpclassOptions[i] = get_attoptions(oldIndexId, i + 1);
	}

	/*
	 * Now create the new index.
	 *
	 * For a partition index, we adjust the partition dependency later, to
	 * ensure a consistent state at all times.  That is why parentIndexRelid
	 * is not set here.
	 */
	newIndexId = index_create(heapRelation,
							  newName,
							  InvalidOid,	/* indexRelationId */
							  InvalidOid,	/* parentIndexRelid */
							  InvalidOid,	/* parentConstraintId */
							  InvalidOid,	/* relFileNode */
							  newInfo,
							  indexColNames,
							  indexRelation->rd_rel->relam,
							  indexRelation->rd_rel->reltablespace,
							  indexRelation->rd_indcollation,
							  indclass->values,
							  indcoloptions->values,
							  optionDatum,
							  INDEX_CREATE_SKIP_BUILD | INDEX_CREATE_CONCURRENT,
							  0,
							  true, /* allow table to be a system catalog? */
							  false,	/* is_internal? */
							  NULL);

	/* Close the relations used and clean up */
	index_close(indexRelation, NoLock);
	ReleaseSysCache(indexTuple);
	ReleaseSysCache(classTuple);

	return newIndexId;
}

/*
 * index_concurrently_build
 *
 * Build index for a concurrent operation.  Low-level locks are taken when
 * this operation is performed to prevent only schema changes, but they need
 * to be kept until the end of the transaction performing this operation.
 * 'indexOid' refers to an index relation OID already created as part of
 * previous processing, and 'heapOid' refers to its parent heap relation.
 */
void
index_concurrently_build(Oid heapRelationId,
						 Oid indexRelationId)
{
	Relation	heapRel;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	Relation	indexRelation;
	IndexInfo  *indexInfo;

	/* This had better make sure that a snapshot is active */
	Assert(ActiveSnapshotSet());

	/* Open and lock the parent heap relation */
	heapRel = table_open(heapRelationId, ShareUpdateExclusiveLock);

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(heapRel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	indexRelation = index_open(indexRelationId, RowExclusiveLock);

	/*
	 * We have to re-build the IndexInfo struct, since it was lost in the
	 * commit of the transaction where this concurrent index was created at
	 * the catalog level.
	 */
	indexInfo = BuildIndexInfo(indexRelation);
	Assert(!indexInfo->ii_ReadyForInserts);
	indexInfo->ii_Concurrent = true;
	indexInfo->ii_BrokenHotChain = false;

	/* Now build the index */
	index_build(heapRel, indexRelation, indexInfo, false, true);

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* Close both the relations, but keep the locks */
	table_close(heapRel, NoLock);
	index_close(indexRelation, NoLock);

	/*
	 * Update the pg_index row to mark the index as ready for inserts. Once we
	 * commit this transaction, any new transactions that open the table must
	 * insert new entries into the index for insertions and non-HOT updates.
	 */
	index_set_state_flags(indexRelationId, INDEX_CREATE_SET_READY);
}

/*
 * index_concurrently_swap
 *
 * Swap name, dependencies, and constraints of the old index over to the new
 * index, while marking the old index as invalid and the new as valid.
 */
void
index_concurrently_swap(Oid newIndexId, Oid oldIndexId, const char *oldName)
{
	Relation	pg_class,
				pg_index,
				pg_constraint,
				pg_trigger;
	Relation	oldClassRel,
				newClassRel;
	HeapTuple	oldClassTuple,
				newClassTuple;
	Form_pg_class oldClassForm,
				newClassForm;
	HeapTuple	oldIndexTuple,
				newIndexTuple;
	Form_pg_index oldIndexForm,
				newIndexForm;
	bool		isPartition;
	Oid			indexConstraintOid;
	List	   *constraintOids = NIL;
	ListCell   *lc;

	/*
	 * Take a necessary lock on the old and new index before swapping them.
	 */
	oldClassRel = relation_open(oldIndexId, ShareUpdateExclusiveLock);
	newClassRel = relation_open(newIndexId, ShareUpdateExclusiveLock);

	/* Now swap names and dependencies of those indexes */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	oldClassTuple = SearchSysCacheCopy1(RELOID,
										ObjectIdGetDatum(oldIndexId));
	if (!HeapTupleIsValid(oldClassTuple))
		elog(ERROR, "could not find tuple for relation %u", oldIndexId);
	newClassTuple = SearchSysCacheCopy1(RELOID,
										ObjectIdGetDatum(newIndexId));
	if (!HeapTupleIsValid(newClassTuple))
		elog(ERROR, "could not find tuple for relation %u", newIndexId);

	oldClassForm = (Form_pg_class) GETSTRUCT(oldClassTuple);
	newClassForm = (Form_pg_class) GETSTRUCT(newClassTuple);

	/* Swap the names */
	namestrcpy(&newClassForm->relname, NameStr(oldClassForm->relname));
	namestrcpy(&oldClassForm->relname, oldName);

	/* Swap the partition flags to track inheritance properly */
	isPartition = newClassForm->relispartition;
	newClassForm->relispartition = oldClassForm->relispartition;
	oldClassForm->relispartition = isPartition;

	CatalogTupleUpdate(pg_class, &oldClassTuple->t_self, oldClassTuple);
	CatalogTupleUpdate(pg_class, &newClassTuple->t_self, newClassTuple);

	heap_freetuple(oldClassTuple);
	heap_freetuple(newClassTuple);

	/* Now swap index info */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);

	oldIndexTuple = SearchSysCacheCopy1(INDEXRELID,
										ObjectIdGetDatum(oldIndexId));
	if (!HeapTupleIsValid(oldIndexTuple))
		elog(ERROR, "could not find tuple for relation %u", oldIndexId);
	newIndexTuple = SearchSysCacheCopy1(INDEXRELID,
										ObjectIdGetDatum(newIndexId));
	if (!HeapTupleIsValid(newIndexTuple))
		elog(ERROR, "could not find tuple for relation %u", newIndexId);

	oldIndexForm = (Form_pg_index) GETSTRUCT(oldIndexTuple);
	newIndexForm = (Form_pg_index) GETSTRUCT(newIndexTuple);

	/*
	 * Copy constraint flags from the old index. This is safe because the old
	 * index guaranteed uniqueness.
	 */
	newIndexForm->indisprimary = oldIndexForm->indisprimary;
	oldIndexForm->indisprimary = false;
	newIndexForm->indisexclusion = oldIndexForm->indisexclusion;
	oldIndexForm->indisexclusion = false;
	newIndexForm->indimmediate = oldIndexForm->indimmediate;
	oldIndexForm->indimmediate = true;

	/* Preserve indisreplident in the new index */
	newIndexForm->indisreplident = oldIndexForm->indisreplident;

	/* Preserve indisclustered in the new index */
	newIndexForm->indisclustered = oldIndexForm->indisclustered;

	/*
	 * Mark the new index as valid, and the old index as invalid similarly to
	 * what index_set_state_flags() does.
	 */
	newIndexForm->indisvalid = true;
	oldIndexForm->indisvalid = false;
	oldIndexForm->indisclustered = false;
	oldIndexForm->indisreplident = false;

	CatalogTupleUpdate(pg_index, &oldIndexTuple->t_self, oldIndexTuple);
	CatalogTupleUpdate(pg_index, &newIndexTuple->t_self, newIndexTuple);

	heap_freetuple(oldIndexTuple);
	heap_freetuple(newIndexTuple);

	/*
	 * Move constraints and triggers over to the new index
	 */

	constraintOids = get_index_ref_constraints(oldIndexId);

	indexConstraintOid = get_index_constraint(oldIndexId);

	if (OidIsValid(indexConstraintOid))
		constraintOids = lappend_oid(constraintOids, indexConstraintOid);

	pg_constraint = table_open(ConstraintRelationId, RowExclusiveLock);
	pg_trigger = table_open(TriggerRelationId, RowExclusiveLock);

	foreach(lc, constraintOids)
	{
		HeapTuple	constraintTuple,
					triggerTuple;
		Form_pg_constraint conForm;
		ScanKeyData key[1];
		SysScanDesc scan;
		Oid			constraintOid = lfirst_oid(lc);

		/* Move the constraint from the old to the new index */
		constraintTuple = SearchSysCacheCopy1(CONSTROID,
											  ObjectIdGetDatum(constraintOid));
		if (!HeapTupleIsValid(constraintTuple))
			elog(ERROR, "could not find tuple for constraint %u", constraintOid);

		conForm = ((Form_pg_constraint) GETSTRUCT(constraintTuple));

		if (conForm->conindid == oldIndexId)
		{
			conForm->conindid = newIndexId;

			CatalogTupleUpdate(pg_constraint, &constraintTuple->t_self, constraintTuple);
		}

		heap_freetuple(constraintTuple);

		/* Search for trigger records */
		ScanKeyInit(&key[0],
					Anum_pg_trigger_tgconstraint,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(constraintOid));

		scan = systable_beginscan(pg_trigger, TriggerConstraintIndexId, true,
								  NULL, 1, key);

		while (HeapTupleIsValid((triggerTuple = systable_getnext(scan))))
		{
			Form_pg_trigger tgForm = (Form_pg_trigger) GETSTRUCT(triggerTuple);

			if (tgForm->tgconstrindid != oldIndexId)
				continue;

			/* Make a modifiable copy */
			triggerTuple = heap_copytuple(triggerTuple);
			tgForm = (Form_pg_trigger) GETSTRUCT(triggerTuple);

			tgForm->tgconstrindid = newIndexId;

			CatalogTupleUpdate(pg_trigger, &triggerTuple->t_self, triggerTuple);

			heap_freetuple(triggerTuple);
		}

		systable_endscan(scan);
	}

	/*
	 * Move comment if any
	 */
	{
		Relation	description;
		ScanKeyData skey[3];
		SysScanDesc sd;
		HeapTuple	tuple;
		Datum		values[Natts_pg_description] = {0};
		bool		nulls[Natts_pg_description] = {0};
		bool		replaces[Natts_pg_description] = {0};

		values[Anum_pg_description_objoid - 1] = ObjectIdGetDatum(newIndexId);
		replaces[Anum_pg_description_objoid - 1] = true;

		ScanKeyInit(&skey[0],
					Anum_pg_description_objoid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(oldIndexId));
		ScanKeyInit(&skey[1],
					Anum_pg_description_classoid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationRelationId));
		ScanKeyInit(&skey[2],
					Anum_pg_description_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(0));

		description = table_open(DescriptionRelationId, RowExclusiveLock);

		sd = systable_beginscan(description, DescriptionObjIndexId, true,
								NULL, 3, skey);

		while ((tuple = systable_getnext(sd)) != NULL)
		{
			tuple = heap_modify_tuple(tuple, RelationGetDescr(description),
									  values, nulls, replaces);
			CatalogTupleUpdate(description, &tuple->t_self, tuple);

			break;				/* Assume there can be only one match */
		}

		systable_endscan(sd);
		table_close(description, NoLock);
	}

	/*
	 * Swap inheritance relationship with parent index
	 */
	if (get_rel_relispartition(oldIndexId))
	{
		List	   *ancestors = get_partition_ancestors(oldIndexId);
		Oid			parentIndexRelid = linitial_oid(ancestors);

		DeleteInheritsTuple(oldIndexId, parentIndexRelid);
		StoreSingleInheritance(newIndexId, parentIndexRelid, 1);

		list_free(ancestors);
	}

	/*
	 * Swap all dependencies of and on the old index to the new one, and
	 * vice-versa.  Note that a call to CommandCounterIncrement() would cause
	 * duplicate entries in pg_depend, so this should not be done.
	 */
	changeDependenciesOf(RelationRelationId, newIndexId, oldIndexId);
	changeDependenciesOn(RelationRelationId, newIndexId, oldIndexId);

	changeDependenciesOf(RelationRelationId, oldIndexId, newIndexId);
	changeDependenciesOn(RelationRelationId, oldIndexId, newIndexId);

	/*
	 * Copy over statistics from old to new index
	 */
	{
		PgStat_StatTabEntry *tabentry;

		tabentry = pgstat_fetch_stat_tabentry(oldIndexId);
		if (tabentry)
		{
			if (newClassRel->pgstat_info)
			{
				newClassRel->pgstat_info->t_counts.t_numscans = tabentry->numscans;
				newClassRel->pgstat_info->t_counts.t_tuples_returned = tabentry->tuples_returned;
				newClassRel->pgstat_info->t_counts.t_tuples_fetched = tabentry->tuples_fetched;
				newClassRel->pgstat_info->t_counts.t_blocks_fetched = tabentry->blocks_fetched;
				newClassRel->pgstat_info->t_counts.t_blocks_hit = tabentry->blocks_hit;

				/*
				 * The data will be sent by the next pgstat_report_stat()
				 * call.
				 */
			}
		}
	}

	/* Copy data of pg_statistic from the old index to the new one */
	CopyStatistics(oldIndexId, newIndexId);

	/* Copy pg_attribute.attstattarget for each index attribute */
	{
		HeapTuple	attrTuple;
		Relation	pg_attribute;
		SysScanDesc scan;
		ScanKeyData key[1];

		pg_attribute = table_open(AttributeRelationId, RowExclusiveLock);
		ScanKeyInit(&key[0],
					Anum_pg_attribute_attrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(newIndexId));
		scan = systable_beginscan(pg_attribute, AttributeRelidNumIndexId,
								  true, NULL, 1, key);

		while (HeapTupleIsValid((attrTuple = systable_getnext(scan))))
		{
			Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attrTuple);
			Datum		repl_val[Natts_pg_attribute];
			bool		repl_null[Natts_pg_attribute];
			bool		repl_repl[Natts_pg_attribute];
			int			attstattarget;
			HeapTuple	newTuple;

			/* Ignore dropped columns */
			if (att->attisdropped)
				continue;

			/*
			 * Get attstattarget from the old index and refresh the new value.
			 */
			attstattarget = get_attstattarget(oldIndexId, att->attnum);

			/* no need for a refresh if both match */
			if (attstattarget == att->attstattarget)
				continue;

			memset(repl_val, 0, sizeof(repl_val));
			memset(repl_null, false, sizeof(repl_null));
			memset(repl_repl, false, sizeof(repl_repl));

			repl_repl[Anum_pg_attribute_attstattarget - 1] = true;
			repl_val[Anum_pg_attribute_attstattarget - 1] = Int32GetDatum(attstattarget);

			newTuple = heap_modify_tuple(attrTuple,
										 RelationGetDescr(pg_attribute),
										 repl_val, repl_null, repl_repl);
			CatalogTupleUpdate(pg_attribute, &newTuple->t_self, newTuple);

			heap_freetuple(newTuple);
		}

		systable_endscan(scan);
		table_close(pg_attribute, RowExclusiveLock);
	}

	/* Close relations */
	table_close(pg_class, RowExclusiveLock);
	table_close(pg_index, RowExclusiveLock);
	table_close(pg_constraint, RowExclusiveLock);
	table_close(pg_trigger, RowExclusiveLock);

	/* The lock taken previously is not released until the end of transaction */
	relation_close(oldClassRel, NoLock);
	relation_close(newClassRel, NoLock);
}

/*
 * index_concurrently_set_dead
 *
 * Perform the last invalidation stage of DROP INDEX CONCURRENTLY or REINDEX
 * CONCURRENTLY before actually dropping the index.  After calling this
 * function, the index is seen by all the backends as dead.  Low-level locks
 * taken here are kept until the end of the transaction calling this function.
 */
void
index_concurrently_set_dead(Oid heapId, Oid indexId)
{
	Relation	userHeapRelation;
	Relation	userIndexRelation;

	/*
	 * No more predicate locks will be acquired on this index, and we're about
	 * to stop doing inserts into the index which could show conflicts with
	 * existing predicate locks, so now is the time to move them to the heap
	 * relation.
	 */
	userHeapRelation = table_open(heapId, ShareUpdateExclusiveLock);
	userIndexRelation = index_open(indexId, ShareUpdateExclusiveLock);
	TransferPredicateLocksToHeapRelation(userIndexRelation);

	/*
	 * Now we are sure that nobody uses the index for queries; they just might
	 * have it open for updating it.  So now we can unset indisready and
	 * indislive, then wait till nobody could be using it at all anymore.
	 */
	index_set_state_flags(indexId, INDEX_DROP_SET_DEAD);

	/*
	 * Invalidate the relcache for the table, so that after this commit all
	 * sessions will refresh the table's index list.  Forgetting just the
	 * index's relcache entry is not enough.
	 */
	CacheInvalidateRelcache(userHeapRelation);

	/*
	 * Close the relations again, though still holding session lock.
	 */
	table_close(userHeapRelation, NoLock);
	index_close(userIndexRelation, NoLock);
}

/*
 * index_constraint_create
 *
 * Set up a constraint associated with an index.  Return the new constraint's
 * address.
 *
 * heapRelation: table owning the index (must be suitably locked by caller)
 * indexRelationId: OID of the index
 * parentConstraintId: if constraint is on a partition, the OID of the
 *		constraint in the parent.
 * indexInfo: same info executor uses to insert into the index
 * constraintName: what it say (generally, should match name of index)
 * constraintType: one of CONSTRAINT_PRIMARY, CONSTRAINT_UNIQUE, or
 *		CONSTRAINT_EXCLUSION
 * flags: bitmask that can include any combination of these bits:
 *		INDEX_CONSTR_CREATE_MARK_AS_PRIMARY: index is a PRIMARY KEY
 *		INDEX_CONSTR_CREATE_DEFERRABLE: constraint is DEFERRABLE
 *		INDEX_CONSTR_CREATE_INIT_DEFERRED: constraint is INITIALLY DEFERRED
 *		INDEX_CONSTR_CREATE_UPDATE_INDEX: update the pg_index row
 *		INDEX_CONSTR_CREATE_REMOVE_OLD_DEPS: remove existing dependencies
 *			of index on table's columns
 * allow_system_table_mods: allow table to be a system catalog
 * is_internal: index is constructed due to internal process
 */
ObjectAddress
index_constraint_create(Relation heapRelation,
						Oid indexRelationId,
						Oid parentConstraintId,
						IndexInfo *indexInfo,
						const char *constraintName,
						char constraintType,
						bits16 constr_flags,
						bool allow_system_table_mods,
						bool is_internal)
{
	Oid			namespaceId = RelationGetNamespace(heapRelation);
	ObjectAddress myself,
				idxaddr;
	Oid			conOid;
	bool		deferrable;
	bool		initdeferred;
	bool		mark_as_primary;
	bool		islocal;
	bool		noinherit;
	int			inhcount;

	deferrable = (constr_flags & INDEX_CONSTR_CREATE_DEFERRABLE) != 0;
	initdeferred = (constr_flags & INDEX_CONSTR_CREATE_INIT_DEFERRED) != 0;
	mark_as_primary = (constr_flags & INDEX_CONSTR_CREATE_MARK_AS_PRIMARY) != 0;

	/* constraint creation support doesn't work while bootstrapping */
	Assert(!IsBootstrapProcessingMode());

	/* enforce system-table restriction */
	if (!allow_system_table_mods &&
		IsSystemRelation(heapRelation) &&
		IsNormalProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user-defined indexes on system catalog tables are not supported")));

	/* primary/unique constraints shouldn't have any expressions */
	if (indexInfo->ii_Expressions &&
		constraintType != CONSTRAINT_EXCLUSION)
		elog(ERROR, "constraints cannot have index expressions");

	/*
	 * If we're manufacturing a constraint for a pre-existing index, we need
	 * to get rid of the existing auto dependencies for the index (the ones
	 * that index_create() would have made instead of calling this function).
	 *
	 * Note: this code would not necessarily do the right thing if the index
	 * has any expressions or predicate, but we'd never be turning such an
	 * index into a UNIQUE or PRIMARY KEY constraint.
	 */
	if (constr_flags & INDEX_CONSTR_CREATE_REMOVE_OLD_DEPS)
		deleteDependencyRecordsForClass(RelationRelationId, indexRelationId,
										RelationRelationId, DEPENDENCY_AUTO);

	if (OidIsValid(parentConstraintId))
	{
		islocal = false;
		inhcount = 1;
		noinherit = false;
	}
	else
	{
		islocal = true;
		inhcount = 0;
		noinherit = true;
	}

	/*
	 * Construct a pg_constraint entry.
	 */
	conOid = CreateConstraintEntry(constraintName,
								   namespaceId,
								   constraintType,
								   deferrable,
								   initdeferred,
								   true,
								   parentConstraintId,
								   RelationGetRelid(heapRelation),
								   indexInfo->ii_IndexAttrNumbers,
								   indexInfo->ii_NumIndexKeyAttrs,
								   indexInfo->ii_NumIndexAttrs,
								   InvalidOid,	/* no domain */
								   indexRelationId, /* index OID */
								   InvalidOid,	/* no foreign key */
								   NULL,
								   NULL,
								   NULL,
								   NULL,
								   0,
								   ' ',
								   ' ',
								   ' ',
								   indexInfo->ii_ExclusionOps,
								   NULL,	/* no check constraint */
								   NULL,
								   islocal,
								   inhcount,
								   noinherit,
								   is_internal);

	/*
	 * Register the index as internally dependent on the constraint.
	 *
	 * Note that the constraint has a dependency on the table, so we don't
	 * need (or want) any direct dependency from the index to the table.
	 */
	ObjectAddressSet(myself, ConstraintRelationId, conOid);
	ObjectAddressSet(idxaddr, RelationRelationId, indexRelationId);
	recordDependencyOn(&idxaddr, &myself, DEPENDENCY_INTERNAL);

	/*
	 * Also, if this is a constraint on a partition, give it partition-type
	 * dependencies on the parent constraint as well as the table.
	 */
	if (OidIsValid(parentConstraintId))
	{
		ObjectAddress referenced;

		ObjectAddressSet(referenced, ConstraintRelationId, parentConstraintId);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_PRI);
		ObjectAddressSet(referenced, RelationRelationId,
						 RelationGetRelid(heapRelation));
		recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_SEC);
	}

	/*
	 * If the constraint is deferrable, create the deferred uniqueness
	 * checking trigger.  (The trigger will be given an internal dependency on
	 * the constraint by CreateTrigger.)
	 */
	if (deferrable)
	{
		CreateTrigStmt *trigger;

		trigger = makeNode(CreateTrigStmt);
		trigger->trigname = (constraintType == CONSTRAINT_PRIMARY) ?
			"PK_ConstraintTrigger" :
			"Unique_ConstraintTrigger";
		trigger->relation = NULL;
		trigger->funcname = SystemFuncName("unique_key_recheck");
		trigger->args = NIL;
		trigger->row = true;
		trigger->timing = TRIGGER_TYPE_AFTER;
		trigger->events = TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE;
		trigger->columns = NIL;
		trigger->whenClause = NULL;
		trigger->isconstraint = true;
		trigger->deferrable = true;
		trigger->initdeferred = initdeferred;
		trigger->constrrel = NULL;

		(void) CreateTrigger(trigger, NULL, RelationGetRelid(heapRelation),
							 InvalidOid, conOid, indexRelationId, InvalidOid,
							 InvalidOid, NULL, true, false);
	}

	/*
	 * If needed, mark the index as primary and/or deferred in pg_index.
	 *
	 * Note: When making an existing index into a constraint, caller must have
	 * a table lock that prevents concurrent table updates; otherwise, there
	 * is a risk that concurrent readers of the table will miss seeing this
	 * index at all.
	 */
	if ((constr_flags & INDEX_CONSTR_CREATE_UPDATE_INDEX) &&
		(mark_as_primary || deferrable))
	{
		Relation	pg_index;
		HeapTuple	indexTuple;
		Form_pg_index indexForm;
		bool		dirty = false;
		bool		marked_as_primary = false;

		pg_index = table_open(IndexRelationId, RowExclusiveLock);

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(indexRelationId));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexRelationId);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		if (mark_as_primary && !indexForm->indisprimary)
		{
			indexForm->indisprimary = true;
			dirty = true;
			marked_as_primary = true;
		}

		if (deferrable && indexForm->indimmediate)
		{
			indexForm->indimmediate = false;
			dirty = true;
		}

		if (dirty)
		{
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);

			/*
			 * When we mark an existing index as primary, force a relcache
			 * flush on its parent table, so that all sessions will become
			 * aware that the table now has a primary key.  This is important
			 * because it affects some replication behaviors.
			 */
			if (marked_as_primary)
				CacheInvalidateRelcache(heapRelation);

			InvokeObjectPostAlterHookArg(IndexRelationId, indexRelationId, 0,
										 InvalidOid, is_internal);
		}

		heap_freetuple(indexTuple);
		table_close(pg_index, RowExclusiveLock);
	}

	return myself;
}

/*
 *		index_drop
 *
 * NOTE: this routine should now only be called through performDeletion(),
 * else associated dependencies won't be cleaned up.
 *
 * If concurrent is true, do a DROP INDEX CONCURRENTLY.  If concurrent is
 * false but concurrent_lock_mode is true, then do a normal DROP INDEX but
 * take a lock for CONCURRENTLY processing.  That is used as part of REINDEX
 * CONCURRENTLY.
 */
void
index_drop(Oid indexId, bool concurrent, bool concurrent_lock_mode)
{
	Oid			heapId;
	Relation	userHeapRelation;
	Relation	userIndexRelation;
	Relation	indexRelation;
	HeapTuple	tuple;
	bool		hasexprs;
	LockRelId	heaprelid,
				indexrelid;
	LOCKTAG		heaplocktag;
	LOCKMODE	lockmode;

	/*
	 * A temporary relation uses a non-concurrent DROP.  Other backends can't
	 * access a temporary relation, so there's no harm in grabbing a stronger
	 * lock (see comments in RemoveRelations), and a non-concurrent DROP is
	 * more efficient.
	 */
	Assert(get_rel_persistence(indexId) != RELPERSISTENCE_TEMP ||
		   (!concurrent && !concurrent_lock_mode));

	/*
	 * To drop an index safely, we must grab exclusive lock on its parent
	 * table.  Exclusive lock on the index alone is insufficient because
	 * another backend might be about to execute a query on the parent table.
	 * If it relies on a previously cached list of index OIDs, then it could
	 * attempt to access the just-dropped index.  We must therefore take a
	 * table lock strong enough to prevent all queries on the table from
	 * proceeding until we commit and send out a shared-cache-inval notice
	 * that will make them update their index lists.
	 *
	 * In the concurrent case we avoid this requirement by disabling index use
	 * in multiple steps and waiting out any transactions that might be using
	 * the index, so we don't need exclusive lock on the parent table. Instead
	 * we take ShareUpdateExclusiveLock, to ensure that two sessions aren't
	 * doing CREATE/DROP INDEX CONCURRENTLY on the same index.  (We will get
	 * AccessExclusiveLock on the index below, once we're sure nobody else is
	 * using it.)
	 */
	heapId = IndexGetRelation(indexId, false);
	lockmode = (concurrent || concurrent_lock_mode) ? ShareUpdateExclusiveLock : AccessExclusiveLock;
	userHeapRelation = table_open(heapId, lockmode);
	userIndexRelation = index_open(indexId, lockmode);

	/*
	 * We might still have open queries using it in our own session, which the
	 * above locking won't prevent, so test explicitly.
	 */
	CheckTableNotInUse(userIndexRelation, "DROP INDEX");

	/*
	 * Drop Index Concurrently is more or less the reverse process of Create
	 * Index Concurrently.
	 *
	 * First we unset indisvalid so queries starting afterwards don't use the
	 * index to answer queries anymore.  We have to keep indisready = true so
	 * transactions that are still scanning the index can continue to see
	 * valid index contents.  For instance, if they are using READ COMMITTED
	 * mode, and another transaction makes changes and commits, they need to
	 * see those new tuples in the index.
	 *
	 * After all transactions that could possibly have used the index for
	 * queries end, we can unset indisready and indislive, then wait till
	 * nobody could be touching it anymore.  (Note: we need indislive because
	 * this state must be distinct from the initial state during CREATE INDEX
	 * CONCURRENTLY, which has indislive true while indisready and indisvalid
	 * are false.  That's because in that state, transactions must examine the
	 * index for HOT-safety decisions, while in this state we don't want them
	 * to open it at all.)
	 *
	 * Since all predicate locks on the index are about to be made invalid, we
	 * must promote them to predicate locks on the heap.  In the
	 * non-concurrent case we can just do that now.  In the concurrent case
	 * it's a bit trickier.  The predicate locks must be moved when there are
	 * no index scans in progress on the index and no more can subsequently
	 * start, so that no new predicate locks can be made on the index.  Also,
	 * they must be moved before heap inserts stop maintaining the index, else
	 * the conflict with the predicate lock on the index gap could be missed
	 * before the lock on the heap relation is in place to detect a conflict
	 * based on the heap tuple insert.
	 */
	if (concurrent)
	{
		/*
		 * We must commit our transaction in order to make the first pg_index
		 * state update visible to other sessions.  If the DROP machinery has
		 * already performed any other actions (removal of other objects,
		 * pg_depend entries, etc), the commit would make those actions
		 * permanent, which would leave us with inconsistent catalog state if
		 * we fail partway through the following sequence.  Since DROP INDEX
		 * CONCURRENTLY is restricted to dropping just one index that has no
		 * dependencies, we should get here before anything's been done ---
		 * but let's check that to be sure.  We can verify that the current
		 * transaction has not executed any transactional updates by checking
		 * that no XID has been assigned.
		 */
		if (GetTopTransactionIdIfAny() != InvalidTransactionId)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DROP INDEX CONCURRENTLY must be first action in transaction")));

		/*
		 * Mark index invalid by updating its pg_index entry
		 */
		index_set_state_flags(indexId, INDEX_DROP_CLEAR_VALID);

		/*
		 * Invalidate the relcache for the table, so that after this commit
		 * all sessions will refresh any cached plans that might reference the
		 * index.
		 */
		CacheInvalidateRelcache(userHeapRelation);

		/* save lockrelid and locktag for below, then close but keep locks */
		heaprelid = userHeapRelation->rd_lockInfo.lockRelId;
		SET_LOCKTAG_RELATION(heaplocktag, heaprelid.dbId, heaprelid.relId);
		indexrelid = userIndexRelation->rd_lockInfo.lockRelId;

		table_close(userHeapRelation, NoLock);
		index_close(userIndexRelation, NoLock);

		/*
		 * We must commit our current transaction so that the indisvalid
		 * update becomes visible to other transactions; then start another.
		 * Note that any previously-built data structures are lost in the
		 * commit.  The only data we keep past here are the relation IDs.
		 *
		 * Before committing, get a session-level lock on the table, to ensure
		 * that neither it nor the index can be dropped before we finish. This
		 * cannot block, even if someone else is waiting for access, because
		 * we already have the same lock within our transaction.
		 */
		LockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);
		LockRelationIdForSession(&indexrelid, ShareUpdateExclusiveLock);

		PopActiveSnapshot();
		CommitTransactionCommand();
		StartTransactionCommand();

		/*
		 * Now we must wait until no running transaction could be using the
		 * index for a query.  Use AccessExclusiveLock here to check for
		 * running transactions that hold locks of any kind on the table. Note
		 * we do not need to worry about xacts that open the table for reading
		 * after this point; they will see the index as invalid when they open
		 * the relation.
		 *
		 * Note: the reason we use actual lock acquisition here, rather than
		 * just checking the ProcArray and sleeping, is that deadlock is
		 * possible if one of the transactions in question is blocked trying
		 * to acquire an exclusive lock on our table.  The lock code will
		 * detect deadlock and error out properly.
		 *
		 * Note: we report progress through WaitForLockers() unconditionally
		 * here, even though it will only be used when we're called by REINDEX
		 * CONCURRENTLY and not when called by DROP INDEX CONCURRENTLY.
		 */
		WaitForLockers(heaplocktag, AccessExclusiveLock, true);

		/* Finish invalidation of index and mark it as dead */
		index_concurrently_set_dead(heapId, indexId);

		/*
		 * Again, commit the transaction to make the pg_index update visible
		 * to other sessions.
		 */
		CommitTransactionCommand();
		StartTransactionCommand();

		/*
		 * Wait till every transaction that saw the old index state has
		 * finished.  See above about progress reporting.
		 */
		WaitForLockers(heaplocktag, AccessExclusiveLock, true);

		/*
		 * Re-open relations to allow us to complete our actions.
		 *
		 * At this point, nothing should be accessing the index, but lets
		 * leave nothing to chance and grab AccessExclusiveLock on the index
		 * before the physical deletion.
		 */
		userHeapRelation = table_open(heapId, ShareUpdateExclusiveLock);
		userIndexRelation = index_open(indexId, AccessExclusiveLock);
	}
	else
	{
		/* Not concurrent, so just transfer predicate locks and we're good */
		TransferPredicateLocksToHeapRelation(userIndexRelation);
	}

	/*
	 * Schedule physical removal of the files (if any)
	 */
	if (userIndexRelation->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		RelationDropStorage(userIndexRelation);

	/*
	 * Close and flush the index's relcache entry, to ensure relcache doesn't
	 * try to rebuild it while we're deleting catalog entries. We keep the
	 * lock though.
	 */
	index_close(userIndexRelation, NoLock);

	RelationForgetRelation(indexId);

	/*
	 * fix INDEX relation, and check for expressional index
	 */
	indexRelation = table_open(IndexRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", indexId);

	hasexprs = !heap_attisnull(tuple, Anum_pg_index_indexprs,
							   RelationGetDescr(indexRelation));

	CatalogTupleDelete(indexRelation, &tuple->t_self);

	ReleaseSysCache(tuple);
	table_close(indexRelation, RowExclusiveLock);

	/*
	 * if it has any expression columns, we might have stored statistics about
	 * them.
	 */
	if (hasexprs)
		RemoveStatistics(indexId, 0);

	/*
	 * fix ATTRIBUTE relation
	 */
	DeleteAttributeTuples(indexId);

	/*
	 * fix RELATION relation
	 */
	DeleteRelationTuple(indexId);

	/*
	 * fix INHERITS relation
	 */
	DeleteInheritsTuple(indexId, InvalidOid);

	/*
	 * We are presently too lazy to attempt to compute the new correct value
	 * of relhasindex (the next VACUUM will fix it if necessary). So there is
	 * no need to update the pg_class tuple for the owning relation. But we
	 * must send out a shared-cache-inval notice on the owning relation to
	 * ensure other backends update their relcache lists of indexes.  (In the
	 * concurrent case, this is redundant but harmless.)
	 */
	CacheInvalidateRelcache(userHeapRelation);

	/*
	 * Close owning rel, but keep lock
	 */
	table_close(userHeapRelation, NoLock);

	/*
	 * Release the session locks before we go.
	 */
	if (concurrent)
	{
		UnlockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);
		UnlockRelationIdForSession(&indexrelid, ShareUpdateExclusiveLock);
	}
}

/* ----------------------------------------------------------------
 *						index_build support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		BuildIndexInfo
 *			Construct an IndexInfo record for an open index
 *
 * IndexInfo stores the information about the index that's needed by
 * FormIndexDatum, which is used for both index_build() and later insertion
 * of individual index tuples.  Normally we build an IndexInfo for an index
 * just once per command, and then use it for (potentially) many tuples.
 * ----------------
 */
IndexInfo *
BuildIndexInfo(Relation index)
{
	IndexInfo  *ii;
	Form_pg_index indexStruct = index->rd_index;
	int			i;
	int			numAtts;

	/* check the number of keys, and copy attr numbers into the IndexInfo */
	numAtts = indexStruct->indnatts;
	if (numAtts < 1 || numAtts > INDEX_MAX_KEYS)
		elog(ERROR, "invalid indnatts %d for index %u",
			 numAtts, RelationGetRelid(index));

	/*
	 * Create the node, fetching any expressions needed for expressional
	 * indexes and index predicate if any.
	 */
	ii = makeIndexInfo(indexStruct->indnatts,
					   indexStruct->indnkeyatts,
					   index->rd_rel->relam,
					   RelationGetIndexExpressions(index),
					   RelationGetIndexPredicate(index),
					   indexStruct->indisunique,
					   indexStruct->indisready,
					   false);

	/* fill in attribute numbers */
	for (i = 0; i < numAtts; i++)
		ii->ii_IndexAttrNumbers[i] = indexStruct->indkey.values[i];

	/* fetch exclusion constraint info if any */
	if (indexStruct->indisexclusion)
	{
		RelationGetExclusionInfo(index,
								 &ii->ii_ExclusionOps,
								 &ii->ii_ExclusionProcs,
								 &ii->ii_ExclusionStrats);
	}

	ii->ii_OpclassOptions = RelationGetIndexRawAttOptions(index);

	return ii;
}

/* ----------------
 *		BuildDummyIndexInfo
 *			Construct a dummy IndexInfo record for an open index
 *
 * This differs from the real BuildIndexInfo in that it will never run any
 * user-defined code that might exist in index expressions or predicates.
 * Instead of the real index expressions, we return null constants that have
 * the right types/typmods/collations.  Predicates and exclusion clauses are
 * just ignored.  This is sufficient for the purpose of truncating an index,
 * since we will not need to actually evaluate the expressions or predicates;
 * the only thing that's likely to be done with the data is construction of
 * a tupdesc describing the index's rowtype.
 * ----------------
 */
IndexInfo *
BuildDummyIndexInfo(Relation index)
{
	IndexInfo  *ii;
	Form_pg_index indexStruct = index->rd_index;
	int			i;
	int			numAtts;

	/* check the number of keys, and copy attr numbers into the IndexInfo */
	numAtts = indexStruct->indnatts;
	if (numAtts < 1 || numAtts > INDEX_MAX_KEYS)
		elog(ERROR, "invalid indnatts %d for index %u",
			 numAtts, RelationGetRelid(index));

	/*
	 * Create the node, using dummy index expressions, and pretending there is
	 * no predicate.
	 */
	ii = makeIndexInfo(indexStruct->indnatts,
					   indexStruct->indnkeyatts,
					   index->rd_rel->relam,
					   RelationGetDummyIndexExpressions(index),
					   NIL,
					   indexStruct->indisunique,
					   indexStruct->indisready,
					   false);

	/* fill in attribute numbers */
	for (i = 0; i < numAtts; i++)
		ii->ii_IndexAttrNumbers[i] = indexStruct->indkey.values[i];

	/* We ignore the exclusion constraint if any */

	return ii;
}

/*
 * CompareIndexInfo
 *		Return whether the properties of two indexes (in different tables)
 *		indicate that they have the "same" definitions.
 *
 * Note: passing collations and opfamilies separately is a kludge.  Adding
 * them to IndexInfo may result in better coding here and elsewhere.
 *
 * Use build_attrmap_by_name(index2, index1) to build the attmap.
 */
bool
CompareIndexInfo(IndexInfo *info1, IndexInfo *info2,
				 Oid *collations1, Oid *collations2,
				 Oid *opfamilies1, Oid *opfamilies2,
				 AttrMap *attmap)
{
	int			i;

	if (info1->ii_Unique != info2->ii_Unique)
		return false;

	/* indexes are only equivalent if they have the same access method */
	if (info1->ii_Am != info2->ii_Am)
		return false;

	/* and same number of attributes */
	if (info1->ii_NumIndexAttrs != info2->ii_NumIndexAttrs)
		return false;

	/* and same number of key attributes */
	if (info1->ii_NumIndexKeyAttrs != info2->ii_NumIndexKeyAttrs)
		return false;

	/*
	 * and columns match through the attribute map (actual attribute numbers
	 * might differ!)  Note that this implies that index columns that are
	 * expressions appear in the same positions.  We will next compare the
	 * expressions themselves.
	 */
	for (i = 0; i < info1->ii_NumIndexAttrs; i++)
	{
		if (attmap->maplen < info2->ii_IndexAttrNumbers[i])
			elog(ERROR, "incorrect attribute map");

		/* ignore expressions at this stage */
		if ((info1->ii_IndexAttrNumbers[i] != InvalidAttrNumber) &&
			(attmap->attnums[info2->ii_IndexAttrNumbers[i] - 1] !=
			 info1->ii_IndexAttrNumbers[i]))
			return false;

		/* collation and opfamily is not valid for including columns */
		if (i >= info1->ii_NumIndexKeyAttrs)
			continue;

		if (collations1[i] != collations2[i])
			return false;
		if (opfamilies1[i] != opfamilies2[i])
			return false;
	}

	/*
	 * For expression indexes: either both are expression indexes, or neither
	 * is; if they are, make sure the expressions match.
	 */
	if ((info1->ii_Expressions != NIL) != (info2->ii_Expressions != NIL))
		return false;
	if (info1->ii_Expressions != NIL)
	{
		bool		found_whole_row;
		Node	   *mapped;

		mapped = map_variable_attnos((Node *) info2->ii_Expressions,
									 1, 0, attmap,
									 InvalidOid, &found_whole_row);
		if (found_whole_row)
		{
			/*
			 * we could throw an error here, but seems out of scope for this
			 * routine.
			 */
			return false;
		}

		if (!equal(info1->ii_Expressions, mapped))
			return false;
	}

	/* Partial index predicates must be identical, if they exist */
	if ((info1->ii_Predicate == NULL) != (info2->ii_Predicate == NULL))
		return false;
	if (info1->ii_Predicate != NULL)
	{
		bool		found_whole_row;
		Node	   *mapped;

		mapped = map_variable_attnos((Node *) info2->ii_Predicate,
									 1, 0, attmap,
									 InvalidOid, &found_whole_row);
		if (found_whole_row)
		{
			/*
			 * we could throw an error here, but seems out of scope for this
			 * routine.
			 */
			return false;
		}
		if (!equal(info1->ii_Predicate, mapped))
			return false;
	}

	/* No support currently for comparing exclusion indexes. */
	if (info1->ii_ExclusionOps != NULL || info2->ii_ExclusionOps != NULL)
		return false;

	return true;
}

/* ----------------
 *		BuildSpeculativeIndexInfo
 *			Add extra state to IndexInfo record
 *
 * For unique indexes, we usually don't want to add info to the IndexInfo for
 * checking uniqueness, since the B-Tree AM handles that directly.  However,
 * in the case of speculative insertion, additional support is required.
 *
 * Do this processing here rather than in BuildIndexInfo() to not incur the
 * overhead in the common non-speculative cases.
 * ----------------
 */
void
BuildSpeculativeIndexInfo(Relation index, IndexInfo *ii)
{
	int			indnkeyatts;
	int			i;

	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);

	/*
	 * fetch info for checking unique indexes
	 */
	Assert(ii->ii_Unique);

	if (index->rd_rel->relam != BTREE_AM_OID)
		elog(ERROR, "unexpected non-btree speculative unique index");

	ii->ii_UniqueOps = (Oid *) palloc(sizeof(Oid) * indnkeyatts);
	ii->ii_UniqueProcs = (Oid *) palloc(sizeof(Oid) * indnkeyatts);
	ii->ii_UniqueStrats = (uint16 *) palloc(sizeof(uint16) * indnkeyatts);

	/*
	 * We have to look up the operator's strategy number.  This provides a
	 * cross-check that the operator does match the index.
	 */
	/* We need the func OIDs and strategy numbers too */
	for (i = 0; i < indnkeyatts; i++)
	{
		ii->ii_UniqueStrats[i] = BTEqualStrategyNumber;
		ii->ii_UniqueOps[i] =
			get_opfamily_member(index->rd_opfamily[i],
								index->rd_opcintype[i],
								index->rd_opcintype[i],
								ii->ii_UniqueStrats[i]);
		if (!OidIsValid(ii->ii_UniqueOps[i]))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 ii->ii_UniqueStrats[i], index->rd_opcintype[i],
				 index->rd_opcintype[i], index->rd_opfamily[i]);
		ii->ii_UniqueProcs[i] = get_opcode(ii->ii_UniqueOps[i]);
	}
}

/* ----------------
 *		FormIndexDatum
 *			Construct values[] and isnull[] arrays for a new index tuple.
 *
 *	indexInfo		Info about the index
 *	slot			Heap tuple for which we must prepare an index entry
 *	estate			executor state for evaluating any index expressions
 *	values			Array of index Datums (output area)
 *	isnull			Array of is-null indicators (output area)
 *
 * When there are no index expressions, estate may be NULL.  Otherwise it
 * must be supplied, *and* the ecxt_scantuple slot of its per-tuple expr
 * context must point to the heap tuple passed in.
 *
 * Notice we don't actually call index_form_tuple() here; we just prepare
 * its input arrays values[] and isnull[].  This is because the index AM
 * may wish to alter the data before storage.
 * ----------------
 */
void
FormIndexDatum(IndexInfo *indexInfo,
			   TupleTableSlot *slot,
			   EState *estate,
			   Datum *values,
			   bool *isnull)
{
	ListCell   *indexpr_item;
	int			i;

	if (indexInfo->ii_Expressions != NIL &&
		indexInfo->ii_ExpressionsState == NIL)
	{
		/* First time through, set up expression evaluation state */
		indexInfo->ii_ExpressionsState =
			ExecPrepareExprList(indexInfo->ii_Expressions, estate);
		/* Check caller has set up context correctly */
		Assert(GetPerTupleExprContext(estate)->ecxt_scantuple == slot);
	}
	indexpr_item = list_head(indexInfo->ii_ExpressionsState);

	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
	{
		int			keycol = indexInfo->ii_IndexAttrNumbers[i];
		Datum		iDatum;
		bool		isNull;

		if (keycol < 0)
			iDatum = slot_getsysattr(slot, keycol, &isNull);
		else if (keycol != 0)
		{
			/*
			 * Plain index column; get the value we need directly from the
			 * heap tuple.
			 */
			iDatum = slot_getattr(slot, keycol, &isNull);
		}
		else
		{
			/*
			 * Index expression --- need to evaluate it.
			 */
			if (indexpr_item == NULL)
				elog(ERROR, "wrong number of index expressions");
			iDatum = ExecEvalExprSwitchContext((ExprState *) lfirst(indexpr_item),
											   GetPerTupleExprContext(estate),
											   &isNull);
			indexpr_item = lnext(indexInfo->ii_ExpressionsState, indexpr_item);
		}
		values[i] = iDatum;
		isnull[i] = isNull;
	}

	if (indexpr_item != NULL)
		elog(ERROR, "wrong number of index expressions");
}


/*
 * index_update_stats --- update pg_class entry after CREATE INDEX or REINDEX
 *
 * This routine updates the pg_class row of either an index or its parent
 * relation after CREATE INDEX or REINDEX.  Its rather bizarre API is designed
 * to ensure we can do all the necessary work in just one update.
 *
 * hasindex: set relhasindex to this value
 * reltuples: if >= 0, set reltuples to this value; else no change
 *
 * If reltuples >= 0, relpages and relallvisible are also updated (using
 * RelationGetNumberOfBlocks() and visibilitymap_count()).
 *
 * NOTE: an important side-effect of this operation is that an SI invalidation
 * message is sent out to all backends --- including me --- causing relcache
 * entries to be flushed or updated with the new data.  This must happen even
 * if we find that no change is needed in the pg_class row.  When updating
 * a heap entry, this ensures that other backends find out about the new
 * index.  When updating an index, it's important because some index AMs
 * expect a relcache flush to occur after REINDEX.
 */
static void
index_update_stats(Relation rel,
				   bool hasindex,
				   double reltuples)
{
	Oid			relid = RelationGetRelid(rel);
	Relation	pg_class;
	HeapTuple	tuple;
	Form_pg_class rd_rel;
	bool		dirty;

	/*
	 * We always update the pg_class row using a non-transactional,
	 * overwrite-in-place update.  There are several reasons for this:
	 *
	 * 1. In bootstrap mode, we have no choice --- UPDATE wouldn't work.
	 *
	 * 2. We could be reindexing pg_class itself, in which case we can't move
	 * its pg_class row because CatalogTupleInsert/CatalogTupleUpdate might
	 * not know about all the indexes yet (see reindex_relation).
	 *
	 * 3. Because we execute CREATE INDEX with just share lock on the parent
	 * rel (to allow concurrent index creations), an ordinary update could
	 * suffer a tuple-concurrently-updated failure against another CREATE
	 * INDEX committing at about the same time.  We can avoid that by having
	 * them both do nontransactional updates (we assume they will both be
	 * trying to change the pg_class row to the same thing, so it doesn't
	 * matter which goes first).
	 *
	 * It is safe to use a non-transactional update even though our
	 * transaction could still fail before committing.  Setting relhasindex
	 * true is safe even if there are no indexes (VACUUM will eventually fix
	 * it).  And of course the new relpages and reltuples counts are correct
	 * regardless.  However, we don't want to change relpages (or
	 * relallvisible) if the caller isn't providing an updated reltuples
	 * count, because that would bollix the reltuples/relpages ratio which is
	 * what's really important.
	 */

	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	/*
	 * Make a copy of the tuple to update.  Normally we use the syscache, but
	 * we can't rely on that during bootstrap or while reindexing pg_class
	 * itself.
	 */
	if (IsBootstrapProcessingMode() ||
		ReindexIsProcessingHeap(RelationRelationId))
	{
		/* don't assume syscache will work */
		TableScanDesc pg_class_scan;
		ScanKeyData key[1];

		ScanKeyInit(&key[0],
					Anum_pg_class_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));

		pg_class_scan = table_beginscan_catalog(pg_class, 1, key);
		tuple = heap_getnext(pg_class_scan, ForwardScanDirection);
		tuple = heap_copytuple(tuple);
		table_endscan(pg_class_scan);
	}
	else
	{
		/* normal case, use syscache */
		tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	}

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for relation %u", relid);
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/* Should this be a more comprehensive test? */
	Assert(rd_rel->relkind != RELKIND_PARTITIONED_INDEX);

	/* Apply required updates, if any, to copied tuple */

	dirty = false;
	if (rd_rel->relhasindex != hasindex)
	{
		rd_rel->relhasindex = hasindex;
		dirty = true;
	}

	if (reltuples >= 0)
	{
		BlockNumber relpages = RelationGetNumberOfBlocks(rel);
		BlockNumber relallvisible;

		if (rd_rel->relkind != RELKIND_INDEX)
			visibilitymap_count(rel, &relallvisible, NULL);
		else					/* don't bother for indexes */
			relallvisible = 0;

		if (rd_rel->relpages != (int32) relpages)
		{
			rd_rel->relpages = (int32) relpages;
			dirty = true;
		}
		if (rd_rel->reltuples != (float4) reltuples)
		{
			rd_rel->reltuples = (float4) reltuples;
			dirty = true;
		}
		if (rd_rel->relallvisible != (int32) relallvisible)
		{
			rd_rel->relallvisible = (int32) relallvisible;
			dirty = true;
		}
	}

	/*
	 * If anything changed, write out the tuple
	 */
	if (dirty)
	{
		heap_inplace_update(pg_class, tuple);
		/* the above sends a cache inval message */
	}
	else
	{
		/* no need to change tuple, but force relcache inval anyway */
		CacheInvalidateRelcacheByTuple(tuple);
	}

	heap_freetuple(tuple);

	table_close(pg_class, RowExclusiveLock);
}


/*
 * index_build - invoke access-method-specific index build procedure
 *
 * On entry, the index's catalog entries are valid, and its physical disk
 * file has been created but is empty.  We call the AM-specific build
 * procedure to fill in the index contents.  We then update the pg_class
 * entries of the index and heap relation as needed, using statistics
 * returned by ambuild as well as data passed by the caller.
 *
 * isreindex indicates we are recreating a previously-existing index.
 * parallel indicates if parallelism may be useful.
 *
 * Note: before Postgres 8.2, the passed-in heap and index Relations
 * were automatically closed by this routine.  This is no longer the case.
 * The caller opened 'em, and the caller should close 'em.
 */
void
index_build(Relation heapRelation,
			Relation indexRelation,
			IndexInfo *indexInfo,
			bool isreindex,
			bool parallel)
{
	IndexBuildResult *stats;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	/*
	 * sanity checks
	 */
	Assert(RelationIsValid(indexRelation));
	Assert(PointerIsValid(indexRelation->rd_indam));
	Assert(PointerIsValid(indexRelation->rd_indam->ambuild));
	Assert(PointerIsValid(indexRelation->rd_indam->ambuildempty));

	/*
	 * Determine worker process details for parallel CREATE INDEX.  Currently,
	 * only btree has support for parallel builds.
	 *
	 * Note that planner considers parallel safety for us.
	 */
	if (parallel && IsNormalProcessingMode() &&
		indexRelation->rd_rel->relam == BTREE_AM_OID)
		indexInfo->ii_ParallelWorkers =
			plan_create_index_workers(RelationGetRelid(heapRelation),
									  RelationGetRelid(indexRelation));

	if (indexInfo->ii_ParallelWorkers == 0)
		ereport(DEBUG1,
				(errmsg("building index \"%s\" on table \"%s\" serially",
						RelationGetRelationName(indexRelation),
						RelationGetRelationName(heapRelation))));
	else
		ereport(DEBUG1,
				(errmsg_plural("building index \"%s\" on table \"%s\" with request for %d parallel worker",
							   "building index \"%s\" on table \"%s\" with request for %d parallel workers",
							   indexInfo->ii_ParallelWorkers,
							   RelationGetRelationName(indexRelation),
							   RelationGetRelationName(heapRelation),
							   indexInfo->ii_ParallelWorkers)));

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(heapRelation->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	/* Set up initial progress report status */
	{
		const int	index[] = {
			PROGRESS_CREATEIDX_PHASE,
			PROGRESS_CREATEIDX_SUBPHASE,
			PROGRESS_CREATEIDX_TUPLES_DONE,
			PROGRESS_CREATEIDX_TUPLES_TOTAL,
			PROGRESS_SCAN_BLOCKS_DONE,
			PROGRESS_SCAN_BLOCKS_TOTAL
		};
		const int64 val[] = {
			PROGRESS_CREATEIDX_PHASE_BUILD,
			PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE,
			0, 0, 0, 0
		};

		pgstat_progress_update_multi_param(6, index, val);
	}

	/*
	 * Call the access method's build procedure
	 */
	stats = indexRelation->rd_indam->ambuild(heapRelation, indexRelation,
											 indexInfo);
	Assert(PointerIsValid(stats));

	/*
	 * If this is an unlogged index, we may need to write out an init fork for
	 * it -- but we must first check whether one already exists.  If, for
	 * example, an unlogged relation is truncated in the transaction that
	 * created it, or truncated twice in a subsequent transaction, the
	 * relfilenode won't change, and nothing needs to be done here.
	 */
	if (indexRelation->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
		!smgrexists(RelationGetSmgr(indexRelation), INIT_FORKNUM))
	{
		smgrcreate(RelationGetSmgr(indexRelation), INIT_FORKNUM, false);
		log_smgrcreate(&indexRelation->rd_node, INIT_FORKNUM);
		indexRelation->rd_indam->ambuildempty(indexRelation);
	}

	/*
	 * If we found any potentially broken HOT chains, mark the index as not
	 * being usable until the current transaction is below the event horizon.
	 * See src/backend/access/heap/README.HOT for discussion.  Also set this
	 * if early pruning/vacuuming is enabled for the heap relation.  While it
	 * might become safe to use the index earlier based on actual cleanup
	 * activity and other active transactions, the test for that would be much
	 * more complex and would require some form of blocking, so keep it simple
	 * and fast by just using the current transaction.
	 *
	 * However, when reindexing an existing index, we should do nothing here.
	 * Any HOT chains that are broken with respect to the index must predate
	 * the index's original creation, so there is no need to change the
	 * index's usability horizon.  Moreover, we *must not* try to change the
	 * index's pg_index entry while reindexing pg_index itself, and this
	 * optimization nicely prevents that.  The more complex rules needed for a
	 * reindex are handled separately after this function returns.
	 *
	 * We also need not set indcheckxmin during a concurrent index build,
	 * because we won't set indisvalid true until all transactions that care
	 * about the broken HOT chains or early pruning/vacuuming are gone.
	 *
	 * Therefore, this code path can only be taken during non-concurrent
	 * CREATE INDEX.  Thus the fact that heap_update will set the pg_index
	 * tuple's xmin doesn't matter, because that tuple was created in the
	 * current transaction anyway.  That also means we don't need to worry
	 * about any concurrent readers of the tuple; no other transaction can see
	 * it yet.
	 */
	if ((indexInfo->ii_BrokenHotChain || EarlyPruningEnabled(heapRelation)) &&
		!isreindex &&
		!indexInfo->ii_Concurrent)
	{
		Oid			indexId = RelationGetRelid(indexRelation);
		Relation	pg_index;
		HeapTuple	indexTuple;
		Form_pg_index indexForm;

		pg_index = table_open(IndexRelationId, RowExclusiveLock);

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(indexId));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexId);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		/* If it's a new index, indcheckxmin shouldn't be set ... */
		Assert(!indexForm->indcheckxmin);

		indexForm->indcheckxmin = true;
		CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);

		heap_freetuple(indexTuple);
		table_close(pg_index, RowExclusiveLock);
	}

	/*
	 * Update heap and index pg_class rows
	 */
	index_update_stats(heapRelation,
					   true,
					   stats->heap_tuples);

	index_update_stats(indexRelation,
					   false,
					   stats->index_tuples);

	/* Make the updated catalog row versions visible */
	CommandCounterIncrement();

	/*
	 * If it's for an exclusion constraint, make a second pass over the heap
	 * to verify that the constraint is satisfied.  We must not do this until
	 * the index is fully valid.  (Broken HOT chains shouldn't matter, though;
	 * see comments for IndexCheckExclusion.)
	 */
	if (indexInfo->ii_ExclusionOps != NULL)
		IndexCheckExclusion(heapRelation, indexRelation, indexInfo);

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);
}

/*
 * IndexCheckExclusion - verify that a new exclusion constraint is satisfied
 *
 * When creating an exclusion constraint, we first build the index normally
 * and then rescan the heap to check for conflicts.  We assume that we only
 * need to validate tuples that are live according to an up-to-date snapshot,
 * and that these were correctly indexed even in the presence of broken HOT
 * chains.  This should be OK since we are holding at least ShareLock on the
 * table, meaning there can be no uncommitted updates from other transactions.
 * (Note: that wouldn't necessarily work for system catalogs, since many
 * operations release write lock early on the system catalogs.)
 */
static void
IndexCheckExclusion(Relation heapRelation,
					Relation indexRelation,
					IndexInfo *indexInfo)
{
	TableScanDesc scan;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;

	/*
	 * If we are reindexing the target index, mark it as no longer being
	 * reindexed, to forestall an Assert in index_beginscan when we try to use
	 * the index for probes.  This is OK because the index is now fully valid.
	 */
	if (ReindexIsCurrentlyProcessingIndex(RelationGetRelid(indexRelation)))
		ResetReindexProcessing();

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heapRelation, NULL);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * Scan all live tuples in the base relation.
	 */
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan_strat(heapRelation,	/* relation */
								 snapshot,	/* snapshot */
								 0, /* number of keys */
								 NULL,	/* scan key */
								 true,	/* buffer access strategy OK */
								 true); /* syncscan OK */

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * In a partial index, ignore tuples that don't satisfy the predicate.
		 */
		if (predicate != NULL)
		{
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * Extract index column values, including computing expressions.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/*
		 * Check that this tuple has no conflicts.
		 */
		check_exclusion_constraint(heapRelation,
								   indexRelation, indexInfo,
								   &(slot->tts_tid), values, isnull,
								   estate, true);

		MemoryContextReset(econtext->ecxt_per_tuple_memory);
	}

	table_endscan(scan);
	UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}


/*
 * validate_index - support code for concurrent index builds
 *
 * We do a concurrent index build by first inserting the catalog entry for the
 * index via index_create(), marking it not indisready and not indisvalid.
 * Then we commit our transaction and start a new one, then we wait for all
 * transactions that could have been modifying the table to terminate.  Now
 * we know that any subsequently-started transactions will see the index and
 * honor its constraints on HOT updates; so while existing HOT-chains might
 * be broken with respect to the index, no currently live tuple will have an
 * incompatible HOT update done to it.  We now build the index normally via
 * index_build(), while holding a weak lock that allows concurrent
 * insert/update/delete.  Also, we index only tuples that are valid
 * as of the start of the scan (see table_index_build_scan), whereas a normal
 * build takes care to include recently-dead tuples.  This is OK because
 * we won't mark the index valid until all transactions that might be able
 * to see those tuples are gone.  The reason for doing that is to avoid
 * bogus unique-index failures due to concurrent UPDATEs (we might see
 * different versions of the same row as being valid when we pass over them,
 * if we used HeapTupleSatisfiesVacuum).  This leaves us with an index that
 * does not contain any tuples added to the table while we built the index.
 *
 * Next, we mark the index "indisready" (but still not "indisvalid") and
 * commit the second transaction and start a third.  Again we wait for all
 * transactions that could have been modifying the table to terminate.  Now
 * we know that any subsequently-started transactions will see the index and
 * insert their new tuples into it.  We then take a new reference snapshot
 * which is passed to validate_index().  Any tuples that are valid according
 * to this snap, but are not in the index, must be added to the index.
 * (Any tuples committed live after the snap will be inserted into the
 * index by their originating transaction.  Any tuples committed dead before
 * the snap need not be indexed, because we will wait out all transactions
 * that might care about them before we mark the index valid.)
 *
 * validate_index() works by first gathering all the TIDs currently in the
 * index, using a bulkdelete callback that just stores the TIDs and doesn't
 * ever say "delete it".  (This should be faster than a plain indexscan;
 * also, not all index AMs support full-index indexscan.)  Then we sort the
 * TIDs, and finally scan the table doing a "merge join" against the TID list
 * to see which tuples are missing from the index.  Thus we will ensure that
 * all tuples valid according to the reference snapshot are in the index.
 *
 * Building a unique index this way is tricky: we might try to insert a
 * tuple that is already dead or is in process of being deleted, and we
 * mustn't have a uniqueness failure against an updated version of the same
 * row.  We could try to check the tuple to see if it's already dead and tell
 * index_insert() not to do the uniqueness check, but that still leaves us
 * with a race condition against an in-progress update.  To handle that,
 * we expect the index AM to recheck liveness of the to-be-inserted tuple
 * before it declares a uniqueness error.
 *
 * After completing validate_index(), we wait until all transactions that
 * were alive at the time of the reference snapshot are gone; this is
 * necessary to be sure there are none left with a transaction snapshot
 * older than the reference (and hence possibly able to see tuples we did
 * not index).  Then we mark the index "indisvalid" and commit.  Subsequent
 * transactions will be able to use it for queries.
 *
 * Doing two full table scans is a brute-force strategy.  We could try to be
 * cleverer, eg storing new tuples in a special area of the table (perhaps
 * making the table append-only by setting use_fsm).  However that would
 * add yet more locking issues.
 */
void
validate_index(Oid heapId, Oid indexId, Snapshot snapshot)
{
	Relation	heapRelation,
				indexRelation;
	IndexInfo  *indexInfo;
	IndexVacuumInfo ivinfo;
	ValidateIndexState state;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	{
		const int	index[] = {
			PROGRESS_CREATEIDX_PHASE,
			PROGRESS_CREATEIDX_TUPLES_DONE,
			PROGRESS_CREATEIDX_TUPLES_TOTAL,
			PROGRESS_SCAN_BLOCKS_DONE,
			PROGRESS_SCAN_BLOCKS_TOTAL
		};
		const int64 val[] = {
			PROGRESS_CREATEIDX_PHASE_VALIDATE_IDXSCAN,
			0, 0, 0, 0
		};

		pgstat_progress_update_multi_param(5, index, val);
	}

	/* Open and lock the parent heap relation */
	heapRelation = table_open(heapId, ShareUpdateExclusiveLock);

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(heapRelation->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	indexRelation = index_open(indexId, RowExclusiveLock);

	/*
	 * Fetch info needed for index_insert.  (You might think this should be
	 * passed in from DefineIndex, but its copy is long gone due to having
	 * been built in a previous transaction.)
	 */
	indexInfo = BuildIndexInfo(indexRelation);

	/* mark build is concurrent just for consistency */
	indexInfo->ii_Concurrent = true;

	/*
	 * Scan the index and gather up all the TIDs into a tuplesort object.
	 */
	ivinfo.index = indexRelation;
	ivinfo.analyze_only = false;
	ivinfo.report_progress = true;
	ivinfo.estimated_count = true;
	ivinfo.message_level = DEBUG2;
	ivinfo.num_heap_tuples = heapRelation->rd_rel->reltuples;
	ivinfo.strategy = NULL;

	/*
	 * Encode TIDs as int8 values for the sort, rather than directly sorting
	 * item pointers.  This can be significantly faster, primarily because TID
	 * is a pass-by-reference type on all platforms, whereas int8 is
	 * pass-by-value on most platforms.
	 */
	state.tuplesort = tuplesort_begin_datum(INT8OID, Int8LessOperator,
											InvalidOid, false,
											maintenance_work_mem,
											NULL, false);
	state.htups = state.itups = state.tups_inserted = 0;

	/* ambulkdelete updates progress metrics */
	(void) index_bulk_delete(&ivinfo, NULL,
							 validate_index_callback, (void *) &state);

	/* Execute the sort */
	{
		const int	index[] = {
			PROGRESS_CREATEIDX_PHASE,
			PROGRESS_SCAN_BLOCKS_DONE,
			PROGRESS_SCAN_BLOCKS_TOTAL
		};
		const int64 val[] = {
			PROGRESS_CREATEIDX_PHASE_VALIDATE_SORT,
			0, 0
		};

		pgstat_progress_update_multi_param(3, index, val);
	}
	tuplesort_performsort(state.tuplesort);

	/*
	 * Now scan the heap and "merge" it with the index
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_VALIDATE_TABLESCAN);
	table_index_validate_scan(heapRelation,
							  indexRelation,
							  indexInfo,
							  snapshot,
							  &state);

	/* Done with tuplesort object */
	tuplesort_end(state.tuplesort);

	elog(DEBUG2,
		 "validate_index found %.0f heap tuples, %.0f index tuples; inserted %.0f missing tuples",
		 state.htups, state.itups, state.tups_inserted);

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* Close rels, but keep locks */
	index_close(indexRelation, NoLock);
	table_close(heapRelation, NoLock);
}

/*
 * validate_index_callback - bulkdelete callback to collect the index TIDs
 */
static bool
validate_index_callback(ItemPointer itemptr, void *opaque)
{
	ValidateIndexState *state = (ValidateIndexState *) opaque;
	int64		encoded = itemptr_encode(itemptr);

	tuplesort_putdatum(state->tuplesort, Int64GetDatum(encoded), false);
	state->itups += 1;
	return false;				/* never actually delete anything */
}

/*
 * index_set_state_flags - adjust pg_index state flags
 *
 * This is used during CREATE/DROP INDEX CONCURRENTLY to adjust the pg_index
 * flags that denote the index's state.
 *
 * Note that CatalogTupleUpdate() sends a cache invalidation message for the
 * tuple, so other sessions will hear about the update as soon as we commit.
 */
void
index_set_state_flags(Oid indexId, IndexStateFlagsAction action)
{
	Relation	pg_index;
	HeapTuple	indexTuple;
	Form_pg_index indexForm;

	/* Open pg_index and fetch a writable copy of the index's tuple */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);

	indexTuple = SearchSysCacheCopy1(INDEXRELID,
									 ObjectIdGetDatum(indexId));
	if (!HeapTupleIsValid(indexTuple))
		elog(ERROR, "cache lookup failed for index %u", indexId);
	indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

	/* Perform the requested state change on the copy */
	switch (action)
	{
		case INDEX_CREATE_SET_READY:
			/* Set indisready during a CREATE INDEX CONCURRENTLY sequence */
			Assert(indexForm->indislive);
			Assert(!indexForm->indisready);
			Assert(!indexForm->indisvalid);
			indexForm->indisready = true;
			break;
		case INDEX_CREATE_SET_VALID:
			/* Set indisvalid during a CREATE INDEX CONCURRENTLY sequence */
			Assert(indexForm->indislive);
			Assert(indexForm->indisready);
			Assert(!indexForm->indisvalid);
			indexForm->indisvalid = true;
			break;
		case INDEX_DROP_CLEAR_VALID:

			/*
			 * Clear indisvalid during a DROP INDEX CONCURRENTLY sequence
			 *
			 * If indisready == true we leave it set so the index still gets
			 * maintained by active transactions.  We only need to ensure that
			 * indisvalid is false.  (We don't assert that either is initially
			 * true, though, since we want to be able to retry a DROP INDEX
			 * CONCURRENTLY that failed partway through.)
			 *
			 * Note: the CLUSTER logic assumes that indisclustered cannot be
			 * set on any invalid index, so clear that flag too.  For
			 * cleanliness, also clear indisreplident.
			 */
			indexForm->indisvalid = false;
			indexForm->indisclustered = false;
			indexForm->indisreplident = false;
			break;
		case INDEX_DROP_SET_DEAD:

			/*
			 * Clear indisready/indislive during DROP INDEX CONCURRENTLY
			 *
			 * We clear both indisready and indislive, because we not only
			 * want to stop updates, we want to prevent sessions from touching
			 * the index at all.
			 */
			Assert(!indexForm->indisvalid);
			Assert(!indexForm->indisclustered);
			Assert(!indexForm->indisreplident);
			indexForm->indisready = false;
			indexForm->indislive = false;
			break;
	}

	/* ... and update it */
	CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);

	table_close(pg_index, RowExclusiveLock);
}


/*
 * IndexGetRelation: given an index's relation OID, get the OID of the
 * relation it is an index on.  Uses the system cache.
 */
Oid
IndexGetRelation(Oid indexId, bool missing_ok)
{
	HeapTuple	tuple;
	Form_pg_index index;
	Oid			result;

	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexId));
	if (!HeapTupleIsValid(tuple))
	{
		if (missing_ok)
			return InvalidOid;
		elog(ERROR, "cache lookup failed for index %u", indexId);
	}
	index = (Form_pg_index) GETSTRUCT(tuple);
	Assert(index->indexrelid == indexId);

	result = index->indrelid;
	ReleaseSysCache(tuple);
	return result;
}

/*
 * reindex_index - This routine is used to recreate a single index
 */
void
reindex_index(Oid indexId, bool skip_constraint_checks, char persistence,
			  int options)
{
	Relation	iRel,
				heapRelation;
	Oid			heapId;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	IndexInfo  *indexInfo;
	volatile bool skipped_constraint = false;
	PGRUsage	ru0;
	bool		progress = (options & REINDEXOPT_REPORT_PROGRESS) != 0;

	pg_rusage_init(&ru0);

	/*
	 * Open and lock the parent heap relation.  ShareLock is sufficient since
	 * we only need to be sure no schema or data changes are going on.
	 */
	heapId = IndexGetRelation(indexId, false);
	heapRelation = table_open(heapId, ShareLock);

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(heapRelation->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	if (progress)
	{
		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX,
									  heapId);
		pgstat_progress_update_param(PROGRESS_CREATEIDX_COMMAND,
									 PROGRESS_CREATEIDX_COMMAND_REINDEX);
		pgstat_progress_update_param(PROGRESS_CREATEIDX_INDEX_OID,
									 indexId);
	}

	/*
	 * Open the target index relation and get an exclusive lock on it, to
	 * ensure that no one else is touching this particular index.
	 */
	iRel = index_open(indexId, AccessExclusiveLock);

	if (progress)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_ACCESS_METHOD_OID,
									 iRel->rd_rel->relam);

	/*
	 * The case of reindexing partitioned tables and indexes is handled
	 * differently by upper layers, so this case shouldn't arise.
	 */
	if (iRel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		elog(ERROR, "unsupported relation kind for index \"%s\"",
			 RelationGetRelationName(iRel));

	/*
	 * Don't allow reindex on temp tables of other backends ... their local
	 * buffer manager is not going to cope.
	 */
	if (RELATION_IS_OTHER_TEMP(iRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot reindex temporary tables of other sessions")));

	/*
	 * Don't allow reindex of an invalid index on TOAST table.  This is a
	 * leftover from a failed REINDEX CONCURRENTLY, and if rebuilt it would
	 * not be possible to drop it anymore.
	 */
	if (IsToastNamespace(RelationGetNamespace(iRel)) &&
		!get_index_isvalid(indexId))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot reindex invalid index on TOAST table")));

	/*
	 * Also check for active uses of the index in the current transaction; we
	 * don't want to reindex underneath an open indexscan.
	 */
	CheckTableNotInUse(iRel, "REINDEX INDEX");

	/*
	 * All predicate locks on the index are about to be made invalid. Promote
	 * them to relation locks on the heap.
	 */
	TransferPredicateLocksToHeapRelation(iRel);

	/* Fetch info needed for index_build */
	indexInfo = BuildIndexInfo(iRel);

	/* If requested, skip checking uniqueness/exclusion constraints */
	if (skip_constraint_checks)
	{
		if (indexInfo->ii_Unique || indexInfo->ii_ExclusionOps != NULL)
			skipped_constraint = true;
		indexInfo->ii_Unique = false;
		indexInfo->ii_ExclusionOps = NULL;
		indexInfo->ii_ExclusionProcs = NULL;
		indexInfo->ii_ExclusionStrats = NULL;
	}

	/* Suppress use of the target index while rebuilding it */
	SetReindexProcessing(heapId, indexId);

	/* Create a new physical relation for the index */
	RelationSetNewRelfilenode(iRel, persistence);

	/* Initialize the index and rebuild */
	/* Note: we do not need to re-establish pkey setting */
	index_build(heapRelation, iRel, indexInfo, true, true);

	/* Re-allow use of target index */
	ResetReindexProcessing();

	/*
	 * If the index is marked invalid/not-ready/dead (ie, it's from a failed
	 * CREATE INDEX CONCURRENTLY, or a DROP INDEX CONCURRENTLY failed midway),
	 * and we didn't skip a uniqueness check, we can now mark it valid.  This
	 * allows REINDEX to be used to clean up in such cases.
	 *
	 * We can also reset indcheckxmin, because we have now done a
	 * non-concurrent index build, *except* in the case where index_build
	 * found some still-broken HOT chains. If it did, and we don't have to
	 * change any of the other flags, we just leave indcheckxmin alone (note
	 * that index_build won't have changed it, because this is a reindex).
	 * This is okay and desirable because not updating the tuple leaves the
	 * index's usability horizon (recorded as the tuple's xmin value) the same
	 * as it was.
	 *
	 * But, if the index was invalid/not-ready/dead and there were broken HOT
	 * chains, we had better force indcheckxmin true, because the normal
	 * argument that the HOT chains couldn't conflict with the index is
	 * suspect for an invalid index.  (A conflict is definitely possible if
	 * the index was dead.  It probably shouldn't happen otherwise, but let's
	 * be conservative.)  In this case advancing the usability horizon is
	 * appropriate.
	 *
	 * Another reason for avoiding unnecessary updates here is that while
	 * reindexing pg_index itself, we must not try to update tuples in it.
	 * pg_index's indexes should always have these flags in their clean state,
	 * so that won't happen.
	 *
	 * If early pruning/vacuuming is enabled for the heap relation, the
	 * usability horizon must be advanced to the current transaction on every
	 * build or rebuild.  pg_index is OK in this regard because catalog tables
	 * are not subject to early cleanup.
	 */
	if (!skipped_constraint)
	{
		Relation	pg_index;
		HeapTuple	indexTuple;
		Form_pg_index indexForm;
		bool		index_bad;
		bool		early_pruning_enabled = EarlyPruningEnabled(heapRelation);

		pg_index = table_open(IndexRelationId, RowExclusiveLock);

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(indexId));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexId);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		index_bad = (!indexForm->indisvalid ||
					 !indexForm->indisready ||
					 !indexForm->indislive);
		if (index_bad ||
			(indexForm->indcheckxmin && !indexInfo->ii_BrokenHotChain) ||
			early_pruning_enabled)
		{
			if (!indexInfo->ii_BrokenHotChain && !early_pruning_enabled)
				indexForm->indcheckxmin = false;
			else if (index_bad || early_pruning_enabled)
				indexForm->indcheckxmin = true;
			indexForm->indisvalid = true;
			indexForm->indisready = true;
			indexForm->indislive = true;
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);

			/*
			 * Invalidate the relcache for the table, so that after we commit
			 * all sessions will refresh the table's index list.  This ensures
			 * that if anyone misses seeing the pg_index row during this
			 * update, they'll refresh their list before attempting any update
			 * on the table.
			 */
			CacheInvalidateRelcache(heapRelation);
		}

		table_close(pg_index, RowExclusiveLock);
	}

	/* Log what we did */
	if (options & REINDEXOPT_VERBOSE)
		ereport(INFO,
				(errmsg("index \"%s\" was reindexed",
						get_rel_name(indexId)),
				 errdetail_internal("%s",
									pg_rusage_show(&ru0))));

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* Close rels, but keep locks */
	index_close(iRel, NoLock);
	table_close(heapRelation, NoLock);

	if (progress)
		pgstat_progress_end_command();
}

/*
 * reindex_relation - This routine is used to recreate all indexes
 * of a relation (and optionally its toast relation too, if any).
 *
 * "flags" is a bitmask that can include any combination of these bits:
 *
 * REINDEX_REL_PROCESS_TOAST: if true, process the toast table too (if any).
 *
 * REINDEX_REL_SUPPRESS_INDEX_USE: if true, the relation was just completely
 * rebuilt by an operation such as VACUUM FULL or CLUSTER, and therefore its
 * indexes are inconsistent with it.  This makes things tricky if the relation
 * is a system catalog that we might consult during the reindexing.  To deal
 * with that case, we mark all of the indexes as pending rebuild so that they
 * won't be trusted until rebuilt.  The caller is required to call us *without*
 * having made the rebuilt table visible by doing CommandCounterIncrement;
 * we'll do CCI after having collected the index list.  (This way we can still
 * use catalog indexes while collecting the list.)
 *
 * REINDEX_REL_CHECK_CONSTRAINTS: if true, recheck unique and exclusion
 * constraint conditions, else don't.  To avoid deadlocks, VACUUM FULL or
 * CLUSTER on a system catalog must omit this flag.  REINDEX should be used to
 * rebuild an index if constraint inconsistency is suspected.  For optimal
 * performance, other callers should include the flag only after transforming
 * the data in a manner that risks a change in constraint validity.
 *
 * REINDEX_REL_FORCE_INDEXES_UNLOGGED: if true, set the persistence of the
 * rebuilt indexes to unlogged.
 *
 * REINDEX_REL_FORCE_INDEXES_PERMANENT: if true, set the persistence of the
 * rebuilt indexes to permanent.
 *
 * Returns true if any indexes were rebuilt (including toast table's index
 * when relevant).  Note that a CommandCounterIncrement will occur after each
 * index rebuild.
 */
bool
reindex_relation(Oid relid, int flags, int options)
{
	Relation	rel;
	Oid			toast_relid;
	List	   *indexIds;
	char		persistence;
	bool		result;
	ListCell   *indexId;
	int			i;

	/*
	 * Open and lock the relation.  ShareLock is sufficient since we only need
	 * to prevent schema and data changes in it.  The lock level used here
	 * should match ReindexTable().
	 */
	rel = table_open(relid, ShareLock);

	/*
	 * This may be useful when implemented someday; but that day is not today.
	 * For now, avoid erroring out when called in a multi-table context
	 * (REINDEX SCHEMA) and happen to come across a partitioned table.  The
	 * partitions may be reindexed on their own anyway.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		ereport(WARNING,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("REINDEX of partitioned tables is not yet implemented, skipping \"%s\"",
						RelationGetRelationName(rel))));
		table_close(rel, ShareLock);
		return false;
	}

	toast_relid = rel->rd_rel->reltoastrelid;

	/*
	 * Get the list of index OIDs for this relation.  (We trust to the
	 * relcache to get this with a sequential scan if ignoring system
	 * indexes.)
	 */
	indexIds = RelationGetIndexList(rel);

	if (flags & REINDEX_REL_SUPPRESS_INDEX_USE)
	{
		/* Suppress use of all the indexes until they are rebuilt */
		SetReindexPending(indexIds);

		/*
		 * Make the new heap contents visible --- now things might be
		 * inconsistent!
		 */
		CommandCounterIncrement();
	}

	/*
	 * Compute persistence of indexes: same as that of owning rel, unless
	 * caller specified otherwise.
	 */
	if (flags & REINDEX_REL_FORCE_INDEXES_UNLOGGED)
		persistence = RELPERSISTENCE_UNLOGGED;
	else if (flags & REINDEX_REL_FORCE_INDEXES_PERMANENT)
		persistence = RELPERSISTENCE_PERMANENT;
	else
		persistence = rel->rd_rel->relpersistence;

	/* Reindex all the indexes. */
	i = 1;
	foreach(indexId, indexIds)
	{
		Oid			indexOid = lfirst_oid(indexId);
		Oid			indexNamespaceId = get_rel_namespace(indexOid);

		/*
		 * Skip any invalid indexes on a TOAST table.  These can only be
		 * duplicate leftovers from a failed REINDEX CONCURRENTLY, and if
		 * rebuilt it would not be possible to drop them anymore.
		 */
		if (IsToastNamespace(indexNamespaceId) &&
			!get_index_isvalid(indexOid))
		{
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot reindex invalid index \"%s.%s\" on TOAST table, skipping",
							get_namespace_name(indexNamespaceId),
							get_rel_name(indexOid))));
			continue;
		}

		reindex_index(indexOid, !(flags & REINDEX_REL_CHECK_CONSTRAINTS),
					  persistence, options);

		CommandCounterIncrement();

		/* Index should no longer be in the pending list */
		Assert(!ReindexIsProcessingIndex(indexOid));

		/* Set index rebuild count */
		pgstat_progress_update_param(PROGRESS_CLUSTER_INDEX_REBUILD_COUNT,
									 i);
		i++;
	}

	/*
	 * Close rel, but continue to hold the lock.
	 */
	table_close(rel, NoLock);

	result = (indexIds != NIL);

	/*
	 * If the relation has a secondary toast rel, reindex that too while we
	 * still hold the lock on the master table.
	 */
	if ((flags & REINDEX_REL_PROCESS_TOAST) && OidIsValid(toast_relid))
		result |= reindex_relation(toast_relid, flags, options);

	return result;
}


/* ----------------------------------------------------------------
 *		System index reindexing support
 *
 * When we are busy reindexing a system index, this code provides support
 * for preventing catalog lookups from using that index.  We also make use
 * of this to catch attempted uses of user indexes during reindexing of
 * those indexes.  This information is propagated to parallel workers;
 * attempting to change it during a parallel operation is not permitted.
 * ----------------------------------------------------------------
 */

static Oid	currentlyReindexedHeap = InvalidOid;
static Oid	currentlyReindexedIndex = InvalidOid;
static List *pendingReindexedIndexes = NIL;
static int	reindexingNestLevel = 0;

/*
 * ReindexIsProcessingHeap
 *		True if heap specified by OID is currently being reindexed.
 */
bool
ReindexIsProcessingHeap(Oid heapOid)
{
	return heapOid == currentlyReindexedHeap;
}

/*
 * ReindexIsCurrentlyProcessingIndex
 *		True if index specified by OID is currently being reindexed.
 */
static bool
ReindexIsCurrentlyProcessingIndex(Oid indexOid)
{
	return indexOid == currentlyReindexedIndex;
}

/*
 * ReindexIsProcessingIndex
 *		True if index specified by OID is currently being reindexed,
 *		or should be treated as invalid because it is awaiting reindex.
 */
bool
ReindexIsProcessingIndex(Oid indexOid)
{
	return indexOid == currentlyReindexedIndex ||
		list_member_oid(pendingReindexedIndexes, indexOid);
}

/*
 * SetReindexProcessing
 *		Set flag that specified heap/index are being reindexed.
 */
static void
SetReindexProcessing(Oid heapOid, Oid indexOid)
{
	Assert(OidIsValid(heapOid) && OidIsValid(indexOid));
	/* Reindexing is not re-entrant. */
	if (OidIsValid(currentlyReindexedHeap))
		elog(ERROR, "cannot reindex while reindexing");
	currentlyReindexedHeap = heapOid;
	currentlyReindexedIndex = indexOid;
	/* Index is no longer "pending" reindex. */
	RemoveReindexPending(indexOid);
	/* This may have been set already, but in case it isn't, do so now. */
	reindexingNestLevel = GetCurrentTransactionNestLevel();
}

/*
 * ResetReindexProcessing
 *		Unset reindexing status.
 */
static void
ResetReindexProcessing(void)
{
	currentlyReindexedHeap = InvalidOid;
	currentlyReindexedIndex = InvalidOid;
	/* reindexingNestLevel remains set till end of (sub)transaction */
}

/*
 * SetReindexPending
 *		Mark the given indexes as pending reindex.
 *
 * NB: we assume that the current memory context stays valid throughout.
 */
static void
SetReindexPending(List *indexes)
{
	/* Reindexing is not re-entrant. */
	if (pendingReindexedIndexes)
		elog(ERROR, "cannot reindex while reindexing");
	if (IsInParallelMode())
		elog(ERROR, "cannot modify reindex state during a parallel operation");
	pendingReindexedIndexes = list_copy(indexes);
	reindexingNestLevel = GetCurrentTransactionNestLevel();
}

/*
 * RemoveReindexPending
 *		Remove the given index from the pending list.
 */
static void
RemoveReindexPending(Oid indexOid)
{
	if (IsInParallelMode())
		elog(ERROR, "cannot modify reindex state during a parallel operation");
	pendingReindexedIndexes = list_delete_oid(pendingReindexedIndexes,
											  indexOid);
}

/*
 * ResetReindexState
 *		Clear all reindexing state during (sub)transaction abort.
 */
void
ResetReindexState(int nestLevel)
{
	/*
	 * Because reindexing is not re-entrant, we don't need to cope with nested
	 * reindexing states.  We just need to avoid messing up the outer-level
	 * state in case a subtransaction fails within a REINDEX.  So checking the
	 * current nest level against that of the reindex operation is sufficient.
	 */
	if (reindexingNestLevel >= nestLevel)
	{
		currentlyReindexedHeap = InvalidOid;
		currentlyReindexedIndex = InvalidOid;

		/*
		 * We needn't try to release the contents of pendingReindexedIndexes;
		 * that list should be in a transaction-lifespan context, so it will
		 * go away automatically.
		 */
		pendingReindexedIndexes = NIL;

		reindexingNestLevel = 0;
	}
}

/*
 * EstimateReindexStateSpace
 *		Estimate space needed to pass reindex state to parallel workers.
 */
Size
EstimateReindexStateSpace(void)
{
	return offsetof(SerializedReindexState, pendingReindexedIndexes)
		+ mul_size(sizeof(Oid), list_length(pendingReindexedIndexes));
}

/*
 * SerializeReindexState
 *		Serialize reindex state for parallel workers.
 */
void
SerializeReindexState(Size maxsize, char *start_address)
{
	SerializedReindexState *sistate = (SerializedReindexState *) start_address;
	int			c = 0;
	ListCell   *lc;

	sistate->currentlyReindexedHeap = currentlyReindexedHeap;
	sistate->currentlyReindexedIndex = currentlyReindexedIndex;
	sistate->numPendingReindexedIndexes = list_length(pendingReindexedIndexes);
	foreach(lc, pendingReindexedIndexes)
		sistate->pendingReindexedIndexes[c++] = lfirst_oid(lc);
}

/*
 * RestoreReindexState
 *		Restore reindex state in a parallel worker.
 */
void
RestoreReindexState(void *reindexstate)
{
	SerializedReindexState *sistate = (SerializedReindexState *) reindexstate;
	int			c = 0;
	MemoryContext oldcontext;

	currentlyReindexedHeap = sistate->currentlyReindexedHeap;
	currentlyReindexedIndex = sistate->currentlyReindexedIndex;

	Assert(pendingReindexedIndexes == NIL);
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	for (c = 0; c < sistate->numPendingReindexedIndexes; ++c)
		pendingReindexedIndexes =
			lappend_oid(pendingReindexedIndexes,
						sistate->pendingReindexedIndexes[c]);
	MemoryContextSwitchTo(oldcontext);

	/* Note the worker has its own transaction nesting level */
	reindexingNestLevel = GetCurrentTransactionNestLevel();
}
