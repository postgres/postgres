/*-------------------------------------------------------------------------
 *
 * aggregatecmds.c
 *
 *	  Routines for aggregate-manipulation commands
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/aggregatecmds.c,v 1.15 2003/09/25 06:57:58 petere Exp $
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
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(aggNamespace));

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
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("aggregate attribute \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * make sure we have our required definitions
	 */
	if (baseType == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate basetype must be specified")));
	if (transType == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate stype must be specified")));
	if (transfuncName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate sfunc must be specified")));

	/*
	 * look up the aggregate's base type (input datatype) and transtype.
	 *
	 * We have historically allowed the command to look like basetype = 'ANY'
	 * so we must do a case-insensitive comparison for the name ANY.  Ugh.
	 *
	 * basetype can be a pseudo-type, but transtype can't, since we need to
	 * be able to store values of the transtype.  However, we can allow
	 * polymorphic transtype in some cases (AggregateCreate will check).
	 */
	if (strcasecmp(TypeNameToString(baseType), "ANY") == 0)
		baseTypeId = ANYOID;
	else
		baseTypeId = typenameTypeId(baseType);

	transTypeId = typenameTypeId(transType);
	if (get_typtype(transTypeId) == 'p' &&
		transTypeId != ANYARRAYOID &&
		transTypeId != ANYELEMENTOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate transition data type cannot be %s",
						format_type_be(transTypeId))));

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

	procOid = find_aggregate_func(aggName, basetypeID, false);

	/*
	 * Find the function tuple, do permissions and validity checks
	 */
	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(procOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", procOid);

	/* Permission check: must own agg or its namespace */
	if (!pg_proc_ownercheck(procOid, GetUserId()) &&
		!pg_namespace_ownercheck(((Form_pg_proc) GETSTRUCT(tup))->pronamespace,
								 GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
					   NameListToString(aggName));

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
	HeapTuple	tup;
	Form_pg_proc procForm;
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

	procOid = find_aggregate_func(name, basetypeOid, false);

	tup = SearchSysCacheCopy(PROCOID,
							 ObjectIdGetDatum(procOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", procOid);
	procForm = (Form_pg_proc) GETSTRUCT(tup);

	namespaceOid = procForm->pronamespace;

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists(PROCNAMENSP,
							 CStringGetDatum(newname),
							 Int16GetDatum(procForm->pronargs),
							 PointerGetDatum(procForm->proargtypes),
							 ObjectIdGetDatum(namespaceOid)))
	{
		if (basetypeOid == ANYOID)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("function %s(*) already exists in schema \"%s\"",
						newname,
						get_namespace_name(namespaceOid))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_FUNCTION),
					 errmsg("function %s already exists in schema \"%s\"",
							funcname_signature_string(newname,
													  procForm->pronargs,
												  procForm->proargtypes),
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
	namestrcpy(&(((Form_pg_proc) GETSTRUCT(tup))->proname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}
