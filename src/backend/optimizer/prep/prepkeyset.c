/*-------------------------------------------------------------------------
 *
 * prepkeyset.c
 *	  Special preperation for keyset queries.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/planmain.h"

/*
 * Node_Copy
 *		  a macro to simplify calling of copyObject on the specified field
 */
#define Node_Copy(from, newnode, field) newnode->field = copyObject(from->field)

bool		_use_keyset_query_optimizer = FALSE;

#ifdef ENABLE_KEY_SET_QUERY

static int	inspectOpNode(Expr *expr);
static int	inspectAndNode(Expr *expr);
static int	inspectOrNode(Expr *expr);
static int	TotalExpr;

/**********************************************************************
 *	 This routine transforms query trees with the following form:
 *		 SELECT a,b, ... FROM one_table WHERE
 *		  (v1 = const1 AND v2 = const2 [ vn = constn ]) OR
 *		  (v1 = const3 AND v2 = const4 [ vn = constn ]) OR
 *		  (v1 = const5 AND v2 = const6 [ vn = constn ]) OR
 *						   ...
 *		  [(v1 = constn AND v2 = constn [ vn = constn ])]
 *
 *							   into
 *
 *		 SELECT a,b, ... FROM one_table WHERE
 *		  (v1 = const1 AND v2 = const2 [ vn = constn ]) UNION
 *		 SELECT a,b, ... FROM one_table WHERE
 *		  (v1 = const3 AND v2 = const4 [ vn = constn ]) UNION
 *		 SELECT a,b, ... FROM one_table WHERE
 *		  (v1 = const5 AND v2 = const6 [ vn = constn ]) UNION
 *						   ...
 *		 SELECT a,b, ... FROM one_table WHERE
 *		  [(v1 = constn AND v2 = constn [ vn = constn ])]
 *
 *
 *	 To qualify for transformation the query must not be a sub select,
 *	 a HAVING, or a GROUP BY.	It must be a single table and have KSQO
 *	 set to 'on'.
 *
 *	 The primary use of this transformation is to avoid the exponrntial
 *	 memory consumption of cnfify() and to make use of index access
 *	 methods.
 *
 *		  daveh@insightdist.com   1998-08-31
 *
 *	 May want to also prune out duplicate terms.
 **********************************************************************/
void
transformKeySetQuery(Query *origNode)
{
	/* Qualify as a key set query candidate  */
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

	/* Qualify single table query	*/
	if (length(origNode->rtable) != 1)
		return;

	/* Sorry about the global, not worth passing around		*/
	/* 9 expressions seems like a good number.	More than 9 */
	/* and it starts to slow down quite a bit				*/
	TotalExpr = 0;
	/*************************/
	/* Qualify where clause */
	/*************************/
	if (!inspectOrNode((Expr *) origNode->jointree->quals) || TotalExpr < 9)
		return;

	/* Copy essential elements into a union node */
	while (((Expr *) origNode->jointree->quals)->opType == OR_EXPR)
	{
		Query	   *unionNode = makeNode(Query);
		List	   *qualargs = ((Expr *) origNode->jointree->quals)->args;

		unionNode->commandType = origNode->commandType;
		unionNode->resultRelation = origNode->resultRelation;
		unionNode->isPortal = origNode->isPortal;
		unionNode->isBinary = origNode->isBinary;

		Node_Copy(origNode, unionNode, distinctClause);
		Node_Copy(origNode, unionNode, sortClause);
		Node_Copy(origNode, unionNode, rtable);
		origNode->jointree->quals = NULL; /* avoid unnecessary copying */
		Node_Copy(origNode, unionNode, jointree);
		Node_Copy(origNode, unionNode, targetList);

		/* Pull up Expr =  */
		unionNode->jointree->quals = lsecond(qualargs);

		/* Pull up balance of tree	*/
		origNode->jointree->quals = lfirst(qualargs);

		origNode->unionClause = lappend(origNode->unionClause, unionNode);
	}
	return;
}




static int
/**********************************************************************
 *	 Checks for 1 or more OR terms w/ 1 or more AND terms.
 *	 AND terms must be equal in size.
 *	 Returns the number of each AND term.
 **********************************************************************/
inspectOrNode(Expr *expr)
{
	int			rc;
	Expr	   *firstExpr,
			   *secondExpr;

	if (!(expr && nodeTag(expr) == T_Expr && expr->opType == OR_EXPR))
		return 0;

	firstExpr = lfirst(expr->args);
	secondExpr = lsecond(expr->args);
	if (nodeTag(firstExpr) != T_Expr || nodeTag(secondExpr) != T_Expr)
		return 0;

	if (firstExpr->opType == OR_EXPR && secondExpr->opType == AND_EXPR)
	{
		if ((rc = inspectOrNode(firstExpr)) == 0)
			return 0;

		return (rc == inspectAndNode(secondExpr)) ? rc : 0;
	}
	else if (firstExpr->opType == AND_EXPR && secondExpr->opType == AND_EXPR)
	{
		if ((rc = inspectAndNode(firstExpr)) == 0)
			return 0;

		return (rc == inspectAndNode(secondExpr)) ? rc : 0;

	}

	return 0;
}


static int
/**********************************************************************
 *	Check for one or more AND terms.   Each sub-term must be a T_Const
 *	T_Var expression.
 *	Returns the number of AND terms.
 **********************************************************************/
inspectAndNode(Expr *expr)
{
	int			rc;
	Expr	   *firstExpr,
			   *secondExpr;

	if (!(expr && nodeTag(expr) == T_Expr && expr->opType == AND_EXPR))
		return 0;

	firstExpr = lfirst(expr->args);
	secondExpr = lsecond(expr->args);
	if (nodeTag(firstExpr) != T_Expr || nodeTag(secondExpr) != T_Expr)
		return 0;

	if (firstExpr->opType == AND_EXPR &&
		secondExpr->opType == OP_EXPR && inspectOpNode(secondExpr))
	{
		rc = inspectAndNode(firstExpr);
		return ((rc) ? (rc + 1) : 0);	/* Add up the AND nodes */
	}
	else if (firstExpr->opType == OP_EXPR && inspectOpNode(firstExpr) &&
			 secondExpr->opType == OP_EXPR && inspectOpNode(secondExpr))
		return 1;

	return 0;
}


static int
/******************************************************************
 *	Return TRUE if T_Var = T_Const, else FALSE
 *	Actually it does not test for =.	Need to do this!
 ******************************************************************/
inspectOpNode(Expr *expr)
{
	Expr	   *firstExpr,
			   *secondExpr;

	if (nodeTag(expr) != T_Expr || expr->opType != OP_EXPR)
		return FALSE;

	TotalExpr++;

	firstExpr = lfirst(expr->args);
	secondExpr = lsecond(expr->args);
	return (firstExpr && secondExpr && nodeTag(firstExpr) == T_Var && nodeTag(secondExpr) == T_Const);
}

#endif /* ENABLE_KEY_SET_QUERY */
