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
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/regproc.c,v 1.72 2002/07/29 22:14:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static void parseNameAndArgTypes(const char *string, const char *caller,
								 const char *type0_spelling,
								 List **names, int *nargs, Oid *argtypes);


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 * regprocin		- converts "proname" to proc OID
 *
 * We also accept a numeric OID, mostly for historical reasons.
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
		Oid			searchOid;

		searchOid = DatumGetObjectId(DirectFunctionCall1(oidin,
									  CStringGetDatum(pro_name_or_oid)));
		result = (RegProcedure) GetSysCacheOid(PROCOID,
											 ObjectIdGetDatum(searchOid),
											   0, 0, 0);
		if (!RegProcedureIsValid(result))
			elog(ERROR, "No procedure with oid %s", pro_name_or_oid);
		PG_RETURN_OID(result);
	}

	/* Else it's a name, possibly schema-qualified */

	/*
	 * In bootstrap mode we assume the given name is not schema-qualified,
	 * and just search pg_proc for a unique match.  This is needed for
	 * initializing other system catalogs (pg_namespace may not exist yet,
	 * and certainly there are no schemas other than pg_catalog).
	 */
	if (IsBootstrapProcessingMode())
	{
		int			matches = 0;
		Relation	hdesc;
		ScanKeyData skey[1];
		SysScanDesc	sysscan;
		HeapTuple	tuple;

		ScanKeyEntryInitialize(&skey[0], 0x0,
							   (AttrNumber) Anum_pg_proc_proname,
							   (RegProcedure) F_NAMEEQ,
							   CStringGetDatum(pro_name_or_oid));

		hdesc = heap_openr(ProcedureRelationName, AccessShareLock);
		sysscan = systable_beginscan(hdesc, ProcedureNameNspIndex, true,
									 SnapshotNow, 1, skey);

		while (HeapTupleIsValid(tuple = systable_getnext(sysscan)))
		{
			AssertTupleDescHasOid(hdesc->rd_att);
			result = (RegProcedure) HeapTupleGetOid(tuple);
			if (++matches > 1)
				break;
		}

		systable_endscan(sysscan);
		heap_close(hdesc, AccessShareLock);

		if (matches == 0)
			elog(ERROR, "No procedure with name %s", pro_name_or_oid);
		else if (matches > 1)
			elog(ERROR, "There is more than one procedure named %s",
				 pro_name_or_oid);
		PG_RETURN_OID(result);
	}

	/*
	 * Normal case: parse the name into components and see if it
	 * matches any pg_proc entries in the current search path.
	 */
	names = stringToQualifiedNameList(pro_name_or_oid, "regprocin");
	clist = FuncnameGetCandidates(names, -1);

	if (clist == NULL)
		elog(ERROR, "No procedure with name %s", pro_name_or_oid);
	else if (clist->next != NULL)
		elog(ERROR, "There is more than one procedure named %s",
			 pro_name_or_oid);

	result = clist->oid;

	PG_RETURN_OID(result);
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

	proctup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(proid),
							 0, 0, 0);

	if (HeapTupleIsValid(proctup))
	{
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		char	   *proname = NameStr(procform->proname);

		/*
		 * In bootstrap mode, skip the fancy namespace stuff and just
		 * return the proc name.  (This path is only needed for debugging
		 * output anyway.)
		 */
		if (IsBootstrapProcessingMode())
		{
			result = pstrdup(proname);
		}
		else
		{
			char	   *nspname;
			FuncCandidateList clist;

			/*
			 * Would this proc be found (uniquely!) by regprocin?
			 * If not, qualify it.
			 */
			clist = FuncnameGetCandidates(makeList1(makeString(proname)), -1);
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
 * regprocedurein		- converts "proname(args)" to proc OID
 *
 * We also accept a numeric OID, mostly for historical reasons.
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
		Oid			searchOid;

		searchOid = DatumGetObjectId(DirectFunctionCall1(oidin,
									  CStringGetDatum(pro_name_or_oid)));
		result = (RegProcedure) GetSysCacheOid(PROCOID,
											 ObjectIdGetDatum(searchOid),
											   0, 0, 0);
		if (!RegProcedureIsValid(result))
			elog(ERROR, "No procedure with oid %s", pro_name_or_oid);
		PG_RETURN_OID(result);
	}

	/*
	 * Else it's a name and arguments.  Parse the name and arguments,
	 * look up potential matches in the current namespace search list,
	 * and scan to see which one exactly matches the given argument
	 * types.  (There will not be more than one match.)
	 *
	 * XXX at present, this code will not work in bootstrap mode, hence this
	 * datatype cannot be used for any system column that needs to receive
	 * data during bootstrap.
	 */
	parseNameAndArgTypes(pro_name_or_oid, "regprocedurein", "opaque",
						 &names, &nargs, argtypes);

	clist = FuncnameGetCandidates(names, nargs);

	for (; clist; clist = clist->next)
	{
		if (memcmp(clist->args, argtypes, nargs * sizeof(Oid)) == 0)
			break;
	}

	if (clist == NULL)
		elog(ERROR, "No procedure with name %s", pro_name_or_oid);

	result = clist->oid;

	PG_RETURN_OID(result);
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
	char	   *result;
	HeapTuple	proctup;

	proctup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(procedure_oid),
							 0, 0, 0);

	if (HeapTupleIsValid(proctup))
	{
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		char	   *proname = NameStr(procform->proname);
		int			nargs = procform->pronargs;
		int			i;
		char	   *nspname;
		StringInfoData buf;

		/* XXX no support here for bootstrap mode */

		initStringInfo(&buf);

		/*
		 * Would this proc be found (given the right args) by regprocedurein?
		 * If not, we need to qualify it.
		 */
		if (FunctionIsVisible(procedure_oid))
			nspname = NULL;
		else
			nspname = get_namespace_name(procform->pronamespace);

		appendStringInfo(&buf, "%s(",
						 quote_qualified_identifier(nspname, proname));
		for (i = 0; i < nargs; i++)
		{
			Oid		thisargtype = procform->proargtypes[i];

			if (i > 0)
				appendStringInfoChar(&buf, ',');
			if (OidIsValid(thisargtype))
				appendStringInfo(&buf, "%s", format_type_be(thisargtype));
			else
				appendStringInfo(&buf, "opaque");
		}
		appendStringInfoChar(&buf, ')');

		result = buf.data;

		ReleaseSysCache(proctup);
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
 * regoperin		- converts "oprname" to operator OID
 *
 * We also accept a numeric OID, mostly for historical reasons.
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
		Oid			searchOid;

		searchOid = DatumGetObjectId(DirectFunctionCall1(oidin,
									  CStringGetDatum(opr_name_or_oid)));
		result = GetSysCacheOid(OPEROID,
								ObjectIdGetDatum(searchOid),
								0, 0, 0);
		if (!OidIsValid(result))
			elog(ERROR, "No operator with oid %s", opr_name_or_oid);
		PG_RETURN_OID(result);
	}

	/* Else it's a name, possibly schema-qualified */

	/*
	 * In bootstrap mode we assume the given name is not schema-qualified,
	 * and just search pg_operator for a unique match.  This is needed for
	 * initializing other system catalogs (pg_namespace may not exist yet,
	 * and certainly there are no schemas other than pg_catalog).
	 */
	if (IsBootstrapProcessingMode())
	{
		int			matches = 0;
		Relation	hdesc;
		ScanKeyData skey[1];
		SysScanDesc	sysscan;
		HeapTuple	tuple;

		ScanKeyEntryInitialize(&skey[0], 0x0,
							   (AttrNumber) Anum_pg_operator_oprname,
							   (RegProcedure) F_NAMEEQ,
							   CStringGetDatum(opr_name_or_oid));

		hdesc = heap_openr(OperatorRelationName, AccessShareLock);
		sysscan = systable_beginscan(hdesc, OperatorNameNspIndex, true,
									 SnapshotNow, 1, skey);

		while (HeapTupleIsValid(tuple = systable_getnext(sysscan)))
		{
			AssertTupleDescHasOid(hdesc->rd_att);
			result = HeapTupleGetOid(tuple);
			if (++matches > 1)
				break;
		}

		systable_endscan(sysscan);
		heap_close(hdesc, AccessShareLock);

		if (matches == 0)
			elog(ERROR, "No operator with name %s", opr_name_or_oid);
		else if (matches > 1)
			elog(ERROR, "There is more than one operator named %s",
				 opr_name_or_oid);
		PG_RETURN_OID(result);
	}

	/*
	 * Normal case: parse the name into components and see if it
	 * matches any pg_operator entries in the current search path.
	 */
	names = stringToQualifiedNameList(opr_name_or_oid, "regoperin");
	clist = OpernameGetCandidates(names, '\0');

	if (clist == NULL)
		elog(ERROR, "No operator with name %s", opr_name_or_oid);
	else if (clist->next != NULL)
		elog(ERROR, "There is more than one operator named %s",
			 opr_name_or_oid);

	result = clist->oid;

	PG_RETURN_OID(result);
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

	opertup = SearchSysCache(OPEROID,
							 ObjectIdGetDatum(oprid),
							 0, 0, 0);

	if (HeapTupleIsValid(opertup))
	{
		Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
		char	   *oprname = NameStr(operform->oprname);

		/*
		 * In bootstrap mode, skip the fancy namespace stuff and just
		 * return the oper name.  (This path is only needed for debugging
		 * output anyway.)
		 */
		if (IsBootstrapProcessingMode())
		{
			result = pstrdup(oprname);
		}
		else
		{
			FuncCandidateList clist;

			/*
			 * Would this oper be found (uniquely!) by regoperin?
			 * If not, qualify it.
			 */
			clist = OpernameGetCandidates(makeList1(makeString(oprname)),
										  '\0');
			if (clist != NULL && clist->next == NULL &&
				clist->oid == oprid)
				result = pstrdup(oprname);
			else
			{
				const char *nspname;

				nspname = get_namespace_name(operform->oprnamespace);
				nspname = quote_identifier(nspname);
				result = (char *) palloc(strlen(nspname)+strlen(oprname)+2);
				sprintf(result, "%s.%s", nspname, oprname);
			}
		}

		ReleaseSysCache(opertup);
	}
	else
	{
		/* If OID doesn't match any pg_operator entry, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", oprid);
	}

	PG_RETURN_CSTRING(result);
}


/*
 * regoperatorin		- converts "oprname(args)" to operator OID
 *
 * We also accept a numeric OID, mostly for historical reasons.
 *
 * '0' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_operator entry.
 */
Datum
regoperatorin(PG_FUNCTION_ARGS)
{
	char	   *opr_name_or_oid = PG_GETARG_CSTRING(0);
	Oid			result = InvalidOid;
	List	   *names;
	int			nargs;
	Oid			argtypes[FUNC_MAX_ARGS];
	char		oprkind;
	FuncCandidateList clist;

	/* '0' ? */
	if (strcmp(opr_name_or_oid, "0") == 0)
		PG_RETURN_OID(InvalidOid);

	/* Numeric OID? */
	if (opr_name_or_oid[0] >= '0' &&
		opr_name_or_oid[0] <= '9' &&
		strspn(opr_name_or_oid, "0123456789") == strlen(opr_name_or_oid))
	{
		Oid			searchOid;

		searchOid = DatumGetObjectId(DirectFunctionCall1(oidin,
									  CStringGetDatum(opr_name_or_oid)));
		result = GetSysCacheOid(OPEROID,
								ObjectIdGetDatum(searchOid),
								0, 0, 0);
		if (!OidIsValid(result))
			elog(ERROR, "No operator with oid %s", opr_name_or_oid);
		PG_RETURN_OID(result);
	}

	/*
	 * Else it's a name and arguments.  Parse the name and arguments,
	 * look up potential matches in the current namespace search list,
	 * and scan to see which one exactly matches the given argument
	 * types.  (There will not be more than one match.)
	 *
	 * XXX at present, this code will not work in bootstrap mode, hence this
	 * datatype cannot be used for any system column that needs to receive
	 * data during bootstrap.
	 */
	parseNameAndArgTypes(opr_name_or_oid, "regoperatorin", "none",
						 &names, &nargs, argtypes);
	if (nargs == 1)
		elog(ERROR, "regoperatorin: use NONE to denote the missing argument of a unary operator");
	if (nargs != 2)
		elog(ERROR, "regoperatorin: provide two argument types for operator");

	if (argtypes[0] == InvalidOid)
		oprkind = 'l';
	else if (argtypes[1] == InvalidOid)
		oprkind = 'r';
	else
		oprkind = 'b';

	clist = OpernameGetCandidates(names, oprkind);

	for (; clist; clist = clist->next)
	{
		if (memcmp(clist->args, argtypes, 2 * sizeof(Oid)) == 0)
			break;
	}

	if (clist == NULL)
		elog(ERROR, "No operator with name %s", opr_name_or_oid);

	result = clist->oid;

	PG_RETURN_OID(result);
}

/*
 * format_operator		- converts operator OID to "opr_name(args)"
 *
 * This exports the useful functionality of regoperatorout for use
 * in other backend modules.  The result is a palloc'd string.
 */
char *
format_operator(Oid operator_oid)
{
	char	   *result;
	HeapTuple	opertup;

	opertup = SearchSysCache(OPEROID,
							 ObjectIdGetDatum(operator_oid),
							 0, 0, 0);

	if (HeapTupleIsValid(opertup))
	{
		Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
		char	   *oprname = NameStr(operform->oprname);
		char	   *nspname;
		StringInfoData buf;

		/* XXX no support here for bootstrap mode */

		initStringInfo(&buf);

		/*
		 * Would this oper be found (given the right args) by regoperatorin?
		 * If not, we need to qualify it.
		 */
		if (!OperatorIsVisible(operator_oid))
		{
			nspname = get_namespace_name(operform->oprnamespace);
			appendStringInfo(&buf, "%s.",
							 quote_identifier(nspname));
		}

		appendStringInfo(&buf, "%s(", oprname);

		if (operform->oprleft)
			appendStringInfo(&buf, "%s,",
							 format_type_be(operform->oprleft));
		else
			appendStringInfo(&buf, "NONE,");

		if (operform->oprright)
			appendStringInfo(&buf, "%s)",
							 format_type_be(operform->oprright));
		else
			appendStringInfo(&buf, "NONE)");

		result = buf.data;

		ReleaseSysCache(opertup);
	}
	else
	{
		/* If OID doesn't match any pg_operator entry, return it numerically */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", operator_oid);
	}

	return result;
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
 * regclassin		- converts "classname" to class OID
 *
 * We also accept a numeric OID, mostly for historical reasons.
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
		Oid			searchOid;

		searchOid = DatumGetObjectId(DirectFunctionCall1(oidin,
									  CStringGetDatum(class_name_or_oid)));
		result = GetSysCacheOid(RELOID,
								ObjectIdGetDatum(searchOid),
								0, 0, 0);
		if (!OidIsValid(result))
			elog(ERROR, "No class with oid %s", class_name_or_oid);
		PG_RETURN_OID(result);
	}

	/* Else it's a name, possibly schema-qualified */

	/*
	 * In bootstrap mode we assume the given name is not schema-qualified,
	 * and just search pg_class for a match.  This is needed for
	 * initializing other system catalogs (pg_namespace may not exist yet,
	 * and certainly there are no schemas other than pg_catalog).
	 */
	if (IsBootstrapProcessingMode())
	{
		Relation	hdesc;
		ScanKeyData skey[1];
		SysScanDesc	sysscan;
		HeapTuple	tuple;

		ScanKeyEntryInitialize(&skey[0], 0x0,
							   (AttrNumber) Anum_pg_class_relname,
							   (RegProcedure) F_NAMEEQ,
							   CStringGetDatum(class_name_or_oid));

		hdesc = heap_openr(RelationRelationName, AccessShareLock);
		sysscan = systable_beginscan(hdesc, ClassNameNspIndex, true,
									 SnapshotNow, 1, skey);

		if (HeapTupleIsValid(tuple = systable_getnext(sysscan)))
		{
			AssertTupleDescHasOid(hdesc->rd_att);
			result = HeapTupleGetOid(tuple);
		}
		else
			elog(ERROR, "No class with name %s", class_name_or_oid);

		/* We assume there can be only one match */

		systable_endscan(sysscan);
		heap_close(hdesc, AccessShareLock);

		PG_RETURN_OID(result);
	}

	/*
	 * Normal case: parse the name into components and see if it
	 * matches any pg_class entries in the current search path.
	 */
	names = stringToQualifiedNameList(class_name_or_oid, "regclassin");

	result = RangeVarGetRelid(makeRangeVarFromNameList(names), false);

	PG_RETURN_OID(result);
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

	classtup = SearchSysCache(RELOID,
							  ObjectIdGetDatum(classid),
							  0, 0, 0);

	if (HeapTupleIsValid(classtup))
	{
		Form_pg_class classform = (Form_pg_class) GETSTRUCT(classtup);
		char	   *classname = NameStr(classform->relname);

		/*
		 * In bootstrap mode, skip the fancy namespace stuff and just
		 * return the class name.  (This path is only needed for debugging
		 * output anyway.)
		 */
		if (IsBootstrapProcessingMode())
		{
			result = pstrdup(classname);
		}
		else
		{
			char	   *nspname;

			/*
			 * Would this class be found by regclassin?
			 * If not, qualify it.
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
 * regtypein		- converts "typename" to type OID
 *
 * We also accept a numeric OID, mostly for historical reasons.
 *
 * '-' signifies unknown (OID 0).  In all other cases, the input must
 * match an existing pg_type entry.
 *
 * In bootstrap mode the name must just equal some existing name in pg_type.
 * In normal mode the type name can be specified using the full type syntax
 * recognized by the parser; for example, DOUBLE PRECISION and INTEGER[] will
 * work and be translated to the correct type names.  (We ignore any typmod
 * info generated by the parser, however.)
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
		Oid			searchOid;

		searchOid = DatumGetObjectId(DirectFunctionCall1(oidin,
									  CStringGetDatum(typ_name_or_oid)));
		result = GetSysCacheOid(TYPEOID,
								ObjectIdGetDatum(searchOid),
								0, 0, 0);
		if (!OidIsValid(result))
			elog(ERROR, "No type with oid %s", typ_name_or_oid);
		PG_RETURN_OID(result);
	}

	/* Else it's a type name, possibly schema-qualified or decorated */

	/*
	 * In bootstrap mode we assume the given name is not schema-qualified,
	 * and just search pg_type for a match.  This is needed for
	 * initializing other system catalogs (pg_namespace may not exist yet,
	 * and certainly there are no schemas other than pg_catalog).
	 */
	if (IsBootstrapProcessingMode())
	{
		Relation	hdesc;
		ScanKeyData skey[1];
		SysScanDesc	sysscan;
		HeapTuple	tuple;

		ScanKeyEntryInitialize(&skey[0], 0x0,
							   (AttrNumber) Anum_pg_type_typname,
							   (RegProcedure) F_NAMEEQ,
							   CStringGetDatum(typ_name_or_oid));

		hdesc = heap_openr(TypeRelationName, AccessShareLock);
		sysscan = systable_beginscan(hdesc, TypeNameNspIndex, true,
									 SnapshotNow, 1, skey);

		if (HeapTupleIsValid(tuple = systable_getnext(sysscan)))
		{
			AssertTupleDescHasOid(hdesc->rd_att);
			result = HeapTupleGetOid(tuple);
		}
		else
			elog(ERROR, "No type with name %s", typ_name_or_oid);

		/* We assume there can be only one match */

		systable_endscan(sysscan);
		heap_close(hdesc, AccessShareLock);

		PG_RETURN_OID(result);
	}

	/*
	 * Normal case: invoke the full parser to deal with special cases
	 * such as array syntax.
	 */
	parseTypeString(typ_name_or_oid, &result, &typmod);

	PG_RETURN_OID(result);
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

	typetup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(typid),
							 0, 0, 0);

	if (HeapTupleIsValid(typetup))
	{
		Form_pg_type typeform = (Form_pg_type) GETSTRUCT(typetup);

		/*
		 * In bootstrap mode, skip the fancy namespace stuff and just
		 * return the type name.  (This path is only needed for debugging
		 * output anyway.)
		 */
		if (IsBootstrapProcessingMode())
		{
			char	   *typname = NameStr(typeform->typname);

			result = pstrdup(typname);
		}
		else
		{
			result = format_type_be(typid);
		}

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
 * Given a C string, parse it into a qualified-name list.
 */
List *
stringToQualifiedNameList(const char *string, const char *caller)
{
	char	   *rawname;
	List	   *result = NIL;
	List	   *namelist;
	List	   *l;

	/* We need a modifiable copy of the input string. */
	rawname = pstrdup(string);

	if (!SplitIdentifierString(rawname, '.', &namelist))
		elog(ERROR, "%s: invalid name syntax", caller);

	if (namelist == NIL)
		elog(ERROR, "%s: invalid name syntax", caller);

	foreach(l, namelist)
	{
		char   *curname = (char *) lfirst(l);

		result = lappend(result, makeString(pstrdup(curname)));
	}

	pfree(rawname);
	freeList(namelist);

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
 * If type0_spelling is not NULL, it is a name to be accepted as a
 * placeholder for OID 0.
 */
static void
parseNameAndArgTypes(const char *string, const char *caller,
					 const char *type0_spelling,
					 List **names, int *nargs, Oid *argtypes)
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
		elog(ERROR, "%s: expected a left parenthesis", caller);

	/* Separate the name and parse it into a list */
	*ptr++ = '\0';
	*names = stringToQualifiedNameList(rawname, caller);

	/* Check for the trailing right parenthesis and remove it */
	ptr2 = ptr + strlen(ptr);
	while (--ptr2 > ptr)
	{
		if (!isspace((unsigned char) *ptr2))
			break;
	}
	if (*ptr2 != ')')
		elog(ERROR, "%s: expected a right parenthesis", caller);
	*ptr2 = '\0';

	/* Separate the remaining string into comma-separated type names */
	*nargs = 0;
	had_comma = false;

	for (;;)
	{
		/* allow leading whitespace */
		while (isspace((unsigned char) *ptr))
			ptr++;
		if (*ptr == '\0')
		{
			/* End of string.  Okay unless we had a comma before. */
			if (had_comma)
				elog(ERROR, "%s: expected a type name", caller);
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
			elog(ERROR, "%s: improper type name", caller);
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
			if (!isspace((unsigned char) *ptr2))
				break;
			*ptr2 = '\0';
		}

		if (type0_spelling && strcasecmp(typename, type0_spelling) == 0)
		{
			/* Special case for OPAQUE or NONE */
			typeid = InvalidOid;
			typmod = -1;
		}
		else
		{
			/* Use full parser to resolve the type name */
			parseTypeString(typename, &typeid, &typmod);
		}
		if (*nargs >= FUNC_MAX_ARGS)
			elog(ERROR, "%s: too many argument datatypes", caller);
		argtypes[*nargs] = typeid;
		(*nargs)++;
	}

	pfree(rawname);
}
