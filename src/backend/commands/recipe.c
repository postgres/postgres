/*-------------------------------------------------------------------------
 *
 * recipe.c--
 *	  routines for handling execution of Tioga recipes
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/recipe.c,v 1.12 1997/11/21 18:09:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <postgres.h>

#include <nodes/parsenodes.h>
#include <nodes/plannodes.h>
#include <nodes/execnodes.h>
#include <nodes/makefuncs.h>
#include <catalog/pg_type.h>
#include <commands/recipe.h>
#include <libpq/libpq-be.h>
#include <utils/builtins.h>
#include <utils/relcache.h>		/* for RelationNameGetRelation */
#include <parser/parse_query.h>
#include <rewrite/rewriteHandler.h>
#include <rewrite/rewriteManip.h>
#include <tcop/pquery.h>
#include <tcop/dest.h>
#include <optimizer/planner.h>
#include <executor/executor.h>

/* from tcop/postgres.c */
extern CommandDest whereToSendOutput;

#ifndef TIOGA

void
beginRecipe(RecipeStmt *stmt)
{
	elog(NOTICE, "You must compile with TIOGA defined in order to use recipes\n");
}

#else

#include <tioga/tgRecipe.h>

#define DEBUG_RECIPE 1

/* structure to keep track of the tee node plans */
typedef struct _teePlanInfo
{
	char	   *tpi_relName;
	Query	   *tpi_parsetree;
	Plan	   *tpi_plan;
}			TeePlanInfo;

typedef struct _teeInfo
{
	int			num;
	TeePlanInfo *val;
}			TeeInfo;

QueryTreeList *appendQlist(QueryTreeList *q1, QueryTreeList *q2);
void		OffsetVarAttno(Node *node, int varno, int offset);

static void
appendTeeQuery(TeeInfo * teeInfo,
			   QueryTreeList *q,
			   char *teeNodeName);

static Plan *
replaceTeeScans(Plan *plan,
				Query *parsetree,
				TeeInfo * teeInfo);
static void
replaceSeqScan(Plan *plan,
			   Plan *parent,
			   int rt_ind,
			   Plan *tplan);

static void
tg_rewriteQuery(TgRecipe * r, TgNode * n,
				QueryTreeList *q,
				QueryTreeList *inputQlist);
static Node *
tg_replaceNumberedParam(Node *expression,
						int pnum,
						int rt_ind,
						char *teeRelName);
static Node *
tg_rewriteParamsInExpr(Node *expression,
					   QueryTreeList *inputQlist);
static QueryTreeList *
tg_parseSubQuery(TgRecipe * r,
				 TgNode * n,
				 TeeInfo * teeInfo);
static QueryTreeList *
tg_parseTeeNode(TgRecipe * r,
				TgNode * n,
				int i,
				QueryTreeList *qList,
				TeeInfo * teeInfo);


/*
   The Tioga recipe rewrite algorithm:

   To parse a Tioga recipe, we start from an eye node and go backwards through
   its input nodes.  To rewrite a Tioga node, we do the following:

	  1) parse the node we're at in the standard way (calling parser() )
	  2) rewrite its input nodes recursively using Tioga rewrite
	  3) now, with the rewritten input parse trees and the original parse tree
		 of the node,  we rewrite the the node.
		 To do the rewrite, we use the target lists, range tables, and
		 qualifications of the input parse trees
*/

/*
 * beginRecipe:
 *	  this is the main function to recipe execution
 *	 this function is invoked for EXECUTE RECIPE ...  statements
 *
 *	takes in a RecipeStmt structure from the parser
 * and returns a list of cursor names
 */

void
beginRecipe(RecipeStmt *stmt)
{
	TgRecipe   *r;
	int			i;
	QueryTreeList *qList;
	char		portalName[1024];

	Plan	   *plan;
	TupleDesc	attinfo;
	QueryDesc  *queryDesc;
	Query	   *parsetree;

	int			numTees;
	TeeInfo    *teeInfo;

	/*
	 * retrieveRecipe() reads the recipe from the database and returns a
	 * TgRecipe* structure we can work with
	 */

	r = retrieveRecipe(stmt->recipeName);

	if (r == NULL)
		return;

	/* find the number of tees in the recipe */
	numTees = r->tees->num;

	if (numTees > 0)
	{
		/* allocate a teePlan structure */
		teeInfo = (TeeInfo *) malloc(sizeof(TeeInfo));
		teeInfo->num = numTees;
		teeInfo->val = (TeePlanInfo *) malloc(numTees * sizeof(TeePlanInfo));
		for (i = 0; i < numTees; i++)
		{
			teeInfo->val[i].tpi_relName = r->tees->val[i]->nodeName;
			teeInfo->val[i].tpi_parsetree = NULL;
			teeInfo->val[i].tpi_plan = NULL;
		}
	}
	else
		teeInfo = NULL;

	/*
	 * for each viewer in the recipe, go backwards from each viewer input
	 * and generate a plan.  Attach the plan to cursors.
	 */
	for (i = 0; i < r->eyes->num; i++)
	{
		TgNodePtr	e;

		e = r->eyes->val[i];
		if (e->inNodes->num > 1)
		{
			elog(NOTICE,
				 "beginRecipe: Currently eyes cannot have more than one input");
		}
		if (e->inNodes->num == 0)
		{
			/* no input to this eye, skip it */
			continue;
		}

#ifdef DEBUG_RECIPE
		elog(NOTICE, "beginRecipe: eyes[%d] = %s\n", i, e->nodeName);
#endif							/* DEBUG_RECIPE */

		qList = tg_parseSubQuery(r, e->inNodes->val[0], teeInfo);

		if (qList == NULL)
		{
			/* eye is directly connected to a tee node */
			/* XXX TODO: handle this case */
		}

		/* now, plan the queries */

		/*
		 * should really do everything pg_plan() does, but for now, we
		 * skip the rule rewrite and time qual stuff
		 */

		/* ----------------------------------------------------------
		 * 1) plan the main query, everything from an eye node back to
			 a Tee
		 * ---------------------------------------------------------- */
		parsetree = qList->qtrees[0];

		/*
		 * before we plan, we want to see all the changes we did, during
		 * the rewrite phase, such as creating the tee tables,
		 * setheapoverride() allows us to see the changes
		 */
		setheapoverride(true);
		plan = planner(parsetree);

		/* ----------------------------------------------------------
		 * 2) plan the tee queries, (subgraphs rooted from a Tee)
			 by the time the eye is processed, all tees that contribute
			 to that eye will have been included in the teeInfo list
		 * ---------------------------------------------------------- */
		if (teeInfo)
		{
			int			t;
			Plan	   *tplan;
			Tee		   *newplan;

			for (t = 0; t < teeInfo->num; t++)
			{
				if (teeInfo->val[t].tpi_plan == NULL)
				{
					/* plan it in the usual fashion */
					tplan = planner(teeInfo->val[t].tpi_parsetree);

					/* now add a tee node to the root of the plan */
					elog(NOTICE, "adding tee plan node to the root of the %s\n",
						 teeInfo->val[t].tpi_relName);
					newplan = (Tee *) makeNode(Tee);
					newplan->plan.targetlist = tplan->targetlist;
					newplan->plan.qual = NULL;	/* tplan->qual; */
					newplan->plan.lefttree = tplan;
					newplan->plan.righttree = NULL;
					newplan->leftParent = NULL;
					newplan->rightParent = NULL;

					/*
					 * the range table of the tee is the range table of
					 * the tplan
					 */
					newplan->rtentries = teeInfo->val[t].tpi_parsetree->rtable;
					strcpy(newplan->teeTableName,
						   teeInfo->val[t].tpi_relName);
					teeInfo->val[t].tpi_plan = (Plan *) newplan;
				}
			}

			/* ----------------------------------------------------------
			 * 3) replace the tee table scans in the main plan with
				  actual tee plannodes
			 * ---------------------------------------------------------- */

			plan = replaceTeeScans(plan, parsetree, teeInfo);

		}						/* if (teeInfo) */

		setheapoverride(false);

		/* define a portal for this viewer input */
		/* for now, eyes can only have one input */
		sprintf(portalName, "%s%d", e->nodeName, 0);

		queryDesc = CreateQueryDesc(parsetree,
									plan,
									whereToSendOutput);
		/* ----------------
		 *		  call ExecStart to prepare the plan for execution
		 * ----------------
		 */
		attinfo = ExecutorStart(queryDesc, NULL);

		ProcessPortal(portalName,
					  parsetree,
					  plan,
					  attinfo,
					  whereToSendOutput);
		elog(NOTICE, "beginRecipe: cursor named %s is now available", portalName);
	}

}



/*
 * tg_rewriteQuery -
 *	  r - the recipe being rewritten
 *	  n - the node that we're current at
 *	  q - a QueryTree List containing the parse tree of the node
 *	  inputQlist - the parsetrees of its input nodes,
 *				   the size of inputQlist must be the same as the
 *				   number of input nodes.  Some elements in the inpuQlist
 *				   may be null if the inputs to those nodes are unconnected
 *
 *	this is the main routine for rewriting the recipe queries
 *	the original query tree 'q' is modified
 */

static void
tg_rewriteQuery(TgRecipe * r,
				TgNode * n,
				QueryTreeList *q,
				QueryTreeList *inputQlist)
{
	Query	   *orig;
	Query	   *inputQ;
	int			i;
	List	   *rtable;
	List	   *input_rtable;
	int			rt_length;

	/* orig is the original parse tree of the node */
	orig = q->qtrees[0];


	/*-------------------------------------------------------------------
	   step 1:

	   form a combined range table from all the range tables in the original
	   query as well as the input nodes

	   form a combined qualification from the qual in the original plus
	   the quals of the input nodes
	  -------------------------------------------------------------------
	*/

	/* start with the original range table */
	rtable = orig->rtable;
	rt_length = length(rtable);

	for (i = 0; i < n->inNodes->num; i++)
	{
		if (n->inNodes->val[i] != NULL &&
			n->inNodes->val[i]->nodeType != TG_TEE_NODE)
		{
			inputQ = inputQlist->qtrees[i];
			input_rtable = inputQ->rtable;

			/*
			 * need to offset the var nodes in the qual and targetlist
			 * because they are indexed off the original rtable
			 */
			OffsetVarNodes((Node *) inputQ->qual, rt_length);
			OffsetVarNodes((Node *) inputQ->targetList, rt_length);

			/* append the range tables from the children nodes	*/
			rtable = nconc(rtable, input_rtable);

			/*
			 * append the qualifications of the child node into the
			 * original qual list
			 */
			AddQual(orig, inputQ->qual);
		}
	}
	orig->rtable = rtable;

	/*
	 * step 2: rewrite the target list of the original parse tree if there
	 * are any references to params, replace them with the appropriate
	 * target list entry of the children node
	 */
	if (orig->targetList != NIL)
	{
		List	   *tl;
		TargetEntry *tle;

		foreach(tl, orig->targetList)
		{
			tle = lfirst(tl);
			if (tle->resdom != NULL)
			{
				tle->expr = tg_rewriteParamsInExpr(tle->expr, inputQlist);
			}
		}
	}

	/*
	 * step 3: rewrite the qual of the original parse tree if there are
	 * any references to params, replace them with the appropriate target
	 * list entry of the children node
	 */
	if (orig->qual)
	{
		if (nodeTag(orig->qual) == T_List)
		{
			elog(WARN, "tg_rewriteQuery: Whoa! why is my qual a List???");
		}
		orig->qual = tg_rewriteParamsInExpr(orig->qual, inputQlist);
	}

	/*
	 * at this point, we're done with the rewrite, the querytreelist q has
	 * been modified
	 */

}


/* tg_replaceNumberedParam:

   this procedure replaces the specified numbered param with a
   reference to a range table

   this procedure recursively calls itself

   it returns a (possibly modified) Node*.

*/
static Node *
tg_replaceNumberedParam(Node *expression,
						int pnum,		/* the number of the parameter */
						int rt_ind,		/* the range table index */
						char *teeRelName)		/* the relname of the tee
												 * table */
{
	TargetEntry *param_tle;
	Param	   *p;
	Var		   *newVar,
			   *oldVar;

	if (expression == NULL)
		return NULL;

	switch (nodeTag(expression))
	{
		case T_Param:
			{

				/*
				 * the node is a parameter, substitute the entry from the
				 * target list of the child that corresponds to the
				 * parameter number
				 */
				p = (Param *) expression;

				/* we only deal with the case of numbered parameters */
				if (p->paramkind == PARAM_NUM && p->paramid == pnum)
				{

					if (p->param_tlist)
					{

						/*
						 * we have a parameter with an attribute like
						 * $N.foo so replace it with a new var node
						 */

						/* param tlist can only have one entry in them! */
						param_tle = (TargetEntry *) (lfirst(p->param_tlist));
						oldVar = (Var *) param_tle->expr;
						oldVar->varno = rt_ind;
						oldVar->varnoold = rt_ind;
						return (Node *) oldVar;
					}
					else
					{
						/* we have $N without the .foo */
						bool		defined;
						bool		isRel;

						/*
						 * TODO here, we need to check to see whether the
						 * type of the tee is a complex type (relation) or
						 * a simple type
						 */

						/*
						 * if it is a simple type, then we need to get the
						 * "result" attribute from the tee relation
						 */

						isRel = (typeid_get_relid(p->paramtype) != 0);
						if (isRel)
						{
							newVar = makeVar(rt_ind,
											 0, /* the whole tuple */
										   TypeGet(teeRelName, &defined),
											 rt_ind,
											 0);
							return (Node *) newVar;
						}
						else
							newVar = makeVar(rt_ind,
											 1, /* just the first field,
												 * which is 'result' */
										   TypeGet(teeRelName, &defined),
											 rt_ind,
											 0);
						return (Node *) newVar;

					}
				}
				else
				{
					elog(NOTICE, "tg_replaceNumberedParam: unexpected paramkind value of %d", p->paramkind);
				}
			}
			break;
		case T_Expr:
			{

				/*
				 * the node is an expression, we need to recursively call
				 * ourselves until we find parameter nodes
				 */
				List	   *l;
				Expr	   *expr = (Expr *) expression;
				List	   *newArgs;

				/*
				 * we have to make a new args lists because Params can be
				 * replaced by Var nodes in tg_replaceNumberedParam()
				 */
				newArgs = NIL;

				/*
				 * we only care about argument to expressions, it doesn't
				 * matter when the opType is
				 */
				/* recursively rewrite the arguments of this expression */
				foreach(l, expr->args)
				{
					newArgs = lappend(newArgs,
									  tg_replaceNumberedParam(lfirst(l),
															  pnum,
															  rt_ind,
															teeRelName));
				}
				/* change the arguments of the expression */
				expr->args = newArgs;
			}
			break;
		default:
			{
				/* ignore other expr types */
			}
	}

	return expression;
}





/* tg_rewriteParamsInExpr:

   rewrite the params in expressions by using the targetlist entries
   from the input parsetrees

   this procedure recursively calls itself

   it returns a (possibly modified) Node*.

*/
static Node *
tg_rewriteParamsInExpr(Node *expression, QueryTreeList *inputQlist)
{
	List	   *tl;
	TargetEntry *param_tle,
			   *tle;
	Param	   *p;
	int			childno;
	char	   *resname;

	if (expression == NULL)
		return NULL;

	switch (nodeTag(expression))
	{
		case T_Param:
			{

				/*
				 * the node is a parameter, substitute the entry from the
				 * target list of the child that corresponds to the
				 * parameter number
				 */
				p = (Param *) expression;

				/* we only deal with the case of numbered parameters */
				if (p->paramkind == PARAM_NUM)
				{
					/* paramid's start from 1 */
					childno = p->paramid - 1;

					if (p->param_tlist)
					{

						/*
						 * we have a parameter with an attribute like
						 * $N.foo so match the resname "foo" against the
						 * target list of the (N-1)th inputQlist
						 */

						/* param tlist can only have one entry in them! */
						param_tle = (TargetEntry *) (lfirst(p->param_tlist));
						resname = param_tle->resdom->resname;

						if (inputQlist->qtrees[childno])
						{
							foreach(tl, inputQlist->qtrees[childno]->targetList)
							{
								tle = lfirst(tl);
								if (strcmp(resname, tle->resdom->resname) == 0)
								{
									return tle->expr;
								}
							}
						}
						else
						{
							elog(WARN, "tg_rewriteParamsInExpr:can't substitute for parameter %d when that input is unconnected", p->paramid);
						}

					}
					else
					{
						/* we have $N without the .foo */
						/* use the first resdom in the targetlist of the */
						/* appropriate child query */
						tl = inputQlist->qtrees[childno]->targetList;
						tle = lfirst(tl);
						return tle->expr;
					}
				}
				else
				{
					elog(NOTICE, "tg_rewriteParamsInExpr: unexpected paramkind value of %d", p->paramkind);
				}
			}
			break;
		case T_Expr:
			{

				/*
				 * the node is an expression, we need to recursively call
				 * ourselves until we find parameter nodes
				 */
				List	   *l;
				Expr	   *expr = (Expr *) expression;
				List	   *newArgs;

				/*
				 * we have to make a new args lists because Params can be
				 * replaced by Var nodes in tg_rewriteParamsInExpr()
				 */
				newArgs = NIL;

				/*
				 * we only care about argument to expressions, it doesn't
				 * matter when the opType is
				 */
				/* recursively rewrite the arguments of this expression */
				foreach(l, expr->args)
				{
					newArgs = lappend(newArgs,
						  tg_rewriteParamsInExpr(lfirst(l), inputQlist));
				}
				/* change the arguments of the expression */
				expr->args = newArgs;
			}
			break;
		default:
			{
				/* ignore other expr types */
			}
	}

	return expression;
}



/*
   getParamTypes:
	  given an element, finds its parameter types.
	  the typev array argument is set to the parameter types.
	  the parameterCount is returned

   this code is very similar to ProcedureDefine() in pg_proc.c
*/
static int
getParamTypes(TgElement * elem, Oid typev[])
{
	/* this code is similar to ProcedureDefine() */
	int16		parameterCount;
	bool		defined;
	Oid			toid;
	char	   *t;
	int			i,
				j;

	parameterCount = 0;
	for (i = 0; i < 8; i++)
	{
		typev[i] = 0;
	}
	for (j = 0; j < elem->inTypes->num; j++)
	{
		if (parameterCount == 8)
		{
			elog(WARN,
				 "getParamTypes: Ingredients cannot take > 8 arguments");
		}
		t = elem->inTypes->val[j];
		if (strcmp(t, "opaque") == 0)
		{
			elog(WARN,
				 "getParamTypes: Ingredient functions cannot take type 'opaque'");
		}
		else
		{
			toid = TypeGet(elem->inTypes->val[j], &defined);
			if (!OidIsValid(toid))
			{
				elog(WARN, "getParamTypes: arg type '%s' is not defined", t);
			}
			if (!defined)
			{
				elog(NOTICE, "getParamTypes: arg type '%s' is only a shell", t);
			}
		}
		typev[parameterCount++] = toid;
	}

	return parameterCount;
}


/*
 * tg_parseTeeNode
 *
 *	 handles the parsing of the tee node
 *
 *
 */

static QueryTreeList *
tg_parseTeeNode(TgRecipe * r,
				TgNode * n,		/* the tee node */
				int i,			/* which input this node is to its parent */
				QueryTreeList *qList,
				TeeInfo * teeInfo)

{
	QueryTreeList *q;
	char	   *tt;
	int			rt_ind;
	Query	   *orig;

	/*
	 * the input Node is a tee node, so we need to do the following: we
	 * need to parse the child of the tee node, we add that to our query
	 * tree list we need the name of the tee node table the tee node table
	 * is the table into which the tee node may materialize results.  Call
	 * it TT we add a range table to our existing query with TT in it we
	 * need to replace the parameter $i with TT (otherwise the optimizer
	 * won't know to use the table on expression containining $i) After
	 * that rewrite, the optimizer will generate sequential scans of TT
	 *
	 * Later, in the glue phase, we replace all instances of TT sequential
	 * scans with the actual Tee node
	 */
	q = tg_parseSubQuery(r, n, teeInfo);

	/* tt is the name of the tee node table */
	tt = n->nodeName;

	if (q)
		appendTeeQuery(teeInfo, q, tt);

	orig = qList->qtrees[0];
	rt_ind = RangeTablePosn(orig->rtable, tt);

	/*
	 * check to see that this table is not part of the range table
	 * already.  This usually only happens if multiple inputs are
	 * connected to the same Tee.
	 */
	if (rt_ind == 0)
	{
		orig->rtable = lappend(orig->rtable,
							   addRangeTableEntry(NULL,
												  tt,
												  tt,
												  FALSE,
												  FALSE));
		rt_ind = length(orig->rtable);
	}

	orig->qual = tg_replaceNumberedParam(orig->qual,
										 i + 1, /* params start at 1 */
										 rt_ind,
										 tt);
	return qList;
}


/*
 * tg_parseSubQuery:
 *	  go backwards from a node and parse the query
 *
 *	 the result parse tree is passed back
 *
 * could return NULL if trying to parse a teeNode
 * that's already been processed by another parent
 *
 */

static QueryTreeList *
tg_parseSubQuery(TgRecipe * r, TgNode * n, TeeInfo * teeInfo)
{
	TgElement  *elem;
	char	   *funcName;
	Oid			typev[8];		/* eight arguments maximum	*/
	int			i;
	int			parameterCount;

	QueryTreeList *qList;		/* the parse tree of the nodeElement */
	QueryTreeList *inputQlist;	/* the list of parse trees for the inputs
								 * to this node */
	QueryTreeList *q;
	Oid			relid;
	TgNode	   *child;
	Relation	rel;
	unsigned int len;
	TupleDesc	tupdesc;

	qList = NULL;

	if (n->nodeType == TG_INGRED_NODE)
	{
		/* parse each ingredient node in turn */

		elem = n->nodeElem;
		switch (elem->srcLang)
		{
			case TG_SQL:
				{

					/*
					 * for SQL ingredients, the SQL query is contained in
					 * the 'src' field
					 */

#ifdef DEBUG_RECIPE
					elog(NOTICE, "calling parser with %s", elem->src);
#endif							/* DEBUG_RECIPE */

					parameterCount = getParamTypes(elem, typev);

					qList = parser(elem->src, typev, parameterCount);

					if (qList->len > 1)
					{
						elog(NOTICE,
							 "tg_parseSubQuery: parser produced > 1 query tree");
					}
				}
				break;
			case TG_C:
				{
					/* C ingredients are registered functions in postgres */

					/*
					 * we create a new query string by using the function
					 * name (found in the 'src' field) and adding
					 * parameters to it so if the function was FOOBAR and
					 * took in two arguments, we would create a string
					 * select FOOBAR($1,$2)
					 */
					char		newquery[1000];

					funcName = elem->src;
					parameterCount = getParamTypes(elem, typev);

					if (parameterCount > 0)
					{
						int			i;

						sprintf(newquery, "select %s($1", funcName);
						for (i = 1; i < parameterCount; i++)
						{
							sprintf(newquery, "%s,$%d", newquery, i);
						}
						sprintf(newquery, "%s)", newquery);
					}
					else
						sprintf(newquery, "select %s()", funcName);

#ifdef DEBUG_RECIPE
					elog(NOTICE, "calling parser with %s", newquery);
#endif							/* DEBUG_RECIPE */

					qList = parser(newquery, typev, parameterCount);
					if (qList->len > 1)
					{
						elog(NOTICE,
							 "tg_parseSubQuery: parser produced > 1 query tree");
					}
				}
				break;
			case TG_RECIPE_GRAPH:
				elog(NOTICE, "tg_parseSubQuery: can't parse recipe graph ingredients yet!");
				break;
			case TG_COMPILED:
				elog(NOTICE, "tg_parseSubQuery: can't parse compiled ingredients yet!");
				break;
			default:
				elog(NOTICE, "tg_parseSubQuery: unknown srcLang: %d", elem->srcLang);
		}

		/* parse each of the subrecipes that are input to this node */

		if (n->inNodes->num > 0)
		{
			inputQlist = malloc(sizeof(QueryTreeList));
			inputQlist->len = n->inNodes->num + 1;
			inputQlist->qtrees = (Query **) malloc(inputQlist->len * sizeof(Query *));
			for (i = 0; i < n->inNodes->num; i++)
			{

				inputQlist->qtrees[i] = NULL;
				if (n->inNodes->val[i])
				{
					if (n->inNodes->val[i]->nodeType == TG_TEE_NODE)
					{
						qList = tg_parseTeeNode(r, n->inNodes->val[i],
												i, qList, teeInfo);
					}
					else
					{			/* input node is not a Tee */
						q = tg_parseSubQuery(r, n->inNodes->val[i],
											 teeInfo);
						Assert(q->len == 1);
						inputQlist->qtrees[i] = q->qtrees[0];
					}
				}
			}

			/* now, we have all the query trees from our input nodes */
			/* transform the original parse tree appropriately */
			tg_rewriteQuery(r, n, qList, inputQlist);
		}
	}
	else if (n->nodeType == TG_EYE_NODE)
	{

		/*
		 * if we hit an eye, we need to stop and make what we have into a
		 * subrecipe query block
		 */
		elog(NOTICE, "tg_parseSubQuery: can't handle eye nodes yet");
	}
	else if (n->nodeType == TG_TEE_NODE)
	{

		/*
		 * if we hit a tee, check to see if the parsing has been done for
		 * this tee already by the other parent
		 */

		rel = RelationNameGetRelation(n->nodeName);
		if (RelationIsValid(rel))
		{

			/*
			 * this tee has already been visited, no need to do any
			 * further processing
			 */
			return NULL;
		}
		else
		{
			/* we need to process the child of the tee first, */
			child = n->inNodes->val[0];

			if (child->nodeType == TG_TEE_NODE)
			{
				/* nested Tee nodes */
				qList = tg_parseTeeNode(r, child, 0, qList, teeInfo);
				return qList;
			}

			Assert(child != NULL);

			/* parse the input node */
			q = tg_parseSubQuery(r, child, teeInfo);
			Assert(q->len == 1);

			/* add the parsed query to the main list of queries */
			qList = appendQlist(qList, q);

			/* need to create the tee table here */

			/*
			 * the tee table created is used both for materializing the
			 * values at the tee node, and for parsing and optimization.
			 * The optimization needs to have a real table before it will
			 * consider scans on it
			 */

			/*
			 * first, find the type of the tuples being produced by the
			 * tee.  The type is the same as the output type of the child
			 * node.
			 *
			 * NOTE: we are assuming that the child node only has a single
			 * output here!
			 */
			getParamTypes(child->nodeElem, typev);

			/*
			 * the output type is either a complex type, (and is thus a
			 * relation) or is a simple type
			 */

			rel = RelationNameGetRelation(child->nodeElem->outTypes->val[0]);

			if (RelationIsValid(rel))
			{

				/*
				 * for complex types, create new relation with the same
				 * tuple descriptor as the output table type
				 */
				len = length(q->qtrees[0]->targetList);
				tupdesc = rel->rd_att;

				relid = heap_create(child->nodeElem->outTypes->val[0], tupdesc);
			}
			else
			{

				/*
				 * we have to create a relation with one attribute of the
				 * simple base type.  That attribute will have an attr
				 * name of "result"
				 */
				/* NOTE: ignore array types for the time being */

				len = 1;
				tupdesc = CreateTemplateTupleDesc(len);

				if (!TupleDescInitEntry(tupdesc, 1,
										"result",
										NULL,
										0, false))
				{
					elog(NOTICE, "tg_parseSubQuery: unexpected result from TupleDescInitEntry");
				}
				else
				{
					relid = heap_create(child->nodeElem->outTypes->val[0],
										tupdesc);
				}
			}
		}
	}
	else if (n->nodeType == TG_RECIPE_NODE)
	{
		elog(NOTICE, "tg_parseSubQuery: can't handle embedded recipes yet!");
	}
	else
		elog(NOTICE, "unknown nodeType: %d", n->nodeType);

	return qList;
}

/*
 * OffsetVarAttno -
 *	  recursively find all the var nodes with the specified varno
 * and offset their varattno with the offset
 *
 *	code is similar to OffsetVarNodes in rewriteManip.c
 */

void
OffsetVarAttno(Node *node, int varno, int offset)
{
	if (node == NULL)
		return;
	switch (nodeTag(node))
	{
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				OffsetVarAttno(tle->expr, varno, offset);
			}
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				OffsetVarAttno((Node *) expr->args, varno, offset);
			}
			break;
		case T_Var:
			{
				Var		   *var = (Var *) node;

				if (var->varno == varno)
					var->varattno += offset;
			}
			break;
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
				{
					OffsetVarAttno(lfirst(l), varno, offset);
				}
			}
			break;
		default:
			/* ignore the others */
			break;
	}
}

/*
 * appendQlist
 *	  add the contents of a QueryTreeList q2 to the end of the QueryTreeList
 *	 q1
 *
 *	returns a new querytree list
 */

QueryTreeList *
appendQlist(QueryTreeList *q1, QueryTreeList *q2)
{
	QueryTreeList *newq;
	int			i,
				j;
	int			newlen;

	if (q1 == NULL)
		return q2;

	if (q2 == NULL)
		return q1;

	newlen = q1->len + q2->len;
	newq = (QueryTreeList *) malloc(sizeof(QueryTreeList));
	newq->len = newlen;
	newq->qtrees = (Query **) malloc(newlen * sizeof(Query *));
	for (i = 0; i < q1->len; i++)
		newq->qtrees[i] = q1->qtrees[i];
	for (j = 0; j < q2->len; j++)
	{
		newq->qtrees[i + j] = q2->qtrees[j];
	}
	return newq;
}

/*
 * appendTeeQuery
 *
 *	modify the query field of the teeInfo list of the particular tee node
 */
static void
appendTeeQuery(TeeInfo * teeInfo, QueryTreeList *q, char *teeNodeName)
{
	int			i;

	Assert(teeInfo);

	for (i = 0; i < teeInfo->num; i++)
	{
		if (strcmp(teeInfo->val[i].tpi_relName, teeNodeName) == 0)
		{

			Assert(q->len == 1);
			teeInfo->val[i].tpi_parsetree = q->qtrees[0];
			return;
		}
	}
	elog(NOTICE, "appendTeeQuery: teeNodeName '%s' not found in teeInfo");
}



/*
 * replaceSeqScan
 *	  replaces sequential scans of a specified relation with the tee plan
 *	the relation is specified by its index in the range table,	 rt_ind
 *
 * returns the modified plan
 * the offset_attno is the offset that needs to be added to the parent's
 * qual or targetlist because the child plan has been replaced with a tee node
 */
static void
replaceSeqScan(Plan *plan, Plan *parent,
			   int rt_ind, Plan *tplan)
{
	Scan	   *snode;
	Tee		   *teePlan;
	Result	   *newPlan;

	if (plan == NULL)
	{
		return;
	}

	if (plan->type == T_SeqScan)
	{
		snode = (Scan *) plan;
		if (snode->scanrelid == rt_ind)
		{

			/*
			 * found the sequential scan that should be replaced with the
			 * tplan.
			 */
			/* we replace the plan, but we also need to modify its parent */

			/*
			 * replace the sequential scan with a Result node the reason
			 * we use a result node is so that we get the proper
			 * projection behavior.  The Result node is simply (ab)used as
			 * a projection node
			 */

			newPlan = makeNode(Result);
			newPlan->plan.cost = 0.0;
			newPlan->plan.state = (EState *) NULL;
			newPlan->plan.targetlist = plan->targetlist;
			newPlan->plan.lefttree = tplan;
			newPlan->plan.righttree = NULL;
			newPlan->resconstantqual = NULL;
			newPlan->resstate = NULL;

			/* change all the varno's to 1 */
			ChangeVarNodes((Node *) newPlan->plan.targetlist,
						   snode->scanrelid, 1);

			if (parent)
			{
				teePlan = (Tee *) tplan;

				if (parent->lefttree == plan)
					parent->lefttree = (Plan *) newPlan;
				else
					parent->righttree = (Plan *) newPlan;


				if (teePlan->leftParent == NULL)
					teePlan->leftParent = (Plan *) newPlan;
				else
					teePlan->rightParent = (Plan *) newPlan;

/* comment for now to test out executor-stuff
				if (parent->state) {
					ExecInitNode((Plan*)newPlan, parent->state, (Plan*)newPlan);
				}
*/
			}
		}

	}
	else
	{
		if (plan->lefttree)
		{
			replaceSeqScan(plan->lefttree, plan, rt_ind, tplan);
		}
		if (plan->righttree)
		{
			replaceSeqScan(plan->righttree, plan, rt_ind, tplan);
		}
	}
}

/*
 * replaceTeeScans
 *	  places the sequential scans of the Tee table with
 * a connection to the actual tee plan node
 */
static Plan *
replaceTeeScans(Plan *plan, Query *parsetree, TeeInfo * teeInfo)
{

	int			i;
	List	   *rtable;
	RangeTblEntry *rte;
	char		prefix[5];
	int			rt_ind;
	Plan	   *tplan;

	rtable = parsetree->rtable;
	if (rtable == NULL)
		return plan;

	/*
	 * look through the range table for the tee relation entry, that will
	 * give use the varno we need to detect which sequential scans need to
	 * be replaced with tee nodes
	 */

	rt_ind = 0;
	while (rtable != NIL)
	{
		rte = lfirst(rtable);
		rtable = lnext(rtable);
		rt_ind++;				/* range table references in varno fields
								 * start w/ 1 */

		/*
		 * look for the "tee_" prefix in the refname, also check to see
		 * that the relname and the refname are the same this should
		 * eliminate any user-specified table and leave us with the tee
		 * table entries only
		 */
		if ((strlen(rte->refname) < 4) ||
			(strcmp(rte->relname, rte->refname) != 0))
			continue;
		StrNCpy(prefix, rte->refname, 5);
		if (strcmp(prefix, "tee_") == 0)
		{
			/* okay, we found a tee node entry in the range table */

			/* find the appropriate plan in the teeInfo list */
			tplan = NULL;
			for (i = 0; i < teeInfo->num; i++)
			{
				if (strcmp(teeInfo->val[i].tpi_relName,
						   rte->refname) == 0)
				{
					tplan = teeInfo->val[i].tpi_plan;
				}
			}
			if (tplan == NULL)
			{
				elog(NOTICE, "replaceTeeScans didn't find the corresponding tee plan");
			}

			/*
			 * replace the sequential scan node with that var number with
			 * the tee plan node
			 */
			replaceSeqScan(plan, NULL, rt_ind, tplan);
		}
	}

	return plan;
}



#endif							/* TIOGA */
