/*-------------------------------------------------------------------------
 *
 * pg_proc.c
 *	  routines to support manipulation of the pg_proc relation
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_proc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"


typedef struct
{
	char	   *proname;
	char	   *prosrc;
} parse_error_callback_arg;

static void sql_function_parse_error_callback(void *arg);
static int	match_prosrc_to_query(const char *prosrc, const char *queryText,
								  int cursorpos);
static bool match_prosrc_to_literal(const char *prosrc, const char *literal,
									int cursorpos, int *newcursorpos);


/* ----------------------------------------------------------------
 *		ProcedureCreate
 *
 * Note: allParameterTypes, parameterModes, parameterNames, trftypes, and proconfig
 * are either arrays of the proper types or NULL.  We declare them Datum,
 * not "ArrayType *", to avoid importing array.h into pg_proc.h.
 * ----------------------------------------------------------------
 */
ObjectAddress
ProcedureCreate(const char *procedureName,
				Oid procNamespace,
				bool replace,
				bool returnsSet,
				Oid returnType,
				Oid proowner,
				Oid languageObjectId,
				Oid languageValidator,
				const char *prosrc,
				const char *probin,
				char prokind,
				bool security_definer,
				bool isLeakProof,
				bool isStrict,
				char volatility,
				char parallel,
				oidvector *parameterTypes,
				Datum allParameterTypes,
				Datum parameterModes,
				Datum parameterNames,
				List *parameterDefaults,
				Datum trftypes,
				Datum proconfig,
				Oid prosupport,
				float4 procost,
				float4 prorows)
{
	Oid			retval;
	int			parameterCount;
	int			allParamCount;
	Oid		   *allParams;
	char	   *paramModes = NULL;
	Oid			variadicType = InvalidOid;
	Acl		   *proacl = NULL;
	Relation	rel;
	HeapTuple	tup;
	HeapTuple	oldtup;
	bool		nulls[Natts_pg_proc];
	Datum		values[Natts_pg_proc];
	bool		replaces[Natts_pg_proc];
	NameData	procname;
	TupleDesc	tupDesc;
	bool		is_update;
	ObjectAddress myself,
				referenced;
	char	   *detailmsg;
	int			i;
	Oid			trfid;

	/*
	 * sanity checks
	 */
	Assert(PointerIsValid(prosrc));

	parameterCount = parameterTypes->dim1;
	if (parameterCount < 0 || parameterCount > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("functions cannot have more than %d argument",
							   "functions cannot have more than %d arguments",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));
	/* note: the above is correct, we do NOT count output arguments */

	/* Deconstruct array inputs */
	if (allParameterTypes != PointerGetDatum(NULL))
	{
		/*
		 * We expect the array to be a 1-D OID array; verify that. We don't
		 * need to use deconstruct_array() since the array data is just going
		 * to look like a C array of OID values.
		 */
		ArrayType  *allParamArray = (ArrayType *) DatumGetPointer(allParameterTypes);

		allParamCount = ARR_DIMS(allParamArray)[0];
		if (ARR_NDIM(allParamArray) != 1 ||
			allParamCount <= 0 ||
			ARR_HASNULL(allParamArray) ||
			ARR_ELEMTYPE(allParamArray) != OIDOID)
			elog(ERROR, "allParameterTypes is not a 1-D Oid array");
		allParams = (Oid *) ARR_DATA_PTR(allParamArray);
		Assert(allParamCount >= parameterCount);
		/* we assume caller got the contents right */
	}
	else
	{
		allParamCount = parameterCount;
		allParams = parameterTypes->values;
	}

	if (parameterModes != PointerGetDatum(NULL))
	{
		/*
		 * We expect the array to be a 1-D CHAR array; verify that. We don't
		 * need to use deconstruct_array() since the array data is just going
		 * to look like a C array of char values.
		 */
		ArrayType  *modesArray = (ArrayType *) DatumGetPointer(parameterModes);

		if (ARR_NDIM(modesArray) != 1 ||
			ARR_DIMS(modesArray)[0] != allParamCount ||
			ARR_HASNULL(modesArray) ||
			ARR_ELEMTYPE(modesArray) != CHAROID)
			elog(ERROR, "parameterModes is not a 1-D char array");
		paramModes = (char *) ARR_DATA_PTR(modesArray);
	}

	/*
	 * Do not allow polymorphic return type unless there is a polymorphic
	 * input argument that we can use to deduce the actual return type.
	 */
	detailmsg = check_valid_polymorphic_signature(returnType,
												  parameterTypes->values,
												  parameterCount);
	if (detailmsg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("cannot determine result data type"),
				 errdetail_internal("%s", detailmsg)));

	/*
	 * Also, do not allow return type INTERNAL unless at least one input
	 * argument is INTERNAL.
	 */
	detailmsg = check_valid_internal_signature(returnType,
											   parameterTypes->values,
											   parameterCount);
	if (detailmsg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("unsafe use of pseudo-type \"internal\""),
				 errdetail_internal("%s", detailmsg)));

	/*
	 * Apply the same tests to any OUT arguments.
	 */
	if (allParameterTypes != PointerGetDatum(NULL))
	{
		for (i = 0; i < allParamCount; i++)
		{
			if (paramModes == NULL ||
				paramModes[i] == PROARGMODE_IN ||
				paramModes[i] == PROARGMODE_VARIADIC)
				continue;		/* ignore input-only params */

			detailmsg = check_valid_polymorphic_signature(allParams[i],
														  parameterTypes->values,
														  parameterCount);
			if (detailmsg)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("cannot determine result data type"),
						 errdetail_internal("%s", detailmsg)));
			detailmsg = check_valid_internal_signature(allParams[i],
													   parameterTypes->values,
													   parameterCount);
			if (detailmsg)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("unsafe use of pseudo-type \"internal\""),
						 errdetail_internal("%s", detailmsg)));
		}
	}

	/* Identify variadic argument type, if any */
	if (paramModes != NULL)
	{
		/*
		 * Only the last input parameter can be variadic; if it is, save its
		 * element type.  Errors here are just elog since caller should have
		 * checked this already.
		 */
		for (i = 0; i < allParamCount; i++)
		{
			switch (paramModes[i])
			{
				case PROARGMODE_IN:
				case PROARGMODE_INOUT:
					if (OidIsValid(variadicType))
						elog(ERROR, "variadic parameter must be last");
					break;
				case PROARGMODE_OUT:
				case PROARGMODE_TABLE:
					/* okay */
					break;
				case PROARGMODE_VARIADIC:
					if (OidIsValid(variadicType))
						elog(ERROR, "variadic parameter must be last");
					switch (allParams[i])
					{
						case ANYOID:
							variadicType = ANYOID;
							break;
						case ANYARRAYOID:
							variadicType = ANYELEMENTOID;
							break;
						case ANYCOMPATIBLEARRAYOID:
							variadicType = ANYCOMPATIBLEOID;
							break;
						default:
							variadicType = get_element_type(allParams[i]);
							if (!OidIsValid(variadicType))
								elog(ERROR, "variadic parameter is not an array");
							break;
					}
					break;
				default:
					elog(ERROR, "invalid parameter mode '%c'", paramModes[i]);
					break;
			}
		}
	}

	/*
	 * All seems OK; prepare the data to be inserted into pg_proc.
	 */

	for (i = 0; i < Natts_pg_proc; ++i)
	{
		nulls[i] = false;
		values[i] = (Datum) 0;
		replaces[i] = true;
	}

	namestrcpy(&procname, procedureName);
	values[Anum_pg_proc_proname - 1] = NameGetDatum(&procname);
	values[Anum_pg_proc_pronamespace - 1] = ObjectIdGetDatum(procNamespace);
	values[Anum_pg_proc_proowner - 1] = ObjectIdGetDatum(proowner);
	values[Anum_pg_proc_prolang - 1] = ObjectIdGetDatum(languageObjectId);
	values[Anum_pg_proc_procost - 1] = Float4GetDatum(procost);
	values[Anum_pg_proc_prorows - 1] = Float4GetDatum(prorows);
	values[Anum_pg_proc_provariadic - 1] = ObjectIdGetDatum(variadicType);
	values[Anum_pg_proc_prosupport - 1] = ObjectIdGetDatum(prosupport);
	values[Anum_pg_proc_prokind - 1] = CharGetDatum(prokind);
	values[Anum_pg_proc_prosecdef - 1] = BoolGetDatum(security_definer);
	values[Anum_pg_proc_proleakproof - 1] = BoolGetDatum(isLeakProof);
	values[Anum_pg_proc_proisstrict - 1] = BoolGetDatum(isStrict);
	values[Anum_pg_proc_proretset - 1] = BoolGetDatum(returnsSet);
	values[Anum_pg_proc_provolatile - 1] = CharGetDatum(volatility);
	values[Anum_pg_proc_proparallel - 1] = CharGetDatum(parallel);
	values[Anum_pg_proc_pronargs - 1] = UInt16GetDatum(parameterCount);
	values[Anum_pg_proc_pronargdefaults - 1] = UInt16GetDatum(list_length(parameterDefaults));
	values[Anum_pg_proc_prorettype - 1] = ObjectIdGetDatum(returnType);
	values[Anum_pg_proc_proargtypes - 1] = PointerGetDatum(parameterTypes);
	if (allParameterTypes != PointerGetDatum(NULL))
		values[Anum_pg_proc_proallargtypes - 1] = allParameterTypes;
	else
		nulls[Anum_pg_proc_proallargtypes - 1] = true;
	if (parameterModes != PointerGetDatum(NULL))
		values[Anum_pg_proc_proargmodes - 1] = parameterModes;
	else
		nulls[Anum_pg_proc_proargmodes - 1] = true;
	if (parameterNames != PointerGetDatum(NULL))
		values[Anum_pg_proc_proargnames - 1] = parameterNames;
	else
		nulls[Anum_pg_proc_proargnames - 1] = true;
	if (parameterDefaults != NIL)
		values[Anum_pg_proc_proargdefaults - 1] = CStringGetTextDatum(nodeToString(parameterDefaults));
	else
		nulls[Anum_pg_proc_proargdefaults - 1] = true;
	if (trftypes != PointerGetDatum(NULL))
		values[Anum_pg_proc_protrftypes - 1] = trftypes;
	else
		nulls[Anum_pg_proc_protrftypes - 1] = true;
	values[Anum_pg_proc_prosrc - 1] = CStringGetTextDatum(prosrc);
	if (probin)
		values[Anum_pg_proc_probin - 1] = CStringGetTextDatum(probin);
	else
		nulls[Anum_pg_proc_probin - 1] = true;
	if (proconfig != PointerGetDatum(NULL))
		values[Anum_pg_proc_proconfig - 1] = proconfig;
	else
		nulls[Anum_pg_proc_proconfig - 1] = true;
	/* proacl will be determined later */

	rel = table_open(ProcedureRelationId, RowExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	/* Check for pre-existing definition */
	oldtup = SearchSysCache3(PROCNAMEARGSNSP,
							 PointerGetDatum(procedureName),
							 PointerGetDatum(parameterTypes),
							 ObjectIdGetDatum(procNamespace));

	if (HeapTupleIsValid(oldtup))
	{
		/* There is one; okay to replace it? */
		Form_pg_proc oldproc = (Form_pg_proc) GETSTRUCT(oldtup);
		Datum		proargnames;
		bool		isnull;
		const char *dropcmd;

		if (!replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_FUNCTION),
					 errmsg("function \"%s\" already exists with same argument types",
							procedureName)));
		if (!pg_proc_ownercheck(oldproc->oid, proowner))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
						   procedureName);

		/* Not okay to change routine kind */
		if (oldproc->prokind != prokind)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change routine kind"),
					 (oldproc->prokind == PROKIND_AGGREGATE ?
					  errdetail("\"%s\" is an aggregate function.", procedureName) :
					  oldproc->prokind == PROKIND_FUNCTION ?
					  errdetail("\"%s\" is a function.", procedureName) :
					  oldproc->prokind == PROKIND_PROCEDURE ?
					  errdetail("\"%s\" is a procedure.", procedureName) :
					  oldproc->prokind == PROKIND_WINDOW ?
					  errdetail("\"%s\" is a window function.", procedureName) :
					  0)));

		dropcmd = (prokind == PROKIND_PROCEDURE ? "DROP PROCEDURE" :
				   prokind == PROKIND_AGGREGATE ? "DROP AGGREGATE" :
				   "DROP FUNCTION");

		/*
		 * Not okay to change the return type of the existing proc, since
		 * existing rules, views, etc may depend on the return type.
		 *
		 * In case of a procedure, a changing return type means that whether
		 * the procedure has output parameters was changed.  Since there is no
		 * user visible return type, we produce a more specific error message.
		 */
		if (returnType != oldproc->prorettype ||
			returnsSet != oldproc->proretset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 prokind == PROKIND_PROCEDURE
					 ? errmsg("cannot change whether a procedure has output parameters")
					 : errmsg("cannot change return type of existing function"),

			/*
			 * translator: first %s is DROP FUNCTION, DROP PROCEDURE, or DROP
			 * AGGREGATE
			 */
					 errhint("Use %s %s first.",
							 dropcmd,
							 format_procedure(oldproc->oid))));

		/*
		 * If it returns RECORD, check for possible change of record type
		 * implied by OUT parameters
		 */
		if (returnType == RECORDOID)
		{
			TupleDesc	olddesc;
			TupleDesc	newdesc;

			olddesc = build_function_result_tupdesc_t(oldtup);
			newdesc = build_function_result_tupdesc_d(prokind,
													  allParameterTypes,
													  parameterModes,
													  parameterNames);
			if (olddesc == NULL && newdesc == NULL)
				 /* ok, both are runtime-defined RECORDs */ ;
			else if (olddesc == NULL || newdesc == NULL ||
					 !equalTupleDescs(olddesc, newdesc))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("cannot change return type of existing function"),
						 errdetail("Row type defined by OUT parameters is different."),
				/* translator: first %s is DROP FUNCTION or DROP PROCEDURE */
						 errhint("Use %s %s first.",
								 dropcmd,
								 format_procedure(oldproc->oid))));
		}

		/*
		 * If there were any named input parameters, check to make sure the
		 * names have not been changed, as this could break existing calls. We
		 * allow adding names to formerly unnamed parameters, though.
		 */
		proargnames = SysCacheGetAttr(PROCNAMEARGSNSP, oldtup,
									  Anum_pg_proc_proargnames,
									  &isnull);
		if (!isnull)
		{
			Datum		proargmodes;
			char	  **old_arg_names;
			char	  **new_arg_names;
			int			n_old_arg_names;
			int			n_new_arg_names;
			int			j;

			proargmodes = SysCacheGetAttr(PROCNAMEARGSNSP, oldtup,
										  Anum_pg_proc_proargmodes,
										  &isnull);
			if (isnull)
				proargmodes = PointerGetDatum(NULL);	/* just to be sure */

			n_old_arg_names = get_func_input_arg_names(proargnames,
													   proargmodes,
													   &old_arg_names);
			n_new_arg_names = get_func_input_arg_names(parameterNames,
													   parameterModes,
													   &new_arg_names);
			for (j = 0; j < n_old_arg_names; j++)
			{
				if (old_arg_names[j] == NULL)
					continue;
				if (j >= n_new_arg_names || new_arg_names[j] == NULL ||
					strcmp(old_arg_names[j], new_arg_names[j]) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("cannot change name of input parameter \"%s\"",
									old_arg_names[j]),
					/* translator: first %s is DROP FUNCTION or DROP PROCEDURE */
							 errhint("Use %s %s first.",
									 dropcmd,
									 format_procedure(oldproc->oid))));
			}
		}

		/*
		 * If there are existing defaults, check compatibility: redefinition
		 * must not remove any defaults nor change their types.  (Removing a
		 * default might cause a function to fail to satisfy an existing call.
		 * Changing type would only be possible if the associated parameter is
		 * polymorphic, and in such cases a change of default type might alter
		 * the resolved output type of existing calls.)
		 */
		if (oldproc->pronargdefaults != 0)
		{
			Datum		proargdefaults;
			List	   *oldDefaults;
			ListCell   *oldlc;
			ListCell   *newlc;

			if (list_length(parameterDefaults) < oldproc->pronargdefaults)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("cannot remove parameter defaults from existing function"),
				/* translator: first %s is DROP FUNCTION or DROP PROCEDURE */
						 errhint("Use %s %s first.",
								 dropcmd,
								 format_procedure(oldproc->oid))));

			proargdefaults = SysCacheGetAttr(PROCNAMEARGSNSP, oldtup,
											 Anum_pg_proc_proargdefaults,
											 &isnull);
			Assert(!isnull);
			oldDefaults = castNode(List, stringToNode(TextDatumGetCString(proargdefaults)));
			Assert(list_length(oldDefaults) == oldproc->pronargdefaults);

			/* new list can have more defaults than old, advance over 'em */
			newlc = list_nth_cell(parameterDefaults,
								  list_length(parameterDefaults) -
								  oldproc->pronargdefaults);

			foreach(oldlc, oldDefaults)
			{
				Node	   *oldDef = (Node *) lfirst(oldlc);
				Node	   *newDef = (Node *) lfirst(newlc);

				if (exprType(oldDef) != exprType(newDef))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("cannot change data type of existing parameter default value"),
					/* translator: first %s is DROP FUNCTION or DROP PROCEDURE */
							 errhint("Use %s %s first.",
									 dropcmd,
									 format_procedure(oldproc->oid))));
				newlc = lnext(parameterDefaults, newlc);
			}
		}

		/*
		 * Do not change existing oid, ownership or permissions, either.  Note
		 * dependency-update code below has to agree with this decision.
		 */
		replaces[Anum_pg_proc_oid - 1] = false;
		replaces[Anum_pg_proc_proowner - 1] = false;
		replaces[Anum_pg_proc_proacl - 1] = false;

		/* Okay, do it... */
		tup = heap_modify_tuple(oldtup, tupDesc, values, nulls, replaces);
		CatalogTupleUpdate(rel, &tup->t_self, tup);

		ReleaseSysCache(oldtup);
		is_update = true;
	}
	else
	{
		/* Creating a new procedure */
		Oid			newOid;

		/* First, get default permissions and set up proacl */
		proacl = get_user_default_acl(OBJECT_FUNCTION, proowner,
									  procNamespace);
		if (proacl != NULL)
			values[Anum_pg_proc_proacl - 1] = PointerGetDatum(proacl);
		else
			nulls[Anum_pg_proc_proacl - 1] = true;

		newOid = GetNewOidWithIndex(rel, ProcedureOidIndexId,
									Anum_pg_proc_oid);
		values[Anum_pg_proc_oid - 1] = ObjectIdGetDatum(newOid);
		tup = heap_form_tuple(tupDesc, values, nulls);
		CatalogTupleInsert(rel, tup);
		is_update = false;
	}


	retval = ((Form_pg_proc) GETSTRUCT(tup))->oid;

	/*
	 * Create dependencies for the new function.  If we are updating an
	 * existing function, first delete any existing pg_depend entries.
	 * (However, since we are not changing ownership or permissions, the
	 * shared dependencies do *not* need to change, and we leave them alone.)
	 */
	if (is_update)
		deleteDependencyRecordsFor(ProcedureRelationId, retval, true);

	myself.classId = ProcedureRelationId;
	myself.objectId = retval;
	myself.objectSubId = 0;

	/* dependency on namespace */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = procNamespace;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on implementation language */
	referenced.classId = LanguageRelationId;
	referenced.objectId = languageObjectId;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on return type */
	referenced.classId = TypeRelationId;
	referenced.objectId = returnType;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on transform used by return type, if any */
	if ((trfid = get_transform_oid(returnType, languageObjectId, true)))
	{
		referenced.classId = TransformRelationId;
		referenced.objectId = trfid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* dependency on parameter types */
	for (i = 0; i < allParamCount; i++)
	{
		referenced.classId = TypeRelationId;
		referenced.objectId = allParams[i];
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

		/* dependency on transform used by parameter type, if any */
		if ((trfid = get_transform_oid(allParams[i], languageObjectId, true)))
		{
			referenced.classId = TransformRelationId;
			referenced.objectId = trfid;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	/* dependency on parameter default expressions */
	if (parameterDefaults)
		recordDependencyOnExpr(&myself, (Node *) parameterDefaults,
							   NIL, DEPENDENCY_NORMAL);

	/* dependency on support function, if any */
	if (OidIsValid(prosupport))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = prosupport;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* dependency on owner */
	if (!is_update)
		recordDependencyOnOwner(ProcedureRelationId, retval, proowner);

	/* dependency on any roles mentioned in ACL */
	if (!is_update)
		recordDependencyOnNewAcl(ProcedureRelationId, retval, 0,
								 proowner, proacl);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself, is_update);

	heap_freetuple(tup);

	/* Post creation hook for new function */
	InvokeObjectPostCreateHook(ProcedureRelationId, retval, 0);

	table_close(rel, RowExclusiveLock);

	/* Verify function body */
	if (OidIsValid(languageValidator))
	{
		ArrayType  *set_items = NULL;
		int			save_nestlevel = 0;

		/* Advance command counter so new tuple can be seen by validator */
		CommandCounterIncrement();

		/*
		 * Set per-function configuration parameters so that the validation is
		 * done with the environment the function expects.  However, if
		 * check_function_bodies is off, we don't do this, because that would
		 * create dump ordering hazards that pg_dump doesn't know how to deal
		 * with.  (For example, a SET clause might refer to a not-yet-created
		 * text search configuration.)	This means that the validator
		 * shouldn't complain about anything that might depend on a GUC
		 * parameter when check_function_bodies is off.
		 */
		if (check_function_bodies)
		{
			set_items = (ArrayType *) DatumGetPointer(proconfig);
			if (set_items)		/* Need a new GUC nesting level */
			{
				save_nestlevel = NewGUCNestLevel();
				ProcessGUCArray(set_items,
								(superuser() ? PGC_SUSET : PGC_USERSET),
								PGC_S_SESSION,
								GUC_ACTION_SAVE);
			}
		}

		OidFunctionCall1(languageValidator, ObjectIdGetDatum(retval));

		if (set_items)
			AtEOXact_GUC(true, save_nestlevel);
	}

	return myself;
}



/*
 * Validator for internal functions
 *
 * Check that the given internal function name (the "prosrc" value) is
 * a known builtin function.
 */
Datum
fmgr_internal_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	bool		isnull;
	Datum		tmp;
	char	   *prosrc;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/*
	 * We do not honor check_function_bodies since it's unlikely the function
	 * name will be found later if it isn't there now.
	 */

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);

	tmp = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");
	prosrc = TextDatumGetCString(tmp);

	if (fmgr_internal_function(prosrc) == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("there is no built-in function named \"%s\"",
						prosrc)));

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}



/*
 * Validator for C language functions
 *
 * Make sure that the library file exists, is loadable, and contains
 * the specified link symbol. Also check for a valid function
 * information record.
 */
Datum
fmgr_c_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	void	   *libraryhandle;
	HeapTuple	tuple;
	bool		isnull;
	Datum		tmp;
	char	   *prosrc;
	char	   *probin;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/*
	 * It'd be most consistent to skip the check if !check_function_bodies,
	 * but the purpose of that switch is to be helpful for pg_dump loading,
	 * and for pg_dump loading it's much better if we *do* check.
	 */

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);

	tmp = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc for C function %u", funcoid);
	prosrc = TextDatumGetCString(tmp);

	tmp = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_probin, &isnull);
	if (isnull)
		elog(ERROR, "null probin for C function %u", funcoid);
	probin = TextDatumGetCString(tmp);

	(void) load_external_function(probin, prosrc, true, &libraryhandle);
	(void) fetch_finfo_record(libraryhandle, prosrc);

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}


/*
 * Validator for SQL language functions
 *
 * Parse it here in order to be sure that it contains no syntax errors.
 */
Datum
fmgr_sql_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc proc;
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	ListCell   *lc;
	bool		isnull;
	Datum		tmp;
	char	   *prosrc;
	parse_error_callback_arg callback_arg;
	ErrorContextCallback sqlerrcontext;
	bool		haspolyarg;
	int			i;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	/* Disallow pseudotype result */
	/* except for RECORD, VOID, or polymorphic */
	if (get_typtype(proc->prorettype) == TYPTYPE_PSEUDO &&
		proc->prorettype != RECORDOID &&
		proc->prorettype != VOIDOID &&
		!IsPolymorphicType(proc->prorettype))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("SQL functions cannot return type %s",
						format_type_be(proc->prorettype))));

	/* Disallow pseudotypes in arguments */
	/* except for polymorphic */
	haspolyarg = false;
	for (i = 0; i < proc->pronargs; i++)
	{
		if (get_typtype(proc->proargtypes.values[i]) == TYPTYPE_PSEUDO)
		{
			if (IsPolymorphicType(proc->proargtypes.values[i]))
				haspolyarg = true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SQL functions cannot have arguments of type %s",
								format_type_be(proc->proargtypes.values[i]))));
		}
	}

	/* Postpone body checks if !check_function_bodies */
	if (check_function_bodies)
	{
		tmp = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");

		prosrc = TextDatumGetCString(tmp);

		/*
		 * Setup error traceback support for ereport().
		 */
		callback_arg.proname = NameStr(proc->proname);
		callback_arg.prosrc = prosrc;

		sqlerrcontext.callback = sql_function_parse_error_callback;
		sqlerrcontext.arg = (void *) &callback_arg;
		sqlerrcontext.previous = error_context_stack;
		error_context_stack = &sqlerrcontext;

		/*
		 * We can't do full prechecking of the function definition if there
		 * are any polymorphic input types, because actual datatypes of
		 * expression results will be unresolvable.  The check will be done at
		 * runtime instead.
		 *
		 * We can run the text through the raw parser though; this will at
		 * least catch silly syntactic errors.
		 */
		raw_parsetree_list = pg_parse_query(prosrc);

		if (!haspolyarg)
		{
			/*
			 * OK to do full precheck: analyze and rewrite the queries, then
			 * verify the result type.
			 */
			SQLFunctionParseInfoPtr pinfo;
			Oid			rettype;
			TupleDesc	rettupdesc;

			/* But first, set up parameter information */
			pinfo = prepare_sql_fn_parse_info(tuple, NULL, InvalidOid);

			querytree_list = NIL;
			foreach(lc, raw_parsetree_list)
			{
				RawStmt    *parsetree = lfirst_node(RawStmt, lc);
				List	   *querytree_sublist;

				querytree_sublist = pg_analyze_and_rewrite_params(parsetree,
																  prosrc,
																  (ParserSetupHook) sql_fn_parser_setup,
																  pinfo,
																  NULL);
				querytree_list = lappend(querytree_list,
										 querytree_sublist);
			}

			check_sql_fn_statements(querytree_list);

			(void) get_func_result_type(funcoid, &rettype, &rettupdesc);

			(void) check_sql_fn_retval(querytree_list,
									   rettype, rettupdesc,
									   false, NULL);
		}

		error_context_stack = sqlerrcontext.previous;
	}

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}

/*
 * Error context callback for handling errors in SQL function definitions
 */
static void
sql_function_parse_error_callback(void *arg)
{
	parse_error_callback_arg *callback_arg = (parse_error_callback_arg *) arg;

	/* See if it's a syntax error; if so, transpose to CREATE FUNCTION */
	if (!function_parse_error_transpose(callback_arg->prosrc))
	{
		/* If it's not a syntax error, push info onto context stack */
		errcontext("SQL function \"%s\"", callback_arg->proname);
	}
}

/*
 * Adjust a syntax error occurring inside the function body of a CREATE
 * FUNCTION or DO command.  This can be used by any function validator or
 * anonymous-block handler, not only for SQL-language functions.
 * It is assumed that the syntax error position is initially relative to the
 * function body string (as passed in).  If possible, we adjust the position
 * to reference the original command text; if we can't manage that, we set
 * up an "internal query" syntax error instead.
 *
 * Returns true if a syntax error was processed, false if not.
 */
bool
function_parse_error_transpose(const char *prosrc)
{
	int			origerrposition;
	int			newerrposition;

	/*
	 * Nothing to do unless we are dealing with a syntax error that has a
	 * cursor position.
	 *
	 * Some PLs may prefer to report the error position as an internal error
	 * to begin with, so check that too.
	 */
	origerrposition = geterrposition();
	if (origerrposition <= 0)
	{
		origerrposition = getinternalerrposition();
		if (origerrposition <= 0)
			return false;
	}

	/* We can get the original query text from the active portal (hack...) */
	if (ActivePortal && ActivePortal->status == PORTAL_ACTIVE)
	{
		const char *queryText = ActivePortal->sourceText;

		/* Try to locate the prosrc in the original text */
		newerrposition = match_prosrc_to_query(prosrc, queryText,
											   origerrposition);
	}
	else
	{
		/*
		 * Quietly give up if no ActivePortal.  This is an unusual situation
		 * but it can happen in, e.g., logical replication workers.
		 */
		newerrposition = -1;
	}

	if (newerrposition > 0)
	{
		/* Successful, so fix error position to reference original query */
		errposition(newerrposition);
		/* Get rid of any report of the error as an "internal query" */
		internalerrposition(0);
		internalerrquery(NULL);
	}
	else
	{
		/*
		 * If unsuccessful, convert the position to an internal position
		 * marker and give the function text as the internal query.
		 */
		errposition(0);
		internalerrposition(origerrposition);
		internalerrquery(prosrc);
	}

	return true;
}

/*
 * Try to locate the string literal containing the function body in the
 * given text of the CREATE FUNCTION or DO command.  If successful, return
 * the character (not byte) index within the command corresponding to the
 * given character index within the literal.  If not successful, return 0.
 */
static int
match_prosrc_to_query(const char *prosrc, const char *queryText,
					  int cursorpos)
{
	/*
	 * Rather than fully parsing the original command, we just scan the
	 * command looking for $prosrc$ or 'prosrc'.  This could be fooled (though
	 * not in any very probable scenarios), so fail if we find more than one
	 * match.
	 */
	int			prosrclen = strlen(prosrc);
	int			querylen = strlen(queryText);
	int			matchpos = 0;
	int			curpos;
	int			newcursorpos;

	for (curpos = 0; curpos < querylen - prosrclen; curpos++)
	{
		if (queryText[curpos] == '$' &&
			strncmp(prosrc, &queryText[curpos + 1], prosrclen) == 0 &&
			queryText[curpos + 1 + prosrclen] == '$')
		{
			/*
			 * Found a $foo$ match.  Since there are no embedded quoting
			 * characters in a dollar-quoted literal, we don't have to do any
			 * fancy arithmetic; just offset by the starting position.
			 */
			if (matchpos)
				return 0;		/* multiple matches, fail */
			matchpos = pg_mbstrlen_with_len(queryText, curpos + 1)
				+ cursorpos;
		}
		else if (queryText[curpos] == '\'' &&
				 match_prosrc_to_literal(prosrc, &queryText[curpos + 1],
										 cursorpos, &newcursorpos))
		{
			/*
			 * Found a 'foo' match.  match_prosrc_to_literal() has adjusted
			 * for any quotes or backslashes embedded in the literal.
			 */
			if (matchpos)
				return 0;		/* multiple matches, fail */
			matchpos = pg_mbstrlen_with_len(queryText, curpos + 1)
				+ newcursorpos;
		}
	}

	return matchpos;
}

/*
 * Try to match the given source text to a single-quoted literal.
 * If successful, adjust newcursorpos to correspond to the character
 * (not byte) index corresponding to cursorpos in the source text.
 *
 * At entry, literal points just past a ' character.  We must check for the
 * trailing quote.
 */
static bool
match_prosrc_to_literal(const char *prosrc, const char *literal,
						int cursorpos, int *newcursorpos)
{
	int			newcp = cursorpos;
	int			chlen;

	/*
	 * This implementation handles backslashes and doubled quotes in the
	 * string literal.  It does not handle the SQL syntax for literals
	 * continued across line boundaries.
	 *
	 * We do the comparison a character at a time, not a byte at a time, so
	 * that we can do the correct cursorpos math.
	 */
	while (*prosrc)
	{
		cursorpos--;			/* characters left before cursor */

		/*
		 * Check for backslashes and doubled quotes in the literal; adjust
		 * newcp when one is found before the cursor.
		 */
		if (*literal == '\\')
		{
			literal++;
			if (cursorpos > 0)
				newcp++;
		}
		else if (*literal == '\'')
		{
			if (literal[1] != '\'')
				goto fail;
			literal++;
			if (cursorpos > 0)
				newcp++;
		}
		chlen = pg_mblen(prosrc);
		if (strncmp(prosrc, literal, chlen) != 0)
			goto fail;
		prosrc += chlen;
		literal += chlen;
	}

	if (*literal == '\'' && literal[1] != '\'')
	{
		/* success */
		*newcursorpos = newcp;
		return true;
	}

fail:
	/* Must set *newcursorpos to suppress compiler warning */
	*newcursorpos = newcp;
	return false;
}

List *
oid_array_to_list(Datum datum)
{
	ArrayType  *array = DatumGetArrayTypeP(datum);
	Datum	   *values;
	int			nelems;
	int			i;
	List	   *result = NIL;

	deconstruct_array(array,
					  OIDOID,
					  sizeof(Oid), true, TYPALIGN_INT,
					  &values, NULL, &nelems);
	for (i = 0; i < nelems; i++)
		result = lappend_oid(result, values[i]);
	return result;
}
