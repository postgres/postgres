/*-------------------------------------------------------------------------
 *
 * parse_oper.c
 *		handle operator things for parser
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_oper.c,v 1.62 2003/04/08 23:20:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static Oid binary_oper_exact(Oid arg1, Oid arg2,
				  FuncCandidateList candidates);
static Oid oper_select_candidate(int nargs, Oid *input_typeids,
					  FuncCandidateList candidates);
static void op_error(List *op, Oid arg1, Oid arg2);
static void unary_op_error(List *op, Oid arg, bool is_left_op);


/*
 * LookupOperName
 *		Given a possibly-qualified operator name and exact input datatypes,
 *		look up the operator.  Returns InvalidOid if no such operator.
 *
 * Pass oprleft = InvalidOid for a prefix op, oprright = InvalidOid for
 * a postfix op.
 *
 * If the operator name is not schema-qualified, it is sought in the current
 * namespace search path.
 */
Oid
LookupOperName(List *opername, Oid oprleft, Oid oprright)
{
	FuncCandidateList clist;
	char		oprkind;

	if (!OidIsValid(oprleft))
		oprkind = 'l';
	else if (!OidIsValid(oprright))
		oprkind = 'r';
	else
		oprkind = 'b';

	clist = OpernameGetCandidates(opername, oprkind);

	while (clist)
	{
		if (clist->args[0] == oprleft && clist->args[1] == oprright)
			return clist->oid;
		clist = clist->next;
	}

	return InvalidOid;
}

/*
 * LookupOperNameTypeNames
 *		Like LookupOperName, but the argument types are specified by
 *		TypeName nodes.  Also, if we fail to find the operator
 *		and caller is not NULL, then an error is reported.
 *
 * Pass oprleft = NULL for a prefix op, oprright = NULL for a postfix op.
 */
Oid
LookupOperNameTypeNames(List *opername, TypeName *oprleft,
						TypeName *oprright, const char *caller)
{
	Oid			operoid;
	Oid			leftoid,
				rightoid;

	if (oprleft == NULL)
		leftoid = InvalidOid;
	else
	{
		leftoid = LookupTypeName(oprleft);
		if (!OidIsValid(leftoid))
			elog(ERROR, "Type \"%s\" does not exist",
				 TypeNameToString(oprleft));
	}
	if (oprright == NULL)
		rightoid = InvalidOid;
	else
	{
		rightoid = LookupTypeName(oprright);
		if (!OidIsValid(rightoid))
			elog(ERROR, "Type \"%s\" does not exist",
				 TypeNameToString(oprright));
	}

	operoid = LookupOperName(opername, leftoid, rightoid);

	if (!OidIsValid(operoid) && caller != NULL)
	{
		if (oprleft == NULL)
			elog(ERROR, "%s: Prefix operator '%s' for type '%s' does not exist",
				 caller, NameListToString(opername),
				 TypeNameToString(oprright));
		else if (oprright == NULL)
			elog(ERROR, "%s: Postfix operator '%s' for type '%s' does not exist",
				 caller, NameListToString(opername),
				 TypeNameToString(oprleft));
		else
			elog(ERROR, "%s: Operator '%s' for types '%s' and '%s' does not exist",
				 caller, NameListToString(opername),
				 TypeNameToString(oprleft),
				 TypeNameToString(oprright));
	}

	return operoid;
}

/*
 * equality_oper - identify a suitable equality operator for a datatype
 *
 * On failure, return NULL if noError, else report a standard error
 */
Operator
equality_oper(Oid argtype, bool noError)
{
	Operator	optup;

	/*
	 * Look for an "=" operator for the datatype.  We require it to be
	 * an exact or binary-compatible match, since most callers are not
	 * prepared to cope with adding any run-time type coercion steps.
	 */
	optup = compatible_oper(makeList1(makeString("=")),
							argtype, argtype, true);
	if (optup != NULL)
	{
		/*
		 * Only believe that it's equality if it's mergejoinable,
		 * hashjoinable, or uses eqsel() as oprrest.
		 */
		Form_pg_operator pgopform = (Form_pg_operator) GETSTRUCT(optup);

		if (OidIsValid(pgopform->oprlsortop) ||
			pgopform->oprcanhash ||
			pgopform->oprrest == F_EQSEL)
			return optup;

		ReleaseSysCache(optup);
	}
	if (!noError)
		elog(ERROR, "Unable to identify an equality operator for type %s",
			 format_type_be(argtype));
	return NULL;
}

/*
 * ordering_oper - identify a suitable sorting operator ("<") for a datatype
 *
 * On failure, return NULL if noError, else report a standard error
 */
Operator
ordering_oper(Oid argtype, bool noError)
{
	Operator	optup;

	/*
	 * Find the type's equality operator, and use its lsortop (it *must*
	 * be mergejoinable).  We use this definition because for sorting and
	 * grouping purposes, it's important that the equality and ordering
	 * operators are consistent.
	 */
	optup = equality_oper(argtype, noError);
	if (optup != NULL)
	{
		Oid		lsortop = ((Form_pg_operator) GETSTRUCT(optup))->oprlsortop;

		ReleaseSysCache(optup);

		if (OidIsValid(lsortop))
		{
			optup = SearchSysCache(OPEROID,
								   ObjectIdGetDatum(lsortop),
								   0, 0, 0);
			if (optup != NULL)
				return optup;
		}
	}
	if (!noError)
		elog(ERROR, "Unable to identify an ordering operator for type %s"
			 "\n\tUse an explicit ordering operator or modify the query",
			 format_type_be(argtype));
	return NULL;
}

/*
 * equality_oper_funcid - convenience routine for oprfuncid(equality_oper())
 */
Oid
equality_oper_funcid(Oid argtype)
{
	Operator	optup;
	Oid			result;

	optup = equality_oper(argtype, false);
	result = oprfuncid(optup);
	ReleaseSysCache(optup);
	return result;
}

/*
 * ordering_oper_opid - convenience routine for oprid(ordering_oper())
 *
 * This was formerly called any_ordering_op()
 */
Oid
ordering_oper_opid(Oid argtype)
{
	Operator	optup;
	Oid			result;

	optup = ordering_oper(argtype, false);
	result = oprid(optup);
	ReleaseSysCache(optup);
	return result;
}


/* given operator tuple, return the operator OID */
Oid
oprid(Operator op)
{
	return HeapTupleGetOid(op);
}

/* given operator tuple, return the underlying function's OID */
Oid
oprfuncid(Operator op)
{
	Form_pg_operator pgopform = (Form_pg_operator) GETSTRUCT(op);

	return pgopform->oprcode;
}


/* binary_oper_exact()
 * Check for an "exact" match to the specified operand types.
 *
 * If one operand is an unknown literal, assume it should be taken to be
 * the same type as the other operand for this purpose.
 */
static Oid
binary_oper_exact(Oid arg1, Oid arg2,
				  FuncCandidateList candidates)
{
	/* Unspecified type for one of the arguments? then use the other */
	if ((arg1 == UNKNOWNOID) && (arg2 != InvalidOid))
		arg1 = arg2;
	else if ((arg2 == UNKNOWNOID) && (arg1 != InvalidOid))
		arg2 = arg1;

	while (candidates != NULL)
	{
		if (arg1 == candidates->args[0] &&
			arg2 == candidates->args[1])
			return candidates->oid;
		candidates = candidates->next;
	}

	return InvalidOid;
}


/* oper_select_candidate()
 * Given the input argtype array and one or more candidates
 * for the function argtype array, attempt to resolve the conflict.
 * Returns the selected argtype array if the conflict can be resolved,
 * otherwise returns NULL.
 *
 * By design, this is pretty similar to func_select_candidate in parse_func.c.
 * However, we can do a couple of extra things here because we know we can
 * have no more than two args to deal with.  Also, the calling convention
 * is a little different: we must prune away "candidates" that aren't actually
 * coercion-compatible with the input types, whereas in parse_func.c that
 * gets done by match_argtypes before func_select_candidate is called.
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
static Oid
oper_select_candidate(int nargs,
					  Oid *input_typeids,
					  FuncCandidateList candidates)
{
	FuncCandidateList current_candidate;
	FuncCandidateList last_candidate;
	Oid		   *current_typeids;
	Oid			current_type;
	int			unknownOids;
	int			i;
	int			ncandidates;
	int			nbestMatch,
				nmatch;
	CATEGORY	slot_category[FUNC_MAX_ARGS],
				current_category;
	bool		slot_has_preferred_type[FUNC_MAX_ARGS];
	bool		resolved_unknowns;

	/*
	 * First, delete any candidates that cannot actually accept the given
	 * input types, whether directly or by coercion.  (Note that
	 * can_coerce_type will assume that UNKNOWN inputs are coercible to
	 * anything, so candidates will not be eliminated on that basis.)
	 */
	ncandidates = 0;
	last_candidate = NULL;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		if (can_coerce_type(nargs, input_typeids, current_candidate->args,
							COERCION_IMPLICIT))
		{
			if (last_candidate == NULL)
			{
				candidates = current_candidate;
				last_candidate = current_candidate;
				ncandidates = 1;
			}
			else
			{
				last_candidate->next = current_candidate;
				last_candidate = current_candidate;
				ncandidates++;
			}
		}
		/* otherwise, don't bother keeping this one... */
	}

	if (last_candidate)			/* terminate rebuilt list */
		last_candidate->next = NULL;

	/* Done if no candidate or only one candidate survives */
	if (ncandidates == 0)
		return InvalidOid;
	if (ncandidates == 1)
		return candidates->oid;

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
			if (input_typeids[i] != UNKNOWNOID &&
				current_typeids[i] == input_typeids[i])
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
		return candidates->oid;

	/*
	 * Still too many candidates? Run through all candidates and keep
	 * those with the most matches on exact types + binary-compatible
	 * types. Keep all candidates if none match.
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
			if (input_typeids[i] != UNKNOWNOID)
			{
				if (IsBinaryCoercible(input_typeids[i], current_typeids[i]))
					nmatch++;
			}
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
		return candidates->oid;

	/*
	 * Still too many candidates? Now look for candidates which are
	 * preferred types at the args that will require coercion. Keep all
	 * candidates if none match.
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
			if (input_typeids[i] != UNKNOWNOID)
			{
				current_category = TypeCategory(current_typeids[i]);
				if (current_typeids[i] == input_typeids[i] ||
					IsPreferredType(current_category, current_typeids[i]))
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
		return candidates->oid;

	/*
	 * Still too many candidates? Try assigning types for the unknown
	 * columns.
	 *
	 * First try: if we have an unknown and a non-unknown input, see whether
	 * there is a candidate all of whose input types are the same as the
	 * known input type (there can be at most one such candidate).	If so,
	 * use that candidate.	NOTE that this is cool only because operators
	 * can't have more than 2 args, so taking the last non-unknown as
	 * current_type can yield only one possibility if there is also an
	 * unknown.
	 */
	unknownOids = FALSE;
	current_type = UNKNOWNOID;
	for (i = 0; i < nargs; i++)
	{
		if ((input_typeids[i] != UNKNOWNOID)
			&& (input_typeids[i] != InvalidOid))
			current_type = input_typeids[i];
		else
			unknownOids = TRUE;
	}

	if (unknownOids && (current_type != UNKNOWNOID))
	{
		for (current_candidate = candidates;
			 current_candidate != NULL;
			 current_candidate = current_candidate->next)
		{
			current_typeids = current_candidate->args;
			nmatch = 0;
			for (i = 0; i < nargs; i++)
			{
				if (current_type == current_typeids[i])
					nmatch++;
			}
			if (nmatch == nargs)
				return current_candidate->oid;
		}
	}

	/*
	 * Second try: same algorithm as for unknown resolution in
	 * parse_func.c.
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

		if (input_typeids[i] != UNKNOWNOID)
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
				if (input_typeids[i] != UNKNOWNOID)
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
		return candidates->oid;

	return InvalidOid;			/* failed to determine a unique candidate */
}	/* oper_select_candidate() */


/* oper() -- search for a binary operator
 * Given operator name, types of arg1 and arg2, return oper struct.
 *
 * IMPORTANT: the returned operator (if any) is only promised to be
 * coercion-compatible with the input datatypes.  Do not use this if
 * you need an exact- or binary-compatible match; see compatible_oper.
 *
 * If no matching operator found, return NULL if noError is true,
 * raise an error if it is false.
 *
 * NOTE: on success, the returned object is a syscache entry.  The caller
 * must ReleaseSysCache() the entry when done with it.
 */
Operator
oper(List *opname, Oid ltypeId, Oid rtypeId, bool noError)
{
	FuncCandidateList clist;
	Oid			operOid;
	Oid			inputOids[2];
	HeapTuple	tup = NULL;

	/* Get binary operators of given name */
	clist = OpernameGetCandidates(opname, 'b');

	/* No operators found? Then fail... */
	if (clist != NULL)
	{
		/*
		 * Check for an "exact" match.
		 */
		operOid = binary_oper_exact(ltypeId, rtypeId, clist);
		if (!OidIsValid(operOid))
		{
			/*
			 * Otherwise, search for the most suitable candidate.
			 */

			/*
			 * Unspecified type for one of the arguments? then use the
			 * other
			 */
			if (rtypeId == InvalidOid)
				rtypeId = ltypeId;
			else if (ltypeId == InvalidOid)
				ltypeId = rtypeId;
			inputOids[0] = ltypeId;
			inputOids[1] = rtypeId;
			operOid = oper_select_candidate(2, inputOids, clist);
		}
		if (OidIsValid(operOid))
			tup = SearchSysCache(OPEROID,
								 ObjectIdGetDatum(operOid),
								 0, 0, 0);
	}

	if (!HeapTupleIsValid(tup) && !noError)
		op_error(opname, ltypeId, rtypeId);

	return (Operator) tup;
}

/* compatible_oper()
 *	given an opname and input datatypes, find a compatible binary operator
 *
 *	This is tighter than oper() because it will not return an operator that
 *	requires coercion of the input datatypes (but binary-compatible operators
 *	are accepted).	Otherwise, the semantics are the same.
 */
Operator
compatible_oper(List *op, Oid arg1, Oid arg2, bool noError)
{
	Operator	optup;
	Form_pg_operator opform;

	/* oper() will find the best available match */
	optup = oper(op, arg1, arg2, noError);
	if (optup == (Operator) NULL)
		return (Operator) NULL; /* must be noError case */

	/* but is it good enough? */
	opform = (Form_pg_operator) GETSTRUCT(optup);
	if (IsBinaryCoercible(arg1, opform->oprleft) &&
		IsBinaryCoercible(arg2, opform->oprright))
		return optup;

	/* nope... */
	ReleaseSysCache(optup);

	if (!noError)
		op_error(op, arg1, arg2);

	return (Operator) NULL;
}

/* compatible_oper_opid() -- get OID of a binary operator
 *
 * This is a convenience routine that extracts only the operator OID
 * from the result of compatible_oper().  InvalidOid is returned if the
 * lookup fails and noError is true.
 */
Oid
compatible_oper_opid(List *op, Oid arg1, Oid arg2, bool noError)
{
	Operator	optup;
	Oid			result;

	optup = compatible_oper(op, arg1, arg2, noError);
	if (optup != NULL)
	{
		result = oprid(optup);
		ReleaseSysCache(optup);
		return result;
	}
	return InvalidOid;
}


/* right_oper() -- search for a unary right operator (operator on right)
 * Given operator name and type of arg, return oper struct.
 *
 * IMPORTANT: the returned operator (if any) is only promised to be
 * coercion-compatible with the input datatype.  Do not use this if
 * you need an exact- or binary-compatible match.
 *
 * If no matching operator found, return NULL if noError is true,
 * raise an error if it is false.
 *
 * NOTE: on success, the returned object is a syscache entry.  The caller
 * must ReleaseSysCache() the entry when done with it.
 */
Operator
right_oper(List *op, Oid arg, bool noError)
{
	FuncCandidateList clist;
	Oid			operOid = InvalidOid;
	HeapTuple	tup = NULL;

	/* Find candidates */
	clist = OpernameGetCandidates(op, 'r');

	if (clist != NULL)
	{
		/*
		 * First, quickly check to see if there is an exactly matching
		 * operator (there can be only one such entry in the list).
		 */
		FuncCandidateList clisti;

		for (clisti = clist; clisti != NULL; clisti = clisti->next)
		{
			if (arg == clisti->args[0])
			{
				operOid = clisti->oid;
				break;
			}
		}

		if (!OidIsValid(operOid))
		{
			/*
			 * We must run oper_select_candidate even if only one
			 * candidate, otherwise we may falsely return a
			 * non-type-compatible operator.
			 */
			operOid = oper_select_candidate(1, &arg, clist);
		}
		if (OidIsValid(operOid))
			tup = SearchSysCache(OPEROID,
								 ObjectIdGetDatum(operOid),
								 0, 0, 0);
	}

	if (!HeapTupleIsValid(tup) && !noError)
		unary_op_error(op, arg, FALSE);

	return (Operator) tup;
}


/* left_oper() -- search for a unary left operator (operator on left)
 * Given operator name and type of arg, return oper struct.
 *
 * IMPORTANT: the returned operator (if any) is only promised to be
 * coercion-compatible with the input datatype.  Do not use this if
 * you need an exact- or binary-compatible match.
 *
 * If no matching operator found, return NULL if noError is true,
 * raise an error if it is false.
 *
 * NOTE: on success, the returned object is a syscache entry.  The caller
 * must ReleaseSysCache() the entry when done with it.
 */
Operator
left_oper(List *op, Oid arg, bool noError)
{
	FuncCandidateList clist;
	Oid			operOid = InvalidOid;
	HeapTuple	tup = NULL;

	/* Find candidates */
	clist = OpernameGetCandidates(op, 'l');

	if (clist != NULL)
	{
		/*
		 * First, quickly check to see if there is an exactly matching
		 * operator (there can be only one such entry in the list).
		 *
		 * The returned list has args in the form (0, oprright).  Move the
		 * useful data into args[0] to keep oper_select_candidate simple.
		 * XXX we are assuming here that we may scribble on the list!
		 */
		FuncCandidateList clisti;

		for (clisti = clist; clisti != NULL; clisti = clisti->next)
		{
			clisti->args[0] = clisti->args[1];
			if (arg == clisti->args[0])
			{
				operOid = clisti->oid;
				break;
			}
		}

		if (!OidIsValid(operOid))
		{
			/*
			 * We must run oper_select_candidate even if only one
			 * candidate, otherwise we may falsely return a
			 * non-type-compatible operator.
			 */
			operOid = oper_select_candidate(1, &arg, clist);
		}
		if (OidIsValid(operOid))
			tup = SearchSysCache(OPEROID,
								 ObjectIdGetDatum(operOid),
								 0, 0, 0);
	}

	if (!HeapTupleIsValid(tup) && !noError)
		unary_op_error(op, arg, TRUE);

	return (Operator) tup;
}


/* op_error()
 * Give a somewhat useful error message when the operator for two types
 * is not found.
 */
static void
op_error(List *op, Oid arg1, Oid arg2)
{
	if (!typeidIsValid(arg1))
		elog(ERROR, "Left hand side of operator '%s' has an unknown type"
			 "\n\tProbably a bad attribute name",
			 NameListToString(op));

	if (!typeidIsValid(arg2))
		elog(ERROR, "Right hand side of operator %s has an unknown type"
			 "\n\tProbably a bad attribute name",
			 NameListToString(op));

	elog(ERROR, "Unable to identify an operator '%s' for types '%s' and '%s'"
		 "\n\tYou will have to retype this query using an explicit cast",
		 NameListToString(op),
		 format_type_be(arg1), format_type_be(arg2));
}

/* unary_op_error()
 * Give a somewhat useful error message when the operator for one type
 * is not found.
 */
static void
unary_op_error(List *op, Oid arg, bool is_left_op)
{
	if (!typeidIsValid(arg))
	{
		if (is_left_op)
			elog(ERROR, "operand of prefix operator '%s' has an unknown type"
				 "\n\t(probably an invalid column reference)",
				 NameListToString(op));
		else
			elog(ERROR, "operand of postfix operator '%s' has an unknown type"
				 "\n\t(probably an invalid column reference)",
				 NameListToString(op));
	}
	else
	{
		if (is_left_op)
			elog(ERROR, "Unable to identify a prefix operator '%s' for type '%s'"
			   "\n\tYou may need to add parentheses or an explicit cast",
				 NameListToString(op), format_type_be(arg));
		else
			elog(ERROR, "Unable to identify a postfix operator '%s' for type '%s'"
			   "\n\tYou may need to add parentheses or an explicit cast",
				 NameListToString(op), format_type_be(arg));
	}
}


/*
 * make_op()
 *		Operator expression construction.
 *
 * Transform operator expression ensuring type compatibility.
 * This is where some type conversion happens.
 */
Expr *
make_op(List *opname, Node *ltree, Node *rtree)
{
	Oid			ltypeId,
				rtypeId;
	Operator	tup;
	Expr	   *result;

	/* Select the operator */
	if (rtree == NULL)
	{
		/* right operator */
		ltypeId = exprType(ltree);
		rtypeId = InvalidOid;
		tup = right_oper(opname, ltypeId, false);
	}
	else if (ltree == NULL)
	{
		/* left operator */
		rtypeId = exprType(rtree);
		ltypeId = InvalidOid;
		tup = left_oper(opname, rtypeId, false);
	}
	else
	{
		/* otherwise, binary operator */
		ltypeId = exprType(ltree);
		rtypeId = exprType(rtree);
		tup = oper(opname, ltypeId, rtypeId, false);
	}

	/* Do typecasting and build the expression tree */
	result = make_op_expr(tup, ltree, rtree, ltypeId, rtypeId);

	ReleaseSysCache(tup);

	return result;
}


/*
 * make_op_expr()
 *		Build operator expression using an already-looked-up operator.
 */
Expr *
make_op_expr(Operator op, Node *ltree, Node *rtree,
			 Oid ltypeId, Oid rtypeId)
{
	Form_pg_operator opform = (Form_pg_operator) GETSTRUCT(op);
	Oid			actual_arg_types[2];
	Oid			declared_arg_types[2];
	int			nargs;
	List	   *args;
	Oid			rettype;
	OpExpr	   *result;

	if (rtree == NULL)
	{
		/* right operator */
		args = makeList1(ltree);
		actual_arg_types[0] = ltypeId;
		declared_arg_types[0] = opform->oprleft;
		nargs = 1;
	}
	else if (ltree == NULL)
	{
		/* left operator */
		args = makeList1(rtree);
		actual_arg_types[0] = rtypeId;
		declared_arg_types[0] = opform->oprright;
		nargs = 1;
	}
	else
	{
		/* otherwise, binary operator */
		args = makeList2(ltree, rtree);
		actual_arg_types[0] = ltypeId;
		actual_arg_types[1] = rtypeId;
		declared_arg_types[0] = opform->oprleft;
		declared_arg_types[1] = opform->oprright;
		nargs = 2;
	}

	/*
	 * enforce consistency with ANYARRAY and ANYELEMENT argument and
	 * return types, possibly adjusting return type or declared_arg_types
	 * (which will be used as the cast destination by make_fn_arguments)
	 */
	rettype = enforce_generic_type_consistency(actual_arg_types,
											   declared_arg_types,
											   nargs,
											   opform->oprresult);

	/* perform the necessary typecasting of arguments */
	make_fn_arguments(args, actual_arg_types, declared_arg_types);

	/* and build the expression node */
	result = makeNode(OpExpr);
	result->opno = oprid(op);
	result->opfuncid = InvalidOid;
	result->opresulttype = rettype;
	result->opretset = get_func_retset(opform->oprcode);
	result->args = args;

	return (Expr *) result;
}
