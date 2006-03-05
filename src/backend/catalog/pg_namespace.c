/*-------------------------------------------------------------------------
 *
 * pg_namespace.c
 *	  routines to support manipulation of the pg_namespace relation
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/pg_namespace.c,v 1.16 2006/03/05 15:58:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_namespace.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/* ----------------
 * NamespaceCreate
 * ---------------
 */
Oid
NamespaceCreate(const char *nspName, Oid ownerId)
{
	Relation	nspdesc;
	HeapTuple	tup;
	Oid			nspoid;
	char		nulls[Natts_pg_namespace];
	Datum		values[Natts_pg_namespace];
	NameData	nname;
	TupleDesc	tupDesc;
	int			i;

	/* sanity checks */
	if (!nspName)
		elog(ERROR, "no namespace name supplied");

	/* make sure there is no existing namespace of same name */
	if (SearchSysCacheExists(NAMESPACENAME,
							 PointerGetDatum(nspName),
							 0, 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" already exists", nspName)));

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_namespace; i++)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}
	namestrcpy(&nname, nspName);
	values[Anum_pg_namespace_nspname - 1] = NameGetDatum(&nname);
	values[Anum_pg_namespace_nspowner - 1] = ObjectIdGetDatum(ownerId);
	nulls[Anum_pg_namespace_nspacl - 1] = 'n';

	nspdesc = heap_open(NamespaceRelationId, RowExclusiveLock);
	tupDesc = nspdesc->rd_att;

	tup = heap_formtuple(tupDesc, values, nulls);

	nspoid = simple_heap_insert(nspdesc, tup);
	Assert(OidIsValid(nspoid));

	CatalogUpdateIndexes(nspdesc, tup);

	heap_close(nspdesc, RowExclusiveLock);

	/* Record dependency on owner */
	recordDependencyOnOwner(NamespaceRelationId, nspoid, ownerId);

	return nspoid;
}
