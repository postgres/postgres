/*-------------------------------------------------------------------------
 *
 * equalfuncs.c
 *	  Equality functions to compare node trees.
 *
 * NOTE: a general convention when copying or comparing plan nodes is
 * that we ignore the executor state subnode.  We do not need to look
 * at it because no current uses of copyObject() or equal() need to
 * deal with already-executing plan trees.  By leaving the state subnodes
 * out, we avoid needing to write copy/compare routines for all the
 * different executor state node types.
 *
 * Currently, in fact, equal() doesn't know how to compare Plan nodes
 * at all, let alone their executor-state subnodes.  This will probably
 * need to be fixed someday, but presently there is no need to compare
 * plan trees.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/equalfuncs.c,v 1.80 2000/11/05 22:50:19 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/plannodes.h"
#include "nodes/relation.h"
#include "utils/acl.h"
#include "utils/datum.h"


/* Macro for comparing string fields that might be NULL */
#define equalstr(a, b)  \
	(((a) != NULL && (b) != NULL) ? (strcmp(a, b) == 0) : (a) == (b))


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
	if (!equalstr(a->resname, b->resname))
		return false;
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

	/*
	 * We do not examine typeOid, since the optimizer often doesn't bother
	 * to set it in created nodes, and it is logically a derivative of the
	 * oper field anyway.
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

	/*
	 * We do not examine opid or op_fcache, since these are
	 * logically derived from opno, and they may not be set yet depending
	 * on how far along the node is in the parse/plan pipeline.
	 *
	 * (Besides, op_fcache is executor state, which we don't check --- see
	 * notes at head of file.)
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
	 * We treat all NULL constants of the same type as equal. Someday this
	 * might need to change?  But datumIsEqual doesn't work on nulls,
	 * so...
	 */
	if (a->constisnull)
		return true;
	return datumIsEqual(a->constvalue, b->constvalue,
						a->constbyval, a->constlen);
}

static bool
_equalParam(Param *a, Param *b)
{
	if (a->paramkind != b->paramkind)
		return false;
	if (a->paramtype != b->paramtype)
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
	/* Note we do not look at func_fcache; see notes for _equalOper */

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

static bool
_equalFieldSelect(FieldSelect *a, FieldSelect *b)
{
	if (!equal(a->arg, b->arg))
		return false;
	if (a->fieldnum != b->fieldnum)
		return false;
	if (a->resulttype != b->resulttype)
		return false;
	if (a->resulttypmod != b->resulttypmod)
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
_equalRangeTblRef(RangeTblRef *a, RangeTblRef *b)
{
	if (a->rtindex != b->rtindex)
		return false;

	return true;
}

static bool
_equalFromExpr(FromExpr *a, FromExpr *b)
{
	if (!equal(a->fromlist, b->fromlist))
		return false;
	if (!equal(a->quals, b->quals))
		return false;

	return true;
}

static bool
_equalJoinExpr(JoinExpr *a, JoinExpr *b)
{
	if (a->jointype != b->jointype)
		return false;
	if (a->isNatural != b->isNatural)
		return false;
	if (!equal(a->larg, b->larg))
		return false;
	if (!equal(a->rarg, b->rarg))
		return false;
	if (!equal(a->using, b->using))
		return false;
	if (!equal(a->quals, b->quals))
		return false;
	if (!equal(a->alias, b->alias))
		return false;
	if (!equal(a->colnames, b->colnames))
		return false;
	if (!equal(a->colvars, b->colvars))
		return false;

	return true;
}

/*
 * Stuff from relation.h
 */

static bool
_equalRelOptInfo(RelOptInfo *a, RelOptInfo *b)
{

	/*
	 * We treat RelOptInfos as equal if they refer to the same base rels
	 * joined in the same order.  Is this appropriate/sufficient?
	 */
	return equali(a->relids, b->relids);
}

static bool
_equalIndexOptInfo(IndexOptInfo *a, IndexOptInfo *b)
{

	/*
	 * We treat IndexOptInfos as equal if they refer to the same index. Is
	 * this sufficient?
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

	/*
	 * do not check path costs, since they may not be set yet, and being
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
	if (a->alljoinquals != b->alljoinquals)
		return false;

	/*
	 * Skip 'rows' because of possibility of floating-point roundoff
	 * error. It should be derivable from the other fields anyway.
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
	if (a->jointype != b->jointype)
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
	if (a->ispusheddown != b->ispusheddown)
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
	if (!equalstr(a->into, b->into))
		return false;
	if (a->isPortal != b->isPortal)
		return false;
	if (a->isBinary != b->isBinary)
		return false;
	if (a->isTemp != b->isTemp)
		return false;
	if (a->hasAggs != b->hasAggs)
		return false;
	if (a->hasSubLinks != b->hasSubLinks)
		return false;
	if (!equal(a->rtable, b->rtable))
		return false;
	if (!equal(a->jointree, b->jointree))
		return false;
	if (!equali(a->rowMarks, b->rowMarks))
		return false;
	if (!equal(a->targetList, b->targetList))
		return false;
	if (!equal(a->groupClause, b->groupClause))
		return false;
	if (!equal(a->havingQual, b->havingQual))
		return false;
	if (!equal(a->distinctClause, b->distinctClause))
		return false;
	if (!equal(a->sortClause, b->sortClause))
		return false;
	if (!equal(a->limitOffset, b->limitOffset))
		return false;
	if (!equal(a->limitCount, b->limitCount))
		return false;
	if (!equal(a->setOperations, b->setOperations))
		return false;

	/*
	 * We do not check the internal-to-the-planner fields: base_rel_list,
	 * join_rel_list, equi_key_list, query_pathkeys. They might not be set
	 * yet, and in any case they should be derivable from the other
	 * fields.
	 */
	return true;
}

static bool
_equalInsertStmt(InsertStmt *a, InsertStmt *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;
	if (!equal(a->cols, b->cols))
		return false;
	if (!equal(a->targetList, b->targetList))
		return false;
	if (!equal(a->selectStmt, b->selectStmt))
		return false;

	return true;
}

static bool
_equalDeleteStmt(DeleteStmt *a, DeleteStmt *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;
	if (!equal(a->whereClause, b->whereClause))
		return false;
	if (a->inh != b->inh)
		return false;

	return true;
}

static bool
_equalUpdateStmt(UpdateStmt *a, UpdateStmt *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;
	if (!equal(a->targetList, b->targetList))
		return false;
	if (!equal(a->whereClause, b->whereClause))
		return false;
	if (!equal(a->fromClause, b->fromClause))
		return false;
	if (a->inh != b->inh)
		return false;

	return true;
}

static bool
_equalSelectStmt(SelectStmt *a, SelectStmt *b)
{
	if (!equal(a->distinctClause, b->distinctClause))
		return false;
	if (!equalstr(a->into, b->into))
		return false;
	if (a->istemp != b->istemp)
		return false;
	if (!equal(a->targetList, b->targetList))
		return false;
	if (!equal(a->fromClause, b->fromClause))
		return false;
	if (!equal(a->whereClause, b->whereClause))
		return false;
	if (!equal(a->groupClause, b->groupClause))
		return false;
	if (!equal(a->havingClause, b->havingClause))
		return false;
	if (!equal(a->sortClause, b->sortClause))
		return false;
	if (!equalstr(a->portalname, b->portalname))
		return false;
	if (a->binary != b->binary)
		return false;
	if (!equal(a->limitOffset, b->limitOffset))
		return false;
	if (!equal(a->limitCount, b->limitCount))
		return false;
	if (!equal(a->forUpdate, b->forUpdate))
		return false;
	if (a->op != b->op)
		return false;
	if (a->all != b->all)
		return false;
	if (!equal(a->larg, b->larg))
		return false;
	if (!equal(a->rarg, b->rarg))
		return false;

	return true;
}

static bool
_equalSetOperationStmt(SetOperationStmt *a, SetOperationStmt *b)
{
	if (a->op != b->op)
		return false;
	if (a->all != b->all)
		return false;
	if (!equal(a->larg, b->larg))
		return false;
	if (!equal(a->rarg, b->rarg))
		return false;
	if (!equali(a->colTypes, b->colTypes))
		return false;

	return true;
}

static bool
_equalAlterTableStmt(AlterTableStmt *a, AlterTableStmt *b)
{
	if (a->subtype != b->subtype)
		return false;
	if (!equalstr(a->relname, b->relname))
		return false;
	if (a->inh != b->inh)
		return false;
	if (!equalstr(a->name, b->name))
		return false;
	if (!equal(a->def, b->def))
		return false;
	if (a->behavior != b->behavior)
		return false;

	return true;
}

static bool
_equalChangeACLStmt(ChangeACLStmt *a, ChangeACLStmt *b)
{
	if (!equal(a->relNames, b->relNames))
		return false;
	if (!equalstr(a->aclString, b->aclString))
		return false;

	return true;
}

static bool
_equalClosePortalStmt(ClosePortalStmt *a, ClosePortalStmt *b)
{
	if (!equalstr(a->portalname, b->portalname))
		return false;

	return true;
}

static bool
_equalClusterStmt(ClusterStmt *a, ClusterStmt *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;
	if (!equalstr(a->indexname, b->indexname))
		return false;

	return true;
}

static bool
_equalCopyStmt(CopyStmt *a, CopyStmt *b)
{
	if (a->binary != b->binary)
		return false;
	if (!equalstr(a->relname, b->relname))
		return false;
	if (a->oids != b->oids)
		return false;
	if (a->direction != b->direction)
		return false;
	if (!equalstr(a->filename, b->filename))
		return false;
	if (!equalstr(a->delimiter, b->delimiter))
		return false;
	if (!equalstr(a->null_print, b->null_print))
		return false;

	return true;
}

static bool
_equalCreateStmt(CreateStmt *a, CreateStmt *b)
{
	if (a->istemp != b->istemp)
		return false;
	if (!equalstr(a->relname, b->relname))
		return false;
	if (!equal(a->tableElts, b->tableElts))
		return false;
	if (!equal(a->inhRelnames, b->inhRelnames))
		return false;
	if (!equal(a->constraints, b->constraints))
		return false;

	return true;
}

static bool
_equalVersionStmt(VersionStmt *a, VersionStmt *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;
	if (a->direction != b->direction)
		return false;
	if (!equalstr(a->fromRelname, b->fromRelname))
		return false;
	if (!equalstr(a->date, b->date))
		return false;

	return true;
}

static bool
_equalDefineStmt(DefineStmt *a, DefineStmt *b)
{
	if (a->defType != b->defType)
		return false;
	if (!equalstr(a->defname, b->defname))
		return false;
	if (!equal(a->definition, b->definition))
		return false;

	return true;
}

static bool
_equalDropStmt(DropStmt *a, DropStmt *b)
{
	if (!equal(a->names, b->names))
		return false;
	if (a->removeType != b->removeType)
		return false;

	return true;
}

static bool
_equalTruncateStmt(TruncateStmt *a, TruncateStmt *b)
{
	if (!equalstr(a->relName, b->relName))
		return false;

	return true;
}

static bool
_equalCommentStmt(CommentStmt *a, CommentStmt *b)
{
	if (a->objtype != b->objtype)
		return false;
	if (!equalstr(a->objname, b->objname))
		return false;
	if (!equalstr(a->objproperty, b->objproperty))
		return false;
	if (!equal(a->objlist, b->objlist))
		return false;
	if (!equalstr(a->comment, b->comment))
		return false;

	return true;
}

static bool
_equalExtendStmt(ExtendStmt *a, ExtendStmt *b)
{
	if (!equalstr(a->idxname, b->idxname))
		return false;
	if (!equal(a->whereClause, b->whereClause))
		return false;
	if (!equal(a->rangetable, b->rangetable))
		return false;

	return true;
}

static bool
_equalFetchStmt(FetchStmt *a, FetchStmt *b)
{
	if (a->direction != b->direction)
		return false;
	if (a->howMany != b->howMany)
		return false;
	if (!equalstr(a->portalname, b->portalname))
		return false;
	if (a->ismove != b->ismove)
		return false;

	return true;
}

static bool
_equalIndexStmt(IndexStmt *a, IndexStmt *b)
{
	if (!equalstr(a->idxname, b->idxname))
		return false;
	if (!equalstr(a->relname, b->relname))
		return false;
	if (!equalstr(a->accessMethod, b->accessMethod))
		return false;
	if (!equal(a->indexParams, b->indexParams))
		return false;
	if (!equal(a->withClause, b->withClause))
		return false;
	if (!equal(a->whereClause, b->whereClause))
		return false;
	if (!equal(a->rangetable, b->rangetable))
		return false;
	if (a->unique != b->unique)
		return false;
	if (a->primary != b->primary)
		return false;

	return true;
}

static bool
_equalProcedureStmt(ProcedureStmt *a, ProcedureStmt *b)
{
	if (!equalstr(a->funcname, b->funcname))
		return false;
	if (!equal(a->argTypes, b->argTypes))
		return false;
	if (!equal(a->returnType, b->returnType))
		return false;
	if (!equal(a->withClause, b->withClause))
		return false;
	if (!equal(a->as, b->as))
		return false;
	if (!equalstr(a->language, b->language))
		return false;

	return true;
}

static bool
_equalRemoveAggrStmt(RemoveAggrStmt *a, RemoveAggrStmt *b)
{
	if (!equalstr(a->aggname, b->aggname))
		return false;
	if (!equal(a->aggtype, b->aggtype))
		return false;

	return true;
}

static bool
_equalRemoveFuncStmt(RemoveFuncStmt *a, RemoveFuncStmt *b)
{
	if (!equalstr(a->funcname, b->funcname))
		return false;
	if (!equal(a->args, b->args))
		return false;

	return true;
}

static bool
_equalRemoveOperStmt(RemoveOperStmt *a, RemoveOperStmt *b)
{
	if (!equalstr(a->opname, b->opname))
		return false;
	if (!equal(a->args, b->args))
		return false;

	return true;
}


static bool
_equalRenameStmt(RenameStmt *a, RenameStmt *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;
	if (a->inh != b->inh)
		return false;
	if (!equalstr(a->column, b->column))
		return false;
	if (!equalstr(a->newname, b->newname))
		return false;

	return true;
}

static bool
_equalRuleStmt(RuleStmt *a, RuleStmt *b)
{
	if (!equalstr(a->rulename, b->rulename))
		return false;
	if (!equal(a->whereClause, b->whereClause))
		return false;
	if (a->event != b->event)
		return false;
	if (!equal(a->object, b->object))
		return false;
	if (a->instead != b->instead)
		return false;
	if (!equal(a->actions, b->actions))
		return false;

	return true;
}

static bool
_equalNotifyStmt(NotifyStmt *a, NotifyStmt *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;

	return true;
}

static bool
_equalListenStmt(ListenStmt *a, ListenStmt *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;

	return true;
}

static bool
_equalUnlistenStmt(UnlistenStmt *a, UnlistenStmt *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;

	return true;
}

static bool
_equalTransactionStmt(TransactionStmt *a, TransactionStmt *b)
{
	if (a->command != b->command)
		return false;

	return true;
}

static bool
_equalViewStmt(ViewStmt *a, ViewStmt *b)
{
	if (!equalstr(a->viewname, b->viewname))
		return false;
	if (!equal(a->aliases, b->aliases))
		return false;
	if (!equal(a->query, b->query))
		return false;

	return true;
}

static bool
_equalLoadStmt(LoadStmt *a, LoadStmt *b)
{
	if (!equalstr(a->filename, b->filename))
		return false;

	return true;
}

static bool
_equalCreatedbStmt(CreatedbStmt *a, CreatedbStmt *b)
{
	if (!equalstr(a->dbname, b->dbname))
		return false;
	if (!equalstr(a->dbpath, b->dbpath))
		return false;
	if (a->encoding != b->encoding)
		return false;

	return true;
}

static bool
_equalDropdbStmt(DropdbStmt *a, DropdbStmt *b)
{
	if (!equalstr(a->dbname, b->dbname))
		return false;

	return true;
}

static bool
_equalVacuumStmt(VacuumStmt *a, VacuumStmt *b)
{
	if (a->verbose != b->verbose)
		return false;
	if (a->analyze != b->analyze)
		return false;
	if (!equalstr(a->vacrel, b->vacrel))
		return false;
	if (!equal(a->va_spec, b->va_spec))
		return false;

	return true;
}

static bool
_equalExplainStmt(ExplainStmt *a, ExplainStmt *b)
{
	if (!equal(a->query, b->query))
		return false;
	if (a->verbose != b->verbose)
		return false;

	return true;
}

static bool
_equalCreateSeqStmt(CreateSeqStmt *a, CreateSeqStmt *b)
{
	if (!equalstr(a->seqname, b->seqname))
		return false;
	if (!equal(a->options, b->options))
		return false;

	return true;
}

static bool
_equalVariableSetStmt(VariableSetStmt *a, VariableSetStmt *b)
{
	if (!equalstr(a->name, b->name))
		return false;
	if (!equalstr(a->value, b->value))
		return false;

	return true;
}

static bool
_equalVariableShowStmt(VariableShowStmt *a, VariableShowStmt *b)
{
	if (!equalstr(a->name, b->name))
		return false;

	return true;
}

static bool
_equalVariableResetStmt(VariableResetStmt *a, VariableResetStmt *b)
{
	if (!equalstr(a->name, b->name))
		return false;

	return true;
}

static bool
_equalCreateTrigStmt(CreateTrigStmt *a, CreateTrigStmt *b)
{
	if (!equalstr(a->trigname, b->trigname))
		return false;
	if (!equalstr(a->relname, b->relname))
		return false;
	if (!equalstr(a->funcname, b->funcname))
		return false;
	if (!equal(a->args, b->args))
		return false;
	if (a->before != b->before)
		return false;
	if (a->row != b->row)
		return false;
	if (strcmp(a->actions, b->actions) != 0)
		return false;
	if (!equalstr(a->lang, b->lang))
		return false;
	if (!equalstr(a->text, b->text))
		return false;
	if (!equal(a->attr, b->attr))
		return false;
	if (!equalstr(a->when, b->when))
		return false;
	if (a->isconstraint != b->isconstraint)
		return false;
	if (a->deferrable != b->deferrable)
		return false;
	if (a->initdeferred != b->initdeferred)
		return false;
	if (!equalstr(a->constrrelname, b->constrrelname))
		return false;

	return true;
}

static bool
_equalDropTrigStmt(DropTrigStmt *a, DropTrigStmt *b)
{
	if (!equalstr(a->trigname, b->trigname))
		return false;
	if (!equalstr(a->relname, b->relname))
		return false;

	return true;
}

static bool
_equalCreatePLangStmt(CreatePLangStmt *a, CreatePLangStmt *b)
{
	if (!equalstr(a->plname, b->plname))
		return false;
	if (!equalstr(a->plhandler, b->plhandler))
		return false;
	if (!equalstr(a->plcompiler, b->plcompiler))
		return false;
	if (a->pltrusted != b->pltrusted)
		return false;

	return true;
}

static bool
_equalDropPLangStmt(DropPLangStmt *a, DropPLangStmt *b)
{
	if (!equalstr(a->plname, b->plname))
		return false;

	return true;
}

static bool
_equalCreateUserStmt(CreateUserStmt *a, CreateUserStmt *b)
{
	if (!equalstr(a->user, b->user))
		return false;
	if (!equalstr(a->password, b->password))
		return false;
	if (a->sysid != b->sysid)
		return false;
	if (a->createdb != b->createdb)
		return false;
	if (a->createuser != b->createuser)
		return false;
	if (!equal(a->groupElts, b->groupElts))
		return false;
	if (!equalstr(a->validUntil, b->validUntil))
		return false;

	return true;
}

static bool
_equalAlterUserStmt(AlterUserStmt *a, AlterUserStmt *b)
{
	if (!equalstr(a->user, b->user))
		return false;
	if (!equalstr(a->password, b->password))
		return false;
	if (a->createdb != b->createdb)
		return false;
	if (a->createuser != b->createuser)
		return false;
	if (!equalstr(a->validUntil, b->validUntil))
		return false;

	return true;
}

static bool
_equalDropUserStmt(DropUserStmt *a, DropUserStmt *b)
{
	if (!equal(a->users, b->users))
		return false;

	return true;
}

static bool
_equalLockStmt(LockStmt *a, LockStmt *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;
	if (a->mode != b->mode)
		return false;

	return true;
}

static bool
_equalConstraintsSetStmt(ConstraintsSetStmt *a, ConstraintsSetStmt *b)
{
	if (!equal(a->constraints, b->constraints))
		return false;
	if (a->deferred != b->deferred)
		return false;

	return true;
}

static bool
_equalCreateGroupStmt(CreateGroupStmt *a, CreateGroupStmt *b)
{
	if (!equalstr(a->name, b->name))
		return false;
	if (a->sysid != b->sysid)
		return false;
	if (!equal(a->initUsers, b->initUsers))
		return false;

	return true;
}

static bool
_equalAlterGroupStmt(AlterGroupStmt *a, AlterGroupStmt *b)
{
	if (!equalstr(a->name, b->name))
		return false;
	if (a->action != b->action)
		return false;
	if (a->sysid != b->sysid)
		return false;
	if (!equal(a->listUsers, b->listUsers))
		return false;

	return true;
}

static bool
_equalDropGroupStmt(DropGroupStmt *a, DropGroupStmt *b)
{
	if (!equalstr(a->name, b->name))
		return false;

	return true;
}

static bool
_equalReindexStmt(ReindexStmt *a, ReindexStmt *b)
{
	if (a->reindexType != b->reindexType)
		return false;
	if (!equalstr(a->name, b->name))
		return false;
	if (a->force != b->force)
		return false;
	if (a->all != b->all)
		return false;

	return true;
}

static bool
_equalSetSessionStmt(SetSessionStmt *a, SetSessionStmt *b)
{
	if (!equal(a->args, b->args))
		return false;

	return true;
}

static bool
_equalAExpr(A_Expr *a, A_Expr *b)
{
	if (a->oper != b->oper)
		return false;
	if (!equalstr(a->opname, b->opname))
		return false;
	if (!equal(a->lexpr, b->lexpr))
		return false;
	if (!equal(a->rexpr, b->rexpr))
		return false;

	return true;
}

static bool
_equalAttr(Attr *a, Attr *b)
{
	if (strcmp(a->relname, b->relname) != 0)
		return false;
	if (!equal(a->paramNo, b->paramNo))
		return false;
	if (!equal(a->attrs, b->attrs))
		return false;
	if (!equal(a->indirection, b->indirection))
		return false;

	return true;
}

static bool
_equalAConst(A_Const *a, A_Const *b)
{
	if (!equal(&a->val, &b->val))
		return false;
	if (!equal(a->typename, b->typename))
		return false;

	return true;
}

static bool
_equalParamNo(ParamNo *a, ParamNo *b)
{
	if (a->number != b->number)
		return false;
	if (!equal(a->typename, b->typename))
		return false;
	if (!equal(a->indirection, b->indirection))
		return false;

	return true;
}

static bool
_equalIdent(Ident *a, Ident *b)
{
	if (!equalstr(a->name, b->name))
		return false;
	if (!equal(a->indirection, b->indirection))
		return false;
	if (a->isRel != b->isRel)
		return false;

	return true;
}

static bool
_equalFuncCall(FuncCall *a, FuncCall *b)
{
	if (!equalstr(a->funcname, b->funcname))
		return false;
	if (!equal(a->args, b->args))
		return false;
	if (a->agg_star != b->agg_star)
		return false;
	if (a->agg_distinct != b->agg_distinct)
		return false;

	return true;
}

static bool
_equalAIndices(A_Indices *a, A_Indices *b)
{
	if (!equal(a->lidx, b->lidx))
		return false;
	if (!equal(a->uidx, b->uidx))
		return false;

	return true;
}

static bool
_equalResTarget(ResTarget *a, ResTarget *b)
{
	if (!equalstr(a->name, b->name))
		return false;
	if (!equal(a->indirection, b->indirection))
		return false;
	if (!equal(a->val, b->val))
		return false;

	return true;
}

static bool
_equalTypeCast(TypeCast *a, TypeCast *b)
{
	if (!equal(a->arg, b->arg))
		return false;
	if (!equal(a->typename, b->typename))
		return false;

	return true;
}

static bool
_equalSortGroupBy(SortGroupBy *a, SortGroupBy *b)
{
	if (!equalstr(a->useOp, b->useOp))
		return false;
	if (!equal(a->node, b->node))
		return false;

	return true;
}

static bool
_equalRangeVar(RangeVar *a, RangeVar *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;
	if (a->inh != b->inh)
		return false;
	if (!equal(a->name, b->name))
		return false;

	return true;
}

static bool
_equalRangeSubselect(RangeSubselect *a, RangeSubselect *b)
{
	if (!equal(a->subquery, b->subquery))
		return false;
	if (!equal(a->name, b->name))
		return false;

	return true;
}

static bool
_equalTypeName(TypeName *a, TypeName *b)
{
	if (!equalstr(a->name, b->name))
		return false;
	if (a->timezone != b->timezone)
		return false;
	if (a->setof != b->setof)
		return false;
	if (a->typmod != b->typmod)
		return false;
	if (!equal(a->arrayBounds, b->arrayBounds))
		return false;

	return true;
}

static bool
_equalIndexElem(IndexElem *a, IndexElem *b)
{
	if (!equalstr(a->name, b->name))
		return false;
	if (!equal(a->args, b->args))
		return false;
	if (!equalstr(a->class, b->class))
		return false;

	return true;
}

static bool
_equalColumnDef(ColumnDef *a, ColumnDef *b)
{
	if (!equalstr(a->colname, b->colname))
		return false;
	if (!equal(a->typename, b->typename))
		return false;
	if (a->is_not_null != b->is_not_null)
		return false;
	if (a->is_sequence != b->is_sequence)
		return false;
	if (!equal(a->raw_default, b->raw_default))
		return false;
	if (!equalstr(a->cooked_default, b->cooked_default))
		return false;
	if (!equal(a->constraints, b->constraints))
		return false;

	return true;
}

static bool
_equalConstraint(Constraint *a, Constraint *b)
{
	if (a->contype != b->contype)
		return false;
	if (!equalstr(a->name, b->name))
		return false;
	if (!equal(a->raw_expr, b->raw_expr))
		return false;
	if (!equalstr(a->cooked_expr, b->cooked_expr))
		return false;
	if (!equal(a->keys, b->keys))
		return false;

	return true;
}

static bool
_equalDefElem(DefElem *a, DefElem *b)
{
	if (!equalstr(a->defname, b->defname))
		return false;
	if (!equal(a->arg, b->arg))
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
_equalRangeTblEntry(RangeTblEntry *a, RangeTblEntry *b)
{
	if (!equalstr(a->relname, b->relname))
		return false;
	if (a->relid != b->relid)
		return false;
	if (!equal(a->subquery, b->subquery))
		return false;
	if (!equal(a->alias, b->alias))
		return false;
	if (!equal(a->eref, b->eref))
		return false;
	if (a->inh != b->inh)
		return false;
	if (a->inFromCl != b->inFromCl)
		return false;
	if (a->checkForRead != b->checkForRead)
		return false;
	if (a->checkForWrite != b->checkForWrite)
		return false;
	if (a->checkAsUser != b->checkAsUser)
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
_equalFkConstraint(FkConstraint *a, FkConstraint *b)
{
	if (!equalstr(a->constr_name, b->constr_name))
		return false;
	if (!equalstr(a->pktable_name, b->pktable_name))
		return false;
	if (!equal(a->fk_attrs, b->fk_attrs))
		return false;
	if (!equal(a->pk_attrs, b->pk_attrs))
		return false;
	if (!equalstr(a->match_type, b->match_type))
		return false;
	if (a->actions != b->actions)
		return false;
	if (a->deferrable != b->deferrable)
		return false;
	if (a->initdeferred != b->initdeferred)
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
		case T_BitString:
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
		case T_SubPlan:
			retval = _equalSubPlan(a, b);
			break;

		case T_Resdom:
			retval = _equalResdom(a, b);
			break;
		case T_Fjoin:
			retval = _equalFjoin(a, b);
			break;
		case T_Expr:
			retval = _equalExpr(a, b);
			break;
		case T_Var:
			retval = _equalVar(a, b);
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
		case T_Func:
			retval = _equalFunc(a, b);
			break;
		case T_FieldSelect:
			retval = _equalFieldSelect(a, b);
			break;
		case T_ArrayRef:
			retval = _equalArrayRef(a, b);
			break;
		case T_Iter:
			retval = _equalIter(a, b);
			break;
		case T_RelabelType:
			retval = _equalRelabelType(a, b);
			break;
		case T_RangeTblRef:
			retval = _equalRangeTblRef(a, b);
			break;
		case T_FromExpr:
			retval = _equalFromExpr(a, b);
			break;
		case T_JoinExpr:
			retval = _equalJoinExpr(a, b);
			break;

		case T_RelOptInfo:
			retval = _equalRelOptInfo(a, b);
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
		case T_PathKeyItem:
			retval = _equalPathKeyItem(a, b);
			break;
		case T_RestrictInfo:
			retval = _equalRestrictInfo(a, b);
			break;
		case T_JoinInfo:
			retval = _equalJoinInfo(a, b);
			break;
		case T_Stream:
			retval = _equalStream(a, b);
			break;
		case T_TidPath:
			retval = _equalTidPath(a, b);
			break;
		case T_IndexOptInfo:
			retval = _equalIndexOptInfo(a, b);
			break;

		case T_EState:
			retval = _equalEState(a, b);
			break;

		case T_List:
			{
				List	   *la = (List *) a;
				List	   *lb = (List *) b;
				List	   *l;

				/*
				 * Try to reject by length check before we grovel through
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
		case T_Integer:
		case T_Float:
		case T_String:
		case T_BitString:
			retval = _equalValue(a, b);
			break;

		case T_Query:
			retval = _equalQuery(a, b);
			break;
		case T_InsertStmt:
			retval = _equalInsertStmt(a, b);
			break;
		case T_DeleteStmt:
			retval = _equalDeleteStmt(a, b);
			break;
		case T_UpdateStmt:
			retval = _equalUpdateStmt(a, b);
			break;
		case T_SelectStmt:
			retval = _equalSelectStmt(a, b);
			break;
		case T_SetOperationStmt:
			retval = _equalSetOperationStmt(a, b);
			break;
		case T_AlterTableStmt:
			retval = _equalAlterTableStmt(a, b);
			break;
		case T_ChangeACLStmt:
			retval = _equalChangeACLStmt(a, b);
			break;
		case T_ClosePortalStmt:
			retval = _equalClosePortalStmt(a, b);
			break;
		case T_ClusterStmt:
			retval = _equalClusterStmt(a, b);
			break;
		case T_CopyStmt:
			retval = _equalCopyStmt(a, b);
			break;
		case T_CreateStmt:
			retval = _equalCreateStmt(a, b);
			break;
		case T_VersionStmt:
			retval = _equalVersionStmt(a, b);
			break;
		case T_DefineStmt:
			retval = _equalDefineStmt(a, b);
			break;
		case T_DropStmt:
			retval = _equalDropStmt(a, b);
			break;
		case T_TruncateStmt:
			retval = _equalTruncateStmt(a, b);
			break;
		case T_CommentStmt:
			retval = _equalCommentStmt(a, b);
			break;
		case T_ExtendStmt:
			retval = _equalExtendStmt(a, b);
			break;
		case T_FetchStmt:
			retval = _equalFetchStmt(a, b);
			break;
		case T_IndexStmt:
			retval = _equalIndexStmt(a, b);
			break;
		case T_ProcedureStmt:
			retval = _equalProcedureStmt(a, b);
			break;
		case T_RemoveAggrStmt:
			retval = _equalRemoveAggrStmt(a, b);
			break;
		case T_RemoveFuncStmt:
			retval = _equalRemoveFuncStmt(a, b);
			break;
		case T_RemoveOperStmt:
			retval = _equalRemoveOperStmt(a, b);
			break;
		case T_RenameStmt:
			retval = _equalRenameStmt(a, b);
			break;
		case T_RuleStmt:
			retval = _equalRuleStmt(a, b);
			break;
		case T_NotifyStmt:
			retval = _equalNotifyStmt(a, b);
			break;
		case T_ListenStmt:
			retval = _equalListenStmt(a, b);
			break;
		case T_UnlistenStmt:
			retval = _equalUnlistenStmt(a, b);
			break;
		case T_TransactionStmt:
			retval = _equalTransactionStmt(a, b);
			break;
		case T_ViewStmt:
			retval = _equalViewStmt(a, b);
			break;
		case T_LoadStmt:
			retval = _equalLoadStmt(a, b);
			break;
		case T_CreatedbStmt:
			retval = _equalCreatedbStmt(a, b);
			break;
		case T_DropdbStmt:
			retval = _equalDropdbStmt(a, b);
			break;
		case T_VacuumStmt:
			retval = _equalVacuumStmt(a, b);
			break;
		case T_ExplainStmt:
			retval = _equalExplainStmt(a, b);
			break;
		case T_CreateSeqStmt:
			retval = _equalCreateSeqStmt(a, b);
			break;
		case T_VariableSetStmt:
			retval = _equalVariableSetStmt(a, b);
			break;
		case T_VariableShowStmt:
			retval = _equalVariableShowStmt(a, b);
			break;
		case T_VariableResetStmt:
			retval = _equalVariableResetStmt(a, b);
			break;
		case T_CreateTrigStmt:
			retval = _equalCreateTrigStmt(a, b);
			break;
		case T_DropTrigStmt:
			retval = _equalDropTrigStmt(a, b);
			break;
		case T_CreatePLangStmt:
			retval = _equalCreatePLangStmt(a, b);
			break;
		case T_DropPLangStmt:
			retval = _equalDropPLangStmt(a, b);
			break;
		case T_CreateUserStmt:
			retval = _equalCreateUserStmt(a, b);
			break;
		case T_AlterUserStmt:
			retval = _equalAlterUserStmt(a, b);
			break;
		case T_DropUserStmt:
			retval = _equalDropUserStmt(a, b);
			break;
		case T_LockStmt:
			retval = _equalLockStmt(a, b);
			break;
		case T_ConstraintsSetStmt:
			retval = _equalConstraintsSetStmt(a, b);
			break;
		case T_CreateGroupStmt:
			retval = _equalCreateGroupStmt(a, b);
			break;
		case T_AlterGroupStmt:
			retval = _equalAlterGroupStmt(a, b);
			break;
		case T_DropGroupStmt:
			retval = _equalDropGroupStmt(a, b);
			break;
		case T_ReindexStmt:
			retval = _equalReindexStmt(a, b);
			break;
		case T_SetSessionStmt:
			retval = _equalSetSessionStmt(a, b);
			break;
		case T_CheckPointStmt:
			retval = true;
			break;

		case T_A_Expr:
			retval = _equalAExpr(a, b);
			break;
		case T_Attr:
			retval = _equalAttr(a, b);
			break;
		case T_A_Const:
			retval = _equalAConst(a, b);
			break;
		case T_ParamNo:
			retval = _equalParamNo(a, b);
			break;
		case T_Ident:
			retval = _equalIdent(a, b);
			break;
		case T_FuncCall:
			retval = _equalFuncCall(a, b);
			break;
		case T_A_Indices:
			retval = _equalAIndices(a, b);
			break;
		case T_ResTarget:
			retval = _equalResTarget(a, b);
			break;
		case T_TypeCast:
			retval = _equalTypeCast(a, b);
			break;
		case T_SortGroupBy:
			retval = _equalSortGroupBy(a, b);
			break;
		case T_RangeVar:
			retval = _equalRangeVar(a, b);
			break;
		case T_RangeSubselect:
			retval = _equalRangeSubselect(a, b);
			break;
		case T_TypeName:
			retval = _equalTypeName(a, b);
			break;
		case T_IndexElem:
			retval = _equalIndexElem(a, b);
			break;
		case T_ColumnDef:
			retval = _equalColumnDef(a, b);
			break;
		case T_Constraint:
			retval = _equalConstraint(a, b);
			break;
		case T_DefElem:
			retval = _equalDefElem(a, b);
			break;
		case T_TargetEntry:
			retval = _equalTargetEntry(a, b);
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
		case T_CaseExpr:
			retval = _equalCaseExpr(a, b);
			break;
		case T_CaseWhen:
			retval = _equalCaseWhen(a, b);
			break;
		case T_FkConstraint:
			retval = _equalFkConstraint(a, b);
			break;

		default:
			elog(NOTICE, "equal: don't know whether nodes of type %d are equal",
				 nodeTag(a));
			break;
	}

	return retval;
}
