/*-------------------------------------------------------------------------
 *
 * indexcmds.c
 *	  POSTGRES define and remove index code.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/indexcmds.c,v 1.114 2003/10/02 06:34:03 petere Exp $
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
				 errmsg("must specify at least one column")));
	if (numberOfAttributes > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot use more than %d columns in an index",
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
				 errmsg("\"%s\" is not a table",
						heapRelation->relname)));

	relationId = RelationGetRelid(rel);
	namespaceId = RelationGetNamespace(rel);

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
			aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
						   get_namespace_name(namespaceId));
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
		   errmsg("access method \"%s\" does not support unique indexes",
				  accessMethodName)));
	if (numberOfAttributes > 1 && !accessMethodForm->amcanmulticol)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support multicolumn indexes",
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
					 errmsg("index expressions and predicates may refer only to the table being indexed")));
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
	 * Check that all of the attributes in a primary key are marked as not
	 * null, otherwise attempt to ALTER TABLE .. SET NOT NULL
	 */
	if (primary)
	{
		List	   *keys;

		foreach(keys, attributeList)
		{
			IndexElem  *key = (IndexElem *) lfirst(keys);
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
				if (!((Form_pg_attribute) GETSTRUCT(atttuple))->attnotnull)
				{
					/*
					 * Try to make it NOT NULL.
					 *
					 * XXX: Shouldn't the ALTER TABLE .. SET NOT NULL cascade
					 * to child tables?  Currently, since the PRIMARY KEY
					 * itself doesn't cascade, we don't cascade the
					 * notnull constraint either; but this is pretty
					 * debatable.
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
	indexInfo->ii_Expressions = NIL;	/* for now */
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
				 errmsg("cannot use subquery in index predicate")));
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
						 errmsg("column \"%s\" does not exist",
								attribute->name)));
			attform = (Form_pg_attribute) GETSTRUCT(atttuple);
			indexInfo->ii_KeyAttrNumbers[attn] = attform->attnum;
			atttype = attform->atttypid;
			ReleaseSysCache(atttuple);
		}
		else if (attribute->expr && IsA(attribute->expr, Var))
		{
			/* Tricky tricky, he wrote (column) ... treat as simple attr */
			Var		   *var = (Var *) attribute->expr;

			indexInfo->ii_KeyAttrNumbers[attn] = var->varattno;
			atttype = get_atttype(relId, var->varattno);
		}
		else
		{
			/* Index expression */
			Assert(attribute->expr != NULL);
			indexInfo->ii_KeyAttrNumbers[attn] = 0;		/* marks expression */
			indexInfo->ii_Expressions = lappend(indexInfo->ii_Expressions,
												attribute->expr);
			atttype = exprType(attribute->expr);

			/*
			 * We don't currently support generation of an actual query
			 * plan for an index expression, only simple scalar
			 * expressions; hence these restrictions.
			 */
			if (contain_subplans(attribute->expr))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				   errmsg("cannot use subquery in index expression")));
			if (contain_agg_clause(attribute->expr))
				ereport(ERROR,
						(errcode(ERRCODE_GROUPING_ERROR),
					errmsg("cannot use aggregate function in index expression")));

			/*
			 * A expression using mutable functions is probably wrong,
			 * since if you aren't going to get the same result for the
			 * same data every time, it's not clear what the index entries
			 * mean at all.
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
	 * Release 7.0 removed network_ops, timespan_ops, and datetime_ops, so
	 * we ignore those opclass names so the default *_ops is used.	This
	 * can be removed in some later release.  bjm 2000/02/07
	 *
	 * Release 7.1 removes lztext_ops, so suppress that too for a while.  tgl
	 * 2000/07/30
	 *
	 * Release 7.2 renames timestamp_ops to timestamptz_ops, so suppress that
	 * too for awhile.	I'm starting to think we need a better approach.
	 * tgl 2000/10/01
	 */
	if (length(opclass) == 1)
	{
		char	   *claname = strVal(lfirst(opclass));

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
	 * opclasses visible in the current namespace search path.  (See also
	 * typcache.c, which applies the same logic, but over all opclasses.)
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
				 errmsg("\"%s\" is not an index",
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

	indOid = RangeVarGetRelid(indexRelation, false);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(indOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))		/* shouldn't happen */
		elog(ERROR, "cache lookup failed for relation %u", indOid);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index",
						indexRelation->relname)));

	/* Check permissions */
	if (!pg_class_ownercheck(indOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   indexRelation->relname);

	ReleaseSysCache(tuple);

	reindex_index(indOid);
}

/*
 * ReindexTable
 *		Recreate indexes of a table.
 */
void
ReindexTable(RangeVar *relation, bool force /* currently unused */ )
{
	Oid			heapOid;
	HeapTuple	tuple;

	heapOid = RangeVarGetRelid(relation, false);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(heapOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))		/* shouldn't happen */
		elog(ERROR, "cache lookup failed for relation %u", heapOid);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_RELATION &&
		((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_TOASTVALUE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						relation->relname)));

	/* Check permissions */
	if (!pg_class_ownercheck(heapOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   relation->relname);

	/* Can't reindex shared tables except in standalone mode */
	if (((Form_pg_class) GETSTRUCT(tuple))->relisshared && IsUnderPostmaster)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("shared table \"%s\" can only be reindexed in stand-alone mode",
						relation->relname)));

	ReleaseSysCache(tuple);

	if (!reindex_relation(heapOid))
		ereport(NOTICE,
				(errmsg("table \"%s\" has no indexes",
						relation->relname)));
}

/*
 * ReindexDatabase
 *		Recreate indexes of a database.
 *
 * To reduce the probability of deadlocks, each table is reindexed in a
 * separate transaction, so we can release the lock on it right away.
 */
void
ReindexDatabase(const char *dbname, bool force /* currently unused */,
				bool all)
{
	Relation	relationRelation;
	HeapScanDesc scan;
	HeapTuple	tuple;
	MemoryContext private_context;
	MemoryContext old;
	List	   *relids = NIL;

	AssertArg(dbname);

	if (strcmp(dbname, get_database_name(MyDatabaseId)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("can only reindex the currently open database")));

	if (!pg_database_ownercheck(MyDatabaseId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   dbname);

	/*
	 * We cannot run inside a user transaction block; if we were inside a
	 * transaction, then our commit- and start-transaction-command calls
	 * would not have the intended effect!
	 */
	PreventTransactionChain((void *) dbname, "REINDEX DATABASE");

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
	 * We always want to reindex pg_class first.  This ensures that if
	 * there is any corruption in pg_class' indexes, they will be fixed
	 * before we process any other tables.  This is critical because
	 * reindexing itself will try to update pg_class.
	 */
	old = MemoryContextSwitchTo(private_context);
	relids = lappendo(relids, RelOid_pg_class);
	MemoryContextSwitchTo(old);

	/*
	 * Scan pg_class to build a list of the relations we need to reindex.
	 *
	 * We only consider plain relations here (toast rels will be processed
	 * indirectly by reindex_relation).
	 */
	relationRelation = heap_openr(RelationRelationName, AccessShareLock);
	scan = heap_beginscan(relationRelation, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classtuple = (Form_pg_class) GETSTRUCT(tuple);

		if (classtuple->relkind != RELKIND_RELATION)
			continue;

		if (!all)				/* only system tables? */
		{
			if (!IsSystemClass(classtuple))
				continue;
		}

		if (IsUnderPostmaster)	/* silently ignore shared tables */
		{
			if (classtuple->relisshared)
				continue;
		}

		if (HeapTupleGetOid(tuple) == RelOid_pg_class)
			continue;			/* got it already */

		old = MemoryContextSwitchTo(private_context);
		relids = lappendo(relids, HeapTupleGetOid(tuple));
		MemoryContextSwitchTo(old);
	}
	heap_endscan(scan);
	heap_close(relationRelation, AccessShareLock);

	/* Now reindex each rel in a separate transaction */
	CommitTransactionCommand();
	while (relids)
	{
		Oid		relid = lfirsto(relids);

		StartTransactionCommand();
		SetQuerySnapshot();		/* might be needed for functions in
								 * indexes */
		if (reindex_relation(relid))
			ereport(NOTICE,
					(errmsg("table \"%s\" was reindexed",
							get_rel_name(relid))));
		CommitTransactionCommand();
		relids = lnext(relids);
	}
	StartTransactionCommand();

	MemoryContextDelete(private_context);
}
