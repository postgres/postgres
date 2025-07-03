/*-------------------------------------------------------------------------
 *
 * clauses.c
 *	  routines to manipulate qualification clauses
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/clauses.c
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Nov 3, 1994		clause.c and clauses.c combined
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_language.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/multibitmapset.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "nodes/supportnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

typedef struct
{
	ParamListInfo boundParams;
	PlannerInfo *root;
	List	   *active_fns;
	Node	   *case_val;
	bool		estimate;
} eval_const_expressions_context;

typedef struct
{
	int			nargs;
	List	   *args;
	int		   *usecounts;
} substitute_actual_parameters_context;

typedef struct
{
	int			nargs;
	List	   *args;
	int			sublevels_up;
} substitute_actual_srf_parameters_context;

typedef struct
{
	char	   *proname;
	char	   *prosrc;
} inline_error_callback_arg;

typedef struct
{
	char		max_hazard;		/* worst proparallel hazard found so far */
	char		max_interesting;	/* worst proparallel hazard of interest */
	List	   *safe_param_ids; /* PARAM_EXEC Param IDs to treat as safe */
} max_parallel_hazard_context;

static bool contain_agg_clause_walker(Node *node, void *context);
static bool find_window_functions_walker(Node *node, WindowFuncLists *lists);
static bool contain_subplans_walker(Node *node, void *context);
static bool contain_mutable_functions_walker(Node *node, void *context);
static bool contain_volatile_functions_walker(Node *node, void *context);
static bool contain_volatile_functions_not_nextval_walker(Node *node, void *context);
static bool max_parallel_hazard_walker(Node *node,
									   max_parallel_hazard_context *context);
static bool contain_nonstrict_functions_walker(Node *node, void *context);
static bool contain_exec_param_walker(Node *node, List *param_ids);
static bool contain_context_dependent_node(Node *clause);
static bool contain_context_dependent_node_walker(Node *node, int *flags);
static bool contain_leaked_vars_walker(Node *node, void *context);
static Relids find_nonnullable_rels_walker(Node *node, bool top_level);
static List *find_nonnullable_vars_walker(Node *node, bool top_level);
static bool is_strict_saop(ScalarArrayOpExpr *expr, bool falseOK);
static bool convert_saop_to_hashed_saop_walker(Node *node, void *context);
static Node *eval_const_expressions_mutator(Node *node,
											eval_const_expressions_context *context);
static bool contain_non_const_walker(Node *node, void *context);
static bool ece_function_is_safe(Oid funcid,
								 eval_const_expressions_context *context);
static List *simplify_or_arguments(List *args,
								   eval_const_expressions_context *context,
								   bool *haveNull, bool *forceTrue);
static List *simplify_and_arguments(List *args,
									eval_const_expressions_context *context,
									bool *haveNull, bool *forceFalse);
static Node *simplify_boolean_equality(Oid opno, List *args);
static Expr *simplify_function(Oid funcid,
							   Oid result_type, int32 result_typmod,
							   Oid result_collid, Oid input_collid, List **args_p,
							   bool funcvariadic, bool process_args, bool allow_non_const,
							   eval_const_expressions_context *context);
static List *reorder_function_arguments(List *args, int pronargs,
										HeapTuple func_tuple);
static List *add_function_defaults(List *args, int pronargs,
								   HeapTuple func_tuple);
static List *fetch_function_defaults(HeapTuple func_tuple);
static void recheck_cast_function_args(List *args, Oid result_type,
									   Oid *proargtypes, int pronargs,
									   HeapTuple func_tuple);
static Expr *evaluate_function(Oid funcid, Oid result_type, int32 result_typmod,
							   Oid result_collid, Oid input_collid, List *args,
							   bool funcvariadic,
							   HeapTuple func_tuple,
							   eval_const_expressions_context *context);
static Expr *inline_function(Oid funcid, Oid result_type, Oid result_collid,
							 Oid input_collid, List *args,
							 bool funcvariadic,
							 HeapTuple func_tuple,
							 eval_const_expressions_context *context);
static Node *substitute_actual_parameters(Node *expr, int nargs, List *args,
										  int *usecounts);
static Node *substitute_actual_parameters_mutator(Node *node,
												  substitute_actual_parameters_context *context);
static void sql_inline_error_callback(void *arg);
static Query *substitute_actual_srf_parameters(Query *expr,
											   int nargs, List *args);
static Node *substitute_actual_srf_parameters_mutator(Node *node,
													  substitute_actual_srf_parameters_context *context);
static bool pull_paramids_walker(Node *node, Bitmapset **context);


/*****************************************************************************
 *		Aggregate-function clause manipulation
 *****************************************************************************/

/*
 * contain_agg_clause
 *	  Recursively search for Aggref/GroupingFunc nodes within a clause.
 *
 *	  Returns true if any aggregate found.
 *
 * This does not descend into subqueries, and so should be used only after
 * reduction of sublinks to subplans, or in contexts where it's known there
 * are no subqueries.  There mustn't be outer-aggregate references either.
 *
 * (If you want something like this but able to deal with subqueries,
 * see rewriteManip.c's contain_aggs_of_level().)
 */
bool
contain_agg_clause(Node *clause)
{
	return contain_agg_clause_walker(clause, NULL);
}

static bool
contain_agg_clause_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Assert(((Aggref *) node)->agglevelsup == 0);
		return true;			/* abort the tree traversal and return true */
	}
	if (IsA(node, GroupingFunc))
	{
		Assert(((GroupingFunc *) node)->agglevelsup == 0);
		return true;			/* abort the tree traversal and return true */
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_walker(node, contain_agg_clause_walker, context);
}

/*****************************************************************************
 *		Window-function clause manipulation
 *****************************************************************************/

/*
 * contain_window_function
 *	  Recursively search for WindowFunc nodes within a clause.
 *
 * Since window functions don't have level fields, but are hard-wired to
 * be associated with the current query level, this is just the same as
 * rewriteManip.c's function.
 */
bool
contain_window_function(Node *clause)
{
	return contain_windowfuncs(clause);
}

/*
 * find_window_functions
 *	  Locate all the WindowFunc nodes in an expression tree, and organize
 *	  them by winref ID number.
 *
 * Caller must provide an upper bound on the winref IDs expected in the tree.
 */
WindowFuncLists *
find_window_functions(Node *clause, Index maxWinRef)
{
	WindowFuncLists *lists = palloc(sizeof(WindowFuncLists));

	lists->numWindowFuncs = 0;
	lists->maxWinRef = maxWinRef;
	lists->windowFuncs = (List **) palloc0((maxWinRef + 1) * sizeof(List *));
	(void) find_window_functions_walker(clause, lists);
	return lists;
}

static bool
find_window_functions_walker(Node *node, WindowFuncLists *lists)
{
	if (node == NULL)
		return false;
	if (IsA(node, WindowFunc))
	{
		WindowFunc *wfunc = (WindowFunc *) node;

		/* winref is unsigned, so one-sided test is OK */
		if (wfunc->winref > lists->maxWinRef)
			elog(ERROR, "WindowFunc contains out-of-range winref %u",
				 wfunc->winref);
		/* eliminate duplicates, so that we avoid repeated computation */
		if (!list_member(lists->windowFuncs[wfunc->winref], wfunc))
		{
			lists->windowFuncs[wfunc->winref] =
				lappend(lists->windowFuncs[wfunc->winref], wfunc);
			lists->numWindowFuncs++;
		}

		/*
		 * We assume that the parser checked that there are no window
		 * functions in the arguments or filter clause.  Hence, we need not
		 * recurse into them.  (If either the parser or the planner screws up
		 * on this point, the executor will still catch it; see ExecInitExpr.)
		 */
		return false;
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_walker(node, find_window_functions_walker, lists);
}


/*****************************************************************************
 *		Support for expressions returning sets
 *****************************************************************************/

/*
 * expression_returns_set_rows
 *	  Estimate the number of rows returned by a set-returning expression.
 *	  The result is 1 if it's not a set-returning expression.
 *
 * We should only examine the top-level function or operator; it used to be
 * appropriate to recurse, but not anymore.  (Even if there are more SRFs in
 * the function's inputs, their multipliers are accounted for separately.)
 *
 * Note: keep this in sync with expression_returns_set() in nodes/nodeFuncs.c.
 */
double
expression_returns_set_rows(PlannerInfo *root, Node *clause)
{
	if (clause == NULL)
		return 1.0;
	if (IsA(clause, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) clause;

		if (expr->funcretset)
			return clamp_row_est(get_function_rows(root, expr->funcid, clause));
	}
	if (IsA(clause, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) clause;

		if (expr->opretset)
		{
			set_opfuncid(expr);
			return clamp_row_est(get_function_rows(root, expr->opfuncid, clause));
		}
	}
	return 1.0;
}


/*****************************************************************************
 *		Subplan clause manipulation
 *****************************************************************************/

/*
 * contain_subplans
 *	  Recursively search for subplan nodes within a clause.
 *
 * If we see a SubLink node, we will return true.  This is only possible if
 * the expression tree hasn't yet been transformed by subselect.c.  We do not
 * know whether the node will produce a true subplan or just an initplan,
 * but we make the conservative assumption that it will be a subplan.
 *
 * Returns true if any subplan found.
 */
bool
contain_subplans(Node *clause)
{
	return contain_subplans_walker(clause, NULL);
}

static bool
contain_subplans_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubPlan) ||
		IsA(node, AlternativeSubPlan) ||
		IsA(node, SubLink))
		return true;			/* abort the tree traversal and return true */
	return expression_tree_walker(node, contain_subplans_walker, context);
}


/*****************************************************************************
 *		Check clauses for mutable functions
 *****************************************************************************/

/*
 * contain_mutable_functions
 *	  Recursively search for mutable functions within a clause.
 *
 * Returns true if any mutable function (or operator implemented by a
 * mutable function) is found.  This test is needed so that we don't
 * mistakenly think that something like "WHERE random() < 0.5" can be treated
 * as a constant qualification.
 *
 * This will give the right answer only for clauses that have been put
 * through expression preprocessing.  Callers outside the planner typically
 * should use contain_mutable_functions_after_planning() instead, for the
 * reasons given there.
 *
 * We will recursively look into Query nodes (i.e., SubLink sub-selects)
 * but not into SubPlans.  See comments for contain_volatile_functions().
 */
bool
contain_mutable_functions(Node *clause)
{
	return contain_mutable_functions_walker(clause, NULL);
}

static bool
contain_mutable_functions_checker(Oid func_id, void *context)
{
	return (func_volatile(func_id) != PROVOLATILE_IMMUTABLE);
}

static bool
contain_mutable_functions_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	/* Check for mutable functions in node itself */
	if (check_functions_in_node(node, contain_mutable_functions_checker,
								context))
		return true;

	if (IsA(node, JsonConstructorExpr))
	{
		const JsonConstructorExpr *ctor = (JsonConstructorExpr *) node;
		ListCell   *lc;
		bool		is_jsonb;

		is_jsonb = ctor->returning->format->format_type == JS_FORMAT_JSONB;

		/*
		 * Check argument_type => json[b] conversions specifically.  We still
		 * recurse to check 'args' below, but here we want to specifically
		 * check whether or not the emitted clause would fail to be immutable
		 * because of TimeZone, for example.
		 */
		foreach(lc, ctor->args)
		{
			Oid			typid = exprType(lfirst(lc));

			if (is_jsonb ?
				!to_jsonb_is_immutable(typid) :
				!to_json_is_immutable(typid))
				return true;
		}

		/* Check all subnodes */
	}

	if (IsA(node, JsonExpr))
	{
		JsonExpr   *jexpr = castNode(JsonExpr, node);
		Const	   *cnst;

		if (!IsA(jexpr->path_spec, Const))
			return true;

		cnst = castNode(Const, jexpr->path_spec);

		Assert(cnst->consttype == JSONPATHOID);
		if (cnst->constisnull)
			return false;

		if (jspIsMutable(DatumGetJsonPathP(cnst->constvalue),
						 jexpr->passing_names, jexpr->passing_values))
			return true;
	}

	if (IsA(node, SQLValueFunction))
	{
		/* all variants of SQLValueFunction are stable */
		return true;
	}

	if (IsA(node, NextValueExpr))
	{
		/* NextValueExpr is volatile */
		return true;
	}

	/*
	 * It should be safe to treat MinMaxExpr as immutable, because it will
	 * depend on a non-cross-type btree comparison function, and those should
	 * always be immutable.  Treating XmlExpr as immutable is more dubious,
	 * and treating CoerceToDomain as immutable is outright dangerous.  But we
	 * have done so historically, and changing this would probably cause more
	 * problems than it would fix.  In practice, if you have a non-immutable
	 * domain constraint you are in for pain anyhow.
	 */

	/* Recurse to check arguments */
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		return query_tree_walker((Query *) node,
								 contain_mutable_functions_walker,
								 context, 0);
	}
	return expression_tree_walker(node, contain_mutable_functions_walker,
								  context);
}

/*
 * contain_mutable_functions_after_planning
 *	  Test whether given expression contains mutable functions.
 *
 * This is a wrapper for contain_mutable_functions() that is safe to use from
 * outside the planner.  The difference is that it first runs the expression
 * through expression_planner().  There are two key reasons why we need that:
 *
 * First, function default arguments will get inserted, which may affect
 * volatility (consider "default now()").
 *
 * Second, inline-able functions will get inlined, which may allow us to
 * conclude that the function is really less volatile than it's marked.
 * As an example, polymorphic functions must be marked with the most volatile
 * behavior that they have for any input type, but once we inline the
 * function we may be able to conclude that it's not so volatile for the
 * particular input type we're dealing with.
 */
bool
contain_mutable_functions_after_planning(Expr *expr)
{
	/* We assume here that expression_planner() won't scribble on its input */
	expr = expression_planner(expr);

	/* Now we can search for non-immutable functions */
	return contain_mutable_functions((Node *) expr);
}


/*****************************************************************************
 *		Check clauses for volatile functions
 *****************************************************************************/

/*
 * contain_volatile_functions
 *	  Recursively search for volatile functions within a clause.
 *
 * Returns true if any volatile function (or operator implemented by a
 * volatile function) is found. This test prevents, for example,
 * invalid conversions of volatile expressions into indexscan quals.
 *
 * This will give the right answer only for clauses that have been put
 * through expression preprocessing.  Callers outside the planner typically
 * should use contain_volatile_functions_after_planning() instead, for the
 * reasons given there.
 *
 * We will recursively look into Query nodes (i.e., SubLink sub-selects)
 * but not into SubPlans.  This is a bit odd, but intentional.  If we are
 * looking at a SubLink, we are probably deciding whether a query tree
 * transformation is safe, and a contained sub-select should affect that;
 * for example, duplicating a sub-select containing a volatile function
 * would be bad.  However, once we've got to the stage of having SubPlans,
 * subsequent planning need not consider volatility within those, since
 * the executor won't change its evaluation rules for a SubPlan based on
 * volatility.
 *
 * For some node types, for example, RestrictInfo and PathTarget, we cache
 * whether we found any volatile functions or not and reuse that value in any
 * future checks for that node.  All of the logic for determining if the
 * cached value should be set to VOLATILITY_NOVOLATILE or VOLATILITY_VOLATILE
 * belongs in this function.  Any code which makes changes to these nodes
 * which could change the outcome this function must set the cached value back
 * to VOLATILITY_UNKNOWN.  That allows this function to redetermine the
 * correct value during the next call, should we need to redetermine if the
 * node contains any volatile functions again in the future.
 */
bool
contain_volatile_functions(Node *clause)
{
	return contain_volatile_functions_walker(clause, NULL);
}

static bool
contain_volatile_functions_checker(Oid func_id, void *context)
{
	return (func_volatile(func_id) == PROVOLATILE_VOLATILE);
}

static bool
contain_volatile_functions_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	/* Check for volatile functions in node itself */
	if (check_functions_in_node(node, contain_volatile_functions_checker,
								context))
		return true;

	if (IsA(node, NextValueExpr))
	{
		/* NextValueExpr is volatile */
		return true;
	}

	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) node;

		/*
		 * For RestrictInfo, check if we've checked the volatility of it
		 * before.  If so, we can just use the cached value and not bother
		 * checking it again.  Otherwise, check it and cache if whether we
		 * found any volatile functions.
		 */
		if (rinfo->has_volatile == VOLATILITY_NOVOLATILE)
			return false;
		else if (rinfo->has_volatile == VOLATILITY_VOLATILE)
			return true;
		else
		{
			bool		hasvolatile;

			hasvolatile = contain_volatile_functions_walker((Node *) rinfo->clause,
															context);
			if (hasvolatile)
				rinfo->has_volatile = VOLATILITY_VOLATILE;
			else
				rinfo->has_volatile = VOLATILITY_NOVOLATILE;

			return hasvolatile;
		}
	}

	if (IsA(node, PathTarget))
	{
		PathTarget *target = (PathTarget *) node;

		/*
		 * We also do caching for PathTarget the same as we do above for
		 * RestrictInfos.
		 */
		if (target->has_volatile_expr == VOLATILITY_NOVOLATILE)
			return false;
		else if (target->has_volatile_expr == VOLATILITY_VOLATILE)
			return true;
		else
		{
			bool		hasvolatile;

			hasvolatile = contain_volatile_functions_walker((Node *) target->exprs,
															context);

			if (hasvolatile)
				target->has_volatile_expr = VOLATILITY_VOLATILE;
			else
				target->has_volatile_expr = VOLATILITY_NOVOLATILE;

			return hasvolatile;
		}
	}

	/*
	 * See notes in contain_mutable_functions_walker about why we treat
	 * MinMaxExpr, XmlExpr, and CoerceToDomain as immutable, while
	 * SQLValueFunction is stable.  Hence, none of them are of interest here.
	 */

	/* Recurse to check arguments */
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		return query_tree_walker((Query *) node,
								 contain_volatile_functions_walker,
								 context, 0);
	}
	return expression_tree_walker(node, contain_volatile_functions_walker,
								  context);
}

/*
 * contain_volatile_functions_after_planning
 *	  Test whether given expression contains volatile functions.
 *
 * This is a wrapper for contain_volatile_functions() that is safe to use from
 * outside the planner.  The difference is that it first runs the expression
 * through expression_planner().  There are two key reasons why we need that:
 *
 * First, function default arguments will get inserted, which may affect
 * volatility (consider "default random()").
 *
 * Second, inline-able functions will get inlined, which may allow us to
 * conclude that the function is really less volatile than it's marked.
 * As an example, polymorphic functions must be marked with the most volatile
 * behavior that they have for any input type, but once we inline the
 * function we may be able to conclude that it's not so volatile for the
 * particular input type we're dealing with.
 */
bool
contain_volatile_functions_after_planning(Expr *expr)
{
	/* We assume here that expression_planner() won't scribble on its input */
	expr = expression_planner(expr);

	/* Now we can search for volatile functions */
	return contain_volatile_functions((Node *) expr);
}

/*
 * Special purpose version of contain_volatile_functions() for use in COPY:
 * ignore nextval(), but treat all other functions normally.
 */
bool
contain_volatile_functions_not_nextval(Node *clause)
{
	return contain_volatile_functions_not_nextval_walker(clause, NULL);
}

static bool
contain_volatile_functions_not_nextval_checker(Oid func_id, void *context)
{
	return (func_id != F_NEXTVAL &&
			func_volatile(func_id) == PROVOLATILE_VOLATILE);
}

static bool
contain_volatile_functions_not_nextval_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	/* Check for volatile functions in node itself */
	if (check_functions_in_node(node,
								contain_volatile_functions_not_nextval_checker,
								context))
		return true;

	/*
	 * See notes in contain_mutable_functions_walker about why we treat
	 * MinMaxExpr, XmlExpr, and CoerceToDomain as immutable, while
	 * SQLValueFunction is stable.  Hence, none of them are of interest here.
	 * Also, since we're intentionally ignoring nextval(), presumably we
	 * should ignore NextValueExpr.
	 */

	/* Recurse to check arguments */
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		return query_tree_walker((Query *) node,
								 contain_volatile_functions_not_nextval_walker,
								 context, 0);
	}
	return expression_tree_walker(node,
								  contain_volatile_functions_not_nextval_walker,
								  context);
}


/*****************************************************************************
 *		Check queries for parallel unsafe and/or restricted constructs
 *****************************************************************************/

/*
 * max_parallel_hazard
 *		Find the worst parallel-hazard level in the given query
 *
 * Returns the worst function hazard property (the earliest in this list:
 * PROPARALLEL_UNSAFE, PROPARALLEL_RESTRICTED, PROPARALLEL_SAFE) that can
 * be found in the given parsetree.  We use this to find out whether the query
 * can be parallelized at all.  The caller will also save the result in
 * PlannerGlobal so as to short-circuit checks of portions of the querytree
 * later, in the common case where everything is SAFE.
 */
char
max_parallel_hazard(Query *parse)
{
	max_parallel_hazard_context context;

	context.max_hazard = PROPARALLEL_SAFE;
	context.max_interesting = PROPARALLEL_UNSAFE;
	context.safe_param_ids = NIL;
	(void) max_parallel_hazard_walker((Node *) parse, &context);
	return context.max_hazard;
}

/*
 * is_parallel_safe
 *		Detect whether the given expr contains only parallel-safe functions
 *
 * root->glob->maxParallelHazard must previously have been set to the
 * result of max_parallel_hazard() on the whole query.
 */
bool
is_parallel_safe(PlannerInfo *root, Node *node)
{
	max_parallel_hazard_context context;
	PlannerInfo *proot;
	ListCell   *l;

	/*
	 * Even if the original querytree contained nothing unsafe, we need to
	 * search the expression if we have generated any PARAM_EXEC Params while
	 * planning, because those are parallel-restricted and there might be one
	 * in this expression.  But otherwise we don't need to look.
	 */
	if (root->glob->maxParallelHazard == PROPARALLEL_SAFE &&
		root->glob->paramExecTypes == NIL)
		return true;
	/* Else use max_parallel_hazard's search logic, but stop on RESTRICTED */
	context.max_hazard = PROPARALLEL_SAFE;
	context.max_interesting = PROPARALLEL_RESTRICTED;
	context.safe_param_ids = NIL;

	/*
	 * The params that refer to the same or parent query level are considered
	 * parallel-safe.  The idea is that we compute such params at Gather or
	 * Gather Merge node and pass their value to workers.
	 */
	for (proot = root; proot != NULL; proot = proot->parent_root)
	{
		foreach(l, proot->init_plans)
		{
			SubPlan    *initsubplan = (SubPlan *) lfirst(l);

			context.safe_param_ids = list_concat(context.safe_param_ids,
												 initsubplan->setParam);
		}
	}

	return !max_parallel_hazard_walker(node, &context);
}

/* core logic for all parallel-hazard checks */
static bool
max_parallel_hazard_test(char proparallel, max_parallel_hazard_context *context)
{
	switch (proparallel)
	{
		case PROPARALLEL_SAFE:
			/* nothing to see here, move along */
			break;
		case PROPARALLEL_RESTRICTED:
			/* increase max_hazard to RESTRICTED */
			Assert(context->max_hazard != PROPARALLEL_UNSAFE);
			context->max_hazard = proparallel;
			/* done if we are not expecting any unsafe functions */
			if (context->max_interesting == proparallel)
				return true;
			break;
		case PROPARALLEL_UNSAFE:
			context->max_hazard = proparallel;
			/* we're always done at the first unsafe construct */
			return true;
		default:
			elog(ERROR, "unrecognized proparallel value \"%c\"", proparallel);
			break;
	}
	return false;
}

/* check_functions_in_node callback */
static bool
max_parallel_hazard_checker(Oid func_id, void *context)
{
	return max_parallel_hazard_test(func_parallel(func_id),
									(max_parallel_hazard_context *) context);
}

static bool
max_parallel_hazard_walker(Node *node, max_parallel_hazard_context *context)
{
	if (node == NULL)
		return false;

	/* Check for hazardous functions in node itself */
	if (check_functions_in_node(node, max_parallel_hazard_checker,
								context))
		return true;

	/*
	 * It should be OK to treat MinMaxExpr as parallel-safe, since btree
	 * opclass support functions are generally parallel-safe.  XmlExpr is a
	 * bit more dubious but we can probably get away with it.  We err on the
	 * side of caution by treating CoerceToDomain as parallel-restricted.
	 * (Note: in principle that's wrong because a domain constraint could
	 * contain a parallel-unsafe function; but useful constraints probably
	 * never would have such, and assuming they do would cripple use of
	 * parallel query in the presence of domain types.)  SQLValueFunction
	 * should be safe in all cases.  NextValueExpr is parallel-unsafe.
	 */
	if (IsA(node, CoerceToDomain))
	{
		if (max_parallel_hazard_test(PROPARALLEL_RESTRICTED, context))
			return true;
	}

	else if (IsA(node, NextValueExpr))
	{
		if (max_parallel_hazard_test(PROPARALLEL_UNSAFE, context))
			return true;
	}

	/*
	 * Treat window functions as parallel-restricted because we aren't sure
	 * whether the input row ordering is fully deterministic, and the output
	 * of window functions might vary across workers if not.  (In some cases,
	 * like where the window frame orders by a primary key, we could relax
	 * this restriction.  But it doesn't currently seem worth expending extra
	 * effort to do so.)
	 */
	else if (IsA(node, WindowFunc))
	{
		if (max_parallel_hazard_test(PROPARALLEL_RESTRICTED, context))
			return true;
	}

	/*
	 * As a notational convenience for callers, look through RestrictInfo.
	 */
	else if (IsA(node, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) node;

		return max_parallel_hazard_walker((Node *) rinfo->clause, context);
	}

	/*
	 * Really we should not see SubLink during a max_interesting == restricted
	 * scan, but if we do, return true.
	 */
	else if (IsA(node, SubLink))
	{
		if (max_parallel_hazard_test(PROPARALLEL_RESTRICTED, context))
			return true;
	}

	/*
	 * Only parallel-safe SubPlans can be sent to workers.  Within the
	 * testexpr of the SubPlan, Params representing the output columns of the
	 * subplan can be treated as parallel-safe, so temporarily add their IDs
	 * to the safe_param_ids list while examining the testexpr.
	 */
	else if (IsA(node, SubPlan))
	{
		SubPlan    *subplan = (SubPlan *) node;
		List	   *save_safe_param_ids;

		if (!subplan->parallel_safe &&
			max_parallel_hazard_test(PROPARALLEL_RESTRICTED, context))
			return true;
		save_safe_param_ids = context->safe_param_ids;
		context->safe_param_ids = list_concat_copy(context->safe_param_ids,
												   subplan->paramIds);
		if (max_parallel_hazard_walker(subplan->testexpr, context))
			return true;		/* no need to restore safe_param_ids */
		list_free(context->safe_param_ids);
		context->safe_param_ids = save_safe_param_ids;
		/* we must also check args, but no special Param treatment there */
		if (max_parallel_hazard_walker((Node *) subplan->args, context))
			return true;
		/* don't want to recurse normally, so we're done */
		return false;
	}

	/*
	 * We can't pass Params to workers at the moment either, so they are also
	 * parallel-restricted, unless they are PARAM_EXTERN Params or are
	 * PARAM_EXEC Params listed in safe_param_ids, meaning they could be
	 * either generated within workers or can be computed by the leader and
	 * then their value can be passed to workers.
	 */
	else if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN)
			return false;

		if (param->paramkind != PARAM_EXEC ||
			!list_member_int(context->safe_param_ids, param->paramid))
		{
			if (max_parallel_hazard_test(PROPARALLEL_RESTRICTED, context))
				return true;
		}
		return false;			/* nothing to recurse to */
	}

	/*
	 * When we're first invoked on a completely unplanned tree, we must
	 * recurse into subqueries so to as to locate parallel-unsafe constructs
	 * anywhere in the tree.
	 */
	else if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;

		/* SELECT FOR UPDATE/SHARE must be treated as unsafe */
		if (query->rowMarks != NULL)
		{
			context->max_hazard = PROPARALLEL_UNSAFE;
			return true;
		}

		/* Recurse into subselects */
		return query_tree_walker(query,
								 max_parallel_hazard_walker,
								 context, 0);
	}

	/* Recurse to check arguments */
	return expression_tree_walker(node,
								  max_parallel_hazard_walker,
								  context);
}


/*****************************************************************************
 *		Check clauses for nonstrict functions
 *****************************************************************************/

/*
 * contain_nonstrict_functions
 *	  Recursively search for nonstrict functions within a clause.
 *
 * Returns true if any nonstrict construct is found --- ie, anything that
 * could produce non-NULL output with a NULL input.
 *
 * The idea here is that the caller has verified that the expression contains
 * one or more Var or Param nodes (as appropriate for the caller's need), and
 * now wishes to prove that the expression result will be NULL if any of these
 * inputs is NULL.  If we return false, then the proof succeeded.
 */
bool
contain_nonstrict_functions(Node *clause)
{
	return contain_nonstrict_functions_walker(clause, NULL);
}

static bool
contain_nonstrict_functions_checker(Oid func_id, void *context)
{
	return !func_strict(func_id);
}

static bool
contain_nonstrict_functions_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		/* an aggregate could return non-null with null input */
		return true;
	}
	if (IsA(node, GroupingFunc))
	{
		/*
		 * A GroupingFunc doesn't evaluate its arguments, and therefore must
		 * be treated as nonstrict.
		 */
		return true;
	}
	if (IsA(node, WindowFunc))
	{
		/* a window function could return non-null with null input */
		return true;
	}
	if (IsA(node, SubscriptingRef))
	{
		SubscriptingRef *sbsref = (SubscriptingRef *) node;
		const SubscriptRoutines *sbsroutines;

		/* Subscripting assignment is always presumed nonstrict */
		if (sbsref->refassgnexpr != NULL)
			return true;
		/* Otherwise we must look up the subscripting support methods */
		sbsroutines = getSubscriptingRoutines(sbsref->refcontainertype, NULL);
		if (!(sbsroutines && sbsroutines->fetch_strict))
			return true;
		/* else fall through to check args */
	}
	if (IsA(node, DistinctExpr))
	{
		/* IS DISTINCT FROM is inherently non-strict */
		return true;
	}
	if (IsA(node, NullIfExpr))
	{
		/* NULLIF is inherently non-strict */
		return true;
	}
	if (IsA(node, BoolExpr))
	{
		BoolExpr   *expr = (BoolExpr *) node;

		switch (expr->boolop)
		{
			case AND_EXPR:
			case OR_EXPR:
				/* AND, OR are inherently non-strict */
				return true;
			default:
				break;
		}
	}
	if (IsA(node, SubLink))
	{
		/* In some cases a sublink might be strict, but in general not */
		return true;
	}
	if (IsA(node, SubPlan))
		return true;
	if (IsA(node, AlternativeSubPlan))
		return true;
	if (IsA(node, FieldStore))
		return true;
	if (IsA(node, CoerceViaIO))
	{
		/*
		 * CoerceViaIO is strict regardless of whether the I/O functions are,
		 * so just go look at its argument; asking check_functions_in_node is
		 * useless expense and could deliver the wrong answer.
		 */
		return contain_nonstrict_functions_walker((Node *) ((CoerceViaIO *) node)->arg,
												  context);
	}
	if (IsA(node, ArrayCoerceExpr))
	{
		/*
		 * ArrayCoerceExpr is strict at the array level, regardless of what
		 * the per-element expression is; so we should ignore elemexpr and
		 * recurse only into the arg.
		 */
		return contain_nonstrict_functions_walker((Node *) ((ArrayCoerceExpr *) node)->arg,
												  context);
	}
	if (IsA(node, CaseExpr))
		return true;
	if (IsA(node, ArrayExpr))
		return true;
	if (IsA(node, RowExpr))
		return true;
	if (IsA(node, RowCompareExpr))
		return true;
	if (IsA(node, CoalesceExpr))
		return true;
	if (IsA(node, MinMaxExpr))
		return true;
	if (IsA(node, XmlExpr))
		return true;
	if (IsA(node, NullTest))
		return true;
	if (IsA(node, BooleanTest))
		return true;

	/* Check other function-containing nodes */
	if (check_functions_in_node(node, contain_nonstrict_functions_checker,
								context))
		return true;

	return expression_tree_walker(node, contain_nonstrict_functions_walker,
								  context);
}

/*****************************************************************************
 *		Check clauses for Params
 *****************************************************************************/

/*
 * contain_exec_param
 *	  Recursively search for PARAM_EXEC Params within a clause.
 *
 * Returns true if the clause contains any PARAM_EXEC Param with a paramid
 * appearing in the given list of Param IDs.  Does not descend into
 * subqueries!
 */
bool
contain_exec_param(Node *clause, List *param_ids)
{
	return contain_exec_param_walker(clause, param_ids);
}

static bool
contain_exec_param_walker(Node *node, List *param_ids)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *p = (Param *) node;

		if (p->paramkind == PARAM_EXEC &&
			list_member_int(param_ids, p->paramid))
			return true;
	}
	return expression_tree_walker(node, contain_exec_param_walker, param_ids);
}

/*****************************************************************************
 *		Check clauses for context-dependent nodes
 *****************************************************************************/

/*
 * contain_context_dependent_node
 *	  Recursively search for context-dependent nodes within a clause.
 *
 * CaseTestExpr nodes must appear directly within the corresponding CaseExpr,
 * not nested within another one, or they'll see the wrong test value.  If one
 * appears "bare" in the arguments of a SQL function, then we can't inline the
 * SQL function for fear of creating such a situation.  The same applies for
 * CaseTestExpr used within the elemexpr of an ArrayCoerceExpr.
 *
 * CoerceToDomainValue would have the same issue if domain CHECK expressions
 * could get inlined into larger expressions, but presently that's impossible.
 * Still, it might be allowed in future, or other node types with similar
 * issues might get invented.  So give this function a generic name, and set
 * up the recursion state to allow multiple flag bits.
 */
static bool
contain_context_dependent_node(Node *clause)
{
	int			flags = 0;

	return contain_context_dependent_node_walker(clause, &flags);
}

#define CCDN_CASETESTEXPR_OK	0x0001	/* CaseTestExpr okay here? */

static bool
contain_context_dependent_node_walker(Node *node, int *flags)
{
	if (node == NULL)
		return false;
	if (IsA(node, CaseTestExpr))
		return !(*flags & CCDN_CASETESTEXPR_OK);
	else if (IsA(node, CaseExpr))
	{
		CaseExpr   *caseexpr = (CaseExpr *) node;

		/*
		 * If this CASE doesn't have a test expression, then it doesn't create
		 * a context in which CaseTestExprs should appear, so just fall
		 * through and treat it as a generic expression node.
		 */
		if (caseexpr->arg)
		{
			int			save_flags = *flags;
			bool		res;

			/*
			 * Note: in principle, we could distinguish the various sub-parts
			 * of a CASE construct and set the flag bit only for some of them,
			 * since we are only expecting CaseTestExprs to appear in the
			 * "expr" subtree of the CaseWhen nodes.  But it doesn't really
			 * seem worth any extra code.  If there are any bare CaseTestExprs
			 * elsewhere in the CASE, something's wrong already.
			 */
			*flags |= CCDN_CASETESTEXPR_OK;
			res = expression_tree_walker(node,
										 contain_context_dependent_node_walker,
										 flags);
			*flags = save_flags;
			return res;
		}
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *ac = (ArrayCoerceExpr *) node;
		int			save_flags;
		bool		res;

		/* Check the array expression */
		if (contain_context_dependent_node_walker((Node *) ac->arg, flags))
			return true;

		/* Check the elemexpr, which is allowed to contain CaseTestExpr */
		save_flags = *flags;
		*flags |= CCDN_CASETESTEXPR_OK;
		res = contain_context_dependent_node_walker((Node *) ac->elemexpr,
													flags);
		*flags = save_flags;
		return res;
	}
	return expression_tree_walker(node, contain_context_dependent_node_walker,
								  flags);
}

/*****************************************************************************
 *		  Check clauses for Vars passed to non-leakproof functions
 *****************************************************************************/

/*
 * contain_leaked_vars
 *		Recursively scan a clause to discover whether it contains any Var
 *		nodes (of the current query level) that are passed as arguments to
 *		leaky functions.
 *
 * Returns true if the clause contains any non-leakproof functions that are
 * passed Var nodes of the current query level, and which might therefore leak
 * data.  Such clauses must be applied after any lower-level security barrier
 * clauses.
 */
bool
contain_leaked_vars(Node *clause)
{
	return contain_leaked_vars_walker(clause, NULL);
}

static bool
contain_leaked_vars_checker(Oid func_id, void *context)
{
	return !get_func_leakproof(func_id);
}

static bool
contain_leaked_vars_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_ArrayExpr:
		case T_FieldSelect:
		case T_FieldStore:
		case T_NamedArgExpr:
		case T_BoolExpr:
		case T_RelabelType:
		case T_CollateExpr:
		case T_CaseExpr:
		case T_CaseTestExpr:
		case T_RowExpr:
		case T_SQLValueFunction:
		case T_NullTest:
		case T_BooleanTest:
		case T_NextValueExpr:
		case T_ReturningExpr:
		case T_List:

			/*
			 * We know these node types don't contain function calls; but
			 * something further down in the node tree might.
			 */
			break;

		case T_FuncExpr:
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
		case T_ScalarArrayOpExpr:
		case T_CoerceViaIO:
		case T_ArrayCoerceExpr:

			/*
			 * If node contains a leaky function call, and there's any Var
			 * underneath it, reject.
			 */
			if (check_functions_in_node(node, contain_leaked_vars_checker,
										context) &&
				contain_var_clause(node))
				return true;
			break;

		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;
				const SubscriptRoutines *sbsroutines;

				/* Consult the subscripting support method info */
				sbsroutines = getSubscriptingRoutines(sbsref->refcontainertype,
													  NULL);
				if (!sbsroutines ||
					!(sbsref->refassgnexpr != NULL ?
					  sbsroutines->store_leakproof :
					  sbsroutines->fetch_leakproof))
				{
					/* Node is leaky, so reject if it contains Vars */
					if (contain_var_clause(node))
						return true;
				}
			}
			break;

		case T_RowCompareExpr:
			{
				/*
				 * It's worth special-casing this because a leaky comparison
				 * function only compromises one pair of row elements, which
				 * might not contain Vars while others do.
				 */
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				ListCell   *opid;
				ListCell   *larg;
				ListCell   *rarg;

				forthree(opid, rcexpr->opnos,
						 larg, rcexpr->largs,
						 rarg, rcexpr->rargs)
				{
					Oid			funcid = get_opcode(lfirst_oid(opid));

					if (!get_func_leakproof(funcid) &&
						(contain_var_clause((Node *) lfirst(larg)) ||
						 contain_var_clause((Node *) lfirst(rarg))))
						return true;
				}
			}
			break;

		case T_MinMaxExpr:
			{
				/*
				 * MinMaxExpr is leakproof if the comparison function it calls
				 * is leakproof.
				 */
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;
				TypeCacheEntry *typentry;
				bool		leakproof;

				/* Look up the btree comparison function for the datatype */
				typentry = lookup_type_cache(minmaxexpr->minmaxtype,
											 TYPECACHE_CMP_PROC);
				if (OidIsValid(typentry->cmp_proc))
					leakproof = get_func_leakproof(typentry->cmp_proc);
				else
				{
					/*
					 * The executor will throw an error, but here we just
					 * treat the missing function as leaky.
					 */
					leakproof = false;
				}

				if (!leakproof &&
					contain_var_clause((Node *) minmaxexpr->args))
					return true;
			}
			break;

		case T_CurrentOfExpr:

			/*
			 * WHERE CURRENT OF doesn't contain leaky function calls.
			 * Moreover, it is essential that this is considered non-leaky,
			 * since the planner must always generate a TID scan when CURRENT
			 * OF is present -- cf. cost_tidscan.
			 */
			return false;

		default:

			/*
			 * If we don't recognize the node tag, assume it might be leaky.
			 * This prevents an unexpected security hole if someone adds a new
			 * node type that can call a function.
			 */
			return true;
	}
	return expression_tree_walker(node, contain_leaked_vars_walker,
								  context);
}

/*
 * find_nonnullable_rels
 *		Determine which base rels are forced nonnullable by given clause.
 *
 * Returns the set of all Relids that are referenced in the clause in such
 * a way that the clause cannot possibly return TRUE if any of these Relids
 * is an all-NULL row.  (It is OK to err on the side of conservatism; hence
 * the analysis here is simplistic.)
 *
 * The semantics here are subtly different from contain_nonstrict_functions:
 * that function is concerned with NULL results from arbitrary expressions,
 * but here we assume that the input is a Boolean expression, and wish to
 * see if NULL inputs will provably cause a FALSE-or-NULL result.  We expect
 * the expression to have been AND/OR flattened and converted to implicit-AND
 * format.
 *
 * Note: this function is largely duplicative of find_nonnullable_vars().
 * The reason not to simplify this function into a thin wrapper around
 * find_nonnullable_vars() is that the tested conditions really are different:
 * a clause like "t1.v1 IS NOT NULL OR t1.v2 IS NOT NULL" does not prove
 * that either v1 or v2 can't be NULL, but it does prove that the t1 row
 * as a whole can't be all-NULL.  Also, the behavior for PHVs is different.
 *
 * top_level is true while scanning top-level AND/OR structure; here, showing
 * the result is either FALSE or NULL is good enough.  top_level is false when
 * we have descended below a NOT or a strict function: now we must be able to
 * prove that the subexpression goes to NULL.
 *
 * We don't use expression_tree_walker here because we don't want to descend
 * through very many kinds of nodes; only the ones we can be sure are strict.
 */
Relids
find_nonnullable_rels(Node *clause)
{
	return find_nonnullable_rels_walker(clause, true);
}

static Relids
find_nonnullable_rels_walker(Node *node, bool top_level)
{
	Relids		result = NULL;
	ListCell   *l;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0)
			result = bms_make_singleton(var->varno);
	}
	else if (IsA(node, List))
	{
		/*
		 * At top level, we are examining an implicit-AND list: if any of the
		 * arms produces FALSE-or-NULL then the result is FALSE-or-NULL. If
		 * not at top level, we are examining the arguments of a strict
		 * function: if any of them produce NULL then the result of the
		 * function must be NULL.  So in both cases, the set of nonnullable
		 * rels is the union of those found in the arms, and we pass down the
		 * top_level flag unmodified.
		 */
		foreach(l, (List *) node)
		{
			result = bms_join(result,
							  find_nonnullable_rels_walker(lfirst(l),
														   top_level));
		}
	}
	else if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;

		if (func_strict(expr->funcid))
			result = find_nonnullable_rels_walker((Node *) expr->args, false);
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;

		set_opfuncid(expr);
		if (func_strict(expr->opfuncid))
			result = find_nonnullable_rels_walker((Node *) expr->args, false);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

		if (is_strict_saop(expr, true))
			result = find_nonnullable_rels_walker((Node *) expr->args, false);
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr   *expr = (BoolExpr *) node;

		switch (expr->boolop)
		{
			case AND_EXPR:
				/* At top level we can just recurse (to the List case) */
				if (top_level)
				{
					result = find_nonnullable_rels_walker((Node *) expr->args,
														  top_level);
					break;
				}

				/*
				 * Below top level, even if one arm produces NULL, the result
				 * could be FALSE (hence not NULL).  However, if *all* the
				 * arms produce NULL then the result is NULL, so we can take
				 * the intersection of the sets of nonnullable rels, just as
				 * for OR.  Fall through to share code.
				 */
				/* FALL THRU */
			case OR_EXPR:

				/*
				 * OR is strict if all of its arms are, so we can take the
				 * intersection of the sets of nonnullable rels for each arm.
				 * This works for both values of top_level.
				 */
				foreach(l, expr->args)
				{
					Relids		subresult;

					subresult = find_nonnullable_rels_walker(lfirst(l),
															 top_level);
					if (result == NULL) /* first subresult? */
						result = subresult;
					else
						result = bms_int_members(result, subresult);

					/*
					 * If the intersection is empty, we can stop looking. This
					 * also justifies the test for first-subresult above.
					 */
					if (bms_is_empty(result))
						break;
				}
				break;
			case NOT_EXPR:
				/* NOT will return null if its arg is null */
				result = find_nonnullable_rels_walker((Node *) expr->args,
													  false);
				break;
			default:
				elog(ERROR, "unrecognized boolop: %d", (int) expr->boolop);
				break;
		}
	}
	else if (IsA(node, RelabelType))
	{
		RelabelType *expr = (RelabelType *) node;

		result = find_nonnullable_rels_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, CoerceViaIO))
	{
		/* not clear this is useful, but it can't hurt */
		CoerceViaIO *expr = (CoerceViaIO *) node;

		result = find_nonnullable_rels_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		/* ArrayCoerceExpr is strict at the array level; ignore elemexpr */
		ArrayCoerceExpr *expr = (ArrayCoerceExpr *) node;

		result = find_nonnullable_rels_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		/* not clear this is useful, but it can't hurt */
		ConvertRowtypeExpr *expr = (ConvertRowtypeExpr *) node;

		result = find_nonnullable_rels_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, CollateExpr))
	{
		CollateExpr *expr = (CollateExpr *) node;

		result = find_nonnullable_rels_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, NullTest))
	{
		/* IS NOT NULL can be considered strict, but only at top level */
		NullTest   *expr = (NullTest *) node;

		if (top_level && expr->nulltesttype == IS_NOT_NULL && !expr->argisrow)
			result = find_nonnullable_rels_walker((Node *) expr->arg, false);
	}
	else if (IsA(node, BooleanTest))
	{
		/* Boolean tests that reject NULL are strict at top level */
		BooleanTest *expr = (BooleanTest *) node;

		if (top_level &&
			(expr->booltesttype == IS_TRUE ||
			 expr->booltesttype == IS_FALSE ||
			 expr->booltesttype == IS_NOT_UNKNOWN))
			result = find_nonnullable_rels_walker((Node *) expr->arg, false);
	}
	else if (IsA(node, SubPlan))
	{
		SubPlan    *splan = (SubPlan *) node;

		/*
		 * For some types of SubPlan, we can infer strictness from Vars in the
		 * testexpr (the LHS of the original SubLink).
		 *
		 * For ANY_SUBLINK, if the subquery produces zero rows, the result is
		 * always FALSE.  If the subquery produces more than one row, the
		 * per-row results of the testexpr are combined using OR semantics.
		 * Hence ANY_SUBLINK can be strict only at top level, but there it's
		 * as strict as the testexpr is.
		 *
		 * For ROWCOMPARE_SUBLINK, if the subquery produces zero rows, the
		 * result is always NULL.  Otherwise, the result is as strict as the
		 * testexpr is.  So we can check regardless of top_level.
		 *
		 * We can't prove anything for other sublink types (in particular,
		 * note that ALL_SUBLINK will return TRUE if the subquery is empty).
		 */
		if ((top_level && splan->subLinkType == ANY_SUBLINK) ||
			splan->subLinkType == ROWCOMPARE_SUBLINK)
			result = find_nonnullable_rels_walker(splan->testexpr, top_level);
	}
	else if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/*
		 * If the contained expression forces any rels non-nullable, so does
		 * the PHV.
		 */
		result = find_nonnullable_rels_walker((Node *) phv->phexpr, top_level);

		/*
		 * If the PHV's syntactic scope is exactly one rel, it will be forced
		 * to be evaluated at that rel, and so it will behave like a Var of
		 * that rel: if the rel's entire output goes to null, so will the PHV.
		 * (If the syntactic scope is a join, we know that the PHV will go to
		 * null if the whole join does; but that is AND semantics while we
		 * need OR semantics for find_nonnullable_rels' result, so we can't do
		 * anything with the knowledge.)
		 */
		if (phv->phlevelsup == 0 &&
			bms_membership(phv->phrels) == BMS_SINGLETON)
			result = bms_add_members(result, phv->phrels);
	}
	return result;
}

/*
 * find_nonnullable_vars
 *		Determine which Vars are forced nonnullable by given clause.
 *
 * Returns the set of all level-zero Vars that are referenced in the clause in
 * such a way that the clause cannot possibly return TRUE if any of these Vars
 * is NULL.  (It is OK to err on the side of conservatism; hence the analysis
 * here is simplistic.)
 *
 * The semantics here are subtly different from contain_nonstrict_functions:
 * that function is concerned with NULL results from arbitrary expressions,
 * but here we assume that the input is a Boolean expression, and wish to
 * see if NULL inputs will provably cause a FALSE-or-NULL result.  We expect
 * the expression to have been AND/OR flattened and converted to implicit-AND
 * format.
 *
 * Attnos of the identified Vars are returned in a multibitmapset (a List of
 * Bitmapsets).  List indexes correspond to relids (varnos), while the per-rel
 * Bitmapsets hold varattnos offset by FirstLowInvalidHeapAttributeNumber.
 *
 * top_level is true while scanning top-level AND/OR structure; here, showing
 * the result is either FALSE or NULL is good enough.  top_level is false when
 * we have descended below a NOT or a strict function: now we must be able to
 * prove that the subexpression goes to NULL.
 *
 * We don't use expression_tree_walker here because we don't want to descend
 * through very many kinds of nodes; only the ones we can be sure are strict.
 */
List *
find_nonnullable_vars(Node *clause)
{
	return find_nonnullable_vars_walker(clause, true);
}

static List *
find_nonnullable_vars_walker(Node *node, bool top_level)
{
	List	   *result = NIL;
	ListCell   *l;

	if (node == NULL)
		return NIL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0)
			result = mbms_add_member(result,
									 var->varno,
									 var->varattno - FirstLowInvalidHeapAttributeNumber);
	}
	else if (IsA(node, List))
	{
		/*
		 * At top level, we are examining an implicit-AND list: if any of the
		 * arms produces FALSE-or-NULL then the result is FALSE-or-NULL. If
		 * not at top level, we are examining the arguments of a strict
		 * function: if any of them produce NULL then the result of the
		 * function must be NULL.  So in both cases, the set of nonnullable
		 * vars is the union of those found in the arms, and we pass down the
		 * top_level flag unmodified.
		 */
		foreach(l, (List *) node)
		{
			result = mbms_add_members(result,
									  find_nonnullable_vars_walker(lfirst(l),
																   top_level));
		}
	}
	else if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;

		if (func_strict(expr->funcid))
			result = find_nonnullable_vars_walker((Node *) expr->args, false);
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;

		set_opfuncid(expr);
		if (func_strict(expr->opfuncid))
			result = find_nonnullable_vars_walker((Node *) expr->args, false);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

		if (is_strict_saop(expr, true))
			result = find_nonnullable_vars_walker((Node *) expr->args, false);
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr   *expr = (BoolExpr *) node;

		switch (expr->boolop)
		{
			case AND_EXPR:

				/*
				 * At top level we can just recurse (to the List case), since
				 * the result should be the union of what we can prove in each
				 * arm.
				 */
				if (top_level)
				{
					result = find_nonnullable_vars_walker((Node *) expr->args,
														  top_level);
					break;
				}

				/*
				 * Below top level, even if one arm produces NULL, the result
				 * could be FALSE (hence not NULL).  However, if *all* the
				 * arms produce NULL then the result is NULL, so we can take
				 * the intersection of the sets of nonnullable vars, just as
				 * for OR.  Fall through to share code.
				 */
				/* FALL THRU */
			case OR_EXPR:

				/*
				 * OR is strict if all of its arms are, so we can take the
				 * intersection of the sets of nonnullable vars for each arm.
				 * This works for both values of top_level.
				 */
				foreach(l, expr->args)
				{
					List	   *subresult;

					subresult = find_nonnullable_vars_walker(lfirst(l),
															 top_level);
					if (result == NIL)	/* first subresult? */
						result = subresult;
					else
						result = mbms_int_members(result, subresult);

					/*
					 * If the intersection is empty, we can stop looking. This
					 * also justifies the test for first-subresult above.
					 */
					if (result == NIL)
						break;
				}
				break;
			case NOT_EXPR:
				/* NOT will return null if its arg is null */
				result = find_nonnullable_vars_walker((Node *) expr->args,
													  false);
				break;
			default:
				elog(ERROR, "unrecognized boolop: %d", (int) expr->boolop);
				break;
		}
	}
	else if (IsA(node, RelabelType))
	{
		RelabelType *expr = (RelabelType *) node;

		result = find_nonnullable_vars_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, CoerceViaIO))
	{
		/* not clear this is useful, but it can't hurt */
		CoerceViaIO *expr = (CoerceViaIO *) node;

		result = find_nonnullable_vars_walker((Node *) expr->arg, false);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		/* ArrayCoerceExpr is strict at the array level; ignore elemexpr */
		ArrayCoerceExpr *expr = (ArrayCoerceExpr *) node;

		result = find_nonnullable_vars_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		/* not clear this is useful, but it can't hurt */
		ConvertRowtypeExpr *expr = (ConvertRowtypeExpr *) node;

		result = find_nonnullable_vars_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, CollateExpr))
	{
		CollateExpr *expr = (CollateExpr *) node;

		result = find_nonnullable_vars_walker((Node *) expr->arg, top_level);
	}
	else if (IsA(node, NullTest))
	{
		/* IS NOT NULL can be considered strict, but only at top level */
		NullTest   *expr = (NullTest *) node;

		if (top_level && expr->nulltesttype == IS_NOT_NULL && !expr->argisrow)
			result = find_nonnullable_vars_walker((Node *) expr->arg, false);
	}
	else if (IsA(node, BooleanTest))
	{
		/* Boolean tests that reject NULL are strict at top level */
		BooleanTest *expr = (BooleanTest *) node;

		if (top_level &&
			(expr->booltesttype == IS_TRUE ||
			 expr->booltesttype == IS_FALSE ||
			 expr->booltesttype == IS_NOT_UNKNOWN))
			result = find_nonnullable_vars_walker((Node *) expr->arg, false);
	}
	else if (IsA(node, SubPlan))
	{
		SubPlan    *splan = (SubPlan *) node;

		/* See analysis in find_nonnullable_rels_walker */
		if ((top_level && splan->subLinkType == ANY_SUBLINK) ||
			splan->subLinkType == ROWCOMPARE_SUBLINK)
			result = find_nonnullable_vars_walker(splan->testexpr, top_level);
	}
	else if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		result = find_nonnullable_vars_walker((Node *) phv->phexpr, top_level);
	}
	return result;
}

/*
 * find_forced_null_vars
 *		Determine which Vars must be NULL for the given clause to return TRUE.
 *
 * This is the complement of find_nonnullable_vars: find the level-zero Vars
 * that must be NULL for the clause to return TRUE.  (It is OK to err on the
 * side of conservatism; hence the analysis here is simplistic.  In fact,
 * we only detect simple "var IS NULL" tests at the top level.)
 *
 * As with find_nonnullable_vars, we return the varattnos of the identified
 * Vars in a multibitmapset.
 */
List *
find_forced_null_vars(Node *node)
{
	List	   *result = NIL;
	Var		   *var;
	ListCell   *l;

	if (node == NULL)
		return NIL;
	/* Check single-clause cases using subroutine */
	var = find_forced_null_var(node);
	if (var)
	{
		result = mbms_add_member(result,
								 var->varno,
								 var->varattno - FirstLowInvalidHeapAttributeNumber);
	}
	/* Otherwise, handle AND-conditions */
	else if (IsA(node, List))
	{
		/*
		 * At top level, we are examining an implicit-AND list: if any of the
		 * arms produces FALSE-or-NULL then the result is FALSE-or-NULL.
		 */
		foreach(l, (List *) node)
		{
			result = mbms_add_members(result,
									  find_forced_null_vars((Node *) lfirst(l)));
		}
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr   *expr = (BoolExpr *) node;

		/*
		 * We don't bother considering the OR case, because it's fairly
		 * unlikely anyone would write "v1 IS NULL OR v1 IS NULL". Likewise,
		 * the NOT case isn't worth expending code on.
		 */
		if (expr->boolop == AND_EXPR)
		{
			/* At top level we can just recurse (to the List case) */
			result = find_forced_null_vars((Node *) expr->args);
		}
	}
	return result;
}

/*
 * find_forced_null_var
 *		Return the Var forced null by the given clause, or NULL if it's
 *		not an IS NULL-type clause.  For success, the clause must enforce
 *		*only* nullness of the particular Var, not any other conditions.
 *
 * This is just the single-clause case of find_forced_null_vars(), without
 * any allowance for AND conditions.  It's used by initsplan.c on individual
 * qual clauses.  The reason for not just applying find_forced_null_vars()
 * is that if an AND of an IS NULL clause with something else were to somehow
 * survive AND/OR flattening, initsplan.c might get fooled into discarding
 * the whole clause when only the IS NULL part of it had been proved redundant.
 */
Var *
find_forced_null_var(Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, NullTest))
	{
		/* check for var IS NULL */
		NullTest   *expr = (NullTest *) node;

		if (expr->nulltesttype == IS_NULL && !expr->argisrow)
		{
			Var		   *var = (Var *) expr->arg;

			if (var && IsA(var, Var) &&
				var->varlevelsup == 0)
				return var;
		}
	}
	else if (IsA(node, BooleanTest))
	{
		/* var IS UNKNOWN is equivalent to var IS NULL */
		BooleanTest *expr = (BooleanTest *) node;

		if (expr->booltesttype == IS_UNKNOWN)
		{
			Var		   *var = (Var *) expr->arg;

			if (var && IsA(var, Var) &&
				var->varlevelsup == 0)
				return var;
		}
	}
	return NULL;
}

/*
 * Can we treat a ScalarArrayOpExpr as strict?
 *
 * If "falseOK" is true, then a "false" result can be considered strict,
 * else we need to guarantee an actual NULL result for NULL input.
 *
 * "foo op ALL array" is strict if the op is strict *and* we can prove
 * that the array input isn't an empty array.  We can check that
 * for the cases of an array constant and an ARRAY[] construct.
 *
 * "foo op ANY array" is strict in the falseOK sense if the op is strict.
 * If not falseOK, the test is the same as for "foo op ALL array".
 */
static bool
is_strict_saop(ScalarArrayOpExpr *expr, bool falseOK)
{
	Node	   *rightop;

	/* The contained operator must be strict. */
	set_sa_opfuncid(expr);
	if (!func_strict(expr->opfuncid))
		return false;
	/* If ANY and falseOK, that's all we need to check. */
	if (expr->useOr && falseOK)
		return true;
	/* Else, we have to see if the array is provably non-empty. */
	Assert(list_length(expr->args) == 2);
	rightop = (Node *) lsecond(expr->args);
	if (rightop && IsA(rightop, Const))
	{
		Datum		arraydatum = ((Const *) rightop)->constvalue;
		bool		arrayisnull = ((Const *) rightop)->constisnull;
		ArrayType  *arrayval;
		int			nitems;

		if (arrayisnull)
			return false;
		arrayval = DatumGetArrayTypeP(arraydatum);
		nitems = ArrayGetNItems(ARR_NDIM(arrayval), ARR_DIMS(arrayval));
		if (nitems > 0)
			return true;
	}
	else if (rightop && IsA(rightop, ArrayExpr))
	{
		ArrayExpr  *arrayexpr = (ArrayExpr *) rightop;

		if (arrayexpr->elements != NIL && !arrayexpr->multidims)
			return true;
	}
	return false;
}


/*****************************************************************************
 *		Check for "pseudo-constant" clauses
 *****************************************************************************/

/*
 * is_pseudo_constant_clause
 *	  Detect whether an expression is "pseudo constant", ie, it contains no
 *	  variables of the current query level and no uses of volatile functions.
 *	  Such an expr is not necessarily a true constant: it can still contain
 *	  Params and outer-level Vars, not to mention functions whose results
 *	  may vary from one statement to the next.  However, the expr's value
 *	  will be constant over any one scan of the current query, so it can be
 *	  used as, eg, an indexscan key.  (Actually, the condition for indexscan
 *	  keys is weaker than this; see is_pseudo_constant_for_index().)
 *
 * CAUTION: this function omits to test for one very important class of
 * not-constant expressions, namely aggregates (Aggrefs).  In current usage
 * this is only applied to WHERE clauses and so a check for Aggrefs would be
 * a waste of cycles; but be sure to also check contain_agg_clause() if you
 * want to know about pseudo-constness in other contexts.  The same goes
 * for window functions (WindowFuncs).
 */
bool
is_pseudo_constant_clause(Node *clause)
{
	/*
	 * We could implement this check in one recursive scan.  But since the
	 * check for volatile functions is both moderately expensive and unlikely
	 * to fail, it seems better to look for Vars first and only check for
	 * volatile functions if we find no Vars.
	 */
	if (!contain_var_clause(clause) &&
		!contain_volatile_functions(clause))
		return true;
	return false;
}

/*
 * is_pseudo_constant_clause_relids
 *	  Same as above, except caller already has available the var membership
 *	  of the expression; this lets us avoid the contain_var_clause() scan.
 */
bool
is_pseudo_constant_clause_relids(Node *clause, Relids relids)
{
	if (bms_is_empty(relids) &&
		!contain_volatile_functions(clause))
		return true;
	return false;
}


/*****************************************************************************
 *																			 *
 *		General clause-manipulating routines								 *
 *																			 *
 *****************************************************************************/

/*
 * NumRelids
 *		(formerly clause_relids)
 *
 * Returns the number of different base relations referenced in 'clause'.
 */
int
NumRelids(PlannerInfo *root, Node *clause)
{
	int			result;
	Relids		varnos = pull_varnos(root, clause);

	varnos = bms_del_members(varnos, root->outer_join_rels);
	result = bms_num_members(varnos);
	bms_free(varnos);
	return result;
}

/*
 * CommuteOpExpr: commute a binary operator clause
 *
 * XXX the clause is destructively modified!
 */
void
CommuteOpExpr(OpExpr *clause)
{
	Oid			opoid;
	Node	   *temp;

	/* Sanity checks: caller is at fault if these fail */
	if (!is_opclause(clause) ||
		list_length(clause->args) != 2)
		elog(ERROR, "cannot commute non-binary-operator clause");

	opoid = get_commutator(clause->opno);

	if (!OidIsValid(opoid))
		elog(ERROR, "could not find commutator for operator %u",
			 clause->opno);

	/*
	 * modify the clause in-place!
	 */
	clause->opno = opoid;
	clause->opfuncid = InvalidOid;
	/* opresulttype, opretset, opcollid, inputcollid need not change */

	temp = linitial(clause->args);
	linitial(clause->args) = lsecond(clause->args);
	lsecond(clause->args) = temp;
}

/*
 * Helper for eval_const_expressions: check that datatype of an attribute
 * is still what it was when the expression was parsed.  This is needed to
 * guard against improper simplification after ALTER COLUMN TYPE.  (XXX we
 * may well need to make similar checks elsewhere?)
 *
 * rowtypeid may come from a whole-row Var, and therefore it can be a domain
 * over composite, but for this purpose we only care about checking the type
 * of a contained field.
 */
static bool
rowtype_field_matches(Oid rowtypeid, int fieldnum,
					  Oid expectedtype, int32 expectedtypmod,
					  Oid expectedcollation)
{
	TupleDesc	tupdesc;
	Form_pg_attribute attr;

	/* No issue for RECORD, since there is no way to ALTER such a type */
	if (rowtypeid == RECORDOID)
		return true;
	tupdesc = lookup_rowtype_tupdesc_domain(rowtypeid, -1, false);
	if (fieldnum <= 0 || fieldnum > tupdesc->natts)
	{
		ReleaseTupleDesc(tupdesc);
		return false;
	}
	attr = TupleDescAttr(tupdesc, fieldnum - 1);
	if (attr->attisdropped ||
		attr->atttypid != expectedtype ||
		attr->atttypmod != expectedtypmod ||
		attr->attcollation != expectedcollation)
	{
		ReleaseTupleDesc(tupdesc);
		return false;
	}
	ReleaseTupleDesc(tupdesc);
	return true;
}


/*--------------------
 * eval_const_expressions
 *
 * Reduce any recognizably constant subexpressions of the given
 * expression tree, for example "2 + 2" => "4".  More interestingly,
 * we can reduce certain boolean expressions even when they contain
 * non-constant subexpressions: "x OR true" => "true" no matter what
 * the subexpression x is.  (XXX We assume that no such subexpression
 * will have important side-effects, which is not necessarily a good
 * assumption in the presence of user-defined functions; do we need a
 * pg_proc flag that prevents discarding the execution of a function?)
 *
 * We do understand that certain functions may deliver non-constant
 * results even with constant inputs, "nextval()" being the classic
 * example.  Functions that are not marked "immutable" in pg_proc
 * will not be pre-evaluated here, although we will reduce their
 * arguments as far as possible.
 *
 * Whenever a function is eliminated from the expression by means of
 * constant-expression evaluation or inlining, we add the function to
 * root->glob->invalItems.  This ensures the plan is known to depend on
 * such functions, even though they aren't referenced anymore.
 *
 * We assume that the tree has already been type-checked and contains
 * only operators and functions that are reasonable to try to execute.
 *
 * NOTE: "root" can be passed as NULL if the caller never wants to do any
 * Param substitutions nor receive info about inlined functions.
 *
 * NOTE: the planner assumes that this will always flatten nested AND and
 * OR clauses into N-argument form.  See comments in prepqual.c.
 *
 * NOTE: another critical effect is that any function calls that require
 * default arguments will be expanded, and named-argument calls will be
 * converted to positional notation.  The executor won't handle either.
 *--------------------
 */
Node *
eval_const_expressions(PlannerInfo *root, Node *node)
{
	eval_const_expressions_context context;

	if (root)
		context.boundParams = root->glob->boundParams;	/* bound Params */
	else
		context.boundParams = NULL;
	context.root = root;		/* for inlined-function dependencies */
	context.active_fns = NIL;	/* nothing being recursively simplified */
	context.case_val = NULL;	/* no CASE being examined */
	context.estimate = false;	/* safe transformations only */
	return eval_const_expressions_mutator(node, &context);
}

#define MIN_ARRAY_SIZE_FOR_HASHED_SAOP 9
/*--------------------
 * convert_saop_to_hashed_saop
 *
 * Recursively search 'node' for ScalarArrayOpExprs and fill in the hash
 * function for any ScalarArrayOpExpr that looks like it would be useful to
 * evaluate using a hash table rather than a linear search.
 *
 * We'll use a hash table if all of the following conditions are met:
 * 1. The 2nd argument of the array contain only Consts.
 * 2. useOr is true or there is a valid negator operator for the
 *	  ScalarArrayOpExpr's opno.
 * 3. There's valid hash function for both left and righthand operands and
 *	  these hash functions are the same.
 * 4. If the array contains enough elements for us to consider it to be
 *	  worthwhile using a hash table rather than a linear search.
 */
void
convert_saop_to_hashed_saop(Node *node)
{
	(void) convert_saop_to_hashed_saop_walker(node, NULL);
}

static bool
convert_saop_to_hashed_saop_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;
		Expr	   *arrayarg = (Expr *) lsecond(saop->args);
		Oid			lefthashfunc;
		Oid			righthashfunc;

		if (arrayarg && IsA(arrayarg, Const) &&
			!((Const *) arrayarg)->constisnull)
		{
			if (saop->useOr)
			{
				if (get_op_hash_functions(saop->opno, &lefthashfunc, &righthashfunc) &&
					lefthashfunc == righthashfunc)
				{
					Datum		arrdatum = ((Const *) arrayarg)->constvalue;
					ArrayType  *arr = (ArrayType *) DatumGetPointer(arrdatum);
					int			nitems;

					/*
					 * Only fill in the hash functions if the array looks
					 * large enough for it to be worth hashing instead of
					 * doing a linear search.
					 */
					nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

					if (nitems >= MIN_ARRAY_SIZE_FOR_HASHED_SAOP)
					{
						/* Looks good. Fill in the hash functions */
						saop->hashfuncid = lefthashfunc;
					}
					return false;
				}
			}
			else				/* !saop->useOr */
			{
				Oid			negator = get_negator(saop->opno);

				/*
				 * Check if this is a NOT IN using an operator whose negator
				 * is hashable.  If so we can still build a hash table and
				 * just ensure the lookup items are not in the hash table.
				 */
				if (OidIsValid(negator) &&
					get_op_hash_functions(negator, &lefthashfunc, &righthashfunc) &&
					lefthashfunc == righthashfunc)
				{
					Datum		arrdatum = ((Const *) arrayarg)->constvalue;
					ArrayType  *arr = (ArrayType *) DatumGetPointer(arrdatum);
					int			nitems;

					/*
					 * Only fill in the hash functions if the array looks
					 * large enough for it to be worth hashing instead of
					 * doing a linear search.
					 */
					nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

					if (nitems >= MIN_ARRAY_SIZE_FOR_HASHED_SAOP)
					{
						/* Looks good. Fill in the hash functions */
						saop->hashfuncid = lefthashfunc;

						/*
						 * Also set the negfuncid.  The executor will need
						 * that to perform hashtable lookups.
						 */
						saop->negfuncid = get_opcode(negator);
					}
					return false;
				}
			}
		}
	}

	return expression_tree_walker(node, convert_saop_to_hashed_saop_walker, NULL);
}


/*--------------------
 * estimate_expression_value
 *
 * This function attempts to estimate the value of an expression for
 * planning purposes.  It is in essence a more aggressive version of
 * eval_const_expressions(): we will perform constant reductions that are
 * not necessarily 100% safe, but are reasonable for estimation purposes.
 *
 * Currently the extra steps that are taken in this mode are:
 * 1. Substitute values for Params, where a bound Param value has been made
 *	  available by the caller of planner(), even if the Param isn't marked
 *	  constant.  This effectively means that we plan using the first supplied
 *	  value of the Param.
 * 2. Fold stable, as well as immutable, functions to constants.
 * 3. Reduce PlaceHolderVar nodes to their contained expressions.
 *--------------------
 */
Node *
estimate_expression_value(PlannerInfo *root, Node *node)
{
	eval_const_expressions_context context;

	context.boundParams = root->glob->boundParams;	/* bound Params */
	/* we do not need to mark the plan as depending on inlined functions */
	context.root = NULL;
	context.active_fns = NIL;	/* nothing being recursively simplified */
	context.case_val = NULL;	/* no CASE being examined */
	context.estimate = true;	/* unsafe transformations OK */
	return eval_const_expressions_mutator(node, &context);
}

/*
 * The generic case in eval_const_expressions_mutator is to recurse using
 * expression_tree_mutator, which will copy the given node unchanged but
 * const-simplify its arguments (if any) as far as possible.  If the node
 * itself does immutable processing, and each of its arguments were reduced
 * to a Const, we can then reduce it to a Const using evaluate_expr.  (Some
 * node types need more complicated logic; for example, a CASE expression
 * might be reducible to a constant even if not all its subtrees are.)
 */
#define ece_generic_processing(node) \
	expression_tree_mutator((Node *) (node), eval_const_expressions_mutator, \
							context)

/*
 * Check whether all arguments of the given node were reduced to Consts.
 * By going directly to expression_tree_walker, contain_non_const_walker
 * is not applied to the node itself, only to its children.
 */
#define ece_all_arguments_const(node) \
	(!expression_tree_walker((Node *) (node), contain_non_const_walker, NULL))

/* Generic macro for applying evaluate_expr */
#define ece_evaluate_expr(node) \
	((Node *) evaluate_expr((Expr *) (node), \
							exprType((Node *) (node)), \
							exprTypmod((Node *) (node)), \
							exprCollation((Node *) (node))))

/*
 * Recursive guts of eval_const_expressions/estimate_expression_value
 */
static Node *
eval_const_expressions_mutator(Node *node,
							   eval_const_expressions_context *context)
{

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	if (node == NULL)
		return NULL;
	switch (nodeTag(node))
	{
		case T_Param:
			{
				Param	   *param = (Param *) node;
				ParamListInfo paramLI = context->boundParams;

				/* Look to see if we've been given a value for this Param */
				if (param->paramkind == PARAM_EXTERN &&
					paramLI != NULL &&
					param->paramid > 0 &&
					param->paramid <= paramLI->numParams)
				{
					ParamExternData *prm;
					ParamExternData prmdata;

					/*
					 * Give hook a chance in case parameter is dynamic.  Tell
					 * it that this fetch is speculative, so it should avoid
					 * erroring out if parameter is unavailable.
					 */
					if (paramLI->paramFetch != NULL)
						prm = paramLI->paramFetch(paramLI, param->paramid,
												  true, &prmdata);
					else
						prm = &paramLI->params[param->paramid - 1];

					/*
					 * We don't just check OidIsValid, but insist that the
					 * fetched type match the Param, just in case the hook did
					 * something unexpected.  No need to throw an error here
					 * though; leave that for runtime.
					 */
					if (OidIsValid(prm->ptype) &&
						prm->ptype == param->paramtype)
					{
						/* OK to substitute parameter value? */
						if (context->estimate ||
							(prm->pflags & PARAM_FLAG_CONST))
						{
							/*
							 * Return a Const representing the param value.
							 * Must copy pass-by-ref datatypes, since the
							 * Param might be in a memory context
							 * shorter-lived than our output plan should be.
							 */
							int16		typLen;
							bool		typByVal;
							Datum		pval;
							Const	   *con;

							get_typlenbyval(param->paramtype,
											&typLen, &typByVal);
							if (prm->isnull || typByVal)
								pval = prm->value;
							else
								pval = datumCopy(prm->value, typByVal, typLen);
							con = makeConst(param->paramtype,
											param->paramtypmod,
											param->paramcollid,
											(int) typLen,
											pval,
											prm->isnull,
											typByVal);
							con->location = param->location;
							return (Node *) con;
						}
					}
				}

				/*
				 * Not replaceable, so just copy the Param (no need to
				 * recurse)
				 */
				return (Node *) copyObject(param);
			}
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;
				Oid			funcid = expr->winfnoid;
				List	   *args;
				Expr	   *aggfilter;
				HeapTuple	func_tuple;
				WindowFunc *newexpr;

				/*
				 * We can't really simplify a WindowFunc node, but we mustn't
				 * just fall through to the default processing, because we
				 * have to apply expand_function_arguments to its argument
				 * list.  That takes care of inserting default arguments and
				 * expanding named-argument notation.
				 */
				func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
				if (!HeapTupleIsValid(func_tuple))
					elog(ERROR, "cache lookup failed for function %u", funcid);

				args = expand_function_arguments(expr->args,
												 false, expr->wintype,
												 func_tuple);

				ReleaseSysCache(func_tuple);

				/* Now, recursively simplify the args (which are a List) */
				args = (List *)
					expression_tree_mutator((Node *) args,
											eval_const_expressions_mutator,
											context);
				/* ... and the filter expression, which isn't */
				aggfilter = (Expr *)
					eval_const_expressions_mutator((Node *) expr->aggfilter,
												   context);

				/* And build the replacement WindowFunc node */
				newexpr = makeNode(WindowFunc);
				newexpr->winfnoid = expr->winfnoid;
				newexpr->wintype = expr->wintype;
				newexpr->wincollid = expr->wincollid;
				newexpr->inputcollid = expr->inputcollid;
				newexpr->args = args;
				newexpr->aggfilter = aggfilter;
				newexpr->runCondition = expr->runCondition;
				newexpr->winref = expr->winref;
				newexpr->winstar = expr->winstar;
				newexpr->winagg = expr->winagg;
				newexpr->location = expr->location;

				return (Node *) newexpr;
			}
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;
				List	   *args = expr->args;
				Expr	   *simple;
				FuncExpr   *newexpr;

				/*
				 * Code for op/func reduction is pretty bulky, so split it out
				 * as a separate function.  Note: exprTypmod normally returns
				 * -1 for a FuncExpr, but not when the node is recognizably a
				 * length coercion; we want to preserve the typmod in the
				 * eventual Const if so.
				 */
				simple = simplify_function(expr->funcid,
										   expr->funcresulttype,
										   exprTypmod(node),
										   expr->funccollid,
										   expr->inputcollid,
										   &args,
										   expr->funcvariadic,
										   true,
										   true,
										   context);
				if (simple)		/* successfully simplified it */
					return (Node *) simple;

				/*
				 * The expression cannot be simplified any further, so build
				 * and return a replacement FuncExpr node using the
				 * possibly-simplified arguments.  Note that we have also
				 * converted the argument list to positional notation.
				 */
				newexpr = makeNode(FuncExpr);
				newexpr->funcid = expr->funcid;
				newexpr->funcresulttype = expr->funcresulttype;
				newexpr->funcretset = expr->funcretset;
				newexpr->funcvariadic = expr->funcvariadic;
				newexpr->funcformat = expr->funcformat;
				newexpr->funccollid = expr->funccollid;
				newexpr->inputcollid = expr->inputcollid;
				newexpr->args = args;
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
		case T_OpExpr:
			{
				OpExpr	   *expr = (OpExpr *) node;
				List	   *args = expr->args;
				Expr	   *simple;
				OpExpr	   *newexpr;

				/*
				 * Need to get OID of underlying function.  Okay to scribble
				 * on input to this extent.
				 */
				set_opfuncid(expr);

				/*
				 * Code for op/func reduction is pretty bulky, so split it out
				 * as a separate function.
				 */
				simple = simplify_function(expr->opfuncid,
										   expr->opresulttype, -1,
										   expr->opcollid,
										   expr->inputcollid,
										   &args,
										   false,
										   true,
										   true,
										   context);
				if (simple)		/* successfully simplified it */
					return (Node *) simple;

				/*
				 * If the operator is boolean equality or inequality, we know
				 * how to simplify cases involving one constant and one
				 * non-constant argument.
				 */
				if (expr->opno == BooleanEqualOperator ||
					expr->opno == BooleanNotEqualOperator)
				{
					simple = (Expr *) simplify_boolean_equality(expr->opno,
																args);
					if (simple) /* successfully simplified it */
						return (Node *) simple;
				}

				/*
				 * The expression cannot be simplified any further, so build
				 * and return a replacement OpExpr node using the
				 * possibly-simplified arguments.
				 */
				newexpr = makeNode(OpExpr);
				newexpr->opno = expr->opno;
				newexpr->opfuncid = expr->opfuncid;
				newexpr->opresulttype = expr->opresulttype;
				newexpr->opretset = expr->opretset;
				newexpr->opcollid = expr->opcollid;
				newexpr->inputcollid = expr->inputcollid;
				newexpr->args = args;
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
		case T_DistinctExpr:
			{
				DistinctExpr *expr = (DistinctExpr *) node;
				List	   *args;
				ListCell   *arg;
				bool		has_null_input = false;
				bool		all_null_input = true;
				bool		has_nonconst_input = false;
				Expr	   *simple;
				DistinctExpr *newexpr;

				/*
				 * Reduce constants in the DistinctExpr's arguments.  We know
				 * args is either NIL or a List node, so we can call
				 * expression_tree_mutator directly rather than recursing to
				 * self.
				 */
				args = (List *) expression_tree_mutator((Node *) expr->args,
														eval_const_expressions_mutator,
														context);

				/*
				 * We must do our own check for NULLs because DistinctExpr has
				 * different results for NULL input than the underlying
				 * operator does.
				 */
				foreach(arg, args)
				{
					if (IsA(lfirst(arg), Const))
					{
						has_null_input |= ((Const *) lfirst(arg))->constisnull;
						all_null_input &= ((Const *) lfirst(arg))->constisnull;
					}
					else
						has_nonconst_input = true;
				}

				/* all constants? then can optimize this out */
				if (!has_nonconst_input)
				{
					/* all nulls? then not distinct */
					if (all_null_input)
						return makeBoolConst(false, false);

					/* one null? then distinct */
					if (has_null_input)
						return makeBoolConst(true, false);

					/* otherwise try to evaluate the '=' operator */
					/* (NOT okay to try to inline it, though!) */

					/*
					 * Need to get OID of underlying function.  Okay to
					 * scribble on input to this extent.
					 */
					set_opfuncid((OpExpr *) expr);	/* rely on struct
													 * equivalence */

					/*
					 * Code for op/func reduction is pretty bulky, so split it
					 * out as a separate function.
					 */
					simple = simplify_function(expr->opfuncid,
											   expr->opresulttype, -1,
											   expr->opcollid,
											   expr->inputcollid,
											   &args,
											   false,
											   false,
											   false,
											   context);
					if (simple) /* successfully simplified it */
					{
						/*
						 * Since the underlying operator is "=", must negate
						 * its result
						 */
						Const	   *csimple = castNode(Const, simple);

						csimple->constvalue =
							BoolGetDatum(!DatumGetBool(csimple->constvalue));
						return (Node *) csimple;
					}
				}

				/*
				 * The expression cannot be simplified any further, so build
				 * and return a replacement DistinctExpr node using the
				 * possibly-simplified arguments.
				 */
				newexpr = makeNode(DistinctExpr);
				newexpr->opno = expr->opno;
				newexpr->opfuncid = expr->opfuncid;
				newexpr->opresulttype = expr->opresulttype;
				newexpr->opretset = expr->opretset;
				newexpr->opcollid = expr->opcollid;
				newexpr->inputcollid = expr->inputcollid;
				newexpr->args = args;
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
		case T_NullIfExpr:
			{
				NullIfExpr *expr;
				ListCell   *arg;
				bool		has_nonconst_input = false;

				/* Copy the node and const-simplify its arguments */
				expr = (NullIfExpr *) ece_generic_processing(node);

				/* If either argument is NULL they can't be equal */
				foreach(arg, expr->args)
				{
					if (!IsA(lfirst(arg), Const))
						has_nonconst_input = true;
					else if (((Const *) lfirst(arg))->constisnull)
						return (Node *) linitial(expr->args);
				}

				/*
				 * Need to get OID of underlying function before checking if
				 * the function is OK to evaluate.
				 */
				set_opfuncid((OpExpr *) expr);

				if (!has_nonconst_input &&
					ece_function_is_safe(expr->opfuncid, context))
					return ece_evaluate_expr(expr);

				return (Node *) expr;
			}
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *saop;

				/* Copy the node and const-simplify its arguments */
				saop = (ScalarArrayOpExpr *) ece_generic_processing(node);

				/* Make sure we know underlying function */
				set_sa_opfuncid(saop);

				/*
				 * If all arguments are Consts, and it's a safe function, we
				 * can fold to a constant
				 */
				if (ece_all_arguments_const(saop) &&
					ece_function_is_safe(saop->opfuncid, context))
					return ece_evaluate_expr(saop);
				return (Node *) saop;
			}
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				switch (expr->boolop)
				{
					case OR_EXPR:
						{
							List	   *newargs;
							bool		haveNull = false;
							bool		forceTrue = false;

							newargs = simplify_or_arguments(expr->args,
															context,
															&haveNull,
															&forceTrue);
							if (forceTrue)
								return makeBoolConst(true, false);
							if (haveNull)
								newargs = lappend(newargs,
												  makeBoolConst(false, true));
							/* If all the inputs are FALSE, result is FALSE */
							if (newargs == NIL)
								return makeBoolConst(false, false);

							/*
							 * If only one nonconst-or-NULL input, it's the
							 * result
							 */
							if (list_length(newargs) == 1)
								return (Node *) linitial(newargs);
							/* Else we still need an OR node */
							return (Node *) make_orclause(newargs);
						}
					case AND_EXPR:
						{
							List	   *newargs;
							bool		haveNull = false;
							bool		forceFalse = false;

							newargs = simplify_and_arguments(expr->args,
															 context,
															 &haveNull,
															 &forceFalse);
							if (forceFalse)
								return makeBoolConst(false, false);
							if (haveNull)
								newargs = lappend(newargs,
												  makeBoolConst(false, true));
							/* If all the inputs are TRUE, result is TRUE */
							if (newargs == NIL)
								return makeBoolConst(true, false);

							/*
							 * If only one nonconst-or-NULL input, it's the
							 * result
							 */
							if (list_length(newargs) == 1)
								return (Node *) linitial(newargs);
							/* Else we still need an AND node */
							return (Node *) make_andclause(newargs);
						}
					case NOT_EXPR:
						{
							Node	   *arg;

							Assert(list_length(expr->args) == 1);
							arg = eval_const_expressions_mutator(linitial(expr->args),
																 context);

							/*
							 * Use negate_clause() to see if we can simplify
							 * away the NOT.
							 */
							return negate_clause(arg);
						}
					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) expr->boolop);
						break;
				}
				break;
			}

		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;
				Node	   *raw_expr = (Node *) jve->raw_expr;
				Node	   *formatted_expr = (Node *) jve->formatted_expr;

				/*
				 * If we can fold formatted_expr to a constant, we can elide
				 * the JsonValueExpr altogether.  Otherwise we must process
				 * raw_expr too.  But JsonFormat is a flat node and requires
				 * no simplification, only copying.
				 */
				formatted_expr = eval_const_expressions_mutator(formatted_expr,
																context);
				if (formatted_expr && IsA(formatted_expr, Const))
					return formatted_expr;

				raw_expr = eval_const_expressions_mutator(raw_expr, context);

				return (Node *) makeJsonValueExpr((Expr *) raw_expr,
												  (Expr *) formatted_expr,
												  copyObject(jve->format));
			}

		case T_SubPlan:
		case T_AlternativeSubPlan:

			/*
			 * Return a SubPlan unchanged --- too late to do anything with it.
			 *
			 * XXX should we ereport() here instead?  Probably this routine
			 * should never be invoked after SubPlan creation.
			 */
			return node;
		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				Node	   *arg;

				/* Simplify the input ... */
				arg = eval_const_expressions_mutator((Node *) relabel->arg,
													 context);
				/* ... and attach a new RelabelType node, if needed */
				return applyRelabelType(arg,
										relabel->resulttype,
										relabel->resulttypmod,
										relabel->resultcollid,
										relabel->relabelformat,
										relabel->location,
										true);
			}
		case T_CoerceViaIO:
			{
				CoerceViaIO *expr = (CoerceViaIO *) node;
				List	   *args;
				Oid			outfunc;
				bool		outtypisvarlena;
				Oid			infunc;
				Oid			intypioparam;
				Expr	   *simple;
				CoerceViaIO *newexpr;

				/* Make a List so we can use simplify_function */
				args = list_make1(expr->arg);

				/*
				 * CoerceViaIO represents calling the source type's output
				 * function then the result type's input function.  So, try to
				 * simplify it as though it were a stack of two such function
				 * calls.  First we need to know what the functions are.
				 *
				 * Note that the coercion functions are assumed not to care
				 * about input collation, so we just pass InvalidOid for that.
				 */
				getTypeOutputInfo(exprType((Node *) expr->arg),
								  &outfunc, &outtypisvarlena);
				getTypeInputInfo(expr->resulttype,
								 &infunc, &intypioparam);

				simple = simplify_function(outfunc,
										   CSTRINGOID, -1,
										   InvalidOid,
										   InvalidOid,
										   &args,
										   false,
										   true,
										   true,
										   context);
				if (simple)		/* successfully simplified output fn */
				{
					/*
					 * Input functions may want 1 to 3 arguments.  We always
					 * supply all three, trusting that nothing downstream will
					 * complain.
					 */
					args = list_make3(simple,
									  makeConst(OIDOID,
												-1,
												InvalidOid,
												sizeof(Oid),
												ObjectIdGetDatum(intypioparam),
												false,
												true),
									  makeConst(INT4OID,
												-1,
												InvalidOid,
												sizeof(int32),
												Int32GetDatum(-1),
												false,
												true));

					simple = simplify_function(infunc,
											   expr->resulttype, -1,
											   expr->resultcollid,
											   InvalidOid,
											   &args,
											   false,
											   false,
											   true,
											   context);
					if (simple) /* successfully simplified input fn */
						return (Node *) simple;
				}

				/*
				 * The expression cannot be simplified any further, so build
				 * and return a replacement CoerceViaIO node using the
				 * possibly-simplified argument.
				 */
				newexpr = makeNode(CoerceViaIO);
				newexpr->arg = (Expr *) linitial(args);
				newexpr->resulttype = expr->resulttype;
				newexpr->resultcollid = expr->resultcollid;
				newexpr->coerceformat = expr->coerceformat;
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *ac = makeNode(ArrayCoerceExpr);
				Node	   *save_case_val;

				/*
				 * Copy the node and const-simplify its arguments.  We can't
				 * use ece_generic_processing() here because we need to mess
				 * with case_val only while processing the elemexpr.
				 */
				memcpy(ac, node, sizeof(ArrayCoerceExpr));
				ac->arg = (Expr *)
					eval_const_expressions_mutator((Node *) ac->arg,
												   context);

				/*
				 * Set up for the CaseTestExpr node contained in the elemexpr.
				 * We must prevent it from absorbing any outer CASE value.
				 */
				save_case_val = context->case_val;
				context->case_val = NULL;

				ac->elemexpr = (Expr *)
					eval_const_expressions_mutator((Node *) ac->elemexpr,
												   context);

				context->case_val = save_case_val;

				/*
				 * If constant argument and the per-element expression is
				 * immutable, we can simplify the whole thing to a constant.
				 * Exception: although contain_mutable_functions considers
				 * CoerceToDomain immutable for historical reasons, let's not
				 * do so here; this ensures coercion to an array-over-domain
				 * does not apply the domain's constraints until runtime.
				 */
				if (ac->arg && IsA(ac->arg, Const) &&
					ac->elemexpr && !IsA(ac->elemexpr, CoerceToDomain) &&
					!contain_mutable_functions((Node *) ac->elemexpr))
					return ece_evaluate_expr(ac);

				return (Node *) ac;
			}
		case T_CollateExpr:
			{
				/*
				 * We replace CollateExpr with RelabelType, so as to improve
				 * uniformity of expression representation and thus simplify
				 * comparison of expressions.  Hence this looks very nearly
				 * the same as the RelabelType case, and we can apply the same
				 * optimizations to avoid unnecessary RelabelTypes.
				 */
				CollateExpr *collate = (CollateExpr *) node;
				Node	   *arg;

				/* Simplify the input ... */
				arg = eval_const_expressions_mutator((Node *) collate->arg,
													 context);
				/* ... and attach a new RelabelType node, if needed */
				return applyRelabelType(arg,
										exprType(arg),
										exprTypmod(arg),
										collate->collOid,
										COERCE_IMPLICIT_CAST,
										collate->location,
										true);
			}
		case T_CaseExpr:
			{
				/*----------
				 * CASE expressions can be simplified if there are constant
				 * condition clauses:
				 *		FALSE (or NULL): drop the alternative
				 *		TRUE: drop all remaining alternatives
				 * If the first non-FALSE alternative is a constant TRUE,
				 * we can simplify the entire CASE to that alternative's
				 * expression.  If there are no non-FALSE alternatives,
				 * we simplify the entire CASE to the default result (ELSE).
				 *
				 * If we have a simple-form CASE with constant test
				 * expression, we substitute the constant value for contained
				 * CaseTestExpr placeholder nodes, so that we have the
				 * opportunity to reduce constant test conditions.  For
				 * example this allows
				 *		CASE 0 WHEN 0 THEN 1 ELSE 1/0 END
				 * to reduce to 1 rather than drawing a divide-by-0 error.
				 * Note that when the test expression is constant, we don't
				 * have to include it in the resulting CASE; for example
				 *		CASE 0 WHEN x THEN y ELSE z END
				 * is transformed by the parser to
				 *		CASE 0 WHEN CaseTestExpr = x THEN y ELSE z END
				 * which we can simplify to
				 *		CASE WHEN 0 = x THEN y ELSE z END
				 * It is not necessary for the executor to evaluate the "arg"
				 * expression when executing the CASE, since any contained
				 * CaseTestExprs that might have referred to it will have been
				 * replaced by the constant.
				 *----------
				 */
				CaseExpr   *caseexpr = (CaseExpr *) node;
				CaseExpr   *newcase;
				Node	   *save_case_val;
				Node	   *newarg;
				List	   *newargs;
				bool		const_true_cond;
				Node	   *defresult = NULL;
				ListCell   *arg;

				/* Simplify the test expression, if any */
				newarg = eval_const_expressions_mutator((Node *) caseexpr->arg,
														context);

				/* Set up for contained CaseTestExpr nodes */
				save_case_val = context->case_val;
				if (newarg && IsA(newarg, Const))
				{
					context->case_val = newarg;
					newarg = NULL;	/* not needed anymore, see above */
				}
				else
					context->case_val = NULL;

				/* Simplify the WHEN clauses */
				newargs = NIL;
				const_true_cond = false;
				foreach(arg, caseexpr->args)
				{
					CaseWhen   *oldcasewhen = lfirst_node(CaseWhen, arg);
					Node	   *casecond;
					Node	   *caseresult;

					/* Simplify this alternative's test condition */
					casecond = eval_const_expressions_mutator((Node *) oldcasewhen->expr,
															  context);

					/*
					 * If the test condition is constant FALSE (or NULL), then
					 * drop this WHEN clause completely, without processing
					 * the result.
					 */
					if (casecond && IsA(casecond, Const))
					{
						Const	   *const_input = (Const *) casecond;

						if (const_input->constisnull ||
							!DatumGetBool(const_input->constvalue))
							continue;	/* drop alternative with FALSE cond */
						/* Else it's constant TRUE */
						const_true_cond = true;
					}

					/* Simplify this alternative's result value */
					caseresult = eval_const_expressions_mutator((Node *) oldcasewhen->result,
																context);

					/* If non-constant test condition, emit a new WHEN node */
					if (!const_true_cond)
					{
						CaseWhen   *newcasewhen = makeNode(CaseWhen);

						newcasewhen->expr = (Expr *) casecond;
						newcasewhen->result = (Expr *) caseresult;
						newcasewhen->location = oldcasewhen->location;
						newargs = lappend(newargs, newcasewhen);
						continue;
					}

					/*
					 * Found a TRUE condition, so none of the remaining
					 * alternatives can be reached.  We treat the result as
					 * the default result.
					 */
					defresult = caseresult;
					break;
				}

				/* Simplify the default result, unless we replaced it above */
				if (!const_true_cond)
					defresult = eval_const_expressions_mutator((Node *) caseexpr->defresult,
															   context);

				context->case_val = save_case_val;

				/*
				 * If no non-FALSE alternatives, CASE reduces to the default
				 * result
				 */
				if (newargs == NIL)
					return defresult;
				/* Otherwise we need a new CASE node */
				newcase = makeNode(CaseExpr);
				newcase->casetype = caseexpr->casetype;
				newcase->casecollid = caseexpr->casecollid;
				newcase->arg = (Expr *) newarg;
				newcase->args = newargs;
				newcase->defresult = (Expr *) defresult;
				newcase->location = caseexpr->location;
				return (Node *) newcase;
			}
		case T_CaseTestExpr:
			{
				/*
				 * If we know a constant test value for the current CASE
				 * construct, substitute it for the placeholder.  Else just
				 * return the placeholder as-is.
				 */
				if (context->case_val)
					return copyObject(context->case_val);
				else
					return copyObject(node);
			}
		case T_SubscriptingRef:
		case T_ArrayExpr:
		case T_RowExpr:
		case T_MinMaxExpr:
			{
				/*
				 * Generic handling for node types whose own processing is
				 * known to be immutable, and for which we need no smarts
				 * beyond "simplify if all inputs are constants".
				 *
				 * Treating SubscriptingRef this way assumes that subscripting
				 * fetch and assignment are both immutable.  This constrains
				 * type-specific subscripting implementations; maybe we should
				 * relax it someday.
				 *
				 * Treating MinMaxExpr this way amounts to assuming that the
				 * btree comparison function it calls is immutable; see the
				 * reasoning in contain_mutable_functions_walker.
				 */

				/* Copy the node and const-simplify its arguments */
				node = ece_generic_processing(node);
				/* If all arguments are Consts, we can fold to a constant */
				if (ece_all_arguments_const(node))
					return ece_evaluate_expr(node);
				return node;
			}
		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesceexpr = (CoalesceExpr *) node;
				CoalesceExpr *newcoalesce;
				List	   *newargs;
				ListCell   *arg;

				newargs = NIL;
				foreach(arg, coalesceexpr->args)
				{
					Node	   *e;

					e = eval_const_expressions_mutator((Node *) lfirst(arg),
													   context);

					/*
					 * We can remove null constants from the list. For a
					 * non-null constant, if it has not been preceded by any
					 * other non-null-constant expressions then it is the
					 * result. Otherwise, it's the next argument, but we can
					 * drop following arguments since they will never be
					 * reached.
					 */
					if (IsA(e, Const))
					{
						if (((Const *) e)->constisnull)
							continue;	/* drop null constant */
						if (newargs == NIL)
							return e;	/* first expr */
						newargs = lappend(newargs, e);
						break;
					}
					newargs = lappend(newargs, e);
				}

				/*
				 * If all the arguments were constant null, the result is just
				 * null
				 */
				if (newargs == NIL)
					return (Node *) makeNullConst(coalesceexpr->coalescetype,
												  -1,
												  coalesceexpr->coalescecollid);

				/*
				 * If there's exactly one surviving argument, we no longer
				 * need COALESCE at all: the result is that argument
				 */
				if (list_length(newargs) == 1)
					return (Node *) linitial(newargs);

				newcoalesce = makeNode(CoalesceExpr);
				newcoalesce->coalescetype = coalesceexpr->coalescetype;
				newcoalesce->coalescecollid = coalesceexpr->coalescecollid;
				newcoalesce->args = newargs;
				newcoalesce->location = coalesceexpr->location;
				return (Node *) newcoalesce;
			}
		case T_SQLValueFunction:
			{
				/*
				 * All variants of SQLValueFunction are stable, so if we are
				 * estimating the expression's value, we should evaluate the
				 * current function value.  Otherwise just copy.
				 */
				SQLValueFunction *svf = (SQLValueFunction *) node;

				if (context->estimate)
					return (Node *) evaluate_expr((Expr *) svf,
												  svf->type,
												  svf->typmod,
												  InvalidOid);
				else
					return copyObject((Node *) svf);
			}
		case T_FieldSelect:
			{
				/*
				 * We can optimize field selection from a whole-row Var into a
				 * simple Var.  (This case won't be generated directly by the
				 * parser, because ParseComplexProjection short-circuits it.
				 * But it can arise while simplifying functions.)  Also, we
				 * can optimize field selection from a RowExpr construct, or
				 * of course from a constant.
				 *
				 * However, replacing a whole-row Var in this way has a
				 * pitfall: if we've already built the rel targetlist for the
				 * source relation, then the whole-row Var is scheduled to be
				 * produced by the relation scan, but the simple Var probably
				 * isn't, which will lead to a failure in setrefs.c.  This is
				 * not a problem when handling simple single-level queries, in
				 * which expression simplification always happens first.  It
				 * is a risk for lateral references from subqueries, though.
				 * To avoid such failures, don't optimize uplevel references.
				 *
				 * We must also check that the declared type of the field is
				 * still the same as when the FieldSelect was created --- this
				 * can change if someone did ALTER COLUMN TYPE on the rowtype.
				 * If it isn't, we skip the optimization; the case will
				 * probably fail at runtime, but that's not our problem here.
				 */
				FieldSelect *fselect = (FieldSelect *) node;
				FieldSelect *newfselect;
				Node	   *arg;

				arg = eval_const_expressions_mutator((Node *) fselect->arg,
													 context);
				if (arg && IsA(arg, Var) &&
					((Var *) arg)->varattno == InvalidAttrNumber &&
					((Var *) arg)->varlevelsup == 0)
				{
					if (rowtype_field_matches(((Var *) arg)->vartype,
											  fselect->fieldnum,
											  fselect->resulttype,
											  fselect->resulttypmod,
											  fselect->resultcollid))
					{
						Var		   *newvar;

						newvar = makeVar(((Var *) arg)->varno,
										 fselect->fieldnum,
										 fselect->resulttype,
										 fselect->resulttypmod,
										 fselect->resultcollid,
										 ((Var *) arg)->varlevelsup);
						/* New Var has same OLD/NEW returning as old one */
						newvar->varreturningtype = ((Var *) arg)->varreturningtype;
						/* New Var is nullable by same rels as the old one */
						newvar->varnullingrels = ((Var *) arg)->varnullingrels;
						return (Node *) newvar;
					}
				}
				if (arg && IsA(arg, RowExpr))
				{
					RowExpr    *rowexpr = (RowExpr *) arg;

					if (fselect->fieldnum > 0 &&
						fselect->fieldnum <= list_length(rowexpr->args))
					{
						Node	   *fld = (Node *) list_nth(rowexpr->args,
															fselect->fieldnum - 1);

						if (rowtype_field_matches(rowexpr->row_typeid,
												  fselect->fieldnum,
												  fselect->resulttype,
												  fselect->resulttypmod,
												  fselect->resultcollid) &&
							fselect->resulttype == exprType(fld) &&
							fselect->resulttypmod == exprTypmod(fld) &&
							fselect->resultcollid == exprCollation(fld))
							return fld;
					}
				}
				newfselect = makeNode(FieldSelect);
				newfselect->arg = (Expr *) arg;
				newfselect->fieldnum = fselect->fieldnum;
				newfselect->resulttype = fselect->resulttype;
				newfselect->resulttypmod = fselect->resulttypmod;
				newfselect->resultcollid = fselect->resultcollid;
				if (arg && IsA(arg, Const))
				{
					Const	   *con = (Const *) arg;

					if (rowtype_field_matches(con->consttype,
											  newfselect->fieldnum,
											  newfselect->resulttype,
											  newfselect->resulttypmod,
											  newfselect->resultcollid))
						return ece_evaluate_expr(newfselect);
				}
				return (Node *) newfselect;
			}
		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;
				NullTest   *newntest;
				Node	   *arg;

				arg = eval_const_expressions_mutator((Node *) ntest->arg,
													 context);
				if (ntest->argisrow && arg && IsA(arg, RowExpr))
				{
					/*
					 * We break ROW(...) IS [NOT] NULL into separate tests on
					 * its component fields.  This form is usually more
					 * efficient to evaluate, as well as being more amenable
					 * to optimization.
					 */
					RowExpr    *rarg = (RowExpr *) arg;
					List	   *newargs = NIL;
					ListCell   *l;

					foreach(l, rarg->args)
					{
						Node	   *relem = (Node *) lfirst(l);

						/*
						 * A constant field refutes the whole NullTest if it's
						 * of the wrong nullness; else we can discard it.
						 */
						if (relem && IsA(relem, Const))
						{
							Const	   *carg = (Const *) relem;

							if (carg->constisnull ?
								(ntest->nulltesttype == IS_NOT_NULL) :
								(ntest->nulltesttype == IS_NULL))
								return makeBoolConst(false, false);
							continue;
						}

						/*
						 * Else, make a scalar (argisrow == false) NullTest
						 * for this field.  Scalar semantics are required
						 * because IS [NOT] NULL doesn't recurse; see comments
						 * in ExecEvalRowNullInt().
						 */
						newntest = makeNode(NullTest);
						newntest->arg = (Expr *) relem;
						newntest->nulltesttype = ntest->nulltesttype;
						newntest->argisrow = false;
						newntest->location = ntest->location;
						newargs = lappend(newargs, newntest);
					}
					/* If all the inputs were constants, result is TRUE */
					if (newargs == NIL)
						return makeBoolConst(true, false);
					/* If only one nonconst input, it's the result */
					if (list_length(newargs) == 1)
						return (Node *) linitial(newargs);
					/* Else we need an AND node */
					return (Node *) make_andclause(newargs);
				}
				if (!ntest->argisrow && arg && IsA(arg, Const))
				{
					Const	   *carg = (Const *) arg;
					bool		result;

					switch (ntest->nulltesttype)
					{
						case IS_NULL:
							result = carg->constisnull;
							break;
						case IS_NOT_NULL:
							result = !carg->constisnull;
							break;
						default:
							elog(ERROR, "unrecognized nulltesttype: %d",
								 (int) ntest->nulltesttype);
							result = false; /* keep compiler quiet */
							break;
					}

					return makeBoolConst(result, false);
				}

				newntest = makeNode(NullTest);
				newntest->arg = (Expr *) arg;
				newntest->nulltesttype = ntest->nulltesttype;
				newntest->argisrow = ntest->argisrow;
				newntest->location = ntest->location;
				return (Node *) newntest;
			}
		case T_BooleanTest:
			{
				/*
				 * This case could be folded into the generic handling used
				 * for ArrayExpr etc.  But because the simplification logic is
				 * so trivial, applying evaluate_expr() to perform it would be
				 * a heavy overhead.  BooleanTest is probably common enough to
				 * justify keeping this bespoke implementation.
				 */
				BooleanTest *btest = (BooleanTest *) node;
				BooleanTest *newbtest;
				Node	   *arg;

				arg = eval_const_expressions_mutator((Node *) btest->arg,
													 context);
				if (arg && IsA(arg, Const))
				{
					Const	   *carg = (Const *) arg;
					bool		result;

					switch (btest->booltesttype)
					{
						case IS_TRUE:
							result = (!carg->constisnull &&
									  DatumGetBool(carg->constvalue));
							break;
						case IS_NOT_TRUE:
							result = (carg->constisnull ||
									  !DatumGetBool(carg->constvalue));
							break;
						case IS_FALSE:
							result = (!carg->constisnull &&
									  !DatumGetBool(carg->constvalue));
							break;
						case IS_NOT_FALSE:
							result = (carg->constisnull ||
									  DatumGetBool(carg->constvalue));
							break;
						case IS_UNKNOWN:
							result = carg->constisnull;
							break;
						case IS_NOT_UNKNOWN:
							result = !carg->constisnull;
							break;
						default:
							elog(ERROR, "unrecognized booltesttype: %d",
								 (int) btest->booltesttype);
							result = false; /* keep compiler quiet */
							break;
					}

					return makeBoolConst(result, false);
				}

				newbtest = makeNode(BooleanTest);
				newbtest->arg = (Expr *) arg;
				newbtest->booltesttype = btest->booltesttype;
				newbtest->location = btest->location;
				return (Node *) newbtest;
			}
		case T_CoerceToDomain:
			{
				/*
				 * If the domain currently has no constraints, we replace the
				 * CoerceToDomain node with a simple RelabelType, which is
				 * both far faster to execute and more amenable to later
				 * optimization.  We must then mark the plan as needing to be
				 * rebuilt if the domain's constraints change.
				 *
				 * Also, in estimation mode, always replace CoerceToDomain
				 * nodes, effectively assuming that the coercion will succeed.
				 */
				CoerceToDomain *cdomain = (CoerceToDomain *) node;
				CoerceToDomain *newcdomain;
				Node	   *arg;

				arg = eval_const_expressions_mutator((Node *) cdomain->arg,
													 context);
				if (context->estimate ||
					!DomainHasConstraints(cdomain->resulttype))
				{
					/* Record dependency, if this isn't estimation mode */
					if (context->root && !context->estimate)
						record_plan_type_dependency(context->root,
													cdomain->resulttype);

					/* Generate RelabelType to substitute for CoerceToDomain */
					return applyRelabelType(arg,
											cdomain->resulttype,
											cdomain->resulttypmod,
											cdomain->resultcollid,
											cdomain->coercionformat,
											cdomain->location,
											true);
				}

				newcdomain = makeNode(CoerceToDomain);
				newcdomain->arg = (Expr *) arg;
				newcdomain->resulttype = cdomain->resulttype;
				newcdomain->resulttypmod = cdomain->resulttypmod;
				newcdomain->resultcollid = cdomain->resultcollid;
				newcdomain->coercionformat = cdomain->coercionformat;
				newcdomain->location = cdomain->location;
				return (Node *) newcdomain;
			}
		case T_PlaceHolderVar:

			/*
			 * In estimation mode, just strip the PlaceHolderVar node
			 * altogether; this amounts to estimating that the contained value
			 * won't be forced to null by an outer join.  In regular mode we
			 * just use the default behavior (ie, simplify the expression but
			 * leave the PlaceHolderVar node intact).
			 */
			if (context->estimate)
			{
				PlaceHolderVar *phv = (PlaceHolderVar *) node;

				return eval_const_expressions_mutator((Node *) phv->phexpr,
													  context);
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *cre = castNode(ConvertRowtypeExpr, node);
				Node	   *arg;
				ConvertRowtypeExpr *newcre;

				arg = eval_const_expressions_mutator((Node *) cre->arg,
													 context);

				newcre = makeNode(ConvertRowtypeExpr);
				newcre->resulttype = cre->resulttype;
				newcre->convertformat = cre->convertformat;
				newcre->location = cre->location;

				/*
				 * In case of a nested ConvertRowtypeExpr, we can convert the
				 * leaf row directly to the topmost row format without any
				 * intermediate conversions. (This works because
				 * ConvertRowtypeExpr is used only for child->parent
				 * conversion in inheritance trees, which works by exact match
				 * of column name, and a column absent in an intermediate
				 * result can't be present in the final result.)
				 *
				 * No need to check more than one level deep, because the
				 * above recursion will have flattened anything else.
				 */
				if (arg != NULL && IsA(arg, ConvertRowtypeExpr))
				{
					ConvertRowtypeExpr *argcre = (ConvertRowtypeExpr *) arg;

					arg = (Node *) argcre->arg;

					/*
					 * Make sure an outer implicit conversion can't hide an
					 * inner explicit one.
					 */
					if (newcre->convertformat == COERCE_IMPLICIT_CAST)
						newcre->convertformat = argcre->convertformat;
				}

				newcre->arg = (Expr *) arg;

				if (arg != NULL && IsA(arg, Const))
					return ece_evaluate_expr((Node *) newcre);
				return (Node *) newcre;
			}
		default:
			break;
	}

	/*
	 * For any node type not handled above, copy the node unchanged but
	 * const-simplify its subexpressions.  This is the correct thing for node
	 * types whose behavior might change between planning and execution, such
	 * as CurrentOfExpr.  It's also a safe default for new node types not
	 * known to this routine.
	 */
	return ece_generic_processing(node);
}

/*
 * Subroutine for eval_const_expressions: check for non-Const nodes.
 *
 * We can abort recursion immediately on finding a non-Const node.  This is
 * critical for performance, else eval_const_expressions_mutator would take
 * O(N^2) time on non-simplifiable trees.  However, we do need to descend
 * into List nodes since expression_tree_walker sometimes invokes the walker
 * function directly on List subtrees.
 */
static bool
contain_non_const_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Const))
		return false;
	if (IsA(node, List))
		return expression_tree_walker(node, contain_non_const_walker, context);
	/* Otherwise, abort the tree traversal and return true */
	return true;
}

/*
 * Subroutine for eval_const_expressions: check if a function is OK to evaluate
 */
static bool
ece_function_is_safe(Oid funcid, eval_const_expressions_context *context)
{
	char		provolatile = func_volatile(funcid);

	/*
	 * Ordinarily we are only allowed to simplify immutable functions. But for
	 * purposes of estimation, we consider it okay to simplify functions that
	 * are merely stable; the risk that the result might change from planning
	 * time to execution time is worth taking in preference to not being able
	 * to estimate the value at all.
	 */
	if (provolatile == PROVOLATILE_IMMUTABLE)
		return true;
	if (context->estimate && provolatile == PROVOLATILE_STABLE)
		return true;
	return false;
}

/*
 * Subroutine for eval_const_expressions: process arguments of an OR clause
 *
 * This includes flattening of nested ORs as well as recursion to
 * eval_const_expressions to simplify the OR arguments.
 *
 * After simplification, OR arguments are handled as follows:
 *		non constant: keep
 *		FALSE: drop (does not affect result)
 *		TRUE: force result to TRUE
 *		NULL: keep only one
 * We must keep one NULL input because OR expressions evaluate to NULL when no
 * input is TRUE and at least one is NULL.  We don't actually include the NULL
 * here, that's supposed to be done by the caller.
 *
 * The output arguments *haveNull and *forceTrue must be initialized false
 * by the caller.  They will be set true if a NULL constant or TRUE constant,
 * respectively, is detected anywhere in the argument list.
 */
static List *
simplify_or_arguments(List *args,
					  eval_const_expressions_context *context,
					  bool *haveNull, bool *forceTrue)
{
	List	   *newargs = NIL;
	List	   *unprocessed_args;

	/*
	 * We want to ensure that any OR immediately beneath another OR gets
	 * flattened into a single OR-list, so as to simplify later reasoning.
	 *
	 * To avoid stack overflow from recursion of eval_const_expressions, we
	 * resort to some tenseness here: we keep a list of not-yet-processed
	 * inputs, and handle flattening of nested ORs by prepending to the to-do
	 * list instead of recursing.  Now that the parser generates N-argument
	 * ORs from simple lists, this complexity is probably less necessary than
	 * it once was, but we might as well keep the logic.
	 */
	unprocessed_args = list_copy(args);
	while (unprocessed_args)
	{
		Node	   *arg = (Node *) linitial(unprocessed_args);

		unprocessed_args = list_delete_first(unprocessed_args);

		/* flatten nested ORs as per above comment */
		if (is_orclause(arg))
		{
			List	   *subargs = ((BoolExpr *) arg)->args;
			List	   *oldlist = unprocessed_args;

			unprocessed_args = list_concat_copy(subargs, unprocessed_args);
			/* perhaps-overly-tense code to avoid leaking old lists */
			list_free(oldlist);
			continue;
		}

		/* If it's not an OR, simplify it */
		arg = eval_const_expressions_mutator(arg, context);

		/*
		 * It is unlikely but not impossible for simplification of a non-OR
		 * clause to produce an OR.  Recheck, but don't be too tense about it
		 * since it's not a mainstream case.  In particular we don't worry
		 * about const-simplifying the input twice, nor about list leakage.
		 */
		if (is_orclause(arg))
		{
			List	   *subargs = ((BoolExpr *) arg)->args;

			unprocessed_args = list_concat_copy(subargs, unprocessed_args);
			continue;
		}

		/*
		 * OK, we have a const-simplified non-OR argument.  Process it per
		 * comments above.
		 */
		if (IsA(arg, Const))
		{
			Const	   *const_input = (Const *) arg;

			if (const_input->constisnull)
				*haveNull = true;
			else if (DatumGetBool(const_input->constvalue))
			{
				*forceTrue = true;

				/*
				 * Once we detect a TRUE result we can just exit the loop
				 * immediately.  However, if we ever add a notion of
				 * non-removable functions, we'd need to keep scanning.
				 */
				return NIL;
			}
			/* otherwise, we can drop the constant-false input */
			continue;
		}

		/* else emit the simplified arg into the result list */
		newargs = lappend(newargs, arg);
	}

	return newargs;
}

/*
 * Subroutine for eval_const_expressions: process arguments of an AND clause
 *
 * This includes flattening of nested ANDs as well as recursion to
 * eval_const_expressions to simplify the AND arguments.
 *
 * After simplification, AND arguments are handled as follows:
 *		non constant: keep
 *		TRUE: drop (does not affect result)
 *		FALSE: force result to FALSE
 *		NULL: keep only one
 * We must keep one NULL input because AND expressions evaluate to NULL when
 * no input is FALSE and at least one is NULL.  We don't actually include the
 * NULL here, that's supposed to be done by the caller.
 *
 * The output arguments *haveNull and *forceFalse must be initialized false
 * by the caller.  They will be set true if a null constant or false constant,
 * respectively, is detected anywhere in the argument list.
 */
static List *
simplify_and_arguments(List *args,
					   eval_const_expressions_context *context,
					   bool *haveNull, bool *forceFalse)
{
	List	   *newargs = NIL;
	List	   *unprocessed_args;

	/* See comments in simplify_or_arguments */
	unprocessed_args = list_copy(args);
	while (unprocessed_args)
	{
		Node	   *arg = (Node *) linitial(unprocessed_args);

		unprocessed_args = list_delete_first(unprocessed_args);

		/* flatten nested ANDs as per above comment */
		if (is_andclause(arg))
		{
			List	   *subargs = ((BoolExpr *) arg)->args;
			List	   *oldlist = unprocessed_args;

			unprocessed_args = list_concat_copy(subargs, unprocessed_args);
			/* perhaps-overly-tense code to avoid leaking old lists */
			list_free(oldlist);
			continue;
		}

		/* If it's not an AND, simplify it */
		arg = eval_const_expressions_mutator(arg, context);

		/*
		 * It is unlikely but not impossible for simplification of a non-AND
		 * clause to produce an AND.  Recheck, but don't be too tense about it
		 * since it's not a mainstream case.  In particular we don't worry
		 * about const-simplifying the input twice, nor about list leakage.
		 */
		if (is_andclause(arg))
		{
			List	   *subargs = ((BoolExpr *) arg)->args;

			unprocessed_args = list_concat_copy(subargs, unprocessed_args);
			continue;
		}

		/*
		 * OK, we have a const-simplified non-AND argument.  Process it per
		 * comments above.
		 */
		if (IsA(arg, Const))
		{
			Const	   *const_input = (Const *) arg;

			if (const_input->constisnull)
				*haveNull = true;
			else if (!DatumGetBool(const_input->constvalue))
			{
				*forceFalse = true;

				/*
				 * Once we detect a FALSE result we can just exit the loop
				 * immediately.  However, if we ever add a notion of
				 * non-removable functions, we'd need to keep scanning.
				 */
				return NIL;
			}
			/* otherwise, we can drop the constant-true input */
			continue;
		}

		/* else emit the simplified arg into the result list */
		newargs = lappend(newargs, arg);
	}

	return newargs;
}

/*
 * Subroutine for eval_const_expressions: try to simplify boolean equality
 * or inequality condition
 *
 * Inputs are the operator OID and the simplified arguments to the operator.
 * Returns a simplified expression if successful, or NULL if cannot
 * simplify the expression.
 *
 * The idea here is to reduce "x = true" to "x" and "x = false" to "NOT x",
 * or similarly "x <> true" to "NOT x" and "x <> false" to "x".
 * This is only marginally useful in itself, but doing it in constant folding
 * ensures that we will recognize these forms as being equivalent in, for
 * example, partial index matching.
 *
 * We come here only if simplify_function has failed; therefore we cannot
 * see two constant inputs, nor a constant-NULL input.
 */
static Node *
simplify_boolean_equality(Oid opno, List *args)
{
	Node	   *leftop;
	Node	   *rightop;

	Assert(list_length(args) == 2);
	leftop = linitial(args);
	rightop = lsecond(args);
	if (leftop && IsA(leftop, Const))
	{
		Assert(!((Const *) leftop)->constisnull);
		if (opno == BooleanEqualOperator)
		{
			if (DatumGetBool(((Const *) leftop)->constvalue))
				return rightop; /* true = foo */
			else
				return negate_clause(rightop);	/* false = foo */
		}
		else
		{
			if (DatumGetBool(((Const *) leftop)->constvalue))
				return negate_clause(rightop);	/* true <> foo */
			else
				return rightop; /* false <> foo */
		}
	}
	if (rightop && IsA(rightop, Const))
	{
		Assert(!((Const *) rightop)->constisnull);
		if (opno == BooleanEqualOperator)
		{
			if (DatumGetBool(((Const *) rightop)->constvalue))
				return leftop;	/* foo = true */
			else
				return negate_clause(leftop);	/* foo = false */
		}
		else
		{
			if (DatumGetBool(((Const *) rightop)->constvalue))
				return negate_clause(leftop);	/* foo <> true */
			else
				return leftop;	/* foo <> false */
		}
	}
	return NULL;
}

/*
 * Subroutine for eval_const_expressions: try to simplify a function call
 * (which might originally have been an operator; we don't care)
 *
 * Inputs are the function OID, actual result type OID (which is needed for
 * polymorphic functions), result typmod, result collation, the input
 * collation to use for the function, the original argument list (not
 * const-simplified yet, unless process_args is false), and some flags;
 * also the context data for eval_const_expressions.
 *
 * Returns a simplified expression if successful, or NULL if cannot
 * simplify the function call.
 *
 * This function is also responsible for converting named-notation argument
 * lists into positional notation and/or adding any needed default argument
 * expressions; which is a bit grotty, but it avoids extra fetches of the
 * function's pg_proc tuple.  For this reason, the args list is
 * pass-by-reference.  Conversion and const-simplification of the args list
 * will be done even if simplification of the function call itself is not
 * possible.
 */
static Expr *
simplify_function(Oid funcid, Oid result_type, int32 result_typmod,
				  Oid result_collid, Oid input_collid, List **args_p,
				  bool funcvariadic, bool process_args, bool allow_non_const,
				  eval_const_expressions_context *context)
{
	List	   *args = *args_p;
	HeapTuple	func_tuple;
	Form_pg_proc func_form;
	Expr	   *newexpr;

	/*
	 * We have three strategies for simplification: execute the function to
	 * deliver a constant result, use a transform function to generate a
	 * substitute node tree, or expand in-line the body of the function
	 * definition (which only works for simple SQL-language functions, but
	 * that is a common case).  Each case needs access to the function's
	 * pg_proc tuple, so fetch it just once.
	 *
	 * Note: the allow_non_const flag suppresses both the second and third
	 * strategies; so if !allow_non_const, simplify_function can only return a
	 * Const or NULL.  Argument-list rewriting happens anyway, though.
	 */
	func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(func_tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	func_form = (Form_pg_proc) GETSTRUCT(func_tuple);

	/*
	 * Process the function arguments, unless the caller did it already.
	 *
	 * Here we must deal with named or defaulted arguments, and then
	 * recursively apply eval_const_expressions to the whole argument list.
	 */
	if (process_args)
	{
		args = expand_function_arguments(args, false, result_type, func_tuple);
		args = (List *) expression_tree_mutator((Node *) args,
												eval_const_expressions_mutator,
												context);
		/* Argument processing done, give it back to the caller */
		*args_p = args;
	}

	/* Now attempt simplification of the function call proper. */

	newexpr = evaluate_function(funcid, result_type, result_typmod,
								result_collid, input_collid,
								args, funcvariadic,
								func_tuple, context);

	if (!newexpr && allow_non_const && OidIsValid(func_form->prosupport))
	{
		/*
		 * Build a SupportRequestSimplify node to pass to the support
		 * function, pointing to a dummy FuncExpr node containing the
		 * simplified arg list.  We use this approach to present a uniform
		 * interface to the support function regardless of how the target
		 * function is actually being invoked.
		 */
		SupportRequestSimplify req;
		FuncExpr	fexpr;

		fexpr.xpr.type = T_FuncExpr;
		fexpr.funcid = funcid;
		fexpr.funcresulttype = result_type;
		fexpr.funcretset = func_form->proretset;
		fexpr.funcvariadic = funcvariadic;
		fexpr.funcformat = COERCE_EXPLICIT_CALL;
		fexpr.funccollid = result_collid;
		fexpr.inputcollid = input_collid;
		fexpr.args = args;
		fexpr.location = -1;

		req.type = T_SupportRequestSimplify;
		req.root = context->root;
		req.fcall = &fexpr;

		newexpr = (Expr *)
			DatumGetPointer(OidFunctionCall1(func_form->prosupport,
											 PointerGetDatum(&req)));

		/* catch a possible API misunderstanding */
		Assert(newexpr != (Expr *) &fexpr);
	}

	if (!newexpr && allow_non_const)
		newexpr = inline_function(funcid, result_type, result_collid,
								  input_collid, args, funcvariadic,
								  func_tuple, context);

	ReleaseSysCache(func_tuple);

	return newexpr;
}

/*
 * expand_function_arguments: convert named-notation args to positional args
 * and/or insert default args, as needed
 *
 * Returns a possibly-transformed version of the args list.
 *
 * If include_out_arguments is true, then the args list and the result
 * include OUT arguments.
 *
 * The expected result type of the call must be given, for sanity-checking
 * purposes.  Also, we ask the caller to provide the function's actual
 * pg_proc tuple, not just its OID.
 *
 * If we need to change anything, the input argument list is copied, not
 * modified.
 *
 * Note: this gets applied to operator argument lists too, even though the
 * cases it handles should never occur there.  This should be OK since it
 * will fall through very quickly if there's nothing to do.
 */
List *
expand_function_arguments(List *args, bool include_out_arguments,
						  Oid result_type, HeapTuple func_tuple)
{
	Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	Oid		   *proargtypes = funcform->proargtypes.values;
	int			pronargs = funcform->pronargs;
	bool		has_named_args = false;
	ListCell   *lc;

	/*
	 * If we are asked to match to OUT arguments, then use the proallargtypes
	 * array (which includes those); otherwise use proargtypes (which
	 * doesn't).  Of course, if proallargtypes is null, we always use
	 * proargtypes.  (Fetching proallargtypes is annoyingly expensive
	 * considering that we may have nothing to do here, but fortunately the
	 * common case is include_out_arguments == false.)
	 */
	if (include_out_arguments)
	{
		Datum		proallargtypes;
		bool		isNull;

		proallargtypes = SysCacheGetAttr(PROCOID, func_tuple,
										 Anum_pg_proc_proallargtypes,
										 &isNull);
		if (!isNull)
		{
			ArrayType  *arr = DatumGetArrayTypeP(proallargtypes);

			pronargs = ARR_DIMS(arr)[0];
			if (ARR_NDIM(arr) != 1 ||
				pronargs < 0 ||
				ARR_HASNULL(arr) ||
				ARR_ELEMTYPE(arr) != OIDOID)
				elog(ERROR, "proallargtypes is not a 1-D Oid array or it contains nulls");
			Assert(pronargs >= funcform->pronargs);
			proargtypes = (Oid *) ARR_DATA_PTR(arr);
		}
	}

	/* Do we have any named arguments? */
	foreach(lc, args)
	{
		Node	   *arg = (Node *) lfirst(lc);

		if (IsA(arg, NamedArgExpr))
		{
			has_named_args = true;
			break;
		}
	}

	/* If so, we must apply reorder_function_arguments */
	if (has_named_args)
	{
		args = reorder_function_arguments(args, pronargs, func_tuple);
		/* Recheck argument types and add casts if needed */
		recheck_cast_function_args(args, result_type,
								   proargtypes, pronargs,
								   func_tuple);
	}
	else if (list_length(args) < pronargs)
	{
		/* No named args, but we seem to be short some defaults */
		args = add_function_defaults(args, pronargs, func_tuple);
		/* Recheck argument types and add casts if needed */
		recheck_cast_function_args(args, result_type,
								   proargtypes, pronargs,
								   func_tuple);
	}

	return args;
}

/*
 * reorder_function_arguments: convert named-notation args to positional args
 *
 * This function also inserts default argument values as needed, since it's
 * impossible to form a truly valid positional call without that.
 */
static List *
reorder_function_arguments(List *args, int pronargs, HeapTuple func_tuple)
{
	Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	int			nargsprovided = list_length(args);
	Node	   *argarray[FUNC_MAX_ARGS];
	ListCell   *lc;
	int			i;

	Assert(nargsprovided <= pronargs);
	if (pronargs < 0 || pronargs > FUNC_MAX_ARGS)
		elog(ERROR, "too many function arguments");
	memset(argarray, 0, pronargs * sizeof(Node *));

	/* Deconstruct the argument list into an array indexed by argnumber */
	i = 0;
	foreach(lc, args)
	{
		Node	   *arg = (Node *) lfirst(lc);

		if (!IsA(arg, NamedArgExpr))
		{
			/* positional argument, assumed to precede all named args */
			Assert(argarray[i] == NULL);
			argarray[i++] = arg;
		}
		else
		{
			NamedArgExpr *na = (NamedArgExpr *) arg;

			Assert(na->argnumber >= 0 && na->argnumber < pronargs);
			Assert(argarray[na->argnumber] == NULL);
			argarray[na->argnumber] = (Node *) na->arg;
		}
	}

	/*
	 * Fetch default expressions, if needed, and insert into array at proper
	 * locations (they aren't necessarily consecutive or all used)
	 */
	if (nargsprovided < pronargs)
	{
		List	   *defaults = fetch_function_defaults(func_tuple);

		i = pronargs - funcform->pronargdefaults;
		foreach(lc, defaults)
		{
			if (argarray[i] == NULL)
				argarray[i] = (Node *) lfirst(lc);
			i++;
		}
	}

	/* Now reconstruct the args list in proper order */
	args = NIL;
	for (i = 0; i < pronargs; i++)
	{
		Assert(argarray[i] != NULL);
		args = lappend(args, argarray[i]);
	}

	return args;
}

/*
 * add_function_defaults: add missing function arguments from its defaults
 *
 * This is used only when the argument list was positional to begin with,
 * and so we know we just need to add defaults at the end.
 */
static List *
add_function_defaults(List *args, int pronargs, HeapTuple func_tuple)
{
	int			nargsprovided = list_length(args);
	List	   *defaults;
	int			ndelete;

	/* Get all the default expressions from the pg_proc tuple */
	defaults = fetch_function_defaults(func_tuple);

	/* Delete any unused defaults from the list */
	ndelete = nargsprovided + list_length(defaults) - pronargs;
	if (ndelete < 0)
		elog(ERROR, "not enough default arguments");
	if (ndelete > 0)
		defaults = list_delete_first_n(defaults, ndelete);

	/* And form the combined argument list, not modifying the input list */
	return list_concat_copy(args, defaults);
}

/*
 * fetch_function_defaults: get function's default arguments as expression list
 */
static List *
fetch_function_defaults(HeapTuple func_tuple)
{
	List	   *defaults;
	Datum		proargdefaults;
	char	   *str;

	proargdefaults = SysCacheGetAttrNotNull(PROCOID, func_tuple,
											Anum_pg_proc_proargdefaults);
	str = TextDatumGetCString(proargdefaults);
	defaults = castNode(List, stringToNode(str));
	pfree(str);
	return defaults;
}

/*
 * recheck_cast_function_args: recheck function args and typecast as needed
 * after adding defaults.
 *
 * It is possible for some of the defaulted arguments to be polymorphic;
 * therefore we can't assume that the default expressions have the correct
 * data types already.  We have to re-resolve polymorphics and do coercion
 * just like the parser did.
 *
 * This should be a no-op if there are no polymorphic arguments,
 * but we do it anyway to be sure.
 *
 * Note: if any casts are needed, the args list is modified in-place;
 * caller should have already copied the list structure.
 */
static void
recheck_cast_function_args(List *args, Oid result_type,
						   Oid *proargtypes, int pronargs,
						   HeapTuple func_tuple)
{
	Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	int			nargs;
	Oid			actual_arg_types[FUNC_MAX_ARGS];
	Oid			declared_arg_types[FUNC_MAX_ARGS];
	Oid			rettype;
	ListCell   *lc;

	if (list_length(args) > FUNC_MAX_ARGS)
		elog(ERROR, "too many function arguments");
	nargs = 0;
	foreach(lc, args)
	{
		actual_arg_types[nargs++] = exprType((Node *) lfirst(lc));
	}
	Assert(nargs == pronargs);
	memcpy(declared_arg_types, proargtypes, pronargs * sizeof(Oid));
	rettype = enforce_generic_type_consistency(actual_arg_types,
											   declared_arg_types,
											   nargs,
											   funcform->prorettype,
											   false);
	/* let's just check we got the same answer as the parser did ... */
	if (rettype != result_type)
		elog(ERROR, "function's resolved result type changed during planning");

	/* perform any necessary typecasting of arguments */
	make_fn_arguments(NULL, args, actual_arg_types, declared_arg_types);
}

/*
 * evaluate_function: try to pre-evaluate a function call
 *
 * We can do this if the function is strict and has any constant-null inputs
 * (just return a null constant), or if the function is immutable and has all
 * constant inputs (call it and return the result as a Const node).  In
 * estimation mode we are willing to pre-evaluate stable functions too.
 *
 * Returns a simplified expression if successful, or NULL if cannot
 * simplify the function.
 */
static Expr *
evaluate_function(Oid funcid, Oid result_type, int32 result_typmod,
				  Oid result_collid, Oid input_collid, List *args,
				  bool funcvariadic,
				  HeapTuple func_tuple,
				  eval_const_expressions_context *context)
{
	Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	bool		has_nonconst_input = false;
	bool		has_null_input = false;
	ListCell   *arg;
	FuncExpr   *newexpr;

	/*
	 * Can't simplify if it returns a set.
	 */
	if (funcform->proretset)
		return NULL;

	/*
	 * Can't simplify if it returns RECORD.  The immediate problem is that it
	 * will be needing an expected tupdesc which we can't supply here.
	 *
	 * In the case where it has OUT parameters, we could build an expected
	 * tupdesc from those, but there may be other gotchas lurking.  In
	 * particular, if the function were to return NULL, we would produce a
	 * null constant with no remaining indication of which concrete record
	 * type it is.  For now, seems best to leave the function call unreduced.
	 */
	if (funcform->prorettype == RECORDOID)
		return NULL;

	/*
	 * Check for constant inputs and especially constant-NULL inputs.
	 */
	foreach(arg, args)
	{
		if (IsA(lfirst(arg), Const))
			has_null_input |= ((Const *) lfirst(arg))->constisnull;
		else
			has_nonconst_input = true;
	}

	/*
	 * If the function is strict and has a constant-NULL input, it will never
	 * be called at all, so we can replace the call by a NULL constant, even
	 * if there are other inputs that aren't constant, and even if the
	 * function is not otherwise immutable.
	 */
	if (funcform->proisstrict && has_null_input)
		return (Expr *) makeNullConst(result_type, result_typmod,
									  result_collid);

	/*
	 * Otherwise, can simplify only if all inputs are constants. (For a
	 * non-strict function, constant NULL inputs are treated the same as
	 * constant non-NULL inputs.)
	 */
	if (has_nonconst_input)
		return NULL;

	/*
	 * Ordinarily we are only allowed to simplify immutable functions. But for
	 * purposes of estimation, we consider it okay to simplify functions that
	 * are merely stable; the risk that the result might change from planning
	 * time to execution time is worth taking in preference to not being able
	 * to estimate the value at all.
	 */
	if (funcform->provolatile == PROVOLATILE_IMMUTABLE)
		 /* okay */ ;
	else if (context->estimate && funcform->provolatile == PROVOLATILE_STABLE)
		 /* okay */ ;
	else
		return NULL;

	/*
	 * OK, looks like we can simplify this operator/function.
	 *
	 * Build a new FuncExpr node containing the already-simplified arguments.
	 */
	newexpr = makeNode(FuncExpr);
	newexpr->funcid = funcid;
	newexpr->funcresulttype = result_type;
	newexpr->funcretset = false;
	newexpr->funcvariadic = funcvariadic;
	newexpr->funcformat = COERCE_EXPLICIT_CALL; /* doesn't matter */
	newexpr->funccollid = result_collid;	/* doesn't matter */
	newexpr->inputcollid = input_collid;
	newexpr->args = args;
	newexpr->location = -1;

	return evaluate_expr((Expr *) newexpr, result_type, result_typmod,
						 result_collid);
}

/*
 * inline_function: try to expand a function call inline
 *
 * If the function is a sufficiently simple SQL-language function
 * (just "SELECT expression"), then we can inline it and avoid the rather
 * high per-call overhead of SQL functions.  Furthermore, this can expose
 * opportunities for constant-folding within the function expression.
 *
 * We have to beware of some special cases however.  A directly or
 * indirectly recursive function would cause us to recurse forever,
 * so we keep track of which functions we are already expanding and
 * do not re-expand them.  Also, if a parameter is used more than once
 * in the SQL-function body, we require it not to contain any volatile
 * functions (volatiles might deliver inconsistent answers) nor to be
 * unreasonably expensive to evaluate.  The expensiveness check not only
 * prevents us from doing multiple evaluations of an expensive parameter
 * at runtime, but is a safety value to limit growth of an expression due
 * to repeated inlining.
 *
 * We must also beware of changing the volatility or strictness status of
 * functions by inlining them.
 *
 * Also, at the moment we can't inline functions returning RECORD.  This
 * doesn't work in the general case because it discards information such
 * as OUT-parameter declarations.
 *
 * Also, context-dependent expression nodes in the argument list are trouble.
 *
 * Returns a simplified expression if successful, or NULL if cannot
 * simplify the function.
 */
static Expr *
inline_function(Oid funcid, Oid result_type, Oid result_collid,
				Oid input_collid, List *args,
				bool funcvariadic,
				HeapTuple func_tuple,
				eval_const_expressions_context *context)
{
	Form_pg_proc funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
	char	   *src;
	Datum		tmp;
	bool		isNull;
	MemoryContext oldcxt;
	MemoryContext mycxt;
	inline_error_callback_arg callback_arg;
	ErrorContextCallback sqlerrcontext;
	FuncExpr   *fexpr;
	SQLFunctionParseInfoPtr pinfo;
	TupleDesc	rettupdesc;
	ParseState *pstate;
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	Query	   *querytree;
	Node	   *newexpr;
	int		   *usecounts;
	ListCell   *arg;
	int			i;

	/*
	 * Forget it if the function is not SQL-language or has other showstopper
	 * properties.  (The prokind and nargs checks are just paranoia.)
	 */
	if (funcform->prolang != SQLlanguageId ||
		funcform->prokind != PROKIND_FUNCTION ||
		funcform->prosecdef ||
		funcform->proretset ||
		funcform->prorettype == RECORDOID ||
		!heap_attisnull(func_tuple, Anum_pg_proc_proconfig, NULL) ||
		funcform->pronargs != list_length(args))
		return NULL;

	/* Check for recursive function, and give up trying to expand if so */
	if (list_member_oid(context->active_fns, funcid))
		return NULL;

	/* Check permission to call function (fail later, if not) */
	if (object_aclcheck(ProcedureRelationId, funcid, GetUserId(), ACL_EXECUTE) != ACLCHECK_OK)
		return NULL;

	/* Check whether a plugin wants to hook function entry/exit */
	if (FmgrHookIsNeeded(funcid))
		return NULL;

	/*
	 * Make a temporary memory context, so that we don't leak all the stuff
	 * that parsing might create.
	 */
	mycxt = AllocSetContextCreate(CurrentMemoryContext,
								  "inline_function",
								  ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(mycxt);

	/*
	 * We need a dummy FuncExpr node containing the already-simplified
	 * arguments.  (In some cases we don't really need it, but building it is
	 * cheap enough that it's not worth contortions to avoid.)
	 */
	fexpr = makeNode(FuncExpr);
	fexpr->funcid = funcid;
	fexpr->funcresulttype = result_type;
	fexpr->funcretset = false;
	fexpr->funcvariadic = funcvariadic;
	fexpr->funcformat = COERCE_EXPLICIT_CALL;	/* doesn't matter */
	fexpr->funccollid = result_collid;	/* doesn't matter */
	fexpr->inputcollid = input_collid;
	fexpr->args = args;
	fexpr->location = -1;

	/* Fetch the function body */
	tmp = SysCacheGetAttrNotNull(PROCOID, func_tuple, Anum_pg_proc_prosrc);
	src = TextDatumGetCString(tmp);

	/*
	 * Setup error traceback support for ereport().  This is so that we can
	 * finger the function that bad information came from.
	 */
	callback_arg.proname = NameStr(funcform->proname);
	callback_arg.prosrc = src;

	sqlerrcontext.callback = sql_inline_error_callback;
	sqlerrcontext.arg = &callback_arg;
	sqlerrcontext.previous = error_context_stack;
	error_context_stack = &sqlerrcontext;

	/* If we have prosqlbody, pay attention to that not prosrc */
	tmp = SysCacheGetAttr(PROCOID,
						  func_tuple,
						  Anum_pg_proc_prosqlbody,
						  &isNull);
	if (!isNull)
	{
		Node	   *n;
		List	   *query_list;

		n = stringToNode(TextDatumGetCString(tmp));
		if (IsA(n, List))
			query_list = linitial_node(List, castNode(List, n));
		else
			query_list = list_make1(n);
		if (list_length(query_list) != 1)
			goto fail;
		querytree = linitial(query_list);

		/*
		 * Because we'll insist below that the querytree have an empty rtable
		 * and no sublinks, it cannot have any relation references that need
		 * to be locked or rewritten.  So we can omit those steps.
		 */
	}
	else
	{
		/* Set up to handle parameters while parsing the function body. */
		pinfo = prepare_sql_fn_parse_info(func_tuple,
										  (Node *) fexpr,
										  input_collid);

		/*
		 * We just do parsing and parse analysis, not rewriting, because
		 * rewriting will not affect table-free-SELECT-only queries, which is
		 * all that we care about.  Also, we can punt as soon as we detect
		 * more than one command in the function body.
		 */
		raw_parsetree_list = pg_parse_query(src);
		if (list_length(raw_parsetree_list) != 1)
			goto fail;

		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = src;
		sql_fn_parser_setup(pstate, pinfo);

		querytree = transformTopLevelStmt(pstate, linitial(raw_parsetree_list));

		free_parsestate(pstate);
	}

	/*
	 * The single command must be a simple "SELECT expression".
	 *
	 * Note: if you change the tests involved in this, see also plpgsql's
	 * exec_simple_check_plan().  That generally needs to have the same idea
	 * of what's a "simple expression", so that inlining a function that
	 * previously wasn't inlined won't change plpgsql's conclusion.
	 */
	if (!IsA(querytree, Query) ||
		querytree->commandType != CMD_SELECT ||
		querytree->hasAggs ||
		querytree->hasWindowFuncs ||
		querytree->hasTargetSRFs ||
		querytree->hasSubLinks ||
		querytree->cteList ||
		querytree->rtable ||
		querytree->jointree->fromlist ||
		querytree->jointree->quals ||
		querytree->groupClause ||
		querytree->groupingSets ||
		querytree->havingQual ||
		querytree->windowClause ||
		querytree->distinctClause ||
		querytree->sortClause ||
		querytree->limitOffset ||
		querytree->limitCount ||
		querytree->setOperations ||
		list_length(querytree->targetList) != 1)
		goto fail;

	/* If the function result is composite, resolve it */
	(void) get_expr_result_type((Node *) fexpr,
								NULL,
								&rettupdesc);

	/*
	 * Make sure the function (still) returns what it's declared to.  This
	 * will raise an error if wrong, but that's okay since the function would
	 * fail at runtime anyway.  Note that check_sql_fn_retval will also insert
	 * a coercion if needed to make the tlist expression match the declared
	 * type of the function.
	 *
	 * Note: we do not try this until we have verified that no rewriting was
	 * needed; that's probably not important, but let's be careful.
	 */
	querytree_list = list_make1(querytree);
	if (check_sql_fn_retval(list_make1(querytree_list),
							result_type, rettupdesc,
							funcform->prokind,
							false))
		goto fail;				/* reject whole-tuple-result cases */

	/*
	 * Given the tests above, check_sql_fn_retval shouldn't have decided to
	 * inject a projection step, but let's just make sure.
	 */
	if (querytree != linitial(querytree_list))
		goto fail;

	/* Now we can grab the tlist expression */
	newexpr = (Node *) ((TargetEntry *) linitial(querytree->targetList))->expr;

	/*
	 * If the SQL function returns VOID, we can only inline it if it is a
	 * SELECT of an expression returning VOID (ie, it's just a redirection to
	 * another VOID-returning function).  In all non-VOID-returning cases,
	 * check_sql_fn_retval should ensure that newexpr returns the function's
	 * declared result type, so this test shouldn't fail otherwise; but we may
	 * as well cope gracefully if it does.
	 */
	if (exprType(newexpr) != result_type)
		goto fail;

	/*
	 * Additional validity checks on the expression.  It mustn't be more
	 * volatile than the surrounding function (this is to avoid breaking hacks
	 * that involve pretending a function is immutable when it really ain't).
	 * If the surrounding function is declared strict, then the expression
	 * must contain only strict constructs and must use all of the function
	 * parameters (this is overkill, but an exact analysis is hard).
	 */
	if (funcform->provolatile == PROVOLATILE_IMMUTABLE &&
		contain_mutable_functions(newexpr))
		goto fail;
	else if (funcform->provolatile == PROVOLATILE_STABLE &&
			 contain_volatile_functions(newexpr))
		goto fail;

	if (funcform->proisstrict &&
		contain_nonstrict_functions(newexpr))
		goto fail;

	/*
	 * If any parameter expression contains a context-dependent node, we can't
	 * inline, for fear of putting such a node into the wrong context.
	 */
	if (contain_context_dependent_node((Node *) args))
		goto fail;

	/*
	 * We may be able to do it; there are still checks on parameter usage to
	 * make, but those are most easily done in combination with the actual
	 * substitution of the inputs.  So start building expression with inputs
	 * substituted.
	 */
	usecounts = (int *) palloc0(funcform->pronargs * sizeof(int));
	newexpr = substitute_actual_parameters(newexpr, funcform->pronargs,
										   args, usecounts);

	/* Now check for parameter usage */
	i = 0;
	foreach(arg, args)
	{
		Node	   *param = lfirst(arg);

		if (usecounts[i] == 0)
		{
			/* Param not used at all: uncool if func is strict */
			if (funcform->proisstrict)
				goto fail;
		}
		else if (usecounts[i] != 1)
		{
			/* Param used multiple times: uncool if expensive or volatile */
			QualCost	eval_cost;

			/*
			 * We define "expensive" as "contains any subplan or more than 10
			 * operators".  Note that the subplan search has to be done
			 * explicitly, since cost_qual_eval() will barf on unplanned
			 * subselects.
			 */
			if (contain_subplans(param))
				goto fail;
			cost_qual_eval(&eval_cost, list_make1(param), NULL);
			if (eval_cost.startup + eval_cost.per_tuple >
				10 * cpu_operator_cost)
				goto fail;

			/*
			 * Check volatility last since this is more expensive than the
			 * above tests
			 */
			if (contain_volatile_functions(param))
				goto fail;
		}
		i++;
	}

	/*
	 * Whew --- we can make the substitution.  Copy the modified expression
	 * out of the temporary memory context, and clean up.
	 */
	MemoryContextSwitchTo(oldcxt);

	newexpr = copyObject(newexpr);

	MemoryContextDelete(mycxt);

	/*
	 * If the result is of a collatable type, force the result to expose the
	 * correct collation.  In most cases this does not matter, but it's
	 * possible that the function result is used directly as a sort key or in
	 * other places where we expect exprCollation() to tell the truth.
	 */
	if (OidIsValid(result_collid))
	{
		Oid			exprcoll = exprCollation(newexpr);

		if (OidIsValid(exprcoll) && exprcoll != result_collid)
		{
			CollateExpr *newnode = makeNode(CollateExpr);

			newnode->arg = (Expr *) newexpr;
			newnode->collOid = result_collid;
			newnode->location = -1;

			newexpr = (Node *) newnode;
		}
	}

	/*
	 * Since there is now no trace of the function in the plan tree, we must
	 * explicitly record the plan's dependency on the function.
	 */
	if (context->root)
		record_plan_function_dependency(context->root, funcid);

	/*
	 * Recursively try to simplify the modified expression.  Here we must add
	 * the current function to the context list of active functions.
	 */
	context->active_fns = lappend_oid(context->active_fns, funcid);
	newexpr = eval_const_expressions_mutator(newexpr, context);
	context->active_fns = list_delete_last(context->active_fns);

	error_context_stack = sqlerrcontext.previous;

	return (Expr *) newexpr;

	/* Here if func is not inlinable: release temp memory and return NULL */
fail:
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(mycxt);
	error_context_stack = sqlerrcontext.previous;

	return NULL;
}

/*
 * Replace Param nodes by appropriate actual parameters
 */
static Node *
substitute_actual_parameters(Node *expr, int nargs, List *args,
							 int *usecounts)
{
	substitute_actual_parameters_context context;

	context.nargs = nargs;
	context.args = args;
	context.usecounts = usecounts;

	return substitute_actual_parameters_mutator(expr, &context);
}

static Node *
substitute_actual_parameters_mutator(Node *node,
									 substitute_actual_parameters_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind != PARAM_EXTERN)
			elog(ERROR, "unexpected paramkind: %d", (int) param->paramkind);
		if (param->paramid <= 0 || param->paramid > context->nargs)
			elog(ERROR, "invalid paramid: %d", param->paramid);

		/* Count usage of parameter */
		context->usecounts[param->paramid - 1]++;

		/* Select the appropriate actual arg and replace the Param with it */
		/* We don't need to copy at this time (it'll get done later) */
		return list_nth(context->args, param->paramid - 1);
	}
	return expression_tree_mutator(node, substitute_actual_parameters_mutator, context);
}

/*
 * error context callback to let us supply a call-stack traceback
 */
static void
sql_inline_error_callback(void *arg)
{
	inline_error_callback_arg *callback_arg = (inline_error_callback_arg *) arg;
	int			syntaxerrposition;

	/* If it's a syntax error, convert to internal syntax error report */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(callback_arg->prosrc);
	}

	errcontext("SQL function \"%s\" during inlining", callback_arg->proname);
}

/*
 * evaluate_expr: pre-evaluate a constant expression
 *
 * We use the executor's routine ExecEvalExpr() to avoid duplication of
 * code and ensure we get the same result as the executor would get.
 */
Expr *
evaluate_expr(Expr *expr, Oid result_type, int32 result_typmod,
			  Oid result_collation)
{
	EState	   *estate;
	ExprState  *exprstate;
	MemoryContext oldcontext;
	Datum		const_val;
	bool		const_is_null;
	int16		resultTypLen;
	bool		resultTypByVal;

	/*
	 * To use the executor, we need an EState.
	 */
	estate = CreateExecutorState();

	/* We can use the estate's working context to avoid memory leaks. */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* Make sure any opfuncids are filled in. */
	fix_opfuncids((Node *) expr);

	/*
	 * Prepare expr for execution.  (Note: we can't use ExecPrepareExpr
	 * because it'd result in recursively invoking eval_const_expressions.)
	 */
	exprstate = ExecInitExpr(expr, NULL);

	/*
	 * And evaluate it.
	 *
	 * It is OK to use a default econtext because none of the ExecEvalExpr()
	 * code used in this situation will use econtext.  That might seem
	 * fortuitous, but it's not so unreasonable --- a constant expression does
	 * not depend on context, by definition, n'est ce pas?
	 */
	const_val = ExecEvalExprSwitchContext(exprstate,
										  GetPerTupleExprContext(estate),
										  &const_is_null);

	/* Get info needed about result datatype */
	get_typlenbyval(result_type, &resultTypLen, &resultTypByVal);

	/* Get back to outer memory context */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Must copy result out of sub-context used by expression eval.
	 *
	 * Also, if it's varlena, forcibly detoast it.  This protects us against
	 * storing TOAST pointers into plans that might outlive the referenced
	 * data.  (makeConst would handle detoasting anyway, but it's worth a few
	 * extra lines here so that we can do the copy and detoast in one step.)
	 */
	if (!const_is_null)
	{
		if (resultTypLen == -1)
			const_val = PointerGetDatum(PG_DETOAST_DATUM_COPY(const_val));
		else
			const_val = datumCopy(const_val, resultTypByVal, resultTypLen);
	}

	/* Release all the junk we just created */
	FreeExecutorState(estate);

	/*
	 * Make the constant result node.
	 */
	return (Expr *) makeConst(result_type, result_typmod, result_collation,
							  resultTypLen,
							  const_val, const_is_null,
							  resultTypByVal);
}


/*
 * inline_set_returning_function
 *		Attempt to "inline" a set-returning function in the FROM clause.
 *
 * "rte" is an RTE_FUNCTION rangetable entry.  If it represents a call of a
 * set-returning SQL function that can safely be inlined, expand the function
 * and return the substitute Query structure.  Otherwise, return NULL.
 *
 * We assume that the RTE's expression has already been put through
 * eval_const_expressions(), which among other things will take care of
 * default arguments and named-argument notation.
 *
 * This has a good deal of similarity to inline_function(), but that's
 * for the non-set-returning case, and there are enough differences to
 * justify separate functions.
 */
Query *
inline_set_returning_function(PlannerInfo *root, RangeTblEntry *rte)
{
	RangeTblFunction *rtfunc;
	FuncExpr   *fexpr;
	Oid			func_oid;
	HeapTuple	func_tuple;
	Form_pg_proc funcform;
	char	   *src;
	Datum		tmp;
	bool		isNull;
	MemoryContext oldcxt;
	MemoryContext mycxt;
	inline_error_callback_arg callback_arg;
	ErrorContextCallback sqlerrcontext;
	SQLFunctionParseInfoPtr pinfo;
	TypeFuncClass functypclass;
	TupleDesc	rettupdesc;
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	Query	   *querytree;

	Assert(rte->rtekind == RTE_FUNCTION);

	/*
	 * It doesn't make a lot of sense for a SQL SRF to refer to itself in its
	 * own FROM clause, since that must cause infinite recursion at runtime.
	 * It will cause this code to recurse too, so check for stack overflow.
	 * (There's no need to do more.)
	 */
	check_stack_depth();

	/* Fail if the RTE has ORDINALITY - we don't implement that here. */
	if (rte->funcordinality)
		return NULL;

	/* Fail if RTE isn't a single, simple FuncExpr */
	if (list_length(rte->functions) != 1)
		return NULL;
	rtfunc = (RangeTblFunction *) linitial(rte->functions);

	if (!IsA(rtfunc->funcexpr, FuncExpr))
		return NULL;
	fexpr = (FuncExpr *) rtfunc->funcexpr;

	func_oid = fexpr->funcid;

	/*
	 * The function must be declared to return a set, else inlining would
	 * change the results if the contained SELECT didn't return exactly one
	 * row.
	 */
	if (!fexpr->funcretset)
		return NULL;

	/*
	 * Refuse to inline if the arguments contain any volatile functions or
	 * sub-selects.  Volatile functions are rejected because inlining may
	 * result in the arguments being evaluated multiple times, risking a
	 * change in behavior.  Sub-selects are rejected partly for implementation
	 * reasons (pushing them down another level might change their behavior)
	 * and partly because they're likely to be expensive and so multiple
	 * evaluation would be bad.
	 */
	if (contain_volatile_functions((Node *) fexpr->args) ||
		contain_subplans((Node *) fexpr->args))
		return NULL;

	/* Check permission to call function (fail later, if not) */
	if (object_aclcheck(ProcedureRelationId, func_oid, GetUserId(), ACL_EXECUTE) != ACLCHECK_OK)
		return NULL;

	/* Check whether a plugin wants to hook function entry/exit */
	if (FmgrHookIsNeeded(func_oid))
		return NULL;

	/*
	 * OK, let's take a look at the function's pg_proc entry.
	 */
	func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_oid));
	if (!HeapTupleIsValid(func_tuple))
		elog(ERROR, "cache lookup failed for function %u", func_oid);
	funcform = (Form_pg_proc) GETSTRUCT(func_tuple);

	/*
	 * Forget it if the function is not SQL-language or has other showstopper
	 * properties.  In particular it mustn't be declared STRICT, since we
	 * couldn't enforce that.  It also mustn't be VOLATILE, because that is
	 * supposed to cause it to be executed with its own snapshot, rather than
	 * sharing the snapshot of the calling query.  We also disallow returning
	 * SETOF VOID, because inlining would result in exposing the actual result
	 * of the function's last SELECT, which should not happen in that case.
	 * (Rechecking prokind, proretset, and pronargs is just paranoia.)
	 */
	if (funcform->prolang != SQLlanguageId ||
		funcform->prokind != PROKIND_FUNCTION ||
		funcform->proisstrict ||
		funcform->provolatile == PROVOLATILE_VOLATILE ||
		funcform->prorettype == VOIDOID ||
		funcform->prosecdef ||
		!funcform->proretset ||
		list_length(fexpr->args) != funcform->pronargs ||
		!heap_attisnull(func_tuple, Anum_pg_proc_proconfig, NULL))
	{
		ReleaseSysCache(func_tuple);
		return NULL;
	}

	/*
	 * Make a temporary memory context, so that we don't leak all the stuff
	 * that parsing might create.
	 */
	mycxt = AllocSetContextCreate(CurrentMemoryContext,
								  "inline_set_returning_function",
								  ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(mycxt);

	/* Fetch the function body */
	tmp = SysCacheGetAttrNotNull(PROCOID, func_tuple, Anum_pg_proc_prosrc);
	src = TextDatumGetCString(tmp);

	/*
	 * Setup error traceback support for ereport().  This is so that we can
	 * finger the function that bad information came from.
	 */
	callback_arg.proname = NameStr(funcform->proname);
	callback_arg.prosrc = src;

	sqlerrcontext.callback = sql_inline_error_callback;
	sqlerrcontext.arg = &callback_arg;
	sqlerrcontext.previous = error_context_stack;
	error_context_stack = &sqlerrcontext;

	/* If we have prosqlbody, pay attention to that not prosrc */
	tmp = SysCacheGetAttr(PROCOID,
						  func_tuple,
						  Anum_pg_proc_prosqlbody,
						  &isNull);
	if (!isNull)
	{
		Node	   *n;

		n = stringToNode(TextDatumGetCString(tmp));
		if (IsA(n, List))
			querytree_list = linitial_node(List, castNode(List, n));
		else
			querytree_list = list_make1(n);
		if (list_length(querytree_list) != 1)
			goto fail;
		querytree = linitial(querytree_list);

		/* Acquire necessary locks, then apply rewriter. */
		AcquireRewriteLocks(querytree, true, false);
		querytree_list = pg_rewrite_query(querytree);
		if (list_length(querytree_list) != 1)
			goto fail;
		querytree = linitial(querytree_list);
	}
	else
	{
		/*
		 * Set up to handle parameters while parsing the function body.  We
		 * can use the FuncExpr just created as the input for
		 * prepare_sql_fn_parse_info.
		 */
		pinfo = prepare_sql_fn_parse_info(func_tuple,
										  (Node *) fexpr,
										  fexpr->inputcollid);

		/*
		 * Parse, analyze, and rewrite (unlike inline_function(), we can't
		 * skip rewriting here).  We can fail as soon as we find more than one
		 * query, though.
		 */
		raw_parsetree_list = pg_parse_query(src);
		if (list_length(raw_parsetree_list) != 1)
			goto fail;

		querytree_list = pg_analyze_and_rewrite_withcb(linitial(raw_parsetree_list),
													   src,
													   (ParserSetupHook) sql_fn_parser_setup,
													   pinfo, NULL);
		if (list_length(querytree_list) != 1)
			goto fail;
		querytree = linitial(querytree_list);
	}

	/*
	 * Also resolve the actual function result tupdesc, if composite.  If we
	 * have a coldeflist, believe that; otherwise use get_expr_result_type.
	 * (This logic should match ExecInitFunctionScan.)
	 */
	if (rtfunc->funccolnames != NIL)
	{
		functypclass = TYPEFUNC_RECORD;
		rettupdesc = BuildDescFromLists(rtfunc->funccolnames,
										rtfunc->funccoltypes,
										rtfunc->funccoltypmods,
										rtfunc->funccolcollations);
	}
	else
		functypclass = get_expr_result_type((Node *) fexpr, NULL, &rettupdesc);

	/*
	 * The single command must be a plain SELECT.
	 */
	if (!IsA(querytree, Query) ||
		querytree->commandType != CMD_SELECT)
		goto fail;

	/*
	 * Make sure the function (still) returns what it's declared to.  This
	 * will raise an error if wrong, but that's okay since the function would
	 * fail at runtime anyway.  Note that check_sql_fn_retval will also insert
	 * coercions if needed to make the tlist expression(s) match the declared
	 * type of the function.  We also ask it to insert dummy NULL columns for
	 * any dropped columns in rettupdesc, so that the elements of the modified
	 * tlist match up to the attribute numbers.
	 *
	 * If the function returns a composite type, don't inline unless the check
	 * shows it's returning a whole tuple result; otherwise what it's
	 * returning is a single composite column which is not what we need.
	 */
	if (!check_sql_fn_retval(list_make1(querytree_list),
							 fexpr->funcresulttype, rettupdesc,
							 funcform->prokind,
							 true) &&
		(functypclass == TYPEFUNC_COMPOSITE ||
		 functypclass == TYPEFUNC_COMPOSITE_DOMAIN ||
		 functypclass == TYPEFUNC_RECORD))
		goto fail;				/* reject not-whole-tuple-result cases */

	/*
	 * check_sql_fn_retval might've inserted a projection step, but that's
	 * fine; just make sure we use the upper Query.
	 */
	querytree = linitial_node(Query, querytree_list);

	/*
	 * Looks good --- substitute parameters into the query.
	 */
	querytree = substitute_actual_srf_parameters(querytree,
												 funcform->pronargs,
												 fexpr->args);

	/*
	 * Copy the modified query out of the temporary memory context, and clean
	 * up.
	 */
	MemoryContextSwitchTo(oldcxt);

	querytree = copyObject(querytree);

	MemoryContextDelete(mycxt);
	error_context_stack = sqlerrcontext.previous;
	ReleaseSysCache(func_tuple);

	/*
	 * We don't have to fix collations here because the upper query is already
	 * parsed, ie, the collations in the RTE are what count.
	 */

	/*
	 * Since there is now no trace of the function in the plan tree, we must
	 * explicitly record the plan's dependency on the function.
	 */
	record_plan_function_dependency(root, func_oid);

	/*
	 * We must also notice if the inserted query adds a dependency on the
	 * calling role due to RLS quals.
	 */
	if (querytree->hasRowSecurity)
		root->glob->dependsOnRole = true;

	return querytree;

	/* Here if func is not inlinable: release temp memory and return NULL */
fail:
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(mycxt);
	error_context_stack = sqlerrcontext.previous;
	ReleaseSysCache(func_tuple);

	return NULL;
}

/*
 * Replace Param nodes by appropriate actual parameters
 *
 * This is just enough different from substitute_actual_parameters()
 * that it needs its own code.
 */
static Query *
substitute_actual_srf_parameters(Query *expr, int nargs, List *args)
{
	substitute_actual_srf_parameters_context context;

	context.nargs = nargs;
	context.args = args;
	context.sublevels_up = 1;

	return query_tree_mutator(expr,
							  substitute_actual_srf_parameters_mutator,
							  &context,
							  0);
}

static Node *
substitute_actual_srf_parameters_mutator(Node *node,
										 substitute_actual_srf_parameters_context *context)
{
	Node	   *result;

	if (node == NULL)
		return NULL;
	if (IsA(node, Query))
	{
		context->sublevels_up++;
		result = (Node *) query_tree_mutator((Query *) node,
											 substitute_actual_srf_parameters_mutator,
											 context,
											 0);
		context->sublevels_up--;
		return result;
	}
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN)
		{
			if (param->paramid <= 0 || param->paramid > context->nargs)
				elog(ERROR, "invalid paramid: %d", param->paramid);

			/*
			 * Since the parameter is being inserted into a subquery, we must
			 * adjust levels.
			 */
			result = copyObject(list_nth(context->args, param->paramid - 1));
			IncrementVarSublevelsUp(result, context->sublevels_up, 0);
			return result;
		}
	}
	return expression_tree_mutator(node,
								   substitute_actual_srf_parameters_mutator,
								   context);
}

/*
 * pull_paramids
 *		Returns a Bitmapset containing the paramids of all Params in 'expr'.
 */
Bitmapset *
pull_paramids(Expr *expr)
{
	Bitmapset  *result = NULL;

	(void) pull_paramids_walker((Node *) expr, &result);

	return result;
}

static bool
pull_paramids_walker(Node *node, Bitmapset **context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		*context = bms_add_member(*context, param->paramid);
		return false;
	}
	return expression_tree_walker(node, pull_paramids_walker, context);
}

/*
 * Build ScalarArrayOpExpr on top of 'exprs.' 'haveNonConst' indicates
 * whether at least one of the expressions is not Const.  When it's false,
 * the array constant is built directly; otherwise, we have to build a child
 * ArrayExpr. The 'exprs' list gets freed if not directly used in the output
 * expression tree.
 */
ScalarArrayOpExpr *
make_SAOP_expr(Oid oper, Node *leftexpr, Oid coltype, Oid arraycollid,
			   Oid inputcollid, List *exprs, bool haveNonConst)
{
	Node	   *arrayNode = NULL;
	ScalarArrayOpExpr *saopexpr = NULL;
	Oid			arraytype = get_array_type(coltype);

	if (!OidIsValid(arraytype))
		return NULL;

	/*
	 * Assemble an array from the list of constants.  It seems more profitable
	 * to build a const array.  But in the presence of other nodes, we don't
	 * have a specific value here and must employ an ArrayExpr instead.
	 */
	if (haveNonConst)
	{
		ArrayExpr  *arrayExpr = makeNode(ArrayExpr);

		/* array_collid will be set by parse_collate.c */
		arrayExpr->element_typeid = coltype;
		arrayExpr->array_typeid = arraytype;
		arrayExpr->multidims = false;
		arrayExpr->elements = exprs;
		arrayExpr->location = -1;

		arrayNode = (Node *) arrayExpr;
	}
	else
	{
		int16		typlen;
		bool		typbyval;
		char		typalign;
		Datum	   *elems;
		bool	   *nulls;
		int			i = 0;
		ArrayType  *arrayConst;
		int			dims[1] = {list_length(exprs)};
		int			lbs[1] = {1};

		get_typlenbyvalalign(coltype, &typlen, &typbyval, &typalign);

		elems = (Datum *) palloc(sizeof(Datum) * list_length(exprs));
		nulls = (bool *) palloc(sizeof(bool) * list_length(exprs));
		foreach_node(Const, value, exprs)
		{
			elems[i] = value->constvalue;
			nulls[i++] = value->constisnull;
		}

		arrayConst = construct_md_array(elems, nulls, 1, dims, lbs,
										coltype, typlen, typbyval, typalign);
		arrayNode = (Node *) makeConst(arraytype, -1, arraycollid,
									   -1, PointerGetDatum(arrayConst),
									   false, false);

		pfree(elems);
		pfree(nulls);
		list_free(exprs);
	}

	/* Build the SAOP expression node */
	saopexpr = makeNode(ScalarArrayOpExpr);
	saopexpr->opno = oper;
	saopexpr->opfuncid = get_opcode(oper);
	saopexpr->hashfuncid = InvalidOid;
	saopexpr->negfuncid = InvalidOid;
	saopexpr->useOr = true;
	saopexpr->inputcollid = inputcollid;
	saopexpr->args = list_make2(leftexpr, arrayNode);
	saopexpr->location = -1;

	return saopexpr;
}
