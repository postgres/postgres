/*-------------------------------------------------------------------------
 *
 * readfuncs.c
 *	  Reader functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/readfuncs.c,v 1.103 2001/01/07 01:08:47 tgl Exp $
 *
 * NOTES
 *	  Most of the read functions for plan nodes are tested. (In fact, they
 *	  pass the regression test as of 11/8/94.) The rest (for path selection)
 *	  are probably never used. No effort has been made to get them to work.
 *	  The simplest way to test these functions is by doing the following in
 *	  ProcessQuery (before executing the plan):
 *				plan = stringToNode(nodeToString(plan));
 *	  Then, run the regression test. Let's just say you'll notice if either
 *	  of the above function are not properly done.
 *														- ay 11/94
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "nodes/plannodes.h"
#include "nodes/readfuncs.h"
#include "nodes/relation.h"


static Datum readDatum(bool typbyval);


/* ----------------
 *		node creator declarations
 * ----------------
 */

static List *
toIntList(List *list)
{
	List	   *l;

	foreach(l, list)
	{
		/* ugly manipulation, should probably free the Value node too */
		lfirst(l) = (void *) intVal(lfirst(l));
	}
	return list;
}

/* ----------------
 *		_readQuery
 * ----------------
 */
static Query *
_readQuery(void)
{
	Query	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Query);

	token = pg_strtok(&length);		/* skip the :command */
	token = pg_strtok(&length);		/* get the commandType */
	local_node->commandType = atoi(token);

	token = pg_strtok(&length);		/* skip :utility */
	token = pg_strtok(&length);
	if (length == 0)
		local_node->utilityStmt = NULL;
	else
	{
		/*
		 * Hack to make up for lack of readfuncs for utility-stmt nodes
		 *
		 * we can't get create or index here, can we?
		 */
		NotifyStmt *n = makeNode(NotifyStmt);

		n->relname = debackslash(token, length);
		local_node->utilityStmt = (Node *) n;
	}

	token = pg_strtok(&length);		/* skip the :resultRelation */
	token = pg_strtok(&length);		/* get the resultRelation */
	local_node->resultRelation = atoi(token);

	token = pg_strtok(&length);		/* skip :into */
	token = pg_strtok(&length);		/* get into */
	if (length == 0)
		local_node->into = NULL;
	else
		local_node->into = debackslash(token, length);

	token = pg_strtok(&length);		/* skip :isPortal */
	token = pg_strtok(&length);		/* get isPortal */
	local_node->isPortal = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* skip :isBinary */
	token = pg_strtok(&length);		/* get isBinary */
	local_node->isBinary = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* skip :isTemp */
	token = pg_strtok(&length);		/* get isTemp */
	local_node->isTemp = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* skip the :hasAggs */
	token = pg_strtok(&length);		/* get hasAggs */
	local_node->hasAggs = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* skip the :hasSubLinks */
	token = pg_strtok(&length);		/* get hasSubLinks */
	local_node->hasSubLinks = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* skip :rtable */
	local_node->rtable = nodeRead(true);

	token = pg_strtok(&length);		/* skip :jointree */
	local_node->jointree = nodeRead(true);

	token = pg_strtok(&length);		/* skip :rowMarks */
	local_node->rowMarks = toIntList(nodeRead(true));

	token = pg_strtok(&length);		/* skip :targetlist */
	local_node->targetList = nodeRead(true);

	token = pg_strtok(&length);		/* skip :groupClause */
	local_node->groupClause = nodeRead(true);

	token = pg_strtok(&length);		/* skip :havingQual */
	local_node->havingQual = nodeRead(true);

	token = pg_strtok(&length);		/* skip :distinctClause */
	local_node->distinctClause = nodeRead(true);

	token = pg_strtok(&length);		/* skip :sortClause */
	local_node->sortClause = nodeRead(true);

	token = pg_strtok(&length);		/* skip :limitOffset */
	local_node->limitOffset = nodeRead(true);

	token = pg_strtok(&length);		/* skip :limitCount */
	local_node->limitCount = nodeRead(true);

	token = pg_strtok(&length);		/* skip :setOperations */
	local_node->setOperations = nodeRead(true);

	token = pg_strtok(&length);		/* skip :resultRelations */
	local_node->resultRelations = toIntList(nodeRead(true));

	return local_node;
}

/* ----------------
 *		_readSortClause
 * ----------------
 */
static SortClause *
_readSortClause(void)
{
	SortClause *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(SortClause);

	token = pg_strtok(&length);		/* skip :tleSortGroupRef */
	token = pg_strtok(&length);		/* get tleSortGroupRef */
	local_node->tleSortGroupRef = strtoul(token, NULL, 10);

	token = pg_strtok(&length);		/* skip :sortop */
	token = pg_strtok(&length);		/* get sortop */
	local_node->sortop = strtoul(token, NULL, 10);

	return local_node;
}

/* ----------------
 *		_readGroupClause
 * ----------------
 */
static GroupClause *
_readGroupClause(void)
{
	GroupClause *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(GroupClause);

	token = pg_strtok(&length);		/* skip :tleSortGroupRef */
	token = pg_strtok(&length);		/* get tleSortGroupRef */
	local_node->tleSortGroupRef = strtoul(token, NULL, 10);

	token = pg_strtok(&length);		/* skip :sortop */
	token = pg_strtok(&length);		/* get sortop */
	local_node->sortop = strtoul(token, NULL, 10);

	return local_node;
}

/* ----------------
 *		_readSetOperationStmt
 * ----------------
 */
static SetOperationStmt *
_readSetOperationStmt(void)
{
	SetOperationStmt *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(SetOperationStmt);

	token = pg_strtok(&length);		/* eat :op */
	token = pg_strtok(&length);		/* get op */
	local_node->op = (SetOperation) atoi(token);

	token = pg_strtok(&length);		/* eat :all */
	token = pg_strtok(&length);		/* get all */
	local_node->all = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* eat :larg */
	local_node->larg = nodeRead(true);	/* get larg */

	token = pg_strtok(&length);		/* eat :rarg */
	local_node->rarg = nodeRead(true);	/* get rarg */

	token = pg_strtok(&length);		/* eat :colTypes */
	local_node->colTypes = toIntList(nodeRead(true));

	return local_node;
}

/* ----------------
 *		_getPlan
 * ----------------
 */
static void
_getPlan(Plan *node)
{
	char	   *token;
	int			length;

	token = pg_strtok(&length);		/* first token is :startup_cost */
	token = pg_strtok(&length);		/* next is the actual cost */
	node->startup_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* skip the :total_cost */
	token = pg_strtok(&length);		/* next is the actual cost */
	node->total_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* skip the :rows */
	token = pg_strtok(&length);		/* get the plan_rows */
	node->plan_rows = atof(token);

	token = pg_strtok(&length);		/* skip the :width */
	token = pg_strtok(&length);		/* get the plan_width */
	node->plan_width = atoi(token);

	token = pg_strtok(&length);		/* eat :qptargetlist */
	node->targetlist = nodeRead(true);

	token = pg_strtok(&length);		/* eat :qpqual */
	node->qual = nodeRead(true);

	token = pg_strtok(&length);		/* eat :lefttree */
	node->lefttree = (Plan *) nodeRead(true);

	token = pg_strtok(&length);		/* eat :righttree */
	node->righttree = (Plan *) nodeRead(true);

	node->state = (EState *) NULL;		/* never read in */

	return;
}

/*
 *	Stuff from plannodes.h
 */

/* ----------------
 *		_readPlan
 * ----------------
 */
static Plan *
_readPlan(void)
{
	Plan	   *local_node;

	local_node = makeNode(Plan);

	_getPlan(local_node);

	return local_node;
}

/* ----------------
 *		_readResult
 * ----------------
 */
static Result *
_readResult(void)
{
	Result	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Result);

	_getPlan((Plan *) local_node);

	token = pg_strtok(&length);		/* eat :resconstantqual */
	local_node->resconstantqual = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readAppend
 *
 *	Append is a subclass of Plan.
 * ----------------
 */

static Append *
_readAppend(void)
{
	Append	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Append);

	_getPlan((Plan *) local_node);

	token = pg_strtok(&length);		/* eat :appendplans */
	local_node->appendplans = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :isTarget */
	token = pg_strtok(&length);		/* get isTarget */
	local_node->isTarget = (token[0] == 't') ? true : false;

	return local_node;
}

/* ----------------
 *		_getJoin
 * ----------------
 */
static void
_getJoin(Join *node)
{
	char	   *token;
	int			length;

	_getPlan((Plan *) node);

	token = pg_strtok(&length);		/* skip the :jointype */
	token = pg_strtok(&length);		/* get the jointype */
	node->jointype = (JoinType) atoi(token);

	token = pg_strtok(&length);		/* skip the :joinqual */
	node->joinqual = nodeRead(true);	/* get the joinqual */
}


/* ----------------
 *		_readJoin
 *
 *	Join is a subclass of Plan
 * ----------------
 */
static Join *
_readJoin(void)
{
	Join	   *local_node;

	local_node = makeNode(Join);

	_getJoin(local_node);

	return local_node;
}

/* ----------------
 *		_readNestLoop
 *
 *	NestLoop is a subclass of Join
 * ----------------
 */

static NestLoop *
_readNestLoop(void)
{
	NestLoop   *local_node;

	local_node = makeNode(NestLoop);

	_getJoin((Join *) local_node);

	return local_node;
}

/* ----------------
 *		_readMergeJoin
 *
 *	MergeJoin is a subclass of Join
 * ----------------
 */
static MergeJoin *
_readMergeJoin(void)
{
	MergeJoin  *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(MergeJoin);

	_getJoin((Join *) local_node);

	token = pg_strtok(&length);		/* eat :mergeclauses */
	local_node->mergeclauses = nodeRead(true);	/* now read it */

	return local_node;
}

/* ----------------
 *		_readHashJoin
 *
 *	HashJoin is a subclass of Join.
 * ----------------
 */
static HashJoin *
_readHashJoin(void)
{
	HashJoin   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(HashJoin);

	_getJoin((Join *) local_node);

	token = pg_strtok(&length);		/* eat :hashclauses */
	local_node->hashclauses = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :hashjoinop */
	token = pg_strtok(&length);		/* get hashjoinop */
	local_node->hashjoinop = strtoul(token, NULL, 10);

	return local_node;
}

/* ----------------
 *		_getScan
 *
 *	Scan is a subclass of Plan.
 *
 *	Scan gets its own get function since stuff inherits it.
 * ----------------
 */
static void
_getScan(Scan *node)
{
	char	   *token;
	int			length;

	_getPlan((Plan *) node);

	token = pg_strtok(&length);		/* eat :scanrelid */
	token = pg_strtok(&length);		/* get scanrelid */
	node->scanrelid = strtoul(token, NULL, 10);
}

/* ----------------
 *		_readScan
 *
 * Scan is a subclass of Plan.
 * ----------------
 */
static Scan *
_readScan(void)
{
	Scan	   *local_node;

	local_node = makeNode(Scan);

	_getScan(local_node);

	return local_node;
}

/* ----------------
 *		_readSeqScan
 *
 *	SeqScan is a subclass of Scan
 * ----------------
 */
static SeqScan *
_readSeqScan(void)
{
	SeqScan    *local_node;

	local_node = makeNode(SeqScan);

	_getScan((Scan *) local_node);

	return local_node;
}

/* ----------------
 *		_readIndexScan
 *
 *	IndexScan is a subclass of Scan
 * ----------------
 */
static IndexScan *
_readIndexScan(void)
{
	IndexScan  *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(IndexScan);

	_getScan((Scan *) local_node);

	token = pg_strtok(&length);		/* eat :indxid */
	local_node->indxid = toIntList(nodeRead(true));		/* now read it */

	token = pg_strtok(&length);		/* eat :indxqual */
	local_node->indxqual = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* eat :indxqualorig */
	local_node->indxqualorig = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :indxorderdir */
	token = pg_strtok(&length);		/* get indxorderdir */
	local_node->indxorderdir = atoi(token);

	return local_node;
}

/* ----------------
 *		_readTidScan
 *
 *	TidScan is a subclass of Scan
 * ----------------
 */
static TidScan *
_readTidScan(void)
{
	TidScan    *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(TidScan);

	_getScan((Scan *) local_node);

	token = pg_strtok(&length);		/* eat :needrescan */
	token = pg_strtok(&length);		/* get needrescan */
	local_node->needRescan = atoi(token);

	token = pg_strtok(&length);		/* eat :tideval */
	local_node->tideval = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readSubqueryScan
 *
 *	SubqueryScan is a subclass of Scan
 * ----------------
 */
static SubqueryScan *
_readSubqueryScan(void)
{
	SubqueryScan  *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(SubqueryScan);

	_getScan((Scan *) local_node);

	token = pg_strtok(&length);			/* eat :subplan */
	local_node->subplan = nodeRead(true);	/* now read it */

	return local_node;
}

/* ----------------
 *		_readSort
 *
 *	Sort is a subclass of Plan
 * ----------------
 */
static Sort *
_readSort(void)
{
	Sort	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Sort);

	_getPlan((Plan *) local_node);

	token = pg_strtok(&length);		/* eat :keycount */
	token = pg_strtok(&length);		/* get keycount */
	local_node->keycount = atoi(token);

	return local_node;
}

static Agg *
_readAgg(void)
{
	Agg		   *local_node;

	local_node = makeNode(Agg);
	_getPlan((Plan *) local_node);

	return local_node;
}

/* ----------------
 *		_readHash
 *
 *	Hash is a subclass of Plan
 * ----------------
 */
static Hash *
_readHash(void)
{
	Hash	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Hash);

	_getPlan((Plan *) local_node);

	token = pg_strtok(&length);		/* eat :hashkey */
	local_node->hashkey = nodeRead(true);

	return local_node;
}

/*
 *	Stuff from primnodes.h.
 */

/* ----------------
 *		_readResdom
 *
 *	Resdom is a subclass of Node
 * ----------------
 */
static Resdom *
_readResdom(void)
{
	Resdom	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Resdom);

	token = pg_strtok(&length);		/* eat :resno */
	token = pg_strtok(&length);		/* get resno */
	local_node->resno = atoi(token);

	token = pg_strtok(&length);		/* eat :restype */
	token = pg_strtok(&length);		/* get restype */
	local_node->restype = (Oid) atol(token);

	token = pg_strtok(&length);		/* eat :restypmod */
	token = pg_strtok(&length);		/* get restypmod */
	local_node->restypmod = atoi(token);

	token = pg_strtok(&length);		/* eat :resname */
	token = pg_strtok(&length);		/* get the name */
	if (length == 0)
		local_node->resname = NULL;
	else
		local_node->resname = debackslash(token, length);

	token = pg_strtok(&length);		/* eat :reskey */
	token = pg_strtok(&length);		/* get reskey */
	local_node->reskey = strtoul(token, NULL, 10);

	token = pg_strtok(&length);		/* eat :reskeyop */
	token = pg_strtok(&length);		/* get reskeyop */
	local_node->reskeyop = (Oid) atol(token);

	token = pg_strtok(&length);		/* eat :ressortgroupref */
	token = pg_strtok(&length);		/* get ressortgroupref */
	local_node->ressortgroupref = strtoul(token, NULL, 10);

	token = pg_strtok(&length);		/* eat :resjunk */
	token = pg_strtok(&length);		/* get resjunk */
	local_node->resjunk = (token[0] == 't') ? true : false;

	return local_node;
}

/* ----------------
 *		_readExpr
 *
 *	Expr is a subclass of Node
 * ----------------
 */
static Expr *
_readExpr(void)
{
	Expr	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Expr);

	token = pg_strtok(&length);		/* eat :typeOid */
	token = pg_strtok(&length);		/* get typeOid */
	local_node->typeOid = (Oid) atol(token);

	token = pg_strtok(&length);		/* eat :opType */
	token = pg_strtok(&length);		/* get opType */
	if (!strncmp(token, "op", 2))
		local_node->opType = OP_EXPR;
	else if (!strncmp(token, "func", 4))
		local_node->opType = FUNC_EXPR;
	else if (!strncmp(token, "or", 2))
		local_node->opType = OR_EXPR;
	else if (!strncmp(token, "and", 3))
		local_node->opType = AND_EXPR;
	else if (!strncmp(token, "not", 3))
		local_node->opType = NOT_EXPR;
	else if (!strncmp(token, "subp", 4))
		local_node->opType = SUBPLAN_EXPR;
	else
		elog(ERROR, "_readExpr: unknown opType \"%.10s\"", token);

	token = pg_strtok(&length);		/* eat :oper */
	local_node->oper = nodeRead(true);

	token = pg_strtok(&length);		/* eat :args */
	local_node->args = nodeRead(true);	/* now read it */

	return local_node;
}

/* ----------------
 *		_readCaseExpr
 *
 *	CaseExpr is a subclass of Node
 * ----------------
 */
static CaseExpr *
_readCaseExpr(void)
{
	CaseExpr   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(CaseExpr);

	token = pg_strtok(&length);		/* eat :casetype */
	token = pg_strtok(&length);		/* get casetype */
	local_node->casetype = (Oid) atol(token);

	token = pg_strtok(&length);		/* eat :arg */
	local_node->arg = nodeRead(true);

	token = pg_strtok(&length);		/* eat :args */
	local_node->args = nodeRead(true);

	token = pg_strtok(&length);		/* eat :defresult */
	local_node->defresult = nodeRead(true);

	return local_node;
}

/* ----------------
 *		_readCaseWhen
 *
 *	CaseWhen is a subclass of Node
 * ----------------
 */
static CaseWhen *
_readCaseWhen(void)
{
	CaseWhen   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(CaseWhen);

	local_node->expr = nodeRead(true);
	token = pg_strtok(&length);		/* eat :then */
	local_node->result = nodeRead(true);

	return local_node;
}

/* ----------------
 *		_readVar
 *
 *	Var is a subclass of Expr
 * ----------------
 */
static Var *
_readVar(void)
{
	Var		   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Var);

	token = pg_strtok(&length);		/* eat :varno */
	token = pg_strtok(&length);		/* get varno */
	local_node->varno = strtoul(token, NULL, 10);

	token = pg_strtok(&length);		/* eat :varattno */
	token = pg_strtok(&length);		/* get varattno */
	local_node->varattno = atoi(token);

	token = pg_strtok(&length);		/* eat :vartype */
	token = pg_strtok(&length);		/* get vartype */
	local_node->vartype = (Oid) atol(token);

	token = pg_strtok(&length);		/* eat :vartypmod */
	token = pg_strtok(&length);		/* get vartypmod */
	local_node->vartypmod = atoi(token);

	token = pg_strtok(&length);		/* eat :varlevelsup */
	token = pg_strtok(&length);		/* get varlevelsup */
	local_node->varlevelsup = (Index) atoi(token);

	token = pg_strtok(&length);		/* eat :varnoold */
	token = pg_strtok(&length);		/* get varnoold */
	local_node->varnoold = (Index) atoi(token);

	token = pg_strtok(&length);		/* eat :varoattno */
	token = pg_strtok(&length);		/* eat :varoattno */
	local_node->varoattno = atoi(token);

	return local_node;
}

/* ----------------
 * _readArrayRef
 *
 * ArrayRef is a subclass of Expr
 * ----------------
 */
static ArrayRef *
_readArrayRef(void)
{
	ArrayRef   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(ArrayRef);

	token = pg_strtok(&length);		/* eat :refelemtype */
	token = pg_strtok(&length);		/* get refelemtype */
	local_node->refelemtype = strtoul(token, NULL, 10);

	token = pg_strtok(&length);		/* eat :refattrlength */
	token = pg_strtok(&length);		/* get refattrlength */
	local_node->refattrlength = atoi(token);

	token = pg_strtok(&length);		/* eat :refelemlength */
	token = pg_strtok(&length);		/* get refelemlength */
	local_node->refelemlength = atoi(token);

	token = pg_strtok(&length);		/* eat :refelembyval */
	token = pg_strtok(&length);		/* get refelembyval */
	local_node->refelembyval = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* eat :refupperindex */
	local_node->refupperindexpr = nodeRead(true);

	token = pg_strtok(&length);		/* eat :reflowerindex */
	local_node->reflowerindexpr = nodeRead(true);

	token = pg_strtok(&length);		/* eat :refexpr */
	local_node->refexpr = nodeRead(true);

	token = pg_strtok(&length);		/* eat :refassgnexpr */
	local_node->refassgnexpr = nodeRead(true);

	return local_node;
}

/* ----------------
 *		_readConst
 *
 *	Const is a subclass of Expr
 * ----------------
 */
static Const *
_readConst(void)
{
	Const	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Const);

	token = pg_strtok(&length);		/* get :consttype */
	token = pg_strtok(&length);		/* now read it */
	local_node->consttype = (Oid) atol(token);

	token = pg_strtok(&length);		/* get :constlen */
	token = pg_strtok(&length);		/* now read it */
	local_node->constlen = strtol(token, NULL, 10);

	token = pg_strtok(&length);		/* get :constbyval */
	token = pg_strtok(&length);		/* now read it */

	if (!strncmp(token, "true", 4))
		local_node->constbyval = true;
	else
		local_node->constbyval = false;

	token = pg_strtok(&length);		/* get :constisnull */
	token = pg_strtok(&length);		/* now read it */

	if (!strncmp(token, "true", 4))
		local_node->constisnull = true;
	else
		local_node->constisnull = false;

	token = pg_strtok(&length);		/* get :constvalue */

	if (local_node->constisnull)
	{
		token = pg_strtok(&length);	/* skip "NIL" */
	}
	else
	{
		local_node->constvalue = readDatum(local_node->constbyval);
	}

	return local_node;
}

/* ----------------
 *		_readFunc
 *
 *	Func is a subclass of Expr
 * ----------------
 */
static Func *
_readFunc(void)
{
	Func	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Func);

	token = pg_strtok(&length);		/* get :funcid */
	token = pg_strtok(&length);		/* now read it */
	local_node->funcid = (Oid) atol(token);

	token = pg_strtok(&length);		/* get :functype */
	token = pg_strtok(&length);		/* now read it */
	local_node->functype = (Oid) atol(token);

	local_node->func_fcache = NULL;

	return local_node;
}

/* ----------------
 *		_readOper
 *
 *	Oper is a subclass of Expr
 * ----------------
 */
static Oper *
_readOper(void)
{
	Oper	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Oper);

	token = pg_strtok(&length);		/* get :opno */
	token = pg_strtok(&length);		/* now read it */
	local_node->opno = (Oid) atol(token);

	token = pg_strtok(&length);		/* get :opid */
	token = pg_strtok(&length);		/* now read it */
	local_node->opid = (Oid) atol(token);

	token = pg_strtok(&length);		/* get :opresulttype */
	token = pg_strtok(&length);		/* now read it */
	local_node->opresulttype = (Oid) atol(token);

	local_node->op_fcache = NULL;

	return local_node;
}

/* ----------------
 *		_readParam
 *
 *	Param is a subclass of Expr
 * ----------------
 */
static Param *
_readParam(void)
{
	Param	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Param);

	token = pg_strtok(&length);		/* get :paramkind */
	token = pg_strtok(&length);		/* now read it */
	local_node->paramkind = atoi(token);

	token = pg_strtok(&length);		/* get :paramid */
	token = pg_strtok(&length);		/* now read it */
	local_node->paramid = atol(token);

	token = pg_strtok(&length);		/* get :paramname */
	token = pg_strtok(&length);		/* now read it */
	if (length == 0)
		local_node->paramname = NULL;
	else
		local_node->paramname = debackslash(token, length);

	token = pg_strtok(&length);		/* get :paramtype */
	token = pg_strtok(&length);		/* now read it */
	local_node->paramtype = (Oid) atol(token);

	return local_node;
}

/* ----------------
 *		_readAggref
 *
 *	Aggref is a subclass of Node
 * ----------------
 */
static Aggref *
_readAggref(void)
{
	Aggref	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Aggref);

	token = pg_strtok(&length);		/* eat :aggname */
	token = pg_strtok(&length);		/* get aggname */
	local_node->aggname = debackslash(token, length);

	token = pg_strtok(&length);		/* eat :basetype */
	token = pg_strtok(&length);		/* get basetype */
	local_node->basetype = (Oid) atol(token);

	token = pg_strtok(&length);		/* eat :aggtype */
	token = pg_strtok(&length);		/* get aggtype */
	local_node->aggtype = (Oid) atol(token);

	token = pg_strtok(&length);		/* eat :target */
	local_node->target = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* eat :aggstar */
	token = pg_strtok(&length);		/* get aggstar */
	local_node->aggstar = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* eat :aggdistinct */
	token = pg_strtok(&length);		/* get aggdistinct */
	local_node->aggdistinct = (token[0] == 't') ? true : false;

	return local_node;
}

/* ----------------
 *		_readSubLink
 *
 *	SubLink is a subclass of Node
 * ----------------
 */
static SubLink *
_readSubLink(void)
{
	SubLink    *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(SubLink);

	token = pg_strtok(&length);		/* eat :subLinkType */
	token = pg_strtok(&length);		/* get subLinkType */
	local_node->subLinkType = atoi(token);

	token = pg_strtok(&length);		/* eat :useor */
	token = pg_strtok(&length);		/* get useor */
	local_node->useor = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* eat :lefthand */
	local_node->lefthand = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* eat :oper */
	local_node->oper = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :subselect */
	local_node->subselect = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readFieldSelect
 *
 *	FieldSelect is a subclass of Node
 * ----------------
 */
static FieldSelect *
_readFieldSelect(void)
{
	FieldSelect *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(FieldSelect);

	token = pg_strtok(&length);		/* eat :arg */
	local_node->arg = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :fieldnum */
	token = pg_strtok(&length);		/* get fieldnum */
	local_node->fieldnum = (AttrNumber) atoi(token);

	token = pg_strtok(&length);		/* eat :resulttype */
	token = pg_strtok(&length);		/* get resulttype */
	local_node->resulttype = (Oid) atol(token);

	token = pg_strtok(&length);		/* eat :resulttypmod */
	token = pg_strtok(&length);		/* get resulttypmod */
	local_node->resulttypmod = atoi(token);

	return local_node;
}

/* ----------------
 *		_readRelabelType
 *
 *	RelabelType is a subclass of Node
 * ----------------
 */
static RelabelType *
_readRelabelType(void)
{
	RelabelType *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(RelabelType);

	token = pg_strtok(&length);		/* eat :arg */
	local_node->arg = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :resulttype */
	token = pg_strtok(&length);		/* get resulttype */
	local_node->resulttype = (Oid) atol(token);

	token = pg_strtok(&length);		/* eat :resulttypmod */
	token = pg_strtok(&length);		/* get resulttypmod */
	local_node->resulttypmod = atoi(token);

	return local_node;
}

/* ----------------
 *		_readRangeTblRef
 *
 *	RangeTblRef is a subclass of Node
 * ----------------
 */
static RangeTblRef *
_readRangeTblRef(void)
{
	RangeTblRef *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(RangeTblRef);

	token = pg_strtok(&length);		/* get rtindex */
	local_node->rtindex = atoi(token);

	return local_node;
}

/* ----------------
 *		_readFromExpr
 *
 *	FromExpr is a subclass of Node
 * ----------------
 */
static FromExpr *
_readFromExpr(void)
{
	FromExpr   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(FromExpr);

	token = pg_strtok(&length);		/* eat :fromlist */
	local_node->fromlist = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :quals */
	local_node->quals = nodeRead(true);	/* now read it */

	return local_node;
}

/* ----------------
 *		_readJoinExpr
 *
 *	JoinExpr is a subclass of Node
 * ----------------
 */
static JoinExpr *
_readJoinExpr(void)
{
	JoinExpr   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(JoinExpr);

	token = pg_strtok(&length);		/* eat :jointype */
	token = pg_strtok(&length);		/* get jointype */
	local_node->jointype = (JoinType) atoi(token);

	token = pg_strtok(&length);		/* eat :isNatural */
	token = pg_strtok(&length);		/* get :isNatural */
	local_node->isNatural = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* eat :larg */
	local_node->larg = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :rarg */
	local_node->rarg = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :using */
	local_node->using = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :quals */
	local_node->quals = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :alias */
	local_node->alias = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :colnames */
	local_node->colnames = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* eat :colvars */
	local_node->colvars = nodeRead(true); /* now read it */

	return local_node;
}

/*
 *	Stuff from relation.h
 */

/* ----------------
 *		_readRelOptInfo
 * ----------------
 */
static RelOptInfo *
_readRelOptInfo(void)
{
	RelOptInfo *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(RelOptInfo);

	token = pg_strtok(&length);		/* get :relids */
	local_node->relids = toIntList(nodeRead(true));		/* now read it */

	token = pg_strtok(&length);		/* get :rows */
	token = pg_strtok(&length);		/* now read it */
	local_node->rows = atof(token);

	token = pg_strtok(&length);		/* get :width */
	token = pg_strtok(&length);		/* now read it */
	local_node->width = atoi(token);

	token = pg_strtok(&length);		/* get :targetlist */
	local_node->targetlist = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* get :pathlist */
	local_node->pathlist = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* get :cheapest_startup_path */
	local_node->cheapest_startup_path = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :cheapest_total_path */
	local_node->cheapest_total_path = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :pruneable */
	token = pg_strtok(&length);		/* get :pruneable */
	local_node->pruneable = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* get :issubquery */
	token = pg_strtok(&length);		/* now read it */
	local_node->issubquery = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* get :indexed */
	token = pg_strtok(&length);		/* now read it */
	local_node->indexed = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* get :pages */
	token = pg_strtok(&length);		/* now read it */
	local_node->pages = atol(token);

	token = pg_strtok(&length);		/* get :tuples */
	token = pg_strtok(&length);		/* now read it */
	local_node->tuples = atof(token);

	token = pg_strtok(&length);		/* get :subplan */
	local_node->subplan = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :baserestrictinfo */
	local_node->baserestrictinfo = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :baserestrictcost */
	token = pg_strtok(&length);		/* now read it */
	local_node->baserestrictcost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :outerjoinset */
	local_node->outerjoinset = toIntList(nodeRead(true)); /* now read it */

	token = pg_strtok(&length);		/* get :joininfo */
	local_node->joininfo = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* get :innerjoin */
	local_node->innerjoin = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readTargetEntry
 * ----------------
 */
static TargetEntry *
_readTargetEntry(void)
{
	TargetEntry *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(TargetEntry);

	token = pg_strtok(&length);		/* get :resdom */
	local_node->resdom = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* get :expr */
	local_node->expr = nodeRead(true);	/* now read it */

	return local_node;
}

static Attr *
_readAttr(void)
{
	Attr	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Attr);

	token = pg_strtok(&length);		/* eat :relname */
	token = pg_strtok(&length);		/* get relname */
	local_node->relname = debackslash(token, length);

	token = pg_strtok(&length);		/* eat :attrs */
	local_node->attrs = nodeRead(true); /* now read it */

	return local_node;
}

/* ----------------
 *		_readRangeTblEntry
 * ----------------
 */
static RangeTblEntry *
_readRangeTblEntry(void)
{
	RangeTblEntry *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(RangeTblEntry);

	token = pg_strtok(&length);		/* eat :relname */
	token = pg_strtok(&length);		/* get :relname */
	if (length == 0)
		local_node->relname = NULL;
	else
		local_node->relname = debackslash(token, length);

	token = pg_strtok(&length);		/* eat :relid */
	token = pg_strtok(&length);		/* get :relid */
	local_node->relid = strtoul(token, NULL, 10);

	token = pg_strtok(&length);		/* eat :subquery */
	local_node->subquery = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :alias */
	local_node->alias = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :eref */
	local_node->eref = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* eat :inh */
	token = pg_strtok(&length);		/* get :inh */
	local_node->inh = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* eat :inFromCl */
	token = pg_strtok(&length);		/* get :inFromCl */
	local_node->inFromCl = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* eat :checkForRead */
	token = pg_strtok(&length);		/* get :checkForRead */
	local_node->checkForRead = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* eat :checkForWrite */
	token = pg_strtok(&length);		/* get :checkForWrite */
	local_node->checkForWrite = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* eat :checkAsUser */
	token = pg_strtok(&length);		/* get :checkAsUser */
	local_node->checkAsUser = strtoul(token, NULL, 10);

	return local_node;
}

/* ----------------
 *		_readPath
 *
 *	Path is a subclass of Node.
 * ----------------
 */
static Path *
_readPath(void)
{
	Path	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Path);

	token = pg_strtok(&length);		/* get :pathtype */
	token = pg_strtok(&length);		/* now read it */
	local_node->pathtype = atol(token);

	token = pg_strtok(&length);		/* get :startup_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->startup_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :total_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->total_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :pathkeys */
	local_node->pathkeys = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readIndexPath
 *
 *	IndexPath is a subclass of Path.
 * ----------------
 */
static IndexPath *
_readIndexPath(void)
{
	IndexPath  *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(IndexPath);

	token = pg_strtok(&length);		/* get :pathtype */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.pathtype = atol(token);

	token = pg_strtok(&length);		/* get :startup_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.startup_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :total_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.total_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :pathkeys */
	local_node->path.pathkeys = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :indexid */
	local_node->indexid = toIntList(nodeRead(true));

	token = pg_strtok(&length);		/* get :indexqual */
	local_node->indexqual = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* get :indexscandir */
	token = pg_strtok(&length);		/* now read it */
	local_node->indexscandir = (ScanDirection) atoi(token);

	token = pg_strtok(&length);		/* get :joinrelids */
	local_node->joinrelids = toIntList(nodeRead(true));

	token = pg_strtok(&length);		/* get :alljoinquals */
	token = pg_strtok(&length);		/* now read it */
	local_node->alljoinquals = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* get :rows */
	token = pg_strtok(&length);		/* now read it */
	local_node->rows = atof(token);

	return local_node;
}

/* ----------------
 *		_readTidPath
 *
 *	TidPath is a subclass of Path.
 * ----------------
 */
static TidPath *
_readTidPath(void)
{
	TidPath    *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(TidPath);

	token = pg_strtok(&length);		/* get :pathtype */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.pathtype = atol(token);

	token = pg_strtok(&length);		/* get :startup_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.startup_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :total_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.total_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :pathkeys */
	local_node->path.pathkeys = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :tideval */
	local_node->tideval = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* get :unjoined_relids */
	local_node->unjoined_relids = toIntList(nodeRead(true));

	return local_node;
}

/* ----------------
 *		_readAppendPath
 *
 *	AppendPath is a subclass of Path.
 * ----------------
 */
static AppendPath *
_readAppendPath(void)
{
	AppendPath *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(AppendPath);

	token = pg_strtok(&length);		/* get :pathtype */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.pathtype = atol(token);

	token = pg_strtok(&length);		/* get :startup_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.startup_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :total_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.total_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :pathkeys */
	local_node->path.pathkeys = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :subpaths */
	local_node->subpaths = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readNestPath
 *
 *	NestPath is a subclass of Path
 * ----------------
 */
static NestPath *
_readNestPath(void)
{
	NestPath   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(NestPath);

	token = pg_strtok(&length);		/* get :pathtype */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.pathtype = atol(token);

	token = pg_strtok(&length);		/* get :startup_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.startup_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :total_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->path.total_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :pathkeys */
	local_node->path.pathkeys = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :jointype */
	token = pg_strtok(&length);		/* now read it */
	local_node->jointype = (JoinType) atoi(token);

	token = pg_strtok(&length);		/* get :outerjoinpath */
	local_node->outerjoinpath = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :innerjoinpath */
	local_node->innerjoinpath = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :joinrestrictinfo */
	local_node->joinrestrictinfo = nodeRead(true); /* now read it */

	return local_node;
}

/* ----------------
 *		_readMergePath
 *
 *	MergePath is a subclass of NestPath.
 * ----------------
 */
static MergePath *
_readMergePath(void)
{
	MergePath  *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(MergePath);

	token = pg_strtok(&length);		/* get :pathtype */
	token = pg_strtok(&length);		/* now read it */
	local_node->jpath.path.pathtype = atol(token);

	token = pg_strtok(&length);		/* get :startup_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->jpath.path.startup_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :total_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->jpath.path.total_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :pathkeys */
	local_node->jpath.path.pathkeys = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* get :jointype */
	token = pg_strtok(&length);		/* now read it */
	local_node->jpath.jointype = (JoinType) atoi(token);

	token = pg_strtok(&length);		/* get :outerjoinpath */
	local_node->jpath.outerjoinpath = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* get :innerjoinpath */
	local_node->jpath.innerjoinpath = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* get :joinrestrictinfo */
	local_node->jpath.joinrestrictinfo = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :path_mergeclauses */
	local_node->path_mergeclauses = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* get :outersortkeys */
	local_node->outersortkeys = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :innersortkeys */
	local_node->innersortkeys = nodeRead(true); /* now read it */

	return local_node;
}

/* ----------------
 *		_readHashPath
 *
 *	HashPath is a subclass of NestPath.
 * ----------------
 */
static HashPath *
_readHashPath(void)
{
	HashPath   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(HashPath);

	token = pg_strtok(&length);		/* get :pathtype */
	token = pg_strtok(&length);		/* now read it */
	local_node->jpath.path.pathtype = atol(token);

	token = pg_strtok(&length);		/* get :startup_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->jpath.path.startup_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :total_cost */
	token = pg_strtok(&length);		/* now read it */
	local_node->jpath.path.total_cost = (Cost) atof(token);

	token = pg_strtok(&length);		/* get :pathkeys */
	local_node->jpath.path.pathkeys = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* get :jointype */
	token = pg_strtok(&length);		/* now read it */
	local_node->jpath.jointype = (JoinType) atoi(token);

	token = pg_strtok(&length);		/* get :outerjoinpath */
	local_node->jpath.outerjoinpath = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* get :innerjoinpath */
	local_node->jpath.innerjoinpath = nodeRead(true);	/* now read it */

	token = pg_strtok(&length);		/* get :joinrestrictinfo */
	local_node->jpath.joinrestrictinfo = nodeRead(true); /* now read it */

	token = pg_strtok(&length);		/* get :path_hashclauses */
	local_node->path_hashclauses = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readPathKeyItem
 *
 *	PathKeyItem is a subclass of Node.
 * ----------------
 */
static PathKeyItem *
_readPathKeyItem(void)
{
	PathKeyItem *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(PathKeyItem);

	token = pg_strtok(&length);		/* get :sortop */
	token = pg_strtok(&length);		/* now read it */

	local_node->sortop = (Oid) atol(token);

	token = pg_strtok(&length);		/* get :key */
	local_node->key = nodeRead(true);	/* now read it */

	return local_node;
}

/* ----------------
 *		_readRestrictInfo
 *
 *	RestrictInfo is a subclass of Node.
 * ----------------
 */
static RestrictInfo *
_readRestrictInfo(void)
{
	RestrictInfo *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(RestrictInfo);

	token = pg_strtok(&length);		/* get :clause */
	local_node->clause = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* get :ispusheddown */
	token = pg_strtok(&length);		/* now read it */
	local_node->ispusheddown = (token[0] == 't') ? true : false;

	token = pg_strtok(&length);		/* get :subclauseindices */
	local_node->subclauseindices = nodeRead(true);		/* now read it */

	token = pg_strtok(&length);		/* get :mergejoinoperator */
	token = pg_strtok(&length);		/* now read it */
	local_node->mergejoinoperator = (Oid) atol(token);

	token = pg_strtok(&length);		/* get :left_sortop */
	token = pg_strtok(&length);		/* now read it */
	local_node->left_sortop = (Oid) atol(token);

	token = pg_strtok(&length);		/* get :right_sortop */
	token = pg_strtok(&length);		/* now read it */
	local_node->right_sortop = (Oid) atol(token);

	token = pg_strtok(&length);		/* get :hashjoinoperator */
	token = pg_strtok(&length);		/* now read it */
	local_node->hashjoinoperator = (Oid) atol(token);

	/* eval_cost is not part of saved representation; compute on first use */
	local_node->eval_cost = -1;
	/* ditto for cached pathkeys and dispersion */
	local_node->left_pathkey = NIL;
	local_node->right_pathkey = NIL;
	local_node->left_dispersion = -1;
	local_node->right_dispersion = -1;

	return local_node;
}

/* ----------------
 *		_readJoinInfo()
 *
 *	JoinInfo is a subclass of Node.
 * ----------------
 */
static JoinInfo *
_readJoinInfo(void)
{
	JoinInfo   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(JoinInfo);

	token = pg_strtok(&length);		/* get :unjoined_relids */
	local_node->unjoined_relids = toIntList(nodeRead(true));	/* now read it */

	token = pg_strtok(&length);		/* get :jinfo_restrictinfo */
	local_node->jinfo_restrictinfo = nodeRead(true);	/* now read it */

	return local_node;
}

/* ----------------
 *		_readIter()
 *
 * ----------------
 */
static Iter *
_readIter(void)
{
	Iter	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Iter);

	token = pg_strtok(&length);		/* eat :iterexpr */
	local_node->iterexpr = nodeRead(true);		/* now read it */

	return local_node;
}


/* ----------------
 *		parsePlanString
 *
 * Given a character string containing a plan, parsePlanString sets up the
 * plan structure representing that plan.
 *
 * The string to be read must already have been loaded into pg_strtok().
 * ----------------
 */
Node *
parsePlanString(void)
{
	char	   *token;
	int			length;
	void	   *return_value = NULL;

	token = pg_strtok(&length);

	if (length == 4 && strncmp(token, "PLAN", length) == 0)
		return_value = _readPlan();
	else if (length == 6 && strncmp(token, "RESULT", length) == 0)
		return_value = _readResult();
	else if (length == 6 && strncmp(token, "APPEND", length) == 0)
		return_value = _readAppend();
	else if (length == 4 && strncmp(token, "JOIN", length) == 0)
		return_value = _readJoin();
	else if (length == 8 && strncmp(token, "NESTLOOP", length) == 0)
		return_value = _readNestLoop();
	else if (length == 9 && strncmp(token, "MERGEJOIN", length) == 0)
		return_value = _readMergeJoin();
	else if (length == 8 && strncmp(token, "HASHJOIN", length) == 0)
		return_value = _readHashJoin();
	else if (length == 4 && strncmp(token, "SCAN", length) == 0)
		return_value = _readScan();
	else if (length == 7 && strncmp(token, "SEQSCAN", length) == 0)
		return_value = _readSeqScan();
	else if (length == 9 && strncmp(token, "INDEXSCAN", length) == 0)
		return_value = _readIndexScan();
	else if (length == 7 && strncmp(token, "TIDSCAN", length) == 0)
		return_value = _readTidScan();
	else if (length == 12 && strncmp(token, "SUBQUERYSCAN", length) == 0)
		return_value = _readSubqueryScan();
	else if (length == 4 && strncmp(token, "SORT", length) == 0)
		return_value = _readSort();
	else if (length == 6 && strncmp(token, "AGGREG", length) == 0)
		return_value = _readAggref();
	else if (length == 7 && strncmp(token, "SUBLINK", length) == 0)
		return_value = _readSubLink();
	else if (length == 11 && strncmp(token, "FIELDSELECT", length) == 0)
		return_value = _readFieldSelect();
	else if (length == 11 && strncmp(token, "RELABELTYPE", length) == 0)
		return_value = _readRelabelType();
	else if (length == 11 && strncmp(token, "RANGETBLREF", length) == 0)
		return_value = _readRangeTblRef();
	else if (length == 8 && strncmp(token, "FROMEXPR", length) == 0)
		return_value = _readFromExpr();
	else if (length == 8 && strncmp(token, "JOINEXPR", length) == 0)
		return_value = _readJoinExpr();
	else if (length == 3 && strncmp(token, "AGG", length) == 0)
		return_value = _readAgg();
	else if (length == 4 && strncmp(token, "HASH", length) == 0)
		return_value = _readHash();
	else if (length == 6 && strncmp(token, "RESDOM", length) == 0)
		return_value = _readResdom();
	else if (length == 4 && strncmp(token, "EXPR", length) == 0)
		return_value = _readExpr();
	else if (length == 8 && strncmp(token, "ARRAYREF", length) == 0)
		return_value = _readArrayRef();
	else if (length == 3 && strncmp(token, "VAR", length) == 0)
		return_value = _readVar();
	else if (length == 4 && strncmp(token, "ATTR", length) == 0)
		return_value = _readAttr();
	else if (length == 5 && strncmp(token, "CONST", length) == 0)
		return_value = _readConst();
	else if (length == 4 && strncmp(token, "FUNC", length) == 0)
		return_value = _readFunc();
	else if (length == 4 && strncmp(token, "OPER", length) == 0)
		return_value = _readOper();
	else if (length == 5 && strncmp(token, "PARAM", length) == 0)
		return_value = _readParam();
	else if (length == 10 && strncmp(token, "RELOPTINFO", length) == 0)
		return_value = _readRelOptInfo();
	else if (length == 11 && strncmp(token, "TARGETENTRY", length) == 0)
		return_value = _readTargetEntry();
	else if (length == 3 && strncmp(token, "RTE", length) == 0)
		return_value = _readRangeTblEntry();
	else if (length == 4 && strncmp(token, "PATH", length) == 0)
		return_value = _readPath();
	else if (length == 9 && strncmp(token, "INDEXPATH", length) == 0)
		return_value = _readIndexPath();
	else if (length == 7 && strncmp(token, "TIDPATH", length) == 0)
		return_value = _readTidPath();
	else if (length == 10 && strncmp(token, "APPENDPATH", length) == 0)
		return_value = _readAppendPath();
	else if (length == 8 && strncmp(token, "NESTPATH", length) == 0)
		return_value = _readNestPath();
	else if (length == 9 && strncmp(token, "MERGEPATH", length) == 0)
		return_value = _readMergePath();
	else if (length == 8 && strncmp(token, "HASHPATH", length) == 0)
		return_value = _readHashPath();
	else if (length == 11 && strncmp(token, "PATHKEYITEM", length) == 0)
		return_value = _readPathKeyItem();
	else if (length == 12 && strncmp(token, "RESTRICTINFO", length) == 0)
		return_value = _readRestrictInfo();
	else if (length == 8 && strncmp(token, "JOININFO", length) == 0)
		return_value = _readJoinInfo();
	else if (length == 4 && strncmp(token, "ITER", length) == 0)
		return_value = _readIter();
	else if (length == 5 && strncmp(token, "QUERY", length) == 0)
		return_value = _readQuery();
	else if (length == 10 && strncmp(token, "SORTCLAUSE", length) == 0)
		return_value = _readSortClause();
	else if (length == 11 && strncmp(token, "GROUPCLAUSE", length) == 0)
		return_value = _readGroupClause();
	else if (length == 16 && strncmp(token, "SETOPERATIONSTMT", length) == 0)
		return_value = _readSetOperationStmt();
	else if (length == 4 && strncmp(token, "CASE", length) == 0)
		return_value = _readCaseExpr();
	else if (length == 4 && strncmp(token, "WHEN", length) == 0)
		return_value = _readCaseWhen();
	else
		elog(ERROR, "badly formatted planstring \"%.10s\"...", token);

	return (Node *) return_value;
}

/*------------------------------------------------------------*/

/* ----------------
 *		readDatum
 *
 * Given a string representation of a constant, recreate the appropriate
 * Datum.  The string representation embeds length info, but not byValue,
 * so we must be told that.
 * ----------------
 */
static Datum
readDatum(bool typbyval)
{
	int			length;
	int			tokenLength;
	char	   *token;
	Datum		res;
	char	   *s;
	int			i;

	/*
	 * read the actual length of the value
	 */
	token = pg_strtok(&tokenLength);
	length = atoi(token);

	token = pg_strtok(&tokenLength); /* skip the '[' */

	if (typbyval)
	{
		if ((Size) length > sizeof(Datum))
			elog(ERROR, "readDatum: byval & length = %d", length);
		res = (Datum) 0;
		s = (char *) (&res);
		for (i = 0; i < (int) sizeof(Datum); i++)
		{
			token = pg_strtok(&tokenLength);
			s[i] = (char) atoi(token);
		}
	}
	else if (length <= 0)
		res = (Datum) NULL;
	else
	{
		s = (char *) palloc(length);
		for (i = 0; i < length; i++)
		{
			token = pg_strtok(&tokenLength);
			s[i] = (char) atoi(token);
		}
		res = PointerGetDatum(s);
	}

	token = pg_strtok(&tokenLength); /* skip the ']' */
	if (token == NULL || token[0] != ']')
		elog(ERROR, "readDatum: ']' expected, length = %d", length);

	return res;
}
