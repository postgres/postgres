/*-------------------------------------------------------------------------
 *
 * indexcmds.c
 *	  POSTGRES define and remove index code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/indexcmds.c,v 1.60 2001/10/25 05:49:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/pg_opclass.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


#define IsFuncIndex(ATTR_LIST) (((IndexElem*)lfirst(ATTR_LIST))->args != NIL)

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
DefineIndex(char *heapRelationName,
			char *indexRelationName,
			char *accessMethodName,
			List *attributeList,
			bool unique,
			bool primary,
			Expr *predicate,
			List *rangetable)
{
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			relationId;
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
	 * compute heap relation id
	 */
	if ((relationId = RelnameFindRelid(heapRelationName)) == InvalidOid)
		elog(ERROR, "DefineIndex: relation \"%s\" not found",
			 heapRelationName);

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
	accessMethodId = tuple->t_data->t_oid;
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
	if (predicate != NULL && rangetable != NIL)
	{
		cnfPred = canonicalize_qual((Expr *) copyObject(predicate), true);
		fix_opids((Node *) cnfPred);
		CheckPredicate(cnfPred, rangetable, relationId);
	}

	if (!IsBootstrapProcessingMode() && IsSystemRelationName(heapRelationName) && !IndexesAreActive(relationId, false))
		elog(ERROR, "Existing indexes are inactive. REINDEX first");

	/*
	 * Prepare arguments for index_create, primarily an IndexInfo
	 * structure
	 */
	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_Predicate = cnfPred;
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
				 primary, allowSystemTableMods);

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
	 * A predicate using noncachable functions is probably wrong, for the
	 * same reasons that we don't allow a functional index to use one.
	 */
	if (contain_noncachable_functions((Node *) predList))
		elog(ERROR, "Cannot use non-cachable function in index predicate");
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

		tuple = SearchSysCache(ATTNAME,
							   ObjectIdGetDatum(relId),
							   PointerGetDatum(arg),
							   0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "DefineIndex: attribute \"%s\" not found", arg);
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
	fdresult = func_get_detail(funcIndex->name, funcIndex->args,
							   nargs, argTypes,
							   &funcid, &rettype, &retset,
							   &true_typeids);
	if (fdresult != FUNCDETAIL_NORMAL)
	{
		if (fdresult == FUNCDETAIL_COERCION)
			elog(ERROR, "DefineIndex: functional index must use a real function, not a type coercion"
				 "\n\tTry specifying the index opclass you want to use, instead");
		else
			func_error("DefineIndex", funcIndex->name, nargs, argTypes, NULL);
	}

	if (retset)
		elog(ERROR, "DefineIndex: cannot index on a function returning a set");

	for (i = 0; i < nargs; i++)
	{
		if (argTypes[i] != true_typeids[i] &&
			!IS_BINARY_COMPATIBLE(argTypes[i], true_typeids[i]))
			func_error("DefineIndex", funcIndex->name, nargs, argTypes,
					   "Index function must be binary-compatible with table datatype");
	}

	/*
	 * Require that the function be marked cachable.  Using a noncachable
	 * function for a functional index is highly questionable, since if
	 * you aren't going to get the same result for the same data every
	 * time, it's not clear what the index entries mean at all.
	 */
	if (!func_iscachable(funcid))
		elog(ERROR, "DefineIndex: index function must be marked iscachable");

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

		atttuple = SearchSysCache(ATTNAME,
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

		ReleaseSysCache(atttuple);
		attn++;
	}
}

static Oid
GetAttrOpClass(IndexElem *attribute, Oid attrType,
			   char *accessMethodName, Oid accessMethodId)
{
	HeapTuple	tuple;
	Oid			opClassId,
				opInputType;

	if (attribute->class == NULL)
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
	 * Find the index operator class and verify that it accepts this
	 * datatype.  Note we will accept binary compatibility.
	 */
	tuple = SearchSysCache(CLAAMNAME,
						   ObjectIdGetDatum(accessMethodId),
						   PointerGetDatum(attribute->class),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "DefineIndex: operator class \"%s\" not supported by access method \"%s\"",
			 attribute->class, accessMethodName);
	opClassId = tuple->t_data->t_oid;
	opInputType = ((Form_pg_opclass) GETSTRUCT(tuple))->opcintype;
	ReleaseSysCache(tuple);

	if (attrType != opInputType &&
		!IS_BINARY_COMPATIBLE(attrType, opInputType))
		elog(ERROR, "operator class \"%s\" does not accept data type %s",
			 attribute->class, format_type_be(attrType));

	return opClassId;
}

static Oid
GetDefaultOpClass(Oid attrType, Oid accessMethodId)
{
	Relation	relation;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	HeapTuple	tuple;
	int			nexact = 0;
	int			ncompatible = 0;
	Oid			exactOid = InvalidOid;
	Oid			compatibleOid = InvalidOid;

	/*
	 * We scan through all the opclasses available for the access method,
	 * looking for one that is marked default and matches the target type
	 * (either exactly or binary-compatibly, but prefer an exact match).
	 *
	 * We could find more than one binary-compatible match, in which case we
	 * require the user to specify which one he wants.	If we find more
	 * than one exact match, then someone put bogus entries in pg_opclass.
	 *
	 * We could use an indexscan here, but since pg_opclass is small and a
	 * scan on opcamid won't be very selective, the indexscan would
	 * probably actually be slower than heapscan.
	 */
	ScanKeyEntryInitialize(&entry[0], 0x0,
						   Anum_pg_opclass_opcamid,
						   F_OIDEQ,
						   ObjectIdGetDatum(accessMethodId));

	relation = heap_openr(OperatorClassRelationName, AccessShareLock);
	scan = heap_beginscan(relation, false, SnapshotNow, 1, entry);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		Form_pg_opclass opclass = (Form_pg_opclass) GETSTRUCT(tuple);

		if (opclass->opcdefault)
		{
			if (opclass->opcintype == attrType)
			{
				nexact++;
				exactOid = tuple->t_data->t_oid;
			}
			else if (IS_BINARY_COMPATIBLE(opclass->opcintype, attrType))
			{
				ncompatible++;
				compatibleOid = tuple->t_data->t_oid;
			}
		}
	}

	heap_endscan(scan);
	heap_close(relation, AccessShareLock);

	if (nexact == 1)
		return exactOid;
	if (nexact != 0)
		elog(ERROR, "pg_opclass contains multiple default opclasses for data tyype %s",
			 format_type_be(attrType));
	if (ncompatible == 1)
		return compatibleOid;

	return InvalidOid;
}

/*
 * RemoveIndex
 *		Deletes an index.
 *
 * Exceptions:
 *		BadArg if name is invalid.
 *		"ERROR" if index nonexistent.
 *		...
 */
void
RemoveIndex(char *name)
{
	HeapTuple	tuple;

	tuple = SearchSysCache(RELNAME,
						   PointerGetDatum(name),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "index \"%s\" does not exist", name);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_INDEX)
		elog(ERROR, "relation \"%s\" is of type \"%c\"",
			 name, ((Form_pg_class) GETSTRUCT(tuple))->relkind);

	index_drop(tuple->t_data->t_oid);

	ReleaseSysCache(tuple);
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
	bool		overwrite = false;

	/*
	 * REINDEX within a transaction block is dangerous, because if the
	 * transaction is later rolled back we have no way to undo truncation
	 * of the index's physical file.  Disallow it.
	 */
	if (IsTransactionBlock())
		elog(ERROR, "REINDEX cannot run inside a BEGIN/END block");

	tuple = SearchSysCache(RELNAME,
						   PointerGetDatum(name),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "index \"%s\" does not exist", name);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_INDEX)
		elog(ERROR, "relation \"%s\" is of type \"%c\"",
			 name, ((Form_pg_class) GETSTRUCT(tuple))->relkind);

	if (IsIgnoringSystemIndexes())
		overwrite = true;
	if (!reindex_index(tuple->t_data->t_oid, force, overwrite))
		elog(NOTICE, "index \"%s\" wasn't reindexed", name);

	ReleaseSysCache(tuple);
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

	/*
	 * REINDEX within a transaction block is dangerous, because if the
	 * transaction is later rolled back we have no way to undo truncation
	 * of the index's physical file.  Disallow it.
	 */
	if (IsTransactionBlock())
		elog(ERROR, "REINDEX cannot run inside a BEGIN/END block");

	tuple = SearchSysCache(RELNAME,
						   PointerGetDatum(name),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "table \"%s\" does not exist", name);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_RELATION)
		elog(ERROR, "relation \"%s\" is of type \"%c\"",
			 name, ((Form_pg_class) GETSTRUCT(tuple))->relkind);

	if (!reindex_relation(tuple->t_data->t_oid, force))
		elog(NOTICE, "table \"%s\" wasn't reindexed", name);

	ReleaseSysCache(tuple);
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

	/*
	 * We cannot run inside a user transaction block; if we were inside a
	 * transaction, then our commit- and start-transaction-command calls
	 * would not have the intended effect!
	 */
	if (IsTransactionBlock())
		elog(ERROR, "REINDEX DATABASE cannot run inside a BEGIN/END block");

	/*
	 * Create a memory context that will survive forced transaction
	 * commits we do below.  Since it is a child of QueryContext, it will
	 * go away eventually even if we suffer an error; there's no need for
	 * special abort cleanup logic.
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
			elog(NOTICE, "relation %u was reindexed", relids[i]);
		CommitTransactionCommand();
	}
	StartTransactionCommand();

	MemoryContextDelete(private_context);
}
