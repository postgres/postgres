/*
 * explain.c
 *	  Explain the query execution plan
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *	  $Id: explain.c,v 1.49 1999/11/07 23:08:02 momjian Exp $
 *
 */

#include "postgres.h"

#include "commands/explain.h"
#include "lib/stringinfo.h"
#include "nodes/print.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "utils/relcache.h"

typedef struct ExplainState
{
	/* options */
	bool		printCost;		/* print cost */
	bool		printNodes;		/* do nodeToString() instead */
	/* other states */
	List	   *rtable;			/* range table */
} ExplainState;

static char *Explain_PlanToString(Plan *plan, ExplainState *es);
static void ExplainOneQuery(Query *query, bool verbose, CommandDest dest);

/* Convert a null string pointer into "<>" */
#define stringStringInfo(s) (((s) == NULL) ? "<>" : (s))


/*
 * ExplainQuery -
 *	  print out the execution plan for a given query
 *
 */
void
ExplainQuery(Query *query, bool verbose, CommandDest dest)
{
	List	   *rewritten;
	List	   *l;

	/* rewriter and planner may not work in aborted state? */
	if (IsAbortedTransactionBlockState())
	{
		elog(NOTICE, "(transaction aborted): %s",
			 "queries ignored until END");
		return;
	}

	/* rewriter and planner will not cope with utility statements */
	if (query->commandType == CMD_UTILITY)
	{
		elog(NOTICE, "Utility statements have no plan structure");
		return;
	}

	/* Rewrite through rule system */
	rewritten = QueryRewrite(query);

	/* In the case of an INSTEAD NOTHING, tell at least that */
	if (rewritten == NIL)
	{
		elog(NOTICE, "Query rewrites to nothing");
		return;
	}

	/* Explain every plan */
	foreach(l, rewritten)
		ExplainOneQuery(lfirst(l), verbose, dest);
}

/*
 * ExplainOneQuery -
 *	  print out the execution plan for one query
 *
 */
static void
ExplainOneQuery(Query *query, bool verbose, CommandDest dest)
{
	char	   *s;
	Plan	   *plan;
	ExplainState *es;

	/* plan the query */
	plan = planner(query);

	/* pg_plan could have failed */
	if (plan == NULL)
		return;

	es = (ExplainState *) palloc(sizeof(ExplainState));
	MemSet(es, 0, sizeof(ExplainState));

	es->printCost = true;		/* default */

	if (verbose)
		es->printNodes = true;

	es->rtable = query->rtable;

	if (es->printNodes)
	{
		s = nodeToString(plan);
		if (s)
		{
			elog(NOTICE, "QUERY DUMP:\n\n%s", s);
			pfree(s);
		}
	}

	if (es->printCost)
	{
		s = Explain_PlanToString(plan, es);
		if (s)
		{
			elog(NOTICE, "QUERY PLAN:\n\n%s", s);
			pfree(s);
		}
	}

	if (es->printNodes)
		pprint(plan);			/* display in postmaster log file */

	pfree(es);
}

/*****************************************************************************
 *
 *****************************************************************************/

/*
 * explain_outNode -
 *	  converts a Node into ascii string and append it to 'str'
 */
static void
explain_outNode(StringInfo str, Plan *plan, int indent, ExplainState *es)
{
	List	   *l;
	Relation	relation;
	char	   *pname;
	int			i;

	if (plan == NULL)
	{
		appendStringInfo(str, "\n");
		return;
	}

	switch (nodeTag(plan))
	{
		case T_Result:
			pname = "Result";
			break;
		case T_Append:
			pname = "Append";
			break;
		case T_NestLoop:
			pname = "Nested Loop";
			break;
		case T_MergeJoin:
			pname = "Merge Join";
			break;
		case T_HashJoin:
			pname = "Hash Join";
			break;
		case T_SeqScan:
			pname = "Seq Scan";
			break;
		case T_IndexScan:
			pname = "Index Scan";
			break;
		case T_Noname:
			pname = "Noname Scan";
			break;
		case T_Material:
			pname = "Materialize";
			break;
		case T_Sort:
			pname = "Sort";
			break;
		case T_Group:
			pname = "Group";
			break;
		case T_Agg:
			pname = "Aggregate";
			break;
		case T_Unique:
			pname = "Unique";
			break;
		case T_Hash:
			pname = "Hash";
			break;
		default:
			pname = "???";
			break;
	}

	appendStringInfo(str, pname);
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			if (ScanDirectionIsBackward(((IndexScan *)plan)->indxorderdir))
				appendStringInfo(str, " Backward");
			appendStringInfo(str, " using ");
			i = 0;
			foreach(l, ((IndexScan *) plan)->indxid)
			{
				relation = RelationIdGetRelation(lfirsti(l));
				Assert(relation);
				if (++i > 1)
					appendStringInfo(str, ", ");
				appendStringInfo(str,
								 stringStringInfo(RelationGetRelationName(relation)));
				/* drop relcache refcount from RelationIdGetRelation */
				RelationDecrementReferenceCount(relation);
			}
		case T_SeqScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = nth(((Scan *) plan)->scanrelid - 1, es->rtable);

				appendStringInfo(str, " on ");
				if (strcmp(rte->refname, rte->relname) != 0)
				{
					appendStringInfo(str, "%s ",
									 stringStringInfo(rte->relname));
				}
				appendStringInfo(str, stringStringInfo(rte->refname));
			}
			break;
		default:
			break;
	}
	if (es->printCost)
	{
		appendStringInfo(str, "  (cost=%.2f rows=%d width=%d)",
						 plan->cost, plan->plan_size, plan->plan_width);
	}
	appendStringInfo(str, "\n");

	/* initPlan-s */
	if (plan->initPlan)
	{
		List	   *saved_rtable = es->rtable;
		List	   *lst;

		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  InitPlan\n");
		foreach(lst, plan->initPlan)
		{
			es->rtable = ((SubPlan *) lfirst(lst))->rtable;
			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "    ->  ");
			explain_outNode(str, ((SubPlan *) lfirst(lst))->plan, indent + 2, es);
		}
		es->rtable = saved_rtable;
	}

	/* lefttree */
	if (outerPlan(plan))
	{
		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  ->  ");
		explain_outNode(str, outerPlan(plan), indent + 3, es);
	}

	/* righttree */
	if (innerPlan(plan))
	{
		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  ->  ");
		explain_outNode(str, innerPlan(plan), indent + 3, es);
	}

	/* subPlan-s */
	if (plan->subPlan)
	{
		List	   *saved_rtable = es->rtable;
		List	   *lst;

		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  SubPlan\n");
		foreach(lst, plan->subPlan)
		{
			es->rtable = ((SubPlan *) lfirst(lst))->rtable;
			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "    ->  ");
			explain_outNode(str, ((SubPlan *) lfirst(lst))->plan, indent + 4, es);
		}
		es->rtable = saved_rtable;
	}

	if (nodeTag(plan) == T_Append)
	{
		List	   *saved_rtable = es->rtable;
		List	   *lst;
		int			whichplan = 0;
		Append	   *appendplan = (Append *) plan;

		foreach(lst, appendplan->appendplans)
		{
			Plan	   *subnode = (Plan *) lfirst(lst);

			if (appendplan->inheritrelid > 0)
			{
				RangeTblEntry *rtentry;

				rtentry = nth(whichplan, appendplan->inheritrtable);
				Assert(rtentry != NULL);
				rt_store(appendplan->inheritrelid, es->rtable, rtentry);
			}
			else
				es->rtable = nth(whichplan, appendplan->unionrtables);

			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "    ->  ");

			explain_outNode(str, subnode, indent + 4, es);

			whichplan++;
		}
		es->rtable = saved_rtable;
	}
}

static char *
Explain_PlanToString(Plan *plan, ExplainState *es)
{
	StringInfoData str;

	/* see stringinfo.h for an explanation of this maneuver */
	initStringInfo(&str);
	if (plan != NULL)
		explain_outNode(&str, plan, 0, es);
	return str.data;
}
