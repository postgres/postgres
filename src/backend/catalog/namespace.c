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
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/namespace.c,v 1.10 2002/04/16 23:08:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shadow.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/backendid.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * The namespace search path is a possibly-empty list of namespace OIDs.
 * In addition to the explicit list, the TEMP table namespace is always
 * implicitly searched first (if it's been initialized).  Also, the system
 * catalog namespace is always searched.  If the system namespace is
 * explicitly present in the path then it will be searched in the specified
 * order; otherwise it will be searched after TEMP tables and *before* the
 * explicit list.  (It might seem that the system namespace should be
 * implicitly last, but this behavior appears to be required by SQL99.
 * Also, this provides a way to search the system namespace first without
 * thereby making it the default creation target namespace.)
 *
 * The default creation target namespace is kept equal to the first element
 * of the (explicit) list.  If the list is empty, there is no default target.
 *
 * In bootstrap mode, the search path is set equal to 'pg_catalog', so that
 * the system namespace is the only one searched or inserted into.
 * The initdb script is also careful to set search_path to 'pg_catalog' for
 * its post-bootstrap standalone backend runs.  Otherwise the default search
 * path is determined by GUC.  The factory default path contains the PUBLIC
 * namespace (if it exists), preceded by the user's personal namespace
 * (if one exists).
 */

static List *namespaceSearchPath = NIL;

/* this flag must be updated correctly when namespaceSearchPath is changed */
static bool pathContainsSystemNamespace = false;

/* default place to create stuff; if InvalidOid, no default */
static Oid	defaultCreationNamespace = InvalidOid;

/*
 * myTempNamespace is InvalidOid until and unless a TEMP namespace is set up
 * in a particular backend session (this happens when a CREATE TEMP TABLE
 * command is first executed).  Thereafter it's the OID of the temp namespace.
 */
static Oid	myTempNamespace = InvalidOid;

/*
 * This is the text equivalent of the search path --- it's the value
 * of the GUC variable 'search_path'.
 */
char *namespace_search_path = NULL;


/*
 * Deletion ordering constraint item.
 */
typedef struct DelConstraint
{
	Oid			referencer;		/* table to delete first */
	Oid			referencee;		/* table to delete second */
	int			pred;			/* workspace for TopoSortRels */
	struct DelConstraint *link;	/* workspace for TopoSortRels */
} DelConstraint;


/* Local functions */
static Oid	GetTempTableNamespace(void);
static void RemoveTempRelations(Oid tempNamespaceId);
static List *FindTempRelations(Oid tempNamespaceId);
static List *FindDeletionConstraints(List *relOids);
static List *TopoSortRels(List *relOids, List *constraintList);
static void RemoveTempRelationsCallback(void);


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
		if (strcmp(relation->catalogname, DatabaseName) != 0)
			elog(ERROR, "Cross-database references are not implemented");
	}

	if (relation->schemaname)
	{
		/* use exact schema given */
		namespaceId = GetSysCacheOid(NAMESPACENAME,
									 CStringGetDatum(relation->schemaname),
									 0, 0, 0);
		if (!OidIsValid(namespaceId))
			elog(ERROR, "Namespace \"%s\" does not exist",
				 relation->schemaname);
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
			elog(ERROR, "Relation \"%s\".\"%s\" does not exist",
				 relation->schemaname, relation->relname);
		else
			elog(ERROR, "Relation \"%s\" does not exist",
				 relation->relname);
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
		if (strcmp(newRelation->catalogname, DatabaseName) != 0)
			elog(ERROR, "Cross-database references are not implemented");
	}

	if (newRelation->istemp)
	{
		/* TEMP tables are created in our backend-local temp namespace */
		if (newRelation->schemaname)
			elog(ERROR, "TEMP tables may not specify a namespace");
		/* Initialize temp namespace if first time through */
		if (!OidIsValid(myTempNamespace))
			myTempNamespace = GetTempTableNamespace();
		return myTempNamespace;
	}

	if (newRelation->schemaname)
	{
		/* use exact schema given */
		namespaceId = GetSysCacheOid(NAMESPACENAME,
									 CStringGetDatum(newRelation->schemaname),
									 0, 0, 0);
		if (!OidIsValid(namespaceId))
			elog(ERROR, "Namespace \"%s\" does not exist",
				 newRelation->schemaname);
	}
	else
	{
		/* use the default creation namespace */
		namespaceId = defaultCreationNamespace;
		if (!OidIsValid(namespaceId))
			elog(ERROR, "No namespace has been selected to create in");
	}

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

	/*
	 * If a TEMP-table namespace has been set up, it is implicitly first
	 * in the search path.
	 */
	if (OidIsValid(myTempNamespace))
	{
		relid = get_relname_relid(relname, myTempNamespace);
		if (OidIsValid(relid))
			return relid;
	}

	/*
	 * If system namespace is not in path, implicitly search it before path
	 */
	if (!pathContainsSystemNamespace)
	{
		relid = get_relname_relid(relname, PG_CATALOG_NAMESPACE);
		if (OidIsValid(relid))
			return relid;
	}

	/*
	 * Else search the path
	 */
	foreach(lptr, namespaceSearchPath)
	{
		Oid			namespaceId = (Oid) lfirsti(lptr);

		relid = get_relname_relid(relname, namespaceId);
		if (OidIsValid(relid))
			return relid;
	}

	/* Not found in path */
	return InvalidOid;
}

/*
 * TypenameGetTypid
 *		Try to resolve an unqualified datatype name.
 *		Returns OID if type found in search path, else InvalidOid.
 *
 * This is essentially the same as RelnameGetRelid, but we never search
 * the TEMP table namespace --- there is no reason to refer to the types
 * of temp tables, AFAICS.
 */
Oid
TypenameGetTypid(const char *typname)
{
	Oid			typid;
	List	   *lptr;

	/*
	 * If system namespace is not in path, implicitly search it before path
	 */
	if (!pathContainsSystemNamespace)
	{
		typid = GetSysCacheOid(TYPENAMENSP,
							   PointerGetDatum(typname),
							   ObjectIdGetDatum(PG_CATALOG_NAMESPACE),
							   0, 0);
		if (OidIsValid(typid))
			return typid;
	}

	/*
	 * Else search the path
	 */
	foreach(lptr, namespaceSearchPath)
	{
		Oid			namespaceId = (Oid) lfirsti(lptr);

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
 * FuncnameGetCandidates
 *		Given a possibly-qualified function name and argument count,
 *		retrieve a list of the possible matches.
 *
 * We search a single namespace if the function name is qualified, else
 * all namespaces in the search path.  The return list will never contain
 * multiple entries with identical argument types --- in the multiple-
 * namespace case, we arrange for entries in earlier namespaces to mask
 * identical entries in later namespaces.
 */
FuncCandidateList
FuncnameGetCandidates(List *names, int nargs)
{
	FuncCandidateList resultList = NULL;
	char	   *catalogname;
	char	   *schemaname = NULL;
	char	   *funcname = NULL;
	Oid			namespaceId;
	CatCList   *catlist;
	int			i;

	/* deconstruct the name list */
	switch (length(names))
	{
		case 1:
			funcname = strVal(lfirst(names));
			break;
		case 2:
			schemaname = strVal(lfirst(names));
			funcname = strVal(lsecond(names));
			break;
		case 3:
			catalogname = strVal(lfirst(names));
			schemaname = strVal(lsecond(names));
			funcname = strVal(lfirst(lnext(lnext(names))));
			/*
			 * We check the catalog name and then ignore it.
			 */
			if (strcmp(catalogname, DatabaseName) != 0)
				elog(ERROR, "Cross-database references are not implemented");
			break;
		default:
			elog(ERROR, "Improper qualified name (too many dotted names)");
			break;
	}

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = GetSysCacheOid(NAMESPACENAME,
									 CStringGetDatum(schemaname),
									 0, 0, 0);
		if (!OidIsValid(namespaceId))
			elog(ERROR, "Namespace \"%s\" does not exist",
				 schemaname);
	}
	else
	{
		/* flag to indicate we need namespace search */
		namespaceId = InvalidOid;
	}

	/* Search syscache by name and nargs only */
	catlist = SearchSysCacheList(PROCNAMENSP, 2,
								 CStringGetDatum(funcname),
								 Int16GetDatum(nargs),
								 0, 0);

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	proctup = &catlist->members[i]->tuple;
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		int			pathpos = 0;
		FuncCandidateList newResult;

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
			if (pathContainsSystemNamespace ||
				!IsSystemNamespace(procform->pronamespace))
			{
				List	   *nsp;

				foreach(nsp, namespaceSearchPath)
				{
					pathpos++;
					if (procform->pronamespace == (Oid) lfirsti(nsp))
						break;
				}
				if (nsp == NIL)
					continue;	/* proc is not in search path */
			}

			/*
			 * Okay, it's in the search path, but does it have the same
			 * arguments as something we already accepted?  If so, keep
			 * only the one that appears earlier in the search path.
			 *
			 * If we have an ordered list from SearchSysCacheList (the
			 * normal case), then any conflicting proc must immediately
			 * adjoin this one in the list, so we only need to look at
			 * the newest result item.  If we have an unordered list,
			 * we have to scan the whole result list.
			 */
			if (resultList)
			{
				FuncCandidateList	prevResult;

				if (catlist->ordered)
				{
					if (memcmp(procform->proargtypes, resultList->args,
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
						if (memcmp(procform->proargtypes, prevResult->args,
								   nargs * sizeof(Oid)) == 0)
							break;
					}
				}
				if (prevResult)
				{
					/* We have a match with a previous result */
					Assert(pathpos != prevResult->pathpos);
					if (pathpos > prevResult->pathpos)
						continue; /* keep previous result */
					/* replace previous result */
					prevResult->pathpos = pathpos;
					prevResult->oid = proctup->t_data->t_oid;
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
		newResult->oid = proctup->t_data->t_oid;
		memcpy(newResult->args, procform->proargtypes, nargs * sizeof(Oid));

		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
}

/*
 * OpernameGetCandidates
 *		Given a possibly-qualified operator name and operator kind,
 *		retrieve a list of the possible matches.
 *
 * We search a single namespace if the operator name is qualified, else
 * all namespaces in the search path.  The return list will never contain
 * multiple entries with identical argument types --- in the multiple-
 * namespace case, we arrange for entries in earlier namespaces to mask
 * identical entries in later namespaces.
 *
 * The returned items always have two args[] entries --- one or the other
 * will be InvalidOid for a prefix or postfix oprkind.
 */
FuncCandidateList
OpernameGetCandidates(List *names, char oprkind)
{
	FuncCandidateList resultList = NULL;
	char	   *catalogname;
	char	   *schemaname = NULL;
	char	   *opername = NULL;
	Oid			namespaceId;
	CatCList   *catlist;
	int			i;

	/* deconstruct the name list */
	switch (length(names))
	{
		case 1:
			opername = strVal(lfirst(names));
			break;
		case 2:
			schemaname = strVal(lfirst(names));
			opername = strVal(lsecond(names));
			break;
		case 3:
			catalogname = strVal(lfirst(names));
			schemaname = strVal(lsecond(names));
			opername = strVal(lfirst(lnext(lnext(names))));
			/*
			 * We check the catalog name and then ignore it.
			 */
			if (strcmp(catalogname, DatabaseName) != 0)
				elog(ERROR, "Cross-database references are not implemented");
			break;
		default:
			elog(ERROR, "Improper qualified name (too many dotted names)");
			break;
	}

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = GetSysCacheOid(NAMESPACENAME,
									 CStringGetDatum(schemaname),
									 0, 0, 0);
		if (!OidIsValid(namespaceId))
			elog(ERROR, "Namespace \"%s\" does not exist",
				 schemaname);
	}
	else
	{
		/* flag to indicate we need namespace search */
		namespaceId = InvalidOid;
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

		/* Ignore operators of wrong kind */
		if (operform->oprkind != oprkind)
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
			if (pathContainsSystemNamespace ||
				!IsSystemNamespace(operform->oprnamespace))
			{
				List	   *nsp;

				foreach(nsp, namespaceSearchPath)
				{
					pathpos++;
					if (operform->oprnamespace == (Oid) lfirsti(nsp))
						break;
				}
				if (nsp == NIL)
					continue;	/* oper is not in search path */
			}

			/*
			 * Okay, it's in the search path, but does it have the same
			 * arguments as something we already accepted?  If so, keep
			 * only the one that appears earlier in the search path.
			 *
			 * If we have an ordered list from SearchSysCacheList (the
			 * normal case), then any conflicting oper must immediately
			 * adjoin this one in the list, so we only need to look at
			 * the newest result item.  If we have an unordered list,
			 * we have to scan the whole result list.
			 */
			if (resultList)
			{
				FuncCandidateList	prevResult;

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
						continue; /* keep previous result */
					/* replace previous result */
					prevResult->pathpos = pathpos;
					prevResult->oid = opertup->t_data->t_oid;
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
		newResult->oid = opertup->t_data->t_oid;
		newResult->args[0] = operform->oprleft;
		newResult->args[1] = operform->oprright;
		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
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
	char	   *catalogname;
	char	   *schemaname = NULL;
	char	   *objname = NULL;
	Oid			namespaceId;

	/* deconstruct the name list */
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
			objname = strVal(lfirst(lnext(lnext(names))));
			/*
			 * We check the catalog name and then ignore it.
			 */
			if (strcmp(catalogname, DatabaseName) != 0)
				elog(ERROR, "Cross-database references are not implemented");
			break;
		default:
			elog(ERROR, "Improper qualified name (too many dotted names)");
			break;
	}

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = GetSysCacheOid(NAMESPACENAME,
									 CStringGetDatum(schemaname),
									 0, 0, 0);
		if (!OidIsValid(namespaceId))
			elog(ERROR, "Namespace \"%s\" does not exist",
				 schemaname);
	}
	else
	{
		/* use the default creation namespace */
		namespaceId = defaultCreationNamespace;
		if (!OidIsValid(namespaceId))
			elog(ERROR, "No namespace has been selected to create in");
	}

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
			rel->relname = strVal(lfirst(lnext(lnext(names))));
			break;
		default:
			elog(ERROR, "Improper relation name (too many dotted names)");
			break;
	}

	return rel;
}

/*
 * NameListToString
 *		Utility routine to convert a qualified-name list into a string.
 *		Used primarily to form error messages.
 */
char *
NameListToString(List *names)
{
	StringInfoData string;
	List		*l;

	initStringInfo(&string);

	foreach(l, names)
	{
		if (l != names)
			appendStringInfoChar(&string, '.');
		appendStringInfo(&string, "%s", strVal(lfirst(l)));
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
 * GetTempTableNamespace
 *		Initialize temp table namespace on first use in a particular backend
 */
static Oid
GetTempTableNamespace(void)
{
	char		namespaceName[NAMEDATALEN];
	Oid			namespaceId;

	snprintf(namespaceName, NAMEDATALEN, "pg_temp_%d", MyBackendId);

	namespaceId = GetSysCacheOid(NAMESPACENAME,
								 CStringGetDatum(namespaceName),
								 0, 0, 0);
	if (!OidIsValid(namespaceId))
	{
		/*
		 * First use of this temp namespace in this database; create it.
		 * The temp namespaces are always owned by the superuser.
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
	 * Register exit callback to clean out temp tables at backend shutdown.
	 */
	on_shmem_exit(RemoveTempRelationsCallback, 0);

	return namespaceId;
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
	List	   *tempRelList;
	List	   *constraintList;
	List	   *lptr;

	/* Get a list of relations to delete */
	tempRelList = FindTempRelations(tempNamespaceId);

	if (tempRelList == NIL)
		return;					/* nothing to do */

	/* If more than one, sort them to respect any deletion-order constraints */
	if (length(tempRelList) > 1)
	{
		constraintList = FindDeletionConstraints(tempRelList);
		if (constraintList != NIL)
			tempRelList = TopoSortRels(tempRelList, constraintList);
	}

	/* Scan the list and delete all entries */
	foreach(lptr, tempRelList)
	{
		Oid			reloid = (Oid) lfirsti(lptr);

		heap_drop_with_catalog(reloid, true);
		/*
		 * Advance cmd counter to make catalog changes visible, in case
		 * a later entry depends on this one.
		 */
		CommandCounterIncrement();
	}
}

/*
 * Find all relations in the specified temp namespace.
 *
 * Returns a list of relation OIDs.
 */
static List *
FindTempRelations(Oid tempNamespaceId)
{
	List	   *tempRelList = NIL;
	Relation	pgclass;
	HeapScanDesc scan;
	HeapTuple	tuple;
	ScanKeyData key;

	/*
	 * Scan pg_class to find all the relations in the target namespace.
	 * Ignore indexes, though, on the assumption that they'll go away
	 * when their tables are deleted.
	 */
	ScanKeyEntryInitialize(&key, 0x0,
						   Anum_pg_class_relnamespace,
						   F_OIDEQ,
						   ObjectIdGetDatum(tempNamespaceId));

	pgclass = heap_openr(RelationRelationName, AccessShareLock);
	scan = heap_beginscan(pgclass, false, SnapshotNow, 1, &key);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		switch (((Form_pg_class) GETSTRUCT(tuple))->relkind)
		{
			case RELKIND_RELATION:
			case RELKIND_SEQUENCE:
			case RELKIND_VIEW:
				tempRelList = lconsi(tuple->t_data->t_oid, tempRelList);
				break;
			default:
				break;
		}
	}

	heap_endscan(scan);
	heap_close(pgclass, AccessShareLock);

	return tempRelList;
}

/*
 * Find deletion-order constraints involving the given relation OIDs.
 *
 * Returns a list of DelConstraint objects.
 */
static List *
FindDeletionConstraints(List *relOids)
{
	List	   *constraintList = NIL;
	Relation	inheritsrel;
	HeapScanDesc scan;
	HeapTuple	tuple;

	/*
	 * Scan pg_inherits to find parents and children that are in the list.
	 */
	inheritsrel = heap_openr(InheritsRelationName, AccessShareLock);
	scan = heap_beginscan(inheritsrel, 0, SnapshotNow, 0, NULL);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		Oid		inhrelid = ((Form_pg_inherits) GETSTRUCT(tuple))->inhrelid;
		Oid		inhparent = ((Form_pg_inherits) GETSTRUCT(tuple))->inhparent;

		if (intMember(inhrelid, relOids) && intMember(inhparent, relOids))
		{
			DelConstraint  *item;

			item = (DelConstraint *) palloc(sizeof(DelConstraint));
			item->referencer = inhrelid;
			item->referencee = inhparent;
			constraintList = lcons(item, constraintList);
		}
	}

	heap_endscan(scan);
	heap_close(inheritsrel, AccessShareLock);

	return constraintList;
}

/*
 * TopoSortRels -- topological sort of a list of rels to delete
 *
 * This is a lot simpler and slower than, for example, the topological sort
 * algorithm shown in Knuth's Volume 1.  However, we are not likely to be
 * working with more than a few constraints, so the apparent slowness of the
 * algorithm won't really matter.
 */
static List *
TopoSortRels(List *relOids, List *constraintList)
{
	int			queue_size = length(relOids);
	Oid		   *rels;
	int		   *beforeConstraints;
	DelConstraint **afterConstraints;
	List	   *resultList = NIL;
	List	   *lptr;
	int			i,
				j,
				k,
				last;

	/* Allocate workspace */
	rels = (Oid *) palloc(queue_size * sizeof(Oid));
	beforeConstraints = (int *) palloc(queue_size * sizeof(int));
	afterConstraints = (DelConstraint **)
		palloc(queue_size * sizeof(DelConstraint*));

	/* Build an array of the target relation OIDs */
	i = 0;
	foreach(lptr, relOids)
	{
		rels[i++] = (Oid) lfirsti(lptr);
	}

	/*
	 * Scan the constraints, and for each rel in the array, generate a
	 * count of the number of constraints that say it must be before
	 * something else, plus a list of the constraints that say it must be
	 * after something else. The count for the j'th rel is stored in
	 * beforeConstraints[j], and the head of its list in
	 * afterConstraints[j].  Each constraint stores its list link in
	 * its link field (note any constraint will be in just one list).
	 * The array index for the before-rel of each constraint is
	 * remembered in the constraint's pred field.
	 */
	MemSet(beforeConstraints, 0, queue_size * sizeof(int));
	MemSet(afterConstraints, 0, queue_size * sizeof(DelConstraint*));
	foreach(lptr, constraintList)
	{
		DelConstraint  *constraint = (DelConstraint *) lfirst(lptr);
		Oid			rel;

		/* Find the referencer rel in the array */
		rel = constraint->referencer;
		for (j = queue_size; --j >= 0;)
		{
			if (rels[j] == rel)
				break;
		}
		Assert(j >= 0);			/* should have found a match */
		/* Find the referencee rel in the array */
		rel = constraint->referencee;
		for (k = queue_size; --k >= 0;)
		{
			if (rels[k] == rel)
				break;
		}
		Assert(k >= 0);			/* should have found a match */
		beforeConstraints[j]++; /* referencer must come before */
		/* add this constraint to list of after-constraints for referencee */
		constraint->pred = j;
		constraint->link = afterConstraints[k];
		afterConstraints[k] = constraint;
	}
	/*--------------------
	 * Now scan the rels array backwards.	At each step, output the
	 * last rel that has no remaining before-constraints, and decrease
	 * the beforeConstraints count of each of the rels it was constrained
	 * against.  (This is the right order since we are building the result
	 * list back-to-front.)
	 * i = counter for number of rels left to output
	 * j = search index for rels[]
	 * dc = temp for scanning constraint list for rel j
	 * last = last valid index in rels (avoid redundant searches)
	 *--------------------
	 */
	last = queue_size - 1;
	for (i = queue_size; --i >= 0;)
	{
		DelConstraint  *dc;

		/* Find next candidate to output */
		while (rels[last] == InvalidOid)
			last--;
		for (j = last; j >= 0; j--)
		{
			if (rels[j] != InvalidOid && beforeConstraints[j] == 0)
				break;
		}
		/* If no available candidate, topological sort fails */
		if (j < 0)
			elog(ERROR, "TopoSortRels: failed to find a workable deletion ordering");
		/* Output candidate, and mark it done by zeroing rels[] entry */
		resultList = lconsi(rels[j], resultList);
		rels[j] = InvalidOid;
		/* Update beforeConstraints counts of its predecessors */
		for (dc = afterConstraints[j]; dc; dc = dc->link)
			beforeConstraints[dc->pred]--;
	}

	/* Done */
	return resultList;
}

/*
 * Callback to remove temp relations at backend exit.
 */
static void
RemoveTempRelationsCallback(void)
{
	if (OidIsValid(myTempNamespace)) /* should always be true */
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

/* parse_hook: is proposed value valid? */
bool
check_search_path(const char *proposed)
{
	char	   *rawname;
	List	   *namelist;
	List	   *l;

	/* Need a modifiable copy of string */
	rawname = pstrdup(proposed);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		pfree(rawname);
		freeList(namelist);
		return false;
	}

	/*
	 * If we aren't inside a transaction, we cannot do database access so
	 * cannot verify the individual names.  Must accept the list on faith.
	 * (This case can happen, for example, when the postmaster reads a
	 * search_path setting from postgresql.conf.)
	 */
	if (!IsTransactionState())
	{
		pfree(rawname);
		freeList(namelist);
		return true;
	}

	/*
	 * Verify that all the names are either valid namespace names or "$user".
	 * (We do not require $user to correspond to a valid namespace; should we?)
	 */
	foreach(l, namelist)
	{
		char   *curname = (char *) lfirst(l);

		if (strcmp(curname, "$user") == 0)
			continue;
		if (!SearchSysCacheExists(NAMESPACENAME,
								  CStringGetDatum(curname),
								  0, 0, 0))
		{
			pfree(rawname);
			freeList(namelist);
			return false;
		}
	}

	pfree(rawname);
	freeList(namelist);

	return true;
}

/* assign_hook: do extra actions needed when assigning to search_path */
void
assign_search_path(const char *newval)
{
	char	   *rawname;
	List	   *namelist;
	List	   *oidlist;
	List	   *newpath;
	List	   *l;
	MemoryContext oldcxt;

	/*
	 * If we aren't inside a transaction, we cannot do database access so
	 * cannot look up the names.  In this case, do nothing; the internal
	 * search path will be fixed later by InitializeSearchPath.  (We assume
	 * this situation can only happen in the postmaster or early in backend
	 * startup.)
	 */
	if (!IsTransactionState())
		return;

	/* Need a modifiable copy of string */
	rawname = pstrdup(newval);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		/* this should not happen if GUC checked check_search_path */
		elog(ERROR, "assign_search_path: invalid list syntax");
	}

	/*
	 * Convert the list of names to a list of OIDs.  If any names are not
	 * recognizable, just leave them out of the list.  (This is our only
	 * reasonable recourse when the already-accepted default is bogus.)
	 */
	oidlist = NIL;
	foreach(l, namelist)
	{
		char   *curname = (char *) lfirst(l);
		Oid		namespaceId;

		if (strcmp(curname, "$user") == 0)
		{
			/* $user --- substitute namespace matching user name, if any */
			HeapTuple	tuple;

			tuple = SearchSysCache(SHADOWSYSID,
								   ObjectIdGetDatum(GetSessionUserId()),
								   0, 0, 0);
			if (HeapTupleIsValid(tuple))
			{
				char   *uname;

				uname = NameStr(((Form_pg_shadow) GETSTRUCT(tuple))->usename);
				namespaceId = GetSysCacheOid(NAMESPACENAME,
											 CStringGetDatum(uname),
											 0, 0, 0);
				if (OidIsValid(namespaceId))
					oidlist = lappendi(oidlist, namespaceId);
				ReleaseSysCache(tuple);
			}
		}
		else
		{
			/* normal namespace reference */
			namespaceId = GetSysCacheOid(NAMESPACENAME,
										 CStringGetDatum(curname),
										 0, 0, 0);
			if (OidIsValid(namespaceId))
				oidlist = lappendi(oidlist, namespaceId);
		}
	}

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
	pathContainsSystemNamespace = intMember(PG_CATALOG_NAMESPACE,
											namespaceSearchPath);

	if (namespaceSearchPath == NIL)
		defaultCreationNamespace = InvalidOid;
	else
		defaultCreationNamespace = (Oid) lfirsti(namespaceSearchPath);

	/* Clean up. */
	pfree(rawname);
	freeList(namelist);
	freeList(oidlist);
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
		 * tables are created in the proper namespace; ignore the GUC setting.
		 */
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		namespaceSearchPath = makeListi1(PG_CATALOG_NAMESPACE);
		MemoryContextSwitchTo(oldcxt);
		pathContainsSystemNamespace = true;
		defaultCreationNamespace = PG_CATALOG_NAMESPACE;
	}
	else
	{
		/*
		 * If a search path setting was provided before we were able to
		 * execute lookups, establish the internal search path now.
		 */
		if (namespace_search_path && *namespace_search_path &&
			namespaceSearchPath == NIL)
			assign_search_path(namespace_search_path);
	}
}
