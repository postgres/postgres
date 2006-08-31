/*-------------------------------------------------------------------------
 *
 * pg_conversion.c
 *	  routines to support manipulation of the pg_conversion relation
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/pg_conversion.c,v 1.33 2006/08/31 17:31:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"
#include "utils/acl.h"
#include "miscadmin.h"

/*
 * ConversionCreate
 *
 * Add a new tuple to pg_conversion.
 */
Oid
ConversionCreate(const char *conname, Oid connamespace,
				 Oid conowner,
				 int32 conforencoding, int32 contoencoding,
				 Oid conproc, bool def)
{
	int			i;
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	char		nulls[Natts_pg_conversion];
	Datum		values[Natts_pg_conversion];
	NameData	cname;
	Oid			oid;
	ObjectAddress myself,
				referenced;

	/* sanity checks */
	if (!conname)
		elog(ERROR, "no conversion name supplied");

	/* make sure there is no existing conversion of same name */
	if (SearchSysCacheExists(CONNAMENSP,
							 PointerGetDatum(conname),
							 ObjectIdGetDatum(connamespace),
							 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("conversion \"%s\" already exists", conname)));

	if (def)
	{
		/*
		 * make sure there is no existing default <for encoding><to encoding>
		 * pair in this name space
		 */
		if (FindDefaultConversion(connamespace,
								  conforencoding,
								  contoencoding))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("default conversion for %s to %s already exists",
							pg_encoding_to_char(conforencoding),
							pg_encoding_to_char(contoencoding))));
	}

	/* open pg_conversion */
	rel = heap_open(ConversionRelationId, RowExclusiveLock);
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
	values[Anum_pg_conversion_conowner - 1] = ObjectIdGetDatum(conowner);
	values[Anum_pg_conversion_conforencoding - 1] = Int32GetDatum(conforencoding);
	values[Anum_pg_conversion_contoencoding - 1] = Int32GetDatum(contoencoding);
	values[Anum_pg_conversion_conproc - 1] = ObjectIdGetDatum(conproc);
	values[Anum_pg_conversion_condefault - 1] = BoolGetDatum(def);

	tup = heap_formtuple(tupDesc, values, nulls);

	/* insert a new tuple */
	oid = simple_heap_insert(rel, tup);
	Assert(OidIsValid(oid));

	/* update the index if any */
	CatalogUpdateIndexes(rel, tup);

	myself.classId = ConversionRelationId;
	myself.objectId = HeapTupleGetOid(tup);
	myself.objectSubId = 0;

	/* create dependency on conversion procedure */
	referenced.classId = ProcedureRelationId;
	referenced.objectId = conproc;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* create dependency on namespace */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = connamespace;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* create dependency on owner */
	recordDependencyOnOwner(ConversionRelationId, HeapTupleGetOid(tup),
							conowner);

	heap_freetuple(tup);
	heap_close(rel, RowExclusiveLock);

	return oid;
}

/*
 * ConversionDrop
 *
 * Drop a conversion after doing permission checks.
 */
void
ConversionDrop(Oid conversionOid, DropBehavior behavior)
{
	HeapTuple	tuple;
	ObjectAddress object;

	tuple = SearchSysCache(CONOID,
						   ObjectIdGetDatum(conversionOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for conversion %u", conversionOid);

	if (!superuser() &&
		((Form_pg_conversion) GETSTRUCT(tuple))->conowner != GetUserId())
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CONVERSION,
				  NameStr(((Form_pg_conversion) GETSTRUCT(tuple))->conname));

	ReleaseSysCache(tuple);

	/*
	 * Do the deletion
	 */
	object.classId = ConversionRelationId;
	object.objectId = conversionOid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}

/*
 * RemoveConversionById
 *
 * Remove a tuple from pg_conversion by Oid. This function is solely
 * called inside catalog/dependency.c
 */
void
RemoveConversionById(Oid conversionOid)
{
	Relation	rel;
	HeapTuple	tuple;
	HeapScanDesc scan;
	ScanKeyData scanKeyData;

	ScanKeyInit(&scanKeyData,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conversionOid));

	/* open pg_conversion */
	rel = heap_open(ConversionRelationId, RowExclusiveLock);

	scan = heap_beginscan(rel, SnapshotNow,
						  1, &scanKeyData);

	/* search for the target tuple */
	if (HeapTupleIsValid(tuple = heap_getnext(scan, ForwardScanDirection)))
		simple_heap_delete(rel, &tuple->t_self);
	else
		elog(ERROR, "could not find tuple for conversion %u", conversionOid);
	heap_endscan(scan);
	heap_close(rel, RowExclusiveLock);
}

/*
 * FindDefaultConversion
 *
 * Find "default" conversion proc by for_encoding and to_encoding in the
 * given namespace.
 *
 * If found, returns the procedure's oid, otherwise InvalidOid.  Note that
 * you get the procedure's OID not the conversion's OID!
 */
Oid
FindDefaultConversion(Oid name_space, int32 for_encoding, int32 to_encoding)
{
	CatCList   *catlist;
	HeapTuple	tuple;
	Form_pg_conversion body;
	Oid			proc = InvalidOid;
	int			i;

	catlist = SearchSysCacheList(CONDEFAULT, 3,
								 ObjectIdGetDatum(name_space),
								 Int32GetDatum(for_encoding),
								 Int32GetDatum(to_encoding),
								 0);

	for (i = 0; i < catlist->n_members; i++)
	{
		tuple = &catlist->members[i]->tuple;
		body = (Form_pg_conversion) GETSTRUCT(tuple);
		if (body->condefault)
		{
			proc = body->conproc;
			break;
		}
	}
	ReleaseSysCacheList(catlist);
	return proc;
}

/*
 * FindConversion
 *
 * Find conversion by namespace and conversion name.
 * Returns conversion OID.
 */
Oid
FindConversion(const char *conname, Oid connamespace)
{
	HeapTuple	tuple;
	Oid			procoid;
	Oid			conoid;
	AclResult	aclresult;

	/* search pg_conversion by connamespace and conversion name */
	tuple = SearchSysCache(CONNAMENSP,
						   PointerGetDatum(conname),
						   ObjectIdGetDatum(connamespace),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
		return InvalidOid;

	procoid = ((Form_pg_conversion) GETSTRUCT(tuple))->conproc;
	conoid = HeapTupleGetOid(tuple);

	ReleaseSysCache(tuple);

	/* Check we have execute rights for the function */
	aclresult = pg_proc_aclcheck(procoid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		return InvalidOid;

	return conoid;
}

/*
 * Execute SQL99's CONVERT function.
 *
 * CONVERT <left paren> <character value expression>
 * USING <form-of-use conversion name> <right paren>
 *
 * TEXT convert_using(TEXT string, TEXT conversion_name)
 */
Datum
pg_convert_using(PG_FUNCTION_ARGS)
{
	text	   *string = PG_GETARG_TEXT_P(0);
	text	   *conv_name = PG_GETARG_TEXT_P(1);
	text	   *retval;
	List	   *parsed_name;
	Oid			convoid;
	HeapTuple	tuple;
	Form_pg_conversion body;
	char	   *str;
	char	   *result;
	int			len;

	/* Convert input string to null-terminated form */
	len = VARSIZE(string) - VARHDRSZ;
	str = palloc(len + 1);
	memcpy(str, VARDATA(string), len);
	*(str + len) = '\0';

	/* Look up the conversion name */
	parsed_name = textToQualifiedNameList(conv_name);
	convoid = FindConversionByName(parsed_name);
	if (!OidIsValid(convoid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("conversion \"%s\" does not exist",
						NameListToString(parsed_name))));

	tuple = SearchSysCache(CONOID,
						   ObjectIdGetDatum(convoid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for conversion %u", convoid);
	body = (Form_pg_conversion) GETSTRUCT(tuple);

	/* Temporary result area should be more than big enough */
	result = palloc(len * 4 + 1);

	OidFunctionCall5(body->conproc,
					 Int32GetDatum(body->conforencoding),
					 Int32GetDatum(body->contoencoding),
					 CStringGetDatum(str),
					 CStringGetDatum(result),
					 Int32GetDatum(len));

	ReleaseSysCache(tuple);

	/*
	 * build text result structure. we cannot use textin() here, since textin
	 * assumes that input string encoding is same as database encoding.
	 */
	len = strlen(result) + VARHDRSZ;
	retval = palloc(len);
	VARATT_SIZEP(retval) = len;
	memcpy(VARDATA(retval), result, len - VARHDRSZ);

	pfree(result);
	pfree(str);

	PG_RETURN_TEXT_P(retval);
}
