/*-------------------------------------------------------------------------
 *
 * conversioncmds.c
 *	  conversion creation command support code
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/conversioncmds.c,v 1.5 2002/11/02 02:33:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_conversion.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "commands/conversioncmds.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"


/*
 * CREATE CONVERSION
 */
void
CreateConversionCommand(CreateConversionStmt *stmt)
{
	Oid			namespaceId;
	char	   *conversion_name;
	AclResult	aclresult;
	int			for_encoding;
	int			to_encoding;
	Oid			funcoid;
	const char *for_encoding_name = stmt->for_encoding_name;
	const char *to_encoding_name = stmt->to_encoding_name;
	List	   *func_name = stmt->func_name;
	static Oid	funcargs[] = {INT4OID, INT4OID, CSTRINGOID, CSTRINGOID, INT4OID};

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->conversion_name,
													&conversion_name);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_namespace_name(namespaceId));

	/* Check the encoding names */
	for_encoding = pg_char_to_encoding(for_encoding_name);
	if (for_encoding < 0)
		elog(ERROR, "Invalid for encoding name: %s", for_encoding_name);

	to_encoding = pg_char_to_encoding(to_encoding_name);
	if (to_encoding < 0)
		elog(ERROR, "Invalid to encoding name: %s", to_encoding_name);

	/*
	 * Check the existence of the conversion function. Function name could
	 * be a qualified name.
	 */
	funcoid = LookupFuncName(func_name, sizeof(funcargs) / sizeof(Oid), funcargs);
	if (!OidIsValid(funcoid))
		func_error("CreateConversion", func_name,
				   sizeof(funcargs) / sizeof(Oid), funcargs, NULL);

	/* Check we have EXECUTE rights for the function */
	aclresult = pg_proc_aclcheck(funcoid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, NameListToString(func_name));

	/*
	 * All seem ok, go ahead (possible failure would be a duplicate
	 * conversion name)
	 */
	ConversionCreate(conversion_name, namespaceId, GetUserId(),
					 for_encoding, to_encoding, funcoid, stmt->def);
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
		elog(ERROR, "conversion %s not found", NameListToString(name));

	ConversionDrop(conversionOid, behavior);
}
