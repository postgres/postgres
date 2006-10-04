/*-------------------------------------------------------------------------
 *
 * parse_oper.c
 *		handle operator things for parser
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/parse_oper.c,v 1.90 2006/10/04 00:29:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static Oid	binary_oper_exact(List *opname, Oid arg1, Oid arg2);
static FuncDetailCode oper_select_candidate(int nargs,
					  Oid *input_typeids,
					  FuncCandidateList candidates,
					  Oid *operOid);
static const char *op_signature_string(List *op, char oprkind,
					Oid arg1, Oid arg2);
static void op_error(ParseState *pstate, List *op, char oprkind,
		 Oid arg1, Oid arg2,
		 FuncDetailCode fdresult, int location);
static Expr *make_op_expr(ParseState *pstate, Operator op,
			 Node *ltree, Node *rtree,
			 Oid ltypeId, Oid rtypeId);


/*
 * LookupOperName
 *		Given a possibly-qualified operator name and exact input datatypes,
 *		look up the operator.
 *
 * Pass oprleft = InvalidOid for a prefix op, oprright = InvalidOid for
 * a postfix op.
 *
 * If the operator name is not schema-qualified, it is sought in the current
 * namespace search path.
 *
 * If the operator is not found, we return InvalidOid if noError is true,
 * else raise an error.  pstate and location are used only to report the
 * error position; pass NULL/-1 if not available.
 */
Oid
LookupOperName(ParseState *pstate, List *opername, Oid oprleft, Oid oprright,
			   bool noError, int location)
{
	Oid			result;

	result = OpernameGetOprid(opername, oprleft, oprright);
	if (OidIsValid(result))
		return result;

	/* we don't use op_error here because only an exact match is wanted */
	if (!noError)
	{
		char		oprkind;

		if (!OidIsValid(oprleft))
			oprkind = 'l';
		else if (!OidIsValid(oprright))
			oprkind = 'r';
		else
			oprkind = 'b';

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator does not exist: %s",
						op_signature_string(opername, oprkind,
											oprleft, oprright)),
				 parser_errposition(pstate, location)));
	}

	return InvalidOid;
}

/*
 * LookupOperNameTypeNames
 *		Like LookupOperName, but the argument types are specified by
 *		TypeName nodes.
 *
 * Pass oprleft = NULL for a prefix op, oprright = NULL for a postfix op.
 */
Oid
LookupOperNameTypeNames(ParseState *pstate, List *opername,
						TypeName *oprleft, TypeName *oprright,
						bool noError, int location)
{
	Oid			leftoid,
				rightoid;

	if (oprleft == NULL)
		leftoid = InvalidOid;
	else
		leftoid = typenameTypeId(pstate, oprleft);

	if (oprright == NULL)
		rightoid = InvalidOid;
	else
		rightoid = typenameTypeId(pstate, oprright);

	return LookupOperName(pstate, opername, leftoid, rightoid,
						  noError, location);
}

/*
 * equality_oper - identify a suitable equality operator for a datatype
 *
 * On failure, return NULL if noError, else report a standard error
 */
Operator
equality_oper(Oid argtype, bool noError)
{
	TypeCacheEntry *typentry;
	Oid			oproid;
	Operator	optup;

	/*
	 * Look for an "=" operator for the datatype.  We require it to be an
	 * exact or binary-compatible match, since most callers are not prepared
	 * to cope with adding any run-time type coercion steps.
	 */
	typentry = lookup_type_cache(argtype, TYPECACHE_EQ_OPR);
	oproid = typentry->eq_opr;

	/*
	 * If the datatype is an array, then we can use array_eq ... but only if
	 * there is a suitable equality operator for the element type. (This check
	 * is not in the raw typcache.c code ... should it be?)
	 */
	if (oproid == ARRAY_EQ_OP)
	{
		Oid			elem_type = get_element_type(argtype);

		if (OidIsValid(elem_type))
		{
			optup = equality_oper(elem_type, true);
			if (optup != NULL)
				ReleaseSysCache(optup);
			else
				oproid = InvalidOid;	/* element type has no "=" */
		}
		else
			oproid = InvalidOid;	/* bogus array type? */
	}

	if (OidIsValid(oproid))
	{
		optup = SearchSysCache(OPEROID,
							   ObjectIdGetDatum(oproid),
							   0, 0, 0);
		if (optup == NULL)		/* should not fail */
			elog(ERROR, "cache lookup failed for operator %u", oproid);
		return optup;
	}

	if (!noError)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify an equality operator for type %s",
						format_type_be(argtype))));
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
	TypeCacheEntry *typentry;
	Oid			oproid;
	Operator	optup;

	/*
	 * Look for a "<" operator for the datatype.  We require it to be an exact
	 * or binary-compatible match, since most callers are not prepared to cope
	 * with adding any run-time type coercion steps.
	 *
	 * Note: the search algorithm used by typcache.c ensures that if a "<"
	 * operator is returned, it will be consistent with the "=" operator
	 * returned by equality_oper.  This is critical for sorting and grouping
	 * purposes.
	 */
	typentry = lookup_type_cache(argtype, TYPECACHE_LT_OPR);
	oproid = typentry->lt_opr;

	/*
	 * If the datatype is an array, then we can use array_lt ... but only if
	 * there is a suitable less-than operator for the element type. (This
	 * check is not in the raw typcache.c code ... should it be?)
	 */
	if (oproid == ARRAY_LT_OP)
	{
		Oid			elem_type = get_element_type(argtype);

		if (OidIsValid(elem_type))
		{
			optup = ordering_oper(elem_type, true);
			if (optup != NULL)
				ReleaseSysCache(optup);
			else
				oproid = InvalidOid;	/* element type has no "<" */
		}
		else
			oproid = InvalidOid;	/* bogus array type? */
	}

	if (OidIsValid(oproid))
	{
		optup = SearchSysCache(OPEROID,
							   ObjectIdGetDatum(oproid),
							   0, 0, 0);
		if (optup == NULL)		/* should not fail */
			elog(ERROR, "cache lookup failed for operator %u", oproid);
		return optup;
	}

	if (!noError)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify an ordering operator for type %s",
						format_type_be(argtype)),
		 errhint("Use an explicit ordering operator or modify the query.")));
	return NULL;
}

/*
 * reverse_ordering_oper - identify DESC sort operator (">") for a datatype
 *
 * On failure, return NULL if noError, else report a standard error
 */
Operator
reverse_ordering_oper(Oid argtype, bool noError)
{
	TypeCacheEntry *typentry;
	Oid			oproid;
	Operator	optup;

	/*
	 * Look for a ">" operator for the datatype.  We require it to be an exact
	 * or binary-compatible match, since most callers are not prepared to cope
	 * with adding any run-time type coercion steps.
	 *
	 * Note: the search algorithm used by typcache.c ensures that if a ">"
	 * operator is returned, it will be consistent with the "=" operator
	 * returned by equality_oper.  This is critical for sorting and grouping
	 * purposes.
	 */
	typentry = lookup_type_cache(argtype, TYPECACHE_GT_OPR);
	oproid = typentry->gt_opr;

	/*
	 * If the datatype is an array, then we can use array_gt ... but only if
	 * there is a suitable greater-than operator for the element type. (This
	 * check is not in the raw typcache.c code ... should it be?)
	 */
	if (oproid == ARRAY_GT_OP)
	{
		Oid			elem_type = get_element_type(argtype);

		if (OidIsValid(elem_type))
		{
			optup = reverse_ordering_oper(elem_type, true);
			if (optup != NULL)
				ReleaseSysCache(optup);
			else
				oproid = InvalidOid;	/* element type has no ">" */
		}
		else
			oproid = InvalidOid;	/* bogus array type? */
	}

	if (OidIsValid(oproid))
	{
		optup = SearchSysCache(OPEROID,
							   ObjectIdGetDatum(oproid),
							   0, 0, 0);
		if (optup == NULL)		/* should not fail */
			elog(ERROR, "cache lookup failed for operator %u", oproid);
		return optup;
	}

	if (!noError)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify an ordering operator for type %s",
						format_type_be(argtype)),
		 errhint("Use an explicit ordering operator or modify the query.")));
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
 * reverse_ordering_oper_opid - convenience routine for oprid(reverse_ordering_oper())
 */
Oid
reverse_ordering_oper_opid(Oid argtype)
{
	Operator	optup;
	Oid			result;

	optup = reverse_ordering_oper(argtype, false);
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
 * the same type as the other operand for this purpose.  Also, consider
 * the possibility that the other operand is a domain type that needs to
 * be reduced to its base type to find an "exact" match.
 */
static Oid
binary_oper_exact(List *opname, Oid arg1, Oid arg2)
{
	Oid			result;
	bool		was_unknown = false;

	/* Unspecified type for one of the arguments? then use the other */
	if ((arg1 == UNKNOWNOID) && (arg2 != InvalidOid))
	{
		arg1 = arg2;
		was_unknown = true;
	}
	else if ((arg2 == UNKNOWNOID) && (arg1 != InvalidOid))
	{
		arg2 = arg1;
		was_unknown = true;
	}

	result = OpernameGetOprid(opname, arg1, arg2);
	if (OidIsValid(result))
		return result;

	if (was_unknown)
	{
		/* arg1 and arg2 are the same here, need only look at arg1 */
		Oid			basetype = getBaseType(arg1);

		if (basetype != arg1)
		{
			result = OpernameGetOprid(opname, basetype, basetype);
			if (OidIsValid(result))
				return result;
		}
	}

	return InvalidOid;
}


/* oper_select_candidate()
 *		Given the input argtype array and one or more candidates
 *		for the operator, attempt to resolve the conflict.
 *
 * Returns FUNCDETAIL_NOTFOUND, FUNCDETAIL_MULTIPLE, or FUNCDETAIL_NORMAL.
 * In the success case the Oid of the best candidate is stored in *operOid.
 *
 * Note that the caller has already determined that there is no candidate
 * exactly matching the input argtype(s).  Incompatible candidates are not yet
 * pruned away, however.
 */
static FuncDetailCode
oper_select_candidate(int nargs,
					  Oid *input_typeids,
					  FuncCandidateList candidates,
					  Oid *operOid)		/* output argument */
{
	int			ncandidates;

	/*
	 * Delete any candidates that cannot actually accept the given input
	 * types, whether directly or by coercion.
	 */
	ncandidates = func_match_argtypes(nargs, input_typeids,
									  candidates, &candidates);

	/* Done if no candidate or only one candidate survives */
	if (ncandidates == 0)
	{
		*operOid = InvalidOid;
		return FUNCDETAIL_NOTFOUND;
	}
	if (ncandidates == 1)
	{
		*operOid = candidates->oid;
		return FUNCDETAIL_NORMAL;
	}

	/*
	 * Use the same heuristics as for ambiguous functions to resolve the
	 * conflict.
	 */
	candidates = func_select_candidate(nargs, input_typeids, candidates);

	if (candidates)
	{
		*operOid = candidates->oid;
		return FUNCDETAIL_NORMAL;
	}

	*operOid = InvalidOid;
	return FUNCDETAIL_MULTIPLE; /* failed to select a best candidate */
}


/* oper() -- search for a binary operator
 * Given operator name, types of arg1 and arg2, return oper struct.
 *
 * IMPORTANT: the returned operator (if any) is only promised to be
 * coercion-compatible with the input datatypes.  Do not use this if
 * you need an exact- or binary-compatible match; see compatible_oper.
 *
 * If no matching operator found, return NULL if noError is true,
 * raise an error if it is false.  pstate and location are used only to report
 * the error position; pass NULL/-1 if not available.
 *
 * NOTE: on success, the returned object is a syscache entry.  The caller
 * must ReleaseSysCache() the entry when done with it.
 */
Operator
oper(ParseState *pstate, List *opname, Oid ltypeId, Oid rtypeId,
	 bool noError, int location)
{
	Oid			operOid;
	FuncDetailCode fdresult = FUNCDETAIL_NOTFOUND;
	HeapTuple	tup = NULL;

	/*
	 * First try for an "exact" match.
	 */
	operOid = binary_oper_exact(opname, ltypeId, rtypeId);
	if (!OidIsValid(operOid))
	{
		/*
		 * Otherwise, search for the most suitable candidate.
		 */
		FuncCandidateList clist;

		/* Get binary operators of given name */
		clist = OpernameGetCandidates(opname, 'b');

		/* No operators found? Then fail... */
		if (clist != NULL)
		{
			/*
			 * Unspecified type for one of the arguments? then use the other
			 * (XXX this is probably dead code?)
			 */
			Oid			inputOids[2];

			if (rtypeId == InvalidOid)
				rtypeId = ltypeId;
			else if (ltypeId == InvalidOid)
				ltypeId = rtypeId;
			inputOids[0] = ltypeId;
			inputOids[1] = rtypeId;
			fdresult = oper_select_candidate(2, inputOids, clist, &operOid);
		}
	}

	if (OidIsValid(operOid))
		tup = SearchSysCache(OPEROID,
							 ObjectIdGetDatum(operOid),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup) && !noError)
		op_error(pstate, opname, 'b', ltypeId, rtypeId, fdresult, location);

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
compatible_oper(ParseState *pstate, List *op, Oid arg1, Oid arg2,
				bool noError, int location)
{
	Operator	optup;
	Form_pg_operator opform;

	/* oper() will find the best available match */
	optup = oper(pstate, op, arg1, arg2, noError, location);
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
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator requires run-time type coercion: %s",
						op_signature_string(op, 'b', arg1, arg2)),
				 parser_errposition(pstate, location)));

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

	optup = compatible_oper(NULL, op, arg1, arg2, noError, -1);
	if (optup != NULL)
	{
		result = oprid(optup);
		ReleaseSysCache(optup);
		return result;
	}
	return InvalidOid;
}


/* right_oper() -- search for a unary right operator (postfix operator)
 * Given operator name and type of arg, return oper struct.
 *
 * IMPORTANT: the returned operator (if any) is only promised to be
 * coercion-compatible with the input datatype.  Do not use this if
 * you need an exact- or binary-compatible match.
 *
 * If no matching operator found, return NULL if noError is true,
 * raise an error if it is false.  pstate and location are used only to report
 * the error position; pass NULL/-1 if not available.
 *
 * NOTE: on success, the returned object is a syscache entry.  The caller
 * must ReleaseSysCache() the entry when done with it.
 */
Operator
right_oper(ParseState *pstate, List *op, Oid arg, bool noError, int location)
{
	Oid			operOid;
	FuncDetailCode fdresult = FUNCDETAIL_NOTFOUND;
	HeapTuple	tup = NULL;

	/*
	 * First try for an "exact" match.
	 */
	operOid = OpernameGetOprid(op, arg, InvalidOid);
	if (!OidIsValid(operOid))
	{
		/*
		 * Otherwise, search for the most suitable candidate.
		 */
		FuncCandidateList clist;

		/* Get postfix operators of given name */
		clist = OpernameGetCandidates(op, 'r');

		/* No operators found? Then fail... */
		if (clist != NULL)
		{
			/*
			 * We must run oper_select_candidate even if only one candidate,
			 * otherwise we may falsely return a non-type-compatible operator.
			 */
			fdresult = oper_select_candidate(1, &arg, clist, &operOid);
		}
	}

	if (OidIsValid(operOid))
		tup = SearchSysCache(OPEROID,
							 ObjectIdGetDatum(operOid),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup) && !noError)
		op_error(pstate, op, 'r', arg, InvalidOid, fdresult, location);

	return (Operator) tup;
}


/* left_oper() -- search for a unary left operator (prefix operator)
 * Given operator name and type of arg, return oper struct.
 *
 * IMPORTANT: the returned operator (if any) is only promised to be
 * coercion-compatible with the input datatype.  Do not use this if
 * you need an exact- or binary-compatible match.
 *
 * If no matching operator found, return NULL if noError is true,
 * raise an error if it is false.  pstate and location are used only to report
 * the error position; pass NULL/-1 if not available.
 *
 * NOTE: on success, the returned object is a syscache entry.  The caller
 * must ReleaseSysCache() the entry when done with it.
 */
Operator
left_oper(ParseState *pstate, List *op, Oid arg, bool noError, int location)
{
	Oid			operOid;
	FuncDetailCode fdresult = FUNCDETAIL_NOTFOUND;
	HeapTuple	tup = NULL;

	/*
	 * First try for an "exact" match.
	 */
	operOid = OpernameGetOprid(op, InvalidOid, arg);
	if (!OidIsValid(operOid))
	{
		/*
		 * Otherwise, search for the most suitable candidate.
		 */
		FuncCandidateList clist;

		/* Get prefix operators of given name */
		clist = OpernameGetCandidates(op, 'l');

		/* No operators found? Then fail... */
		if (clist != NULL)
		{
			/*
			 * The returned list has args in the form (0, oprright). Move the
			 * useful data into args[0] to keep oper_select_candidate simple.
			 * XXX we are assuming here that we may scribble on the list!
			 */
			FuncCandidateList clisti;

			for (clisti = clist; clisti != NULL; clisti = clisti->next)
			{
				clisti->args[0] = clisti->args[1];
			}

			/*
			 * We must run oper_select_candidate even if only one candidate,
			 * otherwise we may falsely return a non-type-compatible operator.
			 */
			fdresult = oper_select_candidate(1, &arg, clist, &operOid);
		}
	}

	if (OidIsValid(operOid))
		tup = SearchSysCache(OPEROID,
							 ObjectIdGetDatum(operOid),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup) && !noError)
		op_error(pstate, op, 'l', InvalidOid, arg, fdresult, location);

	return (Operator) tup;
}

/*
 * op_signature_string
 *		Build a string representing an operator name, including arg type(s).
 *		The result is something like "integer + integer".
 *
 * This is typically used in the construction of operator-not-found error
 * messages.
 */
static const char *
op_signature_string(List *op, char oprkind, Oid arg1, Oid arg2)
{
	StringInfoData argbuf;

	initStringInfo(&argbuf);

	if (oprkind != 'l')
		appendStringInfo(&argbuf, "%s ", format_type_be(arg1));

	appendStringInfoString(&argbuf, NameListToString(op));

	if (oprkind != 'r')
		appendStringInfo(&argbuf, " %s", format_type_be(arg2));

	return argbuf.data;			/* return palloc'd string buffer */
}

/*
 * op_error - utility routine to complain about an unresolvable operator
 */
static void
op_error(ParseState *pstate, List *op, char oprkind,
		 Oid arg1, Oid arg2,
		 FuncDetailCode fdresult, int location)
{
	if (fdresult == FUNCDETAIL_MULTIPLE)
		ereport(ERROR,
				(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
				 errmsg("operator is not unique: %s",
						op_signature_string(op, oprkind, arg1, arg2)),
				 errhint("Could not choose a best candidate operator. "
						 "You may need to add explicit type casts."),
				 parser_errposition(pstate, location)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator does not exist: %s",
						op_signature_string(op, oprkind, arg1, arg2)),
		  errhint("No operator matches the given name and argument type(s). "
				  "You may need to add explicit type casts."),
				 parser_errposition(pstate, location)));
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
make_op(ParseState *pstate, List *opname, Node *ltree, Node *rtree,
		int location)
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
		tup = right_oper(pstate, opname, ltypeId, false, location);
	}
	else if (ltree == NULL)
	{
		/* left operator */
		rtypeId = exprType(rtree);
		ltypeId = InvalidOid;
		tup = left_oper(pstate, opname, rtypeId, false, location);
	}
	else
	{
		/* otherwise, binary operator */
		ltypeId = exprType(ltree);
		rtypeId = exprType(rtree);
		tup = oper(pstate, opname, ltypeId, rtypeId, false, location);
	}

	/* Do typecasting and build the expression tree */
	result = make_op_expr(pstate, tup, ltree, rtree, ltypeId, rtypeId);

	ReleaseSysCache(tup);

	return result;
}

/*
 * make_scalar_array_op()
 *		Build expression tree for "scalar op ANY/ALL (array)" construct.
 */
Expr *
make_scalar_array_op(ParseState *pstate, List *opname,
					 bool useOr,
					 Node *ltree, Node *rtree,
					 int location)
{
	Oid			ltypeId,
				rtypeId,
				atypeId,
				res_atypeId;
	Operator	tup;
	Form_pg_operator opform;
	Oid			actual_arg_types[2];
	Oid			declared_arg_types[2];
	List	   *args;
	Oid			rettype;
	ScalarArrayOpExpr *result;

	ltypeId = exprType(ltree);
	atypeId = exprType(rtree);

	/*
	 * The right-hand input of the operator will be the element type of the
	 * array.  However, if we currently have just an untyped literal on the
	 * right, stay with that and hope we can resolve the operator.
	 */
	if (atypeId == UNKNOWNOID)
		rtypeId = UNKNOWNOID;
	else
	{
		rtypeId = get_element_type(atypeId);
		if (!OidIsValid(rtypeId))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				   errmsg("op ANY/ALL (array) requires array on right side"),
					 parser_errposition(pstate, location)));
	}

	/* Now resolve the operator */
	tup = oper(pstate, opname, ltypeId, rtypeId, false, location);
	opform = (Form_pg_operator) GETSTRUCT(tup);

	args = list_make2(ltree, rtree);
	actual_arg_types[0] = ltypeId;
	actual_arg_types[1] = rtypeId;
	declared_arg_types[0] = opform->oprleft;
	declared_arg_types[1] = opform->oprright;

	/*
	 * enforce consistency with ANYARRAY and ANYELEMENT argument and return
	 * types, possibly adjusting return type or declared_arg_types (which will
	 * be used as the cast destination by make_fn_arguments)
	 */
	rettype = enforce_generic_type_consistency(actual_arg_types,
											   declared_arg_types,
											   2,
											   opform->oprresult);

	/*
	 * Check that operator result is boolean
	 */
	if (rettype != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg("op ANY/ALL (array) requires operator to yield boolean"),
				 parser_errposition(pstate, location)));
	if (get_func_retset(opform->oprcode))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
		  errmsg("op ANY/ALL (array) requires operator not to return a set"),
				 parser_errposition(pstate, location)));

	/*
	 * Now switch back to the array type on the right, arranging for any
	 * needed cast to be applied.
	 */
	res_atypeId = get_array_type(declared_arg_types[1]);
	if (!OidIsValid(res_atypeId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("could not find array type for data type %s",
						format_type_be(declared_arg_types[1])),
				 parser_errposition(pstate, location)));
	actual_arg_types[1] = atypeId;
	declared_arg_types[1] = res_atypeId;

	/* perform the necessary typecasting of arguments */
	make_fn_arguments(pstate, args, actual_arg_types, declared_arg_types);

	/* and build the expression node */
	result = makeNode(ScalarArrayOpExpr);
	result->opno = oprid(tup);
	result->opfuncid = InvalidOid;
	result->useOr = useOr;
	result->args = args;

	ReleaseSysCache(tup);

	return (Expr *) result;
}

/*
 * make_op_expr()
 *		Build operator expression using an already-looked-up operator.
 *
 * As with coerce_type, pstate may be NULL if no special unknown-Param
 * processing is wanted.
 */
static Expr *
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
		args = list_make1(ltree);
		actual_arg_types[0] = ltypeId;
		declared_arg_types[0] = opform->oprleft;
		nargs = 1;
	}
	else if (ltree == NULL)
	{
		/* left operator */
		args = list_make1(rtree);
		actual_arg_types[0] = rtypeId;
		declared_arg_types[0] = opform->oprright;
		nargs = 1;
	}
	else
	{
		/* otherwise, binary operator */
		args = list_make2(ltree, rtree);
		actual_arg_types[0] = ltypeId;
		actual_arg_types[1] = rtypeId;
		declared_arg_types[0] = opform->oprleft;
		declared_arg_types[1] = opform->oprright;
		nargs = 2;
	}

	/*
	 * enforce consistency with ANYARRAY and ANYELEMENT argument and return
	 * types, possibly adjusting return type or declared_arg_types (which will
	 * be used as the cast destination by make_fn_arguments)
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
