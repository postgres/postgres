/*-------------------------------------------------------------------------
 *
 * regproc.c
 *	  Functions for the built-in types regproc, regclass, regtype, etc.
 *
 * These types are all binary-compatible with type Oid, and rely on Oid
 * for comparison and so forth.  Their only interesting behavior is in
 * special I/O conversion routines.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/regproc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "parser/scansup.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

static void parseNameAndArgTypes(const char *string, bool allowNone,
								 List **names, int *nargs, Oid *argtypes);


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 * regprocin		- converts "proname" to proc OID
 *
 * We also accept a numeric OID, for symmetry with the output routine.
 *
 * '-' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_proc entry.
 */
Datum
regprocin(PG_FUNCTION_ARGS)
{
	char	   *pro_name_or_oid = PG_GETARG_CSTRING(0);
	RegProcedure result = InvalidOid;
	List	   *names;
	FuncCandidateList clist;

	/* '-' ? */
	if (strcmp(pro_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (pro_name_or_oid[0] >= '0' &&
		pro_name_or_oid[0] <= '9' &&
		strspn(pro_name_or_oid, "0123456789") == strlen(pro_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(pro_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* Else it's a name, possibly schema-qualified */

	/*
	 * We should never get here in bootstrap mode, as all references should
	 * have been resolved by genbki.pl.
	 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regproc values must be OIDs in bootstrap mode");

	/*
	 * Normal case: parse the name into components and see if it matches any
	 * pg_proc entries in the current search path.
	 */
	names = stringToQualifiedNameList(pro_name_or_oid);
	clist = FuncnameGetCandidates(names, -1, NIL, false, false, false);

	if (clist == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function \"%s\" does not exist", pro_name_or_oid)));
	else if (clist->next != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
				 errmsg("more than one function named \"%s\"",
						pro_name_or_oid)));

	result = clist->oid;

	PG_RETURN_OID(result);
}

/*
 * to_regproc	- converts "proname" to proc OID
 *
 * If the name is not found, we return NULL.
 */
Datum
to_regproc(PG_FUNCTION_ARGS)
{
	char	   *pro_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	List	   *names;
	FuncCandidateList clist;

	/*
	 * Parse the name into components and see if it matches any pg_proc
	 * entries in the current search path.
	 */
	names = stringToQualifiedNameList(pro_name);
	clist = FuncnameGetCandidates(names, -1, NIL, false, false, true);

	if (clist == NULL || clist->next != NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(clist->oid);
}

/*
 * regprocout		- converts proc OID to "pro_name"
 */
Datum
regprocout(PG_FUNCTION_ARGS)
{
	RegProcedure proid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	proctup;

	if (proid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(proid));

	if (HeapTupleIsValid(proctup))
	{
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		char	   *proname = NameStr(procform->proname);

		/*
		 * In bootstrap mode, skip the fancy namespace stuff and just return
		 * the proc name.  (This path is only needed for debugging output
		 * anyway.)
		 */
		if (IsBootstrapProcessingMode())
			result = pstrdup(proname);
		else
		{
			char	   *nspname;
			FuncCandidateList clist;

			/*
			 * Would this proc be found (uniquely!) by regprocin? If not,
			 * qualify it.
			 */
			clist = FuncnameGetCandidates(list_make1(makeString(proname)),
										  -1, NIL, false, false, false);
			if (clist != NULL && clist->next == NULL &&
				clist->oid == proid)
				nspname = NULL;
			else
				nspname = get_namespace_name(procform->pronamespace);

			result = quote_qualified_identifier(nspname, proname);
		}

		ReleaseSysCache(proctup);
	}
	else
	{
		/* If OID doesn't match any pg_proc entry, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", proid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regprocrecv			- converts external binary format to regproc
 */
Datum
regprocrecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regprocsend			- converts regproc to binary format
 */
Datum
regprocsend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}


/*
 * regprocedurein		- converts "proname(args)" to proc OID
 *
 * We also accept a numeric OID, for symmetry with the output routine.
 *
 * '-' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_proc entry.
 */
Datum
regprocedurein(PG_FUNCTION_ARGS)
{
	char	   *pro_name_or_oid = PG_GETARG_CSTRING(0);
	RegProcedure result = InvalidOid;
	List	   *names;
	int			nargs;
	Oid			argtypes[FUNC_MAX_ARGS];
	FuncCandidateList clist;

	/* '-' ? */
	if (strcmp(pro_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (pro_name_or_oid[0] >= '0' &&
		pro_name_or_oid[0] <= '9' &&
		strspn(pro_name_or_oid, "0123456789") == strlen(pro_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(pro_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* The rest of this wouldn't work in bootstrap mode */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regprocedure values must be OIDs in bootstrap mode");

	/*
	 * Else it's a name and arguments.  Parse the name and arguments, look up
	 * potential matches in the current namespace search list, and scan to see
	 * which one exactly matches the given argument types.  (There will not be
	 * more than one match.)
	 */
	parseNameAndArgTypes(pro_name_or_oid, false, &names, &nargs, argtypes);

	clist = FuncnameGetCandidates(names, nargs, NIL, false, false, false);

	for (; clist; clist = clist->next)
	{
		if (memcmp(clist->args, argtypes, nargs * sizeof(Oid)) == 0)
			break;
	}

	if (clist == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function \"%s\" does not exist", pro_name_or_oid)));

	result = clist->oid;

	PG_RETURN_OID(result);
}

/*
 * to_regprocedure	- converts "proname(args)" to proc OID
 *
 * If the name is not found, we return NULL.
 */
Datum
to_regprocedure(PG_FUNCTION_ARGS)
{
	char	   *pro_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	List	   *names;
	int			nargs;
	Oid			argtypes[FUNC_MAX_ARGS];
	FuncCandidateList clist;

	/*
	 * Parse the name and arguments, look up potential matches in the current
	 * namespace search list, and scan to see which one exactly matches the
	 * given argument types.    (There will not be more than one match.)
	 */
	parseNameAndArgTypes(pro_name, false, &names, &nargs, argtypes);

	clist = FuncnameGetCandidates(names, nargs, NIL, false, false, true);

	for (; clist; clist = clist->next)
	{
		if (memcmp(clist->args, argtypes, nargs * sizeof(Oid)) == 0)
			PG_RETURN_OID(clist->oid);
	}

	PG_RETURN_NULL();
}

/*
 * format_procedure		- converts proc OID to "pro_name(args)"
 *
 * This exports the useful functionality of regprocedureout for use
 * in other backend modules.  The result is a palloc'd string.
 */
char *
format_procedure(Oid procedure_oid)
{
	return format_procedure_extended(procedure_oid, 0);
}

char *
format_procedure_qualified(Oid procedure_oid)
{
	return format_procedure_extended(procedure_oid, FORMAT_PROC_FORCE_QUALIFY);
}

/*
 * format_procedure_extended - converts procedure OID to "pro_name(args)"
 *
 * This exports the useful functionality of regprocedureout for use
 * in other backend modules.  The result is a palloc'd string, or NULL.
 *
 * Routine to produce regprocedure names; see format_procedure above.
 *
 * The following bits in 'flags' modify the behavior:
 * - FORMAT_PROC_INVALID_AS_NULL
 *			if the procedure OID is invalid or unknown, return NULL instead
 *			of the numeric OID.
 * - FORMAT_PROC_FORCE_QUALIFY
 *			always schema-qualify procedure names, regardless of search_path
 */
char *
format_procedure_extended(Oid procedure_oid, bits16 flags)
{
	char	   *result;
	HeapTuple	proctup;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(procedure_oid));

	if (HeapTupleIsValid(proctup))
	{
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		char	   *proname = NameStr(procform->proname);
		int			nargs = procform->pronargs;
		int			i;
		char	   *nspname;
		StringInfoData buf;

		/* XXX no support here for bootstrap mode */
		Assert(!IsBootstrapProcessingMode());

		initStringInfo(&buf);

		/*
		 * Would this proc be found (given the right args) by regprocedurein?
		 * If not, or if caller requests it, we need to qualify it.
		 */
		if ((flags & FORMAT_PROC_FORCE_QUALIFY) == 0 &&
			FunctionIsVisible(procedure_oid))
			nspname = NULL;
		else
			nspname = get_namespace_name(procform->pronamespace);

		appendStringInfo(&buf, "%s(",
						 quote_qualified_identifier(nspname, proname));
		for (i = 0; i < nargs; i++)
		{
			Oid			thisargtype = procform->proargtypes.values[i];

			if (i > 0)
				appendStringInfoChar(&buf, ',');
			appendStringInfoString(&buf,
								   (flags & FORMAT_PROC_FORCE_QUALIFY) != 0 ?
								   format_type_be_qualified(thisargtype) :
								   format_type_be(thisargtype));
		}
		appendStringInfoChar(&buf, ')');

		result = buf.data;

		ReleaseSysCache(proctup);
	}
	else if ((flags & FORMAT_PROC_INVALID_AS_NULL) != 0)
	{
		/* If object is undefined, return NULL as wanted by caller */
		result = NULL;
	}
	else
	{
		/* If OID doesn't match any pg_proc entry, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", procedure_oid);
	}

	return result;
}

/*
 * Output an objname/objargs representation for the procedure with the
 * given OID.  If it doesn't exist, an error is thrown.
 *
 * This can be used to feed get_object_address.
 */
void
format_procedure_parts(Oid procedure_oid, List **objnames, List **objargs,
					   bool missing_ok)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	int			nargs;
	int			i;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(procedure_oid));

	if (!HeapTupleIsValid(proctup))
	{
		if (!missing_ok)
			elog(ERROR, "cache lookup failed for procedure with OID %u", procedure_oid);
		return;
	}

	procform = (Form_pg_proc) GETSTRUCT(proctup);
	nargs = procform->pronargs;

	*objnames = list_make2(get_namespace_name_or_temp(procform->pronamespace),
						   pstrdup(NameStr(procform->proname)));
	*objargs = NIL;
	for (i = 0; i < nargs; i++)
	{
		Oid			thisargtype = procform->proargtypes.values[i];

		*objargs = lappend(*objargs, format_type_be_qualified(thisargtype));
	}

	ReleaseSysCache(proctup);
}

/*
 * regprocedureout		- converts proc OID to "pro_name(args)"
 */
Datum
regprocedureout(PG_FUNCTION_ARGS)
{
	RegProcedure proid = PG_GETARG_OID(0);
	char	   *result;

	if (proid == InvalidOid)
		result = pstrdup("-");
	else
		result = format_procedure(proid);

	PG_RETURN_CSTRING(result);
}

/*
 *		regprocedurerecv			- converts external binary format to regprocedure
 */
Datum
regprocedurerecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regproceduresend			- converts regprocedure to binary format
 */
Datum
regproceduresend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}


/*
 * regoperin		- converts "oprname" to operator OID
 *
 * We also accept a numeric OID, for symmetry with the output routine.
 *
 * '0' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_operator entry.
 */
Datum
regoperin(PG_FUNCTION_ARGS)
{
	char	   *opr_name_or_oid = PG_GETARG_CSTRING(0);
	Oid			result = InvalidOid;
	List	   *names;
	FuncCandidateList clist;

	/* '0' ? */
	if (strcmp(opr_name_or_oid, "0") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (opr_name_or_oid[0] >= '0' &&
		opr_name_or_oid[0] <= '9' &&
		strspn(opr_name_or_oid, "0123456789") == strlen(opr_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(opr_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* Else it's a name, possibly schema-qualified */

	/* The rest of this wouldn't work in bootstrap mode */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regoper values must be OIDs in bootstrap mode");

	/*
	 * Normal case: parse the name into components and see if it matches any
	 * pg_operator entries in the current search path.
	 */
	names = stringToQualifiedNameList(opr_name_or_oid);
	clist = OpernameGetCandidates(names, '\0', false);

	if (clist == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator does not exist: %s", opr_name_or_oid)));
	else if (clist->next != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
				 errmsg("more than one operator named %s",
						opr_name_or_oid)));

	result = clist->oid;

	PG_RETURN_OID(result);
}

/*
 * to_regoper		- converts "oprname" to operator OID
 *
 * If the name is not found, we return NULL.
 */
Datum
to_regoper(PG_FUNCTION_ARGS)
{
	char	   *opr_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	List	   *names;
	FuncCandidateList clist;

	/*
	 * Parse the name into components and see if it matches any pg_operator
	 * entries in the current search path.
	 */
	names = stringToQualifiedNameList(opr_name);
	clist = OpernameGetCandidates(names, '\0', true);

	if (clist == NULL || clist->next != NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(clist->oid);
}

/*
 * regoperout		- converts operator OID to "opr_name"
 */
Datum
regoperout(PG_FUNCTION_ARGS)
{
	Oid			oprid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	opertup;

	if (oprid == InvalidOid)
	{
		result = pstrdup("0");
		PG_RETURN_CSTRING(result);
	}

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(oprid));

	if (HeapTupleIsValid(opertup))
	{
		Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
		char	   *oprname = NameStr(operform->oprname);

		/*
		 * In bootstrap mode, skip the fancy namespace stuff and just return
		 * the oper name.  (This path is only needed for debugging output
		 * anyway.)
		 */
		if (IsBootstrapProcessingMode())
			result = pstrdup(oprname);
		else
		{
			FuncCandidateList clist;

			/*
			 * Would this oper be found (uniquely!) by regoperin? If not,
			 * qualify it.
			 */
			clist = OpernameGetCandidates(list_make1(makeString(oprname)),
										  '\0', false);
			if (clist != NULL && clist->next == NULL &&
				clist->oid == oprid)
				result = pstrdup(oprname);
			else
			{
				const char *nspname;

				nspname = get_namespace_name(operform->oprnamespace);
				nspname = quote_identifier(nspname);
				result = (char *) palloc(strlen(nspname) + strlen(oprname) + 2);
				sprintf(result, "%s.%s", nspname, oprname);
			}
		}

		ReleaseSysCache(opertup);
	}
	else
	{
		/*
		 * If OID doesn't match any pg_operator entry, return it numerically
		 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", oprid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regoperrecv			- converts external binary format to regoper
 */
Datum
regoperrecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regopersend			- converts regoper to binary format
 */
Datum
regopersend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}


/*
 * regoperatorin		- converts "oprname(args)" to operator OID
 *
 * We also accept a numeric OID, for symmetry with the output routine.
 *
 * '0' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_operator entry.
 */
Datum
regoperatorin(PG_FUNCTION_ARGS)
{
	char	   *opr_name_or_oid = PG_GETARG_CSTRING(0);
	Oid			result;
	List	   *names;
	int			nargs;
	Oid			argtypes[FUNC_MAX_ARGS];

	/* '0' ? */
	if (strcmp(opr_name_or_oid, "0") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (opr_name_or_oid[0] >= '0' &&
		opr_name_or_oid[0] <= '9' &&
		strspn(opr_name_or_oid, "0123456789") == strlen(opr_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(opr_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* The rest of this wouldn't work in bootstrap mode */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regoperator values must be OIDs in bootstrap mode");

	/*
	 * Else it's a name and arguments.  Parse the name and arguments, look up
	 * potential matches in the current namespace search list, and scan to see
	 * which one exactly matches the given argument types.  (There will not be
	 * more than one match.)
	 */
	parseNameAndArgTypes(opr_name_or_oid, true, &names, &nargs, argtypes);
	if (nargs == 1)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("missing argument"),
				 errhint("Use NONE to denote the missing argument of a unary operator.")));
	if (nargs != 2)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("too many arguments"),
				 errhint("Provide two argument types for operator.")));

	result = OpernameGetOprid(names, argtypes[0], argtypes[1]);

	if (!OidIsValid(result))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator does not exist: %s", opr_name_or_oid)));

	PG_RETURN_OID(result);
}

/*
 * to_regoperator	- converts "oprname(args)" to operator OID
 *
 * If the name is not found, we return NULL.
 */
Datum
to_regoperator(PG_FUNCTION_ARGS)
{
	char	   *opr_name_or_oid = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			result;
	List	   *names;
	int			nargs;
	Oid			argtypes[FUNC_MAX_ARGS];

	/*
	 * Parse the name and arguments, look up potential matches in the current
	 * namespace search list, and scan to see which one exactly matches the
	 * given argument types.    (There will not be more than one match.)
	 */
	parseNameAndArgTypes(opr_name_or_oid, true, &names, &nargs, argtypes);
	if (nargs == 1)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("missing argument"),
				 errhint("Use NONE to denote the missing argument of a unary operator.")));
	if (nargs != 2)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("too many arguments"),
				 errhint("Provide two argument types for operator.")));

	result = OpernameGetOprid(names, argtypes[0], argtypes[1]);

	if (!OidIsValid(result))
		PG_RETURN_NULL();

	PG_RETURN_OID(result);
}

/*
 * format_operator_extended - converts operator OID to "opr_name(args)"
 *
 * This exports the useful functionality of regoperatorout for use
 * in other backend modules.  The result is a palloc'd string, or NULL.
 *
 * The following bits in 'flags' modify the behavior:
 * - FORMAT_OPERATOR_INVALID_AS_NULL
 *			if the operator OID is invalid or unknown, return NULL instead
 *			of the numeric OID.
 * - FORMAT_OPERATOR_FORCE_QUALIFY
 *			always schema-qualify operator names, regardless of search_path
 */
char *
format_operator_extended(Oid operator_oid, bits16 flags)
{
	char	   *result;
	HeapTuple	opertup;

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operator_oid));

	if (HeapTupleIsValid(opertup))
	{
		Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
		char	   *oprname = NameStr(operform->oprname);
		char	   *nspname;
		StringInfoData buf;

		/* XXX no support here for bootstrap mode */
		Assert(!IsBootstrapProcessingMode());

		initStringInfo(&buf);

		/*
		 * Would this oper be found (given the right args) by regoperatorin?
		 * If not, or if caller explicitly requests it, we need to qualify it.
		 */
		if ((flags & FORMAT_OPERATOR_FORCE_QUALIFY) != 0 ||
			!OperatorIsVisible(operator_oid))
		{
			nspname = get_namespace_name(operform->oprnamespace);
			appendStringInfo(&buf, "%s.",
							 quote_identifier(nspname));
		}

		appendStringInfo(&buf, "%s(", oprname);

		if (operform->oprleft)
			appendStringInfo(&buf, "%s,",
							 (flags & FORMAT_OPERATOR_FORCE_QUALIFY) != 0 ?
							 format_type_be_qualified(operform->oprleft) :
							 format_type_be(operform->oprleft));
		else
			appendStringInfoString(&buf, "NONE,");

		if (operform->oprright)
			appendStringInfo(&buf, "%s)",
							 (flags & FORMAT_OPERATOR_FORCE_QUALIFY) != 0 ?
							 format_type_be_qualified(operform->oprright) :
							 format_type_be(operform->oprright));
		else
			appendStringInfoString(&buf, "NONE)");

		result = buf.data;

		ReleaseSysCache(opertup);
	}
	else if ((flags & FORMAT_OPERATOR_INVALID_AS_NULL) != 0)
	{
		/* If object is undefined, return NULL as wanted by caller */
		result = NULL;
	}
	else
	{
		/*
		 * If OID doesn't match any pg_operator entry, return it numerically
		 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", operator_oid);
	}

	return result;
}

char *
format_operator(Oid operator_oid)
{
	return format_operator_extended(operator_oid, 0);
}

char *
format_operator_qualified(Oid operator_oid)
{
	return format_operator_extended(operator_oid,
									FORMAT_OPERATOR_FORCE_QUALIFY);
}

void
format_operator_parts(Oid operator_oid, List **objnames, List **objargs,
					  bool missing_ok)
{
	HeapTuple	opertup;
	Form_pg_operator oprForm;

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operator_oid));
	if (!HeapTupleIsValid(opertup))
	{
		if (!missing_ok)
			elog(ERROR, "cache lookup failed for operator with OID %u",
				 operator_oid);
		return;
	}

	oprForm = (Form_pg_operator) GETSTRUCT(opertup);
	*objnames = list_make2(get_namespace_name_or_temp(oprForm->oprnamespace),
						   pstrdup(NameStr(oprForm->oprname)));
	*objargs = NIL;
	if (oprForm->oprleft)
		*objargs = lappend(*objargs,
						   format_type_be_qualified(oprForm->oprleft));
	if (oprForm->oprright)
		*objargs = lappend(*objargs,
						   format_type_be_qualified(oprForm->oprright));

	ReleaseSysCache(opertup);
}

/*
 * regoperatorout		- converts operator OID to "opr_name(args)"
 */
Datum
regoperatorout(PG_FUNCTION_ARGS)
{
	Oid			oprid = PG_GETARG_OID(0);
	char	   *result;

	if (oprid == InvalidOid)
		result = pstrdup("0");
	else
		result = format_operator(oprid);

	PG_RETURN_CSTRING(result);
}

/*
 *		regoperatorrecv			- converts external binary format to regoperator
 */
Datum
regoperatorrecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regoperatorsend			- converts regoperator to binary format
 */
Datum
regoperatorsend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}


/*
 * regclassin		- converts "classname" to class OID
 *
 * We also accept a numeric OID, for symmetry with the output routine.
 *
 * '-' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_class entry.
 */
Datum
regclassin(PG_FUNCTION_ARGS)
{
	char	   *class_name_or_oid = PG_GETARG_CSTRING(0);
	Oid			result = InvalidOid;
	List	   *names;

	/* '-' ? */
	if (strcmp(class_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (class_name_or_oid[0] >= '0' &&
		class_name_or_oid[0] <= '9' &&
		strspn(class_name_or_oid, "0123456789") == strlen(class_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(class_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* Else it's a name, possibly schema-qualified */

	/* The rest of this wouldn't work in bootstrap mode */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regclass values must be OIDs in bootstrap mode");

	/*
	 * Normal case: parse the name into components and see if it matches any
	 * pg_class entries in the current search path.
	 */
	names = stringToQualifiedNameList(class_name_or_oid);

	/* We might not even have permissions on this relation; don't lock it. */
	result = RangeVarGetRelid(makeRangeVarFromNameList(names), NoLock, false);

	PG_RETURN_OID(result);
}

/*
 * to_regclass		- converts "classname" to class OID
 *
 * If the name is not found, we return NULL.
 */
Datum
to_regclass(PG_FUNCTION_ARGS)
{
	char	   *class_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			result;
	List	   *names;

	/*
	 * Parse the name into components and see if it matches any pg_class
	 * entries in the current search path.
	 */
	names = stringToQualifiedNameList(class_name);

	/* We might not even have permissions on this relation; don't lock it. */
	result = RangeVarGetRelid(makeRangeVarFromNameList(names), NoLock, true);

	if (OidIsValid(result))
		PG_RETURN_OID(result);
	else
		PG_RETURN_NULL();
}

/*
 * regclassout		- converts class OID to "class_name"
 */
Datum
regclassout(PG_FUNCTION_ARGS)
{
	Oid			classid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	classtup;

	if (classid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	classtup = SearchSysCache1(RELOID, ObjectIdGetDatum(classid));

	if (HeapTupleIsValid(classtup))
	{
		Form_pg_class classform = (Form_pg_class) GETSTRUCT(classtup);
		char	   *classname = NameStr(classform->relname);

		/*
		 * In bootstrap mode, skip the fancy namespace stuff and just return
		 * the class name.  (This path is only needed for debugging output
		 * anyway.)
		 */
		if (IsBootstrapProcessingMode())
			result = pstrdup(classname);
		else
		{
			char	   *nspname;

			/*
			 * Would this class be found by regclassin? If not, qualify it.
			 */
			if (RelationIsVisible(classid))
				nspname = NULL;
			else
				nspname = get_namespace_name(classform->relnamespace);

			result = quote_qualified_identifier(nspname, classname);
		}

		ReleaseSysCache(classtup);
	}
	else
	{
		/* If OID doesn't match any pg_class entry, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", classid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regclassrecv			- converts external binary format to regclass
 */
Datum
regclassrecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regclasssend			- converts regclass to binary format
 */
Datum
regclasssend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}


/*
 * regcollationin		- converts "collationname" to collation OID
 *
 * We also accept a numeric OID, for symmetry with the output routine.
 *
 * '-' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_collation entry.
 */
Datum
regcollationin(PG_FUNCTION_ARGS)
{
	char	   *collation_name_or_oid = PG_GETARG_CSTRING(0);
	Oid			result = InvalidOid;
	List	   *names;

	/* '-' ? */
	if (strcmp(collation_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (collation_name_or_oid[0] >= '0' &&
		collation_name_or_oid[0] <= '9' &&
		strspn(collation_name_or_oid, "0123456789") == strlen(collation_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(collation_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* Else it's a name, possibly schema-qualified */

	/* The rest of this wouldn't work in bootstrap mode */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regcollation values must be OIDs in bootstrap mode");

	/*
	 * Normal case: parse the name into components and see if it matches any
	 * pg_collation entries in the current search path.
	 */
	names = stringToQualifiedNameList(collation_name_or_oid);

	result = get_collation_oid(names, false);

	PG_RETURN_OID(result);
}

/*
 * to_regcollation		- converts "collationname" to collation OID
 *
 * If the name is not found, we return NULL.
 */
Datum
to_regcollation(PG_FUNCTION_ARGS)
{
	char	   *collation_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			result;
	List	   *names;

	/*
	 * Parse the name into components and see if it matches any pg_collation
	 * entries in the current search path.
	 */
	names = stringToQualifiedNameList(collation_name);

	/* We might not even have permissions on this relation; don't lock it. */
	result = get_collation_oid(names, true);

	if (OidIsValid(result))
		PG_RETURN_OID(result);
	else
		PG_RETURN_NULL();
}

/*
 * regcollationout		- converts collation OID to "collation_name"
 */
Datum
regcollationout(PG_FUNCTION_ARGS)
{
	Oid			collationid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	collationtup;

	if (collationid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	collationtup = SearchSysCache1(COLLOID, ObjectIdGetDatum(collationid));

	if (HeapTupleIsValid(collationtup))
	{
		Form_pg_collation collationform = (Form_pg_collation) GETSTRUCT(collationtup);
		char	   *collationname = NameStr(collationform->collname);

		/*
		 * In bootstrap mode, skip the fancy namespace stuff and just return
		 * the collation name.  (This path is only needed for debugging output
		 * anyway.)
		 */
		if (IsBootstrapProcessingMode())
			result = pstrdup(collationname);
		else
		{
			char	   *nspname;

			/*
			 * Would this collation be found by regcollationin? If not,
			 * qualify it.
			 */
			if (CollationIsVisible(collationid))
				nspname = NULL;
			else
				nspname = get_namespace_name(collationform->collnamespace);

			result = quote_qualified_identifier(nspname, collationname);
		}

		ReleaseSysCache(collationtup);
	}
	else
	{
		/* If OID doesn't match any pg_collation entry, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", collationid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regcollationrecv			- converts external binary format to regcollation
 */
Datum
regcollationrecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regcollationsend			- converts regcollation to binary format
 */
Datum
regcollationsend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}


/*
 * regtypein		- converts "typename" to type OID
 *
 * The type name can be specified using the full type syntax recognized by
 * the parser; for example, DOUBLE PRECISION and INTEGER[] will work and be
 * translated to the correct type names.  (We ignore any typmod info
 * generated by the parser, however.)
 *
 * We also accept a numeric OID, for symmetry with the output routine,
 * and for possible use in bootstrap mode.
 *
 * '-' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_type entry.
 */
Datum
regtypein(PG_FUNCTION_ARGS)
{
	char	   *typ_name_or_oid = PG_GETARG_CSTRING(0);
	Oid			result = InvalidOid;
	int32		typmod;

	/* '-' ? */
	if (strcmp(typ_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (typ_name_or_oid[0] >= '0' &&
		typ_name_or_oid[0] <= '9' &&
		strspn(typ_name_or_oid, "0123456789") == strlen(typ_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(typ_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* Else it's a type name, possibly schema-qualified or decorated */

	/* The rest of this wouldn't work in bootstrap mode */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regtype values must be OIDs in bootstrap mode");

	/*
	 * Normal case: invoke the full parser to deal with special cases such as
	 * array syntax.
	 */
	parseTypeString(typ_name_or_oid, &result, &typmod, false);

	PG_RETURN_OID(result);
}

/*
 * to_regtype		- converts "typename" to type OID
 *
 * If the name is not found, we return NULL.
 */
Datum
to_regtype(PG_FUNCTION_ARGS)
{
	char	   *typ_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			result;
	int32		typmod;

	/*
	 * Invoke the full parser to deal with special cases such as array syntax.
	 */
	parseTypeString(typ_name, &result, &typmod, true);

	if (OidIsValid(result))
		PG_RETURN_OID(result);
	else
		PG_RETURN_NULL();
}

/*
 * regtypeout		- converts type OID to "typ_name"
 */
Datum
regtypeout(PG_FUNCTION_ARGS)
{
	Oid			typid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	typetup;

	if (typid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	typetup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));

	if (HeapTupleIsValid(typetup))
	{
		Form_pg_type typeform = (Form_pg_type) GETSTRUCT(typetup);

		/*
		 * In bootstrap mode, skip the fancy namespace stuff and just return
		 * the type name.  (This path is only needed for debugging output
		 * anyway.)
		 */
		if (IsBootstrapProcessingMode())
		{
			char	   *typname = NameStr(typeform->typname);

			result = pstrdup(typname);
		}
		else
			result = format_type_be(typid);

		ReleaseSysCache(typetup);
	}
	else
	{
		/* If OID doesn't match any pg_type entry, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", typid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regtyperecv			- converts external binary format to regtype
 */
Datum
regtyperecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regtypesend			- converts regtype to binary format
 */
Datum
regtypesend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}


/*
 * regconfigin		- converts "tsconfigname" to tsconfig OID
 *
 * We also accept a numeric OID, for symmetry with the output routine.
 *
 * '-' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_ts_config entry.
 */
Datum
regconfigin(PG_FUNCTION_ARGS)
{
	char	   *cfg_name_or_oid = PG_GETARG_CSTRING(0);
	Oid			result;
	List	   *names;

	/* '-' ? */
	if (strcmp(cfg_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (cfg_name_or_oid[0] >= '0' &&
		cfg_name_or_oid[0] <= '9' &&
		strspn(cfg_name_or_oid, "0123456789") == strlen(cfg_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(cfg_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* The rest of this wouldn't work in bootstrap mode */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regconfig values must be OIDs in bootstrap mode");

	/*
	 * Normal case: parse the name into components and see if it matches any
	 * pg_ts_config entries in the current search path.
	 */
	names = stringToQualifiedNameList(cfg_name_or_oid);

	result = get_ts_config_oid(names, false);

	PG_RETURN_OID(result);
}

/*
 * regconfigout		- converts tsconfig OID to "tsconfigname"
 */
Datum
regconfigout(PG_FUNCTION_ARGS)
{
	Oid			cfgid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	cfgtup;

	if (cfgid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	cfgtup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgid));

	if (HeapTupleIsValid(cfgtup))
	{
		Form_pg_ts_config cfgform = (Form_pg_ts_config) GETSTRUCT(cfgtup);
		char	   *cfgname = NameStr(cfgform->cfgname);
		char	   *nspname;

		/*
		 * Would this config be found by regconfigin? If not, qualify it.
		 */
		if (TSConfigIsVisible(cfgid))
			nspname = NULL;
		else
			nspname = get_namespace_name(cfgform->cfgnamespace);

		result = quote_qualified_identifier(nspname, cfgname);

		ReleaseSysCache(cfgtup);
	}
	else
	{
		/* If OID doesn't match any pg_ts_config row, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", cfgid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regconfigrecv			- converts external binary format to regconfig
 */
Datum
regconfigrecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regconfigsend			- converts regconfig to binary format
 */
Datum
regconfigsend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}


/*
 * regdictionaryin		- converts "tsdictionaryname" to tsdictionary OID
 *
 * We also accept a numeric OID, for symmetry with the output routine.
 *
 * '-' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_ts_dict entry.
 */
Datum
regdictionaryin(PG_FUNCTION_ARGS)
{
	char	   *dict_name_or_oid = PG_GETARG_CSTRING(0);
	Oid			result;
	List	   *names;

	/* '-' ? */
	if (strcmp(dict_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (dict_name_or_oid[0] >= '0' &&
		dict_name_or_oid[0] <= '9' &&
		strspn(dict_name_or_oid, "0123456789") == strlen(dict_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(dict_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* The rest of this wouldn't work in bootstrap mode */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regdictionary values must be OIDs in bootstrap mode");

	/*
	 * Normal case: parse the name into components and see if it matches any
	 * pg_ts_dict entries in the current search path.
	 */
	names = stringToQualifiedNameList(dict_name_or_oid);

	result = get_ts_dict_oid(names, false);

	PG_RETURN_OID(result);
}

/*
 * regdictionaryout		- converts tsdictionary OID to "tsdictionaryname"
 */
Datum
regdictionaryout(PG_FUNCTION_ARGS)
{
	Oid			dictid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	dicttup;

	if (dictid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	dicttup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictid));

	if (HeapTupleIsValid(dicttup))
	{
		Form_pg_ts_dict dictform = (Form_pg_ts_dict) GETSTRUCT(dicttup);
		char	   *dictname = NameStr(dictform->dictname);
		char	   *nspname;

		/*
		 * Would this dictionary be found by regdictionaryin? If not, qualify
		 * it.
		 */
		if (TSDictionaryIsVisible(dictid))
			nspname = NULL;
		else
			nspname = get_namespace_name(dictform->dictnamespace);

		result = quote_qualified_identifier(nspname, dictname);

		ReleaseSysCache(dicttup);
	}
	else
	{
		/* If OID doesn't match any pg_ts_dict row, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", dictid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regdictionaryrecv	- converts external binary format to regdictionary
 */
Datum
regdictionaryrecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regdictionarysend	- converts regdictionary to binary format
 */
Datum
regdictionarysend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}

/*
 * regrolein	- converts "rolename" to role OID
 *
 * We also accept a numeric OID, for symmetry with the output routine.
 *
 * '-' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_authid entry.
 */
Datum
regrolein(PG_FUNCTION_ARGS)
{
	char	   *role_name_or_oid = PG_GETARG_CSTRING(0);
	Oid			result;
	List	   *names;

	/* '-' ? */
	if (strcmp(role_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (role_name_or_oid[0] >= '0' &&
		role_name_or_oid[0] <= '9' &&
		strspn(role_name_or_oid, "0123456789") == strlen(role_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(role_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* The rest of this wouldn't work in bootstrap mode */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regrole values must be OIDs in bootstrap mode");

	/* Normal case: see if the name matches any pg_authid entry. */
	names = stringToQualifiedNameList(role_name_or_oid);

	if (list_length(names) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	result = get_role_oid(strVal(linitial(names)), false);

	PG_RETURN_OID(result);
}

/*
 * to_regrole		- converts "rolename" to role OID
 *
 * If the name is not found, we return NULL.
 */
Datum
to_regrole(PG_FUNCTION_ARGS)
{
	char	   *role_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			result;
	List	   *names;

	names = stringToQualifiedNameList(role_name);

	if (list_length(names) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	result = get_role_oid(strVal(linitial(names)), true);

	if (OidIsValid(result))
		PG_RETURN_OID(result);
	else
		PG_RETURN_NULL();
}

/*
 * regroleout		- converts role OID to "role_name"
 */
Datum
regroleout(PG_FUNCTION_ARGS)
{
	Oid			roleoid = PG_GETARG_OID(0);
	char	   *result;

	if (roleoid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	result = GetUserNameFromId(roleoid, true);

	if (result)
	{
		/* pstrdup is not really necessary, but it avoids a compiler warning */
		result = pstrdup(quote_identifier(result));
	}
	else
	{
		/* If OID doesn't match any role, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", roleoid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regrolerecv - converts external binary format to regrole
 */
Datum
regrolerecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regrolesend - converts regrole to binary format
 */
Datum
regrolesend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}

/*
 * regnamespacein		- converts "nspname" to namespace OID
 *
 * We also accept a numeric OID, for symmetry with the output routine.
 *
 * '-' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_namespace entry.
 */
Datum
regnamespacein(PG_FUNCTION_ARGS)
{
	char	   *nsp_name_or_oid = PG_GETARG_CSTRING(0);
	Oid			result;
	List	   *names;

	/* '-' ? */
	if (strcmp(nsp_name_or_oid, "-") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (nsp_name_or_oid[0] >= '0' &&
		nsp_name_or_oid[0] <= '9' &&
		strspn(nsp_name_or_oid, "0123456789") == strlen(nsp_name_or_oid))
	{
		result = DatumGetObjectId(DirectFunctionCall1(oidin,
													  CStringGetDatum(nsp_name_or_oid)));
		PG_RETURN_OID(result);
	}

	/* The rest of this wouldn't work in bootstrap mode */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regnamespace values must be OIDs in bootstrap mode");

	/* Normal case: see if the name matches any pg_namespace entry. */
	names = stringToQualifiedNameList(nsp_name_or_oid);

	if (list_length(names) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	result = get_namespace_oid(strVal(linitial(names)), false);

	PG_RETURN_OID(result);
}

/*
 * to_regnamespace		- converts "nspname" to namespace OID
 *
 * If the name is not found, we return NULL.
 */
Datum
to_regnamespace(PG_FUNCTION_ARGS)
{
	char	   *nsp_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			result;
	List	   *names;

	names = stringToQualifiedNameList(nsp_name);

	if (list_length(names) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	result = get_namespace_oid(strVal(linitial(names)), true);

	if (OidIsValid(result))
		PG_RETURN_OID(result);
	else
		PG_RETURN_NULL();
}

/*
 * regnamespaceout		- converts namespace OID to "nsp_name"
 */
Datum
regnamespaceout(PG_FUNCTION_ARGS)
{
	Oid			nspid = PG_GETARG_OID(0);
	char	   *result;

	if (nspid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	result = get_namespace_name(nspid);

	if (result)
	{
		/* pstrdup is not really necessary, but it avoids a compiler warning */
		result = pstrdup(quote_identifier(result));
	}
	else
	{
		/* If OID doesn't match any namespace, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", nspid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regnamespacerecv	- converts external binary format to regnamespace
 */
Datum
regnamespacerecv(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidrecv, so share code */
	return oidrecv(fcinfo);
}

/*
 *		regnamespacesend		- converts regnamespace to binary format
 */
Datum
regnamespacesend(PG_FUNCTION_ARGS)
{
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}

/*
 * text_regclass: convert text to regclass
 *
 * This could be replaced by CoerceViaIO, except that we need to treat
 * text-to-regclass as an implicit cast to support legacy forms of nextval()
 * and related functions.
 */
Datum
text_regclass(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	Oid			result;
	RangeVar   *rv;

	rv = makeRangeVarFromNameList(textToQualifiedNameList(relname));

	/* We might not even have permissions on this relation; don't lock it. */
	result = RangeVarGetRelid(rv, NoLock, false);

	PG_RETURN_OID(result);
}


/*
 * Given a C string, parse it into a qualified-name list.
 */
List *
stringToQualifiedNameList(const char *string)
{
	char	   *rawname;
	List	   *result = NIL;
	List	   *namelist;
	ListCell   *l;

	/* We need a modifiable copy of the input string. */
	rawname = pstrdup(string);

	if (!SplitIdentifierString(rawname, '.', &namelist))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	if (namelist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);

		result = lappend(result, makeString(pstrdup(curname)));
	}

	pfree(rawname);
	list_free(namelist);

	return result;
}

/*****************************************************************************
 *	 SUPPORT ROUTINES														 *
 *****************************************************************************/

/*
 * Given a C string, parse it into a qualified function or operator name
 * followed by a parenthesized list of type names.  Reduce the
 * type names to an array of OIDs (returned into *nargs and *argtypes;
 * the argtypes array should be of size FUNC_MAX_ARGS).  The function or
 * operator name is returned to *names as a List of Strings.
 *
 * If allowNone is true, accept "NONE" and return it as InvalidOid (this is
 * for unary operators).
 */
static void
parseNameAndArgTypes(const char *string, bool allowNone, List **names,
					 int *nargs, Oid *argtypes)
{
	char	   *rawname;
	char	   *ptr;
	char	   *ptr2;
	char	   *typename;
	bool		in_quote;
	bool		had_comma;
	int			paren_count;
	Oid			typeid;
	int32		typmod;

	/* We need a modifiable copy of the input string. */
	rawname = pstrdup(string);

	/* Scan to find the expected left paren; mustn't be quoted */
	in_quote = false;
	for (ptr = rawname; *ptr; ptr++)
	{
		if (*ptr == '"')
			in_quote = !in_quote;
		else if (*ptr == '(' && !in_quote)
			break;
	}
	if (*ptr == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("expected a left parenthesis")));

	/* Separate the name and parse it into a list */
	*ptr++ = '\0';
	*names = stringToQualifiedNameList(rawname);

	/* Check for the trailing right parenthesis and remove it */
	ptr2 = ptr + strlen(ptr);
	while (--ptr2 > ptr)
	{
		if (!scanner_isspace(*ptr2))
			break;
	}
	if (*ptr2 != ')')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("expected a right parenthesis")));

	*ptr2 = '\0';

	/* Separate the remaining string into comma-separated type names */
	*nargs = 0;
	had_comma = false;

	for (;;)
	{
		/* allow leading whitespace */
		while (scanner_isspace(*ptr))
			ptr++;
		if (*ptr == '\0')
		{
			/* End of string.  Okay unless we had a comma before. */
			if (had_comma)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("expected a type name")));
			break;
		}
		typename = ptr;
		/* Find end of type name --- end of string or comma */
		/* ... but not a quoted or parenthesized comma */
		in_quote = false;
		paren_count = 0;
		for (; *ptr; ptr++)
		{
			if (*ptr == '"')
				in_quote = !in_quote;
			else if (*ptr == ',' && !in_quote && paren_count == 0)
				break;
			else if (!in_quote)
			{
				switch (*ptr)
				{
					case '(':
					case '[':
						paren_count++;
						break;
					case ')':
					case ']':
						paren_count--;
						break;
				}
			}
		}
		if (in_quote || paren_count != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("improper type name")));

		ptr2 = ptr;
		if (*ptr == ',')
		{
			had_comma = true;
			*ptr++ = '\0';
		}
		else
		{
			had_comma = false;
			Assert(*ptr == '\0');
		}
		/* Lop off trailing whitespace */
		while (--ptr2 >= typename)
		{
			if (!scanner_isspace(*ptr2))
				break;
			*ptr2 = '\0';
		}

		if (allowNone && pg_strcasecmp(typename, "none") == 0)
		{
			/* Special case for NONE */
			typeid = InvalidOid;
			typmod = -1;
		}
		else
		{
			/* Use full parser to resolve the type name */
			parseTypeString(typename, &typeid, &typmod, false);
		}
		if (*nargs >= FUNC_MAX_ARGS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg("too many arguments")));

		argtypes[*nargs] = typeid;
		(*nargs)++;
	}

	pfree(rawname);
}
