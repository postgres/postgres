/*-------------------------------------------------------------------------
 *
 * namespace.c
 *	  code to support accessing and searching namespaces
 *
 * This is separate from pg_namespace.c, which contains the routines that
 * directly manipulate the pg_namespace system catalog.  This module
 * provides routines associated with defining a "namespace search path"
 * and implementing search-path-controlled searches.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/namespace.c,v 1.1 2002/03/26 19:15:32 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * RangeVarGetRelid
 *		Given a RangeVar describing an existing relation,
 *		select the proper namespace and look up the relation OID.
 *
 * If the relation is not found, return InvalidOid if failOK = true,
 * otherwise raise an error.
 */
Oid
RangeVarGetRelid(const RangeVar *relation, bool failOK)
{
	Oid			namespaceId;
	Oid			relId;

	/*
	 * We check the catalog name and then ignore it.
	 */
	if (relation->catalogname)
	{
		if (strcmp(relation->catalogname, DatabaseName) != 0)
			elog(ERROR, "Cross-database references are not implemented");
	}

	if (relation->schemaname)
	{
		namespaceId = GetSysCacheOid(NAMESPACENAME,
									 CStringGetDatum(relation->schemaname),
									 0, 0, 0);
		if (!OidIsValid(namespaceId))
			elog(ERROR, "Namespace \"%s\" does not exist",
				 relation->schemaname);
		relId = get_relname_relid(relation->relname, namespaceId);
	}
	else
	{
		relId = RelnameGetRelid(relation->relname);
	}

	if (!OidIsValid(relId) && !failOK)
	{
		if (relation->schemaname)
			elog(ERROR, "Relation \"%s\".\"%s\" does not exist",
				 relation->schemaname, relation->relname);
		else
			elog(ERROR, "Relation \"%s\" does not exist",
				 relation->relname);
	}
	return relId;
}

/*
 * RangeVarGetCreationNamespace
 *		Given a RangeVar describing a to-be-created relation,
 *		choose which namespace to create it in.
 */
Oid
RangeVarGetCreationNamespace(const RangeVar *newRelation)
{
	Oid			namespaceId;

	/*
	 * We check the catalog name and then ignore it.
	 */
	if (newRelation->catalogname)
	{
		if (strcmp(newRelation->catalogname, DatabaseName) != 0)
			elog(ERROR, "Cross-database references are not implemented");
	}

	if (newRelation->schemaname)
	{
		namespaceId = GetSysCacheOid(NAMESPACENAME,
									 CStringGetDatum(newRelation->schemaname),
									 0, 0, 0);
		if (!OidIsValid(namespaceId))
			elog(ERROR, "Namespace \"%s\" does not exist",
				 newRelation->schemaname);
	}
	else
	{
		/* XXX Wrong!  Need to get a default schema from somewhere */
		namespaceId = PG_CATALOG_NAMESPACE;
	}

	return namespaceId;
}

/*
 * RelnameGetRelid
 *		Try to resolve an unqualified relation name.
 *		Returns OID if relation found in search path, else InvalidOid.
 */
Oid
RelnameGetRelid(const char *relname)
{
	/* XXX Wrong!  must search search path */
	return get_relname_relid(relname, PG_CATALOG_NAMESPACE);
}
