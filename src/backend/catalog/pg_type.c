/*-------------------------------------------------------------------------
 *
 * pg_type.c
 *	  routines to support manipulation of the pg_type relation
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_type.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/typecmds.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/* Potentially set by pg_upgrade_support functions */
Oid			binary_upgrade_next_pg_type_oid = InvalidOid;

/* ----------------------------------------------------------------
 *		TypeShellMake
 *
 *		This procedure inserts a "shell" tuple into the pg_type relation.
 *		The type tuple inserted has valid but dummy values, and its
 *		"typisdefined" field is false indicating it's not really defined.
 *
 *		This is used so that a tuple exists in the catalogs.  The I/O
 *		functions for the type will link to this tuple.  When the full
 *		CREATE TYPE command is issued, the bogus values will be replaced
 *		with correct ones, and "typisdefined" will be set to true.
 * ----------------------------------------------------------------
 */
ObjectAddress
TypeShellMake(const char *typeName, Oid typeNamespace, Oid ownerId)
{
	Relation	pg_type_desc;
	TupleDesc	tupDesc;
	int			i;
	HeapTuple	tup;
	Datum		values[Natts_pg_type];
	bool		nulls[Natts_pg_type];
	Oid			typoid;
	NameData	name;
	ObjectAddress address;

	Assert(PointerIsValid(typeName));

	/*
	 * open pg_type
	 */
	pg_type_desc = table_open(TypeRelationId, RowExclusiveLock);
	tupDesc = pg_type_desc->rd_att;

	/*
	 * initialize our *nulls and *values arrays
	 */
	for (i = 0; i < Natts_pg_type; ++i)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;	/* redundant, but safe */
	}

	/*
	 * initialize *values with the type name and dummy values
	 *
	 * The representational details are the same as int4 ... it doesn't really
	 * matter what they are so long as they are consistent.  Also note that we
	 * give it typtype = TYPTYPE_PSEUDO as extra insurance that it won't be
	 * mistaken for a usable type.
	 */
	namestrcpy(&name, typeName);
	values[Anum_pg_type_typname - 1] = NameGetDatum(&name);
	values[Anum_pg_type_typnamespace - 1] = ObjectIdGetDatum(typeNamespace);
	values[Anum_pg_type_typowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_type_typlen - 1] = Int16GetDatum(sizeof(int32));
	values[Anum_pg_type_typbyval - 1] = BoolGetDatum(true);
	values[Anum_pg_type_typtype - 1] = CharGetDatum(TYPTYPE_PSEUDO);
	values[Anum_pg_type_typcategory - 1] = CharGetDatum(TYPCATEGORY_PSEUDOTYPE);
	values[Anum_pg_type_typispreferred - 1] = BoolGetDatum(false);
	values[Anum_pg_type_typisdefined - 1] = BoolGetDatum(false);
	values[Anum_pg_type_typdelim - 1] = CharGetDatum(DEFAULT_TYPDELIM);
	values[Anum_pg_type_typrelid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typelem - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typarray - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typinput - 1] = ObjectIdGetDatum(F_SHELL_IN);
	values[Anum_pg_type_typoutput - 1] = ObjectIdGetDatum(F_SHELL_OUT);
	values[Anum_pg_type_typreceive - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typsend - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typmodin - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typmodout - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typanalyze - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typalign - 1] = CharGetDatum(TYPALIGN_INT);
	values[Anum_pg_type_typstorage - 1] = CharGetDatum(TYPSTORAGE_PLAIN);
	values[Anum_pg_type_typnotnull - 1] = BoolGetDatum(false);
	values[Anum_pg_type_typbasetype - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typtypmod - 1] = Int32GetDatum(-1);
	values[Anum_pg_type_typndims - 1] = Int32GetDatum(0);
	values[Anum_pg_type_typcollation - 1] = ObjectIdGetDatum(InvalidOid);
	nulls[Anum_pg_type_typdefaultbin - 1] = true;
	nulls[Anum_pg_type_typdefault - 1] = true;
	nulls[Anum_pg_type_typacl - 1] = true;

	/* Use binary-upgrade override for pg_type.oid? */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type OID value not set when in binary upgrade mode")));

		typoid = binary_upgrade_next_pg_type_oid;
		binary_upgrade_next_pg_type_oid = InvalidOid;
	}
	else
	{
		typoid = GetNewOidWithIndex(pg_type_desc, TypeOidIndexId,
									Anum_pg_type_oid);
	}

	values[Anum_pg_type_oid - 1] = ObjectIdGetDatum(typoid);

	/*
	 * create a new type tuple
	 */
	tup = heap_form_tuple(tupDesc, values, nulls);

	/*
	 * insert the tuple in the relation and get the tuple's oid.
	 */
	CatalogTupleInsert(pg_type_desc, tup);

	/*
	 * Create dependencies.  We can/must skip this in bootstrap mode.
	 */
	if (!IsBootstrapProcessingMode())
		GenerateTypeDependencies(tup,
								 pg_type_desc,
								 NULL,
								 NULL,
								 0,
								 false,
								 false,
								 false);

	/* Post creation hook for new shell type */
	InvokeObjectPostCreateHook(TypeRelationId, typoid, 0);

	ObjectAddressSet(address, TypeRelationId, typoid);

	/*
	 * clean up and return the type-oid
	 */
	heap_freetuple(tup);
	table_close(pg_type_desc, RowExclusiveLock);

	return address;
}

/* ----------------------------------------------------------------
 *		TypeCreate
 *
 *		This does all the necessary work needed to define a new type.
 *
 *		Returns the ObjectAddress assigned to the new type.
 *		If newTypeOid is zero (the normal case), a new OID is created;
 *		otherwise we use exactly that OID.
 * ----------------------------------------------------------------
 */
ObjectAddress
TypeCreate(Oid newTypeOid,
		   const char *typeName,
		   Oid typeNamespace,
		   Oid relationOid,		/* only for relation rowtypes */
		   char relationKind,	/* ditto */
		   Oid ownerId,
		   int16 internalSize,
		   char typeType,
		   char typeCategory,
		   bool typePreferred,
		   char typDelim,
		   Oid inputProcedure,
		   Oid outputProcedure,
		   Oid receiveProcedure,
		   Oid sendProcedure,
		   Oid typmodinProcedure,
		   Oid typmodoutProcedure,
		   Oid analyzeProcedure,
		   Oid elementType,
		   bool isImplicitArray,
		   Oid arrayType,
		   Oid baseType,
		   const char *defaultTypeValue,	/* human-readable rep */
		   char *defaultTypeBin,	/* cooked rep */
		   bool passedByValue,
		   char alignment,
		   char storage,
		   int32 typeMod,
		   int32 typNDims,		/* Array dimensions for baseType */
		   bool typeNotNull,
		   Oid typeCollation)
{
	Relation	pg_type_desc;
	Oid			typeObjectId;
	bool		isDependentType;
	bool		rebuildDeps = false;
	Acl		   *typacl;
	HeapTuple	tup;
	bool		nulls[Natts_pg_type];
	bool		replaces[Natts_pg_type];
	Datum		values[Natts_pg_type];
	NameData	name;
	int			i;
	ObjectAddress address;

	/*
	 * We assume that the caller validated the arguments individually, but did
	 * not check for bad combinations.
	 *
	 * Validate size specifications: either positive (fixed-length) or -1
	 * (varlena) or -2 (cstring).
	 */
	if (!(internalSize > 0 ||
		  internalSize == -1 ||
		  internalSize == -2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("invalid type internal size %d",
						internalSize)));

	if (passedByValue)
	{
		/*
		 * Pass-by-value types must have a fixed length that is one of the
		 * values supported by fetch_att() and store_att_byval(); and the
		 * alignment had better agree, too.  All this code must match
		 * access/tupmacs.h!
		 */
		if (internalSize == (int16) sizeof(char))
		{
			if (alignment != TYPALIGN_CHAR)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
		else if (internalSize == (int16) sizeof(int16))
		{
			if (alignment != TYPALIGN_SHORT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
		else if (internalSize == (int16) sizeof(int32))
		{
			if (alignment != TYPALIGN_INT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
#if SIZEOF_DATUM == 8
		else if (internalSize == (int16) sizeof(Datum))
		{
			if (alignment != TYPALIGN_DOUBLE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
#endif
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("internal size %d is invalid for passed-by-value type",
							internalSize)));
	}
	else
	{
		/* varlena types must have int align or better */
		if (internalSize == -1 &&
			!(alignment == TYPALIGN_INT || alignment == TYPALIGN_DOUBLE))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("alignment \"%c\" is invalid for variable-length type",
							alignment)));
		/* cstring must have char alignment */
		if (internalSize == -2 && !(alignment == TYPALIGN_CHAR))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("alignment \"%c\" is invalid for variable-length type",
							alignment)));
	}

	/* Only varlena types can be toasted */
	if (storage != TYPSTORAGE_PLAIN && internalSize != -1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("fixed-size types must have storage PLAIN")));

	/*
	 * This is a dependent type if it's an implicitly-created array type, or
	 * if it's a relation rowtype that's not a composite type.  For such types
	 * we'll leave the ACL empty, and we'll skip creating some dependency
	 * records because there will be a dependency already through the
	 * depended-on type or relation.  (Caution: this is closely intertwined
	 * with some behavior in GenerateTypeDependencies.)
	 */
	isDependentType = isImplicitArray ||
		(OidIsValid(relationOid) && relationKind != RELKIND_COMPOSITE_TYPE);

	/*
	 * initialize arrays needed for heap_form_tuple or heap_modify_tuple
	 */
	for (i = 0; i < Natts_pg_type; ++i)
	{
		nulls[i] = false;
		replaces[i] = true;
		values[i] = (Datum) 0;
	}

	/*
	 * insert data values
	 */
	namestrcpy(&name, typeName);
	values[Anum_pg_type_typname - 1] = NameGetDatum(&name);
	values[Anum_pg_type_typnamespace - 1] = ObjectIdGetDatum(typeNamespace);
	values[Anum_pg_type_typowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_type_typlen - 1] = Int16GetDatum(internalSize);
	values[Anum_pg_type_typbyval - 1] = BoolGetDatum(passedByValue);
	values[Anum_pg_type_typtype - 1] = CharGetDatum(typeType);
	values[Anum_pg_type_typcategory - 1] = CharGetDatum(typeCategory);
	values[Anum_pg_type_typispreferred - 1] = BoolGetDatum(typePreferred);
	values[Anum_pg_type_typisdefined - 1] = BoolGetDatum(true);
	values[Anum_pg_type_typdelim - 1] = CharGetDatum(typDelim);
	values[Anum_pg_type_typrelid - 1] = ObjectIdGetDatum(relationOid);
	values[Anum_pg_type_typelem - 1] = ObjectIdGetDatum(elementType);
	values[Anum_pg_type_typarray - 1] = ObjectIdGetDatum(arrayType);
	values[Anum_pg_type_typinput - 1] = ObjectIdGetDatum(inputProcedure);
	values[Anum_pg_type_typoutput - 1] = ObjectIdGetDatum(outputProcedure);
	values[Anum_pg_type_typreceive - 1] = ObjectIdGetDatum(receiveProcedure);
	values[Anum_pg_type_typsend - 1] = ObjectIdGetDatum(sendProcedure);
	values[Anum_pg_type_typmodin - 1] = ObjectIdGetDatum(typmodinProcedure);
	values[Anum_pg_type_typmodout - 1] = ObjectIdGetDatum(typmodoutProcedure);
	values[Anum_pg_type_typanalyze - 1] = ObjectIdGetDatum(analyzeProcedure);
	values[Anum_pg_type_typalign - 1] = CharGetDatum(alignment);
	values[Anum_pg_type_typstorage - 1] = CharGetDatum(storage);
	values[Anum_pg_type_typnotnull - 1] = BoolGetDatum(typeNotNull);
	values[Anum_pg_type_typbasetype - 1] = ObjectIdGetDatum(baseType);
	values[Anum_pg_type_typtypmod - 1] = Int32GetDatum(typeMod);
	values[Anum_pg_type_typndims - 1] = Int32GetDatum(typNDims);
	values[Anum_pg_type_typcollation - 1] = ObjectIdGetDatum(typeCollation);

	/*
	 * initialize the default binary value for this type.  Check for nulls of
	 * course.
	 */
	if (defaultTypeBin)
		values[Anum_pg_type_typdefaultbin - 1] = CStringGetTextDatum(defaultTypeBin);
	else
		nulls[Anum_pg_type_typdefaultbin - 1] = true;

	/*
	 * initialize the default value for this type.
	 */
	if (defaultTypeValue)
		values[Anum_pg_type_typdefault - 1] = CStringGetTextDatum(defaultTypeValue);
	else
		nulls[Anum_pg_type_typdefault - 1] = true;

	/*
	 * Initialize the type's ACL, too.  But dependent types don't get one.
	 */
	if (isDependentType)
		typacl = NULL;
	else
		typacl = get_user_default_acl(OBJECT_TYPE, ownerId,
									  typeNamespace);
	if (typacl != NULL)
		values[Anum_pg_type_typacl - 1] = PointerGetDatum(typacl);
	else
		nulls[Anum_pg_type_typacl - 1] = true;

	/*
	 * open pg_type and prepare to insert or update a row.
	 *
	 * NOTE: updating will not work correctly in bootstrap mode; but we don't
	 * expect to be overwriting any shell types in bootstrap mode.
	 */
	pg_type_desc = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy2(TYPENAMENSP,
							  CStringGetDatum(typeName),
							  ObjectIdGetDatum(typeNamespace));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_type typform = (Form_pg_type) GETSTRUCT(tup);

		/*
		 * check that the type is not already defined.  It may exist as a
		 * shell type, however.
		 */
		if (typform->typisdefined)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));

		/*
		 * shell type must have been created by same owner
		 */
		if (typform->typowner != ownerId)
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TYPE, typeName);

		/* trouble if caller wanted to force the OID */
		if (OidIsValid(newTypeOid))
			elog(ERROR, "cannot assign new OID to existing shell type");

		replaces[Anum_pg_type_oid - 1] = false;

		/*
		 * Okay to update existing shell type tuple
		 */
		tup = heap_modify_tuple(tup,
								RelationGetDescr(pg_type_desc),
								values,
								nulls,
								replaces);

		CatalogTupleUpdate(pg_type_desc, &tup->t_self, tup);

		typeObjectId = typform->oid;

		rebuildDeps = true;		/* get rid of shell type's dependencies */
	}
	else
	{
		/* Force the OID if requested by caller */
		if (OidIsValid(newTypeOid))
			typeObjectId = newTypeOid;
		/* Use binary-upgrade override for pg_type.oid, if supplied. */
		else if (IsBinaryUpgrade)
		{
			if (!OidIsValid(binary_upgrade_next_pg_type_oid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("pg_type OID value not set when in binary upgrade mode")));

			typeObjectId = binary_upgrade_next_pg_type_oid;
			binary_upgrade_next_pg_type_oid = InvalidOid;
		}
		else
		{
			typeObjectId = GetNewOidWithIndex(pg_type_desc, TypeOidIndexId,
											  Anum_pg_type_oid);
		}

		values[Anum_pg_type_oid - 1] = ObjectIdGetDatum(typeObjectId);

		tup = heap_form_tuple(RelationGetDescr(pg_type_desc),
							  values, nulls);

		CatalogTupleInsert(pg_type_desc, tup);
	}

	/*
	 * Create dependencies.  We can/must skip this in bootstrap mode.
	 */
	if (!IsBootstrapProcessingMode())
		GenerateTypeDependencies(tup,
								 pg_type_desc,
								 (defaultTypeBin ?
								  stringToNode(defaultTypeBin) :
								  NULL),
								 typacl,
								 relationKind,
								 isImplicitArray,
								 isDependentType,
								 rebuildDeps);

	/* Post creation hook for new type */
	InvokeObjectPostCreateHook(TypeRelationId, typeObjectId, 0);

	ObjectAddressSet(address, TypeRelationId, typeObjectId);

	/*
	 * finish up
	 */
	table_close(pg_type_desc, RowExclusiveLock);

	return address;
}

/*
 * Get a list of all distinct collations that the given type depends on.
 */
List *
GetTypeCollations(Oid typeoid)
{
	List	   *result = NIL;
	HeapTuple	tuple;
	Form_pg_type typeTup;

	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", typeoid);
	typeTup = (Form_pg_type) GETSTRUCT(tuple);

	if (OidIsValid(typeTup->typcollation))
		result = list_append_unique_oid(result, typeTup->typcollation);
	else if (typeTup->typtype == TYPTYPE_COMPOSITE)
	{
		Relation	rel = relation_open(typeTup->typrelid, AccessShareLock);
		TupleDesc	desc = RelationGetDescr(rel);

		for (int i = 0; i < RelationGetNumberOfAttributes(rel); i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (OidIsValid(att->attcollation))
				result = list_append_unique_oid(result, att->attcollation);
			else
				result = list_concat_unique_oid(result,
												GetTypeCollations(att->atttypid));
		}

		relation_close(rel, NoLock);
	}
	else if (typeTup->typtype == TYPTYPE_DOMAIN)
	{
		Assert(OidIsValid(typeTup->typbasetype));

		result = list_concat_unique_oid(result,
										GetTypeCollations(typeTup->typbasetype));
	}
	else if (typeTup->typtype == TYPTYPE_RANGE)
	{
		Oid			rangeid = get_range_subtype(typeTup->oid);

		Assert(OidIsValid(rangeid));

		result = list_concat_unique_oid(result, GetTypeCollations(rangeid));
	}
	else if (OidIsValid(typeTup->typelem))
		result = list_concat_unique_oid(result,
										GetTypeCollations(typeTup->typelem));

	ReleaseSysCache(tuple);

	return result;
}

/*
 * GenerateTypeDependencies: build the dependencies needed for a type
 *
 * Most of what this function needs to know about the type is passed as the
 * new pg_type row, typeTuple.  We make callers pass the pg_type Relation
 * as well, so that we have easy access to a tuple descriptor for the row.
 *
 * While this is able to extract the defaultExpr and typacl from the tuple,
 * doing so is relatively expensive, and callers may have those values at
 * hand already.  Pass those if handy, otherwise pass NULL.  (typacl is really
 * "Acl *", but we declare it "void *" to avoid including acl.h in pg_type.h.)
 *
 * relationKind and isImplicitArray are likewise somewhat expensive to deduce
 * from the tuple, so we make callers pass those (they're not optional).
 *
 * isDependentType is true if this is an implicit array or relation rowtype;
 * that means it doesn't need its own dependencies on owner etc.
 *
 * If rebuild is true, we remove existing dependencies and rebuild them
 * from scratch.  This is needed for ALTER TYPE, and also when replacing
 * a shell type.  We don't remove an existing extension dependency, though.
 * (That means an extension can't absorb a shell type created in another
 * extension, nor ALTER a type created by another extension.  Also, if it
 * replaces a free-standing shell type or ALTERs a free-standing type,
 * that type will become a member of the extension.)
 */
void
GenerateTypeDependencies(HeapTuple typeTuple,
						 Relation typeCatalog,
						 Node *defaultExpr,
						 void *typacl,
						 char relationKind, /* only for relation rowtypes */
						 bool isImplicitArray,
						 bool isDependentType,
						 bool rebuild)
{
	Form_pg_type typeForm = (Form_pg_type) GETSTRUCT(typeTuple);
	Oid			typeObjectId = typeForm->oid;
	Datum		datum;
	bool		isNull;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs_normal;

	/* Extract defaultExpr if caller didn't pass it */
	if (defaultExpr == NULL)
	{
		datum = heap_getattr(typeTuple, Anum_pg_type_typdefaultbin,
							 RelationGetDescr(typeCatalog), &isNull);
		if (!isNull)
			defaultExpr = stringToNode(TextDatumGetCString(datum));
	}
	/* Extract typacl if caller didn't pass it */
	if (typacl == NULL)
	{
		datum = heap_getattr(typeTuple, Anum_pg_type_typacl,
							 RelationGetDescr(typeCatalog), &isNull);
		if (!isNull)
			typacl = DatumGetAclPCopy(datum);
	}

	/* If rebuild, first flush old dependencies, except extension deps */
	if (rebuild)
	{
		deleteDependencyRecordsFor(TypeRelationId, typeObjectId, true);
		deleteSharedDependencyRecordsFor(TypeRelationId, typeObjectId, 0);
	}

	ObjectAddressSet(myself, TypeRelationId, typeObjectId);

	/*
	 * Make dependencies on namespace, owner, ACL, extension.
	 *
	 * Skip these for a dependent type, since it will have such dependencies
	 * indirectly through its depended-on type or relation.
	 */

	/* placeholder for all normal dependencies */
	addrs_normal = new_object_addresses();

	if (!isDependentType)
	{
		ObjectAddressSet(referenced, NamespaceRelationId,
						 typeForm->typnamespace);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

		recordDependencyOnOwner(TypeRelationId, typeObjectId,
								typeForm->typowner);

		recordDependencyOnNewAcl(TypeRelationId, typeObjectId, 0,
								 typeForm->typowner, typacl);

		recordDependencyOnCurrentExtension(&myself, rebuild);
	}

	/* Normal dependencies on the I/O functions */
	if (OidIsValid(typeForm->typinput))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typinput);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typoutput))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typoutput);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typreceive))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typreceive);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typsend))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typsend);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typmodin))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typmodin);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typmodout))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typmodout);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typanalyze))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typanalyze);
		add_exact_object_address(&referenced, addrs_normal);
	}

	/* Normal dependency from a domain to its base type. */
	if (OidIsValid(typeForm->typbasetype))
	{
		ObjectAddressSet(referenced, TypeRelationId, typeForm->typbasetype);
		add_exact_object_address(&referenced, addrs_normal);
	}

	/*
	 * Normal dependency from a domain to its collation.  We know the default
	 * collation is pinned, so don't bother recording it.
	 */
	if (OidIsValid(typeForm->typcollation) &&
		typeForm->typcollation != DEFAULT_COLLATION_OID)
	{
		ObjectAddressSet(referenced, CollationRelationId, typeForm->typcollation);
		add_exact_object_address(&referenced, addrs_normal);
	}

	record_object_address_dependencies(&myself, addrs_normal, DEPENDENCY_NORMAL);
	free_object_addresses(addrs_normal);

	/* Normal dependency on the default expression. */
	if (defaultExpr)
		recordDependencyOnExpr(&myself, defaultExpr, NIL, DEPENDENCY_NORMAL);

	/*
	 * If the type is a rowtype for a relation, mark it as internally
	 * dependent on the relation, *unless* it is a stand-alone composite type
	 * relation. For the latter case, we have to reverse the dependency.
	 *
	 * In the former case, this allows the type to be auto-dropped when the
	 * relation is, and not otherwise. And in the latter, of course we get the
	 * opposite effect.
	 */
	if (OidIsValid(typeForm->typrelid))
	{
		ObjectAddressSet(referenced, RelationRelationId, typeForm->typrelid);

		if (relationKind != RELKIND_COMPOSITE_TYPE)
			recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
		else
			recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
	}

	/*
	 * If the type is an implicitly-created array type, mark it as internally
	 * dependent on the element type.  Otherwise, if it has an element type,
	 * the dependency is a normal one.
	 */
	if (OidIsValid(typeForm->typelem))
	{
		ObjectAddressSet(referenced, TypeRelationId, typeForm->typelem);
		recordDependencyOn(&myself, &referenced,
						   isImplicitArray ? DEPENDENCY_INTERNAL : DEPENDENCY_NORMAL);
	}
}

/*
 * RenameTypeInternal
 *		This renames a type, as well as any associated array type.
 *
 * Caller must have already checked privileges.
 *
 * Currently this is used for renaming table rowtypes and for
 * ALTER TYPE RENAME TO command.
 */
void
RenameTypeInternal(Oid typeOid, const char *newTypeName, Oid typeNamespace)
{
	Relation	pg_type_desc;
	HeapTuple	tuple;
	Form_pg_type typ;
	Oid			arrayOid;
	Oid			oldTypeOid;

	pg_type_desc = table_open(TypeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typ = (Form_pg_type) GETSTRUCT(tuple);

	/* We are not supposed to be changing schemas here */
	Assert(typeNamespace == typ->typnamespace);

	arrayOid = typ->typarray;

	/* Check for a conflicting type name. */
	oldTypeOid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								 CStringGetDatum(newTypeName),
								 ObjectIdGetDatum(typeNamespace));

	/*
	 * If there is one, see if it's an autogenerated array type, and if so
	 * rename it out of the way.  (But we must skip that for a shell type
	 * because moveArrayTypeName will do the wrong thing in that case.)
	 * Otherwise, we can at least give a more friendly error than unique-index
	 * violation.
	 */
	if (OidIsValid(oldTypeOid))
	{
		if (get_typisdefined(oldTypeOid) &&
			moveArrayTypeName(oldTypeOid, newTypeName, typeNamespace))
			 /* successfully dodged the problem */ ;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", newTypeName)));
	}

	/* OK, do the rename --- tuple is a copy, so OK to scribble on it */
	namestrcpy(&(typ->typname), newTypeName);

	CatalogTupleUpdate(pg_type_desc, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	heap_freetuple(tuple);
	table_close(pg_type_desc, RowExclusiveLock);

	/*
	 * If the type has an array type, recurse to handle that.  But we don't
	 * need to do anything more if we already renamed that array type above
	 * (which would happen when, eg, renaming "foo" to "_foo").
	 */
	if (OidIsValid(arrayOid) && arrayOid != oldTypeOid)
	{
		char	   *arrname = makeArrayTypeName(newTypeName, typeNamespace);

		RenameTypeInternal(arrayOid, arrname, typeNamespace);
		pfree(arrname);
	}
}


/*
 * makeArrayTypeName
 *	  - given a base type name, make an array type name for it
 *
 * the caller is responsible for pfreeing the result
 */
char *
makeArrayTypeName(const char *typeName, Oid typeNamespace)
{
	char	   *arr = (char *) palloc(NAMEDATALEN);
	int			namelen = strlen(typeName);
	int			i;

	/*
	 * The idea is to prepend underscores as needed until we make a name that
	 * doesn't collide with anything...
	 */
	for (i = 1; i < NAMEDATALEN - 1; i++)
	{
		arr[i - 1] = '_';
		if (i + namelen < NAMEDATALEN)
			strcpy(arr + i, typeName);
		else
		{
			memcpy(arr + i, typeName, NAMEDATALEN - i);
			truncate_identifier(arr, NAMEDATALEN, false);
		}
		if (!SearchSysCacheExists2(TYPENAMENSP,
								   CStringGetDatum(arr),
								   ObjectIdGetDatum(typeNamespace)))
			break;
	}

	if (i >= NAMEDATALEN - 1)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("could not form array type name for type \"%s\"",
						typeName)));

	return arr;
}


/*
 * moveArrayTypeName
 *	  - try to reassign an array type name that the user wants to use.
 *
 * The given type name has been discovered to already exist (with the given
 * OID).  If it is an autogenerated array type, change the array type's name
 * to not conflict.  This allows the user to create type "foo" followed by
 * type "_foo" without problems.  (Of course, there are race conditions if
 * two backends try to create similarly-named types concurrently, but the
 * worst that can happen is an unnecessary failure --- anything we do here
 * will be rolled back if the type creation fails due to conflicting names.)
 *
 * Note that this must be called *before* calling makeArrayTypeName to
 * determine the new type's own array type name; else the latter will
 * certainly pick the same name.
 *
 * Returns true if successfully moved the type, false if not.
 *
 * We also return true if the given type is a shell type.  In this case
 * the type has not been renamed out of the way, but nonetheless it can
 * be expected that TypeCreate will succeed.  This behavior is convenient
 * for most callers --- those that need to distinguish the shell-type case
 * must do their own typisdefined test.
 */
bool
moveArrayTypeName(Oid typeOid, const char *typeName, Oid typeNamespace)
{
	Oid			elemOid;
	char	   *newname;

	/* We need do nothing if it's a shell type. */
	if (!get_typisdefined(typeOid))
		return true;

	/* Can't change it if it's not an autogenerated array type. */
	elemOid = get_element_type(typeOid);
	if (!OidIsValid(elemOid) ||
		get_array_type(elemOid) != typeOid)
		return false;

	/*
	 * OK, use makeArrayTypeName to pick an unused modification of the name.
	 * Note that since makeArrayTypeName is an iterative process, this will
	 * produce a name that it might have produced the first time, had the
	 * conflicting type we are about to create already existed.
	 */
	newname = makeArrayTypeName(typeName, typeNamespace);

	/* Apply the rename */
	RenameTypeInternal(typeOid, newname, typeNamespace);

	/*
	 * We must bump the command counter so that any subsequent use of
	 * makeArrayTypeName sees what we just did and doesn't pick the same name.
	 */
	CommandCounterIncrement();

	pfree(newname);

	return true;
}
