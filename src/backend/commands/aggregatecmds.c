/*-------------------------------------------------------------------------
 *
 * aggregatecmds.c
 *
 *	  Routines for aggregate-manipulation commands
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/aggregatecmds.c,v 1.8 2003/06/27 14:45:27 petere Exp $
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
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
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
 */
void
DefineAggregate(List *names, List *parameters)
{
	char	   *aggName;
	Oid			aggNamespace;
	AclResult	aclresult;
	List	   *transfuncName = NIL;
	List	   *finalfuncName = NIL;
	TypeName   *baseType = NULL;
	TypeName   *transType = NULL;
	char	   *initval = NULL;
	Oid			baseTypeId;
	Oid			transTypeId;
	List	   *pl;

	/* Convert list of names to a name and namespace */
	aggNamespace = QualifiedNameGetCreationNamespace(names, &aggName);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(aggNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_namespace_name(aggNamespace));

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		/*
		 * sfunc1, stype1, and initcond1 are accepted as obsolete
		 * spellings for sfunc, stype, initcond.
		 */
		if (strcasecmp(defel->defname, "sfunc") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (strcasecmp(defel->defname, "sfunc1") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (strcasecmp(defel->defname, "finalfunc") == 0)
			finalfuncName = defGetQualifiedName(defel);
		else if (strcasecmp(defel->defname, "basetype") == 0)
			baseType = defGetTypeName(defel);
		else if (strcasecmp(defel->defname, "stype") == 0)
			transType = defGetTypeName(defel);
		else if (strcasecmp(defel->defname, "stype1") == 0)
			transType = defGetTypeName(defel);
		else if (strcasecmp(defel->defname, "initcond") == 0)
			initval = defGetString(defel);
		else if (strcasecmp(defel->defname, "initcond1") == 0)
			initval = defGetString(defel);
		else
			elog(WARNING, "DefineAggregate: attribute \"%s\" not recognized",
				 defel->defname);
	}

	/*
	 * make sure we have our required definitions
	 */
	if (baseType == NULL)
		elog(ERROR, "Define: \"basetype\" unspecified");
	if (transType == NULL)
		elog(ERROR, "Define: \"stype\" unspecified");
	if (transfuncName == NIL)
		elog(ERROR, "Define: \"sfunc\" unspecified");

	/*
	 * look up the aggregate's base type (input datatype) and transtype.
	 *
	 * We have historically allowed the command to look like basetype = 'ANY'
	 * so we must do a case-insensitive comparison for the name ANY.  Ugh.
	 *
	 * basetype can be a pseudo-type, but transtype can't, since we need to
	 * be able to store values of the transtype.
	 */
	if (strcasecmp(TypeNameToString(baseType), "ANY") == 0)
		baseTypeId = ANYOID;
	else
		baseTypeId = typenameTypeId(baseType);

	transTypeId = typenameTypeId(transType);
	if (get_typtype(transTypeId) == 'p')
		elog(ERROR, "Aggregate transition datatype cannot be %s",
			 format_type_be(transTypeId));

	/*
	 * Most of the argument-checking is done inside of AggregateCreate
	 */
	AggregateCreate(aggName,	/* aggregate name */
					aggNamespace,		/* namespace */
					transfuncName,		/* step function name */
					finalfuncName,		/* final function name */
					baseTypeId, /* type of data being aggregated */
					transTypeId,	/* transition data type */
					initval);	/* initial condition */
}


/*
 * RemoveAggregate
 *		Deletes an aggregate.
 */
void
RemoveAggregate(RemoveAggrStmt *stmt)
{
	List	   *aggName = stmt->aggname;
	TypeName   *aggType = stmt->aggtype;
	Oid			basetypeID;
	Oid			procOid;
	HeapTuple	tup;
	ObjectAddress object;

	/*
	 * if a basetype is passed in, then attempt to find an aggregate for
	 * that specific type.
	 *
	 * else attempt to find an aggregate with a basetype of ANYOID. This
	 * means that the aggregate is to apply to all basetypes (eg, COUNT).
	 */
	if (aggType)
		basetypeID = typenameTypeId(aggType);
	else
		basetypeID = ANYOID;

	procOid = find_aggregate_func("RemoveAggregate", aggName, basetypeID);

	/*
	 * Find the function tuple, do permissions and validity checks
	 */
	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(procOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "RemoveAggregate: couldn't find pg_proc tuple for %s",
			 NameListToString(aggName));

	/* Permission check: must own agg or its namespace */
	if (!pg_proc_ownercheck(procOid, GetUserId()) &&
		!pg_namespace_ownercheck(((Form_pg_proc) GETSTRUCT(tup))->pronamespace,
								 GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, NameListToString(aggName));

	/* find_aggregate_func already checked it is an aggregate */

	ReleaseSysCache(tup);

	/*
	 * Do the deletion
	 */
	object.classId = RelOid_pg_proc;
	object.objectId = procOid;
	object.objectSubId = 0;

	performDeletion(&object, stmt->behavior);
}


void
RenameAggregate(List *name, TypeName *basetype, const char *newname)
{
	Oid			basetypeOid;
	Oid			procOid;
	Oid			namespaceOid;
	Oid			oid_array[FUNC_MAX_ARGS];
	HeapTuple	tup;
	Relation	rel;
	AclResult	aclresult;

	/*
	 * if a basetype is passed in, then attempt to find an aggregate for
	 * that specific type.
	 *
	 * else attempt to find an aggregate with a basetype of ANYOID. This
	 * means that the aggregate is to apply to all basetypes (eg, COUNT).
	 */
	if (basetype)
		basetypeOid = typenameTypeId(basetype);
	else
		basetypeOid = ANYOID;

	rel = heap_openr(ProcedureRelationName, RowExclusiveLock);

	procOid = find_aggregate_func("RenameAggregate", name, basetypeOid);

	tup = SearchSysCacheCopy(PROCOID,
							 ObjectIdGetDatum(procOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "RenameAggregate: couldn't find pg_proc tuple for %s",
			 NameListToString(name));

	namespaceOid = ((Form_pg_proc) GETSTRUCT(tup))->pronamespace;

	/* make sure the new name doesn't exist */
	MemSet(oid_array, 0, sizeof(oid_array));
	oid_array[0] = basetypeOid;
	if (SearchSysCacheExists(PROCNAMENSP,
							 CStringGetDatum(newname),
							 Int16GetDatum(1),
							 PointerGetDatum(oid_array),
							 ObjectIdGetDatum(namespaceOid)))
	{
		if (basetypeOid == ANYOID)
			elog(ERROR, "function %s(*) already exists in schema %s",
				 newname, get_namespace_name(namespaceOid));
		else
			elog(ERROR, "function %s(%s) already exists in schema %s",
				 newname, format_type_be(basetypeOid), get_namespace_name(namespaceOid));
	}

	/* must be owner */
	if (!pg_proc_ownercheck(procOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, NameListToString(name));

	/* must have CREATE privilege on namespace */
	aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_namespace_name(namespaceOid));

	/* rename */
	namestrcpy(&(((Form_pg_proc) GETSTRUCT(tup))->proname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}
