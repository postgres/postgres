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
 *	  $Header: /cvsroot/pgsql/src/backend/commands/indexcmds.c,v 1.31 2000/06/17 23:41:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_database.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shadow.h"
#include "commands/defrem.h"
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
#include "miscadmin.h"			/* ReindexDatabase() */
#include "utils/portal.h"		/* ReindexDatabase() */
#include "catalog/catalog.h"	/* ReindexDatabase() */

#define IsFuncIndex(ATTR_LIST) (((IndexElem*)lfirst(ATTR_LIST))->args != NIL)

/* non-export function prototypes */
static void CheckPredicate(List *predList, List *rangeTable, Oid baseRelOid);
static void CheckPredExpr(Node *predicate, List *rangeTable, Oid baseRelOid);
static void CheckPredClause(Expr *predicate, List *rangeTable, Oid baseRelOid);
static void FuncIndexArgs(IndexElem *funcIndex, FuncIndexInfo *funcInfo,
						  AttrNumber *attNumP, Oid *opOidP, Oid relId,
						  char *accessMethodName, Oid accessMethodId);
static void NormIndexAttrs(List *attList, AttrNumber *attNumP,
						   Oid *opOidP, Oid relId,
						   char *accessMethodName, Oid accessMethodId);
static void ProcessAttrTypename(IndexElem *attribute,
					Oid defType, int32 defTypmod);
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
 * 'rangetable' is for the predicate
 *
 * Exceptions:
 *		XXX
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
	int			numberOfAttributes;
	AttrNumber *attributeNumberA;
	HeapTuple	tuple;
	FuncIndexInfo fInfo;
	List	   *cnfPred = NULL;
	bool		lossy = false;
	List	   *pl;

	/*
	 * count attributes
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
	{
		elog(ERROR, "DefineIndex: relation \"%s\" not found",
			 heapRelationName);
	}

	/*
	 * XXX Hardwired hacks to check for limitations on supported index types.
	 * We really ought to be learning this info from entries in the pg_am
	 * table, instead of having it wired in here!
	 */
	if (unique && strcmp(accessMethodName, "btree") != 0)
		elog(ERROR, "DefineIndex: unique indices are only available with the btree access method");

	if (numberOfAttributes > 1 && strcmp(accessMethodName, "btree") != 0)
		elog(ERROR, "DefineIndex: multi-column indices are only available with the btree access method");

	/*
	 * compute access method id
	 */
	tuple = SearchSysCacheTuple(AMNAME,
								PointerGetDatum(accessMethodName),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(ERROR, "DefineIndex: access method \"%s\" not found",
			 accessMethodName);
	}
	accessMethodId = tuple->t_data->t_oid;

	/*
	 * WITH clause reinstated to handle lossy indices. -- JMH, 7/22/96
	 */
	foreach(pl, parameterList)
	{
		DefElem    *param = (DefElem *) lfirst(pl);

		if (!strcasecmp(param->defname, "islossy"))
			lossy = TRUE;
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

	if (IsFuncIndex(attributeList))
	{
		IndexElem  *funcIndex = lfirst(attributeList);
		int			nargs;

		nargs = length(funcIndex->args);
		if (nargs > INDEX_MAX_KEYS)
			elog(ERROR, "Index function can take at most %d arguments",
				 INDEX_MAX_KEYS);

		FIsetnArgs(&fInfo, nargs);

		namestrcpy(&fInfo.funcName, funcIndex->name);

		attributeNumberA = (AttrNumber *) palloc(nargs *
											 sizeof attributeNumberA[0]);

		classObjectId = (Oid *) palloc(sizeof(Oid));

		FuncIndexArgs(funcIndex, &fInfo, attributeNumberA,
					  classObjectId, relationId,
					  accessMethodName, accessMethodId);

		index_create(heapRelationName, indexRelationName,
					 &fInfo, NULL,
					 accessMethodId, numberOfAttributes, attributeNumberA,
					 classObjectId,
					 (Node *) cnfPred,
					 lossy, unique, primary);
	}
	else
	{
		attributeNumberA = (AttrNumber *) palloc(numberOfAttributes *
											 sizeof attributeNumberA[0]);

		classObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));

		NormIndexAttrs(attributeList, attributeNumberA,
					   classObjectId, relationId,
					   accessMethodName, accessMethodId);

		index_create(heapRelationName, indexRelationName,
					 NULL, attributeList,
					 accessMethodId, numberOfAttributes, attributeNumberA,
					 classObjectId,
					 (Node *) cnfPred,
					 lossy, unique, primary);
	}

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
 *
 * Exceptions:
 *		XXX
 */
void
ExtendIndex(char *indexRelationName, Expr *predicate, List *rangetable)
{
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			indexId,
				relationId;
	Oid			indproc;
	int			numberOfAttributes;
	AttrNumber *attributeNumberA;
	HeapTuple	tuple;
	FuncIndexInfo fInfo;
	FuncIndexInfo *funcInfo = NULL;
	bool		unique;
	Form_pg_index index;
	Node	   *oldPred = NULL;
	List	   *cnfPred = NULL;
	PredInfo   *predInfo;
	Relation	heapRelation;
	Relation	indexRelation;
	int			i;

	/*
	 * compute index relation id and access method id
	 */
	tuple = SearchSysCacheTuple(RELNAME,
								PointerGetDatum(indexRelationName),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(ERROR, "ExtendIndex: index \"%s\" not found",
			 indexRelationName);
	}
	indexId = tuple->t_data->t_oid;
	accessMethodId = ((Form_pg_class) GETSTRUCT(tuple))->relam;

	/*
	 * find pg_index tuple
	 */
	tuple = SearchSysCacheTuple(INDEXRELID,
								ObjectIdGetDatum(indexId),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(ERROR, "ExtendIndex: relation \"%s\" is not an index",
			 indexRelationName);
	}

	/*
	 * Extract info from the pg_index tuple
	 */
	index = (Form_pg_index) GETSTRUCT(tuple);
	Assert(index->indexrelid == indexId);
	relationId = index->indrelid;
	indproc = index->indproc;
	unique = index->indisunique;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
	{
		if (index->indkey[i] == InvalidAttrNumber)
			break;
	}
	numberOfAttributes = i;

	if (VARSIZE(&index->indpred) != 0)
	{
		char	   *predString;

		predString = textout(&index->indpred);
		oldPred = stringToNode(predString);
		pfree(predString);
	}
	if (oldPred == NULL)
		elog(ERROR, "ExtendIndex: \"%s\" is not a partial index",
			 indexRelationName);

	/*
	 * Convert the extension predicate from parsetree form to plan form,
	 * so it can be readily evaluated during index creation. Note:
	 * "predicate" comes in as a list containing (1) the predicate itself
	 * (a where_clause), and (2) a corresponding range table.
	 */
	if (rangetable != NIL)
	{
		cnfPred = cnfify((Expr *) copyObject(predicate), true);
		fix_opids((Node *) cnfPred);
		CheckPredicate(cnfPred, rangetable, relationId);
	}

	/* make predInfo list to pass to index_build */
	predInfo = (PredInfo *) palloc(sizeof(PredInfo));
	predInfo->pred = (Node *) cnfPred;
	predInfo->oldPred = oldPred;

	attributeNumberA = (AttrNumber *) palloc(numberOfAttributes *
											 sizeof attributeNumberA[0]);
	classObjectId = (Oid *) palloc(numberOfAttributes * sizeof classObjectId[0]);


	for (i = 0; i < numberOfAttributes; i++)
	{
		attributeNumberA[i] = index->indkey[i];
		classObjectId[i] = index->indclass[i];
	}

	if (indproc != InvalidOid)
	{
		funcInfo = &fInfo;
		FIsetnArgs(funcInfo, numberOfAttributes);

		tuple = SearchSysCacheTuple(PROCOID,
									ObjectIdGetDatum(indproc),
									0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "ExtendIndex: index procedure %u not found",
				 indproc);

		namecpy(&(funcInfo->funcName),
				&(((Form_pg_proc) GETSTRUCT(tuple))->proname));

		FIsetProcOid(funcInfo, tuple->t_data->t_oid);
	}

	heapRelation = heap_open(relationId, ShareLock);
	indexRelation = index_open(indexId);

	InitIndexStrategy(numberOfAttributes, indexRelation, accessMethodId);

	index_build(heapRelation, indexRelation, numberOfAttributes,
				attributeNumberA, funcInfo, predInfo, unique);

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
FuncIndexArgs(IndexElem *funcIndex,
			  FuncIndexInfo *funcInfo,
			  AttrNumber *attNumP,
			  Oid *opOidP,
			  Oid relId,
			  char *accessMethodName,
			  Oid accessMethodId)
{
	List	   *rest;
	HeapTuple	tuple;
	Oid			retType;
	int			argn = 0;

	/*
	 * process the function arguments, which are a list of T_String
	 * (someday ought to allow more general expressions?)
	 */
	MemSet(funcInfo->arglist, 0, FUNC_MAX_ARGS * sizeof(Oid));

	foreach(rest, funcIndex->args)
	{
		char	   *arg = strVal(lfirst(rest));
		Form_pg_attribute att;

		tuple = SearchSysCacheTuple(ATTNAME,
									ObjectIdGetDatum(relId),
									PointerGetDatum(arg), 0, 0);

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "DefineIndex: attribute \"%s\" not found", arg);
		att = (Form_pg_attribute) GETSTRUCT(tuple);
		*attNumP++ = att->attnum;
		funcInfo->arglist[argn++] = att->atttypid;
	}

	/* ----------------
	 * Lookup the function procedure to get its OID and result type.
	 * ----------------
	 */
	tuple = SearchSysCacheTuple(PROCNAME,
								PointerGetDatum(FIgetname(funcInfo)),
								Int32GetDatum(FIgetnArgs(funcInfo)),
								PointerGetDatum(FIgetArglist(funcInfo)),
								0);

	if (!HeapTupleIsValid(tuple))
	{
		func_error("DefineIndex", FIgetname(funcInfo),
				   FIgetnArgs(funcInfo), FIgetArglist(funcInfo), NULL);
	}

	FIsetProcOid(funcInfo, tuple->t_data->t_oid);
	retType = ((Form_pg_proc) GETSTRUCT(tuple))->prorettype;

	/* Process type and opclass, using func return type as default */

	ProcessAttrTypename(funcIndex, retType, -1);

	*opOidP = GetAttrOpClass(funcIndex, retType,
							 accessMethodName, accessMethodId);
}

static void
NormIndexAttrs(List *attList,	/* list of IndexElem's */
			   AttrNumber *attNumP,
			   Oid *classOidP,
			   Oid relId,
			   char *accessMethodName,
			   Oid accessMethodId)
{
	List	   *rest;

	/*
	 * process attributeList
	 */
	foreach(rest, attList)
	{
		IndexElem  *attribute = lfirst(rest);
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

		*attNumP++ = attform->attnum;

		ProcessAttrTypename(attribute, attform->atttypid, attform->atttypmod);

		*classOidP++ = GetAttrOpClass(attribute, attform->atttypid,
									  accessMethodName, accessMethodId);

		heap_freetuple(atttuple);
	}
}

static void
ProcessAttrTypename(IndexElem *attribute,
					Oid defType, int32 defTypmod)
{
	HeapTuple	tuple;

	/* build a type node so we can set the proper alignment, etc. */
	if (attribute->typename == NULL)
	{
		tuple = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(defType),
									0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "DefineIndex: type for attribute \"%s\" undefined",
				 attribute->name);

		attribute->typename = makeNode(TypeName);
		attribute->typename->name = nameout(&((Form_pg_type) GETSTRUCT(tuple))->typname);
		attribute->typename->typmod = defTypmod;
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
	 * If pg_opclass is messed up then we're probably screwed anyway...
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

	return nameout(&((Form_pg_opclass) GETSTRUCT(tuple))->opcname);
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
extern Oid	MyDatabaseId;
void
ReindexDatabase(const char *dbname, bool force, bool all)
{
	Relation	relation,
				relationRelation;
	HeapTuple	usertuple,
				dbtuple,
				tuple;
	HeapScanDesc scan;
	int4		user_id,
				db_owner;
	bool		superuser;
	Oid			db_id;
	char	   *username;
	ScanKeyData scankey;
	PortalVariableMemory pmem;
	MemoryContext old;
	int			relcnt,
				relalc,
				i,
				oncealc = 200;
	Oid		   *relids = (Oid *) NULL;

	AssertArg(dbname);

	username = GetPgUserName();
	usertuple = SearchSysCacheTuple(SHADOWNAME, PointerGetDatum(username),
									0, 0, 0);
	if (!HeapTupleIsValid(usertuple))
		elog(ERROR, "Current user \"%s\" is invalid.", username);
	user_id = ((Form_pg_shadow) GETSTRUCT(usertuple))->usesysid;
	superuser = ((Form_pg_shadow) GETSTRUCT(usertuple))->usesuper;

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
	if (user_id != db_owner && !superuser)
		elog(ERROR, "REINDEX DATABASE: Permission denied.");

	if (db_id != MyDatabaseId)
		elog(ERROR, "REINDEX DATABASE: Can be executed only on the currently open database.");

	heap_close(relation, NoLock);

	CommonSpecialPortalOpen();
	pmem = CommonSpecialPortalGetMemory();
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
			old = MemoryContextSwitchTo((MemoryContext) pmem);
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
	CommonSpecialPortalClose();
	StartTransactionCommand();
}
