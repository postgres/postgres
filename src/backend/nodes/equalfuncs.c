/*-------------------------------------------------------------------------
 *
 * equalfuncs.c
 *	  equal functions to compare the nodes
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/equalfuncs.c,v 1.34 1999/02/15 05:21:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "postgres.h"

#include "nodes/nodes.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

#include "utils/builtins.h"		/* for namestrcmp() */
#include "utils/datum.h"
#include "utils/elog.h"
#include "storage/itemptr.h"

static bool equali(List *a, List *b);

/*
 *	Stuff from primnodes.h
 */

/*
 *	Resdom is a subclass of Node.
 */
static bool
_equalResdom(Resdom *a, Resdom *b)
{
	if (a->resno != b->resno)
		return false;
	if (a->restype != b->restype)
		return false;
	if (a->restypmod != b->restypmod)
		return false;
	if (strcmp(a->resname, b->resname) != 0)
		return false;
	if (a->reskey != b->reskey)
		return false;
	if (a->reskeyop != b->reskeyop)
		return false;

	return true;
}

static bool
_equalFjoin(Fjoin *a, Fjoin *b)
{
	int			nNodes;

	if (a->fj_initialized != b->fj_initialized)
		return false;
	if (a->fj_nNodes != b->fj_nNodes)
		return false;
	if (!equal(a->fj_innerNode, b->fj_innerNode))
		return false;

	nNodes = a->fj_nNodes;
	if (memcmp(a->fj_results, b->fj_results, nNodes * sizeof(Datum)) != 0)
		return false;
	if (memcmp(a->fj_alwaysDone, b->fj_alwaysDone, nNodes * sizeof(bool)) != 0)
		return false;

	return true;
}

/*
 *	Expr is a subclass of Node.
 */
static bool
_equalExpr(Expr *a, Expr *b)
{
	if (a->opType != b->opType)
		return false;
	if (!equal(a->oper, b->oper))
		return false;
	if (!equal(a->args, b->args))
		return false;

	return true;
}

static bool
_equalIter(Iter *a, Iter *b)
{
	return equal(a->iterexpr, b->iterexpr);
}

static bool
_equalStream(Stream *a, Stream *b)
{
	if (a->clausetype != b->clausetype)
		return false;
	if (a->groupup != b->groupup)
		return false;
	if (a->groupcost != b->groupcost)
		return false;
	if (a->groupsel != b->groupsel)
		return false;
	if (!equal(a->pathptr, b->pathptr))
		return false;
	if (!equal(a->cinfo, b->cinfo))
		return false;
	if (!equal(a->upstream, b->upstream))
		return false;
	return equal(a->downstream, b->downstream);
}

/*
 *	Var is a subclass of Expr.
 */
static bool
_equalVar(Var *a, Var *b)
{
	if (a->varno != b->varno)
		return false;
	if (a->varattno != b->varattno)
		return false;
	if (a->vartype != b->vartype)
		return false;
	if (a->vartypmod != b->vartypmod)
		return false;
	if (a->varlevelsup != b->varlevelsup)
		return false;
	if (a->varnoold != b->varnoold)
		return false;
	if (a->varoattno != b->varoattno)
		return false;

	return true;
}

static bool
_equalArray(Array *a, Array *b)
{
	if (a->arrayelemtype != b->arrayelemtype)
		return false;
	if (a->arrayndim != b->arrayndim)
		return false;
	if (a->arraylow.indx[0] != b->arraylow.indx[0])
		return false;
	if (a->arrayhigh.indx[0] != b->arrayhigh.indx[0])
		return false;
	if (a->arraylen != b->arraylen)
		return false;
	return TRUE;
}

static bool
_equalArrayRef(ArrayRef *a, ArrayRef *b)
{
	if (a->refelemtype != b->refelemtype)
		return false;
	if (a->refattrlength != b->refattrlength)
		return false;
	if (a->refelemlength != b->refelemlength)
		return false;
	if (a->refelembyval != b->refelembyval)
		return false;
	if (!equal(a->refupperindexpr, b->refupperindexpr))
		return false;
	if (!equal(a->reflowerindexpr, b->reflowerindexpr))
		return false;
	if (!equal(a->refexpr, b->refexpr))
		return false;
	return equal(a->refassgnexpr, b->refassgnexpr);
}

/*
 *	Oper is a subclass of Expr.
 */
static bool
_equalOper(Oper *a, Oper *b)
{
	if (a->opno != b->opno)
		return false;
	if (a->opresulttype != b->opresulttype)
		return false;

	return true;
}

/*
 *	Const is a subclass of Expr.
 */
static bool
_equalConst(Const *a, Const *b)
{

	/*
	 * * this function used to do a pointer compare on a and b.  That's *
	 * ridiculous.	-- JMH, 7/11/92
	 */
	if (a->consttype != b->consttype)
		return false;
	if (a->constlen != b->constlen)
		return false;
	if (a->constisnull != b->constisnull)
		return false;
	if (a->constbyval != b->constbyval)
		return false;
	return (datumIsEqual(a->constvalue, b->constvalue,
						 a->consttype, a->constbyval, a->constlen));
}

/*
 *	Param is a subclass of Expr.
 */
static bool
_equalParam(Param *a, Param *b)
{
	if (a->paramkind != b->paramkind)
		return false;
	if (a->paramtype != b->paramtype)
		return false;
	if (!equal(a->param_tlist, b->param_tlist))
		return false;

	switch (a->paramkind)
	{
		case PARAM_NAMED:
		case PARAM_NEW:
		case PARAM_OLD:
			if (strcmp(a->paramname, b->paramname) != 0)
				return false;
			break;
		case PARAM_NUM:
		case PARAM_EXEC:
			if (a->paramid != b->paramid)
				return false;
			break;
		case PARAM_INVALID:

			/*
			 * XXX: Hmmm... What are we supposed to return in this case ??
			 */
			return true;
			break;
		default:
			elog(ERROR, "_equalParam: Invalid paramkind value: %d",
				 a->paramkind);
	}

	return true;
}

/*
 *	Func is a subclass of Expr.
 */
static bool
_equalFunc(Func *a, Func *b)
{
	if (a->funcid != b->funcid)
		return false;
	if (a->functype != b->functype)
		return false;
	if (a->funcisindex != b->funcisindex)
		return false;
	if (a->funcsize != b->funcsize)
		return false;
	if (!equal(a->func_tlist, b->func_tlist))
		return false;
	if (!equal(a->func_planlist, b->func_planlist))
		return false;

	return true;
}

/*
 * RestrictInfo is a subclass of Node.
 */
static bool
_equalRestrictInfo(RestrictInfo *a, RestrictInfo *b)
{
	Assert(IsA(a, RestrictInfo));
	Assert(IsA(b, RestrictInfo));

	if (!equal(a->clause, b->clause))
		return false;
	if (a->selectivity != b->selectivity)
		return false;
	if (a->notclause != b->notclause)
		return false;
#ifdef EqualMergeOrderExists
	if (!EqualMergeOrder(a->mergejoinorder, b->mergejoinorder))
		return false;
#endif
	if (a->hashjoinoperator != b->hashjoinoperator)
		return false;
	return equal(a->indexids, b->indexids);
}

/*
 * RelOptInfo is a subclass of Node.
 */
static bool
_equalRelOptInfo(RelOptInfo *a, RelOptInfo *b)
{
	Assert(IsA(a, RelOptInfo));
	Assert(IsA(b, RelOptInfo));

	return equal(a->relids, b->relids);
}

static bool
_equalJoinMethod(JoinMethod *a, JoinMethod *b)
{
	Assert(IsA(a, JoinMethod));
	Assert(IsA(b, JoinMethod));

	if (!equal(a->jmkeys, b->jmkeys))
		return false;
	if (!equal(a->clauses, b->clauses))
		return false;
	return true;
}

static bool
_equalPath(Path *a, Path *b)
{
	if (a->pathtype != b->pathtype)
		return false;
	if (a->parent != b->parent)
		return false;

	/*
	 * if (a->path_cost != b->path_cost) return(false);
	 */
	if (a->pathorder->ordtype == SORTOP_ORDER)
	{
		int			i = 0;

		if (a->pathorder->ord.sortop == NULL ||
			b->pathorder->ord.sortop == NULL)
		{
			if (a->pathorder->ord.sortop != b->pathorder->ord.sortop)
				return false;
		}
		else
		{
			while (a->pathorder->ord.sortop[i] != 0 &&
				   b->pathorder->ord.sortop[i] != 0)
			{
				if (a->pathorder->ord.sortop[i] != b->pathorder->ord.sortop[i])
					return false;
				i++;
			}
			if (a->pathorder->ord.sortop[i] != 0 ||
				b->pathorder->ord.sortop[i] != 0)
				return false;
		}
	}
	else
	{
		if (!equal(a->pathorder->ord.merge, b->pathorder->ord.merge))
			return false;
	}
	if (!equal(a->pathkeys, b->pathkeys))
		return false;

	/*
	 * if (a->outerjoincost != b->outerjoincost) return(false);
	 */
	if (!equali(a->joinid, b->joinid))
		return false;
	return true;
}

static bool
_equalIndexPath(IndexPath *a, IndexPath *b)
{
	if (!_equalPath((Path *) a, (Path *) b))
		return false;
	if (!equali(a->indexid, b->indexid))
		return false;
	if (!equal(a->indexqual, b->indexqual))
		return false;
	return true;
}

static bool
_equalNestPath(NestPath *a, NestPath *b)
{
	Assert(IsA_JoinPath(a));
	Assert(IsA_JoinPath(b));

	if (!_equalPath((Path *) a, (Path *) b))
		return false;
	if (!equal(a->pathinfo, b->pathinfo))
		return false;
	if (!equal(a->outerjoinpath, b->outerjoinpath))
		return false;
	if (!equal(a->innerjoinpath, b->innerjoinpath))
		return false;
	return true;
}

static bool
_equalMergePath(MergePath *a, MergePath *b)
{
	Assert(IsA(a, MergePath));
	Assert(IsA(b, MergePath));

	if (!_equalNestPath((NestPath *) a, (NestPath *) b))
		return false;
	if (!equal(a->path_mergeclauses, b->path_mergeclauses))
		return false;
	if (!equal(a->outersortkeys, b->outersortkeys))
		return false;
	if (!equal(a->innersortkeys, b->innersortkeys))
		return false;
	return true;
}

static bool
_equalHashPath(HashPath *a, HashPath *b)
{
	Assert(IsA(a, HashPath));
	Assert(IsA(b, HashPath));

	if (!_equalNestPath((NestPath *) a, (NestPath *) b))
		return false;
	if (!equal((a->path_hashclauses), (b->path_hashclauses)))
		return false;
	if (!equal(a->outerhashkeys, b->outerhashkeys))
		return false;
	if (!equal(a->innerhashkeys, b->innerhashkeys))
		return false;
	return true;
}

static bool
_equalJoinKey(JoinKey *a, JoinKey *b)
{
	Assert(IsA(a, JoinKey));
	Assert(IsA(b, JoinKey));

	if (!equal(a->outer, b->outer))
		return false;
	if (!equal(a->inner, b->inner))
		return false;
	return true;
}

static bool
_equalMergeOrder(MergeOrder *a, MergeOrder *b)
{
	if (a == (MergeOrder *) NULL && b == (MergeOrder *) NULL)
		return true;
	Assert(IsA(a, MergeOrder));
	Assert(IsA(b, MergeOrder));

	if (a->join_operator != b->join_operator)
		return false;
	if (a->left_operator != b->left_operator)
		return false;
	if (a->right_operator != b->right_operator)
		return false;
	if (a->left_type != b->left_type)
		return false;
	if (a->right_type != b->right_type)
		return false;
	return true;
}

static bool
_equalHashInfo(HashInfo *a, HashInfo *b)
{
	Assert(IsA(a, HashInfo));
	Assert(IsA(b, HashInfo));

	if (a->hashop != b->hashop)
		return false;
	return true;
}

/* XXX	This equality function is a quick hack, should be
 *		fixed to compare all fields.
 */
static bool
_equalIndexScan(IndexScan *a, IndexScan *b)
{
	Assert(IsA(a, IndexScan));
	Assert(IsA(b, IndexScan));

	/*
	 * if(a->scan.plan.cost != b->scan.plan.cost) return(false);
	 */

	if (!equal(a->indxqual, b->indxqual))
		return false;

	if (a->scan.scanrelid != b->scan.scanrelid)
		return false;

	if (!equali(a->indxid, b->indxid))
		return false;
	return true;
}

static bool
_equalSubPlan(SubPlan *a, SubPlan *b)
{
	if (a->plan_id != b->plan_id)
		return false;

	if (!equal(a->sublink->oper, b->sublink->oper))
		return false;

	return true;
}

static bool
_equalJoinInfo(JoinInfo *a, JoinInfo *b)
{
	Assert(IsA(a, JoinInfo));
	Assert(IsA(b, JoinInfo));
	if (!equal(a->unjoined_rels, b->unjoined_rels))
		return false;
	if (!equal(a->jinfo_restrictinfo, b->jinfo_restrictinfo))
		return false;
	if (a->mergejoinable != b->mergejoinable)
		return false;
	if (a->hashjoinable != b->hashjoinable)
		return false;
	return true;
}

/*
 *	Stuff from execnodes.h
 */

/*
 *	EState is a subclass of Node.
 */
static bool
_equalEState(EState *a, EState *b)
{
	if (a->es_direction != b->es_direction)
		return false;

	if (!equal(a->es_range_table, b->es_range_table))
		return false;

	if (a->es_result_relation_info != b->es_result_relation_info)
		return false;

	return true;
}

/*
 * Stuff from parsenodes.h
 */

static bool
_equalQuery(Query *a, Query *b)
{
	if (a->commandType != b->commandType)
		return false;
	if (!equal(a->utilityStmt, b->utilityStmt))
		return false;
	if (a->resultRelation != b->resultRelation)
		return false;
	if (a->into && b->into) {
		if (strcmp(a->into, b->into) != 0)
			return false;
	} else {
		if (a->into != b->into)
			return false;
	}
	if (a->isPortal != b->isPortal)
		return false;
	if (a->isBinary != b->isBinary)
		return false;
	if (a->isTemp != b->isTemp)
		return false;
	if (a->unionall != b->unionall)
		return false;
	if (a->hasAggs != b->hasAggs)
		return false;
	if (a->hasSubLinks != b->hasSubLinks)
		return false;
	if (a->uniqueFlag && b->uniqueFlag) {
		if (strcmp(a->uniqueFlag, b->uniqueFlag) != 0)
			return false;
	} else {
		if (a->uniqueFlag != b->uniqueFlag)
			return false;
	}
	if (!equal(a->sortClause, b->sortClause))
		return false;
	if (!equal(a->rtable, b->rtable))
		return false;
	if (!equal(a->targetList, b->targetList))
		return false;
	if (!equal(a->qual, b->qual))
		return false;
	if (!equal(a->rowMark, b->rowMark))
		return false;
	if (!equal(a->groupClause, b->groupClause))
		return false;
	if (!equal(a->havingQual, b->havingQual))
		return false;
	if (!equal(a->intersectClause, b->intersectClause))
		return false;
	if (!equal(a->unionClause, b->unionClause))
		return false;
	if (!equal(a->limitOffset, b->limitOffset))
		return false;
	if (!equal(a->limitCount, b->limitCount))
		return false;

	/* We do not check the internal-to-the-planner fields
	 * base_rel_list and join_rel_list.  They might not be
	 * set yet, and in any case they should be derivable
	 * from the other fields.
	 */
	return true;
}

static bool
_equalRangeTblEntry(RangeTblEntry *a, RangeTblEntry *b)
{
	if (a->relname && b->relname) {
		if (strcmp(a->relname, b->relname) != 0)
			return false;
	} else {
		if (a->relname != b->relname)
			return false;
	}
	if (a->refname && b->refname) {
		if (strcmp(a->refname, b->refname) != 0)
			return false;
	} else {
		if (a->refname != b->refname)
			return false;
	}
	if (a->relid != b->relid)
		return false;
	if (a->inh != b->inh)
		return false;
	if (a->inFromCl != b->inFromCl)
		return false;
	if (a->skipAcl != b->skipAcl)
		return false;

	return true;
}

static bool
_equalTargetEntry(TargetEntry *a, TargetEntry *b)
{
	if (!equal(a->resdom, b->resdom))
		return false;
	if (!equal(a->fjoin, b->fjoin))
		return false;
	if (!equal(a->expr, b->expr))
		return false;

	return true;
}

/*
 * Stuff from pg_list.h
 */

static bool
_equalValue(Value *a, Value *b)
{
	if (a->type != b->type)
		return false;

	switch (a->type)
	{
		case T_String:
			return strcmp(a->val.str, b->val.str);
		case T_Integer:
			return a->val.ival == b->val.ival;
		case T_Float:
			return a->val.dval == b->val.dval;
		default:
			break;
	}

	return true;
}

/*
 * equal
 *	  returns whether two nodes are equal
 */
bool
equal(void *a, void *b)
{
	bool		retval = false;

	if (a == b)
		return true;

	/*
	 * note that a!=b, so only one of them can be NULL
	 */
	if (a == NULL || b == NULL)
		return false;

	/*
	 * are they the same type of nodes?
	 */
	if (nodeTag(a) != nodeTag(b))
		return false;

	switch (nodeTag(a))
	{
		case T_Resdom:
			retval = _equalResdom(a, b);
			break;
		case T_Fjoin:
			retval = _equalFjoin(a, b);
			break;
		case T_Expr:
			retval = _equalExpr(a, b);
			break;
		case T_Iter:
			retval = _equalIter(a, b);
			break;
		case T_Stream:
			retval = _equalStream(a, b);
			break;
		case T_Var:
			retval = _equalVar(a, b);
			break;
		case T_Array:
			retval = _equalArray(a, b);
			break;
		case T_ArrayRef:
			retval = _equalArrayRef(a, b);
			break;
		case T_Oper:
			retval = _equalOper(a, b);
			break;
		case T_Const:
			retval = _equalConst(a, b);
			break;
		case T_Param:
			retval = _equalParam(a, b);
			break;
		case T_Func:
			retval = _equalFunc(a, b);
			break;
		case T_RestrictInfo:
			retval = _equalRestrictInfo(a, b);
			break;
		case T_RelOptInfo:
			retval = _equalRelOptInfo(a, b);
			break;
		case T_JoinMethod:
			retval = _equalJoinMethod(a, b);
			break;
		case T_Path:
			retval = _equalPath(a, b);
			break;
		case T_IndexPath:
			retval = _equalIndexPath(a, b);
			break;
		case T_NestPath:
			retval = _equalNestPath(a, b);
			break;
		case T_MergePath:
			retval = _equalMergePath(a, b);
			break;
		case T_HashPath:
			retval = _equalHashPath(a, b);
			break;
		case T_JoinKey:
			retval = _equalJoinKey(a, b);
			break;
		case T_MergeOrder:
			retval = _equalMergeOrder(a, b);
			break;
		case T_HashInfo:
			retval = _equalHashInfo(a, b);
			break;
		case T_IndexScan:
			retval = _equalIndexScan(a, b);
			break;
		case T_SubPlan:
			retval = _equalSubPlan(a, b);
			break;
		case T_JoinInfo:
			retval = _equalJoinInfo(a, b);
			break;
		case T_EState:
			retval = _equalEState(a, b);
			break;
		case T_Integer:
		case T_String:
		case T_Float:
			retval = _equalValue(a, b);
			break;
		case T_List:
			{
				List	   *la = (List *) a;
				List	   *lb = (List *) b;
				List	   *l;

				if (a == NULL && b == NULL)
					return true;
				if (length(a) != length(b))
					return false;
				foreach(l, la)
				{
					if (!equal(lfirst(l), lfirst(lb)))
						return false;
					lb = lnext(lb);
				}
				retval = true;
			}
			break;
		case T_Query:
			retval = _equalQuery(a, b);
			break;
		case T_RangeTblEntry:
			retval = _equalRangeTblEntry(a, b);
			break;
		case T_TargetEntry:
			retval = _equalTargetEntry(a, b);
			break;
		default:
			elog(NOTICE, "equal: don't know whether nodes of type %d are equal",
				 nodeTag(a));
			break;
	}

	return retval;
}

/*
 * equali
 *	  compares two lists of integers
 *
 * XXX temp hack. needs something like T_IntList
 */
static bool
equali(List *a, List *b)
{
	List	   *la = (List *) a;
	List	   *lb = (List *) b;
	List	   *l;

	if (a == NULL && b == NULL)
		return true;
	if (length(a) != length(b))
		return false;
	foreach(l, la)
	{
		if (lfirsti(l) != lfirsti(lb))
			return false;
		lb = lnext(lb);
	}
	return true;
}
