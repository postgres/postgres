/*-------------------------------------------------------------------------
 *
 * copyfuncs.c
 *	  Copy functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/copyfuncs.c,v 1.114 2000/06/18 22:44:05 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/planmain.h"


/*
 * Node_Copy
 *	  a macro to simplify calling of copyObject on the specified field
 */
#define Node_Copy(from, newnode, field) \
	((newnode)->field = copyObject((from)->field))


/*
 * listCopy
 *	  This copy function only copies the "cons-cells" of the list, not the
 *	  pointed-to objects.  (Use copyObject if you want a "deep" copy.)
 *
 *	  We also use this function for copying lists of integers, which is
 *	  grotty but unlikely to break --- it could fail if sizeof(pointer)
 *	  is less than sizeof(int), but I don't know any such machines...
 *
 *	  Note that copyObject will surely coredump if applied to a list
 *	  of integers!
 */
List *
listCopy(List *list)
{
	List	   *newlist,
			   *l,
			   *nl;

	/* rather ugly coding for speed... */
	if (list == NIL)
		return NIL;

	newlist = nl = lcons(lfirst(list), NIL);

	foreach(l, lnext(list))
	{
		lnext(nl) = lcons(lfirst(l), NIL);
		nl = lnext(nl);
	}
	return newlist;
}

/* ****************************************************************
 *					 plannodes.h copy functions
 * ****************************************************************
 */

/* ----------------
 *		CopyPlanFields
 *
 *		This function copies the fields of the Plan node.  It is used by
 *		all the copy functions for classes which inherit from Plan.
 * ----------------
 */
static void
CopyPlanFields(Plan *from, Plan *newnode)
{
	newnode->startup_cost = from->startup_cost;
	newnode->total_cost = from->total_cost;
	newnode->plan_rows = from->plan_rows;
	newnode->plan_width = from->plan_width;
	/* state is NOT copied */
	newnode->targetlist = copyObject(from->targetlist);
	newnode->qual = copyObject(from->qual);
	newnode->lefttree = copyObject(from->lefttree);
	newnode->righttree = copyObject(from->righttree);
	newnode->extParam = listCopy(from->extParam);
	newnode->locParam = listCopy(from->locParam);
	newnode->chgParam = listCopy(from->chgParam);
	Node_Copy(from, newnode, initPlan);
	/* subPlan list must point to subplans in the new subtree, not the old */
	if (from->subPlan != NIL)
		newnode->subPlan = nconc(pull_subplans((Node *) newnode->targetlist),
								 pull_subplans((Node *) newnode->qual));
	else
		newnode->subPlan = NIL;
	newnode->nParamExec = from->nParamExec;
}

/* ----------------
 *		_copyPlan
 * ----------------
 */
static Plan *
_copyPlan(Plan *from)
{
	Plan	   *newnode = makeNode(Plan);

	/* ----------------
	 *	copy the node superclass fields
	 * ----------------
	 */
	CopyPlanFields(from, newnode);

	return newnode;
}


/* ----------------
 *		_copyResult
 * ----------------
 */
static Result *
_copyResult(Result *from)
{
	Result	   *newnode = makeNode(Result);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, resconstantqual);

	/*
	 * We must add subplans in resconstantqual to the new plan's subPlan
	 * list
	 */
	if (from->plan.subPlan != NIL)
		newnode->plan.subPlan = nconc(newnode->plan.subPlan,
								pull_subplans(newnode->resconstantqual));

	return newnode;
}

/* ----------------
 *		_copyAppend
 * ----------------
 */
static Append *
_copyAppend(Append *from)
{
	Append	   *newnode = makeNode(Append);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, appendplans);
	Node_Copy(from, newnode, unionrtables);
	newnode->inheritrelid = from->inheritrelid;
	Node_Copy(from, newnode, inheritrtable);

	return newnode;
}


/* ----------------
 *		CopyScanFields
 *
 *		This function copies the fields of the Scan node.  It is used by
 *		all the copy functions for classes which inherit from Scan.
 * ----------------
 */
static void
CopyScanFields(Scan *from, Scan *newnode)
{
	newnode->scanrelid = from->scanrelid;
	return;
}

/* ----------------
 *		_copyScan
 * ----------------
 */
static Scan *
_copyScan(Scan *from)
{
	Scan	   *newnode = makeNode(Scan);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);
	CopyScanFields((Scan *) from, (Scan *) newnode);

	return newnode;
}

/* ----------------
 *		_copySeqScan
 * ----------------
 */
static SeqScan *
_copySeqScan(SeqScan *from)
{
	SeqScan    *newnode = makeNode(SeqScan);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);
	CopyScanFields((Scan *) from, (Scan *) newnode);

	return newnode;
}

/* ----------------
 *		_copyIndexScan
 * ----------------
 */
static IndexScan *
_copyIndexScan(IndexScan *from)
{
	IndexScan  *newnode = makeNode(IndexScan);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);
	CopyScanFields((Scan *) from, (Scan *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->indxid = listCopy(from->indxid);
	Node_Copy(from, newnode, indxqual);
	Node_Copy(from, newnode, indxqualorig);
	newnode->indxorderdir = from->indxorderdir;

	/*
	 * We must add subplans in index quals to the new plan's subPlan list
	 */
	if (from->scan.plan.subPlan != NIL)
	{
		newnode->scan.plan.subPlan = nconc(newnode->scan.plan.subPlan,
							  pull_subplans((Node *) newnode->indxqual));
		newnode->scan.plan.subPlan = nconc(newnode->scan.plan.subPlan,
						  pull_subplans((Node *) newnode->indxqualorig));
	}

	return newnode;
}

/* ----------------
 *				_copyTidScan
 * ----------------
 */
static TidScan *
_copyTidScan(TidScan *from)
{
	TidScan    *newnode = makeNode(TidScan);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);
	CopyScanFields((Scan *) from, (Scan *) newnode);
	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->needRescan = from->needRescan;
	Node_Copy(from, newnode, tideval);

	return newnode;
}


/* ----------------
 *		CopyJoinFields
 *
 *		This function copies the fields of the Join node.  It is used by
 *		all the copy functions for classes which inherit from Join.
 * ----------------
 */
static void
CopyJoinFields(Join *from, Join *newnode)
{
	/* nothing extra */
	return;
}


/* ----------------
 *		_copyJoin
 * ----------------
 */
static Join *
_copyJoin(Join *from)
{
	Join	   *newnode = makeNode(Join);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);
	CopyJoinFields(from, newnode);

	return newnode;
}


/* ----------------
 *		_copyNestLoop
 * ----------------
 */
static NestLoop *
_copyNestLoop(NestLoop *from)
{
	NestLoop   *newnode = makeNode(NestLoop);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);
	CopyJoinFields((Join *) from, (Join *) newnode);

	return newnode;
}


/* ----------------
 *		_copyMergeJoin
 * ----------------
 */
static MergeJoin *
_copyMergeJoin(MergeJoin *from)
{
	MergeJoin  *newnode = makeNode(MergeJoin);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);
	CopyJoinFields((Join *) from, (Join *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, mergeclauses);

	/*
	 * We must add subplans in mergeclauses to the new plan's subPlan list
	 */
	if (from->join.subPlan != NIL)
		newnode->join.subPlan = nconc(newnode->join.subPlan,
						  pull_subplans((Node *) newnode->mergeclauses));

	return newnode;
}

/* ----------------
 *		_copyHashJoin
 * ----------------
 */
static HashJoin *
_copyHashJoin(HashJoin *from)
{
	HashJoin   *newnode = makeNode(HashJoin);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);
	CopyJoinFields((Join *) from, (Join *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, hashclauses);
	newnode->hashjoinop = from->hashjoinop;

	/*
	 * We must add subplans in hashclauses to the new plan's subPlan list
	 */
	if (from->join.subPlan != NIL)
		newnode->join.subPlan = nconc(newnode->join.subPlan,
						   pull_subplans((Node *) newnode->hashclauses));

	return newnode;
}


/* ----------------
 *		_copyMaterial
 * ----------------
 */
static Material *
_copyMaterial(Material *from)
{
	Material   *newnode = makeNode(Material);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);

	return newnode;
}


/* ----------------
 *		_copySort
 * ----------------
 */
static Sort *
_copySort(Sort *from)
{
	Sort	   *newnode = makeNode(Sort);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);

	newnode->keycount = from->keycount;

	return newnode;
}


/* ----------------
 *		_copyGroup
 * ----------------
 */
static Group *
_copyGroup(Group *from)
{
	Group	   *newnode = makeNode(Group);

	CopyPlanFields((Plan *) from, (Plan *) newnode);

	newnode->tuplePerGroup = from->tuplePerGroup;
	newnode->numCols = from->numCols;
	newnode->grpColIdx = palloc(from->numCols * sizeof(AttrNumber));
	memcpy(newnode->grpColIdx, from->grpColIdx, from->numCols * sizeof(AttrNumber));

	return newnode;
}

/* ---------------
 *	_copyAgg
 * --------------
 */
static Agg *
_copyAgg(Agg *from)
{
	Agg		   *newnode = makeNode(Agg);

	CopyPlanFields((Plan *) from, (Plan *) newnode);

	return newnode;
}

/* ---------------
 *	_copyGroupClause
 * --------------
 */
static GroupClause *
_copyGroupClause(GroupClause *from)
{
	GroupClause *newnode = makeNode(GroupClause);

	newnode->tleSortGroupRef = from->tleSortGroupRef;
	newnode->sortop = from->sortop;

	return newnode;
}


/* ----------------
 *		_copyUnique
 * ----------------
 */
static Unique *
_copyUnique(Unique *from)
{
	Unique	   *newnode = makeNode(Unique);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->numCols = from->numCols;
	newnode->uniqColIdx = palloc(from->numCols * sizeof(AttrNumber));
	memcpy(newnode->uniqColIdx, from->uniqColIdx, from->numCols * sizeof(AttrNumber));

	return newnode;
}


/* ----------------
 *		_copyHash
 * ----------------
 */
static Hash *
_copyHash(Hash *from)
{
	Hash	   *newnode = makeNode(Hash);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, hashkey);

	return newnode;
}

static SubPlan *
_copySubPlan(SubPlan *from)
{
	SubPlan    *newnode = makeNode(SubPlan);

	Node_Copy(from, newnode, plan);
	newnode->plan_id = from->plan_id;
	Node_Copy(from, newnode, rtable);
	newnode->setParam = listCopy(from->setParam);
	newnode->parParam = listCopy(from->parParam);
	Node_Copy(from, newnode, sublink);

	/* do not copy execution state */
	newnode->shutdown = false;
	newnode->curTuple = NULL;

	return newnode;
}

/* ****************************************************************
 *					   primnodes.h copy functions
 * ****************************************************************
 */

/* ----------------
 *		_copyResdom
 * ----------------
 */
static Resdom *
_copyResdom(Resdom *from)
{
	Resdom	   *newnode = makeNode(Resdom);

	newnode->resno = from->resno;
	newnode->restype = from->restype;
	newnode->restypmod = from->restypmod;
	if (from->resname != NULL)
		newnode->resname = pstrdup(from->resname);
	newnode->ressortgroupref = from->ressortgroupref;
	newnode->reskey = from->reskey;
	newnode->reskeyop = from->reskeyop;
	newnode->resjunk = from->resjunk;

	return newnode;
}

static Fjoin *
_copyFjoin(Fjoin *from)
{
	Fjoin	   *newnode = makeNode(Fjoin);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */

	newnode->fj_initialized = from->fj_initialized;
	newnode->fj_nNodes = from->fj_nNodes;

	Node_Copy(from, newnode, fj_innerNode);

	newnode->fj_results = (DatumPtr)
		palloc((from->fj_nNodes) * sizeof(Datum));
	memmove(from->fj_results,
			newnode->fj_results,
			(from->fj_nNodes) * sizeof(Datum));

	newnode->fj_alwaysDone = (BoolPtr)
		palloc((from->fj_nNodes) * sizeof(bool));
	memmove(from->fj_alwaysDone,
			newnode->fj_alwaysDone,
			(from->fj_nNodes) * sizeof(bool));


	return newnode;
}

/* ----------------
 *		_copyExpr
 * ----------------
 */
static Expr *
_copyExpr(Expr *from)
{
	Expr	   *newnode = makeNode(Expr);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	newnode->typeOid = from->typeOid;
	newnode->opType = from->opType;

	Node_Copy(from, newnode, oper);
	Node_Copy(from, newnode, args);

	return newnode;
}

/* ----------------
 *		_copyVar
 * ----------------
 */
static Var *
_copyVar(Var *from)
{
	Var		   *newnode = makeNode(Var);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->varno = from->varno;
	newnode->varattno = from->varattno;
	newnode->vartype = from->vartype;
	newnode->vartypmod = from->vartypmod;
	newnode->varlevelsup = from->varlevelsup;

	newnode->varnoold = from->varnoold;
	newnode->varoattno = from->varoattno;

	return newnode;
}

static Attr *
_copyAttr(Attr *from)
{
	Attr	   *newnode = makeNode(Attr);

	if (from->relname)
		newnode->relname = pstrdup(from->relname);
	Node_Copy(from, newnode, attrs);

	return newnode;
}

/* ----------------
 *		_copyOper
 * ----------------
 */
static Oper *
_copyOper(Oper *from)
{
	Oper	   *newnode = makeNode(Oper);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->opno = from->opno;
	newnode->opid = from->opid;
	newnode->opresulttype = from->opresulttype;
	newnode->opsize = from->opsize;

	/*
	 * NOTE: shall we copy the cache structure or just the pointer ?
	 * Alternatively we can set 'op_fcache' to NULL, in which case the
	 * executor will initialize it when it needs it...
	 */
	newnode->op_fcache = from->op_fcache;

	return newnode;
}

/* ----------------
 *		_copyConst
 * ----------------
 */
static Const *
_copyConst(Const *from)
{
	Const	   *newnode = makeNode(Const);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->consttype = from->consttype;
	newnode->constlen = from->constlen;

	if (from->constbyval || from->constisnull)
	{
		/* ----------------
		 *	passed by value so just copy the datum.
		 *	Also, don't try to copy struct when value is null!
		 * ----------------
		 */
		newnode->constvalue = from->constvalue;
	}
	else
	{
		/* ----------------
		 *	not passed by value. datum contains a pointer.
		 * ----------------
		 */
		int			length = from->constlen;

		if (length == -1)		/* variable-length type? */
			length = VARSIZE(from->constvalue);
		newnode->constvalue = PointerGetDatum(palloc(length));
		memcpy(DatumGetPointer(newnode->constvalue),
			   DatumGetPointer(from->constvalue),
			   length);
	}

	newnode->constisnull = from->constisnull;
	newnode->constbyval = from->constbyval;
	newnode->constisset = from->constisset;
	newnode->constiscast = from->constiscast;

	return newnode;
}

/* ----------------
 *		_copyParam
 * ----------------
 */
static Param *
_copyParam(Param *from)
{
	Param	   *newnode = makeNode(Param);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->paramkind = from->paramkind;
	newnode->paramid = from->paramid;

	if (from->paramname != NULL)
		newnode->paramname = pstrdup(from->paramname);
	newnode->paramtype = from->paramtype;
	Node_Copy(from, newnode, param_tlist);

	return newnode;
}

/* ----------------
 *		_copyFunc
 * ----------------
 */
static Func *
_copyFunc(Func *from)
{
	Func	   *newnode = makeNode(Func);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->funcid = from->funcid;
	newnode->functype = from->functype;
	newnode->funcisindex = from->funcisindex;
	newnode->funcsize = from->funcsize;
	newnode->func_fcache = from->func_fcache;
	Node_Copy(from, newnode, func_tlist);
	Node_Copy(from, newnode, func_planlist);

	return newnode;
}

/* ----------------
 *		_copyAggref
 * ----------------
 */
static Aggref *
_copyAggref(Aggref *from)
{
	Aggref	   *newnode = makeNode(Aggref);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->aggname = pstrdup(from->aggname);
	newnode->basetype = from->basetype;
	newnode->aggtype = from->aggtype;
	Node_Copy(from, newnode, target);
	newnode->usenulls = from->usenulls;
	newnode->aggstar = from->aggstar;
	newnode->aggdistinct = from->aggdistinct;
	newnode->aggno = from->aggno;		/* probably not needed */

	return newnode;
}

/* ----------------
 *		_copySubLink
 * ----------------
 */
static SubLink *
_copySubLink(SubLink *from)
{
	SubLink    *newnode = makeNode(SubLink);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->subLinkType = from->subLinkType;
	newnode->useor = from->useor;
	Node_Copy(from, newnode, lefthand);
	Node_Copy(from, newnode, oper);
	Node_Copy(from, newnode, subselect);

	return newnode;
}

/* ----------------
 *		_copyRelabelType
 * ----------------
 */
static RelabelType *
_copyRelabelType(RelabelType *from)
{
	RelabelType *newnode = makeNode(RelabelType);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, arg);
	newnode->resulttype = from->resulttype;
	newnode->resulttypmod = from->resulttypmod;

	return newnode;
}

/* ----------------
 *		_copyCaseExpr
 * ----------------
 */
static CaseExpr *
_copyCaseExpr(CaseExpr *from)
{
	CaseExpr   *newnode = makeNode(CaseExpr);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->casetype = from->casetype;

	Node_Copy(from, newnode, arg);
	Node_Copy(from, newnode, args);
	Node_Copy(from, newnode, defresult);

	return newnode;
}

/* ----------------
 *		_copyCaseWhen
 * ----------------
 */
static CaseWhen *
_copyCaseWhen(CaseWhen *from)
{
	CaseWhen   *newnode = makeNode(CaseWhen);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, expr);
	Node_Copy(from, newnode, result);

	return newnode;
}

static Array *
_copyArray(Array *from)
{
	Array	   *newnode = makeNode(Array);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->arrayelemtype = from->arrayelemtype;
	newnode->arrayelemlength = from->arrayelemlength;
	newnode->arrayelembyval = from->arrayelembyval;
	newnode->arrayndim = from->arrayndim;
	newnode->arraylow = from->arraylow;
	newnode->arrayhigh = from->arrayhigh;
	newnode->arraylen = from->arraylen;

	return newnode;
}

static ArrayRef *
_copyArrayRef(ArrayRef *from)
{
	ArrayRef   *newnode = makeNode(ArrayRef);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->refattrlength = from->refattrlength;
	newnode->refelemlength = from->refelemlength;
	newnode->refelemtype = from->refelemtype;
	newnode->refelembyval = from->refelembyval;

	Node_Copy(from, newnode, refupperindexpr);
	Node_Copy(from, newnode, reflowerindexpr);
	Node_Copy(from, newnode, refexpr);
	Node_Copy(from, newnode, refassgnexpr);

	return newnode;
}

/* ****************************************************************
 *						relation.h copy functions
 * ****************************************************************
 */

/* ----------------
 *		_copyRelOptInfo
 * ----------------
 */
/*
 *	when you change this, also make sure to fix up xfunc_copyRelOptInfo in
 *	planner/path/xfunc.c accordingly!!!
 *		-- JMH, 8/2/93
 */
static RelOptInfo *
_copyRelOptInfo(RelOptInfo *from)
{
	RelOptInfo *newnode = makeNode(RelOptInfo);

	newnode->relids = listCopy(from->relids);

	newnode->rows = from->rows;
	newnode->width = from->width;

	Node_Copy(from, newnode, targetlist);
	Node_Copy(from, newnode, pathlist);
	/* XXX cheapest-path fields should point to members of pathlist? */
	Node_Copy(from, newnode, cheapest_startup_path);
	Node_Copy(from, newnode, cheapest_total_path);
	newnode->pruneable = from->pruneable;

	newnode->indexed = from->indexed;
	newnode->pages = from->pages;
	newnode->tuples = from->tuples;

	Node_Copy(from, newnode, baserestrictinfo);
	newnode->baserestrictcost = from->baserestrictcost;
	Node_Copy(from, newnode, joininfo);
	Node_Copy(from, newnode, innerjoin);

	return newnode;
}

/* ----------------
 *		_copyIndexOptInfo
 * ----------------
 */
static IndexOptInfo *
_copyIndexOptInfo(IndexOptInfo *from)
{
	IndexOptInfo *newnode = makeNode(IndexOptInfo);
	int			i,
				len;

	newnode->indexoid = from->indexoid;
	newnode->pages = from->pages;
	newnode->tuples = from->tuples;

	if (from->classlist)
	{
		for (len = 0; from->classlist[len] != 0; len++)
			;
		newnode->classlist = (Oid *) palloc(sizeof(Oid) * (len + 1));
		for (i = 0; i < len; i++)
			newnode->classlist[i] = from->classlist[i];
		newnode->classlist[len] = 0;
	}

	if (from->indexkeys)
	{
		for (len = 0; from->indexkeys[len] != 0; len++)
			;
		newnode->indexkeys = (int *) palloc(sizeof(int) * (len + 1));
		for (i = 0; i < len; i++)
			newnode->indexkeys[i] = from->indexkeys[i];
		newnode->indexkeys[len] = 0;
	}

	if (from->ordering)
	{
		for (len = 0; from->ordering[len] != 0; len++)
			;
		newnode->ordering = (Oid *) palloc(sizeof(Oid) * (len + 1));
		for (i = 0; i < len; i++)
			newnode->ordering[i] = from->ordering[i];
		newnode->ordering[len] = 0;
	}

	newnode->relam = from->relam;
	newnode->amcostestimate = from->amcostestimate;
	newnode->indproc = from->indproc;
	Node_Copy(from, newnode, indpred);
	newnode->lossy = from->lossy;

	return newnode;
}

/* ----------------
 *		CopyPathFields
 *
 *		This function copies the fields of the Path node.  It is used by
 *		all the copy functions for classes which inherit from Path.
 * ----------------
 */
static void
CopyPathFields(Path *from, Path *newnode)
{

	/*
	 * Modify the next line, since it causes the copying to cycle (i.e.
	 * the parent points right back here! -- JMH, 7/7/92. Old version:
	 * Node_Copy(from, newnode, parent);
	 */
	newnode->parent = from->parent;

	newnode->startup_cost = from->startup_cost;
	newnode->total_cost = from->total_cost;

	newnode->pathtype = from->pathtype;

	Node_Copy(from, newnode, pathkeys);
}

/* ----------------
 *		_copyPath
 * ----------------
 */
static Path *
_copyPath(Path *from)
{
	Path	   *newnode = makeNode(Path);

	CopyPathFields(from, newnode);

	return newnode;
}

/* ----------------
 *		_copyIndexPath
 * ----------------
 */
static IndexPath *
_copyIndexPath(IndexPath *from)
{
	IndexPath  *newnode = makeNode(IndexPath);

	/* ----------------
	 *	copy the node superclass fields
	 * ----------------
	 */
	CopyPathFields((Path *) from, (Path *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->indexid = listCopy(from->indexid);
	Node_Copy(from, newnode, indexqual);
	newnode->indexscandir = from->indexscandir;
	newnode->joinrelids = listCopy(from->joinrelids);
	newnode->rows = from->rows;

	return newnode;
}

/* ----------------
 *				_copyTidPath
 * ----------------
 */
static TidPath *
_copyTidPath(TidPath *from)
{
	TidPath    *newnode = makeNode(TidPath);

	/* ----------------
	 *	copy the node superclass fields
	 * ----------------
	 */
	CopyPathFields((Path *) from, (Path *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, tideval);
	newnode->unjoined_relids = listCopy(from->unjoined_relids);

	return newnode;
}

/* ----------------
 *		CopyJoinPathFields
 *
 *		This function copies the fields of the JoinPath node.  It is used by
 *		all the copy functions for classes which inherit from JoinPath.
 * ----------------
 */
static void
CopyJoinPathFields(JoinPath *from, JoinPath *newnode)
{
	Node_Copy(from, newnode, outerjoinpath);
	Node_Copy(from, newnode, innerjoinpath);
	Node_Copy(from, newnode, joinrestrictinfo);
}

/* ----------------
 *		_copyNestPath
 * ----------------
 */
static NestPath *
_copyNestPath(NestPath *from)
{
	NestPath   *newnode = makeNode(NestPath);

	/* ----------------
	 *	copy the node superclass fields
	 * ----------------
	 */
	CopyPathFields((Path *) from, (Path *) newnode);
	CopyJoinPathFields((JoinPath *) from, (JoinPath *) newnode);

	return newnode;
}

/* ----------------
 *		_copyMergePath
 * ----------------
 */
static MergePath *
_copyMergePath(MergePath *from)
{
	MergePath  *newnode = makeNode(MergePath);

	/* ----------------
	 *	copy the node superclass fields
	 * ----------------
	 */
	CopyPathFields((Path *) from, (Path *) newnode);
	CopyJoinPathFields((JoinPath *) from, (JoinPath *) newnode);

	/* ----------------
	 *	copy the remainder of the node
	 * ----------------
	 */
	Node_Copy(from, newnode, path_mergeclauses);
	Node_Copy(from, newnode, outersortkeys);
	Node_Copy(from, newnode, innersortkeys);

	return newnode;
}

/* ----------------
 *		_copyHashPath
 * ----------------
 */
static HashPath *
_copyHashPath(HashPath *from)
{
	HashPath   *newnode = makeNode(HashPath);

	/* ----------------
	 *	copy the node superclass fields
	 * ----------------
	 */
	CopyPathFields((Path *) from, (Path *) newnode);
	CopyJoinPathFields((JoinPath *) from, (JoinPath *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, path_hashclauses);

	return newnode;
}

/* ----------------
 *		_copyPathKeyItem
 * ----------------
 */
static PathKeyItem *
_copyPathKeyItem(PathKeyItem *from)
{
	PathKeyItem *newnode = makeNode(PathKeyItem);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, key);
	newnode->sortop = from->sortop;

	return newnode;
}

/* ----------------
 *		_copyRestrictInfo
 * ----------------
 */
static RestrictInfo *
_copyRestrictInfo(RestrictInfo *from)
{
	RestrictInfo *newnode = makeNode(RestrictInfo);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, clause);
	Node_Copy(from, newnode, subclauseindices);
	newnode->mergejoinoperator = from->mergejoinoperator;
	newnode->left_sortop = from->left_sortop;
	newnode->right_sortop = from->right_sortop;
	newnode->hashjoinoperator = from->hashjoinoperator;

	return newnode;
}

/* ----------------
 *		_copyJoinInfo
 * ----------------
 */
static JoinInfo *
_copyJoinInfo(JoinInfo *from)
{
	JoinInfo   *newnode = makeNode(JoinInfo);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->unjoined_relids = listCopy(from->unjoined_relids);
	Node_Copy(from, newnode, jinfo_restrictinfo);

	return newnode;
}

static Iter *
_copyIter(Iter *from)
{
	Iter	   *newnode = makeNode(Iter);

	Node_Copy(from, newnode, iterexpr);
	newnode->itertype = from->itertype;

	return newnode;
}

static Stream *
_copyStream(Stream *from)
{
	Stream	   *newnode = makeNode(Stream);

	newnode->pathptr = from->pathptr;
	newnode->cinfo = from->cinfo;
	newnode->clausetype = from->clausetype;

	newnode->upstream = (StreamPtr) NULL;		/* only copy nodes
												 * downwards! */
	Node_Copy(from, newnode, downstream);
	if (newnode->downstream)
		((Stream *) newnode->downstream)->upstream = (Stream *) newnode;

	newnode->groupup = from->groupup;
	newnode->groupcost = from->groupcost;
	newnode->groupsel = from->groupsel;

	return newnode;
}

/* ****************************************************************
 *					parsenodes.h copy functions
 * ****************************************************************
 */

static TargetEntry *
_copyTargetEntry(TargetEntry *from)
{
	TargetEntry *newnode = makeNode(TargetEntry);

	Node_Copy(from, newnode, resdom);
	Node_Copy(from, newnode, fjoin);
	Node_Copy(from, newnode, expr);
	return newnode;
}

static RangeTblEntry *
_copyRangeTblEntry(RangeTblEntry *from)
{
	RangeTblEntry *newnode = makeNode(RangeTblEntry);

	if (from->relname)
		newnode->relname = pstrdup(from->relname);
	Node_Copy(from, newnode, ref);
	Node_Copy(from, newnode, eref);
	newnode->relid = from->relid;
	newnode->inh = from->inh;
	newnode->inFromCl = from->inFromCl;
	newnode->inJoinSet = from->inJoinSet;
	newnode->skipAcl = from->skipAcl;

	return newnode;
}

static RowMark *
_copyRowMark(RowMark *from)
{
	RowMark    *newnode = makeNode(RowMark);

	newnode->rti = from->rti;
	newnode->info = from->info;

	return newnode;
}

static SortClause *
_copySortClause(SortClause *from)
{
	SortClause *newnode = makeNode(SortClause);

	newnode->tleSortGroupRef = from->tleSortGroupRef;
	newnode->sortop = from->sortop;

	return newnode;
}

static A_Const *
_copyAConst(A_Const *from)
{
	A_Const    *newnode = makeNode(A_Const);

	newnode->val = *((Value *) (copyObject(&(from->val))));
	Node_Copy(from, newnode, typename);

	return newnode;
}

static TypeName *
_copyTypeName(TypeName *from)
{
	TypeName   *newnode = makeNode(TypeName);

	if (from->name)
		newnode->name = pstrdup(from->name);
	newnode->timezone = from->timezone;
	newnode->setof = from->setof;
	newnode->typmod = from->typmod;
	Node_Copy(from, newnode, arrayBounds);

	return newnode;
}

static TypeCast *
_copyTypeCast(TypeCast *from)
{
	TypeCast   *newnode = makeNode(TypeCast);

	Node_Copy(from, newnode, arg);
	Node_Copy(from, newnode, typename);

	return newnode;
}

static Query *
_copyQuery(Query *from)
{
	Query	   *newnode = makeNode(Query);

	newnode->commandType = from->commandType;
	Node_Copy(from, newnode, utilityStmt);
	newnode->resultRelation = from->resultRelation;
	if (from->into)
		newnode->into = pstrdup(from->into);
	newnode->isPortal = from->isPortal;
	newnode->isBinary = from->isBinary;
	newnode->isTemp = from->isTemp;
	newnode->unionall = from->unionall;
	newnode->hasAggs = from->hasAggs;
	newnode->hasSubLinks = from->hasSubLinks;

	Node_Copy(from, newnode, rtable);
	Node_Copy(from, newnode, targetList);
	Node_Copy(from, newnode, qual);
	Node_Copy(from, newnode, rowMark);

	Node_Copy(from, newnode, distinctClause);
	Node_Copy(from, newnode, sortClause);
	Node_Copy(from, newnode, groupClause);
	Node_Copy(from, newnode, havingQual);

	/* why is intersectClause missing? */
	Node_Copy(from, newnode, unionClause);

	Node_Copy(from, newnode, limitOffset);
	Node_Copy(from, newnode, limitCount);

	/*
	 * We do not copy the planner internal fields: base_rel_list,
	 * join_rel_list, equi_key_list, query_pathkeys. Not entirely clear if
	 * this is right?
	 */

	return newnode;
}

static ClosePortalStmt *
_copyClosePortalStmt(ClosePortalStmt *from)
{
	ClosePortalStmt *newnode = makeNode(ClosePortalStmt);

	if (from->portalname)
		newnode->portalname = pstrdup(from->portalname);

	return newnode;
}

static TruncateStmt *
_copyTruncateStmt(TruncateStmt *from)
{
	TruncateStmt *newnode = makeNode(TruncateStmt);

	newnode->relName = pstrdup(from->relName);

	return newnode;
}

static NotifyStmt *
_copyNotifyStmt(NotifyStmt *from)
{
	NotifyStmt *newnode = makeNode(NotifyStmt);

	if (from->relname)
		newnode->relname = pstrdup(from->relname);

	return newnode;
}

static ListenStmt *
_copyListenStmt(ListenStmt *from)
{
	ListenStmt *newnode = makeNode(ListenStmt);

	if (from->relname)
		newnode->relname = pstrdup(from->relname);

	return newnode;
}

static UnlistenStmt *
_copyUnlistenStmt(UnlistenStmt *from)
{
	UnlistenStmt *newnode = makeNode(UnlistenStmt);

	if (from->relname)
		newnode->relname = pstrdup(from->relname);

	return newnode;
}

static TransactionStmt *
_copyTransactionStmt(TransactionStmt *from)
{
	TransactionStmt *newnode = makeNode(TransactionStmt);

	newnode->command = from->command;

	return newnode;
}

static LoadStmt *
_copyLoadStmt(LoadStmt *from)
{
	LoadStmt   *newnode = makeNode(LoadStmt);

	if (from->filename)
		newnode->filename = pstrdup(from->filename);

	return newnode;
}

static VariableSetStmt *
_copyVariableSetStmt(VariableSetStmt *from)
{
	VariableSetStmt *newnode = makeNode(VariableSetStmt);

	if (from->name)
		newnode->name = pstrdup(from->name);
	if (from->value)
		newnode->value = pstrdup(from->value);

	return newnode;
}

static VariableResetStmt *
_copyVariableResetStmt(VariableResetStmt *from)
{
	VariableResetStmt *newnode = makeNode(VariableResetStmt);

	if (from->name)
		newnode->name = pstrdup(from->name);

	return newnode;
}

static LockStmt *
_copyLockStmt(LockStmt *from)
{
	LockStmt   *newnode = makeNode(LockStmt);

	if (from->relname)
		newnode->relname = pstrdup(from->relname);
	newnode->mode = from->mode;

	return newnode;
}


/* ****************************************************************
 *					pg_list.h copy functions
 * ****************************************************************
 */

static Value *
_copyValue(Value *from)
{
	Value	   *newnode = makeNode(Value);

	newnode->type = from->type;
	switch (from->type)
	{
		case T_Integer:
			newnode->val.ival = from->val.ival;
			break;
		case T_Float:
		case T_String:
			newnode->val.str = pstrdup(from->val.str);
			break;
		default:
			break;
	}
	return newnode;
}

/* ----------------
 *		copyObject returns a copy of the node or list. If it is a list, it
 *		recursively copies its items.
 * ----------------
 */
void *
copyObject(void *from)
{
	void	   *retval;

	if (from == NULL)
		return NULL;
	switch (nodeTag(from))
	{

			/*
			 * PLAN NODES
			 */
		case T_Plan:
			retval = _copyPlan(from);
			break;
		case T_Result:
			retval = _copyResult(from);
			break;
		case T_Append:
			retval = _copyAppend(from);
			break;
		case T_Scan:
			retval = _copyScan(from);
			break;
		case T_SeqScan:
			retval = _copySeqScan(from);
			break;
		case T_IndexScan:
			retval = _copyIndexScan(from);
			break;
		case T_TidScan:
			retval = _copyTidScan(from);
			break;
		case T_Join:
			retval = _copyJoin(from);
			break;
		case T_NestLoop:
			retval = _copyNestLoop(from);
			break;
		case T_MergeJoin:
			retval = _copyMergeJoin(from);
			break;
		case T_HashJoin:
			retval = _copyHashJoin(from);
			break;
		case T_Material:
			retval = _copyMaterial(from);
			break;
		case T_Sort:
			retval = _copySort(from);
			break;
		case T_Group:
			retval = _copyGroup(from);
			break;
		case T_Agg:
			retval = _copyAgg(from);
			break;
		case T_GroupClause:
			retval = _copyGroupClause(from);
			break;
		case T_Unique:
			retval = _copyUnique(from);
			break;
		case T_Hash:
			retval = _copyHash(from);
			break;
		case T_SubPlan:
			retval = _copySubPlan(from);
			break;

			/*
			 * PRIMITIVE NODES
			 */
		case T_Resdom:
			retval = _copyResdom(from);
			break;
		case T_Fjoin:
			retval = _copyFjoin(from);
			break;
		case T_Expr:
			retval = _copyExpr(from);
			break;
		case T_Var:
			retval = _copyVar(from);
			break;
		case T_Attr:
			retval = _copyAttr(from);
			break;
		case T_Oper:
			retval = _copyOper(from);
			break;
		case T_Const:
			retval = _copyConst(from);
			break;
		case T_Param:
			retval = _copyParam(from);
			break;
		case T_Func:
			retval = _copyFunc(from);
			break;
		case T_Array:
			retval = _copyArray(from);
			break;
		case T_ArrayRef:
			retval = _copyArrayRef(from);
			break;
		case T_Aggref:
			retval = _copyAggref(from);
			break;
		case T_SubLink:
			retval = _copySubLink(from);
			break;
		case T_RelabelType:
			retval = _copyRelabelType(from);
			break;
		case T_CaseExpr:
			retval = _copyCaseExpr(from);
			break;
		case T_CaseWhen:
			retval = _copyCaseWhen(from);
			break;

			/*
			 * RELATION NODES
			 */
		case T_RelOptInfo:
			retval = _copyRelOptInfo(from);
			break;
		case T_Path:
			retval = _copyPath(from);
			break;
		case T_IndexPath:
			retval = _copyIndexPath(from);
			break;
		case T_TidPath:
			retval = _copyTidPath(from);
			break;
		case T_NestPath:
			retval = _copyNestPath(from);
			break;
		case T_MergePath:
			retval = _copyMergePath(from);
			break;
		case T_HashPath:
			retval = _copyHashPath(from);
			break;
		case T_PathKeyItem:
			retval = _copyPathKeyItem(from);
			break;
		case T_RestrictInfo:
			retval = _copyRestrictInfo(from);
			break;
		case T_JoinInfo:
			retval = _copyJoinInfo(from);
			break;
		case T_Iter:
			retval = _copyIter(from);
			break;
		case T_Stream:
			retval = _copyStream(from);
			break;
		case T_IndexOptInfo:
			retval = _copyIndexOptInfo(from);
			break;

			/*
			 * PARSE NODES
			 */
		case T_TargetEntry:
			retval = _copyTargetEntry(from);
			break;
		case T_RangeTblEntry:
			retval = _copyRangeTblEntry(from);
			break;
		case T_RowMark:
			retval = _copyRowMark(from);
			break;
		case T_SortClause:
			retval = _copySortClause(from);
			break;
		case T_A_Const:
			retval = _copyAConst(from);
			break;
		case T_TypeName:
			retval = _copyTypeName(from);
			break;
		case T_TypeCast:
			retval = _copyTypeCast(from);
			break;
		case T_Query:
			retval = _copyQuery(from);
			break;
		case T_ClosePortalStmt:
			retval = _copyClosePortalStmt(from);
			break;
		case T_TruncateStmt:
			retval = _copyTruncateStmt(from);
			break;
		case T_NotifyStmt:
			retval = _copyNotifyStmt(from);
			break;
		case T_ListenStmt:
			retval = _copyListenStmt(from);
			break;
		case T_UnlistenStmt:
			retval = _copyUnlistenStmt(from);
			break;
		case T_TransactionStmt:
			retval = _copyTransactionStmt(from);
			break;
		case T_LoadStmt:
			retval = _copyLoadStmt(from);
			break;
		case T_VariableSetStmt:
			retval = _copyVariableSetStmt(from);
			break;
		case T_VariableResetStmt:
			retval = _copyVariableResetStmt(from);
			break;
		case T_LockStmt:
			retval = _copyLockStmt(from);
			break;

			/*
			 * VALUE NODES
			 */
		case T_Integer:
		case T_Float:
		case T_String:
			retval = _copyValue(from);
			break;
		case T_List:
			{
				List	   *list = from,
						   *l,
						   *nl;

				/* rather ugly coding for speed... */
				/* Note the input list cannot be NIL if we got here. */
				nl = lcons(copyObject(lfirst(list)), NIL);
				retval = nl;

				foreach(l, lnext(list))
				{
					lnext(nl) = lcons(copyObject(lfirst(l)), NIL);
					nl = lnext(nl);
				}
			}
			break;
		default:
			elog(ERROR, "copyObject: don't know how to copy %d", nodeTag(from));
			retval = from;
			break;
	}
	return retval;
}
