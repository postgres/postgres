/*-------------------------------------------------------------------------
 *
 * pg_parameter_acl.c
 *	  routines to support manipulation of the pg_parameter_acl relation
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_parameter_acl.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_parameter_acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * ParameterAclLookup - Given a configuration parameter name,
 * look up the associated configuration parameter ACL's OID.
 *
 * If missing_ok is false, throw an error if ACL entry not found.  If
 * true, just return InvalidOid.
 */
Oid
ParameterAclLookup(const char *parameter, bool missing_ok)
{
	Oid			oid;
	char	   *parname;

	/* Convert name to the form it should have in pg_parameter_acl... */
	parname = convert_GUC_name_for_parameter_acl(parameter);

	/* ... and look it up */
	oid = GetSysCacheOid1(PARAMETERACLNAME, Anum_pg_parameter_acl_oid,
						  PointerGetDatum(cstring_to_text(parname)));

	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("parameter ACL \"%s\" does not exist", parameter)));

	pfree(parname);

	return oid;
}

/*
 * ParameterAclCreate
 *
 * Add a new tuple to pg_parameter_acl.
 *
 * parameter: the parameter name to create an entry for.
 * Caller should have verified that there's no such entry already.
 *
 * Returns the new entry's OID.
 */
Oid
ParameterAclCreate(const char *parameter)
{
	Oid			parameterId;
	char	   *parname;
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tuple;
	Datum		values[Natts_pg_parameter_acl] = {0};
	bool		nulls[Natts_pg_parameter_acl] = {0};

	/*
	 * To prevent cluttering pg_parameter_acl with useless entries, insist
	 * that the name be valid.
	 */
	check_GUC_name_for_parameter_acl(parameter);

	/* Convert name to the form it should have in pg_parameter_acl. */
	parname = convert_GUC_name_for_parameter_acl(parameter);

	/*
	 * Create and insert a new record containing a null ACL.
	 *
	 * We don't take a strong enough lock to prevent concurrent insertions,
	 * relying instead on the unique index.
	 */
	rel = table_open(ParameterAclRelationId, RowExclusiveLock);
	tupDesc = RelationGetDescr(rel);
	parameterId = GetNewOidWithIndex(rel,
									 ParameterAclOidIndexId,
									 Anum_pg_parameter_acl_oid);
	values[Anum_pg_parameter_acl_oid - 1] = ObjectIdGetDatum(parameterId);
	values[Anum_pg_parameter_acl_parname - 1] =
		PointerGetDatum(cstring_to_text(parname));
	nulls[Anum_pg_parameter_acl_paracl - 1] = true;
	tuple = heap_form_tuple(tupDesc, values, nulls);
	CatalogTupleInsert(rel, tuple);

	/* Close pg_parameter_acl, but keep lock till commit. */
	heap_freetuple(tuple);
	table_close(rel, NoLock);

	return parameterId;
}
