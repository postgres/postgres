/*-------------------------------------------------------------------------
 *
 * parse_oper.h
 *		handle operator things for parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_oper.c,v 1.11 1998/05/09 23:29:53 thomas Exp $
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

extern
Oid *
func_select_candidate(int nargs, Oid *input_typeids, CandidateList candidates);

extern
Oid *
oper_select_candidate(int nargs, Oid *input_typeids, CandidateList candidates);

static int
binary_oper_get_candidates(char *opname,
						   Oid leftTypeId,
						   Oid rightTypeId,
						   CandidateList *candidates);
static CandidateList
binary_oper_select_candidate(Oid arg1,
							 Oid arg2,
							 CandidateList candidates);
static bool equivalentOpersAfterPromotion(CandidateList candidates);
static void op_error(char *op, Oid arg1, Oid arg2);
static int
unary_oper_get_candidates(char *op,
						  Oid typeId,
						  CandidateList *candidates,
						  char rightleft);


Oid
any_ordering_op(int restype)
{
	Operator	order_op;
	Oid			order_opid;

	order_op = oper("<", restype, restype, false);
	order_opid = oprid(order_op);

	return order_opid;
}

/* given operator, return the operator OID */
Oid
oprid(Operator op)
{
	return (op->t_oid);
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
	OperatorTupleForm oper;
	Buffer		buffer;
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

#if FALSE
	if (leftTypeId == UNKNOWNOID)
	{
		if (rightTypeId == UNKNOWNOID)
		{
			nkeys = 2;
		}
		else
		{
			nkeys = 3;

			ScanKeyEntryInitialize(&opKey[2], 0,
								   Anum_pg_operator_oprright,
								   ObjectIdEqualRegProcedure,
								   ObjectIdGetDatum(rightTypeId));
		}
	}
	else if (rightTypeId == UNKNOWNOID)
	{
		nkeys = 3;

		ScanKeyEntryInitialize(&opKey[2], 0,
							   Anum_pg_operator_oprleft,
							   ObjectIdEqualRegProcedure,
							   ObjectIdGetDatum(leftTypeId));
	}
	else
	{
		/* currently only "unknown" can be coerced */
		return 0;
#endif

	nkeys = 2;

	pg_operator_desc = heap_openr(OperatorRelationName);
	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  true,
									  nkeys,
									  opKey);

	do
	{
		tup = heap_getnext(pg_operator_scan, 0, &buffer);
		if (HeapTupleIsValid(tup))
		{
			current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
			current_candidate->args = (Oid *) palloc(2 * sizeof(Oid));

			oper = (OperatorTupleForm) GETSTRUCT(tup);
			current_candidate->args[0] = oper->oprleft;
			current_candidate->args[1] = oper->oprright;
			current_candidate->next = *candidates;
			*candidates = current_candidate;
			ncandidates++;
			ReleaseBuffer(buffer);
		}
	} while (HeapTupleIsValid(tup));

	heap_endscan(pg_operator_scan);
	heap_close(pg_operator_desc);

	return ncandidates;
} /* binary_oper_get_candidates() */


#if FALSE
/* BinaryOperCandidates()
 *	Given opname, leftTypeId and rightTypeId,
 *	find all possible (arg1, arg2) pairs for which an operator named
 *	opname exists, such that leftTypeId can be coerced to arg1 and
 *	rightTypeId can be coerced to arg2.
 */
static int
BinaryOperCandidates(char *opname,
					 Oid lTypeId,
					 Oid rTypeId,
					 CandidateList *candidates)
{
	CandidateList current_candidate;
	Relation	pg_operator_desc;
	HeapScanDesc pg_operator_scan;
	HeapTuple	tup;
	OperatorTupleForm oper;
	Buffer		buffer;
	int			nkeys;
	int			ncandidates = 0;
	ScanKeyData opKey[3];

	/* Can we promote the lesser type and find a match? */
	lCandidateTypeId = lTypeId;
	rCandidateTypeId = rTypeId;
	higherTypeId = PromoteLowerType(&lCandidateTypeId, &rCandidateTypeId);
	if (lTypeId != higherTypeId)
		lowerTypeId = lTypeId;
	else
		lowerTypeId = rTypeId;

	while (lCandidateTypeId != rCandidateTypeId)
		if ((lCandidateTypeId == InvalidOid) || (rCandidateTypeId == InvalidOid))
			break;

		tup = SearchSysCacheTuple(OPRNAME,
								  PointerGetDatum(op),
								  ObjectIdGetDatum(lCandidateTypeId),
								  ObjectIdGetDatum(rCandidateTypeId),
								  Int8GetDatum('b'));
		if (HeapTupleIsValid(tup))
			return ((Operator) tup);

		PromoteLowerType(&lCandidateTypeId, &rCandidateTypeId);
	}

	/* Can we promote the lesser type directly to the other? */
	if (can_coerce_type(lowerTypeId, higherTypeId))
	{
		tup = SearchSysCacheTuple(OPRNAME,
								  PointerGetDatum(op),
								  ObjectIdGetDatum(higherTypeId),
								  ObjectIdGetDatum(higherTypeId),
								  Int8GetDatum('b'));
		if (HeapTupleIsValid(tup))
			return ((Operator) tup);
	}


	*candidates = NULL;

	ScanKeyEntryInitialize(&opKey[0], 0,
						   Anum_pg_operator_oprname,
						   NameEqualRegProcedure,
						   NameGetDatum(opname));

	ScanKeyEntryInitialize(&opKey[1], 0,
						   Anum_pg_operator_oprkind,
						   CharacterEqualRegProcedure,
						   CharGetDatum('b'));

#if FALSE
	if (leftTypeId == UNKNOWNOID)
	{
		if (rightTypeId == UNKNOWNOID)
		{
			nkeys = 2;
		}
		else
		{
			nkeys = 3;

			ScanKeyEntryInitialize(&opKey[2], 0,
								   Anum_pg_operator_oprright,
								   F_OIDEQ,
								   ObjectIdGetDatum(rightTypeId));
		}
	}
	else if (rightTypeId == UNKNOWNOID)
	{
		nkeys = 3;

		ScanKeyEntryInitialize(&opKey[2], 0,
							   Anum_pg_operator_oprleft,
							   F_OIDEQ,
							   ObjectIdGetDatum(leftTypeId));
	}
	else
	{
		/* currently only "unknown" can be coerced */
		return 0;
#endif

	nkeys = 2;

	pg_operator_desc = heap_openr(OperatorRelationName);
	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  true,
									  nkeys,
									  opKey);

	do
	{
		tup = heap_getnext(pg_operator_scan, 0, &buffer);
		if (HeapTupleIsValid(tup))
		{
			current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
			current_candidate->args = (Oid *) palloc(2 * sizeof(Oid));

			oper = (OperatorTupleForm) GETSTRUCT(tup);
			current_candidate->args[0] = oper->oprleft;
			current_candidate->args[1] = oper->oprright;
			current_candidate->next = *candidates;
			*candidates = current_candidate;
			ncandidates++;
			ReleaseBuffer(buffer);
		}
	} while (HeapTupleIsValid(tup));

	heap_endscan(pg_operator_scan);
	heap_close(pg_operator_desc);

	return ncandidates;
} /* BinaryOperCandidates() */
#endif


/*
 * equivalentOpersAfterPromotion -
 *	  checks if a list of candidate operators obtained from
 *	  binary_oper_get_candidates() contain equivalent operators. If
 *	  this routine is called, we have more than 1 candidate and need to
 *	  decided whether to pick one of them. This routine returns true if
 *	  all the candidates operate on the same data types after
 *	  promotion (int2, int4, float4 -> float8).
 */
static bool
equivalentOpersAfterPromotion(CandidateList candidates)
{
	CandidateList result;
	CandidateList promotedCandidates = NULL;
	Oid			leftarg,
				rightarg;

	for (result = candidates; result != NULL; result = result->next)
	{
		CandidateList c;

		c = (CandidateList) palloc(sizeof(*c));
		c->args = (Oid *) palloc(2 * sizeof(Oid));
		switch (result->args[0])
		{
			case FLOAT4OID:
			case INT4OID:
			case INT2OID:
			case CASHOID:
				c->args[0] = FLOAT8OID;
				break;
			default:
				c->args[0] = result->args[0];
				break;
		}
		switch (result->args[1])
		{
			case FLOAT4OID:
			case INT4OID:
			case INT2OID:
			case CASHOID:
				c->args[1] = FLOAT8OID;
				break;
			default:
				c->args[1] = result->args[1];
				break;
		}
		c->next = promotedCandidates;
		promotedCandidates = c;
	}

	/*
	 * if we get called, we have more than 1 candidates so we can do the
	 * following safely
	 */
	leftarg = promotedCandidates->args[0];
	rightarg = promotedCandidates->args[1];

	for (result = promotedCandidates->next; result != NULL; result = result->next)
	{
		if (result->args[0] != leftarg || result->args[1] != rightarg)

			/*
			 * this list contains operators that operate on different data
			 * types even after promotion. Hence we can't decide on which
			 * one to pick. The user must do explicit type casting.
			 */
			return FALSE;
	}

	/*
	 * all the candidates are equivalent in the following sense: they
	 * operate on equivalent data types and picking any one of them is as
	 * good.
	 */
	return TRUE;
}


/* binary_oper_select_candidate()
 * Given a choice of argument type pairs for a binary operator,
 *  try to choose a default pair.
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
static CandidateList
binary_oper_select_candidate(Oid arg1,
							 Oid arg2,
							 CandidateList candidates)
{
	CandidateList result;

	/*
	 * If both are "unknown", there is no way to select a candidate
	 */
	if (arg1 == UNKNOWNOID && arg2 == UNKNOWNOID)
		return (NULL);

	if (!equivalentOpersAfterPromotion(candidates))
		return NULL;

	/*
	 * if we get here, any one will do but we're more picky and require
	 * both operands be the same.
	 */
	for (result = candidates; result != NULL; result = result->next)
	{
		if (result->args[0] == result->args[1])
			return result;
	}

	return (NULL);
}

/* oper()
 * Given operator, types of arg1, and arg2, return oper struct.
 * Inputs:
 * arg1, arg2: Type IDs
 */
Operator
oper(char *op, Oid arg1, Oid arg2, bool noWarnings)
{
	HeapTuple		tup;
	CandidateList	candidates;
	int				ncandidates;
	Oid			   *targetOids;
	Oid				inputOids[2];

	/* Unspecified type for one of the arguments? then use the other */
	if (arg2 == InvalidOid)
		arg2 = arg1;
	if (arg1 == InvalidOid)
		arg1 = arg2;

	tup = SearchSysCacheTuple(OPRNAME,
							  PointerGetDatum(op),
							  ObjectIdGetDatum(arg1),
							  ObjectIdGetDatum(arg2),
							  Int8GetDatum('b'));

	/* Did not find anything? then look more carefully... */
	if (!HeapTupleIsValid(tup))
	{
		ncandidates = binary_oper_get_candidates(op, arg1, arg2, &candidates);

		/* No operators found? Then throw error or return null... */
		if (ncandidates == 0)
		{
			if (!noWarnings)
				op_error(op, arg1, arg2);
			return (NULL);
		}

		/* Or found exactly one? Then proceed... */
		else if (ncandidates == 1)
		{
			tup = SearchSysCacheTuple(OPRNAME,
									  PointerGetDatum(op),
									  ObjectIdGetDatum(candidates->args[0]),
									  ObjectIdGetDatum(candidates->args[1]),
									  Int8GetDatum('b'));
			Assert(HeapTupleIsValid(tup));
		}

		/* Otherwise, multiple operators of the desired types found... */
		else
		{
#if FALSE
			candidates = binary_oper_select_candidate(arg1, arg2, candidates);
#endif
			inputOids[0] = arg1;
			inputOids[1] = arg2;
			targetOids = oper_select_candidate(2, inputOids, candidates);
#if FALSE
			targetOids = func_select_candidate(2, inputOids, candidates);
#endif
			if (targetOids != NULL)
			{
#if PARSEDEBUG
printf("oper: found candidate\n");
#endif
				tup = SearchSysCacheTuple(OPRNAME,
										  PointerGetDatum(op),
										  ObjectIdGetDatum(targetOids[0]),
										  ObjectIdGetDatum(targetOids[1]),
										  Int8GetDatum('b'));
			}
			else
			{
				tup = NULL;
			}

			/* Could not choose one, for whatever reason... */
			if (!HeapTupleIsValid(tup))
			{
				if (!noWarnings)
				{
					elog(ERROR, "There is more than one operator '%s' for types '%s' and '%s'"
						"\n\tYou will have to retype this query using an explicit cast",
						op, typeTypeName(typeidType(arg1)), typeTypeName(typeidType(arg2)));
				}
				return (NULL);
			}
		}
	}
	return ((Operator) tup);
} /* oper() */


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
	OperatorTupleForm oper;
	Buffer		buffer;
	int			ncandidates = 0;

	static ScanKeyData opKey[2] = {
		{0, Anum_pg_operator_oprname, F_NAMEEQ},
	{0, Anum_pg_operator_oprkind, F_CHAREQ}};

	*candidates = NULL;

	fmgr_info(F_NAMEEQ, (FmgrInfo *) &opKey[0].sk_func);
	opKey[0].sk_argument = NameGetDatum(op);
	fmgr_info(F_CHAREQ, (FmgrInfo *) &opKey[1].sk_func);
	opKey[1].sk_argument = CharGetDatum(rightleft);

#if FALSE
	/* currently, only "unknown" can be coerced */

	/*
	 * but we should allow types that are internally the same to be
	 * "coerced"
	 */
	if (typeId != UNKNOWNOID)
	{
		return 0;
	}
#endif

#ifdef PARSEDEBUG
printf("unary_oper_get_candidates: start scan for '%s'\n", op);
#endif
	pg_operator_desc = heap_openr(OperatorRelationName);
	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  true,
									  2,
									  opKey);

	do
	{
		tup = heap_getnext(pg_operator_scan, 0, &buffer);
		if (HeapTupleIsValid(tup))
		{
			current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
			current_candidate->args = (Oid *) palloc(sizeof(Oid));

			oper = (OperatorTupleForm) GETSTRUCT(tup);
			if (rightleft == 'r')
				current_candidate->args[0] = oper->oprleft;
			else
				current_candidate->args[0] = oper->oprright;
			current_candidate->next = *candidates;
			*candidates = current_candidate;
#ifdef PARSEDEBUG
printf("unary_oper_get_candidates: found candidate '%s' for type %s\n",
 op, typeidTypeName(current_candidate->args[0]));
#endif
			ncandidates++;
			ReleaseBuffer(buffer);
		}
	} while (HeapTupleIsValid(tup));

	heap_endscan(pg_operator_scan);
	heap_close(pg_operator_desc);

#ifdef PARSEDEBUG
printf("unary_oper_get_candidates: found %d candidates\n", ncandidates);
#endif
	return ncandidates;
} /* unary_oper_get_candidates() */


/* Given unary right-side operator (operator on right), return oper struct */
/* arg-- type id */
Operator
right_oper(char *op, Oid arg)
{
	HeapTuple		tup;
	CandidateList	candidates;
	int				ncandidates;
	Oid			   *targetOid;

	tup = SearchSysCacheTuple(OPRNAME,
							  PointerGetDatum(op),
							  ObjectIdGetDatum(arg),
							  ObjectIdGetDatum(InvalidOid),
							  Int8GetDatum('r'));

	if (!HeapTupleIsValid(tup))
	{
		ncandidates = unary_oper_get_candidates(op, arg, &candidates, 'r');
		if (ncandidates == 0)
		{
			elog(ERROR, "Can't find right op '%s' for type %d", op, arg);
			return (NULL);
		}
		else if (ncandidates == 1)
		{
			tup = SearchSysCacheTuple(OPRNAME,
									  PointerGetDatum(op),
								   ObjectIdGetDatum(candidates->args[0]),
									  ObjectIdGetDatum(InvalidOid),
									  Int8GetDatum('r'));
			Assert(HeapTupleIsValid(tup));
		}
		else
		{
#if FALSE
			elog(ERROR, "There is more than one right operator %s"
				"\n\tYou will have to retype this query using an explicit cast", op);
#endif
			targetOid = func_select_candidate(1, &arg, candidates);

			if (targetOid != NULL)
			{
				tup = SearchSysCacheTuple(OPRNAME,
										  PointerGetDatum(op),
										  ObjectIdGetDatum(InvalidOid),
										  ObjectIdGetDatum(*targetOid),
										  Int8GetDatum('r'));
			}
			else
			{
				tup = NULL;
			}

			if (!HeapTupleIsValid(tup))
			{
				elog(ERROR, "Unable to convert right operator '%s' from type %s to %s",
					 op, typeidTypeName(arg), typeidTypeName(*targetOid));
				return (NULL);
			}
		}
	}
	return ((Operator) tup);
} /* right_oper() */


/* Given unary left-side operator (operator on left), return oper struct */
/* arg--type id */
Operator
left_oper(char *op, Oid arg)
{
	HeapTuple		tup;
	CandidateList	candidates;
	int				ncandidates;
	Oid			   *targetOid;

	tup = SearchSysCacheTuple(OPRNAME,
							  PointerGetDatum(op),
							  ObjectIdGetDatum(InvalidOid),
							  ObjectIdGetDatum(arg),
							  Int8GetDatum('l'));

	if (!HeapTupleIsValid(tup))
	{
		ncandidates = unary_oper_get_candidates(op, arg, &candidates, 'l');
		if (ncandidates == 0)
		{
			elog(ERROR, "Can't find left op '%s' for type %d", op, arg);
			return (NULL);
		}
		else if (ncandidates == 1)
		{
			tup = SearchSysCacheTuple(OPRNAME,
									  PointerGetDatum(op),
									  ObjectIdGetDatum(InvalidOid),
									  ObjectIdGetDatum(candidates->args[0]),
									  Int8GetDatum('l'));
			Assert(HeapTupleIsValid(tup));
#ifdef PARSEDEBUG
printf("left_oper: searched cache for single left oper candidate '%s %s'\n",
 op, typeidTypeName((Oid) candidates->args[0]));
#endif
		}
		else
		{
#if FALSE
			elog(ERROR, "There is more than one left operator %s"
				"\n\tYou will have to retype this query using an explicit cast", op);
#endif
			targetOid = func_select_candidate(1, &arg, candidates);
			tup = SearchSysCacheTuple(OPRNAME,
									  PointerGetDatum(op),
									  ObjectIdGetDatum(InvalidOid),
									  ObjectIdGetDatum(*targetOid),
									  Int8GetDatum('l'));

			if (!HeapTupleIsValid(tup))
			{
				elog(ERROR, "Unable to convert left operator '%s' from type %s to %s",
					 op, typeidTypeName(arg), typeidTypeName(*targetOid));
				return (NULL);
			}
#ifdef PARSEDEBUG
printf("left_oper: searched cache for best left oper candidate '%s %s'\n",
 op, typeidTypeName(*targetOid));
#endif
		}
	}
	return ((Operator) tup);
} /* left_oper() */


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
	{
		tp1 = typeidType(arg1);
	}
	else
	{
		elog(ERROR, "Left hand side of operator '%s' has an unknown type"
			"\n\tProbably a bad attribute name", op);
	}

	if (typeidIsValid(arg2))
	{
		tp2 = typeidType(arg2);
	}
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
