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
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/readfuncs.c,v 1.91 2000/06/18 22:44:05 tgl Exp $
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
#include <math.h>

#include "postgres.h"

#include "nodes/plannodes.h"
#include "nodes/readfuncs.h"
#include "nodes/relation.h"
#include "utils/lsyscache.h"

/* ----------------
 *		node creator declarations
 * ----------------
 */

static Datum readDatum(Oid type);

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
_readQuery()
{
	Query	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Query);

	token = lsptok(NULL, &length);		/* skip the :command */
	token = lsptok(NULL, &length);		/* get the commandType */
	local_node->commandType = atoi(token);

	token = lsptok(NULL, &length);		/* skip :utility */
	/* we can't get create or index here, can we? */

	token = lsptok(NULL, &length);		/* get the notify name if any */
	if (length == 0)
		local_node->utilityStmt = NULL;
	else
	{
		NotifyStmt *n = makeNode(NotifyStmt);

		n->relname = debackslash(token, length);
		local_node->utilityStmt = (Node *) n;
	}

	token = lsptok(NULL, &length);		/* skip the :resultRelation */
	token = lsptok(NULL, &length);		/* get the resultRelation */
	local_node->resultRelation = atoi(token);

	token = lsptok(NULL, &length);		/* skip :into */
	token = lsptok(NULL, &length);		/* get into */
	if (length == 0)
		local_node->into = NULL;
	else
		local_node->into = debackslash(token, length);

	token = lsptok(NULL, &length);		/* skip :isPortal */
	token = lsptok(NULL, &length);		/* get isPortal */
	local_node->isPortal = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* skip :isBinary */
	token = lsptok(NULL, &length);		/* get isBinary */
	local_node->isBinary = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* skip :isTemp */
	token = lsptok(NULL, &length);		/* get isTemp */
	local_node->isTemp = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* skip :unionall */
	token = lsptok(NULL, &length);		/* get unionall */
	local_node->unionall = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* skip :distinctClause */
	local_node->distinctClause = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :sortClause */
	local_node->sortClause = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :rtable */
	local_node->rtable = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :targetlist */
	local_node->targetList = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :qual */
	local_node->qual = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :groupClause */
	local_node->groupClause = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :havingQual */
	local_node->havingQual = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip the :hasAggs */
	token = lsptok(NULL, &length);		/* get hasAggs */
	local_node->hasAggs = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* skip the :hasSubLinks */
	token = lsptok(NULL, &length);		/* get hasSubLinks */
	local_node->hasSubLinks = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* skip :unionClause */
	local_node->unionClause = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :intersectClause */
	local_node->intersectClause = nodeRead(true);


	token = lsptok(NULL, &length);		/* skip :limitOffset */
	local_node->limitOffset = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :limitCount */
	local_node->limitCount = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :rowMark */
	local_node->rowMark = nodeRead(true);

	return local_node;
}

/* ----------------
 *		_readSortClause
 * ----------------
 */
static SortClause *
_readSortClause()
{
	SortClause *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(SortClause);

	token = lsptok(NULL, &length);		/* skip :tleSortGroupRef */
	token = lsptok(NULL, &length);		/* get tleSortGroupRef */
	local_node->tleSortGroupRef = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* skip :sortop */
	token = lsptok(NULL, &length);		/* get sortop */
	local_node->sortop = strtoul(token, NULL, 10);

	return local_node;
}

/* ----------------
 *		_readGroupClause
 * ----------------
 */
static GroupClause *
_readGroupClause()
{
	GroupClause *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(GroupClause);

	token = lsptok(NULL, &length);		/* skip :tleSortGroupRef */
	token = lsptok(NULL, &length);		/* get tleSortGroupRef */
	local_node->tleSortGroupRef = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* skip :sortop */
	token = lsptok(NULL, &length);		/* get sortop */
	local_node->sortop = strtoul(token, NULL, 10);

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

	token = lsptok(NULL, &length);		/* first token is :startup_cost */
	token = lsptok(NULL, &length);		/* next is the actual cost */
	node->startup_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* skip the :total_cost */
	token = lsptok(NULL, &length);		/* next is the actual cost */
	node->total_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* skip the :rows */
	token = lsptok(NULL, &length);		/* get the plan_rows */
	node->plan_rows = atof(token);

	token = lsptok(NULL, &length);		/* skip the :width */
	token = lsptok(NULL, &length);		/* get the plan_width */
	node->plan_width = atoi(token);

	token = lsptok(NULL, &length);		/* eat the :state stuff */
	token = lsptok(NULL, &length);		/* now get the state */

	if (length == 0)
		node->state = (EState *) NULL;
	else
	{							/* Disgusting hack until I figure out what
								 * to do here */
		node->state = (EState *) !NULL;
	}

	token = lsptok(NULL, &length);		/* eat :qptargetlist */
	node->targetlist = nodeRead(true);

	token = lsptok(NULL, &length);		/* eat :qpqual */
	node->qual = nodeRead(true);

	token = lsptok(NULL, &length);		/* eat :lefttree */
	node->lefttree = (Plan *) nodeRead(true);

	token = lsptok(NULL, &length);		/* eat :righttree */
	node->righttree = (Plan *) nodeRead(true);

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
_readPlan()
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
_readResult()
{
	Result	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Result);

	_getPlan((Plan *) local_node);

	token = lsptok(NULL, &length);		/* eat :resconstantqual */
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
_readAppend()
{
	Append	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Append);

	_getPlan((Plan *) local_node);

	token = lsptok(NULL, &length);		/* eat :appendplans */
	local_node->appendplans = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* eat :unionrtables */
	local_node->unionrtables = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* eat :inheritrelid */
	token = lsptok(NULL, &length);		/* get inheritrelid */
	local_node->inheritrelid = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :inheritrtable */
	local_node->inheritrtable = nodeRead(true); /* now read it */

	return local_node;
}

/* ----------------
 *		_getJoin
 *
 * In case Join is not the same structure as Plan someday.
 * ----------------
 */
static void
_getJoin(Join *node)
{
	_getPlan((Plan *) node);
}


/* ----------------
 *		_readJoin
 *
 *	Join is a subclass of Plan
 * ----------------
 */
static Join *
_readJoin()
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
_readNestLoop()
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
_readMergeJoin()
{
	MergeJoin  *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(MergeJoin);

	_getJoin((Join *) local_node);
	token = lsptok(NULL, &length);		/* eat :mergeclauses */
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
_readHashJoin()
{
	HashJoin   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(HashJoin);

	_getJoin((Join *) local_node);

	token = lsptok(NULL, &length);		/* eat :hashclauses */
	local_node->hashclauses = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* eat :hashjoinop */
	token = lsptok(NULL, &length);		/* get hashjoinop */
	local_node->hashjoinop = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :hashdone */
	token = lsptok(NULL, &length);		/* eat hashdone */
	local_node->hashdone = false;

	return local_node;
}

/* ----------------
 *		_getScan
 *
 *	Scan is a subclass of Node
 *	(Actually, according to the plannodes.h include file, it is a
 *	subclass of Plan.  This is why _getPlan is used here.)
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

	token = lsptok(NULL, &length);		/* eat :scanrelid */
	token = lsptok(NULL, &length);		/* get scanrelid */
	node->scanrelid = strtoul(token, NULL, 10);
}

/* ----------------
 *		_readScan
 *
 * Scan is a subclass of Plan (Not Node, see above).
 * ----------------
 */
static Scan *
_readScan()
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
_readSeqScan()
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
_readIndexScan()
{
	IndexScan  *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(IndexScan);

	_getScan((Scan *) local_node);

	token = lsptok(NULL, &length);		/* eat :indxid */
	local_node->indxid = toIntList(nodeRead(true));		/* now read it */

	token = lsptok(NULL, &length);		/* eat :indxqual */
	local_node->indxqual = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* eat :indxqualorig */
	local_node->indxqualorig = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* eat :indxorderdir */
	token = lsptok(NULL, &length);		/* get indxorderdir */
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
_readTidScan()
{
	TidScan    *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(TidScan);

	_getScan((Scan *) local_node);

	token = lsptok(NULL, &length);		/* eat :needrescan */
	token = lsptok(NULL, &length);		/* get needrescan */
	local_node->needRescan = atoi(token);

	token = lsptok(NULL, &length);		/* eat :tideval */
	local_node->tideval = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readSort
 *
 *	Sort is a subclass of Plan
 * ----------------
 */
static Sort *
_readSort()
{
	Sort	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Sort);

	_getPlan((Plan *) local_node);

	token = lsptok(NULL, &length);		/* eat :keycount */
	token = lsptok(NULL, &length);		/* get keycount */
	local_node->keycount = atoi(token);

	return local_node;
}

static Agg *
_readAgg()
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
_readHash()
{
	Hash	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Hash);

	_getPlan((Plan *) local_node);

	token = lsptok(NULL, &length);		/* eat :hashkey */
	local_node->hashkey = (Var *) nodeRead(true);

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
_readResdom()
{
	Resdom	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Resdom);

	token = lsptok(NULL, &length);		/* eat :resno */
	token = lsptok(NULL, &length);		/* get resno */
	local_node->resno = atoi(token);

	token = lsptok(NULL, &length);		/* eat :restype */
	token = lsptok(NULL, &length);		/* get restype */
	local_node->restype = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :restypmod */
	token = lsptok(NULL, &length);		/* get restypmod */
	local_node->restypmod = atoi(token);

	token = lsptok(NULL, &length);		/* eat :resname */
	token = lsptok(NULL, &length);		/* get the name */
	if (length == 0)
		local_node->resname = NULL;
	else
		local_node->resname = debackslash(token, length);

	token = lsptok(NULL, &length);		/* eat :reskey */
	token = lsptok(NULL, &length);		/* get reskey */
	local_node->reskey = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :reskeyop */
	token = lsptok(NULL, &length);		/* get reskeyop */
	local_node->reskeyop = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :ressortgroupref */
	token = lsptok(NULL, &length);		/* get ressortgroupref */
	local_node->ressortgroupref = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :resjunk */
	token = lsptok(NULL, &length);		/* get resjunk */
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
_readExpr()
{
	Expr	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Expr);

	token = lsptok(NULL, &length);		/* eat :typeOid */
	token = lsptok(NULL, &length);		/* get typeOid */
	local_node->typeOid = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :opType */
	token = lsptok(NULL, &length);		/* get opType */
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

	token = lsptok(NULL, &length);		/* eat :oper */
	local_node->oper = nodeRead(true);

	token = lsptok(NULL, &length);		/* eat :args */
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
_readCaseExpr()
{
	CaseExpr   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(CaseExpr);

	token = lsptok(NULL, &length);		/* eat :casetype */
	token = lsptok(NULL, &length);		/* get casetype */
	local_node->casetype = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :arg */
	local_node->arg = nodeRead(true);

	token = lsptok(NULL, &length);		/* eat :args */
	local_node->args = nodeRead(true);

	token = lsptok(NULL, &length);		/* eat :defresult */
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
_readCaseWhen()
{
	CaseWhen   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(CaseWhen);

	local_node->expr = nodeRead(true);
	token = lsptok(NULL, &length);		/* eat :then */
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
_readVar()
{
	Var		   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Var);

	token = lsptok(NULL, &length);		/* eat :varno */
	token = lsptok(NULL, &length);		/* get varno */
	local_node->varno = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :varattno */
	token = lsptok(NULL, &length);		/* get varattno */
	local_node->varattno = atoi(token);

	token = lsptok(NULL, &length);		/* eat :vartype */
	token = lsptok(NULL, &length);		/* get vartype */
	local_node->vartype = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :vartypmod */
	token = lsptok(NULL, &length);		/* get vartypmod */
	local_node->vartypmod = atoi(token);

	token = lsptok(NULL, &length);		/* eat :varlevelsup */
	token = lsptok(NULL, &length);		/* get varlevelsup */
	local_node->varlevelsup = (Index) atoi(token);

	token = lsptok(NULL, &length);		/* eat :varnoold */
	token = lsptok(NULL, &length);		/* get varnoold */
	local_node->varnoold = (Index) atoi(token);

	token = lsptok(NULL, &length);		/* eat :varoattno */
	token = lsptok(NULL, &length);		/* eat :varoattno */
	local_node->varoattno = atoi(token);

	return local_node;
}

/* ----------------
 * _readArray
 *
 * Array is a subclass of Expr
 * ----------------
 */
static Array *
_readArray()
{
	Array	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Array);

	token = lsptok(NULL, &length);		/* eat :arrayelemtype */
	token = lsptok(NULL, &length);		/* get arrayelemtype */
	local_node->arrayelemtype = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :arrayelemlength */
	token = lsptok(NULL, &length);		/* get arrayelemlength */
	local_node->arrayelemlength = atoi(token);

	token = lsptok(NULL, &length);		/* eat :arrayelembyval */
	token = lsptok(NULL, &length);		/* get arrayelembyval */
	local_node->arrayelembyval = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* eat :arraylow */
	token = lsptok(NULL, &length);		/* get arraylow */
	local_node->arraylow.indx[0] = atoi(token);

	token = lsptok(NULL, &length);		/* eat :arrayhigh */
	token = lsptok(NULL, &length);		/* get arrayhigh */
	local_node->arrayhigh.indx[0] = atoi(token);

	token = lsptok(NULL, &length);		/* eat :arraylen */
	token = lsptok(NULL, &length);		/* get arraylen */
	local_node->arraylen = atoi(token);

	return local_node;
}

/* ----------------
 * _readArrayRef
 *
 * ArrayRef is a subclass of Expr
 * ----------------
 */
static ArrayRef *
_readArrayRef()
{
	ArrayRef   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(ArrayRef);

	token = lsptok(NULL, &length);		/* eat :refelemtype */
	token = lsptok(NULL, &length);		/* get refelemtype */
	local_node->refelemtype = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :refattrlength */
	token = lsptok(NULL, &length);		/* get refattrlength */
	local_node->refattrlength = atoi(token);

	token = lsptok(NULL, &length);		/* eat :refelemlength */
	token = lsptok(NULL, &length);		/* get refelemlength */
	local_node->refelemlength = atoi(token);

	token = lsptok(NULL, &length);		/* eat :refelembyval */
	token = lsptok(NULL, &length);		/* get refelembyval */
	local_node->refelembyval = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* eat :refupperindex */
	local_node->refupperindexpr = nodeRead(true);

	token = lsptok(NULL, &length);		/* eat :reflowerindex */
	local_node->reflowerindexpr = nodeRead(true);

	token = lsptok(NULL, &length);		/* eat :refexpr */
	local_node->refexpr = nodeRead(true);

	token = lsptok(NULL, &length);		/* eat :refassgnexpr */
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
_readConst()
{
	Const	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Const);

	token = lsptok(NULL, &length);		/* get :consttype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->consttype = (Oid) atol(token);


	token = lsptok(NULL, &length);		/* get :constlen */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->constlen = strtol(token, NULL, 10);

	token = lsptok(NULL, &length);		/* get :constisnull */
	token = lsptok(NULL, &length);		/* now read it */

	if (!strncmp(token, "true", 4))
		local_node->constisnull = true;
	else
		local_node->constisnull = false;


	token = lsptok(NULL, &length);		/* get :constvalue */

	if (local_node->constisnull)
	{
		token = lsptok(NULL, &length);	/* skip "NIL" */
	}
	else
	{

		/*
		 * read the value
		 */
		local_node->constvalue = readDatum(local_node->consttype);
	}

	token = lsptok(NULL, &length);		/* get :constbyval */
	token = lsptok(NULL, &length);		/* now read it */

	if (!strncmp(token, "true", 4))
		local_node->constbyval = true;
	else
		local_node->constbyval = false;

	return local_node;
}

/* ----------------
 *		_readFunc
 *
 *	Func is a subclass of Expr
 * ----------------
 */
static Func *
_readFunc()
{
	Func	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Func);

	token = lsptok(NULL, &length);		/* get :funcid */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->funcid = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* get :functype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->functype = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* get :funcisindex */
	token = lsptok(NULL, &length);		/* now read it */

	if (!strncmp(token, "true", 4))
		local_node->funcisindex = true;
	else
		local_node->funcisindex = false;

	token = lsptok(NULL, &length);		/* get :funcsize */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->funcsize = atol(token);

	token = lsptok(NULL, &length);		/* get :func_fcache */
	token = lsptok(NULL, &length);		/* get @ */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->func_fcache = (FunctionCache *) NULL;

	token = lsptok(NULL, &length);		/* get :func_tlist */
	local_node->func_tlist = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* get :func_planlist */
	local_node->func_planlist = nodeRead(true); /* now read it */

	return local_node;
}

/* ----------------
 *		_readOper
 *
 *	Oper is a subclass of Expr
 * ----------------
 */
static Oper *
_readOper()
{
	Oper	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Oper);

	token = lsptok(NULL, &length);		/* get :opno */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->opno = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* get :opid */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->opid = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* get :opresulttype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->opresulttype = (Oid) atol(token);

	/*
	 * NOTE: Alternatively we can call 'replace_opid' which initializes
	 * both 'opid' and 'op_fcache'.
	 */
	local_node->op_fcache = (FunctionCache *) NULL;

	return local_node;
}

/* ----------------
 *		_readParam
 *
 *	Param is a subclass of Expr
 * ----------------
 */
static Param *
_readParam()
{
	Param	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Param);

	token = lsptok(NULL, &length);		/* get :paramkind */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->paramkind = atoi(token);

	token = lsptok(NULL, &length);		/* get :paramid */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->paramid = atol(token);

	token = lsptok(NULL, &length);		/* get :paramname */
	token = lsptok(NULL, &length);		/* now read it */
	if (length == 0)
		local_node->paramname = NULL;
	else
		local_node->paramname = debackslash(token, length);

	token = lsptok(NULL, &length);		/* get :paramtype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->paramtype = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* get :param_tlist */
	local_node->param_tlist = nodeRead(true);	/* now read it */

	return local_node;
}

/* ----------------
 *		_readAggref
 *
 *	Aggref is a subclass of Node
 * ----------------
 */
static Aggref *
_readAggref()
{
	Aggref	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Aggref);

	token = lsptok(NULL, &length);		/* eat :aggname */
	token = lsptok(NULL, &length);		/* get aggname */
	local_node->aggname = debackslash(token, length);

	token = lsptok(NULL, &length);		/* eat :basetype */
	token = lsptok(NULL, &length);		/* get basetype */
	local_node->basetype = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :aggtype */
	token = lsptok(NULL, &length);		/* get aggtype */
	local_node->aggtype = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :target */
	local_node->target = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* eat :usenulls */
	token = lsptok(NULL, &length);		/* get usenulls */
	local_node->usenulls = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* eat :aggstar */
	token = lsptok(NULL, &length);		/* get aggstar */
	local_node->aggstar = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* eat :aggdistinct */
	token = lsptok(NULL, &length);		/* get aggdistinct */
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
_readSubLink()
{
	SubLink    *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(SubLink);

	token = lsptok(NULL, &length);		/* eat :subLinkType */
	token = lsptok(NULL, &length);		/* get subLinkType */
	local_node->subLinkType = atoi(token);

	token = lsptok(NULL, &length);		/* eat :useor */
	token = lsptok(NULL, &length);		/* get useor */
	local_node->useor = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* eat :lefthand */
	local_node->lefthand = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* eat :oper */
	local_node->oper = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* eat :subselect */
	local_node->subselect = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readRelabelType
 *
 *	RelabelType is a subclass of Node
 * ----------------
 */
static RelabelType *
_readRelabelType()
{
	RelabelType *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(RelabelType);

	token = lsptok(NULL, &length);		/* eat :arg */
	local_node->arg = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* eat :resulttype */
	token = lsptok(NULL, &length);		/* get resulttype */
	local_node->resulttype = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :resulttypmod */
	token = lsptok(NULL, &length);		/* get resulttypmod */
	local_node->resulttypmod = atoi(token);

	return local_node;
}

/*
 *	Stuff from execnodes.h
 */

/* ----------------
 *		_readEState
 *
 *	EState is a subclass of Node.
 * ----------------
 */
static EState *
_readEState()
{
	EState	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(EState);

	token = lsptok(NULL, &length);		/* get :direction */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->es_direction = atoi(token);

	token = lsptok(NULL, &length);		/* get :range_table */

	local_node->es_range_table = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :result_relation_info */
	token = lsptok(NULL, &length);		/* get @ */
	token = lsptok(NULL, &length);		/* now read it */

	sscanf(token, "%x", (unsigned int *) &local_node->es_result_relation_info);

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
_readRelOptInfo()
{
	RelOptInfo *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(RelOptInfo);

	token = lsptok(NULL, &length);		/* get :relids */
	local_node->relids = toIntList(nodeRead(true));		/* now read it */

	token = lsptok(NULL, &length);		/* get :rows */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->rows = atof(token);

	token = lsptok(NULL, &length);		/* get :width */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->width = atoi(token);

	token = lsptok(NULL, &length);		/* get :indexed */
	token = lsptok(NULL, &length);		/* now read it */

	if (!strncmp(token, "true", 4))
		local_node->indexed = true;
	else
		local_node->indexed = false;

	token = lsptok(NULL, &length);		/* get :pages */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->pages = atol(token);

	token = lsptok(NULL, &length);		/* get :tuples */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->tuples = atof(token);

	token = lsptok(NULL, &length);		/* get :targetlist */
	local_node->targetlist = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* get :pathlist */
	local_node->pathlist = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :cheapest_startup_path */
	local_node->cheapest_startup_path = nodeRead(true); /* now read it */

	token = lsptok(NULL, &length);		/* get :cheapest_total_path */
	local_node->cheapest_total_path = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* eat :pruneable */
	token = lsptok(NULL, &length);		/* get :pruneable */
	local_node->pruneable = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* get :baserestrictinfo */
	local_node->baserestrictinfo = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :joininfo */
	local_node->joininfo = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :innerjoin */
	local_node->innerjoin = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readTargetEntry
 * ----------------
 */
static TargetEntry *
_readTargetEntry()
{
	TargetEntry *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(TargetEntry);

	token = lsptok(NULL, &length);		/* get :resdom */
	local_node->resdom = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :expr */
	local_node->expr = nodeRead(true);	/* now read it */

	return local_node;
}

static Attr *
_readAttr()
{
	Attr	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Attr);

	token = lsptok(NULL, &length);		/* eat :relname */
	token = lsptok(NULL, &length);		/* get relname */
	local_node->relname = debackslash(token, length);

	token = lsptok(NULL, &length);		/* eat :attrs */
	local_node->attrs = nodeRead(true); /* now read it */

	return local_node;
}

/* ----------------
 *		_readRangeTblEntry
 * ----------------
 */
static RangeTblEntry *
_readRangeTblEntry()
{
	RangeTblEntry *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(RangeTblEntry);

	token = lsptok(NULL, &length);		/* eat :relname */
	token = lsptok(NULL, &length);		/* get :relname */
	if (length == 0)
		local_node->relname = NULL;
	else
		local_node->relname = debackslash(token, length);

	token = lsptok(NULL, &length);		/* eat :ref */
	local_node->ref = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* eat :relid */
	token = lsptok(NULL, &length);		/* get :relid */
	local_node->relid = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :inh */
	token = lsptok(NULL, &length);		/* get :inh */
	local_node->inh = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* eat :inFromCl */
	token = lsptok(NULL, &length);		/* get :inFromCl */
	local_node->inFromCl = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* eat :inJoinSet */
	token = lsptok(NULL, &length);		/* get :inJoinSet */
	local_node->inJoinSet = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* eat :skipAcl */
	token = lsptok(NULL, &length);		/* get :skipAcl */
	local_node->skipAcl = (token[0] == 't') ? true : false;

	return local_node;
}

static RowMark *
_readRowMark()
{
	RowMark    *local_node = makeNode(RowMark);
	char	   *token;
	int			length;

	token = lsptok(NULL, &length);		/* eat :rti */
	token = lsptok(NULL, &length);		/* get :rti */
	local_node->rti = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :info */
	token = lsptok(NULL, &length);		/* get :info */
	local_node->info = strtoul(token, NULL, 10);

	return local_node;
}

/* ----------------
 *		_readPath
 *
 *	Path is a subclass of Node.
 * ----------------
 */
static Path *
_readPath()
{
	Path	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Path);

	token = lsptok(NULL, &length);		/* get :pathtype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->pathtype = atol(token);

	token = lsptok(NULL, &length);		/* get :startup_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->startup_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :total_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->total_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :pathkeys */
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
_readIndexPath()
{
	IndexPath  *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(IndexPath);

	token = lsptok(NULL, &length);		/* get :pathtype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.pathtype = atol(token);

	token = lsptok(NULL, &length);		/* get :startup_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.startup_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :total_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.total_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :pathkeys */
	local_node->path.pathkeys = nodeRead(true); /* now read it */

	token = lsptok(NULL, &length);		/* get :indexid */
	local_node->indexid = toIntList(nodeRead(true));

	token = lsptok(NULL, &length);		/* get :indexqual */
	local_node->indexqual = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :indexscandir */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->indexscandir = (ScanDirection) atoi(token);

	token = lsptok(NULL, &length);		/* get :joinrelids */
	local_node->joinrelids = toIntList(nodeRead(true));

	token = lsptok(NULL, &length);		/* get :rows */
	token = lsptok(NULL, &length);		/* now read it */
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
_readTidPath()
{
	TidPath    *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(TidPath);

	token = lsptok(NULL, &length);		/* get :pathtype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.pathtype = atol(token);

	token = lsptok(NULL, &length);		/* get :startup_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.startup_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :total_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.total_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :pathkeys */
	local_node->path.pathkeys = nodeRead(true); /* now read it */

	token = lsptok(NULL, &length);		/* get :tideval */
	local_node->tideval = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :unjoined_relids */
	local_node->unjoined_relids = toIntList(nodeRead(true));

	return local_node;
}

/* ----------------
 *		_readNestPath
 *
 *	NestPath is a subclass of Path
 * ----------------
 */
static NestPath *
_readNestPath()
{
	NestPath   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(NestPath);

	token = lsptok(NULL, &length);		/* get :pathtype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.pathtype = atol(token);

	token = lsptok(NULL, &length);		/* get :startup_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.startup_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :total_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.total_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :pathkeys */
	local_node->path.pathkeys = nodeRead(true); /* now read it */

	token = lsptok(NULL, &length);		/* get :outerjoinpath */
	local_node->outerjoinpath = nodeRead(true); /* now read it */

	token = lsptok(NULL, &length);		/* get :innerjoinpath */
	local_node->innerjoinpath = nodeRead(true); /* now read it */

	token = lsptok(NULL, &length);		/* get :joinrestrictinfo */
	local_node->joinrestrictinfo = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readMergePath
 *
 *	MergePath is a subclass of NestPath.
 * ----------------
 */
static MergePath *
_readMergePath()
{
	MergePath  *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(MergePath);

	token = lsptok(NULL, &length);		/* get :pathtype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->jpath.path.pathtype = atol(token);

	token = lsptok(NULL, &length);		/* get :startup_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->jpath.path.startup_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :total_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->jpath.path.total_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :pathkeys */
	local_node->jpath.path.pathkeys = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* get :outerjoinpath */
	local_node->jpath.outerjoinpath = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* get :innerjoinpath */
	local_node->jpath.innerjoinpath = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* get :joinrestrictinfo */
	local_node->jpath.joinrestrictinfo = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :path_mergeclauses */
	local_node->path_mergeclauses = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :outersortkeys */
	local_node->outersortkeys = nodeRead(true); /* now read it */

	token = lsptok(NULL, &length);		/* get :innersortkeys */
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
_readHashPath()
{
	HashPath   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(HashPath);

	token = lsptok(NULL, &length);		/* get :pathtype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->jpath.path.pathtype = atol(token);

	token = lsptok(NULL, &length);		/* get :startup_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->jpath.path.startup_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :total_cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->jpath.path.total_cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :pathkeys */
	local_node->jpath.path.pathkeys = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* get :outerjoinpath */
	local_node->jpath.outerjoinpath = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* get :innerjoinpath */
	local_node->jpath.innerjoinpath = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* get :joinrestrictinfo */
	local_node->jpath.joinrestrictinfo = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :path_hashclauses */
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
_readPathKeyItem()
{
	PathKeyItem *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(PathKeyItem);

	token = lsptok(NULL, &length);		/* get :sortop */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->sortop = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* get :key */
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
_readRestrictInfo()
{
	RestrictInfo *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(RestrictInfo);

	token = lsptok(NULL, &length);		/* get :clause */
	local_node->clause = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :subclauseindices */
	local_node->subclauseindices = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :mergejoinoperator */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->mergejoinoperator = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* get :left_sortop */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->left_sortop = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* get :right_sortop */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->right_sortop = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* get :hashjoinoperator */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->hashjoinoperator = (Oid) atol(token);

	return local_node;
}

/* ----------------
 *		_readJoinInfo()
 *
 *	JoinInfo is a subclass of Node.
 * ----------------
 */
static JoinInfo *
_readJoinInfo()
{
	JoinInfo   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(JoinInfo);

	token = lsptok(NULL, &length);		/* get :unjoined_relids */
	local_node->unjoined_relids = toIntList(nodeRead(true));	/* now read it */

	token = lsptok(NULL, &length);		/* get :jinfo_restrictinfo */
	local_node->jinfo_restrictinfo = nodeRead(true);	/* now read it */

	return local_node;
}

/* ----------------
 *		_readIter()
 *
 * ----------------
 */
static Iter *
_readIter()
{
	Iter	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Iter);

	token = lsptok(NULL, &length);		/* eat :iterexpr */
	local_node->iterexpr = nodeRead(true);		/* now read it */

	return local_node;
}


/* ----------------
 *		parsePlanString
 *
 * Given a character string containing a plan, parsePlanString sets up the
 * plan structure representing that plan.
 *
 * The string to be read must already have been loaded into lsptok().
 * ----------------
 */
Node *
parsePlanString(void)
{
	char	   *token;
	int			length;
	void	   *return_value = NULL;

	token = lsptok(NULL, &length);

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
	else if (length == 4 && strncmp(token, "SORT", length) == 0)
		return_value = _readSort();
	else if (length == 6 && strncmp(token, "AGGREG", length) == 0)
		return_value = _readAggref();
	else if (length == 7 && strncmp(token, "SUBLINK", length) == 0)
		return_value = _readSubLink();
	else if (length == 11 && strncmp(token, "RELABELTYPE", length) == 0)
		return_value = _readRelabelType();
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
	else if (length == 5 && strncmp(token, "ARRAY", length) == 0)
		return_value = _readArray();
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
	else if (length == 6 && strncmp(token, "ESTATE", length) == 0)
		return_value = _readEState();
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
	else if (length == 4 && strncmp(token, "CASE", length) == 0)
		return_value = _readCaseExpr();
	else if (length == 4 && strncmp(token, "WHEN", length) == 0)
		return_value = _readCaseWhen();
	else if (length == 7 && strncmp(token, "ROWMARK", length) == 0)
		return_value = _readRowMark();
	else
		elog(ERROR, "badly formatted planstring \"%.10s\"...\n", token);

	return (Node *) return_value;
}

/*------------------------------------------------------------*/

/* ----------------
 *		readDatum
 *
 * given a string representation of the value of the given type,
 * create the appropriate Datum
 * ----------------
 */
static Datum
readDatum(Oid type)
{
	int			length;
	int			tokenLength;
	char	   *token;
	bool		byValue;
	Datum		res;
	char	   *s;
	int			i;

	byValue = get_typbyval(type);

	/*
	 * read the actual length of the value
	 */
	token = lsptok(NULL, &tokenLength);
	length = atoi(token);
	token = lsptok(NULL, &tokenLength); /* skip the '[' */

	if (byValue)
	{
		if ((Size) length > sizeof(Datum))
			elog(ERROR, "readValue: byval & length = %d", length);
		res = (Datum) 0;
		s = (char *) (&res);
		for (i = 0; i < (int) sizeof(Datum); i++)
		{
			token = lsptok(NULL, &tokenLength);
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
			token = lsptok(NULL, &tokenLength);
			s[i] = (char) atoi(token);
		}
		res = PointerGetDatum(s);
	}

	token = lsptok(NULL, &tokenLength); /* skip the ']' */
	if (token[0] != ']')
		elog(ERROR, "readValue: ']' expected, length =%d", length);

	return res;
}
