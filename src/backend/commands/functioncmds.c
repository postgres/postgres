/*-------------------------------------------------------------------------
 *
 * functioncmds.c
 *
 *	  Routines for CREATE and DROP FUNCTION commands
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/functioncmds.c,v 1.3 2002/04/27 03:45:01 tgl Exp $
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

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/namespace.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 *	 Examine the "returns" clause returnType of the CREATE FUNCTION statement
 *	 and return information about it as *prorettype_p and *returnsSet.
 *
 * This is more complex than the average typename lookup because we want to
 * allow a shell type to be used, or even created if the specified return type
 * doesn't exist yet.  (Without this, there's no way to define the I/O procs
 * for a new type.)  But SQL function creation won't cope, so error out if
 * the target language is SQL.
 */
static void
compute_return_type(TypeName *returnType, Oid languageOid,
					Oid *prorettype_p, bool *returnsSet_p)
{
	Oid		rettype;

	rettype = LookupTypeName(returnType);

	if (OidIsValid(rettype))
	{
		if (!get_typisdefined(rettype))
		{
			if (languageOid == SQLlanguageId)
				elog(ERROR, "SQL functions cannot return shell types");
			else
				elog(WARNING, "Return type \"%s\" is only a shell",
					 TypeNameToString(returnType));
		}
	}
	else
	{
		char      *typnam = TypeNameToString(returnType);

		if (strcmp(typnam, "opaque") == 0)
			rettype = InvalidOid;
		else
		{
			Oid			namespaceId;
			AclResult	aclresult;
			char	   *typname;

			if (languageOid == SQLlanguageId)
				elog(ERROR, "Type \"%s\" does not exist", typnam);
			elog(WARNING, "ProcedureCreate: type %s is not yet defined",
				 typnam);
			namespaceId = QualifiedNameGetCreationNamespace(returnType->names,
															&typname);
			aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(),
											  ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, get_namespace_name(namespaceId));
			rettype = TypeShellMake(typname, namespaceId);
			if (!OidIsValid(rettype))
				elog(ERROR, "could not create type %s", typnam);
		}
	}

	*prorettype_p = rettype;
	*returnsSet_p = returnType->setof;
}

/*
 * Interpret the argument-types list of the CREATE FUNCTION statement.
 */
static int
compute_parameter_types(List *argTypes, Oid languageOid,
						Oid *parameterTypes)
{
	int			parameterCount = 0;
	List	   *x;

	MemSet(parameterTypes, 0, FUNC_MAX_ARGS * sizeof(Oid));
	foreach(x, argTypes)
	{
		TypeName   *t = (TypeName *) lfirst(x);
		Oid			toid;

		if (parameterCount >= FUNC_MAX_ARGS)
			elog(ERROR, "functions cannot have more than %d arguments",
				 FUNC_MAX_ARGS);

		toid = LookupTypeName(t);
		if (OidIsValid(toid))
		{
			if (!get_typisdefined(toid))
				elog(WARNING, "Argument type \"%s\" is only a shell",
					 TypeNameToString(t));
		}
		else
		{
			char      *typnam = TypeNameToString(t);

			if (strcmp(typnam, "opaque") == 0)
			{
				if (languageOid == SQLlanguageId)
					elog(ERROR, "SQL functions cannot have arguments of type \"opaque\"");
				toid = InvalidOid;
			}
			else
				elog(ERROR, "Type \"%s\" does not exist", typnam);
		}

		if (t->setof)
			elog(ERROR, "functions cannot accept set arguments");

		parameterTypes[parameterCount++] = toid;
	}

	return parameterCount;
}

/*-------------
 *	 Interpret the parameters *parameters and return their contents as
 *	 *byte_pct_p, etc.
 *
 *	These parameters supply optional information about a function.
 *	All have defaults if not specified.
 *
 *	Note: currently, only three of these parameters actually do anything:
 *
 *	 * isImplicit means the function may be used as an implicit type
 *	   coercion.
 *
 *	 * isStrict means the function should not be called when any NULL
 *	   inputs are present; instead a NULL result value should be assumed.
 *
 *	 * volatility tells the optimizer whether the function's result can
 *	   be assumed to be repeatable over multiple evaluations.
 *
 *	The other four parameters are not used anywhere.	They used to be
 *	used in the "expensive functions" optimizer, but that's been dead code
 *	for a long time.
 *------------
 */
static void
compute_full_attributes(List *parameters,
						int32 *byte_pct_p, int32 *perbyte_cpu_p,
						int32 *percall_cpu_p, int32 *outin_ratio_p,
						bool *isImplicit_p, bool *isStrict_p,
						char *volatility_p)
{
	List	   *pl;

	/* the defaults */
	*byte_pct_p = BYTE_PCT;
	*perbyte_cpu_p = PERBYTE_CPU;
	*percall_cpu_p = PERCALL_CPU;
	*outin_ratio_p = OUTIN_RATIO;
	*isImplicit_p = false;
	*isStrict_p = false;
	*volatility_p = PROVOLATILE_VOLATILE;

	foreach(pl, parameters)
	{
		DefElem    *param = (DefElem *) lfirst(pl);

		if (strcasecmp(param->defname, "implicitcoercion") == 0)
			*isImplicit_p = true;
		else if (strcasecmp(param->defname, "isstrict") == 0)
			*isStrict_p = true;
		else if (strcasecmp(param->defname, "isimmutable") == 0)
			*volatility_p = PROVOLATILE_IMMUTABLE;
		else if (strcasecmp(param->defname, "isstable") == 0)
			*volatility_p = PROVOLATILE_STABLE;
		else if (strcasecmp(param->defname, "isvolatile") == 0)
			*volatility_p = PROVOLATILE_VOLATILE;
		else if (strcasecmp(param->defname, "iscachable") == 0)
		{
			/* obsolete spelling of isImmutable */
			*volatility_p = PROVOLATILE_IMMUTABLE;
		}
		else if (strcasecmp(param->defname, "trusted") == 0)
		{
			/*
			 * we don't have untrusted functions any more. The 4.2
			 * implementation is lousy anyway so I took it out. -ay 10/94
			 */
			elog(ERROR, "untrusted function has been decommissioned.");
		}
		else if (strcasecmp(param->defname, "byte_pct") == 0)
			*byte_pct_p = (int) defGetNumeric(param);
		else if (strcasecmp(param->defname, "perbyte_cpu") == 0)
			*perbyte_cpu_p = (int) defGetNumeric(param);
		else if (strcasecmp(param->defname, "percall_cpu") == 0)
			*percall_cpu_p = (int) defGetNumeric(param);
		else if (strcasecmp(param->defname, "outin_ratio") == 0)
			*outin_ratio_p = (int) defGetNumeric(param);
		else
			elog(WARNING, "Unrecognized function attribute '%s' ignored",
				 param->defname);
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
 *
 */

static void
interpret_AS_clause(Oid languageOid, const char *languageName, const List *as,
					char **prosrc_str_p, char **probin_str_p)
{
	Assert(as != NIL);

	if (languageOid == ClanguageId)
	{
		/*
		 * For "C" language, store the file name in probin and, when
		 * given, the link symbol name in prosrc.
		 */
		*probin_str_p = strVal(lfirst(as));
		if (lnext(as) == NULL)
			*prosrc_str_p = "-";
		else
			*prosrc_str_p = strVal(lsecond(as));
	}
	else
	{
		/* Everything else wants the given string in prosrc. */
		*prosrc_str_p = strVal(lfirst(as));
		*probin_str_p = "-";

		if (lnext(as) != NIL)
			elog(ERROR, "CREATE FUNCTION: only one AS item needed for %s language",
				 languageName);
	}
}



/*
 * CreateFunction
 *	 Execute a CREATE FUNCTION utility statement.
 */
void
CreateFunction(ProcedureStmt *stmt)
{
	char	   *probin_str;
	char	   *prosrc_str;
	Oid			prorettype;
	bool		returnsSet;
	char		languageName[NAMEDATALEN];
	Oid			languageOid;
	char	   *funcname;
	Oid			namespaceId;
	AclResult	aclresult;
	int			parameterCount;
	Oid			parameterTypes[FUNC_MAX_ARGS];
	int32		byte_pct,
				perbyte_cpu,
				percall_cpu,
				outin_ratio;
	bool		isImplicit,
				isStrict;
	char		volatility;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->funcname,
													&funcname);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_namespace_name(namespaceId));

	/* Convert language name to canonical case */
	case_translate_language_name(stmt->language, languageName);

	/* Look up the language and validate permissions */
	languageTuple = SearchSysCache(LANGNAME,
								   PointerGetDatum(languageName),
								   0, 0, 0);
	if (!HeapTupleIsValid(languageTuple))
		elog(ERROR, "language \"%s\" does not exist", languageName);

	languageOid = languageTuple->t_data->t_oid;
	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);

	if (languageStruct->lanpltrusted)
	{
		/* if trusted language, need USAGE privilege */
		AclResult	aclresult;

		aclresult = pg_language_aclcheck(languageOid, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, NameStr(languageStruct->lanname));
	}
	else
	{
		/* if untrusted language, must be superuser */
		if (!superuser())
			aclcheck_error(ACLCHECK_NO_PRIV, NameStr(languageStruct->lanname));
	}

	ReleaseSysCache(languageTuple);

	/*
	 * Convert remaining parameters of CREATE to form wanted by
	 * ProcedureCreate.
	 */
	compute_return_type(stmt->returnType, languageOid,
						&prorettype, &returnsSet);

	parameterCount = compute_parameter_types(stmt->argTypes, languageOid,
											 parameterTypes);

	compute_full_attributes(stmt->withClause,
							&byte_pct, &perbyte_cpu, &percall_cpu,
							&outin_ratio, &isImplicit, &isStrict,
							&volatility);

	interpret_AS_clause(languageOid, languageName, stmt->as,
						&prosrc_str, &probin_str);

	/*
	 * And now that we have all the parameters, and know we're permitted
	 * to do so, go ahead and create the function.
	 */
	ProcedureCreate(funcname,
					namespaceId,
					stmt->replace,
					returnsSet,
					prorettype,
					languageOid,
					prosrc_str, /* converted to text later */
					probin_str, /* converted to text later */
					false,		/* not an aggregate */
					true,		/* (obsolete "trusted") */
					isImplicit,
					isStrict,
					volatility,
					byte_pct,
					perbyte_cpu,
					percall_cpu,
					outin_ratio,
					parameterCount,
					parameterTypes);
}


/*
 * RemoveFunction
 *		Deletes a function.
 *
 * Exceptions:
 *		BadArg if name is invalid.
 *		"ERROR" if function nonexistent.
 *		...
 */
void
RemoveFunction(List *functionName,		/* function name to be removed */
			   List *argTypes)	/* list of TypeName nodes */
{
	Oid			funcOid;
	Relation	relation;
	HeapTuple	tup;

	funcOid = LookupFuncNameTypeNames(functionName, argTypes, 
									  true, "RemoveFunction");

	relation = heap_openr(ProcedureRelationName, RowExclusiveLock);

	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(funcOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))	/* should not happen */
		elog(ERROR, "RemoveFunction: couldn't find tuple for function %s",
			 NameListToString(functionName));

	/* Permission check: must own func or its namespace */
	if (!pg_proc_ownercheck(funcOid, GetUserId()) &&
		!pg_namespace_ownercheck(((Form_pg_proc) GETSTRUCT(tup))->pronamespace,
								 GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, NameListToString(functionName));

	if (((Form_pg_proc) GETSTRUCT(tup))->proisagg)
		elog(ERROR, "RemoveFunction: function '%s' is an aggregate"
			 "\n\tUse DROP AGGREGATE to remove it",
			 NameListToString(functionName));

	if (((Form_pg_proc) GETSTRUCT(tup))->prolang == INTERNALlanguageId)
	{
		/* "Helpful" WARNING when removing a builtin function ... */
		elog(WARNING, "Removing built-in function \"%s\"",
			 NameListToString(functionName));
	}

	/* Delete any comments associated with this function */
	DeleteComments(funcOid, RelationGetRelid(relation));

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);
}
