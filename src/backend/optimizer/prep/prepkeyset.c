/*-------------------------------------------------------------------------
 *
 * prepkeyset.c--
 *	  Special preperation for keyset queries.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>

#include "postgres.h"
#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "utils/elog.h"

#include "nodes/nodes.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"

#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "optimizer/planmain.h"
/*
 * Node_Copy--
 *        a macro to simplify calling of copyObject on the specified field
 */
#define Node_Copy(from, newnode, field) newnode->field = copyObject(from->field)

/*****  DEBUG stuff
#define TABS {int i; printf("\n"); for (i = 0; i<level; i++) printf("\t"); }
static int level = 0;
******/

bool _use_keyset_query_optimizer = FALSE;

static int inspectOpNode(Expr *expr);
static int inspectAndNode(Expr *expr);
static int inspectOrNode(Expr *expr);

/**********************************************************************
 *   This routine transforms query trees with the following form:
 *       SELECT a,b, ... FROM one_table WHERE
 *        (v1 = const1 AND v2 = const2 [ vn = constn ]) OR
 *        (v1 = const3 AND v2 = const4 [ vn = constn ]) OR
 *        (v1 = const5 AND v2 = const6 [ vn = constn ]) OR
 *                         ...
 *        [(v1 = constn AND v2 = constn [ vn = constn ])]
 *
 *                             into 
 *
 *       SELECT a,b, ... FROM one_table WHERE
 *        (v1 = const1 AND v2 = const2 [ vn = constn ]) UNION
 *       SELECT a,b, ... FROM one_table WHERE
 *        (v1 = const3 AND v2 = const4 [ vn = constn ]) UNION
 *       SELECT a,b, ... FROM one_table WHERE
 *        (v1 = const5 AND v2 = const6 [ vn = constn ]) UNION
 *                         ...
 *       SELECT a,b, ... FROM one_table WHERE
 *        [(v1 = constn AND v2 = constn [ vn = constn ])]
 *
 *
 *   To qualify for transformation the query must not be a sub select,
 *   a HAVING, or a GROUP BY.   It must be a single table and have KSQO
 *   set to 'on'.  
 *
 *   The primary use of this transformation is to avoid the exponrntial 
 *   memory consumption of cnfify() and to make use of index access
 *   methods.
 *
 *        daveh@insightdist.com   1998-08-31
 *
 *   Needs to better identify the signeture WHERE clause.
 *   May want to also prune out duplicate where clauses.
 **********************************************************************/
void
transformKeySetQuery(Query *origNode)
{
	/*   Qualify as a key set query candidate  */
	if (_use_keyset_query_optimizer == FALSE || 
			origNode->groupClause ||
			origNode->havingQual ||
			origNode->hasAggs ||
			origNode->utilityStmt ||
			origNode->unionClause ||
			origNode->unionall ||
			origNode->hasSubLinks ||
			origNode->commandType != CMD_SELECT)
		return;

	/*  Qualify single table query   */

	/*  Qualify where clause */
	if  ( ! inspectOrNode((Expr*)origNode->qual))  {
		return;
	}

	/*  Copy essential elements into a union node */
	/*  
	elog(NOTICE, "OR_EXPR=%d, OP_EXPR=%d, AND_EXPR=%d", OR_EXPR, OP_EXPR, AND_EXPR);
	elog(NOTICE, "T_List=%d, T_Expr=%d, T_Var=%d, T_Const=%d", T_List, T_Expr, T_Var, T_Const);
	elog(NOTICE, "opType=%d", ((Expr*)origNode->qual)->opType);
	*/
	while (((Expr*)origNode->qual)->opType == OR_EXPR)  {
		Query	   *unionNode = makeNode(Query);

		/*   Pull up Expr =  */
		unionNode->qual = lsecond(((Expr*)origNode->qual)->args);

		/*   Pull up balance of tree  */
		origNode->qual = lfirst(((Expr*)origNode->qual)->args);  

		/*
		elog(NOTICE, "origNode: opType=%d, nodeTag=%d", ((Expr*)origNode->qual)->opType, nodeTag(origNode->qual));
		elog(NOTICE, "unionNode: opType=%d, nodeTag=%d", ((Expr*)unionNode->qual)->opType, nodeTag(unionNode->qual));
		*/

		unionNode->commandType = origNode->commandType;
		unionNode->resultRelation = origNode->resultRelation;
		unionNode->isPortal = origNode->isPortal;
		unionNode->isBinary = origNode->isBinary;

		if (origNode->uniqueFlag)
			unionNode->uniqueFlag = pstrdup(origNode->uniqueFlag);

		Node_Copy(origNode, unionNode, sortClause);
		Node_Copy(origNode, unionNode, rtable);
		Node_Copy(origNode, unionNode, targetList);

		origNode->unionClause = lappend(origNode->unionClause, unionNode);
	}
	return;
}




static int
inspectOrNode(Expr *expr)
{
	int fr = 0, sr = 0;
	Expr *firstExpr, *secondExpr;

	if ( ! (expr && nodeTag(expr) == T_Expr && expr->opType == OR_EXPR))
		return 0;

	firstExpr = lfirst(expr->args);
	secondExpr = lsecond(expr->args);
	if (nodeTag(firstExpr) != T_Expr || nodeTag(secondExpr) != T_Expr) 
		return 0;

	if (firstExpr->opType == OR_EXPR)
		fr = inspectOrNode(firstExpr);
	else if (firstExpr->opType == OP_EXPR)    /*   Need to make sure it is last  */
		fr = inspectOpNode(firstExpr);
	else if (firstExpr->opType == AND_EXPR)    /*   Need to make sure it is last  */
		fr = inspectAndNode(firstExpr);


	if (secondExpr->opType == AND_EXPR)
		sr = inspectAndNode(secondExpr);
	else if (secondExpr->opType == OP_EXPR)
		sr = inspectOpNode(secondExpr);
		
	return (fr && sr);		
}


static int
inspectAndNode(Expr *expr)
{
	int fr = 0, sr = 0;
	Expr *firstExpr, *secondExpr;

	if ( ! (expr && nodeTag(expr) == T_Expr && expr->opType == AND_EXPR))
		return 0;

	firstExpr = lfirst(expr->args);
	secondExpr = lsecond(expr->args);
	if (nodeTag(firstExpr) != T_Expr || nodeTag(secondExpr) != T_Expr) 
		return 0;

	if (firstExpr->opType == AND_EXPR)
		fr = inspectAndNode(firstExpr);
	else if (firstExpr->opType == OP_EXPR)
		fr = inspectOpNode(firstExpr);

	if (secondExpr->opType == OP_EXPR)
		sr = inspectOpNode(secondExpr);
		
	return (fr && sr);		
}


static int
/******************************************************************
 *  Return TRUE if T_Var = T_Const, else FALSE
 *  Actually it does not test for =.    Need to do this!
 ******************************************************************/
inspectOpNode(Expr *expr)
{
	Expr *firstExpr, *secondExpr;

	if (nodeTag(expr) != T_Expr || expr->opType != OP_EXPR)
		return 0;

	firstExpr = lfirst(expr->args);
	secondExpr = lsecond(expr->args);
	return  (firstExpr && secondExpr && nodeTag(firstExpr) == T_Var && nodeTag(secondExpr) == T_Const);
}
