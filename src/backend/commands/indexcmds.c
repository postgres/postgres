/*-------------------------------------------------------------------------
 *
 * indexcmds.c
 *	  POSTGRES define and remove index code.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/indexcmds.c,v 1.102 2003/07/20 21:56:32 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_proc.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* non-export function prototypes */
static void CheckPredicate(List *predList);
static void ComputeIndexAttrs(IndexInfo *indexInfo, Oid *classOidP,
			   List *attList,
			   Oid relId,
			   char *accessMethodName, Oid accessMethodId);
static Oid GetIndexOpClass(List *opclass, Oid attrType,
			   char *accessMethodName, Oid accessMethodId);
static Oid	GetDefaultOpClass(Oid attrType, Oid accessMethodId);

/*
 * DefineIndex
 *		Creates a new index.
 *
 * 'attributeList' is a list of IndexElem specifying columns and expressions
 *		to index on.
 * 'predicate' is the qual specified in the where clause.
 * 'rangetable' is needed to interpret the predicate.
 */
void
DefineIndex(RangeVar *heapRelation,
			char *indexRelationName,
			char *accessMethodName,
			List *attributeList,
			bool unique,
			bool primary,
			bool isconstraint,
			Expr *predicate,
			List *rangetable)
{
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			relationId;
	Oid			namespaceId;
	Relation	rel;
	HeapTuple	tuple;
	Form_pg_am	accessMethodForm;
	IndexInfo  *indexInfo;
	int			numberOfAttributes;
	List	   *cnfPred = NIL;

	/*
	 * count attributes in index
	 */
	numberOfAttributes = length(attributeList);
	if (numberOfAttributes <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("must specify at least one attribute")));
	if (numberOfAttributes > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot use more than %d attributes in an index",
						INDEX_MAX_KEYS)));

	/*
	 * Open heap relation, acquire a suitable lock on it, remember its OID
	 */
	rel = heap_openrv(heapRelation, ShareLock);

	/* Note: during bootstrap may see uncataloged relation */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_UNCATALOGED)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a table",
						heapRelation->relname)));

	relationId = RelationGetRelid(rel);
	namespaceId = RelationGetNamespace(rel);

	if (!IsBootstrapProcessingMode() &&
		IsSystemRelation(rel) &&
		!IndexesAreActive(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INDEXES_DEACTIVATED),
				 errmsg("existing indexes are inactive"),
				 errhint("REINDEX the table first.")));

	heap_close(rel, NoLock);

	/*
	 * Verify we (still) have CREATE rights in the rel's namespace.
	 * (Presumably we did when the rel was created, but maybe not
	 * anymore.) Skip check if bootstrapping, since permissions machinery
	 * may not be working yet.
	 */
	if (!IsBootstrapProcessingMode())
	{
		AclResult	aclresult;

		aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(),
										  ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_namespace_name(namespaceId));
	}

	/*
	 * look up the access method, verify it can handle the requested
	 * features
	 */
	tuple = SearchSysCache(AMNAME,
						   PointerGetDatum(accessMethodName),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						accessMethodName)));
	accessMethodId = HeapTupleGetOid(tuple);
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);

	if (unique && !accessMethodForm->amcanunique)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support UNIQUE indexes",
						accessMethodName)));
	if (numberOfAttributes > 1 && !accessMethodForm->amcanmulticol)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support multi-column indexes",
						accessMethodName)));

	ReleaseSysCache(tuple);

	/*
	 * If a range table was created then check that only the base rel is
	 * mentioned.
	 */
	if (rangetable != NIL)
	{
		if (length(rangetable) != 1 || getrelid(1, rangetable) != relationId)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("index expressions and predicates may refer only to the base relation")));
	}

	/*
	 * Convert the partial-index predicate from parsetree form to an
	 * implicit-AND qual expression, for easier evaluation at runtime.
	 * While we are at it, we reduce it to a canonical (CNF or DNF) form
	 * to simplify the task of proving implications.
	 */
	if (predicate)
	{
		cnfPred = canonicalize_qual((Expr *) copyObject(predicate), true);
		CheckPredicate(cnfPred);
	}

	/*
	 * Check that all of the attributes in a primary key are marked
	 * as not null, otherwise attempt to ALTER TABLE .. SET NOT NULL
	 */
	if (primary)
	{
		List   *keys;

		foreach(keys, attributeList)
		{
			IndexElem   *key = (IndexElem *) lfirst(keys);
			HeapTuple	atttuple;

			if (!key->name)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("primary keys cannot be expressions")));

			/* System attributes are never null, so no problem */
			if (SystemAttributeByName(key->name, rel->rd_rel->relhasoids))
				continue;

			atttuple = SearchSysCacheAttName(relationId, key->name);
			if (HeapTupleIsValid(atttuple))
			{
				if (! ((Form_pg_attribute) GETSTRUCT(atttuple))->attnotnull)
				{
					/*
					 * Try to make it NOT NULL.
					 *
					 * XXX: Shouldn't the ALTER TABLE .. SET NOT NULL cascade
					 * to child tables?  Currently, since the PRIMARY KEY
					 * itself doesn't cascade, we don't cascade the notnull
					 * constraint either; but this is pretty debatable.
					 */
					AlterTableAlterColumnSetNotNull(relationId, false,
													key->name);
				}
				ReleaseSysCache(atttuple);
			}
			else
			{
				/* This shouldn't happen if parser did its job ... */
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" named in key does not exist",
								key->name)));
			}
		}
	}

	/*
	 * Prepare arguments for index_create, primarily an IndexInfo
	 * structure
	 */
	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_NumIndexAttrs = numberOfAttributes;
	indexInfo->ii_Expressions = NIL; /* for now */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_Predicate = cnfPred;
	indexInfo->ii_PredicateState = NIL;
	indexInfo->ii_Unique = unique;

	classObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	ComputeIndexAttrs(indexInfo, classObjectId, attributeList,
					  relationId, accessMethodName, accessMethodId);

	index_create(relationId, indexRelationName,
				 indexInfo, accessMethodId, classObjectId,
				 primary, isconstraint, allowSystemTableMods);

	/*
	 * We update the relation's pg_class tuple even if it already has
	 * relhasindex = true.	This is needed to cause a shared-cache-inval
	 * message to be sent for the pg_class tuple, which will cause other
	 * backends to flush their relcache entries and in particular their
	 * cached lists of the indexes for this relation.
	 */
	setRelhasindex(relationId, true, primary, InvalidOid);
}


/*
 * CheckPredicate
 *		Checks that the given list of partial-index predicates is valid.
 *
 * This used to also constrain the form of the predicate to forms that
 * indxpath.c could do something with.	However, that seems overly
 * restrictive.  One useful application of partial indexes is to apply
 * a UNIQUE constraint across a subset of a table, and in that scenario
 * any evaluatable predicate will work.  So accept any predicate here
 * (except ones requiring a plan), and let indxpath.c fend for itself.
 */
static void
CheckPredicate(List *predList)
{
	/*
	 * We don't currently support generation of an actual query plan for a
	 * predicate, only simple scalar expressions; hence these
	 * restrictions.
	 */
	if (contain_subplans((Node *) predList))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use sub-select in index predicate")));
	if (contain_agg_clause((Node *) predList))
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate in index predicate")));

	/*
	 * A predicate using mutable functions is probably wrong, for the same
	 * reasons that we don't allow an index expression to use one.
	 */
	if (contain_mutable_functions((Node *) predList))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("functions in index predicate must be marked IMMUTABLE")));
}

static void
ComputeIndexAttrs(IndexInfo *indexInfo,
				  Oid *classOidP,
				  List *attList,	/* list of IndexElem's */
				  Oid relId,
				  char *accessMethodName,
				  Oid accessMethodId)
{
	List	   *rest;
	int			attn = 0;

	/*
	 * process attributeList
	 */
	foreach(rest, attList)
	{
		IndexElem  *attribute = (IndexElem *) lfirst(rest);
		Oid			atttype;

		if (attribute->name != NULL)
		{
			/* Simple index attribute */
			HeapTuple	atttuple;
			Form_pg_attribute attform;

			Assert(attribute->expr == NULL);
			atttuple = SearchSysCacheAttName(relId, attribute->name);
			if (!HeapTupleIsValid(atttuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("attribute \"%s\" does not exist",
								attribute->name)));
			attform = (Form_pg_attribute) GETSTRUCT(atttuple);
			indexInfo->ii_KeyAttrNumbers[attn] = attform->attnum;
			atttype = attform->atttypid;
			ReleaseSysCache(atttuple);
		}
		else if (attribute->expr && IsA(attribute->expr, Var))
		{
			/* Tricky tricky, he wrote (column) ... treat as simple attr */
			Var	   *var = (Var *) attribute->expr;

			indexInfo->ii_KeyAttrNumbers[attn] = var->varattno;
			atttype = get_atttype(relId, var->varattno);
		}
		else
		{
			/* Index expression */
			Assert(attribute->expr != NULL);
			indexInfo->ii_KeyAttrNumbers[attn] = 0;	/* marks expression */
			indexInfo->ii_Expressions = lappend(indexInfo->ii_Expressions,
												attribute->expr);
			atttype = exprType(attribute->expr);

			/*
			 * We don't currently support generation of an actual query plan
			 * for an index expression, only simple scalar expressions;
			 * hence these restrictions.
			 */
			if (contain_subplans(attribute->expr))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot use sub-select in index expression")));
			if (contain_agg_clause(attribute->expr))
				ereport(ERROR,
						(errcode(ERRCODE_GROUPING_ERROR),
						 errmsg("cannot use aggregate in index expression")));

			/*
			 * A expression using mutable functions is probably wrong,
			 * since if you aren't going to get the same result for the same
			 * data every time, it's not clear what the index entries mean at
			 * all.
			 */
			if (contain_mutable_functions(attribute->expr))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("functions in index expression must be marked IMMUTABLE")));
		}

		classOidP[attn] = GetIndexOpClass(attribute->opclass,
										  atttype,
										  accessMethodName,
										  accessMethodId);
		attn++;
	}
}

/*
 * Resolve possibly-defaulted operator class specification
 */
static Oid
GetIndexOpClass(List *opclass, Oid attrType,
				char *accessMethodName, Oid accessMethodId)
{
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	tuple;
	Oid			opClassId,
				opInputType;

	/*
	 * Release 7.0 removed network_ops, timespan_ops, and
	 * datetime_ops, so we ignore those opclass names
	 * so the default *_ops is used.  This can be
	 * removed in some later release.  bjm 2000/02/07
	 *
	 * Release 7.1 removes lztext_ops, so suppress that too
	 * for a while.  tgl 2000/07/30
	 *
	 * Release 7.2 renames timestamp_ops to timestamptz_ops,
	 * so suppress that too for awhile.  I'm starting to
	 * think we need a better approach.  tgl 2000/10/01
	 */
	if (length(opclass) == 1)
	{
		char   *claname = strVal(lfirst(opclass));

		if (strcmp(claname, "network_ops") == 0 ||
			strcmp(claname, "timespan_ops") == 0 ||
			strcmp(claname, "datetime_ops") == 0 ||
			strcmp(claname, "lztext_ops") == 0 ||
			strcmp(claname, "timestamp_ops") == 0)
			opclass = NIL;
	}

	if (opclass == NIL)
	{
		/* no operator class specified, so find the default */
		opClassId = GetDefaultOpClass(attrType, accessMethodId);
		if (!OidIsValid(opClassId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"%s\"",
							format_type_be(attrType), accessMethodName),
					 errhint("You must specify an operator class for the index or define a default operator class for the data type.")));
		return opClassId;
	}

	/*
	 * Specific opclass name given, so look up the opclass.
	 */

	/* deconstruct the name list */
	DeconstructQualifiedName(opclass, &schemaname, &opcname);

	if (schemaname)
	{
		/* Look in specific schema only */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname);
		tuple = SearchSysCache(CLAAMNAMENSP,
							   ObjectIdGetDatum(accessMethodId),
							   PointerGetDatum(opcname),
							   ObjectIdGetDatum(namespaceId),
							   0);
	}
	else
	{
		/* Unqualified opclass name, so search the search path */
		opClassId = OpclassnameGetOpcid(accessMethodId, opcname);
		if (!OidIsValid(opClassId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
							opcname, accessMethodName)));
		tuple = SearchSysCache(CLAOID,
							   ObjectIdGetDatum(opClassId),
							   0, 0, 0);
	}

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
						NameListToString(opclass), accessMethodName)));

	/*
	 * Verify that the index operator class accepts this datatype.	Note
	 * we will accept binary compatibility.
	 */
	opClassId = HeapTupleGetOid(tuple);
	opInputType = ((Form_pg_opclass) GETSTRUCT(tuple))->opcintype;

	if (!IsBinaryCoercible(attrType, opInputType))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("operator class \"%s\" does not accept data type %s",
						NameListToString(opclass), format_type_be(attrType))));

	ReleaseSysCache(tuple);

	return opClassId;
}

static Oid
GetDefaultOpClass(Oid attrType, Oid accessMethodId)
{
	OpclassCandidateList opclass;
	int			nexact = 0;
	int			ncompatible = 0;
	Oid			exactOid = InvalidOid;
	Oid			compatibleOid = InvalidOid;

	/* If it's a domain, look at the base type instead */
	attrType = getBaseType(attrType);

	/*
	 * We scan through all the opclasses available for the access method,
	 * looking for one that is marked default and matches the target type
	 * (either exactly or binary-compatibly, but prefer an exact match).
	 *
	 * We could find more than one binary-compatible match, in which case we
	 * require the user to specify which one he wants.	If we find more
	 * than one exact match, then someone put bogus entries in pg_opclass.
	 *
	 * The initial search is done by namespace.c so that we only consider
	 * opclasses visible in the current namespace search path.
	 */
	for (opclass = OpclassGetCandidates(accessMethodId);
		 opclass != NULL;
		 opclass = opclass->next)
	{
		if (opclass->opcdefault)
		{
			if (opclass->opcintype == attrType)
			{
				nexact++;
				exactOid = opclass->oid;
			}
			else if (IsBinaryCoercible(attrType, opclass->opcintype))
			{
				ncompatible++;
				compatibleOid = opclass->oid;
			}
		}
	}

	if (nexact == 1)
		return exactOid;
	if (nexact != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("there are multiple default operator classes for data type %s",
						format_type_be(attrType))));
	if (ncompatible == 1)
		return compatibleOid;

	return InvalidOid;
}

/*
 * RemoveIndex
 *		Deletes an index.
 */
void
RemoveIndex(RangeVar *relation, DropBehavior behavior)
{
	Oid			indOid;
	char		relkind;
	ObjectAddress object;

	indOid = RangeVarGetRelid(relation, false);
	relkind = get_rel_relkind(indOid);
	if (relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not an index",
						relation->relname)));

	object.classId = RelOid_pg_class;
	object.objectId = indOid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}

/*
 * ReindexIndex
 *		Recreate an index.
 */
void
ReindexIndex(RangeVar *indexRelation, bool force /* currently unused */ )
{
	Oid			indOid;
	HeapTuple	tuple;
	bool		overwrite;

	/* Choose in-place-or-not mode */
	overwrite = IsIgnoringSystemIndexes();

	indOid = RangeVarGetRelid(indexRelation, false);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(indOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple)) /* shouldn't happen */
		elog(ERROR, "cache lookup failed for relation %u", indOid);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not an index",
						indexRelation->relname)));

	if (IsSystemClass((Form_pg_class) GETSTRUCT(tuple)) &&
		!IsToastClass((Form_pg_class) GETSTRUCT(tuple)))
	{
		if (!allowSystemTableMods)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("\"%s\" is a system index",
							indexRelation->relname),
					 errhint("Do REINDEX in standalone postgres with -O -P options.")));
		if (!IsIgnoringSystemIndexes())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("\"%s\" is a system index",
							indexRelation->relname),
					 errhint("Do REINDEX in standalone postgres with -P -O options.")));
	}

	ReleaseSysCache(tuple);

	/*
	 * In-place REINDEX within a transaction block is dangerous, because
	 * if the transaction is later rolled back we have no way to undo
	 * truncation of the index's physical file.  Disallow it.
	 */
	if (overwrite)
		PreventTransactionChain((void *) indexRelation, "REINDEX");

	if (!reindex_index(indOid, force, overwrite))
		ereport(WARNING,
				(errmsg("index \"%s\" wasn't reindexed",
						indexRelation->relname)));
}

/*
 * ReindexTable
 *		Recreate indexes of a table.
 */
void
ReindexTable(RangeVar *relation, bool force)
{
	Oid			heapOid;
	char		relkind;

	heapOid = RangeVarGetRelid(relation, false);
	relkind = get_rel_relkind(heapOid);

	if (relkind != RELKIND_RELATION && relkind != RELKIND_TOASTVALUE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a table",
						relation->relname)));

	/*
	 * In-place REINDEX within a transaction block is dangerous, because
	 * if the transaction is later rolled back we have no way to undo
	 * truncation of the index's physical file.  Disallow it.
	 *
	 * XXX we assume that in-place reindex will only be done if
	 * IsIgnoringSystemIndexes() is true.
	 */
	if (IsIgnoringSystemIndexes())
		PreventTransactionChain((void *) relation, "REINDEX");

	if (!reindex_relation(heapOid, force))
		ereport(WARNING,
				(errmsg("table \"%s\" wasn't reindexed",
						relation->relname)));
}

/*
 * ReindexDatabase
 *		Recreate indexes of a database.
 */
void
ReindexDatabase(const char *dbname, bool force, bool all)
{
	Relation	relationRelation;
	HeapScanDesc scan;
	HeapTuple	tuple;
	MemoryContext private_context;
	MemoryContext old;
	int			relcnt,
				relalc,
				i,
				oncealc = 200;
	Oid		   *relids = (Oid *) NULL;

	AssertArg(dbname);

	if (strcmp(dbname, get_database_name(MyDatabaseId)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("can only reindex the currently open database")));

	if (!pg_database_ownercheck(MyDatabaseId, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied")));

	if (!allowSystemTableMods)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("REINDEX DATABASE must be done in standalone postgres with -O -P options")));
	if (!IsIgnoringSystemIndexes())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("REINDEX DATABASE must be done in standalone postgres with -P -O options")));

	/*
	 * We cannot run inside a user transaction block; if we were inside a
	 * transaction, then our commit- and start-transaction-command calls
	 * would not have the intended effect!
	 */
	PreventTransactionChain((void *) dbname, "REINDEX");

	/*
	 * Create a memory context that will survive forced transaction
	 * commits we do below.  Since it is a child of PortalContext, it will
	 * go away eventually even if we suffer an error; there's no need for
	 * special abort cleanup logic.
	 */
	private_context = AllocSetContextCreate(PortalContext,
											"ReindexDatabase",
											ALLOCSET_DEFAULT_MINSIZE,
											ALLOCSET_DEFAULT_INITSIZE,
											ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Scan pg_class to build a list of the relations we need to reindex.
	 */
	relationRelation = heap_openr(RelationRelationName, AccessShareLock);
	scan = heap_beginscan(relationRelation, SnapshotNow, 0, NULL);
	relcnt = relalc = 0;
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		char		relkind;

		if (!all)
		{
			if (!(IsSystemClass((Form_pg_class) GETSTRUCT(tuple)) &&
				  !IsToastClass((Form_pg_class) GETSTRUCT(tuple))))
				continue;
		}
		relkind = ((Form_pg_class) GETSTRUCT(tuple))->relkind;
		if (relkind == RELKIND_RELATION || relkind == RELKIND_TOASTVALUE)
		{
			old = MemoryContextSwitchTo(private_context);
			if (relcnt == 0)
			{
				relalc = oncealc;
				relids = palloc(sizeof(Oid) * relalc);
			}
			else if (relcnt >= relalc)
			{
				relalc *= 2;
				relids = repalloc(relids, sizeof(Oid) * relalc);
			}
			MemoryContextSwitchTo(old);
			relids[relcnt] = HeapTupleGetOid(tuple);
			relcnt++;
		}
	}
	heap_endscan(scan);
	heap_close(relationRelation, AccessShareLock);

	/* Now reindex each rel in a separate transaction */
	CommitTransactionCommand();
	for (i = 0; i < relcnt; i++)
	{
		StartTransactionCommand();
		SetQuerySnapshot();		/* might be needed for functions in indexes */
		if (reindex_relation(relids[i], force))
			ereport(NOTICE,
					(errmsg("relation %u was reindexed", relids[i])));
		CommitTransactionCommand();
	}
	StartTransactionCommand();

	MemoryContextDelete(private_context);
}
