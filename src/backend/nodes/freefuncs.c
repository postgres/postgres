 /*-------------------------------------------------------------------------
 *
 * freefuncs.c--
 *	  Free functions for Postgres tree nodes.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/Attic/freefuncs.c,v 1.5 1999/02/10 03:52:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"

#include "utils/syscache.h"
#include "utils/builtins.h"		/* for namecpy */
#include "utils/elog.h"
#include "utils/palloc.h"
#include "catalog/pg_type.h"
#include "storage/lmgr.h"
#include "optimizer/planmain.h"

/* ****************************************************************
 *					 plannodes.h free functions
 * ****************************************************************
 */

/* ----------------
 *		FreePlanFields
 *
 *		This function frees the fields of the Plan node.  It is used by
 *		all the free functions for classes which inherit node Plan.
 * ----------------
 */
static void
FreePlanFields(Plan *node)
{
	freeObject(node->targetlist);
	freeObject(node->qual);
	freeObject(node->lefttree);
	freeObject(node->righttree);
	freeList(node->extParam);
	freeList(node->locParam);
	freeList(node->chgParam);
	freeObject(node->initPlan);
	freeList(node->subPlan);
}

/* ----------------
 *		_freePlan
 * ----------------
 */
static void
_freePlan(Plan *node)
{
	/* ----------------
	 *	free the node superclass fields
	 * ----------------
	 */
	FreePlanFields(node);

	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	pfree(node);
}


/* ----------------
 *		_freeResult
 * ----------------
 */
static void
_freeResult(Result *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);

	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->resconstantqual);

	pfree(node);
}

/* ----------------
 *		_freeAppend
 * ----------------
 */
static void
_freeAppend(Append *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);

	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->appendplans);
	freeObject(node->unionrtables);
	freeObject(node->inheritrtable);

	pfree(node);
}


/* ----------------
 *		FreeScanFields
 *
 *		This function frees the fields of the Scan node.  It is used by
 *		all the free functions for classes which inherit node Scan.
 * ----------------
 */
static void
FreeScanFields(Scan *node)
{
}

/* ----------------
 *		_freeScan
 * ----------------
 */
static void
_freeScan(Scan *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeScanFields((Scan *) node);

	pfree(node);
}

/* ----------------
 *		_freeSeqScan
 * ----------------
 */
static void
_freeSeqScan(SeqScan *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeScanFields((Scan *) node);

	pfree(node);
}

/* ----------------
 *		_freeIndexScan
 * ----------------
 */
static void
_freeIndexScan(IndexScan *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeScanFields((Scan *) node);

	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeList(node->indxid);
	freeObject(node->indxqual);
	freeObject(node->indxqualorig);

	pfree(node);
}

/* ----------------
 *		FreeJoinFields
 *
 *		This function frees the fields of the Join node.  It is used by
 *		all the free functions for classes which inherit node Join.
 * ----------------
 */
static void
FreeJoinFields(Join *node)
{
	/* nothing extra */
	return;
}


/* ----------------
 *		_freeJoin
 * ----------------
 */
static void
_freeJoin(Join *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeJoinFields(node);

	pfree(node);
}


/* ----------------
 *		_freeNestLoop
 * ----------------
 */
static void
_freeNestLoop(NestLoop *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeJoinFields((Join *) node);

	pfree(node);
}


/* ----------------
 *		_freeMergeJoin
 * ----------------
 */
static void
_freeMergeJoin(MergeJoin *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeJoinFields((Join *) node);

	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->mergeclauses);

	pfree(node->mergerightorder);
	pfree(node->mergeleftorder);

	pfree(node);
}

/* ----------------
 *		_freeHashJoin
 * ----------------
 */
static void
_freeHashJoin(HashJoin *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeJoinFields((Join *) node);

	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->hashclauses);

	pfree(node);
}


/* ----------------
 *		FreeNonameFields
 *
 *		This function frees the fields of the Noname node.  It is used by
 *		all the free functions for classes which inherit node Noname.
 * ----------------
 */
static void
FreeNonameFields(Noname *node)
{
	return;
}


/* ----------------
 *		_freeNoname
 * ----------------
 */
static void
_freeNoname(Noname *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeNonameFields(node);

	pfree(node);
}

/* ----------------
 *		_freeMaterial
 * ----------------
 */
static void
_freeMaterial(Material *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeNonameFields((Noname *) node);

	pfree(node);
}


/* ----------------
 *		_freeSort
 * ----------------
 */
static void
_freeSort(Sort *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeNonameFields((Noname *) node);

	pfree(node);
}


/* ----------------
 *		_freeGroup
 * ----------------
 */
static void
_freeGroup(Group *node)
{
	FreePlanFields((Plan *) node);

	pfree(node->grpColIdx);

	pfree(node);
}

/* ---------------
 *	_freeAgg
 * --------------
 */
static void
_freeAgg(Agg *node)
{
	FreePlanFields((Plan *) node);

	freeList(node->aggs);

	pfree(node);
}

/* ---------------
 *	_freeGroupClause
 * --------------
 */
static void
_freeGroupClause(GroupClause *node)
{
	freeObject(node->entry);

	pfree(node);
}


/* ----------------
 *		_freeUnique
 * ----------------
 */
static void
_freeUnique(Unique *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);
	FreeNonameFields((Noname *) node);

	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	if (node->uniqueAttr)
		pfree(node->uniqueAttr);

	pfree(node);
}


/* ----------------
 *		_freeHash
 * ----------------
 */
static void
_freeHash(Hash *node)
{
	/* ----------------
	 *	free node superclass fields
	 * ----------------
	 */
	FreePlanFields((Plan *) node);

	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->hashkey);

	pfree(node);
}

static void
_freeSubPlan(SubPlan *node)
{
	freeObject(node->plan);
	freeObject(node->rtable);
	freeList(node->setParam);
	freeList(node->parParam);
	freeObject(node->sublink);

	pfree(node);
}

/* ****************************************************************
 *					   primnodes.h free functions
 * ****************************************************************
 */

/* ----------------
 *		_freeResdom
 * ----------------
 */
static void
_freeResdom(Resdom *node)
{
	if (node->resname != NULL)
		pfree(node->resname);

	pfree(node);
}

static void
_freeFjoin(Fjoin *node)
{
	freeObject(node->fj_innerNode);
	pfree(node->fj_results);
	pfree(node->fj_alwaysDone);

	pfree(node);
}

/* ----------------
 *		_freeExpr
 * ----------------
 */
static void
_freeExpr(Expr *node)
{
	freeObject(node->oper);
	freeObject(node->args);

	pfree(node);
}

/* ----------------
 *		_freeVar
 * ----------------
 */
static void
_freeVar(Var *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	pfree(node);
}

static void
_freeFcache(FunctionCachePtr ptr)
{
	if (ptr->argOidVect)
		pfree(ptr->argOidVect);
	if (ptr->nullVect)
		pfree(ptr->nullVect);
	if (ptr->src)
		pfree(ptr->src);
	if (ptr->bin)
		pfree(ptr->bin);
	if (ptr->func_state)
		pfree(ptr->func_state);
	if (ptr->setArg)
		pfree(ptr->setArg);

	pfree(ptr);
}

/* ----------------
 *		_freeOper
 * ----------------
 */
static void
_freeOper(Oper *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	if (node->op_fcache)
		_freeFcache(node->op_fcache);

	pfree(node);
}

/* ----------------
 *		_freeConst
 * ----------------
 */
static void
_freeConst(Const *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	if (!node->constbyval)
		pfree((void *)node->constvalue);

	pfree(node);
}

/* ----------------
 *		_freeParam
 * ----------------
 */
static void
_freeParam(Param *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	if (node->paramname != NULL)
		pfree(node->paramname);
	freeObject(node->param_tlist);

	pfree(node);
}

/* ----------------
 *		_freeFunc
 * ----------------
 */
static void
_freeFunc(Func *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->func_tlist);
	freeObject(node->func_planlist);
	if (node->func_fcache)
		_freeFcache(node->func_fcache);

	pfree(node);
}

/* ----------------
 *		_freeAggref
 * ----------------
 */
static void
_freeAggref(Aggref *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	pfree(node->aggname);
	freeObject(node->target);

	pfree(node);
}

/* ----------------
 *		_freeSubLink
 * ----------------
 */
static void
_freeSubLink(SubLink *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->lefthand);
	freeObject(node->oper);
	freeObject(node->subselect);

	pfree(node);
}

/* ----------------
 *		_freeCaseExpr
 * ----------------
 */
static void
_freeCaseExpr(CaseExpr *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->arg);
	freeObject(node->args);
	freeObject(node->defresult);

	pfree(node);
}

/* ----------------
 *		_freeCaseWhen
 * ----------------
 */
static void
_freeCaseWhen(CaseWhen *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->expr);
	freeObject(node->result);

	pfree(node);
}

static void
_freeArray(Array *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	pfree(node);
}

static void
_freeArrayRef(ArrayRef *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->refupperindexpr);
	freeObject(node->reflowerindexpr);
	freeObject(node->refexpr);
	freeObject(node->refassgnexpr);

	pfree(node);
}

/* ****************************************************************
 *						relation.h free functions
 * ****************************************************************
 */

/* ----------------
 *		_freeRelOptInfo
 * ----------------
 */
static void
_freeRelOptInfo(RelOptInfo * node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeList(node->relids);

	freeObject(node->targetlist);
	freeObject(node->pathlist);
	freeObject(node->unorderedpath);
	freeObject(node->cheapestpath);

	if (node->classlist)
		pfree(node->classlist);

	if (node->indexkeys)
		pfree(node->indexkeys);

	freeObject(node->indpred);

	if (node->ordering)
		pfree(node->ordering);

	freeObject(node->restrictinfo);
	freeObject(node->joininfo);
	freeObject(node->innerjoin);
	freeObject(node->superrels);

	pfree(node);
}

/* ----------------
 *		FreePathFields
 *
 *		This function frees the fields of the Path node.  It is used by
 *		all the free functions for classes which inherit node Path.
 * ----------------
 */
static void
FreePathFields(Path *node)
{
	if (node->path_order->ordtype == SORTOP_ORDER)
	{
		if (node->path_order->ord.sortop)
			pfree(node->path_order->ord.sortop);
	}
	else
		freeObject(node->path_order->ord.merge);

	pfree(node->path_order);	/* is it an object, but we don't have
								   separate free for it */

	freeObject(node->pathkeys);

	freeList(node->joinid);
	freeObject(node->loc_restrictinfo);
}

/* ----------------
 *		_freePath
 * ----------------
 */
static void
_freePath(Path *node)
{
	FreePathFields(node);

	pfree(node);
}

/* ----------------
 *		_freeIndexPath
 * ----------------
 */
static void
_freeIndexPath(IndexPath *node)
{
	/* ----------------
	 *	free the node superclass fields
	 * ----------------
	 */
	FreePathFields((Path *) node);

	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeList(node->indexid);
	freeObject(node->indexqual);

	if (node->indexkeys)
		pfree(node->indexkeys);

	pfree(node);
}

/* ----------------
 *		FreeJoinPathFields
 *
 *		This function frees the fields of the JoinPath node.  It is used by
 *		all the free functions for classes which inherit node JoinPath.
 * ----------------
 */
static void
FreeJoinPathFields(JoinPath *node)
{
	freeObject(node->pathinfo);
	freeObject(node->outerjoinpath);
	freeObject(node->innerjoinpath);
}

/* ----------------
 *		_freeJoinPath
 * ----------------
 */
static void
_freeJoinPath(JoinPath *node)
{
	/* ----------------
	 *	free the node superclass fields
	 * ----------------
	 */
	FreePathFields((Path *) node);
	FreeJoinPathFields(node);

	pfree(node);
}

/* ----------------
 *		_freeMergePath
 * ----------------
 */
static void
_freeMergePath(MergePath *node)
{
	/* ----------------
	 *	free the node superclass fields
	 * ----------------
	 */
	FreePathFields((Path *) node);
	FreeJoinPathFields((JoinPath *) node);

	/* ----------------
	 *	free the remainder of the node
	 * ----------------
	 */
	freeObject(node->path_mergeclauses);
	freeObject(node->outersortkeys);
	freeObject(node->innersortkeys);

	pfree(node);
}

/* ----------------
 *		_freeHashPath
 * ----------------
 */
static void
_freeHashPath(HashPath *node)
{
	/* ----------------
	 *	free the node superclass fields
	 * ----------------
	 */
	FreePathFields((Path *) node);
	FreeJoinPathFields((JoinPath *) node);

	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->path_hashclauses);
	freeObject(node->outerhashkeys);
	freeObject(node->innerhashkeys);

	pfree(node);
}

/* ----------------
 *		_freeOrderKey
 * ----------------
 */
static void
_freeOrderKey(OrderKey *node)
{
	pfree(node);
}


/* ----------------
 *		_freeJoinKey
 * ----------------
 */
static void
_freeJoinKey(JoinKey *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->outer);
	freeObject(node->inner);

	pfree(node);
}

/* ----------------
 *		_freeMergeOrder
 * ----------------
 */
static void
_freeMergeOrder(MergeOrder *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	pfree(node);
}

/* ----------------
 *		_freeRestrictInfo
 * ----------------
 */
static void
_freeRestrictInfo(RestrictInfo * node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeObject(node->clause);
	freeObject(node->indexids);
	freeObject(node->mergejoinorder);
	freeList(node->restrictinfojoinid);

	pfree(node);
}

/* ----------------
 *		FreeJoinMethodFields
 *
 *		This function frees the fields of the JoinMethod node.  It is used by
 *		all the free functions for classes which inherit node JoinMethod.
 * ----------------
 */
static void
FreeJoinMethodFields(JoinMethod *node)
{
	freeObject(node->jmkeys);
	freeObject(node->clauses);
	return;
}

/* ----------------
 *		_freeJoinMethod
 * ----------------
 */
static void
_freeJoinMethod(JoinMethod *node)
{
	FreeJoinMethodFields(node);

	pfree(node);
}

/* ----------------
 *		_freeHInfo
 * ----------------
 */
static void
_freeHashInfo(HashInfo *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	FreeJoinMethodFields((JoinMethod *) node);

	pfree(node);
}

/* ----------------
 *		_freeMInfo
 * ----------------
 */
static void
_freeMergeInfo(MergeInfo *node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	FreeJoinMethodFields((JoinMethod *) node);
	freeObject(node->m_ordering);

	pfree(node);
}

/* ----------------
 *		_freeJoinInfo
 * ----------------
 */
static void
_freeJoinInfo(JoinInfo * node)
{
	/* ----------------
	 *	free remainder of node
	 * ----------------
	 */
	freeList(node->otherrels);
	freeObject(node->jinfo_restrictinfo);

	pfree(node);
}

static void
_freeIter(Iter *node)
{
	freeObject(node->iterexpr);

	pfree(node);
}

static void
_freeStream(Stream *node)
{
	freeObject(node->downstream);

	pfree(node);
}

/* ****************
 *			  parsenodes.h routines have no free functions
 * ****************
 */

static void
_freeTargetEntry(TargetEntry *node)
{
	freeObject(node->resdom);
	freeObject(node->fjoin);
	freeObject(node->expr);

	pfree(node);
}

static void
_freeRangeTblEntry(RangeTblEntry *node)
{
	if (node->relname)
		pfree(node->relname);
	if (node->refname)
		pfree(node->refname);

	pfree(node);
}

static void
_freeRowMark(RowMark *node)
{
	pfree(node);
}

static void
_freeSortClause(SortClause *node)
{
	freeObject(node->resdom);

	pfree(node);
}

static void
_freeAConst(A_Const *node)
{
	freeObject(&(node->val));
	freeObject(node->typename);

	pfree(node);
}

static void
_freeTypeName(TypeName *node)
{
	if (node->name)
		pfree(node->name);
	freeObject(node->arrayBounds);

	pfree(node);
}

static void
_freeQuery(Query *node)
{
	if (node->utilityStmt && nodeTag(node->utilityStmt) == T_NotifyStmt)
	{
		NotifyStmt *node_notify = (NotifyStmt *) node->utilityStmt;

		pfree(node_notify->relname);
		pfree(node_notify);
	}
	if (node->into)
		pfree(node->into);
	if (node->uniqueFlag)
		pfree(node->uniqueFlag);

	freeObject(node->sortClause);
	freeObject(node->rtable);
	freeObject(node->targetList);
	freeObject(node->qual);
	freeObject(node->groupClause);
	freeObject(node->havingQual);
	freeObject(node->unionClause);
	freeObject(node->limitOffset);
	freeObject(node->limitCount);
	freeObject(node->rowMark);

	pfree(node);
}


/* ****************
 *			  mnodes.h routines have no free functions
 * ****************
 */

/* ****************************************************************
 *					pg_list.h free functions
 * ****************************************************************
 */

static void
_freeValue(Value *node)
{
	switch (node->type)
	{
		case T_String:
			pfree(node->val.str);
			break;
		default:
			break;
	}

	pfree(node);
}

/* ----------------
 *		freeObject free's the node or list. If it is a list, it
 *		recursively frees its items.
 * ----------------
 */
void
freeObject(void *node)
{
	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
			/*
			 * PLAN NODES
			 */
		case T_Plan:
			_freePlan(node);
			break;
		case T_Result:
			_freeResult(node);
			break;
		case T_Append:
			_freeAppend(node);
			break;
		case T_Scan:
			_freeScan(node);
			break;
		case T_SeqScan:
			_freeSeqScan(node);
			break;
		case T_IndexScan:
			_freeIndexScan(node);
			break;
		case T_Join:
			_freeJoin(node);
			break;
		case T_NestLoop:
			_freeNestLoop(node);
			break;
		case T_MergeJoin:
			_freeMergeJoin(node);
			break;
		case T_HashJoin:
			_freeHashJoin(node);
			break;
		case T_Noname:
			_freeNoname(node);
			break;
		case T_Material:
			_freeMaterial(node);
			break;
		case T_Sort:
			_freeSort(node);
			break;
		case T_Group:
			_freeGroup(node);
			break;
		case T_Agg:
			_freeAgg(node);
			break;
		case T_GroupClause:
			_freeGroupClause(node);
			break;
		case T_Unique:
			_freeUnique(node);
			break;
		case T_Hash:
			_freeHash(node);
			break;
		case T_SubPlan:
			_freeSubPlan(node);
			break;

			/*
			 * PRIMITIVE NODES
			 */
		case T_Resdom:
			_freeResdom(node);
			break;
		case T_Fjoin:
			_freeFjoin(node);
			break;
		case T_Expr:
			_freeExpr(node);
			break;
		case T_Var:
			_freeVar(node);
			break;
		case T_Oper:
			_freeOper(node);
			break;
		case T_Const:
			_freeConst(node);
			break;
		case T_Param:
			_freeParam(node);
			break;
		case T_Func:
			_freeFunc(node);
			break;
		case T_Array:
			_freeArray(node);
			break;
		case T_ArrayRef:
			_freeArrayRef(node);
			break;
		case T_Aggref:
			_freeAggref(node);
			break;
		case T_SubLink:
			_freeSubLink(node);
			break;
		case T_CaseExpr:
			_freeCaseExpr(node);
			break;
		case T_CaseWhen:
			_freeCaseWhen(node);
			break;

			/*
			 * RELATION NODES
			 */
		case T_RelOptInfo:
			_freeRelOptInfo(node);
			break;
		case T_Path:
			_freePath(node);
			break;
		case T_IndexPath:
			_freeIndexPath(node);
			break;
		case T_JoinPath:
			_freeJoinPath(node);
			break;
		case T_MergePath:
			_freeMergePath(node);
			break;
		case T_HashPath:
			_freeHashPath(node);
			break;
		case T_OrderKey:
			_freeOrderKey(node);
			break;
		case T_JoinKey:
			_freeJoinKey(node);
			break;
		case T_MergeOrder:
			_freeMergeOrder(node);
			break;
		case T_RestrictInfo:
			_freeRestrictInfo(node);
			break;
		case T_JoinMethod:
			_freeJoinMethod(node);
			break;
		case T_HashInfo:
			_freeHashInfo(node);
			break;
		case T_MergeInfo:
			_freeMergeInfo(node);
			break;
		case T_JoinInfo:
			_freeJoinInfo(node);
			break;
		case T_Iter:
			_freeIter(node);
			break;
		case T_Stream:
			_freeStream(node);
			break;

			/*
			 * PARSE NODES
			 */
		case T_Query:
			_freeQuery(node);
			break;
		case T_TargetEntry:
			_freeTargetEntry(node);
			break;
		case T_RangeTblEntry:
			_freeRangeTblEntry(node);
			break;
		case T_RowMark:
			_freeRowMark(node);
			break;
		case T_SortClause:
			_freeSortClause(node);
			break;
		case T_A_Const:
			_freeAConst(node);
			break;
		case T_TypeName:
			_freeTypeName(node);
			break;

			/*
			 * VALUE NODES
			 */
		case T_Integer:
		case T_String:
		case T_Float:
			_freeValue(node);
			break;
		case T_List:
			{
				List	   *list = node,
						   *l;

				foreach(l, list)
					freeObject(lfirst(l));
				freeList(list);
			}
			break;
		default:
			elog(ERROR, "freeObject: don't know how to free %d", nodeTag(node));
			break;
	}
}
