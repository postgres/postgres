/*-------------------------------------------------------------------------
 *
 * analyze.c--
 *    transform the parse tree into a query tree
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/parser/analyze.c,v 1.8 1996/10/30 02:01:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "postgres.h"
#include "nodes/nodes.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"
#include "parse.h"		/* for AND, OR, etc. */
#include "catalog/pg_type.h"	/* for INT4OID, etc. */
#include "utils/elog.h"
#include "utils/builtins.h"	/* namecmp(), textout() */
#include "utils/lsyscache.h"
#include "utils/palloc.h"
#include "utils/mcxt.h"
#include "parser/parse_query.h"
#include "parser/parse_state.h"
#include "nodes/makefuncs.h"	/* for makeResdom(), etc. */
#include "nodes/nodeFuncs.h"

#include "optimizer/clauses.h"
#include "access/heapam.h"

/* convert the parse tree into a query tree */
static Query *transformStmt(ParseState *pstate, Node *stmt);

static Query *transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt);
static Query *transformInsertStmt(ParseState *pstate, AppendStmt *stmt);
static Query *transformIndexStmt(ParseState *pstate, IndexStmt *stmt);
static Query *transformExtendStmt(ParseState *pstate, ExtendStmt *stmt);
static Query *transformRuleStmt(ParseState *query, RuleStmt *stmt);
static Query *transformSelectStmt(ParseState *pstate, RetrieveStmt *stmt);
static Query *transformUpdateStmt(ParseState *pstate, ReplaceStmt *stmt);
static Query *transformCursorStmt(ParseState *pstate, CursorStmt *stmt);
static Node *handleNestedDots(ParseState *pstate, Attr *attr, int *curr_resno);

static Node *transformExpr(ParseState *pstate, Node *expr);

static void makeRangeTable(ParseState *pstate, char *relname, List *frmList);
static List *expandAllTables(ParseState *pstate);
static char *figureColname(Node *expr, Node *resval);
static List *makeTargetNames(ParseState *pstate, List *cols);
static List *transformTargetList(ParseState *pstate, List *targetlist);
static TargetEntry *make_targetlist_expr(ParseState *pstate,
					 char *colname, Node *expr,
					 List *arrayRef);
static Node *transformWhereClause(ParseState *pstate, Node *a_expr);
static List *transformGroupClause(ParseState *pstate, List *grouplist);
static List *transformSortClause(ParseState *pstate,
				 List *orderlist, List *targetlist,
				 char* uniqueFlag);

static void parseFromClause(ParseState *pstate, List *frmList);
static Node *ParseFunc(ParseState *pstate, char *funcname, 
		       List *fargs, int *curr_resno);
static List *setup_tlist(char *attname, Oid relid);
static List *setup_base_tlist(Oid typeid);
static void make_arguments(int nargs, List *fargs, Oid *input_typeids,
							Oid *function_typeids);
static void AddAggToParseState(ParseState *pstate, Aggreg *aggreg);
static void finalizeAggregates(ParseState *pstate, Query *qry);
static void parseCheckAggregates(ParseState *pstate, Query *qry);

/*****************************************************************************
 *
 *****************************************************************************/

/*
 * makeParseState() -- 
 *    allocate and initialize a new ParseState.
 *  the CALLERS is responsible for freeing the ParseState* returned
 *
 */

ParseState* 
makeParseState() {
    ParseState *pstate;

    pstate = malloc(sizeof(ParseState));
    pstate->p_last_resno = 1;
    pstate->p_rtable = NIL;
    pstate->p_numAgg = 0;
    pstate->p_aggs = NIL;
    pstate->p_is_insert = false;
    pstate->p_insert_columns = NIL;
    pstate->p_is_update = false;
    pstate->p_is_rule = false;
    pstate->p_target_relation = NULL;
    pstate->p_target_rangetblentry = NULL;

    return (pstate);
}

/*
 * parse_analyze -
 *    analyze a list of parse trees and transform them if necessary.
 *
 * Returns a list of transformed parse trees. Optimizable statements are
 * all transformed to Query while the rest stays the same.
 *
 * CALLER is responsible for freeing the QueryTreeList* returned
 */
QueryTreeList *
parse_analyze(List *pl)
{
    QueryTreeList *result;
    ParseState *pstate;
    int i = 0;

    result = malloc(sizeof(QueryTreeList));
    result->len = length(pl);
    result->qtrees = (Query**)malloc(result->len * sizeof(Query*));

    while(pl!=NIL) {
	pstate = makeParseState();
	result->qtrees[i++] = transformStmt(pstate, lfirst(pl));
	pl = lnext(pl);
	if (pstate->p_target_relation != NULL)
	    heap_close(pstate->p_target_relation);
	free(pstate);
    }

    return result;
}

/*
 * transformStmt -
 *    transform a Parse tree. If it is an optimizable statement, turn it
 *    into a Query tree.
 */
static Query *
transformStmt(ParseState* pstate, Node *parseTree)
{
    Query* result = NULL;

    switch(nodeTag(parseTree)) {
      /*------------------------
       *  Non-optimizable statements
       *------------------------
       */
    case T_IndexStmt:
      result = transformIndexStmt(pstate, (IndexStmt *)parseTree);
      break;

    case T_ExtendStmt:
      result = transformExtendStmt(pstate, (ExtendStmt *)parseTree);
      break;

    case T_RuleStmt:
      result = transformRuleStmt(pstate, (RuleStmt *)parseTree);
      break;

    case T_ViewStmt:
      {
	ViewStmt *n = (ViewStmt *)parseTree;
	n->query = (Query *)transformStmt(pstate, (Node*)n->query);
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node*)n;
      }
      break;

    case T_VacuumStmt:
      {
	MemoryContext oldcontext;
	/* make sure that this Query is allocated in TopMemory context
	   because vacuum spans transactions and we don't want to lose
	   the vacuum Query due to end-of-transaction free'ing*/
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node*)parseTree;
	MemoryContextSwitchTo(oldcontext);
	break;
	    
      }
  case T_ExplainStmt:
      {
	  ExplainStmt *n = (ExplainStmt *)parseTree;
	  result = makeNode(Query);
	  result->commandType = CMD_UTILITY;
	  n->query = transformStmt(pstate, (Node*)n->query);
	  result->utilityStmt = (Node*)parseTree;
      }
      break;
      
      /*------------------------
       *  Optimizable statements
       *------------------------
       */
    case T_AppendStmt:
      result = transformInsertStmt(pstate, (AppendStmt *)parseTree);
      break;

    case T_DeleteStmt:
      result = transformDeleteStmt(pstate, (DeleteStmt *)parseTree);
      break;

    case T_ReplaceStmt:
      result = transformUpdateStmt(pstate, (ReplaceStmt *)parseTree);
      break;

    case T_CursorStmt:
      result = transformCursorStmt(pstate, (CursorStmt *)parseTree);
      break;

    case T_RetrieveStmt:
      result = transformSelectStmt(pstate, (RetrieveStmt *)parseTree);
      break;

    default:
      /*
       * other statments don't require any transformation-- just
       * return the original parsetree 
       */
      result = makeNode(Query);
      result->commandType = CMD_UTILITY;
      result->utilityStmt = (Node*)parseTree;
      break;
    }
    return result;
}

/*
 * transformDeleteStmt -
 *    transforms a Delete Statement
 */
static Query *
transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
    Query *qry = makeNode(Query);

    qry->commandType = CMD_DELETE;

    /* set up a range table */
    makeRangeTable(pstate, stmt->relname, NULL);
    
    qry->uniqueFlag = NULL; 

    /* fix where clause */
    qry->qual = transformWhereClause(pstate, stmt->whereClause);

    qry->rtable = pstate->p_rtable;
    qry->resultRelation = refnameRangeTablePosn(pstate->p_rtable, stmt->relname);

    /* make sure we don't have aggregates in the where clause */
    if (pstate->p_numAgg > 0)
	parseCheckAggregates(pstate, qry);

    return (Query *)qry;
}

/*
 * transformInsertStmt -
 *    transform an Insert Statement
 */
static Query *
transformInsertStmt(ParseState *pstate, AppendStmt *stmt)
{
    Query *qry = makeNode(Query);	/* make a new query tree */
    List *targetlist;

    qry->commandType = CMD_INSERT;
    pstate->p_is_insert = true;

    /* set up a range table */
    makeRangeTable(pstate, stmt->relname, stmt->fromClause);

    qry->uniqueFlag = NULL; 

    /* fix the target list */
    pstate->p_insert_columns = makeTargetNames(pstate, stmt->cols);

    qry->targetList = transformTargetList(pstate, stmt->targetList);

    /* fix where clause */
    qry->qual = transformWhereClause(pstate, stmt->whereClause);

    /* now the range table will not change */
    qry->rtable = pstate->p_rtable;
    qry->resultRelation = refnameRangeTablePosn(pstate->p_rtable, stmt->relname);

    if (pstate->p_numAgg > 0)
	finalizeAggregates(pstate, qry);

    return (Query *)qry;
}

/*
 * transformIndexStmt -
 *    transforms the qualification of the index statement
 */
static Query *
transformIndexStmt(ParseState *pstate, IndexStmt *stmt)
{
    Query* q;

    q = makeNode(Query);
    q->commandType = CMD_UTILITY;
    
    /* take care of the where clause */
    stmt->whereClause = transformWhereClause(pstate,stmt->whereClause);
    stmt->rangetable = pstate->p_rtable;

    q->utilityStmt = (Node*)stmt;

    return q;
}

/*
 * transformExtendStmt -
 *    transform the qualifications of the Extend Index Statement
 *
 */
static Query *
transformExtendStmt(ParseState *pstate, ExtendStmt *stmt)
{
    Query  *q;

    q = makeNode(Query);
    q->commandType = CMD_UTILITY;

    /* take care of the where clause */
    stmt->whereClause = transformWhereClause(pstate,stmt->whereClause);
    stmt->rangetable = pstate->p_rtable;

    q->utilityStmt = (Node*)stmt;
    return q;
}

/*
 * transformRuleStmt -
 *    transform a Create Rule Statement. The actions is a list of parse
 *    trees which is transformed into a list of query trees.
 */
static Query *
transformRuleStmt(ParseState *pstate, RuleStmt *stmt)
{
    Query *q;
    List *actions;
    
    q = makeNode(Query);
    q->commandType = CMD_UTILITY;

    actions = stmt->actions;
    /*
     * transform each statment, like parse_analyze()
     */
    while (actions != NIL) {
	/*
	 * NOTE: 'CURRENT' must always have a varno equal to 1 and 'NEW' 
	 * equal to 2.
	 */
	addRangeTableEntry(pstate, stmt->object->relname, "*CURRENT*",
					FALSE, FALSE, NULL);
	addRangeTableEntry(pstate, stmt->object->relname, "*NEW*",
					FALSE, FALSE, NULL);

	pstate->p_last_resno = 1;
	pstate->p_is_rule = true;	/* for expand all */
	pstate->p_numAgg = 0;
	pstate->p_aggs = NULL;
	
	lfirst(actions) =  transformStmt(pstate, lfirst(actions));
	actions = lnext(actions);
    }

    /* take care of the where clause */
    stmt->whereClause = transformWhereClause(pstate,stmt->whereClause);

    q->utilityStmt = (Node*)stmt;
    return q;
}


/*
 * transformSelectStmt -
 *    transforms a Select Statement
 *
 */
static Query *
transformSelectStmt(ParseState *pstate, RetrieveStmt *stmt)
{
    Query *qry = makeNode(Query);

    qry->commandType = CMD_SELECT;

    /* set up a range table */
    makeRangeTable(pstate, NULL, stmt->fromClause);

    qry->uniqueFlag = stmt->unique;

    qry->into	  = stmt->into;
    qry->isPortal = FALSE;

    /* fix the target list */
    qry->targetList = transformTargetList(pstate, stmt->targetList);

    /* fix where clause */
    qry->qual = transformWhereClause(pstate,stmt->whereClause);

    /* fix order clause */
    qry->sortClause = transformSortClause(pstate,
					  stmt->orderClause,
					  qry->targetList,
					  qry->uniqueFlag);

    /* fix group by clause */
    qry->groupClause = transformGroupClause(pstate,
					    stmt->groupClause);
    qry->rtable = pstate->p_rtable;

    if (pstate->p_numAgg > 0)
	finalizeAggregates(pstate, qry);
	
    return (Query *)qry;
}

/*
 * transformUpdateStmt -
 *    transforms an update statement
 *
 */
static Query *
transformUpdateStmt(ParseState *pstate, ReplaceStmt *stmt)
{
    Query *qry = makeNode(Query);

    qry->commandType = CMD_UPDATE;
    pstate->p_is_update = true;
    /*
     * the FROM clause is non-standard SQL syntax. We used to be able to
     * do this with REPLACE in POSTQUEL so we keep the feature.
     */
    makeRangeTable(pstate, stmt->relname, stmt->fromClause); 

    /* fix the target list */
    qry->targetList = transformTargetList(pstate, stmt->targetList);

    /* fix where clause */
    qry->qual = transformWhereClause(pstate,stmt->whereClause);

    qry->rtable = pstate->p_rtable;
    qry->resultRelation = refnameRangeTablePosn(pstate->p_rtable, stmt->relname);

    /* make sure we don't have aggregates in the where clause */
    if (pstate->p_numAgg > 0)
	parseCheckAggregates(pstate, qry);

    return (Query *)qry;
}

/*
 * transformCursorStmt -
 *    transform a Create Cursor Statement
 *
 */
static Query *
transformCursorStmt(ParseState *pstate, CursorStmt *stmt)
{
    Query *qry = makeNode(Query);

    /*
     * in the old days, a cursor statement is a 'retrieve into portal';
     * If you change the following, make sure you also go through the code
     * in various places that tests the kind of operation.
     */
    qry->commandType = CMD_SELECT;

    /* set up a range table */
    makeRangeTable(pstate, NULL, stmt->fromClause);

    qry->uniqueFlag = stmt->unique;

    qry->into	  = stmt->portalname;
    qry->isPortal = TRUE;
    qry->isBinary = stmt->binary;	/* internal portal */

    /* fix the target list */
    qry->targetList = transformTargetList(pstate, stmt->targetList);

    /* fix where clause */
    qry->qual = transformWhereClause(pstate,stmt->whereClause);

    /* fix order clause */
    qry->sortClause = transformSortClause(pstate,
					  stmt->orderClause,
					  qry->targetList,
					  qry->uniqueFlag);
    /* fix group by clause */
    qry->groupClause = transformGroupClause(pstate,
                                          stmt->groupClause);

    qry->rtable = pstate->p_rtable;

    if (pstate->p_numAgg > 0)
	finalizeAggregates(pstate, qry);

    return (Query *)qry;
}

/*****************************************************************************
 *
 * Transform Exprs, Aggs, etc.
 *
 *****************************************************************************/

/*
 * transformExpr -
 *    analyze and transform expressions. Type checking and type casting is
 *    done here. The optimizer and the executor cannot handle the original
 *    (raw) expressions collected by the parse tree. Hence the transformation
 *    here.
 */
static Node *
transformExpr(ParseState *pstate, Node *expr)
{
    Node *result;

    if (expr==NULL)
	return NULL;
    
    switch(nodeTag(expr)) {
    case T_Attr: {
	Attr *att = (Attr *)expr;
	Node *temp;

	/* what if att.attrs == "*"?? */
	temp = handleNestedDots(pstate, att, &pstate->p_last_resno);
	if (att->indirection != NIL) {
	    List *idx = att->indirection;
	    while(idx!=NIL) {
		A_Indices *ai = (A_Indices *)lfirst(idx);
		Node *lexpr=NULL, *uexpr;
		uexpr = transformExpr(pstate, ai->uidx);  /* must exists */
		if (exprType(uexpr) != INT4OID)
		    elog(WARN, "array index expressions must be int4's");
		if (ai->lidx != NULL) {
		    lexpr = transformExpr(pstate, ai->lidx);
		    if (exprType(lexpr) != INT4OID)
			elog(WARN, "array index expressions must be int4's");
		}
#if 0
		pfree(ai->uidx);
		if (ai->lidx!=NULL) pfree(ai->lidx);
#endif
		ai->lidx = lexpr;
		ai->uidx = uexpr;
		/* note we reuse the list of indices, make sure we don't free
		   them! Otherwise, make a new list here */
		idx = lnext(idx);
	    }
	    result = (Node*)make_array_ref(temp, att->indirection);
	}else {
	    result = temp;
	}
	break;
    }
    case T_A_Const: {
	A_Const *con= (A_Const *)expr;
	Value *val = &con->val;
	if (con->typename != NULL) {
	    result = parser_typecast(val, con->typename, -1);
	}else {
	    result = (Node *)make_const(val);
	}
	break;
    }
    case T_ParamNo: {
	ParamNo *pno = (ParamNo *)expr;
	Oid toid;
	int paramno;
	Param *param;

	paramno = pno->number;
	toid = param_type(paramno);
	if (!OidIsValid(toid)) {
	    elog(WARN, "Parameter '$%d' is out of range",
		 paramno);
	}
	param = makeNode(Param);
	param->paramkind = PARAM_NUM;
	param->paramid = (AttrNumber) paramno;
	param->paramname = "<unnamed>";
	param->paramtype = (Oid)toid;
	param->param_tlist = (List*) NULL;

	result = (Node *)param;
	break;
    }
    case T_A_Expr: {
	A_Expr *a = (A_Expr *)expr;

	switch(a->oper) {
	case OP:
	    {
		Node *lexpr = transformExpr(pstate, a->lexpr);
		Node *rexpr = transformExpr(pstate, a->rexpr);
		result = (Node *)make_op(a->opname, lexpr, rexpr);
	    }
	    break;
	case ISNULL:
	    {
		Node *lexpr = transformExpr(pstate, a->lexpr);
		result = ParseFunc(pstate, 
				   "NullValue", lcons(lexpr, NIL),
				   &pstate->p_last_resno);
	    }
	    break;
	case NOTNULL:
	    {
		Node *lexpr = transformExpr(pstate, a->lexpr);
		result = ParseFunc(pstate,
				   "NonNullValue", lcons(lexpr, NIL),
				   &pstate->p_last_resno);
	    }
	    break;
	case AND:
	    {
		Expr *expr = makeNode(Expr);
		Node *lexpr = transformExpr(pstate, a->lexpr);
		Node *rexpr = transformExpr(pstate, a->rexpr);
		if (exprType(lexpr) != BOOLOID)
		    elog(WARN,
			 "left-hand side of AND is type '%s', not bool",
			 tname(get_id_type(exprType(lexpr))));
		if (exprType(rexpr) != BOOLOID)
		    elog(WARN,
			 "right-hand side of AND is type '%s', not bool",
			 tname(get_id_type(exprType(rexpr))));
		expr->typeOid = BOOLOID;
		expr->opType = AND_EXPR;
		expr->args = makeList(lexpr, rexpr, -1);
		result = (Node *)expr;
	    }
	    break;
	case OR:
	    {
		Expr *expr = makeNode(Expr);
		Node *lexpr = transformExpr(pstate, a->lexpr);
		Node *rexpr = transformExpr(pstate, a->rexpr);
		if (exprType(lexpr) != BOOLOID)
		    elog(WARN,
			 "left-hand side of OR is type '%s', not bool",
			 tname(get_id_type(exprType(lexpr))));
		if (exprType(rexpr) != BOOLOID)
		    elog(WARN,
			 "right-hand side of OR is type '%s', not bool",
			 tname(get_id_type(exprType(rexpr))));
		expr->typeOid = BOOLOID;
		expr->opType = OR_EXPR;
		expr->args = makeList(lexpr, rexpr, -1);
		result = (Node *)expr;
	    }
	    break;
	case NOT:
	    {
		Expr *expr = makeNode(Expr);
		Node *rexpr = transformExpr(pstate, a->rexpr);
		if (exprType(rexpr) != BOOLOID)
		    elog(WARN,
			 "argument to NOT is type '%s', not bool",
			 tname(get_id_type(exprType(rexpr))));
		expr->typeOid = BOOLOID;
		expr->opType = NOT_EXPR;
		expr->args = makeList(rexpr, -1);
		result = (Node *)expr;
	    }
	    break;
	}
	break;
    }
    case T_Ident: {
	Ident *ident = (Ident*)expr;
    	RangeTblEntry *rte;

		/* could be a column name or a relation_name */
	if (refnameRangeTableEntry(pstate->p_rtable, ident->name) != NULL) {
		ident->isRel = TRUE;
		result = (Node*)ident;
	}
	else if ((rte = colnameRangeTableEntry(pstate, ident->name)) != NULL)
 	{
	    Attr *att = makeNode(Attr);

	    att->relname = rte->refname;
	    att->attrs = lcons(makeString(ident->name), NIL);
	    result =
		(Node*)handleNestedDots(pstate, att, &pstate->p_last_resno);
	} else
	    elog(WARN, "attribute \"%s\" not found", ident->name);
	break;
    }
    case T_FuncCall: {
	FuncCall *fn = (FuncCall *)expr;
	List *args;

	/* transform the list of arguments */
	foreach(args, fn->args)
	    lfirst(args) = transformExpr(pstate, (Node*)lfirst(args));
	result = ParseFunc(pstate,
			   fn->funcname, fn->args, &pstate->p_last_resno);
	break;
    }
    default:
	/* should not reach here */
	elog(WARN, "transformExpr: does not know how to transform %d\n",
	     nodeTag(expr));
	break;
    }

    return result;
}

/*****************************************************************************
 *
 * From Clause
 *
 *****************************************************************************/

/*
 * parseFromClause -
 *    turns the table references specified in the from-clause into a
 *    range table. The range table may grow as we transform the expressions
 *    in the target list. (Note that this happens because in POSTQUEL, we
 *    allow references to relations not specified in the from-clause. We
 *    also allow that in our POST-SQL)
 *
 */
static void
parseFromClause(ParseState *pstate, List *frmList)
{
    List *fl;

    foreach(fl, frmList)
    {
	RangeVar *r = lfirst(fl);
	RelExpr	*baserel = r->relExpr;
	char *relname = baserel->relname;
	char *refname = r->name;
	RangeTblEntry *rte;
	
	if (refname==NULL)
	    refname = relname;

	/*
	 * marks this entry to indicate it comes from the FROM clause. In
	 * SQL, the target list can only refer to range variables specified
	 * in the from clause but we follow the more powerful POSTQUEL
	 * semantics and automatically generate the range variable if not
	 * specified. However there are times we need to know whether the
	 * entries are legitimate.
	 *
	 * eg. select * from foo f where f.x = 1; will generate wrong answer
	 *     if we expand * to foo.x.
	 */
	rte = addRangeTableEntry(pstate, relname, refname, baserel->inh, TRUE,
				  baserel->timeRange);
    }
}

/*
 * makeRangeTable -
 *    make a range table with the specified relation (optional) and the
 *    from-clause.
 */
static void
makeRangeTable(ParseState *pstate, char *relname, List *frmList)
{
    RangeTblEntry *rte;

    parseFromClause(pstate, frmList);

    if (relname == NULL)
	return;
    
    if (refnameRangeTablePosn(pstate->p_rtable, relname) < 1)
	rte = addRangeTableEntry(pstate, relname, relname, FALSE, FALSE, NULL);
    else
	rte = refnameRangeTableEntry(pstate->p_rtable, relname);

    pstate->p_target_rangetblentry = rte;
    Assert(pstate->p_target_relation == NULL);
    pstate->p_target_relation = heap_open(rte->relid);
    Assert(pstate->p_target_relation != NULL);
	/* will close relation later */
}

/*
 *  exprType -
 *    returns the Oid of the type of the expression. (Used for typechecking.)
 */
Oid
exprType(Node *expr)
{
    Oid type;
    
    switch(nodeTag(expr)) {
    case T_Func:
	type = ((Func*)expr)->functype;
	break;
    case T_Iter:
	type = ((Iter*)expr)->itertype;
	break;
    case T_Var:
	type = ((Var*)expr)->vartype;
	break;
    case T_Expr:
	type = ((Expr*)expr)->typeOid;
	break;
    case T_Const:
	type = ((Const*)expr)->consttype;
	break;
    case T_ArrayRef:
	type = ((ArrayRef*)expr)->refelemtype;
	break;
    case T_Aggreg:
	type = ((Aggreg*)expr)->aggtype;
	break;
    case T_Param:
	type = ((Param*)expr)->paramtype;
	break;
    case T_Ident:
	/* is this right? */
	type = UNKNOWNOID;
	break;
    default:
	elog(WARN, "exprType: don't know how to get type for %d node",
	     nodeTag(expr));
	break;
    }
    return type;
}

/*
 * expandAllTables -
 *    turns '*' (in the target list) into a list of attributes (of all
 *    relations in the range table)
 */
static List *
expandAllTables(ParseState *pstate)
{
    List *target= NIL;
    List *legit_rtable=NIL;
    List *rt, *rtable;

    rtable = pstate->p_rtable;
    if (pstate->p_is_rule) {
	/*
	 * skip first two entries, "*new*" and "*current*"
	 */
	rtable = lnext(lnext(pstate->p_rtable));
    }
    
    /* this should not happen */
    if (rtable==NULL) 
	elog(WARN, "cannot expand: null p_rtable");

    /* 
     * go through the range table and make a list of range table entries
     * which we will expand.
     */
    foreach(rt, rtable) {
	RangeTblEntry *rte = lfirst(rt);

	/*
	 * we only expand those specify in the from clause. (This will
	 * also prevent us from using the wrong table in inserts: eg. tenk2
	 * in "insert into tenk2 select * from tenk1;")
	 */
	if (!rte->inFromCl)
	    continue;
	legit_rtable = lappend(legit_rtable, rte);
    }

    foreach(rt, legit_rtable) {
	RangeTblEntry *rte = lfirst(rt);
	List *temp = target;
	
	if(temp == NIL )
	    target = expandAll(pstate, rte->relname, rte->refname,
							&pstate->p_last_resno);
	else {
	    while (temp != NIL && lnext(temp) != NIL)
		temp = lnext(temp);
	    lnext(temp) = expandAll(pstate, rte->relname, rte->refname,
							&pstate->p_last_resno);
	}
    }
    return target;
}


/*
 * figureColname -
 *    if the name of the resulting column is not specified in the target
 *    list, we have to guess.
 *
 */
static char *
figureColname(Node *expr, Node *resval)
{
    switch (nodeTag(expr)) {
    case T_Aggreg:
	return (char*) /* XXX */
	    ((Aggreg *)expr)->aggname;
    case T_Expr:
	if (((Expr*)expr)->opType == FUNC_EXPR) {
	    if (nodeTag(resval)==T_FuncCall)
		return ((FuncCall*)resval)->funcname;
	}
	break;
    default:
	break;
    }
	
    return "?column?";
}

/*****************************************************************************
 *
 * Target list
 *
 *****************************************************************************/

/*
 * makeTargetNames -
 *    generate a list of column names if not supplied or
 *    test supplied column names to make sure they are in target table
 *    (used exclusively for inserts)
 */
static List *
makeTargetNames(ParseState *pstate, List *cols)
{
    List *tl=NULL;

    	/* Generate ResTarget if not supplied */
    	
    if (cols == NIL) {
	int numcol;
	int i;
	AttributeTupleForm *attr = pstate->p_target_relation->rd_att->attrs;
	
	numcol = pstate->p_target_relation->rd_rel->relnatts;
	for(i=0; i < numcol; i++) {
	    Ident *id = makeNode(Ident);

	    id->name = palloc(NAMEDATALEN+1);
	    strncpy(id->name, attr[i]->attname.data, NAMEDATALEN);
	    id->name[NAMEDATALEN]='\0';
	    id->indirection = NIL;
	    id->isRel = false;
	    if (tl == NIL)
	        cols = tl = lcons(id, NIL);
	    else {
	        lnext(tl) = lcons(id,NIL);
	        tl = lnext(tl);
	    }
	}
    }
    else
        foreach(tl, cols)
			/* elog on failure */
    	 (void)varattno(pstate->p_target_relation,((Ident *)lfirst(tl))->name);

    return cols;
}

/*
 * transformTargetList -
 *    turns a list of ResTarget's into a list of TargetEntry's
 */
static List *
transformTargetList(ParseState *pstate, List *targetlist)
{
    List *p_target= NIL;
    List *tail_p_target = NIL;

    while(targetlist != NIL) {
	ResTarget *res= (ResTarget *)lfirst(targetlist);
	TargetEntry *tent = makeNode(TargetEntry);

	switch(nodeTag(res->val)) {
	case T_Ident: {
	    Node *expr;
	    Oid type_id;
	    int type_len;
	    char *identname;
	    char *resname;
	    
	    identname = ((Ident*)res->val)->name;
	    handleTargetColname(pstate, &res->name, NULL, res->name);
	    expr = transformExpr(pstate, (Node*)res->val);
	    type_id = exprType(expr);
	    type_len = tlen(get_id_type(type_id));
	    resname = (res->name) ? res->name : identname;
	    tent->resdom = makeResdom((AttrNumber)pstate->p_last_resno++,
				      (Oid)type_id,
				      (Size)type_len,
				      resname,
				      (Index)0,
				      (Oid)0,
				      0);
				      
	    tent->expr = expr;
	    break;
	}
	case T_ParamNo:
	case T_FuncCall:
	case T_A_Const:    
	case T_A_Expr: {
	    Node *expr = transformExpr(pstate, (Node *)res->val);

	    handleTargetColname(pstate, &res->name, NULL, NULL);
	    /* note indirection has not been transformed */
	    if (pstate->p_is_insert && res->indirection!=NIL) {
		/* this is an array assignment */
		char *val;
		char *str, *save_str;
		List *elt;
		int i = 0, ndims;
		int lindx[MAXDIM], uindx[MAXDIM];
		int resdomno;
		Relation rd;
		Value *constval;
		
		if (exprType(expr) != UNKNOWNOID ||
		    !IsA(expr,Const))
		    elog(WARN, "yyparse: string constant expected");

		val = (char *) textout((struct varlena *)
				       ((Const *)expr)->constvalue);
		str = save_str = (char*)palloc(strlen(val) + MAXDIM * 25 + 2);
		foreach(elt, res->indirection) {
		    A_Indices *aind = (A_Indices *)lfirst(elt);
		    aind->uidx = transformExpr(pstate, aind->uidx);
		    if (!IsA(aind->uidx,Const)) 
			elog(WARN,
			     "Array Index for Append should be a constant");
		    uindx[i] = ((Const *)aind->uidx)->constvalue;
		    if (aind->lidx!=NULL) {
			aind->lidx = transformExpr(pstate, aind->lidx);
			if (!IsA(aind->lidx,Const))
			    elog(WARN,
				"Array Index for Append should be a constant");
			lindx[i] = ((Const*)aind->lidx)->constvalue;
		    }else {
			lindx[i] = 1;
		    }
		    if (lindx[i] > uindx[i]) 
			elog(WARN, "yyparse: lower index cannot be greater than upper index");
		    sprintf(str, "[%d:%d]", lindx[i], uindx[i]);
		    str += strlen(str);
		    i++;
		}
		sprintf(str, "=%s", val);
		rd = pstate->p_target_relation;
		Assert(rd != NULL);
		resdomno = varattno(rd, res->name);
		ndims = att_attnelems(rd, resdomno);
		if (i != ndims)
		    elog(WARN, "yyparse: array dimensions do not match");
		constval = makeNode(Value);
		constval->type = T_String;
		constval->val.str = save_str;
		tent = make_targetlist_expr(pstate, res->name,
					    (Node*)make_const(constval),
					    NULL);
		pfree(save_str);
	    } else {
		char *colname= res->name;
		/* this is not an array assignment */
		if (colname==NULL) {
		    /* if you're wondering why this is here, look at
		     * the yacc grammar for why a name can be missing. -ay
		     */
		    colname = figureColname(expr, res->val);
		}
		if (res->indirection) {
		    List *ilist = res->indirection;
		    while (ilist!=NIL) {
			A_Indices *ind = lfirst(ilist);
			ind->lidx = transformExpr(pstate, ind->lidx);
			ind->uidx = transformExpr(pstate, ind->uidx);
			ilist = lnext(ilist);
		    }
		}
		res->name = colname;
		tent = make_targetlist_expr(pstate, res->name, expr, 
					    res->indirection);
	    }
	    break;
	}
	case T_Attr: {
	    Oid type_id;
	    int type_len;
	    Attr *att = (Attr *)res->val;
	    Node *result;
	    char *attrname;
	    char *resname;
	    Resdom *resnode;
	    List *attrs = att->attrs;

	    /*
	     * Target item is a single '*', expand all tables
	     * (eg. SELECT * FROM emp)
	     */
	    if (att->relname!=NULL && !strcmp(att->relname, "*")) {
		if(lnext(targetlist)!=NULL)
		    elog(WARN, "cannot expand target list *, ...");
		p_target = expandAllTables(pstate);

		/*
		 * skip rest of while loop
		 */
		targetlist = lnext(targetlist);
		continue;
	    }

	    /*
	     * Target item is relation.*, expand the table
	     * (eg. SELECT emp.*, dname FROM emp, dept)
	     */
	    attrname = strVal(lfirst(att->attrs));
	    if (att->attrs!=NIL && !strcmp(attrname,"*")) {
		/* tail_p_target is the target list we're building in the while
		 * loop. Make sure we fix it after appending more nodes.
		 */
		if (tail_p_target == NIL) {
		    p_target = tail_p_target = expandAll(pstate, att->relname,
					att->relname, &pstate->p_last_resno);
		} else {
		    lnext(tail_p_target) =
			expandAll(pstate, att->relname, att->relname,
							&pstate->p_last_resno);
		}
		while(lnext(tail_p_target)!=NIL)
			/* make sure we point to the last target entry */
		    tail_p_target = lnext(tail_p_target);
		/*
		 * skip the rest of the while loop
		 */
		targetlist = lnext(targetlist);
		continue; 
	    }


	    /*
	     * Target item is fully specified: ie. relation.attribute
	     */
	    result = handleNestedDots(pstate, att, &pstate->p_last_resno);
	    handleTargetColname(pstate, &res->name, att->relname, attrname);
	    if (att->indirection != NIL) {
		List *ilist = att->indirection;
		while (ilist!=NIL) {
		    A_Indices *ind = lfirst(ilist);
		    ind->lidx = transformExpr(pstate, ind->lidx);
		    ind->uidx = transformExpr(pstate, ind->uidx);
		    ilist = lnext(ilist);
		}
		result = (Node*)make_array_ref(result, att->indirection);
	    }
	    type_id = exprType(result);
	    type_len = tlen(get_id_type(type_id));
	    	/* move to last entry */
	    while(lnext(attrs)!=NIL)
		attrs=lnext(attrs);
	    resname = (res->name) ? res->name : strVal(lfirst(attrs));
	    resnode = makeResdom((AttrNumber)pstate->p_last_resno++,
				 (Oid)type_id,
				 (Size)type_len,
				 resname,
				 (Index)0,
				 (Oid)0,
				 0);
	    tent->resdom = resnode;
	    tent->expr = result;
	    break;
	}
	default:
	    /* internal error */
	    elog(WARN,
		 "internal error: do not know how to transform targetlist");
	    break;
	}

	if (p_target == NIL) {
	    p_target = tail_p_target = lcons(tent, NIL);
	}else {
	    lnext(tail_p_target) = lcons(tent, NIL);
	    tail_p_target = lnext(tail_p_target);
	}
	targetlist = lnext(targetlist);
    }

    return p_target;
}


/*
 * make_targetlist_expr -
 *    make a TargetEntry from an expression
 *
 * arrayRef is a list of transformed A_Indices
 */
static TargetEntry *
make_targetlist_expr(ParseState *pstate,
		     char *colname,
		     Node *expr,
		     List *arrayRef)
{
     int type_id, type_len, attrtype, attrlen;
     int resdomno;
     Relation rd;
     bool attrisset;
     TargetEntry *tent;
     Resdom *resnode;
     
     if (expr == NULL)
	 elog(WARN, "make_targetlist_expr: invalid use of NULL expression");

     type_id = exprType(expr);
#ifdef NULL_PATCH
     if (!type_id) {
	 type_len = 0;
     } else
#endif
     type_len = tlen(get_id_type(type_id));

     /* I have no idea what the following does! */
     /* It appears to process target columns that will be receiving results */
     if (pstate->p_is_insert||pstate->p_is_update) {
	  /*
	   * append or replace query -- 
	   * append, replace work only on one relation,
	   * so multiple occurence of same resdomno is bogus
	   */
	  rd = pstate->p_target_relation;
	  Assert(rd != NULL);
	  resdomno = varattno(rd,colname);
	  attrisset = varisset(rd,colname);
	  attrtype = att_typeid(rd,resdomno);
	  if ((arrayRef != NIL) && (lfirst(arrayRef) == NIL))
	       attrtype = GetArrayElementType(attrtype);
	  if (attrtype==BPCHAROID || attrtype==VARCHAROID) {
	      attrlen = rd->rd_att->attrs[resdomno-1]->attlen;
	  } else {
	      attrlen = tlen(get_id_type(attrtype));
	  }
#if 0
	  if(Input_is_string && Typecast_ok){
	       Datum val;
	       if (type_id == typeid(type("unknown"))){
		    val = (Datum)textout((struct varlena *)
					 ((Const)lnext(expr))->constvalue);
	       }else{
		    val = ((Const)lnext(expr))->constvalue;
	       }
	       if (attrisset) {
		    lnext(expr) =  makeConst(attrtype,
					   attrlen,
					   val,
					   false,
					   true,
					   true /* is set */);
	       } else {
		    lnext(expr) = 
			 makeConst(attrtype, 
				   attrlen,
				   (Datum)fmgr(typeid_get_retinfunc(attrtype),
					       val,get_typelem(attrtype),-1),
				   false, 
				   true /* Maybe correct-- 80% chance */,
				   false /* is not a set */);
	       }
	  } else if((Typecast_ok) && (attrtype != type_id)){
	       lnext(expr) = 
		    parser_typecast2(expr, get_id_type((long)attrtype));
	  } else
	       if (attrtype != type_id) {
		    if ((attrtype == INT2OID) && (type_id == INT4OID))
			 lfirst(expr) = lispInteger (INT2OID);
                    else if ((attrtype == FLOAT4OID) && (type_id == FLOAT8OID))
			 lfirst(expr) = lispInteger (FLOAT4OID);
                    else
			 elog(WARN, "unequal type in tlist : %s \n",
			      colname));
	       }
	  
	  Input_is_string = false;
	  Input_is_integer = false;
	  Typecast_ok = true;
#endif

	  if (attrtype != type_id) {
	      if (IsA(expr,Const)) {
		  /* try to cast the constant */
#ifdef ARRAY_PATCH
		  if (arrayRef && !(((A_Indices *)lfirst(arrayRef))->lidx)) {
		      /* updating a single item */
		      Oid typelem = get_typelem(attrtype);
		      expr = (Node*)parser_typecast2(expr,
						   type_id,
						   get_id_type((long)typelem),
					           attrlen);
		  } else
#endif
		  expr = (Node*)parser_typecast2(expr,
						 type_id,
						 get_id_type((long)attrtype),
						 attrlen);
	      } else {
		  /* currently, we can't handle casting of expressions */
		  elog(WARN, "parser: attribute '%s' is of type '%.*s' but expression is of type '%.*s'",
		       colname,
		       NAMEDATALEN, get_id_typname(attrtype),
		       NAMEDATALEN, get_id_typname(type_id));
	      }
	  }

	  if (arrayRef != NIL) {
	       Expr *target_expr;
	       Attr *att = makeNode(Attr);
	       List *ar = arrayRef;
	       List *upperIndexpr = NIL;
	       List *lowerIndexpr = NIL;

	       att->relname = pstrdup(RelationGetRelationName(rd)->data);
	       att->attrs = lcons(makeString(colname), NIL);
	       target_expr = (Expr*)handleNestedDots(pstate, att,
						     &pstate->p_last_resno);
	       while(ar!=NIL) {
		   A_Indices *ind = lfirst(ar);
#ifdef ARRAY_PATCH
		   if (lowerIndexpr || (!upperIndexpr && ind->lidx)) {
#else
		   if (lowerIndexpr) {
#endif
		       /* XXX assume all lowerIndexpr is non-null in
			* this case
			*/
		       lowerIndexpr = lappend(lowerIndexpr, ind->lidx);
		   }
		   upperIndexpr = lappend(upperIndexpr, ind->uidx);
		   ar = lnext(ar);
	       }
	       
	       expr = (Node*)make_array_set(target_expr,
					    upperIndexpr,
					    lowerIndexpr,
					    (Expr*)expr);	
	       attrtype = att_typeid(rd,resdomno);
	       attrlen = tlen(get_id_type(attrtype)); 
	  }
     } else {
	  resdomno = pstate->p_last_resno++;
	  attrtype = type_id;
	  attrlen = type_len;
     }
     tent = makeNode(TargetEntry);

     resnode = makeResdom((AttrNumber)resdomno,
			  (Oid) attrtype,
			  (Size) attrlen,
			  colname, 
			  (Index)0,
			  (Oid)0,
			  0);

     tent->resdom = resnode;
     tent->expr = expr;
	 
     return  tent;
 }


/*****************************************************************************
 *
 * Where Clause
 *
 *****************************************************************************/

/*
 * transformWhereClause -
 *    transforms the qualification and make sure it is of type Boolean
 *
 */
static Node *
transformWhereClause(ParseState *pstate, Node *a_expr)
{
    Node *qual;

    if (a_expr == NULL)
	return (Node *)NULL;		/* no qualifiers */

    qual = transformExpr(pstate, a_expr);
    if (exprType(qual) != BOOLOID) {
	elog(WARN,
	     "where clause must return type bool, not %s",
	     tname(get_id_type(exprType(qual))));
    }
    return qual;
}

/*****************************************************************************
 *
 * Sort Clause
 *
 *****************************************************************************/

/*
 *  find_tl_elt -
 *    returns the Resdom in the target list matching the specified varname
 *    and range
 *
 */
static Resdom *
find_tl_elt(ParseState *pstate, char *refname, char *colname, List *tlist)
{
    List *i;
    int real_rtable_pos;

    if(refname)
	real_rtable_pos = refnameRangeTablePosn(pstate->p_rtable, refname);

    foreach(i, tlist) {
	TargetEntry *target = (TargetEntry *)lfirst(i);
	Resdom *resnode = target->resdom;
	Var *var = (Var *)target->expr;
	char *resname = resnode->resname;
	int test_rtable_pos = var->varno;

	if (!strcmp(resname, colname)) {
	    if(refname) {
		if(real_rtable_pos == test_rtable_pos) {
		    return (resnode);
		}
	    } else {
		return (resnode);
	    }
	}
    }
    return ((Resdom *)NULL);
}

static Oid
any_ordering_op(int restype)
{
    Operator order_op;
    Oid order_opid;
    
    order_op = oper("<",restype,restype);
    order_opid = (Oid)oprid(order_op);
    
    return order_opid;
}

/*
 * transformGroupClause -
 *    transform an Group By clause
 *
 */
static List *
transformGroupClause(ParseState *pstate, List *grouplist)
{
    List *glist = NIL, *gl;

    while (grouplist != NIL) {
	GroupClause *grpcl = makeNode(GroupClause);
	Var *groupAttr = (Var*)transformExpr(pstate, (Node*)lfirst(grouplist));

	if (nodeTag(groupAttr) != T_Var) {
	    elog(WARN, "parser: can only specify attribute in group by");
	}
	grpcl->grpAttr = groupAttr;
	grpcl->grpOpoid = any_ordering_op(groupAttr->vartype);
	if (glist == NIL) {
	    gl = glist = lcons(grpcl, NIL);
	} else {
	    lnext(gl) = lcons(grpcl, NIL);
	    gl = lnext(gl);
	}
	grouplist = lnext(grouplist);
    }

    return glist;
}

/*
 * transformSortClause -
 *    transform an Order By clause
 *
 */
static List *
transformSortClause(ParseState *pstate,
		    List *orderlist, List *targetlist,
		    char* uniqueFlag)
{
    List *sortlist = NIL;
    List *s, *i;

    while(orderlist != NIL) {
	SortBy *sortby = lfirst(orderlist);
	SortClause *sortcl = makeNode(SortClause);
	Resdom *resdom;
	
	resdom = find_tl_elt(pstate, sortby->range, sortby->name, targetlist);
	if (resdom == NULL)
	    elog(WARN,"The field being sorted by must appear in the target list");
	
	sortcl->resdom = resdom;
	sortcl->opoid = oprid(oper(sortby->useOp,
				   resdom->restype,
				   resdom->restype));
	if (sortlist == NIL) {
	    s = sortlist = lcons(sortcl, NIL);
	}else {
	    lnext(s) = lcons(sortcl, NIL);
	    s = lnext(s);
	}
	orderlist = lnext(orderlist);
    }
    
    if (uniqueFlag) {
      if (uniqueFlag[0] == '*') {
	/* concatenate all elements from target list
	   that are not already in the sortby list */
        foreach (i,targetlist) {
	    TargetEntry *tlelt = (TargetEntry *)lfirst(i);

	    s = sortlist;
	    while(s != NIL) {
		SortClause *sortcl = lfirst(s);
		if (sortcl->resdom==tlelt->resdom)
		    break;
		s = lnext(s);
	    }
	    if (s == NIL) {
		/* not a member of the sortclauses yet */
		SortClause *sortcl = makeNode(SortClause);
		
		sortcl->resdom = tlelt->resdom;
		sortcl->opoid = any_ordering_op(tlelt->resdom->restype);

		sortlist = lappend(sortlist, sortcl);
	      }
	  }
      }
      else {
	TargetEntry *tlelt;
	char* uniqueAttrName = uniqueFlag;

	  /* only create sort clause with the specified unique attribute */
	  foreach (i, targetlist) {
	    tlelt = (TargetEntry*)lfirst(i);
	    if (strcmp(tlelt->resdom->resname, uniqueAttrName) == 0)
	      break;
	  }
	  if (i == NIL) {
	    elog(WARN, "The field specified in the UNIQUE ON clause is not in the targetlist");
	  }
	  s = sortlist;
	  foreach (s, sortlist) {
	    SortClause *sortcl = lfirst(s);
	    if (sortcl->resdom == tlelt->resdom)
	      break;
	  }
	  if (s == NIL) { 
		/* not a member of the sortclauses yet */
		SortClause *sortcl = makeNode(SortClause);
		
		sortcl->resdom = tlelt->resdom;
		sortcl->opoid = any_ordering_op(tlelt->resdom->restype);

		sortlist = lappend(sortlist, sortcl);
	      }
	}

    }
    
    return sortlist;
}

/*
 ** HandleNestedDots --
 **    Given a nested dot expression (i.e. (relation func ... attr), build up
 ** a tree with of Iter and Func nodes.
 */
static Node*
handleNestedDots(ParseState *pstate, Attr *attr, int *curr_resno)
{
    List *mutator_iter;
    Node *retval = NULL;
    
    if (attr->paramNo != NULL) {
	Param *param = (Param *)transformExpr(pstate, (Node*)attr->paramNo);

	retval = 
	    ParseFunc(pstate, strVal(lfirst(attr->attrs)),
		      lcons(param, NIL),
		      curr_resno);
    } else {
	Ident *ident = makeNode(Ident);

	ident->name = attr->relname;
	ident->isRel = TRUE;
	retval =
	    ParseFunc(pstate, strVal(lfirst(attr->attrs)),
		      lcons(ident, NIL),
		      curr_resno);
    }
    
    foreach (mutator_iter, lnext(attr->attrs)) {
	retval = ParseFunc(pstate,strVal(lfirst(mutator_iter)), 
			   lcons(retval, NIL),
			   curr_resno);
    }
    
    return(retval);
}

/*
 ** make_arguments --
 **   Given the number and types of arguments to a function, and the 
 **   actual arguments and argument types, do the necessary typecasting.
 */
static void
make_arguments(int nargs,
	       List *fargs,
	       Oid *input_typeids,
	       Oid *function_typeids)
{
    /*
     * there are two ways an input typeid can differ from a function typeid :
     * either the input type inherits the function type, so no typecasting is
     * necessary, or the input type can be typecast into the function type.
     * right now, we only typecast unknowns, and that is all we check for.
     */
    
    List *current_fargs;
    int i;
    
    for (i=0, current_fargs = fargs;
	 i<nargs;
	 i++, current_fargs = lnext(current_fargs)) {

	if (input_typeids[i] == UNKNOWNOID && function_typeids[i] != 0) {
	    lfirst(current_fargs) =
		parser_typecast2(lfirst(current_fargs),
				 input_typeids[i],
				 get_id_type(function_typeids[i]),
				 -1);
	}
    }
}

/*
 ** setup_tlist --
 **     Build a tlist that says which attribute to project to.
 **     This routine is called by ParseFunc() to set up a target list
 **     on a tuple parameter or return value.  Due to a bug in 4.0,
 **     it's not possible to refer to system attributes in this case.
 */
static List *
setup_tlist(char *attname, Oid relid)
{
    TargetEntry *tle;
    Resdom *resnode;
    Var *varnode;
    Oid typeid;
    int attno;
    
    attno = get_attnum(relid, attname);
    if (attno < 0)
	elog(WARN, "cannot reference attribute %s of tuple params/return values for functions", attname);
    
    typeid = find_atttype(relid, attname);
    resnode = makeResdom(1,
			 typeid,
			 tlen(get_id_type(typeid)),
			 get_attname(relid, attno),
			 0,
			 (Oid)0,
			 0);
    varnode = makeVar(-1, attno, typeid, -1, attno);

    tle = makeNode(TargetEntry);
    tle->resdom = resnode;
    tle->expr = (Node*)varnode;
    return (lcons(tle, NIL));
}

/*
 ** setup_base_tlist --
 **	Build a tlist that extracts a base type from the tuple
 **	returned by the executor.
 */
static List *
setup_base_tlist(Oid typeid)
{
    TargetEntry *tle;
    Resdom *resnode;
    Var *varnode;
    
    resnode = makeResdom(1,
			 typeid,
			 tlen(get_id_type(typeid)),
			 "<noname>",
			 0,
			 (Oid)0,
			 0);
    varnode = makeVar(-1, 1, typeid, -1, 1);
    tle = makeNode(TargetEntry);
    tle->resdom = resnode;
    tle->expr = (Node*)varnode;

    return (lcons(tle, NIL));
}

/*
 * ParseComplexProjection -
 *    handles function calls with a single argument that is of complex type.
 *    This routine returns NULL if it can't handle the projection (eg. sets).
 */
static Node *
ParseComplexProjection(ParseState *pstate,
		       char *funcname,
		       Node *first_arg,
		       bool *attisset)
{
    Oid argtype;
    Oid argrelid;
    Name relname;
    Relation rd;
    Oid relid;
    int attnum;

    switch (nodeTag(first_arg)) {
    case T_Iter:
	{
	    Func *func;
	    Iter *iter;

	    iter = (Iter*)first_arg;	    
	    func = (Func *)((Expr*)iter->iterexpr)->oper;
	    argtype = funcid_get_rettype(func->funcid);
	    argrelid = typeid_get_relid(argtype);
	    if (argrelid &&
		((attnum = get_attnum(argrelid, funcname))
		!= InvalidAttrNumber)) {
		
		/* the argument is a function returning a tuple, so funcname
		   may be a projection */

		/* add a tlist to the func node and return the Iter */
		rd = heap_openr(tname(get_id_type(argtype)));
		if (RelationIsValid(rd)) {
		    relid = RelationGetRelationId(rd);
		    relname = RelationGetRelationName(rd);
		    heap_close(rd);
		}
		if (RelationIsValid(rd)) {
		    func->func_tlist =
			setup_tlist(funcname, argrelid);
		    iter->itertype = att_typeid(rd,attnum);
		    return ((Node*)iter);
		}else {
		    elog(WARN, 
			 "Function %s has bad returntype %d", 
			 funcname, argtype);
		}
	    }else { 
		/* drop through */
		;
	    }
	    break;
	}
    case T_Var:
	{
	    /*
	     * The argument is a set, so this is either a projection
	     * or a function call on this set.
	     */
	    *attisset = true;
	    break;
	}
    case T_Expr:
	{
	    Expr *expr = (Expr*)first_arg;
	    Func *funcnode;

	    if (expr->opType != FUNC_EXPR)
		break;

	    funcnode= (Func *) expr->oper;
	    argtype = funcid_get_rettype(funcnode->funcid);
	    argrelid = typeid_get_relid(argtype);
	    /*
	     * the argument is a function returning a tuple, so funcname
	     * may be a projection
	     */
	    if (argrelid &&
		(attnum = get_attnum(argrelid, funcname)) 
		!= InvalidAttrNumber) {

		/* add a tlist to the func node */
		rd = heap_openr(tname(get_id_type(argtype)));
		if (RelationIsValid(rd)) {
		    relid = RelationGetRelationId(rd);
		    relname = RelationGetRelationName(rd);
		    heap_close(rd);
		}
		if (RelationIsValid(rd)) {
		    Expr *newexpr;
		    
		    funcnode->func_tlist =
			setup_tlist(funcname, argrelid);
		    funcnode->functype = att_typeid(rd,attnum);

		    newexpr = makeNode(Expr);
		    newexpr->typeOid = funcnode->functype;
		    newexpr->opType = FUNC_EXPR;
		    newexpr->oper = (Node *)funcnode;
		    newexpr->args = lcons(first_arg, NIL);

		    return ((Node*)newexpr);
		}
	    
	    }

	    elog(WARN, "Function %s has bad returntype %d", 
		funcname, argtype);
	    break;
	}
    case T_Param:
	{
	    Param *param = (Param*)first_arg;
	    /*
	     * If the Param is a complex type, this could be a projection
	     */
	    rd = heap_openr(tname(get_id_type(param->paramtype)));
	    if (RelationIsValid(rd)) {
		relid = RelationGetRelationId(rd);
		relname = RelationGetRelationName(rd);
		heap_close(rd);
	    }
	    if (RelationIsValid(rd) && 
		(attnum = get_attnum(relid, funcname))
		!= InvalidAttrNumber) {

		param->paramtype = att_typeid(rd, attnum);
		param->param_tlist = setup_tlist(funcname, relid);
		return ((Node*)param);
	    }
	    break;
	}
    default:
	break;
    }

    return NULL;
}
		       
static Node *
ParseFunc(ParseState *pstate, char *funcname, List *fargs, int *curr_resno)
{
    Oid rettype = (Oid)0;
    Oid argrelid;
    Oid funcid = (Oid)0;
    List *i = NIL;
    Node *first_arg= NULL;
    char *relname;
    char *refname;
    Relation rd;
    Oid relid;
    int nargs;
    Func *funcnode;
    Oid oid_array[8];
    Oid *true_oid_array;
    Node *retval;
    bool retset;
    bool exists;
    bool attisset = false;
    Oid toid;
    Expr *expr;

    if (fargs) {
	first_arg = lfirst(fargs);
	if (first_arg == NULL)
	    elog (WARN,"function %s does not allow NULL input",funcname);
    }
    
    /*
     ** check for projection methods: if function takes one argument, and 
     ** that argument is a relation, param, or PQ function returning a complex 
     ** type, then the function could be a projection.
     */
    if (length(fargs) == 1) {
    	
	if (nodeTag(first_arg)==T_Ident && ((Ident*)first_arg)->isRel) {
	    RangeTblEntry *rte;
	    Ident *ident = (Ident*)first_arg;

	    /*
	     * first arg is a relation. This could be a projection.
	     */
	    refname = ident->name;

	    rte = refnameRangeTableEntry(pstate->p_rtable, refname);
	    if (rte == NULL)
		rte = addRangeTableEntry(pstate, refname, refname, FALSE, FALSE,NULL);

	    relname = rte->relname;
	    relid = rte->relid;

	    /* If the attr isn't a set, just make a var for it.  If
	     * it is a set, treat it like a function and drop through.
	     */
	    if (get_attnum(relid, funcname) != InvalidAttrNumber) {
		int dummyTypeId;

		return
		    ((Node*)make_var(pstate,
				     refname,
				     funcname,
				     &dummyTypeId));
	    } else {
		/* drop through - attr is a set */
		;
	    }
	} else if (ISCOMPLEX(exprType(first_arg))) {
	    /*
	     * Attempt to handle projection of a complex argument. If
	     * ParseComplexProjection can't handle the projection, we
	     * have to keep going.
	     */
	    retval = ParseComplexProjection(pstate,
					    funcname,
					    first_arg,
					    &attisset);
	    if (attisset) {
		toid = exprType(first_arg);
		rd = heap_openr(tname(get_id_type(toid)));
		if (RelationIsValid(rd)) {
		    relname = RelationGetRelationName(rd)->data;
		    heap_close(rd);
		} else
		    elog(WARN,
			 "Type %s is not a relation type",
			 tname(get_id_type(toid)));
		argrelid = typeid_get_relid(toid);
		/* A projection contains either an attribute name or the
		 * "*".
		 */
		if ((get_attnum(argrelid, funcname) == InvalidAttrNumber) 
		    && strcmp(funcname, "*")) {
		    elog(WARN, "Functions on sets are not yet supported");
		}
	    }
		
	    if (retval)
		return retval;
	} else {
	    /*
	     * Parsing aggregates.
	     */
	    Oid basetype;
	    /* the aggregate count is a special case,
	       ignore its base type.  Treat it as zero */
	    if (strcmp(funcname, "count") == 0)
		basetype = 0;
	    else
		basetype = exprType(lfirst(fargs));
	    if (SearchSysCacheTuple(AGGNAME, 
				    PointerGetDatum(funcname), 
				    ObjectIdGetDatum(basetype),
				    0, 0)) {
		Aggreg *aggreg = ParseAgg(funcname, basetype, lfirst(fargs));

		AddAggToParseState(pstate, aggreg);
		return (Node*)aggreg;
	    }
	}
    }
    
    
    /*
     ** If we dropped through to here it's really a function (or a set, which
     ** is implemented as a function.)
     ** extract arg type info and transform relation name arguments into
     ** varnodes of the appropriate form.
     */
    memset(&oid_array[0], 0, 8 * sizeof(Oid)); 

    nargs=0;
    foreach ( i , fargs ) {
	int vnum;
	RangeTblEntry *rte;
	Node *pair = lfirst(i);

	if (nodeTag(pair)==T_Ident && ((Ident*)pair)->isRel) {
	    /*
	     * a relation
	     */
	    refname = ((Ident*)pair)->name;
		    
	    rte = refnameRangeTableEntry(pstate->p_rtable, refname);
	    if (rte == NULL)
		rte = addRangeTableEntry(pstate, refname, refname,
						FALSE, FALSE, NULL);
	    relname = rte->relname;

            vnum = refnameRangeTablePosn (pstate->p_rtable, rte->refname);
	   
	    /*
	     *  for func(relname), the param to the function
	     *  is the tuple under consideration.  we build a special
	     *  VarNode to reflect this -- it has varno set to the 
	     *  correct range table entry, but has varattno == 0 to 
	     *  signal that the whole tuple is the argument.
	     */
	    toid = typeid(type(relname));		   
	    /* replace it in the arg list */
	    lfirst(fargs) =
		makeVar(vnum, 0, toid, vnum, 0);
	}else if (!attisset) { /* set functions don't have parameters */
 
 	  /* any functiona args which are typed "unknown", but aren't
 	     constants, we don't know what to do with, because we
 	     can't cast them    - jolly*/
 	  if (exprType(pair) == UNKNOWNOID &&
 	       !IsA(pair, Const))
	      {
		  elog(WARN, "ParseFunc: no function named %s that takes in an unknown type as argument #%d", funcname, nargs);
	      }
 	  else
	      toid = exprType(pair);
	}
	    
	oid_array[nargs++] = toid;
    }
    
    /*
     *  func_get_detail looks up the function in the catalogs, does
     *  disambiguation for polymorphic functions, handles inheritance,
     *  and returns the funcid and type and set or singleton status of
     *  the function's return value.  it also returns the true argument
     *  types to the function.  if func_get_detail returns true,
     *  the function exists.  otherwise, there was an error.
     */
    if (attisset) { /* we know all of these fields already */
	/* We create a funcnode with a placeholder function SetEval.
	 * SetEval() never actually gets executed.  When the function
	 * evaluation routines see it, they use the funcid projected
	 * out from the relation as the actual function to call.
	 * Example:  retrieve (emp.mgr.name)
	 * The plan for this will scan the emp relation, projecting
	 * out the mgr attribute, which is a funcid.  This function
	 * is then called (instead of SetEval) and "name" is projected
	 * from its result.
	 */
	funcid = SetEvalRegProcedure;
	rettype = toid;
	retset = true;
	true_oid_array = oid_array;
	exists = true;
    } else {
	exists = func_get_detail(funcname, nargs, oid_array, &funcid,
				 &rettype, &retset, &true_oid_array);
    }
    
    if (!exists)
	elog(WARN, "no such attribute or function %s", funcname);
    
    /* got it */
    funcnode = makeNode(Func);
    funcnode->funcid = funcid;
    funcnode->functype = rettype;
    funcnode->funcisindex = false;
    funcnode->funcsize = 0;
    funcnode->func_fcache = NULL;
    funcnode->func_tlist = NIL;
    funcnode->func_planlist = NIL;
    
    /* perform the necessary typecasting */
    make_arguments(nargs, fargs, oid_array, true_oid_array);
    
    /*
     *  for functions returning base types, we want to project out the
     *  return value.  set up a target list to do that.  the executor
     *  will ignore these for c functions, and do the right thing for
     *  postquel functions.
     */
    
    if (typeid_get_relid(rettype) == InvalidOid)
	funcnode->func_tlist = setup_base_tlist(rettype);
    
    /* For sets, we want to make a targetlist to project out this
     * attribute of the set tuples.
     */
    if (attisset) {
	if (!strcmp(funcname, "*")) {
	    funcnode->func_tlist =
		expandAll(pstate, relname, refname, curr_resno);
	} else {
	    funcnode->func_tlist = setup_tlist(funcname,argrelid);
	    rettype = find_atttype(argrelid, funcname);
	}
    }

    expr = makeNode(Expr);
    expr->typeOid = rettype;
    expr->opType = FUNC_EXPR;
    expr->oper = (Node *)funcnode;
    expr->args = fargs;
    retval = (Node*)expr;
    
    /*
     *  if the function returns a set of values, then we need to iterate
     *  over all the returned values in the executor, so we stick an
     *  iter node here.  if it returns a singleton, then we don't need
     *  the iter node.
     */
    
    if (retset) {
	Iter *iter = makeNode(Iter);
	iter->itertype = rettype;
	iter->iterexpr = retval;
	retval = (Node*)iter;
    }
    
    return(retval);
}

/*****************************************************************************
 *
 *****************************************************************************/

/*
 * AddAggToParseState -
 *    add the aggregate to the list of unique aggregates in pstate. 
 *
 * SIDE EFFECT: aggno in target list entry will be modified
 */
static void
AddAggToParseState(ParseState *pstate, Aggreg *aggreg)
{
    List *ag;
    int i;

    /*
     * see if we have the aggregate already (we only need to record
     * the aggregate once)
     */
    i = 0;
    foreach(ag, pstate->p_aggs) {
	Aggreg *a = lfirst(ag);
	
	if (!strcmp(a->aggname, aggreg->aggname) &&
	    equal(a->target, aggreg->target)) {

	    /* fill in the aggno and we're done */
	    aggreg->aggno = i;
	    return;
	}
	i++;
    }

    /* not found, new aggregate */
    aggreg->aggno = i;
    pstate->p_numAgg++;
    pstate->p_aggs = lappend(pstate->p_aggs, aggreg);
    return;
}

/*
 * finalizeAggregates -
 *    fill in qry_aggs from pstate. Also checks to make sure that aggregates
 *    are used in the proper place.
 */
static void
finalizeAggregates(ParseState *pstate, Query *qry)
{    
    List *l;
    int i;

    parseCheckAggregates(pstate, qry);
	
    qry->qry_numAgg = pstate->p_numAgg;
    qry->qry_aggs =
	(Aggreg **)palloc(sizeof(Aggreg *) * qry->qry_numAgg);
    i = 0;
    foreach(l, pstate->p_aggs)
	qry->qry_aggs[i++] = (Aggreg*)lfirst(l);
}

/*    
 * contain_agg_clause--
 *    Recursively find aggreg nodes from a clause.
 *    
 *    Returns true if any aggregate found.
 */
static bool
contain_agg_clause(Node *clause)
{
    if (clause==NULL) 
	return FALSE;
    else if (IsA(clause,Aggreg))
	return TRUE;
    else if (IsA(clause,Iter))
	return contain_agg_clause(((Iter*)clause)->iterexpr);
    else if (single_node(clause)) 
	return FALSE;
    else if (or_clause(clause)) {
	List *temp;

	foreach (temp, ((Expr*)clause)->args)
	    if (contain_agg_clause(lfirst(temp)))
		return TRUE;
	return FALSE;
    } else if (is_funcclause (clause)) {
	List *temp;

	foreach(temp, ((Expr *)clause)->args)
	    if (contain_agg_clause(lfirst(temp)))
		return TRUE;
	return FALSE;
    } else if (IsA(clause,ArrayRef)) {
	List *temp;

	foreach(temp, ((ArrayRef*)clause)->refupperindexpr)
	    if (contain_agg_clause(lfirst(temp)))
		return TRUE;
	foreach(temp, ((ArrayRef*)clause)->reflowerindexpr)
	    if (contain_agg_clause(lfirst(temp)))
		return TRUE;
	if (contain_agg_clause(((ArrayRef*)clause)->refexpr))
	    return TRUE;
	if (contain_agg_clause(((ArrayRef*)clause)->refassgnexpr))
	    return TRUE;
	return FALSE;
    } else if (not_clause(clause))
	return contain_agg_clause((Node*)get_notclausearg((Expr*)clause));
    else if (is_opclause(clause))
	return (contain_agg_clause((Node*)get_leftop((Expr*)clause)) ||
		contain_agg_clause((Node*)get_rightop((Expr*)clause)));

    return FALSE;
}

/*
 * exprIsAggOrGroupCol -
 *    returns true if the expression does not contain non-group columns.
 */
static bool
exprIsAggOrGroupCol(Node *expr, List *groupClause)
{
    if (expr==NULL)
	return TRUE;
    else if (IsA(expr,Const))
	return TRUE;
    else if (IsA(expr,Var)) {
	List *gl;
	Var *var = (Var*)expr;
	/*
	 * only group columns are legal
	 */
	foreach (gl, groupClause) {
	    GroupClause *grpcl = lfirst(gl);
	    if ((grpcl->grpAttr->varno == var->varno) &&
		(grpcl->grpAttr->varattno == var->varattno))
		return TRUE;
	}
	return FALSE;
    } else if (IsA(expr,Aggreg))
	/* aggregates can take group column or non-group column as argument,
	   no further check necessary. */
	return TRUE;
    else if (IsA(expr,Expr)) {
	List *temp;

	foreach (temp, ((Expr*)expr)->args)
	    if (!exprIsAggOrGroupCol(lfirst(temp),groupClause))
		return FALSE;
	return TRUE;
    }

    return FALSE;
}

/*
 * parseCheckAggregates -
 *    this should really be done earlier but the current grammar
 *    cannot differentiate functions from aggregates. So we have do check
 *    here when the target list and the qualifications are finalized.
 */
static void
parseCheckAggregates(ParseState *pstate, Query *qry)
{
    List *tl;
    Assert(pstate->p_numAgg > 0);

    /*
     * aggregates never appear in WHERE clauses. (we have to check where
     * clause first because if there is an aggregate, the check for
     * non-group column in target list may fail.)
     */
    if (contain_agg_clause(qry->qual))
	elog(WARN, "parser: aggregates not allowed in WHERE clause");

    /*
     * the target list can only contain aggregates, group columns and
     * functions thereof.
     */
    foreach (tl, qry->targetList) {
	TargetEntry *tle = lfirst(tl);
	if (!exprIsAggOrGroupCol(tle->expr, qry->groupClause))
	    elog(WARN,
		 "parser: illegal use of aggregates or non-group column in target list");
    }
	
    /*
     * the expression specified in the HAVING clause has the same restriction
     * as those in the target list.
     */
    if (!exprIsAggOrGroupCol(qry->havingQual, qry->groupClause))
	elog(WARN,
	     "parser: illegal use of aggregates or non-group column in HAVING clause");
    
    return;
}
