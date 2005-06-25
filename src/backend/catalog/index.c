/*-------------------------------------------------------------------------
 *
 * index.c
 *	  code to create and destroy POSTGRES index relations
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/index.c,v 1.219.2.1 2005/06/25 16:54:12 tgl Exp $
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

#include "access/genam.h"
#include "access/heapam.h"
#include "access/istrat.h"
#include "bootstrap/bootstrap.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/*
 * macros used in guessing how many tuples are on a page.
 */
#define AVG_ATTR_SIZE 8
#define NTUPLES_PER_PAGE(natts) \
	((BLCKSZ - MAXALIGN(sizeof(PageHeaderData))) / \
	((natts) * AVG_ATTR_SIZE + MAXALIGN(sizeof(HeapTupleHeaderData))))

/* non-export function prototypes */
static TupleDesc ConstructTupleDescriptor(Relation heapRelation,
						 IndexInfo *indexInfo,
						 Oid *classObjectId);
static void UpdateRelationRelation(Relation indexRelation);
static void InitializeAttributeOids(Relation indexRelation,
						int numatts, Oid indexoid);
static void AppendAttributeTuples(Relation indexRelation, int numatts);
static void UpdateIndexRelation(Oid indexoid, Oid heapoid,
					IndexInfo *indexInfo,
					Oid *classOids,
					bool primary);
static Oid	IndexGetRelation(Oid indexId);


/*
 *		ConstructTupleDescriptor
 *
 * Build an index tuple descriptor for a new index
 */
static TupleDesc
ConstructTupleDescriptor(Relation heapRelation,
						 IndexInfo *indexInfo,
						 Oid *classObjectId)
{
	int			numatts = indexInfo->ii_NumIndexAttrs;
	List	   *indexprs = indexInfo->ii_Expressions;
	TupleDesc	heapTupDesc;
	TupleDesc	indexTupDesc;
	int			natts;			/* #atts in heap rel --- for error checks */
	int			i;

	heapTupDesc = RelationGetDescr(heapRelation);
	natts = RelationGetForm(heapRelation)->relnatts;

	/*
	 * allocate the new tuple descriptor
	 */
	indexTupDesc = CreateTemplateTupleDesc(numatts, false);

	/*
	 * For simple index columns, we copy the pg_attribute row from the
	 * parent relation and modify it as necessary.	For expressions we
	 * have to cons up a pg_attribute row the hard way.
	 */
	for (i = 0; i < numatts; i++)
	{
		AttrNumber	atnum = indexInfo->ii_KeyAttrNumbers[i];
		Form_pg_attribute to;
		HeapTuple	tuple;
		Form_pg_type typeTup;
		Oid			keyType;

		indexTupDesc->attrs[i] = to =
			(Form_pg_attribute) palloc0(ATTRIBUTE_TUPLE_SIZE);

		if (atnum != 0)
		{
			/* Simple index column */
			Form_pg_attribute from;

			if (atnum < 0)
			{
				/*
				 * here we are indexing on a system attribute (-1...-n)
				 */
				from = SystemAttributeDefinition(atnum,
									   heapRelation->rd_rel->relhasoids);
			}
			else
			{
				/*
				 * here we are indexing on a normal attribute (1...n)
				 */
				if (atnum > natts)		/* safety check */
					elog(ERROR, "invalid column number %d", atnum);
				from = heapTupDesc->attrs[AttrNumberGetAttrOffset(atnum)];
			}

			/*
			 * now that we've determined the "from", let's copy the tuple
			 * desc data...
			 */
			memcpy(to, from, ATTRIBUTE_TUPLE_SIZE);

			/*
			 * Fix the stuff that should not be the same as the underlying
			 * attr
			 */
			to->attnum = i + 1;

			to->attstattarget = 0;
			to->attcacheoff = -1;
			to->attnotnull = false;
			to->atthasdef = false;
			to->attislocal = true;
			to->attinhcount = 0;
		}
		else
		{
			/* Expressional index */
			Node	   *indexkey;

			if (indexprs == NIL)	/* shouldn't happen */
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexprs);
			indexprs = lnext(indexprs);

			/*
			 * Make the attribute's name "pg_expresssion_nnn" (maybe think
			 * of something better later)
			 */
			sprintf(NameStr(to->attname), "pg_expression_%d", i + 1);

			/*
			 * Lookup the expression type in pg_type for the type length
			 * etc.
			 */
			keyType = exprType(indexkey);
			tuple = SearchSysCache(TYPEOID,
								   ObjectIdGetDatum(keyType),
								   0, 0, 0);
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for type %u", keyType);
			typeTup = (Form_pg_type) GETSTRUCT(tuple);

			/*
			 * Assign some of the attributes values. Leave the rest as 0.
			 */
			to->attnum = i + 1;
			to->atttypid = keyType;
			to->attlen = typeTup->typlen;
			to->attbyval = typeTup->typbyval;
			to->attstorage = typeTup->typstorage;
			to->attalign = typeTup->typalign;
			to->attcacheoff = -1;
			to->atttypmod = -1;
			to->attislocal = true;

			ReleaseSysCache(tuple);
		}

		/*
		 * We do not yet have the correct relation OID for the index, so
		 * just set it invalid for now.  InitializeAttributeOids() will
		 * fix it later.
		 */
		to->attrelid = InvalidOid;

		/*
		 * Check the opclass to see if it provides a keytype (overriding
		 * the attribute type).
		 */
		tuple = SearchSysCache(CLAOID,
							   ObjectIdGetDatum(classObjectId[i]),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for opclass %u",
				 classObjectId[i]);
		keyType = ((Form_pg_opclass) GETSTRUCT(tuple))->opckeytype;
		ReleaseSysCache(tuple);

		if (OidIsValid(keyType) && keyType != to->atttypid)
		{
			/* index value and heap value have different types */
			tuple = SearchSysCache(TYPEOID,
								   ObjectIdGetDatum(keyType),
								   0, 0, 0);
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

	return indexTupDesc;
}

/* ----------------------------------------------------------------
 *		UpdateRelationRelation
 * ----------------------------------------------------------------
 */
static void
UpdateRelationRelation(Relation indexRelation)
{
	Relation	pg_class;
	HeapTuple	tuple;

	pg_class = heap_openr(RelationRelationName, RowExclusiveLock);

	/* XXX Natts_pg_class_fixed is a hack - see pg_class.h */
	tuple = heap_addheader(Natts_pg_class_fixed,
						   true,
						   CLASS_TUPLE_SIZE,
						   (void *) indexRelation->rd_rel);

	/*
	 * the new tuple must have the oid already chosen for the index. sure
	 * would be embarrassing to do this sort of thing in polite company.
	 */
	HeapTupleSetOid(tuple, RelationGetRelid(indexRelation));
	simple_heap_insert(pg_class, tuple);

	/* update the system catalog indexes */
	CatalogUpdateIndexes(pg_class, tuple);

	heap_freetuple(tuple);
	heap_close(pg_class, RowExclusiveLock);
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
		tupleDescriptor->attrs[i]->attrelid = indexoid;
}

/* ----------------------------------------------------------------
 *		AppendAttributeTuples
 * ----------------------------------------------------------------
 */
static void
AppendAttributeTuples(Relation indexRelation, int numatts)
{
	Relation	pg_attribute;
	CatalogIndexState indstate;
	TupleDesc	indexTupDesc;
	HeapTuple	new_tuple;
	int			i;

	/*
	 * open the attribute relation and its indexes
	 */
	pg_attribute = heap_openr(AttributeRelationName, RowExclusiveLock);

	indstate = CatalogOpenIndexes(pg_attribute);

	/*
	 * insert data from new index's tupdesc into pg_attribute
	 */
	indexTupDesc = RelationGetDescr(indexRelation);

	for (i = 0; i < numatts; i++)
	{
		/*
		 * There used to be very grotty code here to set these fields, but
		 * I think it's unnecessary.  They should be set already.
		 */
		Assert(indexTupDesc->attrs[i]->attnum == i + 1);
		Assert(indexTupDesc->attrs[i]->attcacheoff == -1);

		new_tuple = heap_addheader(Natts_pg_attribute,
								   false,
								   ATTRIBUTE_TUPLE_SIZE,
								   (void *) indexTupDesc->attrs[i]);

		simple_heap_insert(pg_attribute, new_tuple);

		CatalogIndexInsert(indstate, new_tuple);

		heap_freetuple(new_tuple);
	}

	CatalogCloseIndexes(indstate);

	heap_close(pg_attribute, RowExclusiveLock);
}

/* ----------------------------------------------------------------
 *		UpdateIndexRelation
 * ----------------------------------------------------------------
 */
static void
UpdateIndexRelation(Oid indexoid,
					Oid heapoid,
					IndexInfo *indexInfo,
					Oid *classOids,
					bool primary)
{
	int16		indkey[INDEX_MAX_KEYS];
	Oid			indclass[INDEX_MAX_KEYS];
	Datum		exprsDatum;
	Datum		predDatum;
	Datum		values[Natts_pg_index];
	char		nulls[Natts_pg_index];
	Relation	pg_index;
	HeapTuple	tuple;
	int			i;

	/*
	 * Copy the index key and opclass info into zero-filled vectors
	 */
	MemSet(indkey, 0, sizeof(indkey));
	MemSet(indclass, 0, sizeof(indclass));
	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
	{
		indkey[i] = indexInfo->ii_KeyAttrNumbers[i];
		indclass[i] = classOids[i];
	}

	/*
	 * Convert the index expressions (if any) to a text datum
	 */
	if (indexInfo->ii_Expressions != NIL)
	{
		char	   *exprsString;

		exprsString = nodeToString(indexInfo->ii_Expressions);
		exprsDatum = DirectFunctionCall1(textin,
										 CStringGetDatum(exprsString));
		pfree(exprsString);
	}
	else
		exprsDatum = (Datum) 0;

	/*
	 * Convert the index predicate (if any) to a text datum
	 */
	if (indexInfo->ii_Predicate != NIL)
	{
		char	   *predString;

		predString = nodeToString(indexInfo->ii_Predicate);
		predDatum = DirectFunctionCall1(textin,
										CStringGetDatum(predString));
		pfree(predString);
	}
	else
		predDatum = (Datum) 0;

	/*
	 * open the system catalog index relation
	 */
	pg_index = heap_openr(IndexRelationName, RowExclusiveLock);

	/*
	 * Build a pg_index tuple
	 */
	MemSet(nulls, ' ', sizeof(nulls));

	values[Anum_pg_index_indexrelid - 1] = ObjectIdGetDatum(indexoid);
	values[Anum_pg_index_indrelid - 1] = ObjectIdGetDatum(heapoid);
	values[Anum_pg_index_indkey - 1] = PointerGetDatum(indkey);
	values[Anum_pg_index_indclass - 1] = PointerGetDatum(indclass);
	values[Anum_pg_index_indnatts - 1] = Int16GetDatum(indexInfo->ii_NumIndexAttrs);
	values[Anum_pg_index_indisunique - 1] = BoolGetDatum(indexInfo->ii_Unique);
	values[Anum_pg_index_indisprimary - 1] = BoolGetDatum(primary);
	values[Anum_pg_index_indisclustered - 1] = BoolGetDatum(false);
	values[Anum_pg_index_indexprs - 1] = exprsDatum;
	if (exprsDatum == (Datum) 0)
		nulls[Anum_pg_index_indexprs - 1] = 'n';
	values[Anum_pg_index_indpred - 1] = predDatum;
	if (predDatum == (Datum) 0)
		nulls[Anum_pg_index_indpred - 1] = 'n';

	tuple = heap_formtuple(RelationGetDescr(pg_index), values, nulls);

	/*
	 * insert the tuple into the pg_index catalog
	 */
	simple_heap_insert(pg_index, tuple);

	/* update the indexes on pg_index */
	CatalogUpdateIndexes(pg_index, tuple);

	/*
	 * close the relation and free the tuple
	 */
	heap_close(pg_index, RowExclusiveLock);
	heap_freetuple(tuple);
}


/* ----------------------------------------------------------------
 *		index_create
 *
 * Returns OID of the created index.
 * ----------------------------------------------------------------
 */
Oid
index_create(Oid heapRelationId,
			 const char *indexRelationName,
			 IndexInfo *indexInfo,
			 Oid accessMethodObjectId,
			 Oid *classObjectId,
			 bool primary,
			 bool isconstraint,
			 bool allow_system_table_mods)
{
	Relation	heapRelation;
	Relation	indexRelation;
	TupleDesc	indexTupDesc;
	bool		shared_relation;
	Oid			namespaceId;
	Oid			indexoid;
	int			i;

	/*
	 * Only SELECT ... FOR UPDATE are allowed while doing this
	 */
	heapRelation = heap_open(heapRelationId, ShareLock);

	/*
	 * The index will be in the same namespace as its parent table, and is
	 * shared across databases if and only if the parent is.
	 */
	namespaceId = RelationGetNamespace(heapRelation);
	shared_relation = heapRelation->rd_rel->relisshared;

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
	 * We cannot allow indexing a shared relation after initdb (because
	 * there's no way to make the entry in other databases' pg_class).
	 * Unfortunately we can't distinguish initdb from a manually started
	 * standalone backend.	However, we can at least prevent this mistake
	 * under normal multi-user operation.
	 */
	if (shared_relation && IsUnderPostmaster)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			   errmsg("shared indexes cannot be created after initdb")));

	if (get_relname_relid(indexRelationName, namespaceId))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists",
						indexRelationName)));

	/*
	 * construct tuple descriptor for index tuples
	 */
	indexTupDesc = ConstructTupleDescriptor(heapRelation,
											indexInfo,
											classObjectId);

	/*
	 * create the index relation's relcache entry and physical disk file.
	 * (If we fail further down, it's the smgr's responsibility to remove
	 * the disk file again.)
	 */
	indexRelation = heap_create(indexRelationName,
								namespaceId,
								indexTupDesc,
								shared_relation,
								true,
								allow_system_table_mods);

	/* Fetch the relation OID assigned by heap_create */
	indexoid = RelationGetRelid(indexRelation);

	/*
	 * Obtain exclusive lock on it.  Although no other backends can see it
	 * until we commit, this prevents deadlock-risk complaints from lock
	 * manager in cases such as CLUSTER.
	 */
	LockRelation(indexRelation, AccessExclusiveLock);

	/*
	 * Fill in fields of the index's pg_class entry that are not set
	 * correctly by heap_create.
	 *
	 * XXX should have a cleaner way to create cataloged indexes
	 */
	indexRelation->rd_rel->relowner = GetUserId();
	indexRelation->rd_rel->relam = accessMethodObjectId;
	indexRelation->rd_rel->relkind = RELKIND_INDEX;
	indexRelation->rd_rel->relhasoids = false;

	/*
	 * store index's pg_class entry
	 */
	UpdateRelationRelation(indexRelation);

	/*
	 * now update the object id's of all the attribute tuple forms in the
	 * index relation's tuple descriptor
	 */
	InitializeAttributeOids(indexRelation,
							indexInfo->ii_NumIndexAttrs,
							indexoid);

	/*
	 * append ATTRIBUTE tuples for the index
	 */
	AppendAttributeTuples(indexRelation, indexInfo->ii_NumIndexAttrs);

	/* ----------------
	 *	  update pg_index
	 *	  (append INDEX tuple)
	 *
	 *	  Note that this stows away a representation of "predicate".
	 *	  (Or, could define a rule to maintain the predicate) --Nels, Feb '92
	 * ----------------
	 */
	UpdateIndexRelation(indexoid, heapRelationId, indexInfo,
						classObjectId, primary);

	/*
	 * Register constraint and dependencies for the index.
	 *
	 * If the index is from a CONSTRAINT clause, construct a pg_constraint
	 * entry.  The index is then linked to the constraint, which in turn
	 * is linked to the table.	If it's not a CONSTRAINT, make the
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

		myself.classId = RelOid_pg_class;
		myself.objectId = indexoid;
		myself.objectSubId = 0;

		if (isconstraint)
		{
			char		constraintType;
			Oid			conOid;

			if (primary)
				constraintType = CONSTRAINT_PRIMARY;
			else if (indexInfo->ii_Unique)
				constraintType = CONSTRAINT_UNIQUE;
			else
			{
				elog(ERROR, "constraint must be PRIMARY or UNIQUE");
				constraintType = 0;		/* keep compiler quiet */
			}

			/* Shouldn't have any expressions */
			if (indexInfo->ii_Expressions)
				elog(ERROR, "constraints can't have index expressions");

			conOid = CreateConstraintEntry(indexRelationName,
										   namespaceId,
										   constraintType,
										   false,		/* isDeferrable */
										   false,		/* isDeferred */
										   heapRelationId,
										   indexInfo->ii_KeyAttrNumbers,
										   indexInfo->ii_NumIndexAttrs,
										   InvalidOid,	/* no domain */
										   InvalidOid,	/* no foreign key */
										   NULL,
										   0,
										   ' ',
										   ' ',
										   ' ',
										   InvalidOid,	/* no associated index */
										   NULL,		/* no check constraint */
										   NULL,
										   NULL);

			referenced.classId = get_system_catalog_relid(ConstraintRelationName);
			referenced.objectId = conOid;
			referenced.objectSubId = 0;

			recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
		}
		else
		{
			/* Create auto dependencies on simply-referenced columns */
			for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
			{
				if (indexInfo->ii_KeyAttrNumbers[i] != 0)
				{
					referenced.classId = RelOid_pg_class;
					referenced.objectId = heapRelationId;
					referenced.objectSubId = indexInfo->ii_KeyAttrNumbers[i];

					recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
				}
			}
		}

		/* Store dependency on operator classes */
		referenced.classId = get_system_catalog_relid(OperatorClassRelationName);
		for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
		{
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
											DEPENDENCY_AUTO);
		}

		/* Store dependencies on anything mentioned in predicate */
		if (indexInfo->ii_Predicate)
		{
			recordDependencyOnSingleRelExpr(&myself,
										(Node *) indexInfo->ii_Predicate,
											heapRelationId,
											DEPENDENCY_NORMAL,
											DEPENDENCY_AUTO);
		}
	}

	/*
	 * Advance the command counter so that we can see the newly-entered
	 * catalog tuples for the index.
	 */
	CommandCounterIncrement();

	/*
	 * In bootstrap mode, we have to fill in the index strategy structure
	 * with information from the catalogs.  If we aren't bootstrapping,
	 * then the relcache entry has already been rebuilt thanks to sinval
	 * update during CommandCounterIncrement.
	 */
	if (IsBootstrapProcessingMode())
		RelationInitIndexAccessInfo(indexRelation);
	else
		Assert(indexRelation->rd_indexcxt != NULL);

	/*
	 * If this is bootstrap (initdb) time, then we don't actually fill in
	 * the index yet.  We'll be creating more indexes and classes later,
	 * so we delay filling them in until just before we're done with
	 * bootstrapping.  Otherwise, we call the routine that constructs the
	 * index.
	 *
	 * In normal processing mode, the heap and index relations are closed by
	 * index_build() --- but we continue to hold the ShareLock on the heap
	 * and the exclusive lock on the index that we acquired above, until
	 * end of transaction.
	 */
	if (IsBootstrapProcessingMode())
	{
		index_register(heapRelationId, indexoid, indexInfo);
		/* XXX shouldn't we close the heap and index rels here? */
	}
	else
		index_build(heapRelation, indexRelation, indexInfo);

	return indexoid;
}

/*
 *		index_drop
 *
 * NOTE: this routine should now only be called through performDeletion(),
 * else associated dependencies won't be cleaned up.
 */
void
index_drop(Oid indexId)
{
	Oid			heapId;
	Relation	userHeapRelation;
	Relation	userIndexRelation;
	Relation	indexRelation;
	HeapTuple	tuple;
	int			i;

	Assert(OidIsValid(indexId));

	/*
	 * To drop an index safely, we must grab exclusive lock on its parent
	 * table; otherwise there could be other backends using the index!
	 * Exclusive lock on the index alone is insufficient because another
	 * backend might be in the midst of devising a query plan that will
	 * use the index.  The parser and planner take care to hold an
	 * appropriate lock on the parent table while working, but having them
	 * hold locks on all the indexes too seems overly complex.	We do grab
	 * exclusive lock on the index too, just to be safe. Both locks must
	 * be held till end of transaction, else other backends will still see
	 * this index in pg_index.
	 */
	heapId = IndexGetRelation(indexId);
	userHeapRelation = heap_open(heapId, AccessExclusiveLock);

	userIndexRelation = index_open(indexId);
	LockRelation(userIndexRelation, AccessExclusiveLock);

	/*
	 * fix RELATION relation
	 */
	DeleteRelationTuple(indexId);

	/*
	 * fix ATTRIBUTE relation
	 */
	DeleteAttributeTuples(indexId);

	/*
	 * fix INDEX relation
	 */
	indexRelation = heap_openr(IndexRelationName, RowExclusiveLock);

	tuple = SearchSysCache(INDEXRELID,
						   ObjectIdGetDatum(indexId),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", indexId);

	simple_heap_delete(indexRelation, &tuple->t_self);

	ReleaseSysCache(tuple);
	heap_close(indexRelation, RowExclusiveLock);

	/*
	 * flush buffer cache and physically remove the file
	 */
	i = FlushRelationBuffers(userIndexRelation, (BlockNumber) 0);
	if (i < 0)
		elog(ERROR, "FlushRelationBuffers returned %d", i);

	smgrunlink(DEFAULT_SMGR, userIndexRelation);

	/*
	 * We are presently too lazy to attempt to compute the new correct
	 * value of relhasindex (the next VACUUM will fix it if necessary). So
	 * there is no need to update the pg_class tuple for the owning
	 * relation. But we must send out a shared-cache-inval notice on the
	 * owning relation to ensure other backends update their relcache
	 * lists of indexes.
	 */
	CacheInvalidateRelcache(heapId);

	/*
	 * Close rels, but keep locks
	 */
	index_close(userIndexRelation);
	heap_close(userHeapRelation, NoLock);

	RelationForgetRelation(indexId);
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
 * of individual index tuples.	Normally we build an IndexInfo for an index
 * just once per command, and then use it for (potentially) many tuples.
 * ----------------
 */
IndexInfo *
BuildIndexInfo(Relation index)
{
	IndexInfo  *ii = makeNode(IndexInfo);
	Form_pg_index indexStruct = index->rd_index;
	int			i;
	int			numKeys;

	/* check the number of keys, and copy attr numbers into the IndexInfo */
	numKeys = indexStruct->indnatts;
	if (numKeys < 1 || numKeys > INDEX_MAX_KEYS)
		elog(ERROR, "invalid indnatts %d for index %u",
			 numKeys, RelationGetRelid(index));
	ii->ii_NumIndexAttrs = numKeys;
	for (i = 0; i < numKeys; i++)
		ii->ii_KeyAttrNumbers[i] = indexStruct->indkey[i];

	/* fetch any expressions needed for expressional indexes */
	ii->ii_Expressions = RelationGetIndexExpressions(index);
	ii->ii_ExpressionsState = NIL;

	/* fetch index predicate if any */
	ii->ii_Predicate = RelationGetIndexPredicate(index);
	ii->ii_PredicateState = NIL;

	/* other info */
	ii->ii_Unique = indexStruct->indisunique;

	return ii;
}

/* ----------------
 *		FormIndexDatum
 *			Construct Datum[] and nullv[] arrays for a new index tuple.
 *
 *	indexInfo		Info about the index
 *	heapTuple		Heap tuple for which we must prepare an index entry
 *	heapDescriptor	tupledesc for heap tuple
 *	estate			executor state for evaluating any index expressions
 *	datum			Array of index Datums (output area)
 *	nullv			Array of is-null indicators (output area)
 *
 * When there are no index expressions, estate may be NULL.  Otherwise it
 * must be supplied, *and* the ecxt_scantuple slot of its per-tuple expr
 * context must point to the heap tuple passed in.
 *
 * For largely historical reasons, we don't actually call index_formtuple()
 * here, we just prepare its input arrays datum[] and nullv[].
 * ----------------
 */
void
FormIndexDatum(IndexInfo *indexInfo,
			   HeapTuple heapTuple,
			   TupleDesc heapDescriptor,
			   EState *estate,
			   Datum *datum,
			   char *nullv)
{
	List	   *indexprs;
	int			i;

	if (indexInfo->ii_Expressions != NIL &&
		indexInfo->ii_ExpressionsState == NIL)
	{
		/* First time through, set up expression evaluation state */
		indexInfo->ii_ExpressionsState = (List *)
			ExecPrepareExpr((Expr *) indexInfo->ii_Expressions,
							estate);
		/* Check caller has set up context correctly */
		Assert(GetPerTupleExprContext(estate)->ecxt_scantuple->val == heapTuple);
	}
	indexprs = indexInfo->ii_ExpressionsState;

	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
	{
		int			keycol = indexInfo->ii_KeyAttrNumbers[i];
		Datum		iDatum;
		bool		isNull;

		if (keycol != 0)
		{
			/*
			 * Plain index column; get the value we need directly from the
			 * heap tuple.
			 */
			iDatum = heap_getattr(heapTuple, keycol, heapDescriptor, &isNull);
		}
		else
		{
			/*
			 * Index expression --- need to evaluate it.
			 */
			if (indexprs == NIL)
				elog(ERROR, "wrong number of index expressions");
			iDatum = ExecEvalExprSwitchContext((ExprState *) lfirst(indexprs),
										  GetPerTupleExprContext(estate),
											   &isNull,
											   NULL);
			indexprs = lnext(indexprs);
		}
		datum[i] = iDatum;
		nullv[i] = (isNull) ? 'n' : ' ';
	}

	if (indexprs != NIL)
		elog(ERROR, "wrong number of index expressions");
}


/* ----------------
 *		set relhasindex of relation's pg_class entry
 *
 * If isprimary is TRUE, we are defining a primary index, so also set
 * relhaspkey to TRUE.	Otherwise, leave relhaspkey alone.
 *
 * If reltoastidxid is not InvalidOid, also set reltoastidxid to that value.
 * This is only used for TOAST relations.
 *
 * NOTE: an important side-effect of this operation is that an SI invalidation
 * message is sent out to all backends --- including me --- causing relcache
 * entries to be flushed or updated with the new hasindex data.  This must
 * happen even if we find that no change is needed in the pg_class row.
 * ----------------
 */
void
setRelhasindex(Oid relid, bool hasindex, bool isprimary, Oid reltoastidxid)
{
	Relation	pg_class;
	HeapTuple	tuple;
	Form_pg_class classtuple;
	bool		dirty = false;
	HeapScanDesc pg_class_scan = NULL;

	/*
	 * Find the tuple to update in pg_class.  In bootstrap mode we can't
	 * use heap_update, so cheat and overwrite the tuple in-place.  In
	 * normal processing, make a copy to scribble on.
	 */
	pg_class = heap_openr(RelationRelationName, RowExclusiveLock);

	if (!IsBootstrapProcessingMode())
	{
		tuple = SearchSysCacheCopy(RELOID,
								   ObjectIdGetDatum(relid),
								   0, 0, 0);
	}
	else
	{
		ScanKeyData key[1];

		ScanKeyEntryInitialize(&key[0], 0,
							   ObjectIdAttributeNumber,
							   F_OIDEQ,
							   ObjectIdGetDatum(relid));

		pg_class_scan = heap_beginscan(pg_class, SnapshotNow, 1, key);
		tuple = heap_getnext(pg_class_scan, ForwardScanDirection);
	}

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for relation %u", relid);
	classtuple = (Form_pg_class) GETSTRUCT(tuple);

	/* Apply required updates */

	if (pg_class_scan)
		LockBuffer(pg_class_scan->rs_cbuf, BUFFER_LOCK_EXCLUSIVE);

	if (classtuple->relhasindex != hasindex)
	{
		classtuple->relhasindex = hasindex;
		dirty = true;
	}
	if (isprimary)
	{
		if (!classtuple->relhaspkey)
		{
			classtuple->relhaspkey = true;
			dirty = true;
		}
	}
	if (OidIsValid(reltoastidxid))
	{
		Assert(classtuple->relkind == RELKIND_TOASTVALUE);
		if (classtuple->reltoastidxid != reltoastidxid)
		{
			classtuple->reltoastidxid = reltoastidxid;
			dirty = true;
		}
	}

	if (pg_class_scan)
		LockBuffer(pg_class_scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

	if (pg_class_scan)
	{
		/* Write the modified tuple in-place */
		WriteNoReleaseBuffer(pg_class_scan->rs_cbuf);
		/* Send out shared cache inval if necessary */
		if (!IsBootstrapProcessingMode())
			CacheInvalidateHeapTuple(pg_class, tuple);
		BufferSync();
	}
	else if (dirty)
	{
		simple_heap_update(pg_class, &tuple->t_self, tuple);

		/* Keep the catalog indexes up to date */
		CatalogUpdateIndexes(pg_class, tuple);
	}
	else
	{
		/* no need to change tuple, but force relcache rebuild anyway */
		CacheInvalidateRelcache(relid);
	}

	if (!pg_class_scan)
		heap_freetuple(tuple);
	else
		heap_endscan(pg_class_scan);

	heap_close(pg_class, RowExclusiveLock);
}

/*
 * setNewRelfilenode		- assign a new relfilenode value to the relation
 *
 * Caller must already hold exclusive lock on the relation.
 */
void
setNewRelfilenode(Relation relation)
{
	Oid			newrelfilenode;
	Relation	pg_class;
	HeapTuple	tuple;
	Form_pg_class rd_rel;
	RelationData workrel;

	/* Can't change relfilenode for nailed tables (indexes ok though) */
	Assert(!relation->rd_isnailed ||
		   relation->rd_rel->relkind == RELKIND_INDEX);
	/* Can't change for shared tables or indexes */
	Assert(!relation->rd_rel->relisshared);

	/* Allocate a new relfilenode */
	newrelfilenode = newoid();

	/*
	 * Find the pg_class tuple for the given relation.  This is not used
	 * during bootstrap, so okay to use heap_update always.
	 */
	pg_class = heap_openr(RelationRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(RelationGetRelid(relation)),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for relation %u",
			 RelationGetRelid(relation));
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/* create another storage file. Is it a little ugly ? */
	/* NOTE: any conflict in relfilenode value will be caught here */
	memcpy((char *) &workrel, relation, sizeof(RelationData));
	workrel.rd_fd = -1;
	workrel.rd_node.relNode = newrelfilenode;
	heap_storage_create(&workrel);
	smgrclose(DEFAULT_SMGR, &workrel);

	/* schedule unlinking old relfilenode */
	smgrunlink(DEFAULT_SMGR, relation);

	/* update the pg_class row */
	rd_rel->relfilenode = newrelfilenode;
	simple_heap_update(pg_class, &tuple->t_self, tuple);
	CatalogUpdateIndexes(pg_class, tuple);

	heap_freetuple(tuple);

	heap_close(pg_class, RowExclusiveLock);

	/* Make sure the relfilenode change is visible */
	CommandCounterIncrement();
}


/* ----------------
 *		UpdateStats
 *
 * Update pg_class' relpages and reltuples statistics for the given relation
 * (which can be either a table or an index).  Note that this is not used
 * in the context of VACUUM.
 * ----------------
 */
void
UpdateStats(Oid relid, double reltuples)
{
	Relation	whichRel;
	Relation	pg_class;
	HeapTuple	tuple;
	BlockNumber relpages;
	Form_pg_class rd_rel;
	HeapScanDesc pg_class_scan = NULL;
	bool		in_place_upd;

	/*
	 * This routine handles updates for both the heap and index relation
	 * statistics.	In order to guarantee that we're able to *see* the
	 * index relation tuple, we bump the command counter id here.  The
	 * index relation tuple was created in the current transaction.
	 */
	CommandCounterIncrement();

	/*
	 * CommandCounterIncrement() flushes invalid cache entries, including
	 * those for the heap and index relations for which we're updating
	 * statistics.	Now that the cache is flushed, it's safe to open the
	 * relation again.	We need the relation open in order to figure out
	 * how many blocks it contains.
	 */

	/*
	 * Grabbing lock here is probably redundant ...
	 */
	whichRel = relation_open(relid, ShareLock);

	/*
	 * Find the tuple to update in pg_class.  Normally we make a copy of
	 * the tuple using the syscache, modify it, and apply heap_update.
	 * But in bootstrap mode we can't use heap_update, so we cheat and
	 * overwrite the tuple in-place.
	 *
	 * We also must cheat if reindexing pg_class itself, because the
	 * target index may presently not be part of the set of indexes that
	 * CatalogUpdateIndexes would update (see reindex_relation).  In this
	 * case the stats updates will not be WAL-logged and so could be lost
	 * in a crash.  This seems OK considering VACUUM does the same thing.
	 */
	pg_class = heap_openr(RelationRelationName, RowExclusiveLock);

	in_place_upd = IsBootstrapProcessingMode() ||
		ReindexIsProcessingHeap(RelationGetRelid(pg_class));

	if (!in_place_upd)
	{
		tuple = SearchSysCacheCopy(RELOID,
								   ObjectIdGetDatum(relid),
								   0, 0, 0);
	}
	else
	{
		ScanKeyData key[1];

		ScanKeyEntryInitialize(&key[0], 0,
							   ObjectIdAttributeNumber,
							   F_OIDEQ,
							   ObjectIdGetDatum(relid));

		pg_class_scan = heap_beginscan(pg_class, SnapshotNow, 1, key);
		tuple = heap_getnext(pg_class_scan, ForwardScanDirection);
	}

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for relation %u", relid);
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/*
	 * Figure values to insert.
	 *
	 * If we found zero tuples in the scan, do NOT believe it; instead put a
	 * bogus estimate into the statistics fields.  Otherwise, the common
	 * pattern "CREATE TABLE; CREATE INDEX; insert data" leaves the table
	 * with zero size statistics until a VACUUM is done.  The optimizer
	 * will generate very bad plans if the stats claim the table is empty
	 * when it is actually sizable.  See also CREATE TABLE in heap.c.
	 *
	 * Note: this path is also taken during bootstrap, because bootstrap.c
	 * passes reltuples = 0 after loading a table.	We have to estimate
	 * some number for reltuples based on the actual number of pages.
	 */
	relpages = RelationGetNumberOfBlocks(whichRel);

	if (reltuples == 0)
	{
		if (relpages == 0)
		{
			/* Bogus defaults for a virgin table, same as heap.c */
			reltuples = 1000;
			relpages = 10;
		}
		else if (whichRel->rd_rel->relkind == RELKIND_INDEX && relpages <= 2)
		{
			/* Empty index, leave bogus defaults in place */
			reltuples = 1000;
		}
		else
			reltuples = ((double) relpages) * NTUPLES_PER_PAGE(whichRel->rd_rel->relnatts);
	}

	/*
	 * Update statistics in pg_class, if they changed.	(Avoiding an
	 * unnecessary update is not just a tiny performance improvement; it
	 * also reduces the window wherein concurrent CREATE INDEX commands
	 * may conflict.)
	 */
	if (rd_rel->relpages != (int32) relpages ||
		rd_rel->reltuples != (float4) reltuples)
	{
		if (in_place_upd)
		{
			/* Bootstrap or reindex case: overwrite fields in place. */
			LockBuffer(pg_class_scan->rs_cbuf, BUFFER_LOCK_EXCLUSIVE);
			rd_rel->relpages = (int32) relpages;
			rd_rel->reltuples = (float4) reltuples;
			LockBuffer(pg_class_scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			WriteNoReleaseBuffer(pg_class_scan->rs_cbuf);
			if (!IsBootstrapProcessingMode())
				CacheInvalidateHeapTuple(pg_class, tuple);
		}
		else
		{
			/* During normal processing, must work harder. */
			rd_rel->relpages = (int32) relpages;
			rd_rel->reltuples = (float4) reltuples;
			simple_heap_update(pg_class, &tuple->t_self, tuple);
			CatalogUpdateIndexes(pg_class, tuple);
		}
	}

	if (!pg_class_scan)
		heap_freetuple(tuple);
	else
		heap_endscan(pg_class_scan);

	/*
	 * We shouldn't have to do this, but we do...  Modify the reldesc in
	 * place with the new values so that the cache contains the latest
	 * copy.  (XXX is this really still necessary?	The relcache will get
	 * fixed at next CommandCounterIncrement, so why bother here?)
	 */
	whichRel->rd_rel->relpages = (int32) relpages;
	whichRel->rd_rel->reltuples = (float4) reltuples;

	heap_close(pg_class, RowExclusiveLock);
	relation_close(whichRel, NoLock);
}


/*
 * index_build - invoke access-method-specific index build procedure
 */
void
index_build(Relation heapRelation,
			Relation indexRelation,
			IndexInfo *indexInfo)
{
	RegProcedure procedure;

	/*
	 * sanity checks
	 */
	Assert(RelationIsValid(indexRelation));
	Assert(PointerIsValid(indexRelation->rd_am));

	procedure = indexRelation->rd_am->ambuild;
	Assert(RegProcedureIsValid(procedure));

	/*
	 * Call the access method's build procedure
	 */
	OidFunctionCall3(procedure,
					 PointerGetDatum(heapRelation),
					 PointerGetDatum(indexRelation),
					 PointerGetDatum(indexInfo));
}


/*
 * IndexBuildHeapScan - scan the heap relation to find tuples to be indexed
 *
 * This is called back from an access-method-specific index build procedure
 * after the AM has done whatever setup it needs.  The parent heap relation
 * is scanned to find tuples that should be entered into the index.  Each
 * such tuple is passed to the AM's callback routine, which does the right
 * things to add it to the new index.  After we return, the AM's index
 * build procedure does whatever cleanup is needed; in particular, it should
 * close the heap and index relations.
 *
 * The total count of heap tuples is returned.	This is for updating pg_class
 * statistics.	(It's annoying not to be able to do that here, but we can't
 * do it until after the relation is closed.)  Note that the index AM itself
 * must keep track of the number of index tuples; we don't do so here because
 * the AM might reject some of the tuples for its own reasons, such as being
 * unable to store NULLs.
 */
double
IndexBuildHeapScan(Relation heapRelation,
				   Relation indexRelation,
				   IndexInfo *indexInfo,
				   IndexBuildCallback callback,
				   void *callback_state)
{
	HeapScanDesc scan;
	HeapTuple	heapTuple;
	TupleDesc	heapDescriptor;
	Datum		attdata[INDEX_MAX_KEYS];
	char		nulls[INDEX_MAX_KEYS];
	double		reltuples;
	List	   *predicate;
	TupleTable	tupleTable;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;
	TransactionId OldestXmin;

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	heapDescriptor = RelationGetDescr(heapRelation);

	/*
	 * Need an EState for evaluation of index expressions and
	 * partial-index predicates.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);

	/*
	 * If this is a predicate (partial) index, we will need to evaluate
	 * the predicate using ExecQual, which requires the current tuple to
	 * be in a slot of a TupleTable.  Likewise if there are any
	 * expressions.
	 */
	if (indexInfo->ii_Predicate != NIL || indexInfo->ii_Expressions != NIL)
	{
		tupleTable = ExecCreateTupleTable(1);
		slot = ExecAllocTableSlot(tupleTable);
		ExecSetSlotDescriptor(slot, heapDescriptor, false);

		/* Arrange for econtext's scan tuple to be the tuple under test */
		econtext->ecxt_scantuple = slot;

		/* Set up execution state for predicate. */
		predicate = (List *)
			ExecPrepareExpr((Expr *) indexInfo->ii_Predicate,
							estate);
	}
	else
	{
		tupleTable = NULL;
		slot = NULL;
		predicate = NIL;
	}

	/*
	 * Ok, begin our scan of the base relation.  We use SnapshotAny
	 * because we must retrieve all tuples and do our own time qual
	 * checks.
	 */
	if (IsBootstrapProcessingMode())
	{
		snapshot = SnapshotNow;
		OldestXmin = InvalidTransactionId;
	}
	else
	{
		snapshot = SnapshotAny;
		OldestXmin = GetOldestXmin(heapRelation->rd_rel->relisshared);
	}

	scan = heap_beginscan(heapRelation, /* relation */
						  snapshot,		/* seeself */
						  0,	/* number of keys */
						  (ScanKey) NULL);		/* scan key */

	reltuples = 0;

	/*
	 * Scan all tuples in the base relation.
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool		tupleIsAlive;

		CHECK_FOR_INTERRUPTS();

		if (snapshot == SnapshotAny)
		{
			/* do our own time qual check */
			bool		indexIt;
			uint16		sv_infomask;

			/*
			 * HeapTupleSatisfiesVacuum may update tuple's hint status
			 * bits. We could possibly get away with not locking the
			 * buffer here, since caller should hold ShareLock on the
			 * relation, but let's be conservative about it.
			 */
			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
			sv_infomask = heapTuple->t_data->t_infomask;

			switch (HeapTupleSatisfiesVacuum(heapTuple->t_data, OldestXmin))
			{
				case HEAPTUPLE_DEAD:
					indexIt = false;
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_LIVE:
					indexIt = true;
					tupleIsAlive = true;
					break;
				case HEAPTUPLE_RECENTLY_DEAD:

					/*
					 * If tuple is recently deleted then we must index it
					 * anyway to keep VACUUM from complaining.
					 */
					indexIt = true;
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:

					/*
					 * Since caller should hold ShareLock or better, we
					 * should not see any tuples inserted by open
					 * transactions --- unless it's our own transaction.
					 * (Consider INSERT followed by CREATE INDEX within a
					 * transaction.)  An exception occurs when reindexing
					 * a system catalog, because we often release lock on
					 * system catalogs before committing.
					 */
					if (!TransactionIdIsCurrentTransactionId(
							  HeapTupleHeaderGetXmin(heapTuple->t_data))
						&& !IsSystemRelation(heapRelation))
						elog(ERROR, "concurrent insert in progress");
					indexIt = true;
					tupleIsAlive = true;
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:

					/*
					 * Since caller should hold ShareLock or better, we
					 * should not see any tuples deleted by open
					 * transactions --- unless it's our own transaction.
					 * (Consider DELETE followed by CREATE INDEX within a
					 * transaction.)  An exception occurs when reindexing
					 * a system catalog, because we often release lock on
					 * system catalogs before committing.
					 */
					if (!TransactionIdIsCurrentTransactionId(
							  HeapTupleHeaderGetXmax(heapTuple->t_data))
						&& !IsSystemRelation(heapRelation))
						elog(ERROR, "concurrent delete in progress");
					indexIt = true;
					tupleIsAlive = false;
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					indexIt = tupleIsAlive = false;		/* keep compiler quiet */
					break;
			}

			/* check for hint-bit update by HeapTupleSatisfiesVacuum */
			if (sv_infomask != heapTuple->t_data->t_infomask)
				SetBufferCommitInfoNeedsSave(scan->rs_cbuf);

			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			if (!indexIt)
				continue;
		}
		else
		{
			/* heap_getnext did the time qual check */
			tupleIsAlive = true;
		}

		reltuples += 1;

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		/* Set up for predicate or expression evaluation */
		if (slot)
			ExecStoreTuple(heapTuple, slot, InvalidBuffer, false);

		/*
		 * In a partial index, discard tuples that don't satisfy the
		 * predicate.  We can also discard recently-dead tuples, since
		 * VACUUM doesn't complain about tuple count mismatch for partial
		 * indexes.
		 */
		if (predicate != NIL)
		{
			if (!tupleIsAlive)
				continue;
			if (!ExecQual(predicate, econtext, false))
				continue;
		}

		/*
		 * For the current heap tuple, extract all the attributes we use
		 * in this index, and note which are null.	This also performs
		 * evaluation of any expressions needed.
		 */
		FormIndexDatum(indexInfo,
					   heapTuple,
					   heapDescriptor,
					   estate,
					   attdata,
					   nulls);

		/*
		 * You'd think we should go ahead and build the index tuple here,
		 * but some index AMs want to do further processing on the data
		 * first.  So pass the attdata and nulls arrays, instead.
		 */

		/* Call the AM's callback routine to process the tuple */
		callback(indexRelation, heapTuple, attdata, nulls, tupleIsAlive,
				 callback_state);
	}

	heap_endscan(scan);

	if (tupleTable)
		ExecDropTupleTable(tupleTable, true);

	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NIL;

	return reltuples;
}


/*
 * IndexGetRelation: given an index's relation OID, get the OID of the
 * relation it is an index on.	Uses the system cache.
 */
static Oid
IndexGetRelation(Oid indexId)
{
	HeapTuple	tuple;
	Form_pg_index index;
	Oid			result;

	tuple = SearchSysCache(INDEXRELID,
						   ObjectIdGetDatum(indexId),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", indexId);
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
reindex_index(Oid indexId)
{
	Relation	iRel,
				heapRelation;
	IndexInfo  *indexInfo;
	Oid			heapId;
	bool		inplace;

	/*
	 * Open our index relation and get an exclusive lock on it.
	 *
	 * Note: for REINDEX INDEX, doing this before opening the parent heap
	 * relation means there's a possibility for deadlock failure against
	 * another xact that is doing normal accesses to the heap and index.
	 * However, it's not real clear why you'd be wanting to do REINDEX INDEX
	 * on a table that's in active use, so I'd rather have the protection of
	 * making sure the index is locked down.  In the REINDEX TABLE and
	 * REINDEX DATABASE cases, there is no problem because caller already
	 * holds exclusive lock on the parent table.
	 */
	iRel = index_open(indexId);
	LockRelation(iRel, AccessExclusiveLock);

	/* Get OID of index's parent table */
	heapId = iRel->rd_index->indrelid;

	/* Open and lock the parent heap relation */
	heapRelation = heap_open(heapId, AccessExclusiveLock);

	SetReindexProcessing(heapId, indexId);

	/*
	 * If it's a shared index, we must do inplace processing (because we
	 * have no way to update relfilenode in other databases).  Otherwise
	 * we can do it the normal transaction-safe way.
	 *
	 * Since inplace processing isn't crash-safe, we only allow it in a
	 * standalone backend.  (In the REINDEX TABLE and REINDEX DATABASE cases,
	 * the caller should have detected this.)
	 */
	inplace = iRel->rd_rel->relisshared;

	if (inplace && IsUnderPostmaster)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("shared index \"%s\" can only be reindexed in stand-alone mode",
						RelationGetRelationName(iRel))));

	/* Fetch info needed for index_build */
	indexInfo = BuildIndexInfo(iRel);

	if (inplace)
	{
		/*
		 * Release any buffers associated with this index.	If they're
		 * dirty, they're just dropped without bothering to flush to disk.
		 */
		DropRelationBuffers(iRel);

		/* Now truncate the actual data and set blocks to zero */
		smgrtruncate(DEFAULT_SMGR, iRel, 0);
		iRel->rd_nblocks = 0;
		iRel->rd_targblock = InvalidBlockNumber;
	}
	else
	{
		/*
		 * We'll build a new physical relation for the index.
		 */
		setNewRelfilenode(iRel);
	}

	/* Initialize the index and rebuild */
	index_build(heapRelation, iRel, indexInfo);

	/*
	 * index_build will close both the heap and index relations (but not
	 * give up the locks we hold on them).	So we're done.
	 */
	SetReindexProcessing(InvalidOid, InvalidOid);
}

/*
 * reindex_relation - This routine is used to recreate all indexes
 * of a relation (and its toast relation too, if any).
 *
 * Returns true if any indexes were rebuilt.
 */
bool
reindex_relation(Oid relid)
{
	Relation	rel;
	Oid			toast_relid;
	bool		is_pg_class;
	bool		result;
	List	   *indexIds,
			   *doneIndexes,
			   *indexId;

	/*
	 * Ensure to hold an exclusive lock throughout the transaction. The
	 * lock could perhaps be less intensive (in the non-overwrite case)
	 * but for now it's AccessExclusiveLock for simplicity.
	 */
	rel = heap_open(relid, AccessExclusiveLock);

	toast_relid = rel->rd_rel->reltoastrelid;

	/*
	 * Get the list of index OIDs for this relation.  (We trust to the
	 * relcache to get this with a sequential scan if ignoring system
	 * indexes.)
	 */
	indexIds = RelationGetIndexList(rel);

	/*
	 * reindex_index will attempt to update the pg_class rows for the
	 * relation and index.  If we are processing pg_class itself, we
	 * want to make sure that the updates do not try to insert index
	 * entries into indexes we have not processed yet.  (When we are
	 * trying to recover from corrupted indexes, that could easily
	 * cause a crash.)  We can accomplish this because CatalogUpdateIndexes
	 * will use the relcache's index list to know which indexes to update.
	 * We just force the index list to be only the stuff we've processed.
	 *
	 * It is okay to not insert entries into the indexes we have not
	 * processed yet because all of this is transaction-safe.  If we fail
	 * partway through, the updated rows are dead and it doesn't matter
	 * whether they have index entries.  Also, a new pg_class index will
	 * be created with an entry for its own pg_class row because we do
	 * setNewRelfilenode() before we do index_build().
	 */
	is_pg_class = (RelationGetRelid(rel) == RelOid_pg_class);
	doneIndexes = NIL;

	/* Reindex all the indexes. */
	foreach(indexId, indexIds)
	{
		Oid		indexOid = lfirsto(indexId);

		if (is_pg_class)
			RelationSetIndexList(rel, doneIndexes);

		reindex_index(indexOid);

		CommandCounterIncrement();

		if (is_pg_class)
			doneIndexes = lappendo(doneIndexes, indexOid);
	}

	if (is_pg_class)
		RelationSetIndexList(rel, indexIds);

	/*
	 * Close rel, but continue to hold the lock.
	 */
	heap_close(rel, NoLock);

	result = (indexIds != NIL);

	/*
	 * If the relation has a secondary toast rel, reindex that too while we
	 * still hold the lock on the master table.
	 */
	if (toast_relid != InvalidOid)
		result |= reindex_relation(toast_relid);

	return result;
}
