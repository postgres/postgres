/*-------------------------------------------------------------------------
 *
 * parse_oper.c
 *		handle operator things for parser
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_oper.c,v 1.39 2000/03/19 00:19:39 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_operator.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/syscache.h"

static Oid *oper_select_candidate(int nargs, Oid *input_typeids,
					  CandidateList candidates);
static Operator oper_exact(char *op, Oid arg1, Oid arg2);
static Operator oper_inexact(char *op, Oid arg1, Oid arg2);
static int binary_oper_get_candidates(char *opname,
									  CandidateList *candidates);
static int unary_oper_get_candidates(char *opname,
									 CandidateList *candidates,
									 char rightleft);
static void op_error(char *op, Oid arg1, Oid arg2);
static void unary_op_error(char *op, Oid arg, bool is_left_op);


Oid
any_ordering_op(Oid restype)
{
	Operator	order_op;
	Oid			order_opid;

	order_op = oper("<", restype, restype, TRUE);
	if (!HeapTupleIsValid(order_op))
	{
		elog(ERROR, "Unable to identify an ordering operator '%s' for type '%s'"
			 "\n\tUse an explicit ordering operator or modify the query",
			 "<", typeidTypeName(restype));
	}
	order_opid = oprid(order_op);

	return order_opid;
}

/* given operator, return the operator OID */
Oid
oprid(Operator op)
{
	return op->t_data->t_oid;
}


/* binary_oper_get_candidates()
 *	given opname, find all possible input type pairs for which an operator
 *	named opname exists.
 *	Build a list of the candidate input types.
 *	Returns number of candidates found.
 */
static int
binary_oper_get_candidates(char *opname,
						   CandidateList *candidates)
{
	CandidateList current_candidate;
	Relation	pg_operator_desc;
	HeapScanDesc pg_operator_scan;
	HeapTuple	tup;
	Form_pg_operator oper;
	int			ncandidates = 0;
	ScanKeyData opKey[2];

	*candidates = NULL;

	ScanKeyEntryInitialize(&opKey[0], 0,
						   Anum_pg_operator_oprname,
						   F_NAMEEQ,
						   NameGetDatum(opname));

	ScanKeyEntryInitialize(&opKey[1], 0,
						   Anum_pg_operator_oprkind,
						   F_CHAREQ,
						   CharGetDatum('b'));

	pg_operator_desc = heap_openr(OperatorRelationName, AccessShareLock);
	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  SnapshotSelf,		/* ??? */
									  2,
									  opKey);

	while (HeapTupleIsValid(tup = heap_getnext(pg_operator_scan, 0)))
	{
		oper = (Form_pg_operator) GETSTRUCT(tup);

		current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
		current_candidate->args = (Oid *) palloc(2 * sizeof(Oid));

		current_candidate->args[0] = oper->oprleft;
		current_candidate->args[1] = oper->oprright;
		current_candidate->next = *candidates;
		*candidates = current_candidate;
		ncandidates++;
	}

	heap_endscan(pg_operator_scan);
	heap_close(pg_operator_desc, AccessShareLock);

	return ncandidates;
}	/* binary_oper_get_candidates() */


/* oper_select_candidate()
 * Given the input argtype array and more than one candidate
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
static Oid *
oper_select_candidate(int nargs,
					  Oid *input_typeids,
					  CandidateList candidates)
{
	CandidateList current_candidate;
	CandidateList last_candidate;
	Oid		   *current_typeids;
	int			unknownOids;
	int			i;
	int			ncandidates;
	int			nbestMatch,
				nmatch;
	CATEGORY	slot_category,
				current_category;
	Oid			slot_type,
				current_type;

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
		if (can_coerce_type(nargs, input_typeids, current_candidate->args))
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
		return NULL;
	if (ncandidates == 1)
		return candidates->args;

	/*
	 * Run through all candidates and keep those with the most matches
	 * on exact types. Keep all candidates if none match.
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
		return candidates->args;

	/*
	 * Still too many candidates?
	 * Run through all candidates and keep those with the most matches
	 * on exact types + binary-compatible types.
	 * Keep all candidates if none match.
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
				if (current_typeids[i] == input_typeids[i] ||
					IS_BINARY_COMPATIBLE(current_typeids[i],
										 input_typeids[i]))
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
		return candidates->args;

	/*
	 * Still too many candidates?
	 * Now look for candidates which are preferred types at the args that
	 * will require coercion.
	 * Keep all candidates if none match.
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
		return candidates->args;

	/*
	 * Still too many candidates?
	 * Try assigning types for the unknown columns.
	 *
	 * First try: if we have an unknown and a non-unknown input, see whether
	 * there is a candidate all of whose input types are the same as the known
	 * input type (there can be at most one such candidate).  If so, use that
	 * candidate.  NOTE that this is cool only because operators can't
	 * have more than 2 args, so taking the last non-unknown as current_type
	 * can yield only one possibility if there is also an unknown.
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
				return current_typeids;
		}
	}

	/*
	 * Second try: examine each unknown argument position to see if all the
	 * candidates agree on the type category of that slot.  If so, and if some
	 * candidates accept the preferred type in that category, eliminate the
	 * candidates with other input types.  If we are down to one candidate
	 * at the end, we win.
	 *
	 * XXX It's kinda bogus to do this left-to-right, isn't it?  If we
	 * eliminate some candidates because they are non-preferred at the first
	 * slot, we won't notice that they didn't have the same type category for
	 * a later slot.
	 */
	for (i = 0; i < nargs; i++)
	{
		if (input_typeids[i] == UNKNOWNOID)
		{
			slot_category = INVALID_TYPE;
			slot_type = InvalidOid;
			last_candidate = NULL;
			for (current_candidate = candidates;
				 current_candidate != NULL;
				 current_candidate = current_candidate->next)
			{
				current_typeids = current_candidate->args;
				current_type = current_typeids[i];
				current_category = TypeCategory(current_type);
				if (slot_category == INVALID_TYPE)
				{
					slot_category = current_category;
					slot_type = current_type;
					last_candidate = current_candidate;
				}
				else if (current_category != slot_category)
				{
					/* punt if more than one category for this slot */
					return NULL;
				}
				else if (current_type != slot_type)
				{
					if (IsPreferredType(slot_category, current_type))
					{
						slot_type = current_type;
						/* forget all previous candidates */
						candidates = current_candidate;
						last_candidate = current_candidate;
					}
					else if (IsPreferredType(slot_category, slot_type))
					{
						/* forget this candidate */
						if (last_candidate)
							last_candidate->next = current_candidate->next;
						else
							candidates = current_candidate->next;
					}
					else
						last_candidate = current_candidate;
				}
				else
				{
					/* keep this candidate */
					last_candidate = current_candidate;
				}
			}
			if (last_candidate)			/* terminate rebuilt list */
				last_candidate->next = NULL;
		}
	}

	if (candidates == NULL)
		return NULL;			/* no remaining candidates */
	if (candidates->next != NULL)
		return NULL;			/* more than one remaining candidate */
	return candidates->args;
}	/* oper_select_candidate() */


/* oper_exact()
 * Given operator, and arguments, return oper struct or NULL.
 * Inputs:
 * arg1, arg2: Type IDs
 */
static Operator
oper_exact(char *op, Oid arg1, Oid arg2)
{
	HeapTuple	tup;

	/* Unspecified type for one of the arguments? then use the other */
	if ((arg1 == UNKNOWNOID) && (arg2 != InvalidOid))
		arg1 = arg2;
	else if ((arg2 == UNKNOWNOID) && (arg1 != InvalidOid))
		arg2 = arg1;

	tup = SearchSysCacheTuple(OPERNAME,
							  PointerGetDatum(op),
							  ObjectIdGetDatum(arg1),
							  ObjectIdGetDatum(arg2),
							  CharGetDatum('b'));

	return (Operator) tup;
}	/* oper_exact() */


/* oper_inexact()
 * Given operator, types of arg1, and arg2, return oper struct or NULL.
 * Inputs:
 * arg1, arg2: Type IDs
 */
static Operator
oper_inexact(char *op, Oid arg1, Oid arg2)
{
	HeapTuple	tup;
	CandidateList candidates;
	int			ncandidates;
	Oid		   *targetOids;
	Oid			inputOids[2];

	/* Unspecified type for one of the arguments? then use the other */
	if (arg2 == InvalidOid)
		arg2 = arg1;
	if (arg1 == InvalidOid)
		arg1 = arg2;

	ncandidates = binary_oper_get_candidates(op, &candidates);

	/* No operators found? Then return null... */
	if (ncandidates == 0)
		return NULL;

	/* Or found exactly one? Then proceed... */
	else if (ncandidates == 1)
	{
		tup = SearchSysCacheTuple(OPERNAME,
								  PointerGetDatum(op),
								  ObjectIdGetDatum(candidates->args[0]),
								  ObjectIdGetDatum(candidates->args[1]),
								  CharGetDatum('b'));
		Assert(HeapTupleIsValid(tup));
	}

	/* Otherwise, multiple operators of the desired types found... */
	else
	{
		inputOids[0] = arg1;
		inputOids[1] = arg2;
		targetOids = oper_select_candidate(2, inputOids, candidates);
		if (targetOids != NULL)
		{
			tup = SearchSysCacheTuple(OPERNAME,
									  PointerGetDatum(op),
									  ObjectIdGetDatum(targetOids[0]),
									  ObjectIdGetDatum(targetOids[1]),
									  CharGetDatum('b'));
		}
		else
			tup = NULL;
	}
	return (Operator) tup;
}	/* oper_inexact() */


/* oper()
 * Given operator, types of arg1, and arg2, return oper struct.
 * Inputs:
 * arg1, arg2: Type IDs
 */
Operator
oper(char *opname, Oid ltypeId, Oid rtypeId, bool noWarnings)
{
	HeapTuple	tup;

	/* check for exact match on this operator... */
	if (HeapTupleIsValid(tup = oper_exact(opname, ltypeId, rtypeId)))
	{
	}
	/* try to find a match on likely candidates... */
	else if (HeapTupleIsValid(tup = oper_inexact(opname, ltypeId, rtypeId)))
	{
	}
	else if (!noWarnings)
	{
		op_error(opname, ltypeId, rtypeId);
	}

	return (Operator) tup;
}	/* oper() */


/* unary_oper_get_candidates()
 *	given opname, find all possible types for which
 *	a right/left unary operator named opname exists.
 *	Build a list of the candidate input types.
 *	Returns number of candidates found.
 */
static int
unary_oper_get_candidates(char *opname,
						  CandidateList *candidates,
						  char rightleft)
{
	CandidateList current_candidate;
	Relation	pg_operator_desc;
	HeapScanDesc pg_operator_scan;
	HeapTuple	tup;
	Form_pg_operator oper;
	int			ncandidates = 0;
	ScanKeyData opKey[2];

	*candidates = NULL;

	ScanKeyEntryInitialize(&opKey[0], 0,
						   Anum_pg_operator_oprname,
						   F_NAMEEQ,
						   NameGetDatum(opname));

	ScanKeyEntryInitialize(&opKey[1], 0,
						   Anum_pg_operator_oprkind,
						   F_CHAREQ,
						   CharGetDatum(rightleft));

	pg_operator_desc = heap_openr(OperatorRelationName, AccessShareLock);
	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  SnapshotSelf,		/* ??? */
									  2,
									  opKey);

	while (HeapTupleIsValid(tup = heap_getnext(pg_operator_scan, 0)))
	{
		oper = (Form_pg_operator) GETSTRUCT(tup);

		current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
		current_candidate->args = (Oid *) palloc(sizeof(Oid));

		if (rightleft == 'r')
			current_candidate->args[0] = oper->oprleft;
		else
			current_candidate->args[0] = oper->oprright;
		current_candidate->next = *candidates;
		*candidates = current_candidate;
		ncandidates++;
	}

	heap_endscan(pg_operator_scan);
	heap_close(pg_operator_desc, AccessShareLock);

	return ncandidates;
}	/* unary_oper_get_candidates() */


/* Given unary right-side operator (operator on right), return oper struct */
/* arg-- type id */
Operator
right_oper(char *op, Oid arg)
{
	HeapTuple	tup;
	CandidateList candidates;
	int			ncandidates;
	Oid		   *targetOid;

	/* Try for exact match */
	tup = SearchSysCacheTuple(OPERNAME,
							  PointerGetDatum(op),
							  ObjectIdGetDatum(arg),
							  ObjectIdGetDatum(InvalidOid),
							  CharGetDatum('r'));

	if (!HeapTupleIsValid(tup))
	{
		/* Try for inexact matches */
		ncandidates = unary_oper_get_candidates(op, &candidates, 'r');
		if (ncandidates == 0)
		{
			unary_op_error(op, arg, FALSE);
		}
		else if (ncandidates == 1)
		{
			tup = SearchSysCacheTuple(OPERNAME,
									  PointerGetDatum(op),
									  ObjectIdGetDatum(candidates->args[0]),
									  ObjectIdGetDatum(InvalidOid),
									  CharGetDatum('r'));
		}
		else
		{
			targetOid = oper_select_candidate(1, &arg, candidates);
			if (targetOid != NULL)
				tup = SearchSysCacheTuple(OPERNAME,
										  PointerGetDatum(op),
										  ObjectIdGetDatum(targetOid[0]),
										  ObjectIdGetDatum(InvalidOid),
										  CharGetDatum('r'));
		}

		if (!HeapTupleIsValid(tup))
			unary_op_error(op, arg, FALSE);
	}

	return (Operator) tup;
}	/* right_oper() */


/* Given unary left-side operator (operator on left), return oper struct */
/* arg--type id */
Operator
left_oper(char *op, Oid arg)
{
	HeapTuple	tup;
	CandidateList candidates;
	int			ncandidates;
	Oid		   *targetOid;

	/* Try for exact match */
	tup = SearchSysCacheTuple(OPERNAME,
							  PointerGetDatum(op),
							  ObjectIdGetDatum(InvalidOid),
							  ObjectIdGetDatum(arg),
							  CharGetDatum('l'));

	if (!HeapTupleIsValid(tup))
	{
		/* Try for inexact matches */
		ncandidates = unary_oper_get_candidates(op, &candidates, 'l');
		if (ncandidates == 0)
		{
			unary_op_error(op, arg, TRUE);
		}
		else if (ncandidates == 1)
		{
			tup = SearchSysCacheTuple(OPERNAME,
									  PointerGetDatum(op),
									  ObjectIdGetDatum(InvalidOid),
									  ObjectIdGetDatum(candidates->args[0]),
									  CharGetDatum('l'));
		}
		else
		{
			targetOid = oper_select_candidate(1, &arg, candidates);
			if (targetOid != NULL)
				tup = SearchSysCacheTuple(OPERNAME,
										  PointerGetDatum(op),
										  ObjectIdGetDatum(InvalidOid),
										  ObjectIdGetDatum(targetOid[0]),
										  CharGetDatum('l'));
		}

		if (!HeapTupleIsValid(tup))
			unary_op_error(op, arg, TRUE);
	}

	return (Operator) tup;
}	/* left_oper() */


/* op_error()
 * Give a somewhat useful error message when the operator for two types
 * is not found.
 */
static void
op_error(char *op, Oid arg1, Oid arg2)
{
	Type		tp1 = NULL,
				tp2 = NULL;

	if (typeidIsValid(arg1))
		tp1 = typeidType(arg1);
	else
		elog(ERROR, "Left hand side of operator '%s' has an unknown type"
			 "\n\tProbably a bad attribute name", op);

	if (typeidIsValid(arg2))
		tp2 = typeidType(arg2);
	else
		elog(ERROR, "Right hand side of operator %s has an unknown type"
			 "\n\tProbably a bad attribute name", op);

	elog(ERROR, "Unable to identify an operator '%s' for types '%s' and '%s'"
		 "\n\tYou will have to retype this query using an explicit cast",
		 op, typeTypeName(tp1), typeTypeName(tp2));
}

/* unary_op_error()
 * Give a somewhat useful error message when the operator for one type
 * is not found.
 */
static void
unary_op_error(char *op, Oid arg, bool is_left_op)
{
	Type		tp1 = NULL;

	if (typeidIsValid(arg))
		tp1 = typeidType(arg);
	else
	{
		elog(ERROR, "Argument of %s operator '%s' has an unknown type"
			 "\n\tProbably a bad attribute name",
			 (is_left_op ? "left" : "right"),
			 op);
	}

	elog(ERROR, "Unable to identify a %s operator '%s' for type '%s'"
		 "\n\tYou may need to add parentheses or an explicit cast",
		 (is_left_op ? "left" : "right"),
		 op, typeTypeName(tp1));
}
