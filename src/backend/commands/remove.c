/*-------------------------------------------------------------------------
 *
 * remove.c--
 *	  POSTGRES remove (function | type | operator ) utilty code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/remove.c,v 1.18 1997/11/28 17:27:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <postgres.h>

#include <utils/acl.h>
#include <access/heapam.h>
#include <utils/builtins.h>
#include <utils/syscache.h>
#include <catalog/catname.h>
#include <commands/defrem.h>
#include <miscadmin.h>
#include <catalog/pg_aggregate.h>
#include <catalog/pg_language.h>
#include <catalog/pg_operator.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <parser/parse_func.h>
#include <storage/bufmgr.h>
#include <fmgr.h>
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

/*
 * RemoveOperator --
 *		Deletes an operator.
 *
 * Exceptions:
 *		BadArg if name is invalid.
 *		BadArg if type1 is invalid.
 *		"WARN" if operator nonexistent.
 *		...
 */
void
RemoveOperator(char *operatorName,		/* operator name */
			   char *typeName1, /* first type name */
			   char *typeName2) /* optional second type name */
{
	Relation	relation;
	HeapScanDesc scan;
	HeapTuple	tup;
	Oid			typeId1 = InvalidOid;
	Oid			typeId2 = InvalidOid;
	bool		defined;
	ItemPointerData itemPointerData;
	Buffer		buffer;
	ScanKeyData operatorKey[3];
	char	   *userName;

	if (typeName1)
	{
		typeId1 = TypeGet(typeName1, &defined);
		if (!OidIsValid(typeId1))
		{
			elog(WARN, "RemoveOperator: type '%s' does not exist", typeName1);
			return;
		}
	}

	if (typeName2)
	{
		typeId2 = TypeGet(typeName2, &defined);
		if (!OidIsValid(typeId2))
		{
			elog(WARN, "RemoveOperator: type '%s' does not exist", typeName2);
			return;
		}
	}

	ScanKeyEntryInitialize(&operatorKey[0], 0x0,
						   Anum_pg_operator_oprname,
						   NameEqualRegProcedure,
						   PointerGetDatum(operatorName));

	ScanKeyEntryInitialize(&operatorKey[1], 0x0,
						   Anum_pg_operator_oprleft,
						   ObjectIdEqualRegProcedure,
						   ObjectIdGetDatum(typeId1));

	ScanKeyEntryInitialize(&operatorKey[2], 0x0,
						   Anum_pg_operator_oprright,
						   ObjectIdEqualRegProcedure,
						   ObjectIdGetDatum(typeId2));

	relation = heap_openr(OperatorRelationName);
	scan = heap_beginscan(relation, 0, false, 3, operatorKey);
	tup = heap_getnext(scan, 0, &buffer);
	if (HeapTupleIsValid(tup))
	{
#ifndef NO_SECURITY
		userName = GetPgUserName();
		if (!pg_ownercheck(userName,
						   (char *) ObjectIdGetDatum(tup->t_oid),
						   OPROID))
			elog(WARN, "RemoveOperator: operator '%s': permission denied",
				 operatorName);
#endif
		ItemPointerCopy(&tup->t_ctid, &itemPointerData);
		heap_delete(relation, &itemPointerData);
	}
	else
	{
		if (OidIsValid(typeId1) && OidIsValid(typeId2))
		{
			elog(WARN, "RemoveOperator: binary operator '%s' taking '%s' and '%s' does not exist",
				 operatorName,
				 typeName1,
				 typeName2);
		}
		else if (OidIsValid(typeId1))
		{
			elog(WARN, "RemoveOperator: right unary operator '%s' taking '%s' does not exist",
				 operatorName,
				 typeName1);
		}
		else
		{
			elog(WARN, "RemoveOperator: left unary operator '%s' taking '%s' does not exist",
				 operatorName,
				 typeName2);
		}
	}
	heap_endscan(scan);
	heap_close(relation);
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
	Relation	rdesc;
	ScanKeyData key[3];
	HeapScanDesc sdesc;
	HeapTuple	tup;
	ItemPointerData itemPointerData;
	Buffer		buffer;
	static		attnums[3] = {7, 8, 9}; /* left, right, return */
	register	i;

	ScanKeyEntryInitialize(&key[0],
					   0, 0, ObjectIdEqualRegProcedure, (Datum) typeOid);
	rdesc = heap_openr(OperatorRelationName);
	for (i = 0; i < 3; ++i)
	{
		key[0].sk_attno = attnums[i];
		sdesc = heap_beginscan(rdesc, 0, false, 1, key);
		while (PointerIsValid(tup = heap_getnext(sdesc, 0, &buffer)))
		{
			ItemPointerCopy(&tup->t_ctid, &itemPointerData);
			/* XXX LOCK not being passed */
			heap_delete(rdesc, &itemPointerData);
		}
		heap_endscan(sdesc);
	}
	heap_close(rdesc);
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
	Relation	rdesc;
	ScanKeyData key[1];
	HeapScanDesc sdesc;
	HeapTuple	tup;
	ItemPointerData itemPointerData;
	Buffer		buffer;

	/*
	 * Get the oid's of the relations to be removed by scanning the entire
	 * attribute relation. We don't need to remove the attributes here,
	 * because amdestroy will remove all attributes of the relation. XXX
	 * should check for duplicate relations
	 */

	ScanKeyEntryInitialize(&key[0],
					   0, 3, ObjectIdEqualRegProcedure, (Datum) typeOid);

	oidptr = (struct oidlist *) palloc(sizeof(*oidptr));
	oidptr->next = NULL;
	optr = oidptr;
	rdesc = heap_openr(AttributeRelationName);
	sdesc = heap_beginscan(rdesc, 0, false, 1, key);
	while (PointerIsValid(tup = heap_getnext(sdesc, 0, &buffer)))
	{
		ItemPointerCopy(&tup->t_ctid, &itemPointerData);
		optr->reloid = ((AttributeTupleForm) GETSTRUCT(tup))->attrelid;
		optr->next = (struct oidlist *) palloc(sizeof(*oidptr));
		optr = optr->next;
	}
	optr->next = NULL;
	heap_endscan(sdesc);
	heap_close(rdesc);


	ScanKeyEntryInitialize(&key[0], 0,
						   ObjectIdAttributeNumber,
						   ObjectIdEqualRegProcedure, (Datum) 0);
	optr = oidptr;
	rdesc = heap_openr(RelationRelationName);
	while (PointerIsValid((char *) optr->next))
	{
		key[0].sk_argument = (Datum) (optr++)->reloid;
		sdesc = heap_beginscan(rdesc, 0, false, 1, key);
		tup = heap_getnext(sdesc, 0, &buffer);
		if (PointerIsValid(tup))
		{
			char	   *name;

			name = (((Form_pg_class) GETSTRUCT(tup))->relname).data;
			heap_destroy_with_catalog(name);
		}
	}
	heap_endscan(sdesc);
	heap_close(rdesc);
}

#endif							/* NOTYET */

/*
 *	TypeRemove
 *		Removes the type 'typeName' and all attributes and relations that
 *		use it.
 */
void
RemoveType(char *typeName)		/* type name to be removed */
{
	Relation	relation;
	HeapScanDesc scan;
	HeapTuple	tup;
	Oid			typeOid;
	ItemPointerData itemPointerData;
	static ScanKeyData typeKey[1] = {
		{0, Anum_pg_type_typname, NameEqualRegProcedure}
	};
	char	   *shadow_type;
	char	   *userName;

#ifndef NO_SECURITY
	userName = GetPgUserName();
	if (!pg_ownercheck(userName, typeName, TYPNAME))
		elog(WARN, "RemoveType: type '%s': permission denied",
			 typeName);
#endif

	relation = heap_openr(TypeRelationName);
	fmgr_info(typeKey[0].sk_procedure, &typeKey[0].sk_func,
			  &typeKey[0].sk_nargs);

	/* Delete the primary type */

	typeKey[0].sk_argument = PointerGetDatum(typeName);

	scan = heap_beginscan(relation, 0, false, 1, typeKey);
	tup = heap_getnext(scan, 0, (Buffer *) 0);
	if (!HeapTupleIsValid(tup))
	{
		heap_endscan(scan);
		heap_close(relation);
		elog(WARN, "RemoveType: type '%s' does not exist",
			 typeName);
	}
	typeOid = tup->t_oid;
	ItemPointerCopy(&tup->t_ctid, &itemPointerData);
	heap_delete(relation, &itemPointerData);
	heap_endscan(scan);

	/* Now, Delete the "array of" that type */
	shadow_type = makeArrayTypeName(typeName);
	typeKey[0].sk_argument = NameGetDatum(shadow_type);

	scan = heap_beginscan(relation, 0, false,
						  1, (ScanKey) typeKey);
	tup = heap_getnext(scan, 0, (Buffer *) 0);

	if (!HeapTupleIsValid(tup))
	{
		elog(WARN, "RemoveType: type '%s': array stub not found",
			 typeName);
	}
	typeOid = tup->t_oid;
	ItemPointerCopy(&tup->t_ctid, &itemPointerData);
	heap_delete(relation, &itemPointerData);
	heap_endscan(scan);

	heap_close(relation);
}

/*
 * RemoveFunction --
 *		Deletes a function.
 *
 * Exceptions:
 *		BadArg if name is invalid.
 *		"WARN" if function nonexistent.
 *		...
 */
void
RemoveFunction(char *functionName,		/* function name to be removed */
			   int nargs,
			   List *argNameList /* list of TypeNames */ )
{
	Relation	relation;
	HeapScanDesc scan;
	HeapTuple	tup;
	Buffer		buffer = InvalidBuffer;
	bool		bufferUsed = FALSE;
	Oid			argList[8];
	Form_pg_proc the_proc = NULL;
	ItemPointerData itemPointerData;
	static ScanKeyData key[3] = {
		{0, Anum_pg_proc_proname, NameEqualRegProcedure}
	};
	char	   *userName;
	char	   *typename;
	int			i;

	MemSet(argList, 0, 8 * sizeof(Oid));
	for (i = 0; i < nargs; i++)
	{
/*		typename = ((TypeName*)(lfirst(argNameList)))->name; */
		typename = strVal(lfirst(argNameList));
		argNameList = lnext(argNameList);

		if (strcmp(typename, "opaque") == 0)
			argList[i] = 0;
		else
		{
			tup = SearchSysCacheTuple(TYPNAME, PointerGetDatum(typename),
									  0, 0, 0);

			if (!HeapTupleIsValid(tup))
			{
				elog(WARN, "RemoveFunction: type '%s' not found", typename);
			}
			argList[i] = tup->t_oid;
		}
	}

	tup = SearchSysCacheTuple(PRONAME, PointerGetDatum(functionName),
							  Int32GetDatum(nargs),
							  PointerGetDatum(argList), 0);
	if (!HeapTupleIsValid(tup))
		func_error("RemoveFunction", functionName, nargs, argList);

#ifndef NO_SECURITY
	userName = GetPgUserName();
	if (!pg_func_ownercheck(userName, functionName, nargs, argList))
	{
		elog(WARN, "RemoveFunction: function '%s': permission denied",
			 functionName);
	}
#endif

	key[0].sk_argument = PointerGetDatum(functionName);

	fmgr_info(key[0].sk_procedure, &key[0].sk_func, &key[0].sk_nargs);

	relation = heap_openr(ProcedureRelationName);
	scan = heap_beginscan(relation, 0, false, 1, key);

	do
	{							/* hope this is ok because it's indexed */
		if (bufferUsed)
		{
			ReleaseBuffer(buffer);
			bufferUsed = FALSE;
		}
		tup = heap_getnext(scan, 0, (Buffer *) &buffer);
		if (!HeapTupleIsValid(tup))
			break;
		bufferUsed = TRUE;
		the_proc = (Form_pg_proc) GETSTRUCT(tup);
	} while ((namestrcmp(&(the_proc->proname), functionName) == 0) &&
			 (the_proc->pronargs != nargs ||
			  !oid8eq(&(the_proc->proargtypes[0]), &argList[0])));


	if (!HeapTupleIsValid(tup) || namestrcmp(&(the_proc->proname),
											 functionName) != 0)
	{
		heap_endscan(scan);
		heap_close(relation);
		func_error("RemoveFunction", functionName, nargs, argList);
	}

	/* ok, function has been found */

	if (the_proc->prolang == INTERNALlanguageId)
		elog(WARN, "RemoveFunction: function \"%s\" is built-in",
			 functionName);

	ItemPointerCopy(&tup->t_ctid, &itemPointerData);
	heap_delete(relation, &itemPointerData);
	heap_endscan(scan);
	heap_close(relation);
}

void
RemoveAggregate(char *aggName, char *aggType)
{
	Relation	relation;
	HeapScanDesc scan;
	HeapTuple	tup;
	ItemPointerData itemPointerData;
	char	   *userName;
	Oid			basetypeID = InvalidOid;
	bool		defined;
	ScanKeyData aggregateKey[3];


	/*
	 * if a basetype is passed in, then attempt to find an aggregate for
	 * that specific type.
	 *
	 * else if the basetype is blank, then attempt to find an aggregate with
	 * a basetype of zero.	This is valid. It means that the aggregate is
	 * to apply to all basetypes.  ie, a counter of some sort.
	 *
	 */

	if (aggType)
	{
		basetypeID = TypeGet(aggType, &defined);
		if (!OidIsValid(basetypeID))
		{
			elog(WARN, "RemoveAggregate: type '%s' does not exist", aggType);
		}
	}
	else
	{
		basetypeID = 0;
	}

/*
#ifndef NO_SECURITY
*/
	userName = GetPgUserName();
	if (!pg_aggr_ownercheck(userName, aggName, basetypeID))
	{
		if (aggType)
		{
			elog(WARN, "RemoveAggregate: aggregate '%s' on type '%s': permission denied",
				 aggName, aggType);
		}
		else
		{
			elog(WARN, "RemoveAggregate: aggregate '%s': permission denied",
				 aggName);
		}
	}
/*
#endif
*/

	ScanKeyEntryInitialize(&aggregateKey[0], 0x0,
						   Anum_pg_aggregate_aggname,
						   NameEqualRegProcedure,
						   PointerGetDatum(aggName));

	ScanKeyEntryInitialize(&aggregateKey[1], 0x0,
						   Anum_pg_aggregate_aggbasetype,
						   ObjectIdEqualRegProcedure,
						   ObjectIdGetDatum(basetypeID));

	relation = heap_openr(AggregateRelationName);
	scan = heap_beginscan(relation, 0, false, 2, aggregateKey);
	tup = heap_getnext(scan, 0, (Buffer *) 0);
	if (!HeapTupleIsValid(tup))
	{
		heap_endscan(scan);
		heap_close(relation);
		if (aggType)
		{
			elog(WARN, "RemoveAggregate: aggregate '%s' for '%s' does not exist",
				 aggName, aggType);
		}
		else
		{
			elog(WARN, "RemoveAggregate: aggregate '%s' for all types does not exist",
				 aggName);
		}
	}
	ItemPointerCopy(&tup->t_ctid, &itemPointerData);
	heap_delete(relation, &itemPointerData);
	heap_endscan(scan);
	heap_close(relation);
}
