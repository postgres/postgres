/*-------------------------------------------------------------------------
 *
 * indexcmds.c
 *	  POSTGRES define, extend and remove index code.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/indexcmds.c,v 1.38 2000/09/06 14:15:16 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_database.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shadow.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"

#define IsFuncIndex(ATTR_LIST) (((IndexElem*)lfirst(ATTR_LIST))->args != NIL)

/* non-export function prototypes */
static void CheckPredicate(List *predList, List *rangeTable, Oid baseRelOid);
static void CheckPredExpr(Node *predicate, List *rangeTable, Oid baseRelOid);
static void CheckPredClause(Expr *predicate, List *rangeTable, Oid baseRelOid);
static void FuncIndexArgs(IndexInfo *indexInfo, Oid *classOidP,
						  IndexElem *funcIndex,
						  Oid relId,
						  char *accessMethodName, Oid accessMethodId);
static void NormIndexAttrs(IndexInfo *indexInfo, Oid *classOidP,
						   List *attList,
						   Oid relId,
						   char *accessMethodName, Oid accessMethodId);
static Oid	GetAttrOpClass(IndexElem *attribute, Oid attrType,
						   char *accessMethodName, Oid accessMethodId);
static char *GetDefaultOpClass(Oid atttypid);

/*
 * DefineIndex
 *		Creates a new index.
 *
 * 'attributeList' is a list of IndexElem specifying either a functional
 *		index or a list of attributes to index on.
 * 'parameterList' is a list of DefElem specified in the with clause.
 * 'predicate' is the qual specified in the where clause.
 * 'rangetable' is needed to interpret the predicate
 */
void
DefineIndex(char *heapRelationName,
			char *indexRelationName,
			char *accessMethodName,
			List *attributeList,
			List *parameterList,
			bool unique,
			bool primary,
			Expr *predicate,
			List *rangetable)
{
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			relationId;
	IndexInfo  *indexInfo;
	int			numberOfAttributes;
	HeapTuple	tuple;
	List	   *cnfPred = NIL;
	bool		lossy = false;
	List	   *pl;

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
	 * compute heap relation id
	 */
	if ((relationId = RelnameFindRelid(heapRelationName)) == InvalidOid)
		elog(ERROR, "DefineIndex: relation \"%s\" not found",
			 heapRelationName);

	/*
	 * compute access method id
	 */
	tuple = SearchSysCacheTuple(AMNAME,
								PointerGetDatum(accessMethodName),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "DefineIndex: access method \"%s\" not found",
			 accessMethodName);
	accessMethodId = tuple->t_data->t_oid;

	/*
	 * XXX Hardwired hacks to check for limitations on supported index types.
	 * We really ought to be learning this info from entries in the pg_am
	 * table, instead of having it wired in here!
	 */
	if (unique && accessMethodId != BTREE_AM_OID)
		elog(ERROR, "DefineIndex: unique indices are only available with the btree access method");

	if (numberOfAttributes > 1 && accessMethodId != BTREE_AM_OID)
		elog(ERROR, "DefineIndex: multi-column indices are only available with the btree access method");

	/*
	 * WITH clause reinstated to handle lossy indices. -- JMH, 7/22/96
	 */
	foreach(pl, parameterList)
	{
		DefElem    *param = (DefElem *) lfirst(pl);

		if (!strcasecmp(param->defname, "islossy"))
			lossy = true;
		else
			elog(NOTICE, "Unrecognized index attribute \"%s\" ignored",
				 param->defname);
	}

	/*
	 * Convert the partial-index predicate from parsetree form to plan
	 * form, so it can be readily evaluated during index creation. Note:
	 * "predicate" comes in as a list containing (1) the predicate itself
	 * (a where_clause), and (2) a corresponding range table.
	 *
	 * [(1) is 'predicate' and (2) is 'rangetable' now. - ay 10/94]
	 */
	if (predicate != NULL && rangetable != NIL)
	{
		cnfPred = cnfify((Expr *) copyObject(predicate), true);
		fix_opids((Node *) cnfPred);
		CheckPredicate(cnfPred, rangetable, relationId);
	}

	if (!IsBootstrapProcessingMode() && !IndexesAreActive(relationId, false))
		elog(ERROR, "Existing indexes are inactive. REINDEX first");

	/*
	 * Prepare arguments for index_create, primarily an IndexInfo structure
	 */
	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_Predicate = (Node *) cnfPred;
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

	index_create(heapRelationName, indexRelationName,
				 indexInfo, accessMethodId, classObjectId,
				 lossy, primary, allowSystemTableMods);

	/*
	 * We update the relation's pg_class tuple even if it already has
	 * relhasindex = true.  This is needed to cause a shared-cache-inval
	 * message to be sent for the pg_class tuple, which will cause other
	 * backends to flush their relcache entries and in particular their
	 * cached lists of the indexes for this relation.
	 */
	setRelhasindexInplace(relationId, true, false);
}


/*
 * ExtendIndex
 *		Extends a partial index.
 */
void
ExtendIndex(char *indexRelationName, Expr *predicate, List *rangetable)
{
	Relation	heapRelation;
	Relation	indexRelation;
	Oid			accessMethodId,
				indexId,
				relationId;
	HeapTuple	tuple;
	Form_pg_index index;
	List	   *cnfPred = NIL;
	IndexInfo  *indexInfo;
	Node	   *oldPred;

	/*
	 * Get index's relation id and access method id from pg_class
	 */
	tuple = SearchSysCacheTuple(RELNAME,
								PointerGetDatum(indexRelationName),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ExtendIndex: index \"%s\" not found",
			 indexRelationName);
	indexId = tuple->t_data->t_oid;
	accessMethodId = ((Form_pg_class) GETSTRUCT(tuple))->relam;

	/*
	 * Extract info from the pg_index tuple for the index
	 */
	tuple = SearchSysCacheTuple(INDEXRELID,
								ObjectIdGetDatum(indexId),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ExtendIndex: relation \"%s\" is not an index",
			 indexRelationName);
	index = (Form_pg_index) GETSTRUCT(tuple);
	Assert(index->indexrelid == indexId);
	relationId = index->indrelid;
	indexInfo = BuildIndexInfo(tuple);
	oldPred = indexInfo->ii_Predicate;

	if (oldPred == NULL)
		elog(ERROR, "ExtendIndex: \"%s\" is not a partial index",
			 indexRelationName);

	/*
	 * Convert the extension predicate from parsetree form to plan form,
	 * so it can be readily evaluated during index creation. Note:
	 * "predicate" comes in two parts (1) the predicate expression itself,
	 * and (2) a corresponding range table.
	 *
	 * XXX I think this code is broken --- index_build expects a single
	 * expression not a list --- tgl Jul 00
	 */
	if (rangetable != NIL)
	{
		cnfPred = cnfify((Expr *) copyObject(predicate), true);
		fix_opids((Node *) cnfPred);
		CheckPredicate(cnfPred, rangetable, relationId);
	}

	/* pass new predicate to index_build */
	indexInfo->ii_Predicate = (Node *) cnfPred;

	/* Open heap and index rels, and get suitable locks */
	heapRelation = heap_open(relationId, ShareLock);
	indexRelation = index_open(indexId);

	/* Obtain exclusive lock on it, just to be sure */
	LockRelation(indexRelation, AccessExclusiveLock);

	InitIndexStrategy(indexInfo->ii_NumIndexAttrs,
					  indexRelation, accessMethodId);

	index_build(heapRelation, indexRelation, indexInfo, oldPred);

	/* heap and index rels are closed as a side-effect of index_build */
}


/*
 * CheckPredicate
 *		Checks that the given list of partial-index predicates refer
 *		(via the given range table) only to the given base relation oid,
 *		and that they're in a form the planner can handle, i.e.,
 *		boolean combinations of "ATTR OP CONST" (yes, for now, the ATTR
 *		has to be on the left).
 */

static void
CheckPredicate(List *predList, List *rangeTable, Oid baseRelOid)
{
	List	   *item;

	foreach(item, predList)
		CheckPredExpr(lfirst(item), rangeTable, baseRelOid);
}

static void
CheckPredExpr(Node *predicate, List *rangeTable, Oid baseRelOid)
{
	List	   *clauses = NIL,
			   *clause;

	if (is_opclause(predicate))
	{
		CheckPredClause((Expr *) predicate, rangeTable, baseRelOid);
		return;
	}
	else if (or_clause(predicate) || and_clause(predicate))
		clauses = ((Expr *) predicate)->args;
	else
		elog(ERROR, "Unsupported partial-index predicate expression type");

	foreach(clause, clauses)
		CheckPredExpr(lfirst(clause), rangeTable, baseRelOid);
}

static void
CheckPredClause(Expr *predicate, List *rangeTable, Oid baseRelOid)
{
	Var		   *pred_var;
	Const	   *pred_const;

	pred_var = (Var *) get_leftop(predicate);
	pred_const = (Const *) get_rightop(predicate);

	if (!IsA(predicate->oper, Oper) ||
		!IsA(pred_var, Var) ||
		!IsA(pred_const, Const))
		elog(ERROR, "Unsupported partial-index predicate clause type");

	if (getrelid(pred_var->varno, rangeTable) != baseRelOid)
		elog(ERROR,
		 "Partial-index predicates may refer only to the base relation");
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

		tuple = SearchSysCacheTuple(ATTNAME,
									ObjectIdGetDatum(relId),
									PointerGetDatum(arg),
									0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "DefineIndex: attribute \"%s\" not found", arg);
		att = (Form_pg_attribute) GETSTRUCT(tuple);

		indexInfo->ii_KeyAttrNumbers[nargs] = att->attnum;
		argTypes[nargs] = att->atttypid;
		nargs++;
	}

	/* ----------------
	 * Lookup the function procedure to get its OID and result type.
	 *
	 * We rely on parse_func.c to find the correct function in the
	 * possible presence of binary-compatible types.  However, parse_func
	 * may do too much: it will accept a function that requires run-time
	 * coercion of input types, and the executor is not currently set up
	 * to support that.  So, check to make sure that the selected function
	 * has exact-match or binary-compatible input types.
	 * ----------------
	 */
	if (! func_get_detail(funcIndex->name, nargs, argTypes,
						  &funcid, &rettype, &retset, &true_typeids))
		func_error("DefineIndex", funcIndex->name, nargs, argTypes, NULL);

	if (retset)
		elog(ERROR, "DefineIndex: cannot index on a function returning a set");

	for (i = 0; i < nargs; i++)
	{
		if (argTypes[i] != true_typeids[i] &&
			! IS_BINARY_COMPATIBLE(argTypes[i], true_typeids[i]))
			func_error("DefineIndex", funcIndex->name, nargs, argTypes,
					   "Index function must be binary-compatible with table datatype");
	}

	/* Process opclass, using func return type as default type */

	classOidP[0] = GetAttrOpClass(funcIndex, rettype,
								  accessMethodName, accessMethodId);

	/* OK, return results */

	indexInfo->ii_FuncOid = funcid;
	/* Need to do the fmgr function lookup now, too */
	fmgr_info(funcid, & indexInfo->ii_FuncInfo);
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

		atttuple = SearchSysCacheTupleCopy(ATTNAME,
										   ObjectIdGetDatum(relId),
										PointerGetDatum(attribute->name),
										   0, 0);
		if (!HeapTupleIsValid(atttuple))
			elog(ERROR, "DefineIndex: attribute \"%s\" not found",
				 attribute->name);
		attform = (Form_pg_attribute) GETSTRUCT(atttuple);

		indexInfo->ii_KeyAttrNumbers[attn] = attform->attnum;

		classOidP[attn] = GetAttrOpClass(attribute, attform->atttypid,
										 accessMethodName, accessMethodId);

		heap_freetuple(atttuple);
		attn++;
	}
}

static Oid
GetAttrOpClass(IndexElem *attribute, Oid attrType,
			   char *accessMethodName, Oid accessMethodId)
{
	Relation	relation;
	HeapScanDesc scan;
	ScanKeyData entry[2];
	HeapTuple	tuple;
	Oid			opClassId,
				oprId;
	bool		doTypeCheck = true;

	if (attribute->class == NULL)
	{
		/* no operator class specified, so find the default */
		attribute->class = GetDefaultOpClass(attrType);
		if (attribute->class == NULL)
			elog(ERROR, "DefineIndex: type %s has no default operator class",
				 typeidTypeName(attrType));
		/* assume we need not check type compatibility */
		doTypeCheck = false;
	}

	tuple = SearchSysCacheTuple(CLANAME,
								PointerGetDatum(attribute->class),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "DefineIndex: opclass \"%s\" not found",
			 attribute->class);
	opClassId = tuple->t_data->t_oid;

	/*
	 * Assume the opclass is supported by this index access method
	 * if we can find at least one relevant entry in pg_amop.
	 */
	ScanKeyEntryInitialize(&entry[0], 0,
						   Anum_pg_amop_amopid,
						   F_OIDEQ,
						   ObjectIdGetDatum(accessMethodId));
	ScanKeyEntryInitialize(&entry[1], 0,
						   Anum_pg_amop_amopclaid,
						   F_OIDEQ,
						   ObjectIdGetDatum(opClassId));

	relation = heap_openr(AccessMethodOperatorRelationName, AccessShareLock);
	scan = heap_beginscan(relation, false, SnapshotNow, 2, entry);

	if (! HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		elog(ERROR, "DefineIndex: opclass \"%s\" not supported by access method \"%s\"",
			 attribute->class, accessMethodName);
	}

	oprId = ((Form_pg_amop) GETSTRUCT(tuple))->amopopr;

	heap_endscan(scan);
	heap_close(relation, AccessShareLock);

	/*
	 * Make sure the operators associated with this opclass actually accept
	 * the column data type.  This prevents possible coredumps caused by
	 * user errors like applying text_ops to an int4 column.  We will accept
	 * an opclass as OK if the operator's input datatype is binary-compatible
	 * with the actual column datatype.  Note we assume that all the operators
	 * associated with an opclass accept the same datatypes, so checking the
	 * first one we happened to find in the table is sufficient.
	 *
	 * If the opclass was the default for the datatype, assume we can skip
	 * this check --- that saves a few cycles in the most common case.
	 * If pg_opclass is wrong then we're probably screwed anyway...
	 */
	if (doTypeCheck)
	{
		tuple = SearchSysCacheTuple(OPEROID,
									ObjectIdGetDatum(oprId),
									0, 0, 0);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tuple);
			Oid		opInputType = (optup->oprkind == 'l') ?
				optup->oprright : optup->oprleft;

			if (attrType != opInputType &&
				! IS_BINARY_COMPATIBLE(attrType, opInputType))
				elog(ERROR, "DefineIndex: opclass \"%s\" does not accept datatype \"%s\"",
					 attribute->class, typeidTypeName(attrType));
		}
	}

	return opClassId;
}

static char *
GetDefaultOpClass(Oid atttypid)
{
	HeapTuple	tuple;

	tuple = SearchSysCacheTuple(CLADEFTYPE,
								ObjectIdGetDatum(atttypid),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		return NULL;

	return DatumGetCString(DirectFunctionCall1(nameout,
			NameGetDatum(&((Form_pg_opclass) GETSTRUCT(tuple))->opcname)));
}

/*
 * RemoveIndex
 *		Deletes an index.
 *
 * Exceptions:
 *		BadArg if name is invalid.
 *		"WARN" if index nonexistent.
 *		...
 */
void
RemoveIndex(char *name)
{
	HeapTuple	tuple;

	tuple = SearchSysCacheTuple(RELNAME,
								PointerGetDatum(name),
								0, 0, 0);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "index \"%s\" nonexistent", name);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_INDEX)
	{
		elog(ERROR, "relation \"%s\" is of type \"%c\"",
			 name,
			 ((Form_pg_class) GETSTRUCT(tuple))->relkind);
	}

	index_drop(tuple->t_data->t_oid);
}

/*
 * Reindex
 *		Recreate an index.
 *
 * Exceptions:
 *		"ERROR" if index nonexistent.
 *		...
 */
void
ReindexIndex(const char *name, bool force /* currently unused */ )
{
	HeapTuple	tuple;

	tuple = SearchSysCacheTuple(RELNAME,
								PointerGetDatum(name),
								0, 0, 0);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "index \"%s\" nonexistent", name);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_INDEX)
	{
		elog(ERROR, "relation \"%s\" is of type \"%c\"",
			 name,
			 ((Form_pg_class) GETSTRUCT(tuple))->relkind);
	}

	if (!reindex_index(tuple->t_data->t_oid, force))
		elog(NOTICE, "index '%s' wasn't reindexed", name);
}

/*
 * ReindexTable
 *		Recreate indexes of a table.
 *
 * Exceptions:
 *		"ERROR" if table nonexistent.
 *		...
 */
void
ReindexTable(const char *name, bool force)
{
	HeapTuple	tuple;

	tuple = SearchSysCacheTuple(RELNAME,
								PointerGetDatum(name),
								0, 0, 0);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "table \"%s\" nonexistent", name);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_RELATION)
	{
		elog(ERROR, "relation \"%s\" is of type \"%c\"",
			 name,
			 ((Form_pg_class) GETSTRUCT(tuple))->relkind);
	}

	if (!reindex_relation(tuple->t_data->t_oid, force))
		elog(NOTICE, "table '%s' wasn't reindexed", name);
}

/*
 * ReindexDatabase
 *		Recreate indexes of a database.
 *
 * Exceptions:
 *		"ERROR" if table nonexistent.
 *		...
 */
void
ReindexDatabase(const char *dbname, bool force, bool all)
{
	Relation	relation,
				relationRelation;
	HeapTuple	dbtuple,
				tuple;
	HeapScanDesc scan;
	int4		db_owner;
	Oid			db_id;
	ScanKeyData scankey;
	MemoryContext private_context;
	MemoryContext old;
	int			relcnt,
				relalc,
				i,
				oncealc = 200;
	Oid		   *relids = (Oid *) NULL;

	AssertArg(dbname);

	relation = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&scankey, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(dbname));
	scan = heap_beginscan(relation, 0, SnapshotNow, 1, &scankey);
	dbtuple = heap_getnext(scan, 0);
	if (!HeapTupleIsValid(dbtuple))
		elog(ERROR, "Database \"%s\" doesn't exist", dbname);
	db_id = dbtuple->t_data->t_oid;
	db_owner = ((Form_pg_database) GETSTRUCT(dbtuple))->datdba;
	heap_endscan(scan);
	heap_close(relation, NoLock);

	if (GetUserId() != db_owner && !superuser())
		elog(ERROR, "REINDEX DATABASE: Permission denied.");

	if (db_id != MyDatabaseId)
		elog(ERROR, "REINDEX DATABASE: Can be executed only on the currently open database.");

	/*
	 * We cannot run inside a user transaction block; if we were
	 * inside a transaction, then our commit- and
	 * start-transaction-command calls would not have the intended effect!
	 */
	if (IsTransactionBlock())
		elog(ERROR, "REINDEX DATABASE cannot run inside a BEGIN/END block");

	/*
	 * Create a memory context that will survive forced transaction commits
	 * we do below.  Since it is a child of QueryContext, it will go away
	 * eventually even if we suffer an error; there's no need for special
	 * abort cleanup logic.
	 */
	private_context = AllocSetContextCreate(QueryContext,
											"ReindexDatabase",
											ALLOCSET_DEFAULT_MINSIZE,
											ALLOCSET_DEFAULT_INITSIZE,
											ALLOCSET_DEFAULT_MAXSIZE);

	relationRelation = heap_openr(RelationRelationName, AccessShareLock);
	scan = heap_beginscan(relationRelation, false, SnapshotNow, 0, NULL);
	relcnt = relalc = 0;
	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		if (!all)
		{
			if (!IsSystemRelationName(NameStr(((Form_pg_class) GETSTRUCT(tuple))->relname)))
				continue;
			if (((Form_pg_class) GETSTRUCT(tuple))->relhasrules)
				continue;
		}
		if (((Form_pg_class) GETSTRUCT(tuple))->relkind == RELKIND_RELATION)
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
			relids[relcnt] = tuple->t_data->t_oid;
			relcnt++;
		}
	}
	heap_endscan(scan);
	heap_close(relationRelation, AccessShareLock);

	CommitTransactionCommand();
	for (i = 0; i < relcnt; i++)
	{
		StartTransactionCommand();
		if (reindex_relation(relids[i], force))
			elog(NOTICE, "relation %d was reindexed", relids[i]);
		CommitTransactionCommand();
	}
	StartTransactionCommand();

	MemoryContextDelete(private_context);
}
