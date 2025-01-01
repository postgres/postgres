/*-------------------------------------------------------------------------
 *
 * conversioncmds.c
 *	  conversion creation command support code
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/conversioncmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_conversion.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/conversioncmds.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"

/*
 * CREATE CONVERSION
 */
ObjectAddress
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
	static const Oid funcargs[] = {INT4OID, INT4OID, CSTRINGOID, INTERNALOID, INT4OID, BOOLOID};
	char		result[1];
	Datum		funcresult;

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->conversion_name,
													&conversion_name);

	/* Check we have creation rights in target namespace */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
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
	 * We consider conversions to or from SQL_ASCII to be meaningless.  (If
	 * you wish to change this, note that pg_do_encoding_conversion() and its
	 * sister functions have hard-wired fast paths for any conversion in which
	 * the source or target encoding is SQL_ASCII, so that an encoding
	 * conversion function declared for such a case will never be used.)
	 */
	if (from_encoding == PG_SQL_ASCII || to_encoding == PG_SQL_ASCII)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding conversion to or from \"SQL_ASCII\" is not supported")));

	/*
	 * Check the existence of the conversion function. Function name could be
	 * a qualified name.
	 */
	funcoid = LookupFuncName(func_name, sizeof(funcargs) / sizeof(Oid),
							 funcargs, false);

	/* Check it returns int4, else it's probably the wrong function */
	if (get_func_rettype(funcoid) != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding conversion function %s must return type %s",
						NameListToString(func_name), "integer")));

	/* Check we have EXECUTE rights for the function */
	aclresult = object_aclcheck(ProcedureRelationId, funcoid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   NameListToString(func_name));

	/*
	 * Check that the conversion function is suitable for the requested source
	 * and target encodings. We do that by calling the function with an empty
	 * string; the conversion function should throw an error if it can't
	 * perform the requested conversion.
	 */
	funcresult = OidFunctionCall6(funcoid,
								  Int32GetDatum(from_encoding),
								  Int32GetDatum(to_encoding),
								  CStringGetDatum(""),
								  CStringGetDatum(result),
								  Int32GetDatum(0),
								  BoolGetDatum(false));

	/*
	 * The function should return 0 for empty input. Might as well check that,
	 * too.
	 */
	if (DatumGetInt32(funcresult) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding conversion function %s returned incorrect result for empty input",
						NameListToString(func_name))));

	/*
	 * All seem ok, go ahead (possible failure would be a duplicate conversion
	 * name)
	 */
	return ConversionCreate(conversion_name, namespaceId, GetUserId(),
							from_encoding, to_encoding, funcoid, stmt->def);
}
