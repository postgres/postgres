/*-------------------------------------------------------------------------
 *
 * pg_aggregate.c
 *	  routines to support manipulation of the pg_aggregate relation
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_aggregate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_proc_fn.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static Oid lookup_agg_function(List *fnName, int nargs, Oid *input_types,
					Oid *rettype);


/*
 * AggregateCreate
 */
Oid
AggregateCreate(const char *aggName,
				Oid aggNamespace,
				Oid *aggArgTypes,
				int numArgs,
				List *aggtransfnName,
				List *aggfinalfnName,
				List *aggsortopName,
				Oid aggTransType,
				const char *agginitval)
{
	Relation	aggdesc;
	HeapTuple	tup;
	bool		nulls[Natts_pg_aggregate];
	Datum		values[Natts_pg_aggregate];
	Form_pg_proc proc;
	Oid			transfn;
	Oid			finalfn = InvalidOid;	/* can be omitted */
	Oid			sortop = InvalidOid;	/* can be omitted */
	bool		hasPolyArg;
	bool		hasInternalArg;
	Oid			rettype;
	Oid			finaltype;
	Oid		   *fnArgs;
	int			nargs_transfn;
	Oid			procOid;
	TupleDesc	tupDesc;
	int			i;
	ObjectAddress myself,
				referenced;
	AclResult	aclresult;

	/* sanity checks (caller should have caught these) */
	if (!aggName)
		elog(ERROR, "no aggregate name supplied");

	if (!aggtransfnName)
		elog(ERROR, "aggregate must have a transition function");

	/* check for polymorphic and INTERNAL arguments */
	hasPolyArg = false;
	hasInternalArg = false;
	for (i = 0; i < numArgs; i++)
	{
		if (IsPolymorphicType(aggArgTypes[i]))
			hasPolyArg = true;
		else if (aggArgTypes[i] == INTERNALOID)
			hasInternalArg = true;
	}

	/*
	 * If transtype is polymorphic, must have polymorphic argument also; else
	 * we will have no way to deduce the actual transtype.
	 */
	if (IsPolymorphicType(aggTransType) && !hasPolyArg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("cannot determine transition data type"),
				 errdetail("An aggregate using a polymorphic transition type must have at least one polymorphic argument.")));

	/* find the transfn */
	nargs_transfn = numArgs + 1;
	fnArgs = (Oid *) palloc(nargs_transfn * sizeof(Oid));
	fnArgs[0] = aggTransType;
	memcpy(fnArgs + 1, aggArgTypes, numArgs * sizeof(Oid));
	transfn = lookup_agg_function(aggtransfnName, nargs_transfn, fnArgs,
								  &rettype);

	/*
	 * Return type of transfn (possibly after refinement by
	 * enforce_generic_type_consistency, if transtype isn't polymorphic) must
	 * exactly match declared transtype.
	 *
	 * In the non-polymorphic-transtype case, it might be okay to allow a
	 * rettype that's binary-coercible to transtype, but I'm not quite
	 * convinced that it's either safe or useful.  When transtype is
	 * polymorphic we *must* demand exact equality.
	 */
	if (rettype != aggTransType)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("return type of transition function %s is not %s",
						NameListToString(aggtransfnName),
						format_type_be(aggTransType))));

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(transfn));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", transfn);
	proc = (Form_pg_proc) GETSTRUCT(tup);

	/*
	 * If the transfn is strict and the initval is NULL, make sure first input
	 * type and transtype are the same (or at least binary-compatible), so
	 * that it's OK to use the first input value as the initial transValue.
	 */
	if (proc->proisstrict && agginitval == NULL)
	{
		if (numArgs < 1 ||
			!IsBinaryCoercible(aggArgTypes[0], aggTransType))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("must not omit initial value when transition function is strict and transition type is not compatible with input type")));
	}
	ReleaseSysCache(tup);

	/* handle finalfn, if supplied */
	if (aggfinalfnName)
	{
		fnArgs[0] = aggTransType;
		finalfn = lookup_agg_function(aggfinalfnName, 1, fnArgs,
									  &finaltype);
	}
	else
	{
		/*
		 * If no finalfn, aggregate result type is type of the state value
		 */
		finaltype = aggTransType;
	}
	Assert(OidIsValid(finaltype));

	/*
	 * If finaltype (i.e. aggregate return type) is polymorphic, inputs must
	 * be polymorphic also, else parser will fail to deduce result type.
	 * (Note: given the previous test on transtype and inputs, this cannot
	 * happen, unless someone has snuck a finalfn definition into the catalogs
	 * that itself violates the rule against polymorphic result with no
	 * polymorphic input.)
	 */
	if (IsPolymorphicType(finaltype) && !hasPolyArg)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot determine result data type"),
				 errdetail("An aggregate returning a polymorphic type "
						   "must have at least one polymorphic argument.")));

	/*
	 * Also, the return type can't be INTERNAL unless there's at least one
	 * INTERNAL argument.  This is the same type-safety restriction we enforce
	 * for regular functions, but at the level of aggregates.  We must test
	 * this explicitly because we allow INTERNAL as the transtype.
	 */
	if (finaltype == INTERNALOID && !hasInternalArg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("unsafe use of pseudo-type \"internal\""),
				 errdetail("A function returning \"internal\" must have at least one \"internal\" argument.")));

	/* handle sortop, if supplied */
	if (aggsortopName)
	{
		if (numArgs != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("sort operator can only be specified for single-argument aggregates")));
		sortop = LookupOperName(NULL, aggsortopName,
								aggArgTypes[0], aggArgTypes[0],
								false, -1);
	}

	/*
	 * permission checks on used types
	 */
	for (i = 0; i < numArgs; i++)
	{
		aclresult = pg_type_aclcheck(aggArgTypes[i], GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, aggArgTypes[i]);
	}

	aclresult = pg_type_aclcheck(aggTransType, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, aggTransType);

	aclresult = pg_type_aclcheck(finaltype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, finaltype);


	/*
	 * Everything looks okay.  Try to create the pg_proc entry for the
	 * aggregate.  (This could fail if there's already a conflicting entry.)
	 */

	procOid = ProcedureCreate(aggName,
							  aggNamespace,
							  false,	/* no replacement */
							  false,	/* doesn't return a set */
							  finaltype,		/* returnType */
							  GetUserId(),		/* proowner */
							  INTERNALlanguageId,		/* languageObjectId */
							  InvalidOid,		/* no validator */
							  "aggregate_dummy",		/* placeholder proc */
							  NULL,		/* probin */
							  true,		/* isAgg */
							  false,	/* isWindowFunc */
							  false,	/* security invoker (currently not
										 * definable for agg) */
							  false,	/* isLeakProof */
							  false,	/* isStrict (not needed for agg) */
							  PROVOLATILE_IMMUTABLE,	/* volatility (not
														 * needed for agg) */
							  buildoidvector(aggArgTypes,
											 numArgs),	/* paramTypes */
							  PointerGetDatum(NULL),	/* allParamTypes */
							  PointerGetDatum(NULL),	/* parameterModes */
							  PointerGetDatum(NULL),	/* parameterNames */
							  NIL,		/* parameterDefaults */
							  PointerGetDatum(NULL),	/* proconfig */
							  1,	/* procost */
							  0);		/* prorows */

	/*
	 * Okay to create the pg_aggregate entry.
	 */

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_aggregate; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
	}
	values[Anum_pg_aggregate_aggfnoid - 1] = ObjectIdGetDatum(procOid);
	values[Anum_pg_aggregate_aggtransfn - 1] = ObjectIdGetDatum(transfn);
	values[Anum_pg_aggregate_aggfinalfn - 1] = ObjectIdGetDatum(finalfn);
	values[Anum_pg_aggregate_aggsortop - 1] = ObjectIdGetDatum(sortop);
	values[Anum_pg_aggregate_aggtranstype - 1] = ObjectIdGetDatum(aggTransType);
	if (agginitval)
		values[Anum_pg_aggregate_agginitval - 1] = CStringGetTextDatum(agginitval);
	else
		nulls[Anum_pg_aggregate_agginitval - 1] = true;

	aggdesc = heap_open(AggregateRelationId, RowExclusiveLock);
	tupDesc = aggdesc->rd_att;

	tup = heap_form_tuple(tupDesc, values, nulls);
	simple_heap_insert(aggdesc, tup);

	CatalogUpdateIndexes(aggdesc, tup);

	heap_close(aggdesc, RowExclusiveLock);

	/*
	 * Create dependencies for the aggregate (above and beyond those already
	 * made by ProcedureCreate).  Note: we don't need an explicit dependency
	 * on aggTransType since we depend on it indirectly through transfn.
	 */
	myself.classId = ProcedureRelationId;
	myself.objectId = procOid;
	myself.objectSubId = 0;

	/* Depends on transition function */
	referenced.classId = ProcedureRelationId;
	referenced.objectId = transfn;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* Depends on final function, if any */
	if (OidIsValid(finalfn))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = finalfn;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Depends on sort operator, if any */
	if (OidIsValid(sortop))
	{
		referenced.classId = OperatorRelationId;
		referenced.objectId = sortop;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	return procOid;
}

/*
 * lookup_agg_function -- common code for finding both transfn and finalfn
 */
static Oid
lookup_agg_function(List *fnName,
					int nargs,
					Oid *input_types,
					Oid *rettype)
{
	Oid			fnOid;
	bool		retset;
	int			nvargs;
	Oid		   *true_oid_array;
	FuncDetailCode fdresult;
	AclResult	aclresult;
	int			i;

	/*
	 * func_get_detail looks up the function in the catalogs, does
	 * disambiguation for polymorphic functions, handles inheritance, and
	 * returns the funcid and type and set or singleton status of the
	 * function's return value.  it also returns the true argument types to
	 * the function.
	 */
	fdresult = func_get_detail(fnName, NIL, NIL,
							   nargs, input_types, false, false,
							   &fnOid, rettype, &retset, &nvargs,
							   &true_oid_array, NULL);

	/* only valid case is a normal function not returning a set */
	if (fdresult != FUNCDETAIL_NORMAL || !OidIsValid(fnOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(fnName, nargs,
											  NIL, input_types))));
	if (retset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function %s returns a set",
						func_signature_string(fnName, nargs,
											  NIL, input_types))));

	/*
	 * If there are any polymorphic types involved, enforce consistency, and
	 * possibly refine the result type.  It's OK if the result is still
	 * polymorphic at this point, though.
	 */
	*rettype = enforce_generic_type_consistency(input_types,
												true_oid_array,
												nargs,
												*rettype,
												true);

	/*
	 * func_get_detail will find functions requiring run-time argument type
	 * coercion, but nodeAgg.c isn't prepared to deal with that
	 */
	for (i = 0; i < nargs; i++)
	{
		if (!IsPolymorphicType(true_oid_array[i]) &&
			!IsBinaryCoercible(input_types[i], true_oid_array[i]))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function %s requires run-time type coercion",
							func_signature_string(fnName, nargs,
												  NIL, true_oid_array))));
	}

	/* Check aggregate creator has permission to call the function */
	aclresult = pg_proc_aclcheck(fnOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_PROC, get_func_name(fnOid));

	return fnOid;
}
