/*-------------------------------------------------------------------------
 *
 * pg_namespace.c
 *	  routines to support manipulation of the pg_namespace relation
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_namespace.c,v 1.1 2002/03/22 21:34:44 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_namespace.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/* ----------------
 * NamespaceCreate
 * ---------------
 */
Oid
NamespaceCreate(const char *nspName)
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
		elog(ERROR, "namespace \"%s\" already exists", nspName);

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_namespace; i++)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}
	namestrcpy(&nname, nspName);
	values[Anum_pg_namespace_nspname - 1] = NameGetDatum(&nname);
	values[Anum_pg_namespace_nspowner - 1] = Int32GetDatum(GetUserId());
	nulls[Anum_pg_namespace_nspacl - 1] = 'n';

	nspdesc = heap_openr(NamespaceRelationName, RowExclusiveLock);
	tupDesc = nspdesc->rd_att;
	if (!HeapTupleIsValid(tup = heap_formtuple(tupDesc,
											   values,
											   nulls)))
		elog(ERROR, "NamespaceCreate: heap_formtuple failed");
	nspoid = heap_insert(nspdesc, tup);
	if (!OidIsValid(nspoid))
		elog(ERROR, "NamespaceCreate: heap_insert failed");

	if (RelationGetForm(nspdesc)->relhasindex)
	{
		Relation	idescs[Num_pg_namespace_indices];

		CatalogOpenIndices(Num_pg_namespace_indices, Name_pg_namespace_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_namespace_indices, nspdesc, tup);
		CatalogCloseIndices(Num_pg_namespace_indices, idescs);
	}

	heap_close(nspdesc, RowExclusiveLock);

	return nspoid;
}
