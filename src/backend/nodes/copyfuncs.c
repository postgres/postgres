/*-------------------------------------------------------------------------
 *
 * copyfuncs.c--
 *	  Copy functions for Postgres tree nodes.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/copyfuncs.c,v 1.17 1997/12/18 03:03:31 momjian Exp $
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

/*
 * listCopy--
 *	  this copy function only copies the "lcons-cells" of the list but not
 *	  its contents. (good for list of pointers as well as list of integers).
 */
List	   *
listCopy(List *list)
{
	List	   *newlist = NIL;
	List	   *l,
			   *nl = NIL;

	foreach(l, list)
	{
		if (newlist == NIL)
		{
			newlist = nl = lcons(lfirst(l), NIL);
		}
		else
		{
			lnext(nl) = lcons(lfirst(l), NIL);
			nl = lnext(nl);
		}
	}
	return newlist;
}

/*
 * Node_Copy--
 *	  a macro to simplify calling of copyObject on the specified field
 */
#define Node_Copy(from, newnode, field) \
	newnode->field = copyObject(from->field)

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
	newnode->cost = from->cost;
	newnode->plan_size = from->plan_size;
	newnode->plan_width = from->plan_width;
	newnode->state = from->state;
	newnode->targetlist = copyObject(from->targetlist);
	newnode->qual = copyObject(from->qual);
	newnode->lefttree = copyObject(from->lefttree);
	newnode->righttree = copyObject(from->righttree);
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
 *		_copyExistential
 * ----------------
 */
static Existential *
_copyExistential(Existential *from)
{
	Existential *newnode = makeNode(Existential);

	/* ----------------
	 *	copy node superclass fields
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
	Node_Copy(from, newnode, resstate);

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
	Node_Copy(from, newnode, unionplans);
	newnode->unionrelid = from->unionrelid;
	Node_Copy(from, newnode, unionrtentries);
	Node_Copy(from, newnode, unionstate);

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
	Node_Copy(from, newnode, scanstate);
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
	CopyScanFields(from, newnode);

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
	Node_Copy(from, newnode, indxstate);

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

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, nlstate);

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
	List	   *newlist;

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

	newnode->mergesortop = from->mergesortop;
	newlist = NIL;

	newnode->mergerightorder = (Oid *) palloc(sizeof(Oid) * 2);
	newnode->mergerightorder[0] = from->mergerightorder[0];
	newnode->mergerightorder[1] = 0;

	newnode->mergeleftorder = (Oid *) palloc(sizeof(Oid) * 2);
	newnode->mergeleftorder[0] = from->mergeleftorder[0];
	newnode->mergeleftorder[1] = 0;

	Node_Copy(from, newnode, mergestate);

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

	Node_Copy(from, newnode, hashjoinstate);

	newnode->hashjointable = from->hashjointable;
	newnode->hashjointablekey = from->hashjointablekey;
	newnode->hashjointablesize = from->hashjointablesize;
	newnode->hashdone = from->hashdone;

	return newnode;
}


/* ----------------
 *		CopyTempFields
 *
 *		This function copies the fields of the Temp node.  It is used by
 *		all the copy functions for classes which inherit from Temp.
 * ----------------
 */
static void
CopyTempFields(Temp *from, Temp *newnode)
{
	newnode->tempid = from->tempid;
	newnode->keycount = from->keycount;
	return;
}


/* ----------------
 *		_copyTemp
 * ----------------
 */
static Temp *
_copyTemp(Temp *from)
{
	Temp	   *newnode = makeNode(Temp);

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyPlanFields((Plan *) from, (Plan *) newnode);
	CopyTempFields(from, newnode);

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
	CopyTempFields((Temp *) from, (Temp *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, matstate);

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
	CopyTempFields((Temp *) from, (Temp *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, sortstate);

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
	CopyTempFields((Temp *) from, (Temp *) newnode);
	
	newnode->tuplePerGroup = from->tuplePerGroup;
	newnode->numCols = from->numCols;
	newnode->grpColIdx = palloc (from->numCols * sizeof (AttrNumber));
	memcpy (newnode->grpColIdx, from->grpColIdx, from->numCols * sizeof (AttrNumber));
	Node_Copy(from, newnode, grpstate);

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
	int			i;

	CopyPlanFields((Plan *) from, (Plan *) newnode);
	CopyTempFields((Temp *) from, (Temp *) newnode);

	newnode->numAgg = from->numAgg;
    newnode->aggs = palloc(sizeof(Aggreg *));
	for (i = 0; i < from->numAgg; i++)
	{
		newnode->aggs[i] = copyObject(from->aggs[i]);
	}

	Node_Copy(from, newnode, aggstate);

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
	CopyTempFields((Temp *) from, (Temp *) newnode);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, uniquestate);

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
	Node_Copy(from, newnode, hashstate);

	newnode->hashtable = from->hashtable;
	newnode->hashtablekey = from->hashtablekey;
	newnode->hashtablesize = from->hashtablesize;

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
	newnode->reslen = from->reslen;

	if (from->resname != NULL)
	{
		newnode->resname = palloc(strlen(from->resname) + 1);
		strcpy(newnode->resname, from->resname);
	}
	else
		newnode->resname = (char *) NULL;

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

	newnode->fj_alwaysDone = (BoolPtr)
		palloc((from->fj_nNodes) * sizeof(bool));

	memmove(from->fj_results,
			newnode->fj_results,
			(from->fj_nNodes) * sizeof(Datum));

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

	newnode->varnoold = from->varnoold;
	newnode->varoattno = from->varoattno;

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
	static Oid	cached_type;
	static bool cached_typbyval;

	Const	   *newnode = makeNode(Const);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->consttype = from->consttype;
	newnode->constlen = from->constlen;

	/* ----------------
	 *	XXX super cheesy hack until parser/planner
	 *	puts in the right values here.
	 * ----------------
	 */
	if (!from->constisnull && cached_type != from->consttype)
	{
		HeapTuple	typeTuple;
		TypeTupleForm typeStruct;

		/* ----------------
		 *	 get the type tuple corresponding to the paramList->type,
		 *	 If this fails, returnValue has been pre-initialized
		 *	 to "null" so we just return it.
		 * ----------------
		 */
		typeTuple = SearchSysCacheTuple(TYPOID,
										ObjectIdGetDatum(from->consttype),
										0, 0, 0);

		/* ----------------
		 *	 get the type length and by-value from the type tuple and
		 *	 save the information in our one element cache.
		 * ----------------
		 */
		Assert(PointerIsValid(typeTuple));

		typeStruct = (TypeTupleForm) GETSTRUCT(typeTuple);
		cached_typbyval = (typeStruct)->typbyval ? true : false;
		cached_type = from->consttype;
	}

	from->constbyval = cached_typbyval;

	if (!from->constisnull)
	{
		/* ----------------
		 *		copying the Datum in a const node is a bit trickier
		 *	because it might be a pointer and it might also be of
		 *	variable length...
		 * ----------------
		 */
		if (from->constbyval == true)
		{
			/* ----------------
			 *	passed by value so just copy the datum.
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
			if (from->constlen != -1)
			{
				/* ----------------
				 *		fixed length structure
				 * ----------------
				 */
				newnode->constvalue = PointerGetDatum(palloc(from->constlen));
				memmove((char *) newnode->constvalue,
						(char *) from->constvalue, from->constlen);
			}
			else
			{
				/* ----------------
				 *		variable length structure.	here the length is stored
				 *	in the first int pointed to by the constval.
				 * ----------------
				 */
				int			length;

				length = *((int *) from->constvalue);
				newnode->constvalue = PointerGetDatum(palloc(length));
				memmove((char *) newnode->constvalue,
						(char *) from->constvalue, length);
			}
		}
	}
	else
	{
		newnode->constvalue = from->constvalue;
	}
	newnode->constisnull = from->constisnull;
	newnode->constbyval = from->constbyval;

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
	{
		newnode->paramname = pstrdup(from->paramname);
	}
	else
		newnode->paramname = (char *) NULL;

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
 *		_copyAggreg
 * ----------------
 */
static Aggreg *
_copyAggreg(Aggreg *from)
{
	Aggreg	   *newnode = makeNode(Aggreg);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->aggname = pstrdup(from->aggname);
	newnode->basetype = from->basetype;
	newnode->aggtype = from->aggtype;

	Node_Copy(from, newnode, target);

	newnode->aggno = from->aggno;

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
	newnode->refelemtype = from->refelemtype;
	newnode->refattrlength = from->refattrlength;
	newnode->refelemlength = from->refelemlength;
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
 *		_copyRel
 * ----------------
 */
/*
 ** when you change this, also make sure to fix up xfunc_copyRel in
 ** planner/path/xfunc.c accordingly!!!
 **			-- JMH, 8/2/93
 */
static Rel *
_copyRel(Rel *from)
{
	Rel		   *newnode = makeNode(Rel);
	int			i,
				len;

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->relids = listCopy(from->relids);

	newnode->indexed = from->indexed;
	newnode->pages = from->pages;
	newnode->tuples = from->tuples;
	newnode->size = from->size;
	newnode->width = from->width;
	newnode->indproc = from->indproc;

	Node_Copy(from, newnode, targetlist);
	Node_Copy(from, newnode, pathlist);
	Node_Copy(from, newnode, unorderedpath);
	Node_Copy(from, newnode, cheapestpath);
	newnode->pruneable = from->pruneable;
	newnode->relam = from->relam;

	if (from->classlist)
	{
		for (len = 0; from->classlist[len] != 0; len++)
			;
		newnode->classlist = (Oid *) palloc(sizeof(Oid) * (len + 1));
		for (i = 0; i < len; i++)
		{
			newnode->classlist[i] = from->classlist[i];
		}
		newnode->classlist[len] = 0;
	}

	if (from->indexkeys)
	{
		for (len = 0; from->indexkeys[len] != 0; len++)
			;
		newnode->indexkeys = (int *) palloc(sizeof(int) * (len + 1));
		for (i = 0; i < len; i++)
		{
			newnode->indexkeys[i] = from->indexkeys[i];
		}
		newnode->indexkeys[len] = 0;
	}

	if (from->ordering)
	{
		for (len = 0; from->ordering[len] != 0; len++)
			;
		newnode->ordering = (Oid *) palloc(sizeof(Oid) * (len + 1));
		for (i = 0; i < len; i++)
		{
			newnode->ordering[i] = from->ordering[i];
		}
		newnode->ordering[len] = 0;
	}

	Node_Copy(from, newnode, clauseinfo);
	Node_Copy(from, newnode, joininfo);
	Node_Copy(from, newnode, innerjoin);
	Node_Copy(from, newnode, superrels);

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
	newnode->pathtype = from->pathtype;

	/*
	 * Modify the next line, since it causes the copying to cycle (i.e.
	 * the parent points right back here! -- JMH, 7/7/92. Old version:
	 * Node_Copy(from, newnode, parent);
	 */
	newnode->parent = from->parent;

	newnode->path_cost = from->path_cost;

	newnode->p_ordering.ordtype = from->p_ordering.ordtype;
	if (from->p_ordering.ordtype == SORTOP_ORDER)
	{
		int			len,
					i;
		Oid		   *ordering = from->p_ordering.ord.sortop;

		if (ordering)
		{
			for (len = 0; ordering[len] != 0; len++)
				;
			newnode->p_ordering.ord.sortop =
				(Oid *) palloc(sizeof(Oid) * (len + 1));
			for (i = 0; i < len; i++)
			{
				newnode->p_ordering.ord.sortop[i] = ordering[i];
			}
			newnode->p_ordering.ord.sortop[len] = 0;
		}
		else
		{
			newnode->p_ordering.ord.sortop = NULL;
		}
	}
	else
	{
		Node_Copy(from, newnode, p_ordering.ord.merge);
	}

	Node_Copy(from, newnode, keys);

	newnode->outerjoincost = from->outerjoincost;

	newnode->joinid = listCopy(from->joinid);
	Node_Copy(from, newnode, locclauseinfo);
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

	if (from->indexkeys)
	{
		int			i,
					len;

		for (len = 0; from->indexkeys[len] != 0; len++)
			;
		newnode->indexkeys = (int *) palloc(sizeof(int) * (len + 1));
		for (i = 0; i < len; i++)
		{
			newnode->indexkeys[i] = from->indexkeys[i];
		}
		newnode->indexkeys[len] = 0;
	}

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
	Node_Copy(from, newnode, pathclauseinfo);
	Node_Copy(from, newnode, outerjoinpath);
	Node_Copy(from, newnode, innerjoinpath);
}

/* ----------------
 *		_copyJoinPath
 * ----------------
 */
static JoinPath *
_copyJoinPath(JoinPath *from)
{
	JoinPath   *newnode = makeNode(JoinPath);

	/* ----------------
	 *	copy the node superclass fields
	 * ----------------
	 */
	CopyPathFields((Path *) from, (Path *) newnode);
	CopyJoinPathFields(from, newnode);

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
	Node_Copy(from, newnode, outerhashkeys);
	Node_Copy(from, newnode, innerhashkeys);

	return newnode;
}

/* ----------------
 *		_copyOrderKey
 * ----------------
 */
static OrderKey *
_copyOrderKey(OrderKey *from)
{
	OrderKey   *newnode = makeNode(OrderKey);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->attribute_number = from->attribute_number;
	newnode->array_index = from->array_index;

	return newnode;
}


/* ----------------
 *		_copyJoinKey
 * ----------------
 */
static JoinKey *
_copyJoinKey(JoinKey *from)
{
	JoinKey    *newnode = makeNode(JoinKey);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, outer);
	Node_Copy(from, newnode, inner);

	return newnode;
}

/* ----------------
 *		_copyMergeOrder
 * ----------------
 */
static MergeOrder *
_copyMergeOrder(MergeOrder *from)
{
	MergeOrder *newnode = makeNode(MergeOrder);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->join_operator = from->join_operator;
	newnode->left_operator = from->left_operator;
	newnode->right_operator = from->right_operator;
	newnode->left_type = from->left_type;
	newnode->right_type = from->right_type;

	return newnode;
}

/* ----------------
 *		_copyCInfo
 * ----------------
 */
static CInfo *
_copyCInfo(CInfo *from)
{
	CInfo	   *newnode = makeNode(CInfo);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, clause);

	newnode->selectivity = from->selectivity;
	newnode->notclause = from->notclause;

	Node_Copy(from, newnode, indexids);
	Node_Copy(from, newnode, mergesortorder);
	newnode->hashjoinoperator = from->hashjoinoperator;
	newnode->cinfojoinid = listCopy(from->cinfojoinid);

	return newnode;
}

/* ----------------
 *		CopyJoinMethodFields
 *
 *		This function copies the fields of the JoinMethod node.  It is used by
 *		all the copy functions for classes which inherit from JoinMethod.
 * ----------------
 */
static void
CopyJoinMethodFields(JoinMethod *from, JoinMethod *newnode)
{
	Node_Copy(from, newnode, jmkeys);
	Node_Copy(from, newnode, clauses);
	return;
}

/* ----------------
 *		_copyJoinMethod
 * ----------------
 */
static JoinMethod *
_copyJoinMethod(JoinMethod *from)
{
	JoinMethod *newnode = makeNode(JoinMethod);

	CopyJoinMethodFields(from, newnode);

	return newnode;
}

/* ----------------
 *		_copyHInfo
 * ----------------
 */
static HInfo *
_copyHInfo(HInfo *from)
{
	HInfo	   *newnode = makeNode(HInfo);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->hashop = from->hashop;

	return newnode;
}

/* ----------------
 *		_copyMInfo
 * ----------------
 */
static MInfo *
_copyMInfo(MInfo *from)
{
	MInfo	   *newnode = makeNode(MInfo);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, m_ordering);

	return newnode;
}

/* ----------------
 *		_copyJInfo
 * ----------------
 */
static JInfo *
_copyJInfo(JInfo *from)
{
	JInfo	   *newnode = makeNode(JInfo);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	newnode->otherrels = listCopy(from->otherrels);
	Node_Copy(from, newnode, jinfoclauseinfo);

	newnode->mergesortable = from->mergesortable;
	newnode->hashjoinable = from->hashjoinable;
	newnode->inactive = from->inactive;

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
	newnode->groupup = from->groupup;
	newnode->groupcost = from->groupcost;
	newnode->groupsel = from->groupsel;
	newnode->upstream = (StreamPtr) NULL;		/* only copy nodes
												 * downwards! */
	Node_Copy(from, newnode, downstream);
	if (newnode->downstream)
		((Stream *) newnode->downstream)->upstream = (Stream *) newnode;

	return newnode;
}

/* ****************
 *			  parsenodes.h routines have no copy functions
 * ****************
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

	memcpy(newnode, from, sizeof(RangeTblEntry));
	if (from->relname)
		newnode->relname = pstrdup(from->relname);
	if (from->refname)
		newnode->refname = pstrdup(from->refname);

	return newnode;
}

static SortClause *
_copySortClause(SortClause *from)
{
	SortClause *newnode = makeNode(SortClause);

	Node_Copy(from, newnode, resdom);
	newnode->opoid = from->opoid;

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
	{
		newnode->name = pstrdup(from->name);
	}
	else
	{
		from->name = (char *) 0;
	}
	newnode->setof = from->setof;
	Node_Copy(from, newnode, arrayBounds);
	newnode->typlen = from->typlen;

	return newnode;
}

static Query *
_copyQuery(Query *from)
{
	Query	   *newnode = makeNode(Query);
	
	newnode->commandType = from->commandType;
	newnode->resultRelation = from->resultRelation;
	/* probably should dup this string instead of just pointing */
	/* to the old one  --djm */
	if (from->into)
	{
		newnode->into = pstrdup(from->into);
	}
	else
	{
		newnode->into = (char *) 0;
	}
	newnode->isPortal = from->isPortal;
	Node_Copy(from, newnode, rtable);
	if (from->utilityStmt && nodeTag(from->utilityStmt) == T_NotifyStmt)
	{
		NotifyStmt *from_notify = (NotifyStmt *) from->utilityStmt;
		NotifyStmt *n = makeNode(NotifyStmt);
		int			length = strlen(from_notify->relname);

		n->relname = palloc(length + 1);
		strcpy(n->relname, from_notify->relname);
		newnode->utilityStmt = (Node *) n;
	}
	if (from->uniqueFlag)
	{
		newnode->uniqueFlag = (char *) palloc(strlen(from->uniqueFlag) + 1);
		strcpy(newnode->uniqueFlag, from->uniqueFlag);
	}
	else
		newnode->uniqueFlag = NULL;
	Node_Copy(from, newnode, sortClause);
	Node_Copy(from, newnode, targetList);
	Node_Copy(from, newnode, qual);

	Node_Copy(from, newnode, groupClause);
	Node_Copy(from, newnode, havingQual); /* currently ignored */

	Node_Copy(from, newnode, Aggreg);

	return newnode;
}


/* ****************
 *			  mnodes.h routines have no copy functions
 * ****************
 */

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
		case T_String:
			newnode->val.str = pstrdup(from->val.str);
			break;
		case T_Integer:
			newnode->val.ival = from->val.ival;
			break;
		case T_Float:
			newnode->val.dval = from->val.dval;
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
void	   *
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
		case T_Existential:
			retval = _copyExistential(from);
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
		case T_Temp:
			retval = _copyTemp(from);
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
		case T_Unique:
			retval = _copyUnique(from);
			break;
		case T_Hash:
			retval = _copyHash(from);
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
		case T_Aggreg:
			retval = _copyAggreg(from);
			break;

			/*
			 * RELATION NODES
			 */
		case T_Rel:
			retval = _copyRel(from);
			break;
		case T_Path:
			retval = _copyPath(from);
			break;
		case T_IndexPath:
			retval = _copyIndexPath(from);
			break;
		case T_JoinPath:
			retval = _copyJoinPath(from);
			break;
		case T_MergePath:
			retval = _copyMergePath(from);
			break;
		case T_HashPath:
			retval = _copyHashPath(from);
			break;
		case T_OrderKey:
			retval = _copyOrderKey(from);
			break;
		case T_JoinKey:
			retval = _copyJoinKey(from);
			break;
		case T_MergeOrder:
			retval = _copyMergeOrder(from);
			break;
		case T_CInfo:
			retval = _copyCInfo(from);
			break;
		case T_JoinMethod:
			retval = _copyJoinMethod(from);
			break;
		case T_HInfo:
			retval = _copyHInfo(from);
			break;
		case T_MInfo:
			retval = _copyMInfo(from);
			break;
		case T_JInfo:
			retval = _copyJInfo(from);
			break;
		case T_Iter:
			retval = _copyIter(from);
			break;
		case T_Stream:
			retval = _copyStream(from);
			break;

			/*
			 * PARSE NODES
			 */
		case T_Query:
			retval = _copyQuery(from);
			break;
		case T_TargetEntry:
			retval = _copyTargetEntry(from);
			break;
		case T_RangeTblEntry:
			retval = _copyRangeTblEntry(from);
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

			/*
			 * VALUE NODES
			 */
		case T_Integer:
		case T_String:
		case T_Float:
			retval = _copyValue(from);
			break;
		case T_List:
			{
				List	   *list = from,
						   *l;
				List	   *newlist = NIL,
						   *nl = NIL;

				foreach(l, list)
				{
					if (newlist == NIL)
					{
						newlist = nl = lcons(copyObject(lfirst(l)), NIL);
					}
					else
					{
						lnext(nl) = lcons(copyObject(lfirst(l)), NIL);
						nl = lnext(nl);
					}
				}
				retval = newlist;
			}
			break;
		default:
			elog(NOTICE, "copyObject: don't know how to copy %d", nodeTag(from));
			retval = from;
			break;
	}
	return retval;
}
