/*-------------------------------------------------------------------------
 *
 * parse_oper.h
 *		handle operator things for parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_oper.c,v 1.19 1998/09/25 13:36:07 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "postgres.h"

#include "access/heapam.h"
#include "access/relscan.h"
#include "catalog/catname.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "parser/parse_coerce.h"
#include "storage/bufmgr.h"
#include "utils/syscache.h"

Oid * oper_select_candidate(int nargs, Oid *input_typeids, CandidateList candidates);
static int binary_oper_get_candidates(char *opname,
						   Oid leftTypeId,
						   Oid rightTypeId,
						   CandidateList *candidates);
static int unary_oper_get_candidates(char *op,
						  Oid typeId,
						  CandidateList *candidates,
						  char rightleft);
static void op_error(char *op, Oid arg1, Oid arg2);

Oid
any_ordering_op(int restype)
{
	Operator	order_op;
	Oid			order_opid;

	order_op = oper("<", restype, restype, TRUE);
	if (!HeapTupleIsValid(order_op))
	{
		elog(ERROR, "Unable to find an ordering operator '%s' for type %s."
			 "\n\tUse an explicit ordering operator or modify the query.",
			 "<", typeidTypeName(restype));
	}
	order_opid = oprid(order_op);

	return order_opid;
}

/* given operator, return the operator OID */
Oid
oprid(Operator op)
{
	return op->t_oid;
}


/* binary_oper_get_candidates()
 *	given opname, leftTypeId and rightTypeId,
 *	find all possible (arg1, arg2) pairs for which an operator named
 *	opname exists, such that leftTypeId can be coerced to arg1 and
 *	rightTypeId can be coerced to arg2
 */
static int
binary_oper_get_candidates(char *opname,
						   Oid leftTypeId,
						   Oid rightTypeId,
						   CandidateList *candidates)
{
	CandidateList current_candidate;
	Relation	pg_operator_desc;
	HeapScanDesc pg_operator_scan;
	HeapTuple	tup;
	Form_pg_operator oper;
	int			nkeys;
	int			ncandidates = 0;
	ScanKeyData opKey[3];

	*candidates = NULL;

	ScanKeyEntryInitialize(&opKey[0], 0,
						   Anum_pg_operator_oprname,
						   F_NAMEEQ,
						   NameGetDatum(opname));

	ScanKeyEntryInitialize(&opKey[1], 0,
						   Anum_pg_operator_oprkind,
						   F_CHAREQ,
						   CharGetDatum('b'));

	nkeys = 2;

	pg_operator_desc = heap_openr(OperatorRelationName);
	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  SnapshotSelf,		/* ??? */
									  nkeys,
									  opKey);

	while (HeapTupleIsValid(tup = heap_getnext(pg_operator_scan, 0)))
	{
		current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
		current_candidate->args = (Oid *) palloc(2 * sizeof(Oid));

		oper = (Form_pg_operator) GETSTRUCT(tup);
		current_candidate->args[0] = oper->oprleft;
		current_candidate->args[1] = oper->oprright;
		current_candidate->next = *candidates;
		*candidates = current_candidate;
		ncandidates++;
	}

	heap_endscan(pg_operator_scan);
	heap_close(pg_operator_desc);

	return ncandidates;
}	/* binary_oper_get_candidates() */


/* oper_select_candidate()
 * Given the input argtype array and more than one candidate
 * for the function argtype array, attempt to resolve the conflict.
 * returns the selected argtype array if the conflict can be resolved,
 * otherwise returns NULL.
 *
 * This routine is new code, replacing binary_oper_select_candidate()
 * which dates from v4.2/v1.0.x days. It tries very hard to match up
 * operators with types, including allowing type coersions if necessary.
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
Oid *
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
 * Run through all candidates and keep those with the most matches
 *	on explicit types. Keep all candidates if none match.
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
			if ((input_typeids[i] != UNKNOWNOID)
				&& (current_typeids[i] == input_typeids[i]))
				nmatch++;
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
		else
		{
			last_candidate->next = NULL;
		}
	}

	if (ncandidates <= 1)
	{
		if (!can_coerce_type(1, &input_typeids[0], &candidates->args[0])
		 || !can_coerce_type(1, &input_typeids[1], &candidates->args[1]))
			ncandidates = 0;
		return (ncandidates == 1) ? candidates->args : NULL;
	}

/*
 * Still too many candidates?
 * Now look for candidates which allow coersion and are preferred types.
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
			current_category = TypeCategory(current_typeids[i]);
			if (input_typeids[i] != UNKNOWNOID)
			{
				if (current_typeids[i] == input_typeids[i])
					nmatch++;
				else if (IsPreferredType(current_category, current_typeids[i])
						 && can_coerce_type(1, &input_typeids[i], &current_typeids[i]))
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
		else
		{
			last_candidate->next = NULL;
		}
	}

	if (ncandidates <= 1)
	{
		if (!can_coerce_type(1, &input_typeids[0], &candidates->args[0])
		 || ((nargs > 1) && !can_coerce_type(1, &input_typeids[1], &candidates->args[1])))
		{
			ncandidates = 0;
		}
		return (ncandidates == 1) ? candidates->args : NULL;
	}

/*
 * Still too many candidates?
 * Try assigning types for the unknown columns.
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
			nmatch = 0;
			for (i = 0; i < nargs; i++)
			{
				current_typeids = current_candidate->args;
				if ((current_type == current_typeids[i])
				|| IS_BINARY_COMPATIBLE(current_type, current_typeids[i]))
					nmatch++;
			}
			if (nmatch == nargs)
				return candidates->args;
		}
	}

	for (i = 0; i < nargs; i++)
	{
		if (input_typeids[i] == UNKNOWNOID)
		{
			slot_category = INVALID_TYPE;
			slot_type = InvalidOid;
			for (current_candidate = candidates;
				 current_candidate != NULL;
				 current_candidate = current_candidate->next)
			{
				current_typeids = current_candidate->args;
				current_type = current_typeids[i];
				current_category = TypeCategory(current_typeids[i]);
				if (slot_category == InvalidOid)
				{
					slot_category = current_category;
					slot_type = current_type;
				}
				else if (current_category != slot_category)
				{
					return NULL;
				}
				else if (current_type != slot_type)
				{
					if (IsPreferredType(slot_category, current_type))
					{
						slot_type = current_type;
						candidates = current_candidate;
					}
					else
					{
					}
				}
			}

			if (slot_type != InvalidOid)
			{
				input_typeids[i] = slot_type;
			}
		}
		else
		{
		}
	}

	ncandidates = 0;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		if (can_coerce_type(1, &input_typeids[0], &current_candidate->args[0])
			&& can_coerce_type(1, &input_typeids[1], &current_candidate->args[1]))
			ncandidates++;
	}

	return (ncandidates == 1) ? candidates->args : NULL;
}	/* oper_select_candidate() */


/* oper_exact()
 * Given operator, and arguments, return oper struct.
 * Inputs:
 * arg1, arg2: Type IDs
 */
Operator
oper_exact(char *op, Oid arg1, Oid arg2, Node **ltree, Node **rtree, bool noWarnings)
{
	HeapTuple	tup;
	Node	   *tree;

	/* Unspecified type for one of the arguments? then use the other */
	if ((arg1 == UNKNOWNOID) && (arg2 != InvalidOid))
		arg1 = arg2;
	else if ((arg2 == UNKNOWNOID) && (arg1 != InvalidOid))
		arg2 = arg1;

	tup = SearchSysCacheTuple(OPRNAME,
							  PointerGetDatum(op),
							  ObjectIdGetDatum(arg1),
							  ObjectIdGetDatum(arg2),
							  CharGetDatum('b'));

	/*
	 * Did not find anything? then try flipping arguments on a commutative
	 * operator...
	 */
	if (!HeapTupleIsValid(tup) && (arg1 != arg2))
	{
		tup = SearchSysCacheTuple(OPRNAME,
								  PointerGetDatum(op),
								  ObjectIdGetDatum(arg2),
								  ObjectIdGetDatum(arg1),
								  CharGetDatum('b'));

		if (HeapTupleIsValid(tup))
		{
			Form_pg_operator opform;

			opform = (Form_pg_operator) GETSTRUCT(tup);
			if (opform->oprcom == tup->t_oid)
			{
				if ((ltree != NULL) && (rtree != NULL))
				{
					tree = *ltree;
					*ltree = *rtree;
					*rtree = tree;
				}
			}
			/* disable for now... - thomas 1998-05-14 */
			else
				tup = NULL;
		}
		if (!HeapTupleIsValid(tup) && (!noWarnings))
			op_error(op, arg1, arg2);
	}

	return tup;
}	/* oper_exact() */


/* oper_inexact()
 * Given operator, types of arg1, and arg2, return oper struct.
 * Inputs:
 * arg1, arg2: Type IDs
 */
Operator
oper_inexact(char *op, Oid arg1, Oid arg2, Node **ltree, Node **rtree, bool noWarnings)
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

	ncandidates = binary_oper_get_candidates(op, arg1, arg2, &candidates);

	/* No operators found? Then throw error or return null... */
	if (ncandidates == 0)
	{
		if (!noWarnings)
			op_error(op, arg1, arg2);
		return NULL;
	}

	/* Or found exactly one? Then proceed... */
	else if (ncandidates == 1)
	{
		tup = SearchSysCacheTuple(OPRNAME,
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
			tup = SearchSysCacheTuple(OPRNAME,
									  PointerGetDatum(op),
									  ObjectIdGetDatum(targetOids[0]),
									  ObjectIdGetDatum(targetOids[1]),
									  CharGetDatum('b'));
		}
		else
			tup = NULL;

		/* Could not choose one, for whatever reason... */
		if (!HeapTupleIsValid(tup))
		{
			if (!noWarnings)
			{
				elog(ERROR, "There is more than one possible operator '%s' for types '%s' and '%s'"
					 "\n\tYou will have to retype this query using an explicit cast",
					 op, typeTypeName(typeidType(arg1)), typeTypeName(typeidType(arg2)));
			}
			return NULL;
		}
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
	if (HeapTupleIsValid(tup = oper_exact(opname, ltypeId, rtypeId, NULL, NULL, TRUE)))
	{
	}
	/* try to find a match on likely candidates... */
	else if (HeapTupleIsValid(tup = oper_inexact(opname, ltypeId, rtypeId, NULL, NULL, TRUE)))
	{
	}
	else if (!noWarnings)
	{
		elog(ERROR, "Unable to find binary operator '%s' for types %s and %s",
			 opname, typeTypeName(typeidType(ltypeId)), typeTypeName(typeidType(rtypeId)));
	}

	return (Operator) tup;
}	/* oper() */


/* unary_oper_get_candidates()
 *	given opname and typeId, find all possible types for which
 *	a right/left unary operator named opname exists,
 *	such that typeId can be coerced to it
 */
static int
unary_oper_get_candidates(char *op,
						  Oid typeId,
						  CandidateList *candidates,
						  char rightleft)
{
	CandidateList current_candidate;
	Relation	pg_operator_desc;
	HeapScanDesc pg_operator_scan;
	HeapTuple	tup;
	Form_pg_operator oper;
	int			ncandidates = 0;

	static ScanKeyData opKey[2] = {
		{0, Anum_pg_operator_oprname, F_NAMEEQ},
	{0, Anum_pg_operator_oprkind, F_CHAREQ}};

	*candidates = NULL;

	fmgr_info(F_NAMEEQ, (FmgrInfo *) &opKey[0].sk_func);
	opKey[0].sk_argument = NameGetDatum(op);
	fmgr_info(F_CHAREQ, (FmgrInfo *) &opKey[1].sk_func);
	opKey[1].sk_argument = CharGetDatum(rightleft);

	pg_operator_desc = heap_openr(OperatorRelationName);
	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  SnapshotSelf,		/* ??? */
									  2,
									  opKey);

	while (HeapTupleIsValid(tup = heap_getnext(pg_operator_scan, 0)))
	{
		current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
		current_candidate->args = (Oid *) palloc(sizeof(Oid));

		oper = (Form_pg_operator) GETSTRUCT(tup);
		if (rightleft == 'r')
			current_candidate->args[0] = oper->oprleft;
		else
			current_candidate->args[0] = oper->oprright;
		current_candidate->next = *candidates;
		*candidates = current_candidate;
		ncandidates++;
	}

	heap_endscan(pg_operator_scan);
	heap_close(pg_operator_desc);

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

	tup = SearchSysCacheTuple(OPRNAME,
							  PointerGetDatum(op),
							  ObjectIdGetDatum(arg),
							  ObjectIdGetDatum(InvalidOid),
							  CharGetDatum('r'));

	if (!HeapTupleIsValid(tup))
	{
		ncandidates = unary_oper_get_candidates(op, arg, &candidates, 'r');
		if (ncandidates == 0)
		{
			elog(ERROR, "Can't find right op '%s' for type %d", op, arg);
			return NULL;
		}
		else if (ncandidates == 1)
		{
			tup = SearchSysCacheTuple(OPRNAME,
									  PointerGetDatum(op),
								   ObjectIdGetDatum(candidates->args[0]),
									  ObjectIdGetDatum(InvalidOid),
									  CharGetDatum('r'));
			Assert(HeapTupleIsValid(tup));
		}
		else
		{
			targetOid = oper_select_candidate(1, &arg, candidates);

			if (targetOid != NULL)
			{
				tup = SearchSysCacheTuple(OPRNAME,
										  PointerGetDatum(op),
										  ObjectIdGetDatum(InvalidOid),
										  ObjectIdGetDatum(*targetOid),
										  CharGetDatum('r'));
			}
			else
			{
				tup = NULL;
			}

			if (!HeapTupleIsValid(tup))
			{
				elog(ERROR, "Unable to convert right operator '%s' from type %s",
					 op, typeidTypeName(arg));
				return NULL;
			}
		}
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

	tup = SearchSysCacheTuple(OPRNAME,
							  PointerGetDatum(op),
							  ObjectIdGetDatum(InvalidOid),
							  ObjectIdGetDatum(arg),
							  CharGetDatum('l'));

	if (!HeapTupleIsValid(tup))
	{
		ncandidates = unary_oper_get_candidates(op, arg, &candidates, 'l');
		if (ncandidates == 0)
		{
			elog(ERROR, "Can't find left op '%s' for type %d", op, arg);
			return NULL;
		}
		else if (ncandidates == 1)
		{
			tup = SearchSysCacheTuple(OPRNAME,
									  PointerGetDatum(op),
									  ObjectIdGetDatum(InvalidOid),
								   ObjectIdGetDatum(candidates->args[0]),
									  CharGetDatum('l'));
			Assert(HeapTupleIsValid(tup));
		}
		else
		{
			targetOid = oper_select_candidate(1, &arg, candidates);
			if (targetOid != NULL)
			{
				tup = SearchSysCacheTuple(OPRNAME,
										  PointerGetDatum(op),
										  ObjectIdGetDatum(InvalidOid),
										  ObjectIdGetDatum(*targetOid),
										  CharGetDatum('l'));
			}
			else
			{
				tup = NULL;
			}

			if (!HeapTupleIsValid(tup))
			{
				elog(ERROR, "Unable to convert left operator '%s' from type %s",
					 op, typeidTypeName(arg));
				return NULL;
			}
		}
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
	{
		elog(ERROR, "Left hand side of operator '%s' has an unknown type"
			 "\n\tProbably a bad attribute name", op);
	}

	if (typeidIsValid(arg2))
		tp2 = typeidType(arg2);
	else
	{
		elog(ERROR, "Right hand side of operator %s has an unknown type"
			 "\n\tProbably a bad attribute name", op);
	}

	elog(ERROR, "There is no operator '%s' for types '%s' and '%s'"
		 "\n\tYou will either have to retype this query using an explicit cast,"
	 "\n\tor you will have to define the operator using CREATE OPERATOR",
		 op, typeTypeName(tp1), typeTypeName(tp2));
}
