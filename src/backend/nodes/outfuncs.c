/*
 * outfuncs.c
 *	  routines to convert a node to ascii representation
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Header: /cvsroot/pgsql/src/backend/nodes/outfuncs.c,v 1.139 2001/01/24 19:42:57 momjian Exp $
 *
 * NOTES
 *	  Every (plan) node in POSTGRES has an associated "out" routine which
 *	  knows how to create its ascii representation. These functions are
 *	  useful for debugging as well as for storing plans in the system
 *	  catalogs (eg. views).
 */
#include "postgres.h"

#include <ctype.h>

#include "lib/stringinfo.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "parser/parse.h"
#include "utils/datum.h"


#define booltostr(x)  ((x) ? "true" : "false")

static void _outDatum(StringInfo str, Datum value, int typlen, bool typbyval);
static void _outNode(StringInfo str, void *obj);

/*
 * _outToken
 *	  Convert an ordinary string (eg, an identifier) into a form that
 *	  will be decoded back to a plain token by read.c's functions.
 *
 *	  If a null or empty string is given, it is encoded as "<>".
 */
static void
_outToken(StringInfo str, char *s)
{
	if (s == NULL || *s == '\0')
	{
		appendStringInfo(str, "<>");
		return;
	}

	/*
	 * Look for characters or patterns that are treated specially by
	 * read.c (either in pg_strtok() or in nodeRead()), and therefore need
	 * a protective backslash.
	 */
	/* These characters only need to be quoted at the start of the string */
	if (*s == '<' ||
		*s == '\"' ||
		*s == '@' ||
		isdigit((unsigned char) *s) ||
		((*s == '+' || *s == '-') &&
		 (isdigit((unsigned char) s[1]) || s[1] == '.')))
		appendStringInfoChar(str, '\\');
	while (*s)
	{
		/* These chars must be backslashed anywhere in the string */
		if (*s == ' ' || *s == '\n' || *s == '\t' ||
			*s == '(' || *s == ')' || *s == '{' || *s == '}' ||
			*s == '\\')
			appendStringInfoChar(str, '\\');
		appendStringInfoChar(str, *s++);
	}
}

/*
 * _outIntList -
 *	   converts a List of integers
 */
static void
_outIntList(StringInfo str, List *list)
{
	List	   *l;

	appendStringInfoChar(str, '(');
	foreach(l, list)
		appendStringInfo(str, " %d", lfirsti(l));
	appendStringInfoChar(str, ')');
}

/*
 * _outOidList -
 *	   converts a List of OIDs
 */
static void
_outOidList(StringInfo str, List *list)
{
	List	   *l;

	appendStringInfoChar(str, '(');
	foreach(l, list)
		appendStringInfo(str, " %u", (Oid) lfirsti(l));
	appendStringInfoChar(str, ')');
}

static void
_outCreateStmt(StringInfo str, CreateStmt *node)
{
	appendStringInfo(str, " CREATE :relname ");
	_outToken(str, node->relname);

	appendStringInfo(str, " :istemp %s ",
					 booltostr(node->istemp));

	appendStringInfo(str, "	:columns ");
	_outNode(str, node->tableElts);

	appendStringInfo(str, " :inhRelnames ");
	_outNode(str, node->inhRelnames);

	appendStringInfo(str, " :constraints ");
	_outNode(str, node->constraints);
}

static void
_outIndexStmt(StringInfo str, IndexStmt *node)
{
	appendStringInfo(str, " INDEX :idxname ");
	_outToken(str, node->idxname);
	appendStringInfo(str, " :relname ");
	_outToken(str, node->relname);
	appendStringInfo(str, " :accessMethod ");
	_outToken(str, node->accessMethod);
	appendStringInfo(str, " :indexParams ");
	_outNode(str, node->indexParams);

	appendStringInfo(str, " :withClause ");
	_outNode(str, node->withClause);

	appendStringInfo(str, " :whereClause ");
	_outNode(str, node->whereClause);

	appendStringInfo(str, " :rangetable ");
	_outNode(str, node->rangetable);

	appendStringInfo(str, " :unique %s :primary %s ",
					 booltostr(node->unique),
					 booltostr(node->primary));
}

static void
_outSelectStmt(StringInfo str, SelectStmt *node)
{
	/* XXX this is pretty durn incomplete */
	appendStringInfo(str, "SELECT :where ");
	_outNode(str, node->whereClause);
}

static void
_outFuncCall(StringInfo str, FuncCall *node)
{
	appendStringInfo(str, "FUNCTION ");
	_outToken(str, node->funcname);
	appendStringInfo(str, " :args ");
	_outNode(str, node->args);
	appendStringInfo(str, " :agg_star %s :agg_distinct %s ",
					 booltostr(node->agg_star),
					 booltostr(node->agg_distinct));
}

static void
_outColumnDef(StringInfo str, ColumnDef *node)
{
	appendStringInfo(str, " COLUMNDEF :colname ");
	_outToken(str, node->colname);
	appendStringInfo(str, " :typename ");
	_outNode(str, node->typename);
	appendStringInfo(str, " :is_not_null %s :is_sequence %s :raw_default ",
					 booltostr(node->is_not_null),
					 booltostr(node->is_sequence));
	_outNode(str, node->raw_default);
	appendStringInfo(str, " :cooked_default ");
	_outToken(str, node->cooked_default);
	appendStringInfo(str, " :constraints ");
	_outNode(str, node->constraints);
}

static void
_outTypeName(StringInfo str, TypeName *node)
{
	appendStringInfo(str, " TYPENAME :name ");
	_outToken(str, node->name);
	appendStringInfo(str, " :timezone %s :setof %s typmod %d :arrayBounds ",
					 booltostr(node->timezone),
					 booltostr(node->setof),
					 node->typmod);
	_outNode(str, node->arrayBounds);
}

static void
_outTypeCast(StringInfo str, TypeCast *node)
{
	appendStringInfo(str, " TYPECAST :arg ");
	_outNode(str, node->arg);
	appendStringInfo(str, " :typename ");
	_outNode(str, node->typename);
}

static void
_outIndexElem(StringInfo str, IndexElem *node)
{
	appendStringInfo(str, " INDEXELEM :name ");
	_outToken(str, node->name);
	appendStringInfo(str, " :args ");
	_outNode(str, node->args);
	appendStringInfo(str, " :class ");
	_outToken(str, node->class);
}

static void
_outQuery(StringInfo str, Query *node)
{

	appendStringInfo(str, " QUERY :command %d ", node->commandType);

	if (node->utilityStmt)
	{
		/*
		 * Hack to make up for lack of outfuncs for utility-stmt nodes
		 */
		switch (nodeTag(node->utilityStmt))
		{
			case T_CreateStmt:
				appendStringInfo(str, " :create ");
				_outToken(str, ((CreateStmt *) (node->utilityStmt))->relname);
				appendStringInfo(str, " ");
				_outNode(str, node->utilityStmt);
				break;

			case T_IndexStmt:
				appendStringInfo(str, " :index ");
				_outToken(str, ((IndexStmt *) (node->utilityStmt))->idxname);
				appendStringInfo(str, " on ");
				_outToken(str, ((IndexStmt *) (node->utilityStmt))->relname);
				appendStringInfo(str, " ");
				_outNode(str, node->utilityStmt);
				break;

			case T_NotifyStmt:
				appendStringInfo(str, " :notify ");
				_outToken(str, ((NotifyStmt *) (node->utilityStmt))->relname);
				break;

			default:
				appendStringInfo(str, " :utility ? ");
		}
	}
	else
		appendStringInfo(str, " :utility <>");

	appendStringInfo(str, " :resultRelation %d :into ",
					 node->resultRelation);
	_outToken(str, node->into);

	appendStringInfo(str, " :isPortal %s :isBinary %s :isTemp %s"
					 " :hasAggs %s :hasSubLinks %s :rtable ",
					 booltostr(node->isPortal),
					 booltostr(node->isBinary),
					 booltostr(node->isTemp),
					 booltostr(node->hasAggs),
					 booltostr(node->hasSubLinks));
	_outNode(str, node->rtable);

	appendStringInfo(str, " :jointree ");
	_outNode(str, node->jointree);

	appendStringInfo(str, " :rowMarks ");
	_outIntList(str, node->rowMarks);

	appendStringInfo(str, " :targetList ");
	_outNode(str, node->targetList);

	appendStringInfo(str, " :groupClause ");
	_outNode(str, node->groupClause);

	appendStringInfo(str, " :havingQual ");
	_outNode(str, node->havingQual);

	appendStringInfo(str, " :distinctClause ");
	_outNode(str, node->distinctClause);

	appendStringInfo(str, " :sortClause ");
	_outNode(str, node->sortClause);

	appendStringInfo(str, " :limitOffset ");
	_outNode(str, node->limitOffset);

	appendStringInfo(str, " :limitCount ");
	_outNode(str, node->limitCount);

	appendStringInfo(str, " :setOperations ");
	_outNode(str, node->setOperations);

	appendStringInfo(str, " :resultRelations ");
	_outIntList(str, node->resultRelations);
}

static void
_outSortClause(StringInfo str, SortClause *node)
{
	appendStringInfo(str, " SORTCLAUSE :tleSortGroupRef %u :sortop %u ",
					 node->tleSortGroupRef, node->sortop);
}

static void
_outGroupClause(StringInfo str, GroupClause *node)
{
	appendStringInfo(str, " GROUPCLAUSE :tleSortGroupRef %u :sortop %u ",
					 node->tleSortGroupRef, node->sortop);
}

static void
_outSetOperationStmt(StringInfo str, SetOperationStmt *node)
{
	appendStringInfo(str, " SETOPERATIONSTMT :op %d :all %s :larg ",
					 (int) node->op,
					 booltostr(node->all));
	_outNode(str, node->larg);
	appendStringInfo(str, " :rarg ");
	_outNode(str, node->rarg);
	appendStringInfo(str, " :colTypes ");
	_outOidList(str, node->colTypes);
}

/*
 * print the basic stuff of all nodes that inherit from Plan
 *
 * NOTE: we deliberately omit the execution state (EState)
 */
static void
_outPlanInfo(StringInfo str, Plan *node)
{
	appendStringInfo(str,
					 ":startup_cost %.2f :total_cost %.2f :rows %.0f :width %d :qptargetlist ",
					 node->startup_cost,
					 node->total_cost,
					 node->plan_rows,
					 node->plan_width);
	_outNode(str, node->targetlist);

	appendStringInfo(str, " :qpqual ");
	_outNode(str, node->qual);

	appendStringInfo(str, " :lefttree ");
	_outNode(str, node->lefttree);

	appendStringInfo(str, " :righttree ");
	_outNode(str, node->righttree);

	appendStringInfo(str, " :extprm ");
	_outIntList(str, node->extParam);

	appendStringInfo(str, " :locprm ");
	_outIntList(str, node->locParam);

	appendStringInfo(str, " :initplan ");
	_outNode(str, node->initPlan);

	appendStringInfo(str, " :nprm %d ", node->nParamExec);
}

/*
 *	Stuff from plannodes.h
 */
static void
_outPlan(StringInfo str, Plan *node)
{
	appendStringInfo(str, " PLAN ");
	_outPlanInfo(str, (Plan *) node);
}

static void
_outResult(StringInfo str, Result *node)
{
	appendStringInfo(str, " RESULT ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :resconstantqual ");
	_outNode(str, node->resconstantqual);

}

/*
 *	Append is a subclass of Plan.
 */
static void
_outAppend(StringInfo str, Append *node)
{
	appendStringInfo(str, " APPEND ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :appendplans ");
	_outNode(str, node->appendplans);

	appendStringInfo(str, " :isTarget %s ",
					 booltostr(node->isTarget));
}

/*
 *	Join is a subclass of Plan
 */
static void
_outJoin(StringInfo str, Join *node)
{
	appendStringInfo(str, " JOIN ");
	_outPlanInfo(str, (Plan *) node);
	appendStringInfo(str, " :jointype %d :joinqual ",
					 (int) node->jointype);
	_outNode(str, node->joinqual);
}

/*
 *	NestLoop is a subclass of Join
 */
static void
_outNestLoop(StringInfo str, NestLoop *node)
{
	appendStringInfo(str, " NESTLOOP ");
	_outPlanInfo(str, (Plan *) node);
	appendStringInfo(str, " :jointype %d :joinqual ",
					 (int) node->join.jointype);
	_outNode(str, node->join.joinqual);
}

/*
 *	MergeJoin is a subclass of Join
 */
static void
_outMergeJoin(StringInfo str, MergeJoin *node)
{
	appendStringInfo(str, " MERGEJOIN ");
	_outPlanInfo(str, (Plan *) node);
	appendStringInfo(str, " :jointype %d :joinqual ",
					 (int) node->join.jointype);
	_outNode(str, node->join.joinqual);

	appendStringInfo(str, " :mergeclauses ");
	_outNode(str, node->mergeclauses);
}

/*
 *	HashJoin is a subclass of Join.
 */
static void
_outHashJoin(StringInfo str, HashJoin *node)
{
	appendStringInfo(str, " HASHJOIN ");
	_outPlanInfo(str, (Plan *) node);
	appendStringInfo(str, " :jointype %d :joinqual ",
					 (int) node->join.jointype);
	_outNode(str, node->join.joinqual);

	appendStringInfo(str, " :hashclauses ");
	_outNode(str, node->hashclauses);
	appendStringInfo(str, " :hashjoinop %u ",
					 node->hashjoinop);
}

static void
_outSubPlan(StringInfo str, SubPlan *node)
{
	appendStringInfo(str, " SUBPLAN :plan ");
	_outNode(str, node->plan);

	appendStringInfo(str, " :planid %d :rtable ", node->plan_id);
	_outNode(str, node->rtable);

	appendStringInfo(str, " :setprm ");
	_outIntList(str, node->setParam);

	appendStringInfo(str, " :parprm ");
	_outIntList(str, node->parParam);

	appendStringInfo(str, " :slink ");
	_outNode(str, node->sublink);
}

/*
 *	Scan is a subclass of Node
 */
static void
_outScan(StringInfo str, Scan *node)
{
	appendStringInfo(str, " SCAN ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :scanrelid %u ", node->scanrelid);
}

/*
 *	SeqScan is a subclass of Scan
 */
static void
_outSeqScan(StringInfo str, SeqScan *node)
{
	appendStringInfo(str, " SEQSCAN ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :scanrelid %u ", node->scanrelid);
}

/*
 *	IndexScan is a subclass of Scan
 */
static void
_outIndexScan(StringInfo str, IndexScan *node)
{
	appendStringInfo(str, " INDEXSCAN ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :scanrelid %u :indxid ", node->scan.scanrelid);
	_outOidList(str, node->indxid);

	appendStringInfo(str, " :indxqual ");
	_outNode(str, node->indxqual);

	appendStringInfo(str, " :indxqualorig ");
	_outNode(str, node->indxqualorig);

	appendStringInfo(str, " :indxorderdir %d ", node->indxorderdir);
}

/*
 *	TidScan is a subclass of Scan
 */
static void
_outTidScan(StringInfo str, TidScan *node)
{
	appendStringInfo(str, " TIDSCAN ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :scanrelid %u ", node->scan.scanrelid);
	appendStringInfo(str, " :needrescan %d ", node->needRescan);

	appendStringInfo(str, " :tideval ");
	_outNode(str, node->tideval);

}

/*
 *	SubqueryScan is a subclass of Scan
 */
static void
_outSubqueryScan(StringInfo str, SubqueryScan *node)
{
	appendStringInfo(str, " SUBQUERYSCAN ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :scanrelid %u :subplan ", node->scan.scanrelid);
	_outNode(str, node->subplan);
}

/*
 *	Material is a subclass of Plan
 */
static void
_outMaterial(StringInfo str, Material *node)
{
	appendStringInfo(str, " MATERIAL ");
	_outPlanInfo(str, (Plan *) node);
}

/*
 *	Sort is a subclass of Plan
 */
static void
_outSort(StringInfo str, Sort *node)
{
	appendStringInfo(str, " SORT ");
	_outPlanInfo(str, (Plan *) node);
	appendStringInfo(str, " :keycount %d ", node->keycount);
}

static void
_outAgg(StringInfo str, Agg *node)
{
	appendStringInfo(str, " AGG ");
	_outPlanInfo(str, (Plan *) node);
}

static void
_outGroup(StringInfo str, Group *node)
{
	appendStringInfo(str, " GRP ");
	_outPlanInfo(str, (Plan *) node);

	/* the actual Group fields */
	appendStringInfo(str, " :numCols %d :tuplePerGroup %s ",
					 node->numCols,
					 booltostr(node->tuplePerGroup));
}

static void
_outUnique(StringInfo str, Unique *node)
{
	int		i;

	appendStringInfo(str, " UNIQUE ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :numCols %d :uniqColIdx ",
					 node->numCols);
	for (i = 0; i < node->numCols; i++)
		appendStringInfo(str, "%d ", (int) node->uniqColIdx[i]);
}

static void
_outSetOp(StringInfo str, SetOp *node)
{
	int		i;

	appendStringInfo(str, " SETOP ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :cmd %d :numCols %d :dupColIdx ",
					 (int) node->cmd, node->numCols);
	for (i = 0; i < node->numCols; i++)
		appendStringInfo(str, "%d ", (int) node->dupColIdx[i]);
	appendStringInfo(str, " :flagColIdx %d ",
					 (int) node->flagColIdx);
}

static void
_outLimit(StringInfo str, Limit *node)
{
	appendStringInfo(str, " LIMIT ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :limitOffset ");
	_outNode(str, node->limitOffset);
	appendStringInfo(str, " :limitCount ");
	_outNode(str, node->limitCount);
}

/*
 *	Hash is a subclass of Plan
 */
static void
_outHash(StringInfo str, Hash *node)
{
	appendStringInfo(str, " HASH ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :hashkey ");
	_outNode(str, node->hashkey);
}

/*****************************************************************************
 *
 *	Stuff from primnodes.h.
 *
 *****************************************************************************/

/*
 *	Resdom is a subclass of Node
 */
static void
_outResdom(StringInfo str, Resdom *node)
{
	appendStringInfo(str,
				 " RESDOM :resno %d :restype %u :restypmod %d :resname ",
					 node->resno,
					 node->restype,
					 node->restypmod);
	_outToken(str, node->resname);
	appendStringInfo(str, " :reskey %u :reskeyop %u :ressortgroupref %u :resjunk %s ",
					 node->reskey,
					 node->reskeyop,
					 node->ressortgroupref,
					 booltostr(node->resjunk));
}

static void
_outFjoin(StringInfo str, Fjoin *node)
{
	int			i;

	appendStringInfo(str, " FJOIN :initialized %s :nNodes %d ",
					 booltostr(node->fj_initialized),
					 node->fj_nNodes);

	appendStringInfo(str, " :innerNode ");
	_outNode(str, node->fj_innerNode);

	appendStringInfo(str, " :results @ 0x%p :alwaysdone",
					 node->fj_results);

	for (i = 0; i < node->fj_nNodes; i++)
		appendStringInfo(str,
						 booltostr(node->fj_alwaysDone[i]));
}

/*
 *	Expr is a subclass of Node
 */
static void
_outExpr(StringInfo str, Expr *node)
{
	char	   *opstr = NULL;

	appendStringInfo(str, " EXPR :typeOid %u ",
					 node->typeOid);

	switch (node->opType)
	{
		case OP_EXPR:
			opstr = "op";
			break;
		case FUNC_EXPR:
			opstr = "func";
			break;
		case OR_EXPR:
			opstr = "or";
			break;
		case AND_EXPR:
			opstr = "and";
			break;
		case NOT_EXPR:
			opstr = "not";
			break;
		case SUBPLAN_EXPR:
			opstr = "subp";
			break;
	}
	appendStringInfo(str, " :opType ");
	_outToken(str, opstr);
	appendStringInfo(str, " :oper ");
	_outNode(str, node->oper);

	appendStringInfo(str, " :args ");
	_outNode(str, node->args);
}

/*
 *	Var is a subclass of Expr
 */
static void
_outVar(StringInfo str, Var *node)
{
	appendStringInfo(str,
				" VAR :varno %u :varattno %d :vartype %u :vartypmod %d ",
					 node->varno,
					 node->varattno,
					 node->vartype,
					 node->vartypmod);

	appendStringInfo(str, " :varlevelsup %u :varnoold %u :varoattno %d",
					 node->varlevelsup,
					 node->varnoold,
					 node->varoattno);
}

/*
 *	Const is a subclass of Expr
 */
static void
_outConst(StringInfo str, Const *node)
{
	appendStringInfo(str,
					 " CONST :consttype %u :constlen %d :constbyval %s"
					 " :constisnull %s :constvalue ",
					 node->consttype,
					 node->constlen,
					 booltostr(node->constbyval),
					 booltostr(node->constisnull));

	if (node->constisnull)
		appendStringInfo(str, "<>");
	else
		_outDatum(str, node->constvalue, node->constlen, node->constbyval);
}

/*
 *	Aggref
 */
static void
_outAggref(StringInfo str, Aggref *node)
{
	appendStringInfo(str, " AGGREG :aggname ");
	_outToken(str, node->aggname);
	appendStringInfo(str, " :basetype %u :aggtype %u :target ",
					 node->basetype, node->aggtype);
	_outNode(str, node->target);

	appendStringInfo(str, " :aggstar %s :aggdistinct %s ",
					 booltostr(node->aggstar),
					 booltostr(node->aggdistinct));
	/* aggno is not dumped */
}

/*
 *	SubLink
 */
static void
_outSubLink(StringInfo str, SubLink *node)
{
	appendStringInfo(str,
					 " SUBLINK :subLinkType %d :useor %s :lefthand ",
					 node->subLinkType,
					 booltostr(node->useor));
	_outNode(str, node->lefthand);

	appendStringInfo(str, " :oper ");
	_outNode(str, node->oper);

	appendStringInfo(str, " :subselect ");
	_outNode(str, node->subselect);
}

/*
 *	ArrayRef is a subclass of Expr
 */
static void
_outArrayRef(StringInfo str, ArrayRef *node)
{
	appendStringInfo(str,
		" ARRAYREF :refelemtype %u :refattrlength %d :refelemlength %d ",
					 node->refelemtype,
					 node->refattrlength,
					 node->refelemlength);

	appendStringInfo(str, " :refelembyval %c :refupperindex ",
					 node->refelembyval ? 't' : 'f');
	_outNode(str, node->refupperindexpr);

	appendStringInfo(str, " :reflowerindex ");
	_outNode(str, node->reflowerindexpr);

	appendStringInfo(str, " :refexpr ");
	_outNode(str, node->refexpr);

	appendStringInfo(str, " :refassgnexpr ");
	_outNode(str, node->refassgnexpr);
}

/*
 *	Func is a subclass of Expr
 */
static void
_outFunc(StringInfo str, Func *node)
{
	appendStringInfo(str, " FUNC :funcid %u :functype %u ",
					 node->funcid,
					 node->functype);
}

/*
 *	Oper is a subclass of Expr
 */
static void
_outOper(StringInfo str, Oper *node)
{
	appendStringInfo(str,
					 " OPER :opno %u :opid %u :opresulttype %u ",
					 node->opno,
					 node->opid,
					 node->opresulttype);
}

/*
 *	Param is a subclass of Expr
 */
static void
_outParam(StringInfo str, Param *node)
{
	appendStringInfo(str, " PARAM :paramkind %d :paramid %d :paramname ",
					 node->paramkind,
					 node->paramid);
	_outToken(str, node->paramname);
	appendStringInfo(str, " :paramtype %u ", node->paramtype);
}

/*
 *	FieldSelect
 */
static void
_outFieldSelect(StringInfo str, FieldSelect *node)
{
	appendStringInfo(str, " FIELDSELECT :arg ");
	_outNode(str, node->arg);

	appendStringInfo(str, " :fieldnum %d :resulttype %u :resulttypmod %d ",
					 node->fieldnum, node->resulttype, node->resulttypmod);
}

/*
 *	RelabelType
 */
static void
_outRelabelType(StringInfo str, RelabelType *node)
{
	appendStringInfo(str, " RELABELTYPE :arg ");
	_outNode(str, node->arg);

	appendStringInfo(str, " :resulttype %u :resulttypmod %d ",
					 node->resulttype, node->resulttypmod);
}

/*
 *	RangeTblRef
 */
static void
_outRangeTblRef(StringInfo str, RangeTblRef *node)
{
	appendStringInfo(str, " RANGETBLREF %d ",
					 node->rtindex);
}

/*
 *	FromExpr
 */
static void
_outFromExpr(StringInfo str, FromExpr *node)
{
	appendStringInfo(str, " FROMEXPR :fromlist ");
	_outNode(str, node->fromlist);
	appendStringInfo(str, " :quals ");
	_outNode(str, node->quals);
}

/*
 *	JoinExpr
 */
static void
_outJoinExpr(StringInfo str, JoinExpr *node)
{
	appendStringInfo(str, " JOINEXPR :jointype %d :isNatural %s :larg ",
					 (int) node->jointype,
					 booltostr(node->isNatural));
	_outNode(str, node->larg);
	appendStringInfo(str, " :rarg ");
	_outNode(str, node->rarg);
	appendStringInfo(str, " :using ");
	_outNode(str, node->using);
	appendStringInfo(str, " :quals ");
	_outNode(str, node->quals);
	appendStringInfo(str, " :alias ");
	_outNode(str, node->alias);
	appendStringInfo(str, " :colnames ");
	_outNode(str, node->colnames);
	appendStringInfo(str, " :colvars ");
	_outNode(str, node->colvars);
}

/*
 *	Stuff from relation.h
 */

static void
_outRelOptInfo(StringInfo str, RelOptInfo *node)
{
	appendStringInfo(str, " RELOPTINFO :relids ");
	_outIntList(str, node->relids);

	appendStringInfo(str, " :rows %.0f :width %d :targetlist ",
					 node->rows,
					 node->width);
	_outNode(str, node->targetlist);

	appendStringInfo(str, " :pathlist ");
	_outNode(str, node->pathlist);
	appendStringInfo(str, " :cheapest_startup_path ");
	_outNode(str, node->cheapest_startup_path);
	appendStringInfo(str, " :cheapest_total_path ");
	_outNode(str, node->cheapest_total_path);

	appendStringInfo(str, " :pruneable %s :issubquery %s :indexed %s :pages %ld :tuples %.0f :subplan ",
					 booltostr(node->pruneable),
					 booltostr(node->issubquery),
					 booltostr(node->indexed),
					 node->pages,
					 node->tuples);
	_outNode(str, node->subplan);

	appendStringInfo(str, " :baserestrictinfo ");
	_outNode(str, node->baserestrictinfo);
	appendStringInfo(str, " :baserestrictcost %.2f :outerjoinset ",
					 node->baserestrictcost);
	_outIntList(str, node->outerjoinset);
	appendStringInfo(str, " :joininfo ");
	_outNode(str, node->joininfo);
	appendStringInfo(str, " :innerjoin ");
	_outNode(str, node->innerjoin);
}

static void
_outIndexOptInfo(StringInfo str, IndexOptInfo *node)
{
	appendStringInfo(str, " INDEXOPTINFO :indexoid %u :pages %ld :tuples %g ",
					 node->indexoid,
					 node->pages,
					 node->tuples);
}

/*
 *	TargetEntry is a subclass of Node.
 */
static void
_outTargetEntry(StringInfo str, TargetEntry *node)
{
	appendStringInfo(str, " TARGETENTRY :resdom ");
	_outNode(str, node->resdom);

	appendStringInfo(str, " :expr ");
	_outNode(str, node->expr);
}

static void
_outRangeTblEntry(StringInfo str, RangeTblEntry *node)
{
	appendStringInfo(str, " RTE :relname ");
	_outToken(str, node->relname);
	appendStringInfo(str, " :relid %u ",
					 node->relid);
	appendStringInfo(str, " :subquery ");
	_outNode(str, node->subquery);
	appendStringInfo(str, " :alias ");
	_outNode(str, node->alias);
	appendStringInfo(str, " :eref ");
	_outNode(str, node->eref);
	appendStringInfo(str, " :inh %s :inFromCl %s :checkForRead %s"
					 " :checkForWrite %s :checkAsUser %u",
					 booltostr(node->inh),
					 booltostr(node->inFromCl),
					 booltostr(node->checkForRead),
					 booltostr(node->checkForWrite),
					 node->checkAsUser);
}

/*
 *	Path is a subclass of Node.
 */
static void
_outPath(StringInfo str, Path *node)
{
	appendStringInfo(str,
	 " PATH :pathtype %d :startup_cost %.2f :total_cost %.2f :pathkeys ",
					 node->pathtype,
					 node->startup_cost,
					 node->total_cost);
	_outNode(str, node->pathkeys);
}

/*
 *	IndexPath is a subclass of Path.
 */
static void
_outIndexPath(StringInfo str, IndexPath *node)
{
	appendStringInfo(str,
					 " INDEXPATH :pathtype %d :startup_cost %.2f :total_cost %.2f :pathkeys ",
					 node->path.pathtype,
					 node->path.startup_cost,
					 node->path.total_cost);
	_outNode(str, node->path.pathkeys);

	appendStringInfo(str, " :indexid ");
	_outOidList(str, node->indexid);

	appendStringInfo(str, " :indexqual ");
	_outNode(str, node->indexqual);

	appendStringInfo(str, " :indexscandir %d :joinrelids ",
					 (int) node->indexscandir);
	_outIntList(str, node->joinrelids);

	appendStringInfo(str, " :alljoinquals %s :rows %.2f ",
					 booltostr(node->alljoinquals),
					 node->rows);
}

/*
 *	TidPath is a subclass of Path.
 */
static void
_outTidPath(StringInfo str, TidPath *node)
{
	appendStringInfo(str,
					 " TIDPATH :pathtype %d :startup_cost %.2f :total_cost %.2f :pathkeys ",
					 node->path.pathtype,
					 node->path.startup_cost,
					 node->path.total_cost);
	_outNode(str, node->path.pathkeys);

	appendStringInfo(str, " :tideval ");
	_outNode(str, node->tideval);

	appendStringInfo(str, " :unjoined_relids ");
	_outIntList(str, node->unjoined_relids);
}

/*
 *	AppendPath is a subclass of Path.
 */
static void
_outAppendPath(StringInfo str, AppendPath *node)
{
	appendStringInfo(str,
					 " APPENDPATH :pathtype %d :startup_cost %.2f :total_cost %.2f :pathkeys ",
					 node->path.pathtype,
					 node->path.startup_cost,
					 node->path.total_cost);
	_outNode(str, node->path.pathkeys);

	appendStringInfo(str, " :subpaths ");
	_outNode(str, node->subpaths);
}

/*
 *	NestPath is a subclass of Path
 */
static void
_outNestPath(StringInfo str, NestPath *node)
{
	appendStringInfo(str,
					 " NESTPATH :pathtype %d :startup_cost %.2f :total_cost %.2f :pathkeys ",
					 node->path.pathtype,
					 node->path.startup_cost,
					 node->path.total_cost);
	_outNode(str, node->path.pathkeys);
	appendStringInfo(str, " :jointype %d :outerjoinpath ",
					 (int) node->jointype);
	_outNode(str, node->outerjoinpath);
	appendStringInfo(str, " :innerjoinpath ");
	_outNode(str, node->innerjoinpath);
	appendStringInfo(str, " :joinrestrictinfo ");
	_outNode(str, node->joinrestrictinfo);
}

/*
 *	MergePath is a subclass of NestPath.
 */
static void
_outMergePath(StringInfo str, MergePath *node)
{
	appendStringInfo(str,
					 " MERGEPATH :pathtype %d :startup_cost %.2f :total_cost %.2f :pathkeys ",
					 node->jpath.path.pathtype,
					 node->jpath.path.startup_cost,
					 node->jpath.path.total_cost);
	_outNode(str, node->jpath.path.pathkeys);
	appendStringInfo(str, " :jointype %d :outerjoinpath ",
					 (int) node->jpath.jointype);
	_outNode(str, node->jpath.outerjoinpath);
	appendStringInfo(str, " :innerjoinpath ");
	_outNode(str, node->jpath.innerjoinpath);
	appendStringInfo(str, " :joinrestrictinfo ");
	_outNode(str, node->jpath.joinrestrictinfo);

	appendStringInfo(str, " :path_mergeclauses ");
	_outNode(str, node->path_mergeclauses);

	appendStringInfo(str, " :outersortkeys ");
	_outNode(str, node->outersortkeys);

	appendStringInfo(str, " :innersortkeys ");
	_outNode(str, node->innersortkeys);
}

/*
 *	HashPath is a subclass of NestPath.
 */
static void
_outHashPath(StringInfo str, HashPath *node)
{
	appendStringInfo(str,
					 " HASHPATH :pathtype %d :startup_cost %.2f :total_cost %.2f :pathkeys ",
					 node->jpath.path.pathtype,
					 node->jpath.path.startup_cost,
					 node->jpath.path.total_cost);
	_outNode(str, node->jpath.path.pathkeys);
	appendStringInfo(str, " :jointype %d :outerjoinpath ",
					 (int) node->jpath.jointype);
	_outNode(str, node->jpath.outerjoinpath);
	appendStringInfo(str, " :innerjoinpath ");
	_outNode(str, node->jpath.innerjoinpath);
	appendStringInfo(str, " :joinrestrictinfo ");
	_outNode(str, node->jpath.joinrestrictinfo);

	appendStringInfo(str, " :path_hashclauses ");
	_outNode(str, node->path_hashclauses);
}

/*
 *	PathKeyItem is a subclass of Node.
 */
static void
_outPathKeyItem(StringInfo str, PathKeyItem *node)
{
	appendStringInfo(str, " PATHKEYITEM :sortop %u :key ",
					 node->sortop);
	_outNode(str, node->key);
}

/*
 *	RestrictInfo is a subclass of Node.
 */
static void
_outRestrictInfo(StringInfo str, RestrictInfo *node)
{
	appendStringInfo(str, " RESTRICTINFO :clause ");
	_outNode(str, node->clause);

	appendStringInfo(str, " :ispusheddown %s :subclauseindices ",
					 booltostr(node->ispusheddown));
	_outNode(str, node->subclauseindices);

	appendStringInfo(str, " :mergejoinoperator %u ", node->mergejoinoperator);
	appendStringInfo(str, " :left_sortop %u ", node->left_sortop);
	appendStringInfo(str, " :right_sortop %u ", node->right_sortop);
	appendStringInfo(str, " :hashjoinoperator %u ", node->hashjoinoperator);
}

/*
 *	JoinInfo is a subclass of Node.
 */
static void
_outJoinInfo(StringInfo str, JoinInfo *node)
{
	appendStringInfo(str, " JINFO :unjoined_relids ");
	_outIntList(str, node->unjoined_relids);

	appendStringInfo(str, " :jinfo_restrictinfo ");
	_outNode(str, node->jinfo_restrictinfo);
}

/*
 * Print the value of a Datum given its type.
 */
static void
_outDatum(StringInfo str, Datum value, int typlen, bool typbyval)
{
	Size		length,
				i;
	char	   *s;

	length = datumGetSize(value, typbyval, typlen);

	if (typbyval)
	{
		s = (char *) (&value);
		appendStringInfo(str, " %u [ ", (unsigned int) length);
		for (i = 0; i < (Size) sizeof(Datum); i++)
			appendStringInfo(str, "%d ", (int) (s[i]));
		appendStringInfo(str, "] ");
	}
	else
	{
		s = (char *) DatumGetPointer(value);
		if (!PointerIsValid(s))
			appendStringInfo(str, " 0 [ ] ");
		else
		{
			appendStringInfo(str, " %u [ ", (unsigned int) length);
			for (i = 0; i < length; i++)
				appendStringInfo(str, "%d ", (int) (s[i]));
			appendStringInfo(str, "] ");
		}
	}
}

static void
_outIter(StringInfo str, Iter *node)
{
	appendStringInfo(str, " ITER :iterexpr ");
	_outNode(str, node->iterexpr);
}

static void
_outStream(StringInfo str, Stream *node)
{
	appendStringInfo(str,
					 " STREAM :pathptr @ %p :cinfo @ %p :clausetype %p :upstream @ %p ",
					 node->pathptr,
					 node->cinfo,
					 node->clausetype,
					 node->upstream);

	appendStringInfo(str,
		   " :downstream @ %p :groupup %d :groupcost %f :groupsel %f ",
					 node->downstream,
					 node->groupup,
					 node->groupcost,
					 node->groupsel);
}

static void
_outAExpr(StringInfo str, A_Expr *node)
{
	appendStringInfo(str, " AEXPR ");
	switch (node->oper)
	{
		case AND:
			appendStringInfo(str, "AND ");
			break;
		case OR:
			appendStringInfo(str, "OR ");
			break;
		case NOT:
			appendStringInfo(str, "NOT ");
			break;
		case ISNULL:
			appendStringInfo(str, "ISNULL ");
			break;
		case NOTNULL:
			appendStringInfo(str, "NOTNULL ");
			break;
		case OP:
			_outToken(str, node->opname);
			appendStringInfo(str, " ");
			break;
		default:
			appendStringInfo(str, "?? ");
			break;
	}
	_outNode(str, node->lexpr);
	appendStringInfo(str, " ");
	_outNode(str, node->rexpr);
}

static void
_outValue(StringInfo str, Value *value)
{
	switch (value->type)
	{
		case T_Integer:
			appendStringInfo(str, " %ld ", value->val.ival);
			break;
		case T_Float:

			/*
			 * We assume the value is a valid numeric literal and so does
			 * not need quoting.
			 */
			appendStringInfo(str, " %s ", value->val.str);
			break;
		case T_String:
			appendStringInfo(str, " \"");
			_outToken(str, value->val.str);
			appendStringInfo(str, "\" ");
			break;
		case T_BitString:
			/* internal representation already has leading 'b' */
			appendStringInfo(str, " %s ", value->val.str);
			break;
		default:
			elog(NOTICE, "_outValue: don't know how to print type %d ",
				 value->type);
			break;
	}
}

static void
_outIdent(StringInfo str, Ident *node)
{
	appendStringInfo(str, " IDENT ");
	_outToken(str, node->name);
}

static void
_outAttr(StringInfo str, Attr *node)
{
	appendStringInfo(str, " ATTR :relname ");
	_outToken(str, node->relname);
	appendStringInfo(str, " :attrs ");
	_outNode(str, node->attrs);
}

static void
_outAConst(StringInfo str, A_Const *node)
{
	appendStringInfo(str, "CONST ");
	_outValue(str, &(node->val));
	appendStringInfo(str, " :typename ");
	_outNode(str, node->typename);
}

static void
_outConstraint(StringInfo str, Constraint *node)
{
	appendStringInfo(str, " ");
	_outToken(str, node->name);
	appendStringInfo(str, " :type ");

	switch (node->contype)
	{
		case CONSTR_PRIMARY:
			appendStringInfo(str, "PRIMARY KEY ");
			_outNode(str, node->keys);
			break;

		case CONSTR_CHECK:
			appendStringInfo(str, "CHECK :raw ");
			_outNode(str, node->raw_expr);
			appendStringInfo(str, " :cooked ");
			_outToken(str, node->cooked_expr);
			break;

		case CONSTR_DEFAULT:
			appendStringInfo(str, "DEFAULT :raw ");
			_outNode(str, node->raw_expr);
			appendStringInfo(str, " :cooked ");
			_outToken(str, node->cooked_expr);
			break;

		case CONSTR_NOTNULL:
			appendStringInfo(str, "NOT NULL");
			break;

		case CONSTR_UNIQUE:
			appendStringInfo(str, "UNIQUE ");
			_outNode(str, node->keys);
			break;

		default:
			appendStringInfo(str, "<unrecognized_constraint>");
			break;
	}
}

static void
_outCaseExpr(StringInfo str, CaseExpr *node)
{
	appendStringInfo(str, " CASE :casetype %u :arg ",
					 node->casetype);
	_outNode(str, node->arg);

	appendStringInfo(str, " :args ");
	_outNode(str, node->args);

	appendStringInfo(str, " :defresult ");
	_outNode(str, node->defresult);
}

static void
_outCaseWhen(StringInfo str, CaseWhen *node)
{
	appendStringInfo(str, " WHEN ");
	_outNode(str, node->expr);

	appendStringInfo(str, " :then ");
	_outNode(str, node->result);
}

/*
 * _outNode -
 *	  converts a Node into ascii string and append it to 'str'
 */
static void
_outNode(StringInfo str, void *obj)
{
	if (obj == NULL)
	{
		appendStringInfo(str, "<>");
		return;
	}

	if (IsA(obj, List))
	{
		List	   *l;

		appendStringInfoChar(str, '(');
		foreach(l, (List *) obj)
		{
			_outNode(str, lfirst(l));
			if (lnext(l))
				appendStringInfoChar(str, ' ');
		}
		appendStringInfoChar(str, ')');
	}
	else if (IsA(obj, Integer) || IsA(obj, Float) || IsA(obj, String) || IsA(obj, BitString))
	{
		/* nodeRead does not want to see { } around these! */
		_outValue(str, obj);
	}
	else
	{
		appendStringInfoChar(str, '{');
		switch (nodeTag(obj))
		{
			case T_CreateStmt:
				_outCreateStmt(str, obj);
				break;
			case T_IndexStmt:
				_outIndexStmt(str, obj);
				break;
			case T_ColumnDef:
				_outColumnDef(str, obj);
				break;
			case T_TypeName:
				_outTypeName(str, obj);
				break;
			case T_TypeCast:
				_outTypeCast(str, obj);
				break;
			case T_IndexElem:
				_outIndexElem(str, obj);
				break;
			case T_Query:
				_outQuery(str, obj);
				break;
			case T_SortClause:
				_outSortClause(str, obj);
				break;
			case T_GroupClause:
				_outGroupClause(str, obj);
				break;
			case T_SetOperationStmt:
				_outSetOperationStmt(str, obj);
				break;
			case T_Plan:
				_outPlan(str, obj);
				break;
			case T_Result:
				_outResult(str, obj);
				break;
			case T_Append:
				_outAppend(str, obj);
				break;
			case T_Join:
				_outJoin(str, obj);
				break;
			case T_NestLoop:
				_outNestLoop(str, obj);
				break;
			case T_MergeJoin:
				_outMergeJoin(str, obj);
				break;
			case T_HashJoin:
				_outHashJoin(str, obj);
				break;
			case T_Scan:
				_outScan(str, obj);
				break;
			case T_SeqScan:
				_outSeqScan(str, obj);
				break;
			case T_IndexScan:
				_outIndexScan(str, obj);
				break;
			case T_TidScan:
				_outTidScan(str, obj);
				break;
			case T_SubqueryScan:
				_outSubqueryScan(str, obj);
				break;
			case T_Material:
				_outMaterial(str, obj);
				break;
			case T_Sort:
				_outSort(str, obj);
				break;
			case T_Agg:
				_outAgg(str, obj);
				break;
			case T_Group:
				_outGroup(str, obj);
				break;
			case T_Unique:
				_outUnique(str, obj);
				break;
			case T_SetOp:
				_outSetOp(str, obj);
				break;
			case T_Limit:
				_outLimit(str, obj);
				break;
			case T_Hash:
				_outHash(str, obj);
				break;
			case T_SubPlan:
				_outSubPlan(str, obj);
				break;
			case T_Resdom:
				_outResdom(str, obj);
				break;
			case T_Fjoin:
				_outFjoin(str, obj);
				break;
			case T_Expr:
				_outExpr(str, obj);
				break;
			case T_Var:
				_outVar(str, obj);
				break;
			case T_Const:
				_outConst(str, obj);
				break;
			case T_Aggref:
				_outAggref(str, obj);
				break;
			case T_SubLink:
				_outSubLink(str, obj);
				break;
			case T_ArrayRef:
				_outArrayRef(str, obj);
				break;
			case T_Func:
				_outFunc(str, obj);
				break;
			case T_Oper:
				_outOper(str, obj);
				break;
			case T_Param:
				_outParam(str, obj);
				break;
			case T_FieldSelect:
				_outFieldSelect(str, obj);
				break;
			case T_RelabelType:
				_outRelabelType(str, obj);
				break;
			case T_RangeTblRef:
				_outRangeTblRef(str, obj);
				break;
			case T_FromExpr:
				_outFromExpr(str, obj);
				break;
			case T_JoinExpr:
				_outJoinExpr(str, obj);
				break;
			case T_RelOptInfo:
				_outRelOptInfo(str, obj);
				break;
			case T_IndexOptInfo:
				_outIndexOptInfo(str, obj);
				break;
			case T_TargetEntry:
				_outTargetEntry(str, obj);
				break;
			case T_RangeTblEntry:
				_outRangeTblEntry(str, obj);
				break;
			case T_Path:
				_outPath(str, obj);
				break;
			case T_IndexPath:
				_outIndexPath(str, obj);
				break;
			case T_TidPath:
				_outTidPath(str, obj);
				break;
			case T_AppendPath:
				_outAppendPath(str, obj);
				break;
			case T_NestPath:
				_outNestPath(str, obj);
				break;
			case T_MergePath:
				_outMergePath(str, obj);
				break;
			case T_HashPath:
				_outHashPath(str, obj);
				break;
			case T_PathKeyItem:
				_outPathKeyItem(str, obj);
				break;
			case T_RestrictInfo:
				_outRestrictInfo(str, obj);
				break;
			case T_JoinInfo:
				_outJoinInfo(str, obj);
				break;
			case T_Iter:
				_outIter(str, obj);
				break;
			case T_Stream:
				_outStream(str, obj);
				break;
			case T_A_Expr:
				_outAExpr(str, obj);
				break;
			case T_Ident:
				_outIdent(str, obj);
				break;
			case T_A_Const:
				_outAConst(str, obj);
				break;
			case T_Constraint:
				_outConstraint(str, obj);
				break;
			case T_CaseExpr:
				_outCaseExpr(str, obj);
				break;
			case T_CaseWhen:
				_outCaseWhen(str, obj);
				break;

			case T_VariableSetStmt:
				break;
			case T_SelectStmt:
				_outSelectStmt(str, obj);
				break;
			case T_FuncCall:
				_outFuncCall(str, obj);
				break;
			case T_Attr:
				_outAttr(str, obj);
				break;

			default:
				elog(NOTICE, "_outNode: don't know how to print type %d ",
					 nodeTag(obj));
				break;
		}
		appendStringInfoChar(str, '}');
	}
}

/*
 * nodeToString -
 *	   returns the ascii representation of the Node as a palloc'd string
 */
char *
nodeToString(void *obj)
{
	StringInfoData str;

	/* see stringinfo.h for an explanation of this maneuver */
	initStringInfo(&str);
	_outNode(&str, obj);
	return str.data;
}
