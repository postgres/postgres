/*-------------------------------------------------------------------------
 *
 * conversioncmds.c
 *	  conversion creation command support code
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/conversioncmds.c,v 1.11.4.1 2005/05/03 19:18:31 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_conversion.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "commands/conversioncmds.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


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
	 * Check the existence of the conversion function. Function name could
	 * be a qualified name.
	 */
	funcoid = LookupFuncName(func_name, sizeof(funcargs) / sizeof(Oid),
							 funcargs, false);

	/* Check we have EXECUTE rights for the function */
	aclresult = pg_proc_aclcheck(funcoid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_PROC,
					   NameListToString(func_name));

	/*
	 * All seem ok, go ahead (possible failure would be a duplicate
	 * conversion name)
	 */
	ConversionCreate(conversion_name, namespaceId, GetUserId(),
					 from_encoding, to_encoding, funcoid, stmt->def);
}

/*
 * DROP CONVERSION
 */
void
DropConversionCommand(List *name, DropBehavior behavior)
{
	Oid			conversionOid;

	conversionOid = FindConversionByName(name);
	if (!OidIsValid(conversionOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("conversion \"%s\" does not exist",
						NameListToString(name))));

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

	rel = heap_openr(ConversionRelationName, RowExclusiveLock);

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
	if (!superuser() &&
		((Form_pg_conversion) GETSTRUCT(tup))->conowner != GetUserId())
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
