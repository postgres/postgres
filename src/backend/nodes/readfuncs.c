/*-------------------------------------------------------------------------
 *
 * readfuncs.c--
 *	  Reader functions for Postgres tree nodes.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/readfuncs.c,v 1.40 1998/12/14 00:01:47 thomas Exp $
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
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/palloc.h"

#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "catalog/pg_type.h"

#include "nodes/primnodes.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/execnodes.h"
#include "nodes/relation.h"
#include "nodes/readfuncs.h"

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

		n->relname = palloc(length + 1);
		StrNCpy(n->relname, token, length + 1);
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
	{
		local_node->into = palloc(length + 1);
		StrNCpy(local_node->into, token, length + 1);
	}

	token = lsptok(NULL, &length);		/* skip :isPortal */
	token = lsptok(NULL, &length);		/* get isPortal */
	local_node->isPortal = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* skip :isBinary */
	token = lsptok(NULL, &length);		/* get isBinary */
	local_node->isBinary = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* skip :unionall */
	token = lsptok(NULL, &length);		/* get unionall */
	local_node->unionall = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* skip :uniqueFlag */
	token = lsptok(NULL, &length);		/* get uniqueFlag */
	if (length == 0)
		local_node->uniqueFlag = NULL;
	else
	{
		local_node->uniqueFlag = palloc(length + 1);
		StrNCpy(local_node->uniqueFlag, token, length + 1);
	}

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

	token = lsptok(NULL, &length);		/* skip :limitOffset */
	local_node->limitOffset = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :limitCount */
	local_node->limitCount = nodeRead(true);

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

	token = lsptok(NULL, &length);		/* skip the :resdom */
	local_node->resdom = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :opoid */
	token = lsptok(NULL, &length);		/* get opoid */
	local_node->opoid = strtoul(token, NULL, 10);

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

	token = lsptok(NULL, &length);		/* skip the :entry */
	local_node->entry = nodeRead(true);

	token = lsptok(NULL, &length);		/* skip :grpOpoid */
	token = lsptok(NULL, &length);		/* get grpOpoid */
	local_node->grpOpoid = strtoul(token, NULL, 10);

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

	token = lsptok(NULL, &length);		/* first token is :cost */
	token = lsptok(NULL, &length);		/* next is the actual cost */
	node->cost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* skip the :size */
	token = lsptok(NULL, &length);		/* get the plan_size */
	node->plan_size = atoi(token);

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
 *
 *		Does some obscene, possibly unportable, magic with
 *		sizes of things.
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

	token = lsptok(NULL, &length);		/* eat :mergejoinop */
	token = lsptok(NULL, &length);		/* get mergejoinop */
	local_node->mergejoinop = atol(token);

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

	token = lsptok(NULL, &length);		/* eat :hashjointable */
	token = lsptok(NULL, &length);		/* eat hashjointable */
	local_node->hashjointable = NULL;

	token = lsptok(NULL, &length);		/* eat :hashjointablekey */
	token = lsptok(NULL, &length);		/* eat hashjointablekey */
	local_node->hashjointablekey = 0;

	token = lsptok(NULL, &length);		/* eat :hashjointablesize */
	token = lsptok(NULL, &length);		/* eat hashjointablesize */
	local_node->hashjointablesize = 0;

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
	local_node->indxid =
		toIntList(nodeRead(true));		/* now read it */

	token = lsptok(NULL, &length);		/* eat :indxqual */
	local_node->indxqual = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* eat :indxqualorig */
	local_node->indxqualorig = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readTemp
 *
 *	Temp is a subclass of Plan
 * ----------------
 */
static Temp *
_readTemp()
{
	Temp	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Temp);

	_getPlan((Plan *) local_node);

	token = lsptok(NULL, &length);		/* eat :tempid */
	token = lsptok(NULL, &length);		/* get tempid */
	local_node->tempid = atol(token);

	token = lsptok(NULL, &length);		/* eat :keycount */
	token = lsptok(NULL, &length);		/* get keycount */
	local_node->keycount = atoi(token);

	return local_node;
}

/* ----------------
 *		_readSort
 *
 *	Sort is a subclass of Temp
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

	token = lsptok(NULL, &length);		/* eat :tempid */
	token = lsptok(NULL, &length);		/* get tempid */
	local_node->tempid = atol(token);

	token = lsptok(NULL, &length);		/* eat :keycount */
	token = lsptok(NULL, &length);		/* get keycount */
	local_node->keycount = atoi(token);

	return local_node;
}

static Agg *
_readAgg()
{
	Agg		   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Agg);
	_getPlan((Plan *) local_node);

	token = lsptok(NULL, &length);		/* eat :agg */
	local_node->aggs = nodeRead(true);	/* now read it */

	return local_node;
}

/* ----------------
 *		_readUnique
 *
 * For some reason, unique is a subclass of Temp.
 */
static Unique *
_readUnique()
{
	Unique	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Unique);

	_getPlan((Plan *) local_node);

	token = lsptok(NULL, &length);		/* eat :tempid */
	token = lsptok(NULL, &length);		/* get :tempid */
	local_node->tempid = atol(token);

	token = lsptok(NULL, &length);		/* eat :keycount */
	token = lsptok(NULL, &length);		/* get :keycount */
	local_node->keycount = atoi(token);

	return local_node;
}

/* ----------------
 *		_readHash
 *
 *	Hash is a subclass of Temp
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

	token = lsptok(NULL, &length);		/* eat :hashtable */
	token = lsptok(NULL, &length);		/* eat hashtable address */
	local_node->hashtable = NULL;

	token = lsptok(NULL, &length);		/* eat :hashtablekey */
	token = lsptok(NULL, &length);		/* get hashtablekey */
	local_node->hashtablekey = 0;

	token = lsptok(NULL, &length);		/* eat :hashtablesize */
	token = lsptok(NULL, &length);		/* get hashtablesize */
	local_node->hashtablesize = 0;

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
	local_node->restype = atol(token);

	token = lsptok(NULL, &length);		/* eat :restypmod */
	token = lsptok(NULL, &length);		/* get restypmod */
	local_node->restypmod = atoi(token);

	token = lsptok(NULL, &length);		/* eat :resname */
	token = lsptok(NULL, &length);		/* get the name */

	if (length == 0)
		local_node->resname = NULL;
	else
	{
		local_node->resname = (char *) palloc(length + 1);
		StrNCpy(local_node->resname, token + 1, length + 1 - 2);		/* strip quotes */
	}

	token = lsptok(NULL, &length);		/* eat :reskey */
	token = lsptok(NULL, &length);		/* get reskey */
	local_node->reskey = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :reskeyop */
	token = lsptok(NULL, &length);		/* get reskeyop */
	local_node->reskeyop = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :resjunk */
	token = lsptok(NULL, &length);		/* get resjunk */
	local_node->resjunk = atoi(token);

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

	local_node->args = nodeRead(true);
	token = lsptok(NULL, &length);		/* eat :default */
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
	local_node->varlevelsup = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :varnoold */
	token = lsptok(NULL, &length);		/* get varnoold */
	local_node->varnoold = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :varoattno */
	token = lsptok(NULL, &length);		/* eat :varoattno */
	local_node->varoattno = (int) atol(token);

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
	local_node->consttype = atol(token);


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
	local_node->funcid = atol(token);

	token = lsptok(NULL, &length);		/* get :functype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->functype = atol(token);

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
	local_node->opno = atol(token);

	token = lsptok(NULL, &length);		/* get :opid */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->opid = atol(token);

	token = lsptok(NULL, &length);		/* get :opresulttype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->opresulttype = atol(token);

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
	{
		local_node->paramname = (char *) palloc(length + 1);
		StrNCpy(local_node->paramname, token, length + 1);
	}

	token = lsptok(NULL, &length);		/* get :paramtype */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->paramtype = atol(token);
	token = lsptok(NULL, &length);		/* get :param_tlist */
	local_node->param_tlist = nodeRead(true);	/* now read it */

	return local_node;
}

/* ----------------
 *		_readAggreg
 *
 *	Aggreg is a subclass of Node
 * ----------------
 */
static Aggreg *
_readAggreg()
{
	Aggreg	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(Aggreg);

	token = lsptok(NULL, &length);		/* eat :aggname */
	token = lsptok(NULL, &length);		/* get aggname */
	local_node->aggname = (char *) palloc(length + 1);
	StrNCpy(local_node->aggname, token, length + 1);

	token = lsptok(NULL, &length);		/* eat :basetype */
	token = lsptok(NULL, &length);		/* get basetype */
	local_node->basetype = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :aggtype */
	token = lsptok(NULL, &length);		/* get aggtype */
	local_node->aggtype = (Oid) atol(token);

	token = lsptok(NULL, &length);		/* eat :target */
	local_node->target = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* eat :aggno */
	token = lsptok(NULL, &length);		/* get aggno */
	local_node->aggno = atoi(token);

	token = lsptok(NULL, &length);		/* eat :usenulls */
	token = lsptok(NULL, &length);		/* get usenulls */
	local_node->usenulls = (token[0] == 't') ? true : false;

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
	local_node->relids =
		toIntList(nodeRead(true));		/* now read it */

	token = lsptok(NULL, &length);		/* get :indexed */
	token = lsptok(NULL, &length);		/* now read it */

	if (!strncmp(token, "true", 4))
		local_node->indexed = true;
	else
		local_node->indexed = false;

	token = lsptok(NULL, &length);		/* get :pages */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->pages = (unsigned int) atoi(token);

	token = lsptok(NULL, &length);		/* get :tuples */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->tuples = (unsigned int) atoi(token);

	token = lsptok(NULL, &length);		/* get :size */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->size = (unsigned int) atoi(token);

	token = lsptok(NULL, &length);		/* get :width */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->width = (unsigned int) atoi(token);

	token = lsptok(NULL, &length);		/* get :targetlist */
	local_node->targetlist = nodeRead(true);	/* now read it */

	token = lsptok(NULL, &length);		/* get :pathlist */
	local_node->pathlist = nodeRead(true);		/* now read it */

	/*
	 * Not sure if these are nodes or not.	They're declared as struct
	 * Path *.	Since i don't know, i'll just print the addresses for now.
	 * This can be changed later, if necessary.
	 */

	token = lsptok(NULL, &length);		/* get :unorderpath */
	token = lsptok(NULL, &length);		/* get @ */
	token = lsptok(NULL, &length);		/* now read it */

	sscanf(token, "%x", (unsigned int *) &local_node->unorderedpath);

	token = lsptok(NULL, &length);		/* get :cheapestpath */
	token = lsptok(NULL, &length);		/* get @ */
	token = lsptok(NULL, &length);		/* now read it */

	sscanf(token, "%x", (unsigned int *) &local_node->cheapestpath);


	token = lsptok(NULL, &length);		/* get :clauseinfo */
	local_node->clauseinfo = nodeRead(true);	/* now read it */

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
	{
		local_node->relname = (char *) palloc(length + 1);
		StrNCpy(local_node->relname, token, length + 1);
	}

	token = lsptok(NULL, &length);		/* eat :refname */
	token = lsptok(NULL, &length);		/* get :refname */
	if (length == 0)
		local_node->refname = NULL;
	else
	{
		local_node->refname = (char *) palloc(length + 1);
		StrNCpy(local_node->refname, token, length + 1);
	}

	token = lsptok(NULL, &length);		/* eat :relid */
	token = lsptok(NULL, &length);		/* get :relid */
	local_node->relid = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* eat :inh */
	token = lsptok(NULL, &length);		/* get :inh */
	local_node->inh = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* eat :inFromCl */
	token = lsptok(NULL, &length);		/* get :inFromCl */
	local_node->inFromCl = (token[0] == 't') ? true : false;

	token = lsptok(NULL, &length);		/* eat :skipAcl */
	token = lsptok(NULL, &length);		/* get :skipAcl */
	local_node->skipAcl = (token[0] == 't') ? true : false;

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

	token = lsptok(NULL, &length);		/* get :cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path_cost = (Cost) atof(token);

#if 0
	token = lsptok(NULL, &length);		/* get :p_ordering */
	local_node->p_ordering =
		nodeRead(true);			/* now read it */
#endif

	token = lsptok(NULL, &length);		/* get :keys */
	local_node->keys = nodeRead(true);	/* now read it */

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

	token = lsptok(NULL, &length);		/* get :cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.path_cost = (Cost) atof(token);

#if 0
	token = lsptok(NULL, &length);		/* get :p_ordering */
	local_node->path.p_ordering = nodeRead(true);		/* now read it */
#endif

	token = lsptok(NULL, &length);		/* get :keys */
	local_node->path.keys = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :indexid */
	local_node->indexid =
		toIntList(nodeRead(true));

	token = lsptok(NULL, &length);		/* get :indexqual */
	local_node->indexqual = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readJoinPath
 *
 *	JoinPath is a subclass of Path
 * ----------------
 */
static JoinPath *
_readJoinPath()
{
	JoinPath   *local_node;
	char	   *token;
	int			length;


	local_node = makeNode(JoinPath);

	token = lsptok(NULL, &length);		/* get :pathtype */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.pathtype = atol(token);

	token = lsptok(NULL, &length);		/* get :cost */
	token = lsptok(NULL, &length);		/* now read it */
	local_node->path.path_cost = (Cost) atof(token);

#if 0
	token = lsptok(NULL, &length);		/* get :p_ordering */
	local_node->path.p_ordering = nodeRead(true);		/* now read it */
#endif

	token = lsptok(NULL, &length);		/* get :keys */
	local_node->path.keys = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :pathclauseinfo */
	local_node->pathclauseinfo = nodeRead(true);		/* now read it */

	/*
	 * Not sure if these are nodes; they're declared as "struct path *".
	 * For now, i'll just print the addresses.
	 *
	 * GJK:  Since I am parsing this stuff, I'll just ignore the addresses,
	 * and initialize these pointers to NULL.
	 */

	token = lsptok(NULL, &length);		/* get :outerjoinpath */
	token = lsptok(NULL, &length);		/* get @ */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->outerjoinpath = NULL;

	token = lsptok(NULL, &length);		/* get :innerjoinpath */
	token = lsptok(NULL, &length);		/* get @ */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->innerjoinpath = NULL;

	token = lsptok(NULL, &length);		/* get :outerjoincost */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->path.outerjoincost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :joinid */
	local_node->path.joinid =
		toIntList(nodeRead(true));		/* now read it */

	return local_node;
}

/* ----------------
 *		_readMergePath
 *
 *	MergePath is a subclass of JoinPath.
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

	token = lsptok(NULL, &length);		/* get :cost */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->jpath.path.path_cost = (Cost) atof(token);

#if 0
	token = lsptok(NULL, &length);		/* get :p_ordering */
	local_node->jpath.path.p_ordering = nodeRead(true); /* now read it */
#endif

	token = lsptok(NULL, &length);		/* get :keys */
	local_node->jpath.path.keys = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :pathclauseinfo */
	local_node->jpath.pathclauseinfo = nodeRead(true);	/* now read it */

	/*
	 * Not sure if these are nodes; they're declared as "struct path *".
	 * For now, i'll just print the addresses.
	 *
	 * GJK:  Since I am parsing this stuff, I'll just ignore the addresses,
	 * and initialize these pointers to NULL.
	 */

	token = lsptok(NULL, &length);		/* get :outerjoinpath */
	token = lsptok(NULL, &length);		/* get @ */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->jpath.outerjoinpath = NULL;

	token = lsptok(NULL, &length);		/* get :innerjoinpath */
	token = lsptok(NULL, &length);		/* get @ */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->jpath.innerjoinpath = NULL;

	token = lsptok(NULL, &length);		/* get :outerjoincost */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->jpath.path.outerjoincost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :joinid */
	local_node->jpath.path.joinid =
		toIntList(nodeRead(true));		/* now read it */

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
 *	HashPath is a subclass of JoinPath.
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

	token = lsptok(NULL, &length);		/* get :cost */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->jpath.path.path_cost = (Cost) atof(token);

#if 0
	token = lsptok(NULL, &length);		/* get :p_ordering */
	local_node->jpath.path.p_ordering = nodeRead(true); /* now read it */
#endif

	token = lsptok(NULL, &length);		/* get :keys */
	local_node->jpath.path.keys = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :pathclauseinfo */
	local_node->jpath.pathclauseinfo = nodeRead(true);	/* now read it */

	/*
	 * Not sure if these are nodes; they're declared as "struct path *".
	 * For now, i'll just print the addresses.
	 *
	 * GJK:  Since I am parsing this stuff, I'll just ignore the addresses,
	 * and initialize these pointers to NULL.
	 */

	token = lsptok(NULL, &length);		/* get :outerjoinpath */
	token = lsptok(NULL, &length);		/* get @ */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->jpath.outerjoinpath = NULL;

	token = lsptok(NULL, &length);		/* get :innerjoinpath */
	token = lsptok(NULL, &length);		/* get @ */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->jpath.innerjoinpath = NULL;

	token = lsptok(NULL, &length);		/* get :outerjoincost */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->jpath.path.outerjoincost = (Cost) atof(token);

	token = lsptok(NULL, &length);		/* get :joinid */
	local_node->jpath.path.joinid =
		toIntList(nodeRead(true));		/* now read it */

	token = lsptok(NULL, &length);		/* get :path_hashclauses */
	local_node->path_hashclauses = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :outerhashkeys */
	local_node->outerhashkeys = nodeRead(true); /* now read it */

	token = lsptok(NULL, &length);		/* get :innerhashkeys */
	local_node->innerhashkeys = nodeRead(true); /* now read it */

	return local_node;
}

/* ----------------
 *		_readOrderKey
 *
 *	OrderKey is a subclass of Node.
 * ----------------
 */
static OrderKey *
_readOrderKey()
{
	OrderKey   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(OrderKey);

	token = lsptok(NULL, &length);		/* get :attribute_number */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->attribute_number = atoi(token);

	token = lsptok(NULL, &length);		/* get :array_index */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->array_index = strtoul(token, NULL, 10);

	return local_node;
}

/* ----------------
 *		_readJoinKey
 *
 *	JoinKey is a subclass of Node.
 * ----------------
 */
static JoinKey *
_readJoinKey()
{
	JoinKey    *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(JoinKey);

	token = lsptok(NULL, &length);		/* get :outer */
	local_node->outer = nodeRead(true); /* now read it */

	token = lsptok(NULL, &length);		/* get :inner */
	local_node->inner = nodeRead(true); /* now read it */

	return local_node;
}

/* ----------------
 *		_readMergeOrder
 *
 *	MergeOrder is a subclass of Node.
 * ----------------
 */
static MergeOrder *
_readMergeOrder()
{
	MergeOrder *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(MergeOrder);
	token = lsptok(NULL, &length);		/* get :join_operator */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->join_operator = atol(token);

	token = lsptok(NULL, &length);		/* get :left_operator */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->left_operator = atol(token);

	token = lsptok(NULL, &length);		/* get :right_operator */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->right_operator = atol(token);

	token = lsptok(NULL, &length);		/* get :left_type */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->left_type = atol(token);

	token = lsptok(NULL, &length);		/* get :right_type */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->right_type = atol(token);

	return local_node;
}

/* ----------------
 *		_readClauseInfo
 *
 *	ClauseInfo is a subclass of Node.
 * ----------------
 */
static ClauseInfo *
_readClauseInfo()
{
	ClauseInfo *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(ClauseInfo);

	token = lsptok(NULL, &length);		/* get :clause */
	local_node->clause = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :selectivity */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->selectivity = atof(token);

	token = lsptok(NULL, &length);		/* get :notclause */
	token = lsptok(NULL, &length);		/* now read it */

	if (!strncmp(token, "true", 4))
		local_node->notclause = true;
	else
		local_node->notclause = false;

	token = lsptok(NULL, &length);		/* get :indexids */
	local_node->indexids = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :mergejoinorder */
	local_node->mergejoinorder = (MergeOrder *) nodeRead(true);

	token = lsptok(NULL, &length);		/* get :hashjoinoperator */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->hashjoinoperator = atol(token);

	return local_node;
}

/* ----------------
 *		_readJoinMethod
 *
 *	JoinMethod is a subclass of Node.
 * ----------------
 */
static JoinMethod *
_readJoinMethod()
{
	JoinMethod *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(JoinMethod);

	token = lsptok(NULL, &length);		/* get :jmkeys */
	local_node->jmkeys = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :clauses */
	local_node->clauses = nodeRead(true);		/* now read it */

	return local_node;
}

/* ----------------
 *		_readHInfo
 *
 * HInfo is a subclass of JoinMethod.
 * ----------------
 */
static HInfo *
_readHInfo()
{
	HInfo	   *local_node;
	char	   *token;
	int			length;

	local_node = makeNode(HInfo);

	token = lsptok(NULL, &length);		/* get :hashop */
	token = lsptok(NULL, &length);		/* now read it */

	local_node->hashop = strtoul(token, NULL, 10);

	token = lsptok(NULL, &length);		/* get :jmkeys */
	local_node->jmethod.jmkeys = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :clauses */
	local_node->jmethod.clauses = nodeRead(true);		/* now read it */

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

	token = lsptok(NULL, &length);		/* get :otherrels */
	local_node->otherrels =
		toIntList(nodeRead(true));		/* now read it */

	token = lsptok(NULL, &length);		/* get :jinfoclauseinfo */
	local_node->jinfoclauseinfo = nodeRead(true);		/* now read it */

	token = lsptok(NULL, &length);		/* get :mergejoinable */

	if (!strncmp(token, "true", 4))
		local_node->mergejoinable = true;
	else
		local_node->mergejoinable = false;

	token = lsptok(NULL, &length);		/* get :hashjoinable */

	if (!strncmp(token, "true", 4))
		local_node->hashjoinable = true;
	else
		local_node->hashjoinable = false;

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
 * The string passed to parsePlanString must be null-terminated.
 * ----------------
 */
Node *
parsePlanString(void)
{
	char	   *token;
	int			length;
	void	   *return_value = NULL;

	token = lsptok(NULL, &length);

	if (!strncmp(token, "PLAN", length))
		return_value = _readPlan();
	else if (!strncmp(token, "RESULT", length))
		return_value = _readResult();
	else if (!strncmp(token, "APPEND", length))
		return_value = _readAppend();
	else if (!strncmp(token, "JOIN", length))
		return_value = _readJoin();
	else if (!strncmp(token, "NESTLOOP", length))
		return_value = _readNestLoop();
	else if (!strncmp(token, "MERGEJOIN", length))
		return_value = _readMergeJoin();
	else if (!strncmp(token, "HASHJOIN", length))
		return_value = _readHashJoin();
	else if (!strncmp(token, "SCAN", length))
		return_value = _readScan();
	else if (!strncmp(token, "SEQSCAN", length))
		return_value = _readSeqScan();
	else if (!strncmp(token, "INDEXSCAN", length))
		return_value = _readIndexScan();
	else if (!strncmp(token, "TEMP", length))
		return_value = _readTemp();
	else if (!strncmp(token, "SORT", length))
		return_value = _readSort();
	else if (!strncmp(token, "AGGREG", length))
		return_value = _readAggreg();
	else if (!strncmp(token, "SUBLINK", length))
		return_value = _readSubLink();
	else if (!strncmp(token, "AGG", length))
		return_value = _readAgg();
	else if (!strncmp(token, "UNIQUE", length))
		return_value = _readUnique();
	else if (!strncmp(token, "HASH", length))
		return_value = _readHash();
	else if (!strncmp(token, "RESDOM", length))
		return_value = _readResdom();
	else if (!strncmp(token, "EXPR", length))
		return_value = _readExpr();
	else if (!strncmp(token, "ARRAYREF", length))
		return_value = _readArrayRef();
	else if (!strncmp(token, "ARRAY", length))
		return_value = _readArray();
	else if (!strncmp(token, "VAR", length))
		return_value = _readVar();
	else if (!strncmp(token, "CONST", length))
		return_value = _readConst();
	else if (!strncmp(token, "FUNC", length))
		return_value = _readFunc();
	else if (!strncmp(token, "OPER", length))
		return_value = _readOper();
	else if (!strncmp(token, "PARAM", length))
		return_value = _readParam();
	else if (!strncmp(token, "ESTATE", length))
		return_value = _readEState();
	else if (!strncmp(token, "RELOPTINFO", length))
		return_value = _readRelOptInfo();
	else if (!strncmp(token, "TARGETENTRY", length))
		return_value = _readTargetEntry();
	else if (!strncmp(token, "RTE", length))
		return_value = _readRangeTblEntry();
	else if (!strncmp(token, "PATH", length))
		return_value = _readPath();
	else if (!strncmp(token, "INDEXPATH", length))
		return_value = _readIndexPath();
	else if (!strncmp(token, "JOINPATH", length))
		return_value = _readJoinPath();
	else if (!strncmp(token, "MERGEPATH", length))
		return_value = _readMergePath();
	else if (!strncmp(token, "HASHPATH", length))
		return_value = _readHashPath();
	else if (!strncmp(token, "ORDERKEY", length))
		return_value = _readOrderKey();
	else if (!strncmp(token, "JOINKEY", length))
		return_value = _readJoinKey();
	else if (!strncmp(token, "MERGEORDER", length))
		return_value = _readMergeOrder();
	else if (!strncmp(token, "CLAUSEINFO", length))
		return_value = _readClauseInfo();
	else if (!strncmp(token, "JOINMETHOD", length))
		return_value = _readJoinMethod();
	else if (!strncmp(token, "JOININFO", length))
		return_value = _readJoinInfo();
	else if (!strncmp(token, "HINFO", length))
		return_value = _readHInfo();
	else if (!strncmp(token, "ITER", length))
		return_value = _readIter();
	else if (!strncmp(token, "QUERY", length))
		return_value = _readQuery();
	else if (!strncmp(token, "SORTCLAUSE", length))
		return_value = _readSortClause();
	else if (!strncmp(token, "GROUPCLAUSE", length))
		return_value = _readGroupClause();
	else if (!strncmp(token, "CASE", length))
		return_value = _readCaseExpr();
	else if (!strncmp(token, "WHEN", length))
		return_value = _readCaseWhen();
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
		if (length > sizeof(Datum))
			elog(ERROR, "readValue: byval & length = %d", length);
		s = (char *) (&res);
		for (i = 0; i < sizeof(Datum); i++)
		{
			token = lsptok(NULL, &tokenLength);
			s[i] = (char) atoi(token);
		}
	}
	else if (length <= 0)
		s = NULL;
	else if (length >= 1)
	{
		s = (char *) palloc(length);
		Assert(s != NULL);
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
