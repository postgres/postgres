/*-------------------------------------------------------------------------
 *
 * parse_func.c
 *		handle function calls in parser
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_func.c,v 1.161 2003/09/29 00:05:25 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static Node *ParseComplexProjection(char *funcname, Node *first_arg);
static Oid **argtype_inherit(int nargs, Oid *argtypes);

static int	find_inheritors(Oid relid, Oid **supervec);
static Oid **gen_cross_product(InhPaths *arginh, int nargs);
static FieldSelect *setup_field_select(Node *input, char *attname, Oid relid);
static void unknown_attribute(const char *schemaname, const char *relname,
				  const char *attname);


/*
 *	Parse a function call
 *
 *	For historical reasons, Postgres tries to treat the notations tab.col
 *	and col(tab) as equivalent: if a single-argument function call has an
 *	argument of complex type and the (unqualified) function name matches
 *	any attribute of the type, we take it as a column projection.
 *
 *	Hence, both cases come through here.  The is_column parameter tells us
 *	which syntactic construct is actually being dealt with, but this is
 *	intended to be used only to deliver an appropriate error message,
 *	not to affect the semantics.  When is_column is true, we should have
 *	a single argument (the putative table), unqualified function name
 *	equal to the column name, and no aggregate decoration.
 *
 *	In the function-call case, the argument expressions have been transformed
 *	already.  In the column case, we may get either a transformed expression
 *	or a RangeVar node as argument.
 */
Node *
ParseFuncOrColumn(ParseState *pstate, List *funcname, List *fargs,
				  bool agg_star, bool agg_distinct, bool is_column)
{
	Oid			rettype;
	Oid			funcid;
	List	   *i;
	Node	   *first_arg = NULL;
	int			nargs = length(fargs);
	int			argn;
	Oid			actual_arg_types[FUNC_MAX_ARGS];
	Oid		   *declared_arg_types;
	Node	   *retval;
	bool		retset;
	FuncDetailCode fdresult;

	/*
	 * Most of the rest of the parser just assumes that functions do not
	 * have more than FUNC_MAX_ARGS parameters.  We have to test here to
	 * protect against array overruns, etc.  Of course, this may not be a
	 * function, but the test doesn't hurt.
	 */
	if (nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
			   errmsg("cannot pass more than %d arguments to a function",
					  FUNC_MAX_ARGS)));

	if (fargs)
	{
		first_arg = lfirst(fargs);
		Assert(first_arg != NULL);
	}

	/*
	 * check for column projection: if function has one argument, and that
	 * argument is of complex type, and function name is not qualified,
	 * then the "function call" could be a projection.	We also check that
	 * there wasn't any aggregate decoration.
	 */
	if (nargs == 1 && !agg_star && !agg_distinct && length(funcname) == 1)
	{
		char	   *cname = strVal(lfirst(funcname));

		/* Is it a not-yet-transformed RangeVar node? */
		if (IsA(first_arg, RangeVar))
		{
			/* First arg is a relation. This could be a projection. */
			retval = qualifiedNameToVar(pstate,
									((RangeVar *) first_arg)->schemaname,
										((RangeVar *) first_arg)->relname,
										cname,
										true);
			if (retval)
				return retval;
		}
		else if (ISCOMPLEX(exprType(first_arg)))
		{
			/*
			 * Attempt to handle projection of a complex argument. If
			 * ParseComplexProjection can't handle the projection, we have
			 * to keep going.
			 */
			retval = ParseComplexProjection(cname, first_arg);
			if (retval)
				return retval;
		}
	}

	/*
	 * Okay, it's not a column projection, so it must really be a
	 * function. Extract arg type info and transform RangeVar arguments
	 * into varnodes of the appropriate form.
	 */
	MemSet(actual_arg_types, 0, FUNC_MAX_ARGS * sizeof(Oid));

	argn = 0;
	foreach(i, fargs)
	{
		Node	   *arg = lfirst(i);
		Oid			toid;

		if (IsA(arg, RangeVar))
		{
			char	   *schemaname;
			char	   *relname;
			RangeTblEntry *rte;
			int			vnum;
			int			sublevels_up;

			/*
			 * a relation: look it up in the range table, or add if needed
			 */
			schemaname = ((RangeVar *) arg)->schemaname;
			relname = ((RangeVar *) arg)->relname;

			rte = refnameRangeTblEntry(pstate, schemaname, relname,
									   &sublevels_up);

			if (rte == NULL)
				rte = addImplicitRTE(pstate, (RangeVar *) arg);

			vnum = RTERangeTablePosn(pstate, rte, &sublevels_up);

			/*
			 * The parameter to be passed to the function is the whole
			 * tuple from the relation.  We build a special VarNode to
			 * reflect this -- it has varno set to the correct range table
			 * entry, but has varattno == 0 to signal that the whole tuple
			 * is the argument.  Also, it has typmod set to
			 * sizeof(Pointer) to signal that the runtime representation
			 * will be a pointer not an Oid.
			 */
			switch (rte->rtekind)
			{
				case RTE_RELATION:
					toid = get_rel_type_id(rte->relid);
					if (!OidIsValid(toid))
						elog(ERROR, "could not find type OID for relation %u",
							 rte->relid);
					/* replace RangeVar in the arg list */
					lfirst(i) = makeVar(vnum,
										InvalidAttrNumber,
										toid,
										sizeof(Pointer),
										sublevels_up);
					break;
				case RTE_FUNCTION:
					toid = exprType(rte->funcexpr);
					if (get_typtype(toid) == 'c')
					{
						/* func returns composite; same as relation case */
						lfirst(i) = makeVar(vnum,
											InvalidAttrNumber,
											toid,
											sizeof(Pointer),
											sublevels_up);
					}
					else
					{
						/* func returns scalar; use attno 1 instead */
						lfirst(i) = makeVar(vnum,
											1,
											toid,
											-1,
											sublevels_up);
					}
					break;
				default:

					/*
					 * RTE is a join or subselect; must fail for lack of a
					 * named tuple type
					 */
					if (is_column)
						unknown_attribute(schemaname, relname,
										  strVal(lfirst(funcname)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot pass result of subquery or join \"%s\" to a function",
										relname)));
					toid = InvalidOid;	/* keep compiler quiet */
					break;
			}
		}
		else
			toid = exprType(arg);

		actual_arg_types[argn++] = toid;
	}

	/*
	 * func_get_detail looks up the function in the catalogs, does
	 * disambiguation for polymorphic functions, handles inheritance, and
	 * returns the funcid and type and set or singleton status of the
	 * function's return value.  it also returns the true argument types
	 * to the function.
	 */
	fdresult = func_get_detail(funcname, fargs, nargs, actual_arg_types,
							   &funcid, &rettype, &retset,
							   &declared_arg_types);
	if (fdresult == FUNCDETAIL_COERCION)
	{
		/*
		 * We can do it as a trivial coercion. coerce_type can handle
		 * these cases, so why duplicate code...
		 */
		return coerce_type(pstate, lfirst(fargs), actual_arg_types[0],
						   rettype,
						   COERCION_EXPLICIT, COERCE_EXPLICIT_CALL);
	}
	else if (fdresult == FUNCDETAIL_NORMAL)
	{
		/*
		 * Normal function found; was there anything indicating it must be
		 * an aggregate?
		 */
		if (agg_star)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			errmsg("%s(*) specified, but %s is not an aggregate function",
				   NameListToString(funcname),
				   NameListToString(funcname))));
		if (agg_distinct)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("DISTINCT specified, but %s is not an aggregate function",
							NameListToString(funcname))));
	}
	else if (fdresult != FUNCDETAIL_AGGREGATE)
	{
		/*
		 * Oops.  Time to die.
		 *
		 * If we are dealing with the attribute notation rel.function, give
		 * an error message that is appropriate for that case.
		 */
		if (is_column)
		{
			char	   *colname = strVal(lfirst(funcname));
			Oid			relTypeId;

			Assert(nargs == 1);
			if (IsA(first_arg, RangeVar))
				unknown_attribute(((RangeVar *) first_arg)->schemaname,
								  ((RangeVar *) first_arg)->relname,
								  colname);
			relTypeId = exprType(first_arg);
			if (!ISCOMPLEX(relTypeId))
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("attribute notation .%s applied to type %s, which is not a complex type",
								colname, format_type_be(relTypeId))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
					  errmsg("attribute \"%s\" not found in data type %s",
							 colname, format_type_be(relTypeId))));
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
						   "You may need to add explicit type casts.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(funcname, nargs,
												  actual_arg_types)),
					 errhint("No function matches the given name and argument types. "
							 "You may need to add explicit type casts.")));
	}

	/*
	 * enforce consistency with ANYARRAY and ANYELEMENT argument and
	 * return types, possibly adjusting return type or declared_arg_types
	 * (which will be used as the cast destination by make_fn_arguments)
	 */
	rettype = enforce_generic_type_consistency(actual_arg_types,
											   declared_arg_types,
											   nargs,
											   rettype);

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
		aggref->target = lfirst(fargs);
		aggref->aggstar = agg_star;
		aggref->aggdistinct = agg_distinct;

		/* parse_agg.c does additional aggregate-specific processing */
		transformAggregateCall(pstate, aggref);

		retval = (Node *) aggref;

		if (retset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregates may not return sets")));
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

	/*
	 * If any input types are domains, reduce them to their base types.
	 * This ensures that we will consider functions on the base type to be
	 * "exact matches" in the exact-match heuristic; it also makes it
	 * possible to do something useful with the type-category heuristics.
	 * Note that this makes it difficult, but not impossible, to use
	 * functions declared to take a domain as an input datatype.  Such a
	 * function will be selected over the base-type function only if it is
	 * an exact match at all argument positions, and so was already chosen
	 * by our caller.
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
	 * Still too many candidates? Now look for candidates which have
	 * either exact matches or preferred types at the args that will
	 * require coercion. (Restriction added in 7.4: preferred type must be
	 * of same category as input type; give no preference to
	 * cross-category conversions to preferred types.)	Keep all
	 * candidates if none match.
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
	 * Still too many candidates? Try assigning types for the unknown
	 * columns.
	 *
	 * NOTE: for a binary operator with one unknown and one non-unknown
	 * input, we already tried the heuristic of looking for a candidate
	 * with the known input type on both sides (see binary_oper_exact()).
	 * That's essentially a special case of the general algorithm we try
	 * next.
	 *
	 * We do this by examining each unknown argument position to see if we
	 * can determine a "type category" for it.	If any candidate has an
	 * input datatype of STRING category, use STRING category (this bias
	 * towards STRING is appropriate since unknown-type literals look like
	 * strings).  Otherwise, if all the candidates agree on the type
	 * category of this argument position, use that category.  Otherwise,
	 * fail because we cannot determine a category.
	 *
	 * If we are able to determine a type category, also notice whether any
	 * of the candidates takes a preferred datatype within the category.
	 *
	 * Having completed this examination, remove candidates that accept the
	 * wrong category at any unknown position.	Also, if at least one
	 * candidate accepted a preferred type at a position, remove
	 * candidates that accept non-preferred types.
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
					 * Remember conflict, but keep going (might find
					 * STRING)
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
 *	1) check for possible interpretation as a trivial type coercion
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
	 * Quickly check if there is an exact match to the input datatypes
	 * (there can be only one)
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
		 * function call where the function name is a type name.  If so,
		 * and if we can do the coercion trivially (no run-time function
		 * call needed), then go ahead and treat the "function call" as a
		 * coercion.  This interpretation needs to be given higher
		 * priority than interpretations involving a type coercion
		 * followed by a function call, otherwise we can produce
		 * surprising results. For example, we want "text(varchar)" to be
		 * interpreted as a trivial coercion, not as "text(name(varchar))"
		 * which the code below this point is entirely capable of
		 * selecting.
		 *
		 * "Trivial" coercions are ones that involve binary-compatible types
		 * and ones that are coercing a previously-unknown-type literal
		 * constant to a specific type.
		 *
		 * The reason we can restrict our check to binary-compatible
		 * coercions here is that we expect non-binary-compatible
		 * coercions to have an implementation function named after the
		 * target type. That function will be found by normal lookup if
		 * appropriate.
		 *
		 * NB: it's important that this code stays in sync with what
		 * coerce_type can do, because the caller will try to apply
		 * coerce_type if we return FUNCDETAIL_COERCION.  If we return
		 * that result for something coerce_type can't handle, we'll cause
		 * infinite recursion between this module and coerce_type!
		 */
		if (nargs == 1 && fargs != NIL)
		{
			Oid			targetType;
			TypeName   *tn = makeNode(TypeName);

			tn->names = funcname;
			tn->typmod = -1;
			targetType = LookupTypeName(tn);
			if (OidIsValid(targetType) &&
				!ISCOMPLEX(targetType))
			{
				Oid			sourceType = argtypes[0];
				Node	   *arg1 = lfirst(fargs);

				if ((sourceType == UNKNOWNOID && IsA(arg1, Const)) ||
					(find_coercion_pathway(targetType, sourceType,
										   COERCION_EXPLICIT, funcid) &&
					 *funcid == InvalidOid))
				{
					/* Yup, it's a type coercion */
					*funcid = InvalidOid;
					*rettype = targetType;
					*retset = false;
					*true_typeids = argtypes;
					return FUNCDETAIL_COERCION;
				}
			}
		}

		/*
		 * didn't find an exact match, so now try to match up
		 * candidates...
		 */
		if (raw_candidates != NULL)
		{
			Oid		  **input_typeid_vector = NULL;
			Oid		   *current_input_typeids;

			/*
			 * First we will search with the given argtypes, then with
			 * variants based on replacing complex types with their
			 * inheritance ancestors.  Stop as soon as any match is found.
			 */
			current_input_typeids = argtypes;

			do
			{
				FuncCandidateList current_candidates;
				int			ncandidates;

				ncandidates = func_match_argtypes(nargs,
												  current_input_typeids,
												  raw_candidates,
												  &current_candidates);

				/* one match only? then run with it... */
				if (ncandidates == 1)
				{
					best_candidate = current_candidates;
					break;
				}

				/*
				 * multiple candidates? then better decide or throw an
				 * error...
				 */
				if (ncandidates > 1)
				{
					best_candidate = func_select_candidate(nargs,
												   current_input_typeids,
													 current_candidates);

					/*
					 * If we were able to choose a best candidate, we're
					 * done.  Otherwise, ambiguous function call.
					 */
					if (best_candidate)
						break;
					return FUNCDETAIL_MULTIPLE;
				}

				/*
				 * No match here, so try the next inherited type vector.
				 * First time through, we need to compute the list of
				 * vectors.
				 */
				if (input_typeid_vector == NULL)
					input_typeid_vector = argtype_inherit(nargs, argtypes);

				current_input_typeids = *input_typeid_vector++;
			}
			while (current_input_typeids != NULL);
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
 *	argtype_inherit() -- Construct an argtype vector reflecting the
 *						 inheritance properties of the supplied argv.
 *
 *		This function is used to handle resolution of function calls when
 *		there is no match to the given argument types, but there might be
 *		matches based on considering complex types as members of their
 *		superclass types (parent classes).
 *
 *		It takes an array of input type ids.  For each type id in the array
 *		that's a complex type (a class), it walks up the inheritance tree,
 *		finding all superclasses of that type. A vector of new Oid type
 *		arrays is returned to the caller, listing possible alternative
 *		interpretations of the input typeids as members of their superclasses
 *		rather than the actually given argument types.	The vector is
 *		terminated by a NULL pointer.
 *
 *		The order of this vector is as follows:  all superclasses of the
 *		rightmost complex class are explored first.  The exploration
 *		continues from right to left.  This policy means that we favor
 *		keeping the leftmost argument type as low in the inheritance tree
 *		as possible.  This is intentional; it is exactly what we need to
 *		do for method dispatch.
 *
 *		The vector does not include the case where no complex classes have
 *		been promoted, since that was already tried before this routine
 *		got called.
 */
static Oid **
argtype_inherit(int nargs, Oid *argtypes)
{
	Oid			relid;
	int			i;
	InhPaths	arginh[FUNC_MAX_ARGS];

	for (i = 0; i < nargs; i++)
	{
		arginh[i].self = argtypes[i];
		if ((relid = typeidTypeRelid(argtypes[i])) != InvalidOid)
			arginh[i].nsupers = find_inheritors(relid, &(arginh[i].supervec));
		else
		{
			arginh[i].nsupers = 0;
			arginh[i].supervec = (Oid *) NULL;
		}
	}

	/* return an ordered cross-product of the classes involved */
	return gen_cross_product(arginh, nargs);
}

/*
 * Look up the parent superclass(es) of the given relation.
 *
 * *supervec is set to an array of the type OIDs (not the relation OIDs)
 * of the parents, with nearest ancestors listed first.  It's set to NULL
 * if there are no parents.  The return value is the number of parents.
 */
static int
find_inheritors(Oid relid, Oid **supervec)
{
	Relation	inhrel;
	HeapScanDesc inhscan;
	ScanKeyData skey;
	HeapTuple	inhtup;
	Oid		   *relidvec;
	int			nvisited;
	List	   *visited,
			   *queue;
	List	   *elt;
	bool		newrelid;

	nvisited = 0;
	queue = NIL;
	visited = NIL;

	inhrel = heap_openr(InheritsRelationName, AccessShareLock);

	/*
	 * Use queue to do a breadth-first traversal of the inheritance graph
	 * from the relid supplied up to the root.	At the top of the loop,
	 * relid is the OID of the reltype to check next, queue is the list of
	 * pending relids to check after this one, and visited is the list of
	 * relids we need to output.
	 */
	do
	{
		/* find all types this relid inherits from, and add them to queue */

		ScanKeyEntryInitialize(&skey, 0x0, Anum_pg_inherits_inhrelid,
							   F_OIDEQ,
							   ObjectIdGetDatum(relid));

		inhscan = heap_beginscan(inhrel, SnapshotNow, 1, &skey);

		while ((inhtup = heap_getnext(inhscan, ForwardScanDirection)) != NULL)
		{
			Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(inhtup);

			queue = lappendo(queue, inh->inhparent);
		}

		heap_endscan(inhscan);

		/* pull next unvisited relid off the queue */

		newrelid = false;
		while (queue != NIL)
		{
			relid = lfirsto(queue);
			queue = lnext(queue);
			if (!oidMember(relid, visited))
			{
				newrelid = true;
				break;
			}
		}

		if (newrelid)
		{
			visited = lappendo(visited, relid);
			nvisited++;
		}
	} while (newrelid);

	heap_close(inhrel, AccessShareLock);

	if (nvisited > 0)
	{
		relidvec = (Oid *) palloc(nvisited * sizeof(Oid));
		*supervec = relidvec;

		foreach(elt, visited)
		{
			/* return the type id, rather than the relation id */
			*relidvec++ = get_rel_type_id(lfirsto(elt));
		}
	}
	else
		*supervec = (Oid *) NULL;

	freeList(visited);

	/*
	 * there doesn't seem to be any equally easy way to release the queue
	 * list cells, but since they're palloc'd space it's not critical.
	 */

	return nvisited;
}

/*
 * Generate the ordered list of substitute argtype vectors to try.
 *
 * See comments for argtype_inherit.
 */
static Oid **
gen_cross_product(InhPaths *arginh, int nargs)
{
	int			nanswers;
	Oid		  **result;
	Oid		   *oneres;
	int			i,
				j;
	int			cur[FUNC_MAX_ARGS];

	/*
	 * At each position we want to try the original datatype, plus each
	 * supertype.  So the number of possible combinations is this:
	 */
	nanswers = 1;
	for (i = 0; i < nargs; i++)
		nanswers *= (arginh[i].nsupers + 1);

	/*
	 * We also need an extra slot for the terminating NULL in the result
	 * array, but that cancels out with the fact that we don't want to
	 * generate the zero-changes case.	So we need exactly nanswers slots.
	 */
	result = (Oid **) palloc(sizeof(Oid *) * nanswers);
	j = 0;

	/*
	 * Compute the cross product from right to left.  When cur[i] == 0,
	 * generate the original input type at position i.	When cur[i] == k
	 * for k > 0, generate its k'th supertype.
	 */
	MemSet(cur, 0, sizeof(cur));

	for (;;)
	{
		/*
		 * Find a column we can increment.	All the columns after it get
		 * reset to zero.  (Essentially, we're adding one to the multi-
		 * digit number represented by cur[].)
		 */
		for (i = nargs - 1; i >= 0 && cur[i] >= arginh[i].nsupers; i--)
			cur[i] = 0;

		/* if none, we're done */
		if (i < 0)
			break;

		/* increment this column */
		cur[i] += 1;

		/* Generate the proper output type-OID vector */
		oneres = (Oid *) palloc0(FUNC_MAX_ARGS * sizeof(Oid));

		for (i = 0; i < nargs; i++)
		{
			if (cur[i] == 0)
				oneres[i] = arginh[i].self;
			else
				oneres[i] = arginh[i].supervec[cur[i] - 1];
		}

		result[j++] = oneres;
	}

	/* terminate result vector with NULL pointer */
	result[j++] = NULL;

	Assert(j == nanswers);

	return result;
}


/*
 * Given two type OIDs, determine whether the first is a complex type
 * (class type) that inherits from the second.
 */
bool
typeInheritsFrom(Oid subclassTypeId, Oid superclassTypeId)
{
	Oid			relid;
	Oid		   *supervec;
	int			nsupers,
				i;
	bool		result;

	if (!ISCOMPLEX(subclassTypeId) || !ISCOMPLEX(superclassTypeId))
		return false;
	relid = typeidTypeRelid(subclassTypeId);
	if (relid == InvalidOid)
		return false;
	nsupers = find_inheritors(relid, &supervec);
	result = false;
	for (i = 0; i < nsupers; i++)
	{
		if (supervec[i] == superclassTypeId)
		{
			result = true;
			break;
		}
	}
	if (supervec)
		pfree(supervec);
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
	List	   *current_fargs;
	int			i = 0;

	foreach(current_fargs, fargs)
	{
		/* types don't match? then force coercion using a function call... */
		if (actual_arg_types[i] != declared_arg_types[i])
		{
			lfirst(current_fargs) = coerce_type(pstate,
												lfirst(current_fargs),
												actual_arg_types[i],
												declared_arg_types[i],
												COERCION_IMPLICIT,
												COERCE_IMPLICIT_CAST);
		}
		i++;
	}
}

/*
 * setup_field_select
 *		Build a FieldSelect node that says which attribute to project to.
 *		This routine is called by ParseFuncOrColumn() when we have found
 *		a projection on a function result or parameter.
 */
static FieldSelect *
setup_field_select(Node *input, char *attname, Oid relid)
{
	FieldSelect *fselect = makeNode(FieldSelect);
	AttrNumber	attno;

	attno = get_attnum(relid, attname);
	if (attno == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
			 errmsg("column \"%s\" of relation \"%s\" does not exist",
					attname, get_rel_name(relid))));

	fselect->arg = (Expr *) input;
	fselect->fieldnum = attno;
	fselect->resulttype = get_atttype(relid, attno);
	fselect->resulttypmod = get_atttypmod(relid, attno);

	return fselect;
}

/*
 * ParseComplexProjection -
 *	  handles function calls with a single argument that is of complex type.
 *	  If the function call is actually a column projection, return a suitably
 *	  transformed expression tree.	If not, return NULL.
 *
 * NB: argument is expected to be transformed already, ie, not a RangeVar.
 */
static Node *
ParseComplexProjection(char *funcname, Node *first_arg)
{
	Oid			argtype = exprType(first_arg);
	Oid			argrelid;
	AttrNumber	attnum;
	FieldSelect *fselect;

	argrelid = typeidTypeRelid(argtype);
	if (!argrelid)
		return NULL;			/* probably should not happen */
	attnum = get_attnum(argrelid, funcname);
	if (attnum == InvalidAttrNumber)
		return NULL;			/* funcname does not match any column */

	/*
	 * Check for special cases where we don't want to return a
	 * FieldSelect.
	 */
	switch (nodeTag(first_arg))
	{
		case T_Var:
			{
				Var		   *var = (Var *) first_arg;

				/*
				 * If the Var is a whole-row tuple, we can just replace it
				 * with a simple Var reference.
				 */
				if (var->varattno == InvalidAttrNumber)
				{
					Oid			vartype;
					int32		vartypmod;

					get_atttypetypmod(argrelid, attnum,
									  &vartype, &vartypmod);

					return (Node *) makeVar(var->varno,
											attnum,
											vartype,
											vartypmod,
											var->varlevelsup);
				}
				break;
			}
		default:
			break;
	}

	/* Else generate a FieldSelect expression */
	fselect = setup_field_select(first_arg, funcname, argrelid);
	return (Node *) fselect;
}

/*
 * Simple helper routine for delivering "column does not exist" error message
 */
static void
unknown_attribute(const char *schemaname, const char *relname,
				  const char *attname)
{
	if (schemaname)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column %s.%s.%s does not exist",
						schemaname, relname, attname)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column %s.%s does not exist",
						relname, attname)));
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
 * find_aggregate_func
 *		Convenience routine to check that a function exists and is an
 *		aggregate.
 *
 * Note: basetype is ANYOID if we are looking for an aggregate on
 * all types.
 */
Oid
find_aggregate_func(List *aggname, Oid basetype, bool noError)
{
	Oid			oid;
	HeapTuple	ftup;
	Form_pg_proc pform;

	oid = LookupFuncName(aggname, 1, &basetype, true);

	if (!OidIsValid(oid))
	{
		if (noError)
			return InvalidOid;
		if (basetype == ANYOID)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("aggregate %s(*) does not exist",
							NameListToString(aggname))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("aggregate %s(%s) does not exist",
							NameListToString(aggname),
							format_type_be(basetype))));
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
				 errmsg("function %s(%s) is not an aggregate",
				  NameListToString(aggname), format_type_be(basetype))));
	}

	ReleaseSysCache(ftup);

	return oid;
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

	MemSet(argoids, 0, FUNC_MAX_ARGS * sizeof(Oid));
	argcount = length(argtypes);
	if (argcount > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("functions cannot have more than %d arguments",
						FUNC_MAX_ARGS)));

	for (i = 0; i < argcount; i++)
	{
		TypeName   *t = (TypeName *) lfirst(argtypes);

		argoids[i] = LookupTypeName(t);

		if (!OidIsValid(argoids[i]))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" does not exist",
							TypeNameToString(t))));

		argtypes = lnext(argtypes);
	}

	return LookupFuncName(funcname, argcount, argoids, noError);
}
