/*-------------------------------------------------------------------------
 *
 * remove.c
 *	  POSTGRES remove (domain | function | type | operator ) utilty code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/remove.c,v 1.74 2002/04/11 19:59:58 tgl Exp $
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
#include "parser/parse.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/*
 * RemoveOperator
 *		Deletes an operator.
 *
 * Exceptions:
 *		BadArg if name is invalid.
 *		BadArg if type1 is invalid.
 *		"ERROR" if operator nonexistent.
 *		...
 */
void
RemoveOperator(char *operatorName,		/* operator name */
			   TypeName *typeName1, /* left argument type name */
			   TypeName *typeName2) /* right argument type name */
{
	Relation	relation;
	HeapTuple	tup;
	Oid			typeId1 = InvalidOid;
	Oid			typeId2 = InvalidOid;
	char		oprtype;

	if (typeName1)
		typeId1 = typenameTypeId(typeName1);

	if (typeName2)
		typeId2 = typenameTypeId(typeName2);

	if (OidIsValid(typeId1) && OidIsValid(typeId2))
		oprtype = 'b';
	else if (OidIsValid(typeId1))
		oprtype = 'r';
	else
		oprtype = 'l';

	relation = heap_openr(OperatorRelationName, RowExclusiveLock);

	tup = SearchSysCacheCopy(OPERNAME,
							 PointerGetDatum(operatorName),
							 ObjectIdGetDatum(typeId1),
							 ObjectIdGetDatum(typeId2),
							 CharGetDatum(oprtype));

	if (HeapTupleIsValid(tup))
	{
		if (!pg_oper_ownercheck(tup->t_data->t_oid, GetUserId()))
			elog(ERROR, "RemoveOperator: operator '%s': permission denied",
				 operatorName);

		/* Delete any comments associated with this operator */
		DeleteComments(tup->t_data->t_oid, RelationGetRelid(relation));

		simple_heap_delete(relation, &tup->t_self);
	}
	else
	{
		if (OidIsValid(typeId1) && OidIsValid(typeId2))
		{
			elog(ERROR, "RemoveOperator: binary operator '%s' taking '%s' and '%s' does not exist",
				 operatorName,
				 TypeNameToString(typeName1),
				 TypeNameToString(typeName2));
		}
		else if (OidIsValid(typeId1))
		{
			elog(ERROR, "RemoveOperator: right unary operator '%s' taking '%s' does not exist",
				 operatorName,
				 TypeNameToString(typeName1));
		}
		else
		{
			elog(ERROR, "RemoveOperator: left unary operator '%s' taking '%s' does not exist",
				 operatorName,
				 TypeNameToString(typeName2));
		}
	}
	heap_freetuple(tup);
	heap_close(relation, RowExclusiveLock);
}

#ifdef NOTYET
/*
 * this stuff is to support removing all reference to a type
 * don't use it  - pma 2/1/94
 */
/*
 *	SingleOpOperatorRemove
 *		Removes all operators that have operands or a result of type 'typeOid'.
 */
static void
SingleOpOperatorRemove(Oid typeOid)
{
	Relation	rel;
	ScanKeyData key[3];
	HeapScanDesc scan;
	HeapTuple	tup;
	static		attnums[3] = {7, 8, 9}; /* left, right, return */
	int			i;

	ScanKeyEntryInitialize(&key[0],
						   0, 0, F_OIDEQ, (Datum) typeOid);
	rel = heap_openr(OperatorRelationName, RowExclusiveLock);
	for (i = 0; i < 3; ++i)
	{
		key[0].sk_attno = attnums[i];
		scan = heap_beginscan(rel, 0, SnapshotNow, 1, key);
		while (HeapTupleIsValid(tup = heap_getnext(scan, 0)))
		{
			/* Delete any comments associated with this operator */
			DeleteComments(tup->t_data->t_oid, RelationGetRelid(rel));

			simple_heap_delete(rel, &tup->t_self);
		}

		heap_endscan(scan);
	}
	heap_close(rel, RowExclusiveLock);
}

/*
 *	AttributeAndRelationRemove
 *		Removes all entries in the attribute and relation relations
 *		that contain entries of type 'typeOid'.
 *		Currently nothing calls this code, it is untested.
 */
static void
AttributeAndRelationRemove(Oid typeOid)
{
	struct oidlist
	{
		Oid			reloid;
		struct oidlist *next;
	};
	struct oidlist *oidptr,
			   *optr;
	Relation	rel;
	ScanKeyData key[1];
	HeapScanDesc scan;
	HeapTuple	tup;

	/*
	 * Get the oid's of the relations to be removed by scanning the entire
	 * attribute relation. We don't need to remove the attributes here,
	 * because amdestroy will remove all attributes of the relation. XXX
	 * should check for duplicate relations
	 */

	ScanKeyEntryInitialize(&key[0],
						   0, 3, F_OIDEQ, (Datum) typeOid);

	oidptr = (struct oidlist *) palloc(sizeof(*oidptr));
	oidptr->next = NULL;
	optr = oidptr;
	rel = heap_openr(AttributeRelationName, AccessShareLock);
	scan = heap_beginscan(rel, 0, SnapshotNow, 1, key);
	while (HeapTupleIsValid(tup = heap_getnext(scan, 0)))
	{
		optr->reloid = ((Form_pg_attribute) GETSTRUCT(tup))->attrelid;
		optr->next = (struct oidlist *) palloc(sizeof(*oidptr));
		optr = optr->next;
	}
	optr->next = NULL;
	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	optr = oidptr;

	ScanKeyEntryInitialize(&key[0], 0,
						   ObjectIdAttributeNumber,
						   F_OIDEQ, (Datum) 0);
	/* get RowExclusiveLock because heap_destroy will need it */
	rel = heap_openr(RelationRelationName, RowExclusiveLock);
	while (PointerIsValid((char *) optr->next))
	{
		Oid		relOid = (optr++)->reloid;

		key[0].sk_argument = ObjectIdGetDatum(relOid);
		scan = heap_beginscan(rel, 0, SnapshotNow, 1, key);
		tup = heap_getnext(scan, 0);
		if (HeapTupleIsValid(tup))
			heap_drop_with_catalog(relOid, allowSystemTableMods);
		heap_endscan(scan);
	}
	heap_close(rel, RowExclusiveLock);
}
#endif   /* NOTYET */

/*
 *	TypeRemove
 *		Removes a datatype.
 *
 * NOTE: since this tries to remove the associated array type too, it'll
 * only work on scalar types.
 */
void
RemoveType(List *names)
{
	TypeName   *typename;
	Relation	relation;
	Oid			typeoid;
	HeapTuple	tup;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeNode(TypeName);
	typename->names = names;
	typename->typmod = -1;
	typename->arrayBounds = NIL;

	relation = heap_openr(TypeRelationName, RowExclusiveLock);

	/* Use LookupTypeName here so that shell types can be removed. */
	typeoid = LookupTypeName(typename);
	if (!OidIsValid(typeoid))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(typeoid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	if (!pg_type_ownercheck(typeoid, GetUserId()))
		elog(ERROR, "RemoveType: type '%s': permission denied",
			 TypeNameToString(typename));

	/* Delete any comments associated with this type */
	DeleteComments(typeoid, RelationGetRelid(relation));

	/* Remove the type tuple from pg_type */
	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	/* Now, delete the "array of" that type */
	typename->arrayBounds = makeList1(makeInteger(1));

	typeoid = LookupTypeName(typename);
	if (!OidIsValid(typeoid))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(typeoid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	DeleteComments(typeoid, RelationGetRelid(relation));

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);
}

/*
 *	RemoveDomain
 *		Removes a domain.
 */
void
RemoveDomain(List *names, int behavior)
{
	TypeName   *typename;
	Relation	relation;
	Oid			typeoid;
	HeapTuple	tup;
	char		typtype;

	/* CASCADE unsupported */
	if (behavior == CASCADE)
		elog(ERROR, "DROP DOMAIN does not support the CASCADE keyword");

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeNode(TypeName);
	typename->names = names;
	typename->typmod = -1;
	typename->arrayBounds = NIL;

	relation = heap_openr(TypeRelationName, RowExclusiveLock);

	typeoid = typenameTypeId(typename);

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(typeoid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "RemoveDomain: type '%s' does not exist",
			 TypeNameToString(typename));

	if (!pg_type_ownercheck(typeoid, GetUserId()))
		elog(ERROR, "RemoveDomain: type '%s': permission denied",
			 TypeNameToString(typename));

	/* Check that this is actually a domain */
	typtype = ((Form_pg_type) GETSTRUCT(tup))->typtype;

	if (typtype != 'd')
		elog(ERROR, "%s is not a domain",
			 TypeNameToString(typename));

	/* Delete any comments associated with this type */
	DeleteComments(typeoid, RelationGetRelid(relation));

	/* Remove the type tuple from pg_type */
	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	/* At present, domains don't have associated array types */

	heap_close(relation, RowExclusiveLock);
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

	if (!pg_proc_ownercheck(funcOid, GetUserId()))
		elog(ERROR, "RemoveFunction: function '%s': permission denied",
			 NameListToString(functionName));

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
