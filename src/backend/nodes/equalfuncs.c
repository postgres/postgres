/*-------------------------------------------------------------------------
 *
 * equalfuncs.c
 *	  equality functions to compare node trees
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/equalfuncs.c,v 1.65 2000/03/22 22:08:32 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/plannodes.h"
#include "nodes/relation.h"
#include "utils/datum.h"

static bool equali(List *a, List *b);


/*
 *	Stuff from primnodes.h
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
	if (a->resname && b->resname)
	{
		if (strcmp(a->resname, b->resname) != 0)
			return false;
	}
	else
	{
		/* must both be null to be equal */
		if (a->resname != b->resname)
			return false;
	}
	if (a->ressortgroupref != b->ressortgroupref)
		return false;
	if (a->reskey != b->reskey)
		return false;
	if (a->reskeyop != b->reskeyop)
		return false;
	/* we ignore resjunk flag ... is this correct? */

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

static bool
_equalExpr(Expr *a, Expr *b)
{
	/* We do not examine typeOid, since the optimizer often doesn't
	 * bother to set it in created nodes, and it is logically a
	 * derivative of the oper field anyway.
	 */
	if (a->opType != b->opType)
		return false;
	if (!equal(a->oper, b->oper))
		return false;
	if (!equal(a->args, b->args))
		return false;

	return true;
}

static bool
_equalAttr(Attr *a, Attr *b)
{
	if (strcmp(a->relname, b->relname) != 0)
		return false;
	if (!equal(a->attrs, b->attrs))
		return false;

	return true;
}

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
_equalOper(Oper *a, Oper *b)
{
	if (a->opno != b->opno)
		return false;
	if (a->opresulttype != b->opresulttype)
		return false;
	/* We do not examine opid, opsize, or op_fcache, since these are
	 * logically derived from opno, and they may not be set yet depending
	 * on how far along the node is in the parse/plan pipeline.
	 *
	 * It's probably not really necessary to check opresulttype either...
	 */

	return true;
}

static bool
_equalConst(Const *a, Const *b)
{
	if (a->consttype != b->consttype)
		return false;
	if (a->constlen != b->constlen)
		return false;
	if (a->constisnull != b->constisnull)
		return false;
	if (a->constbyval != b->constbyval)
		return false;
	/* XXX What about constisset and constiscast? */
	/*
	 * We treat all NULL constants of the same type as equal.
	 * Someday this might need to change?  But datumIsEqual
	 * doesn't work on nulls, so...
	 */
	if (a->constisnull)
		return true;
	return (datumIsEqual(a->constvalue, b->constvalue,
						 a->consttype, a->constbyval, a->constlen));
}

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
	/* Note we do not look at func_fcache */
	if (!equal(a->func_tlist, b->func_tlist))
		return false;
	if (!equal(a->func_planlist, b->func_planlist))
		return false;

	return true;
}

static bool
_equalAggref(Aggref *a, Aggref *b)
{
	if (strcmp(a->aggname, b->aggname) != 0)
		return false;
	if (a->basetype != b->basetype)
		return false;
	if (a->aggtype != b->aggtype)
		return false;
	if (!equal(a->target, b->target))
		return false;
	if (a->usenulls != b->usenulls)
		return false;
	if (a->aggstar != b->aggstar)
		return false;
	if (a->aggdistinct != b->aggdistinct)
		return false;
	/* ignore aggno, which is only a private field for the executor */
	return true;
}

static bool
_equalSubLink(SubLink *a, SubLink *b)
{
	if (a->subLinkType != b->subLinkType)
		return false;
	if (a->useor != b->useor)
		return false;
	if (!equal(a->lefthand, b->lefthand))
		return false;
	if (!equal(a->oper, b->oper))
		return false;
	if (!equal(a->subselect, b->subselect))
		return false;
	return true;
}

static bool
_equalRelabelType(RelabelType *a, RelabelType *b)
{
	if (!equal(a->arg, b->arg))
		return false;
	if (a->resulttype != b->resulttype)
		return false;
	if (a->resulttypmod != b->resulttypmod)
		return false;
	return true;
}

static bool
_equalArray(Array *a, Array *b)
{
	if (a->arrayelemtype != b->arrayelemtype)
		return false;
	/* We need not check arrayelemlength, arrayelembyval if types match */
	if (a->arrayndim != b->arrayndim)
		return false;
	/* XXX shouldn't we be checking all indices??? */
	if (a->arraylow.indx[0] != b->arraylow.indx[0])
		return false;
	if (a->arrayhigh.indx[0] != b->arrayhigh.indx[0])
		return false;
	if (a->arraylen != b->arraylen)
		return false;

	return true;
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
 * Stuff from relation.h
 */

static bool
_equalRelOptInfo(RelOptInfo *a, RelOptInfo *b)
{
	/* We treat RelOptInfos as equal if they refer to the same base rels
	 * joined in the same order.  Is this sufficient?
	 */
	return equali(a->relids, b->relids);
}

static bool
_equalIndexOptInfo(IndexOptInfo *a, IndexOptInfo *b)
{
	/* We treat IndexOptInfos as equal if they refer to the same index.
	 * Is this sufficient?
	 */
	if (a->indexoid != b->indexoid)
		return false;
	return true;
}

static bool
_equalPathKeyItem(PathKeyItem *a, PathKeyItem *b)
{
	if (a->sortop != b->sortop)
		return false;
	if (!equal(a->key, b->key))
		return false;
	return true;
}

static bool
_equalPath(Path *a, Path *b)
{
	if (a->pathtype != b->pathtype)
		return false;
	if (!equal(a->parent, b->parent))
		return false;
	/* do not check path costs, since they may not be set yet, and being
	 * float values there are roundoff error issues anyway...
	 */
	if (!equal(a->pathkeys, b->pathkeys))
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
	if (a->indexscandir != b->indexscandir)
		return false;
	if (!equali(a->joinrelids, b->joinrelids))
		return false;
	/* Skip 'rows' because of possibility of floating-point roundoff error.
	 * It should be derivable from the other fields anyway.
	 */
	return true;
}

static bool
_equalTidPath(TidPath *a, TidPath *b)
{
	if (!_equalPath((Path *) a, (Path *) b))
		return false;
	if (!equal(a->tideval, b->tideval))
		return false;
	if (!equali(a->unjoined_relids, b->unjoined_relids))
		return false;
	return true;
}

static bool
_equalJoinPath(JoinPath *a, JoinPath *b)
{
	if (!_equalPath((Path *) a, (Path *) b))
		return false;
	if (!equal(a->outerjoinpath, b->outerjoinpath))
		return false;
	if (!equal(a->innerjoinpath, b->innerjoinpath))
		return false;
	if (!equal(a->joinrestrictinfo, b->joinrestrictinfo))
		return false;
	return true;
}

static bool
_equalNestPath(NestPath *a, NestPath *b)
{
	if (!_equalJoinPath((JoinPath *) a, (JoinPath *) b))
		return false;
	return true;
}

static bool
_equalMergePath(MergePath *a, MergePath *b)
{
	if (!_equalJoinPath((JoinPath *) a, (JoinPath *) b))
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
	if (!_equalJoinPath((JoinPath *) a, (JoinPath *) b))
		return false;
	if (!equal(a->path_hashclauses, b->path_hashclauses))
		return false;
	return true;
}

/* XXX	This equality function is a quick hack, should be
 *		fixed to compare all fields.
 *
 * XXX  Why is this even here?  We don't have equal() funcs for
 *      any other kinds of Plan nodes... likely this is dead code...
 */
static bool
_equalIndexScan(IndexScan *a, IndexScan *b)
{
	/*
	 * if(a->scan.plan.cost != b->scan.plan.cost) return(false);
	 */

	if (!equal(a->indxqual, b->indxqual))
		return false;

	if (a->scan.scanrelid != b->scan.scanrelid)
		return false;

	if (a->indxorderdir != b->indxorderdir)
		return false;

	if (!equali(a->indxid, b->indxid))
		return false;
	return true;
}

static bool
_equalTidScan(TidScan *a, TidScan *b)
{
	Assert(IsA(a, TidScan));
	Assert(IsA(b, TidScan));

	/*
	 * if(a->scan.plan.cost != b->scan.plan.cost) return(false);
	 */

	if (a->needRescan != b->needRescan)
		return false;

	if (!equal(a->tideval, b->tideval))
		return false;

	if (a->scan.scanrelid != b->scan.scanrelid)
		return false;

	return true;
}

static bool
_equalSubPlan(SubPlan *a, SubPlan *b)
{
	/* should compare plans, but have to settle for comparing plan IDs */
	if (a->plan_id != b->plan_id)
		return false;

	if (!equal(a->rtable, b->rtable))
		return false;

	if (!equal(a->sublink, b->sublink))
		return false;

	return true;
}

static bool
_equalRestrictInfo(RestrictInfo *a, RestrictInfo *b)
{
	if (!equal(a->clause, b->clause))
		return false;
	if (!equal(a->subclauseindices, b->subclauseindices))
		return false;
	if (a->mergejoinoperator != b->mergejoinoperator)
		return false;
	if (a->left_sortop != b->left_sortop)
		return false;
	if (a->right_sortop != b->right_sortop)
		return false;
	if (a->hashjoinoperator != b->hashjoinoperator)
		return false;
	return true;
}

static bool
_equalJoinInfo(JoinInfo *a, JoinInfo *b)
{
	if (!equali(a->unjoined_relids, b->unjoined_relids))
		return false;
	if (!equal(a->jinfo_restrictinfo, b->jinfo_restrictinfo))
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
	if (a->into && b->into)
	{
		if (strcmp(a->into, b->into) != 0)
			return false;
	}
	else
	{
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
	if (!equal(a->rtable, b->rtable))
		return false;
	if (!equal(a->targetList, b->targetList))
		return false;
	if (!equal(a->qual, b->qual))
		return false;
	if (!equal(a->rowMark, b->rowMark))
		return false;
	if (!equal(a->distinctClause, b->distinctClause))
		return false;
	if (!equal(a->sortClause, b->sortClause))
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

	/*
	 * We do not check the internal-to-the-planner fields: base_rel_list,
	 * join_rel_list, equi_key_list, query_pathkeys.
	 * They might not be set yet, and in any case they should be derivable
	 * from the other fields.
	 */
	return true;
}

static bool
_equalRangeTblEntry(RangeTblEntry *a, RangeTblEntry *b)
{
	if (a->relname && b->relname)
	{
		if (strcmp(a->relname, b->relname) != 0)
			return false;
	}
	else
	{
		if (a->relname != b->relname)
			return false;
	}
	if (!equal(a->ref, b->ref))
		return false;
	if (a->relid != b->relid)
		return false;
	if (a->inh != b->inh)
		return false;
	if (a->inFromCl != b->inFromCl)
		return false;
	if (a->inJoinSet != b->inJoinSet)
		return false;
	if (a->skipAcl != b->skipAcl)
		return false;

	return true;
}

static bool
_equalSortClause(SortClause *a, SortClause *b)
{
	if (a->tleSortGroupRef != b->tleSortGroupRef)
		return false;
	if (a->sortop != b->sortop)
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

static bool
_equalCaseExpr(CaseExpr *a, CaseExpr *b)
{
	if (a->casetype != b->casetype)
		return false;
	if (!equal(a->arg, b->arg))
		return false;
	if (!equal(a->args, b->args))
		return false;
	if (!equal(a->defresult, b->defresult))
		return false;

	return true;
}

static bool
_equalCaseWhen(CaseWhen *a, CaseWhen *b)
{
	if (!equal(a->expr, b->expr))
		return false;
	if (!equal(a->result, b->result))
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
		case T_Integer:
			return a->val.ival == b->val.ival;
		case T_Float:
		case T_String:
			return strcmp(a->val.str, b->val.str) == 0;
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
		case T_Attr:
			retval = _equalAttr(a, b);
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
		case T_Aggref:
			retval = _equalAggref(a, b);
			break;
		case T_SubLink:
			retval = _equalSubLink(a, b);
			break;
		case T_RelabelType:
			retval = _equalRelabelType(a, b);
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
		case T_IndexOptInfo:
			retval = _equalIndexOptInfo(a, b);
			break;
		case T_PathKeyItem:
			retval = _equalPathKeyItem(a, b);
			break;
		case T_Path:
			retval = _equalPath(a, b);
			break;
		case T_IndexPath:
			retval = _equalIndexPath(a, b);
			break;
		case T_TidPath:
			retval = _equalTidPath(a, b);
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
		case T_IndexScan:
			retval = _equalIndexScan(a, b);
			break;
		case T_TidScan:
			retval = _equalTidScan(a, b);
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
		case T_Float:
		case T_String:
			retval = _equalValue(a, b);
			break;
		case T_List:
			{
				List	   *la = (List *) a;
				List	   *lb = (List *) b;
				List	   *l;

				/* Try to reject by length check before we grovel through
				 * all the elements...
				 */
				if (length(la) != length(lb))
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
		case T_SortClause:
			retval = _equalSortClause(a, b);
			break;
		case T_GroupClause:
			/* GroupClause is equivalent to SortClause */
			retval = _equalSortClause(a, b);
			break;
		case T_TargetEntry:
			retval = _equalTargetEntry(a, b);
			break;
		case T_CaseExpr:
			retval = _equalCaseExpr(a, b);
			break;
		case T_CaseWhen:
			retval = _equalCaseWhen(a, b);
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
 */
static bool
equali(List *a, List *b)
{
	List	   *l;

	foreach(l, a)
	{
		if (b == NIL)
			return false;
		if (lfirsti(l) != lfirsti(b))
			return false;
		b = lnext(b);
	}
	if (b != NIL)
		return false;
	return true;
}
