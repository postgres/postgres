/*-------------------------------------------------------------------------
 *
 * index.c
 *	  code to create and destroy POSTGRES index relations
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/index.c,v 1.292 2008/01/30 19:46:48 tgl Exp $
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
#include "access/transam.h"
#include "access/xact.h"
#include "bootstrap/bootstrap.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"


/* state info for validate_index bulkdelete callback */
typedef struct
{
	Tuplesortstate *tuplesort;	/* for sorting the index TIDs */
	/* statistics (for debug purposes only): */
	double		htups,
				itups,
				tups_inserted;
} v_i_state;

/* non-export function prototypes */
static TupleDesc ConstructTupleDescriptor(Relation heapRelation,
						 IndexInfo *indexInfo,
						 Oid *classObjectId);
static void InitializeAttributeOids(Relation indexRelation,
						int numatts, Oid indexoid);
static void AppendAttributeTuples(Relation indexRelation, int numatts);
static void UpdateIndexRelation(Oid indexoid, Oid heapoid,
					IndexInfo *indexInfo,
					Oid *classOids,
					int16 *coloptions,
					bool primary,
					bool isvalid);
static void index_update_stats(Relation rel, bool hasindex, bool isprimary,
				   Oid reltoastidxid, double reltuples);
static bool validate_index_callback(ItemPointer itemptr, void *opaque);
static void validate_index_heapscan(Relation heapRelation,
						Relation indexRelation,
						IndexInfo *indexInfo,
						Snapshot snapshot,
						v_i_state *state);
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
	ListCell   *indexpr_item = list_head(indexInfo->ii_Expressions);
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
	 * For simple index columns, we copy the pg_attribute row from the parent
	 * relation and modify it as necessary.  For expressions we have to cons
	 * up a pg_attribute row the hard way.
	 */
	for (i = 0; i < numatts; i++)
	{
		AttrNumber	atnum = indexInfo->ii_KeyAttrNumbers[i];
		Form_pg_attribute to = indexTupDesc->attrs[i];
		HeapTuple	tuple;
		Form_pg_type typeTup;
		Oid			keyType;

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
			 * now that we've determined the "from", let's copy the tuple desc
			 * data...
			 */
			memcpy(to, from, ATTRIBUTE_TUPLE_SIZE);

			/*
			 * Fix the stuff that should not be the same as the underlying
			 * attr
			 */
			to->attnum = i + 1;

			to->attstattarget = -1;
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

			MemSet(to, 0, ATTRIBUTE_TUPLE_SIZE);

			if (indexpr_item == NULL)	/* shouldn't happen */
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(indexpr_item);

			/*
			 * Make the attribute's name "pg_expresssion_nnn" (maybe think of
			 * something better later)
			 */
			sprintf(NameStr(to->attname), "pg_expression_%d", i + 1);

			/*
			 * Lookup the expression type in pg_type for the type length etc.
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
			to->attstattarget = -1;
			to->attcacheoff = -1;
			to->atttypmod = -1;
			to->attislocal = true;

			ReleaseSysCache(tuple);
		}

		/*
		 * We do not yet have the correct relation OID for the index, so just
		 * set it invalid for now.	InitializeAttributeOids() will fix it
		 * later.
		 */
		to->attrelid = InvalidOid;

		/*
		 * Check the opclass to see if it provides a keytype (overriding the
		 * attribute type).
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
	pg_attribute = heap_open(AttributeRelationId, RowExclusiveLock);

	indstate = CatalogOpenIndexes(pg_attribute);

	/*
	 * insert data from new index's tupdesc into pg_attribute
	 */
	indexTupDesc = RelationGetDescr(indexRelation);

	for (i = 0; i < numatts; i++)
	{
		/*
		 * There used to be very grotty code here to set these fields, but I
		 * think it's unnecessary.  They should be set already.
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
 *
 * Construct and insert a new entry in the pg_index catalog
 * ----------------------------------------------------------------
 */
static void
UpdateIndexRelation(Oid indexoid,
					Oid heapoid,
					IndexInfo *indexInfo,
					Oid *classOids,
					int16 *coloptions,
					bool primary,
					bool isvalid)
{
	int2vector *indkey;
	oidvector  *indclass;
	int2vector *indoption;
	Datum		exprsDatum;
	Datum		predDatum;
	Datum		values[Natts_pg_index];
	char		nulls[Natts_pg_index];
	Relation	pg_index;
	HeapTuple	tuple;
	int			i;

	/*
	 * Copy the index key, opclass, and indoption info into arrays (should we
	 * make the caller pass them like this to start with?)
	 */
	indkey = buildint2vector(NULL, indexInfo->ii_NumIndexAttrs);
	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
		indkey->values[i] = indexInfo->ii_KeyAttrNumbers[i];
	indclass = buildoidvector(classOids, indexInfo->ii_NumIndexAttrs);
	indoption = buildint2vector(coloptions, indexInfo->ii_NumIndexAttrs);

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
	 * Convert the index predicate (if any) to a text datum.  Note we convert
	 * implicit-AND format to normal explicit-AND for storage.
	 */
	if (indexInfo->ii_Predicate != NIL)
	{
		char	   *predString;

		predString = nodeToString(make_ands_explicit(indexInfo->ii_Predicate));
		predDatum = DirectFunctionCall1(textin,
										CStringGetDatum(predString));
		pfree(predString);
	}
	else
		predDatum = (Datum) 0;

	/*
	 * open the system catalog index relation
	 */
	pg_index = heap_open(IndexRelationId, RowExclusiveLock);

	/*
	 * Build a pg_index tuple
	 */
	MemSet(nulls, ' ', sizeof(nulls));

	values[Anum_pg_index_indexrelid - 1] = ObjectIdGetDatum(indexoid);
	values[Anum_pg_index_indrelid - 1] = ObjectIdGetDatum(heapoid);
	values[Anum_pg_index_indnatts - 1] = Int16GetDatum(indexInfo->ii_NumIndexAttrs);
	values[Anum_pg_index_indisunique - 1] = BoolGetDatum(indexInfo->ii_Unique);
	values[Anum_pg_index_indisprimary - 1] = BoolGetDatum(primary);
	values[Anum_pg_index_indisclustered - 1] = BoolGetDatum(false);
	values[Anum_pg_index_indisvalid - 1] = BoolGetDatum(isvalid);
	values[Anum_pg_index_indcheckxmin - 1] = BoolGetDatum(false);
	/* we set isvalid and isready the same way */
	values[Anum_pg_index_indisready - 1] = BoolGetDatum(isvalid);
	values[Anum_pg_index_indkey - 1] = PointerGetDatum(indkey);
	values[Anum_pg_index_indclass - 1] = PointerGetDatum(indclass);
	values[Anum_pg_index_indoption - 1] = PointerGetDatum(indoption);
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


/*
 * index_create
 *
 * heapRelationId: OID of table to build index on
 * indexRelationName: what it say
 * indexRelationId: normally, pass InvalidOid to let this routine
 *		generate an OID for the index.	During bootstrap this may be
 *		nonzero to specify a preselected OID.
 * indexInfo: same info executor uses to insert into the index
 * accessMethodObjectId: OID of index AM to use
 * tableSpaceId: OID of tablespace to use
 * classObjectId: array of index opclass OIDs, one per index column
 * coloptions: array of per-index-column indoption settings
 * reloptions: AM-specific options
 * isprimary: index is a PRIMARY KEY
 * isconstraint: index is owned by a PRIMARY KEY or UNIQUE constraint
 * allow_system_table_mods: allow table to be a system catalog
 * skip_build: true to skip the index_build() step for the moment; caller
 *		must do it later (typically via reindex_index())
 * concurrent: if true, do not lock the table against writers.	The index
 *		will be marked "invalid" and the caller must take additional steps
 *		to fix it up.
 *
 * Returns OID of the created index.
 */
Oid
index_create(Oid heapRelationId,
			 const char *indexRelationName,
			 Oid indexRelationId,
			 IndexInfo *indexInfo,
			 Oid accessMethodObjectId,
			 Oid tableSpaceId,
			 Oid *classObjectId,
			 int16 *coloptions,
			 Datum reloptions,
			 bool isprimary,
			 bool isconstraint,
			 bool allow_system_table_mods,
			 bool skip_build,
			 bool concurrent)
{
	Relation	pg_class;
	Relation	heapRelation;
	Relation	indexRelation;
	TupleDesc	indexTupDesc;
	bool		shared_relation;
	Oid			namespaceId;
	int			i;

	pg_class = heap_open(RelationRelationId, RowExclusiveLock);

	/*
	 * Only SELECT ... FOR UPDATE/SHARE are allowed while doing a standard
	 * index build; but for concurrent builds we allow INSERT/UPDATE/DELETE
	 * (but not VACUUM).
	 */
	heapRelation = heap_open(heapRelationId,
						(concurrent ? ShareUpdateExclusiveLock : ShareLock));

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
	 * concurrent index build on a system catalog is unsafe because we tend to
	 * release locks before committing in catalogs
	 */
	if (concurrent &&
		IsSystemRelation(heapRelation))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("concurrent index creation on system catalog tables is not supported")));

	/*
	 * We cannot allow indexing a shared relation after initdb (because
	 * there's no way to make the entry in other databases' pg_class).
	 */
	if (shared_relation && !IsBootstrapProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("shared indexes cannot be created after initdb")));

	/*
	 * Validate shared/non-shared tablespace (must check this before doing
	 * GetNewRelFileNode, to prevent Assert therein)
	 */
	if (shared_relation)
	{
		if (tableSpaceId != GLOBALTABLESPACE_OID)
			/* elog since this is not a user-facing error */
			elog(ERROR,
				 "shared relations must be placed in pg_global tablespace");
	}
	else
	{
		if (tableSpaceId == GLOBALTABLESPACE_OID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("only shared relations can be placed in pg_global tablespace")));
	}

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
	 * Allocate an OID for the index, unless we were told what to use.
	 *
	 * The OID will be the relfilenode as well, so make sure it doesn't
	 * collide with either pg_class OIDs or existing physical files.
	 */
	if (!OidIsValid(indexRelationId))
		indexRelationId = GetNewRelFileNode(tableSpaceId, shared_relation,
											pg_class);

	/*
	 * create the index relation's relcache entry and physical disk file. (If
	 * we fail further down, it's the smgr's responsibility to remove the disk
	 * file again.)
	 */
	indexRelation = heap_create(indexRelationName,
								namespaceId,
								tableSpaceId,
								indexRelationId,
								indexTupDesc,
								RELKIND_INDEX,
								shared_relation,
								allow_system_table_mods);

	Assert(indexRelationId == RelationGetRelid(indexRelation));

	/*
	 * Obtain exclusive lock on it.  Although no other backends can see it
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
	indexRelation->rd_rel->relkind = RELKIND_INDEX;
	indexRelation->rd_rel->relhasoids = false;

	/*
	 * store index's pg_class entry
	 */
	InsertPgClassTuple(pg_class, indexRelation,
					   RelationGetRelid(indexRelation),
					   reloptions);

	/* done with pg_class */
	heap_close(pg_class, RowExclusiveLock);

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
	AppendAttributeTuples(indexRelation, indexInfo->ii_NumIndexAttrs);

	/* ----------------
	 *	  update pg_index
	 *	  (append INDEX tuple)
	 *
	 *	  Note that this stows away a representation of "predicate".
	 *	  (Or, could define a rule to maintain the predicate) --Nels, Feb '92
	 * ----------------
	 */
	UpdateIndexRelation(indexRelationId, heapRelationId, indexInfo,
						classObjectId, coloptions, isprimary, !concurrent);

	/*
	 * Register constraint and dependencies for the index.
	 *
	 * If the index is from a CONSTRAINT clause, construct a pg_constraint
	 * entry. The index is then linked to the constraint, which in turn is
	 * linked to the table.  If it's not a CONSTRAINT, make the dependency
	 * directly on the table.
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

		if (isconstraint)
		{
			char		constraintType;
			Oid			conOid;

			if (isprimary)
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
				elog(ERROR, "constraints cannot have index expressions");

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
										   NULL,
										   NULL,
										   NULL,
										   0,
										   ' ',
										   ' ',
										   ' ',
										   InvalidOid,	/* no associated index */
										   NULL,		/* no check constraint */
										   NULL,
										   NULL);

			referenced.classId = ConstraintRelationId;
			referenced.objectId = conOid;
			referenced.objectSubId = 0;

			recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
		}
		else
		{
			bool		have_simple_col = false;

			/* Create auto dependencies on simply-referenced columns */
			for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
			{
				if (indexInfo->ii_KeyAttrNumbers[i] != 0)
				{
					referenced.classId = RelationRelationId;
					referenced.objectId = heapRelationId;
					referenced.objectSubId = indexInfo->ii_KeyAttrNumbers[i];

					recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

					have_simple_col = true;
				}
			}

			/*
			 * It's possible for an index to not depend on any columns of the
			 * table at all, in which case we need to give it a dependency on
			 * the table as a whole; else it won't get dropped when the table
			 * is dropped.	This edge case is not totally useless; for
			 * example, a unique index on a constant expression can serve to
			 * prevent a table from containing more than one row.
			 */
			if (!have_simple_col &&
			 !contain_vars_of_level((Node *) indexInfo->ii_Expressions, 0) &&
				!contain_vars_of_level((Node *) indexInfo->ii_Predicate, 0))
			{
				referenced.classId = RelationRelationId;
				referenced.objectId = heapRelationId;
				referenced.objectSubId = 0;

				recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
			}
		}

		/* Store dependency on operator classes */
		for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
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
	 * In bootstrap mode, we have to fill in the index strategy structure with
	 * information from the catalogs.  If we aren't bootstrapping, then the
	 * relcache entry has already been rebuilt thanks to sinval update during
	 * CommandCounterIncrement.
	 */
	if (IsBootstrapProcessingMode())
		RelationInitIndexAccessInfo(indexRelation);
	else
		Assert(indexRelation->rd_indexcxt != NULL);

	/*
	 * If this is bootstrap (initdb) time, then we don't actually fill in the
	 * index yet.  We'll be creating more indexes and classes later, so we
	 * delay filling them in until just before we're done with bootstrapping.
	 * Similarly, if the caller specified skip_build then filling the index is
	 * delayed till later (ALTER TABLE can save work in some cases with this).
	 * Otherwise, we call the AM routine that constructs the index.
	 */
	if (IsBootstrapProcessingMode())
	{
		index_register(heapRelationId, indexRelationId, indexInfo);
	}
	else if (skip_build)
	{
		/*
		 * Caller is responsible for filling the index later on.  However,
		 * we'd better make sure that the heap relation is correctly marked as
		 * having an index.
		 */
		index_update_stats(heapRelation,
						   true,
						   isprimary,
						   InvalidOid,
						   heapRelation->rd_rel->reltuples);
		/* Make the above update visible */
		CommandCounterIncrement();
	}
	else
	{
		index_build(heapRelation, indexRelation, indexInfo, isprimary);
	}

	/*
	 * Close the heap and index; but we keep the locks that we acquired above
	 * until end of transaction.
	 */
	index_close(indexRelation, NoLock);
	heap_close(heapRelation, NoLock);

	return indexRelationId;
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
	bool		hasexprs;

	/*
	 * To drop an index safely, we must grab exclusive lock on its parent
	 * table; otherwise there could be other backends using the index!
	 * Exclusive lock on the index alone is insufficient because another
	 * backend might be in the midst of devising a query plan that will use
	 * the index.  The parser and planner take care to hold an appropriate
	 * lock on the parent table while working, but having them hold locks on
	 * all the indexes too seems overly expensive.	We do grab exclusive lock
	 * on the index too, just to be safe. Both locks must be held till end of
	 * transaction, else other backends will still see this index in pg_index.
	 */
	heapId = IndexGetRelation(indexId);
	userHeapRelation = heap_open(heapId, AccessExclusiveLock);

	userIndexRelation = index_open(indexId, AccessExclusiveLock);

	/*
	 * Schedule physical removal of the file
	 */
	RelationOpenSmgr(userIndexRelation);
	smgrscheduleunlink(userIndexRelation->rd_smgr,
					   userIndexRelation->rd_istemp);

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
	indexRelation = heap_open(IndexRelationId, RowExclusiveLock);

	tuple = SearchSysCache(INDEXRELID,
						   ObjectIdGetDatum(indexId),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", indexId);

	hasexprs = !heap_attisnull(tuple, Anum_pg_index_indexprs);

	simple_heap_delete(indexRelation, &tuple->t_self);

	ReleaseSysCache(tuple);
	heap_close(indexRelation, RowExclusiveLock);

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
	 * We are presently too lazy to attempt to compute the new correct value
	 * of relhasindex (the next VACUUM will fix it if necessary). So there is
	 * no need to update the pg_class tuple for the owning relation. But we
	 * must send out a shared-cache-inval notice on the owning relation to
	 * ensure other backends update their relcache lists of indexes.
	 */
	CacheInvalidateRelcache(userHeapRelation);

	/*
	 * Close owning rel, but keep lock
	 */
	heap_close(userHeapRelation, NoLock);
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
		ii->ii_KeyAttrNumbers[i] = indexStruct->indkey.values[i];

	/* fetch any expressions needed for expressional indexes */
	ii->ii_Expressions = RelationGetIndexExpressions(index);
	ii->ii_ExpressionsState = NIL;

	/* fetch index predicate if any */
	ii->ii_Predicate = RelationGetIndexPredicate(index);
	ii->ii_PredicateState = NIL;

	/* other info */
	ii->ii_Unique = indexStruct->indisunique;
	ii->ii_ReadyForInserts = indexStruct->indisready;

	/* initialize index-build state to default */
	ii->ii_Concurrent = false;
	ii->ii_BrokenHotChain = false;

	return ii;
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
 * its input arrays values[] and isnull[].	This is because the index AM
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
		indexInfo->ii_ExpressionsState = (List *)
			ExecPrepareExpr((Expr *) indexInfo->ii_Expressions,
							estate);
		/* Check caller has set up context correctly */
		Assert(GetPerTupleExprContext(estate)->ecxt_scantuple == slot);
	}
	indexpr_item = list_head(indexInfo->ii_ExpressionsState);

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
											   &isNull,
											   NULL);
			indexpr_item = lnext(indexpr_item);
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
 * relation after CREATE INDEX or REINDEX.	Its rather bizarre API is designed
 * to ensure we can do all the necessary work in just one update.
 *
 * hasindex: set relhasindex to this value
 * isprimary: if true, set relhaspkey true; else no change
 * reltoastidxid: if not InvalidOid, set reltoastidxid to this value;
 *		else no change
 * reltuples: set reltuples to this value
 *
 * relpages is also updated (using RelationGetNumberOfBlocks()).
 *
 * NOTE: an important side-effect of this operation is that an SI invalidation
 * message is sent out to all backends --- including me --- causing relcache
 * entries to be flushed or updated with the new data.	This must happen even
 * if we find that no change is needed in the pg_class row.  When updating
 * a heap entry, this ensures that other backends find out about the new
 * index.  When updating an index, it's important because some index AMs
 * expect a relcache flush to occur after REINDEX.
 */
static void
index_update_stats(Relation rel, bool hasindex, bool isprimary,
				   Oid reltoastidxid, double reltuples)
{
	BlockNumber relpages = RelationGetNumberOfBlocks(rel);
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
	 * its pg_class row because CatalogUpdateIndexes might not know about all
	 * the indexes yet (see reindex_relation).
	 *
	 * 3. Because we execute CREATE INDEX with just share lock on the parent
	 * rel (to allow concurrent index creations), an ordinary update could
	 * suffer a tuple-concurrently-updated failure against another CREATE
	 * INDEX committing at about the same time.  We can avoid that by having
	 * them both do nontransactional updates (we assume they will both be
	 * trying to change the pg_class row to the same thing, so it doesn't
	 * matter which goes first).
	 *
	 * 4. Even with just a single CREATE INDEX, there's a risk factor because
	 * someone else might be trying to open the rel while we commit, and this
	 * creates a race condition as to whether he will see both or neither of
	 * the pg_class row versions as valid.	Again, a non-transactional update
	 * avoids the risk.  It is indeterminate which state of the row the other
	 * process will see, but it doesn't matter (if he's only taking
	 * AccessShareLock, then it's not critical that he see relhasindex true).
	 *
	 * It is safe to use a non-transactional update even though our
	 * transaction could still fail before committing.	Setting relhasindex
	 * true is safe even if there are no indexes (VACUUM will eventually fix
	 * it), and of course the relpages and reltuples counts are correct (or at
	 * least more so than the old values) regardless.
	 */

	pg_class = heap_open(RelationRelationId, RowExclusiveLock);

	/*
	 * Make a copy of the tuple to update.	Normally we use the syscache, but
	 * we can't rely on that during bootstrap or while reindexing pg_class
	 * itself.
	 */
	if (IsBootstrapProcessingMode() ||
		ReindexIsProcessingHeap(RelationRelationId))
	{
		/* don't assume syscache will work */
		HeapScanDesc pg_class_scan;
		ScanKeyData key[1];

		ScanKeyInit(&key[0],
					ObjectIdAttributeNumber,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));

		pg_class_scan = heap_beginscan(pg_class, SnapshotNow, 1, key);
		tuple = heap_getnext(pg_class_scan, ForwardScanDirection);
		tuple = heap_copytuple(tuple);
		heap_endscan(pg_class_scan);
	}
	else
	{
		/* normal case, use syscache */
		tuple = SearchSysCacheCopy(RELOID,
								   ObjectIdGetDatum(relid),
								   0, 0, 0);
	}

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for relation %u", relid);
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/* Apply required updates, if any, to copied tuple */

	dirty = false;
	if (rd_rel->relhasindex != hasindex)
	{
		rd_rel->relhasindex = hasindex;
		dirty = true;
	}
	if (isprimary)
	{
		if (!rd_rel->relhaspkey)
		{
			rd_rel->relhaspkey = true;
			dirty = true;
		}
	}
	if (OidIsValid(reltoastidxid))
	{
		Assert(rd_rel->relkind == RELKIND_TOASTVALUE);
		if (rd_rel->reltoastidxid != reltoastidxid)
		{
			rd_rel->reltoastidxid = reltoastidxid;
			dirty = true;
		}
	}
	if (rd_rel->reltuples != (float4) reltuples)
	{
		rd_rel->reltuples = (float4) reltuples;
		dirty = true;
	}
	if (rd_rel->relpages != (int32) relpages)
	{
		rd_rel->relpages = (int32) relpages;
		dirty = true;
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

	heap_close(pg_class, RowExclusiveLock);
}

/*
 * setNewRelfilenode		- assign a new relfilenode value to the relation
 *
 * Caller must already hold exclusive lock on the relation.
 *
 * The relation is marked with relfrozenxid=freezeXid (InvalidTransactionId
 * must be passed for indexes)
 */
void
setNewRelfilenode(Relation relation, TransactionId freezeXid)
{
	Oid			newrelfilenode;
	RelFileNode newrnode;
	SMgrRelation srel;
	Relation	pg_class;
	HeapTuple	tuple;
	Form_pg_class rd_rel;

	/* Can't change relfilenode for nailed tables (indexes ok though) */
	Assert(!relation->rd_isnailed ||
		   relation->rd_rel->relkind == RELKIND_INDEX);
	/* Can't change for shared tables or indexes */
	Assert(!relation->rd_rel->relisshared);
	/* Indexes must have Invalid frozenxid; other relations must not */
	Assert((relation->rd_rel->relkind == RELKIND_INDEX &&
			freezeXid == InvalidTransactionId) ||
		   TransactionIdIsNormal(freezeXid));

	/* Allocate a new relfilenode */
	newrelfilenode = GetNewRelFileNode(relation->rd_rel->reltablespace,
									   relation->rd_rel->relisshared,
									   NULL);

	/*
	 * Find the pg_class tuple for the given relation.	This is not used
	 * during bootstrap, so okay to use heap_update always.
	 */
	pg_class = heap_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(RelationGetRelid(relation)),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for relation %u",
			 RelationGetRelid(relation));
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/* create another storage file. Is it a little ugly ? */
	/* NOTE: any conflict in relfilenode value will be caught here */
	newrnode = relation->rd_node;
	newrnode.relNode = newrelfilenode;

	srel = smgropen(newrnode);
	smgrcreate(srel, relation->rd_istemp, false);
	smgrclose(srel);

	/* schedule unlinking old relfilenode */
	RelationOpenSmgr(relation);
	smgrscheduleunlink(relation->rd_smgr, relation->rd_istemp);

	/* update the pg_class row */
	rd_rel->relfilenode = newrelfilenode;
	rd_rel->relpages = 0;		/* it's empty until further notice */
	rd_rel->reltuples = 0;
	rd_rel->relfrozenxid = freezeXid;
	simple_heap_update(pg_class, &tuple->t_self, tuple);
	CatalogUpdateIndexes(pg_class, tuple);

	heap_freetuple(tuple);

	heap_close(pg_class, RowExclusiveLock);

	/* Make sure the relfilenode change is visible */
	CommandCounterIncrement();

	/* Mark the rel as having a new relfilenode in current transaction */
	RelationCacheMarkNewRelfilenode(relation);
}


/*
 * index_build - invoke access-method-specific index build procedure
 *
 * On entry, the index's catalog entries are valid, and its physical disk
 * file has been created but is empty.	We call the AM-specific build
 * procedure to fill in the index contents.  We then update the pg_class
 * entries of the index and heap relation as needed, using statistics
 * returned by ambuild as well as data passed by the caller.
 *
 * Note: when reindexing an existing index, isprimary can be false;
 * the index is already properly marked and need not be re-marked.
 *
 * Note: before Postgres 8.2, the passed-in heap and index Relations
 * were automatically closed by this routine.  This is no longer the case.
 * The caller opened 'em, and the caller should close 'em.
 */
void
index_build(Relation heapRelation,
			Relation indexRelation,
			IndexInfo *indexInfo,
			bool isprimary)
{
	RegProcedure procedure;
	IndexBuildResult *stats;
	Oid			save_userid;
	bool		save_secdefcxt;

	/*
	 * sanity checks
	 */
	Assert(RelationIsValid(indexRelation));
	Assert(PointerIsValid(indexRelation->rd_am));

	procedure = indexRelation->rd_am->ambuild;
	Assert(RegProcedureIsValid(procedure));

	/*
	 * Switch to the table owner's userid, so that any index functions are
	 * run as that user.
	 */
	GetUserIdAndContext(&save_userid, &save_secdefcxt);
	SetUserIdAndContext(heapRelation->rd_rel->relowner, true);

	/*
	 * Call the access method's build procedure
	 */
	stats = (IndexBuildResult *)
		DatumGetPointer(OidFunctionCall3(procedure,
										 PointerGetDatum(heapRelation),
										 PointerGetDatum(indexRelation),
										 PointerGetDatum(indexInfo)));
	Assert(PointerIsValid(stats));

	/* Restore userid */
	SetUserIdAndContext(save_userid, save_secdefcxt);

	/*
	 * If we found any potentially broken HOT chains, mark the index as not
	 * being usable until the current transaction is below the event horizon.
	 * See src/backend/access/heap/README.HOT for discussion.
	 */
	if (indexInfo->ii_BrokenHotChain)
	{
		Oid			indexId = RelationGetRelid(indexRelation);
		Relation	pg_index;
		HeapTuple	indexTuple;
		Form_pg_index indexForm;

		pg_index = heap_open(IndexRelationId, RowExclusiveLock);

		indexTuple = SearchSysCacheCopy(INDEXRELID,
										ObjectIdGetDatum(indexId),
										0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexId);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		indexForm->indcheckxmin = true;
		simple_heap_update(pg_index, &indexTuple->t_self, indexTuple);
		CatalogUpdateIndexes(pg_index, indexTuple);

		heap_freetuple(indexTuple);
		heap_close(pg_index, RowExclusiveLock);
	}

	/*
	 * Update heap and index pg_class rows
	 */
	index_update_stats(heapRelation,
					   true,
					   isprimary,
					   (heapRelation->rd_rel->relkind == RELKIND_TOASTVALUE) ?
					   RelationGetRelid(indexRelation) : InvalidOid,
					   stats->heap_tuples);

	index_update_stats(indexRelation,
					   false,
					   false,
					   InvalidOid,
					   stats->index_tuples);

	/* Make the updated versions visible */
	CommandCounterIncrement();
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
 *
 * A side effect is to set indexInfo->ii_BrokenHotChain to true if we detect
 * any potentially broken HOT chains.  Currently, we set this if there are
 * any RECENTLY_DEAD entries in a HOT chain, without trying very hard to
 * detect whether they're really incompatible with the chain tip.
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
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples;
	List	   *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;
	TransactionId OldestXmin;
	BlockNumber root_blkno = InvalidBlockNumber;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.	Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation));

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = (List *)
		ExecPrepareExpr((Expr *) indexInfo->ii_Predicate,
						estate);

	/*
	 * Prepare for scan of the base relation.  In a normal index build, we use
	 * SnapshotAny because we must retrieve all tuples and do our own time
	 * qual checks (because we have to index RECENTLY_DEAD tuples). In a
	 * concurrent build, we take a regular MVCC snapshot and index whatever's
	 * live according to that.	During bootstrap we just use SnapshotNow.
	 */
	if (IsBootstrapProcessingMode())
	{
		snapshot = SnapshotNow;
		OldestXmin = InvalidTransactionId;		/* not used */
	}
	else if (indexInfo->ii_Concurrent)
	{
		snapshot = CopySnapshot(GetTransactionSnapshot());
		OldestXmin = InvalidTransactionId;		/* not used */
	}
	else
	{
		snapshot = SnapshotAny;
		/* okay to ignore lazy VACUUMs here */
		OldestXmin = GetOldestXmin(heapRelation->rd_rel->relisshared, true);
	}

	scan = heap_beginscan(heapRelation, /* relation */
						  snapshot,		/* seeself */
						  0,	/* number of keys */
						  NULL);	/* scan key */

	reltuples = 0;

	/*
	 * Scan all tuples in the base relation.
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool		tupleIsAlive;

		CHECK_FOR_INTERRUPTS();

		/*
		 * When dealing with a HOT-chain of updated tuples, we want to index
		 * the values of the live tuple (if any), but index it under the TID
		 * of the chain's root tuple.  This approach is necessary to preserve
		 * the HOT-chain structure in the heap. So we need to be able to find
		 * the root item offset for every tuple that's in a HOT-chain.  When
		 * first reaching a new page of the relation, call
		 * heap_get_root_tuples() to build a map of root item offsets on the
		 * page.
		 *
		 * It might look unsafe to use this information across buffer
		 * lock/unlock.  However, we hold ShareLock on the table so no
		 * ordinary insert/update/delete should occur; and we hold pin on the
		 * buffer continuously while visiting the page, so no pruning
		 * operation can occur either.
		 *
		 * Note the implied assumption that there is no more than one live
		 * tuple per HOT-chain ...
		 */
		if (scan->rs_cblock != root_blkno)
		{
			Page		page = BufferGetPage(scan->rs_cbuf);

			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(page, root_offsets);
			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			root_blkno = scan->rs_cblock;
		}

		if (snapshot == SnapshotAny)
		{
			/* do our own time qual check */
			bool		indexIt;

	recheck:

			/*
			 * We could possibly get away with not locking the buffer here,
			 * since caller should hold ShareLock on the relation, but let's
			 * be conservative about it.  (This remark is still correct even
			 * with HOT-pruning: our pin on the buffer prevents pruning.)
			 */
			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

			switch (HeapTupleSatisfiesVacuum(heapTuple->t_data, OldestXmin,
											 scan->rs_cbuf))
			{
				case HEAPTUPLE_DEAD:
					/* Definitely dead, we can ignore it */
					indexIt = false;
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_LIVE:
					/* Normal case, index and unique-check it */
					indexIt = true;
					tupleIsAlive = true;
					break;
				case HEAPTUPLE_RECENTLY_DEAD:

					/*
					 * If tuple is recently deleted then we must index it
					 * anyway to preserve MVCC semantics.  (Pre-existing
					 * transactions could try to use the index after we finish
					 * building it, and may need to see such tuples.)
					 *
					 * However, if it was HOT-updated then we must only index
					 * the live tuple at the end of the HOT-chain.	Since this
					 * breaks semantics for pre-existing snapshots, mark the
					 * index as unusable for them.
					 *
					 * If we've already decided that the index will be unsafe
					 * for old snapshots, we may as well stop indexing
					 * recently-dead tuples, since there's no longer any
					 * point.
					 */
					if (HeapTupleIsHotUpdated(heapTuple))
					{
						indexIt = false;
						/* mark the index as unsafe for old snapshots */
						indexInfo->ii_BrokenHotChain = true;
					}
					else if (indexInfo->ii_BrokenHotChain)
						indexIt = false;
					else
						indexIt = true;
					/* In any case, exclude the tuple from unique-checking */
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:

					/*
					 * Since caller should hold ShareLock or better, we should
					 * not see any tuples inserted by open transactions ---
					 * unless it's our own transaction. (Consider INSERT
					 * followed by CREATE INDEX within a transaction.)	An
					 * exception occurs when reindexing a system catalog,
					 * because we often release lock on system catalogs before
					 * committing.	In that case we wait for the inserting
					 * transaction to finish and check again.  (We could do
					 * that on user tables too, but since the case is not
					 * expected it seems better to throw an error.)
					 */
					if (!TransactionIdIsCurrentTransactionId(
								  HeapTupleHeaderGetXmin(heapTuple->t_data)))
					{
						if (!IsSystemRelation(heapRelation))
							elog(ERROR, "concurrent insert in progress");
						else
						{
							/*
							 * Must drop the lock on the buffer before we wait
							 */
							TransactionId xwait = HeapTupleHeaderGetXmin(heapTuple->t_data);

							LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							XactLockTableWait(xwait);
							goto recheck;
						}
					}

					/*
					 * We must index such tuples, since if the index build
					 * commits then they're good.
					 */
					indexIt = true;
					tupleIsAlive = true;
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:

					/*
					 * Since caller should hold ShareLock or better, we should
					 * not see any tuples deleted by open transactions ---
					 * unless it's our own transaction. (Consider DELETE
					 * followed by CREATE INDEX within a transaction.)	An
					 * exception occurs when reindexing a system catalog,
					 * because we often release lock on system catalogs before
					 * committing.	In that case we wait for the deleting
					 * transaction to finish and check again.  (We could do
					 * that on user tables too, but since the case is not
					 * expected it seems better to throw an error.)
					 */
					Assert(!(heapTuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI));
					if (!TransactionIdIsCurrentTransactionId(
								  HeapTupleHeaderGetXmax(heapTuple->t_data)))
					{
						if (!IsSystemRelation(heapRelation))
							elog(ERROR, "concurrent delete in progress");
						else
						{
							/*
							 * Must drop the lock on the buffer before we wait
							 */
							TransactionId xwait = HeapTupleHeaderGetXmax(heapTuple->t_data);

							LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							XactLockTableWait(xwait);
							goto recheck;
						}
					}

					/*
					 * Otherwise, we have to treat these tuples just like
					 * RECENTLY_DELETED ones.
					 */
					if (HeapTupleIsHotUpdated(heapTuple))
					{
						indexIt = false;
						/* mark the index as unsafe for old snapshots */
						indexInfo->ii_BrokenHotChain = true;
					}
					else if (indexInfo->ii_BrokenHotChain)
						indexIt = false;
					else
						indexIt = true;
					/* In any case, exclude the tuple from unique-checking */
					tupleIsAlive = false;
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					indexIt = tupleIsAlive = false;		/* keep compiler quiet */
					break;
			}

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
		ExecStoreTuple(heapTuple, slot, InvalidBuffer, false);

		/*
		 * In a partial index, discard tuples that don't satisfy the
		 * predicate.
		 */
		if (predicate != NIL)
		{
			if (!ExecQual(predicate, econtext, false))
				continue;
		}

		/*
		 * For the current heap tuple, extract all the attributes we use in
		 * this index, and note which are null.  This also performs evaluation
		 * of any expressions needed.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/*
		 * You'd think we should go ahead and build the index tuple here, but
		 * some index AMs want to do further processing on the data first.	So
		 * pass the values[] and isnull[] arrays, instead.
		 */

		if (HeapTupleIsHeapOnly(heapTuple))
		{
			/*
			 * For a heap-only tuple, pretend its TID is that of the root. See
			 * src/backend/access/heap/README.HOT for discussion.
			 */
			HeapTupleData rootTuple;
			OffsetNumber offnum;

			rootTuple = *heapTuple;
			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_self);

			Assert(OffsetNumberIsValid(root_offsets[offnum - 1]));

			ItemPointerSetOffsetNumber(&rootTuple.t_self,
									   root_offsets[offnum - 1]);

			/* Call the AM's callback routine to process the tuple */
			callback(indexRelation, &rootTuple, values, isnull, tupleIsAlive,
					 callback_state);
		}
		else
		{
			/* Call the AM's callback routine to process the tuple */
			callback(indexRelation, heapTuple, values, isnull, tupleIsAlive,
					 callback_state);
		}
	}

	heap_endscan(scan);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NIL;

	return reltuples;
}


/*
 * validate_index - support code for concurrent index builds
 *
 * We do a concurrent index build by first inserting the catalog entry for the
 * index via index_create(), marking it not indisready and not indisvalid.
 * Then we commit our transaction and start a new one, then we wait for all
 * transactions that could have been modifying the table to terminate.	Now
 * we know that any subsequently-started transactions will see the index and
 * honor its constraints on HOT updates; so while existing HOT-chains might
 * be broken with respect to the index, no currently live tuple will have an
 * incompatible HOT update done to it.	We now build the index normally via
 * index_build(), while holding a weak lock that allows concurrent
 * insert/update/delete.  Also, we index only tuples that are valid
 * as of the start of the scan (see IndexBuildHeapScan), whereas a normal
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
 * transactions that could have been modifying the table to terminate.	Now
 * we know that any subsequently-started transactions will see the index and
 * insert their new tuples into it.  We then take a new reference snapshot
 * which is passed to validate_index().  Any tuples that are valid according
 * to this snap, but are not in the index, must be added to the index.
 * (Any tuples committed live after the snap will be inserted into the
 * index by their originating transaction.	Any tuples committed dead before
 * the snap need not be indexed, because we will wait out all transactions
 * that might care about them before we mark the index valid.)
 *
 * validate_index() works by first gathering all the TIDs currently in the
 * index, using a bulkdelete callback that just stores the TIDs and doesn't
 * ever say "delete it".  (This should be faster than a plain indexscan;
 * also, not all index AMs support full-index indexscan.)  Then we sort the
 * TIDs, and finally scan the table doing a "merge join" against the TID list
 * to see which tuples are missing from the index.	Thus we will ensure that
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
 * necessary to be sure there are none left with a serializable snapshot
 * older than the reference (and hence possibly able to see tuples we did
 * not index).	Then we mark the index "indisvalid" and commit.  Subsequent
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
	v_i_state	state;
	Oid			save_userid;
	bool		save_secdefcxt;

	/* Open and lock the parent heap relation */
	heapRelation = heap_open(heapId, ShareUpdateExclusiveLock);
	/* And the target index relation */
	indexRelation = index_open(indexId, RowExclusiveLock);

	/*
	 * Fetch info needed for index_insert.	(You might think this should be
	 * passed in from DefineIndex, but its copy is long gone due to having
	 * been built in a previous transaction.)
	 */
	indexInfo = BuildIndexInfo(indexRelation);

	/* mark build is concurrent just for consistency */
	indexInfo->ii_Concurrent = true;

	/*
	 * Switch to the table owner's userid, so that any index functions are
	 * run as that user.
	 */
	GetUserIdAndContext(&save_userid, &save_secdefcxt);
	SetUserIdAndContext(heapRelation->rd_rel->relowner, true);

	/*
	 * Scan the index and gather up all the TIDs into a tuplesort object.
	 */
	ivinfo.index = indexRelation;
	ivinfo.vacuum_full = false;
	ivinfo.message_level = DEBUG2;
	ivinfo.num_heap_tuples = -1;
	ivinfo.strategy = NULL;

	state.tuplesort = tuplesort_begin_datum(TIDOID,
											TIDLessOperator, false,
											maintenance_work_mem,
											false);
	state.htups = state.itups = state.tups_inserted = 0;

	(void) index_bulk_delete(&ivinfo, NULL,
							 validate_index_callback, (void *) &state);

	/* Execute the sort */
	tuplesort_performsort(state.tuplesort);

	/*
	 * Now scan the heap and "merge" it with the index
	 */
	validate_index_heapscan(heapRelation,
							indexRelation,
							indexInfo,
							snapshot,
							&state);

	/* Done with tuplesort object */
	tuplesort_end(state.tuplesort);

	elog(DEBUG2,
		 "validate_index found %.0f heap tuples, %.0f index tuples; inserted %.0f missing tuples",
		 state.htups, state.itups, state.tups_inserted);

	/* Restore userid */
	SetUserIdAndContext(save_userid, save_secdefcxt);

	/* Close rels, but keep locks */
	index_close(indexRelation, NoLock);
	heap_close(heapRelation, NoLock);
}

/*
 * validate_index_callback - bulkdelete callback to collect the index TIDs
 */
static bool
validate_index_callback(ItemPointer itemptr, void *opaque)
{
	v_i_state  *state = (v_i_state *) opaque;

	tuplesort_putdatum(state->tuplesort, PointerGetDatum(itemptr), false);
	state->itups += 1;
	return false;				/* never actually delete anything */
}

/*
 * validate_index_heapscan - second table scan for concurrent index build
 *
 * This has much code in common with IndexBuildHeapScan, but it's enough
 * different that it seems cleaner to have two routines not one.
 */
static void
validate_index_heapscan(Relation heapRelation,
						Relation indexRelation,
						IndexInfo *indexInfo,
						Snapshot snapshot,
						v_i_state *state)
{
	HeapScanDesc scan;
	HeapTuple	heapTuple;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	List	   *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	BlockNumber root_blkno = InvalidBlockNumber;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];
	bool		in_index[MaxHeapTuplesPerPage];

	/* state variables for the merge */
	ItemPointer indexcursor = NULL;
	bool		tuplesort_empty = false;

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.	Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation));

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = (List *)
		ExecPrepareExpr((Expr *) indexInfo->ii_Predicate,
						estate);

	/*
	 * Prepare for scan of the base relation.  We need just those tuples
	 * satisfying the passed-in reference snapshot.  We must disable syncscan
	 * here, because it's critical that we read from block zero forward to
	 * match the sorted TIDs.
	 */
	scan = heap_beginscan_strat(heapRelation,	/* relation */
								snapshot,		/* snapshot */
								0,				/* number of keys */
								NULL,			/* scan key */
								true,			/* buffer access strategy OK */
								false);			/* syncscan not OK */

	/*
	 * Scan all tuples matching the snapshot.
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		ItemPointer heapcursor = &heapTuple->t_self;
		ItemPointerData rootTuple;
		OffsetNumber root_offnum;

		CHECK_FOR_INTERRUPTS();

		state->htups += 1;

		/*
		 * As commented in IndexBuildHeapScan, we should index heap-only
		 * tuples under the TIDs of their root tuples; so when we advance onto
		 * a new heap page, build a map of root item offsets on the page.
		 *
		 * This complicates merging against the tuplesort output: we will
		 * visit the live tuples in order by their offsets, but the root
		 * offsets that we need to compare against the index contents might be
		 * ordered differently.  So we might have to "look back" within the
		 * tuplesort output, but only within the current page.	We handle that
		 * by keeping a bool array in_index[] showing all the
		 * already-passed-over tuplesort output TIDs of the current page. We
		 * clear that array here, when advancing onto a new heap page.
		 */
		if (scan->rs_cblock != root_blkno)
		{
			Page		page = BufferGetPage(scan->rs_cbuf);

			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(page, root_offsets);
			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			memset(in_index, 0, sizeof(in_index));

			root_blkno = scan->rs_cblock;
		}

		/* Convert actual tuple TID to root TID */
		rootTuple = *heapcursor;
		root_offnum = ItemPointerGetOffsetNumber(heapcursor);

		if (HeapTupleIsHeapOnly(heapTuple))
		{
			root_offnum = root_offsets[root_offnum - 1];
			Assert(OffsetNumberIsValid(root_offnum));
			ItemPointerSetOffsetNumber(&rootTuple, root_offnum);
		}

		/*
		 * "merge" by skipping through the index tuples until we find or pass
		 * the current root tuple.
		 */
		while (!tuplesort_empty &&
			   (!indexcursor ||
				ItemPointerCompare(indexcursor, &rootTuple) < 0))
		{
			Datum		ts_val;
			bool		ts_isnull;

			if (indexcursor)
			{
				/*
				 * Remember index items seen earlier on the current heap page
				 */
				if (ItemPointerGetBlockNumber(indexcursor) == root_blkno)
					in_index[ItemPointerGetOffsetNumber(indexcursor) - 1] = true;
				pfree(indexcursor);
			}

			tuplesort_empty = !tuplesort_getdatum(state->tuplesort, true,
												  &ts_val, &ts_isnull);
			Assert(tuplesort_empty || !ts_isnull);
			indexcursor = (ItemPointer) DatumGetPointer(ts_val);
		}

		/*
		 * If the tuplesort has overshot *and* we didn't see a match earlier,
		 * then this tuple is missing from the index, so insert it.
		 */
		if ((tuplesort_empty ||
			 ItemPointerCompare(indexcursor, &rootTuple) > 0) &&
			!in_index[root_offnum - 1])
		{
			MemoryContextReset(econtext->ecxt_per_tuple_memory);

			/* Set up for predicate or expression evaluation */
			ExecStoreTuple(heapTuple, slot, InvalidBuffer, false);

			/*
			 * In a partial index, discard tuples that don't satisfy the
			 * predicate.
			 */
			if (predicate != NIL)
			{
				if (!ExecQual(predicate, econtext, false))
					continue;
			}

			/*
			 * For the current heap tuple, extract all the attributes we use
			 * in this index, and note which are null.	This also performs
			 * evaluation of any expressions needed.
			 */
			FormIndexDatum(indexInfo,
						   slot,
						   estate,
						   values,
						   isnull);

			/*
			 * You'd think we should go ahead and build the index tuple here,
			 * but some index AMs want to do further processing on the data
			 * first. So pass the values[] and isnull[] arrays, instead.
			 */

			/*
			 * If the tuple is already committed dead, you might think we
			 * could suppress uniqueness checking, but this is no longer true
			 * in the presence of HOT, because the insert is actually a proxy
			 * for a uniqueness check on the whole HOT-chain.  That is, the
			 * tuple we have here could be dead because it was already
			 * HOT-updated, and if so the updating transaction will not have
			 * thought it should insert index entries.	The index AM will
			 * check the whole HOT-chain and correctly detect a conflict if
			 * there is one.
			 */

			index_insert(indexRelation,
						 values,
						 isnull,
						 &rootTuple,
						 heapRelation,
						 indexInfo->ii_Unique);

			state->tups_inserted += 1;
		}
	}

	heap_endscan(scan);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NIL;
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
				heapRelation,
				pg_index;
	Oid			heapId;
	bool		inplace;
	HeapTuple	indexTuple;
	Form_pg_index indexForm;

	/*
	 * Open and lock the parent heap relation.	ShareLock is sufficient since
	 * we only need to be sure no schema or data changes are going on.
	 */
	heapId = IndexGetRelation(indexId);
	heapRelation = heap_open(heapId, ShareLock);

	/*
	 * Open the target index relation and get an exclusive lock on it, to
	 * ensure that no one else is touching this particular index.
	 */
	iRel = index_open(indexId, AccessExclusiveLock);

	/*
	 * Don't allow reindex on temp tables of other backends ... their local
	 * buffer manager is not going to cope.
	 */
	if (isOtherTempNamespace(RelationGetNamespace(iRel)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot reindex temporary tables of other sessions")));

	/*
	 * Also check for active uses of the index in the current transaction;
	 * we don't want to reindex underneath an open indexscan.
	 */
	CheckTableNotInUse(iRel, "REINDEX INDEX");

	/*
	 * If it's a shared index, we must do inplace processing (because we have
	 * no way to update relfilenode in other databases).  Otherwise we can do
	 * it the normal transaction-safe way.
	 *
	 * Since inplace processing isn't crash-safe, we only allow it in a
	 * standalone backend.	(In the REINDEX TABLE and REINDEX DATABASE cases,
	 * the caller should have detected this.)
	 */
	inplace = iRel->rd_rel->relisshared;

	if (inplace && IsUnderPostmaster)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("shared index \"%s\" can only be reindexed in stand-alone mode",
						RelationGetRelationName(iRel))));

	PG_TRY();
	{
		IndexInfo  *indexInfo;

		/* Suppress use of the target index while rebuilding it */
		SetReindexProcessing(heapId, indexId);

		/* Fetch info needed for index_build */
		indexInfo = BuildIndexInfo(iRel);

		if (inplace)
		{
			/* Truncate the actual file (and discard buffers) */
			RelationTruncate(iRel, 0);
		}
		else
		{
			/*
			 * We'll build a new physical relation for the index.
			 */
			setNewRelfilenode(iRel, InvalidTransactionId);
		}

		/* Initialize the index and rebuild */
		/* Note: we do not need to re-establish pkey setting */
		index_build(heapRelation, iRel, indexInfo, false);
	}
	PG_CATCH();
	{
		/* Make sure flag gets cleared on error exit */
		ResetReindexProcessing();
		PG_RE_THROW();
	}
	PG_END_TRY();
	ResetReindexProcessing();

	/*
	 * If the index is marked invalid or not ready (ie, it's from a failed
	 * CREATE INDEX CONCURRENTLY), we can now mark it valid.  This allows
	 * REINDEX to be used to clean up in such cases.
	 */
	pg_index = heap_open(IndexRelationId, RowExclusiveLock);

	indexTuple = SearchSysCacheCopy(INDEXRELID,
									ObjectIdGetDatum(indexId),
									0, 0, 0);
	if (!HeapTupleIsValid(indexTuple))
		elog(ERROR, "cache lookup failed for index %u", indexId);
	indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

	if (!indexForm->indisvalid || !indexForm->indisready)
	{
		indexForm->indisvalid = true;
		indexForm->indisready = true;
		simple_heap_update(pg_index, &indexTuple->t_self, indexTuple);
		CatalogUpdateIndexes(pg_index, indexTuple);
	}
	heap_close(pg_index, RowExclusiveLock);

	/* Close rels, but keep locks */
	index_close(iRel, NoLock);
	heap_close(heapRelation, NoLock);
}

/*
 * reindex_relation - This routine is used to recreate all indexes
 * of a relation (and optionally its toast relation too, if any).
 *
 * Returns true if any indexes were rebuilt.  Note that a
 * CommandCounterIncrement will occur after each index rebuild.
 */
bool
reindex_relation(Oid relid, bool toast_too)
{
	Relation	rel;
	Oid			toast_relid;
	bool		is_pg_class;
	bool		result;
	List	   *indexIds,
			   *doneIndexes;
	ListCell   *indexId;

	/*
	 * Open and lock the relation.	ShareLock is sufficient since we only need
	 * to prevent schema and data changes in it.
	 */
	rel = heap_open(relid, ShareLock);

	toast_relid = rel->rd_rel->reltoastrelid;

	/*
	 * Get the list of index OIDs for this relation.  (We trust to the
	 * relcache to get this with a sequential scan if ignoring system
	 * indexes.)
	 */
	indexIds = RelationGetIndexList(rel);

	/*
	 * reindex_index will attempt to update the pg_class rows for the relation
	 * and index.  If we are processing pg_class itself, we want to make sure
	 * that the updates do not try to insert index entries into indexes we
	 * have not processed yet.	(When we are trying to recover from corrupted
	 * indexes, that could easily cause a crash.) We can accomplish this
	 * because CatalogUpdateIndexes will use the relcache's index list to know
	 * which indexes to update. We just force the index list to be only the
	 * stuff we've processed.
	 *
	 * It is okay to not insert entries into the indexes we have not processed
	 * yet because all of this is transaction-safe.  If we fail partway
	 * through, the updated rows are dead and it doesn't matter whether they
	 * have index entries.	Also, a new pg_class index will be created with an
	 * entry for its own pg_class row because we do setNewRelfilenode() before
	 * we do index_build().
	 *
	 * Note that we also clear pg_class's rd_oidindex until the loop is done,
	 * so that that index can't be accessed either.  This means we cannot
	 * safely generate new relation OIDs while in the loop; shouldn't be a
	 * problem.
	 */
	is_pg_class = (RelationGetRelid(rel) == RelationRelationId);
	doneIndexes = NIL;

	/* Reindex all the indexes. */
	foreach(indexId, indexIds)
	{
		Oid			indexOid = lfirst_oid(indexId);

		if (is_pg_class)
			RelationSetIndexList(rel, doneIndexes, InvalidOid);

		reindex_index(indexOid);

		CommandCounterIncrement();

		if (is_pg_class)
			doneIndexes = lappend_oid(doneIndexes, indexOid);
	}

	if (is_pg_class)
		RelationSetIndexList(rel, indexIds, ClassOidIndexId);

	/*
	 * Close rel, but continue to hold the lock.
	 */
	heap_close(rel, NoLock);

	result = (indexIds != NIL);

	/*
	 * If the relation has a secondary toast rel, reindex that too while we
	 * still hold the lock on the master table.
	 */
	if (toast_too && OidIsValid(toast_relid))
		result |= reindex_relation(toast_relid, false);

	return result;
}
