/*-------------------------------------------------------------------------
 *
 * explain.c
 *	  Explain query execution plans
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/explain.c,v 1.152 2006/10/04 00:29:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/prepare.h"
#include "commands/trigger.h"
#include "executor/instrument.h"
#include "nodes/print.h"
#include "optimizer/clauses.h"
#include "optimizer/planner.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"


typedef struct ExplainState
{
	/* options */
	bool		printNodes;		/* do nodeToString() too */
	bool		printAnalyze;	/* print actual times */
	/* other states */
	List	   *rtable;			/* range table */
} ExplainState;

static void ExplainOneQuery(Query *query, ExplainStmt *stmt,
				ParamListInfo params, TupOutputState *tstate);
static double elapsed_time(instr_time *starttime);
static void explain_outNode(StringInfo str,
				Plan *plan, PlanState *planstate,
				Plan *outer_plan,
				int indent, ExplainState *es);
static void show_scan_qual(List *qual, const char *qlabel,
			   int scanrelid, Plan *outer_plan,
			   StringInfo str, int indent, ExplainState *es);
static void show_upper_qual(List *qual, const char *qlabel,
				const char *outer_name, int outer_varno, Plan *outer_plan,
				const char *inner_name, int inner_varno, Plan *inner_plan,
				StringInfo str, int indent, ExplainState *es);
static void show_sort_keys(Plan *sortplan, int nkeys, AttrNumber *keycols,
			   const char *qlabel,
			   StringInfo str, int indent, ExplainState *es);

/*
 * ExplainQuery -
 *	  execute an EXPLAIN command
 */
void
ExplainQuery(ExplainStmt *stmt, ParamListInfo params, DestReceiver *dest)
{
	Query	   *query = stmt->query;
	TupOutputState *tstate;
	List	   *rewritten;
	ListCell   *l;

	/*
	 * Because the planner is not cool about not scribbling on its input, we
	 * make a preliminary copy of the source querytree.  This prevents
	 * problems in the case that the EXPLAIN is in a portal or plpgsql
	 * function and is executed repeatedly.  (See also the same hack in
	 * DECLARE CURSOR and PREPARE.)  XXX the planner really shouldn't modify
	 * its input ... FIXME someday.
	 */
	query = copyObject(query);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, ExplainResultDesc(stmt));

	if (query->commandType == CMD_UTILITY)
	{
		/* Rewriter will not cope with utility statements */
		if (query->utilityStmt && IsA(query->utilityStmt, DeclareCursorStmt))
			ExplainOneQuery(query, stmt, params, tstate);
		else if (query->utilityStmt && IsA(query->utilityStmt, ExecuteStmt))
			ExplainExecuteQuery(stmt, params, tstate);
		else
			do_text_output_oneline(tstate, "Utility statements have no plan structure");
	}
	else
	{
		/*
		 * Must acquire locks in case we didn't come fresh from the parser.
		 * XXX this also scribbles on query, another reason for copyObject
		 */
		AcquireRewriteLocks(query);

		/* Rewrite through rule system */
		rewritten = QueryRewrite(query);

		if (rewritten == NIL)
		{
			/* In the case of an INSTEAD NOTHING, tell at least that */
			do_text_output_oneline(tstate, "Query rewrites to nothing");
		}
		else
		{
			/* Explain every plan */
			foreach(l, rewritten)
			{
				ExplainOneQuery(lfirst(l), stmt, params, tstate);
				/* put a blank line between plans */
				if (lnext(l) != NULL)
					do_text_output_oneline(tstate, "");
			}
		}
	}

	end_tup_output(tstate);
}

/*
 * ExplainResultDesc -
 *	  construct the result tupledesc for an EXPLAIN
 */
TupleDesc
ExplainResultDesc(ExplainStmt *stmt)
{
	TupleDesc	tupdesc;

	/* need a tuple descriptor representing a single TEXT column */
	tupdesc = CreateTemplateTupleDesc(1, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "QUERY PLAN",
					   TEXTOID, -1, 0);
	return tupdesc;
}

/*
 * ExplainOneQuery -
 *	  print out the execution plan for one query
 */
static void
ExplainOneQuery(Query *query, ExplainStmt *stmt, ParamListInfo params,
				TupOutputState *tstate)
{
	Plan	   *plan;
	QueryDesc  *queryDesc;
	bool		isCursor = false;
	int			cursorOptions = 0;

	/* planner will not cope with utility statements */
	if (query->commandType == CMD_UTILITY)
	{
		if (query->utilityStmt && IsA(query->utilityStmt, DeclareCursorStmt))
		{
			DeclareCursorStmt *dcstmt;
			List	   *rewritten;

			dcstmt = (DeclareCursorStmt *) query->utilityStmt;
			query = (Query *) dcstmt->query;
			isCursor = true;
			cursorOptions = dcstmt->options;
			/* Still need to rewrite cursor command */
			Assert(query->commandType == CMD_SELECT);
			/* get locks (we assume ExplainQuery already copied tree) */
			AcquireRewriteLocks(query);
			rewritten = QueryRewrite(query);
			if (list_length(rewritten) != 1)
				elog(ERROR, "unexpected rewrite result");
			query = (Query *) linitial(rewritten);
			Assert(query->commandType == CMD_SELECT);
			/* do not actually execute the underlying query! */
			stmt->analyze = false;
		}
		else if (query->utilityStmt && IsA(query->utilityStmt, NotifyStmt))
		{
			do_text_output_oneline(tstate, "NOTIFY");
			return;
		}
		else
		{
			do_text_output_oneline(tstate, "UTILITY");
			return;
		}
	}

	/* plan the query */
	plan = planner(query, isCursor, cursorOptions, params);

	/*
	 * Update snapshot command ID to ensure this query sees results of any
	 * previously executed queries.  (It's a bit cheesy to modify
	 * ActiveSnapshot without making a copy, but for the limited ways in which
	 * EXPLAIN can be invoked, I think it's OK, because the active snapshot
	 * shouldn't be shared with anything else anyway.)
	 */
	ActiveSnapshot->curcid = GetCurrentCommandId();

	/* Create a QueryDesc requesting no output */
	queryDesc = CreateQueryDesc(query, plan,
								ActiveSnapshot, InvalidSnapshot,
								None_Receiver, params,
								stmt->analyze);

	ExplainOnePlan(queryDesc, stmt, tstate);
}

/*
 * ExplainOnePlan -
 *		given a planned query, execute it if needed, and then print
 *		EXPLAIN output
 *
 * This is exported because it's called back from prepare.c in the
 * EXPLAIN EXECUTE case
 *
 * Note: the passed-in QueryDesc is freed when we're done with it
 */
void
ExplainOnePlan(QueryDesc *queryDesc, ExplainStmt *stmt,
			   TupOutputState *tstate)
{
	instr_time	starttime;
	double		totaltime = 0;
	ExplainState *es;
	StringInfoData buf;
	int			eflags;

	INSTR_TIME_SET_CURRENT(starttime);

	/* If analyzing, we need to cope with queued triggers */
	if (stmt->analyze)
		AfterTriggerBeginQuery();

	/* Select execution options */
	if (stmt->analyze)
		eflags = 0;				/* default run-to-completion flags */
	else
		eflags = EXEC_FLAG_EXPLAIN_ONLY;

	/* call ExecutorStart to prepare the plan for execution */
	ExecutorStart(queryDesc, eflags);

	/* Execute the plan for statistics if asked for */
	if (stmt->analyze)
	{
		/* run the plan */
		ExecutorRun(queryDesc, ForwardScanDirection, 0L);

		/* We can't clean up 'till we're done printing the stats... */
		totaltime += elapsed_time(&starttime);
	}

	es = (ExplainState *) palloc0(sizeof(ExplainState));

	es->printNodes = stmt->verbose;
	es->printAnalyze = stmt->analyze;
	es->rtable = queryDesc->parsetree->rtable;

	if (es->printNodes)
	{
		char	   *s;
		char	   *f;

		s = nodeToString(queryDesc->plantree);
		if (s)
		{
			if (Explain_pretty_print)
				f = pretty_format_node_dump(s);
			else
				f = format_node_dump(s);
			pfree(s);
			do_text_output_multiline(tstate, f);
			pfree(f);
			do_text_output_oneline(tstate, ""); /* separator line */
		}
	}

	initStringInfo(&buf);
	explain_outNode(&buf, queryDesc->plantree, queryDesc->planstate,
					NULL, 0, es);

	/*
	 * If we ran the command, run any AFTER triggers it queued.  (Note this
	 * will not include DEFERRED triggers; since those don't run until end of
	 * transaction, we can't measure them.)  Include into total runtime.
	 */
	if (stmt->analyze)
	{
		INSTR_TIME_SET_CURRENT(starttime);
		AfterTriggerEndQuery(queryDesc->estate);
		totaltime += elapsed_time(&starttime);
	}

	/* Print info about runtime of triggers */
	if (es->printAnalyze)
	{
		ResultRelInfo *rInfo;
		int			numrels = queryDesc->estate->es_num_result_relations;
		int			nr;

		rInfo = queryDesc->estate->es_result_relations;
		for (nr = 0; nr < numrels; rInfo++, nr++)
		{
			int			nt;

			if (!rInfo->ri_TrigDesc || !rInfo->ri_TrigInstrument)
				continue;
			for (nt = 0; nt < rInfo->ri_TrigDesc->numtriggers; nt++)
			{
				Trigger    *trig = rInfo->ri_TrigDesc->triggers + nt;
				Instrumentation *instr = rInfo->ri_TrigInstrument + nt;
				char	   *conname;

				/* Must clean up instrumentation state */
				InstrEndLoop(instr);

				/*
				 * We ignore triggers that were never invoked; they likely
				 * aren't relevant to the current query type.
				 */
				if (instr->ntuples == 0)
					continue;

				if (trig->tgisconstraint &&
				(conname = GetConstraintNameForTrigger(trig->tgoid)) != NULL)
				{
					appendStringInfo(&buf, "Trigger for constraint %s",
									 conname);
					pfree(conname);
				}
				else
					appendStringInfo(&buf, "Trigger %s", trig->tgname);

				if (numrels > 1)
					appendStringInfo(&buf, " on %s",
							RelationGetRelationName(rInfo->ri_RelationDesc));

				appendStringInfo(&buf, ": time=%.3f calls=%.0f\n",
								 1000.0 * instr->total,
								 instr->ntuples);
			}
		}
	}

	/*
	 * Close down the query and free resources.  Include time for this in the
	 * total runtime (although it should be pretty minimal).
	 */
	INSTR_TIME_SET_CURRENT(starttime);

	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	/* We need a CCI just in case query expanded to multiple plans */
	if (stmt->analyze)
		CommandCounterIncrement();

	totaltime += elapsed_time(&starttime);

	if (stmt->analyze)
		appendStringInfo(&buf, "Total runtime: %.3f ms\n",
						 1000.0 * totaltime);
	do_text_output_multiline(tstate, buf.data);

	pfree(buf.data);
	pfree(es);
}

/* Compute elapsed time in seconds since given timestamp */
static double
elapsed_time(instr_time *starttime)
{
	instr_time	endtime;

	INSTR_TIME_SET_CURRENT(endtime);

#ifndef WIN32
	endtime.tv_sec -= starttime->tv_sec;
	endtime.tv_usec -= starttime->tv_usec;
	while (endtime.tv_usec < 0)
	{
		endtime.tv_usec += 1000000;
		endtime.tv_sec--;
	}
#else							/* WIN32 */
	endtime.QuadPart -= starttime->QuadPart;
#endif

	return INSTR_TIME_GET_DOUBLE(endtime);
}

/*
 * explain_outNode -
 *	  converts a Plan node into ascii string and appends it to 'str'
 *
 * planstate points to the executor state node corresponding to the plan node.
 * We need this to get at the instrumentation data (if any) as well as the
 * list of subplans.
 *
 * outer_plan, if not null, references another plan node that is the outer
 * side of a join with the current node.  This is only interesting for
 * deciphering runtime keys of an inner indexscan.
 */
static void
explain_outNode(StringInfo str,
				Plan *plan, PlanState *planstate,
				Plan *outer_plan,
				int indent, ExplainState *es)
{
	char	   *pname;
	int			i;

	if (plan == NULL)
	{
		appendStringInfoChar(str, '\n');
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
		case T_BitmapAnd:
			pname = "BitmapAnd";
			break;
		case T_BitmapOr:
			pname = "BitmapOr";
			break;
		case T_NestLoop:
			switch (((NestLoop *) plan)->join.jointype)
			{
				case JOIN_INNER:
					pname = "Nested Loop";
					break;
				case JOIN_LEFT:
					pname = "Nested Loop Left Join";
					break;
				case JOIN_FULL:
					pname = "Nested Loop Full Join";
					break;
				case JOIN_RIGHT:
					pname = "Nested Loop Right Join";
					break;
				case JOIN_IN:
					pname = "Nested Loop IN Join";
					break;
				default:
					pname = "Nested Loop ??? Join";
					break;
			}
			break;
		case T_MergeJoin:
			switch (((MergeJoin *) plan)->join.jointype)
			{
				case JOIN_INNER:
					pname = "Merge Join";
					break;
				case JOIN_LEFT:
					pname = "Merge Left Join";
					break;
				case JOIN_FULL:
					pname = "Merge Full Join";
					break;
				case JOIN_RIGHT:
					pname = "Merge Right Join";
					break;
				case JOIN_IN:
					pname = "Merge IN Join";
					break;
				default:
					pname = "Merge ??? Join";
					break;
			}
			break;
		case T_HashJoin:
			switch (((HashJoin *) plan)->join.jointype)
			{
				case JOIN_INNER:
					pname = "Hash Join";
					break;
				case JOIN_LEFT:
					pname = "Hash Left Join";
					break;
				case JOIN_FULL:
					pname = "Hash Full Join";
					break;
				case JOIN_RIGHT:
					pname = "Hash Right Join";
					break;
				case JOIN_IN:
					pname = "Hash IN Join";
					break;
				default:
					pname = "Hash ??? Join";
					break;
			}
			break;
		case T_SeqScan:
			pname = "Seq Scan";
			break;
		case T_IndexScan:
			pname = "Index Scan";
			break;
		case T_BitmapIndexScan:
			pname = "Bitmap Index Scan";
			break;
		case T_BitmapHeapScan:
			pname = "Bitmap Heap Scan";
			break;
		case T_TidScan:
			pname = "Tid Scan";
			break;
		case T_SubqueryScan:
			pname = "Subquery Scan";
			break;
		case T_FunctionScan:
			pname = "Function Scan";
			break;
		case T_ValuesScan:
			pname = "Values Scan";
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
			switch (((Agg *) plan)->aggstrategy)
			{
				case AGG_PLAIN:
					pname = "Aggregate";
					break;
				case AGG_SORTED:
					pname = "GroupAggregate";
					break;
				case AGG_HASHED:
					pname = "HashAggregate";
					break;
				default:
					pname = "Aggregate ???";
					break;
			}
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

	appendStringInfoString(str, pname);
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			if (ScanDirectionIsBackward(((IndexScan *) plan)->indexorderdir))
				appendStringInfoString(str, " Backward");
			appendStringInfo(str, " using %s",
			  quote_identifier(get_rel_name(((IndexScan *) plan)->indexid)));
			/* FALL THRU */
		case T_SeqScan:
		case T_BitmapHeapScan:
		case T_TidScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);
				char	   *relname;

				/* Assume it's on a real relation */
				Assert(rte->rtekind == RTE_RELATION);

				/* We only show the rel name, not schema name */
				relname = get_rel_name(rte->relid);

				appendStringInfo(str, " on %s",
								 quote_identifier(relname));
				if (strcmp(rte->eref->aliasname, relname) != 0)
					appendStringInfo(str, " %s",
									 quote_identifier(rte->eref->aliasname));
			}
			break;
		case T_BitmapIndexScan:
			appendStringInfo(str, " on %s",
							 quote_identifier(get_rel_name(((BitmapIndexScan *) plan)->indexid)));
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
		case T_FunctionScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);
				char	   *proname;

				/* Assert it's on a RangeFunction */
				Assert(rte->rtekind == RTE_FUNCTION);

				/*
				 * If the expression is still a function call, we can get the
				 * real name of the function.  Otherwise, punt (this can
				 * happen if the optimizer simplified away the function call,
				 * for example).
				 */
				if (rte->funcexpr && IsA(rte->funcexpr, FuncExpr))
				{
					FuncExpr   *funcexpr = (FuncExpr *) rte->funcexpr;
					Oid			funcid = funcexpr->funcid;

					/* We only show the func name, not schema name */
					proname = get_func_name(funcid);
				}
				else
					proname = rte->eref->aliasname;

				appendStringInfo(str, " on %s",
								 quote_identifier(proname));
				if (strcmp(rte->eref->aliasname, proname) != 0)
					appendStringInfo(str, " %s",
									 quote_identifier(rte->eref->aliasname));
			}
			break;
		case T_ValuesScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);
				char	   *valsname;

				/* Assert it's on a values rte */
				Assert(rte->rtekind == RTE_VALUES);

				valsname = rte->eref->aliasname;

				appendStringInfo(str, " on %s",
								 quote_identifier(valsname));
			}
			break;
		default:
			break;
	}

	appendStringInfo(str, "  (cost=%.2f..%.2f rows=%.0f width=%d)",
					 plan->startup_cost, plan->total_cost,
					 plan->plan_rows, plan->plan_width);

	/*
	 * We have to forcibly clean up the instrumentation state because we
	 * haven't done ExecutorEnd yet.  This is pretty grotty ...
	 */
	if (planstate->instrument)
		InstrEndLoop(planstate->instrument);

	if (planstate->instrument && planstate->instrument->nloops > 0)
	{
		double		nloops = planstate->instrument->nloops;

		appendStringInfo(str, " (actual time=%.3f..%.3f rows=%.0f loops=%.0f)",
						 1000.0 * planstate->instrument->startup / nloops,
						 1000.0 * planstate->instrument->total / nloops,
						 planstate->instrument->ntuples / nloops,
						 planstate->instrument->nloops);
	}
	else if (es->printAnalyze)
		appendStringInfo(str, " (never executed)");
	appendStringInfoChar(str, '\n');

	/* quals, sort keys, etc */
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			show_scan_qual(((IndexScan *) plan)->indexqualorig,
						   "Index Cond",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			show_scan_qual(plan->qual,
						   "Filter",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			break;
		case T_BitmapIndexScan:
			show_scan_qual(((BitmapIndexScan *) plan)->indexqualorig,
						   "Index Cond",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			break;
		case T_BitmapHeapScan:
			/* XXX do we want to show this in production? */
			show_scan_qual(((BitmapHeapScan *) plan)->bitmapqualorig,
						   "Recheck Cond",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			/* FALL THRU */
		case T_SeqScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_ValuesScan:
			show_scan_qual(plan->qual,
						   "Filter",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			break;
		case T_TidScan:
			{
				/*
				 * The tidquals list has OR semantics, so be sure to show it
				 * as an OR condition.
				 */
				List	   *tidquals = ((TidScan *) plan)->tidquals;

				if (list_length(tidquals) > 1)
					tidquals = list_make1(make_orclause(tidquals));
				show_scan_qual(tidquals,
							   "TID Cond",
							   ((Scan *) plan)->scanrelid,
							   outer_plan,
							   str, indent, es);
				show_scan_qual(plan->qual,
							   "Filter",
							   ((Scan *) plan)->scanrelid,
							   outer_plan,
							   str, indent, es);
			}
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
		case T_Agg:
		case T_Group:
			show_upper_qual(plan->qual,
							"Filter",
							"subplan", 0, outerPlan(plan),
							"", 0, NULL,
							str, indent, es);
			break;
		case T_Sort:
			show_sort_keys(plan,
						   ((Sort *) plan)->numCols,
						   ((Sort *) plan)->sortColIdx,
						   "Sort Key",
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
		ListCell   *lst;

		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  InitPlan\n");
		foreach(lst, planstate->initPlan)
		{
			SubPlanState *sps = (SubPlanState *) lfirst(lst);
			SubPlan    *sp = (SubPlan *) sps->xprstate.expr;

			es->rtable = sp->rtable;
			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "    ->  ");
			explain_outNode(str, sp->plan,
							sps->planstate,
							NULL,
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

		/*
		 * Ordinarily we don't pass down our own outer_plan value to our child
		 * nodes, but in bitmap scan trees we must, since the bottom
		 * BitmapIndexScan nodes may have outer references.
		 */
		explain_outNode(str, outerPlan(plan),
						outerPlanState(planstate),
						IsA(plan, BitmapHeapScan) ? outer_plan : NULL,
						indent + 3, es);
	}

	/* righttree */
	if (innerPlan(plan))
	{
		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  ->  ");
		explain_outNode(str, innerPlan(plan),
						innerPlanState(planstate),
						outerPlan(plan),
						indent + 3, es);
	}

	if (IsA(plan, Append))
	{
		Append	   *appendplan = (Append *) plan;
		AppendState *appendstate = (AppendState *) planstate;
		ListCell   *lst;
		int			j;

		j = 0;
		foreach(lst, appendplan->appendplans)
		{
			Plan	   *subnode = (Plan *) lfirst(lst);

			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "  ->  ");

			/*
			 * Ordinarily we don't pass down our own outer_plan value to our
			 * child nodes, but in an Append we must, since we might be
			 * looking at an appendrel indexscan with outer references from
			 * the member scans.
			 */
			explain_outNode(str, subnode,
							appendstate->appendplans[j],
							outer_plan,
							indent + 3, es);
			j++;
		}
	}

	if (IsA(plan, BitmapAnd))
	{
		BitmapAnd  *bitmapandplan = (BitmapAnd *) plan;
		BitmapAndState *bitmapandstate = (BitmapAndState *) planstate;
		ListCell   *lst;
		int			j;

		j = 0;
		foreach(lst, bitmapandplan->bitmapplans)
		{
			Plan	   *subnode = (Plan *) lfirst(lst);

			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "  ->  ");

			explain_outNode(str, subnode,
							bitmapandstate->bitmapplans[j],
							outer_plan, /* pass down same outer plan */
							indent + 3, es);
			j++;
		}
	}

	if (IsA(plan, BitmapOr))
	{
		BitmapOr   *bitmaporplan = (BitmapOr *) plan;
		BitmapOrState *bitmaporstate = (BitmapOrState *) planstate;
		ListCell   *lst;
		int			j;

		j = 0;
		foreach(lst, bitmaporplan->bitmapplans)
		{
			Plan	   *subnode = (Plan *) lfirst(lst);

			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "  ->  ");

			explain_outNode(str, subnode,
							bitmaporstate->bitmapplans[j],
							outer_plan, /* pass down same outer plan */
							indent + 3, es);
			j++;
		}
	}

	if (IsA(plan, SubqueryScan))
	{
		SubqueryScan *subqueryscan = (SubqueryScan *) plan;
		SubqueryScanState *subquerystate = (SubqueryScanState *) planstate;
		Plan	   *subnode = subqueryscan->subplan;
		RangeTblEntry *rte = rt_fetch(subqueryscan->scan.scanrelid,
									  es->rtable);
		List	   *saved_rtable = es->rtable;

		Assert(rte->rtekind == RTE_SUBQUERY);
		es->rtable = rte->subquery->rtable;

		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  ->  ");

		explain_outNode(str, subnode,
						subquerystate->subplan,
						NULL,
						indent + 3, es);

		es->rtable = saved_rtable;
	}

	/* subPlan-s */
	if (planstate->subPlan)
	{
		List	   *saved_rtable = es->rtable;
		ListCell   *lst;

		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  SubPlan\n");
		foreach(lst, planstate->subPlan)
		{
			SubPlanState *sps = (SubPlanState *) lfirst(lst);
			SubPlan    *sp = (SubPlan *) sps->xprstate.expr;

			es->rtable = sp->rtable;
			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "    ->  ");
			explain_outNode(str, sp->plan,
							sps->planstate,
							NULL,
							indent + 4, es);
		}
		es->rtable = saved_rtable;
	}
}

/*
 * Show a qualifier expression for a scan plan node
 */
static void
show_scan_qual(List *qual, const char *qlabel,
			   int scanrelid, Plan *outer_plan,
			   StringInfo str, int indent, ExplainState *es)
{
	Node	   *outercontext;
	List	   *context;
	Node	   *node;
	char	   *exprstr;
	int			i;

	/* No work if empty qual */
	if (qual == NIL)
		return;

	/* Convert AND list to explicit AND */
	node = (Node *) make_ands_explicit(qual);

	/*
	 * If we have an outer plan that is referenced by the qual, add it to the
	 * deparse context.  If not, don't (so that we don't force prefixes
	 * unnecessarily).
	 */
	if (outer_plan)
	{
		Relids		varnos = pull_varnos(node);

		if (bms_is_member(OUTER, varnos))
			outercontext = deparse_context_for_subplan("outer",
													   (Node *) outer_plan);
		else
			outercontext = NULL;
		bms_free(varnos);
	}
	else
		outercontext = NULL;

	context = deparse_context_for_plan(OUTER, outercontext,
									   0, NULL,
									   es->rtable);

	/* Deparse the expression */
	exprstr = deparse_expression(node, context, (outercontext != NULL), false);

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
												   (Node *) outer_plan);
	else
		outercontext = NULL;
	if (inner_plan)
		innercontext = deparse_context_for_subplan(inner_name,
												   (Node *) inner_plan);
	else
		innercontext = NULL;
	context = deparse_context_for_plan(outer_varno, outercontext,
									   inner_varno, innercontext,
									   es->rtable);

	/* Deparse the expression */
	node = (Node *) make_ands_explicit(qual);
	exprstr = deparse_expression(node, context, (inner_plan != NULL), false);

	/* And add to str */
	for (i = 0; i < indent; i++)
		appendStringInfo(str, "  ");
	appendStringInfo(str, "  %s: %s\n", qlabel, exprstr);
}

/*
 * Show the sort keys for a Sort node.
 */
static void
show_sort_keys(Plan *sortplan, int nkeys, AttrNumber *keycols,
			   const char *qlabel,
			   StringInfo str, int indent, ExplainState *es)
{
	List	   *context;
	bool		useprefix;
	int			keyno;
	char	   *exprstr;
	Relids		varnos;
	int			i;

	if (nkeys <= 0)
		return;

	for (i = 0; i < indent; i++)
		appendStringInfo(str, "  ");
	appendStringInfo(str, "  %s: ", qlabel);

	/*
	 * In this routine we expect that the plan node's tlist has not been
	 * processed by set_plan_references().	Normally, any Vars will contain
	 * valid varnos referencing the actual rtable.	But we might instead be
	 * looking at a dummy tlist generated by prepunion.c; if there are Vars
	 * with zero varno, use the tlist itself to determine their names.
	 */
	varnos = pull_varnos((Node *) sortplan->targetlist);
	if (bms_is_member(0, varnos))
	{
		Node	   *outercontext;

		outercontext = deparse_context_for_subplan("sort",
												   (Node *) sortplan);
		context = deparse_context_for_plan(0, outercontext,
										   0, NULL,
										   es->rtable);
		useprefix = false;
	}
	else
	{
		context = deparse_context_for_plan(0, NULL,
										   0, NULL,
										   es->rtable);
		useprefix = list_length(es->rtable) > 1;
	}
	bms_free(varnos);

	for (keyno = 0; keyno < nkeys; keyno++)
	{
		/* find key expression in tlist */
		AttrNumber	keyresno = keycols[keyno];
		TargetEntry *target = get_tle_by_resno(sortplan->targetlist, keyresno);

		if (!target)
			elog(ERROR, "no tlist entry for key %d", keyresno);
		/* Deparse the expression, showing any top-level cast */
		exprstr = deparse_expression((Node *) target->expr, context,
									 useprefix, true);
		/* And add to str */
		if (keyno > 0)
			appendStringInfo(str, ", ");
		appendStringInfoString(str, exprstr);
	}

	appendStringInfo(str, "\n");
}
