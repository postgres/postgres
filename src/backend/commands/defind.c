/*-------------------------------------------------------------------------
 *
 * defind.c--
 *	  POSTGRES define, extend and remove index code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/defind.c,v 1.19 1998/01/05 03:30:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include <postgres.h>

#include <access/attnum.h>
#include <access/genam.h>
#include <access/heapam.h>
#include <utils/builtins.h>
#include <utils/syscache.h>
#include <catalog/index.h>
#include <catalog/pg_index.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_opclass.h>
#include <nodes/plannodes.h>
#include <nodes/primnodes.h>
#include <nodes/relation.h>
#include <utils/relcache.h>
#include <utils/lsyscache.h>
#include <commands/defrem.h>
#include <parser/parsetree.h>	/* for getrelid() */
#include <optimizer/prep.h>
#include <optimizer/clauses.h>
#include <storage/lmgr.h>
#include <fmgr.h>

#define IsFuncIndex(ATTR_LIST) (((IndexElem*)lfirst(ATTR_LIST))->args!=NULL)

/* non-export function prototypes */
static void CheckPredicate(List *predList, List *rangeTable, Oid baseRelOid);
static void
CheckPredExpr(Node *predicate, List *rangeTable,
			  Oid baseRelOid);
static void
			CheckPredClause(Expr *predicate, List *rangeTable, Oid baseRelOid);
static void
FuncIndexArgs(IndexElem *funcIndex, AttrNumber *attNumP,
			  Oid *argTypes, Oid *opOidP, Oid relId);
static void
NormIndexAttrs(List *attList, AttrNumber *attNumP,
			   Oid *opOidP, Oid relId);
static char *GetDefaultOpClass(Oid atttypid);

/*
 * DefineIndex --
 *		Creates a new index.
 *
 * 'attributeList' is a list of IndexElem specifying either a functional
 *		index or a list of attributes to index on.
 * 'parameterList' is a list of ParamString specified in the with clause.
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
			Expr *predicate,
			List *rangetable)
{
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			relationId;
	int			numberOfAttributes;
	AttrNumber *attributeNumberA;
	HeapTuple	tuple;
	uint16		parameterCount = 0;
	Datum	   *parameterA = NULL;
	FuncIndexInfo fInfo;
	List	   *cnfPred = NULL;
	bool		lossy = FALSE;
	List	   *pl;

	/*
	 * Handle attributes
	 */
	numberOfAttributes = length(attributeList);
	if (numberOfAttributes <= 0)
	{
		elog(ABORT, "DefineIndex: must specify at least one attribute");
	}

	/*
	 * compute heap relation id
	 */
	tuple = SearchSysCacheTuple(RELNAME,
								PointerGetDatum(heapRelationName),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(ABORT, "DefineIndex: %s relation not found",
			 heapRelationName);
	}
	relationId = tuple->t_oid;

	if (unique && strcmp(accessMethodName, "btree") != 0)
		elog(ABORT, "DefineIndex: unique indices are only available with the btree access method");

	if (numberOfAttributes > 1 && strcmp(accessMethodName, "btree") != 0)
		elog(ABORT, "DefineIndex: multi-column indices are only available with the btree access method");

	/*
	 * compute access method id
	 */
	tuple = SearchSysCacheTuple(AMNAME, PointerGetDatum(accessMethodName),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(ABORT, "DefineIndex: %s access method not found",
			 accessMethodName);
	}
	accessMethodId = tuple->t_oid;


	/*
	 * Handle parameters [param list is now different (NOT USED, really) -
	 * ay 10/94]
	 *
	 * WITH clause reinstated to handle lossy indices. -- JMH, 7/22/96
	 */
	foreach(pl, parameterList)
	{
		ParamString *param = (ParamString *) lfirst(pl);

		if (!strcasecmp(param->name, "islossy"))
			lossy = TRUE;
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
		fix_opids(cnfPred);
		CheckPredicate(cnfPred, rangetable, relationId);
	}

	if (IsFuncIndex(attributeList))
	{
		IndexElem  *funcIndex = lfirst(attributeList);
		int			nargs;

		nargs = length(funcIndex->args);
		if (nargs > INDEX_MAX_KEYS)
		{
			elog(ABORT,
				 "Too many args to function, limit of %d",
				 INDEX_MAX_KEYS);
		}

		FIsetnArgs(&fInfo, nargs);

		strcpy(FIgetname(&fInfo), funcIndex->name);

		attributeNumberA =
			(AttrNumber *) palloc(nargs * sizeof attributeNumberA[0]);

		classObjectId = (Oid *) palloc(sizeof classObjectId[0]);


		FuncIndexArgs(funcIndex, attributeNumberA,
					  &(FIgetArg(&fInfo, 0)),
					  classObjectId, relationId);

		index_create(heapRelationName,
					 indexRelationName,
					 &fInfo, NULL, accessMethodId,
					 numberOfAttributes, attributeNumberA,
			 classObjectId, parameterCount, parameterA, (Node *) cnfPred,
					 lossy, unique);
	}
	else
	{
		attributeNumberA =
			(AttrNumber *) palloc(numberOfAttributes *
								  sizeof attributeNumberA[0]);

		classObjectId =
			(Oid *) palloc(numberOfAttributes * sizeof classObjectId[0]);

		NormIndexAttrs(attributeList, attributeNumberA,
					   classObjectId, relationId);

		index_create(heapRelationName, indexRelationName, NULL,
					 attributeList,
					 accessMethodId, numberOfAttributes, attributeNumberA,
			 classObjectId, parameterCount, parameterA, (Node *) cnfPred,
					 lossy, unique);
	}
}


/*
 * ExtendIndex --
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
	IndexTupleForm index;
	Node	   *oldPred = NULL;
	List	   *cnfPred = NULL;
	PredInfo   *predInfo;
	Relation	heapRelation;
	Relation	indexRelation;
	int			i;

	/*
	 * compute index relation id and access method id
	 */
	tuple = SearchSysCacheTuple(RELNAME, PointerGetDatum(indexRelationName),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(ABORT, "ExtendIndex: %s index not found",
			 indexRelationName);
	}
	indexId = tuple->t_oid;
	accessMethodId = ((Form_pg_class) GETSTRUCT(tuple))->relam;

	/*
	 * find pg_index tuple
	 */
	tuple = SearchSysCacheTuple(INDEXRELID,
								ObjectIdGetDatum(indexId),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(ABORT, "ExtendIndex: %s is not an index",
			 indexRelationName);
	}

	/*
	 * Extract info from the pg_index tuple
	 */
	index = (IndexTupleForm) GETSTRUCT(tuple);
	Assert(index->indexrelid == indexId);
	relationId = index->indrelid;
	indproc = index->indproc;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
		if (index->indkey[i] == 0)
			break;
	numberOfAttributes = i;

	if (VARSIZE(&index->indpred) != 0)
	{
		char	   *predString;

		predString = fmgr(F_TEXTOUT, &index->indpred);
		oldPred = stringToNode(predString);
		pfree(predString);
	}
	if (oldPred == NULL)
		elog(ABORT, "ExtendIndex: %s is not a partial index",
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
		fix_opids(cnfPred);
		CheckPredicate(cnfPred, rangetable, relationId);
	}

	/* make predInfo list to pass to index_build */
	predInfo = (PredInfo *) palloc(sizeof(PredInfo));
	predInfo->pred = (Node *) cnfPred;
	predInfo->oldPred = oldPred;

	attributeNumberA =
		(AttrNumber *) palloc(numberOfAttributes *
							  sizeof attributeNumberA[0]);
	classObjectId =
		(Oid *) palloc(numberOfAttributes * sizeof classObjectId[0]);


	for (i = 0; i < numberOfAttributes; i++)
	{
		attributeNumberA[i] = index->indkey[i];
		classObjectId[i] = index->indclass[i];
	}

	if (indproc != InvalidOid)
	{
		funcInfo = &fInfo;
/*		FIgetnArgs(funcInfo) = numberOfAttributes; */
		FIsetnArgs(funcInfo, numberOfAttributes);

		tuple = SearchSysCacheTuple(PROOID,
									ObjectIdGetDatum(indproc),
									0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ABORT, "ExtendIndex: index procedure not found");

		namecpy(&(funcInfo->funcName),
				&(((Form_pg_proc) GETSTRUCT(tuple))->proname));

		FIsetProcOid(funcInfo, tuple->t_oid);
	}

	heapRelation = heap_open(relationId);
	indexRelation = index_open(indexId);

	RelationSetLockForWrite(heapRelation);

	InitIndexStrategy(numberOfAttributes, indexRelation, accessMethodId);

	index_build(heapRelation, indexRelation, numberOfAttributes,
				attributeNumberA, 0, NULL, funcInfo, predInfo);
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
	{
		CheckPredExpr(lfirst(item), rangeTable, baseRelOid);
	}
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
		elog(ABORT, "Unsupported partial-index predicate expression type");

	foreach(clause, clauses)
	{
		CheckPredExpr(lfirst(clause), rangeTable, baseRelOid);
	}
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
	{
		elog(ABORT, "Unsupported partial-index predicate clause type");
	}

	if (getrelid(pred_var->varno, rangeTable) != baseRelOid)
		elog(ABORT,
		 "Partial-index predicates may refer only to the base relation");
}


static void
FuncIndexArgs(IndexElem *funcIndex,
			  AttrNumber *attNumP,
			  Oid *argTypes,
			  Oid *opOidP,
			  Oid relId)
{
	List	   *rest;
	HeapTuple	tuple;
	AttributeTupleForm att;

	tuple = SearchSysCacheTuple(CLANAME,
								PointerGetDatum(funcIndex->class),
								0, 0, 0);

	if (!HeapTupleIsValid(tuple))
	{
		elog(ABORT, "DefineIndex: %s class not found",
			 funcIndex->class);
	}
	*opOidP = tuple->t_oid;

	MemSet(argTypes, 0, 8 * sizeof(Oid));

	/*
	 * process the function arguments
	 */
	for (rest = funcIndex->args; rest != NIL; rest = lnext(rest))
	{
		char	   *arg;

		arg = strVal(lfirst(rest));

		tuple = SearchSysCacheTuple(ATTNAME,
									ObjectIdGetDatum(relId),
									PointerGetDatum(arg), 0, 0);

		if (!HeapTupleIsValid(tuple))
		{
			elog(ABORT,
				 "DefineIndex: attribute \"%s\" not found",
				 arg);
		}
		att = (AttributeTupleForm) GETSTRUCT(tuple);
		*attNumP++ = att->attnum;
		*argTypes++ = att->atttypid;
	}
}

static void
NormIndexAttrs(List *attList,	/* list of IndexElem's */
			   AttrNumber *attNumP,
			   Oid *opOidP,
			   Oid relId)
{
	List	   *rest;
	HeapTuple	tuple;

	/*
	 * process attributeList
	 */

	for (rest = attList; rest != NIL; rest = lnext(rest))
	{
		IndexElem  *attribute;
		AttributeTupleForm attform;

		attribute = lfirst(rest);

		if (attribute->name == NULL)
			elog(ABORT, "missing attribute for define index");

		tuple = SearchSysCacheTuple(ATTNAME,
									ObjectIdGetDatum(relId),
									PointerGetDatum(attribute->name),
									0, 0);
		if (!HeapTupleIsValid(tuple))
		{
			elog(ABORT,
				 "DefineIndex: attribute \"%s\" not found",
				 attribute->name);
		}

		attform = (AttributeTupleForm) GETSTRUCT(tuple);
		*attNumP++ = attform->attnum;

		if (attribute->class == NULL)
		{
			/* no operator class specified, so find the default */
			attribute->class = GetDefaultOpClass(attform->atttypid);
			if (attribute->class == NULL)
			{
				elog(ABORT,
					 "Can't find a default operator class for type %d.",
					 attform->atttypid);
			}
		}

		tuple = SearchSysCacheTuple(CLANAME,
									PointerGetDatum(attribute->class),
									0, 0, 0);

		if (!HeapTupleIsValid(tuple))
		{
			elog(ABORT, "DefineIndex: %s class not found",
				 attribute->class);
		}
		*opOidP++ = tuple->t_oid;
	}
}

static char *
GetDefaultOpClass(Oid atttypid)
{
	HeapTuple	tuple;

	tuple = SearchSysCacheTuple(CLADEFTYPE,
								ObjectIdGetDatum(atttypid),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		return 0;
	}

	return nameout(&(((Form_pg_opclass) GETSTRUCT(tuple))->opcname));
}

/*
 * RemoveIndex --
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
	{
		elog(ABORT, "index \"%s\" nonexistent", name);
	}

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_INDEX)
	{
		elog(ABORT, "relation \"%s\" is of type \"%c\"",
			 name,
			 ((Form_pg_class) GETSTRUCT(tuple))->relkind);
	}

	index_destroy(tuple->t_oid);
}
