/*-------------------------------------------------------------------------
 *
 * namespace.c
 *	  code to support accessing and searching namespaces
 *
 * This is separate from pg_namespace.c, which contains the routines that
 * directly manipulate the pg_namespace system catalog.  This module
 * provides routines associated with defining a "namespace search path"
 * and implementing search-path-controlled searches.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/namespace.c,v 1.58 2003/09/25 06:57:57 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


/*
 * The namespace search path is a possibly-empty list of namespace OIDs.
 * In addition to the explicit list, several implicitly-searched namespaces
 * may be included:
 *
 * 1. If a "special" namespace has been set by PushSpecialNamespace, it is
 * always searched first.  (This is a hack for CREATE SCHEMA.)
 *
 * 2. If a TEMP table namespace has been initialized in this session, it
 * is always searched just after any special namespace.
 *
 * 3. The system catalog namespace is always searched.	If the system
 * namespace is present in the explicit path then it will be searched in
 * the specified order; otherwise it will be searched after TEMP tables and
 * *before* the explicit list.	(It might seem that the system namespace
 * should be implicitly last, but this behavior appears to be required by
 * SQL99.  Also, this provides a way to search the system namespace first
 * without thereby making it the default creation target namespace.)
 *
 * The default creation target namespace is normally equal to the first
 * element of the explicit list, but is the "special" namespace when one
 * has been set.  If the explicit list is empty and there is no special
 * namespace, there is no default target.
 *
 * In bootstrap mode, the search path is set equal to 'pg_catalog', so that
 * the system namespace is the only one searched or inserted into.
 * The initdb script is also careful to set search_path to 'pg_catalog' for
 * its post-bootstrap standalone backend runs.	Otherwise the default search
 * path is determined by GUC.  The factory default path contains the PUBLIC
 * namespace (if it exists), preceded by the user's personal namespace
 * (if one exists).
 *
 * If namespaceSearchPathValid is false, then namespaceSearchPath (and other
 * derived variables) need to be recomputed from namespace_search_path.
 * We mark it invalid upon an assignment to namespace_search_path or receipt
 * of a syscache invalidation event for pg_namespace.  The recomputation
 * is done during the next lookup attempt.
 *
 * Any namespaces mentioned in namespace_search_path that are not readable
 * by the current user ID are simply left out of namespaceSearchPath; so
 * we have to be willing to recompute the path when current userid changes.
 * namespaceUser is the userid the path has been computed for.
 */

static List *namespaceSearchPath = NIL;

static Oid	namespaceUser = InvalidOid;

/* default place to create stuff; if InvalidOid, no default */
static Oid	defaultCreationNamespace = InvalidOid;

/* first explicit member of list; usually same as defaultCreationNamespace */
static Oid	firstExplicitNamespace = InvalidOid;

/* The above four values are valid only if namespaceSearchPathValid */
static bool namespaceSearchPathValid = true;

/*
 * myTempNamespace is InvalidOid until and unless a TEMP namespace is set up
 * in a particular backend session (this happens when a CREATE TEMP TABLE
 * command is first executed).	Thereafter it's the OID of the temp namespace.
 * firstTempTransaction flags whether we've committed creation of the TEMP
 * namespace or not.
 */
static Oid	myTempNamespace = InvalidOid;

static bool firstTempTransaction = false;

/*
 * "Special" namespace for CREATE SCHEMA.  If set, it's the first search
 * path element, and also the default creation namespace.
 */
static Oid	mySpecialNamespace = InvalidOid;

/*
 * This is the text equivalent of the search path --- it's the value
 * of the GUC variable 'search_path'.
 */
char	   *namespace_search_path = NULL;


/* Local functions */
static void recomputeNamespacePath(void);
static void InitTempTableNamespace(void);
static void RemoveTempRelations(Oid tempNamespaceId);
static void RemoveTempRelationsCallback(void);
static void NamespaceCallback(Datum arg, Oid relid);

/* These don't really need to appear in any header file */
Datum		pg_table_is_visible(PG_FUNCTION_ARGS);
Datum		pg_type_is_visible(PG_FUNCTION_ARGS);
Datum		pg_function_is_visible(PG_FUNCTION_ARGS);
Datum		pg_operator_is_visible(PG_FUNCTION_ARGS);
Datum		pg_opclass_is_visible(PG_FUNCTION_ARGS);
Datum		pg_conversion_is_visible(PG_FUNCTION_ARGS);


/*
 * RangeVarGetRelid
 *		Given a RangeVar describing an existing relation,
 *		select the proper namespace and look up the relation OID.
 *
 * If the relation is not found, return InvalidOid if failOK = true,
 * otherwise raise an error.
 */
Oid
RangeVarGetRelid(const RangeVar *relation, bool failOK)
{
	Oid			namespaceId;
	Oid			relId;

	/*
	 * We check the catalog name and then ignore it.
	 */
	if (relation->catalogname)
	{
		if (strcmp(relation->catalogname, get_database_name(MyDatabaseId)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("cross-database references are not implemented")));
	}

	if (relation->schemaname)
	{
		/* use exact schema given */
		namespaceId = LookupExplicitNamespace(relation->schemaname);
		relId = get_relname_relid(relation->relname, namespaceId);
	}
	else
	{
		/* search the namespace path */
		relId = RelnameGetRelid(relation->relname);
	}

	if (!OidIsValid(relId) && !failOK)
	{
		if (relation->schemaname)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s.%s\" does not exist",
							relation->schemaname, relation->relname)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" does not exist",
							relation->relname)));
	}
	return relId;
}

/*
 * RangeVarGetCreationNamespace
 *		Given a RangeVar describing a to-be-created relation,
 *		choose which namespace to create it in.
 *
 * Note: calling this may result in a CommandCounterIncrement operation.
 * That will happen on the first request for a temp table in any particular
 * backend run; we will need to either create or clean out the temp schema.
 */
Oid
RangeVarGetCreationNamespace(const RangeVar *newRelation)
{
	Oid			namespaceId;

	/*
	 * We check the catalog name and then ignore it.
	 */
	if (newRelation->catalogname)
	{
		if (strcmp(newRelation->catalogname, get_database_name(MyDatabaseId)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("cross-database references are not implemented")));
	}

	if (newRelation->istemp)
	{
		/* TEMP tables are created in our backend-local temp namespace */
		if (newRelation->schemaname)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				   errmsg("temporary tables may not specify a schema name")));
		/* Initialize temp namespace if first time through */
		if (!OidIsValid(myTempNamespace))
			InitTempTableNamespace();
		return myTempNamespace;
	}

	if (newRelation->schemaname)
	{
		/* use exact schema given */
		namespaceId = GetSysCacheOid(NAMESPACENAME,
								CStringGetDatum(newRelation->schemaname),
									 0, 0, 0);
		if (!OidIsValid(namespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("schema \"%s\" does not exist",
							newRelation->schemaname)));
		/* we do not check for USAGE rights here! */
	}
	else
	{
		/* use the default creation namespace */
		recomputeNamespacePath();
		namespaceId = defaultCreationNamespace;
		if (!OidIsValid(namespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
	}

	/* Note: callers will check for CREATE rights when appropriate */

	return namespaceId;
}

/*
 * RelnameGetRelid
 *		Try to resolve an unqualified relation name.
 *		Returns OID if relation found in search path, else InvalidOid.
 */
Oid
RelnameGetRelid(const char *relname)
{
	Oid			relid;
	List	   *lptr;

	recomputeNamespacePath();

	foreach(lptr, namespaceSearchPath)
	{
		Oid			namespaceId = lfirsto(lptr);

		relid = get_relname_relid(relname, namespaceId);
		if (OidIsValid(relid))
			return relid;
	}

	/* Not found in path */
	return InvalidOid;
}


/*
 * RelationIsVisible
 *		Determine whether a relation (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified relation name".
 */
bool
RelationIsVisible(Oid relid)
{
	HeapTuple	reltup;
	Form_pg_class relform;
	Oid			relnamespace;
	bool		visible;

	reltup = SearchSysCache(RELOID,
							ObjectIdGetDatum(relid),
							0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	relform = (Form_pg_class) GETSTRUCT(reltup);

	recomputeNamespacePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible.
	 * Items in the system namespace are surely in the path and so we
	 * needn't even do oidMember() for them.
	 */
	relnamespace = relform->relnamespace;
	if (relnamespace != PG_CATALOG_NAMESPACE &&
		!oidMember(relnamespace, namespaceSearchPath))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could
		 * be hidden by another relation of the same name earlier in the
		 * path. So we must do a slow check to see if this rel would be
		 * found by RelnameGetRelid.
		 */
		char	   *relname = NameStr(relform->relname);

		visible = (RelnameGetRelid(relname) == relid);
	}

	ReleaseSysCache(reltup);

	return visible;
}


/*
 * TypenameGetTypid
 *		Try to resolve an unqualified datatype name.
 *		Returns OID if type found in search path, else InvalidOid.
 *
 * This is essentially the same as RelnameGetRelid.
 */
Oid
TypenameGetTypid(const char *typname)
{
	Oid			typid;
	List	   *lptr;

	recomputeNamespacePath();

	foreach(lptr, namespaceSearchPath)
	{
		Oid			namespaceId = lfirsto(lptr);

		typid = GetSysCacheOid(TYPENAMENSP,
							   PointerGetDatum(typname),
							   ObjectIdGetDatum(namespaceId),
							   0, 0);
		if (OidIsValid(typid))
			return typid;
	}

	/* Not found in path */
	return InvalidOid;
}

/*
 * TypeIsVisible
 *		Determine whether a type (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified type name".
 */
bool
TypeIsVisible(Oid typid)
{
	HeapTuple	typtup;
	Form_pg_type typform;
	Oid			typnamespace;
	bool		visible;

	typtup = SearchSysCache(TYPEOID,
							ObjectIdGetDatum(typid),
							0, 0, 0);
	if (!HeapTupleIsValid(typtup))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typform = (Form_pg_type) GETSTRUCT(typtup);

	recomputeNamespacePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible.
	 * Items in the system namespace are surely in the path and so we
	 * needn't even do oidMember() for them.
	 */
	typnamespace = typform->typnamespace;
	if (typnamespace != PG_CATALOG_NAMESPACE &&
		!oidMember(typnamespace, namespaceSearchPath))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could
		 * be hidden by another type of the same name earlier in the path.
		 * So we must do a slow check to see if this type would be found
		 * by TypenameGetTypid.
		 */
		char	   *typname = NameStr(typform->typname);

		visible = (TypenameGetTypid(typname) == typid);
	}

	ReleaseSysCache(typtup);

	return visible;
}


/*
 * FuncnameGetCandidates
 *		Given a possibly-qualified function name and argument count,
 *		retrieve a list of the possible matches.
 *
 * If nargs is -1, we return all functions matching the given name,
 * regardless of argument count.
 *
 * We search a single namespace if the function name is qualified, else
 * all namespaces in the search path.  The return list will never contain
 * multiple entries with identical argument lists --- in the multiple-
 * namespace case, we arrange for entries in earlier namespaces to mask
 * identical entries in later namespaces.
 */
FuncCandidateList
FuncnameGetCandidates(List *names, int nargs)
{
	FuncCandidateList resultList = NULL;
	char	   *schemaname;
	char	   *funcname;
	Oid			namespaceId;
	CatCList   *catlist;
	int			i;

	/* deconstruct the name list */
	DeconstructQualifiedName(names, &schemaname, &funcname);

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = LookupExplicitNamespace(schemaname);
	}
	else
	{
		/* flag to indicate we need namespace search */
		namespaceId = InvalidOid;
		recomputeNamespacePath();
	}

	/* Search syscache by name and (optionally) nargs only */
	if (nargs >= 0)
		catlist = SearchSysCacheList(PROCNAMENSP, 2,
									 CStringGetDatum(funcname),
									 Int16GetDatum(nargs),
									 0, 0);
	else
		catlist = SearchSysCacheList(PROCNAMENSP, 1,
									 CStringGetDatum(funcname),
									 0, 0, 0);

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	proctup = &catlist->members[i]->tuple;
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		int			pathpos = 0;
		FuncCandidateList newResult;

		nargs = procform->pronargs;

		if (OidIsValid(namespaceId))
		{
			/* Consider only procs in specified namespace */
			if (procform->pronamespace != namespaceId)
				continue;
			/* No need to check args, they must all be different */
		}
		else
		{
			/* Consider only procs that are in the search path */
			List	   *nsp;

			foreach(nsp, namespaceSearchPath)
			{
				if (procform->pronamespace == lfirsto(nsp))
					break;
				pathpos++;
			}
			if (nsp == NIL)
				continue;		/* proc is not in search path */

			/*
			 * Okay, it's in the search path, but does it have the same
			 * arguments as something we already accepted?	If so, keep
			 * only the one that appears earlier in the search path.
			 *
			 * If we have an ordered list from SearchSysCacheList (the normal
			 * case), then any conflicting proc must immediately adjoin
			 * this one in the list, so we only need to look at the newest
			 * result item.  If we have an unordered list, we have to scan
			 * the whole result list.
			 */
			if (resultList)
			{
				FuncCandidateList prevResult;

				if (catlist->ordered)
				{
					if (nargs == resultList->nargs &&
						memcmp(procform->proargtypes, resultList->args,
							   nargs * sizeof(Oid)) == 0)
						prevResult = resultList;
					else
						prevResult = NULL;
				}
				else
				{
					for (prevResult = resultList;
						 prevResult;
						 prevResult = prevResult->next)
					{
						if (nargs == prevResult->nargs &&
						  memcmp(procform->proargtypes, prevResult->args,
								 nargs * sizeof(Oid)) == 0)
							break;
					}
				}
				if (prevResult)
				{
					/* We have a match with a previous result */
					Assert(pathpos != prevResult->pathpos);
					if (pathpos > prevResult->pathpos)
						continue;		/* keep previous result */
					/* replace previous result */
					prevResult->pathpos = pathpos;
					prevResult->oid = HeapTupleGetOid(proctup);
					continue;	/* args are same, of course */
				}
			}
		}

		/*
		 * Okay to add it to result list
		 */
		newResult = (FuncCandidateList)
			palloc(sizeof(struct _FuncCandidateList) - sizeof(Oid)
				   + nargs * sizeof(Oid));
		newResult->pathpos = pathpos;
		newResult->oid = HeapTupleGetOid(proctup);
		newResult->nargs = nargs;
		memcpy(newResult->args, procform->proargtypes, nargs * sizeof(Oid));

		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
}

/*
 * FunctionIsVisible
 *		Determine whether a function (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified function name with exact argument matches".
 */
bool
FunctionIsVisible(Oid funcid)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	Oid			pronamespace;
	bool		visible;

	proctup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(funcid),
							 0, 0, 0);
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	recomputeNamespacePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible.
	 * Items in the system namespace are surely in the path and so we
	 * needn't even do oidMember() for them.
	 */
	pronamespace = procform->pronamespace;
	if (pronamespace != PG_CATALOG_NAMESPACE &&
		!oidMember(pronamespace, namespaceSearchPath))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could
		 * be hidden by another proc of the same name and arguments
		 * earlier in the path.  So we must do a slow check to see if this
		 * is the same proc that would be found by FuncnameGetCandidates.
		 */
		char	   *proname = NameStr(procform->proname);
		int			nargs = procform->pronargs;
		FuncCandidateList clist;

		visible = false;

		clist = FuncnameGetCandidates(makeList1(makeString(proname)), nargs);

		for (; clist; clist = clist->next)
		{
			if (memcmp(clist->args, procform->proargtypes,
					   nargs * sizeof(Oid)) == 0)
			{
				/* Found the expected entry; is it the right proc? */
				visible = (clist->oid == funcid);
				break;
			}
		}
	}

	ReleaseSysCache(proctup);

	return visible;
}


/*
 * OpernameGetCandidates
 *		Given a possibly-qualified operator name and operator kind,
 *		retrieve a list of the possible matches.
 *
 * If oprkind is '\0', we return all operators matching the given name,
 * regardless of arguments.
 *
 * We search a single namespace if the operator name is qualified, else
 * all namespaces in the search path.  The return list will never contain
 * multiple entries with identical argument lists --- in the multiple-
 * namespace case, we arrange for entries in earlier namespaces to mask
 * identical entries in later namespaces.
 *
 * The returned items always have two args[] entries --- one or the other
 * will be InvalidOid for a prefix or postfix oprkind.	nargs is 2, too.
 */
FuncCandidateList
OpernameGetCandidates(List *names, char oprkind)
{
	FuncCandidateList resultList = NULL;
	char	   *schemaname;
	char	   *opername;
	Oid			namespaceId;
	CatCList   *catlist;
	int			i;

	/* deconstruct the name list */
	DeconstructQualifiedName(names, &schemaname, &opername);

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = LookupExplicitNamespace(schemaname);
	}
	else
	{
		/* flag to indicate we need namespace search */
		namespaceId = InvalidOid;
		recomputeNamespacePath();
	}

	/* Search syscache by name only */
	catlist = SearchSysCacheList(OPERNAMENSP, 1,
								 CStringGetDatum(opername),
								 0, 0, 0);

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	opertup = &catlist->members[i]->tuple;
		Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
		int			pathpos = 0;
		FuncCandidateList newResult;

		/* Ignore operators of wrong kind, if specific kind requested */
		if (oprkind && operform->oprkind != oprkind)
			continue;

		if (OidIsValid(namespaceId))
		{
			/* Consider only opers in specified namespace */
			if (operform->oprnamespace != namespaceId)
				continue;
			/* No need to check args, they must all be different */
		}
		else
		{
			/* Consider only opers that are in the search path */
			List	   *nsp;

			foreach(nsp, namespaceSearchPath)
			{
				if (operform->oprnamespace == lfirsto(nsp))
					break;
				pathpos++;
			}
			if (nsp == NIL)
				continue;		/* oper is not in search path */

			/*
			 * Okay, it's in the search path, but does it have the same
			 * arguments as something we already accepted?	If so, keep
			 * only the one that appears earlier in the search path.
			 *
			 * If we have an ordered list from SearchSysCacheList (the normal
			 * case), then any conflicting oper must immediately adjoin
			 * this one in the list, so we only need to look at the newest
			 * result item.  If we have an unordered list, we have to scan
			 * the whole result list.
			 */
			if (resultList)
			{
				FuncCandidateList prevResult;

				if (catlist->ordered)
				{
					if (operform->oprleft == resultList->args[0] &&
						operform->oprright == resultList->args[1])
						prevResult = resultList;
					else
						prevResult = NULL;
				}
				else
				{
					for (prevResult = resultList;
						 prevResult;
						 prevResult = prevResult->next)
					{
						if (operform->oprleft == prevResult->args[0] &&
							operform->oprright == prevResult->args[1])
							break;
					}
				}
				if (prevResult)
				{
					/* We have a match with a previous result */
					Assert(pathpos != prevResult->pathpos);
					if (pathpos > prevResult->pathpos)
						continue;		/* keep previous result */
					/* replace previous result */
					prevResult->pathpos = pathpos;
					prevResult->oid = HeapTupleGetOid(opertup);
					continue;	/* args are same, of course */
				}
			}
		}

		/*
		 * Okay to add it to result list
		 */
		newResult = (FuncCandidateList)
			palloc(sizeof(struct _FuncCandidateList) + sizeof(Oid));
		newResult->pathpos = pathpos;
		newResult->oid = HeapTupleGetOid(opertup);
		newResult->nargs = 2;
		newResult->args[0] = operform->oprleft;
		newResult->args[1] = operform->oprright;
		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
}

/*
 * OperatorIsVisible
 *		Determine whether an operator (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified operator name with exact argument matches".
 */
bool
OperatorIsVisible(Oid oprid)
{
	HeapTuple	oprtup;
	Form_pg_operator oprform;
	Oid			oprnamespace;
	bool		visible;

	oprtup = SearchSysCache(OPEROID,
							ObjectIdGetDatum(oprid),
							0, 0, 0);
	if (!HeapTupleIsValid(oprtup))
		elog(ERROR, "cache lookup failed for operator %u", oprid);
	oprform = (Form_pg_operator) GETSTRUCT(oprtup);

	recomputeNamespacePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible.
	 * Items in the system namespace are surely in the path and so we
	 * needn't even do oidMember() for them.
	 */
	oprnamespace = oprform->oprnamespace;
	if (oprnamespace != PG_CATALOG_NAMESPACE &&
		!oidMember(oprnamespace, namespaceSearchPath))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could
		 * be hidden by another operator of the same name and arguments
		 * earlier in the path.  So we must do a slow check to see if this
		 * is the same operator that would be found by
		 * OpernameGetCandidates.
		 */
		char	   *oprname = NameStr(oprform->oprname);
		FuncCandidateList clist;

		visible = false;

		clist = OpernameGetCandidates(makeList1(makeString(oprname)),
									  oprform->oprkind);

		for (; clist; clist = clist->next)
		{
			if (clist->args[0] == oprform->oprleft &&
				clist->args[1] == oprform->oprright)
			{
				/* Found the expected entry; is it the right op? */
				visible = (clist->oid == oprid);
				break;
			}
		}
	}

	ReleaseSysCache(oprtup);

	return visible;
}


/*
 * OpclassGetCandidates
 *		Given an index access method OID, retrieve a list of all the
 *		opclasses for that AM that are visible in the search path.
 *
 * NOTE: the opcname_tmp field in the returned structs should not be used
 * by callers, because it points at syscache entries that we release at
 * the end of this routine.  If any callers needed the name information,
 * we could pstrdup() the names ... but at present it'd be wasteful.
 */
OpclassCandidateList
OpclassGetCandidates(Oid amid)
{
	OpclassCandidateList resultList = NULL;
	CatCList   *catlist;
	int			i;

	/* Search syscache by AM OID only */
	catlist = SearchSysCacheList(CLAAMNAMENSP, 1,
								 ObjectIdGetDatum(amid),
								 0, 0, 0);

	recomputeNamespacePath();

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	opctup = &catlist->members[i]->tuple;
		Form_pg_opclass opcform = (Form_pg_opclass) GETSTRUCT(opctup);
		int			pathpos = 0;
		OpclassCandidateList newResult;
		List	   *nsp;

		/* Consider only opclasses that are in the search path */
		foreach(nsp, namespaceSearchPath)
		{
			if (opcform->opcnamespace == lfirsto(nsp))
				break;
			pathpos++;
		}
		if (nsp == NIL)
			continue;			/* opclass is not in search path */

		/*
		 * Okay, it's in the search path, but does it have the same name
		 * as something we already accepted?  If so, keep only the one
		 * that appears earlier in the search path.
		 *
		 * If we have an ordered list from SearchSysCacheList (the normal
		 * case), then any conflicting opclass must immediately adjoin
		 * this one in the list, so we only need to look at the newest
		 * result item.  If we have an unordered list, we have to scan the
		 * whole result list.
		 */
		if (resultList)
		{
			OpclassCandidateList prevResult;

			if (catlist->ordered)
			{
				if (strcmp(NameStr(opcform->opcname),
						   resultList->opcname_tmp) == 0)
					prevResult = resultList;
				else
					prevResult = NULL;
			}
			else
			{
				for (prevResult = resultList;
					 prevResult;
					 prevResult = prevResult->next)
				{
					if (strcmp(NameStr(opcform->opcname),
							   prevResult->opcname_tmp) == 0)
						break;
				}
			}
			if (prevResult)
			{
				/* We have a match with a previous result */
				Assert(pathpos != prevResult->pathpos);
				if (pathpos > prevResult->pathpos)
					continue;	/* keep previous result */
				/* replace previous result */
				prevResult->opcname_tmp = NameStr(opcform->opcname);
				prevResult->pathpos = pathpos;
				prevResult->oid = HeapTupleGetOid(opctup);
				prevResult->opcintype = opcform->opcintype;
				prevResult->opcdefault = opcform->opcdefault;
				prevResult->opckeytype = opcform->opckeytype;
				continue;
			}
		}

		/*
		 * Okay to add it to result list
		 */
		newResult = (OpclassCandidateList)
			palloc(sizeof(struct _OpclassCandidateList));
		newResult->opcname_tmp = NameStr(opcform->opcname);
		newResult->pathpos = pathpos;
		newResult->oid = HeapTupleGetOid(opctup);
		newResult->opcintype = opcform->opcintype;
		newResult->opcdefault = opcform->opcdefault;
		newResult->opckeytype = opcform->opckeytype;
		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
}

/*
 * OpclassnameGetOpcid
 *		Try to resolve an unqualified index opclass name.
 *		Returns OID if opclass found in search path, else InvalidOid.
 *
 * This is essentially the same as TypenameGetTypid, but we have to have
 * an extra argument for the index AM OID.
 */
Oid
OpclassnameGetOpcid(Oid amid, const char *opcname)
{
	Oid			opcid;
	List	   *lptr;

	recomputeNamespacePath();

	foreach(lptr, namespaceSearchPath)
	{
		Oid			namespaceId = lfirsto(lptr);

		opcid = GetSysCacheOid(CLAAMNAMENSP,
							   ObjectIdGetDatum(amid),
							   PointerGetDatum(opcname),
							   ObjectIdGetDatum(namespaceId),
							   0);
		if (OidIsValid(opcid))
			return opcid;
	}

	/* Not found in path */
	return InvalidOid;
}

/*
 * OpclassIsVisible
 *		Determine whether an opclass (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified opclass name".
 */
bool
OpclassIsVisible(Oid opcid)
{
	HeapTuple	opctup;
	Form_pg_opclass opcform;
	Oid			opcnamespace;
	bool		visible;

	opctup = SearchSysCache(CLAOID,
							ObjectIdGetDatum(opcid),
							0, 0, 0);
	if (!HeapTupleIsValid(opctup))
		elog(ERROR, "cache lookup failed for opclass %u", opcid);
	opcform = (Form_pg_opclass) GETSTRUCT(opctup);

	recomputeNamespacePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible.
	 * Items in the system namespace are surely in the path and so we
	 * needn't even do oidMember() for them.
	 */
	opcnamespace = opcform->opcnamespace;
	if (opcnamespace != PG_CATALOG_NAMESPACE &&
		!oidMember(opcnamespace, namespaceSearchPath))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could
		 * be hidden by another opclass of the same name earlier in the
		 * path. So we must do a slow check to see if this opclass would
		 * be found by OpclassnameGetOpcid.
		 */
		char	   *opcname = NameStr(opcform->opcname);

		visible = (OpclassnameGetOpcid(opcform->opcamid, opcname) == opcid);
	}

	ReleaseSysCache(opctup);

	return visible;
}

/*
 * ConversionGetConid
 *		Try to resolve an unqualified conversion name.
 *		Returns OID if conversion found in search path, else InvalidOid.
 *
 * This is essentially the same as RelnameGetRelid.
 */
Oid
ConversionGetConid(const char *conname)
{
	Oid			conid;
	List	   *lptr;

	recomputeNamespacePath();

	foreach(lptr, namespaceSearchPath)
	{
		Oid			namespaceId = lfirsto(lptr);

		conid = GetSysCacheOid(CONNAMENSP,
							   PointerGetDatum(conname),
							   ObjectIdGetDatum(namespaceId),
							   0, 0);
		if (OidIsValid(conid))
			return conid;
	}

	/* Not found in path */
	return InvalidOid;
}

/*
 * ConversionIsVisible
 *		Determine whether a conversion (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified conversion name".
 */
bool
ConversionIsVisible(Oid conid)
{
	HeapTuple	contup;
	Form_pg_conversion conform;
	Oid			connamespace;
	bool		visible;

	contup = SearchSysCache(CONOID,
							ObjectIdGetDatum(conid),
							0, 0, 0);
	if (!HeapTupleIsValid(contup))
		elog(ERROR, "cache lookup failed for conversion %u", conid);
	conform = (Form_pg_conversion) GETSTRUCT(contup);

	recomputeNamespacePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible.
	 * Items in the system namespace are surely in the path and so we
	 * needn't even do oidMember() for them.
	 */
	connamespace = conform->connamespace;
	if (connamespace != PG_CATALOG_NAMESPACE &&
		!oidMember(connamespace, namespaceSearchPath))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could
		 * be hidden by another conversion of the same name earlier in the
		 * path. So we must do a slow check to see if this conversion
		 * would be found by ConversionGetConid.
		 */
		char	   *conname = NameStr(conform->conname);

		visible = (ConversionGetConid(conname) == conid);
	}

	ReleaseSysCache(contup);

	return visible;
}

/*
 * DeconstructQualifiedName
 *		Given a possibly-qualified name expressed as a list of String nodes,
 *		extract the schema name and object name.
 *
 * *nspname_p is set to NULL if there is no explicit schema name.
 */
void
DeconstructQualifiedName(List *names,
						 char **nspname_p,
						 char **objname_p)
{
	char	   *catalogname;
	char	   *schemaname = NULL;
	char	   *objname = NULL;

	switch (length(names))
	{
		case 1:
			objname = strVal(lfirst(names));
			break;
		case 2:
			schemaname = strVal(lfirst(names));
			objname = strVal(lsecond(names));
			break;
		case 3:
			catalogname = strVal(lfirst(names));
			schemaname = strVal(lsecond(names));
			objname = strVal(lthird(names));

			/*
			 * We check the catalog name and then ignore it.
			 */
			if (strcmp(catalogname, get_database_name(MyDatabaseId)) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cross-database references are not implemented")));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("improper qualified name (too many dotted names): %s",
				   NameListToString(names))));
			break;
	}

	*nspname_p = schemaname;
	*objname_p = objname;
}

/*
 * LookupExplicitNamespace
 *		Process an explicitly-specified schema name: look up the schema
 *		and verify we have USAGE (lookup) rights in it.
 *
 * Returns the namespace OID.  Raises ereport if any problem.
 */
Oid
LookupExplicitNamespace(const char *nspname)
{
	Oid			namespaceId;
	AclResult	aclresult;

	namespaceId = GetSysCacheOid(NAMESPACENAME,
								 CStringGetDatum(nspname),
								 0, 0, 0);
	if (!OidIsValid(namespaceId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", nspname)));

	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   nspname);

	return namespaceId;
}

/*
 * QualifiedNameGetCreationNamespace
 *		Given a possibly-qualified name for an object (in List-of-Values
 *		format), determine what namespace the object should be created in.
 *		Also extract and return the object name (last component of list).
 *
 * This is *not* used for tables.  Hence, the TEMP table namespace is
 * never selected as the creation target.
 */
Oid
QualifiedNameGetCreationNamespace(List *names, char **objname_p)
{
	char	   *schemaname;
	char	   *objname;
	Oid			namespaceId;

	/* deconstruct the name list */
	DeconstructQualifiedName(names, &schemaname, &objname);

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = GetSysCacheOid(NAMESPACENAME,
									 CStringGetDatum(schemaname),
									 0, 0, 0);
		if (!OidIsValid(namespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("schema \"%s\" does not exist", schemaname)));
		/* we do not check for USAGE rights here! */
	}
	else
	{
		/* use the default creation namespace */
		recomputeNamespacePath();
		namespaceId = defaultCreationNamespace;
		if (!OidIsValid(namespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
	}

	/* Note: callers will check for CREATE rights when appropriate */

	*objname_p = objname;
	return namespaceId;
}

/*
 * makeRangeVarFromNameList
 *		Utility routine to convert a qualified-name list into RangeVar form.
 */
RangeVar *
makeRangeVarFromNameList(List *names)
{
	RangeVar   *rel = makeRangeVar(NULL, NULL);

	switch (length(names))
	{
		case 1:
			rel->relname = strVal(lfirst(names));
			break;
		case 2:
			rel->schemaname = strVal(lfirst(names));
			rel->relname = strVal(lsecond(names));
			break;
		case 3:
			rel->catalogname = strVal(lfirst(names));
			rel->schemaname = strVal(lsecond(names));
			rel->relname = strVal(lthird(names));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("improper relation name (too many dotted names): %s",
					NameListToString(names))));
			break;
	}

	return rel;
}

/*
 * NameListToString
 *		Utility routine to convert a qualified-name list into a string.
 *
 * This is used primarily to form error messages, and so we do not quote
 * the list elements, for the sake of legibility.
 */
char *
NameListToString(List *names)
{
	StringInfoData string;
	List	   *l;

	initStringInfo(&string);

	foreach(l, names)
	{
		if (l != names)
			appendStringInfoChar(&string, '.');
		appendStringInfoString(&string, strVal(lfirst(l)));
	}

	return string.data;
}

/*
 * NameListToQuotedString
 *		Utility routine to convert a qualified-name list into a string.
 *
 * Same as above except that names will be double-quoted where necessary,
 * so the string could be re-parsed (eg, by textToQualifiedNameList).
 */
char *
NameListToQuotedString(List *names)
{
	StringInfoData string;
	List	   *l;

	initStringInfo(&string);

	foreach(l, names)
	{
		if (l != names)
			appendStringInfoChar(&string, '.');
		appendStringInfoString(&string, quote_identifier(strVal(lfirst(l))));
	}

	return string.data;
}

/*
 * isTempNamespace - is the given namespace my temporary-table namespace?
 */
bool
isTempNamespace(Oid namespaceId)
{
	if (OidIsValid(myTempNamespace) && myTempNamespace == namespaceId)
		return true;
	return false;
}

/*
 * isOtherTempNamespace - is the given namespace some other backend's
 * temporary-table namespace?
 */
bool
isOtherTempNamespace(Oid namespaceId)
{
	bool		result;
	char	   *nspname;

	/* If it's my own temp namespace, say "false" */
	if (isTempNamespace(namespaceId))
		return false;
	/* Else, if the namespace name starts with "pg_temp_", say "true" */
	nspname = get_namespace_name(namespaceId);
	if (!nspname)
		return false;			/* no such namespace? */
	result = (strncmp(nspname, "pg_temp_", 8) == 0);
	pfree(nspname);
	return result;
}

/*
 * PushSpecialNamespace - push a "special" namespace onto the front of the
 * search path.
 *
 * This is a slightly messy hack intended only for support of CREATE SCHEMA.
 * Although the API is defined to allow a stack of pushed namespaces, we
 * presently only support one at a time.
 *
 * The pushed namespace will be removed from the search path at end of
 * transaction, whether commit or abort.
 */
void
PushSpecialNamespace(Oid namespaceId)
{
	Assert(!OidIsValid(mySpecialNamespace));
	mySpecialNamespace = namespaceId;
	namespaceSearchPathValid = false;
}

/*
 * PopSpecialNamespace - remove previously pushed special namespace.
 */
void
PopSpecialNamespace(Oid namespaceId)
{
	Assert(mySpecialNamespace == namespaceId);
	mySpecialNamespace = InvalidOid;
	namespaceSearchPathValid = false;
}

/*
 * FindConversionByName - find a conversion by possibly qualified name
 */
Oid
FindConversionByName(List *name)
{
	char	   *schemaname;
	char	   *conversion_name;
	Oid			namespaceId;
	Oid			conoid;
	List	   *lptr;

	/* deconstruct the name list */
	DeconstructQualifiedName(name, &schemaname, &conversion_name);

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = LookupExplicitNamespace(schemaname);
		return FindConversion(conversion_name, namespaceId);
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(lptr, namespaceSearchPath)
		{
			namespaceId = lfirsto(lptr);
			conoid = FindConversion(conversion_name, namespaceId);
			if (OidIsValid(conoid))
				return conoid;
		}
	}

	/* Not found in path */
	return InvalidOid;
}

/*
 * FindDefaultConversionProc - find default encoding conversion proc
 */
Oid
FindDefaultConversionProc(int4 for_encoding, int4 to_encoding)
{
	Oid			proc;
	List	   *lptr;

	recomputeNamespacePath();

	foreach(lptr, namespaceSearchPath)
	{
		Oid			namespaceId = lfirsto(lptr);

		proc = FindDefaultConversion(namespaceId, for_encoding, to_encoding);
		if (OidIsValid(proc))
			return proc;
	}

	/* Not found in path */
	return InvalidOid;
}

/*
 * recomputeNamespacePath - recompute path derived variables if needed.
 */
static void
recomputeNamespacePath(void)
{
	AclId		userId = GetUserId();
	char	   *rawname;
	List	   *namelist;
	List	   *oidlist;
	List	   *newpath;
	List	   *l;
	Oid			firstNS;
	MemoryContext oldcxt;

	/*
	 * Do nothing if path is already valid.
	 */
	if (namespaceSearchPathValid && namespaceUser == userId)
		return;

	/* Need a modifiable copy of namespace_search_path string */
	rawname = pstrdup(namespace_search_path);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		/* this should not happen if GUC checked check_search_path */
		elog(ERROR, "invalid list syntax");
	}

	/*
	 * Convert the list of names to a list of OIDs.  If any names are not
	 * recognizable or we don't have read access, just leave them out of
	 * the list.  (We can't raise an error, since the search_path setting
	 * has already been accepted.)	Don't make duplicate entries, either.
	 */
	oidlist = NIL;
	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);
		Oid			namespaceId;

		if (strcmp(curname, "$user") == 0)
		{
			/* $user --- substitute namespace matching user name, if any */
			HeapTuple	tuple;

			tuple = SearchSysCache(SHADOWSYSID,
								   ObjectIdGetDatum(userId),
								   0, 0, 0);
			if (HeapTupleIsValid(tuple))
			{
				char	   *uname;

				uname = NameStr(((Form_pg_shadow) GETSTRUCT(tuple))->usename);
				namespaceId = GetSysCacheOid(NAMESPACENAME,
											 CStringGetDatum(uname),
											 0, 0, 0);
				ReleaseSysCache(tuple);
				if (OidIsValid(namespaceId) &&
					!oidMember(namespaceId, oidlist) &&
					pg_namespace_aclcheck(namespaceId, userId,
										  ACL_USAGE) == ACLCHECK_OK)
					oidlist = lappendo(oidlist, namespaceId);
			}
		}
		else
		{
			/* normal namespace reference */
			namespaceId = GetSysCacheOid(NAMESPACENAME,
										 CStringGetDatum(curname),
										 0, 0, 0);
			if (OidIsValid(namespaceId) &&
				!oidMember(namespaceId, oidlist) &&
				pg_namespace_aclcheck(namespaceId, userId,
									  ACL_USAGE) == ACLCHECK_OK)
				oidlist = lappendo(oidlist, namespaceId);
		}
	}

	/*
	 * Remember the first member of the explicit list.
	 */
	if (oidlist == NIL)
		firstNS = InvalidOid;
	else
		firstNS = lfirsto(oidlist);

	/*
	 * Add any implicitly-searched namespaces to the list.	Note these go
	 * on the front, not the back; also notice that we do not check USAGE
	 * permissions for these.
	 */
	if (!oidMember(PG_CATALOG_NAMESPACE, oidlist))
		oidlist = lconso(PG_CATALOG_NAMESPACE, oidlist);

	if (OidIsValid(myTempNamespace) &&
		!oidMember(myTempNamespace, oidlist))
		oidlist = lconso(myTempNamespace, oidlist);

	if (OidIsValid(mySpecialNamespace) &&
		!oidMember(mySpecialNamespace, oidlist))
		oidlist = lconso(mySpecialNamespace, oidlist);

	/*
	 * Now that we've successfully built the new list of namespace OIDs,
	 * save it in permanent storage.
	 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	newpath = listCopy(oidlist);
	MemoryContextSwitchTo(oldcxt);

	/* Now safe to assign to state variable. */
	freeList(namespaceSearchPath);
	namespaceSearchPath = newpath;

	/*
	 * Update info derived from search path.
	 */
	firstExplicitNamespace = firstNS;
	if (OidIsValid(mySpecialNamespace))
		defaultCreationNamespace = mySpecialNamespace;
	else
		defaultCreationNamespace = firstNS;

	/* Mark the path valid. */
	namespaceSearchPathValid = true;
	namespaceUser = userId;

	/* Clean up. */
	pfree(rawname);
	freeList(namelist);
	freeList(oidlist);
}

/*
 * InitTempTableNamespace
 *		Initialize temp table namespace on first use in a particular backend
 */
static void
InitTempTableNamespace(void)
{
	char		namespaceName[NAMEDATALEN];
	Oid			namespaceId;

	/*
	 * First, do permission check to see if we are authorized to make temp
	 * tables.	We use a nonstandard error message here since
	 * "databasename: permission denied" might be a tad cryptic.
	 *
	 * Note we apply the check to the session user, not the currently active
	 * userid, since we are not going to change our minds about temp table
	 * availability during the session.
	 */
	if (pg_database_aclcheck(MyDatabaseId, GetSessionUserId(),
							 ACL_CREATE_TEMP) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create temporary tables in database \"%s\"",
						get_database_name(MyDatabaseId))));

	snprintf(namespaceName, sizeof(namespaceName), "pg_temp_%d", MyBackendId);

	namespaceId = GetSysCacheOid(NAMESPACENAME,
								 CStringGetDatum(namespaceName),
								 0, 0, 0);
	if (!OidIsValid(namespaceId))
	{
		/*
		 * First use of this temp namespace in this database; create it.
		 * The temp namespaces are always owned by the superuser.  We
		 * leave their permissions at default --- i.e., no access except
		 * to superuser --- to ensure that unprivileged users can't peek
		 * at other backends' temp tables.  This works because the places
		 * that access the temp namespace for my own backend skip
		 * permissions checks on it.
		 */
		namespaceId = NamespaceCreate(namespaceName, BOOTSTRAP_USESYSID);
		/* Advance command counter to make namespace visible */
		CommandCounterIncrement();
	}
	else
	{
		/*
		 * If the namespace already exists, clean it out (in case the
		 * former owner crashed without doing so).
		 */
		RemoveTempRelations(namespaceId);
	}

	/*
	 * Okay, we've prepared the temp namespace ... but it's not committed
	 * yet, so all our work could be undone by transaction rollback.  Set
	 * flag for AtEOXact_Namespace to know what to do.
	 */
	myTempNamespace = namespaceId;

	firstTempTransaction = true;

	namespaceSearchPathValid = false;	/* need to rebuild list */
}

/*
 * End-of-transaction cleanup for namespaces.
 */
void
AtEOXact_Namespace(bool isCommit)
{
	/*
	 * If we abort the transaction in which a temp namespace was selected,
	 * we'll have to do any creation or cleanout work over again.  So,
	 * just forget the namespace entirely until next time.	On the other
	 * hand, if we commit then register an exit callback to clean out the
	 * temp tables at backend shutdown.  (We only want to register the
	 * callback once per session, so this is a good place to do it.)
	 */
	if (firstTempTransaction)
	{
		if (isCommit)
			on_shmem_exit(RemoveTempRelationsCallback, 0);
		else
		{
			myTempNamespace = InvalidOid;
			namespaceSearchPathValid = false;	/* need to rebuild list */
		}
		firstTempTransaction = false;
	}

	/*
	 * Clean up if someone failed to do PopSpecialNamespace
	 */
	if (OidIsValid(mySpecialNamespace))
	{
		mySpecialNamespace = InvalidOid;
		namespaceSearchPathValid = false;		/* need to rebuild list */
	}
}

/*
 * Remove all relations in the specified temp namespace.
 *
 * This is called at backend shutdown (if we made any temp relations).
 * It is also called when we begin using a pre-existing temp namespace,
 * in order to clean out any relations that might have been created by
 * a crashed backend.
 */
static void
RemoveTempRelations(Oid tempNamespaceId)
{
	ObjectAddress object;

	/*
	 * We want to get rid of everything in the target namespace, but not
	 * the namespace itself (deleting it only to recreate it later would
	 * be a waste of cycles).  We do this by finding everything that has a
	 * dependency on the namespace.
	 */
	object.classId = get_system_catalog_relid(NamespaceRelationName);
	object.objectId = tempNamespaceId;
	object.objectSubId = 0;

	deleteWhatDependsOn(&object, false);
}

/*
 * Callback to remove temp relations at backend exit.
 */
static void
RemoveTempRelationsCallback(void)
{
	if (OidIsValid(myTempNamespace))	/* should always be true */
	{
		/* Need to ensure we have a usable transaction. */
		AbortOutOfAnyTransaction();
		StartTransactionCommand();

		RemoveTempRelations(myTempNamespace);

		CommitTransactionCommand();
	}
}


/*
 * Routines for handling the GUC variable 'search_path'.
 */

/* assign_hook: validate new search_path, do extra actions as needed */
const char *
assign_search_path(const char *newval, bool doit, bool interactive)
{
	char	   *rawname;
	List	   *namelist;
	List	   *l;

	/* Need a modifiable copy of string */
	rawname = pstrdup(newval);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		pfree(rawname);
		freeList(namelist);
		return NULL;
	}

	/*
	 * If we aren't inside a transaction, we cannot do database access so
	 * cannot verify the individual names.	Must accept the list on faith.
	 */
	if (interactive && IsTransactionState())
	{
		/*
		 * Verify that all the names are either valid namespace names or
		 * "$user".  We do not require $user to correspond to a valid
		 * namespace.  We do not check for USAGE rights, either; should
		 * we?
		 */
		foreach(l, namelist)
		{
			char	   *curname = (char *) lfirst(l);

			if (strcmp(curname, "$user") == 0)
				continue;
			if (!SearchSysCacheExists(NAMESPACENAME,
									  CStringGetDatum(curname),
									  0, 0, 0))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_SCHEMA),
					   errmsg("schema \"%s\" does not exist", curname)));
		}
	}

	pfree(rawname);
	freeList(namelist);

	/*
	 * We mark the path as needing recomputation, but don't do anything
	 * until it's needed.  This avoids trying to do database access during
	 * GUC initialization.
	 */
	if (doit)
		namespaceSearchPathValid = false;

	return newval;
}

/*
 * InitializeSearchPath: initialize module during InitPostgres.
 *
 * This is called after we are up enough to be able to do catalog lookups.
 */
void
InitializeSearchPath(void)
{
	if (IsBootstrapProcessingMode())
	{
		/*
		 * In bootstrap mode, the search path must be 'pg_catalog' so that
		 * tables are created in the proper namespace; ignore the GUC
		 * setting.
		 */
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		namespaceSearchPath = makeListo1(PG_CATALOG_NAMESPACE);
		MemoryContextSwitchTo(oldcxt);
		defaultCreationNamespace = PG_CATALOG_NAMESPACE;
		firstExplicitNamespace = PG_CATALOG_NAMESPACE;
		namespaceSearchPathValid = true;
		namespaceUser = GetUserId();
	}
	else
	{
		/*
		 * In normal mode, arrange for a callback on any syscache
		 * invalidation of pg_namespace rows.
		 */
		CacheRegisterSyscacheCallback(NAMESPACEOID,
									  NamespaceCallback,
									  (Datum) 0);
		/* Force search path to be recomputed on next use */
		namespaceSearchPathValid = false;
	}
}

/*
 * NamespaceCallback
 *		Syscache inval callback function
 */
static void
NamespaceCallback(Datum arg, Oid relid)
{
	/* Force search path to be recomputed on next use */
	namespaceSearchPathValid = false;
}

/*
 * Fetch the active search path, expressed as a List of OIDs.
 *
 * The returned list includes the implicitly-prepended namespaces only if
 * includeImplicit is true.
 *
 * NB: caller must treat the list as read-only!
 */
List *
fetch_search_path(bool includeImplicit)
{
	List	   *result;

	recomputeNamespacePath();

	result = namespaceSearchPath;
	if (!includeImplicit)
	{
		while (result && lfirsto(result) != firstExplicitNamespace)
			result = lnext(result);
	}

	return result;
}

/*
 * Export the FooIsVisible functions as SQL-callable functions.
 */

Datum
pg_table_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	PG_RETURN_BOOL(RelationIsVisible(oid));
}

Datum
pg_type_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	PG_RETURN_BOOL(TypeIsVisible(oid));
}

Datum
pg_function_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	PG_RETURN_BOOL(FunctionIsVisible(oid));
}

Datum
pg_operator_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	PG_RETURN_BOOL(OperatorIsVisible(oid));
}

Datum
pg_opclass_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	PG_RETURN_BOOL(OpclassIsVisible(oid));
}

Datum
pg_conversion_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	PG_RETURN_BOOL(ConversionIsVisible(oid));
}
