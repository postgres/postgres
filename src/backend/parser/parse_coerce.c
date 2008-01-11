/*-------------------------------------------------------------------------
 *
 * parse_coerce.c
 *		handle type coercions/conversions for parser
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/parse_coerce.c,v 2.161 2008/01/11 18:39:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_cast.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static Node *coerce_type_typmod(Node *node,
				   Oid targetTypeId, int32 targetTypMod,
				   CoercionForm cformat, bool isExplicit,
				   bool hideInputCoercion);
static void hide_coercion_node(Node *node);
static Node *build_coercion_expression(Node *node,
						  CoercionPathType pathtype,
						  Oid funcId,
						  Oid targetTypeId, int32 targetTypMod,
						  CoercionForm cformat, bool isExplicit);
static Node *coerce_record_to_complex(ParseState *pstate, Node *node,
						 Oid targetTypeId,
						 CoercionContext ccontext,
						 CoercionForm cformat);


/*
 * coerce_to_target_type()
 *		Convert an expression to a target type and typmod.
 *
 * This is the general-purpose entry point for arbitrary type coercion
 * operations.	Direct use of the component operations can_coerce_type,
 * coerce_type, and coerce_type_typmod should be restricted to special
 * cases (eg, when the conversion is expected to succeed).
 *
 * Returns the possibly-transformed expression tree, or NULL if the type
 * conversion is not possible.	(We do this, rather than ereport'ing directly,
 * so that callers can generate custom error messages indicating context.)
 *
 * pstate - parse state (can be NULL, see coerce_type)
 * expr - input expression tree (already transformed by transformExpr)
 * exprtype - result type of expr
 * targettype - desired result type
 * targettypmod - desired result typmod
 * ccontext, cformat - context indicators to control coercions
 */
Node *
coerce_to_target_type(ParseState *pstate, Node *expr, Oid exprtype,
					  Oid targettype, int32 targettypmod,
					  CoercionContext ccontext,
					  CoercionForm cformat)
{
	Node	   *result;

	if (!can_coerce_type(1, &exprtype, &targettype, ccontext))
		return NULL;

	result = coerce_type(pstate, expr, exprtype,
						 targettype, targettypmod,
						 ccontext, cformat);

	/*
	 * If the target is a fixed-length type, it may need a length coercion as
	 * well as a type coercion.  If we find ourselves adding both, force the
	 * inner coercion node to implicit display form.
	 */
	result = coerce_type_typmod(result,
								targettype, targettypmod,
								cformat,
								(cformat != COERCE_IMPLICIT_CAST),
								(result != expr && !IsA(result, Const)));

	return result;
}


/*
 * coerce_type()
 *		Convert an expression to a different type.
 *
 * The caller should already have determined that the coercion is possible;
 * see can_coerce_type.
 *
 * Normally, no coercion to a typmod (length) is performed here.  The caller
 * must call coerce_type_typmod as well, if a typmod constraint is wanted.
 * (But if the target type is a domain, it may internally contain a
 * typmod constraint, which will be applied inside coerce_to_domain.)
 * In some cases pg_cast specifies a type coercion function that also
 * applies length conversion, and in those cases only, the result will
 * already be properly coerced to the specified typmod.
 *
 * pstate is only used in the case that we are able to resolve the type of
 * a previously UNKNOWN Param.	It is okay to pass pstate = NULL if the
 * caller does not want type information updated for Params.
 */
Node *
coerce_type(ParseState *pstate, Node *node,
			Oid inputTypeId, Oid targetTypeId, int32 targetTypeMod,
			CoercionContext ccontext, CoercionForm cformat)
{
	Node	   *result;
	CoercionPathType pathtype;
	Oid			funcId;

	if (targetTypeId == inputTypeId ||
		node == NULL)
	{
		/* no conversion needed */
		return node;
	}
	if (targetTypeId == ANYOID ||
		targetTypeId == ANYELEMENTOID ||
		targetTypeId == ANYNONARRAYOID ||
		(targetTypeId == ANYARRAYOID && inputTypeId != UNKNOWNOID) ||
		(targetTypeId == ANYENUMOID && inputTypeId != UNKNOWNOID))
	{
		/*
		 * Assume can_coerce_type verified that implicit coercion is okay.
		 *
		 * Note: by returning the unmodified node here, we are saying that
		 * it's OK to treat an UNKNOWN constant as a valid input for a
		 * function accepting ANY, ANYELEMENT, or ANYNONARRAY.	This should be
		 * all right, since an UNKNOWN value is still a perfectly valid Datum.
		 * However an UNKNOWN value is definitely *not* an array, and so we
		 * mustn't accept it for ANYARRAY.  (Instead, we will call anyarray_in
		 * below, which will produce an error.)  Likewise, UNKNOWN input is no
		 * good for ANYENUM.
		 *
		 * NB: we do NOT want a RelabelType here.
		 */
		return node;
	}
	if (inputTypeId == UNKNOWNOID && IsA(node, Const))
	{
		/*
		 * Input is a string constant with previously undetermined type. Apply
		 * the target type's typinput function to it to produce a constant of
		 * the target type.
		 *
		 * NOTE: this case cannot be folded together with the other
		 * constant-input case, since the typinput function does not
		 * necessarily behave the same as a type conversion function. For
		 * example, int4's typinput function will reject "1.2", whereas
		 * float-to-int type conversion will round to integer.
		 *
		 * XXX if the typinput function is not immutable, we really ought to
		 * postpone evaluation of the function call until runtime. But there
		 * is no way to represent a typinput function call as an expression
		 * tree, because C-string values are not Datums. (XXX This *is*
		 * possible as of 7.3, do we want to do it?)
		 */
		Const	   *con = (Const *) node;
		Const	   *newcon = makeNode(Const);
		Oid			baseTypeId;
		int32		baseTypeMod;
		Type		targetType;

		/*
		 * If the target type is a domain, we want to call its base type's
		 * input routine, not domain_in().	This is to avoid premature failure
		 * when the domain applies a typmod: existing input routines follow
		 * implicit-coercion semantics for length checks, which is not always
		 * what we want here.  The needed check will be applied properly
		 * inside coerce_to_domain().
		 */
		baseTypeMod = -1;
		baseTypeId = getBaseTypeAndTypmod(targetTypeId, &baseTypeMod);

		targetType = typeidType(baseTypeId);

		newcon->consttype = baseTypeId;
		newcon->consttypmod = -1;
		newcon->constlen = typeLen(targetType);
		newcon->constbyval = typeByVal(targetType);
		newcon->constisnull = con->constisnull;

		/*
		 * We pass typmod -1 to the input routine, primarily because existing
		 * input routines follow implicit-coercion semantics for length
		 * checks, which is not always what we want here. Any length
		 * constraint will be applied later by our caller.
		 *
		 * We assume here that UNKNOWN's internal representation is the same
		 * as CSTRING.
		 */
		if (!con->constisnull)
			newcon->constvalue = stringTypeDatum(targetType,
											DatumGetCString(con->constvalue),
												 -1);
		else
			newcon->constvalue = stringTypeDatum(targetType, NULL, -1);

		result = (Node *) newcon;

		/* If target is a domain, apply constraints. */
		if (baseTypeId != targetTypeId)
			result = coerce_to_domain(result,
									  baseTypeId, baseTypeMod,
									  targetTypeId,
									  cformat, false, false);

		ReleaseSysCache(targetType);

		return result;
	}
	if (inputTypeId == UNKNOWNOID && IsA(node, Param) &&
		((Param *) node)->paramkind == PARAM_EXTERN &&
		pstate != NULL && pstate->p_variableparams)
	{
		/*
		 * Input is a Param of previously undetermined type, and we want to
		 * update our knowledge of the Param's type.  Find the topmost
		 * ParseState and update the state.
		 */
		Param	   *param = (Param *) node;
		int			paramno = param->paramid;
		ParseState *toppstate;

		toppstate = pstate;
		while (toppstate->parentParseState != NULL)
			toppstate = toppstate->parentParseState;

		if (paramno <= 0 ||		/* shouldn't happen, but... */
			paramno > toppstate->p_numparams)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PARAMETER),
					 errmsg("there is no parameter $%d", paramno)));

		if (toppstate->p_paramtypes[paramno - 1] == UNKNOWNOID)
		{
			/* We've successfully resolved the type */
			toppstate->p_paramtypes[paramno - 1] = targetTypeId;
		}
		else if (toppstate->p_paramtypes[paramno - 1] == targetTypeId)
		{
			/* We previously resolved the type, and it matches */
		}
		else
		{
			/* Ooops */
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_PARAMETER),
					 errmsg("inconsistent types deduced for parameter $%d",
							paramno),
					 errdetail("%s versus %s",
						format_type_be(toppstate->p_paramtypes[paramno - 1]),
							   format_type_be(targetTypeId))));
		}

		param->paramtype = targetTypeId;

		/*
		 * Note: it is tempting here to set the Param's paramtypmod to
		 * targetTypeMod, but that is probably unwise because we have no
		 * infrastructure that enforces that the value delivered for a Param
		 * will match any particular typmod.  Leaving it -1 ensures that a
		 * run-time length check/coercion will occur if needed.
		 */
		param->paramtypmod = -1;

		return (Node *) param;
	}
	pathtype = find_coercion_pathway(targetTypeId, inputTypeId, ccontext,
									 &funcId);
	if (pathtype != COERCION_PATH_NONE)
	{
		if (pathtype != COERCION_PATH_RELABELTYPE)
		{
			/*
			 * Generate an expression tree representing run-time application
			 * of the conversion function.	If we are dealing with a domain
			 * target type, the conversion function will yield the base type,
			 * and we need to extract the correct typmod to use from the
			 * domain's typtypmod.
			 */
			Oid			baseTypeId;
			int32		baseTypeMod;

			baseTypeMod = targetTypeMod;
			baseTypeId = getBaseTypeAndTypmod(targetTypeId, &baseTypeMod);

			result = build_coercion_expression(node, pathtype, funcId,
											   baseTypeId, baseTypeMod,
											   cformat,
										  (cformat != COERCE_IMPLICIT_CAST));

			/*
			 * If domain, coerce to the domain type and relabel with domain
			 * type ID.  We can skip the internal length-coercion step if the
			 * selected coercion function was a type-and-length coercion.
			 */
			if (targetTypeId != baseTypeId)
				result = coerce_to_domain(result, baseTypeId, baseTypeMod,
										  targetTypeId,
										  cformat, true,
										  exprIsLengthCoercion(result,
															   NULL));
		}
		else
		{
			/*
			 * We don't need to do a physical conversion, but we do need to
			 * attach a RelabelType node so that the expression will be seen
			 * to have the intended type when inspected by higher-level code.
			 *
			 * Also, domains may have value restrictions beyond the base type
			 * that must be accounted for.	If the destination is a domain
			 * then we won't need a RelabelType node.
			 */
			result = coerce_to_domain(node, InvalidOid, -1, targetTypeId,
									  cformat, false, false);
			if (result == node)
			{
				/*
				 * XXX could we label result with exprTypmod(node) instead of
				 * default -1 typmod, to save a possible length-coercion
				 * later? Would work if both types have same interpretation of
				 * typmod, which is likely but not certain.
				 */
				result = (Node *) makeRelabelType((Expr *) result,
												  targetTypeId, -1,
												  cformat);
			}
		}
		return result;
	}
	if (inputTypeId == RECORDOID &&
		ISCOMPLEX(targetTypeId))
	{
		/* Coerce a RECORD to a specific complex type */
		return coerce_record_to_complex(pstate, node, targetTypeId,
										ccontext, cformat);
	}
	if (targetTypeId == RECORDOID &&
		ISCOMPLEX(inputTypeId))
	{
		/* Coerce a specific complex type to RECORD */
		/* NB: we do NOT want a RelabelType here */
		return node;
	}
	if (typeInheritsFrom(inputTypeId, targetTypeId))
	{
		/*
		 * Input class type is a subclass of target, so generate an
		 * appropriate runtime conversion (removing unneeded columns and
		 * possibly rearranging the ones that are wanted).
		 */
		ConvertRowtypeExpr *r = makeNode(ConvertRowtypeExpr);

		r->arg = (Expr *) node;
		r->resulttype = targetTypeId;
		r->convertformat = cformat;
		return (Node *) r;
	}
	/* If we get here, caller blew it */
	elog(ERROR, "failed to find conversion function from %s to %s",
		 format_type_be(inputTypeId), format_type_be(targetTypeId));
	return NULL;				/* keep compiler quiet */
}


/*
 * can_coerce_type()
 *		Can input_typeids be coerced to target_typeids?
 *
 * We must be told the context (CAST construct, assignment, implicit coercion)
 * as this determines the set of available casts.
 */
bool
can_coerce_type(int nargs, Oid *input_typeids, Oid *target_typeids,
				CoercionContext ccontext)
{
	bool		have_generics = false;
	int			i;

	/* run through argument list... */
	for (i = 0; i < nargs; i++)
	{
		Oid			inputTypeId = input_typeids[i];
		Oid			targetTypeId = target_typeids[i];
		CoercionPathType pathtype;
		Oid			funcId;

		/* no problem if same type */
		if (inputTypeId == targetTypeId)
			continue;

		/* accept if target is ANY */
		if (targetTypeId == ANYOID)
			continue;

		/* accept if target is polymorphic, for now */
		if (IsPolymorphicType(targetTypeId))
		{
			have_generics = true;		/* do more checking later */
			continue;
		}

		/*
		 * If input is an untyped string constant, assume we can convert it to
		 * anything.
		 */
		if (inputTypeId == UNKNOWNOID)
			continue;

		/*
		 * If pg_cast shows that we can coerce, accept.  This test now covers
		 * both binary-compatible and coercion-function cases.
		 */
		pathtype = find_coercion_pathway(targetTypeId, inputTypeId, ccontext,
										 &funcId);
		if (pathtype != COERCION_PATH_NONE)
			continue;

		/*
		 * If input is RECORD and target is a composite type, assume we can
		 * coerce (may need tighter checking here)
		 */
		if (inputTypeId == RECORDOID &&
			ISCOMPLEX(targetTypeId))
			continue;

		/*
		 * If input is a composite type and target is RECORD, accept
		 */
		if (targetTypeId == RECORDOID &&
			ISCOMPLEX(inputTypeId))
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

	/* If we found any generic argument types, cross-check them */
	if (have_generics)
	{
		if (!check_generic_type_consistency(input_typeids, target_typeids,
											nargs))
			return false;
	}

	return true;
}


/*
 * Create an expression tree to represent coercion to a domain type.
 *
 * 'arg': input expression
 * 'baseTypeId': base type of domain, if known (pass InvalidOid if caller
 *		has not bothered to look this up)
 * 'baseTypeMod': base type typmod of domain, if known (pass -1 if caller
 *		has not bothered to look this up)
 * 'typeId': target type to coerce to
 * 'cformat': coercion format
 * 'hideInputCoercion': if true, hide the input coercion under this one.
 * 'lengthCoercionDone': if true, caller already accounted for length,
 *		ie the input is already of baseTypMod as well as baseTypeId.
 *
 * If the target type isn't a domain, the given 'arg' is returned as-is.
 */
Node *
coerce_to_domain(Node *arg, Oid baseTypeId, int32 baseTypeMod, Oid typeId,
				 CoercionForm cformat, bool hideInputCoercion,
				 bool lengthCoercionDone)
{
	CoerceToDomain *result;

	/* Get the base type if it hasn't been supplied */
	if (baseTypeId == InvalidOid)
		baseTypeId = getBaseTypeAndTypmod(typeId, &baseTypeMod);

	/* If it isn't a domain, return the node as it was passed in */
	if (baseTypeId == typeId)
		return arg;

	/* Suppress display of nested coercion steps */
	if (hideInputCoercion)
		hide_coercion_node(arg);

	/*
	 * If the domain applies a typmod to its base type, build the appropriate
	 * coercion step.  Mark it implicit for display purposes, because we don't
	 * want it shown separately by ruleutils.c; but the isExplicit flag passed
	 * to the conversion function depends on the manner in which the domain
	 * coercion is invoked, so that the semantics of implicit and explicit
	 * coercion differ.  (Is that really the behavior we want?)
	 *
	 * NOTE: because we apply this as part of the fixed expression structure,
	 * ALTER DOMAIN cannot alter the typtypmod.  But it's unclear that that
	 * would be safe to do anyway, without lots of knowledge about what the
	 * base type thinks the typmod means.
	 */
	if (!lengthCoercionDone)
	{
		if (baseTypeMod >= 0)
			arg = coerce_type_typmod(arg, baseTypeId, baseTypeMod,
									 COERCE_IMPLICIT_CAST,
									 (cformat != COERCE_IMPLICIT_CAST),
									 false);
	}

	/*
	 * Now build the domain coercion node.	This represents run-time checking
	 * of any constraints currently attached to the domain.  This also ensures
	 * that the expression is properly labeled as to result type.
	 */
	result = makeNode(CoerceToDomain);
	result->arg = (Expr *) arg;
	result->resulttype = typeId;
	result->resulttypmod = -1;	/* currently, always -1 for domains */
	result->coercionformat = cformat;

	return (Node *) result;
}


/*
 * coerce_type_typmod()
 *		Force a value to a particular typmod, if meaningful and possible.
 *
 * This is applied to values that are going to be stored in a relation
 * (where we have an atttypmod for the column) as well as values being
 * explicitly CASTed (where the typmod comes from the target type spec).
 *
 * The caller must have already ensured that the value is of the correct
 * type, typically by applying coerce_type.
 *
 * cformat determines the display properties of the generated node (if any),
 * while isExplicit may affect semantics.  If hideInputCoercion is true
 * *and* we generate a node, the input node is forced to IMPLICIT display
 * form, so that only the typmod coercion node will be visible when
 * displaying the expression.
 *
 * NOTE: this does not need to work on domain types, because any typmod
 * coercion for a domain is considered to be part of the type coercion
 * needed to produce the domain value in the first place.  So, no getBaseType.
 */
static Node *
coerce_type_typmod(Node *node, Oid targetTypeId, int32 targetTypMod,
				   CoercionForm cformat, bool isExplicit,
				   bool hideInputCoercion)
{
	CoercionPathType pathtype;
	Oid			funcId;

	/*
	 * A negative typmod is assumed to mean that no coercion is wanted. Also,
	 * skip coercion if already done.
	 */
	if (targetTypMod < 0 || targetTypMod == exprTypmod(node))
		return node;

	pathtype = find_typmod_coercion_function(targetTypeId, &funcId);

	if (pathtype != COERCION_PATH_NONE)
	{
		/* Suppress display of nested coercion steps */
		if (hideInputCoercion)
			hide_coercion_node(node);

		node = build_coercion_expression(node, pathtype, funcId,
										 targetTypeId, targetTypMod,
										 cformat, isExplicit);
	}

	return node;
}

/*
 * Mark a coercion node as IMPLICIT so it will never be displayed by
 * ruleutils.c.  We use this when we generate a nest of coercion nodes
 * to implement what is logically one conversion; the inner nodes are
 * forced to IMPLICIT_CAST format.	This does not change their semantics,
 * only display behavior.
 *
 * It is caller error to call this on something that doesn't have a
 * CoercionForm field.
 */
static void
hide_coercion_node(Node *node)
{
	if (IsA(node, FuncExpr))
		((FuncExpr *) node)->funcformat = COERCE_IMPLICIT_CAST;
	else if (IsA(node, RelabelType))
		((RelabelType *) node)->relabelformat = COERCE_IMPLICIT_CAST;
	else if (IsA(node, CoerceViaIO))
		((CoerceViaIO *) node)->coerceformat = COERCE_IMPLICIT_CAST;
	else if (IsA(node, ArrayCoerceExpr))
		((ArrayCoerceExpr *) node)->coerceformat = COERCE_IMPLICIT_CAST;
	else if (IsA(node, ConvertRowtypeExpr))
		((ConvertRowtypeExpr *) node)->convertformat = COERCE_IMPLICIT_CAST;
	else if (IsA(node, RowExpr))
		((RowExpr *) node)->row_format = COERCE_IMPLICIT_CAST;
	else if (IsA(node, CoerceToDomain))
		((CoerceToDomain *) node)->coercionformat = COERCE_IMPLICIT_CAST;
	else
		elog(ERROR, "unsupported node type: %d", (int) nodeTag(node));
}

/*
 * build_coercion_expression()
 *		Construct an expression tree for applying a pg_cast entry.
 *
 * This is used for both type-coercion and length-coercion operations,
 * since there is no difference in terms of the calling convention.
 */
static Node *
build_coercion_expression(Node *node,
						  CoercionPathType pathtype,
						  Oid funcId,
						  Oid targetTypeId, int32 targetTypMod,
						  CoercionForm cformat, bool isExplicit)
{
	int			nargs = 0;

	if (OidIsValid(funcId))
	{
		HeapTuple	tp;
		Form_pg_proc procstruct;

		tp = SearchSysCache(PROCOID,
							ObjectIdGetDatum(funcId),
							0, 0, 0);
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for function %u", funcId);
		procstruct = (Form_pg_proc) GETSTRUCT(tp);

		/*
		 * These Asserts essentially check that function is a legal coercion
		 * function.  We can't make the seemingly obvious tests on prorettype
		 * and proargtypes[0], even in the COERCION_PATH_FUNC case, because of
		 * various binary-compatibility cases.
		 */
		/* Assert(targetTypeId == procstruct->prorettype); */
		Assert(!procstruct->proretset);
		Assert(!procstruct->proisagg);
		nargs = procstruct->pronargs;
		Assert(nargs >= 1 && nargs <= 3);
		/* Assert(procstruct->proargtypes.values[0] == exprType(node)); */
		Assert(nargs < 2 || procstruct->proargtypes.values[1] == INT4OID);
		Assert(nargs < 3 || procstruct->proargtypes.values[2] == BOOLOID);

		ReleaseSysCache(tp);
	}

	if (pathtype == COERCION_PATH_FUNC)
	{
		/* We build an ordinary FuncExpr with special arguments */
		List	   *args;
		Const	   *cons;

		Assert(OidIsValid(funcId));

		args = list_make1(node);

		if (nargs >= 2)
		{
			/* Pass target typmod as an int4 constant */
			cons = makeConst(INT4OID,
							 -1,
							 sizeof(int32),
							 Int32GetDatum(targetTypMod),
							 false,
							 true);

			args = lappend(args, cons);
		}

		if (nargs == 3)
		{
			/* Pass it a boolean isExplicit parameter, too */
			cons = makeConst(BOOLOID,
							 -1,
							 sizeof(bool),
							 BoolGetDatum(isExplicit),
							 false,
							 true);

			args = lappend(args, cons);
		}

		return (Node *) makeFuncExpr(funcId, targetTypeId, args, cformat);
	}
	else if (pathtype == COERCION_PATH_ARRAYCOERCE)
	{
		/* We need to build an ArrayCoerceExpr */
		ArrayCoerceExpr *acoerce = makeNode(ArrayCoerceExpr);

		acoerce->arg = (Expr *) node;
		acoerce->elemfuncid = funcId;
		acoerce->resulttype = targetTypeId;

		/*
		 * Label the output as having a particular typmod only if we are
		 * really invoking a length-coercion function, ie one with more than
		 * one argument.
		 */
		acoerce->resulttypmod = (nargs >= 2) ? targetTypMod : -1;
		acoerce->isExplicit = isExplicit;
		acoerce->coerceformat = cformat;

		return (Node *) acoerce;
	}
	else if (pathtype == COERCION_PATH_COERCEVIAIO)
	{
		/* We need to build a CoerceViaIO node */
		CoerceViaIO *iocoerce = makeNode(CoerceViaIO);

		Assert(!OidIsValid(funcId));

		iocoerce->arg = (Expr *) node;
		iocoerce->resulttype = targetTypeId;
		iocoerce->coerceformat = cformat;

		return (Node *) iocoerce;
	}
	else
	{
		elog(ERROR, "unsupported pathtype %d in build_coercion_expression",
			 (int) pathtype);
		return NULL;			/* keep compiler quiet */
	}
}


/*
 * coerce_record_to_complex
 *		Coerce a RECORD to a specific composite type.
 *
 * Currently we only support this for inputs that are RowExprs or whole-row
 * Vars.
 */
static Node *
coerce_record_to_complex(ParseState *pstate, Node *node,
						 Oid targetTypeId,
						 CoercionContext ccontext,
						 CoercionForm cformat)
{
	RowExpr    *rowexpr;
	TupleDesc	tupdesc;
	List	   *args = NIL;
	List	   *newargs;
	int			i;
	int			ucolno;
	ListCell   *arg;

	if (node && IsA(node, RowExpr))
	{
		/*
		 * Since the RowExpr must be of type RECORD, we needn't worry about it
		 * containing any dropped columns.
		 */
		args = ((RowExpr *) node)->args;
	}
	else if (node && IsA(node, Var) &&
			 ((Var *) node)->varattno == InvalidAttrNumber)
	{
		int			rtindex = ((Var *) node)->varno;
		int			sublevels_up = ((Var *) node)->varlevelsup;
		RangeTblEntry *rte;

		rte = GetRTEByRangeTablePosn(pstate, rtindex, sublevels_up);
		expandRTE(rte, rtindex, sublevels_up, false,
				  NULL, &args);
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
				 errmsg("cannot cast type %s to %s",
						format_type_be(RECORDOID),
						format_type_be(targetTypeId))));

	tupdesc = lookup_rowtype_tupdesc(targetTypeId, -1);
	newargs = NIL;
	ucolno = 1;
	arg = list_head(args);
	for (i = 0; i < tupdesc->natts; i++)
	{
		Node	   *expr;
		Oid			exprtype;

		/* Fill in NULLs for dropped columns in rowtype */
		if (tupdesc->attrs[i]->attisdropped)
		{
			/*
			 * can't use atttypid here, but it doesn't really matter what type
			 * the Const claims to be.
			 */
			newargs = lappend(newargs, makeNullConst(INT4OID, -1));
			continue;
		}

		if (arg == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CANNOT_COERCE),
					 errmsg("cannot cast type %s to %s",
							format_type_be(RECORDOID),
							format_type_be(targetTypeId)),
					 errdetail("Input has too few columns.")));
		expr = (Node *) lfirst(arg);
		exprtype = exprType(expr);

		expr = coerce_to_target_type(pstate,
									 expr, exprtype,
									 tupdesc->attrs[i]->atttypid,
									 tupdesc->attrs[i]->atttypmod,
									 ccontext,
									 COERCE_IMPLICIT_CAST);
		if (expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CANNOT_COERCE),
					 errmsg("cannot cast type %s to %s",
							format_type_be(RECORDOID),
							format_type_be(targetTypeId)),
					 errdetail("Cannot cast type %s to %s in column %d.",
							   format_type_be(exprtype),
							   format_type_be(tupdesc->attrs[i]->atttypid),
							   ucolno)));
		newargs = lappend(newargs, expr);
		ucolno++;
		arg = lnext(arg);
	}
	if (arg != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
				 errmsg("cannot cast type %s to %s",
						format_type_be(RECORDOID),
						format_type_be(targetTypeId)),
				 errdetail("Input has too many columns.")));

	ReleaseTupleDesc(tupdesc);

	rowexpr = makeNode(RowExpr);
	rowexpr->args = newargs;
	rowexpr->row_typeid = targetTypeId;
	rowexpr->row_format = cformat;
	return (Node *) rowexpr;
}

/*
 * coerce_to_boolean()
 *		Coerce an argument of a construct that requires boolean input
 *		(AND, OR, NOT, etc).  Also check that input is not a set.
 *
 * Returns the possibly-transformed node tree.
 *
 * As with coerce_type, pstate may be NULL if no special unknown-Param
 * processing is wanted.
 */
Node *
coerce_to_boolean(ParseState *pstate, Node *node,
				  const char *constructName)
{
	Oid			inputTypeId = exprType(node);

	if (inputTypeId != BOOLOID)
	{
		node = coerce_to_target_type(pstate, node, inputTypeId,
									 BOOLOID, -1,
									 COERCION_ASSIGNMENT,
									 COERCE_IMPLICIT_CAST);
		if (node == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
			/* translator: first %s is name of a SQL construct, eg WHERE */
				   errmsg("argument of %s must be type boolean, not type %s",
						  constructName, format_type_be(inputTypeId))));
	}

	if (expression_returns_set(node))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		/* translator: %s is name of a SQL construct, eg WHERE */
				 errmsg("argument of %s must not return a set",
						constructName)));

	return node;
}

/*
 * coerce_to_specific_type()
 *		Coerce an argument of a construct that requires a specific data type.
 *		Also check that input is not a set.
 *
 * Returns the possibly-transformed node tree.
 *
 * As with coerce_type, pstate may be NULL if no special unknown-Param
 * processing is wanted.
 */
Node *
coerce_to_specific_type(ParseState *pstate, Node *node,
						Oid targetTypeId,
						const char *constructName)
{
	Oid			inputTypeId = exprType(node);

	if (inputTypeId != targetTypeId)
	{
		node = coerce_to_target_type(pstate, node, inputTypeId,
									 targetTypeId, -1,
									 COERCION_ASSIGNMENT,
									 COERCE_IMPLICIT_CAST);
		if (node == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
			/* translator: first %s is name of a SQL construct, eg LIMIT */
					 errmsg("argument of %s must be type %s, not type %s",
							constructName,
							format_type_be(targetTypeId),
							format_type_be(inputTypeId))));
	}

	if (expression_returns_set(node))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		/* translator: %s is name of a SQL construct, eg LIMIT */
				 errmsg("argument of %s must not return a set",
						constructName)));

	return node;
}


/* select_common_type()
 *		Determine the common supertype of a list of input expression types.
 *		This is used for determining the output type of CASE and UNION
 *		constructs.
 *
 * 'typeids' is a nonempty list of type OIDs.  Note that earlier items
 * in the list will be preferred if there is doubt.
 * 'context' is a phrase to use in the error message if we fail to select
 * a usable type.
 */
Oid
select_common_type(List *typeids, const char *context)
{
	Oid			ptype;
	CATEGORY	pcategory;
	ListCell   *type_item;

	Assert(typeids != NIL);
	ptype = linitial_oid(typeids);

	/*
	 * If all input types are valid and exactly the same, just pick that type.
	 * This is the only way that we will resolve the result as being a domain
	 * type; otherwise domains are smashed to their base types for comparison.
	 */
	if (ptype != UNKNOWNOID)
	{
		for_each_cell(type_item, lnext(list_head(typeids)))
		{
			Oid		ntype = lfirst_oid(type_item);

			if (ntype != ptype)
				break;
		}
		if (type_item == NULL)			/* got to the end of the list? */
			return ptype;
	}

	/* Nope, so set up for the full algorithm */
	ptype = getBaseType(ptype);
	pcategory = TypeCategory(ptype);

	for_each_cell(type_item, lnext(list_head(typeids)))
	{
		Oid			ntype = getBaseType(lfirst_oid(type_item));

		/* move on to next one if no new information... */
		if (ntype != UNKNOWNOID && ntype != ptype)
		{
			if (ptype == UNKNOWNOID)
			{
				/* so far, only unknowns so take anything... */
				ptype = ntype;
				pcategory = TypeCategory(ptype);
			}
			else if (TypeCategory(ntype) != pcategory)
			{
				/*
				 * both types in different categories? then not much hope...
				 */
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),

				/*------
				  translator: first %s is name of a SQL construct, eg CASE */
						 errmsg("%s types %s and %s cannot be matched",
								context,
								format_type_be(ptype),
								format_type_be(ntype))));
			}
			else if (!IsPreferredType(pcategory, ptype) &&
					 can_coerce_type(1, &ptype, &ntype, COERCION_IMPLICIT) &&
					 !can_coerce_type(1, &ntype, &ptype, COERCION_IMPLICIT))
			{
				/*
				 * take new type if can coerce to it implicitly but not the
				 * other way; but if we have a preferred type, stay on it.
				 */
				ptype = ntype;
				pcategory = TypeCategory(ptype);
			}
		}
	}

	/*
	 * If all the inputs were UNKNOWN type --- ie, unknown-type literals ---
	 * then resolve as type TEXT.  This situation comes up with constructs
	 * like SELECT (CASE WHEN foo THEN 'bar' ELSE 'baz' END); SELECT 'foo'
	 * UNION SELECT 'bar'; It might seem desirable to leave the construct's
	 * output type as UNKNOWN, but that really doesn't work, because we'd
	 * probably end up needing a runtime coercion from UNKNOWN to something
	 * else, and we usually won't have it.  We need to coerce the unknown
	 * literals while they are still literals, so a decision has to be made
	 * now.
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
 * As with coerce_type, pstate may be NULL if no special unknown-Param
 * processing is wanted.
 */
Node *
coerce_to_common_type(ParseState *pstate, Node *node,
					  Oid targetTypeId, const char *context)
{
	Oid			inputTypeId = exprType(node);

	if (inputTypeId == targetTypeId)
		return node;			/* no work */
	if (can_coerce_type(1, &inputTypeId, &targetTypeId, COERCION_IMPLICIT))
		node = coerce_type(pstate, node, inputTypeId, targetTypeId, -1,
						   COERCION_IMPLICIT, COERCE_IMPLICIT_CAST);
	else
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
		/* translator: first %s is name of a SQL construct, eg CASE */
				 errmsg("%s could not convert type %s to %s",
						context,
						format_type_be(inputTypeId),
						format_type_be(targetTypeId))));
	return node;
}

/*
 * check_generic_type_consistency()
 *		Are the actual arguments potentially compatible with a
 *		polymorphic function?
 *
 * The argument consistency rules are:
 *
 * 1) All arguments declared ANYARRAY must have matching datatypes,
 *	  and must in fact be varlena arrays.
 * 2) All arguments declared ANYELEMENT must have matching datatypes.
 * 3) If there are arguments of both ANYELEMENT and ANYARRAY, make sure
 *	  the actual ANYELEMENT datatype is in fact the element type for
 *	  the actual ANYARRAY datatype.
 * 4) ANYENUM is treated the same as ANYELEMENT except that if it is used
 *	  (alone or in combination with plain ANYELEMENT), we add the extra
 *	  condition that the ANYELEMENT type must be an enum.
 * 5) ANYNONARRAY is treated the same as ANYELEMENT except that if it is used,
 *	  we add the extra condition that the ANYELEMENT type must not be an array.
 *	  (This is a no-op if used in combination with ANYARRAY or ANYENUM, but
 *	  is an extra restriction if not.)
 *
 * If we have UNKNOWN input (ie, an untyped literal) for any polymorphic
 * argument, assume it is okay.
 *
 * If an input is of type ANYARRAY (ie, we know it's an array, but not
 * what element type), we will accept it as a match to an argument declared
 * ANYARRAY, so long as we don't have to determine an element type ---
 * that is, so long as there is no use of ANYELEMENT.  This is mostly for
 * backwards compatibility with the pre-7.4 behavior of ANYARRAY.
 *
 * We do not ereport here, but just return FALSE if a rule is violated.
 */
bool
check_generic_type_consistency(Oid *actual_arg_types,
							   Oid *declared_arg_types,
							   int nargs)
{
	int			j;
	Oid			elem_typeid = InvalidOid;
	Oid			array_typeid = InvalidOid;
	Oid			array_typelem;
	bool		have_anyelement = false;
	bool		have_anynonarray = false;
	bool		have_anyenum = false;

	/*
	 * Loop through the arguments to see if we have any that are polymorphic.
	 * If so, require the actual types to be consistent.
	 */
	for (j = 0; j < nargs; j++)
	{
		Oid			decl_type = declared_arg_types[j];
		Oid			actual_type = actual_arg_types[j];

		if (decl_type == ANYELEMENTOID ||
			decl_type == ANYNONARRAYOID ||
			decl_type == ANYENUMOID)
		{
			have_anyelement = true;
			if (decl_type == ANYNONARRAYOID)
				have_anynonarray = true;
			else if (decl_type == ANYENUMOID)
				have_anyenum = true;
			if (actual_type == UNKNOWNOID)
				continue;
			if (OidIsValid(elem_typeid) && actual_type != elem_typeid)
				return false;
			elem_typeid = actual_type;
		}
		else if (decl_type == ANYARRAYOID)
		{
			if (actual_type == UNKNOWNOID)
				continue;
			if (OidIsValid(array_typeid) && actual_type != array_typeid)
				return false;
			array_typeid = actual_type;
		}
	}

	/* Get the element type based on the array type, if we have one */
	if (OidIsValid(array_typeid))
	{
		if (array_typeid == ANYARRAYOID)
		{
			/* Special case for ANYARRAY input: okay iff no ANYELEMENT */
			if (have_anyelement)
				return false;
			return true;
		}

		array_typelem = get_element_type(array_typeid);
		if (!OidIsValid(array_typelem))
			return false;		/* should be an array, but isn't */

		if (!OidIsValid(elem_typeid))
		{
			/*
			 * if we don't have an element type yet, use the one we just got
			 */
			elem_typeid = array_typelem;
		}
		else if (array_typelem != elem_typeid)
		{
			/* otherwise, they better match */
			return false;
		}
	}

	if (have_anynonarray)
	{
		/* require the element type to not be an array */
		if (type_is_array(elem_typeid))
			return false;
	}

	if (have_anyenum)
	{
		/* require the element type to be an enum */
		if (!type_is_enum(elem_typeid))
			return false;
	}

	/* Looks valid */
	return true;
}

/*
 * enforce_generic_type_consistency()
 *		Make sure a polymorphic function is legally callable, and
 *		deduce actual argument and result types.
 *
 * If any polymorphic pseudotype is used in a function's arguments or
 * return type, we make sure the actual data types are consistent with
 * each other. The argument consistency rules are shown above for
 * check_generic_type_consistency().
 *
 * If we have UNKNOWN input (ie, an untyped literal) for any polymorphic
 * argument, we attempt to deduce the actual type it should have.  If
 * successful, we alter that position of declared_arg_types[] so that
 * make_fn_arguments will coerce the literal to the right thing.
 *
 * Rules are applied to the function's return type (possibly altering it)
 * if it is declared as a polymorphic type:
 *
 * 1) If return type is ANYARRAY, and any argument is ANYARRAY, use the
 *	  argument's actual type as the function's return type.
 * 2) If return type is ANYARRAY, no argument is ANYARRAY, but any argument
 *	  is ANYELEMENT, use the actual type of the argument to determine
 *	  the function's return type, i.e. the element type's corresponding
 *	  array type.
 * 3) If return type is ANYARRAY, no argument is ANYARRAY or ANYELEMENT,
 *	  generate an ERROR. This condition is prevented by CREATE FUNCTION
 *	  and is therefore not expected here.
 * 4) If return type is ANYELEMENT, and any argument is ANYELEMENT, use the
 *	  argument's actual type as the function's return type.
 * 5) If return type is ANYELEMENT, no argument is ANYELEMENT, but any
 *	  argument is ANYARRAY, use the actual type of the argument to determine
 *	  the function's return type, i.e. the array type's corresponding
 *	  element type.
 * 6) If return type is ANYELEMENT, no argument is ANYARRAY or ANYELEMENT,
 *	  generate an ERROR. This condition is prevented by CREATE FUNCTION
 *	  and is therefore not expected here.
 * 7) ANYENUM is treated the same as ANYELEMENT except that if it is used
 *	  (alone or in combination with plain ANYELEMENT), we add the extra
 *	  condition that the ANYELEMENT type must be an enum.
 * 8) ANYNONARRAY is treated the same as ANYELEMENT except that if it is used,
 *	  we add the extra condition that the ANYELEMENT type must not be an array.
 *	  (This is a no-op if used in combination with ANYARRAY or ANYENUM, but
 *	  is an extra restriction if not.)
 *
 * When allow_poly is false, we are not expecting any of the actual_arg_types
 * to be polymorphic, and we should not return a polymorphic result type
 * either.  When allow_poly is true, it is okay to have polymorphic "actual"
 * arg types, and we can return ANYARRAY or ANYELEMENT as the result.  (This
 * case is currently used only to check compatibility of an aggregate's
 * declaration with the underlying transfn.)
 */
Oid
enforce_generic_type_consistency(Oid *actual_arg_types,
								 Oid *declared_arg_types,
								 int nargs,
								 Oid rettype,
								 bool allow_poly)
{
	int			j;
	bool		have_generics = false;
	bool		have_unknowns = false;
	Oid			elem_typeid = InvalidOid;
	Oid			array_typeid = InvalidOid;
	Oid			array_typelem;
	bool		have_anynonarray = (rettype == ANYNONARRAYOID);
	bool		have_anyenum = (rettype == ANYENUMOID);

	/*
	 * Loop through the arguments to see if we have any that are polymorphic.
	 * If so, require the actual types to be consistent.
	 */
	for (j = 0; j < nargs; j++)
	{
		Oid			decl_type = declared_arg_types[j];
		Oid			actual_type = actual_arg_types[j];

		if (decl_type == ANYELEMENTOID ||
			decl_type == ANYNONARRAYOID ||
			decl_type == ANYENUMOID)
		{
			have_generics = true;
			if (decl_type == ANYNONARRAYOID)
				have_anynonarray = true;
			else if (decl_type == ANYENUMOID)
				have_anyenum = true;
			if (actual_type == UNKNOWNOID)
			{
				have_unknowns = true;
				continue;
			}
			if (allow_poly && decl_type == actual_type)
				continue;		/* no new information here */
			if (OidIsValid(elem_typeid) && actual_type != elem_typeid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
				errmsg("arguments declared \"anyelement\" are not all alike"),
						 errdetail("%s versus %s",
								   format_type_be(elem_typeid),
								   format_type_be(actual_type))));
			elem_typeid = actual_type;
		}
		else if (decl_type == ANYARRAYOID)
		{
			have_generics = true;
			if (actual_type == UNKNOWNOID)
			{
				have_unknowns = true;
				continue;
			}
			if (allow_poly && decl_type == actual_type)
				continue;		/* no new information here */
			if (OidIsValid(array_typeid) && actual_type != array_typeid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("arguments declared \"anyarray\" are not all alike"),
						 errdetail("%s versus %s",
								   format_type_be(array_typeid),
								   format_type_be(actual_type))));
			array_typeid = actual_type;
		}
	}

	/*
	 * Fast Track: if none of the arguments are polymorphic, return the
	 * unmodified rettype.	We assume it can't be polymorphic either.
	 */
	if (!have_generics)
		return rettype;

	/* Get the element type based on the array type, if we have one */
	if (OidIsValid(array_typeid))
	{
		array_typelem = get_element_type(array_typeid);
		if (!OidIsValid(array_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared \"anyarray\" is not an array but type %s",
							format_type_be(array_typeid))));

		if (!OidIsValid(elem_typeid))
		{
			/*
			 * if we don't have an element type yet, use the one we just got
			 */
			elem_typeid = array_typelem;
		}
		else if (array_typelem != elem_typeid)
		{
			/* otherwise, they better match */
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared \"anyarray\" is not consistent with argument declared \"anyelement\""),
					 errdetail("%s versus %s",
							   format_type_be(array_typeid),
							   format_type_be(elem_typeid))));
		}
	}
	else if (!OidIsValid(elem_typeid))
	{
		if (allow_poly)
		{
			array_typeid = ANYARRAYOID;
			elem_typeid = ANYELEMENTOID;
		}
		else
		{
			/* Only way to get here is if all the generic args are UNKNOWN */
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("could not determine polymorphic type because input has type \"unknown\"")));
		}
	}

	if (have_anynonarray && elem_typeid != ANYELEMENTOID)
	{
		/* require the element type to not be an array */
		if (type_is_array(elem_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
				   errmsg("type matched to anynonarray is an array type: %s",
						  format_type_be(elem_typeid))));
	}

	if (have_anyenum && elem_typeid != ANYELEMENTOID)
	{
		/* require the element type to be an enum */
		if (!type_is_enum(elem_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("type matched to anyenum is not an enum type: %s",
							format_type_be(elem_typeid))));
	}

	/*
	 * If we had any unknown inputs, re-scan to assign correct types
	 */
	if (have_unknowns)
	{
		for (j = 0; j < nargs; j++)
		{
			Oid			decl_type = declared_arg_types[j];
			Oid			actual_type = actual_arg_types[j];

			if (actual_type != UNKNOWNOID)
				continue;

			if (decl_type == ANYELEMENTOID ||
				decl_type == ANYNONARRAYOID ||
				decl_type == ANYENUMOID)
				declared_arg_types[j] = elem_typeid;
			else if (decl_type == ANYARRAYOID)
			{
				if (!OidIsValid(array_typeid))
				{
					array_typeid = get_array_type(elem_typeid);
					if (!OidIsValid(array_typeid))
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("could not find array type for data type %s",
								format_type_be(elem_typeid))));
				}
				declared_arg_types[j] = array_typeid;
			}
		}
	}

	/* if we return ANYARRAY use the appropriate argument type */
	if (rettype == ANYARRAYOID)
	{
		if (!OidIsValid(array_typeid))
		{
			array_typeid = get_array_type(elem_typeid);
			if (!OidIsValid(array_typeid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("could not find array type for data type %s",
								format_type_be(elem_typeid))));
		}
		return array_typeid;
	}

	/* if we return ANYELEMENT use the appropriate argument type */
	if (rettype == ANYELEMENTOID ||
		rettype == ANYNONARRAYOID ||
		rettype == ANYENUMOID)
		return elem_typeid;

	/* we don't return a generic type; send back the original return type */
	return rettype;
}

/*
 * resolve_generic_type()
 *		Deduce an individual actual datatype on the assumption that
 *		the rules for polymorphic types are being followed.
 *
 * declared_type is the declared datatype we want to resolve.
 * context_actual_type is the actual input datatype to some argument
 * that has declared datatype context_declared_type.
 *
 * If declared_type isn't polymorphic, we just return it.  Otherwise,
 * context_declared_type must be polymorphic, and we deduce the correct
 * return type based on the relationship of the two polymorphic types.
 */
Oid
resolve_generic_type(Oid declared_type,
					 Oid context_actual_type,
					 Oid context_declared_type)
{
	if (declared_type == ANYARRAYOID)
	{
		if (context_declared_type == ANYARRAYOID)
		{
			/* Use actual type, but it must be an array */
			Oid			array_typelem = get_element_type(context_actual_type);

			if (!OidIsValid(array_typelem))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("argument declared \"anyarray\" is not an array but type %s",
								format_type_be(context_actual_type))));
			return context_actual_type;
		}
		else if (context_declared_type == ANYELEMENTOID ||
				 context_declared_type == ANYNONARRAYOID ||
				 context_declared_type == ANYENUMOID)
		{
			/* Use the array type corresponding to actual type */
			Oid			array_typeid = get_array_type(context_actual_type);

			if (!OidIsValid(array_typeid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("could not find array type for data type %s",
								format_type_be(context_actual_type))));
			return array_typeid;
		}
	}
	else if (declared_type == ANYELEMENTOID ||
			 declared_type == ANYNONARRAYOID ||
			 declared_type == ANYENUMOID)
	{
		if (context_declared_type == ANYARRAYOID)
		{
			/* Use the element type corresponding to actual type */
			Oid			array_typelem = get_element_type(context_actual_type);

			if (!OidIsValid(array_typelem))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("argument declared \"anyarray\" is not an array but type %s",
								format_type_be(context_actual_type))));
			return array_typelem;
		}
		else if (context_declared_type == ANYELEMENTOID ||
				 context_declared_type == ANYNONARRAYOID ||
				 context_declared_type == ANYENUMOID)
		{
			/* Use the actual type; it doesn't matter if array or not */
			return context_actual_type;
		}
	}
	else
	{
		/* declared_type isn't polymorphic, so return it as-is */
		return declared_type;
	}
	/* If we get here, declared_type is polymorphic and context isn't */
	/* NB: this is a calling-code logic error, not a user error */
	elog(ERROR, "could not determine polymorphic type because context isn't polymorphic");
	return InvalidOid;			/* keep compiler quiet */
}


/* TypeCategory()
 *		Assign a category to the specified type OID.
 *
 * NB: this must not return INVALID_TYPE.
 *
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
		case (REGCONFIGOID):
		case (REGDICTIONARYOID):
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

		case (RECORDOID):
		case (CSTRINGOID):
		case (ANYOID):
		case (ANYARRAYOID):
		case (VOIDOID):
		case (TRIGGEROID):
		case (LANGUAGE_HANDLEROID):
		case (INTERNALOID):
		case (OPAQUEOID):
		case (ANYELEMENTOID):
		case (ANYNONARRAYOID):
		case (ANYENUMOID):
			result = GENERIC_TYPE;
			break;

		default:
			result = USER_TYPE;
			break;
	}
	return result;
}	/* TypeCategory() */


/* IsPreferredType()
 *		Check if this type is a preferred type for the given category.
 *
 * If category is INVALID_TYPE, then we'll return TRUE for preferred types
 * of any category; otherwise, only for preferred types of that category.
 *
 * XXX This should be moved to system catalog lookups
 * to allow for better type extensibility.
 * - thomas 2001-09-30
 */
bool
IsPreferredType(CATEGORY category, Oid type)
{
	Oid			preftype;

	if (category == INVALID_TYPE)
		category = TypeCategory(type);
	else if (category != TypeCategory(type))
		return false;

	/*
	 * This switch should agree with TypeCategory(), above.  Note that at this
	 * point, category certainly matches the type.
	 */
	switch (category)
	{
		case (UNKNOWN_TYPE):
		case (GENERIC_TYPE):
			preftype = UNKNOWNOID;
			break;

		case (BOOLEAN_TYPE):
			preftype = BOOLOID;
			break;

		case (STRING_TYPE):
			preftype = TEXTOID;
			break;

		case (BITSTRING_TYPE):
			preftype = VARBITOID;
			break;

		case (NUMERIC_TYPE):
			if (type == OIDOID ||
				type == REGPROCOID ||
				type == REGPROCEDUREOID ||
				type == REGOPEROID ||
				type == REGOPERATOROID ||
				type == REGCLASSOID ||
				type == REGTYPEOID ||
				type == REGCONFIGOID ||
				type == REGDICTIONARYOID)
				preftype = OIDOID;
			else
				preftype = FLOAT8OID;
			break;

		case (DATETIME_TYPE):
			if (type == DATEOID)
				preftype = TIMESTAMPOID;
			else
				preftype = TIMESTAMPTZOID;
			break;

		case (TIMESPAN_TYPE):
			preftype = INTERVALOID;
			break;

		case (GEOMETRIC_TYPE):
			preftype = type;
			break;

		case (NETWORK_TYPE):
			preftype = INETOID;
			break;

		case (USER_TYPE):
			preftype = type;
			break;

		default:
			elog(ERROR, "unrecognized type category: %d", (int) category);
			preftype = UNKNOWNOID;
			break;
	}

	return (type == preftype);
}	/* IsPreferredType() */


/* IsBinaryCoercible()
 *		Check if srctype is binary-coercible to targettype.
 *
 * This notion allows us to cheat and directly exchange values without
 * going through the trouble of calling a conversion function.	Note that
 * in general, this should only be an implementation shortcut.	Before 7.4,
 * this was also used as a heuristic for resolving overloaded functions and
 * operators, but that's basically a bad idea.
 *
 * As of 7.3, binary coercibility isn't hardwired into the code anymore.
 * We consider two types binary-coercible if there is an implicitly
 * invokable, no-function-needed pg_cast entry.  Also, a domain is always
 * binary-coercible to its base type, though *not* vice versa (in the other
 * direction, one must apply domain constraint checks before accepting the
 * value as legitimate).  We also need to special-case the polymorphic
 * ANYARRAY type.
 *
 * This function replaces IsBinaryCompatible(), which was an inherently
 * symmetric test.	Since the pg_cast entries aren't necessarily symmetric,
 * the order of the operands is now significant.
 */
bool
IsBinaryCoercible(Oid srctype, Oid targettype)
{
	HeapTuple	tuple;
	Form_pg_cast castForm;
	bool		result;

	/* Fast path if same type */
	if (srctype == targettype)
		return true;

	/* If srctype is a domain, reduce to its base type */
	if (OidIsValid(srctype))
		srctype = getBaseType(srctype);

	/* Somewhat-fast path for domain -> base type case */
	if (srctype == targettype)
		return true;

	/* Also accept any array type as coercible to ANYARRAY */
	if (targettype == ANYARRAYOID)
		if (type_is_array(srctype))
			return true;

	/* Also accept any non-array type as coercible to ANYNONARRAY */
	if (targettype == ANYNONARRAYOID)
		if (!type_is_array(srctype))
			return true;

	/* Also accept any enum type as coercible to ANYENUM */
	if (targettype == ANYENUMOID)
		if (type_is_enum(srctype))
			return true;

	/* Else look in pg_cast */
	tuple = SearchSysCache(CASTSOURCETARGET,
						   ObjectIdGetDatum(srctype),
						   ObjectIdGetDatum(targettype),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
		return false;			/* no cast */
	castForm = (Form_pg_cast) GETSTRUCT(tuple);

	result = (castForm->castfunc == InvalidOid &&
			  castForm->castcontext == COERCION_CODE_IMPLICIT);

	ReleaseSysCache(tuple);

	return result;
}


/*
 * find_coercion_pathway
 *		Look for a coercion pathway between two types.
 *
 * Currently, this deals only with scalar-type cases; it does not consider
 * polymorphic types nor casts between composite types.  (Perhaps fold
 * those in someday?)
 *
 * ccontext determines the set of available casts.
 *
 * The possible result codes are:
 *	COERCION_PATH_NONE: failed to find any coercion pathway
 *				*funcid is set to InvalidOid
 *	COERCION_PATH_FUNC: apply the coercion function returned in *funcid
 *	COERCION_PATH_RELABELTYPE: binary-compatible cast, no function needed
 *				*funcid is set to InvalidOid
 *	COERCION_PATH_ARRAYCOERCE: need an ArrayCoerceExpr node
 *				*funcid is set to the element cast function, or InvalidOid
 *				if the array elements are binary-compatible
 *	COERCION_PATH_COERCEVIAIO: need a CoerceViaIO node
 *				*funcid is set to InvalidOid
 *
 * Note: COERCION_PATH_RELABELTYPE does not necessarily mean that no work is
 * needed to do the coercion; if the target is a domain then we may need to
 * apply domain constraint checking.  If you want to check for a zero-effort
 * conversion then use IsBinaryCoercible().
 */
CoercionPathType
find_coercion_pathway(Oid targetTypeId, Oid sourceTypeId,
					  CoercionContext ccontext,
					  Oid *funcid)
{
	CoercionPathType result = COERCION_PATH_NONE;
	HeapTuple	tuple;

	*funcid = InvalidOid;

	/* Perhaps the types are domains; if so, look at their base types */
	if (OidIsValid(sourceTypeId))
		sourceTypeId = getBaseType(sourceTypeId);
	if (OidIsValid(targetTypeId))
		targetTypeId = getBaseType(targetTypeId);

	/* Domains are always coercible to and from their base type */
	if (sourceTypeId == targetTypeId)
		return COERCION_PATH_RELABELTYPE;

	/* Look in pg_cast */
	tuple = SearchSysCache(CASTSOURCETARGET,
						   ObjectIdGetDatum(sourceTypeId),
						   ObjectIdGetDatum(targetTypeId),
						   0, 0);

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_cast castForm = (Form_pg_cast) GETSTRUCT(tuple);
		CoercionContext castcontext;

		/* convert char value for castcontext to CoercionContext enum */
		switch (castForm->castcontext)
		{
			case COERCION_CODE_IMPLICIT:
				castcontext = COERCION_IMPLICIT;
				break;
			case COERCION_CODE_ASSIGNMENT:
				castcontext = COERCION_ASSIGNMENT;
				break;
			case COERCION_CODE_EXPLICIT:
				castcontext = COERCION_EXPLICIT;
				break;
			default:
				elog(ERROR, "unrecognized castcontext: %d",
					 (int) castForm->castcontext);
				castcontext = 0;	/* keep compiler quiet */
				break;
		}

		/* Rely on ordering of enum for correct behavior here */
		if (ccontext >= castcontext)
		{
			*funcid = castForm->castfunc;
			if (OidIsValid(*funcid))
				result = COERCION_PATH_FUNC;
			else
				result = COERCION_PATH_RELABELTYPE;
		}

		ReleaseSysCache(tuple);
	}
	else
	{
		/*
		 * If there's no pg_cast entry, perhaps we are dealing with a pair of
		 * array types.  If so, and if the element types have a suitable cast,
		 * report that we can coerce with an ArrayCoerceExpr.
		 *
		 * Hack: disallow coercions to oidvector and int2vector, which
		 * otherwise tend to capture coercions that should go to "real" array
		 * types.  We want those types to be considered "real" arrays for many
		 * purposes, but not this one.	(Also, ArrayCoerceExpr isn't
		 * guaranteed to produce an output that meets the restrictions of
		 * these datatypes, such as being 1-dimensional.)
		 */
		if (targetTypeId != OIDVECTOROID && targetTypeId != INT2VECTOROID)
		{
			Oid			targetElem;
			Oid			sourceElem;

			if ((targetElem = get_element_type(targetTypeId)) != InvalidOid &&
				(sourceElem = get_element_type(sourceTypeId)) != InvalidOid)
			{
				CoercionPathType elempathtype;
				Oid			elemfuncid;

				elempathtype = find_coercion_pathway(targetElem,
													 sourceElem,
													 ccontext,
													 &elemfuncid);
				if (elempathtype != COERCION_PATH_NONE &&
					elempathtype != COERCION_PATH_ARRAYCOERCE)
				{
					*funcid = elemfuncid;
					if (elempathtype == COERCION_PATH_COERCEVIAIO)
						result = COERCION_PATH_COERCEVIAIO;
					else
						result = COERCION_PATH_ARRAYCOERCE;
				}
			}
		}

		/*
		 * If we still haven't found a possibility, consider automatic casting
		 * using I/O functions.  We allow assignment casts to textual types
		 * and explicit casts from textual types to be handled this way. (The
		 * CoerceViaIO mechanism is a lot more general than that, but this is
		 * all we want to allow in the absence of a pg_cast entry.) It would
		 * probably be better to insist on explicit casts in both directions,
		 * but this is a compromise to preserve something of the pre-8.3
		 * behavior that many types had implicit (yipes!) casts to text.
		 */
		if (result == COERCION_PATH_NONE)
		{
			if (ccontext >= COERCION_ASSIGNMENT &&
				(targetTypeId == TEXTOID ||
				 targetTypeId == VARCHAROID ||
				 targetTypeId == BPCHAROID))
				result = COERCION_PATH_COERCEVIAIO;
			else if (ccontext >= COERCION_EXPLICIT &&
					 (sourceTypeId == TEXTOID ||
					  sourceTypeId == VARCHAROID ||
					  sourceTypeId == BPCHAROID))
				result = COERCION_PATH_COERCEVIAIO;
		}
	}

	return result;
}


/*
 * find_typmod_coercion_function -- does the given type need length coercion?
 *
 * If the target type possesses a pg_cast function from itself to itself,
 * it must need length coercion.
 *
 * "bpchar" (ie, char(N)) and "numeric" are examples of such types.
 *
 * If the given type is a varlena array type, we do not look for a coercion
 * function associated directly with the array type, but instead look for
 * one associated with the element type.  An ArrayCoerceExpr node must be
 * used to apply such a function.
 *
 * We use the same result enum as find_coercion_pathway, but the only possible
 * result codes are:
 *	COERCION_PATH_NONE: no length coercion needed
 *	COERCION_PATH_FUNC: apply the function returned in *funcid
 *	COERCION_PATH_ARRAYCOERCE: apply the function using ArrayCoerceExpr
 */
CoercionPathType
find_typmod_coercion_function(Oid typeId,
							  Oid *funcid)
{
	CoercionPathType result;
	Type		targetType;
	Form_pg_type typeForm;
	HeapTuple	tuple;

	*funcid = InvalidOid;
	result = COERCION_PATH_FUNC;

	targetType = typeidType(typeId);
	typeForm = (Form_pg_type) GETSTRUCT(targetType);

	/* Check for a varlena array type (and not a domain) */
	if (typeForm->typelem != InvalidOid &&
		typeForm->typlen == -1 &&
		typeForm->typtype != TYPTYPE_DOMAIN)
	{
		/* Yes, switch our attention to the element type */
		typeId = typeForm->typelem;
		result = COERCION_PATH_ARRAYCOERCE;
	}
	ReleaseSysCache(targetType);

	/* Look in pg_cast */
	tuple = SearchSysCache(CASTSOURCETARGET,
						   ObjectIdGetDatum(typeId),
						   ObjectIdGetDatum(typeId),
						   0, 0);

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_cast castForm = (Form_pg_cast) GETSTRUCT(tuple);

		*funcid = castForm->castfunc;
		ReleaseSysCache(tuple);
	}

	if (!OidIsValid(*funcid))
		result = COERCION_PATH_NONE;

	return result;
}
