/*-------------------------------------------------------------------------
 *
 * pg_type.c
 *	  routines to support manipulation of the pg_type relation
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_type.c,v 1.59 2001/02/12 20:07:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"


static Oid TypeShellMakeWithOpenRelation(Relation pg_type_desc,
							  char *typeName);

/* ----------------------------------------------------------------
 *		TypeGetWithOpenRelation
 *
 *		preforms a scan on pg_type for a type tuple with the
 *		given type name.
 * ----------------------------------------------------------------
 *		pg_type_desc			 -- reldesc for pg_type
 *		typeName				 -- name of type to be fetched
 *		defined					 -- has the type been defined?
 */
static Oid
TypeGetWithOpenRelation(Relation pg_type_desc,
						char *typeName,
						bool *defined)
{
	HeapScanDesc scan;
	HeapTuple	tup;
	Oid			typoid;
	ScanKeyData typeKey[1];

	/* ----------------
	 *	initialize the scan key and begin a scan of pg_type
	 * ----------------
	 */
	ScanKeyEntryInitialize(typeKey,
						   0,
						   Anum_pg_type_typname,
						   F_NAMEEQ,
						   PointerGetDatum(typeName));

	scan = heap_beginscan(pg_type_desc,
						  0,
						  SnapshotSelf, /* cache? */
						  1,
						  typeKey);

	/* ----------------
	 *	get the type tuple, if it exists.
	 * ----------------
	 */
	tup = heap_getnext(scan, 0);

	/* ----------------
	 *	if no type tuple exists for the given type name, then
	 *	end the scan and return appropriate information.
	 * ----------------
	 */
	if (!HeapTupleIsValid(tup))
	{
		heap_endscan(scan);
		*defined = false;
		return InvalidOid;
	}

	/* ----------------
	 *	here, the type tuple does exist so we pull information from
	 *	the typisdefined field of the tuple and return the tuple's
	 *	oid, which is the oid of the type.
	 * ----------------
	 */
	*defined = (bool) ((Form_pg_type) GETSTRUCT(tup))->typisdefined;
	typoid = tup->t_data->t_oid;

	heap_endscan(scan);

	return typoid;
}

/* ----------------------------------------------------------------
 *		TypeGet
 *
 *		Finds the ObjectId of a type, even if uncommitted; "defined"
 *		is only set if the type has actually been defined, i.e., if
 *		the type tuple is not a shell.
 *
 *		Note: the meat of this function is now in the function
 *			  TypeGetWithOpenRelation().  -cim 6/15/90
 *
 *		Also called from util/remove.c
 * ----------------------------------------------------------------
 */
Oid
TypeGet(char *typeName,			/* name of type to be fetched */
		bool *defined)			/* has the type been defined? */
{
	Relation	pg_type_desc;
	Oid			typeoid;

	/* ----------------
	 *	open the pg_type relation
	 * ----------------
	 */
	pg_type_desc = heap_openr(TypeRelationName, AccessShareLock);

	/* ----------------
	 *	scan the type relation for the information we want
	 * ----------------
	 */
	typeoid = TypeGetWithOpenRelation(pg_type_desc,
									  typeName,
									  defined);

	/* ----------------
	 *	close the type relation and return the type oid.
	 * ----------------
	 */
	heap_close(pg_type_desc, AccessShareLock);

	return typeoid;
}

/* ----------------------------------------------------------------
 *		TypeShellMakeWithOpenRelation
 *
 * ----------------------------------------------------------------
 */
static Oid
TypeShellMakeWithOpenRelation(Relation pg_type_desc, char *typeName)
{
	int			i;
	HeapTuple	tup;
	Datum		values[Natts_pg_type];
	char		nulls[Natts_pg_type];
	Oid			typoid;
	NameData	name;
	TupleDesc	tupDesc;

	/* ----------------
	 *	initialize our *nulls and *values arrays
	 * ----------------
	 */
	for (i = 0; i < Natts_pg_type; ++i)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;		/* redundant, but safe */
	}

	/* ----------------
	 *	initialize *values with the type name and dummy values
	 * ----------------
	 */
	i = 0;
	namestrcpy(&name, typeName);
	values[i++] = NameGetDatum(&name);			/* 1 */
	values[i++] = ObjectIdGetDatum(InvalidOid);	/* 2 */
	values[i++] = Int16GetDatum(0);				/* 3 */
	values[i++] = Int16GetDatum(0);				/* 4 */
	values[i++] = BoolGetDatum(false);			/* 5 */
	values[i++] = CharGetDatum(0);				/* 6 */
	values[i++] = BoolGetDatum(false);			/* 7 */
	values[i++] = CharGetDatum(0);				/* 8 */
	values[i++] = ObjectIdGetDatum(InvalidOid);	/* 9 */
	values[i++] = ObjectIdGetDatum(InvalidOid);	/* 10 */
	values[i++] = ObjectIdGetDatum(InvalidOid);	/* 11 */
	values[i++] = ObjectIdGetDatum(InvalidOid);	/* 12 */
	values[i++] = ObjectIdGetDatum(InvalidOid);	/* 13 */
	values[i++] = ObjectIdGetDatum(InvalidOid);	/* 14 */
	values[i++] = CharGetDatum('i');			/* 15 */
	values[i++] = CharGetDatum('p');			/* 16 */
	values[i++] = DirectFunctionCall1(textin,
									  CStringGetDatum(typeName));	/* 17 */

	/* ----------------
	 *	create a new type tuple with FormHeapTuple
	 * ----------------
	 */
	tupDesc = pg_type_desc->rd_att;

	tup = heap_formtuple(tupDesc, values, nulls);

	/* ----------------
	 *	insert the tuple in the relation and get the tuple's oid.
	 * ----------------
	 */
	heap_insert(pg_type_desc, tup);
	typoid = tup->t_data->t_oid;

	if (RelationGetForm(pg_type_desc)->relhasindex)
	{
		Relation	idescs[Num_pg_type_indices];

		CatalogOpenIndices(Num_pg_type_indices, Name_pg_type_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_type_indices, pg_type_desc, tup);
		CatalogCloseIndices(Num_pg_type_indices, idescs);
	}
	/* ----------------
	 *	free the tuple and return the type-oid
	 * ----------------
	 */
	heap_freetuple(tup);

	return typoid;
}

/* ----------------------------------------------------------------
 *		TypeShellMake
 *
 *		This procedure inserts a "shell" tuple into the type
 *		relation.  The type tuple inserted has invalid values
 *		and in particular, the "typisdefined" field is false.
 *
 *		This is used so that a tuple exists in the catalogs.
 *		The invalid fields should be fixed up sometime after
 *		this routine is called, and then the "typeisdefined"
 *		field is set to true. -cim 6/15/90
 * ----------------------------------------------------------------
 */
Oid
TypeShellMake(char *typeName)
{
	Relation	pg_type_desc;
	Oid			typoid;

	Assert(PointerIsValid(typeName));

	/* ----------------
	 *	open pg_type
	 * ----------------
	 */
	pg_type_desc = heap_openr(TypeRelationName, RowExclusiveLock);

	/* ----------------
	 *	insert the shell tuple
	 * ----------------
	 */
	typoid = TypeShellMakeWithOpenRelation(pg_type_desc, typeName);

	/* ----------------
	 *	close pg_type and return the tuple's oid.
	 * ----------------
	 */
	heap_close(pg_type_desc, RowExclusiveLock);

	return typoid;
}

/* ----------------------------------------------------------------
 *		TypeCreate
 *
 *		This does all the necessary work needed to define a new type.
 *
 * NOTE: if assignedTypeOid is not InvalidOid, then that OID is assigned to
 * the new type (which, therefore, cannot already exist as a shell type).
 * This hack is only intended for use in creating a relation's associated
 * type, where we need to have created the relation tuple already.
 * ----------------------------------------------------------------
 */
Oid
TypeCreate(char *typeName,
		   Oid assignedTypeOid,
		   Oid relationOid,		/* only for 'c'atalog typeTypes */
		   int16 internalSize,
		   int16 externalSize,
		   char typeType,
		   char typDelim,
		   char *inputProcedure,
		   char *outputProcedure,
		   char *receiveProcedure,
		   char *sendProcedure,
		   char *elementTypeName,
		   char *defaultTypeValue,		/* internal rep */
		   bool passedByValue,
		   char alignment,
		   char storage)
{
	int			i,
				j;
	Relation	pg_type_desc;
	HeapScanDesc pg_type_scan;
	Oid			typeObjectId;
	Oid			elementObjectId = InvalidOid;
	HeapTuple	tup;
	char		nulls[Natts_pg_type];
	char		replaces[Natts_pg_type];
	Datum		values[Natts_pg_type];
	char	   *procname;
	char	   *procs[4];
	bool		defined;
	NameData	name;
	TupleDesc	tupDesc;
	Oid			argList[FUNC_MAX_ARGS];
	ScanKeyData typeKey[1];

	/* ----------------
	 *	check that the type is not already defined.  It might exist as
	 *	a shell type, however (but only if assignedTypeOid is not given).
	 * ----------------
	 */
	typeObjectId = TypeGet(typeName, &defined);
	if (OidIsValid(typeObjectId) &&
		(defined || assignedTypeOid != InvalidOid))
		elog(ERROR, "TypeCreate: type %s already defined", typeName);

	/* ----------------
	 *	if this type has an associated elementType, then we check that
	 *	it is defined.
	 * ----------------
	 */
	if (elementTypeName)
	{
		elementObjectId = TypeGet(elementTypeName, &defined);
		if (!defined)
			elog(ERROR, "TypeCreate: type %s is not defined", elementTypeName);
	}

	/* ----------------
	 *	XXX comment me
	 * ----------------
	 */
	if (externalSize == 0)
		externalSize = -1;		/* variable length */

	/* ----------------
	 *	initialize arrays needed by FormHeapTuple
	 * ----------------
	 */
	for (i = 0; i < Natts_pg_type; ++i)
	{
		nulls[i] = ' ';
		replaces[i] = 'r';
		values[i] = (Datum) NULL;		/* redundant, but nice */
	}

	/*
	 * XXX
	 *
	 * Do this so that user-defined types have size -1 instead of zero if
	 * they are variable-length - this is so that everything else in the
	 * backend works.
	 */

	if (internalSize == 0)
		internalSize = -1;

	/* ----------------
	 *	initialize the *values information
	 * ----------------
	 */
	i = 0;
	namestrcpy(&name, typeName);
	values[i++] = NameGetDatum(&name);			/* 1 */
	values[i++] = Int32GetDatum(GetUserId());	/* 2 */
	values[i++] = Int16GetDatum(internalSize);	/* 3 */
	values[i++] = Int16GetDatum(externalSize);	/* 4 */
	values[i++] = BoolGetDatum(passedByValue);	/* 5 */
	values[i++] = CharGetDatum(typeType);		/* 6 */
	values[i++] = BoolGetDatum(true);			/* 7 */
	values[i++] = CharGetDatum(typDelim);		/* 8 */
	values[i++] = ObjectIdGetDatum(typeType == 'c' ? relationOid : InvalidOid); /* 9 */
	values[i++] = ObjectIdGetDatum(elementObjectId); /* 10 */

	procs[0] = inputProcedure;
	procs[1] = outputProcedure;
	procs[2] = (receiveProcedure) ? receiveProcedure : inputProcedure;
	procs[3] = (sendProcedure) ? sendProcedure : outputProcedure;

	for (j = 0; j < 4; ++j)
	{
		Oid		procOid;

		procname = procs[j];

		/*
		 * First look for a 1-argument func with all argtypes 0. This is
		 * valid for all four kinds of procedure.
		 */
		MemSet(argList, 0, FUNC_MAX_ARGS * sizeof(Oid));

		procOid = GetSysCacheOid(PROCNAME,
								 PointerGetDatum(procname),
								 Int32GetDatum(1),
								 PointerGetDatum(argList),
								 0);

		if (!OidIsValid(procOid))
		{

			/*
			 * For array types, the input procedures may take 3 args (data
			 * value, element OID, atttypmod); the pg_proc argtype
			 * signature is 0,OIDOID,INT4OID.  The output procedures may
			 * take 2 args (data value, element OID).
			 */
			if (OidIsValid(elementObjectId))
			{
				int			nargs;

				if (j % 2)
				{
					/* output proc */
					nargs = 2;
					argList[1] = OIDOID;
				}
				else
				{
					/* input proc */
					nargs = 3;
					argList[1] = OIDOID;
					argList[2] = INT4OID;
				}
				procOid = GetSysCacheOid(PROCNAME,
										 PointerGetDatum(procname),
										 Int32GetDatum(nargs),
										 PointerGetDatum(argList),
										 0);
			}
			if (!OidIsValid(procOid))
				func_error("TypeCreate", procname, 1, argList, NULL);
		}

		values[i++] = ObjectIdGetDatum(procOid);	/* 11 - 14 */
	}

	/* ----------------
	 * set default alignment
	 * ----------------
	 */
	values[i++] = CharGetDatum(alignment);	/* 15 */

	/* ----------------
	 *	set default storage for TOAST
	 * ----------------
	 */
	values[i++] = CharGetDatum(storage);	/* 16 */

	/* ----------------
	 *	initialize the default value for this type.
	 * ----------------
	 */
	values[i] = DirectFunctionCall1(textin,	/* 17 */
				CStringGetDatum(defaultTypeValue ? defaultTypeValue : "-"));

	/* ----------------
	 *	open pg_type and begin a scan for the type name.
	 * ----------------
	 */
	pg_type_desc = heap_openr(TypeRelationName, RowExclusiveLock);

	ScanKeyEntryInitialize(typeKey,
						   0,
						   Anum_pg_type_typname,
						   F_NAMEEQ,
						   PointerGetDatum(typeName));

	pg_type_scan = heap_beginscan(pg_type_desc,
								  0,
								  SnapshotSelf, /* cache? */
								  1,
								  typeKey);

	/* ----------------
	 *	define the type either by adding a tuple to the type
	 *	relation, or by updating the fields of the "shell" tuple
	 *	already there.
	 * ----------------
	 */
	tup = heap_getnext(pg_type_scan, 0);
	if (HeapTupleIsValid(tup))
	{
		/* should not happen given prior test? */
		if (assignedTypeOid != InvalidOid)
			elog(ERROR, "TypeCreate: type %s already defined", typeName);

		tup = heap_modifytuple(tup,
							   pg_type_desc,
							   values,
							   nulls,
							   replaces);

		simple_heap_update(pg_type_desc, &tup->t_self, tup);

		typeObjectId = tup->t_data->t_oid;
	}
	else
	{
		tupDesc = pg_type_desc->rd_att;

		tup = heap_formtuple(tupDesc,
							 values,
							 nulls);

		/* preassign tuple Oid, if one was given */
		tup->t_data->t_oid = assignedTypeOid;

		heap_insert(pg_type_desc, tup);

		typeObjectId = tup->t_data->t_oid;
	}

	/* ----------------
	 *	finish up
	 * ----------------
	 */
	heap_endscan(pg_type_scan);

	if (RelationGetForm(pg_type_desc)->relhasindex)
	{
		Relation	idescs[Num_pg_type_indices];

		CatalogOpenIndices(Num_pg_type_indices, Name_pg_type_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_type_indices, pg_type_desc, tup);
		CatalogCloseIndices(Num_pg_type_indices, idescs);
	}

	heap_close(pg_type_desc, RowExclusiveLock);

	return typeObjectId;
}

/* ----------------------------------------------------------------
 *		TypeRename
 *
 *		This renames a type
 * ----------------------------------------------------------------
 */
void
TypeRename(const char *oldTypeName, const char *newTypeName)
{
	Relation	pg_type_desc;
	Relation	idescs[Num_pg_type_indices];
	HeapTuple	tuple;

	pg_type_desc = heap_openr(TypeRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(TYPENAME,
							   PointerGetDatum(oldTypeName),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "TypeRename: type \"%s\" not defined", oldTypeName);

	if (SearchSysCacheExists(TYPENAME,
							 PointerGetDatum(newTypeName),
							 0, 0, 0))
		elog(ERROR, "TypeRename: type \"%s\" already defined", newTypeName);

	namestrcpy(&(((Form_pg_type) GETSTRUCT(tuple))->typname), newTypeName);

	simple_heap_update(pg_type_desc, &tuple->t_self, tuple);

	/* update the system catalog indices */
	CatalogOpenIndices(Num_pg_type_indices, Name_pg_type_indices, idescs);
	CatalogIndexInsert(idescs, Num_pg_type_indices, pg_type_desc, tuple);
	CatalogCloseIndices(Num_pg_type_indices, idescs);

	heap_freetuple(tuple);
	heap_close(pg_type_desc, RowExclusiveLock);
}

/*
 * makeArrayTypeName(typeName);
 *	  - given a base type name, make an array of type name out of it
 *
 * the CALLER is responsible for pfreeing the
 */

char *
makeArrayTypeName(char *typeName)
{
	char	   *arr;

	if (!typeName)
		return NULL;
	arr = palloc(strlen(typeName) + 2);
	arr[0] = '_';
	strcpy(arr + 1, typeName);

	return arr;

}
