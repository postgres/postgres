/*
 * explain.c--
 *	  Explain the query execution plan
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *	  $Id: explain.c,v 1.30 1998/12/18 14:45:07 wieck Exp $
 *
 */
#include <stdio.h>
#include <string.h>

#include <postgres.h>

#include <nodes/plannodes.h>
#include <nodes/print.h>
#include <tcop/tcopprot.h>
#include <lib/stringinfo.h>
#include <commands/explain.h>
#include <parser/parsetree.h>
#include <parser/parse_node.h>
#include <optimizer/planner.h>
#include <access/xact.h>
#include <utils/relcache.h>
#include <rewrite/rewriteHandler.h>

typedef struct ExplainState
{
	/* options */
	bool		printCost;		/* print cost */
	bool		printNodes;		/* do nodeToString() instead */
	/* other states */
	List	   *rtable;			/* range table */
} ExplainState;

static char *Explain_PlanToString(Plan *plan, ExplainState *es);
static void printLongNotice(const char * header, const char * message);
static void ExplainOneQuery(Query *query, bool verbose, CommandDest dest);


/*
 * ExplainQuery -
 *	  print out the execution plan for a given query
 *
 */
void
ExplainQuery(Query *query, bool verbose, CommandDest dest)
{
	List	*rewritten;
	List	*l;

	if (IsAbortedTransactionBlockState())
	{
		char	   *tag = "*ABORT STATE*";

		EndCommand(tag, dest);

		elog(NOTICE, "(transaction aborted): %s",
			 "queries ignored until END");

		return;
	}

	/* Rewrite through rule system */
	rewritten = QueryRewrite(query);

	/* In the case of an INSTEAD NOTHING, tell at least that */
	if (rewritten == NIL)
	{
		elog(NOTICE, "query rewrites to nothing");
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

	/* plan the queries (XXX we've ignored rewrite!!) */
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
			printLongNotice("QUERY DUMP:\n\n", s);
			pfree(s);
		}
	}

	if (es->printCost)
	{
		s = Explain_PlanToString(plan, es);
		if (s)
		{
			printLongNotice("QUERY PLAN:\n\n", s);
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
	List			*l;
	Relation	relation;
	char			*pname;
	int				i;

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
		case T_Temp:
			pname = "Temp Scan";
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
		case T_Tee:
			pname = "Tee";
			break;
		default:
			pname = "";
			break;
	}

	appendStringInfo(str, pname);
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			appendStringInfo(str, " using ");
			i = 0;
			foreach (l, ((IndexScan *) plan)->indxid)
			{
				relation = RelationIdCacheGetRelation((int) lfirst(l));
				if (++i > 1)
				{
					appendStringInfo(str, ", ");
				}
				appendStringInfo(str, 
					stringStringInfo((RelationGetRelationName(relation))->data));
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
		appendStringInfo(str, "  (cost=%.2f size=%d width=%d)",
				plan->cost, plan->plan_size, plan->plan_width);
	}
	appendStringInfo(str, "\n");

	/* initPlan-s */
	if (plan->initPlan)
	{
		List	   *saved_rtable = es->rtable;
		List	   *lst;

		for (i = 0; i < indent; i++) 
		{
			appendStringInfo(str, "  ");
		}
		appendStringInfo(str, "  InitPlan\n");
		foreach(lst, plan->initPlan)
		{
			es->rtable = ((SubPlan *) lfirst(lst))->rtable;
			for (i = 0; i < indent; i++)
			{
				appendStringInfo(str, "  ");
			}
			appendStringInfo(str, "    ->  ");
			explain_outNode(str, ((SubPlan *) lfirst(lst))->plan, indent + 2, es);
		}
		es->rtable = saved_rtable;
	}

	/* lefttree */
	if (outerPlan(plan))
	{
		for (i = 0; i < indent; i++)
		{
			appendStringInfo(str, "  ");
		}
		appendStringInfo(str, "  ->  ");
		explain_outNode(str, outerPlan(plan), indent + 3, es);
	}

	/* righttree */
	if (innerPlan(plan))
	{
		for (i = 0; i < indent; i++)
		{
			appendStringInfo(str, "  ");
		}
		appendStringInfo(str, "  ->  ");
		explain_outNode(str, innerPlan(plan), indent + 3, es);
	}

	/* subPlan-s */
	if (plan->subPlan)
	{
		List	   *saved_rtable = es->rtable;
		List	   *lst;

		for (i = 0; i < indent; i++)
		{
			appendStringInfo(str, "  ");
		}
		appendStringInfo(str, "  SubPlan\n");
		foreach(lst, plan->subPlan)
		{
			es->rtable = ((SubPlan *) lfirst(lst))->rtable;
			for (i = 0; i < indent; i++)
			{
				appendStringInfo(str, "  ");
			}
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
				ResTarget  *rtentry;

				es->rtable = appendplan->inheritrtable;
				rtentry = nth(whichplan, appendplan->inheritrtable);
				Assert(rtentry != NULL);
				rt_store(appendplan->inheritrelid, es->rtable, rtentry);
			}
			else
				es->rtable = nth(whichplan, appendplan->unionrtables);

			for (i = 0; i < indent; i++)
			{
				appendStringInfo(str, "  ");
			}
			appendStringInfo(str, "    ->  ");

			explain_outNode(str, subnode, indent + 4, es);

			whichplan++;
		}
		es->rtable = saved_rtable;
	}
	return;
}

static char *
Explain_PlanToString(Plan *plan, ExplainState *es)
{
	StringInfo	str;
	char	   *s;

	if (plan == NULL)
		return "";
	Assert(plan != NULL);
	str = makeStringInfo();
	explain_outNode(str, plan, 0, es);
	s = str->data;
	pfree(str);

	return s;
}

/*
 * Print a message that might exceed the size of the elog message buffer.
 * This is a crock ... there shouldn't be an upper limit to what you can elog().
 */
static void
printLongNotice(const char * header, const char * message)
{
	int		len = strlen(message);

	elog(NOTICE, "%.20s%.*s", header, ELOG_MAXLEN - 64, message);
	len -= ELOG_MAXLEN - 64;
	while (len > 0)
	{
		message += ELOG_MAXLEN - 64;
		elog(NOTICE, "%.*s", ELOG_MAXLEN - 64, message);
		len -= ELOG_MAXLEN - 64;
	}
}
