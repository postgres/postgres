/*-------------------------------------------------------------------------
 *
 * pg_type.c
 *	  routines to support manipulation of the pg_type relation
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_type.c,v 1.72 2002/06/20 20:29:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


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
TypeShellMake(const char *typeName, Oid typeNamespace)
{
	Relation	pg_type_desc;
	TupleDesc	tupDesc;
	int			i;
	HeapTuple	tup;
	Datum		values[Natts_pg_type];
	char		nulls[Natts_pg_type];
	Oid			typoid;
	NameData	name;

	Assert(PointerIsValid(typeName));

	/*
	 * open pg_type
	 */
	pg_type_desc = heap_openr(TypeRelationName, RowExclusiveLock);
	tupDesc = pg_type_desc->rd_att;

	/*
	 * initialize our *nulls and *values arrays
	 */
	for (i = 0; i < Natts_pg_type; ++i)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;		/* redundant, but safe */
	}

	/*
	 * initialize *values with the type name and dummy values
	 */
	i = 0;
	namestrcpy(&name, typeName);
	values[i++] = NameGetDatum(&name);	/* typname */
	values[i++] = ObjectIdGetDatum(typeNamespace); /* typnamespace */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* typowner */
	values[i++] = Int16GetDatum(0);		/* typlen */
	values[i++] = Int16GetDatum(0);		/* typprtlen */
	values[i++] = BoolGetDatum(false);	/* typbyval */
	values[i++] = CharGetDatum(0);		/* typtype */
	values[i++] = BoolGetDatum(false);	/* typisdefined */
	values[i++] = CharGetDatum(0);		/* typdelim */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* typrelid */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* typelem */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* typinput */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* typoutput */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* typreceive */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* typsend */
	values[i++] = CharGetDatum('i');			/* typalign */
	values[i++] = CharGetDatum('p');			/* typstorage */
	values[i++] = BoolGetDatum(false);			/* typnotnull */
	values[i++] = ObjectIdGetDatum(InvalidOid);	/* typbasetype */
	values[i++] = Int32GetDatum(-1);			/* typtypmod */
	values[i++] = Int32GetDatum(0);				/* typndims */
	nulls[i++] = 'n';			/* typdefaultbin */
	nulls[i++] = 'n';			/* typdefault */

	/*
	 * create a new type tuple
	 */
	tup = heap_formtuple(tupDesc, values, nulls);

	/*
	 * insert the tuple in the relation and get the tuple's oid.
	 */
	typoid = simple_heap_insert(pg_type_desc, tup);

	if (RelationGetForm(pg_type_desc)->relhasindex)
	{
		Relation	idescs[Num_pg_type_indices];

		CatalogOpenIndices(Num_pg_type_indices, Name_pg_type_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_type_indices, pg_type_desc, tup);
		CatalogCloseIndices(Num_pg_type_indices, idescs);
	}

	/*
	 * clean up and return the type-oid
	 */
	heap_freetuple(tup);
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
TypeCreate(const char *typeName,
		   Oid typeNamespace,
		   Oid assignedTypeOid,
		   Oid relationOid,			/* only for 'c'atalog typeTypes */
		   int16 internalSize,
		   int16 externalSize,
		   char typeType,
		   char typDelim,
		   Oid inputProcedure,
		   Oid outputProcedure,
		   Oid receiveProcedure,
		   Oid sendProcedure,
		   Oid elementType,
		   Oid baseType,
		   const char *defaultTypeValue,	/* human readable rep */
		   const char *defaultTypeBin,	/* cooked rep */
		   bool passedByValue,
		   char alignment,
		   char storage,
		   int32 typeMod,
		   int32 typNDims,			/* Array dimensions for baseType */
		   bool typeNotNull)
{
	Relation	pg_type_desc;
	Oid			typeObjectId;
	HeapTuple	tup;
	char		nulls[Natts_pg_type];
	char		replaces[Natts_pg_type];
	Datum		values[Natts_pg_type];
	NameData	name;
	TupleDesc	tupDesc;
	int			i;

	/*
	 * validate size specifications: either positive (fixed-length) or -1
	 * (variable-length).
	 */
	if (!(internalSize > 0 || internalSize == -1))
		elog(ERROR, "TypeCreate: invalid type internal size %d",
			 internalSize);
	if (!(externalSize > 0 || externalSize == -1))
		elog(ERROR, "TypeCreate: invalid type external size %d",
			 externalSize);

	if (internalSize != -1 && storage != 'p')
		elog(ERROR, "TypeCreate: fixed size types must have storage PLAIN");

	/*
	 * initialize arrays needed for heap_formtuple or heap_modifytuple
	 */
	for (i = 0; i < Natts_pg_type; ++i)
	{
		nulls[i] = ' ';
		replaces[i] = 'r';
		values[i] = (Datum) 0;
	}

	/*
	 * initialize the *values information
	 */
	i = 0;
	namestrcpy(&name, typeName);
	values[i++] = NameGetDatum(&name);	/* typname */
	values[i++] = ObjectIdGetDatum(typeNamespace);	/* typnamespace */
	values[i++] = Int32GetDatum(GetUserId());	/* typowner */
	values[i++] = Int16GetDatum(internalSize);	/* typlen */
	values[i++] = Int16GetDatum(externalSize);	/* typprtlen */
	values[i++] = BoolGetDatum(passedByValue);	/* typbyval */
	values[i++] = CharGetDatum(typeType);		/* typtype */
	values[i++] = BoolGetDatum(true);			/* typisdefined */
	values[i++] = CharGetDatum(typDelim);		/* typdelim */
	values[i++] = ObjectIdGetDatum(typeType == 'c' ? relationOid : InvalidOid); /* typrelid */
	values[i++] = ObjectIdGetDatum(elementType);	/* typelem */
	values[i++] = ObjectIdGetDatum(inputProcedure);	/* typinput */
	values[i++] = ObjectIdGetDatum(outputProcedure); /* typoutput */
	values[i++] = ObjectIdGetDatum(receiveProcedure); /* typreceive */
	values[i++] = ObjectIdGetDatum(sendProcedure);	/* typsend */
	values[i++] = CharGetDatum(alignment);		/* typalign */
	values[i++] = CharGetDatum(storage);		/* typstorage */
	values[i++] = BoolGetDatum(typeNotNull);		/* typnotnull */
	values[i++] = ObjectIdGetDatum(baseType);		/* typbasetype */
	values[i++] = Int32GetDatum(typeMod);			/* typtypmod */
	values[i++] = Int32GetDatum(typNDims);			/* typndims */

	/*
	 * initialize the default binary value for this type.  Check for
	 * nulls of course.
	 */
	if (defaultTypeBin)
		values[i] = DirectFunctionCall1(textin,
										CStringGetDatum(defaultTypeBin));
	else
		nulls[i] = 'n';
	i++;						/* typdefaultbin */

	/*
	 * initialize the default value for this type.
	 */
	if (defaultTypeValue)
		values[i] = DirectFunctionCall1(textin,
										CStringGetDatum(defaultTypeValue));
	else
		nulls[i] = 'n';
	i++;						/* typdefault */

	/*
	 * open pg_type and prepare to insert or update a row.
	 *
	 * NOTE: updating will not work correctly in bootstrap mode; but we don't
	 * expect to be overwriting any shell types in bootstrap mode.
	 */
	pg_type_desc = heap_openr(TypeRelationName, RowExclusiveLock);

	tup = SearchSysCacheCopy(TYPENAMENSP,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace),
							 0, 0);
	if (HeapTupleIsValid(tup))
	{
		/*
		 * check that the type is not already defined.	It may exist as a
		 * shell type, however (but only if assignedTypeOid is not given).
		 */
		if (((Form_pg_type) GETSTRUCT(tup))->typisdefined ||
			assignedTypeOid != InvalidOid)
			elog(ERROR, "type %s already exists", typeName);

		/*
		 * Okay to update existing "shell" type tuple
		 */
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

		typeObjectId = simple_heap_insert(pg_type_desc, tup);
	}

	/* Update indices (not necessary if bootstrapping) */
	if (RelationGetForm(pg_type_desc)->relhasindex)
	{
		Relation	idescs[Num_pg_type_indices];

		CatalogOpenIndices(Num_pg_type_indices, Name_pg_type_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_type_indices, pg_type_desc, tup);
		CatalogCloseIndices(Num_pg_type_indices, idescs);
	}

	/*
	 * finish up
	 */
	heap_close(pg_type_desc, RowExclusiveLock);

	return typeObjectId;
}

/*
 * TypeRename
 *		This renames a type
 *
 * Note: any associated array type is *not* renamed; caller must make
 * another call to handle that case.  Currently this is only used for
 * renaming types associated with tables, for which there are no arrays.
 */
void
TypeRename(const char *oldTypeName, Oid typeNamespace,
		   const char *newTypeName)
{
	Relation	pg_type_desc;
	Relation	idescs[Num_pg_type_indices];
	HeapTuple	tuple;

	pg_type_desc = heap_openr(TypeRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(TYPENAMENSP,
							   CStringGetDatum(oldTypeName),
							   ObjectIdGetDatum(typeNamespace),
							   0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "type %s does not exist", oldTypeName);

	if (SearchSysCacheExists(TYPENAMENSP,
							 CStringGetDatum(newTypeName),
							 ObjectIdGetDatum(typeNamespace),
							 0, 0))
		elog(ERROR, "type named %s already exists", newTypeName);

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
 * the caller is responsible for pfreeing the result
 */
char *
makeArrayTypeName(const char *typeName)
{
	char	   *arr;

	if (!typeName)
		return NULL;
	arr = palloc(NAMEDATALEN);
	snprintf(arr, NAMEDATALEN,
			 "_%.*s", NAMEDATALEN - 2, typeName);
	return arr;
}
