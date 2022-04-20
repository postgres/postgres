/*-------------------------------------------------------------------------
 *
 * funcapi.c
 *	  Utility and convenience functions for fmgr functions that return
 *	  sets and/or composite types, or deal with VARIADIC inputs.
 *
 * Copyright (c) 2002-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/fmgr/funcapi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"


typedef struct polymorphic_actuals
{
	Oid			anyelement_type;	/* anyelement mapping, if known */
	Oid			anyarray_type;	/* anyarray mapping, if known */
	Oid			anyrange_type;	/* anyrange mapping, if known */
	Oid			anymultirange_type; /* anymultirange mapping, if known */
} polymorphic_actuals;

static void shutdown_MultiFuncCall(Datum arg);
static TypeFuncClass internal_get_result_type(Oid funcid,
											  Node *call_expr,
											  ReturnSetInfo *rsinfo,
											  Oid *resultTypeId,
											  TupleDesc *resultTupleDesc);
static void resolve_anyelement_from_others(polymorphic_actuals *actuals);
static void resolve_anyarray_from_others(polymorphic_actuals *actuals);
static void resolve_anyrange_from_others(polymorphic_actuals *actuals);
static void resolve_anymultirange_from_others(polymorphic_actuals *actuals);
static bool resolve_polymorphic_tupdesc(TupleDesc tupdesc,
										oidvector *declared_args,
										Node *call_expr);
static TypeFuncClass get_type_func_class(Oid typid, Oid *base_typeid);


/*
 * SetSingleFuncCall
 *
 * Helper function to build the state of a set-returning function used
 * in the context of a single call with materialize mode.  This code
 * includes sanity checks on ReturnSetInfo, creates the Tuplestore and
 * the TupleDesc used with the function and stores them into the
 * function's ReturnSetInfo.
 *
 * "flags" can be set to SRF_SINGLE_USE_EXPECTED, to use the tuple
 * descriptor coming from expectedDesc, which is the tuple descriptor
 * expected by the caller.  SRF_SINGLE_BLESS can be set to complete the
 * information associated to the tuple descriptor, which is necessary
 * in some cases where the tuple descriptor comes from a transient
 * RECORD datatype.
 */
void
SetSingleFuncCall(FunctionCallInfo fcinfo, bits32 flags)
{
	bool		random_access;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *tupstore;
	MemoryContext old_context,
				per_query_ctx;
	TupleDesc	stored_tupdesc;

	/* check to see if caller supports returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize) ||
		((flags & SRF_SINGLE_USE_EXPECTED) != 0 && rsinfo->expectedDesc == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/*
	 * Store the tuplestore and the tuple descriptor in ReturnSetInfo.  This
	 * must be done in the per-query memory context.
	 */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	old_context = MemoryContextSwitchTo(per_query_ctx);

	/* build a tuple descriptor for our result type */
	if ((flags & SRF_SINGLE_USE_EXPECTED) != 0)
		stored_tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	else
	{
		if (get_call_result_type(fcinfo, NULL, &stored_tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
	}

	/* If requested, bless the tuple descriptor */
	if ((flags & SRF_SINGLE_BLESS) != 0)
		BlessTupleDesc(stored_tupdesc);

	random_access = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;

	tupstore = tuplestore_begin_heap(random_access, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = stored_tupdesc;
	MemoryContextSwitchTo(old_context);
}


/*
 * init_MultiFuncCall
 * Create an empty FuncCallContext data structure
 * and do some other basic Multi-function call setup
 * and error checking
 */
FuncCallContext *
init_MultiFuncCall(PG_FUNCTION_ARGS)
{
	FuncCallContext *retval;

	/*
	 * Bail if we're called in the wrong context
	 */
	if (fcinfo->resultinfo == NULL || !IsA(fcinfo->resultinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (fcinfo->flinfo->fn_extra == NULL)
	{
		/*
		 * First call
		 */
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
		MemoryContext multi_call_ctx;

		/*
		 * Create a suitably long-lived context to hold cross-call data
		 */
		multi_call_ctx = AllocSetContextCreate(fcinfo->flinfo->fn_mcxt,
											   "SRF multi-call context",
											   ALLOCSET_SMALL_SIZES);

		/*
		 * Allocate suitably long-lived space and zero it
		 */
		retval = (FuncCallContext *)
			MemoryContextAllocZero(multi_call_ctx,
								   sizeof(FuncCallContext));

		/*
		 * initialize the elements
		 */
		retval->call_cntr = 0;
		retval->max_calls = 0;
		retval->user_fctx = NULL;
		retval->attinmeta = NULL;
		retval->tuple_desc = NULL;
		retval->multi_call_memory_ctx = multi_call_ctx;

		/*
		 * save the pointer for cross-call use
		 */
		fcinfo->flinfo->fn_extra = retval;

		/*
		 * Ensure we will get shut down cleanly if the exprcontext is not run
		 * to completion.
		 */
		RegisterExprContextCallback(rsi->econtext,
									shutdown_MultiFuncCall,
									PointerGetDatum(fcinfo->flinfo));
	}
	else
	{
		/* second and subsequent calls */
		elog(ERROR, "init_MultiFuncCall cannot be called more than once");

		/* never reached, but keep compiler happy */
		retval = NULL;
	}

	return retval;
}

/*
 * per_MultiFuncCall
 *
 * Do Multi-function per-call setup
 */
FuncCallContext *
per_MultiFuncCall(PG_FUNCTION_ARGS)
{
	FuncCallContext *retval = (FuncCallContext *) fcinfo->flinfo->fn_extra;

	return retval;
}

/*
 * end_MultiFuncCall
 * Clean up after init_MultiFuncCall
 */
void
end_MultiFuncCall(PG_FUNCTION_ARGS, FuncCallContext *funcctx)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	/* Deregister the shutdown callback */
	UnregisterExprContextCallback(rsi->econtext,
								  shutdown_MultiFuncCall,
								  PointerGetDatum(fcinfo->flinfo));

	/* But use it to do the real work */
	shutdown_MultiFuncCall(PointerGetDatum(fcinfo->flinfo));
}

/*
 * shutdown_MultiFuncCall
 * Shutdown function to clean up after init_MultiFuncCall
 */
static void
shutdown_MultiFuncCall(Datum arg)
{
	FmgrInfo   *flinfo = (FmgrInfo *) DatumGetPointer(arg);
	FuncCallContext *funcctx = (FuncCallContext *) flinfo->fn_extra;

	/* unbind from flinfo */
	flinfo->fn_extra = NULL;

	/*
	 * Delete context that holds all multi-call data, including the
	 * FuncCallContext itself
	 */
	MemoryContextDelete(funcctx->multi_call_memory_ctx);
}


/*
 * get_call_result_type
 *		Given a function's call info record, determine the kind of datatype
 *		it is supposed to return.  If resultTypeId isn't NULL, *resultTypeId
 *		receives the actual datatype OID (this is mainly useful for scalar
 *		result types).  If resultTupleDesc isn't NULL, *resultTupleDesc
 *		receives a pointer to a TupleDesc when the result is of a composite
 *		type, or NULL when it's a scalar result.
 *
 * One hard case that this handles is resolution of actual rowtypes for
 * functions returning RECORD (from either the function's OUT parameter
 * list, or a ReturnSetInfo context node).  TYPEFUNC_RECORD is returned
 * only when we couldn't resolve the actual rowtype for lack of information.
 *
 * The other hard case that this handles is resolution of polymorphism.
 * We will never return polymorphic pseudotypes (ANYELEMENT etc), either
 * as a scalar result type or as a component of a rowtype.
 *
 * This function is relatively expensive --- in a function returning set,
 * try to call it only the first time through.
 */
TypeFuncClass
get_call_result_type(FunctionCallInfo fcinfo,
					 Oid *resultTypeId,
					 TupleDesc *resultTupleDesc)
{
	return internal_get_result_type(fcinfo->flinfo->fn_oid,
									fcinfo->flinfo->fn_expr,
									(ReturnSetInfo *) fcinfo->resultinfo,
									resultTypeId,
									resultTupleDesc);
}

/*
 * get_expr_result_type
 *		As above, but work from a calling expression node tree
 */
TypeFuncClass
get_expr_result_type(Node *expr,
					 Oid *resultTypeId,
					 TupleDesc *resultTupleDesc)
{
	TypeFuncClass result;

	if (expr && IsA(expr, FuncExpr))
		result = internal_get_result_type(((FuncExpr *) expr)->funcid,
										  expr,
										  NULL,
										  resultTypeId,
										  resultTupleDesc);
	else if (expr && IsA(expr, OpExpr))
		result = internal_get_result_type(get_opcode(((OpExpr *) expr)->opno),
										  expr,
										  NULL,
										  resultTypeId,
										  resultTupleDesc);
	else if (expr && IsA(expr, RowExpr) &&
			 ((RowExpr *) expr)->row_typeid == RECORDOID)
	{
		/* We can resolve the record type by generating the tupdesc directly */
		RowExpr    *rexpr = (RowExpr *) expr;
		TupleDesc	tupdesc;
		AttrNumber	i = 1;
		ListCell   *lcc,
				   *lcn;

		tupdesc = CreateTemplateTupleDesc(list_length(rexpr->args));
		Assert(list_length(rexpr->args) == list_length(rexpr->colnames));
		forboth(lcc, rexpr->args, lcn, rexpr->colnames)
		{
			Node	   *col = (Node *) lfirst(lcc);
			char	   *colname = strVal(lfirst(lcn));

			TupleDescInitEntry(tupdesc, i,
							   colname,
							   exprType(col),
							   exprTypmod(col),
							   0);
			TupleDescInitEntryCollation(tupdesc, i,
										exprCollation(col));
			i++;
		}
		if (resultTypeId)
			*resultTypeId = rexpr->row_typeid;
		if (resultTupleDesc)
			*resultTupleDesc = BlessTupleDesc(tupdesc);
		return TYPEFUNC_COMPOSITE;
	}
	else
	{
		/* handle as a generic expression; no chance to resolve RECORD */
		Oid			typid = exprType(expr);
		Oid			base_typid;

		if (resultTypeId)
			*resultTypeId = typid;
		if (resultTupleDesc)
			*resultTupleDesc = NULL;
		result = get_type_func_class(typid, &base_typid);
		if ((result == TYPEFUNC_COMPOSITE ||
			 result == TYPEFUNC_COMPOSITE_DOMAIN) &&
			resultTupleDesc)
			*resultTupleDesc = lookup_rowtype_tupdesc_copy(base_typid, -1);
	}

	return result;
}

/*
 * get_func_result_type
 *		As above, but work from a function's OID only
 *
 * This will not be able to resolve pure-RECORD results nor polymorphism.
 */
TypeFuncClass
get_func_result_type(Oid functionId,
					 Oid *resultTypeId,
					 TupleDesc *resultTupleDesc)
{
	return internal_get_result_type(functionId,
									NULL,
									NULL,
									resultTypeId,
									resultTupleDesc);
}

/*
 * internal_get_result_type -- workhorse code implementing all the above
 *
 * funcid must always be supplied.  call_expr and rsinfo can be NULL if not
 * available.  We will return TYPEFUNC_RECORD, and store NULL into
 * *resultTupleDesc, if we cannot deduce the complete result rowtype from
 * the available information.
 */
static TypeFuncClass
internal_get_result_type(Oid funcid,
						 Node *call_expr,
						 ReturnSetInfo *rsinfo,
						 Oid *resultTypeId,
						 TupleDesc *resultTupleDesc)
{
	TypeFuncClass result;
	HeapTuple	tp;
	Form_pg_proc procform;
	Oid			rettype;
	Oid			base_rettype;
	TupleDesc	tupdesc;

	/* First fetch the function's pg_proc row to inspect its rettype */
	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(tp);

	rettype = procform->prorettype;

	/* Check for OUT parameters defining a RECORD result */
	tupdesc = build_function_result_tupdesc_t(tp);
	if (tupdesc)
	{
		/*
		 * It has OUT parameters, so it's basically like a regular composite
		 * type, except we have to be able to resolve any polymorphic OUT
		 * parameters.
		 */
		if (resultTypeId)
			*resultTypeId = rettype;

		if (resolve_polymorphic_tupdesc(tupdesc,
										&procform->proargtypes,
										call_expr))
		{
			if (tupdesc->tdtypeid == RECORDOID &&
				tupdesc->tdtypmod < 0)
				assign_record_type_typmod(tupdesc);
			if (resultTupleDesc)
				*resultTupleDesc = tupdesc;
			result = TYPEFUNC_COMPOSITE;
		}
		else
		{
			if (resultTupleDesc)
				*resultTupleDesc = NULL;
			result = TYPEFUNC_RECORD;
		}

		ReleaseSysCache(tp);

		return result;
	}

	/*
	 * If scalar polymorphic result, try to resolve it.
	 */
	if (IsPolymorphicType(rettype))
	{
		Oid			newrettype = exprType(call_expr);

		if (newrettype == InvalidOid)	/* this probably should not happen */
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("could not determine actual result type for function \"%s\" declared to return type %s",
							NameStr(procform->proname),
							format_type_be(rettype))));
		rettype = newrettype;
	}

	if (resultTypeId)
		*resultTypeId = rettype;
	if (resultTupleDesc)
		*resultTupleDesc = NULL;	/* default result */

	/* Classify the result type */
	result = get_type_func_class(rettype, &base_rettype);
	switch (result)
	{
		case TYPEFUNC_COMPOSITE:
		case TYPEFUNC_COMPOSITE_DOMAIN:
			if (resultTupleDesc)
				*resultTupleDesc = lookup_rowtype_tupdesc_copy(base_rettype, -1);
			/* Named composite types can't have any polymorphic columns */
			break;
		case TYPEFUNC_SCALAR:
			break;
		case TYPEFUNC_RECORD:
			/* We must get the tupledesc from call context */
			if (rsinfo && IsA(rsinfo, ReturnSetInfo) &&
				rsinfo->expectedDesc != NULL)
			{
				result = TYPEFUNC_COMPOSITE;
				if (resultTupleDesc)
					*resultTupleDesc = rsinfo->expectedDesc;
				/* Assume no polymorphic columns here, either */
			}
			break;
		default:
			break;
	}

	ReleaseSysCache(tp);

	return result;
}

/*
 * get_expr_result_tupdesc
 *		Get a tupdesc describing the result of a composite-valued expression
 *
 * If expression is not composite or rowtype can't be determined, returns NULL
 * if noError is true, else throws error.
 *
 * This is a simpler version of get_expr_result_type() for use when the caller
 * is only interested in determinate rowtype results.
 */
TupleDesc
get_expr_result_tupdesc(Node *expr, bool noError)
{
	TupleDesc	tupleDesc;
	TypeFuncClass functypclass;

	functypclass = get_expr_result_type(expr, NULL, &tupleDesc);

	if (functypclass == TYPEFUNC_COMPOSITE ||
		functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
		return tupleDesc;

	if (!noError)
	{
		Oid			exprTypeId = exprType(expr);

		if (exprTypeId != RECORDOID)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(exprTypeId))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("record type has not been registered")));
	}

	return NULL;
}

/*
 * Resolve actual type of ANYELEMENT from other polymorphic inputs
 *
 * Note: the error cases here and in the sibling functions below are not
 * really user-facing; they could only occur if the function signature is
 * incorrect or the parser failed to enforce consistency of the actual
 * argument types.  Hence, we don't sweat too much over the error messages.
 */
static void
resolve_anyelement_from_others(polymorphic_actuals *actuals)
{
	if (OidIsValid(actuals->anyarray_type))
	{
		/* Use the element type corresponding to actual type */
		Oid			array_base_type = getBaseType(actuals->anyarray_type);
		Oid			array_typelem = get_element_type(array_base_type);

		if (!OidIsValid(array_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared %s is not an array but type %s",
							"anyarray",
							format_type_be(array_base_type))));
		actuals->anyelement_type = array_typelem;
	}
	else if (OidIsValid(actuals->anyrange_type))
	{
		/* Use the element type corresponding to actual type */
		Oid			range_base_type = getBaseType(actuals->anyrange_type);
		Oid			range_typelem = get_range_subtype(range_base_type);

		if (!OidIsValid(range_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared %s is not a range type but type %s",
							"anyrange",
							format_type_be(range_base_type))));
		actuals->anyelement_type = range_typelem;
	}
	else if (OidIsValid(actuals->anymultirange_type))
	{
		/* Use the element type based on the multirange type */
		Oid			multirange_base_type;
		Oid			multirange_typelem;
		Oid			range_base_type;
		Oid			range_typelem;

		multirange_base_type = getBaseType(actuals->anymultirange_type);
		multirange_typelem = get_multirange_range(multirange_base_type);
		if (!OidIsValid(multirange_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared %s is not a multirange type but type %s",
							"anymultirange",
							format_type_be(multirange_base_type))));

		range_base_type = getBaseType(multirange_typelem);
		range_typelem = get_range_subtype(range_base_type);

		if (!OidIsValid(range_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared %s does not contain a range type but type %s",
							"anymultirange",
							format_type_be(range_base_type))));
		actuals->anyelement_type = range_typelem;
	}
	else
		elog(ERROR, "could not determine polymorphic type");
}

/*
 * Resolve actual type of ANYARRAY from other polymorphic inputs
 */
static void
resolve_anyarray_from_others(polymorphic_actuals *actuals)
{
	/* If we don't know ANYELEMENT, resolve that first */
	if (!OidIsValid(actuals->anyelement_type))
		resolve_anyelement_from_others(actuals);

	if (OidIsValid(actuals->anyelement_type))
	{
		/* Use the array type corresponding to actual type */
		Oid			array_typeid = get_array_type(actuals->anyelement_type);

		if (!OidIsValid(array_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("could not find array type for data type %s",
							format_type_be(actuals->anyelement_type))));
		actuals->anyarray_type = array_typeid;
	}
	else
		elog(ERROR, "could not determine polymorphic type");
}

/*
 * Resolve actual type of ANYRANGE from other polymorphic inputs
 */
static void
resolve_anyrange_from_others(polymorphic_actuals *actuals)
{
	/*
	 * We can't deduce a range type from other polymorphic array or base
	 * types, because there may be multiple range types with the same subtype,
	 * but we can deduce it from a polymorphic multirange type.
	 */
	if (OidIsValid(actuals->anymultirange_type))
	{
		/* Use the element type based on the multirange type */
		Oid			multirange_base_type = getBaseType(actuals->anymultirange_type);
		Oid			multirange_typelem = get_multirange_range(multirange_base_type);

		if (!OidIsValid(multirange_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared %s is not a multirange type but type %s",
							"anymultirange",
							format_type_be(multirange_base_type))));
		actuals->anyrange_type = multirange_typelem;
	}
	else
		elog(ERROR, "could not determine polymorphic type");
}

/*
 * Resolve actual type of ANYMULTIRANGE from other polymorphic inputs
 */
static void
resolve_anymultirange_from_others(polymorphic_actuals *actuals)
{
	/*
	 * We can't deduce a multirange type from polymorphic array or base types,
	 * because there may be multiple range types with the same subtype, but we
	 * can deduce it from a polymorphic range type.
	 */
	if (OidIsValid(actuals->anyrange_type))
	{
		Oid			range_base_type = getBaseType(actuals->anyrange_type);
		Oid			multirange_typeid = get_range_multirange(range_base_type);

		if (!OidIsValid(multirange_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("could not find multirange type for data type %s",
							format_type_be(actuals->anyrange_type))));
		actuals->anymultirange_type = multirange_typeid;
	}
	else
		elog(ERROR, "could not determine polymorphic type");
}

/*
 * Given the result tuple descriptor for a function with OUT parameters,
 * replace any polymorphic column types (ANYELEMENT etc) in the tupdesc
 * with concrete data types deduced from the input arguments.
 * declared_args is an oidvector of the function's declared input arg types
 * (showing which are polymorphic), and call_expr is the call expression.
 *
 * Returns true if able to deduce all types, false if necessary information
 * is not provided (call_expr is NULL or arg types aren't identifiable).
 */
static bool
resolve_polymorphic_tupdesc(TupleDesc tupdesc, oidvector *declared_args,
							Node *call_expr)
{
	int			natts = tupdesc->natts;
	int			nargs = declared_args->dim1;
	bool		have_polymorphic_result = false;
	bool		have_anyelement_result = false;
	bool		have_anyarray_result = false;
	bool		have_anyrange_result = false;
	bool		have_anymultirange_result = false;
	bool		have_anycompatible_result = false;
	bool		have_anycompatible_array_result = false;
	bool		have_anycompatible_range_result = false;
	bool		have_anycompatible_multirange_result = false;
	polymorphic_actuals poly_actuals;
	polymorphic_actuals anyc_actuals;
	Oid			anycollation = InvalidOid;
	Oid			anycompatcollation = InvalidOid;
	int			i;

	/* See if there are any polymorphic outputs; quick out if not */
	for (i = 0; i < natts; i++)
	{
		switch (TupleDescAttr(tupdesc, i)->atttypid)
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				have_polymorphic_result = true;
				have_anyelement_result = true;
				break;
			case ANYARRAYOID:
				have_polymorphic_result = true;
				have_anyarray_result = true;
				break;
			case ANYRANGEOID:
				have_polymorphic_result = true;
				have_anyrange_result = true;
				break;
			case ANYMULTIRANGEOID:
				have_polymorphic_result = true;
				have_anymultirange_result = true;
				break;
			case ANYCOMPATIBLEOID:
			case ANYCOMPATIBLENONARRAYOID:
				have_polymorphic_result = true;
				have_anycompatible_result = true;
				break;
			case ANYCOMPATIBLEARRAYOID:
				have_polymorphic_result = true;
				have_anycompatible_array_result = true;
				break;
			case ANYCOMPATIBLERANGEOID:
				have_polymorphic_result = true;
				have_anycompatible_range_result = true;
				break;
			case ANYCOMPATIBLEMULTIRANGEOID:
				have_polymorphic_result = true;
				have_anycompatible_multirange_result = true;
				break;
			default:
				break;
		}
	}
	if (!have_polymorphic_result)
		return true;

	/*
	 * Otherwise, extract actual datatype(s) from input arguments.  (We assume
	 * the parser already validated consistency of the arguments.  Also, for
	 * the ANYCOMPATIBLE pseudotype family, we expect that all matching
	 * arguments were coerced to the selected common supertype, so that it
	 * doesn't matter which one's exposed type we look at.)
	 */
	if (!call_expr)
		return false;			/* no hope */

	memset(&poly_actuals, 0, sizeof(poly_actuals));
	memset(&anyc_actuals, 0, sizeof(anyc_actuals));

	for (i = 0; i < nargs; i++)
	{
		switch (declared_args->values[i])
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				if (!OidIsValid(poly_actuals.anyelement_type))
				{
					poly_actuals.anyelement_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(poly_actuals.anyelement_type))
						return false;
				}
				break;
			case ANYARRAYOID:
				if (!OidIsValid(poly_actuals.anyarray_type))
				{
					poly_actuals.anyarray_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(poly_actuals.anyarray_type))
						return false;
				}
				break;
			case ANYRANGEOID:
				if (!OidIsValid(poly_actuals.anyrange_type))
				{
					poly_actuals.anyrange_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(poly_actuals.anyrange_type))
						return false;
				}
				break;
			case ANYMULTIRANGEOID:
				if (!OidIsValid(poly_actuals.anymultirange_type))
				{
					poly_actuals.anymultirange_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(poly_actuals.anymultirange_type))
						return false;
				}
				break;
			case ANYCOMPATIBLEOID:
			case ANYCOMPATIBLENONARRAYOID:
				if (!OidIsValid(anyc_actuals.anyelement_type))
				{
					anyc_actuals.anyelement_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(anyc_actuals.anyelement_type))
						return false;
				}
				break;
			case ANYCOMPATIBLEARRAYOID:
				if (!OidIsValid(anyc_actuals.anyarray_type))
				{
					anyc_actuals.anyarray_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(anyc_actuals.anyarray_type))
						return false;
				}
				break;
			case ANYCOMPATIBLERANGEOID:
				if (!OidIsValid(anyc_actuals.anyrange_type))
				{
					anyc_actuals.anyrange_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(anyc_actuals.anyrange_type))
						return false;
				}
				break;
			case ANYCOMPATIBLEMULTIRANGEOID:
				if (!OidIsValid(anyc_actuals.anymultirange_type))
				{
					anyc_actuals.anymultirange_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(anyc_actuals.anymultirange_type))
						return false;
				}
				break;
			default:
				break;
		}
	}

	/* If needed, deduce one polymorphic type from others */
	if (have_anyelement_result && !OidIsValid(poly_actuals.anyelement_type))
		resolve_anyelement_from_others(&poly_actuals);

	if (have_anyarray_result && !OidIsValid(poly_actuals.anyarray_type))
		resolve_anyarray_from_others(&poly_actuals);

	if (have_anyrange_result && !OidIsValid(poly_actuals.anyrange_type))
		resolve_anyrange_from_others(&poly_actuals);

	if (have_anymultirange_result && !OidIsValid(poly_actuals.anymultirange_type))
		resolve_anymultirange_from_others(&poly_actuals);

	if (have_anycompatible_result && !OidIsValid(anyc_actuals.anyelement_type))
		resolve_anyelement_from_others(&anyc_actuals);

	if (have_anycompatible_array_result && !OidIsValid(anyc_actuals.anyarray_type))
		resolve_anyarray_from_others(&anyc_actuals);

	if (have_anycompatible_range_result && !OidIsValid(anyc_actuals.anyrange_type))
		resolve_anyrange_from_others(&anyc_actuals);

	if (have_anycompatible_multirange_result && !OidIsValid(anyc_actuals.anymultirange_type))
		resolve_anymultirange_from_others(&anyc_actuals);

	/*
	 * Identify the collation to use for polymorphic OUT parameters. (It'll
	 * necessarily be the same for both anyelement and anyarray, likewise for
	 * anycompatible and anycompatiblearray.)  Note that range types are not
	 * collatable, so any possible internal collation of a range type is not
	 * considered here.
	 */
	if (OidIsValid(poly_actuals.anyelement_type))
		anycollation = get_typcollation(poly_actuals.anyelement_type);
	else if (OidIsValid(poly_actuals.anyarray_type))
		anycollation = get_typcollation(poly_actuals.anyarray_type);

	if (OidIsValid(anyc_actuals.anyelement_type))
		anycompatcollation = get_typcollation(anyc_actuals.anyelement_type);
	else if (OidIsValid(anyc_actuals.anyarray_type))
		anycompatcollation = get_typcollation(anyc_actuals.anyarray_type);

	if (OidIsValid(anycollation) || OidIsValid(anycompatcollation))
	{
		/*
		 * The types are collatable, so consider whether to use a nondefault
		 * collation.  We do so if we can identify the input collation used
		 * for the function.
		 */
		Oid			inputcollation = exprInputCollation(call_expr);

		if (OidIsValid(inputcollation))
		{
			if (OidIsValid(anycollation))
				anycollation = inputcollation;
			if (OidIsValid(anycompatcollation))
				anycompatcollation = inputcollation;
		}
	}

	/* And finally replace the tuple column types as needed */
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		switch (att->atttypid)
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   poly_actuals.anyelement_type,
								   -1,
								   0);
				TupleDescInitEntryCollation(tupdesc, i + 1, anycollation);
				break;
			case ANYARRAYOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   poly_actuals.anyarray_type,
								   -1,
								   0);
				TupleDescInitEntryCollation(tupdesc, i + 1, anycollation);
				break;
			case ANYRANGEOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   poly_actuals.anyrange_type,
								   -1,
								   0);
				/* no collation should be attached to a range type */
				break;
			case ANYMULTIRANGEOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   poly_actuals.anymultirange_type,
								   -1,
								   0);
				/* no collation should be attached to a multirange type */
				break;
			case ANYCOMPATIBLEOID:
			case ANYCOMPATIBLENONARRAYOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   anyc_actuals.anyelement_type,
								   -1,
								   0);
				TupleDescInitEntryCollation(tupdesc, i + 1, anycompatcollation);
				break;
			case ANYCOMPATIBLEARRAYOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   anyc_actuals.anyarray_type,
								   -1,
								   0);
				TupleDescInitEntryCollation(tupdesc, i + 1, anycompatcollation);
				break;
			case ANYCOMPATIBLERANGEOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   anyc_actuals.anyrange_type,
								   -1,
								   0);
				/* no collation should be attached to a range type */
				break;
			case ANYCOMPATIBLEMULTIRANGEOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   anyc_actuals.anymultirange_type,
								   -1,
								   0);
				/* no collation should be attached to a multirange type */
				break;
			default:
				break;
		}
	}

	return true;
}

/*
 * Given the declared argument types and modes for a function, replace any
 * polymorphic types (ANYELEMENT etc) in argtypes[] with concrete data types
 * deduced from the input arguments found in call_expr.
 *
 * Returns true if able to deduce all types, false if necessary information
 * is not provided (call_expr is NULL or arg types aren't identifiable).
 *
 * This is the same logic as resolve_polymorphic_tupdesc, but with a different
 * argument representation, and slightly different output responsibilities.
 *
 * argmodes may be NULL, in which case all arguments are assumed to be IN mode.
 */
bool
resolve_polymorphic_argtypes(int numargs, Oid *argtypes, char *argmodes,
							 Node *call_expr)
{
	bool		have_polymorphic_result = false;
	bool		have_anyelement_result = false;
	bool		have_anyarray_result = false;
	bool		have_anyrange_result = false;
	bool		have_anymultirange_result = false;
	bool		have_anycompatible_result = false;
	bool		have_anycompatible_array_result = false;
	bool		have_anycompatible_range_result = false;
	bool		have_anycompatible_multirange_result = false;
	polymorphic_actuals poly_actuals;
	polymorphic_actuals anyc_actuals;
	int			inargno;
	int			i;

	/*
	 * First pass: resolve polymorphic inputs, check for outputs.  As in
	 * resolve_polymorphic_tupdesc, we rely on the parser to have enforced
	 * type consistency and coerced ANYCOMPATIBLE args to a common supertype.
	 */
	memset(&poly_actuals, 0, sizeof(poly_actuals));
	memset(&anyc_actuals, 0, sizeof(anyc_actuals));
	inargno = 0;
	for (i = 0; i < numargs; i++)
	{
		char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

		switch (argtypes[i])
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anyelement_result = true;
				}
				else
				{
					if (!OidIsValid(poly_actuals.anyelement_type))
					{
						poly_actuals.anyelement_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(poly_actuals.anyelement_type))
							return false;
					}
					argtypes[i] = poly_actuals.anyelement_type;
				}
				break;
			case ANYARRAYOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anyarray_result = true;
				}
				else
				{
					if (!OidIsValid(poly_actuals.anyarray_type))
					{
						poly_actuals.anyarray_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(poly_actuals.anyarray_type))
							return false;
					}
					argtypes[i] = poly_actuals.anyarray_type;
				}
				break;
			case ANYRANGEOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anyrange_result = true;
				}
				else
				{
					if (!OidIsValid(poly_actuals.anyrange_type))
					{
						poly_actuals.anyrange_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(poly_actuals.anyrange_type))
							return false;
					}
					argtypes[i] = poly_actuals.anyrange_type;
				}
				break;
			case ANYMULTIRANGEOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anymultirange_result = true;
				}
				else
				{
					if (!OidIsValid(poly_actuals.anymultirange_type))
					{
						poly_actuals.anymultirange_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(poly_actuals.anymultirange_type))
							return false;
					}
					argtypes[i] = poly_actuals.anymultirange_type;
				}
				break;
			case ANYCOMPATIBLEOID:
			case ANYCOMPATIBLENONARRAYOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anycompatible_result = true;
				}
				else
				{
					if (!OidIsValid(anyc_actuals.anyelement_type))
					{
						anyc_actuals.anyelement_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(anyc_actuals.anyelement_type))
							return false;
					}
					argtypes[i] = anyc_actuals.anyelement_type;
				}
				break;
			case ANYCOMPATIBLEARRAYOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anycompatible_array_result = true;
				}
				else
				{
					if (!OidIsValid(anyc_actuals.anyarray_type))
					{
						anyc_actuals.anyarray_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(anyc_actuals.anyarray_type))
							return false;
					}
					argtypes[i] = anyc_actuals.anyarray_type;
				}
				break;
			case ANYCOMPATIBLERANGEOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anycompatible_range_result = true;
				}
				else
				{
					if (!OidIsValid(anyc_actuals.anyrange_type))
					{
						anyc_actuals.anyrange_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(anyc_actuals.anyrange_type))
							return false;
					}
					argtypes[i] = anyc_actuals.anyrange_type;
				}
				break;
			case ANYCOMPATIBLEMULTIRANGEOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anycompatible_multirange_result = true;
				}
				else
				{
					if (!OidIsValid(anyc_actuals.anymultirange_type))
					{
						anyc_actuals.anymultirange_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(anyc_actuals.anymultirange_type))
							return false;
					}
					argtypes[i] = anyc_actuals.anymultirange_type;
				}
				break;
			default:
				break;
		}
		if (argmode != PROARGMODE_OUT && argmode != PROARGMODE_TABLE)
			inargno++;
	}

	/* Done? */
	if (!have_polymorphic_result)
		return true;

	/* If needed, deduce one polymorphic type from others */
	if (have_anyelement_result && !OidIsValid(poly_actuals.anyelement_type))
		resolve_anyelement_from_others(&poly_actuals);

	if (have_anyarray_result && !OidIsValid(poly_actuals.anyarray_type))
		resolve_anyarray_from_others(&poly_actuals);

	if (have_anyrange_result && !OidIsValid(poly_actuals.anyrange_type))
		resolve_anyrange_from_others(&poly_actuals);

	if (have_anymultirange_result && !OidIsValid(poly_actuals.anymultirange_type))
		resolve_anymultirange_from_others(&poly_actuals);

	if (have_anycompatible_result && !OidIsValid(anyc_actuals.anyelement_type))
		resolve_anyelement_from_others(&anyc_actuals);

	if (have_anycompatible_array_result && !OidIsValid(anyc_actuals.anyarray_type))
		resolve_anyarray_from_others(&anyc_actuals);

	if (have_anycompatible_range_result && !OidIsValid(anyc_actuals.anyrange_type))
		resolve_anyrange_from_others(&anyc_actuals);

	if (have_anycompatible_multirange_result && !OidIsValid(anyc_actuals.anymultirange_type))
		resolve_anymultirange_from_others(&anyc_actuals);

	/* And finally replace the output column types as needed */
	for (i = 0; i < numargs; i++)
	{
		switch (argtypes[i])
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				argtypes[i] = poly_actuals.anyelement_type;
				break;
			case ANYARRAYOID:
				argtypes[i] = poly_actuals.anyarray_type;
				break;
			case ANYRANGEOID:
				argtypes[i] = poly_actuals.anyrange_type;
				break;
			case ANYMULTIRANGEOID:
				argtypes[i] = poly_actuals.anymultirange_type;
				break;
			case ANYCOMPATIBLEOID:
			case ANYCOMPATIBLENONARRAYOID:
				argtypes[i] = anyc_actuals.anyelement_type;
				break;
			case ANYCOMPATIBLEARRAYOID:
				argtypes[i] = anyc_actuals.anyarray_type;
				break;
			case ANYCOMPATIBLERANGEOID:
				argtypes[i] = anyc_actuals.anyrange_type;
				break;
			case ANYCOMPATIBLEMULTIRANGEOID:
				argtypes[i] = anyc_actuals.anymultirange_type;
				break;
			default:
				break;
		}
	}

	return true;
}

/*
 * get_type_func_class
 *		Given the type OID, obtain its TYPEFUNC classification.
 *		Also, if it's a domain, return the base type OID.
 *
 * This is intended to centralize a bunch of formerly ad-hoc code for
 * classifying types.  The categories used here are useful for deciding
 * how to handle functions returning the datatype.
 */
static TypeFuncClass
get_type_func_class(Oid typid, Oid *base_typeid)
{
	*base_typeid = typid;

	switch (get_typtype(typid))
	{
		case TYPTYPE_COMPOSITE:
			return TYPEFUNC_COMPOSITE;
		case TYPTYPE_BASE:
		case TYPTYPE_ENUM:
		case TYPTYPE_RANGE:
		case TYPTYPE_MULTIRANGE:
			return TYPEFUNC_SCALAR;
		case TYPTYPE_DOMAIN:
			*base_typeid = typid = getBaseType(typid);
			if (get_typtype(typid) == TYPTYPE_COMPOSITE)
				return TYPEFUNC_COMPOSITE_DOMAIN;
			else				/* domain base type can't be a pseudotype */
				return TYPEFUNC_SCALAR;
		case TYPTYPE_PSEUDO:
			if (typid == RECORDOID)
				return TYPEFUNC_RECORD;

			/*
			 * We treat VOID and CSTRING as legitimate scalar datatypes,
			 * mostly for the convenience of the JDBC driver (which wants to
			 * be able to do "SELECT * FROM foo()" for all legitimately
			 * user-callable functions).
			 */
			if (typid == VOIDOID || typid == CSTRINGOID)
				return TYPEFUNC_SCALAR;
			return TYPEFUNC_OTHER;
	}
	/* shouldn't get here, probably */
	return TYPEFUNC_OTHER;
}


/*
 * get_func_arg_info
 *
 * Fetch info about the argument types, names, and IN/OUT modes from the
 * pg_proc tuple.  Return value is the total number of arguments.
 * Other results are palloc'd.  *p_argtypes is always filled in, but
 * *p_argnames and *p_argmodes will be set NULL in the default cases
 * (no names, and all IN arguments, respectively).
 *
 * Note that this function simply fetches what is in the pg_proc tuple;
 * it doesn't do any interpretation of polymorphic types.
 */
int
get_func_arg_info(HeapTuple procTup,
				  Oid **p_argtypes, char ***p_argnames, char **p_argmodes)
{
	Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	Datum		proallargtypes;
	Datum		proargmodes;
	Datum		proargnames;
	bool		isNull;
	ArrayType  *arr;
	int			numargs;
	Datum	   *elems;
	int			nelems;
	int			i;

	/* First discover the total number of parameters and get their types */
	proallargtypes = SysCacheGetAttr(PROCOID, procTup,
									 Anum_pg_proc_proallargtypes,
									 &isNull);
	if (!isNull)
	{
		/*
		 * We expect the arrays to be 1-D arrays of the right types; verify
		 * that.  For the OID and char arrays, we don't need to use
		 * deconstruct_array() since the array data is just going to look like
		 * a C array of values.
		 */
		arr = DatumGetArrayTypeP(proallargtypes);	/* ensure not toasted */
		numargs = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			numargs < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "proallargtypes is not a 1-D Oid array or it contains nulls");
		Assert(numargs >= procStruct->pronargs);
		*p_argtypes = (Oid *) palloc(numargs * sizeof(Oid));
		memcpy(*p_argtypes, ARR_DATA_PTR(arr),
			   numargs * sizeof(Oid));
	}
	else
	{
		/* If no proallargtypes, use proargtypes */
		numargs = procStruct->proargtypes.dim1;
		Assert(numargs == procStruct->pronargs);
		*p_argtypes = (Oid *) palloc(numargs * sizeof(Oid));
		memcpy(*p_argtypes, procStruct->proargtypes.values,
			   numargs * sizeof(Oid));
	}

	/* Get argument names, if available */
	proargnames = SysCacheGetAttr(PROCOID, procTup,
								  Anum_pg_proc_proargnames,
								  &isNull);
	if (isNull)
		*p_argnames = NULL;
	else
	{
		deconstruct_array(DatumGetArrayTypeP(proargnames),
						  TEXTOID, -1, false, TYPALIGN_INT,
						  &elems, NULL, &nelems);
		if (nelems != numargs)	/* should not happen */
			elog(ERROR, "proargnames must have the same number of elements as the function has arguments");
		*p_argnames = (char **) palloc(sizeof(char *) * numargs);
		for (i = 0; i < numargs; i++)
			(*p_argnames)[i] = TextDatumGetCString(elems[i]);
	}

	/* Get argument modes, if available */
	proargmodes = SysCacheGetAttr(PROCOID, procTup,
								  Anum_pg_proc_proargmodes,
								  &isNull);
	if (isNull)
		*p_argmodes = NULL;
	else
	{
		arr = DatumGetArrayTypeP(proargmodes);	/* ensure not toasted */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array of length %d or it contains nulls",
				 numargs);
		*p_argmodes = (char *) palloc(numargs * sizeof(char));
		memcpy(*p_argmodes, ARR_DATA_PTR(arr),
			   numargs * sizeof(char));
	}

	return numargs;
}

/*
 * get_func_trftypes
 *
 * Returns the number of transformed types used by the function.
 * If there are any, a palloc'd array of the type OIDs is returned
 * into *p_trftypes.
 */
int
get_func_trftypes(HeapTuple procTup,
				  Oid **p_trftypes)
{
	Datum		protrftypes;
	ArrayType  *arr;
	int			nelems;
	bool		isNull;

	protrftypes = SysCacheGetAttr(PROCOID, procTup,
								  Anum_pg_proc_protrftypes,
								  &isNull);
	if (!isNull)
	{
		/*
		 * We expect the arrays to be 1-D arrays of the right types; verify
		 * that.  For the OID and char arrays, we don't need to use
		 * deconstruct_array() since the array data is just going to look like
		 * a C array of values.
		 */
		arr = DatumGetArrayTypeP(protrftypes);	/* ensure not toasted */
		nelems = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			nelems < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "protrftypes is not a 1-D Oid array or it contains nulls");
		*p_trftypes = (Oid *) palloc(nelems * sizeof(Oid));
		memcpy(*p_trftypes, ARR_DATA_PTR(arr),
			   nelems * sizeof(Oid));

		return nelems;
	}
	else
		return 0;
}

/*
 * get_func_input_arg_names
 *
 * Extract the names of input arguments only, given a function's
 * proargnames and proargmodes entries in Datum form.
 *
 * Returns the number of input arguments, which is the length of the
 * palloc'd array returned to *arg_names.  Entries for unnamed args
 * are set to NULL.  You don't get anything if proargnames is NULL.
 */
int
get_func_input_arg_names(Datum proargnames, Datum proargmodes,
						 char ***arg_names)
{
	ArrayType  *arr;
	int			numargs;
	Datum	   *argnames;
	char	   *argmodes;
	char	  **inargnames;
	int			numinargs;
	int			i;

	/* Do nothing if null proargnames */
	if (proargnames == PointerGetDatum(NULL))
	{
		*arg_names = NULL;
		return 0;
	}

	/*
	 * We expect the arrays to be 1-D arrays of the right types; verify that.
	 * For proargmodes, we don't need to use deconstruct_array() since the
	 * array data is just going to look like a C array of values.
	 */
	arr = DatumGetArrayTypeP(proargnames);	/* ensure not toasted */
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != TEXTOID)
		elog(ERROR, "proargnames is not a 1-D text array or it contains nulls");
	deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT,
					  &argnames, NULL, &numargs);
	if (proargmodes != PointerGetDatum(NULL))
	{
		arr = DatumGetArrayTypeP(proargmodes);	/* ensure not toasted */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array of length %d or it contains nulls",
				 numargs);
		argmodes = (char *) ARR_DATA_PTR(arr);
	}
	else
		argmodes = NULL;

	/* zero elements probably shouldn't happen, but handle it gracefully */
	if (numargs <= 0)
	{
		*arg_names = NULL;
		return 0;
	}

	/* extract input-argument names */
	inargnames = (char **) palloc(numargs * sizeof(char *));
	numinargs = 0;
	for (i = 0; i < numargs; i++)
	{
		if (argmodes == NULL ||
			argmodes[i] == PROARGMODE_IN ||
			argmodes[i] == PROARGMODE_INOUT ||
			argmodes[i] == PROARGMODE_VARIADIC)
		{
			char	   *pname = TextDatumGetCString(argnames[i]);

			if (pname[0] != '\0')
				inargnames[numinargs] = pname;
			else
				inargnames[numinargs] = NULL;
			numinargs++;
		}
	}

	*arg_names = inargnames;
	return numinargs;
}


/*
 * get_func_result_name
 *
 * If the function has exactly one output parameter, and that parameter
 * is named, return the name (as a palloc'd string).  Else return NULL.
 *
 * This is used to determine the default output column name for functions
 * returning scalar types.
 */
char *
get_func_result_name(Oid functionId)
{
	char	   *result;
	HeapTuple	procTuple;
	Datum		proargmodes;
	Datum		proargnames;
	bool		isnull;
	ArrayType  *arr;
	int			numargs;
	char	   *argmodes;
	Datum	   *argnames;
	int			numoutargs;
	int			nargnames;
	int			i;

	/* First fetch the function's pg_proc row */
	procTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionId));
	if (!HeapTupleIsValid(procTuple))
		elog(ERROR, "cache lookup failed for function %u", functionId);

	/* If there are no named OUT parameters, return NULL */
	if (heap_attisnull(procTuple, Anum_pg_proc_proargmodes, NULL) ||
		heap_attisnull(procTuple, Anum_pg_proc_proargnames, NULL))
		result = NULL;
	else
	{
		/* Get the data out of the tuple */
		proargmodes = SysCacheGetAttr(PROCOID, procTuple,
									  Anum_pg_proc_proargmodes,
									  &isnull);
		Assert(!isnull);
		proargnames = SysCacheGetAttr(PROCOID, procTuple,
									  Anum_pg_proc_proargnames,
									  &isnull);
		Assert(!isnull);

		/*
		 * We expect the arrays to be 1-D arrays of the right types; verify
		 * that.  For the char array, we don't need to use deconstruct_array()
		 * since the array data is just going to look like a C array of
		 * values.
		 */
		arr = DatumGetArrayTypeP(proargmodes);	/* ensure not toasted */
		numargs = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			numargs < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array or it contains nulls");
		argmodes = (char *) ARR_DATA_PTR(arr);
		arr = DatumGetArrayTypeP(proargnames);	/* ensure not toasted */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != TEXTOID)
			elog(ERROR, "proargnames is not a 1-D text array of length %d or it contains nulls",
				 numargs);
		deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT,
						  &argnames, NULL, &nargnames);
		Assert(nargnames == numargs);

		/* scan for output argument(s) */
		result = NULL;
		numoutargs = 0;
		for (i = 0; i < numargs; i++)
		{
			if (argmodes[i] == PROARGMODE_IN ||
				argmodes[i] == PROARGMODE_VARIADIC)
				continue;
			Assert(argmodes[i] == PROARGMODE_OUT ||
				   argmodes[i] == PROARGMODE_INOUT ||
				   argmodes[i] == PROARGMODE_TABLE);
			if (++numoutargs > 1)
			{
				/* multiple out args, so forget it */
				result = NULL;
				break;
			}
			result = TextDatumGetCString(argnames[i]);
			if (result == NULL || result[0] == '\0')
			{
				/* Parameter is not named, so forget it */
				result = NULL;
				break;
			}
		}
	}

	ReleaseSysCache(procTuple);

	return result;
}


/*
 * build_function_result_tupdesc_t
 *
 * Given a pg_proc row for a function, return a tuple descriptor for the
 * result rowtype, or NULL if the function does not have OUT parameters.
 *
 * Note that this does not handle resolution of polymorphic types;
 * that is deliberate.
 */
TupleDesc
build_function_result_tupdesc_t(HeapTuple procTuple)
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(procTuple);
	Datum		proallargtypes;
	Datum		proargmodes;
	Datum		proargnames;
	bool		isnull;

	/* Return NULL if the function isn't declared to return RECORD */
	if (procform->prorettype != RECORDOID)
		return NULL;

	/* If there are no OUT parameters, return NULL */
	if (heap_attisnull(procTuple, Anum_pg_proc_proallargtypes, NULL) ||
		heap_attisnull(procTuple, Anum_pg_proc_proargmodes, NULL))
		return NULL;

	/* Get the data out of the tuple */
	proallargtypes = SysCacheGetAttr(PROCOID, procTuple,
									 Anum_pg_proc_proallargtypes,
									 &isnull);
	Assert(!isnull);
	proargmodes = SysCacheGetAttr(PROCOID, procTuple,
								  Anum_pg_proc_proargmodes,
								  &isnull);
	Assert(!isnull);
	proargnames = SysCacheGetAttr(PROCOID, procTuple,
								  Anum_pg_proc_proargnames,
								  &isnull);
	if (isnull)
		proargnames = PointerGetDatum(NULL);	/* just to be sure */

	return build_function_result_tupdesc_d(procform->prokind,
										   proallargtypes,
										   proargmodes,
										   proargnames);
}

/*
 * build_function_result_tupdesc_d
 *
 * Build a RECORD function's tupledesc from the pg_proc proallargtypes,
 * proargmodes, and proargnames arrays.  This is split out for the
 * convenience of ProcedureCreate, which needs to be able to compute the
 * tupledesc before actually creating the function.
 *
 * For functions (but not for procedures), returns NULL if there are not at
 * least two OUT or INOUT arguments.
 */
TupleDesc
build_function_result_tupdesc_d(char prokind,
								Datum proallargtypes,
								Datum proargmodes,
								Datum proargnames)
{
	TupleDesc	desc;
	ArrayType  *arr;
	int			numargs;
	Oid		   *argtypes;
	char	   *argmodes;
	Datum	   *argnames = NULL;
	Oid		   *outargtypes;
	char	  **outargnames;
	int			numoutargs;
	int			nargnames;
	int			i;

	/* Can't have output args if columns are null */
	if (proallargtypes == PointerGetDatum(NULL) ||
		proargmodes == PointerGetDatum(NULL))
		return NULL;

	/*
	 * We expect the arrays to be 1-D arrays of the right types; verify that.
	 * For the OID and char arrays, we don't need to use deconstruct_array()
	 * since the array data is just going to look like a C array of values.
	 */
	arr = DatumGetArrayTypeP(proallargtypes);	/* ensure not toasted */
	numargs = ARR_DIMS(arr)[0];
	if (ARR_NDIM(arr) != 1 ||
		numargs < 0 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "proallargtypes is not a 1-D Oid array or it contains nulls");
	argtypes = (Oid *) ARR_DATA_PTR(arr);
	arr = DatumGetArrayTypeP(proargmodes);	/* ensure not toasted */
	if (ARR_NDIM(arr) != 1 ||
		ARR_DIMS(arr)[0] != numargs ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != CHAROID)
		elog(ERROR, "proargmodes is not a 1-D char array of length %d or it contains nulls",
			 numargs);
	argmodes = (char *) ARR_DATA_PTR(arr);
	if (proargnames != PointerGetDatum(NULL))
	{
		arr = DatumGetArrayTypeP(proargnames);	/* ensure not toasted */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != TEXTOID)
			elog(ERROR, "proargnames is not a 1-D text array of length %d or it contains nulls",
				 numargs);
		deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT,
						  &argnames, NULL, &nargnames);
		Assert(nargnames == numargs);
	}

	/* zero elements probably shouldn't happen, but handle it gracefully */
	if (numargs <= 0)
		return NULL;

	/* extract output-argument types and names */
	outargtypes = (Oid *) palloc(numargs * sizeof(Oid));
	outargnames = (char **) palloc(numargs * sizeof(char *));
	numoutargs = 0;
	for (i = 0; i < numargs; i++)
	{
		char	   *pname;

		if (argmodes[i] == PROARGMODE_IN ||
			argmodes[i] == PROARGMODE_VARIADIC)
			continue;
		Assert(argmodes[i] == PROARGMODE_OUT ||
			   argmodes[i] == PROARGMODE_INOUT ||
			   argmodes[i] == PROARGMODE_TABLE);
		outargtypes[numoutargs] = argtypes[i];
		if (argnames)
			pname = TextDatumGetCString(argnames[i]);
		else
			pname = NULL;
		if (pname == NULL || pname[0] == '\0')
		{
			/* Parameter is not named, so gin up a column name */
			pname = psprintf("column%d", numoutargs + 1);
		}
		outargnames[numoutargs] = pname;
		numoutargs++;
	}

	/*
	 * If there is no output argument, or only one, the function does not
	 * return tuples.
	 */
	if (numoutargs < 2 && prokind != PROKIND_PROCEDURE)
		return NULL;

	desc = CreateTemplateTupleDesc(numoutargs);
	for (i = 0; i < numoutargs; i++)
	{
		TupleDescInitEntry(desc, i + 1,
						   outargnames[i],
						   outargtypes[i],
						   -1,
						   0);
	}

	return desc;
}


/*
 * RelationNameGetTupleDesc
 *
 * Given a (possibly qualified) relation name, build a TupleDesc.
 *
 * Note: while this works as advertised, it's seldom the best way to
 * build a tupdesc for a function's result type.  It's kept around
 * only for backwards compatibility with existing user-written code.
 */
TupleDesc
RelationNameGetTupleDesc(const char *relname)
{
	RangeVar   *relvar;
	Relation	rel;
	TupleDesc	tupdesc;
	List	   *relname_list;

	/* Open relation and copy the tuple description */
	relname_list = stringToQualifiedNameList(relname);
	relvar = makeRangeVarFromNameList(relname_list);
	rel = relation_openrv(relvar, AccessShareLock);
	tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
	relation_close(rel, AccessShareLock);

	return tupdesc;
}

/*
 * TypeGetTupleDesc
 *
 * Given a type Oid, build a TupleDesc.  (In most cases you should be
 * using get_call_result_type or one of its siblings instead of this
 * routine, so that you can handle OUT parameters, RECORD result type,
 * and polymorphic results.)
 *
 * If the type is composite, *and* a colaliases List is provided, *and*
 * the List is of natts length, use the aliases instead of the relation
 * attnames.  (NB: this usage is deprecated since it may result in
 * creation of unnecessary transient record types.)
 *
 * If the type is a base type, a single item alias List is required.
 */
TupleDesc
TypeGetTupleDesc(Oid typeoid, List *colaliases)
{
	Oid			base_typeoid;
	TypeFuncClass functypclass = get_type_func_class(typeoid, &base_typeoid);
	TupleDesc	tupdesc = NULL;

	/*
	 * Build a suitable tupledesc representing the output rows.  We
	 * intentionally do not support TYPEFUNC_COMPOSITE_DOMAIN here, as it's
	 * unlikely that legacy callers of this obsolete function would be
	 * prepared to apply domain constraints.
	 */
	if (functypclass == TYPEFUNC_COMPOSITE)
	{
		/* Composite data type, e.g. a table's row type */
		tupdesc = lookup_rowtype_tupdesc_copy(base_typeoid, -1);

		if (colaliases != NIL)
		{
			int			natts = tupdesc->natts;
			int			varattno;

			/* does the list length match the number of attributes? */
			if (list_length(colaliases) != natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("number of aliases does not match number of columns")));

			/* OK, use the aliases instead */
			for (varattno = 0; varattno < natts; varattno++)
			{
				char	   *label = strVal(list_nth(colaliases, varattno));
				Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);

				if (label != NULL)
					namestrcpy(&(attr->attname), label);
			}

			/* The tuple type is now an anonymous record type */
			tupdesc->tdtypeid = RECORDOID;
			tupdesc->tdtypmod = -1;
		}
	}
	else if (functypclass == TYPEFUNC_SCALAR)
	{
		/* Base data type, i.e. scalar */
		char	   *attname;

		/* the alias list is required for base types */
		if (colaliases == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("no column alias was provided")));

		/* the alias list length must be 1 */
		if (list_length(colaliases) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("number of aliases does not match number of columns")));

		/* OK, get the column alias */
		attname = strVal(linitial(colaliases));

		tupdesc = CreateTemplateTupleDesc(1);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) 1,
						   attname,
						   typeoid,
						   -1,
						   0);
	}
	else if (functypclass == TYPEFUNC_RECORD)
	{
		/* XXX can't support this because typmod wasn't passed in ... */
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("could not determine row description for function returning record")));
	}
	else
	{
		/* crummy error message, but parser should have caught this */
		elog(ERROR, "function in FROM has unsupported return type");
	}

	return tupdesc;
}

/*
 * extract_variadic_args
 *
 * Extract a set of argument values, types and NULL markers for a given
 * input function which makes use of a VARIADIC input whose argument list
 * depends on the caller context. When doing a VARIADIC call, the caller
 * has provided one argument made of an array of values, so deconstruct the
 * array data before using it for the next processing. If no VARIADIC call
 * is used, just fill in the status data based on all the arguments given
 * by the caller.
 *
 * This function returns the number of arguments generated, or -1 in the
 * case of "VARIADIC NULL".
 */
int
extract_variadic_args(FunctionCallInfo fcinfo, int variadic_start,
					  bool convert_unknown, Datum **args, Oid **types,
					  bool **nulls)
{
	bool		variadic = get_fn_expr_variadic(fcinfo->flinfo);
	Datum	   *args_res;
	bool	   *nulls_res;
	Oid		   *types_res;
	int			nargs,
				i;

	*args = NULL;
	*types = NULL;
	*nulls = NULL;

	if (variadic)
	{
		ArrayType  *array_in;
		Oid			element_type;
		bool		typbyval;
		char		typalign;
		int16		typlen;

		Assert(PG_NARGS() == variadic_start + 1);

		if (PG_ARGISNULL(variadic_start))
			return -1;

		array_in = PG_GETARG_ARRAYTYPE_P(variadic_start);
		element_type = ARR_ELEMTYPE(array_in);

		get_typlenbyvalalign(element_type,
							 &typlen, &typbyval, &typalign);
		deconstruct_array(array_in, element_type, typlen, typbyval,
						  typalign, &args_res, &nulls_res,
						  &nargs);

		/* All the elements of the array have the same type */
		types_res = (Oid *) palloc0(nargs * sizeof(Oid));
		for (i = 0; i < nargs; i++)
			types_res[i] = element_type;
	}
	else
	{
		nargs = PG_NARGS() - variadic_start;
		Assert(nargs > 0);
		nulls_res = (bool *) palloc0(nargs * sizeof(bool));
		args_res = (Datum *) palloc0(nargs * sizeof(Datum));
		types_res = (Oid *) palloc0(nargs * sizeof(Oid));

		for (i = 0; i < nargs; i++)
		{
			nulls_res[i] = PG_ARGISNULL(i + variadic_start);
			types_res[i] = get_fn_expr_argtype(fcinfo->flinfo,
											   i + variadic_start);

			/*
			 * Turn a constant (more or less literal) value that's of unknown
			 * type into text if required. Unknowns come in as a cstring
			 * pointer. Note: for functions declared as taking type "any", the
			 * parser will not do any type conversion on unknown-type literals
			 * (that is, undecorated strings or NULLs).
			 */
			if (convert_unknown &&
				types_res[i] == UNKNOWNOID &&
				get_fn_expr_arg_stable(fcinfo->flinfo, i + variadic_start))
			{
				types_res[i] = TEXTOID;

				if (PG_ARGISNULL(i + variadic_start))
					args_res[i] = (Datum) 0;
				else
					args_res[i] =
						CStringGetTextDatum(PG_GETARG_POINTER(i + variadic_start));
			}
			else
			{
				/* no conversion needed, just take the datum as given */
				args_res[i] = PG_GETARG_DATUM(i + variadic_start);
			}

			if (!OidIsValid(types_res[i]) ||
				(convert_unknown && types_res[i] == UNKNOWNOID))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not determine data type for argument %d",
								i + 1)));
		}
	}

	/* Fill in results */
	*args = args_res;
	*nulls = nulls_res;
	*types = types_res;

	return nargs;
}
