/*-------------------------------------------------------------------------
 *
 * parse_coerce.c
 *		handle type coercions/conversions for parser
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_coerce.c,v 2.83 2002/09/04 20:31:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_cast.h"
#include "catalog/pg_proc.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static Oid	PreferredType(CATEGORY category, Oid type);
static bool find_coercion_pathway(Oid targetTypeId, Oid sourceTypeId,
					  bool isExplicit,
					  Oid *funcid);
static Oid	find_typmod_coercion_function(Oid typeId);
static Node *build_func_call(Oid funcid, Oid rettype, List *args);


/*
 * coerce_type()
 *		Convert a function argument to a different type.
 *
 * The caller should already have determined that the coercion is possible;
 * see can_coerce_type.
 */
Node *
coerce_type(ParseState *pstate, Node *node, Oid inputTypeId,
			Oid targetTypeId, int32 atttypmod, bool isExplicit)
{
	Node	   *result;
	Oid			funcId;

	if (targetTypeId == inputTypeId ||
		node == NULL)
	{
		/* no conversion needed */
		result = node;
	}
	else if (inputTypeId == UNKNOWNOID && IsA(node, Const))
	{
		/*
		 * Input is a string constant with previously undetermined type.
		 * Apply the target type's typinput function to it to produce a
		 * constant of the target type.
		 *
		 * NOTE: this case cannot be folded together with the other
		 * constant-input case, since the typinput function does not
		 * necessarily behave the same as a type conversion function. For
		 * example, int4's typinput function will reject "1.2", whereas
		 * float-to-int type conversion will round to integer.
		 *
		 * XXX if the typinput function is not cachable, we really ought to
		 * postpone evaluation of the function call until runtime. But
		 * there is no way to represent a typinput function call as an
		 * expression tree, because C-string values are not Datums. (XXX
		 * This *is* possible as of 7.3, do we want to do it?)
		 */
		Const	   *con = (Const *) node;
		Const	   *newcon = makeNode(Const);
		Type		targetType = typeidType(targetTypeId);
		char		targetTyptype = typeTypType(targetType);

		newcon->consttype = targetTypeId;
		newcon->constlen = typeLen(targetType);
		newcon->constbyval = typeByVal(targetType);
		newcon->constisnull = con->constisnull;
		newcon->constisset = false;

		if (!con->constisnull)
		{
			char	   *val = DatumGetCString(DirectFunctionCall1(unknownout,
													   con->constvalue));

			/*
			 * If target is a domain, use the typmod it applies to the
			 * base type.  Note that we call stringTypeDatum using the
			 * domain's pg_type row, though.  This works because the
			 * domain row has the same typinput and typelem as the base
			 * type --- ugly...
			 */
			if (targetTyptype == 'd')
				atttypmod = getBaseTypeMod(targetTypeId, atttypmod);

			newcon->constvalue = stringTypeDatum(targetType, val, atttypmod);
			pfree(val);
		}

		result = (Node *) newcon;

		/*
		 * If target is a domain, apply constraints (except for typmod,
		 * which we assume the input routine took care of).
		 */
		if (targetTyptype == 'd')
			result = coerce_type_constraints(pstate, result, targetTypeId,
											 false);

		ReleaseSysCache(targetType);
	}
	else if (targetTypeId == ANYOID ||
			 targetTypeId == ANYARRAYOID)
	{
		/* assume can_coerce_type verified that implicit coercion is okay */
		result = node;
	}
	else if (find_coercion_pathway(targetTypeId, inputTypeId, isExplicit,
								   &funcId))
	{
		if (OidIsValid(funcId))
		{
			/*
			 * Generate an expression tree representing run-time
			 * application of the conversion function.	If we are dealing
			 * with a domain target type, the conversion function will
			 * yield the base type.
			 */
			Oid			baseTypeId = getBaseType(targetTypeId);

			result = build_func_call(funcId, baseTypeId, makeList1(node));

			/*
			 * If domain, test against domain constraints and relabel with
			 * domain type ID
			 */
			if (targetTypeId != baseTypeId)
			{
				result = coerce_type_constraints(pstate, result,
												 targetTypeId, true);
				result = (Node *) makeRelabelType(result, targetTypeId, -1);
			}

			/*
			 * If the input is a constant, apply the type conversion
			 * function now instead of delaying to runtime.  (We could, of
			 * course, just leave this to be done during
			 * planning/optimization; but it's a very frequent special
			 * case, and we save cycles in the rewriter if we fold the
			 * expression now.)
			 *
			 * Note that no folding will occur if the conversion function is
			 * not marked 'immutable'.
			 *
			 * HACK: if constant is NULL, don't fold it here.  This is needed
			 * by make_subplan(), which calls this routine on placeholder
			 * Const nodes that mustn't be collapsed.  (It'd be a lot
			 * cleaner to make a separate node type for that purpose...)
			 */
			if (IsA(node, Const) &&
				!((Const *) node)->constisnull)
				result = eval_const_expressions(result);
		}
		else
		{
			/*
			 * We don't need to do a physical conversion, but we do need
			 * to attach a RelabelType node so that the expression will be
			 * seen to have the intended type when inspected by
			 * higher-level code.
			 *
			 * Also, domains may have value restrictions beyond the base type
			 * that must be accounted for.
			 */
			result = coerce_type_constraints(pstate, node,
											 targetTypeId, true);

			/*
			 * XXX could we label result with exprTypmod(node) instead of
			 * default -1 typmod, to save a possible length-coercion
			 * later? Would work if both types have same interpretation of
			 * typmod, which is likely but not certain (wrong if target is
			 * a domain, in any case).
			 */
			result = (Node *) makeRelabelType(result, targetTypeId, -1);
		}
	}
	else if (typeInheritsFrom(inputTypeId, targetTypeId))
	{
		/*
		 * Input class type is a subclass of target, so nothing to do ---
		 * except relabel the type.  This is binary compatibility for
		 * complex types.
		 */
		result = (Node *) makeRelabelType(node, targetTypeId, -1);
	}
	else
	{
		/* If we get here, caller blew it */
		elog(ERROR, "coerce_type: no conversion function from %s to %s",
			 format_type_be(inputTypeId), format_type_be(targetTypeId));
		result = NULL;			/* keep compiler quiet */
	}

	return result;
}


/*
 * can_coerce_type()
 *		Can input_typeids be coerced to func_typeids?
 *
 * We must be told whether this is an implicit or explicit coercion
 * (explicit being a CAST construct, explicit function call, etc).
 * We will accept a wider set of coercion cases for an explicit coercion.
 */
bool
can_coerce_type(int nargs, Oid *input_typeids, Oid *func_typeids,
				bool isExplicit)
{
	int			i;

	/* run through argument list... */
	for (i = 0; i < nargs; i++)
	{
		Oid			inputTypeId = input_typeids[i];
		Oid			targetTypeId = func_typeids[i];
		Oid			funcId;

		/* no problem if same type */
		if (inputTypeId == targetTypeId)
			continue;

		/* don't choke on references to no-longer-existing types */
		if (!typeidIsValid(inputTypeId))
			return false;
		if (!typeidIsValid(targetTypeId))
			return false;

		/*
		 * If input is an untyped string constant, assume we can convert
		 * it to anything except a class type.
		 */
		if (inputTypeId == UNKNOWNOID)
		{
			if (ISCOMPLEX(targetTypeId))
				return false;
			continue;
		}

		/* accept if target is ANY */
		if (targetTypeId == ANYOID)
			continue;

		/*
		 * if target is ANYARRAY and source is a varlena array type,
		 * accept
		 */
		if (targetTypeId == ANYARRAYOID)
		{
			Oid			typOutput;
			Oid			typElem;
			bool		typIsVarlena;

			if (getTypeOutputInfo(inputTypeId, &typOutput, &typElem,
								  &typIsVarlena))
			{
				if (OidIsValid(typElem) && typIsVarlena)
					continue;
			}

			/*
			 * Otherwise reject; this assumes there are no explicit
			 * coercions to ANYARRAY.  If we don't reject then
			 * parse_coerce would have to repeat the above test.
			 */
			return false;
		}

		/*
		 * If pg_cast shows that we can coerce, accept.  This test now
		 * covers both binary-compatible and coercion-function cases.
		 */
		if (find_coercion_pathway(targetTypeId, inputTypeId, isExplicit,
								  &funcId))
			continue;

		/*
		 * If input is a class type that inherits from target, accept
		 */
		if (typeInheritsFrom(inputTypeId, targetTypeId))
			continue;

		/*
		 * Else, cannot coerce at this argument position
		 */
		return false;
	}

	return true;
}


/*
 * Create an expression tree to enforce the constraints (if any)
 * that should be applied by the type.	Currently this is only
 * interesting for domain types.
 */
Node *
coerce_type_constraints(ParseState *pstate, Node *arg,
						Oid typeId, bool applyTypmod)
{
	char	   *notNull = NULL;
	int32		typmod = -1;

	for (;;)
	{
		HeapTuple	tup;
		Form_pg_type typTup;

		tup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(typeId),
							 0, 0, 0);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "coerce_type_constraints: failed to lookup type %u",
				 typeId);
		typTup = (Form_pg_type) GETSTRUCT(tup);

		/* Test for NOT NULL Constraint */
		if (typTup->typnotnull && notNull == NULL)
			notNull = pstrdup(NameStr(typTup->typname));

		/* TODO: Add CHECK Constraints to domains */

		if (typTup->typtype != 'd')
		{
			/* Not a domain, so done */
			ReleaseSysCache(tup);
			break;
		}

		Assert(typmod < 0);

		typeId = typTup->typbasetype;
		typmod = typTup->typtypmod;
		ReleaseSysCache(tup);
	}

	/*
	 * If domain applies a typmod to its base type, do length coercion.
	 */
	if (applyTypmod && typmod >= 0)
		arg = coerce_type_typmod(pstate, arg, typeId, typmod);

	/*
	 * Only need to add one NOT NULL check regardless of how many domains
	 * in the stack request it.  The topmost domain that requested it is
	 * used as the constraint name.
	 */
	if (notNull)
	{
		ConstraintTest *r = makeNode(ConstraintTest);

		r->arg = arg;
		r->testtype = CONSTR_TEST_NOTNULL;
		r->name = notNull;
		r->check_expr = NULL;

		arg = (Node *) r;
	}

	return arg;
}


/* coerce_type_typmod()
 * Force a value to a particular typmod, if meaningful and possible.
 *
 * This is applied to values that are going to be stored in a relation
 * (where we have an atttypmod for the column) as well as values being
 * explicitly CASTed (where the typmod comes from the target type spec).
 *
 * The caller must have already ensured that the value is of the correct
 * type, typically by applying coerce_type.
 *
 * NOTE: this does not need to work on domain types, because any typmod
 * coercion for a domain is considered to be part of the type coercion
 * needed to produce the domain value in the first place.  So, no getBaseType.
 */
Node *
coerce_type_typmod(ParseState *pstate, Node *node,
				   Oid targetTypeId, int32 atttypmod)
{
	Oid			funcId;

	/*
	 * A negative typmod is assumed to mean that no coercion is wanted.
	 */
	if (atttypmod < 0 || atttypmod == exprTypmod(node))
		return node;

	funcId = find_typmod_coercion_function(targetTypeId);

	if (OidIsValid(funcId))
	{
		Const	   *cons;

		cons = makeConst(INT4OID,
						 sizeof(int32),
						 Int32GetDatum(atttypmod),
						 false,
						 true,
						 false,
						 false);

		node = build_func_call(funcId, targetTypeId, makeList2(node, cons));
	}

	return node;
}


/* coerce_to_boolean()
 *		Coerce an argument of a construct that requires boolean input
 *		(AND, OR, NOT, etc).  Also check that input is not a set.
 *
 * Returns the possibly-transformed node tree.
 */
Node *
coerce_to_boolean(Node *node, const char *constructName)
{
	Oid			inputTypeId = exprType(node);
	Oid			targetTypeId;

	if (inputTypeId != BOOLOID)
	{
		targetTypeId = BOOLOID;
		if (!can_coerce_type(1, &inputTypeId, &targetTypeId, false))
		{
			/* translator: first %s is name of a SQL construct, eg WHERE */
			elog(ERROR, "Argument of %s must be type boolean, not type %s",
				 constructName, format_type_be(inputTypeId));
		}
		node = coerce_type(NULL, node, inputTypeId, targetTypeId, -1,
						   false);
	}

	if (expression_returns_set(node))
	{
		/* translator: %s is name of a SQL construct, eg WHERE */
		elog(ERROR, "Argument of %s must not be a set function",
			 constructName);
	}

	return node;
}


/* select_common_type()
 *		Determine the common supertype of a list of input expression types.
 *		This is used for determining the output type of CASE and UNION
 *		constructs.
 *
 * typeids is a nonempty integer list of type OIDs.  Note that earlier items
 * in the list will be preferred if there is doubt.
 * 'context' is a phrase to use in the error message if we fail to select
 * a usable type.
 *
 * XXX this code is WRONG, since (for example) given the input (int4,int8)
 * it will select int4, whereas according to SQL92 clause 9.3 the correct
 * answer is clearly int8.	To fix this we need a notion of a promotion
 * hierarchy within type categories --- something more complete than
 * just a single preferred type.
 */
Oid
select_common_type(List *typeids, const char *context)
{
	Oid			ptype;
	CATEGORY	pcategory;
	List	   *l;

	Assert(typeids != NIL);
	ptype = (Oid) lfirsti(typeids);
	pcategory = TypeCategory(ptype);
	foreach(l, lnext(typeids))
	{
		Oid			ntype = (Oid) lfirsti(l);

		/* move on to next one if no new information... */
		if ((ntype != InvalidOid) && (ntype != UNKNOWNOID) && (ntype != ptype))
		{
			if ((ptype == InvalidOid) || ptype == UNKNOWNOID)
			{
				/* so far, only nulls so take anything... */
				ptype = ntype;
				pcategory = TypeCategory(ptype);
			}
			else if (TypeCategory(ntype) != pcategory)
			{
				/*
				 * both types in different categories? then not much
				 * hope...
				 */
				elog(ERROR, "%s types '%s' and '%s' not matched",
				  context, format_type_be(ptype), format_type_be(ntype));
			}
			else if (IsPreferredType(pcategory, ntype)
					 && !IsPreferredType(pcategory, ptype)
					 && can_coerce_type(1, &ptype, &ntype, false))
			{
				/*
				 * new one is preferred and can convert? then take it...
				 */
				ptype = ntype;
				pcategory = TypeCategory(ptype);
			}
		}
	}

	/*
	 * If all the inputs were UNKNOWN type --- ie, unknown-type literals
	 * --- then resolve as type TEXT.  This situation comes up with
	 * constructs like SELECT (CASE WHEN foo THEN 'bar' ELSE 'baz' END);
	 * SELECT 'foo' UNION SELECT 'bar'; It might seem desirable to leave
	 * the construct's output type as UNKNOWN, but that really doesn't
	 * work, because we'd probably end up needing a runtime coercion from
	 * UNKNOWN to something else, and we usually won't have it.  We need
	 * to coerce the unknown literals while they are still literals, so a
	 * decision has to be made now.
	 */
	if (ptype == UNKNOWNOID)
		ptype = TEXTOID;

	return ptype;
}

/* coerce_to_common_type()
 *		Coerce an expression to the given type.
 *
 * This is used following select_common_type() to coerce the individual
 * expressions to the desired type.  'context' is a phrase to use in the
 * error message if we fail to coerce.
 *
 * NOTE: pstate may be NULL.
 */
Node *
coerce_to_common_type(ParseState *pstate, Node *node,
					  Oid targetTypeId,
					  const char *context)
{
	Oid			inputTypeId = exprType(node);

	if (inputTypeId == targetTypeId)
		return node;			/* no work */
	if (can_coerce_type(1, &inputTypeId, &targetTypeId, false))
		node = coerce_type(pstate, node, inputTypeId, targetTypeId, -1,
						   false);
	else
	{
		elog(ERROR, "%s unable to convert to type %s",
			 context, format_type_be(targetTypeId));
	}
	return node;
}


/* TypeCategory()
 * Assign a category to the specified OID.
 * XXX This should be moved to system catalog lookups
 * to allow for better type extensibility.
 * - thomas 2001-09-30
 */
CATEGORY
TypeCategory(Oid inType)
{
	CATEGORY	result;

	switch (inType)
	{
		case (BOOLOID):
			result = BOOLEAN_TYPE;
			break;

		case (CHAROID):
		case (NAMEOID):
		case (BPCHAROID):
		case (VARCHAROID):
		case (TEXTOID):
			result = STRING_TYPE;
			break;

		case (BITOID):
		case (VARBITOID):
			result = BITSTRING_TYPE;
			break;

		case (OIDOID):
		case (REGPROCOID):
		case (REGPROCEDUREOID):
		case (REGOPEROID):
		case (REGOPERATOROID):
		case (REGCLASSOID):
		case (REGTYPEOID):
		case (INT2OID):
		case (INT4OID):
		case (INT8OID):
		case (FLOAT4OID):
		case (FLOAT8OID):
		case (NUMERICOID):
		case (CASHOID):
			result = NUMERIC_TYPE;
			break;

		case (DATEOID):
		case (TIMEOID):
		case (TIMETZOID):
		case (ABSTIMEOID):
		case (TIMESTAMPOID):
		case (TIMESTAMPTZOID):
			result = DATETIME_TYPE;
			break;

		case (RELTIMEOID):
		case (TINTERVALOID):
		case (INTERVALOID):
			result = TIMESPAN_TYPE;
			break;

		case (POINTOID):
		case (LSEGOID):
		case (PATHOID):
		case (BOXOID):
		case (POLYGONOID):
		case (LINEOID):
		case (CIRCLEOID):
			result = GEOMETRIC_TYPE;
			break;

		case (INETOID):
		case (CIDROID):
			result = NETWORK_TYPE;
			break;

		case (UNKNOWNOID):
		case (InvalidOid):
			result = UNKNOWN_TYPE;
			break;

		default:
			result = USER_TYPE;
			break;
	}
	return result;
}	/* TypeCategory() */


/* IsPreferredType()
 * Check if this type is a preferred type.
 * XXX This should be moved to system catalog lookups
 * to allow for better type extensibility.
 * - thomas 2001-09-30
 */
bool
IsPreferredType(CATEGORY category, Oid type)
{
	return (type == PreferredType(category, type));
}	/* IsPreferredType() */


/* PreferredType()
 * Return the preferred type OID for the specified category.
 * XXX This should be moved to system catalog lookups
 * to allow for better type extensibility.
 * - thomas 2001-09-30
 */
static Oid
PreferredType(CATEGORY category, Oid type)
{
	Oid			result;

	switch (category)
	{
		case (BOOLEAN_TYPE):
			result = BOOLOID;
			break;

		case (STRING_TYPE):
			result = TEXTOID;
			break;

		case (BITSTRING_TYPE):
			result = VARBITOID;
			break;

		case (NUMERIC_TYPE):
			if (type == OIDOID ||
				type == REGPROCOID ||
				type == REGPROCEDUREOID ||
				type == REGOPEROID ||
				type == REGOPERATOROID ||
				type == REGCLASSOID ||
				type == REGTYPEOID)
				result = OIDOID;
			else if (type == NUMERICOID)
				result = NUMERICOID;
			else
				result = FLOAT8OID;
			break;

		case (DATETIME_TYPE):
			if (type == DATEOID)
				result = TIMESTAMPOID;
			else
				result = TIMESTAMPTZOID;
			break;

		case (TIMESPAN_TYPE):
			result = INTERVALOID;
			break;

		case (NETWORK_TYPE):
			result = INETOID;
			break;

		case (GEOMETRIC_TYPE):
		case (USER_TYPE):
			result = type;
			break;

		default:
			result = UNKNOWNOID;
			break;
	}
	return result;
}	/* PreferredType() */


/* IsBinaryCompatible()
 *		Check if two types are binary-compatible.
 *
 * This notion allows us to cheat and directly exchange values without
 * going through the trouble of calling a conversion function.
 *
 * As of 7.3, binary compatibility isn't hardwired into the code anymore.
 * We consider two types binary-compatible if there is an implicit,
 * no-function-needed pg_cast entry.  NOTE that we assume that such
 * entries are symmetric, ie, it doesn't matter which type we consider
 * source and which target.  (cf. checks in opr_sanity regression test)
 */
bool
IsBinaryCompatible(Oid type1, Oid type2)
{
	HeapTuple	tuple;
	Form_pg_cast castForm;
	bool		result;

	/* Fast path if same type */
	if (type1 == type2)
		return true;

	/* Perhaps the types are domains; if so, look at their base types */
	if (OidIsValid(type1))
		type1 = getBaseType(type1);
	if (OidIsValid(type2))
		type2 = getBaseType(type2);

	/* Somewhat-fast path if same base type */
	if (type1 == type2)
		return true;

	/* Else look in pg_cast */
	tuple = SearchSysCache(CASTSOURCETARGET,
						   ObjectIdGetDatum(type1),
						   ObjectIdGetDatum(type2),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
		return false;			/* no cast */
	castForm = (Form_pg_cast) GETSTRUCT(tuple);

	result = (castForm->castfunc == InvalidOid) && castForm->castimplicit;

	ReleaseSysCache(tuple);

	return result;
}


/*
 * find_coercion_pathway
 *		Look for a coercion pathway between two types.
 *
 * If we find a matching entry in pg_cast, return TRUE, and set *funcid
 * to the castfunc value (which may be InvalidOid for a binary-compatible
 * coercion).
 */
static bool
find_coercion_pathway(Oid targetTypeId, Oid sourceTypeId, bool isExplicit,
					  Oid *funcid)
{
	bool		result = false;
	HeapTuple	tuple;

	*funcid = InvalidOid;

	/* Perhaps the types are domains; if so, look at their base types */
	if (OidIsValid(sourceTypeId))
		sourceTypeId = getBaseType(sourceTypeId);
	if (OidIsValid(targetTypeId))
		targetTypeId = getBaseType(targetTypeId);

	/* Domains are automatically binary-compatible with their base type */
	if (sourceTypeId == targetTypeId)
		return true;

	/* Else look in pg_cast */
	tuple = SearchSysCache(CASTSOURCETARGET,
						   ObjectIdGetDatum(sourceTypeId),
						   ObjectIdGetDatum(targetTypeId),
						   0, 0);

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_cast castForm = (Form_pg_cast) GETSTRUCT(tuple);

		if (isExplicit || castForm->castimplicit)
		{
			*funcid = castForm->castfunc;
			result = true;
		}

		ReleaseSysCache(tuple);
	}

	return result;
}


/*
 * find_typmod_coercion_function -- does the given type need length coercion?
 *
 * If the target type possesses a function named for the type
 * and having parameter signature (targettype, int4), we assume that
 * the type requires coercion to its own length and that the said
 * function should be invoked to do that.
 *
 * "bpchar" (ie, char(N)) and "numeric" are examples of such types.
 *
 * This mechanism may seem pretty grotty and in need of replacement by
 * something in pg_cast, but since typmod is only interesting for datatypes
 * that have special handling in the grammar, there's not really much
 * percentage in making it any easier to apply such coercions ...
 */
static Oid
find_typmod_coercion_function(Oid typeId)
{
	Oid			funcid = InvalidOid;
	Type		targetType;
	char	   *typname;
	Oid			typnamespace;
	Oid			oid_array[FUNC_MAX_ARGS];
	HeapTuple	ftup;

	targetType = typeidType(typeId);
	typname = NameStr(((Form_pg_type) GETSTRUCT(targetType))->typname);
	typnamespace = ((Form_pg_type) GETSTRUCT(targetType))->typnamespace;

	MemSet(oid_array, 0, FUNC_MAX_ARGS * sizeof(Oid));
	oid_array[0] = typeId;
	oid_array[1] = INT4OID;

	ftup = SearchSysCache(PROCNAMENSP,
						  CStringGetDatum(typname),
						  Int16GetDatum(2),
						  PointerGetDatum(oid_array),
						  ObjectIdGetDatum(typnamespace));
	if (HeapTupleIsValid(ftup))
	{
		Form_pg_proc pform = (Form_pg_proc) GETSTRUCT(ftup);

		/* Make sure the function's result type is as expected */
		if (pform->prorettype == typeId && !pform->proretset &&
			!pform->proisagg)
		{
			/* Okay to use it */
			funcid = HeapTupleGetOid(ftup);
		}
		ReleaseSysCache(ftup);
	}

	ReleaseSysCache(targetType);

	return funcid;
}

/*
 * Build an expression tree representing a function call.
 *
 * The argument expressions must have been transformed already.
 */
static Node *
build_func_call(Oid funcid, Oid rettype, List *args)
{
	Func	   *funcnode;
	Expr	   *expr;

	funcnode = makeNode(Func);
	funcnode->funcid = funcid;
	funcnode->funcresulttype = rettype;
	funcnode->funcretset = false;		/* only possible case here */
	funcnode->func_fcache = NULL;

	expr = makeNode(Expr);
	expr->typeOid = rettype;
	expr->opType = FUNC_EXPR;
	expr->oper = (Node *) funcnode;
	expr->args = args;

	return (Node *) expr;
}
