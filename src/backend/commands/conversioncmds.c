/*-------------------------------------------------------------------------
 *
 * conversioncmds.c
 *	  conversion creation command support code
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/conversioncmds.c,v 1.29 2006/07/14 14:52:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_type.h"
#include "commands/conversioncmds.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static void AlterConversionOwner_internal(Relation rel, Oid conversionOid,
							  Oid newOwnerId);

/*
 * CREATE CONVERSION
 */
void
CreateConversionCommand(CreateConversionStmt *stmt)
{
	Oid			namespaceId;
	char	   *conversion_name;
	AclResult	aclresult;
	int			from_encoding;
	int			to_encoding;
	Oid			funcoid;
	const char *from_encoding_name = stmt->for_encoding_name;
	const char *to_encoding_name = stmt->to_encoding_name;
	List	   *func_name = stmt->func_name;
	static Oid	funcargs[] = {INT4OID, INT4OID, CSTRINGOID, INTERNALOID, INT4OID};

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->conversion_name,
													&conversion_name);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceId));

	/* Check the encoding names */
	from_encoding = pg_char_to_encoding(from_encoding_name);
	if (from_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("source encoding \"%s\" does not exist",
						from_encoding_name)));

	to_encoding = pg_char_to_encoding(to_encoding_name);
	if (to_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("destination encoding \"%s\" does not exist",
						to_encoding_name)));

	/*
	 * Check the existence of the conversion function. Function name could be
	 * a qualified name.
	 */
	funcoid = LookupFuncName(func_name, sizeof(funcargs) / sizeof(Oid),
							 funcargs, false);

	/* Check we have EXECUTE rights for the function */
	aclresult = pg_proc_aclcheck(funcoid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_PROC,
					   NameListToString(func_name));

	/*
	 * All seem ok, go ahead (possible failure would be a duplicate conversion
	 * name)
	 */
	ConversionCreate(conversion_name, namespaceId, GetUserId(),
					 from_encoding, to_encoding, funcoid, stmt->def);
}

/*
 * DROP CONVERSION
 */
void
DropConversionCommand(List *name, DropBehavior behavior, bool missing_ok)
{
	Oid			conversionOid;

	conversionOid = FindConversionByName(name);
	if (!OidIsValid(conversionOid))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("conversion \"%s\" does not exist",
							NameListToString(name))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("conversion \"%s\" does not exist, skipping",
							NameListToString(name))));
		}

		return;
	}

	ConversionDrop(conversionOid, behavior);
}

/*
 * Rename conversion
 */
void
RenameConversion(List *name, const char *newname)
{
	Oid			conversionOid;
	Oid			namespaceOid;
	HeapTuple	tup;
	Relation	rel;
	AclResult	aclresult;

	rel = heap_open(ConversionRelationId, RowExclusiveLock);

	conversionOid = FindConversionByName(name);
	if (!OidIsValid(conversionOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("conversion \"%s\" does not exist",
						NameListToString(name))));

	tup = SearchSysCacheCopy(CONOID,
							 ObjectIdGetDatum(conversionOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for conversion %u", conversionOid);

	namespaceOid = ((Form_pg_conversion) GETSTRUCT(tup))->connamespace;

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists(CONNAMENSP,
							 CStringGetDatum(newname),
							 ObjectIdGetDatum(namespaceOid),
							 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("conversion \"%s\" already exists in schema \"%s\"",
						newname, get_namespace_name(namespaceOid))));

	/* must be owner */
	if (!pg_conversion_ownercheck(conversionOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CONVERSION,
					   NameListToString(name));

	/* must have CREATE privilege on namespace */
	aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceOid));

	/* rename */
	namestrcpy(&(((Form_pg_conversion) GETSTRUCT(tup))->conname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/*
 * Change conversion owner, by name
 */
void
AlterConversionOwner(List *name, Oid newOwnerId)
{
	Oid			conversionOid;
	Relation	rel;

	rel = heap_open(ConversionRelationId, RowExclusiveLock);

	conversionOid = FindConversionByName(name);
	if (!OidIsValid(conversionOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("conversion \"%s\" does not exist",
						NameListToString(name))));

	AlterConversionOwner_internal(rel, conversionOid, newOwnerId);

	heap_close(rel, NoLock);
}

/*
 * Change conversion owner, by oid
 */
void
AlterConversionOwner_oid(Oid conversionOid, Oid newOwnerId)
{
	Relation	rel;

	rel = heap_open(ConversionRelationId, RowExclusiveLock);

	AlterConversionOwner_internal(rel, conversionOid, newOwnerId);

	heap_close(rel, NoLock);
}

/*
 * AlterConversionOwner_internal
 *
 * Internal routine for changing the owner.  rel must be pg_conversion, already
 * open and suitably locked; it will not be closed.
 */
static void
AlterConversionOwner_internal(Relation rel, Oid conversionOid, Oid newOwnerId)
{
	Form_pg_conversion convForm;
	HeapTuple	tup;

	Assert(RelationGetRelid(rel) == ConversionRelationId);

	tup = SearchSysCacheCopy(CONOID,
							 ObjectIdGetDatum(conversionOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for conversion %u", conversionOid);

	convForm = (Form_pg_conversion) GETSTRUCT(tup);

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (convForm->conowner != newOwnerId)
	{
		AclResult	aclresult;

		/* Superusers can always do it */
		if (!superuser())
		{
			/* Otherwise, must be owner of the existing object */
			if (!pg_conversion_ownercheck(HeapTupleGetOid(tup), GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CONVERSION,
							   NameStr(convForm->conname));

			/* Must be able to become new owner */
			check_is_member_of_role(GetUserId(), newOwnerId);

			/* New owner must have CREATE privilege on namespace */
			aclresult = pg_namespace_aclcheck(convForm->connamespace,
											  newOwnerId,
											  ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
							   get_namespace_name(convForm->connamespace));
		}

		/*
		 * Modify the owner --- okay to scribble on tup because it's a copy
		 */
		convForm->conowner = newOwnerId;

		simple_heap_update(rel, &tup->t_self, tup);

		CatalogUpdateIndexes(rel, tup);

		/* Update owner dependency reference */
		changeDependencyOnOwner(ConversionRelationId, conversionOid,
								newOwnerId);
	}

	heap_freetuple(tup);
}
