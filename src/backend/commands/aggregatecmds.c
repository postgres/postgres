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
 *	  $Header: /cvsroot/pgsql/src/backend/commands/aggregatecmds.c,v 1.1 2002/04/15 05:22:03 tgl Exp $
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
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "commands/comment.h"
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
	 * Handle the aggregate's base type (input data type).  This can be
	 * specified as 'ANY' for a data-independent transition function, such
	 * as COUNT(*).
	 */
	baseTypeId = LookupTypeName(baseType);
	if (OidIsValid(baseTypeId))
	{
		/* no need to allow aggregates on as-yet-undefined types */
		if (!get_typisdefined(baseTypeId))
			elog(ERROR, "Type \"%s\" is only a shell",
				 TypeNameToString(baseType));
	}
	else
	{
		char      *typnam = TypeNameToString(baseType);

		if (strcasecmp(typnam, "ANY") != 0)
			elog(ERROR, "Type \"%s\" does not exist", typnam);
		baseTypeId = InvalidOid;
	}

	/* handle transtype --- no special cases here */
	transTypeId = typenameTypeId(transType);

	/*
	 * Most of the argument-checking is done inside of AggregateCreate
	 */
	AggregateCreate(aggName,	/* aggregate name */
					aggNamespace,	/* namespace */
					transfuncName,		/* step function name */
					finalfuncName,		/* final function name */
					baseTypeId,	/* type of data being aggregated */
					transTypeId,	/* transition data type */
					initval);	/* initial condition */
}


void
RemoveAggregate(List *aggName, TypeName *aggType)
{
	Relation	relation;
	HeapTuple	tup;
	Oid			basetypeID;
	Oid			procOid;

	/*
	 * if a basetype is passed in, then attempt to find an aggregate for
	 * that specific type.
	 *
	 * else if the basetype is blank, then attempt to find an aggregate with
	 * a basetype of zero.	This is valid. It means that the aggregate is
	 * to apply to all basetypes (eg, COUNT).
	 */
	if (aggType)
		basetypeID = typenameTypeId(aggType);
	else
		basetypeID = InvalidOid;

	procOid = find_aggregate_func("RemoveAggregate", aggName, basetypeID);

	/* Permission check */

	if (!pg_proc_ownercheck(procOid, GetUserId()))
	{
		if (basetypeID == InvalidOid)
			elog(ERROR, "RemoveAggregate: aggregate %s for all types: permission denied",
				 NameListToString(aggName));
		else
			elog(ERROR, "RemoveAggregate: aggregate %s for type %s: permission denied",
				 NameListToString(aggName), format_type_be(basetypeID));
	}

	/* Remove the pg_proc tuple */

	relation = heap_openr(ProcedureRelationName, RowExclusiveLock);

	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(procOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))	/* should not happen */
		elog(ERROR, "RemoveAggregate: couldn't find pg_proc tuple for %s",
			 NameListToString(aggName));

	/* Delete any comments associated with this function */
	DeleteComments(procOid, RelationGetRelid(relation));

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);

	/* Remove the pg_aggregate tuple */

	relation = heap_openr(AggregateRelationName, RowExclusiveLock);

	tup = SearchSysCache(AGGFNOID,
						 ObjectIdGetDatum(procOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))	/* should not happen */
		elog(ERROR, "RemoveAggregate: couldn't find pg_aggregate tuple for %s",
			 NameListToString(aggName));

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);
}
