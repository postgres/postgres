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
 *	  $Header: /cvsroot/pgsql/src/backend/commands/indexcmds.c,v 1.99 2003/05/14 03:26:01 tgl Exp $
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
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


#define IsFuncIndex(ATTR_LIST) (((IndexElem*)lfirst(ATTR_LIST))->funcname != NIL)

/* non-export function prototypes */
static void CheckPredicate(List *predList, List *rangeTable, Oid baseRelOid);
static void FuncIndexArgs(IndexInfo *indexInfo, Oid *classOidP,
			  IndexElem *funcIndex,
			  Oid relId,
			  char *accessMethodName, Oid accessMethodId);
static void NormIndexAttrs(IndexInfo *indexInfo, Oid *classOidP,
			   List *attList,
			   Oid relId,
			   char *accessMethodName, Oid accessMethodId);
static Oid GetAttrOpClass(IndexElem *attribute, Oid attrType,
			   char *accessMethodName, Oid accessMethodId);
static Oid	GetDefaultOpClass(Oid attrType, Oid accessMethodId);

/*
 * DefineIndex
 *		Creates a new index.
 *
 * 'attributeList' is a list of IndexElem specifying either a functional
 *		index or a list of attributes to index on.
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
		elog(ERROR, "DefineIndex: must specify at least one attribute");
	if (numberOfAttributes > INDEX_MAX_KEYS)
		elog(ERROR, "Cannot use more than %d attributes in an index",
			 INDEX_MAX_KEYS);

	/*
	 * Open heap relation, acquire a suitable lock on it, remember its OID
	 */
	rel = heap_openrv(heapRelation, ShareLock);

	/* Note: during bootstrap may see uncataloged relation */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_UNCATALOGED)
		elog(ERROR, "DefineIndex: relation \"%s\" is not a table",
			 heapRelation->relname);

	relationId = RelationGetRelid(rel);
	namespaceId = RelationGetNamespace(rel);

	if (!IsBootstrapProcessingMode() &&
		IsSystemRelation(rel) &&
		!IndexesAreActive(rel))
		elog(ERROR, "Existing indexes are inactive. REINDEX first");

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
		elog(ERROR, "DefineIndex: access method \"%s\" not found",
			 accessMethodName);
	accessMethodId = HeapTupleGetOid(tuple);
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);

	if (unique && !accessMethodForm->amcanunique)
		elog(ERROR, "DefineIndex: access method \"%s\" does not support UNIQUE indexes",
			 accessMethodName);
	if (numberOfAttributes > 1 && !accessMethodForm->amcanmulticol)
		elog(ERROR, "DefineIndex: access method \"%s\" does not support multi-column indexes",
			 accessMethodName);

	ReleaseSysCache(tuple);

	/*
	 * Convert the partial-index predicate from parsetree form to an
	 * implicit-AND qual expression, for easier evaluation at runtime.
	 * While we are at it, we reduce it to a canonical (CNF or DNF) form
	 * to simplify the task of proving implications.
	 */
	if (predicate)
	{
		cnfPred = canonicalize_qual((Expr *) copyObject(predicate), true);
		CheckPredicate(cnfPred, rangetable, relationId);
	}

	/*
	 * Check that all of the attributes in a primary key are marked
	 * as not null, otherwise attempt to ALTER TABLE .. SET NOT NULL
	 */
	if (primary && !IsFuncIndex(attributeList))
	{
		List   *keys;

		foreach(keys, attributeList)
		{
			IndexElem   *key = (IndexElem *) lfirst(keys);
			HeapTuple	atttuple;

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
				elog(ERROR, "DefineIndex: column \"%s\" named in key does not exist",
					 key->name);
			}
		}
	}

	/*
	 * Prepare arguments for index_create, primarily an IndexInfo
	 * structure
	 */
	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_Predicate = cnfPred;
	indexInfo->ii_PredicateState = NIL;
	indexInfo->ii_FuncOid = InvalidOid;
	indexInfo->ii_Unique = unique;

	if (IsFuncIndex(attributeList))
	{
		IndexElem  *funcIndex = (IndexElem *) lfirst(attributeList);
		int			nargs;

		/* Parser should have given us only one list item, but check */
		if (numberOfAttributes != 1)
			elog(ERROR, "Functional index can only have one attribute");

		nargs = length(funcIndex->args);
		if (nargs > INDEX_MAX_KEYS)
			elog(ERROR, "Index function can take at most %d arguments",
				 INDEX_MAX_KEYS);

		indexInfo->ii_NumIndexAttrs = 1;
		indexInfo->ii_NumKeyAttrs = nargs;

		classObjectId = (Oid *) palloc(sizeof(Oid));

		FuncIndexArgs(indexInfo, classObjectId, funcIndex,
					  relationId, accessMethodName, accessMethodId);
	}
	else
	{
		indexInfo->ii_NumIndexAttrs = numberOfAttributes;
		indexInfo->ii_NumKeyAttrs = numberOfAttributes;

		classObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));

		NormIndexAttrs(indexInfo, classObjectId, attributeList,
					   relationId, accessMethodName, accessMethodId);
	}

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
 *		Checks that the given list of partial-index predicates refer
 *		(via the given range table) only to the given base relation oid.
 *
 * This used to also constrain the form of the predicate to forms that
 * indxpath.c could do something with.	However, that seems overly
 * restrictive.  One useful application of partial indexes is to apply
 * a UNIQUE constraint across a subset of a table, and in that scenario
 * any evaluatable predicate will work.  So accept any predicate here
 * (except ones requiring a plan), and let indxpath.c fend for itself.
 */

static void
CheckPredicate(List *predList, List *rangeTable, Oid baseRelOid)
{
	if (length(rangeTable) != 1 || getrelid(1, rangeTable) != baseRelOid)
		elog(ERROR,
		 "Partial-index predicates may refer only to the base relation");

	/*
	 * We don't currently support generation of an actual query plan for a
	 * predicate, only simple scalar expressions; hence these
	 * restrictions.
	 */
	if (contain_subplans((Node *) predList))
		elog(ERROR, "Cannot use subselect in index predicate");
	if (contain_agg_clause((Node *) predList))
		elog(ERROR, "Cannot use aggregate in index predicate");

	/*
	 * A predicate using mutable functions is probably wrong, for the same
	 * reasons that we don't allow a functional index to use one.
	 */
	if (contain_mutable_functions((Node *) predList))
		elog(ERROR, "Functions in index predicate must be marked IMMUTABLE");
}


static void
FuncIndexArgs(IndexInfo *indexInfo,
			  Oid *classOidP,
			  IndexElem *funcIndex,
			  Oid relId,
			  char *accessMethodName,
			  Oid accessMethodId)
{
	Oid			argTypes[FUNC_MAX_ARGS];
	List	   *arglist;
	int			nargs = 0;
	int			i;
	FuncDetailCode fdresult;
	Oid			funcid;
	Oid			rettype;
	bool		retset;
	Oid		   *true_typeids;

	/*
	 * process the function arguments, which are a list of T_String
	 * (someday ought to allow more general expressions?)
	 *
	 * Note caller already checked that list is not too long.
	 */
	MemSet(argTypes, 0, sizeof(argTypes));

	foreach(arglist, funcIndex->args)
	{
		char	   *arg = strVal(lfirst(arglist));
		HeapTuple	tuple;
		Form_pg_attribute att;

		tuple = SearchSysCacheAttName(relId, arg);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "DefineIndex: column \"%s\" named in key does not exist", arg);
		att = (Form_pg_attribute) GETSTRUCT(tuple);
		indexInfo->ii_KeyAttrNumbers[nargs] = att->attnum;
		argTypes[nargs] = att->atttypid;
		ReleaseSysCache(tuple);
		nargs++;
	}

	/*
	 * Lookup the function procedure to get its OID and result type.
	 *
	 * We rely on parse_func.c to find the correct function in the possible
	 * presence of binary-compatible types.  However, parse_func may do
	 * too much: it will accept a function that requires run-time coercion
	 * of input types, and the executor is not currently set up to support
	 * that.  So, check to make sure that the selected function has
	 * exact-match or binary-compatible input types.
	 */
	fdresult = func_get_detail(funcIndex->funcname, funcIndex->args,
							   nargs, argTypes,
							   &funcid, &rettype, &retset,
							   &true_typeids);
	if (fdresult != FUNCDETAIL_NORMAL)
	{
		if (fdresult == FUNCDETAIL_AGGREGATE)
			elog(ERROR, "DefineIndex: functional index may not use an aggregate function");
		else if (fdresult == FUNCDETAIL_COERCION)
			elog(ERROR, "DefineIndex: functional index must use a real function, not a type coercion"
				 "\n\tTry specifying the index opclass you want to use, instead");
		else
			func_error("DefineIndex", funcIndex->funcname, nargs, argTypes,
					   NULL);
	}

	if (retset)
		elog(ERROR, "DefineIndex: cannot index on a function returning a set");

	for (i = 0; i < nargs; i++)
	{
		if (!IsBinaryCoercible(argTypes[i], true_typeids[i]))
			func_error("DefineIndex", funcIndex->funcname, nargs, true_typeids,
					   "Index function must be binary-compatible with table datatype");
	}

	/*
	 * Require that the function be marked immutable.  Using a mutable
	 * function for a functional index is highly questionable, since if
	 * you aren't going to get the same result for the same data every
	 * time, it's not clear what the index entries mean at all.
	 */
	if (func_volatile(funcid) != PROVOLATILE_IMMUTABLE)
		elog(ERROR, "DefineIndex: index function must be marked IMMUTABLE");

	/* Process opclass, using func return type as default type */

	classOidP[0] = GetAttrOpClass(funcIndex, rettype,
								  accessMethodName, accessMethodId);

	/* OK, return results */

	indexInfo->ii_FuncOid = funcid;
	/* Need to do the fmgr function lookup now, too */
	fmgr_info(funcid, &indexInfo->ii_FuncInfo);
}

static void
NormIndexAttrs(IndexInfo *indexInfo,
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
		HeapTuple	atttuple;
		Form_pg_attribute attform;

		if (attribute->name == NULL)
			elog(ERROR, "missing attribute for define index");

		atttuple = SearchSysCacheAttName(relId, attribute->name);
		if (!HeapTupleIsValid(atttuple))
			elog(ERROR, "DefineIndex: attribute \"%s\" not found",
				 attribute->name);
		attform = (Form_pg_attribute) GETSTRUCT(atttuple);

		indexInfo->ii_KeyAttrNumbers[attn] = attform->attnum;

		classOidP[attn] = GetAttrOpClass(attribute, attform->atttypid,
									   accessMethodName, accessMethodId);

		ReleaseSysCache(atttuple);
		attn++;
	}
}

static Oid
GetAttrOpClass(IndexElem *attribute, Oid attrType,
			   char *accessMethodName, Oid accessMethodId)
{
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	tuple;
	Oid			opClassId,
				opInputType;

	if (attribute->opclass == NIL)
	{
		/* no operator class specified, so find the default */
		opClassId = GetDefaultOpClass(attrType, accessMethodId);
		if (!OidIsValid(opClassId))
			elog(ERROR, "data type %s has no default operator class for access method \"%s\""
				 "\n\tYou must specify an operator class for the index or define a"
				 "\n\tdefault operator class for the data type",
				 format_type_be(attrType), accessMethodName);
		return opClassId;
	}

	/*
	 * Specific opclass name given, so look up the opclass.
	 */

	/* deconstruct the name list */
	DeconstructQualifiedName(attribute->opclass, &schemaname, &opcname);

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
			elog(ERROR, "DefineIndex: operator class \"%s\" not supported by access method \"%s\"",
				 opcname, accessMethodName);
		tuple = SearchSysCache(CLAOID,
							   ObjectIdGetDatum(opClassId),
							   0, 0, 0);
	}

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "DefineIndex: operator class \"%s\" not supported by access method \"%s\"",
			 NameListToString(attribute->opclass), accessMethodName);

	/*
	 * Verify that the index operator class accepts this datatype.	Note
	 * we will accept binary compatibility.
	 */
	opClassId = HeapTupleGetOid(tuple);
	opInputType = ((Form_pg_opclass) GETSTRUCT(tuple))->opcintype;

	if (!IsBinaryCoercible(attrType, opInputType))
		elog(ERROR, "operator class \"%s\" does not accept data type %s",
		 NameListToString(attribute->opclass), format_type_be(attrType));

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
		elog(ERROR, "pg_opclass contains multiple default opclasses for data type %s",
			 format_type_be(attrType));
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
		elog(ERROR, "relation \"%s\" is of type \"%c\"",
			 relation->relname, relkind);

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
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "index \"%s\" does not exist", indexRelation->relname);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_INDEX)
		elog(ERROR, "relation \"%s\" is of type \"%c\"",
			 indexRelation->relname,
			 ((Form_pg_class) GETSTRUCT(tuple))->relkind);

	if (IsSystemClass((Form_pg_class) GETSTRUCT(tuple)) &&
		!IsToastClass((Form_pg_class) GETSTRUCT(tuple)))
	{
		if (!allowSystemTableMods)
			elog(ERROR, "\"%s\" is a system index. call REINDEX under standalone postgres with -O -P options",
				 indexRelation->relname);
		if (!IsIgnoringSystemIndexes())
			elog(ERROR, "\"%s\" is a system index. call REINDEX under standalone postgres with -P -O options",
				 indexRelation->relname);
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
		elog(WARNING, "index \"%s\" wasn't reindexed", indexRelation->relname);
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
		elog(ERROR, "relation \"%s\" is of type \"%c\"",
			 relation->relname, relkind);

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
		elog(WARNING, "table \"%s\" wasn't reindexed", relation->relname);
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

	if (strcmp(dbname, DatabaseName) != 0)
		elog(ERROR, "REINDEX DATABASE: Can be executed only on the currently open database.");

	if (!(superuser() || is_dbadmin(MyDatabaseId)))
		elog(ERROR, "REINDEX DATABASE: Permission denied.");

	if (!allowSystemTableMods)
		elog(ERROR, "must be called under standalone postgres with -O -P options");
	if (!IsIgnoringSystemIndexes())
		elog(ERROR, "must be called under standalone postgres with -P -O options");

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
		SetQuerySnapshot();		/* might be needed for functional index */
		if (reindex_relation(relids[i], force))
			elog(NOTICE, "relation %u was reindexed", relids[i]);
		CommitTransactionCommand();
	}
	StartTransactionCommand();

	MemoryContextDelete(private_context);
}
