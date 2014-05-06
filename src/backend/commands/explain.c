/*-------------------------------------------------------------------------
 *
 * explain.c
 *	  Explain query execution plans
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/explain.c,v 1.206 2010/06/10 01:26:30 rhaas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/prepare.h"
#include "commands/trigger.h"
#include "executor/hashjoin.h"
#include "executor/instrument.h"
#include "optimizer/clauses.h"
#include "optimizer/planner.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/tuplesort.h"
#include "utils/snapmgr.h"
#include "utils/xml.h"


/* Hook for plugins to get control in ExplainOneQuery() */
ExplainOneQuery_hook_type ExplainOneQuery_hook = NULL;

/* Hook for plugins to get control in explain_get_index_name() */
explain_get_index_name_hook_type explain_get_index_name_hook = NULL;


/* OR-able flags for ExplainXMLTag() */
#define X_OPENING 0
#define X_CLOSING 1
#define X_CLOSE_IMMEDIATE 2
#define X_NOWHITESPACE 4

static void ExplainOneQuery(Query *query, ExplainState *es,
				const char *queryString, ParamListInfo params);
static void report_triggers(ResultRelInfo *rInfo, bool show_relname,
				ExplainState *es);
static double elapsed_time(instr_time *starttime);
static void ExplainNode(Plan *plan, PlanState *planstate,
			Plan *outer_plan,
			const char *relationship, const char *plan_name,
			ExplainState *es);
static void show_plan_tlist(Plan *plan, ExplainState *es);
static void show_qual(List *qual, const char *qlabel, Plan *plan,
		  Plan *outer_plan, bool useprefix, ExplainState *es);
static void show_scan_qual(List *qual, const char *qlabel,
			   Plan *scan_plan, Plan *outer_plan,
			   ExplainState *es);
static void show_upper_qual(List *qual, const char *qlabel, Plan *plan,
				ExplainState *es);
static void show_sort_keys(Plan *sortplan, ExplainState *es);
static void show_sort_info(SortState *sortstate, ExplainState *es);
static void show_hash_info(HashState *hashstate, ExplainState *es);
static const char *explain_get_index_name(Oid indexId);
static void ExplainScanTarget(Scan *plan, ExplainState *es);
static void ExplainMemberNodes(List *plans, PlanState **planstate,
				   Plan *outer_plan, ExplainState *es);
static void ExplainSubPlans(List *plans, const char *relationship,
				ExplainState *es);
static void ExplainPropertyList(const char *qlabel, List *data,
					ExplainState *es);
static void ExplainProperty(const char *qlabel, const char *value,
				bool numeric, ExplainState *es);

#define ExplainPropertyText(qlabel, value, es)	\
	ExplainProperty(qlabel, value, false, es)
static void ExplainPropertyInteger(const char *qlabel, int value,
					   ExplainState *es);
static void ExplainPropertyLong(const char *qlabel, long value,
					ExplainState *es);
static void ExplainPropertyFloat(const char *qlabel, double value, int ndigits,
					 ExplainState *es);
static void ExplainOpenGroup(const char *objtype, const char *labelname,
				 bool labeled, ExplainState *es);
static void ExplainCloseGroup(const char *objtype, const char *labelname,
				  bool labeled, ExplainState *es);
static void ExplainDummyGroup(const char *objtype, const char *labelname,
				  ExplainState *es);
static void ExplainXMLTag(const char *tagname, int flags, ExplainState *es);
static void ExplainJSONLineEnding(ExplainState *es);
static void ExplainYAMLLineStarting(ExplainState *es);
static void escape_json(StringInfo buf, const char *str);
static void escape_yaml(StringInfo buf, const char *str);



/*
 * ExplainQuery -
 *	  execute an EXPLAIN command
 */
void
ExplainQuery(ExplainStmt *stmt, const char *queryString,
			 ParamListInfo params, DestReceiver *dest)
{
	ExplainState es;
	TupOutputState *tstate;
	List	   *rewritten;
	ListCell   *lc;

	/* Initialize ExplainState. */
	ExplainInitState(&es);

	/* Parse options list. */
	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "analyze") == 0)
			es.analyze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "verbose") == 0)
			es.verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "costs") == 0)
			es.costs = defGetBoolean(opt);
		else if (strcmp(opt->defname, "buffers") == 0)
			es.buffers = defGetBoolean(opt);
		else if (strcmp(opt->defname, "format") == 0)
		{
			char	   *p = defGetString(opt);

			if (strcmp(p, "text") == 0)
				es.format = EXPLAIN_FORMAT_TEXT;
			else if (strcmp(p, "xml") == 0)
				es.format = EXPLAIN_FORMAT_XML;
			else if (strcmp(p, "json") == 0)
				es.format = EXPLAIN_FORMAT_JSON;
			else if (strcmp(p, "yaml") == 0)
				es.format = EXPLAIN_FORMAT_YAML;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("unrecognized value for EXPLAIN option \"%s\": \"%s\"",
					   opt->defname, p)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized EXPLAIN option \"%s\"",
							opt->defname)));
	}

	if (es.buffers && !es.analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option BUFFERS requires ANALYZE")));

	/*
	 * Parse analysis was done already, but we still have to run the rule
	 * rewriter.  We do not do AcquireRewriteLocks: we assume the query either
	 * came straight from the parser, or suitable locks were acquired by
	 * plancache.c.
	 *
	 * Because the rewriter and planner tend to scribble on the input, we make
	 * a preliminary copy of the source querytree.  This prevents problems in
	 * the case that the EXPLAIN is in a portal or plpgsql function and is
	 * executed repeatedly.  (See also the same hack in DECLARE CURSOR and
	 * PREPARE.)  XXX FIXME someday.
	 */
	Assert(IsA(stmt->query, Query));
	rewritten = QueryRewrite((Query *) copyObject(stmt->query));

	/* emit opening boilerplate */
	ExplainBeginOutput(&es);

	if (rewritten == NIL)
	{
		/*
		 * In the case of an INSTEAD NOTHING, tell at least that.  But in
		 * non-text format, the output is delimited, so this isn't necessary.
		 */
		if (es.format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es.str, "Query rewrites to nothing\n");
	}
	else
	{
		ListCell   *l;

		/* Explain every plan */
		foreach(l, rewritten)
		{
			ExplainOneQuery((Query *) lfirst(l), &es, queryString, params);

			/* Separate plans with an appropriate separator */
			if (lnext(l) != NULL)
				ExplainSeparatePlans(&es);
		}
	}

	/* emit closing boilerplate */
	ExplainEndOutput(&es);
	Assert(es.indent == 0);

	/* output tuples */
	tstate = begin_tup_output_tupdesc(dest, ExplainResultDesc(stmt));
	if (es.format == EXPLAIN_FORMAT_TEXT)
		do_text_output_multiline(tstate, es.str->data);
	else
		do_text_output_oneline(tstate, es.str->data);
	end_tup_output(tstate);

	pfree(es.str->data);
}

/*
 * Initialize ExplainState.
 */
void
ExplainInitState(ExplainState *es)
{
	/* Set default options. */
	memset(es, 0, sizeof(ExplainState));
	es->costs = true;
	/* Prepare output buffer. */
	es->str = makeStringInfo();
}

/*
 * ExplainResultDesc -
 *	  construct the result tupledesc for an EXPLAIN
 */
TupleDesc
ExplainResultDesc(ExplainStmt *stmt)
{
	TupleDesc	tupdesc;
	ListCell   *lc;
	bool		xml = false;

	/* Check for XML format option */
	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "format") == 0)
		{
			char	   *p = defGetString(opt);

			xml = (strcmp(p, "xml") == 0);
			/* don't "break", as ExplainQuery will use the last value */
		}
	}

	/* Need a tuple descriptor representing a single TEXT or XML column */
	tupdesc = CreateTemplateTupleDesc(1, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "QUERY PLAN",
					   xml ? XMLOID : TEXTOID, -1, 0);
	return tupdesc;
}

/*
 * ExplainOneQuery -
 *	  print out the execution plan for one Query
 */
static void
ExplainOneQuery(Query *query, ExplainState *es,
				const char *queryString, ParamListInfo params)
{
	/* planner will not cope with utility statements */
	if (query->commandType == CMD_UTILITY)
	{
		ExplainOneUtility(query->utilityStmt, es, queryString, params);
		return;
	}

	/* if an advisor plugin is present, let it manage things */
	if (ExplainOneQuery_hook)
		(*ExplainOneQuery_hook) (query, es, queryString, params);
	else
	{
		PlannedStmt *plan;

		/* plan the query */
		plan = pg_plan_query(query, 0, params);

		/* run it (if needed) and produce output */
		ExplainOnePlan(plan, es, queryString, params);
	}
}

/*
 * ExplainOneUtility -
 *	  print out the execution plan for one utility statement
 *	  (In general, utility statements don't have plans, but there are some
 *	  we treat as special cases)
 *
 * This is exported because it's called back from prepare.c in the
 * EXPLAIN EXECUTE case
 */
void
ExplainOneUtility(Node *utilityStmt, ExplainState *es,
				  const char *queryString, ParamListInfo params)
{
	if (utilityStmt == NULL)
		return;

	if (IsA(utilityStmt, ExecuteStmt))
		ExplainExecuteQuery((ExecuteStmt *) utilityStmt, es,
							queryString, params);
	else if (IsA(utilityStmt, NotifyStmt))
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str, "NOTIFY\n");
		else
			ExplainDummyGroup("Notify", NULL, es);
	}
	else
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str,
							  "Utility statements have no plan structure\n");
		else
			ExplainDummyGroup("Utility Statement", NULL, es);
	}
}

/*
 * ExplainOnePlan -
 *		given a planned query, execute it if needed, and then print
 *		EXPLAIN output
 *
 * Since we ignore any DeclareCursorStmt that might be attached to the query,
 * if you say EXPLAIN ANALYZE DECLARE CURSOR then we'll actually run the
 * query.  This is different from pre-8.3 behavior but seems more useful than
 * not running the query.  No cursor will be created, however.
 *
 * This is exported because it's called back from prepare.c in the
 * EXPLAIN EXECUTE case, and because an index advisor plugin would need
 * to call it.
 */
void
ExplainOnePlan(PlannedStmt *plannedstmt, ExplainState *es,
			   const char *queryString, ParamListInfo params)
{
	QueryDesc  *queryDesc;
	instr_time	starttime;
	double		totaltime = 0;
	int			eflags;
	int			instrument_option = 0;

	if (es->analyze)
		instrument_option |= INSTRUMENT_TIMER;
	if (es->buffers)
		instrument_option |= INSTRUMENT_BUFFERS;

	/*
	 * Use a snapshot with an updated command ID to ensure this query sees
	 * results of any previously executed queries.
	 */
	PushUpdatedSnapshot(GetActiveSnapshot());

	/* Create a QueryDesc requesting no output */
	queryDesc = CreateQueryDesc(plannedstmt, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								None_Receiver, params, instrument_option);

	INSTR_TIME_SET_CURRENT(starttime);

	/* If analyzing, we need to cope with queued triggers */
	if (es->analyze)
		AfterTriggerBeginQuery();

	/* Select execution options */
	if (es->analyze)
		eflags = 0;				/* default run-to-completion flags */
	else
		eflags = EXEC_FLAG_EXPLAIN_ONLY;

	/* call ExecutorStart to prepare the plan for execution */
	ExecutorStart(queryDesc, eflags);

	/* Execute the plan for statistics if asked for */
	if (es->analyze)
	{
		/* run the plan */
		ExecutorRun(queryDesc, ForwardScanDirection, 0L);

		/* We can't clean up 'till we're done printing the stats... */
		totaltime += elapsed_time(&starttime);
	}

	ExplainOpenGroup("Query", NULL, true, es);

	/* Create textual dump of plan tree */
	ExplainPrintPlan(es, queryDesc);

	/*
	 * If we ran the command, run any AFTER triggers it queued.  (Note this
	 * will not include DEFERRED triggers; since those don't run until end of
	 * transaction, we can't measure them.)  Include into total runtime.
	 */
	if (es->analyze)
	{
		INSTR_TIME_SET_CURRENT(starttime);
		AfterTriggerEndQuery(queryDesc->estate);
		totaltime += elapsed_time(&starttime);
	}

	/* Print info about runtime of triggers */
	if (es->analyze)
	{
		ResultRelInfo *rInfo;
		bool		show_relname;
		int			numrels = queryDesc->estate->es_num_result_relations;
		List	   *targrels = queryDesc->estate->es_trig_target_relations;
		int			nr;
		ListCell   *l;

		ExplainOpenGroup("Triggers", "Triggers", false, es);

		show_relname = (numrels > 1 || targrels != NIL);
		rInfo = queryDesc->estate->es_result_relations;
		for (nr = 0; nr < numrels; rInfo++, nr++)
			report_triggers(rInfo, show_relname, es);

		foreach(l, targrels)
		{
			rInfo = (ResultRelInfo *) lfirst(l);
			report_triggers(rInfo, show_relname, es);
		}

		ExplainCloseGroup("Triggers", "Triggers", false, es);
	}

	/*
	 * Close down the query and free resources.  Include time for this in the
	 * total runtime (although it should be pretty minimal).
	 */
	INSTR_TIME_SET_CURRENT(starttime);

	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();

	/* We need a CCI just in case query expanded to multiple plans */
	if (es->analyze)
		CommandCounterIncrement();

	totaltime += elapsed_time(&starttime);

	if (es->analyze)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfo(es->str, "Total runtime: %.3f ms\n",
							 1000.0 * totaltime);
		else
			ExplainPropertyFloat("Total Runtime", 1000.0 * totaltime,
								 3, es);
	}

	ExplainCloseGroup("Query", NULL, true, es);
}

/*
 * ExplainPrintPlan -
 *	  convert a QueryDesc's plan tree to text and append it to es->str
 *
 * The caller should have set up the options fields of *es, as well as
 * initializing the output buffer es->str.  Other fields in *es are
 * initialized here.
 *
 * NB: will not work on utility statements
 */
void
ExplainPrintPlan(ExplainState *es, QueryDesc *queryDesc)
{
	Assert(queryDesc->plannedstmt != NULL);
	es->pstmt = queryDesc->plannedstmt;
	es->rtable = queryDesc->plannedstmt->rtable;
	ExplainNode(queryDesc->plannedstmt->planTree, queryDesc->planstate,
				NULL, NULL, NULL, es);
}

/*
 * ExplainQueryText -
 *	  add a "Query Text" node that contains the actual text of the query
 *
 * The caller should have set up the options fields of *es, as well as
 * initializing the output buffer es->str.
 *
 */
void
ExplainQueryText(ExplainState *es, QueryDesc *queryDesc)
{
	if (queryDesc->sourceText)
		ExplainPropertyText("Query Text", queryDesc->sourceText, es);
}

/*
 * report_triggers -
 *		report execution stats for a single relation's triggers
 */
static void
report_triggers(ResultRelInfo *rInfo, bool show_relname, ExplainState *es)
{
	int			nt;

	if (!rInfo->ri_TrigDesc || !rInfo->ri_TrigInstrument)
		return;
	for (nt = 0; nt < rInfo->ri_TrigDesc->numtriggers; nt++)
	{
		Trigger    *trig = rInfo->ri_TrigDesc->triggers + nt;
		Instrumentation *instr = rInfo->ri_TrigInstrument + nt;
		char	   *relname;
		char	   *conname = NULL;

		/* Must clean up instrumentation state */
		InstrEndLoop(instr);

		/*
		 * We ignore triggers that were never invoked; they likely aren't
		 * relevant to the current query type.
		 */
		if (instr->ntuples == 0)
			continue;

		ExplainOpenGroup("Trigger", NULL, true, es);

		relname = RelationGetRelationName(rInfo->ri_RelationDesc);
		if (OidIsValid(trig->tgconstraint))
			conname = get_constraint_name(trig->tgconstraint);

		/*
		 * In text format, we avoid printing both the trigger name and the
		 * constraint name unless VERBOSE is specified.  In non-text formats
		 * we just print everything.
		 */
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			if (es->verbose || conname == NULL)
				appendStringInfo(es->str, "Trigger %s", trig->tgname);
			else
				appendStringInfoString(es->str, "Trigger");
			if (conname)
				appendStringInfo(es->str, " for constraint %s", conname);
			if (show_relname)
				appendStringInfo(es->str, " on %s", relname);
			appendStringInfo(es->str, ": time=%.3f calls=%.0f\n",
							 1000.0 * instr->total, instr->ntuples);
		}
		else
		{
			ExplainPropertyText("Trigger Name", trig->tgname, es);
			if (conname)
				ExplainPropertyText("Constraint Name", conname, es);
			ExplainPropertyText("Relation", relname, es);
			ExplainPropertyFloat("Time", 1000.0 * instr->total, 3, es);
			ExplainPropertyFloat("Calls", instr->ntuples, 0, es);
		}

		if (conname)
			pfree(conname);

		ExplainCloseGroup("Trigger", NULL, true, es);
	}
}

/* Compute elapsed time in seconds since given timestamp */
static double
elapsed_time(instr_time *starttime)
{
	instr_time	endtime;

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_SUBTRACT(endtime, *starttime);
	return INSTR_TIME_GET_DOUBLE(endtime);
}

/*
 * ExplainNode -
 *	  Appends a description of the Plan node to es->str
 *
 * planstate points to the executor state node corresponding to the plan node.
 * We need this to get at the instrumentation data (if any) as well as the
 * list of subplans.
 *
 * outer_plan, if not null, references another plan node that is the outer
 * side of a join with the current node.  This is only interesting for
 * deciphering runtime keys of an inner indexscan.
 *
 * relationship describes the relationship of this plan node to its parent
 * (eg, "Outer", "Inner"); it can be null at top level.  plan_name is an
 * optional name to be attached to the node.
 *
 * In text format, es->indent is controlled in this function since we only
 * want it to change at Plan-node boundaries.  In non-text formats, es->indent
 * corresponds to the nesting depth of logical output groups, and therefore
 * is controlled by ExplainOpenGroup/ExplainCloseGroup.
 */
static void
ExplainNode(Plan *plan, PlanState *planstate,
			Plan *outer_plan,
			const char *relationship, const char *plan_name,
			ExplainState *es)
{
	const char *pname;			/* node type name for text output */
	const char *sname;			/* node type name for non-text output */
	const char *strategy = NULL;
	const char *operation = NULL;
	int			save_indent = es->indent;
	bool		haschildren;

	Assert(plan);

	switch (nodeTag(plan))
	{
		case T_Result:
			pname = sname = "Result";
			break;
		case T_ModifyTable:
			sname = "ModifyTable";
			switch (((ModifyTable *) plan)->operation)
			{
				case CMD_INSERT:
					pname = operation = "Insert";
					break;
				case CMD_UPDATE:
					pname = operation = "Update";
					break;
				case CMD_DELETE:
					pname = operation = "Delete";
					break;
				default:
					pname = "???";
					break;
			}
			break;
		case T_Append:
			pname = sname = "Append";
			break;
		case T_RecursiveUnion:
			pname = sname = "Recursive Union";
			break;
		case T_BitmapAnd:
			pname = sname = "BitmapAnd";
			break;
		case T_BitmapOr:
			pname = sname = "BitmapOr";
			break;
		case T_NestLoop:
			pname = sname = "Nested Loop";
			break;
		case T_MergeJoin:
			pname = "Merge";	/* "Join" gets added by jointype switch */
			sname = "Merge Join";
			break;
		case T_HashJoin:
			pname = "Hash";		/* "Join" gets added by jointype switch */
			sname = "Hash Join";
			break;
		case T_SeqScan:
			pname = sname = "Seq Scan";
			break;
		case T_IndexScan:
			pname = sname = "Index Scan";
			break;
		case T_BitmapIndexScan:
			pname = sname = "Bitmap Index Scan";
			break;
		case T_BitmapHeapScan:
			pname = sname = "Bitmap Heap Scan";
			break;
		case T_TidScan:
			pname = sname = "Tid Scan";
			break;
		case T_SubqueryScan:
			pname = sname = "Subquery Scan";
			break;
		case T_FunctionScan:
			pname = sname = "Function Scan";
			break;
		case T_ValuesScan:
			pname = sname = "Values Scan";
			break;
		case T_CteScan:
			pname = sname = "CTE Scan";
			break;
		case T_WorkTableScan:
			pname = sname = "WorkTable Scan";
			break;
		case T_Material:
			pname = sname = "Materialize";
			break;
		case T_Sort:
			pname = sname = "Sort";
			break;
		case T_Group:
			pname = sname = "Group";
			break;
		case T_Agg:
			sname = "Aggregate";
			switch (((Agg *) plan)->aggstrategy)
			{
				case AGG_PLAIN:
					pname = "Aggregate";
					strategy = "Plain";
					break;
				case AGG_SORTED:
					pname = "GroupAggregate";
					strategy = "Sorted";
					break;
				case AGG_HASHED:
					pname = "HashAggregate";
					strategy = "Hashed";
					break;
				default:
					pname = "Aggregate ???";
					strategy = "???";
					break;
			}
			break;
		case T_WindowAgg:
			pname = sname = "WindowAgg";
			break;
		case T_Unique:
			pname = sname = "Unique";
			break;
		case T_SetOp:
			sname = "SetOp";
			switch (((SetOp *) plan)->strategy)
			{
				case SETOP_SORTED:
					pname = "SetOp";
					strategy = "Sorted";
					break;
				case SETOP_HASHED:
					pname = "HashSetOp";
					strategy = "Hashed";
					break;
				default:
					pname = "SetOp ???";
					strategy = "???";
					break;
			}
			break;
		case T_LockRows:
			pname = sname = "LockRows";
			break;
		case T_Limit:
			pname = sname = "Limit";
			break;
		case T_Hash:
			pname = sname = "Hash";
			break;
		default:
			pname = sname = "???";
			break;
	}

	ExplainOpenGroup("Plan",
					 relationship ? NULL : "Plan",
					 true, es);

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (plan_name)
		{
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str, "%s\n", plan_name);
			es->indent++;
		}
		if (es->indent)
		{
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfoString(es->str, "->  ");
			es->indent += 2;
		}
		appendStringInfoString(es->str, pname);
		es->indent++;
	}
	else
	{
		ExplainPropertyText("Node Type", sname, es);
		if (strategy)
			ExplainPropertyText("Strategy", strategy, es);
		if (operation)
			ExplainPropertyText("Operation", operation, es);
		if (relationship)
			ExplainPropertyText("Parent Relationship", relationship, es);
		if (plan_name)
			ExplainPropertyText("Subplan Name", plan_name, es);
	}

	switch (nodeTag(plan))
	{
		case T_IndexScan:
			{
				IndexScan  *indexscan = (IndexScan *) plan;
				const char *indexname =
				explain_get_index_name(indexscan->indexid);

				if (es->format == EXPLAIN_FORMAT_TEXT)
				{
					if (ScanDirectionIsBackward(indexscan->indexorderdir))
						appendStringInfoString(es->str, " Backward");
					appendStringInfo(es->str, " using %s", indexname);
				}
				else
				{
					const char *scandir;

					switch (indexscan->indexorderdir)
					{
						case BackwardScanDirection:
							scandir = "Backward";
							break;
						case NoMovementScanDirection:
							scandir = "NoMovement";
							break;
						case ForwardScanDirection:
							scandir = "Forward";
							break;
						default:
							scandir = "???";
							break;
					}
					ExplainPropertyText("Scan Direction", scandir, es);
					ExplainPropertyText("Index Name", indexname, es);
				}
			}
			/* FALL THRU */
		case T_SeqScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
			ExplainScanTarget((Scan *) plan, es);
			break;
		case T_BitmapIndexScan:
			{
				BitmapIndexScan *bitmapindexscan = (BitmapIndexScan *) plan;
				const char *indexname =
				explain_get_index_name(bitmapindexscan->indexid);

				if (es->format == EXPLAIN_FORMAT_TEXT)
					appendStringInfo(es->str, " on %s", indexname);
				else
					ExplainPropertyText("Index Name", indexname, es);
			}
			break;
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			{
				const char *jointype;

				switch (((Join *) plan)->jointype)
				{
					case JOIN_INNER:
						jointype = "Inner";
						break;
					case JOIN_LEFT:
						jointype = "Left";
						break;
					case JOIN_FULL:
						jointype = "Full";
						break;
					case JOIN_RIGHT:
						jointype = "Right";
						break;
					case JOIN_SEMI:
						jointype = "Semi";
						break;
					case JOIN_ANTI:
						jointype = "Anti";
						break;
					default:
						jointype = "???";
						break;
				}
				if (es->format == EXPLAIN_FORMAT_TEXT)
				{
					/*
					 * For historical reasons, the join type is interpolated
					 * into the node type name...
					 */
					if (((Join *) plan)->jointype != JOIN_INNER)
						appendStringInfo(es->str, " %s Join", jointype);
					else if (!IsA(plan, NestLoop))
						appendStringInfo(es->str, " Join");
				}
				else
					ExplainPropertyText("Join Type", jointype, es);
			}
			break;
		case T_SetOp:
			{
				const char *setopcmd;

				switch (((SetOp *) plan)->cmd)
				{
					case SETOPCMD_INTERSECT:
						setopcmd = "Intersect";
						break;
					case SETOPCMD_INTERSECT_ALL:
						setopcmd = "Intersect All";
						break;
					case SETOPCMD_EXCEPT:
						setopcmd = "Except";
						break;
					case SETOPCMD_EXCEPT_ALL:
						setopcmd = "Except All";
						break;
					default:
						setopcmd = "???";
						break;
				}
				if (es->format == EXPLAIN_FORMAT_TEXT)
					appendStringInfo(es->str, " %s", setopcmd);
				else
					ExplainPropertyText("Command", setopcmd, es);
			}
			break;
		default:
			break;
	}

	if (es->costs)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfo(es->str, "  (cost=%.2f..%.2f rows=%.0f width=%d)",
							 plan->startup_cost, plan->total_cost,
							 plan->plan_rows, plan->plan_width);
		}
		else
		{
			ExplainPropertyFloat("Startup Cost", plan->startup_cost, 2, es);
			ExplainPropertyFloat("Total Cost", plan->total_cost, 2, es);
			ExplainPropertyFloat("Plan Rows", plan->plan_rows, 0, es);
			ExplainPropertyInteger("Plan Width", plan->plan_width, es);
		}
	}

	/*
	 * We have to forcibly clean up the instrumentation state because we
	 * haven't done ExecutorEnd yet.  This is pretty grotty ...
	 */
	if (planstate->instrument)
		InstrEndLoop(planstate->instrument);

	if (planstate->instrument && planstate->instrument->nloops > 0)
	{
		double		nloops = planstate->instrument->nloops;
		double		startup_sec = 1000.0 * planstate->instrument->startup / nloops;
		double		total_sec = 1000.0 * planstate->instrument->total / nloops;
		double		rows = planstate->instrument->ntuples / nloops;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfo(es->str,
							 " (actual time=%.3f..%.3f rows=%.0f loops=%.0f)",
							 startup_sec, total_sec, rows, nloops);
		}
		else
		{
			ExplainPropertyFloat("Actual Startup Time", startup_sec, 3, es);
			ExplainPropertyFloat("Actual Total Time", total_sec, 3, es);
			ExplainPropertyFloat("Actual Rows", rows, 0, es);
			ExplainPropertyFloat("Actual Loops", nloops, 0, es);
		}
	}
	else if (es->analyze)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfo(es->str, " (never executed)");
		else
		{
			ExplainPropertyFloat("Actual Startup Time", 0.0, 3, es);
			ExplainPropertyFloat("Actual Total Time", 0.0, 3, es);
			ExplainPropertyFloat("Actual Rows", 0.0, 0, es);
			ExplainPropertyFloat("Actual Loops", 0.0, 0, es);
		}
	}

	/* in text format, first line ends here */
	if (es->format == EXPLAIN_FORMAT_TEXT)
		appendStringInfoChar(es->str, '\n');

	/* target list */
	if (es->verbose)
		show_plan_tlist(plan, es);

	/* quals, sort keys, etc */
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			show_scan_qual(((IndexScan *) plan)->indexqualorig,
						   "Index Cond", plan, outer_plan, es);
			show_scan_qual(plan->qual, "Filter", plan, outer_plan, es);
			break;
		case T_BitmapIndexScan:
			show_scan_qual(((BitmapIndexScan *) plan)->indexqualorig,
						   "Index Cond", plan, outer_plan, es);
			break;
		case T_BitmapHeapScan:
			show_scan_qual(((BitmapHeapScan *) plan)->bitmapqualorig,
						   "Recheck Cond", plan, outer_plan, es);
			/* FALL THRU */
		case T_SeqScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_SubqueryScan:
			show_scan_qual(plan->qual, "Filter", plan, outer_plan, es);
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
				show_scan_qual(tidquals, "TID Cond", plan, outer_plan, es);
				show_scan_qual(plan->qual, "Filter", plan, outer_plan, es);
			}
			break;
		case T_NestLoop:
			show_upper_qual(((NestLoop *) plan)->join.joinqual,
							"Join Filter", plan, es);
			show_upper_qual(plan->qual, "Filter", plan, es);
			break;
		case T_MergeJoin:
			show_upper_qual(((MergeJoin *) plan)->mergeclauses,
							"Merge Cond", plan, es);
			show_upper_qual(((MergeJoin *) plan)->join.joinqual,
							"Join Filter", plan, es);
			show_upper_qual(plan->qual, "Filter", plan, es);
			break;
		case T_HashJoin:
			show_upper_qual(((HashJoin *) plan)->hashclauses,
							"Hash Cond", plan, es);
			show_upper_qual(((HashJoin *) plan)->join.joinqual,
							"Join Filter", plan, es);
			show_upper_qual(plan->qual, "Filter", plan, es);
			break;
		case T_Agg:
		case T_Group:
			show_upper_qual(plan->qual, "Filter", plan, es);
			break;
		case T_Sort:
			show_sort_keys(plan, es);
			show_sort_info((SortState *) planstate, es);
			break;
		case T_Result:
			show_upper_qual((List *) ((Result *) plan)->resconstantqual,
							"One-Time Filter", plan, es);
			show_upper_qual(plan->qual, "Filter", plan, es);
			break;
		case T_Hash:
			show_hash_info((HashState *) planstate, es);
			break;
		default:
			break;
	}

	/* Show buffer usage */
	if (es->buffers)
	{
		const BufferUsage *usage = &planstate->instrument->bufusage;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			bool		has_shared = (usage->shared_blks_hit > 0 ||
									  usage->shared_blks_read > 0 ||
									  usage->shared_blks_written);
			bool		has_local = (usage->local_blks_hit > 0 ||
									 usage->local_blks_read > 0 ||
									 usage->local_blks_written);
			bool		has_temp = (usage->temp_blks_read > 0 ||
									usage->temp_blks_written);

			/* Show only positive counter values. */
			if (has_shared || has_local || has_temp)
			{
				appendStringInfoSpaces(es->str, es->indent * 2);
				appendStringInfoString(es->str, "Buffers:");

				if (has_shared)
				{
					appendStringInfoString(es->str, " shared");
					if (usage->shared_blks_hit > 0)
						appendStringInfo(es->str, " hit=%ld",
										 usage->shared_blks_hit);
					if (usage->shared_blks_read > 0)
						appendStringInfo(es->str, " read=%ld",
										 usage->shared_blks_read);
					if (usage->shared_blks_written > 0)
						appendStringInfo(es->str, " written=%ld",
										 usage->shared_blks_written);
					if (has_local || has_temp)
						appendStringInfoChar(es->str, ',');
				}
				if (has_local)
				{
					appendStringInfoString(es->str, " local");
					if (usage->local_blks_hit > 0)
						appendStringInfo(es->str, " hit=%ld",
										 usage->local_blks_hit);
					if (usage->local_blks_read > 0)
						appendStringInfo(es->str, " read=%ld",
										 usage->local_blks_read);
					if (usage->local_blks_written > 0)
						appendStringInfo(es->str, " written=%ld",
										 usage->local_blks_written);
					if (has_temp)
						appendStringInfoChar(es->str, ',');
				}
				if (has_temp)
				{
					appendStringInfoString(es->str, " temp");
					if (usage->temp_blks_read > 0)
						appendStringInfo(es->str, " read=%ld",
										 usage->temp_blks_read);
					if (usage->temp_blks_written > 0)
						appendStringInfo(es->str, " written=%ld",
										 usage->temp_blks_written);
				}
				appendStringInfoChar(es->str, '\n');
			}
		}
		else
		{
			ExplainPropertyLong("Shared Hit Blocks", usage->shared_blks_hit, es);
			ExplainPropertyLong("Shared Read Blocks", usage->shared_blks_read, es);
			ExplainPropertyLong("Shared Written Blocks", usage->shared_blks_written, es);
			ExplainPropertyLong("Local Hit Blocks", usage->local_blks_hit, es);
			ExplainPropertyLong("Local Read Blocks", usage->local_blks_read, es);
			ExplainPropertyLong("Local Written Blocks", usage->local_blks_written, es);
			ExplainPropertyLong("Temp Read Blocks", usage->temp_blks_read, es);
			ExplainPropertyLong("Temp Written Blocks", usage->temp_blks_written, es);
		}
	}

	/* Get ready to display the child plans */
	haschildren = plan->initPlan ||
		outerPlan(plan) ||
		innerPlan(plan) ||
		IsA(plan, ModifyTable) ||
		IsA(plan, Append) ||
		IsA(plan, BitmapAnd) ||
		IsA(plan, BitmapOr) ||
		IsA(plan, SubqueryScan) ||
		planstate->subPlan;
	if (haschildren)
		ExplainOpenGroup("Plans", "Plans", false, es);

	/* initPlan-s */
	if (plan->initPlan)
		ExplainSubPlans(planstate->initPlan, "InitPlan", es);

	/* lefttree */
	if (outerPlan(plan))
	{
		/*
		 * Ordinarily we don't pass down our own outer_plan value to our child
		 * nodes, but in bitmap scan trees we must, since the bottom
		 * BitmapIndexScan nodes may have outer references.  Also, we must do
		 * so at a Result node, because it might be a pseudoconstant-qual
		 * gating node inserted into an inner indexscan plan.
		 */
		ExplainNode(outerPlan(plan), outerPlanState(planstate),
					(IsA(plan, BitmapHeapScan) ||
					 IsA(plan, Result)) ? outer_plan : NULL,
					"Outer", NULL, es);
	}

	/* righttree */
	if (innerPlan(plan))
	{
		ExplainNode(innerPlan(plan), innerPlanState(planstate),
					outerPlan(plan),
					"Inner", NULL, es);
	}

	/* special child plans */
	switch (nodeTag(plan))
	{
		case T_ModifyTable:
			ExplainMemberNodes(((ModifyTable *) plan)->plans,
							   ((ModifyTableState *) planstate)->mt_plans,
							   outer_plan, es);
			break;
		case T_Append:
			ExplainMemberNodes(((Append *) plan)->appendplans,
							   ((AppendState *) planstate)->appendplans,
							   outer_plan, es);
			break;
		case T_BitmapAnd:
			ExplainMemberNodes(((BitmapAnd *) plan)->bitmapplans,
							   ((BitmapAndState *) planstate)->bitmapplans,
							   outer_plan, es);
			break;
		case T_BitmapOr:
			ExplainMemberNodes(((BitmapOr *) plan)->bitmapplans,
							   ((BitmapOrState *) planstate)->bitmapplans,
							   outer_plan, es);
			break;
		case T_SubqueryScan:
			{
				SubqueryScan *subqueryscan = (SubqueryScan *) plan;
				SubqueryScanState *subquerystate = (SubqueryScanState *) planstate;

				ExplainNode(subqueryscan->subplan, subquerystate->subplan,
							NULL,
							"Subquery", NULL, es);
			}
			break;
		default:
			break;
	}

	/* subPlan-s */
	if (planstate->subPlan)
		ExplainSubPlans(planstate->subPlan, "SubPlan", es);

	/* end of child plans */
	if (haschildren)
		ExplainCloseGroup("Plans", "Plans", false, es);

	/* in text format, undo whatever indentation we added */
	if (es->format == EXPLAIN_FORMAT_TEXT)
		es->indent = save_indent;

	ExplainCloseGroup("Plan",
					  relationship ? NULL : "Plan",
					  true, es);
}

/*
 * Show the targetlist of a plan node
 */
static void
show_plan_tlist(Plan *plan, ExplainState *es)
{
	List	   *context;
	List	   *result = NIL;
	bool		useprefix;
	ListCell   *lc;
	int			i;

	/* No work if empty tlist (this occurs eg in bitmap indexscans) */
	if (plan->targetlist == NIL)
		return;
	/* The tlist of an Append isn't real helpful, so suppress it */
	if (IsA(plan, Append))
		return;
	/* Likewise for RecursiveUnion */
	if (IsA(plan, RecursiveUnion))
		return;

	/* Set up deparsing context */
	context = deparse_context_for_plan((Node *) plan,
									   NULL,
									   es->rtable,
									   es->pstmt->subplans);
	useprefix = list_length(es->rtable) > 1;

	/* Deparse each result column (we now include resjunk ones) */
	i = 0;
	foreach(lc, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		result = lappend(result,
						 deparse_expression((Node *) tle->expr, context,
											useprefix, false));
	}

	/* Print results */
	ExplainPropertyList("Output", result, es);
}

/*
 * Show a qualifier expression
 *
 * Note: outer_plan is the referent for any OUTER vars in the scan qual;
 * this would be the outer side of a nestloop plan.  Pass NULL if none.
 */
static void
show_qual(List *qual, const char *qlabel, Plan *plan, Plan *outer_plan,
		  bool useprefix, ExplainState *es)
{
	List	   *context;
	Node	   *node;
	char	   *exprstr;

	/* No work if empty qual */
	if (qual == NIL)
		return;

	/* Convert AND list to explicit AND */
	node = (Node *) make_ands_explicit(qual);

	/* Set up deparsing context */
	context = deparse_context_for_plan((Node *) plan,
									   (Node *) outer_plan,
									   es->rtable,
									   es->pstmt->subplans);

	/* Deparse the expression */
	exprstr = deparse_expression(node, context, useprefix, false);

	/* And add to es->str */
	ExplainPropertyText(qlabel, exprstr, es);
}

/*
 * Show a qualifier expression for a scan plan node
 */
static void
show_scan_qual(List *qual, const char *qlabel,
			   Plan *scan_plan, Plan *outer_plan,
			   ExplainState *es)
{
	bool		useprefix;

	useprefix = (outer_plan != NULL || IsA(scan_plan, SubqueryScan) ||
				 es->verbose);
	show_qual(qual, qlabel, scan_plan, outer_plan, useprefix, es);
}

/*
 * Show a qualifier expression for an upper-level plan node
 */
static void
show_upper_qual(List *qual, const char *qlabel, Plan *plan, ExplainState *es)
{
	bool		useprefix;

	useprefix = (list_length(es->rtable) > 1 || es->verbose);
	show_qual(qual, qlabel, plan, NULL, useprefix, es);
}

/*
 * Show the sort keys for a Sort node.
 */
static void
show_sort_keys(Plan *sortplan, ExplainState *es)
{
	int			nkeys = ((Sort *) sortplan)->numCols;
	AttrNumber *keycols = ((Sort *) sortplan)->sortColIdx;
	List	   *context;
	List	   *result = NIL;
	bool		useprefix;
	int			keyno;
	char	   *exprstr;

	if (nkeys <= 0)
		return;

	/* Set up deparsing context */
	context = deparse_context_for_plan((Node *) sortplan,
									   NULL,
									   es->rtable,
									   es->pstmt->subplans);
	useprefix = (list_length(es->rtable) > 1 || es->verbose);

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
		result = lappend(result, exprstr);
	}

	ExplainPropertyList("Sort Key", result, es);
}

/*
 * If it's EXPLAIN ANALYZE, show tuplesort stats for a sort node
 */
static void
show_sort_info(SortState *sortstate, ExplainState *es)
{
	Assert(IsA(sortstate, SortState));
	if (es->analyze && sortstate->sort_Done &&
		sortstate->tuplesortstate != NULL)
	{
		Tuplesortstate *state = (Tuplesortstate *) sortstate->tuplesortstate;
		const char *sortMethod;
		const char *spaceType;
		long		spaceUsed;

		tuplesort_get_stats(state, &sortMethod, &spaceType, &spaceUsed);

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str, "Sort Method:  %s  %s: %ldkB\n",
							 sortMethod, spaceType, spaceUsed);
		}
		else
		{
			ExplainPropertyText("Sort Method", sortMethod, es);
			ExplainPropertyLong("Sort Space Used", spaceUsed, es);
			ExplainPropertyText("Sort Space Type", spaceType, es);
		}
	}
}

/*
 * Show information on hash buckets/batches.
 */
static void
show_hash_info(HashState *hashstate, ExplainState *es)
{
	HashJoinTable hashtable;

	Assert(IsA(hashstate, HashState));
	hashtable = hashstate->hashtable;

	if (hashtable)
	{
		long		spacePeakKb = (hashtable->spacePeak + 1023) / 1024;

		if (es->format != EXPLAIN_FORMAT_TEXT)
		{
			ExplainPropertyLong("Hash Buckets", hashtable->nbuckets, es);
			ExplainPropertyLong("Hash Batches", hashtable->nbatch, es);
			ExplainPropertyLong("Original Hash Batches",
								hashtable->nbatch_original, es);
			ExplainPropertyLong("Peak Memory Usage", spacePeakKb, es);
		}
		else if (hashtable->nbatch_original != hashtable->nbatch)
		{
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str,
			"Buckets: %d  Batches: %d (originally %d)  Memory Usage: %ldkB\n",
							 hashtable->nbuckets, hashtable->nbatch,
							 hashtable->nbatch_original, spacePeakKb);
		}
		else
		{
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str,
						   "Buckets: %d  Batches: %d  Memory Usage: %ldkB\n",
							 hashtable->nbuckets, hashtable->nbatch,
							 spacePeakKb);
		}
	}
}

/*
 * Fetch the name of an index in an EXPLAIN
 *
 * We allow plugins to get control here so that plans involving hypothetical
 * indexes can be explained.
 */
static const char *
explain_get_index_name(Oid indexId)
{
	const char *result;

	if (explain_get_index_name_hook)
		result = (*explain_get_index_name_hook) (indexId);
	else
		result = NULL;
	if (result == NULL)
	{
		/* default behavior: look in the catalogs and quote it */
		result = get_rel_name(indexId);
		if (result == NULL)
			elog(ERROR, "cache lookup failed for index %u", indexId);
		result = quote_identifier(result);
	}
	return result;
}

/*
 * Show the target of a Scan node
 */
static void
ExplainScanTarget(Scan *plan, ExplainState *es)
{
	char	   *objectname = NULL;
	char	   *namespace = NULL;
	const char *objecttag = NULL;
	RangeTblEntry *rte;

	if (plan->scanrelid <= 0)	/* Is this still possible? */
		return;
	rte = rt_fetch(plan->scanrelid, es->rtable);

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_IndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
			/* Assert it's on a real relation */
			Assert(rte->rtekind == RTE_RELATION);
			objectname = get_rel_name(rte->relid);
			if (es->verbose)
				namespace = get_namespace_name(get_rel_namespace(rte->relid));
			objecttag = "Relation Name";
			break;
		case T_FunctionScan:
			{
				Node	   *funcexpr;

				/* Assert it's on a RangeFunction */
				Assert(rte->rtekind == RTE_FUNCTION);

				/*
				 * If the expression is still a function call, we can get the
				 * real name of the function.  Otherwise, punt (this can
				 * happen if the optimizer simplified away the function call,
				 * for example).
				 */
				funcexpr = ((FunctionScan *) plan)->funcexpr;
				if (funcexpr && IsA(funcexpr, FuncExpr))
				{
					Oid			funcid = ((FuncExpr *) funcexpr)->funcid;

					objectname = get_func_name(funcid);
					if (es->verbose)
						namespace =
							get_namespace_name(get_func_namespace(funcid));
				}
				objecttag = "Function Name";
			}
			break;
		case T_ValuesScan:
			Assert(rte->rtekind == RTE_VALUES);
			break;
		case T_CteScan:
			/* Assert it's on a non-self-reference CTE */
			Assert(rte->rtekind == RTE_CTE);
			Assert(!rte->self_reference);
			objectname = rte->ctename;
			objecttag = "CTE Name";
			break;
		case T_WorkTableScan:
			/* Assert it's on a self-reference CTE */
			Assert(rte->rtekind == RTE_CTE);
			Assert(rte->self_reference);
			objectname = rte->ctename;
			objecttag = "CTE Name";
			break;
		default:
			break;
	}

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		appendStringInfoString(es->str, " on");
		if (namespace != NULL)
			appendStringInfo(es->str, " %s.%s", quote_identifier(namespace),
							 quote_identifier(objectname));
		else if (objectname != NULL)
			appendStringInfo(es->str, " %s", quote_identifier(objectname));
		if (objectname == NULL ||
			strcmp(rte->eref->aliasname, objectname) != 0)
			appendStringInfo(es->str, " %s",
							 quote_identifier(rte->eref->aliasname));
	}
	else
	{
		if (objecttag != NULL && objectname != NULL)
			ExplainPropertyText(objecttag, objectname, es);
		if (namespace != NULL)
			ExplainPropertyText("Schema", namespace, es);
		ExplainPropertyText("Alias", rte->eref->aliasname, es);
	}
}

/*
 * Explain the constituent plans of a ModifyTable, Append, BitmapAnd,
 * or BitmapOr node.
 *
 * Ordinarily we don't pass down outer_plan to our child nodes, but in these
 * cases we must, since the node could be an "inner indexscan" in which case
 * outer references can appear in the child nodes.
 */
static void
ExplainMemberNodes(List *plans, PlanState **planstate, Plan *outer_plan,
				   ExplainState *es)
{
	ListCell   *lst;
	int			j = 0;

	foreach(lst, plans)
	{
		Plan	   *subnode = (Plan *) lfirst(lst);

		ExplainNode(subnode, planstate[j],
					outer_plan,
					"Member", NULL,
					es);
		j++;
	}
}

/*
 * Explain a list of SubPlans (or initPlans, which also use SubPlan nodes).
 */
static void
ExplainSubPlans(List *plans, const char *relationship, ExplainState *es)
{
	ListCell   *lst;

	foreach(lst, plans)
	{
		SubPlanState *sps = (SubPlanState *) lfirst(lst);
		SubPlan    *sp = (SubPlan *) sps->xprstate.expr;

		ExplainNode(exec_subplan_get_plan(es->pstmt, sp),
					sps->planstate,
					NULL,
					relationship, sp->plan_name,
					es);
	}
}

/*
 * Explain a property, such as sort keys or targets, that takes the form of
 * a list of unlabeled items.  "data" is a list of C strings.
 */
static void
ExplainPropertyList(const char *qlabel, List *data, ExplainState *es)
{
	ListCell   *lc;
	bool		first = true;

	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str, "%s: ", qlabel);
			foreach(lc, data)
			{
				if (!first)
					appendStringInfoString(es->str, ", ");
				appendStringInfoString(es->str, (const char *) lfirst(lc));
				first = false;
			}
			appendStringInfoChar(es->str, '\n');
			break;

		case EXPLAIN_FORMAT_XML:
			ExplainXMLTag(qlabel, X_OPENING, es);
			foreach(lc, data)
			{
				char	   *str;

				appendStringInfoSpaces(es->str, es->indent * 2 + 2);
				appendStringInfoString(es->str, "<Item>");
				str = escape_xml((const char *) lfirst(lc));
				appendStringInfoString(es->str, str);
				pfree(str);
				appendStringInfoString(es->str, "</Item>\n");
			}
			ExplainXMLTag(qlabel, X_CLOSING, es);
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, es->indent * 2);
			escape_json(es->str, qlabel);
			appendStringInfoString(es->str, ": [");
			foreach(lc, data)
			{
				if (!first)
					appendStringInfoString(es->str, ", ");
				escape_json(es->str, (const char *) lfirst(lc));
				first = false;
			}
			appendStringInfoChar(es->str, ']');
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			appendStringInfo(es->str, "%s: ", qlabel);
			foreach(lc, data)
			{
				appendStringInfoChar(es->str, '\n');
				appendStringInfoSpaces(es->str, es->indent * 2 + 2);
				appendStringInfoString(es->str, "- ");
				escape_yaml(es->str, (const char *) lfirst(lc));
			}
			break;
	}
}

/*
 * Explain a simple property.
 *
 * If "numeric" is true, the value is a number (or other value that
 * doesn't need quoting in JSON).
 *
 * This usually should not be invoked directly, but via one of the datatype
 * specific routines ExplainPropertyText, ExplainPropertyInteger, etc.
 */
static void
ExplainProperty(const char *qlabel, const char *value, bool numeric,
				ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str, "%s: %s\n", qlabel, value);
			break;

		case EXPLAIN_FORMAT_XML:
			{
				char	   *str;

				appendStringInfoSpaces(es->str, es->indent * 2);
				ExplainXMLTag(qlabel, X_OPENING | X_NOWHITESPACE, es);
				str = escape_xml(value);
				appendStringInfoString(es->str, str);
				pfree(str);
				ExplainXMLTag(qlabel, X_CLOSING | X_NOWHITESPACE, es);
				appendStringInfoChar(es->str, '\n');
			}
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, es->indent * 2);
			escape_json(es->str, qlabel);
			appendStringInfoString(es->str, ": ");
			if (numeric)
				appendStringInfoString(es->str, value);
			else
				escape_json(es->str, value);
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			appendStringInfo(es->str, "%s: ", qlabel);
			if (numeric)
				appendStringInfoString(es->str, value);
			else
				escape_yaml(es->str, value);
			break;
	}
}

/*
 * Explain an integer-valued property.
 */
static void
ExplainPropertyInteger(const char *qlabel, int value, ExplainState *es)
{
	char		buf[32];

	snprintf(buf, sizeof(buf), "%d", value);
	ExplainProperty(qlabel, buf, true, es);
}

/*
 * Explain a long-integer-valued property.
 */
static void
ExplainPropertyLong(const char *qlabel, long value, ExplainState *es)
{
	char		buf[32];

	snprintf(buf, sizeof(buf), "%ld", value);
	ExplainProperty(qlabel, buf, true, es);
}

/*
 * Explain a float-valued property, using the specified number of
 * fractional digits.
 */
static void
ExplainPropertyFloat(const char *qlabel, double value, int ndigits,
					 ExplainState *es)
{
	char		buf[256];

	snprintf(buf, sizeof(buf), "%.*f", ndigits, value);
	ExplainProperty(qlabel, buf, true, es);
}

/*
 * Open a group of related objects.
 *
 * objtype is the type of the group object, labelname is its label within
 * a containing object (if any).
 *
 * If labeled is true, the group members will be labeled properties,
 * while if it's false, they'll be unlabeled objects.
 */
static void
ExplainOpenGroup(const char *objtype, const char *labelname,
				 bool labeled, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			ExplainXMLTag(objtype, X_OPENING, es);
			es->indent++;
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, 2 * es->indent);
			if (labelname)
			{
				escape_json(es->str, labelname);
				appendStringInfoString(es->str, ": ");
			}
			appendStringInfoChar(es->str, labeled ? '{' : '[');

			/*
			 * In JSON format, the grouping_stack is an integer list.  0 means
			 * we've emitted nothing at this grouping level, 1 means we've
			 * emitted something (and so the next item needs a comma). See
			 * ExplainJSONLineEnding().
			 */
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			es->indent++;
			break;

		case EXPLAIN_FORMAT_YAML:

			/*
			 * In YAML format, the grouping stack is an integer list.  0 means
			 * we've emitted nothing at this grouping level AND this grouping
			 * level is unlabelled and must be marked with "- ".  See
			 * ExplainYAMLLineStarting().
			 */
			ExplainYAMLLineStarting(es);
			if (labelname)
			{
				appendStringInfo(es->str, "%s: ", labelname);
				es->grouping_stack = lcons_int(1, es->grouping_stack);
			}
			else
			{
				appendStringInfoString(es->str, "- ");
				es->grouping_stack = lcons_int(0, es->grouping_stack);
			}
			es->indent++;
			break;
	}
}

/*
 * Close a group of related objects.
 * Parameters must match the corresponding ExplainOpenGroup call.
 */
static void
ExplainCloseGroup(const char *objtype, const char *labelname,
				  bool labeled, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent--;
			ExplainXMLTag(objtype, X_CLOSING, es);
			break;

		case EXPLAIN_FORMAT_JSON:
			es->indent--;
			appendStringInfoChar(es->str, '\n');
			appendStringInfoSpaces(es->str, 2 * es->indent);
			appendStringInfoChar(es->str, labeled ? '}' : ']');
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;

		case EXPLAIN_FORMAT_YAML:
			es->indent--;
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;
	}
}

/*
 * Emit a "dummy" group that never has any members.
 *
 * objtype is the type of the group object, labelname is its label within
 * a containing object (if any).
 */
static void
ExplainDummyGroup(const char *objtype, const char *labelname, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			ExplainXMLTag(objtype, X_CLOSE_IMMEDIATE, es);
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, 2 * es->indent);
			if (labelname)
			{
				escape_json(es->str, labelname);
				appendStringInfoString(es->str, ": ");
			}
			escape_json(es->str, objtype);
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			if (labelname)
			{
				escape_yaml(es->str, labelname);
				appendStringInfoString(es->str, ": ");
			}
			else
			{
				appendStringInfoString(es->str, "- ");
			}
			escape_yaml(es->str, objtype);
			break;
	}
}

/*
 * Emit the start-of-output boilerplate.
 *
 * This is just enough different from processing a subgroup that we need
 * a separate pair of subroutines.
 */
void
ExplainBeginOutput(ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			appendStringInfoString(es->str,
			 "<explain xmlns=\"http://www.postgresql.org/2009/explain\">\n");
			es->indent++;
			break;

		case EXPLAIN_FORMAT_JSON:
			/* top-level structure is an array of plans */
			appendStringInfoChar(es->str, '[');
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			es->indent++;
			break;

		case EXPLAIN_FORMAT_YAML:
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			break;
	}
}

/*
 * Emit the end-of-output boilerplate.
 */
void
ExplainEndOutput(ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent--;
			appendStringInfoString(es->str, "</explain>");
			break;

		case EXPLAIN_FORMAT_JSON:
			es->indent--;
			appendStringInfoString(es->str, "\n]");
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;

		case EXPLAIN_FORMAT_YAML:
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;
	}
}

/*
 * Put an appropriate separator between multiple plans
 */
void
ExplainSeparatePlans(ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* add a blank line */
			appendStringInfoChar(es->str, '\n');
			break;

		case EXPLAIN_FORMAT_XML:
		case EXPLAIN_FORMAT_JSON:
		case EXPLAIN_FORMAT_YAML:
			/* nothing to do */
			break;
	}
}

/*
 * Emit opening or closing XML tag.
 *
 * "flags" must contain X_OPENING, X_CLOSING, or X_CLOSE_IMMEDIATE.
 * Optionally, OR in X_NOWHITESPACE to suppress the whitespace we'd normally
 * add.
 *
 * XML tag names can't contain white space, so we replace any spaces in
 * "tagname" with dashes.
 */
static void
ExplainXMLTag(const char *tagname, int flags, ExplainState *es)
{
	const char *s;

	if ((flags & X_NOWHITESPACE) == 0)
		appendStringInfoSpaces(es->str, 2 * es->indent);
	appendStringInfoCharMacro(es->str, '<');
	if ((flags & X_CLOSING) != 0)
		appendStringInfoCharMacro(es->str, '/');
	for (s = tagname; *s; s++)
		appendStringInfoCharMacro(es->str, (*s == ' ') ? '-' : *s);
	if ((flags & X_CLOSE_IMMEDIATE) != 0)
		appendStringInfoString(es->str, " /");
	appendStringInfoCharMacro(es->str, '>');
	if ((flags & X_NOWHITESPACE) == 0)
		appendStringInfoCharMacro(es->str, '\n');
}

/*
 * Emit a JSON line ending.
 *
 * JSON requires a comma after each property but the last.  To facilitate this,
 * in JSON format, the text emitted for each property begins just prior to the
 * preceding line-break (and comma, if applicable).
 */
static void
ExplainJSONLineEnding(ExplainState *es)
{
	Assert(es->format == EXPLAIN_FORMAT_JSON);
	if (linitial_int(es->grouping_stack) != 0)
		appendStringInfoChar(es->str, ',');
	else
		linitial_int(es->grouping_stack) = 1;
	appendStringInfoChar(es->str, '\n');
}

/*
 * Indent a YAML line.
 *
 * YAML lines are ordinarily indented by two spaces per indentation level.
 * The text emitted for each property begins just prior to the preceding
 * line-break, except for the first property in an unlabelled group, for which
 * it begins immediately after the "- " that introduces the group.  The first
 * property of the group appears on the same line as the opening "- ".
 */
static void
ExplainYAMLLineStarting(ExplainState *es)
{
	Assert(es->format == EXPLAIN_FORMAT_YAML);
	if (linitial_int(es->grouping_stack) == 0)
	{
		linitial_int(es->grouping_stack) = 1;
	}
	else
	{
		appendStringInfoChar(es->str, '\n');
		appendStringInfoSpaces(es->str, es->indent * 2);
	}
}

/*
 * Produce a JSON string literal, properly escaping characters in the text.
 */
static void
escape_json(StringInfo buf, const char *str)
{
	const char *p;

	appendStringInfoCharMacro(buf, '\"');
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			case '"':
				appendStringInfoString(buf, "\\\"");
				break;
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			default:
				if ((unsigned char) *p < ' ')
					appendStringInfo(buf, "\\u%04x", (int) *p);
				else
					appendStringInfoCharMacro(buf, *p);
				break;
		}
	}
	appendStringInfoCharMacro(buf, '\"');
}

/*
 * YAML is a superset of JSON; unfortuantely, the YAML quoting rules are
 * ridiculously complicated -- as documented in sections 5.3 and 7.3.3 of
 * http://yaml.org/spec/1.2/spec.html -- so we chose to just quote everything.
 * Empty strings, strings with leading or trailing whitespace, and strings
 * containing a variety of special characters must certainly be quoted or the
 * output is invalid; and other seemingly harmless strings like "0xa" or
 * "true" must be quoted, lest they be interpreted as a hexadecimal or Boolean
 * constant rather than a string.
 */
static void
escape_yaml(StringInfo buf, const char *str)
{
	escape_json(buf, str);
}
