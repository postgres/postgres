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
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_oper.c,v 1.67 2003/06/27 00:33:25 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

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
	Oid			elem_type;

	/*
	 * If the datatype is an array, then we can use array_eq ... but only
	 * if there is a suitable equality operator for the element type.
	 * (We must run this test first, since compatible_oper will find
	 * array_eq, but would not notice the lack of an element operator.)
	 */
	elem_type = get_element_type(argtype);
	if (OidIsValid(elem_type))
	{
		optup = equality_oper(elem_type, true);
		if (optup != NULL)
		{
			ReleaseSysCache(optup);
			return SearchSysCache(OPEROID,
								  ObjectIdGetDatum(ARRAY_EQ_OP),
								  0, 0, 0);
		}
	}
	else
	{
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
	Oid			elem_type;

	/*
	 * If the datatype is an array, then we can use array_lt ... but only
	 * if there is a suitable ordering operator for the element type.
	 * (We must run this test first, since the code below would find
	 * array_lt if there's an element = operator, but would not notice the
	 * lack of an element < operator.)
	 */
	elem_type = get_element_type(argtype);
	if (OidIsValid(elem_type))
	{
		optup = ordering_oper(elem_type, true);
		if (optup != NULL)
		{
			ReleaseSysCache(optup);
			return SearchSysCache(OPEROID,
								  ObjectIdGetDatum(ARRAY_LT_OP),
								  0, 0, 0);
		}
	}
	else
	{
		/*
		 * Find the type's equality operator, and use its lsortop (it *must*
		 * be mergejoinable).  We use this definition because for sorting and
		 * grouping purposes, it's important that the equality and ordering
		 * operators are consistent.
		 */
		optup = equality_oper(argtype, noError);
		if (optup != NULL)
		{
			Oid		lsortop;

			lsortop = ((Form_pg_operator) GETSTRUCT(optup))->oprlsortop;
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

/*
 * ordering_oper_funcid - convenience routine for oprfuncid(ordering_oper())
 */
Oid
ordering_oper_funcid(Oid argtype)
{
	Operator	optup;
	Oid			result;

	optup = ordering_oper(argtype, false);
	result = oprfuncid(optup);
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
 *		Given the input argtype array and one or more candidates
 *		for the operator, attempt to resolve the conflict.
 *
 * Returns the OID of the selected operator if the conflict can be resolved,
 * otherwise returns InvalidOid.
 *
 * Note that the caller has already determined that there is no candidate
 * exactly matching the input argtype(s).  Incompatible candidates are not yet
 * pruned away, however.
 */
static Oid
oper_select_candidate(int nargs,
					  Oid *input_typeids,
					  FuncCandidateList candidates)
{
	int			ncandidates;

	/*
	 * Delete any candidates that cannot actually accept the given
	 * input types, whether directly or by coercion.
	 */
	ncandidates = func_match_argtypes(nargs, input_typeids,
									  candidates, &candidates);

	/* Done if no candidate or only one candidate survives */
	if (ncandidates == 0)
		return InvalidOid;
	if (ncandidates == 1)
		return candidates->oid;

	/*
	 * Use the same heuristics as for ambiguous functions to resolve
	 * the conflict.
	 */
	candidates = func_select_candidate(nargs, input_typeids, candidates);

	if (candidates)
		return candidates->oid;

	return InvalidOid;			/* failed to select a best candidate */
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
			 * other (XXX this is probably dead code?)
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
 *
 * As with coerce_type, pstate may be NULL if no special unknown-Param
 * processing is wanted.
 */
Expr *
make_op(ParseState *pstate, List *opname, Node *ltree, Node *rtree)
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
	result = make_op_expr(pstate, tup, ltree, rtree, ltypeId, rtypeId);

	ReleaseSysCache(tup);

	return result;
}


/*
 * make_op_expr()
 *		Build operator expression using an already-looked-up operator.
 *
 * As with coerce_type, pstate may be NULL if no special unknown-Param
 * processing is wanted.
 */
Expr *
make_op_expr(ParseState *pstate, Operator op,
			 Node *ltree, Node *rtree,
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
	make_fn_arguments(pstate, args, actual_arg_types, declared_arg_types);

	/* and build the expression node */
	result = makeNode(OpExpr);
	result->opno = oprid(op);
	result->opfuncid = InvalidOid;
	result->opresulttype = rettype;
	result->opretset = get_func_retset(opform->oprcode);
	result->args = args;

	return (Expr *) result;
}
