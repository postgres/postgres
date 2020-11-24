/*-------------------------------------------------------------------------
 *
 * parse_func.c
 *		handle function calls in parser
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_func.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* Possible error codes from LookupFuncNameInternal */
typedef enum
{
	FUNCLOOKUP_NOSUCHFUNC,
	FUNCLOOKUP_AMBIGUOUS
} FuncLookupError;

static void unify_hypothetical_args(ParseState *pstate,
									List *fargs, int numAggregatedArgs,
									Oid *actual_arg_types, Oid *declared_arg_types);
static Oid	FuncNameAsType(List *funcname);
static Node *ParseComplexProjection(ParseState *pstate, const char *funcname,
									Node *first_arg, int location);
static Oid	LookupFuncNameInternal(List *funcname, int nargs,
								   const Oid *argtypes,
								   bool missing_ok, FuncLookupError *lookupError);


/*
 *	Parse a function call
 *
 *	For historical reasons, Postgres tries to treat the notations tab.col
 *	and col(tab) as equivalent: if a single-argument function call has an
 *	argument of complex type and the (unqualified) function name matches
 *	any attribute of the type, we can interpret it as a column projection.
 *	Conversely a function of a single complex-type argument can be written
 *	like a column reference, allowing functions to act like computed columns.
 *
 *	If both interpretations are possible, we prefer the one matching the
 *	syntactic form, but otherwise the form does not matter.
 *
 *	Hence, both cases come through here.  If fn is null, we're dealing with
 *	column syntax not function syntax.  In the function-syntax case,
 *	the FuncCall struct is needed to carry various decoration that applies
 *	to aggregate and window functions.
 *
 *	Also, when fn is null, we return NULL on failure rather than
 *	reporting a no-such-function error.
 *
 *	The argument expressions (in fargs) must have been transformed
 *	already.  However, nothing in *fn has been transformed.
 *
 *	last_srf should be a copy of pstate->p_last_srf from just before we
 *	started transforming fargs.  If the caller knows that fargs couldn't
 *	contain any SRF calls, last_srf can just be pstate->p_last_srf.
 *
 *	proc_call is true if we are considering a CALL statement, so that the
 *	name must resolve to a procedure name, not anything else.
 */
Node *
ParseFuncOrColumn(ParseState *pstate, List *funcname, List *fargs,
				  Node *last_srf, FuncCall *fn, bool proc_call, int location)
{
	bool		is_column = (fn == NULL);
	List	   *agg_order = (fn ? fn->agg_order : NIL);
	Expr	   *agg_filter = NULL;
	WindowDef  *over = (fn ? fn->over : NULL);
	bool		agg_within_group = (fn ? fn->agg_within_group : false);
	bool		agg_star = (fn ? fn->agg_star : false);
	bool		agg_distinct = (fn ? fn->agg_distinct : false);
	bool		func_variadic = (fn ? fn->func_variadic : false);
	CoercionForm funcformat = (fn ? fn->funcformat : COERCE_EXPLICIT_CALL);
	bool		could_be_projection;
	Oid			rettype;
	Oid			funcid;
	ListCell   *l;
	Node	   *first_arg = NULL;
	int			nargs;
	int			nargsplusdefs;
	Oid			actual_arg_types[FUNC_MAX_ARGS];
	Oid		   *declared_arg_types;
	List	   *argnames;
	List	   *argdefaults;
	Node	   *retval;
	bool		retset;
	int			nvargs;
	Oid			vatype;
	FuncDetailCode fdresult;
	char		aggkind = 0;
	ParseCallbackState pcbstate;

	/*
	 * If there's an aggregate filter, transform it using transformWhereClause
	 */
	if (fn && fn->agg_filter != NULL)
		agg_filter = (Expr *) transformWhereClause(pstate, fn->agg_filter,
												   EXPR_KIND_FILTER,
												   "FILTER");

	/*
	 * Most of the rest of the parser just assumes that functions do not have
	 * more than FUNC_MAX_ARGS parameters.  We have to test here to protect
	 * against array overruns, etc.  Of course, this may not be a function,
	 * but the test doesn't hurt.
	 */
	if (list_length(fargs) > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a function",
							   "cannot pass more than %d arguments to a function",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS),
				 parser_errposition(pstate, location)));

	/*
	 * Extract arg type info in preparation for function lookup.
	 *
	 * If any arguments are Param markers of type VOID, we discard them from
	 * the parameter list. This is a hack to allow the JDBC driver to not have
	 * to distinguish "input" and "output" parameter symbols while parsing
	 * function-call constructs.  Don't do this if dealing with column syntax,
	 * nor if we had WITHIN GROUP (because in that case it's critical to keep
	 * the argument count unchanged).
	 */
	nargs = 0;
	foreach(l, fargs)
	{
		Node	   *arg = lfirst(l);
		Oid			argtype = exprType(arg);

		if (argtype == VOIDOID && IsA(arg, Param) &&
			!is_column && !agg_within_group)
		{
			fargs = foreach_delete_current(fargs, l);
			continue;
		}

		actual_arg_types[nargs++] = argtype;
	}

	/*
	 * Check for named arguments; if there are any, build a list of names.
	 *
	 * We allow mixed notation (some named and some not), but only with all
	 * the named parameters after all the unnamed ones.  So the name list
	 * corresponds to the last N actual parameters and we don't need any extra
	 * bookkeeping to match things up.
	 */
	argnames = NIL;
	foreach(l, fargs)
	{
		Node	   *arg = lfirst(l);

		if (IsA(arg, NamedArgExpr))
		{
			NamedArgExpr *na = (NamedArgExpr *) arg;
			ListCell   *lc;

			/* Reject duplicate arg names */
			foreach(lc, argnames)
			{
				if (strcmp(na->name, (char *) lfirst(lc)) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("argument name \"%s\" used more than once",
									na->name),
							 parser_errposition(pstate, na->location)));
			}
			argnames = lappend(argnames, na->name);
		}
		else
		{
			if (argnames != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("positional argument cannot follow named argument"),
						 parser_errposition(pstate, exprLocation(arg))));
		}
	}

	if (fargs)
	{
		first_arg = linitial(fargs);
		Assert(first_arg != NULL);
	}

	/*
	 * Decide whether it's legitimate to consider the construct to be a column
	 * projection.  For that, there has to be a single argument of complex
	 * type, the function name must not be qualified, and there cannot be any
	 * syntactic decoration that'd require it to be a function (such as
	 * aggregate or variadic decoration, or named arguments).
	 */
	could_be_projection = (nargs == 1 && !proc_call &&
						   agg_order == NIL && agg_filter == NULL &&
						   !agg_star && !agg_distinct && over == NULL &&
						   !func_variadic && argnames == NIL &&
						   funcformat == COERCE_EXPLICIT_CALL &&
						   list_length(funcname) == 1 &&
						   (actual_arg_types[0] == RECORDOID ||
							ISCOMPLEX(actual_arg_types[0])));

	/*
	 * If it's column syntax, check for column projection case first.
	 */
	if (could_be_projection && is_column)
	{
		retval = ParseComplexProjection(pstate,
										strVal(linitial(funcname)),
										first_arg,
										location);
		if (retval)
			return retval;

		/*
		 * If ParseComplexProjection doesn't recognize it as a projection,
		 * just press on.
		 */
	}

	/*
	 * func_get_detail looks up the function in the catalogs, does
	 * disambiguation for polymorphic functions, handles inheritance, and
	 * returns the funcid and type and set or singleton status of the
	 * function's return value.  It also returns the true argument types to
	 * the function.
	 *
	 * Note: for a named-notation or variadic function call, the reported
	 * "true" types aren't really what is in pg_proc: the types are reordered
	 * to match the given argument order of named arguments, and a variadic
	 * argument is replaced by a suitable number of copies of its element
	 * type.  We'll fix up the variadic case below.  We may also have to deal
	 * with default arguments.
	 */

	setup_parser_errposition_callback(&pcbstate, pstate, location);

	fdresult = func_get_detail(funcname, fargs, argnames, nargs,
							   actual_arg_types,
							   !func_variadic, true,
							   &funcid, &rettype, &retset,
							   &nvargs, &vatype,
							   &declared_arg_types, &argdefaults);

	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * Check for various wrong-kind-of-routine cases.
	 */

	/* If this is a CALL, reject things that aren't procedures */
	if (proc_call &&
		(fdresult == FUNCDETAIL_NORMAL ||
		 fdresult == FUNCDETAIL_AGGREGATE ||
		 fdresult == FUNCDETAIL_WINDOWFUNC ||
		 fdresult == FUNCDETAIL_COERCION))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a procedure",
						func_signature_string(funcname, nargs,
											  argnames,
											  actual_arg_types)),
				 errhint("To call a function, use SELECT."),
				 parser_errposition(pstate, location)));
	/* Conversely, if not a CALL, reject procedures */
	if (fdresult == FUNCDETAIL_PROCEDURE && !proc_call)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is a procedure",
						func_signature_string(funcname, nargs,
											  argnames,
											  actual_arg_types)),
				 errhint("To call a procedure, use CALL."),
				 parser_errposition(pstate, location)));

	if (fdresult == FUNCDETAIL_NORMAL ||
		fdresult == FUNCDETAIL_PROCEDURE ||
		fdresult == FUNCDETAIL_COERCION)
	{
		/*
		 * In these cases, complain if there was anything indicating it must
		 * be an aggregate or window function.
		 */
		if (agg_star)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("%s(*) specified, but %s is not an aggregate function",
							NameListToString(funcname),
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		if (agg_distinct)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("DISTINCT specified, but %s is not an aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		if (agg_within_group)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("WITHIN GROUP specified, but %s is not an aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		if (agg_order != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("ORDER BY specified, but %s is not an aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		if (agg_filter)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("FILTER specified, but %s is not an aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		if (over)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("OVER specified, but %s is not a window function nor an aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
	}

	/*
	 * So far so good, so do some fdresult-type-specific processing.
	 */
	if (fdresult == FUNCDETAIL_NORMAL || fdresult == FUNCDETAIL_PROCEDURE)
	{
		/* Nothing special to do for these cases. */
	}
	else if (fdresult == FUNCDETAIL_AGGREGATE)
	{
		/*
		 * It's an aggregate; fetch needed info from the pg_aggregate entry.
		 */
		HeapTuple	tup;
		Form_pg_aggregate classForm;
		int			catDirectArgs;

		tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(funcid));
		if (!HeapTupleIsValid(tup)) /* should not happen */
			elog(ERROR, "cache lookup failed for aggregate %u", funcid);
		classForm = (Form_pg_aggregate) GETSTRUCT(tup);
		aggkind = classForm->aggkind;
		catDirectArgs = classForm->aggnumdirectargs;
		ReleaseSysCache(tup);

		/* Now check various disallowed cases. */
		if (AGGKIND_IS_ORDERED_SET(aggkind))
		{
			int			numAggregatedArgs;
			int			numDirectArgs;

			if (!agg_within_group)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("WITHIN GROUP is required for ordered-set aggregate %s",
								NameListToString(funcname)),
						 parser_errposition(pstate, location)));
			if (over)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("OVER is not supported for ordered-set aggregate %s",
								NameListToString(funcname)),
						 parser_errposition(pstate, location)));
			/* gram.y rejects DISTINCT + WITHIN GROUP */
			Assert(!agg_distinct);
			/* gram.y rejects VARIADIC + WITHIN GROUP */
			Assert(!func_variadic);

			/*
			 * Since func_get_detail was working with an undifferentiated list
			 * of arguments, it might have selected an aggregate that doesn't
			 * really match because it requires a different division of direct
			 * and aggregated arguments.  Check that the number of direct
			 * arguments is actually OK; if not, throw an "undefined function"
			 * error, similarly to the case where a misplaced ORDER BY is used
			 * in a regular aggregate call.
			 */
			numAggregatedArgs = list_length(agg_order);
			numDirectArgs = nargs - numAggregatedArgs;
			Assert(numDirectArgs >= 0);

			if (!OidIsValid(vatype))
			{
				/* Test is simple if aggregate isn't variadic */
				if (numDirectArgs != catDirectArgs)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_FUNCTION),
							 errmsg("function %s does not exist",
									func_signature_string(funcname, nargs,
														  argnames,
														  actual_arg_types)),
							 errhint("There is an ordered-set aggregate %s, but it requires %d direct arguments, not %d.",
									 NameListToString(funcname),
									 catDirectArgs, numDirectArgs),
							 parser_errposition(pstate, location)));
			}
			else
			{
				/*
				 * If it's variadic, we have two cases depending on whether
				 * the agg was "... ORDER BY VARIADIC" or "..., VARIADIC ORDER
				 * BY VARIADIC".  It's the latter if catDirectArgs equals
				 * pronargs; to save a catalog lookup, we reverse-engineer
				 * pronargs from the info we got from func_get_detail.
				 */
				int			pronargs;

				pronargs = nargs;
				if (nvargs > 1)
					pronargs -= nvargs - 1;
				if (catDirectArgs < pronargs)
				{
					/* VARIADIC isn't part of direct args, so still easy */
					if (numDirectArgs != catDirectArgs)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_FUNCTION),
								 errmsg("function %s does not exist",
										func_signature_string(funcname, nargs,
															  argnames,
															  actual_arg_types)),
								 errhint("There is an ordered-set aggregate %s, but it requires %d direct arguments, not %d.",
										 NameListToString(funcname),
										 catDirectArgs, numDirectArgs),
								 parser_errposition(pstate, location)));
				}
				else
				{
					/*
					 * Both direct and aggregated args were declared variadic.
					 * For a standard ordered-set aggregate, it's okay as long
					 * as there aren't too few direct args.  For a
					 * hypothetical-set aggregate, we assume that the
					 * hypothetical arguments are those that matched the
					 * variadic parameter; there must be just as many of them
					 * as there are aggregated arguments.
					 */
					if (aggkind == AGGKIND_HYPOTHETICAL)
					{
						if (nvargs != 2 * numAggregatedArgs)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("function %s does not exist",
											func_signature_string(funcname, nargs,
																  argnames,
																  actual_arg_types)),
									 errhint("To use the hypothetical-set aggregate %s, the number of hypothetical direct arguments (here %d) must match the number of ordering columns (here %d).",
											 NameListToString(funcname),
											 nvargs - numAggregatedArgs, numAggregatedArgs),
									 parser_errposition(pstate, location)));
					}
					else
					{
						if (nvargs <= numAggregatedArgs)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("function %s does not exist",
											func_signature_string(funcname, nargs,
																  argnames,
																  actual_arg_types)),
									 errhint("There is an ordered-set aggregate %s, but it requires at least %d direct arguments.",
											 NameListToString(funcname),
											 catDirectArgs),
									 parser_errposition(pstate, location)));
					}
				}
			}

			/* Check type matching of hypothetical arguments */
			if (aggkind == AGGKIND_HYPOTHETICAL)
				unify_hypothetical_args(pstate, fargs, numAggregatedArgs,
										actual_arg_types, declared_arg_types);
		}
		else
		{
			/* Normal aggregate, so it can't have WITHIN GROUP */
			if (agg_within_group)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("%s is not an ordered-set aggregate, so it cannot have WITHIN GROUP",
								NameListToString(funcname)),
						 parser_errposition(pstate, location)));
		}
	}
	else if (fdresult == FUNCDETAIL_WINDOWFUNC)
	{
		/*
		 * True window functions must be called with a window definition.
		 */
		if (!over)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("window function %s requires an OVER clause",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		/* And, per spec, WITHIN GROUP isn't allowed */
		if (agg_within_group)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("window function %s cannot have WITHIN GROUP",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
	}
	else if (fdresult == FUNCDETAIL_COERCION)
	{
		/*
		 * We interpreted it as a type coercion. coerce_type can handle these
		 * cases, so why duplicate code...
		 */
		return coerce_type(pstate, linitial(fargs),
						   actual_arg_types[0], rettype, -1,
						   COERCION_EXPLICIT, COERCE_EXPLICIT_CALL, location);
	}
	else if (fdresult == FUNCDETAIL_MULTIPLE)
	{
		/*
		 * We found multiple possible functional matches.  If we are dealing
		 * with attribute notation, return failure, letting the caller report
		 * "no such column" (we already determined there wasn't one).  If
		 * dealing with function notation, report "ambiguous function",
		 * regardless of whether there's also a column by this name.
		 */
		if (is_column)
			return NULL;

		if (proc_call)
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("procedure %s is not unique",
							func_signature_string(funcname, nargs, argnames,
												  actual_arg_types)),
					 errhint("Could not choose a best candidate procedure. "
							 "You might need to add explicit type casts."),
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("function %s is not unique",
							func_signature_string(funcname, nargs, argnames,
												  actual_arg_types)),
					 errhint("Could not choose a best candidate function. "
							 "You might need to add explicit type casts."),
					 parser_errposition(pstate, location)));
	}
	else
	{
		/*
		 * Not found as a function.  If we are dealing with attribute
		 * notation, return failure, letting the caller report "no such
		 * column" (we already determined there wasn't one).
		 */
		if (is_column)
			return NULL;

		/*
		 * Check for column projection interpretation, since we didn't before.
		 */
		if (could_be_projection)
		{
			retval = ParseComplexProjection(pstate,
											strVal(linitial(funcname)),
											first_arg,
											location);
			if (retval)
				return retval;
		}

		/*
		 * No function, and no column either.  Since we're dealing with
		 * function notation, report "function does not exist".
		 */
		if (list_length(agg_order) > 1 && !agg_within_group)
		{
			/* It's agg(x, ORDER BY y,z) ... perhaps misplaced ORDER BY */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(funcname, nargs, argnames,
												  actual_arg_types)),
					 errhint("No aggregate function matches the given name and argument types. "
							 "Perhaps you misplaced ORDER BY; ORDER BY must appear "
							 "after all regular arguments of the aggregate."),
					 parser_errposition(pstate, location)));
		}
		else if (proc_call)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("procedure %s does not exist",
							func_signature_string(funcname, nargs, argnames,
												  actual_arg_types)),
					 errhint("No procedure matches the given name and argument types. "
							 "You might need to add explicit type casts."),
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(funcname, nargs, argnames,
												  actual_arg_types)),
					 errhint("No function matches the given name and argument types. "
							 "You might need to add explicit type casts."),
					 parser_errposition(pstate, location)));
	}

	/*
	 * If there are default arguments, we have to include their types in
	 * actual_arg_types for the purpose of checking generic type consistency.
	 * However, we do NOT put them into the generated parse node, because
	 * their actual values might change before the query gets run.  The
	 * planner has to insert the up-to-date values at plan time.
	 */
	nargsplusdefs = nargs;
	foreach(l, argdefaults)
	{
		Node	   *expr = (Node *) lfirst(l);

		/* probably shouldn't happen ... */
		if (nargsplusdefs >= FUNC_MAX_ARGS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg_plural("cannot pass more than %d argument to a function",
								   "cannot pass more than %d arguments to a function",
								   FUNC_MAX_ARGS,
								   FUNC_MAX_ARGS),
					 parser_errposition(pstate, location)));

		actual_arg_types[nargsplusdefs++] = exprType(expr);
	}

	/*
	 * enforce consistency with polymorphic argument and return types,
	 * possibly adjusting return type or declared_arg_types (which will be
	 * used as the cast destination by make_fn_arguments)
	 */
	rettype = enforce_generic_type_consistency(actual_arg_types,
											   declared_arg_types,
											   nargsplusdefs,
											   rettype,
											   false);

	/* perform the necessary typecasting of arguments */
	make_fn_arguments(pstate, fargs, actual_arg_types, declared_arg_types);

	/*
	 * If the function isn't actually variadic, forget any VARIADIC decoration
	 * on the call.  (Perhaps we should throw an error instead, but
	 * historically we've allowed people to write that.)
	 */
	if (!OidIsValid(vatype))
	{
		Assert(nvargs == 0);
		func_variadic = false;
	}

	/*
	 * If it's a variadic function call, transform the last nvargs arguments
	 * into an array --- unless it's an "any" variadic.
	 */
	if (nvargs > 0 && vatype != ANYOID)
	{
		ArrayExpr  *newa = makeNode(ArrayExpr);
		int			non_var_args = nargs - nvargs;
		List	   *vargs;

		Assert(non_var_args >= 0);
		vargs = list_copy_tail(fargs, non_var_args);
		fargs = list_truncate(fargs, non_var_args);

		newa->elements = vargs;
		/* assume all the variadic arguments were coerced to the same type */
		newa->element_typeid = exprType((Node *) linitial(vargs));
		newa->array_typeid = get_array_type(newa->element_typeid);
		if (!OidIsValid(newa->array_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("could not find array type for data type %s",
							format_type_be(newa->element_typeid)),
					 parser_errposition(pstate, exprLocation((Node *) vargs))));
		/* array_collid will be set by parse_collate.c */
		newa->multidims = false;
		newa->location = exprLocation((Node *) vargs);

		fargs = lappend(fargs, newa);

		/* We could not have had VARIADIC marking before ... */
		Assert(!func_variadic);
		/* ... but now, it's a VARIADIC call */
		func_variadic = true;
	}

	/*
	 * If an "any" variadic is called with explicit VARIADIC marking, insist
	 * that the variadic parameter be of some array type.
	 */
	if (nargs > 0 && vatype == ANYOID && func_variadic)
	{
		Oid			va_arr_typid = actual_arg_types[nargs - 1];

		if (!OidIsValid(get_base_element_type(va_arr_typid)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("VARIADIC argument must be an array"),
					 parser_errposition(pstate,
										exprLocation((Node *) llast(fargs)))));
	}

	/* if it returns a set, check that's OK */
	if (retset)
		check_srf_call_placement(pstate, last_srf, location);

	/* build the appropriate output structure */
	if (fdresult == FUNCDETAIL_NORMAL || fdresult == FUNCDETAIL_PROCEDURE)
	{
		FuncExpr   *funcexpr = makeNode(FuncExpr);

		funcexpr->funcid = funcid;
		funcexpr->funcresulttype = rettype;
		funcexpr->funcretset = retset;
		funcexpr->funcvariadic = func_variadic;
		funcexpr->funcformat = funcformat;
		/* funccollid and inputcollid will be set by parse_collate.c */
		funcexpr->args = fargs;
		funcexpr->location = location;

		retval = (Node *) funcexpr;
	}
	else if (fdresult == FUNCDETAIL_AGGREGATE && !over)
	{
		/* aggregate function */
		Aggref	   *aggref = makeNode(Aggref);

		aggref->aggfnoid = funcid;
		aggref->aggtype = rettype;
		/* aggcollid and inputcollid will be set by parse_collate.c */
		aggref->aggtranstype = InvalidOid;	/* will be set by planner */
		/* aggargtypes will be set by transformAggregateCall */
		/* aggdirectargs and args will be set by transformAggregateCall */
		/* aggorder and aggdistinct will be set by transformAggregateCall */
		aggref->aggfilter = agg_filter;
		aggref->aggstar = agg_star;
		aggref->aggvariadic = func_variadic;
		aggref->aggkind = aggkind;
		/* agglevelsup will be set by transformAggregateCall */
		aggref->aggsplit = AGGSPLIT_SIMPLE; /* planner might change this */
		aggref->aggno = -1;		/* planner will set aggno and aggtransno */
		aggref->aggtransno = -1;
		aggref->location = location;

		/*
		 * Reject attempt to call a parameterless aggregate without (*)
		 * syntax.  This is mere pedantry but some folks insisted ...
		 */
		if (fargs == NIL && !agg_star && !agg_within_group)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("%s(*) must be used to call a parameterless aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));

		if (retset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregates cannot return sets"),
					 parser_errposition(pstate, location)));

		/*
		 * We might want to support named arguments later, but disallow it for
		 * now.  We'd need to figure out the parsed representation (should the
		 * NamedArgExprs go above or below the TargetEntry nodes?) and then
		 * teach the planner to reorder the list properly.  Or maybe we could
		 * make transformAggregateCall do that?  However, if you'd also like
		 * to allow default arguments for aggregates, we'd need to do it in
		 * planning to avoid semantic problems.
		 */
		if (argnames != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("aggregates cannot use named arguments"),
					 parser_errposition(pstate, location)));

		/* parse_agg.c does additional aggregate-specific processing */
		transformAggregateCall(pstate, aggref, fargs, agg_order, agg_distinct);

		retval = (Node *) aggref;
	}
	else
	{
		/* window function */
		WindowFunc *wfunc = makeNode(WindowFunc);

		Assert(over);			/* lack of this was checked above */
		Assert(!agg_within_group);	/* also checked above */

		wfunc->winfnoid = funcid;
		wfunc->wintype = rettype;
		/* wincollid and inputcollid will be set by parse_collate.c */
		wfunc->args = fargs;
		/* winref will be set by transformWindowFuncCall */
		wfunc->winstar = agg_star;
		wfunc->winagg = (fdresult == FUNCDETAIL_AGGREGATE);
		wfunc->aggfilter = agg_filter;
		wfunc->location = location;

		/*
		 * agg_star is allowed for aggregate functions but distinct isn't
		 */
		if (agg_distinct)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DISTINCT is not implemented for window functions"),
					 parser_errposition(pstate, location)));

		/*
		 * Reject attempt to call a parameterless aggregate without (*)
		 * syntax.  This is mere pedantry but some folks insisted ...
		 */
		if (wfunc->winagg && fargs == NIL && !agg_star)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("%s(*) must be used to call a parameterless aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));

		/*
		 * ordered aggs not allowed in windows yet
		 */
		if (agg_order != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("aggregate ORDER BY is not implemented for window functions"),
					 parser_errposition(pstate, location)));

		/*
		 * FILTER is not yet supported with true window functions
		 */
		if (!wfunc->winagg && agg_filter)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("FILTER is not implemented for non-aggregate window functions"),
					 parser_errposition(pstate, location)));

		/*
		 * Window functions can't either take or return sets
		 */
		if (pstate->p_last_srf != last_srf)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("window function calls cannot contain set-returning function calls"),
					 errhint("You might be able to move the set-returning function into a LATERAL FROM item."),
					 parser_errposition(pstate,
										exprLocation(pstate->p_last_srf))));

		if (retset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("window functions cannot return sets"),
					 parser_errposition(pstate, location)));

		/* parse_agg.c does additional window-func-specific processing */
		transformWindowFuncCall(pstate, wfunc, over);

		retval = (Node *) wfunc;
	}

	/* if it returns a set, remember it for error checks at higher levels */
	if (retset)
		pstate->p_last_srf = retval;

	return retval;
}


/* func_match_argtypes()
 *
 * Given a list of candidate functions (having the right name and number
 * of arguments) and an array of input datatype OIDs, produce a shortlist of
 * those candidates that actually accept the input datatypes (either exactly
 * or by coercion), and return the number of such candidates.
 *
 * Note that can_coerce_type will assume that UNKNOWN inputs are coercible to
 * anything, so candidates will not be eliminated on that basis.
 *
 * NB: okay to modify input list structure, as long as we find at least
 * one match.  If no match at all, the list must remain unmodified.
 */
int
func_match_argtypes(int nargs,
					Oid *input_typeids,
					FuncCandidateList raw_candidates,
					FuncCandidateList *candidates)	/* return value */
{
	FuncCandidateList current_candidate;
	FuncCandidateList next_candidate;
	int			ncandidates = 0;

	*candidates = NULL;

	for (current_candidate = raw_candidates;
		 current_candidate != NULL;
		 current_candidate = next_candidate)
	{
		next_candidate = current_candidate->next;
		if (can_coerce_type(nargs, input_typeids, current_candidate->args,
							COERCION_IMPLICIT))
		{
			current_candidate->next = *candidates;
			*candidates = current_candidate;
			ncandidates++;
		}
	}

	return ncandidates;
}								/* func_match_argtypes() */


/* func_select_candidate()
 *		Given the input argtype array and more than one candidate
 *		for the function, attempt to resolve the conflict.
 *
 * Returns the selected candidate if the conflict can be resolved,
 * otherwise returns NULL.
 *
 * Note that the caller has already determined that there is no candidate
 * exactly matching the input argtypes, and has pruned away any "candidates"
 * that aren't actually coercion-compatible with the input types.
 *
 * This is also used for resolving ambiguous operator references.  Formerly
 * parse_oper.c had its own, essentially duplicate code for the purpose.
 * The following comments (formerly in parse_oper.c) are kept to record some
 * of the history of these heuristics.
 *
 * OLD COMMENTS:
 *
 * This routine is new code, replacing binary_oper_select_candidate()
 * which dates from v4.2/v1.0.x days. It tries very hard to match up
 * operators with types, including allowing type coercions if necessary.
 * The important thing is that the code do as much as possible,
 * while _never_ doing the wrong thing, where "the wrong thing" would
 * be returning an operator when other better choices are available,
 * or returning an operator which is a non-intuitive possibility.
 * - thomas 1998-05-21
 *
 * The comments below came from binary_oper_select_candidate(), and
 * illustrate the issues and choices which are possible:
 * - thomas 1998-05-20
 *
 * current wisdom holds that the default operator should be one in which
 * both operands have the same type (there will only be one such
 * operator)
 *
 * 7.27.93 - I have decided not to do this; it's too hard to justify, and
 * it's easy enough to typecast explicitly - avi
 * [the rest of this routine was commented out since then - ay]
 *
 * 6/23/95 - I don't complete agree with avi. In particular, casting
 * floats is a pain for users. Whatever the rationale behind not doing
 * this is, I need the following special case to work.
 *
 * In the WHERE clause of a query, if a float is specified without
 * quotes, we treat it as float8. I added the float48* operators so
 * that we can operate on float4 and float8. But now we have more than
 * one matching operator if the right arg is unknown (eg. float
 * specified with quotes). This break some stuff in the regression
 * test where there are floats in quotes not properly casted. Below is
 * the solution. In addition to requiring the operator operates on the
 * same type for both operands [as in the code Avi originally
 * commented out], we also require that the operators be equivalent in
 * some sense. (see equivalentOpersAfterPromotion for details.)
 * - ay 6/95
 */
FuncCandidateList
func_select_candidate(int nargs,
					  Oid *input_typeids,
					  FuncCandidateList candidates)
{
	FuncCandidateList current_candidate,
				first_candidate,
				last_candidate;
	Oid		   *current_typeids;
	Oid			current_type;
	int			i;
	int			ncandidates;
	int			nbestMatch,
				nmatch,
				nunknowns;
	Oid			input_base_typeids[FUNC_MAX_ARGS];
	TYPCATEGORY slot_category[FUNC_MAX_ARGS],
				current_category;
	bool		current_is_preferred;
	bool		slot_has_preferred_type[FUNC_MAX_ARGS];
	bool		resolved_unknowns;

	/* protect local fixed-size arrays */
	if (nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a function",
							   "cannot pass more than %d arguments to a function",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));

	/*
	 * If any input types are domains, reduce them to their base types. This
	 * ensures that we will consider functions on the base type to be "exact
	 * matches" in the exact-match heuristic; it also makes it possible to do
	 * something useful with the type-category heuristics. Note that this
	 * makes it difficult, but not impossible, to use functions declared to
	 * take a domain as an input datatype.  Such a function will be selected
	 * over the base-type function only if it is an exact match at all
	 * argument positions, and so was already chosen by our caller.
	 *
	 * While we're at it, count the number of unknown-type arguments for use
	 * later.
	 */
	nunknowns = 0;
	for (i = 0; i < nargs; i++)
	{
		if (input_typeids[i] != UNKNOWNOID)
			input_base_typeids[i] = getBaseType(input_typeids[i]);
		else
		{
			/* no need to call getBaseType on UNKNOWNOID */
			input_base_typeids[i] = UNKNOWNOID;
			nunknowns++;
		}
	}

	/*
	 * Run through all candidates and keep those with the most matches on
	 * exact types. Keep all candidates if none match.
	 */
	ncandidates = 0;
	nbestMatch = 0;
	last_candidate = NULL;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeids = current_candidate->args;
		nmatch = 0;
		for (i = 0; i < nargs; i++)
		{
			if (input_base_typeids[i] != UNKNOWNOID &&
				current_typeids[i] == input_base_typeids[i])
				nmatch++;
		}

		/* take this one as the best choice so far? */
		if ((nmatch > nbestMatch) || (last_candidate == NULL))
		{
			nbestMatch = nmatch;
			candidates = current_candidate;
			last_candidate = current_candidate;
			ncandidates = 1;
		}
		/* no worse than the last choice, so keep this one too? */
		else if (nmatch == nbestMatch)
		{
			last_candidate->next = current_candidate;
			last_candidate = current_candidate;
			ncandidates++;
		}
		/* otherwise, don't bother keeping this one... */
	}

	if (last_candidate)			/* terminate rebuilt list */
		last_candidate->next = NULL;

	if (ncandidates == 1)
		return candidates;

	/*
	 * Still too many candidates? Now look for candidates which have either
	 * exact matches or preferred types at the args that will require
	 * coercion. (Restriction added in 7.4: preferred type must be of same
	 * category as input type; give no preference to cross-category
	 * conversions to preferred types.)  Keep all candidates if none match.
	 */
	for (i = 0; i < nargs; i++) /* avoid multiple lookups */
		slot_category[i] = TypeCategory(input_base_typeids[i]);
	ncandidates = 0;
	nbestMatch = 0;
	last_candidate = NULL;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeids = current_candidate->args;
		nmatch = 0;
		for (i = 0; i < nargs; i++)
		{
			if (input_base_typeids[i] != UNKNOWNOID)
			{
				if (current_typeids[i] == input_base_typeids[i] ||
					IsPreferredType(slot_category[i], current_typeids[i]))
					nmatch++;
			}
		}

		if ((nmatch > nbestMatch) || (last_candidate == NULL))
		{
			nbestMatch = nmatch;
			candidates = current_candidate;
			last_candidate = current_candidate;
			ncandidates = 1;
		}
		else if (nmatch == nbestMatch)
		{
			last_candidate->next = current_candidate;
			last_candidate = current_candidate;
			ncandidates++;
		}
	}

	if (last_candidate)			/* terminate rebuilt list */
		last_candidate->next = NULL;

	if (ncandidates == 1)
		return candidates;

	/*
	 * Still too many candidates?  Try assigning types for the unknown inputs.
	 *
	 * If there are no unknown inputs, we have no more heuristics that apply,
	 * and must fail.
	 */
	if (nunknowns == 0)
		return NULL;			/* failed to select a best candidate */

	/*
	 * The next step examines each unknown argument position to see if we can
	 * determine a "type category" for it.  If any candidate has an input
	 * datatype of STRING category, use STRING category (this bias towards
	 * STRING is appropriate since unknown-type literals look like strings).
	 * Otherwise, if all the candidates agree on the type category of this
	 * argument position, use that category.  Otherwise, fail because we
	 * cannot determine a category.
	 *
	 * If we are able to determine a type category, also notice whether any of
	 * the candidates takes a preferred datatype within the category.
	 *
	 * Having completed this examination, remove candidates that accept the
	 * wrong category at any unknown position.  Also, if at least one
	 * candidate accepted a preferred type at a position, remove candidates
	 * that accept non-preferred types.  If just one candidate remains, return
	 * that one.  However, if this rule turns out to reject all candidates,
	 * keep them all instead.
	 */
	resolved_unknowns = false;
	for (i = 0; i < nargs; i++)
	{
		bool		have_conflict;

		if (input_base_typeids[i] != UNKNOWNOID)
			continue;
		resolved_unknowns = true;	/* assume we can do it */
		slot_category[i] = TYPCATEGORY_INVALID;
		slot_has_preferred_type[i] = false;
		have_conflict = false;
		for (current_candidate = candidates;
			 current_candidate != NULL;
			 current_candidate = current_candidate->next)
		{
			current_typeids = current_candidate->args;
			current_type = current_typeids[i];
			get_type_category_preferred(current_type,
										&current_category,
										&current_is_preferred);
			if (slot_category[i] == TYPCATEGORY_INVALID)
			{
				/* first candidate */
				slot_category[i] = current_category;
				slot_has_preferred_type[i] = current_is_preferred;
			}
			else if (current_category == slot_category[i])
			{
				/* more candidates in same category */
				slot_has_preferred_type[i] |= current_is_preferred;
			}
			else
			{
				/* category conflict! */
				if (current_category == TYPCATEGORY_STRING)
				{
					/* STRING always wins if available */
					slot_category[i] = current_category;
					slot_has_preferred_type[i] = current_is_preferred;
				}
				else
				{
					/*
					 * Remember conflict, but keep going (might find STRING)
					 */
					have_conflict = true;
				}
			}
		}
		if (have_conflict && slot_category[i] != TYPCATEGORY_STRING)
		{
			/* Failed to resolve category conflict at this position */
			resolved_unknowns = false;
			break;
		}
	}

	if (resolved_unknowns)
	{
		/* Strip non-matching candidates */
		ncandidates = 0;
		first_candidate = candidates;
		last_candidate = NULL;
		for (current_candidate = candidates;
			 current_candidate != NULL;
			 current_candidate = current_candidate->next)
		{
			bool		keepit = true;

			current_typeids = current_candidate->args;
			for (i = 0; i < nargs; i++)
			{
				if (input_base_typeids[i] != UNKNOWNOID)
					continue;
				current_type = current_typeids[i];
				get_type_category_preferred(current_type,
											&current_category,
											&current_is_preferred);
				if (current_category != slot_category[i])
				{
					keepit = false;
					break;
				}
				if (slot_has_preferred_type[i] && !current_is_preferred)
				{
					keepit = false;
					break;
				}
			}
			if (keepit)
			{
				/* keep this candidate */
				last_candidate = current_candidate;
				ncandidates++;
			}
			else
			{
				/* forget this candidate */
				if (last_candidate)
					last_candidate->next = current_candidate->next;
				else
					first_candidate = current_candidate->next;
			}
		}

		/* if we found any matches, restrict our attention to those */
		if (last_candidate)
		{
			candidates = first_candidate;
			/* terminate rebuilt list */
			last_candidate->next = NULL;
		}

		if (ncandidates == 1)
			return candidates;
	}

	/*
	 * Last gasp: if there are both known- and unknown-type inputs, and all
	 * the known types are the same, assume the unknown inputs are also that
	 * type, and see if that gives us a unique match.  If so, use that match.
	 *
	 * NOTE: for a binary operator with one unknown and one non-unknown input,
	 * we already tried this heuristic in binary_oper_exact().  However, that
	 * code only finds exact matches, whereas here we will handle matches that
	 * involve coercion, polymorphic type resolution, etc.
	 */
	if (nunknowns < nargs)
	{
		Oid			known_type = UNKNOWNOID;

		for (i = 0; i < nargs; i++)
		{
			if (input_base_typeids[i] == UNKNOWNOID)
				continue;
			if (known_type == UNKNOWNOID)	/* first known arg? */
				known_type = input_base_typeids[i];
			else if (known_type != input_base_typeids[i])
			{
				/* oops, not all match */
				known_type = UNKNOWNOID;
				break;
			}
		}

		if (known_type != UNKNOWNOID)
		{
			/* okay, just one known type, apply the heuristic */
			for (i = 0; i < nargs; i++)
				input_base_typeids[i] = known_type;
			ncandidates = 0;
			last_candidate = NULL;
			for (current_candidate = candidates;
				 current_candidate != NULL;
				 current_candidate = current_candidate->next)
			{
				current_typeids = current_candidate->args;
				if (can_coerce_type(nargs, input_base_typeids, current_typeids,
									COERCION_IMPLICIT))
				{
					if (++ncandidates > 1)
						break;	/* not unique, give up */
					last_candidate = current_candidate;
				}
			}
			if (ncandidates == 1)
			{
				/* successfully identified a unique match */
				last_candidate->next = NULL;
				return last_candidate;
			}
		}
	}

	return NULL;				/* failed to select a best candidate */
}								/* func_select_candidate() */


/* func_get_detail()
 *
 * Find the named function in the system catalogs.
 *
 * Attempt to find the named function in the system catalogs with
 * arguments exactly as specified, so that the normal case (exact match)
 * is as quick as possible.
 *
 * If an exact match isn't found:
 *	1) check for possible interpretation as a type coercion request
 *	2) apply the ambiguous-function resolution rules
 *
 * Return values *funcid through *true_typeids receive info about the function.
 * If argdefaults isn't NULL, *argdefaults receives a list of any default
 * argument expressions that need to be added to the given arguments.
 *
 * When processing a named- or mixed-notation call (ie, fargnames isn't NIL),
 * the returned true_typeids and argdefaults are ordered according to the
 * call's argument ordering: first any positional arguments, then the named
 * arguments, then defaulted arguments (if needed and allowed by
 * expand_defaults).  Some care is needed if this information is to be compared
 * to the function's pg_proc entry, but in practice the caller can usually
 * just work with the call's argument ordering.
 *
 * We rely primarily on fargnames/nargs/argtypes as the argument description.
 * The actual expression node list is passed in fargs so that we can check
 * for type coercion of a constant.  Some callers pass fargs == NIL indicating
 * they don't need that check made.  Note also that when fargnames isn't NIL,
 * the fargs list must be passed if the caller wants actual argument position
 * information to be returned into the NamedArgExpr nodes.
 */
FuncDetailCode
func_get_detail(List *funcname,
				List *fargs,
				List *fargnames,
				int nargs,
				Oid *argtypes,
				bool expand_variadic,
				bool expand_defaults,
				Oid *funcid,	/* return value */
				Oid *rettype,	/* return value */
				bool *retset,	/* return value */
				int *nvargs,	/* return value */
				Oid *vatype,	/* return value */
				Oid **true_typeids, /* return value */
				List **argdefaults) /* optional return value */
{
	FuncCandidateList raw_candidates;
	FuncCandidateList best_candidate;

	/* initialize output arguments to silence compiler warnings */
	*funcid = InvalidOid;
	*rettype = InvalidOid;
	*retset = false;
	*nvargs = 0;
	*vatype = InvalidOid;
	*true_typeids = NULL;
	if (argdefaults)
		*argdefaults = NIL;

	/* Get list of possible candidates from namespace search */
	raw_candidates = FuncnameGetCandidates(funcname, nargs, fargnames,
										   expand_variadic, expand_defaults,
										   false);

	/*
	 * Quickly check if there is an exact match to the input datatypes (there
	 * can be only one)
	 */
	for (best_candidate = raw_candidates;
		 best_candidate != NULL;
		 best_candidate = best_candidate->next)
	{
		/* if nargs==0, argtypes can be null; don't pass that to memcmp */
		if (nargs == 0 ||
			memcmp(argtypes, best_candidate->args, nargs * sizeof(Oid)) == 0)
			break;
	}

	if (best_candidate == NULL)
	{
		/*
		 * If we didn't find an exact match, next consider the possibility
		 * that this is really a type-coercion request: a single-argument
		 * function call where the function name is a type name.  If so, and
		 * if the coercion path is RELABELTYPE or COERCEVIAIO, then go ahead
		 * and treat the "function call" as a coercion.
		 *
		 * This interpretation needs to be given higher priority than
		 * interpretations involving a type coercion followed by a function
		 * call, otherwise we can produce surprising results. For example, we
		 * want "text(varchar)" to be interpreted as a simple coercion, not as
		 * "text(name(varchar))" which the code below this point is entirely
		 * capable of selecting.
		 *
		 * We also treat a coercion of a previously-unknown-type literal
		 * constant to a specific type this way.
		 *
		 * The reason we reject COERCION_PATH_FUNC here is that we expect the
		 * cast implementation function to be named after the target type.
		 * Thus the function will be found by normal lookup if appropriate.
		 *
		 * The reason we reject COERCION_PATH_ARRAYCOERCE is mainly that you
		 * can't write "foo[] (something)" as a function call.  In theory
		 * someone might want to invoke it as "_foo (something)" but we have
		 * never supported that historically, so we can insist that people
		 * write it as a normal cast instead.
		 *
		 * We also reject the specific case of COERCEVIAIO for a composite
		 * source type and a string-category target type.  This is a case that
		 * find_coercion_pathway() allows by default, but experience has shown
		 * that it's too commonly invoked by mistake.  So, again, insist that
		 * people use cast syntax if they want to do that.
		 *
		 * NB: it's important that this code does not exceed what coerce_type
		 * can do, because the caller will try to apply coerce_type if we
		 * return FUNCDETAIL_COERCION.  If we return that result for something
		 * coerce_type can't handle, we'll cause infinite recursion between
		 * this module and coerce_type!
		 */
		if (nargs == 1 && fargs != NIL && fargnames == NIL)
		{
			Oid			targetType = FuncNameAsType(funcname);

			if (OidIsValid(targetType))
			{
				Oid			sourceType = argtypes[0];
				Node	   *arg1 = linitial(fargs);
				bool		iscoercion;

				if (sourceType == UNKNOWNOID && IsA(arg1, Const))
				{
					/* always treat typename('literal') as coercion */
					iscoercion = true;
				}
				else
				{
					CoercionPathType cpathtype;
					Oid			cfuncid;

					cpathtype = find_coercion_pathway(targetType, sourceType,
													  COERCION_EXPLICIT,
													  &cfuncid);
					switch (cpathtype)
					{
						case COERCION_PATH_RELABELTYPE:
							iscoercion = true;
							break;
						case COERCION_PATH_COERCEVIAIO:
							if ((sourceType == RECORDOID ||
								 ISCOMPLEX(sourceType)) &&
								TypeCategory(targetType) == TYPCATEGORY_STRING)
								iscoercion = false;
							else
								iscoercion = true;
							break;
						default:
							iscoercion = false;
							break;
					}
				}

				if (iscoercion)
				{
					/* Treat it as a type coercion */
					*funcid = InvalidOid;
					*rettype = targetType;
					*retset = false;
					*nvargs = 0;
					*vatype = InvalidOid;
					*true_typeids = argtypes;
					return FUNCDETAIL_COERCION;
				}
			}
		}

		/*
		 * didn't find an exact match, so now try to match up candidates...
		 */
		if (raw_candidates != NULL)
		{
			FuncCandidateList current_candidates;
			int			ncandidates;

			ncandidates = func_match_argtypes(nargs,
											  argtypes,
											  raw_candidates,
											  &current_candidates);

			/* one match only? then run with it... */
			if (ncandidates == 1)
				best_candidate = current_candidates;

			/*
			 * multiple candidates? then better decide or throw an error...
			 */
			else if (ncandidates > 1)
			{
				best_candidate = func_select_candidate(nargs,
													   argtypes,
													   current_candidates);

				/*
				 * If we were able to choose a best candidate, we're done.
				 * Otherwise, ambiguous function call.
				 */
				if (!best_candidate)
					return FUNCDETAIL_MULTIPLE;
			}
		}
	}

	if (best_candidate)
	{
		HeapTuple	ftup;
		Form_pg_proc pform;
		FuncDetailCode result;

		/*
		 * If processing named args or expanding variadics or defaults, the
		 * "best candidate" might represent multiple equivalently good
		 * functions; treat this case as ambiguous.
		 */
		if (!OidIsValid(best_candidate->oid))
			return FUNCDETAIL_MULTIPLE;

		/*
		 * We disallow VARIADIC with named arguments unless the last argument
		 * (the one with VARIADIC attached) actually matched the variadic
		 * parameter.  This is mere pedantry, really, but some folks insisted.
		 */
		if (fargnames != NIL && !expand_variadic && nargs > 0 &&
			best_candidate->argnumbers[nargs - 1] != nargs - 1)
			return FUNCDETAIL_NOTFOUND;

		*funcid = best_candidate->oid;
		*nvargs = best_candidate->nvargs;
		*true_typeids = best_candidate->args;

		/*
		 * If processing named args, return actual argument positions into
		 * NamedArgExpr nodes in the fargs list.  This is a bit ugly but not
		 * worth the extra notation needed to do it differently.
		 */
		if (best_candidate->argnumbers != NULL)
		{
			int			i = 0;
			ListCell   *lc;

			foreach(lc, fargs)
			{
				NamedArgExpr *na = (NamedArgExpr *) lfirst(lc);

				if (IsA(na, NamedArgExpr))
					na->argnumber = best_candidate->argnumbers[i];
				i++;
			}
		}

		ftup = SearchSysCache1(PROCOID,
							   ObjectIdGetDatum(best_candidate->oid));
		if (!HeapTupleIsValid(ftup))	/* should not happen */
			elog(ERROR, "cache lookup failed for function %u",
				 best_candidate->oid);
		pform = (Form_pg_proc) GETSTRUCT(ftup);
		*rettype = pform->prorettype;
		*retset = pform->proretset;
		*vatype = pform->provariadic;
		/* fetch default args if caller wants 'em */
		if (argdefaults && best_candidate->ndargs > 0)
		{
			Datum		proargdefaults;
			bool		isnull;
			char	   *str;
			List	   *defaults;

			/* shouldn't happen, FuncnameGetCandidates messed up */
			if (best_candidate->ndargs > pform->pronargdefaults)
				elog(ERROR, "not enough default arguments");

			proargdefaults = SysCacheGetAttr(PROCOID, ftup,
											 Anum_pg_proc_proargdefaults,
											 &isnull);
			Assert(!isnull);
			str = TextDatumGetCString(proargdefaults);
			defaults = castNode(List, stringToNode(str));
			pfree(str);

			/* Delete any unused defaults from the returned list */
			if (best_candidate->argnumbers != NULL)
			{
				/*
				 * This is a bit tricky in named notation, since the supplied
				 * arguments could replace any subset of the defaults.  We
				 * work by making a bitmapset of the argnumbers of defaulted
				 * arguments, then scanning the defaults list and selecting
				 * the needed items.  (This assumes that defaulted arguments
				 * should be supplied in their positional order.)
				 */
				Bitmapset  *defargnumbers;
				int		   *firstdefarg;
				List	   *newdefaults;
				ListCell   *lc;
				int			i;

				defargnumbers = NULL;
				firstdefarg = &best_candidate->argnumbers[best_candidate->nargs - best_candidate->ndargs];
				for (i = 0; i < best_candidate->ndargs; i++)
					defargnumbers = bms_add_member(defargnumbers,
												   firstdefarg[i]);
				newdefaults = NIL;
				i = pform->pronargs - pform->pronargdefaults;
				foreach(lc, defaults)
				{
					if (bms_is_member(i, defargnumbers))
						newdefaults = lappend(newdefaults, lfirst(lc));
					i++;
				}
				Assert(list_length(newdefaults) == best_candidate->ndargs);
				bms_free(defargnumbers);
				*argdefaults = newdefaults;
			}
			else
			{
				/*
				 * Defaults for positional notation are lots easier; just
				 * remove any unwanted ones from the front.
				 */
				int			ndelete;

				ndelete = list_length(defaults) - best_candidate->ndargs;
				if (ndelete > 0)
					defaults = list_copy_tail(defaults, ndelete);
				*argdefaults = defaults;
			}
		}

		switch (pform->prokind)
		{
			case PROKIND_AGGREGATE:
				result = FUNCDETAIL_AGGREGATE;
				break;
			case PROKIND_FUNCTION:
				result = FUNCDETAIL_NORMAL;
				break;
			case PROKIND_PROCEDURE:
				result = FUNCDETAIL_PROCEDURE;
				break;
			case PROKIND_WINDOW:
				result = FUNCDETAIL_WINDOWFUNC;
				break;
			default:
				elog(ERROR, "unrecognized prokind: %c", pform->prokind);
				result = FUNCDETAIL_NORMAL; /* keep compiler quiet */
				break;
		}

		ReleaseSysCache(ftup);
		return result;
	}

	return FUNCDETAIL_NOTFOUND;
}


/*
 * unify_hypothetical_args()
 *
 * Ensure that each hypothetical direct argument of a hypothetical-set
 * aggregate has the same type as the corresponding aggregated argument.
 * Modify the expressions in the fargs list, if necessary, and update
 * actual_arg_types[].
 *
 * If the agg declared its args non-ANY (even ANYELEMENT), we need only a
 * sanity check that the declared types match; make_fn_arguments will coerce
 * the actual arguments to match the declared ones.  But if the declaration
 * is ANY, nothing will happen in make_fn_arguments, so we need to fix any
 * mismatch here.  We use the same type resolution logic as UNION etc.
 */
static void
unify_hypothetical_args(ParseState *pstate,
						List *fargs,
						int numAggregatedArgs,
						Oid *actual_arg_types,
						Oid *declared_arg_types)
{
	int			numDirectArgs,
				numNonHypotheticalArgs;
	int			hargpos;

	numDirectArgs = list_length(fargs) - numAggregatedArgs;
	numNonHypotheticalArgs = numDirectArgs - numAggregatedArgs;
	/* safety check (should only trigger with a misdeclared agg) */
	if (numNonHypotheticalArgs < 0)
		elog(ERROR, "incorrect number of arguments to hypothetical-set aggregate");

	/* Check each hypothetical arg and corresponding aggregated arg */
	for (hargpos = numNonHypotheticalArgs; hargpos < numDirectArgs; hargpos++)
	{
		int			aargpos = numDirectArgs + (hargpos - numNonHypotheticalArgs);
		ListCell   *harg = list_nth_cell(fargs, hargpos);
		ListCell   *aarg = list_nth_cell(fargs, aargpos);
		Oid			commontype;
		int32		commontypmod;

		/* A mismatch means AggregateCreate didn't check properly ... */
		if (declared_arg_types[hargpos] != declared_arg_types[aargpos])
			elog(ERROR, "hypothetical-set aggregate has inconsistent declared argument types");

		/* No need to unify if make_fn_arguments will coerce */
		if (declared_arg_types[hargpos] != ANYOID)
			continue;

		/*
		 * Select common type, giving preference to the aggregated argument's
		 * type (we'd rather coerce the direct argument once than coerce all
		 * the aggregated values).
		 */
		commontype = select_common_type(pstate,
										list_make2(lfirst(aarg), lfirst(harg)),
										"WITHIN GROUP",
										NULL);
		commontypmod = select_common_typmod(pstate,
											list_make2(lfirst(aarg), lfirst(harg)),
											commontype);

		/*
		 * Perform the coercions.  We don't need to worry about NamedArgExprs
		 * here because they aren't supported with aggregates.
		 */
		lfirst(harg) = coerce_type(pstate,
								   (Node *) lfirst(harg),
								   actual_arg_types[hargpos],
								   commontype, commontypmod,
								   COERCION_IMPLICIT,
								   COERCE_IMPLICIT_CAST,
								   -1);
		actual_arg_types[hargpos] = commontype;
		lfirst(aarg) = coerce_type(pstate,
								   (Node *) lfirst(aarg),
								   actual_arg_types[aargpos],
								   commontype, commontypmod,
								   COERCION_IMPLICIT,
								   COERCE_IMPLICIT_CAST,
								   -1);
		actual_arg_types[aargpos] = commontype;
	}
}


/*
 * make_fn_arguments()
 *
 * Given the actual argument expressions for a function, and the desired
 * input types for the function, add any necessary typecasting to the
 * expression tree.  Caller should already have verified that casting is
 * allowed.
 *
 * Caution: given argument list is modified in-place.
 *
 * As with coerce_type, pstate may be NULL if no special unknown-Param
 * processing is wanted.
 */
void
make_fn_arguments(ParseState *pstate,
				  List *fargs,
				  Oid *actual_arg_types,
				  Oid *declared_arg_types)
{
	ListCell   *current_fargs;
	int			i = 0;

	foreach(current_fargs, fargs)
	{
		/* types don't match? then force coercion using a function call... */
		if (actual_arg_types[i] != declared_arg_types[i])
		{
			Node	   *node = (Node *) lfirst(current_fargs);

			/*
			 * If arg is a NamedArgExpr, coerce its input expr instead --- we
			 * want the NamedArgExpr to stay at the top level of the list.
			 */
			if (IsA(node, NamedArgExpr))
			{
				NamedArgExpr *na = (NamedArgExpr *) node;

				node = coerce_type(pstate,
								   (Node *) na->arg,
								   actual_arg_types[i],
								   declared_arg_types[i], -1,
								   COERCION_IMPLICIT,
								   COERCE_IMPLICIT_CAST,
								   -1);
				na->arg = (Expr *) node;
			}
			else
			{
				node = coerce_type(pstate,
								   node,
								   actual_arg_types[i],
								   declared_arg_types[i], -1,
								   COERCION_IMPLICIT,
								   COERCE_IMPLICIT_CAST,
								   -1);
				lfirst(current_fargs) = node;
			}
		}
		i++;
	}
}

/*
 * FuncNameAsType -
 *	  convenience routine to see if a function name matches a type name
 *
 * Returns the OID of the matching type, or InvalidOid if none.  We ignore
 * shell types and complex types.
 */
static Oid
FuncNameAsType(List *funcname)
{
	Oid			result;
	Type		typtup;

	/*
	 * temp_ok=false protects the <refsect1 id="sql-createfunction-security">
	 * contract for writing SECURITY DEFINER functions safely.
	 */
	typtup = LookupTypeNameExtended(NULL, makeTypeNameFromNameList(funcname),
									NULL, false, false);
	if (typtup == NULL)
		return InvalidOid;

	if (((Form_pg_type) GETSTRUCT(typtup))->typisdefined &&
		!OidIsValid(typeTypeRelid(typtup)))
		result = typeTypeId(typtup);
	else
		result = InvalidOid;

	ReleaseSysCache(typtup);
	return result;
}

/*
 * ParseComplexProjection -
 *	  handles function calls with a single argument that is of complex type.
 *	  If the function call is actually a column projection, return a suitably
 *	  transformed expression tree.  If not, return NULL.
 */
static Node *
ParseComplexProjection(ParseState *pstate, const char *funcname, Node *first_arg,
					   int location)
{
	TupleDesc	tupdesc;
	int			i;

	/*
	 * Special case for whole-row Vars so that we can resolve (foo.*).bar even
	 * when foo is a reference to a subselect, join, or RECORD function. A
	 * bonus is that we avoid generating an unnecessary FieldSelect; our
	 * result can omit the whole-row Var and just be a Var for the selected
	 * field.
	 *
	 * This case could be handled by expandRecordVariable, but it's more
	 * efficient to do it this way when possible.
	 */
	if (IsA(first_arg, Var) &&
		((Var *) first_arg)->varattno == InvalidAttrNumber)
	{
		ParseNamespaceItem *nsitem;

		nsitem = GetNSItemByRangeTablePosn(pstate,
										   ((Var *) first_arg)->varno,
										   ((Var *) first_arg)->varlevelsup);
		/* Return a Var if funcname matches a column, else NULL */
		return scanNSItemForColumn(pstate, nsitem,
								   ((Var *) first_arg)->varlevelsup,
								   funcname, location);
	}

	/*
	 * Else do it the hard way with get_expr_result_tupdesc().
	 *
	 * If it's a Var of type RECORD, we have to work even harder: we have to
	 * find what the Var refers to, and pass that to get_expr_result_tupdesc.
	 * That task is handled by expandRecordVariable().
	 */
	if (IsA(first_arg, Var) &&
		((Var *) first_arg)->vartype == RECORDOID)
		tupdesc = expandRecordVariable(pstate, (Var *) first_arg, 0);
	else
		tupdesc = get_expr_result_tupdesc(first_arg, true);
	if (!tupdesc)
		return NULL;			/* unresolvable RECORD type */

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (strcmp(funcname, NameStr(att->attname)) == 0 &&
			!att->attisdropped)
		{
			/* Success, so generate a FieldSelect expression */
			FieldSelect *fselect = makeNode(FieldSelect);

			fselect->arg = (Expr *) first_arg;
			fselect->fieldnum = i + 1;
			fselect->resulttype = att->atttypid;
			fselect->resulttypmod = att->atttypmod;
			/* save attribute's collation for parse_collate.c */
			fselect->resultcollid = att->attcollation;
			return (Node *) fselect;
		}
	}

	return NULL;				/* funcname does not match any column */
}

/*
 * funcname_signature_string
 *		Build a string representing a function name, including arg types.
 *		The result is something like "foo(integer)".
 *
 * If argnames isn't NIL, it is a list of C strings representing the actual
 * arg names for the last N arguments.  This must be considered part of the
 * function signature too, when dealing with named-notation function calls.
 *
 * This is typically used in the construction of function-not-found error
 * messages.
 */
const char *
funcname_signature_string(const char *funcname, int nargs,
						  List *argnames, const Oid *argtypes)
{
	StringInfoData argbuf;
	int			numposargs;
	ListCell   *lc;
	int			i;

	initStringInfo(&argbuf);

	appendStringInfo(&argbuf, "%s(", funcname);

	numposargs = nargs - list_length(argnames);
	lc = list_head(argnames);

	for (i = 0; i < nargs; i++)
	{
		if (i)
			appendStringInfoString(&argbuf, ", ");
		if (i >= numposargs)
		{
			appendStringInfo(&argbuf, "%s => ", (char *) lfirst(lc));
			lc = lnext(argnames, lc);
		}
		appendStringInfoString(&argbuf, format_type_be(argtypes[i]));
	}

	appendStringInfoChar(&argbuf, ')');

	return argbuf.data;			/* return palloc'd string buffer */
}

/*
 * func_signature_string
 *		As above, but function name is passed as a qualified name list.
 */
const char *
func_signature_string(List *funcname, int nargs,
					  List *argnames, const Oid *argtypes)
{
	return funcname_signature_string(NameListToString(funcname),
									 nargs, argnames, argtypes);
}

/*
 * LookupFuncNameInternal
 *		Workhorse for LookupFuncName/LookupFuncWithArgs
 *
 * In an error situation, e.g. can't find the function, then we return
 * InvalidOid and set *lookupError to indicate what went wrong.
 *
 * Possible errors:
 *	FUNCLOOKUP_NOSUCHFUNC: we can't find a function of this name.
 *	FUNCLOOKUP_AMBIGUOUS: nargs == -1 and more than one function matches.
 */
static Oid
LookupFuncNameInternal(List *funcname, int nargs, const Oid *argtypes,
					   bool missing_ok, FuncLookupError *lookupError)
{
	FuncCandidateList clist;

	/* NULL argtypes allowed for nullary functions only */
	Assert(argtypes != NULL || nargs == 0);

	/* Always set *lookupError, to forestall uninitialized-variable warnings */
	*lookupError = FUNCLOOKUP_NOSUCHFUNC;

	clist = FuncnameGetCandidates(funcname, nargs, NIL, false, false,
								  missing_ok);

	/*
	 * If no arguments were specified, the name must yield a unique candidate.
	 */
	if (nargs < 0)
	{
		if (clist)
		{
			/* If there is a second match then it's ambiguous */
			if (clist->next)
			{
				*lookupError = FUNCLOOKUP_AMBIGUOUS;
				return InvalidOid;
			}
			/* Otherwise return the match */
			return clist->oid;
		}
		else
			return InvalidOid;
	}

	/*
	 * Otherwise, look for a match to the arg types.  FuncnameGetCandidates
	 * has ensured that there's at most one match in the returned list.
	 */
	while (clist)
	{
		/* if nargs==0, argtypes can be null; don't pass that to memcmp */
		if (nargs == 0 ||
			memcmp(argtypes, clist->args, nargs * sizeof(Oid)) == 0)
			return clist->oid;
		clist = clist->next;
	}

	return InvalidOid;
}

/*
 * LookupFuncName
 *
 * Given a possibly-qualified function name and optionally a set of argument
 * types, look up the function.  Pass nargs == -1 to indicate that the number
 * and types of the arguments are unspecified (this is NOT the same as
 * specifying that there are no arguments).
 *
 * If the function name is not schema-qualified, it is sought in the current
 * namespace search path.
 *
 * If the function is not found, we return InvalidOid if missing_ok is true,
 * else raise an error.
 *
 * If nargs == -1 and multiple functions are found matching this function name
 * we will raise an ambiguous-function error, regardless of what missing_ok is
 * set to.
 */
Oid
LookupFuncName(List *funcname, int nargs, const Oid *argtypes, bool missing_ok)
{
	Oid			funcoid;
	FuncLookupError lookupError;

	funcoid = LookupFuncNameInternal(funcname, nargs, argtypes, missing_ok,
									 &lookupError);

	if (OidIsValid(funcoid))
		return funcoid;

	switch (lookupError)
	{
		case FUNCLOOKUP_NOSUCHFUNC:
			/* Let the caller deal with it when missing_ok is true */
			if (missing_ok)
				return InvalidOid;

			if (nargs < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not find a function named \"%s\"",
								NameListToString(funcname))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("function %s does not exist",
								func_signature_string(funcname, nargs,
													  NIL, argtypes))));
			break;

		case FUNCLOOKUP_AMBIGUOUS:
			/* Raise an error regardless of missing_ok */
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("function name \"%s\" is not unique",
							NameListToString(funcname)),
					 errhint("Specify the argument list to select the function unambiguously.")));
			break;
	}

	return InvalidOid;			/* Keep compiler quiet */
}

/*
 * LookupFuncWithArgs
 *
 * Like LookupFuncName, but the argument types are specified by an
 * ObjectWithArgs node.  Also, this function can check whether the result is a
 * function, procedure, or aggregate, based on the objtype argument.  Pass
 * OBJECT_ROUTINE to accept any of them.
 *
 * For historical reasons, we also accept aggregates when looking for a
 * function.
 *
 * When missing_ok is true we don't generate any error for missing objects and
 * return InvalidOid.  Other types of errors can still be raised, regardless
 * of the value of missing_ok.
 */
Oid
LookupFuncWithArgs(ObjectType objtype, ObjectWithArgs *func, bool missing_ok)
{
	Oid			argoids[FUNC_MAX_ARGS];
	int			argcount;
	int			nargs;
	int			i;
	ListCell   *args_item;
	Oid			oid;
	FuncLookupError lookupError;

	Assert(objtype == OBJECT_AGGREGATE ||
		   objtype == OBJECT_FUNCTION ||
		   objtype == OBJECT_PROCEDURE ||
		   objtype == OBJECT_ROUTINE);

	argcount = list_length(func->objargs);
	if (argcount > FUNC_MAX_ARGS)
	{
		if (objtype == OBJECT_PROCEDURE)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg_plural("procedures cannot have more than %d argument",
								   "procedures cannot have more than %d arguments",
								   FUNC_MAX_ARGS,
								   FUNC_MAX_ARGS)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg_plural("functions cannot have more than %d argument",
								   "functions cannot have more than %d arguments",
								   FUNC_MAX_ARGS,
								   FUNC_MAX_ARGS)));
	}

	i = 0;
	foreach(args_item, func->objargs)
	{
		TypeName   *t = (TypeName *) lfirst(args_item);

		argoids[i] = LookupTypeNameOid(NULL, t, missing_ok);
		if (!OidIsValid(argoids[i]))
			return InvalidOid;	/* missing_ok must be true */
		i++;
	}

	/*
	 * Set nargs for LookupFuncNameInternal. It expects -1 to mean no args
	 * were specified.
	 */
	nargs = func->args_unspecified ? -1 : argcount;

	oid = LookupFuncNameInternal(func->objname, nargs, argoids, missing_ok,
								 &lookupError);

	if (OidIsValid(oid))
	{
		/*
		 * Even if we found the function, perform validation that the objtype
		 * matches the prokind of the found function.  For historical reasons
		 * we allow the objtype of FUNCTION to include aggregates and window
		 * functions; but we draw the line if the object is a procedure.  That
		 * is a new enough feature that this historical rule does not apply.
		 */
		switch (objtype)
		{
			case OBJECT_FUNCTION:
				/* Only complain if it's a procedure. */
				if (get_func_prokind(oid) == PROKIND_PROCEDURE)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("%s is not a function",
									func_signature_string(func->objname, argcount,
														  NIL, argoids))));
				break;

			case OBJECT_PROCEDURE:
				/* Reject if found object is not a procedure. */
				if (get_func_prokind(oid) != PROKIND_PROCEDURE)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("%s is not a procedure",
									func_signature_string(func->objname, argcount,
														  NIL, argoids))));
				break;

			case OBJECT_AGGREGATE:
				/* Reject if found object is not an aggregate. */
				if (get_func_prokind(oid) != PROKIND_AGGREGATE)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("function %s is not an aggregate",
									func_signature_string(func->objname, argcount,
														  NIL, argoids))));
				break;

			default:
				/* OBJECT_ROUTINE accepts anything. */
				break;
		}

		return oid;				/* All good */
	}
	else
	{
		/* Deal with cases where the lookup failed */
		switch (lookupError)
		{
			case FUNCLOOKUP_NOSUCHFUNC:
				/* Suppress no-such-func errors when missing_ok is true */
				if (missing_ok)
					break;

				switch (objtype)
				{
					case OBJECT_PROCEDURE:
						if (func->args_unspecified)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("could not find a procedure named \"%s\"",
											NameListToString(func->objname))));
						else
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("procedure %s does not exist",
											func_signature_string(func->objname, argcount,
																  NIL, argoids))));
						break;

					case OBJECT_AGGREGATE:
						if (func->args_unspecified)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("could not find an aggregate named \"%s\"",
											NameListToString(func->objname))));
						else if (argcount == 0)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("aggregate %s(*) does not exist",
											NameListToString(func->objname))));
						else
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("aggregate %s does not exist",
											func_signature_string(func->objname, argcount,
																  NIL, argoids))));
						break;

					default:
						/* FUNCTION and ROUTINE */
						if (func->args_unspecified)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("could not find a function named \"%s\"",
											NameListToString(func->objname))));
						else
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("function %s does not exist",
											func_signature_string(func->objname, argcount,
																  NIL, argoids))));
						break;
				}
				break;

			case FUNCLOOKUP_AMBIGUOUS:
				switch (objtype)
				{
					case OBJECT_FUNCTION:
						ereport(ERROR,
								(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
								 errmsg("function name \"%s\" is not unique",
										NameListToString(func->objname)),
								 errhint("Specify the argument list to select the function unambiguously.")));
						break;
					case OBJECT_PROCEDURE:
						ereport(ERROR,
								(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
								 errmsg("procedure name \"%s\" is not unique",
										NameListToString(func->objname)),
								 errhint("Specify the argument list to select the procedure unambiguously.")));
						break;
					case OBJECT_AGGREGATE:
						ereport(ERROR,
								(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
								 errmsg("aggregate name \"%s\" is not unique",
										NameListToString(func->objname)),
								 errhint("Specify the argument list to select the aggregate unambiguously.")));
						break;
					case OBJECT_ROUTINE:
						ereport(ERROR,
								(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
								 errmsg("routine name \"%s\" is not unique",
										NameListToString(func->objname)),
								 errhint("Specify the argument list to select the routine unambiguously.")));
						break;

					default:
						Assert(false);	/* Disallowed by Assert above */
						break;
				}
				break;
		}

		return InvalidOid;
	}
}

/*
 * check_srf_call_placement
 *		Verify that a set-returning function is called in a valid place,
 *		and throw a nice error if not.
 *
 * A side-effect is to set pstate->p_hasTargetSRFs true if appropriate.
 *
 * last_srf should be a copy of pstate->p_last_srf from just before we
 * started transforming the function's arguments.  This allows detection
 * of whether the SRF's arguments contain any SRFs.
 */
void
check_srf_call_placement(ParseState *pstate, Node *last_srf, int location)
{
	const char *err;
	bool		errkind;

	/*
	 * Check to see if the set-returning function is in an invalid place
	 * within the query.  Basically, we don't allow SRFs anywhere except in
	 * the targetlist (which includes GROUP BY/ORDER BY expressions), VALUES,
	 * and functions in FROM.
	 *
	 * For brevity we support two schemes for reporting an error here: set
	 * "err" to a custom message, or set "errkind" true if the error context
	 * is sufficiently identified by what ParseExprKindName will return, *and*
	 * what it will return is just a SQL keyword.  (Otherwise, use a custom
	 * message to avoid creating translation problems.)
	 */
	err = NULL;
	errkind = false;
	switch (pstate->p_expr_kind)
	{
		case EXPR_KIND_NONE:
			Assert(false);		/* can't happen */
			break;
		case EXPR_KIND_OTHER:
			/* Accept SRF here; caller must throw error if wanted */
			break;
		case EXPR_KIND_JOIN_ON:
		case EXPR_KIND_JOIN_USING:
			err = _("set-returning functions are not allowed in JOIN conditions");
			break;
		case EXPR_KIND_FROM_SUBSELECT:
			/* can't get here, but just in case, throw an error */
			errkind = true;
			break;
		case EXPR_KIND_FROM_FUNCTION:
			/* okay, but we don't allow nested SRFs here */
			/* errmsg is chosen to match transformRangeFunction() */
			/* errposition should point to the inner SRF */
			if (pstate->p_last_srf != last_srf)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("set-returning functions must appear at top level of FROM"),
						 parser_errposition(pstate,
											exprLocation(pstate->p_last_srf))));
			break;
		case EXPR_KIND_WHERE:
			errkind = true;
			break;
		case EXPR_KIND_POLICY:
			err = _("set-returning functions are not allowed in policy expressions");
			break;
		case EXPR_KIND_HAVING:
			errkind = true;
			break;
		case EXPR_KIND_FILTER:
			errkind = true;
			break;
		case EXPR_KIND_WINDOW_PARTITION:
		case EXPR_KIND_WINDOW_ORDER:
			/* okay, these are effectively GROUP BY/ORDER BY */
			pstate->p_hasTargetSRFs = true;
			break;
		case EXPR_KIND_WINDOW_FRAME_RANGE:
		case EXPR_KIND_WINDOW_FRAME_ROWS:
		case EXPR_KIND_WINDOW_FRAME_GROUPS:
			err = _("set-returning functions are not allowed in window definitions");
			break;
		case EXPR_KIND_SELECT_TARGET:
		case EXPR_KIND_INSERT_TARGET:
			/* okay */
			pstate->p_hasTargetSRFs = true;
			break;
		case EXPR_KIND_UPDATE_SOURCE:
		case EXPR_KIND_UPDATE_TARGET:
			/* disallowed because it would be ambiguous what to do */
			errkind = true;
			break;
		case EXPR_KIND_GROUP_BY:
		case EXPR_KIND_ORDER_BY:
			/* okay */
			pstate->p_hasTargetSRFs = true;
			break;
		case EXPR_KIND_DISTINCT_ON:
			/* okay */
			pstate->p_hasTargetSRFs = true;
			break;
		case EXPR_KIND_LIMIT:
		case EXPR_KIND_OFFSET:
			errkind = true;
			break;
		case EXPR_KIND_RETURNING:
			errkind = true;
			break;
		case EXPR_KIND_VALUES:
			/* SRFs are presently not supported by nodeValuesscan.c */
			errkind = true;
			break;
		case EXPR_KIND_VALUES_SINGLE:
			/* okay, since we process this like a SELECT tlist */
			pstate->p_hasTargetSRFs = true;
			break;
		case EXPR_KIND_CHECK_CONSTRAINT:
		case EXPR_KIND_DOMAIN_CHECK:
			err = _("set-returning functions are not allowed in check constraints");
			break;
		case EXPR_KIND_COLUMN_DEFAULT:
		case EXPR_KIND_FUNCTION_DEFAULT:
			err = _("set-returning functions are not allowed in DEFAULT expressions");
			break;
		case EXPR_KIND_INDEX_EXPRESSION:
			err = _("set-returning functions are not allowed in index expressions");
			break;
		case EXPR_KIND_INDEX_PREDICATE:
			err = _("set-returning functions are not allowed in index predicates");
			break;
		case EXPR_KIND_ALTER_COL_TRANSFORM:
			err = _("set-returning functions are not allowed in transform expressions");
			break;
		case EXPR_KIND_EXECUTE_PARAMETER:
			err = _("set-returning functions are not allowed in EXECUTE parameters");
			break;
		case EXPR_KIND_TRIGGER_WHEN:
			err = _("set-returning functions are not allowed in trigger WHEN conditions");
			break;
		case EXPR_KIND_PARTITION_BOUND:
			err = _("set-returning functions are not allowed in partition bound");
			break;
		case EXPR_KIND_PARTITION_EXPRESSION:
			err = _("set-returning functions are not allowed in partition key expressions");
			break;
		case EXPR_KIND_CALL_ARGUMENT:
			err = _("set-returning functions are not allowed in CALL arguments");
			break;
		case EXPR_KIND_COPY_WHERE:
			err = _("set-returning functions are not allowed in COPY FROM WHERE conditions");
			break;
		case EXPR_KIND_GENERATED_COLUMN:
			err = _("set-returning functions are not allowed in column generation expressions");
			break;

			/*
			 * There is intentionally no default: case here, so that the
			 * compiler will warn if we add a new ParseExprKind without
			 * extending this switch.  If we do see an unrecognized value at
			 * runtime, the behavior will be the same as for EXPR_KIND_OTHER,
			 * which is sane anyway.
			 */
	}
	if (err)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg_internal("%s", err),
				 parser_errposition(pstate, location)));
	if (errkind)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is name of a SQL construct, eg GROUP BY */
				 errmsg("set-returning functions are not allowed in %s",
						ParseExprKindName(pstate->p_expr_kind)),
				 parser_errposition(pstate, location)));
}
