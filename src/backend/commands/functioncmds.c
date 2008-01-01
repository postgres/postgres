/*-------------------------------------------------------------------------
 *
 * functioncmds.c
 *
 *	  Routines for CREATE and DROP FUNCTION commands and CREATE and DROP
 *	  CAST commands.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/functioncmds.c,v 1.88 2008/01/01 19:45:49 momjian Exp $
 *
 * DESCRIPTION
 *	  These routines take the parse tree and pick out the
 *	  appropriate arguments/flags, and pass the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send procedures
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/proclang.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void AlterFunctionOwner_internal(Relation rel, HeapTuple tup,
							Oid newOwnerId);


/*
 *	 Examine the RETURNS clause of the CREATE FUNCTION statement
 *	 and return information about it as *prorettype_p and *returnsSet.
 *
 * This is more complex than the average typename lookup because we want to
 * allow a shell type to be used, or even created if the specified return type
 * doesn't exist yet.  (Without this, there's no way to define the I/O procs
 * for a new type.)  But SQL function creation won't cope, so error out if
 * the target language is SQL.	(We do this here, not in the SQL-function
 * validator, so as not to produce a NOTICE and then an ERROR for the same
 * condition.)
 */
static void
compute_return_type(TypeName *returnType, Oid languageOid,
					Oid *prorettype_p, bool *returnsSet_p)
{
	Oid			rettype;
	Type		typtup;

	typtup = LookupTypeName(NULL, returnType, NULL);

	if (typtup)
	{
		if (!((Form_pg_type) GETSTRUCT(typtup))->typisdefined)
		{
			if (languageOid == SQLlanguageId)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SQL function cannot return shell type %s",
								TypeNameToString(returnType))));
			else
				ereport(NOTICE,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("return type %s is only a shell",
								TypeNameToString(returnType))));
		}
		rettype = typeTypeId(typtup);
		ReleaseSysCache(typtup);
	}
	else
	{
		char	   *typnam = TypeNameToString(returnType);
		Oid			namespaceId;
		AclResult	aclresult;
		char	   *typname;

		/*
		 * Only C-coded functions can be I/O functions.  We enforce this
		 * restriction here mainly to prevent littering the catalogs with
		 * shell types due to simple typos in user-defined function
		 * definitions.
		 */
		if (languageOid != INTERNALlanguageId &&
			languageOid != ClanguageId)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" does not exist", typnam)));

		/* Reject if there's typmod decoration, too */
		if (returnType->typmods != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("type modifier cannot be specified for shell type \"%s\"",
				   typnam)));

		/* Otherwise, go ahead and make a shell type */
		ereport(NOTICE,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" is not yet defined", typnam),
				 errdetail("Creating a shell type definition.")));
		namespaceId = QualifiedNameGetCreationNamespace(returnType->names,
														&typname);
		aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(),
										  ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
						   get_namespace_name(namespaceId));
		rettype = TypeShellMake(typname, namespaceId);
		Assert(OidIsValid(rettype));
	}

	*prorettype_p = rettype;
	*returnsSet_p = returnType->setof;
}

/*
 * Interpret the parameter list of the CREATE FUNCTION statement.
 *
 * Results are stored into output parameters.  parameterTypes must always
 * be created, but the other arrays are set to NULL if not needed.
 * requiredResultType is set to InvalidOid if there are no OUT parameters,
 * else it is set to the OID of the implied result type.
 */
static void
examine_parameter_list(List *parameters, Oid languageOid,
					   oidvector **parameterTypes,
					   ArrayType **allParameterTypes,
					   ArrayType **parameterModes,
					   ArrayType **parameterNames,
					   Oid *requiredResultType)
{
	int			parameterCount = list_length(parameters);
	Oid		   *inTypes;
	int			inCount = 0;
	Datum	   *allTypes;
	Datum	   *paramModes;
	Datum	   *paramNames;
	int			outCount = 0;
	bool		have_names = false;
	ListCell   *x;
	int			i;

	*requiredResultType = InvalidOid;	/* default result */

	inTypes = (Oid *) palloc(parameterCount * sizeof(Oid));
	allTypes = (Datum *) palloc(parameterCount * sizeof(Datum));
	paramModes = (Datum *) palloc(parameterCount * sizeof(Datum));
	paramNames = (Datum *) palloc0(parameterCount * sizeof(Datum));

	/* Scan the list and extract data into work arrays */
	i = 0;
	foreach(x, parameters)
	{
		FunctionParameter *fp = (FunctionParameter *) lfirst(x);
		TypeName   *t = fp->argType;
		Oid			toid;
		Type		typtup;

		typtup = LookupTypeName(NULL, t, NULL);
		if (typtup)
		{
			if (!((Form_pg_type) GETSTRUCT(typtup))->typisdefined)
			{
				/* As above, hard error if language is SQL */
				if (languageOid == SQLlanguageId)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						   errmsg("SQL function cannot accept shell type %s",
								  TypeNameToString(t))));
				else
					ereport(NOTICE,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("argument type %s is only a shell",
									TypeNameToString(t))));
			}
			toid = typeTypeId(typtup);
			ReleaseSysCache(typtup);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type %s does not exist",
							TypeNameToString(t))));
			toid = InvalidOid;	/* keep compiler quiet */
		}

		if (t->setof)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("functions cannot accept set arguments")));

		if (fp->mode != FUNC_PARAM_OUT)
			inTypes[inCount++] = toid;

		if (fp->mode != FUNC_PARAM_IN)
		{
			if (outCount == 0)	/* save first OUT param's type */
				*requiredResultType = toid;
			outCount++;
		}

		allTypes[i] = ObjectIdGetDatum(toid);

		paramModes[i] = CharGetDatum(fp->mode);

		if (fp->name && fp->name[0])
		{
			paramNames[i] = DirectFunctionCall1(textin,
												CStringGetDatum(fp->name));
			have_names = true;
		}

		i++;
	}

	/* Now construct the proper outputs as needed */
	*parameterTypes = buildoidvector(inTypes, inCount);

	if (outCount > 0)
	{
		*allParameterTypes = construct_array(allTypes, parameterCount, OIDOID,
											 sizeof(Oid), true, 'i');
		*parameterModes = construct_array(paramModes, parameterCount, CHAROID,
										  1, true, 'c');
		if (outCount > 1)
			*requiredResultType = RECORDOID;
		/* otherwise we set requiredResultType correctly above */
	}
	else
	{
		*allParameterTypes = NULL;
		*parameterModes = NULL;
	}

	if (have_names)
	{
		for (i = 0; i < parameterCount; i++)
		{
			if (paramNames[i] == PointerGetDatum(NULL))
				paramNames[i] = DirectFunctionCall1(textin,
													CStringGetDatum(""));
		}
		*parameterNames = construct_array(paramNames, parameterCount, TEXTOID,
										  -1, false, 'i');
	}
	else
		*parameterNames = NULL;
}


/*
 * Recognize one of the options that can be passed to both CREATE
 * FUNCTION and ALTER FUNCTION and return it via one of the out
 * parameters. Returns true if the passed option was recognized. If
 * the out parameter we were going to assign to points to non-NULL,
 * raise a duplicate-clause error.	(We don't try to detect duplicate
 * SET parameters though --- if you're redundant, the last one wins.)
 */
static bool
compute_common_attribute(DefElem *defel,
						 DefElem **volatility_item,
						 DefElem **strict_item,
						 DefElem **security_item,
						 List **set_items,
						 DefElem **cost_item,
						 DefElem **rows_item)
{
	if (strcmp(defel->defname, "volatility") == 0)
	{
		if (*volatility_item)
			goto duplicate_error;

		*volatility_item = defel;
	}
	else if (strcmp(defel->defname, "strict") == 0)
	{
		if (*strict_item)
			goto duplicate_error;

		*strict_item = defel;
	}
	else if (strcmp(defel->defname, "security") == 0)
	{
		if (*security_item)
			goto duplicate_error;

		*security_item = defel;
	}
	else if (strcmp(defel->defname, "set") == 0)
	{
		*set_items = lappend(*set_items, defel->arg);
	}
	else if (strcmp(defel->defname, "cost") == 0)
	{
		if (*cost_item)
			goto duplicate_error;

		*cost_item = defel;
	}
	else if (strcmp(defel->defname, "rows") == 0)
	{
		if (*rows_item)
			goto duplicate_error;

		*rows_item = defel;
	}
	else
		return false;

	/* Recognized an option */
	return true;

duplicate_error:
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("conflicting or redundant options")));
	return false;				/* keep compiler quiet */
}

static char
interpret_func_volatility(DefElem *defel)
{
	char	   *str = strVal(defel->arg);

	if (strcmp(str, "immutable") == 0)
		return PROVOLATILE_IMMUTABLE;
	else if (strcmp(str, "stable") == 0)
		return PROVOLATILE_STABLE;
	else if (strcmp(str, "volatile") == 0)
		return PROVOLATILE_VOLATILE;
	else
	{
		elog(ERROR, "invalid volatility \"%s\"", str);
		return 0;				/* keep compiler quiet */
	}
}

/*
 * Update a proconfig value according to a list of VariableSetStmt items.
 *
 * The input and result may be NULL to signify a null entry.
 */
static ArrayType *
update_proconfig_value(ArrayType *a, List *set_items)
{
	ListCell   *l;

	foreach(l, set_items)
	{
		VariableSetStmt *sstmt = (VariableSetStmt *) lfirst(l);

		Assert(IsA(sstmt, VariableSetStmt));
		if (sstmt->kind == VAR_RESET_ALL)
			a = NULL;
		else
		{
			char	   *valuestr = ExtractSetVariableArgs(sstmt);

			if (valuestr)
				a = GUCArrayAdd(a, sstmt->name, valuestr);
			else	/* RESET */
				a = GUCArrayDelete(a, sstmt->name);
		}
	}

	return a;
}


/*
 * Dissect the list of options assembled in gram.y into function
 * attributes.
 */
static void
compute_attributes_sql_style(List *options,
							 List **as,
							 char **language,
							 char *volatility_p,
							 bool *strict_p,
							 bool *security_definer,
							 ArrayType **proconfig,
							 float4 *procost,
							 float4 *prorows)
{
	ListCell   *option;
	DefElem    *as_item = NULL;
	DefElem    *language_item = NULL;
	DefElem    *volatility_item = NULL;
	DefElem    *strict_item = NULL;
	DefElem    *security_item = NULL;
	List	   *set_items = NIL;
	DefElem    *cost_item = NULL;
	DefElem    *rows_item = NULL;

	foreach(option, options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "as") == 0)
		{
			if (as_item)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			as_item = defel;
		}
		else if (strcmp(defel->defname, "language") == 0)
		{
			if (language_item)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			language_item = defel;
		}
		else if (compute_common_attribute(defel,
										  &volatility_item,
										  &strict_item,
										  &security_item,
										  &set_items,
										  &cost_item,
										  &rows_item))
		{
			/* recognized common option */
			continue;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/* process required items */
	if (as_item)
		*as = (List *) as_item->arg;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("no function body specified")));
		*as = NIL;				/* keep compiler quiet */
	}

	if (language_item)
		*language = strVal(language_item->arg);
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("no language specified")));
		*language = NULL;		/* keep compiler quiet */
	}

	/* process optional items */
	if (volatility_item)
		*volatility_p = interpret_func_volatility(volatility_item);
	if (strict_item)
		*strict_p = intVal(strict_item->arg);
	if (security_item)
		*security_definer = intVal(security_item->arg);
	if (set_items)
		*proconfig = update_proconfig_value(NULL, set_items);
	if (cost_item)
	{
		*procost = defGetNumeric(cost_item);
		if (*procost <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("COST must be positive")));
	}
	if (rows_item)
	{
		*prorows = defGetNumeric(rows_item);
		if (*prorows <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ROWS must be positive")));
	}
}


/*-------------
 *	 Interpret the parameters *parameters and return their contents via
 *	 *isStrict_p and *volatility_p.
 *
 *	These parameters supply optional information about a function.
 *	All have defaults if not specified. Parameters:
 *
 *	 * isStrict means the function should not be called when any NULL
 *	   inputs are present; instead a NULL result value should be assumed.
 *
 *	 * volatility tells the optimizer whether the function's result can
 *	   be assumed to be repeatable over multiple evaluations.
 *------------
 */
static void
compute_attributes_with_style(List *parameters, bool *isStrict_p, char *volatility_p)
{
	ListCell   *pl;

	foreach(pl, parameters)
	{
		DefElem    *param = (DefElem *) lfirst(pl);

		if (pg_strcasecmp(param->defname, "isstrict") == 0)
			*isStrict_p = defGetBoolean(param);
		else if (pg_strcasecmp(param->defname, "iscachable") == 0)
		{
			/* obsolete spelling of isImmutable */
			if (defGetBoolean(param))
				*volatility_p = PROVOLATILE_IMMUTABLE;
		}
		else
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized function attribute \"%s\" ignored",
							param->defname)));
	}
}


/*
 * For a dynamically linked C language object, the form of the clause is
 *
 *	   AS <object file name> [, <link symbol name> ]
 *
 * In all other cases
 *
 *	   AS <object reference, or sql code>
 */
static void
interpret_AS_clause(Oid languageOid, const char *languageName, List *as,
					char **prosrc_str_p, char **probin_str_p)
{
	Assert(as != NIL);

	if (languageOid == ClanguageId)
	{
		/*
		 * For "C" language, store the file name in probin and, when given,
		 * the link symbol name in prosrc.
		 */
		*probin_str_p = strVal(linitial(as));
		if (list_length(as) == 1)
			*prosrc_str_p = "-";
		else
			*prosrc_str_p = strVal(lsecond(as));
	}
	else
	{
		/* Everything else wants the given string in prosrc. */
		*prosrc_str_p = strVal(linitial(as));
		*probin_str_p = "-";

		if (list_length(as) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only one AS item needed for language \"%s\"",
							languageName)));
	}
}



/*
 * CreateFunction
 *	 Execute a CREATE FUNCTION utility statement.
 */
void
CreateFunction(CreateFunctionStmt *stmt)
{
	char	   *probin_str;
	char	   *prosrc_str;
	Oid			prorettype;
	bool		returnsSet;
	char	   *language;
	char	   *languageName;
	Oid			languageOid;
	Oid			languageValidator;
	char	   *funcname;
	Oid			namespaceId;
	AclResult	aclresult;
	oidvector  *parameterTypes;
	ArrayType  *allParameterTypes;
	ArrayType  *parameterModes;
	ArrayType  *parameterNames;
	Oid			requiredResultType;
	bool		isStrict,
				security;
	char		volatility;
	ArrayType  *proconfig;
	float4		procost;
	float4		prorows;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;
	List	   *as_clause;

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->funcname,
													&funcname);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceId));

	/* default attributes */
	isStrict = false;
	security = false;
	volatility = PROVOLATILE_VOLATILE;
	proconfig = NULL;
	procost = -1;				/* indicates not set */
	prorows = -1;				/* indicates not set */

	/* override attributes from explicit list */
	compute_attributes_sql_style(stmt->options,
								 &as_clause, &language,
								 &volatility, &isStrict, &security,
								 &proconfig, &procost, &prorows);

	/* Convert language name to canonical case */
	languageName = case_translate_language_name(language);

	/* Look up the language and validate permissions */
	languageTuple = SearchSysCache(LANGNAME,
								   PointerGetDatum(languageName),
								   0, 0, 0);
	if (!HeapTupleIsValid(languageTuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", languageName),
				 (PLTemplateExists(languageName) ?
				  errhint("Use CREATE LANGUAGE to load the language into the database.") : 0)));

	languageOid = HeapTupleGetOid(languageTuple);
	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);

	if (languageStruct->lanpltrusted)
	{
		/* if trusted language, need USAGE privilege */
		AclResult	aclresult;

		aclresult = pg_language_aclcheck(languageOid, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_LANGUAGE,
						   NameStr(languageStruct->lanname));
	}
	else
	{
		/* if untrusted language, must be superuser */
		if (!superuser())
			aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_LANGUAGE,
						   NameStr(languageStruct->lanname));
	}

	languageValidator = languageStruct->lanvalidator;

	ReleaseSysCache(languageTuple);

	/*
	 * Convert remaining parameters of CREATE to form wanted by
	 * ProcedureCreate.
	 */
	examine_parameter_list(stmt->parameters, languageOid,
						   &parameterTypes,
						   &allParameterTypes,
						   &parameterModes,
						   &parameterNames,
						   &requiredResultType);

	if (stmt->returnType)
	{
		/* explicit RETURNS clause */
		compute_return_type(stmt->returnType, languageOid,
							&prorettype, &returnsSet);
		if (OidIsValid(requiredResultType) && prorettype != requiredResultType)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("function result type must be %s because of OUT parameters",
							format_type_be(requiredResultType))));
	}
	else if (OidIsValid(requiredResultType))
	{
		/* default RETURNS clause from OUT parameters */
		prorettype = requiredResultType;
		returnsSet = false;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("function result type must be specified")));
		/* Alternative possibility: default to RETURNS VOID */
		prorettype = VOIDOID;
		returnsSet = false;
	}

	compute_attributes_with_style(stmt->withClause, &isStrict, &volatility);

	interpret_AS_clause(languageOid, languageName, as_clause,
						&prosrc_str, &probin_str);

	if (languageOid == INTERNALlanguageId)
	{
		/*
		 * In PostgreSQL versions before 6.5, the SQL name of the created
		 * function could not be different from the internal name, and
		 * "prosrc" wasn't used.  So there is code out there that does CREATE
		 * FUNCTION xyz AS '' LANGUAGE internal. To preserve some modicum of
		 * backwards compatibility, accept an empty "prosrc" value as meaning
		 * the supplied SQL function name.
		 */
		if (strlen(prosrc_str) == 0)
			prosrc_str = funcname;
	}

	if (languageOid == ClanguageId)
	{
		/* If link symbol is specified as "-", substitute procedure name */
		if (strcmp(prosrc_str, "-") == 0)
			prosrc_str = funcname;
	}

	/*
	 * Set default values for COST and ROWS depending on other parameters;
	 * reject ROWS if it's not returnsSet.  NB: pg_dump knows these default
	 * values, keep it in sync if you change them.
	 */
	if (procost < 0)
	{
		/* SQL and PL-language functions are assumed more expensive */
		if (languageOid == INTERNALlanguageId ||
			languageOid == ClanguageId)
			procost = 1;
		else
			procost = 100;
	}
	if (prorows < 0)
	{
		if (returnsSet)
			prorows = 1000;
		else
			prorows = 0;		/* dummy value if not returnsSet */
	}
	else if (!returnsSet)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ROWS is not applicable when function does not return a set")));

	/*
	 * And now that we have all the parameters, and know we're permitted to do
	 * so, go ahead and create the function.
	 */
	ProcedureCreate(funcname,
					namespaceId,
					stmt->replace,
					returnsSet,
					prorettype,
					languageOid,
					languageValidator,
					prosrc_str, /* converted to text later */
					probin_str, /* converted to text later */
					false,		/* not an aggregate */
					security,
					isStrict,
					volatility,
					parameterTypes,
					PointerGetDatum(allParameterTypes),
					PointerGetDatum(parameterModes),
					PointerGetDatum(parameterNames),
					PointerGetDatum(proconfig),
					procost,
					prorows);
}


/*
 * RemoveFunction
 *		Deletes a function.
 */
void
RemoveFunction(RemoveFuncStmt *stmt)
{
	List	   *functionName = stmt->name;
	List	   *argTypes = stmt->args;	/* list of TypeName nodes */
	Oid			funcOid;
	HeapTuple	tup;
	ObjectAddress object;

	/*
	 * Find the function, do permissions and validity checks
	 */
	funcOid = LookupFuncNameTypeNames(functionName, argTypes, stmt->missing_ok);
	if (!OidIsValid(funcOid))
	{
		/* can only get here if stmt->missing_ok */
		ereport(NOTICE,
				(errmsg("function %s(%s) does not exist, skipping",
						NameListToString(functionName),
						TypeNameListToString(argTypes))));
		return;
	}

	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(funcOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", funcOid);

	/* Permission check: must own func or its namespace */
	if (!pg_proc_ownercheck(funcOid, GetUserId()) &&
	  !pg_namespace_ownercheck(((Form_pg_proc) GETSTRUCT(tup))->pronamespace,
							   GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
					   NameListToString(functionName));

	if (((Form_pg_proc) GETSTRUCT(tup))->proisagg)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an aggregate function",
						NameListToString(functionName)),
				 errhint("Use DROP AGGREGATE to drop aggregate functions.")));

	if (((Form_pg_proc) GETSTRUCT(tup))->prolang == INTERNALlanguageId)
	{
		/* "Helpful" NOTICE when removing a builtin function ... */
		ereport(NOTICE,
				(errcode(ERRCODE_WARNING),
				 errmsg("removing built-in function \"%s\"",
						NameListToString(functionName))));
	}

	ReleaseSysCache(tup);

	/*
	 * Do the deletion
	 */
	object.classId = ProcedureRelationId;
	object.objectId = funcOid;
	object.objectSubId = 0;

	performDeletion(&object, stmt->behavior);
}

/*
 * Guts of function deletion.
 *
 * Note: this is also used for aggregate deletion, since the OIDs of
 * both functions and aggregates point to pg_proc.
 */
void
RemoveFunctionById(Oid funcOid)
{
	Relation	relation;
	HeapTuple	tup;
	bool		isagg;

	/*
	 * Delete the pg_proc tuple.
	 */
	relation = heap_open(ProcedureRelationId, RowExclusiveLock);

	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(funcOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", funcOid);

	isagg = ((Form_pg_proc) GETSTRUCT(tup))->proisagg;

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);

	/*
	 * If there's a pg_aggregate tuple, delete that too.
	 */
	if (isagg)
	{
		relation = heap_open(AggregateRelationId, RowExclusiveLock);

		tup = SearchSysCache(AGGFNOID,
							 ObjectIdGetDatum(funcOid),
							 0, 0, 0);
		if (!HeapTupleIsValid(tup))		/* should not happen */
			elog(ERROR, "cache lookup failed for pg_aggregate tuple for function %u", funcOid);

		simple_heap_delete(relation, &tup->t_self);

		ReleaseSysCache(tup);

		heap_close(relation, RowExclusiveLock);
	}
}


/*
 * Rename function
 */
void
RenameFunction(List *name, List *argtypes, const char *newname)
{
	Oid			procOid;
	Oid			namespaceOid;
	HeapTuple	tup;
	Form_pg_proc procForm;
	Relation	rel;
	AclResult	aclresult;

	rel = heap_open(ProcedureRelationId, RowExclusiveLock);

	procOid = LookupFuncNameTypeNames(name, argtypes, false);

	tup = SearchSysCacheCopy(PROCOID,
							 ObjectIdGetDatum(procOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", procOid);
	procForm = (Form_pg_proc) GETSTRUCT(tup);

	if (procForm->proisagg)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an aggregate function",
						NameListToString(name)),
			 errhint("Use ALTER AGGREGATE to rename aggregate functions.")));

	namespaceOid = procForm->pronamespace;

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists(PROCNAMEARGSNSP,
							 CStringGetDatum(newname),
							 PointerGetDatum(&procForm->proargtypes),
							 ObjectIdGetDatum(namespaceOid),
							 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("function %s already exists in schema \"%s\"",
						funcname_signature_string(newname,
												  procForm->pronargs,
											   procForm->proargtypes.values),
						get_namespace_name(namespaceOid))));
	}

	/* must be owner */
	if (!pg_proc_ownercheck(procOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
					   NameListToString(name));

	/* must have CREATE privilege on namespace */
	aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceOid));

	/* rename */
	namestrcpy(&(procForm->proname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/*
 * Change function owner by name and args
 */
void
AlterFunctionOwner(List *name, List *argtypes, Oid newOwnerId)
{
	Relation	rel;
	Oid			procOid;
	HeapTuple	tup;

	rel = heap_open(ProcedureRelationId, RowExclusiveLock);

	procOid = LookupFuncNameTypeNames(name, argtypes, false);

	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(procOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", procOid);

	if (((Form_pg_proc) GETSTRUCT(tup))->proisagg)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an aggregate function",
						NameListToString(name)),
				 errhint("Use ALTER AGGREGATE to change owner of aggregate functions.")));

	AlterFunctionOwner_internal(rel, tup, newOwnerId);

	heap_close(rel, NoLock);
}

/*
 * Change function owner by Oid
 */
void
AlterFunctionOwner_oid(Oid procOid, Oid newOwnerId)
{
	Relation	rel;
	HeapTuple	tup;

	rel = heap_open(ProcedureRelationId, RowExclusiveLock);

	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(procOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", procOid);
	AlterFunctionOwner_internal(rel, tup, newOwnerId);

	heap_close(rel, NoLock);
}

static void
AlterFunctionOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_proc procForm;
	AclResult	aclresult;
	Oid			procOid;

	Assert(RelationGetRelid(rel) == ProcedureRelationId);
	Assert(tup->t_tableOid == ProcedureRelationId);

	procForm = (Form_pg_proc) GETSTRUCT(tup);
	procOid = HeapTupleGetOid(tup);

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (procForm->proowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_proc];
		char		repl_null[Natts_pg_proc];
		char		repl_repl[Natts_pg_proc];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Superusers can always do it */
		if (!superuser())
		{
			/* Otherwise, must be owner of the existing object */
			if (!pg_proc_ownercheck(procOid, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
							   NameStr(procForm->proname));

			/* Must be able to become new owner */
			check_is_member_of_role(GetUserId(), newOwnerId);

			/* New owner must have CREATE privilege on namespace */
			aclresult = pg_namespace_aclcheck(procForm->pronamespace,
											  newOwnerId,
											  ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
							   get_namespace_name(procForm->pronamespace));
		}

		memset(repl_null, ' ', sizeof(repl_null));
		memset(repl_repl, ' ', sizeof(repl_repl));

		repl_repl[Anum_pg_proc_proowner - 1] = 'r';
		repl_val[Anum_pg_proc_proowner - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 */
		aclDatum = SysCacheGetAttr(PROCOID, tup,
								   Anum_pg_proc_proacl,
								   &isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 procForm->proowner, newOwnerId);
			repl_repl[Anum_pg_proc_proacl - 1] = 'r';
			repl_val[Anum_pg_proc_proacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modifytuple(tup, RelationGetDescr(rel), repl_val,
									repl_null, repl_repl);

		simple_heap_update(rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(rel, newtuple);

		heap_freetuple(newtuple);

		/* Update owner dependency reference */
		changeDependencyOnOwner(ProcedureRelationId, procOid, newOwnerId);
	}

	ReleaseSysCache(tup);
}

/*
 * Implements the ALTER FUNCTION utility command (except for the
 * RENAME and OWNER clauses, which are handled as part of the generic
 * ALTER framework).
 */
void
AlterFunction(AlterFunctionStmt *stmt)
{
	HeapTuple	tup;
	Oid			funcOid;
	Form_pg_proc procForm;
	Relation	rel;
	ListCell   *l;
	DefElem    *volatility_item = NULL;
	DefElem    *strict_item = NULL;
	DefElem    *security_def_item = NULL;
	List	   *set_items = NIL;
	DefElem    *cost_item = NULL;
	DefElem    *rows_item = NULL;

	rel = heap_open(ProcedureRelationId, RowExclusiveLock);

	funcOid = LookupFuncNameTypeNames(stmt->func->funcname,
									  stmt->func->funcargs,
									  false);

	tup = SearchSysCacheCopy(PROCOID,
							 ObjectIdGetDatum(funcOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", funcOid);

	procForm = (Form_pg_proc) GETSTRUCT(tup);

	/* Permission check: must own function */
	if (!pg_proc_ownercheck(funcOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
					   NameListToString(stmt->func->funcname));

	if (procForm->proisagg)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an aggregate function",
						NameListToString(stmt->func->funcname))));

	/* Examine requested actions. */
	foreach(l, stmt->actions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (compute_common_attribute(defel,
									 &volatility_item,
									 &strict_item,
									 &security_def_item,
									 &set_items,
									 &cost_item,
									 &rows_item) == false)
			elog(ERROR, "option \"%s\" not recognized", defel->defname);
	}

	if (volatility_item)
		procForm->provolatile = interpret_func_volatility(volatility_item);
	if (strict_item)
		procForm->proisstrict = intVal(strict_item->arg);
	if (security_def_item)
		procForm->prosecdef = intVal(security_def_item->arg);
	if (cost_item)
	{
		procForm->procost = defGetNumeric(cost_item);
		if (procForm->procost <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("COST must be positive")));
	}
	if (rows_item)
	{
		procForm->prorows = defGetNumeric(rows_item);
		if (procForm->prorows <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ROWS must be positive")));
		if (!procForm->proretset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ROWS is not applicable when function does not return a set")));
	}
	if (set_items)
	{
		Datum		datum;
		bool		isnull;
		ArrayType  *a;
		Datum		repl_val[Natts_pg_proc];
		char		repl_null[Natts_pg_proc];
		char		repl_repl[Natts_pg_proc];

		/* extract existing proconfig setting */
		datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_proconfig, &isnull);
		a = isnull ? NULL : DatumGetArrayTypeP(datum);

		/* update according to each SET or RESET item, left to right */
		a = update_proconfig_value(a, set_items);

		/* update the tuple */
		memset(repl_repl, ' ', sizeof(repl_repl));
		repl_repl[Anum_pg_proc_proconfig - 1] = 'r';

		if (a == NULL)
		{
			repl_val[Anum_pg_proc_proconfig - 1] = (Datum) 0;
			repl_null[Anum_pg_proc_proconfig - 1] = 'n';
		}
		else
		{
			repl_val[Anum_pg_proc_proconfig - 1] = PointerGetDatum(a);
			repl_null[Anum_pg_proc_proconfig - 1] = ' ';
		}

		tup = heap_modifytuple(tup, RelationGetDescr(rel),
							   repl_val, repl_null, repl_repl);
	}

	/* Do the update */
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/*
 * SetFunctionReturnType - change declared return type of a function
 *
 * This is presently only used for adjusting legacy functions that return
 * OPAQUE to return whatever we find their correct definition should be.
 * The caller should emit a suitable warning explaining what we did.
 */
void
SetFunctionReturnType(Oid funcOid, Oid newRetType)
{
	Relation	pg_proc_rel;
	HeapTuple	tup;
	Form_pg_proc procForm;

	pg_proc_rel = heap_open(ProcedureRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy(PROCOID,
							 ObjectIdGetDatum(funcOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", funcOid);
	procForm = (Form_pg_proc) GETSTRUCT(tup);

	if (procForm->prorettype != OPAQUEOID)		/* caller messed up */
		elog(ERROR, "function %u doesn't return OPAQUE", funcOid);

	/* okay to overwrite copied tuple */
	procForm->prorettype = newRetType;

	/* update the catalog and its indexes */
	simple_heap_update(pg_proc_rel, &tup->t_self, tup);

	CatalogUpdateIndexes(pg_proc_rel, tup);

	heap_close(pg_proc_rel, RowExclusiveLock);
}


/*
 * SetFunctionArgType - change declared argument type of a function
 *
 * As above, but change an argument's type.
 */
void
SetFunctionArgType(Oid funcOid, int argIndex, Oid newArgType)
{
	Relation	pg_proc_rel;
	HeapTuple	tup;
	Form_pg_proc procForm;

	pg_proc_rel = heap_open(ProcedureRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy(PROCOID,
							 ObjectIdGetDatum(funcOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", funcOid);
	procForm = (Form_pg_proc) GETSTRUCT(tup);

	if (argIndex < 0 || argIndex >= procForm->pronargs ||
		procForm->proargtypes.values[argIndex] != OPAQUEOID)
		elog(ERROR, "function %u doesn't take OPAQUE", funcOid);

	/* okay to overwrite copied tuple */
	procForm->proargtypes.values[argIndex] = newArgType;

	/* update the catalog and its indexes */
	simple_heap_update(pg_proc_rel, &tup->t_self, tup);

	CatalogUpdateIndexes(pg_proc_rel, tup);

	heap_close(pg_proc_rel, RowExclusiveLock);
}



/*
 * CREATE CAST
 */
void
CreateCast(CreateCastStmt *stmt)
{
	Oid			sourcetypeid;
	Oid			targettypeid;
	Oid			funcid;
	int			nargs;
	char		castcontext;
	Relation	relation;
	HeapTuple	tuple;
	Datum		values[Natts_pg_cast];
	char		nulls[Natts_pg_cast];
	ObjectAddress myself,
				referenced;

	sourcetypeid = typenameTypeId(NULL, stmt->sourcetype, NULL);
	targettypeid = typenameTypeId(NULL, stmt->targettype, NULL);

	/* No pseudo-types allowed */
	if (get_typtype(sourcetypeid) == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("source data type %s is a pseudo-type",
						TypeNameToString(stmt->sourcetype))));

	if (get_typtype(targettypeid) == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("target data type %s is a pseudo-type",
						TypeNameToString(stmt->targettype))));

	/* Permission check */
	if (!pg_type_ownercheck(sourcetypeid, GetUserId())
		&& !pg_type_ownercheck(targettypeid, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be owner of type %s or type %s",
						TypeNameToString(stmt->sourcetype),
						TypeNameToString(stmt->targettype))));

	if (stmt->func != NULL)
	{
		Form_pg_proc procstruct;

		funcid = LookupFuncNameTypeNames(stmt->func->funcname,
										 stmt->func->funcargs,
										 false);

		tuple = SearchSysCache(PROCOID,
							   ObjectIdGetDatum(funcid),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", funcid);

		procstruct = (Form_pg_proc) GETSTRUCT(tuple);
		nargs = procstruct->pronargs;
		if (nargs < 1 || nargs > 3)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				  errmsg("cast function must take one to three arguments")));
		if (procstruct->proargtypes.values[0] != sourcetypeid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			errmsg("argument of cast function must match source data type")));
		if (nargs > 1 && procstruct->proargtypes.values[1] != INT4OID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			errmsg("second argument of cast function must be type integer")));
		if (nargs > 2 && procstruct->proargtypes.values[2] != BOOLOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			errmsg("third argument of cast function must be type boolean")));
		if (procstruct->prorettype != targettypeid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("return data type of cast function must match target data type")));

		/*
		 * Restricting the volatility of a cast function may or may not be a
		 * good idea in the abstract, but it definitely breaks many old
		 * user-defined types.	Disable this check --- tgl 2/1/03
		 */
#ifdef NOT_USED
		if (procstruct->provolatile == PROVOLATILE_VOLATILE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cast function must not be volatile")));
#endif
		if (procstruct->proisagg)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("cast function must not be an aggregate function")));
		if (procstruct->proretset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cast function must not return a set")));

		ReleaseSysCache(tuple);
	}
	else
	{
		int16		typ1len;
		int16		typ2len;
		bool		typ1byval;
		bool		typ2byval;
		char		typ1align;
		char		typ2align;

		/* indicates binary coercibility */
		funcid = InvalidOid;
		nargs = 0;

		/*
		 * Must be superuser to create binary-compatible casts, since
		 * erroneous casts can easily crash the backend.
		 */
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("must be superuser to create a cast WITHOUT FUNCTION")));

		/*
		 * Also, insist that the types match as to size, alignment, and
		 * pass-by-value attributes; this provides at least a crude check that
		 * they have similar representations.  A pair of types that fail this
		 * test should certainly not be equated.
		 */
		get_typlenbyvalalign(sourcetypeid, &typ1len, &typ1byval, &typ1align);
		get_typlenbyvalalign(targettypeid, &typ2len, &typ2byval, &typ2align);
		if (typ1len != typ2len ||
			typ1byval != typ2byval ||
			typ1align != typ2align)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("source and target data types are not physically compatible")));
	}

	/*
	 * Allow source and target types to be same only for length coercion
	 * functions.  We assume a multi-arg function does length coercion.
	 */
	if (sourcetypeid == targettypeid && nargs < 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			  errmsg("source data type and target data type are the same")));

	/* convert CoercionContext enum to char value for castcontext */
	switch (stmt->context)
	{
		case COERCION_IMPLICIT:
			castcontext = COERCION_CODE_IMPLICIT;
			break;
		case COERCION_ASSIGNMENT:
			castcontext = COERCION_CODE_ASSIGNMENT;
			break;
		case COERCION_EXPLICIT:
			castcontext = COERCION_CODE_EXPLICIT;
			break;
		default:
			elog(ERROR, "unrecognized CoercionContext: %d", stmt->context);
			castcontext = 0;	/* keep compiler quiet */
			break;
	}

	relation = heap_open(CastRelationId, RowExclusiveLock);

	/*
	 * Check for duplicate.  This is just to give a friendly error message,
	 * the unique index would catch it anyway (so no need to sweat about race
	 * conditions).
	 */
	tuple = SearchSysCache(CASTSOURCETARGET,
						   ObjectIdGetDatum(sourcetypeid),
						   ObjectIdGetDatum(targettypeid),
						   0, 0);
	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("cast from type %s to type %s already exists",
						TypeNameToString(stmt->sourcetype),
						TypeNameToString(stmt->targettype))));

	/* ready to go */
	values[Anum_pg_cast_castsource - 1] = ObjectIdGetDatum(sourcetypeid);
	values[Anum_pg_cast_casttarget - 1] = ObjectIdGetDatum(targettypeid);
	values[Anum_pg_cast_castfunc - 1] = ObjectIdGetDatum(funcid);
	values[Anum_pg_cast_castcontext - 1] = CharGetDatum(castcontext);

	MemSet(nulls, ' ', Natts_pg_cast);

	tuple = heap_formtuple(RelationGetDescr(relation), values, nulls);

	simple_heap_insert(relation, tuple);

	CatalogUpdateIndexes(relation, tuple);

	/* make dependency entries */
	myself.classId = CastRelationId;
	myself.objectId = HeapTupleGetOid(tuple);
	myself.objectSubId = 0;

	/* dependency on source type */
	referenced.classId = TypeRelationId;
	referenced.objectId = sourcetypeid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on target type */
	referenced.classId = TypeRelationId;
	referenced.objectId = targettypeid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on function */
	if (OidIsValid(funcid))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = funcid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	heap_freetuple(tuple);

	heap_close(relation, RowExclusiveLock);
}



/*
 * DROP CAST
 */
void
DropCast(DropCastStmt *stmt)
{
	Oid			sourcetypeid;
	Oid			targettypeid;
	HeapTuple	tuple;
	ObjectAddress object;

	/* when dropping a cast, the types must exist even if you use IF EXISTS */
	sourcetypeid = typenameTypeId(NULL, stmt->sourcetype, NULL);
	targettypeid = typenameTypeId(NULL, stmt->targettype, NULL);

	tuple = SearchSysCache(CASTSOURCETARGET,
						   ObjectIdGetDatum(sourcetypeid),
						   ObjectIdGetDatum(targettypeid),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		if (!stmt->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("cast from type %s to type %s does not exist",
							TypeNameToString(stmt->sourcetype),
							TypeNameToString(stmt->targettype))));
		else
			ereport(NOTICE,
			 (errmsg("cast from type %s to type %s does not exist, skipping",
					 TypeNameToString(stmt->sourcetype),
					 TypeNameToString(stmt->targettype))));

		return;
	}

	/* Permission check */
	if (!pg_type_ownercheck(sourcetypeid, GetUserId())
		&& !pg_type_ownercheck(targettypeid, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be owner of type %s or type %s",
						TypeNameToString(stmt->sourcetype),
						TypeNameToString(stmt->targettype))));

	/*
	 * Do the deletion
	 */
	object.classId = CastRelationId;
	object.objectId = HeapTupleGetOid(tuple);
	object.objectSubId = 0;

	ReleaseSysCache(tuple);

	performDeletion(&object, stmt->behavior);
}


void
DropCastById(Oid castOid)
{
	Relation	relation;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tuple;

	relation = heap_open(CastRelationId, RowExclusiveLock);

	ScanKeyInit(&scankey,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(castOid));
	scan = systable_beginscan(relation, CastOidIndexId, true,
							  SnapshotNow, 1, &scankey);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for cast %u", castOid);
	simple_heap_delete(relation, &tuple->t_self);

	systable_endscan(scan);
	heap_close(relation, RowExclusiveLock);
}

/*
 * Execute ALTER FUNCTION/AGGREGATE SET SCHEMA
 *
 * These commands are identical except for the lookup procedure, so share code.
 */
void
AlterFunctionNamespace(List *name, List *argtypes, bool isagg,
					   const char *newschema)
{
	Oid			procOid;
	Oid			oldNspOid;
	Oid			nspOid;
	HeapTuple	tup;
	Relation	procRel;
	Form_pg_proc proc;

	procRel = heap_open(ProcedureRelationId, RowExclusiveLock);

	/* get function OID */
	if (isagg)
		procOid = LookupAggNameTypeNames(name, argtypes, false);
	else
		procOid = LookupFuncNameTypeNames(name, argtypes, false);

	/* check permissions on function */
	if (!pg_proc_ownercheck(procOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
					   NameListToString(name));

	tup = SearchSysCacheCopy(PROCOID,
							 ObjectIdGetDatum(procOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", procOid);
	proc = (Form_pg_proc) GETSTRUCT(tup);

	oldNspOid = proc->pronamespace;

	/* get schema OID and check its permissions */
	nspOid = LookupCreationNamespace(newschema);

	if (oldNspOid == nspOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("function \"%s\" is already in schema \"%s\"",
						NameListToString(name),
						newschema)));

	/* disallow renaming into or out of temp schemas */
	if (isAnyTempNamespace(nspOid) || isAnyTempNamespace(oldNspOid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("cannot move objects into or out of temporary schemas")));

	/* same for TOAST schema */
	if (nspOid == PG_TOAST_NAMESPACE || oldNspOid == PG_TOAST_NAMESPACE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move objects into or out of TOAST schema")));

	/* check for duplicate name (more friendly than unique-index failure) */
	if (SearchSysCacheExists(PROCNAMEARGSNSP,
							 CStringGetDatum(NameStr(proc->proname)),
							 PointerGetDatum(&proc->proargtypes),
							 ObjectIdGetDatum(nspOid),
							 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("function \"%s\" already exists in schema \"%s\"",
						NameStr(proc->proname),
						newschema)));

	/* OK, modify the pg_proc row */

	/* tup is a copy, so we can scribble directly on it */
	proc->pronamespace = nspOid;

	simple_heap_update(procRel, &tup->t_self, tup);
	CatalogUpdateIndexes(procRel, tup);

	/* Update dependency on schema */
	if (changeDependencyFor(ProcedureRelationId, procOid,
							NamespaceRelationId, oldNspOid, nspOid) != 1)
		elog(ERROR, "failed to change schema dependency for function \"%s\"",
			 NameListToString(name));

	heap_freetuple(tup);

	heap_close(procRel, RowExclusiveLock);
}
