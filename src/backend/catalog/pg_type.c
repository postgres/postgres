/*-------------------------------------------------------------------------
 *
 * pg_type.c
 *	  routines to support manipulation of the pg_type relation
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_type.c,v 1.90 2003/08/04 02:39:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
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
	values[i++] = ObjectIdGetDatum(typeNamespace);		/* typnamespace */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* typowner */
	values[i++] = Int16GetDatum(0);		/* typlen */
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
	values[i++] = CharGetDatum('i');	/* typalign */
	values[i++] = CharGetDatum('p');	/* typstorage */
	values[i++] = BoolGetDatum(false);	/* typnotnull */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* typbasetype */
	values[i++] = Int32GetDatum(-1);	/* typtypmod */
	values[i++] = Int32GetDatum(0);		/* typndims */
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

	CatalogUpdateIndexes(pg_type_desc, tup);

	/*
	 * Create dependencies.  We can/must skip this in bootstrap mode.
	 */
	if (!IsBootstrapProcessingMode())
		GenerateTypeDependencies(typeNamespace,
								 typoid,
								 InvalidOid,
								 0,
								 InvalidOid,
								 InvalidOid,
								 InvalidOid,
								 InvalidOid,
								 InvalidOid,
								 InvalidOid,
								 NULL,
								 false);

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
		   Oid relationOid,		/* only for 'c'atalog types */
		   char relationKind,	/* ditto */
		   int16 internalSize,
		   char typeType,
		   char typDelim,
		   Oid inputProcedure,
		   Oid outputProcedure,
		   Oid receiveProcedure,
		   Oid sendProcedure,
		   Oid elementType,
		   Oid baseType,
		   const char *defaultTypeValue,		/* human readable rep */
		   char *defaultTypeBin,	/* cooked rep */
		   bool passedByValue,
		   char alignment,
		   char storage,
		   int32 typeMod,
		   int32 typNDims,		/* Array dimensions for baseType */
		   bool typeNotNull)
{
	Relation	pg_type_desc;
	Oid			typeObjectId;
	bool		rebuildDeps = false;
	HeapTuple	tup;
	char		nulls[Natts_pg_type];
	char		replaces[Natts_pg_type];
	Datum		values[Natts_pg_type];
	NameData	name;
	TupleDesc	tupDesc;
	int			i;

	/*
	 * We assume that the caller validated the arguments individually, but
	 * did not check for bad combinations.
	 *
	 * Validate size specifications: either positive (fixed-length) or -1
	 * (varlena) or -2 (cstring).  Pass-by-value types must have a fixed
	 * length not more than sizeof(Datum).
	 */
	if (!(internalSize > 0 ||
		  internalSize == -1 ||
		  internalSize == -2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("invalid type internal size %d",
						internalSize)));
	if (passedByValue &&
		(internalSize <= 0 || internalSize > (int16) sizeof(Datum)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("invalid type internal size %d",
						internalSize)));

	/* Only varlena types can be toasted */
	if (storage != 'p' && internalSize != -1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("fixed-size types must have storage PLAIN")));

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
	values[i++] = ObjectIdGetDatum(typeNamespace);		/* typnamespace */
	values[i++] = Int32GetDatum(GetUserId());	/* typowner */
	values[i++] = Int16GetDatum(internalSize);	/* typlen */
	values[i++] = BoolGetDatum(passedByValue);	/* typbyval */
	values[i++] = CharGetDatum(typeType);		/* typtype */
	values[i++] = BoolGetDatum(true);	/* typisdefined */
	values[i++] = CharGetDatum(typDelim);		/* typdelim */
	values[i++] = ObjectIdGetDatum(typeType == 'c' ? relationOid : InvalidOid); /* typrelid */
	values[i++] = ObjectIdGetDatum(elementType);		/* typelem */
	values[i++] = ObjectIdGetDatum(inputProcedure);		/* typinput */
	values[i++] = ObjectIdGetDatum(outputProcedure);	/* typoutput */
	values[i++] = ObjectIdGetDatum(receiveProcedure);	/* typreceive */
	values[i++] = ObjectIdGetDatum(sendProcedure);		/* typsend */
	values[i++] = CharGetDatum(alignment);		/* typalign */
	values[i++] = CharGetDatum(storage);		/* typstorage */
	values[i++] = BoolGetDatum(typeNotNull);	/* typnotnull */
	values[i++] = ObjectIdGetDatum(baseType);	/* typbasetype */
	values[i++] = Int32GetDatum(typeMod);		/* typtypmod */
	values[i++] = Int32GetDatum(typNDims);		/* typndims */

	/*
	 * initialize the default binary value for this type.  Check for nulls
	 * of course.
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
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));

		/*
		 * Okay to update existing "shell" type tuple
		 */
		tup = heap_modifytuple(tup,
							   pg_type_desc,
							   values,
							   nulls,
							   replaces);

		simple_heap_update(pg_type_desc, &tup->t_self, tup);

		typeObjectId = HeapTupleGetOid(tup);

		rebuildDeps = true;		/* get rid of shell type's dependencies */
	}
	else
	{
		tupDesc = pg_type_desc->rd_att;

		tup = heap_formtuple(tupDesc,
							 values,
							 nulls);

		/* preassign tuple Oid, if one was given */
		HeapTupleSetOid(tup, assignedTypeOid);

		typeObjectId = simple_heap_insert(pg_type_desc, tup);
	}

	/* Update indexes */
	CatalogUpdateIndexes(pg_type_desc, tup);

	/*
	 * Create dependencies.  We can/must skip this in bootstrap mode.
	 */
	if (!IsBootstrapProcessingMode())
		GenerateTypeDependencies(typeNamespace,
								 typeObjectId,
								 relationOid,
								 relationKind,
								 inputProcedure,
								 outputProcedure,
								 receiveProcedure,
								 sendProcedure,
								 elementType,
								 baseType,
								 (defaultTypeBin ?
								  stringToNode(defaultTypeBin) :
								  (void *) NULL),
								 rebuildDeps);

	/*
	 * finish up
	 */
	heap_close(pg_type_desc, RowExclusiveLock);

	return typeObjectId;
}

/*
 * GenerateTypeDependencies: build the dependencies needed for a type
 *
 * If rebuild is true, we remove existing dependencies and rebuild them
 * from scratch.  This is needed for ALTER TYPE, and also when replacing
 * a shell type.
 *
 * NOTE: a shell type will have a dependency to its namespace, and no others.
 */
void
GenerateTypeDependencies(Oid typeNamespace,
						 Oid typeObjectId,
						 Oid relationOid,		/* only for 'c'atalog
												 * types */
						 char relationKind,		/* ditto */
						 Oid inputProcedure,
						 Oid outputProcedure,
						 Oid receiveProcedure,
						 Oid sendProcedure,
						 Oid elementType,
						 Oid baseType,
						 Node *defaultExpr,
						 bool rebuild)
{
	ObjectAddress myself,
				referenced;

	if (rebuild)
		deleteDependencyRecordsFor(RelOid_pg_type,
								   typeObjectId);

	myself.classId = RelOid_pg_type;
	myself.objectId = typeObjectId;
	myself.objectSubId = 0;

	/* dependency on namespace */
	/* skip for relation rowtype, since we have indirect dependency */
	if (!OidIsValid(relationOid))
	{
		referenced.classId = get_system_catalog_relid(NamespaceRelationName);
		referenced.objectId = typeNamespace;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Normal dependencies on the I/O functions */
	if (OidIsValid(inputProcedure))
	{
		referenced.classId = RelOid_pg_proc;
		referenced.objectId = inputProcedure;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	if (OidIsValid(outputProcedure))
	{
		referenced.classId = RelOid_pg_proc;
		referenced.objectId = outputProcedure;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	if (OidIsValid(receiveProcedure))
	{
		referenced.classId = RelOid_pg_proc;
		referenced.objectId = receiveProcedure;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	if (OidIsValid(sendProcedure))
	{
		referenced.classId = RelOid_pg_proc;
		referenced.objectId = sendProcedure;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/*
	 * If the type is a rowtype for a relation, mark it as internally
	 * dependent on the relation, *unless* it is a stand-alone composite
	 * type relation. For the latter case, we have to reverse the
	 * dependency.
	 *
	 * In the former case, this allows the type to be auto-dropped when the
	 * relation is, and not otherwise. And in the latter, of course we get
	 * the opposite effect.
	 */
	if (OidIsValid(relationOid))
	{
		referenced.classId = RelOid_pg_class;
		referenced.objectId = relationOid;
		referenced.objectSubId = 0;

		if (relationKind != RELKIND_COMPOSITE_TYPE)
			recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
		else
			recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
	}

	/*
	 * If the type is an array type, mark it auto-dependent on the base
	 * type.  (This is a compromise between the typical case where the
	 * array type is automatically generated and the case where it is
	 * manually created: we'd prefer INTERNAL for the former case and
	 * NORMAL for the latter.)
	 */
	if (OidIsValid(elementType))
	{
		referenced.classId = RelOid_pg_type;
		referenced.objectId = elementType;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
	}

	/* Normal dependency from a domain to its base type. */
	if (OidIsValid(baseType))
	{
		referenced.classId = RelOid_pg_type;
		referenced.objectId = baseType;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Normal dependency on the default expression. */
	if (defaultExpr)
		recordDependencyOnExpr(&myself, defaultExpr, NIL, DEPENDENCY_NORMAL);
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
	HeapTuple	tuple;

	pg_type_desc = heap_openr(TypeRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(TYPENAMENSP,
							   CStringGetDatum(oldTypeName),
							   ObjectIdGetDatum(typeNamespace),
							   0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist", oldTypeName)));

	if (SearchSysCacheExists(TYPENAMENSP,
							 CStringGetDatum(newTypeName),
							 ObjectIdGetDatum(typeNamespace),
							 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("type \"%s\" already exists", newTypeName)));

	namestrcpy(&(((Form_pg_type) GETSTRUCT(tuple))->typname), newTypeName);

	simple_heap_update(pg_type_desc, &tuple->t_self, tuple);

	/* update the system catalog indexes */
	CatalogUpdateIndexes(pg_type_desc, tuple);

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
