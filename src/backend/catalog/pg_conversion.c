/*-------------------------------------------------------------------------
 *
 * pg_conversion.c
 *	  routines to support manipulation of the pg_conversion relation
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_conversion.c,v 1.1 2002/07/11 07:39:27 ishii Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_conversion.h"
#include "catalog/namespace.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "mb/pg_wchar.h"
#include "utils/fmgroids.h"
#include "utils/acl.h"
#include "miscadmin.h"

/* ----------------
 * ConversionCreate
 * ---------------
 */
Oid	ConversionCreate(const char *conname, Oid connamespace,
							 int32 conowner,
							 int4 conforencoding, int4 contoencoding,
							 Oid conproc, bool def)
{
	int i;
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	char		nulls[Natts_pg_conversion];
	Datum		values[Natts_pg_conversion];
	NameData	cname;
	Oid			oid;

	/* sanity checks */
	if (!conname)
		elog(ERROR, "no conversion name supplied");

	/* make sure there is no existing conversion of same name */
	if (SearchSysCacheExists(CONNAMESP,
							PointerGetDatum(conname),
							ObjectIdGetDatum(connamespace),
							 0,0))
		elog(ERROR, "conversion name \"%s\" already exists", conname);

	if (def)
	{
		/* make sure there is no existing default
		   <for encoding><to encoding> pair in this name space */
		if (FindDefaultConversion(connamespace,
								  conforencoding,
								  contoencoding))
			elog(ERROR, "default conversion for %s to %s already exists",
				 pg_encoding_to_char(conforencoding),pg_encoding_to_char(contoencoding));
	}

	/* open pg_conversion */
	rel = heap_openr(ConversionRelationName, RowExclusiveLock);
	tupDesc = rel->rd_att;

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_conversion; i++)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}

	/* form a tuple */
	namestrcpy(&cname, conname);
	values[Anum_pg_conversion_conname - 1] = NameGetDatum(&cname);
	values[Anum_pg_conversion_connamespace - 1] = ObjectIdGetDatum(connamespace);
	values[Anum_pg_conversion_conowner - 1] = Int32GetDatum(conowner);
	values[Anum_pg_conversion_conforencoding - 1] = Int32GetDatum(conforencoding);
	values[Anum_pg_conversion_contoencoding - 1] = Int32GetDatum(contoencoding);
	values[Anum_pg_conversion_conproc - 1] = ObjectIdGetDatum(conproc);
	values[Anum_pg_conversion_condefault - 1] = BoolGetDatum(def);

	tup = heap_formtuple(tupDesc, values, nulls);

	/* insert a new tuple */
	oid = simple_heap_insert(rel, tup);
	Assert(OidIsValid(oid));

	/* update the index if any */
	if (RelationGetForm(rel)->relhasindex)
	{
		Relation	idescs[Num_pg_conversion_indices];

		CatalogOpenIndices(Num_pg_conversion_indices, Name_pg_conversion_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_conversion_indices, rel, tup);
		CatalogCloseIndices(Num_pg_conversion_indices, idescs);
	}

	heap_close(rel, RowExclusiveLock);

	return oid;
}

/* ----------------
 * ConversionDrop
 * ---------------
 */
void	ConversionDrop(const char *conname, Oid connamespace, int32 conowner)
{
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tuple;
	HeapScanDesc scan;
	ScanKeyData scanKeyData;
	Form_pg_conversion body;

	/* sanity checks */
	if (!conname)
		elog(ERROR, "no conversion name supplied");

	ScanKeyEntryInitialize(&scanKeyData,
						   0,
						   Anum_pg_conversion_connamespace,
						   F_OIDEQ,
						   ObjectIdGetDatum(connamespace));

	/* open pg_conversion */
	rel = heap_openr(ConversionRelationName, RowExclusiveLock);
	tupDesc = rel->rd_att;

	scan = heap_beginscan(rel, SnapshotNow,
							  1, &scanKeyData);

	/* search for the target tuple */
	while (HeapTupleIsValid(tuple = heap_getnext(scan, ForwardScanDirection)))
	{
		body = (Form_pg_conversion)GETSTRUCT(tuple);
		if (!strncmp(NameStr(body->conname), conname, NAMEDATALEN))
			break;
	}

	if (!HeapTupleIsValid(tuple))
	{
		elog(ERROR, "conversion %s not found", conname);
		return;
	}

	if (!superuser() && ((Form_pg_conversion)GETSTRUCT(tuple))->conowner != GetUserId())
		elog(ERROR, "permission denied");

	simple_heap_delete(rel, &tuple->t_self);

	heap_endscan(scan);
	heap_close(rel, RowExclusiveLock);
}

/* ----------------
 * FindDefaultConversion
 *
 * find default conversion proc by for_encoding and to_encoding in this name space
 * ---------------
 */
Oid FindDefaultConversion(Oid name_space, int4 for_encoding, int4 to_encoding)
{
	Relation rel;
	HeapScanDesc scan;
	ScanKeyData scanKeyData;
	HeapTuple	tuple;
	Form_pg_conversion body;
	Oid proc = InvalidOid;

	/* Check we have usage rights in target namespace */
	if (pg_namespace_aclcheck(name_space, GetUserId(), ACL_USAGE) != ACLCHECK_OK)
		return InvalidOid;

	ScanKeyEntryInitialize(&scanKeyData,
						   0,
						   Anum_pg_conversion_connamespace,
						   F_OIDEQ,
						   ObjectIdGetDatum(name_space));

	rel = heap_openr(ConversionRelationName, AccessShareLock);
	scan = heap_beginscan(rel, SnapshotNow,
							  1, &scanKeyData);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, ForwardScanDirection)))
	{
		body = (Form_pg_conversion)GETSTRUCT(tuple);
		if (body->conforencoding == for_encoding &&
			body->conforencoding == to_encoding &&
			body->condefault == TRUE) {
			proc = body->conproc;
			break;
		}
	}
	heap_endscan(scan);
	heap_close(rel, AccessShareLock);
	return proc;
}

/* ----------------
 * FindConversionByName
 *
 * find conversion proc by possibly qualified conversion name.
 * ---------------
 */
Oid FindConversionByName(List *name)
{
	HeapTuple	tuple;
	char		*conversion_name;
	Oid	namespaceId;
	Oid procoid;
	AclResult	aclresult;

	/* Convert list of names to a name and namespace */
	namespaceId = QualifiedNameGetCreationNamespace(name, &conversion_name);

	/* Check we have usage rights in target namespace */
	if (pg_namespace_aclcheck(namespaceId, GetUserId(), ACL_USAGE) != ACLCHECK_OK)
		return InvalidOid;

	/* search pg_conversion by namespaceId and conversion name */
	tuple = SearchSysCache(CONNAMESP,
						   PointerGetDatum(conversion_name),
						   ObjectIdGetDatum(namespaceId),
						   0,0);

	if (!HeapTupleIsValid(tuple))
		return InvalidOid;

	procoid = ((Form_pg_conversion)GETSTRUCT(tuple))->conproc;

	ReleaseSysCache(tuple);

	/* Check we have execute rights for the function */
	aclresult = pg_proc_aclcheck(procoid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		return InvalidOid;

	return procoid;
}

