/*
 * explain.c
 *	  Explain the query execution plan
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/commands/explain.c,v 1.76 2002/05/03 15:56:45 tgl Exp $
 *
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "executor/instrument.h"
#include "lib/stringinfo.h"
#include "nodes/print.h"
#include "optimizer/clauses.h"
#include "optimizer/planner.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"


typedef struct ExplainState
{
	/* options */
	bool		printCost;		/* print cost */
	bool		printNodes;		/* do nodeToString() instead */
	/* other states */
	List	   *rtable;			/* range table */
} ExplainState;

typedef struct TextOutputState
{
	TupleDesc	tupdesc;
	DestReceiver *destfunc;
} TextOutputState;

static StringInfo Explain_PlanToString(Plan *plan, ExplainState *es);
static void ExplainOneQuery(Query *query, ExplainStmt *stmt,
							TextOutputState *tstate);
static void explain_outNode(StringInfo str, Plan *plan, Plan *outer_plan,
							int indent, ExplainState *es);
static void show_scan_qual(List *qual, bool is_or_qual, const char *qlabel,
						   int scanrelid, Plan *outer_plan,
						   StringInfo str, int indent, ExplainState *es);
static void show_upper_qual(List *qual, const char *qlabel,
							const char *outer_name, int outer_varno, Plan *outer_plan,
							const char *inner_name, int inner_varno, Plan *inner_plan,
							StringInfo str, int indent, ExplainState *es);
static Node *make_ors_ands_explicit(List *orclauses);
static TextOutputState *begin_text_output(CommandDest dest, char *title);
static void do_text_output(TextOutputState *tstate, char *aline);
static void do_text_output_multiline(TextOutputState *tstate, char *text);
static void end_text_output(TextOutputState *tstate);


/*
 * ExplainQuery -
 *	  execute an EXPLAIN command
 */
void
ExplainQuery(ExplainStmt *stmt, CommandDest dest)
{
	Query	   *query = stmt->query;
	TextOutputState *tstate;
	List	   *rewritten;
	List	   *l;

	tstate = begin_text_output(dest, "QUERY PLAN");

	if (query->commandType == CMD_UTILITY)
	{
		/* rewriter will not cope with utility statements */
		do_text_output(tstate, "Utility statements have no plan structure");
	}
	else
	{
		/* Rewrite through rule system */
		rewritten = QueryRewrite(query);

		if (rewritten == NIL)
		{
			/* In the case of an INSTEAD NOTHING, tell at least that */
			do_text_output(tstate, "Query rewrites to nothing");
		}
		else
		{
			/* Explain every plan */
			foreach(l, rewritten)
			{
				ExplainOneQuery(lfirst(l), stmt, tstate);
				/* put a blank line between plans */
				if (lnext(l) != NIL)
					do_text_output(tstate, "");
			}
		}
	}

	end_text_output(tstate);
}

/*
 * ExplainOneQuery -
 *	  print out the execution plan for one query
 */
static void
ExplainOneQuery(Query *query, ExplainStmt *stmt, TextOutputState *tstate)
{
	Plan	   *plan;
	ExplainState *es;
	double		totaltime = 0;

	/* planner will not cope with utility statements */
	if (query->commandType == CMD_UTILITY)
	{
		if (query->utilityStmt && IsA(query->utilityStmt, NotifyStmt))
			do_text_output(tstate, "NOTIFY");
		else
			do_text_output(tstate, "UTILITY");
		return;
	}

	/* plan the query */
	plan = planner(query);

	/* pg_plan could have failed */
	if (plan == NULL)
		return;

	/* Execute the plan for statistics if asked for */
	if (stmt->analyze)
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

	if (stmt->verbose)
		es->printNodes = true;

	es->rtable = query->rtable;

	if (es->printNodes)
	{
		char	   *s;
		char	   *f;

		s = nodeToString(plan);
		if (s)
		{
			if (Explain_pretty_print)
				f = pretty_format_node_dump(s);
			else
				f = format_node_dump(s);
			pfree(s);
			do_text_output_multiline(tstate, f);
			pfree(f);
			if (es->printCost)
				do_text_output(tstate, "");	/* separator line */
		}
	}

	if (es->printCost)
	{
		StringInfo	str;

		str = Explain_PlanToString(plan, es);
		if (stmt->analyze)
			appendStringInfo(str, "Total runtime: %.2f msec\n",
							 1000.0 * totaltime);
		do_text_output_multiline(tstate, str->data);
		pfree(str->data);
		pfree(str);
	}

	pfree(es);
}


/*
 * explain_outNode -
 *	  converts a Plan node into ascii string and appends it to 'str'
 *
 * outer_plan, if not null, references another plan node that is the outer
 * side of a join with the current node.  This is only interesting for
 * deciphering runtime keys of an inner indexscan.
 */
static void
explain_outNode(StringInfo str, Plan *plan, Plan *outer_plan,
				int indent, ExplainState *es)
{
	List	   *l;
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
				Relation	relation;

				relation = index_open(lfirsti(l));
				appendStringInfo(str, "%s%s",
								 (++i > 1) ? ", " : "",
								 quote_identifier(RelationGetRelationName(relation)));
				index_close(relation);
			}
			/* FALL THRU */
		case T_SeqScan:
		case T_TidScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);
				char   *relname;

				/* Assume it's on a real relation */
				Assert(rte->relid);

				/* We only show the rel name, not schema name */
				relname = get_rel_name(rte->relid);

				appendStringInfo(str, " on %s",
								 quote_identifier(relname));
				if (strcmp(rte->eref->aliasname, relname) != 0)
					appendStringInfo(str, " %s",
									 quote_identifier(rte->eref->aliasname));
			}
			break;
		case T_SubqueryScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);

				appendStringInfo(str, " %s",
								 quote_identifier(rte->eref->aliasname));
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

	/* quals */
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			show_scan_qual(((IndexScan *) plan)->indxqualorig, true,
						   "Index Cond",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			show_scan_qual(plan->qual, false,
						   "Filter",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			break;
		case T_SeqScan:
		case T_TidScan:
			show_scan_qual(plan->qual, false,
						   "Filter",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			break;
		case T_NestLoop:
			show_upper_qual(((NestLoop *) plan)->join.joinqual,
							"Join Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			show_upper_qual(plan->qual,
							"Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			break;
		case T_MergeJoin:
			show_upper_qual(((MergeJoin *) plan)->mergeclauses,
							"Merge Cond",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			show_upper_qual(((MergeJoin *) plan)->join.joinqual,
							"Join Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			show_upper_qual(plan->qual,
							"Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			break;
		case T_HashJoin:
			show_upper_qual(((HashJoin *) plan)->hashclauses,
							"Hash Cond",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			show_upper_qual(((HashJoin *) plan)->join.joinqual,
							"Join Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			show_upper_qual(plan->qual,
							"Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			break;
		case T_SubqueryScan:
			show_upper_qual(plan->qual,
							"Filter",
							"subplan", 1, ((SubqueryScan *) plan)->subplan,
							"", 0, NULL,
							str, indent, es);
			break;
		case T_Agg:
		case T_Group:
			show_upper_qual(plan->qual,
							"Filter",
							"subplan", 0, outerPlan(plan),
							"", 0, NULL,
							str, indent, es);
			break;
		case T_Result:
			show_upper_qual((List *) ((Result *) plan)->resconstantqual,
							"One-Time Filter",
							"subplan", OUTER, outerPlan(plan),
							"", 0, NULL,
							str, indent, es);
			show_upper_qual(plan->qual,
							"Filter",
							"subplan", OUTER, outerPlan(plan),
							"", 0, NULL,
							str, indent, es);
			break;
		default:
			break;
	}

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
			explain_outNode(str, ((SubPlan *) lfirst(lst))->plan, NULL,
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
		explain_outNode(str, outerPlan(plan), NULL, indent + 3, es);
	}

	/* righttree */
	if (innerPlan(plan))
	{
		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  ->  ");
		explain_outNode(str, innerPlan(plan), outerPlan(plan),
						indent + 3, es);
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

			explain_outNode(str, subnode, NULL, indent + 3, es);
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

		explain_outNode(str, subnode, NULL, indent + 3, es);

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
			explain_outNode(str, ((SubPlan *) lfirst(lst))->plan, NULL,
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
		explain_outNode(str, plan, NULL, 0, es);
	return str;
}

/*
 * Show a qualifier expression for a scan plan node
 */
static void
show_scan_qual(List *qual, bool is_or_qual, const char *qlabel,
			   int scanrelid, Plan *outer_plan,
			   StringInfo str, int indent, ExplainState *es)
{
	RangeTblEntry *rte;
	Node	   *scancontext;
	Node	   *outercontext;
	List	   *context;
	Node	   *node;
	char	   *exprstr;
	int			i;

	/* No work if empty qual */
	if (qual == NIL)
		return;
	if (is_or_qual)
	{
		if (lfirst(qual) == NIL && lnext(qual) == NIL)
			return;
	}

	/* Fix qual --- indexqual requires different processing */
	if (is_or_qual)
		node = make_ors_ands_explicit(qual);
	else
		node = (Node *) make_ands_explicit(qual);

	/* Generate deparse context */
	Assert(scanrelid > 0 && scanrelid <= length(es->rtable));
	rte = rt_fetch(scanrelid, es->rtable);

	/* Assume it's on a real relation */
	Assert(rte->relid);
	scancontext = deparse_context_for_relation(rte->eref->aliasname,
											   rte->relid);

	/*
	 * If we have an outer plan that is referenced by the qual, add it to
	 * the deparse context.  If not, don't (so that we don't force prefixes
	 * unnecessarily).
	 */
	if (outer_plan)
	{
		if (intMember(OUTER, pull_varnos(node)))
			outercontext = deparse_context_for_subplan("outer",
													   outer_plan->targetlist,
													   es->rtable);
		else
			outercontext = NULL;
	}
	else
		outercontext = NULL;

	context = deparse_context_for_plan(scanrelid, scancontext,
									   OUTER, outercontext);

	/* Deparse the expression */
	exprstr = deparse_expression(node, context, (outercontext != NULL));

	/* And add to str */
	for (i = 0; i < indent; i++)
		appendStringInfo(str, "  ");
	appendStringInfo(str, "  %s: %s\n", qlabel, exprstr);
}

/*
 * Show a qualifier expression for an upper-level plan node
 */
static void
show_upper_qual(List *qual, const char *qlabel,
				const char *outer_name, int outer_varno, Plan *outer_plan,
				const char *inner_name, int inner_varno, Plan *inner_plan,
				StringInfo str, int indent, ExplainState *es)
{
	List	   *context;
	Node	   *outercontext;
	Node	   *innercontext;
	Node	   *node;
	char	   *exprstr;
	int			i;

	/* No work if empty qual */
	if (qual == NIL)
		return;

	/* Generate deparse context */
	if (outer_plan)
		outercontext = deparse_context_for_subplan(outer_name,
												   outer_plan->targetlist,
												   es->rtable);
	else
		outercontext = NULL;
	if (inner_plan)
		innercontext = deparse_context_for_subplan(inner_name,
												   inner_plan->targetlist,
												   es->rtable);
	else
		innercontext = NULL;
	context = deparse_context_for_plan(outer_varno, outercontext,
									   inner_varno, innercontext);

	/* Deparse the expression */
	node = (Node *) make_ands_explicit(qual);
	exprstr = deparse_expression(node, context, (inner_plan != NULL));

	/* And add to str */
	for (i = 0; i < indent; i++)
		appendStringInfo(str, "  ");
	appendStringInfo(str, "  %s: %s\n", qlabel, exprstr);
}

/*
 * Indexscan qual lists have an implicit OR-of-ANDs structure.  Make it
 * explicit so deparsing works properly.
 */
static Node *
make_ors_ands_explicit(List *orclauses)
{
	if (orclauses == NIL)
		return NULL;			/* probably can't happen */
	else if (lnext(orclauses) == NIL)
		return (Node *) make_ands_explicit(lfirst(orclauses));
	else
	{
		List   *args = NIL;
		List   *orptr;

		foreach(orptr, orclauses)
		{
			args = lappend(args, make_ands_explicit(lfirst(orptr)));
		}

		return (Node *) make_orclause(args);
	}
}


/*
 * Functions for sending text to the frontend (or other specified destination)
 * as though it is a SELECT result.
 *
 * We tell the frontend that the table structure is a single TEXT column.
 */

static TextOutputState *
begin_text_output(CommandDest dest, char *title)
{
	TextOutputState *tstate;
	TupleDesc	tupdesc;

	tstate = (TextOutputState *) palloc(sizeof(TextOutputState));

	/* need a tuple descriptor representing a single TEXT column */
	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, title,
					   TEXTOID, -1, 0, false);

	tstate->tupdesc = tupdesc;
	tstate->destfunc = DestToFunction(dest);

	(*tstate->destfunc->setup) (tstate->destfunc, (int) CMD_SELECT,
								NULL, tupdesc);

	return tstate;
}

/* write a single line of text */
static void
do_text_output(TextOutputState *tstate, char *aline)
{
	HeapTuple	tuple;
	Datum		values[1];
	char		nulls[1];

	/* form a tuple and send it to the receiver */
	values[0] = DirectFunctionCall1(textin, CStringGetDatum(aline));
	nulls[0] = ' ';
	tuple = heap_formtuple(tstate->tupdesc, values, nulls);
	(*tstate->destfunc->receiveTuple) (tuple,
									   tstate->tupdesc,
									   tstate->destfunc);
	pfree(DatumGetPointer(values[0]));
	heap_freetuple(tuple);
}

/* write a chunk of text, breaking at newline characters */
/* NB: scribbles on its input! */
static void
do_text_output_multiline(TextOutputState *tstate, char *text)
{
	while (*text)
	{
		char   *eol;

		eol = strchr(text, '\n');
		if (eol)
			*eol++ = '\0';
		else
			eol = text + strlen(text);
		do_text_output(tstate, text);
		text = eol;
	}
}

static void
end_text_output(TextOutputState *tstate)
{
	(*tstate->destfunc->cleanup) (tstate->destfunc);
	pfree(tstate);
}
