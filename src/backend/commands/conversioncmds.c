/*-------------------------------------------------------------------------
 *
 * conversionmacmds.c
 *	  conversion creation command support code
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/conversioncmds.c,v 1.2 2002/07/25 10:07:11 ishii Exp $
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
	char		*conversion_name;
	AclResult	aclresult;
	int			for_encoding;
	int			to_encoding;
	Oid			funcoid;
	Oid			funcnamespace;
	char		*dummy;

	const char *for_encoding_name = stmt->for_encoding_name;
	const char *to_encoding_name = stmt->to_encoding_name;
	List *func_name = stmt->func_name;

	static Oid funcargs[] = {INT4OID, INT4OID, 0, 0, INT4OID};

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->conversion_name, &conversion_name);

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

	/* Check the existence of the conversion function.
	 * Function name could be a qualified name.
	 */
	funcoid = LookupFuncName(func_name, sizeof(funcargs)/sizeof(Oid), funcargs);
	if (!OidIsValid(funcoid))
		elog(ERROR, "Function %s does not exist", NameListToString(func_name));

	/* Check the rights for this function and name space */
	funcnamespace = QualifiedNameGetCreationNamespace(func_name, &dummy);
	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_namespace_name(funcnamespace));

	aclresult = pg_proc_aclcheck(funcoid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_namespace_name(funcnamespace));
		
	/* All seem ok, go ahead (possible failure would be a duplicate conversion name) */
	ConversionCreate(conversion_name, namespaceId, GetUserId(),
					 for_encoding, to_encoding, funcoid, stmt->def);
}

/*
 * DROP CONVERSION
 */
void
DropConversionCommand(List *name, DropBehavior behavior)
{
	Oid			namespaceId;
	char		*conversion_name;
	AclResult	aclresult;

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameGetCreationNamespace(name, &conversion_name);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_namespace_name(namespaceId));

	/* Go ahead (possible failure would be:
	 * none existing conversion
	 * not ower of this conversion
	 */
	ConversionDrop(conversion_name, namespaceId, GetUserId(), behavior);
}
