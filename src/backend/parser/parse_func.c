/*-------------------------------------------------------------------------
 *
 * parse_func.c
 *		handle function calls in parser
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/parse_func.c,v 1.201 2008/01/11 18:39:41 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static Oid	FuncNameAsType(List *funcname);
static Node *ParseComplexProjection(ParseState *pstate, char *funcname,
					   Node *first_arg, int location);
static void unknown_attribute(ParseState *pstate, Node *relref, char *attname,
				  int location);


/*
 *	Parse a function call
 *
 *	For historical reasons, Postgres tries to treat the notations tab.col
 *	and col(tab) as equivalent: if a single-argument function call has an
 *	argument of complex type and the (unqualified) function name matches
 *	any attribute of the type, we take it as a column projection.  Conversely
 *	a function of a single complex-type argument can be written like a
 *	column reference, allowing functions to act like computed columns.
 *
 *	Hence, both cases come through here.  The is_column parameter tells us
 *	which syntactic construct is actually being dealt with, but this is
 *	intended to be used only to deliver an appropriate error message,
 *	not to affect the semantics.  When is_column is true, we should have
 *	a single argument (the putative table), unqualified function name
 *	equal to the column name, and no aggregate decoration.
 *
 *	The argument expressions (in fargs) must have been transformed already.
 */
Node *
ParseFuncOrColumn(ParseState *pstate, List *funcname, List *fargs,
				  bool agg_star, bool agg_distinct, bool is_column,
				  int location)
{
	Oid			rettype;
	Oid			funcid;
	ListCell   *l;
	ListCell   *nextl;
	Node	   *first_arg = NULL;
	int			nargs;
	Oid			actual_arg_types[FUNC_MAX_ARGS];
	Oid		   *declared_arg_types;
	Node	   *retval;
	bool		retset;
	FuncDetailCode fdresult;

	/*
	 * Most of the rest of the parser just assumes that functions do not have
	 * more than FUNC_MAX_ARGS parameters.	We have to test here to protect
	 * against array overruns, etc.  Of course, this may not be a function,
	 * but the test doesn't hurt.
	 */
	if (list_length(fargs) > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("cannot pass more than %d arguments to a function",
						FUNC_MAX_ARGS),
				 parser_errposition(pstate, location)));

	/*
	 * Extract arg type info in preparation for function lookup.
	 *
	 * If any arguments are Param markers of type VOID, we discard them from
	 * the parameter list.	This is a hack to allow the JDBC driver to not
	 * have to distinguish "input" and "output" parameter symbols while
	 * parsing function-call constructs.  We can't use foreach() because we
	 * may modify the list ...
	 */
	nargs = 0;
	for (l = list_head(fargs); l != NULL; l = nextl)
	{
		Node	   *arg = lfirst(l);
		Oid			argtype = exprType(arg);

		nextl = lnext(l);

		if (argtype == VOIDOID && IsA(arg, Param) &&!is_column)
		{
			fargs = list_delete_ptr(fargs, arg);
			continue;
		}

		actual_arg_types[nargs++] = argtype;
	}

	if (fargs)
	{
		first_arg = linitial(fargs);
		Assert(first_arg != NULL);
	}

	/*
	 * Check for column projection: if function has one argument, and that
	 * argument is of complex type, and function name is not qualified, then
	 * the "function call" could be a projection.  We also check that there
	 * wasn't any aggregate decoration.
	 */
	if (nargs == 1 && !agg_star && !agg_distinct && list_length(funcname) == 1)
	{
		Oid			argtype = actual_arg_types[0];

		if (argtype == RECORDOID || ISCOMPLEX(argtype))
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
	}

	/*
	 * Okay, it's not a column projection, so it must really be a function.
	 * func_get_detail looks up the function in the catalogs, does
	 * disambiguation for polymorphic functions, handles inheritance, and
	 * returns the funcid and type and set or singleton status of the
	 * function's return value.  it also returns the true argument types to
	 * the function.
	 */
	fdresult = func_get_detail(funcname, fargs, nargs, actual_arg_types,
							   &funcid, &rettype, &retset,
							   &declared_arg_types);
	if (fdresult == FUNCDETAIL_COERCION)
	{
		/*
		 * We interpreted it as a type coercion. coerce_type can handle these
		 * cases, so why duplicate code...
		 */
		return coerce_type(pstate, linitial(fargs),
						   actual_arg_types[0], rettype, -1,
						   COERCION_EXPLICIT, COERCE_EXPLICIT_CALL);
	}
	else if (fdresult == FUNCDETAIL_NORMAL)
	{
		/*
		 * Normal function found; was there anything indicating it must be an
		 * aggregate?
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
	}
	else if (fdresult != FUNCDETAIL_AGGREGATE)
	{
		/*
		 * Oops.  Time to die.
		 *
		 * If we are dealing with the attribute notation rel.function, give an
		 * error message that is appropriate for that case.
		 */
		if (is_column)
		{
			Assert(nargs == 1);
			Assert(list_length(funcname) == 1);
			unknown_attribute(pstate, first_arg, strVal(linitial(funcname)),
							  location);
		}

		/*
		 * Else generate a detailed complaint for a function
		 */
		if (fdresult == FUNCDETAIL_MULTIPLE)
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("function %s is not unique",
							func_signature_string(funcname, nargs,
												  actual_arg_types)),
					 errhint("Could not choose a best candidate function. "
							 "You might need to add explicit type casts."),
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(funcname, nargs,
												  actual_arg_types)),
			errhint("No function matches the given name and argument types. "
					"You might need to add explicit type casts."),
					 parser_errposition(pstate, location)));
	}

	/*
	 * enforce consistency with polymorphic argument and return types,
	 * possibly adjusting return type or declared_arg_types (which will be
	 * used as the cast destination by make_fn_arguments)
	 */
	rettype = enforce_generic_type_consistency(actual_arg_types,
											   declared_arg_types,
											   nargs,
											   rettype,
											   false);

	/* perform the necessary typecasting of arguments */
	make_fn_arguments(pstate, fargs, actual_arg_types, declared_arg_types);

	/* build the appropriate output structure */
	if (fdresult == FUNCDETAIL_NORMAL)
	{
		FuncExpr   *funcexpr = makeNode(FuncExpr);

		funcexpr->funcid = funcid;
		funcexpr->funcresulttype = rettype;
		funcexpr->funcretset = retset;
		funcexpr->funcformat = COERCE_EXPLICIT_CALL;
		funcexpr->args = fargs;

		retval = (Node *) funcexpr;
	}
	else
	{
		/* aggregate function */
		Aggref	   *aggref = makeNode(Aggref);

		aggref->aggfnoid = funcid;
		aggref->aggtype = rettype;
		aggref->args = fargs;
		aggref->aggstar = agg_star;
		aggref->aggdistinct = agg_distinct;

		/*
		 * Reject attempt to call a parameterless aggregate without (*)
		 * syntax.	This is mere pedantry but some folks insisted ...
		 */
		if (fargs == NIL && !agg_star)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("%s(*) must be used to call a parameterless aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));

		/* parse_agg.c does additional aggregate-specific processing */
		transformAggregateCall(pstate, aggref);

		retval = (Node *) aggref;

		if (retset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregates cannot return sets"),
					 parser_errposition(pstate, location)));
	}

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
					FuncCandidateList *candidates)		/* return value */
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
}	/* func_match_argtypes() */


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
	FuncCandidateList current_candidate;
	FuncCandidateList last_candidate;
	Oid		   *current_typeids;
	Oid			current_type;
	int			i;
	int			ncandidates;
	int			nbestMatch,
				nmatch;
	Oid			input_base_typeids[FUNC_MAX_ARGS];
	CATEGORY	slot_category[FUNC_MAX_ARGS],
				current_category;
	bool		slot_has_preferred_type[FUNC_MAX_ARGS];
	bool		resolved_unknowns;

	/* protect local fixed-size arrays */
	if (nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("cannot pass more than %d arguments to a function",
						FUNC_MAX_ARGS)));

	/*
	 * If any input types are domains, reduce them to their base types. This
	 * ensures that we will consider functions on the base type to be "exact
	 * matches" in the exact-match heuristic; it also makes it possible to do
	 * something useful with the type-category heuristics. Note that this
	 * makes it difficult, but not impossible, to use functions declared to
	 * take a domain as an input datatype.	Such a function will be selected
	 * over the base-type function only if it is an exact match at all
	 * argument positions, and so was already chosen by our caller.
	 */
	for (i = 0; i < nargs; i++)
		input_base_typeids[i] = getBaseType(input_typeids[i]);

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
	 * Still too many candidates? Try assigning types for the unknown columns.
	 *
	 * NOTE: for a binary operator with one unknown and one non-unknown input,
	 * we already tried the heuristic of looking for a candidate with the
	 * known input type on both sides (see binary_oper_exact()). That's
	 * essentially a special case of the general algorithm we try next.
	 *
	 * We do this by examining each unknown argument position to see if we can
	 * determine a "type category" for it.	If any candidate has an input
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
	 * wrong category at any unknown position.	Also, if at least one
	 * candidate accepted a preferred type at a position, remove candidates
	 * that accept non-preferred types.
	 *
	 * If we are down to one candidate at the end, we win.
	 */
	resolved_unknowns = false;
	for (i = 0; i < nargs; i++)
	{
		bool		have_conflict;

		if (input_base_typeids[i] != UNKNOWNOID)
			continue;
		resolved_unknowns = true;		/* assume we can do it */
		slot_category[i] = INVALID_TYPE;
		slot_has_preferred_type[i] = false;
		have_conflict = false;
		for (current_candidate = candidates;
			 current_candidate != NULL;
			 current_candidate = current_candidate->next)
		{
			current_typeids = current_candidate->args;
			current_type = current_typeids[i];
			current_category = TypeCategory(current_type);
			if (slot_category[i] == INVALID_TYPE)
			{
				/* first candidate */
				slot_category[i] = current_category;
				slot_has_preferred_type[i] =
					IsPreferredType(current_category, current_type);
			}
			else if (current_category == slot_category[i])
			{
				/* more candidates in same category */
				slot_has_preferred_type[i] |=
					IsPreferredType(current_category, current_type);
			}
			else
			{
				/* category conflict! */
				if (current_category == STRING_TYPE)
				{
					/* STRING always wins if available */
					slot_category[i] = current_category;
					slot_has_preferred_type[i] =
						IsPreferredType(current_category, current_type);
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
		if (have_conflict && slot_category[i] != STRING_TYPE)
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
				current_category = TypeCategory(current_type);
				if (current_category != slot_category[i])
				{
					keepit = false;
					break;
				}
				if (slot_has_preferred_type[i] &&
					!IsPreferredType(current_category, current_type))
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
					candidates = current_candidate->next;
			}
		}
		if (last_candidate)		/* terminate rebuilt list */
			last_candidate->next = NULL;
	}

	if (ncandidates == 1)
		return candidates;

	return NULL;				/* failed to select a best candidate */
}	/* func_select_candidate() */


/* func_get_detail()
 *
 * Find the named function in the system catalogs.
 *
 * Attempt to find the named function in the system catalogs with
 *	arguments exactly as specified, so that the normal case
 *	(exact match) is as quick as possible.
 *
 * If an exact match isn't found:
 *	1) check for possible interpretation as a type coercion request
 *	2) get a vector of all possible input arg type arrays constructed
 *	   from the superclasses of the original input arg types
 *	3) get a list of all possible argument type arrays to the function
 *	   with given name and number of arguments
 *	4) for each input arg type array from vector #1:
 *	 a) find how many of the function arg type arrays from list #2
 *		it can be coerced to
 *	 b) if the answer is one, we have our function
 *	 c) if the answer is more than one, attempt to resolve the conflict
 *	 d) if the answer is zero, try the next array from vector #1
 *
 * Note: we rely primarily on nargs/argtypes as the argument description.
 * The actual expression node list is passed in fargs so that we can check
 * for type coercion of a constant.  Some callers pass fargs == NIL
 * indicating they don't want that check made.
 */
FuncDetailCode
func_get_detail(List *funcname,
				List *fargs,
				int nargs,
				Oid *argtypes,
				Oid *funcid,	/* return value */
				Oid *rettype,	/* return value */
				bool *retset,	/* return value */
				Oid **true_typeids)		/* return value */
{
	FuncCandidateList raw_candidates;
	FuncCandidateList best_candidate;

	/* Get list of possible candidates from namespace search */
	raw_candidates = FuncnameGetCandidates(funcname, nargs);

	/*
	 * Quickly check if there is an exact match to the input datatypes (there
	 * can be only one)
	 */
	for (best_candidate = raw_candidates;
		 best_candidate != NULL;
		 best_candidate = best_candidate->next)
	{
		if (memcmp(argtypes, best_candidate->args, nargs * sizeof(Oid)) == 0)
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
		 * write it as a normal cast instead.  Lack of historical support is
		 * also the reason for not considering composite-type casts here.
		 *
		 * NB: it's important that this code does not exceed what coerce_type
		 * can do, because the caller will try to apply coerce_type if we
		 * return FUNCDETAIL_COERCION.	If we return that result for something
		 * coerce_type can't handle, we'll cause infinite recursion between
		 * this module and coerce_type!
		 */
		if (nargs == 1 && fargs != NIL)
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
					iscoercion = (cpathtype == COERCION_PATH_RELABELTYPE ||
								  cpathtype == COERCION_PATH_COERCEVIAIO);
				}

				if (iscoercion)
				{
					/* Treat it as a type coercion */
					*funcid = InvalidOid;
					*rettype = targetType;
					*retset = false;
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

		*funcid = best_candidate->oid;
		*true_typeids = best_candidate->args;

		ftup = SearchSysCache(PROCOID,
							  ObjectIdGetDatum(best_candidate->oid),
							  0, 0, 0);
		if (!HeapTupleIsValid(ftup))	/* should not happen */
			elog(ERROR, "cache lookup failed for function %u",
				 best_candidate->oid);
		pform = (Form_pg_proc) GETSTRUCT(ftup);
		*rettype = pform->prorettype;
		*retset = pform->proretset;
		result = pform->proisagg ? FUNCDETAIL_AGGREGATE : FUNCDETAIL_NORMAL;
		ReleaseSysCache(ftup);
		return result;
	}

	return FUNCDETAIL_NOTFOUND;
}


/*
 * Given two type OIDs, determine whether the first is a complex type
 * (class type) that inherits from the second.
 */
bool
typeInheritsFrom(Oid subclassTypeId, Oid superclassTypeId)
{
	bool		result = false;
	Oid			relid;
	Relation	inhrel;
	List	   *visited,
			   *queue;
	ListCell   *queue_item;

	if (!ISCOMPLEX(subclassTypeId) || !ISCOMPLEX(superclassTypeId))
		return false;
	relid = typeidTypeRelid(subclassTypeId);
	if (relid == InvalidOid)
		return false;

	/*
	 * Begin the search at the relation itself, so add relid to the queue.
	 */
	queue = list_make1_oid(relid);
	visited = NIL;

	inhrel = heap_open(InheritsRelationId, AccessShareLock);

	/*
	 * Use queue to do a breadth-first traversal of the inheritance graph from
	 * the relid supplied up to the root.  Notice that we append to the queue
	 * inside the loop --- this is okay because the foreach() macro doesn't
	 * advance queue_item until the next loop iteration begins.
	 */
	foreach(queue_item, queue)
	{
		Oid			this_relid = lfirst_oid(queue_item);
		ScanKeyData skey;
		HeapScanDesc inhscan;
		HeapTuple	inhtup;

		/* If we've seen this relid already, skip it */
		if (list_member_oid(visited, this_relid))
			continue;

		/*
		 * Okay, this is a not-yet-seen relid. Add it to the list of
		 * already-visited OIDs, then find all the types this relid inherits
		 * from and add them to the queue. The one exception is we don't add
		 * the original relation to 'visited'.
		 */
		if (queue_item != list_head(queue))
			visited = lappend_oid(visited, this_relid);

		ScanKeyInit(&skey,
					Anum_pg_inherits_inhrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(this_relid));

		inhscan = heap_beginscan(inhrel, SnapshotNow, 1, &skey);

		while ((inhtup = heap_getnext(inhscan, ForwardScanDirection)) != NULL)
		{
			Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(inhtup);
			Oid			inhparent = inh->inhparent;

			/* If this is the target superclass, we're done */
			if (get_rel_type_id(inhparent) == superclassTypeId)
			{
				result = true;
				break;
			}

			/* Else add to queue */
			queue = lappend_oid(queue, inhparent);
		}

		heap_endscan(inhscan);

		if (result)
			break;
	}

	heap_close(inhrel, AccessShareLock);

	list_free(visited);
	list_free(queue);

	return result;
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
			lfirst(current_fargs) = coerce_type(pstate,
												lfirst(current_fargs),
												actual_arg_types[i],
												declared_arg_types[i], -1,
												COERCION_IMPLICIT,
												COERCE_IMPLICIT_CAST);
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

	typtup = LookupTypeName(NULL, makeTypeNameFromNameList(funcname), NULL);
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
 *	  transformed expression tree.	If not, return NULL.
 */
static Node *
ParseComplexProjection(ParseState *pstate, char *funcname, Node *first_arg,
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
		RangeTblEntry *rte;

		rte = GetRTEByRangeTablePosn(pstate,
									 ((Var *) first_arg)->varno,
									 ((Var *) first_arg)->varlevelsup);
		/* Return a Var if funcname matches a column, else NULL */
		return scanRTEForColumn(pstate, rte, funcname, location);
	}

	/*
	 * Else do it the hard way with get_expr_result_type().
	 *
	 * If it's a Var of type RECORD, we have to work even harder: we have to
	 * find what the Var refers to, and pass that to get_expr_result_type.
	 * That task is handled by expandRecordVariable().
	 */
	if (IsA(first_arg, Var) &&
		((Var *) first_arg)->vartype == RECORDOID)
		tupdesc = expandRecordVariable(pstate, (Var *) first_arg, 0);
	else if (get_expr_result_type(first_arg, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		return NULL;			/* unresolvable RECORD type */
	Assert(tupdesc);

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = tupdesc->attrs[i];

		if (strcmp(funcname, NameStr(att->attname)) == 0 &&
			!att->attisdropped)
		{
			/* Success, so generate a FieldSelect expression */
			FieldSelect *fselect = makeNode(FieldSelect);

			fselect->arg = (Expr *) first_arg;
			fselect->fieldnum = i + 1;
			fselect->resulttype = att->atttypid;
			fselect->resulttypmod = att->atttypmod;
			return (Node *) fselect;
		}
	}

	return NULL;				/* funcname does not match any column */
}

/*
 * helper routine for delivering "column does not exist" error message
 */
static void
unknown_attribute(ParseState *pstate, Node *relref, char *attname,
				  int location)
{
	RangeTblEntry *rte;

	if (IsA(relref, Var) &&
		((Var *) relref)->varattno == InvalidAttrNumber)
	{
		/* Reference the RTE by alias not by actual table name */
		rte = GetRTEByRangeTablePosn(pstate,
									 ((Var *) relref)->varno,
									 ((Var *) relref)->varlevelsup);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column %s.%s does not exist",
						rte->eref->aliasname, attname),
				 parser_errposition(pstate, location)));
	}
	else
	{
		/* Have to do it by reference to the type of the expression */
		Oid			relTypeId = exprType(relref);

		if (ISCOMPLEX(relTypeId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" not found in data type %s",
							attname, format_type_be(relTypeId)),
					 parser_errposition(pstate, location)));
		else if (relTypeId == RECORDOID)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
			   errmsg("could not identify column \"%s\" in record data type",
					  attname),
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("column notation .%s applied to type %s, "
							"which is not a composite type",
							attname, format_type_be(relTypeId)),
					 parser_errposition(pstate, location)));
	}
}

/*
 * funcname_signature_string
 *		Build a string representing a function name, including arg types.
 *		The result is something like "foo(integer)".
 *
 * This is typically used in the construction of function-not-found error
 * messages.
 */
const char *
funcname_signature_string(const char *funcname,
						  int nargs, const Oid *argtypes)
{
	StringInfoData argbuf;
	int			i;

	initStringInfo(&argbuf);

	appendStringInfo(&argbuf, "%s(", funcname);

	for (i = 0; i < nargs; i++)
	{
		if (i)
			appendStringInfoString(&argbuf, ", ");
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
func_signature_string(List *funcname, int nargs, const Oid *argtypes)
{
	return funcname_signature_string(NameListToString(funcname),
									 nargs, argtypes);
}

/*
 * LookupFuncName
 *		Given a possibly-qualified function name and a set of argument types,
 *		look up the function.
 *
 * If the function name is not schema-qualified, it is sought in the current
 * namespace search path.
 *
 * If the function is not found, we return InvalidOid if noError is true,
 * else raise an error.
 */
Oid
LookupFuncName(List *funcname, int nargs, const Oid *argtypes, bool noError)
{
	FuncCandidateList clist;

	clist = FuncnameGetCandidates(funcname, nargs);

	while (clist)
	{
		if (memcmp(argtypes, clist->args, nargs * sizeof(Oid)) == 0)
			return clist->oid;
		clist = clist->next;
	}

	if (!noError)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(funcname, nargs, argtypes))));

	return InvalidOid;
}

/*
 * LookupTypeNameOid
 *		Convenience routine to look up a type, silently accepting shell types
 */
static Oid
LookupTypeNameOid(const TypeName *typename)
{
	Oid			result;
	Type		typtup;

	typtup = LookupTypeName(NULL, typename, NULL);
	if (typtup == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist",
						TypeNameToString(typename))));
	result = typeTypeId(typtup);
	ReleaseSysCache(typtup);
	return result;
}

/*
 * LookupFuncNameTypeNames
 *		Like LookupFuncName, but the argument types are specified by a
 *		list of TypeName nodes.
 */
Oid
LookupFuncNameTypeNames(List *funcname, List *argtypes, bool noError)
{
	Oid			argoids[FUNC_MAX_ARGS];
	int			argcount;
	int			i;
	ListCell   *args_item;

	argcount = list_length(argtypes);
	if (argcount > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("functions cannot have more than %d arguments",
						FUNC_MAX_ARGS)));

	args_item = list_head(argtypes);
	for (i = 0; i < argcount; i++)
	{
		TypeName   *t = (TypeName *) lfirst(args_item);

		argoids[i] = LookupTypeNameOid(t);
		args_item = lnext(args_item);
	}

	return LookupFuncName(funcname, argcount, argoids, noError);
}

/*
 * LookupAggNameTypeNames
 *		Find an aggregate function given a name and list of TypeName nodes.
 *
 * This is almost like LookupFuncNameTypeNames, but the error messages refer
 * to aggregates rather than plain functions, and we verify that the found
 * function really is an aggregate.
 */
Oid
LookupAggNameTypeNames(List *aggname, List *argtypes, bool noError)
{
	Oid			argoids[FUNC_MAX_ARGS];
	int			argcount;
	int			i;
	ListCell   *lc;
	Oid			oid;
	HeapTuple	ftup;
	Form_pg_proc pform;

	argcount = list_length(argtypes);
	if (argcount > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("functions cannot have more than %d arguments",
						FUNC_MAX_ARGS)));

	i = 0;
	foreach(lc, argtypes)
	{
		TypeName   *t = (TypeName *) lfirst(lc);

		argoids[i] = LookupTypeNameOid(t);
		i++;
	}

	oid = LookupFuncName(aggname, argcount, argoids, true);

	if (!OidIsValid(oid))
	{
		if (noError)
			return InvalidOid;
		if (argcount == 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("aggregate %s(*) does not exist",
							NameListToString(aggname))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("aggregate %s does not exist",
							func_signature_string(aggname,
												  argcount, argoids))));
	}

	/* Make sure it's an aggregate */
	ftup = SearchSysCache(PROCOID,
						  ObjectIdGetDatum(oid),
						  0, 0, 0);
	if (!HeapTupleIsValid(ftup))	/* should not happen */
		elog(ERROR, "cache lookup failed for function %u", oid);
	pform = (Form_pg_proc) GETSTRUCT(ftup);

	if (!pform->proisagg)
	{
		ReleaseSysCache(ftup);
		if (noError)
			return InvalidOid;
		/* we do not use the (*) notation for functions... */
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s is not an aggregate",
						func_signature_string(aggname,
											  argcount, argoids))));
	}

	ReleaseSysCache(ftup);

	return oid;
}
