/*-------------------------------------------------------------------------
 *
 * aggregatecmds.c
 *
 *	  Routines for aggregate-manipulation commands
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/aggregatecmds.c
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 *	DefineAggregate
 *
 * "oldstyle" signals the old (pre-8.2) style where the aggregate input type
 * is specified by a BASETYPE element in the parameters.  Otherwise,
 * "args" is a pair, whose first element is a list of FunctionParameter structs
 * defining the agg's arguments (both direct and aggregated), and whose second
 * element is an Integer node with the number of direct args, or -1 if this
 * isn't an ordered-set aggregate.
 * "parameters" is a list of DefElem representing the agg's definition clauses.
 */
Oid
DefineAggregate(List *name, List *args, bool oldstyle, List *parameters,
				const char *queryString)
{
	char	   *aggName;
	Oid			aggNamespace;
	AclResult	aclresult;
	char		aggKind = AGGKIND_NORMAL;
	List	   *transfuncName = NIL;
	List	   *finalfuncName = NIL;
	List	   *sortoperatorName = NIL;
	TypeName   *baseType = NULL;
	TypeName   *transType = NULL;
	int32		transSpace = 0;
	char	   *initval = NULL;
	int			numArgs;
	int			numDirectArgs = 0;
	oidvector  *parameterTypes;
	ArrayType  *allParameterTypes;
	ArrayType  *parameterModes;
	ArrayType  *parameterNames;
	List	   *parameterDefaults;
	Oid			variadicArgType;
	Oid			transTypeId;
	char		transTypeType;
	ListCell   *pl;

	/* Convert list of names to a name and namespace */
	aggNamespace = QualifiedNameGetCreationNamespace(name, &aggName);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(aggNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(aggNamespace));

	/* Deconstruct the output of the aggr_args grammar production */
	if (!oldstyle)
	{
		Assert(list_length(args) == 2);
		numDirectArgs = intVal(lsecond(args));
		if (numDirectArgs >= 0)
			aggKind = AGGKIND_ORDERED_SET;
		else
			numDirectArgs = 0;
		args = (List *) linitial(args);
	}

	/* Examine aggregate's definition clauses */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		/*
		 * sfunc1, stype1, and initcond1 are accepted as obsolete spellings
		 * for sfunc, stype, initcond.
		 */
		if (pg_strcasecmp(defel->defname, "sfunc") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "sfunc1") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "finalfunc") == 0)
			finalfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "sortop") == 0)
			sortoperatorName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "basetype") == 0)
			baseType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "hypothetical") == 0)
		{
			if (defGetBoolean(defel))
			{
				if (aggKind == AGGKIND_NORMAL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("only ordered-set aggregates can be hypothetical")));
				aggKind = AGGKIND_HYPOTHETICAL;
			}
		}
		else if (pg_strcasecmp(defel->defname, "stype") == 0)
			transType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "stype1") == 0)
			transType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "sspace") == 0)
			transSpace = defGetInt32(defel);
		else if (pg_strcasecmp(defel->defname, "initcond") == 0)
			initval = defGetString(defel);
		else if (pg_strcasecmp(defel->defname, "initcond1") == 0)
			initval = defGetString(defel);
		else
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("aggregate attribute \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * make sure we have our required definitions
	 */
	if (transType == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate stype must be specified")));
	if (transfuncName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate sfunc must be specified")));

	/*
	 * look up the aggregate's input datatype(s).
	 */
	if (oldstyle)
	{
		/*
		 * Old style: use basetype parameter.  This supports aggregates of
		 * zero or one input, with input type ANY meaning zero inputs.
		 *
		 * Historically we allowed the command to look like basetype = 'ANY'
		 * so we must do a case-insensitive comparison for the name ANY. Ugh.
		 */
		Oid			aggArgTypes[1];

		if (baseType == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate input type must be specified")));

		if (pg_strcasecmp(TypeNameToString(baseType), "ANY") == 0)
		{
			numArgs = 0;
			aggArgTypes[0] = InvalidOid;
		}
		else
		{
			numArgs = 1;
			aggArgTypes[0] = typenameTypeId(NULL, baseType);
		}
		parameterTypes = buildoidvector(aggArgTypes, numArgs);
		allParameterTypes = NULL;
		parameterModes = NULL;
		parameterNames = NULL;
		parameterDefaults = NIL;
		variadicArgType = InvalidOid;
	}
	else
	{
		/*
		 * New style: args is a list of FunctionParameters (possibly zero of
		 * 'em).  We share functioncmds.c's code for processing them.
		 */
		Oid			requiredResultType;

		if (baseType != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("basetype is redundant with aggregate input type specification")));

		numArgs = list_length(args);
		interpret_function_parameter_list(args,
										  InvalidOid,
										  true, /* is an aggregate */
										  queryString,
										  &parameterTypes,
										  &allParameterTypes,
										  &parameterModes,
										  &parameterNames,
										  &parameterDefaults,
										  &variadicArgType,
										  &requiredResultType);
		/* Parameter defaults are not currently allowed by the grammar */
		Assert(parameterDefaults == NIL);
		/* There shouldn't have been any OUT parameters, either */
		Assert(requiredResultType == InvalidOid);
	}

	/*
	 * look up the aggregate's transtype.
	 *
	 * transtype can't be a pseudo-type, since we need to be able to store
	 * values of the transtype.  However, we can allow polymorphic transtype
	 * in some cases (AggregateCreate will check).	Also, we allow "internal"
	 * for functions that want to pass pointers to private data structures;
	 * but allow that only to superusers, since you could crash the system (or
	 * worse) by connecting up incompatible internal-using functions in an
	 * aggregate.
	 */
	transTypeId = typenameTypeId(NULL, transType);
	transTypeType = get_typtype(transTypeId);
	if (transTypeType == TYPTYPE_PSEUDO &&
		!IsPolymorphicType(transTypeId))
	{
		if (transTypeId == INTERNALOID && superuser())
			 /* okay */ ;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate transition data type cannot be %s",
							format_type_be(transTypeId))));
	}

	/*
	 * If we have an initval, and it's not for a pseudotype (particularly a
	 * polymorphic type), make sure it's acceptable to the type's input
	 * function.  We will store the initval as text, because the input
	 * function isn't necessarily immutable (consider "now" for timestamp),
	 * and we want to use the runtime not creation-time interpretation of the
	 * value.  However, if it's an incorrect value it seems much more
	 * user-friendly to complain at CREATE AGGREGATE time.
	 */
	if (initval && transTypeType != TYPTYPE_PSEUDO)
	{
		Oid			typinput,
					typioparam;

		getTypeInputInfo(transTypeId, &typinput, &typioparam);
		(void) OidInputFunctionCall(typinput, initval, typioparam, -1);
	}

	/*
	 * Most of the argument-checking is done inside of AggregateCreate
	 */
	return AggregateCreate(aggName,		/* aggregate name */
						   aggNamespace,		/* namespace */
						   aggKind,
						   numArgs,
						   numDirectArgs,
						   parameterTypes,
						   PointerGetDatum(allParameterTypes),
						   PointerGetDatum(parameterModes),
						   PointerGetDatum(parameterNames),
						   parameterDefaults,
						   variadicArgType,
						   transfuncName,		/* step function name */
						   finalfuncName,		/* final function name */
						   sortoperatorName,	/* sort operator name */
						   transTypeId, /* transition data type */
						   transSpace,	/* transition space */
						   initval);	/* initial condition */
}
