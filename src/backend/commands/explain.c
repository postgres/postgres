/*
 * explain.c
 *	  Explain the query execution plan
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/commands/explain.c,v 1.68 2002/02/26 22:47:04 tgl Exp $
 *
 */

#include "postgres.h"

#include "commands/explain.h"
#include "executor/instrument.h"
#include "lib/stringinfo.h"
#include "nodes/print.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "utils/relcache.h"

typedef struct ExplainState
{
	/* options */
	bool		printCost;		/* print cost */
	bool		printNodes;		/* do nodeToString() instead */
	/* other states */
	List	   *rtable;			/* range table */
} ExplainState;

static StringInfo Explain_PlanToString(Plan *plan, ExplainState *es);
static void ExplainOneQuery(Query *query, bool verbose, bool analyze, CommandDest dest);

/* Convert a null string pointer into "<>" */
#define stringStringInfo(s) (((s) == NULL) ? "<>" : (s))


/*
 * ExplainQuery -
 *	  print out the execution plan for a given query
 *
 */
void
ExplainQuery(Query *query, bool verbose, bool analyze, CommandDest dest)
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

	/* rewriter will not cope with utility statements */
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
		ExplainOneQuery(lfirst(l), verbose, analyze, dest);
}

/*
 * ExplainOneQuery -
 *	  print out the execution plan for one query
 *
 */
static void
ExplainOneQuery(Query *query, bool verbose, bool analyze, CommandDest dest)
{
	Plan	   *plan;
	ExplainState *es;
	double		totaltime = 0;

	/* planner will not cope with utility statements */
	if (query->commandType == CMD_UTILITY)
	{
		if (query->utilityStmt && IsA(query->utilityStmt, NotifyStmt))
			elog(NOTICE, "QUERY PLAN:\n\nNOTIFY\n");
		else
			elog(NOTICE, "QUERY PLAN:\n\nUTILITY\n");
		return;
	}

	/* plan the query */
	plan = planner(query);

	/* pg_plan could have failed */
	if (plan == NULL)
		return;

	/* Execute the plan for statistics if asked for */
	if (analyze)
	{
		struct timeval starttime;
		struct timeval endtime;

		/*
		 * Set up the instrumentation for the top node. This will cascade
		 * during plan initialisation
		 */
		plan->instrument = InstrAlloc();

		gettimeofday(&starttime, NULL);
		ProcessQuery(query, plan, None, NULL);
		CommandCounterIncrement();
		gettimeofday(&endtime, NULL);

		endtime.tv_sec -= starttime.tv_sec;
		endtime.tv_usec -= starttime.tv_usec;
		while (endtime.tv_usec < 0)
		{
			endtime.tv_usec += 1000000;
			endtime.tv_sec--;
		}
		totaltime = (double) endtime.tv_sec +
			(double) endtime.tv_usec / 1000000.0;
	}

	es = (ExplainState *) palloc(sizeof(ExplainState));
	MemSet(es, 0, sizeof(ExplainState));

	es->printCost = true;		/* default */

	if (verbose)
		es->printNodes = true;

	es->rtable = query->rtable;

	if (es->printNodes)
	{
		char	   *s;

		s = nodeToString(plan);
		if (s)
		{
			elog(NOTICE, "QUERY DUMP:\n\n%s", s);
			pfree(s);
		}
	}

	if (es->printCost)
	{
		StringInfo	str;

		str = Explain_PlanToString(plan, es);
		if (analyze)
			appendStringInfo(str, "Total runtime: %.2f msec\n",
							 1000.0 * totaltime);
		elog(NOTICE, "QUERY PLAN:\n\n%s", str->data);
		pfree(str->data);
		pfree(str);
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
		case T_TidScan:
			pname = "Tid Scan";
			break;
		case T_SubqueryScan:
			pname = "Subquery Scan";
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
		case T_SetOp:
			switch (((SetOp *) plan)->cmd)
			{
				case SETOPCMD_INTERSECT:
					pname = "SetOp Intersect";
					break;
				case SETOPCMD_INTERSECT_ALL:
					pname = "SetOp Intersect All";
					break;
				case SETOPCMD_EXCEPT:
					pname = "SetOp Except";
					break;
				case SETOPCMD_EXCEPT_ALL:
					pname = "SetOp Except All";
					break;
				default:
					pname = "SetOp ???";
					break;
			}
			break;
		case T_Limit:
			pname = "Limit";
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
			if (ScanDirectionIsBackward(((IndexScan *) plan)->indxorderdir))
				appendStringInfo(str, " Backward");
			appendStringInfo(str, " using ");
			i = 0;
			foreach(l, ((IndexScan *) plan)->indxid)
			{
				relation = RelationIdGetRelation(lfirsti(l));
				Assert(relation);
				appendStringInfo(str, "%s%s",
								 (++i > 1) ? ", " : "",
					stringStringInfo(RelationGetRelationName(relation)));
				/* drop relcache refcount from RelationIdGetRelation */
				RelationDecrementReferenceCount(relation);
			}
			/* FALL THRU */
		case T_SeqScan:
		case T_TidScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);

				/* Assume it's on a real relation */
				Assert(rte->relname);

				appendStringInfo(str, " on %s",
								 stringStringInfo(rte->relname));
				if (strcmp(rte->eref->relname, rte->relname) != 0)
					appendStringInfo(str, " %s",
								   stringStringInfo(rte->eref->relname));
			}
			break;
		case T_SubqueryScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);

				appendStringInfo(str, " %s",
								 stringStringInfo(rte->eref->relname));
			}
			break;
		default:
			break;
	}
	if (es->printCost)
	{
		appendStringInfo(str, "  (cost=%.2f..%.2f rows=%.0f width=%d)",
						 plan->startup_cost, plan->total_cost,
						 plan->plan_rows, plan->plan_width);

		if (plan->instrument && plan->instrument->nloops > 0)
		{
			double		nloops = plan->instrument->nloops;

			appendStringInfo(str, " (actual time=%.2f..%.2f rows=%.0f loops=%.0f)",
							 1000.0 * plan->instrument->startup / nloops,
							 1000.0 * plan->instrument->total / nloops,
							 plan->instrument->ntuples / nloops,
							 plan->instrument->nloops);
		}
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
			explain_outNode(str, ((SubPlan *) lfirst(lst))->plan,
							indent + 4, es);
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

	if (IsA(plan, Append))
	{
		Append	   *appendplan = (Append *) plan;
		List	   *lst;

		foreach(lst, appendplan->appendplans)
		{
			Plan	   *subnode = (Plan *) lfirst(lst);

			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "  ->  ");

			explain_outNode(str, subnode, indent + 3, es);
		}
	}

	if (IsA(plan, SubqueryScan))
	{
		SubqueryScan *subqueryscan = (SubqueryScan *) plan;
		Plan	   *subnode = subqueryscan->subplan;
		RangeTblEntry *rte = rt_fetch(subqueryscan->scan.scanrelid,
									  es->rtable);
		List	   *saved_rtable = es->rtable;

		Assert(rte->subquery != NULL);
		es->rtable = rte->subquery->rtable;

		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  ->  ");

		explain_outNode(str, subnode, indent + 3, es);

		es->rtable = saved_rtable;
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
			explain_outNode(str, ((SubPlan *) lfirst(lst))->plan,
							indent + 4, es);
		}
		es->rtable = saved_rtable;
	}
}

static StringInfo
Explain_PlanToString(Plan *plan, ExplainState *es)
{
	StringInfo	str = makeStringInfo();

	if (plan != NULL)
		explain_outNode(str, plan, 0, es);
	return str;
}
