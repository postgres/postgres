/*-------------------------------------------------------------------------
 *
 * outfuncs.c--
 *    routines to convert a node to ascii representation
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/nodes/outfuncs.c,v 1.2 1996/11/08 05:56:43 momjian Exp $
 *
 * NOTES
 *    Every (plan) node in POSTGRES has an associated "out" routine which
 *    knows how to create its ascii representation. These functions are
 *    useful for debugging as well as for storing plans in the system
 *    catalogs (eg. indexes). This is also the plan string sent out in
 *    Mariposa.
 *
 *    These functions update the in/out argument of type StringInfo
 *    passed to them. This argument contains the string holding the ASCII
 *    representation plus some other information (string length, etc.)
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "fmgr.h"
#include "utils/elog.h"
#include "utils/datum.h"
#include "utils/palloc.h"

#include "nodes/nodes.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"

#include "catalog/pg_type.h"
#include "lib/stringinfo.h"

static void _outDatum(StringInfo str, Datum value, Oid type);
static void _outNode(StringInfo str, void *obj);

/*
 * _outIntList -
 *     converts a List of integers
 */
void
_outIntList(StringInfo str, List *list)
{
    List *l;
    char buf[500];

    appendStringInfo(str, "(");
    foreach(l, list) {
	sprintf(buf, "%d ", (int)lfirst(l));
	appendStringInfo(str, buf);
    }
    appendStringInfo(str, ")");
}

static void
_outQuery(StringInfo str, Query *node)
{
    char buf[500];
    
    sprintf(buf, "QUERY");
    appendStringInfo(str,buf);

    sprintf(buf, " :command %d", node->commandType);
    appendStringInfo(str,buf);
    if (node->utilityStmt &&
	nodeTag(node->utilityStmt) == T_NotifyStmt)
	sprintf(buf," :utility %s", 
		((NotifyStmt*)(node->utilityStmt))->relname);
    else /* use "" to designate  */
	sprintf(buf," :utility \"\"");
    appendStringInfo(str,buf);

    sprintf(buf, " :resrel %d", node->resultRelation);
    appendStringInfo(str,buf);
    sprintf(buf, " :rtable ");
    appendStringInfo(str,buf);
    _outNode(str, node->rtable);
    if (node->uniqueFlag)
      sprintf(buf, " :unique %s", node->uniqueFlag);
    else /* use "" to designate non-unique */
      sprintf(buf, " :unique \"\"");
    appendStringInfo(str,buf);
    sprintf(buf, " :targetlist ");
    appendStringInfo(str,buf);
    _outNode(str, node->targetList);
    sprintf(buf, " :qual ");
    appendStringInfo(str,buf); 
    _outNode(str, node->qual);
    
}

/*
 * print the basic stuff of all nodes that inherit from Plan
 */
static void
_outPlanInfo(StringInfo str, Plan *node)
{
    char buf[500];
    
    sprintf(buf, " :cost %g", node->cost );
    appendStringInfo(str,buf);
    sprintf(buf, " :size %d", node->plan_size);
    appendStringInfo(str,buf);
    sprintf(buf, " :width %d", node->plan_width);
    appendStringInfo(str,buf);
    sprintf(buf, " :state %s", (node->state == (EState*) NULL ?
				"nil" : "non-NIL"));
    appendStringInfo(str,buf);
    sprintf(buf, " :qptargetlist ");
    appendStringInfo(str,buf);
    _outNode(str, node->targetlist);
    sprintf(buf, " :qpqual ");
    appendStringInfo(str,buf);
    _outNode(str, node->qual);
    sprintf(buf, " :lefttree ");
    appendStringInfo(str,buf);
    _outNode(str, node->lefttree);
    sprintf(buf, " :righttree ");
    appendStringInfo(str,buf); 
    _outNode(str, node->righttree);
    
}

/*
 *  Stuff from plannodes.h
 */
static void
_outPlan(StringInfo str, Plan *node)
{
    char buf[500];
    
    sprintf(buf, "PLAN");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
}

static void
_outResult(StringInfo str, Result *node)
{
    char buf[500];
    
    sprintf(buf, "RESULT");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :resconstantqual ");
    appendStringInfo(str,buf); 
    _outNode(str, node->resconstantqual);
    
}

/*
 *  Existential is a subclass of Plan.
 */
static void
_outExistential(StringInfo str, Existential *node)
{
    char buf[500];
    
    sprintf(buf, "EXISTENTIAL");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    
}

/*
 *  Append is a subclass of Plan.
 */
static void
_outAppend(StringInfo str, Append *node)
{
    char buf[500];
    
    sprintf(buf, "APPEND");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :unionplans ");
    appendStringInfo(str,buf);
    _outNode(str, node->unionplans);
    
    sprintf(buf, " :unionrelid %d", node->unionrelid);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :unionrtentries ");
    appendStringInfo(str,buf);
    _outNode(str, node->unionrtentries);
    
}

/*
 *  Join is a subclass of Plan
 */
static void
_outJoin(StringInfo str, Join *node)
{
    char buf[500];
    
    sprintf(buf, "JOIN");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
}

/*
 *  NestLoop is a subclass of Join
 */
static void
_outNestLoop(StringInfo str, NestLoop *node)
{
    char buf[500];
    
    sprintf(buf, "NESTLOOP");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
}

/*
 *  MergeJoin is a subclass of Join
 */
static void
_outMergeJoin(StringInfo str, MergeJoin *node)
{
    char buf[500];
    
    sprintf(buf, "MERGEJOIN");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :mergeclauses ");
    appendStringInfo(str,buf);
    _outNode(str, node->mergeclauses);
    
    sprintf(buf, " :mergesortop %d", node->mergesortop);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :mergerightorder %d", node->mergerightorder[0]);
    appendStringInfo(str, buf);

    sprintf(buf, " :mergeleftorder %d", node->mergeleftorder[0]);
    appendStringInfo(str, buf);
}

/*
 *  HashJoin is a subclass of Join.
 */
static void
_outHashJoin(StringInfo str, HashJoin *node)
{
    char buf[500];
    
    sprintf(buf, "HASHJOIN");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :hashclauses ");
    appendStringInfo(str,buf);
    _outNode(str, node->hashclauses);
    
    sprintf(buf, " :hashjoinop %d",node->hashjoinop);
    appendStringInfo(str,buf);
    sprintf(buf, " :hashjointable 0x%x", (int) node->hashjointable);
    appendStringInfo(str,buf);
    sprintf(buf, " :hashjointablekey %d", node->hashjointablekey);
    appendStringInfo(str,buf);
    sprintf(buf, " :hashjointablesize %d", node->hashjointablesize);
    appendStringInfo(str,buf);
    sprintf(buf, " :hashdone %d", node->hashdone);
    appendStringInfo(str,buf);
}

/*
 *  Scan is a subclass of Node
 */
static void
_outScan(StringInfo str, Scan *node)
{
    char buf[500];
    
    sprintf(buf, "SCAN");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :scanrelid %d", node->scanrelid);
    appendStringInfo(str,buf);
    
}

/*
 *  SeqScan is a subclass of Scan
 */
static void
_outSeqScan(StringInfo str, SeqScan *node)
{
    char buf[500];
    
    sprintf(buf, "SEQSCAN");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :scanrelid %d", node->scanrelid);
    appendStringInfo(str,buf);
    
    
}

/*
 *  IndexScan is a subclass of Scan
 */
static void
_outIndexScan(StringInfo str, IndexScan *node)
{
    char buf[500];
    
    sprintf(buf, "INDEXSCAN");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :scanrelid %d", node->scan.scanrelid);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :indxid ");
    appendStringInfo(str,buf);
    _outIntList(str, node->indxid);
    
    sprintf(buf, " :indxqual ");
    appendStringInfo(str,buf);
    _outNode(str, node->indxqual);
    
}

/*
 *  Temp is a subclass of Plan
 */
static void
_outTemp(StringInfo str, Temp *node)
{
    char buf[500];
    
    sprintf(buf, "TEMP");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :tempid %d", node->tempid);
    appendStringInfo(str,buf);
    sprintf(buf, " :keycount %d", node->keycount);
    appendStringInfo(str,buf);
    
}

/*
 *  Sort is a subclass of Temp
 */
static void
_outSort(StringInfo str, Sort *node)
{
    char buf[500];
    
    sprintf(buf, "SORT");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :tempid %d", node->tempid);
    appendStringInfo(str,buf);
    sprintf(buf, " :keycount %d", node->keycount);
    appendStringInfo(str,buf);
    
}

static void
_outAgg(StringInfo str, Agg *node)
{
    char buf[500];
    sprintf(buf, "AGG");
    appendStringInfo(str,buf);
    _outPlanInfo(str,(Plan*)node);
    
    /* the actual Agg fields */
    sprintf(buf, " :numagg %d ", node->numAgg);
    appendStringInfo(str, buf);
}

static void
_outGroup(StringInfo str, Group *node)
{
     char buf[500];
     sprintf(buf, "GRP");
     appendStringInfo(str,buf);
     _outPlanInfo(str,(Plan*)node);
     
     /* the actual Group fields */
     sprintf(buf, " :numCols %d ", node->numCols);
     appendStringInfo(str, buf);
     sprintf(buf, " :tuplePerGroup %s", node->tuplePerGroup ? "true" : "nil");
     appendStringInfo(str, buf);
}
  
 

/*
 *  For some reason, unique is a subclass of Temp.
 */
static void
_outUnique(StringInfo str, Unique *node)
{
    char buf[500];
    
    sprintf(buf, "UNIQUE");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :tempid %d", node->tempid);
    appendStringInfo(str,buf);
    sprintf(buf, " :keycount %d", node->keycount);
    appendStringInfo(str,buf);
    
}


/*
 *  Hash is a subclass of Temp
 */
static void
_outHash(StringInfo str, Hash *node)
{
    char buf[500];
    
    sprintf(buf, "HASH");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :hashkey ");
    appendStringInfo(str,buf);
    _outNode(str, node->hashkey);
    
    sprintf(buf, " :hashtable 0x%x", (int) (node->hashtable));
    appendStringInfo(str,buf);
    sprintf(buf, " :hashtablekey %d", node->hashtablekey);
    appendStringInfo(str,buf);
    sprintf(buf, " :hashtablesize %d", node->hashtablesize);
    appendStringInfo(str,buf);
}

static void
_outTee(StringInfo str, Tee *node)
{
    char buf[500];
    
    sprintf(buf, "TEE");
    appendStringInfo(str,buf);
    _outPlanInfo(str, (Plan*) node);
    
    sprintf(buf, " :leftParent %X", (int) (node->leftParent));
    appendStringInfo(str,buf);
    sprintf(buf, " :rightParent %X", (int) (node->rightParent));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :rtentries ");
    appendStringInfo(str,buf);
    _outNode(str, node->rtentries);
}



/*****************************************************************************
 *
 *  Stuff from primnodes.h.
 *
 *****************************************************************************/


/*
 *  Resdom is a subclass of Node
 */
static void
_outResdom(StringInfo str, Resdom *node)
{
    char buf[500];
    
    sprintf(buf, "RESDOM");
    appendStringInfo(str,buf);
    sprintf(buf, " :resno %hd", node->resno);
    appendStringInfo(str,buf);
    sprintf(buf, " :restype %d", node->restype);
    appendStringInfo(str,buf);
    sprintf(buf, " :reslen %d", node->reslen);
    appendStringInfo(str,buf);
    sprintf(buf, " :resname \"%.*s\"", NAMEDATALEN,
	    ((node->resname) ? ((char *) node->resname) : "null"));
    appendStringInfo(str,buf);
    sprintf(buf, " :reskey %d", node->reskey);
    appendStringInfo(str,buf);
    sprintf(buf, " :reskeyop %ld", (long int) node->reskeyop);
    appendStringInfo(str,buf);
    sprintf(buf, " :resjunk %d", node->resjunk);
    appendStringInfo(str,buf);
    
}

static void
_outFjoin(StringInfo str, Fjoin *node)
{
    char buf[500];
    int i;
    
    sprintf(buf, "FJOIN");
    appendStringInfo(str,buf);
    sprintf(buf, " :initialized %s", node->fj_initialized ? "true":"nil");
    appendStringInfo(str,buf);
    sprintf(buf, " :nNodes %d", node->fj_nNodes);
    appendStringInfo(str,buf);

    appendStringInfo(str," :innerNode ");
    appendStringInfo(str,buf);
    _outNode(str, node->fj_innerNode);

    sprintf(buf, " :results @  0x%x ", (int)(node->fj_results));
    appendStringInfo(str, buf);
    
    appendStringInfo( str, " :alwaysdone ");
    for (i = 0; i<node->fj_nNodes; i++)
	{
	    sprintf(buf, " %s ", ((node->fj_alwaysDone[i]) ? "true" : "nil"));
	    appendStringInfo(str, buf);
	}
}

/*
 *  Expr is a subclass of Node
 */
static void
_outExpr(StringInfo str, Expr *node)
{
    char buf[500];
    char *opstr = NULL;
    
    sprintf(buf, "EXPR");
    appendStringInfo(str,buf);

    sprintf(buf, " :typeOid %d", node->typeOid);
    appendStringInfo(str,buf);
    switch(node->opType) {
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
    }
    sprintf(buf, " :opType %s", opstr);
    appendStringInfo(str,buf);
    sprintf(buf, " :oper ");
    appendStringInfo(str,buf);
    _outNode(str, node->oper);
    sprintf(buf, " :args ");
    appendStringInfo(str,buf);
    _outNode(str, node->args);
}

/*
 *  Var is a subclass of Expr
 */
static void
_outVar(StringInfo str, Var *node)
{
    char buf[500];
    
    sprintf(buf, "VAR");
    appendStringInfo(str,buf);
    sprintf(buf, " :varno %d", node->varno);
    appendStringInfo(str,buf);
    sprintf(buf, " :varattno %hd", node->varattno);
    appendStringInfo(str,buf);
    sprintf(buf, " :vartype %d", node->vartype);
    appendStringInfo(str,buf);
    sprintf(buf, " :varnoold %d", node->varnoold);
    appendStringInfo(str,buf);
    sprintf(buf, " :varoattno %d", node->varoattno);
    appendStringInfo(str,buf);
}

/*
 *  Const is a subclass of Expr
 */
static void
_outConst(StringInfo str, Const *node)
{
    char buf[500];
    
    sprintf(buf, "CONST");
    appendStringInfo(str,buf);
    sprintf(buf, " :consttype %d", node->consttype);
    appendStringInfo(str,buf);
    sprintf(buf, " :constlen %hd", node->constlen);
    appendStringInfo(str,buf);
    sprintf(buf, " :constisnull %s", (node->constisnull ? "true" : "nil"));
    appendStringInfo(str,buf);
    sprintf(buf, " :constvalue ");
    appendStringInfo(str,buf);
    if (node->constisnull) {
	sprintf(buf, "NIL ");
	appendStringInfo(str,buf);
    } else {
	_outDatum(str, node->constvalue, node->consttype);
    }
    sprintf(buf, " :constbyval %s", (node->constbyval ? "true" : "nil"));
    appendStringInfo(str,buf);
    
}

/*
 *  Aggreg
 */
static void
_outAggreg(StringInfo str, Aggreg *node)
{
    char buf[500];
    
    sprintf(buf, "AGGREG");
    appendStringInfo(str,buf);
    sprintf(buf, " :aggname \"%.*s\"", NAMEDATALEN, (char*)node->aggname);
    appendStringInfo(str,buf);
    sprintf(buf, " :basetype %d", node->basetype);
    appendStringInfo(str,buf);
    sprintf(buf, " :aggtype %d", node->aggtype);
    appendStringInfo(str,buf);
    sprintf(buf, " :aggno %d", node->aggno);
    appendStringInfo(str,buf);

    sprintf(buf, " :target ");
    appendStringInfo(str,buf);
    _outNode(str, node->target);
}

/*
 *  Array is a subclass of Expr
 */
static void
_outArray(StringInfo str, Array *node)
{
    char buf[500];
    int i;
    sprintf(buf, "ARRAY");
    appendStringInfo(str, buf);
    sprintf(buf, " :arrayelemtype %d", node->arrayelemtype);
    appendStringInfo(str, buf);
    sprintf(buf, " :arrayelemlength %d", node->arrayelemlength);
    appendStringInfo(str, buf);
    sprintf(buf, " :arrayelembyval %c", (node->arrayelembyval) ? 't' : 'f');
    appendStringInfo(str, buf);
    sprintf(buf, " :arrayndim %d", node->arrayndim);
    appendStringInfo(str, buf);
    sprintf(buf, " :arraylow ");
    appendStringInfo(str, buf);
    for (i = 0; i < node->arrayndim; i++){
	sprintf(buf, "  %d", node->arraylow.indx[i]);
	appendStringInfo(str, buf);
    }
    sprintf(buf, " :arrayhigh ");
    appendStringInfo(str, buf);
    for (i = 0; i < node->arrayndim; i++){
	sprintf(buf, " %d", node->arrayhigh.indx[i]);
	appendStringInfo(str, buf);
    }
    sprintf(buf, " :arraylen %d", node->arraylen);
    appendStringInfo(str, buf);
}

/*
 *  ArrayRef is a subclass of Expr
 */
static void
_outArrayRef(StringInfo str, ArrayRef *node)
{
    char buf[500];
    
    sprintf(buf, "ARRAYREF");
    appendStringInfo(str, buf);
    sprintf(buf, " :refelemtype %d", node->refelemtype);
    appendStringInfo(str, buf);
    sprintf(buf, " :refattrlength %d", node->refattrlength);
    appendStringInfo(str, buf);
    sprintf(buf, " :refelemlength %d", node->refelemlength);
    appendStringInfo(str, buf);
    sprintf(buf, " :refelembyval %c", (node->refelembyval) ? 't' : 'f');
    appendStringInfo(str, buf);

    sprintf(buf, " :refupperindex ");
    appendStringInfo(str, buf);
    _outNode(str, node->refupperindexpr);

    sprintf(buf, " :reflowerindex ");
    appendStringInfo(str, buf);
    _outNode(str, node->reflowerindexpr);

    sprintf(buf, " :refexpr ");
    appendStringInfo(str, buf);
    _outNode(str, node->refexpr);

    sprintf(buf, " :refassgnexpr ");
    appendStringInfo(str, buf);
    _outNode(str, node->refassgnexpr);
}

/*
 *  Func is a subclass of Expr
 */
static void
_outFunc(StringInfo str, Func *node)
{
    char buf[500];
    
    sprintf(buf, "FUNC");
    appendStringInfo(str,buf);
    sprintf(buf, " :funcid %d", node->funcid);
    appendStringInfo(str,buf);
    sprintf(buf, " :functype %d", node->functype);
    appendStringInfo(str,buf);
    sprintf(buf, " :funcisindex %s",
	    (node->funcisindex ? "true" : "nil"));
    appendStringInfo(str,buf);
    sprintf(buf, " :funcsize %d", node->funcsize);
    appendStringInfo(str, buf);
    sprintf(buf, " :func_fcache @ 0x%x", (int)(node->func_fcache));
    appendStringInfo(str, buf);

    appendStringInfo(str, " :func_tlist ");
    _outNode(str, node->func_tlist);

    appendStringInfo(str, " :func_planlist ");
    _outNode(str, node->func_planlist);
}

/*
 *  Oper is a subclass of Expr
 */
static void
_outOper(StringInfo str, Oper *node)
{
    char buf[500];
    
    sprintf(buf, "OPER");
    appendStringInfo(str,buf);
    sprintf(buf, " :opno %d", node->opno);
    appendStringInfo(str,buf);
    sprintf(buf, " :opid %d", node->opid);
    appendStringInfo(str,buf);
    sprintf(buf, " :opresulttype %d", node->opresulttype);
    appendStringInfo(str,buf);
    
}

/*
 *  Param is a subclass of Expr
 */
static void
_outParam(StringInfo str, Param *node)
{
    char buf[500];
    
    sprintf(buf, "PARAM");
    appendStringInfo(str,buf);
    sprintf(buf, " :paramkind %d", node->paramkind);
    appendStringInfo(str,buf);
    sprintf(buf, " :paramid %hd", node->paramid);
    appendStringInfo(str,buf);
    sprintf(buf, " :paramname \"%.*s\"", NAMEDATALEN, node->paramname);
    appendStringInfo(str,buf);
    sprintf(buf, " :paramtype %d", node->paramtype);
    appendStringInfo(str,buf);
    
    appendStringInfo(str, " :param_tlist ");
    _outNode(str, node->param_tlist);
}

/*
 *  Stuff from execnodes.h
 */

/*
 *  EState is a subclass of Node.
 */
static void
_outEState(StringInfo str, EState *node)
{
    char buf[500];
    
    sprintf(buf, "ESTATE");
    appendStringInfo(str,buf);
    sprintf(buf, " :direction %d", node->es_direction);
    appendStringInfo(str,buf);

    sprintf(buf, " :range_table ");
    appendStringInfo(str,buf);
    _outNode(str, node->es_range_table);
    
    sprintf(buf, " :result_relation_info @ 0x%x",
	    (int) (node->es_result_relation_info));
    appendStringInfo(str,buf);
    
}

/*
 *  Stuff from relation.h
 */
static void
_outRel(StringInfo str, Rel *node)
{
    char buf[500];
    
    sprintf(buf, "REL");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :relids ");
    appendStringInfo(str,buf);
    _outIntList(str, node->relids);
    
    sprintf(buf, " :indexed %s", (node->indexed ? "true" : "nil"));
    appendStringInfo(str,buf);
    sprintf(buf, " :pages %u", node->pages);
    appendStringInfo(str,buf);
    sprintf(buf, " :tuples %u", node->tuples);
    appendStringInfo(str,buf);
    sprintf(buf, " :size %u", node->size);
    appendStringInfo(str,buf);
    sprintf(buf, " :width %u", node->width);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :targetlist ");
    appendStringInfo(str,buf);
    _outNode(str, node->targetlist);
    
    sprintf(buf, " :pathlist ");
    appendStringInfo(str,buf);
    _outNode(str, node->pathlist);
    
    /*
     *  Not sure if these are nodes or not.  They're declared as
     *  struct Path *.  Since i don't know, i'll just print the
     *  addresses for now.  This can be changed later, if necessary.
     */
    
    sprintf(buf, " :unorderedpath @ 0x%x", (int)(node->unorderedpath));
    appendStringInfo(str,buf);
    sprintf(buf, " :cheapestpath @ 0x%x", (int)(node->cheapestpath));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :pruneable %s", (node->pruneable ? "true" : "nil"));
    appendStringInfo(str,buf);
    
#if 0
    sprintf(buf, " :classlist ");
    appendStringInfo(str,buf);
    _outNode(str, node->classlist);
    
    sprintf(buf, " :indexkeys ");
    appendStringInfo(str,buf);
    _outNode(str, node->indexkeys);
    
    sprintf(buf, " :ordering ");
    appendStringInfo(str,buf);
    _outNode(str, node->ordering);
#endif    

    sprintf(buf, " :clauseinfo ");
    appendStringInfo(str,buf);
    _outNode(str, node->clauseinfo);
    
    sprintf(buf, " :joininfo ");
    appendStringInfo(str,buf);
    _outNode(str, node->joininfo);
    
    sprintf(buf, " :innerjoin ");
    appendStringInfo(str,buf);
    _outNode(str, node->innerjoin);
    
}

/*
 *  TargetEntry is a subclass of Node.
 */
static void
_outTargetEntry(StringInfo str, TargetEntry *node)
{
    char buf[500];
  
    sprintf(buf, "TLE");
    appendStringInfo(str,buf);
    sprintf(buf, " :resdom ");
    appendStringInfo(str,buf);
    _outNode(str, node->resdom);
  
    sprintf(buf, " :expr ");
    appendStringInfo(str,buf);
    if (node->expr) {
	_outNode(str, node->expr);
    }else {
	appendStringInfo(str, "nil");
    }
} 

static void
_outRangeTblEntry(StringInfo str, RangeTblEntry *node)
{
    char buf[500];
  
    sprintf(buf, "RTE");
    appendStringInfo(str,buf);

    sprintf(buf, " :relname \"%.*s\"", NAMEDATALEN,
	    ((node->relname) ? ((char *) node->relname) : "null"));
    appendStringInfo(str,buf);

    sprintf(buf, " :inh %d ", node->inh);
    appendStringInfo(str,buf);
  
    sprintf(buf, " :refname \"%.*s\"", NAMEDATALEN,
	    ((node->refname) ? ((char *) node->refname) : "null"));
    appendStringInfo(str,buf);

    sprintf(buf, " :relid %d ", node->relid);
    appendStringInfo(str,buf);
} 

/*
 *  Path is a subclass of Node.
 */
static void
_outPath(StringInfo str, Path *node)
{
    char buf[500];
    
    sprintf(buf, "PATH");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :pathtype %d", node->pathtype);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :cost %f", node->path_cost);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :keys ");
    appendStringInfo(str,buf);
    _outNode(str, node->keys);
    
}

/*
 *  IndexPath is a subclass of Path.
 */
static void
_outIndexPath(StringInfo str, IndexPath *node)
{
    char buf[500];
    
    sprintf(buf, "INDEXPATH");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :pathtype %d", node->path.pathtype);
    appendStringInfo(str,buf);
    
    /*	sprintf(buf, " :parent ");
	appendStringInfo(str,buf);
	_outNode(str, node->parent); */
    
    sprintf(buf, " :cost %f", node->path.path_cost);
    appendStringInfo(str,buf);
    
#if 0
    sprintf(buf, " :p_ordering ");
    appendStringInfo(str,buf);
    _outNode(str, node->path.p_ordering);
#endif    
    sprintf(buf, " :keys ");
    appendStringInfo(str,buf);
    _outNode(str, node->path.keys);
    
    sprintf(buf, " :indexid ");
    appendStringInfo(str,buf);
    _outIntList(str, node->indexid);
    
    sprintf(buf, " :indexqual ");
    appendStringInfo(str,buf);
    _outNode(str, node->indexqual);
    
}

/*
 *  JoinPath is a subclass of Path
 */
static void
_outJoinPath(StringInfo str, JoinPath *node)
{
    char buf[500];
    
    sprintf(buf, "JOINPATH");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :pathtype %d", node->path.pathtype);
    appendStringInfo(str,buf);
    
    /*	sprintf(buf, " :parent ");
	appendStringInfo(str,buf);
	_outNode(str, node->parent); */
    
    sprintf(buf, " :cost %f", node->path.path_cost);
    appendStringInfo(str,buf);
    
#if 0
    sprintf(buf, " :p_ordering ");
    appendStringInfo(str,buf);
    _outNode(str, node->path.p_ordering);
#endif    
    sprintf(buf, " :keys ");
    appendStringInfo(str,buf);
    _outNode(str, node->path.keys);
    
    sprintf(buf, " :pathclauseinfo ");
    appendStringInfo(str,buf);
    _outNode(str, node->pathclauseinfo);
    
    /*
     *  Not sure if these are nodes; they're declared as "struct path *".
     *  For now, i'll just print the addresses.
     */
    
    sprintf(buf, " :outerjoinpath @ 0x%x", (int)(node->outerjoinpath));
    appendStringInfo(str,buf);
    sprintf(buf, " :innerjoinpath @ 0x%x", (int)(node->innerjoinpath));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :outerjoincost %f", node->path.outerjoincost);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :joinid ");
    appendStringInfo(str,buf);
    _outIntList(str, node->path.joinid);
    
}

/*
 *  MergePath is a subclass of JoinPath.
 */
static void
_outMergePath(StringInfo str, MergePath *node)
{
    char buf[500];
    
    sprintf(buf, "MERGEPATH");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :pathtype %d", node->jpath.path.pathtype);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :cost %f", node->jpath.path.path_cost);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :keys ");
    appendStringInfo(str,buf);
    _outNode(str, node->jpath.path.keys);
    
    sprintf(buf, " :pathclauseinfo ");
    appendStringInfo(str,buf);
    _outNode(str, node->jpath.pathclauseinfo);
    
    /*
     *  Not sure if these are nodes; they're declared as "struct path *".
     *  For now, i'll just print the addresses.
     */
    
    sprintf(buf, " :outerjoinpath @ 0x%x", (int)(node->jpath.outerjoinpath));
    appendStringInfo(str,buf);
    sprintf(buf, " :innerjoinpath @ 0x%x", (int)(node->jpath.innerjoinpath));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :outerjoincost %f", node->jpath.path.outerjoincost);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :joinid ");
    appendStringInfo(str,buf);
    _outIntList(str, node->jpath.path.joinid);
    
    sprintf(buf, " :path_mergeclauses ");
    appendStringInfo(str,buf);
    _outNode(str, node->path_mergeclauses);
    
    sprintf(buf, " :outersortkeys ");
    appendStringInfo(str,buf);
    _outNode(str, node->outersortkeys);
    
    sprintf(buf, " :innersortkeys ");
    appendStringInfo(str,buf);
    _outNode(str, node->innersortkeys);
    
}

/*
 *  HashPath is a subclass of JoinPath.
 */
static void
_outHashPath(StringInfo str, HashPath *node)
{
    char buf[500];
    
    sprintf(buf, "HASHPATH");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :pathtype %d", node->jpath.path.pathtype);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :cost %f", node->jpath.path.path_cost);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :keys ");
    appendStringInfo(str,buf);
    _outNode(str, node->jpath.path.keys);
    
    sprintf(buf, " :pathclauseinfo ");
    appendStringInfo(str,buf);
    _outNode(str, node->jpath.pathclauseinfo);
    
    /*
     *  Not sure if these are nodes; they're declared as "struct path *".
     *  For now, i'll just print the addresses.
     */
    
    sprintf(buf, " :outerjoinpath @ 0x%x", (int) (node->jpath.outerjoinpath));
    appendStringInfo(str,buf);
    sprintf(buf, " :innerjoinpath @ 0x%x", (int) (node->jpath.innerjoinpath));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :outerjoincost %f", node->jpath.path.outerjoincost);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :joinid ");
    appendStringInfo(str,buf);
    _outIntList(str, node->jpath.path.joinid);
    
    sprintf(buf, " :path_hashclauses ");
    appendStringInfo(str,buf);
    _outNode(str, node->path_hashclauses);
    
    sprintf(buf, " :outerhashkeys ");
    appendStringInfo(str,buf);
    _outNode(str, node->outerhashkeys);
    
    sprintf(buf, " :innerhashkeys ");
    appendStringInfo(str,buf);
    _outNode(str, node->innerhashkeys);
    
}

/*
 *  OrderKey is a subclass of Node.
 */
static void
_outOrderKey(StringInfo str, OrderKey *node)
{
    char buf[500];
    
    sprintf(buf, "ORDERKEY");
    appendStringInfo(str,buf);
    sprintf(buf, " :attribute_number %d", node->attribute_number);
    appendStringInfo(str,buf);
    sprintf(buf, " :array_index %d", node->array_index);
    appendStringInfo(str,buf);
    
}

/*
 *  JoinKey is a subclass of Node.
 */
static void
_outJoinKey(StringInfo str, JoinKey *node)
{
    char buf[500];
    
    sprintf(buf, "JOINKEY");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :outer ");
    appendStringInfo(str,buf);
    _outNode(str, node->outer);
    
    sprintf(buf, " :inner ");
    appendStringInfo(str,buf);
    _outNode(str, node->inner);
    
}

/*
 *  MergeOrder is a subclass of Node.
 */
static void
_outMergeOrder(StringInfo str, MergeOrder *node)
{
    char buf[500];
    
    sprintf(buf, "MERGEORDER");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :join_operator %d", node->join_operator);
    appendStringInfo(str,buf);
    sprintf(buf, " :left_operator %d", node->left_operator);
    appendStringInfo(str,buf);
    sprintf(buf, " :right_operator %d", node->right_operator);
    appendStringInfo(str,buf);
    sprintf(buf, " :left_type %d", node->left_type);
    appendStringInfo(str,buf);
    sprintf(buf, " :right_type %d", node->right_type);
    appendStringInfo(str,buf);
    
}

/*
 *  CInfo is a subclass of Node.
 */
static void
_outCInfo(StringInfo str, CInfo *node)
{
    char buf[500];
    
    sprintf(buf, "CINFO");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :clause ");
    appendStringInfo(str,buf);
    _outNode(str, node->clause);
    
    sprintf(buf, " :selectivity %f", node->selectivity);
    appendStringInfo(str,buf);
    sprintf(buf, " :notclause %s", (node->notclause ? "true" : "nil"));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :indexids ");
    appendStringInfo(str,buf);
    _outNode(str, node->indexids);
    
    sprintf(buf, " :mergesortorder ");
    appendStringInfo(str,buf);
    _outNode(str, node->mergesortorder);
    
    sprintf(buf, " :hashjoinoperator %d", node->hashjoinoperator);
    appendStringInfo(str,buf);
    
}

/*
 *  JoinMethod is a subclass of Node.
 */
static void
_outJoinMethod(StringInfo str, JoinMethod *node)
{
    char buf[500];
    
    sprintf(buf, "JOINMETHOD");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :jmkeys ");
    appendStringInfo(str,buf);
    _outNode(str, node->jmkeys);
    
    sprintf(buf, " :clauses ");
    appendStringInfo(str,buf);
    _outNode(str, node->clauses);
    
    
}

/*
 * HInfo is a subclass of JoinMethod.
 */
static void
_outHInfo(StringInfo str, HInfo *node)
{
    char buf[500];
    
    sprintf(buf, "HASHINFO");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :hashop ");
    appendStringInfo(str,buf);
    sprintf(buf, "%d",node->hashop);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :jmkeys ");
    appendStringInfo(str,buf);
    _outNode(str, node->jmethod.jmkeys);
    
    sprintf(buf, " :clauses ");
    appendStringInfo(str,buf);
    _outNode(str, node->jmethod.clauses);
    
}

/*
 *  JInfo is a subclass of Node.
 */
static void
_outJInfo(StringInfo str, JInfo *node)
{
    char buf[500];
    
    sprintf(buf, "JINFO");
    appendStringInfo(str,buf);
    
    sprintf(buf, " :otherrels ");
    appendStringInfo(str,buf);
    _outIntList(str, node->otherrels);
    
    sprintf(buf, " :jinfoclauseinfo ");
    appendStringInfo(str,buf);
    _outNode(str, node->jinfoclauseinfo);
    
    sprintf(buf, " :mergesortable %s",
	    (node->mergesortable ? "true" : "nil"));
    appendStringInfo(str,buf);
    sprintf(buf, " :hashjoinable %s",
	    (node->hashjoinable ? "true" : "nil"));
    appendStringInfo(str,buf);
    
}

/*
 * Print the value of a Datum given its type.
 */
static void
_outDatum(StringInfo str, Datum value, Oid type)
{
    char buf[500];
    Size length, typeLength;
    bool byValue;
    int i;
    char *s;
    
    /*
     * find some information about the type and the "real" length
     * of the datum.
     */
    byValue = get_typbyval(type);
    typeLength = get_typlen(type);
    length = datumGetSize(value, type, byValue, typeLength);
    
    if (byValue) {
	s = (char *) (&value);
	sprintf(buf, " %d [ ", length);
	appendStringInfo(str,buf);
	for (i=0; i<sizeof(Datum); i++) {
	    sprintf(buf, "%d ", (int) (s[i]) );
	    appendStringInfo(str,buf);
	}
	sprintf(buf, "] ");
	appendStringInfo(str,buf);
    } else { /* !byValue */
	s = (char *) DatumGetPointer(value);
	if (!PointerIsValid(s)) {
	    sprintf(buf, " 0 [ ] ");
	    appendStringInfo(str,buf);
	} else {
	    /*
	     * length is unsigned - very bad to do < comparison to -1 without
	     * casting it to int first!! -mer 8 Jan 1991
	     */
	    if (((int)length) <= -1) {
		length = VARSIZE(s);
	    }
	    sprintf(buf, " %d [ ", length);
	    appendStringInfo(str,buf);
	    for (i=0; i<length; i++) {
		sprintf(buf, "%d ", (int) (s[i]) );
		appendStringInfo(str,buf);
	    }
	    sprintf(buf, "] ");
	    appendStringInfo(str,buf);
	}
    }
    
}

static void
_outIter(StringInfo str, Iter *node)
{
    appendStringInfo(str,"ITER");
    
    appendStringInfo(str," :iterexpr ");
    _outNode(str, node->iterexpr);
}

static void
_outStream(StringInfo str, Stream *node)
{
    char buf[500];
    
    appendStringInfo(str,"STREAM");
    
    sprintf(buf, " :pathptr @ 0x%x", (int)(node->pathptr));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :cinfo @ 0x%x", (int)(node->cinfo));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :clausetype %d", (int)(node->clausetype));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :upstream @ 0x%x", (int)(node->upstream));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :downstream @ 0x%x", (int)(node->downstream));
    appendStringInfo(str,buf);
    
    sprintf(buf, " :groupup %d", node->groupup);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :groupcost %f", node->groupcost);
    appendStringInfo(str,buf);
    
    sprintf(buf, " :groupsel %f", node->groupsel);
    appendStringInfo(str,buf);
}    

static void
_outValue(StringInfo str, Value *value)
{
    char buf[500];
	     
    switch(value->type) {
    case T_String:
	sprintf(buf, "\"%s\"", value->val.str);
	appendStringInfo(str, buf);
	break;
    case T_Integer:
	sprintf(buf, "%ld", value->val.ival);
	appendStringInfo(str, buf);
	break;
    case T_Float:
	sprintf(buf, "%f", value->val.dval);
	appendStringInfo(str, buf);
	break;
    default:
	break;
    }
    return;
}

/*
 * _outNode -
 *    converts a Node into ascii string and append it to 'str'
 */
static void
_outNode(StringInfo str, void *obj)
{
    if (obj==NULL) {
	appendStringInfo(str, "nil");
	return;
    }

    if (nodeTag(obj)==T_List) {
	List *l;
	appendStringInfo(str, "(");
	foreach(l, (List*)obj) {
	    _outNode(str, lfirst(l));
	    if (lnext(l))
		appendStringInfo(str, " ");
	}
	appendStringInfo(str, ")");
    }else {
	appendStringInfo(str, "{");
	switch(nodeTag(obj)) {
	case T_Query:
	    _outQuery(str, obj);
	    break;
	case T_Plan:
	    _outPlan(str, obj);
	    break;
	case T_Result:
	    _outResult(str, obj);
	    break;
	case T_Existential:
	    _outExistential(str, obj);
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
	case T_Temp:
	    _outTemp(str, obj);
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
	case T_Hash:
	    _outHash(str, obj);
	    break;
	case T_Tee:
	    _outTee(str, obj);
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
	case T_Aggreg:
	    _outAggreg(str, obj);
	    break;
	case T_Array:
	    _outArray(str, obj);
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
	case T_EState:
	    _outEState(str, obj);
	    break;
	case T_Rel:
	    _outRel(str, obj);
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
	    _outIndexPath (str, obj);
	    break;
	case T_JoinPath:
	    _outJoinPath(str, obj);
	    break;
	case T_MergePath:
	    _outMergePath(str, obj);
	    break;
	case T_HashPath:
	    _outHashPath(str, obj);
	    break;
	case T_OrderKey:
	    _outOrderKey(str, obj);
	    break;
	case T_JoinKey:
	    _outJoinKey(str, obj);
	    break;
	case T_MergeOrder:
	    _outMergeOrder(str, obj);
	    break;
	case T_CInfo:
	    _outCInfo(str, obj);
	    break;
	case T_JoinMethod:
	    _outJoinMethod(str, obj);
	    break;
	case T_HInfo:
	    _outHInfo(str, obj);
	    break;
	case T_JInfo:
	    _outJInfo(str, obj);
	    break;
	case T_Iter:
	    _outIter(str, obj);
	    break;
	case T_Stream:
	    _outStream(str, obj);
	    break;
	case T_Integer: case T_String: case T_Float:
	    _outValue(str, obj);
	    break;
	default:
	    elog(NOTICE, "_outNode: don't know how to print type %d",
		 nodeTag(obj));
	    break;
	}
	appendStringInfo(str, "}");
    }
    return;
}

/*
 * nodeToString -
 *     returns the ascii representation of the Node
 */
char *
nodeToString(void *obj)
{
    StringInfo str;
    char *s;
    
    if (obj==NULL)
	return "";
    Assert(obj!=NULL);
    str = makeStringInfo();
    _outNode(str, obj);
    s = str->data;
    pfree(str);

    return s;
}
