/*-------------------------------------------------------------------------
 *
 * indexing.c--
 *	  This file contains routines to support indices defined on system
 *	  catalogs.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/indexing.c,v 1.20 1998/08/19 02:01:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

/*
 * Names of indices on the following system catalogs:
 *
 *		pg_attribute
 *		pg_proc
 *		pg_type
 *		pg_naming
 *		pg_class
 *		pg_attrdef
 *		pg_relcheck
 *		pg_trigger
 */

char	   *Name_pg_attr_indices[Num_pg_attr_indices] = {AttributeNameIndex,
	AttributeNumIndex,
AttributeRelidIndex};
char	   *Name_pg_proc_indices[Num_pg_proc_indices] = {ProcedureNameIndex,
	ProcedureOidIndex,
ProcedureSrcIndex};
char	   *Name_pg_type_indices[Num_pg_type_indices] = {TypeNameIndex,
TypeOidIndex};
char	   *Name_pg_class_indices[Num_pg_class_indices] = {ClassNameIndex,
ClassOidIndex};
char	   *Name_pg_attrdef_indices[Num_pg_attrdef_indices] = {AttrDefaultIndex};

char	   *Name_pg_relcheck_indices[Num_pg_relcheck_indices] = {RelCheckIndex};

char	   *Name_pg_trigger_indices[Num_pg_trigger_indices] = {TriggerRelidIndex};


static HeapTuple CatalogIndexFetchTuple(Relation heapRelation,
					   Relation idesc,
					   ScanKey skey,
					   int16 num_keys);


/*
 * Changes (appends) to catalogs can (and does) happen at various places
 * throughout the code.  We need a generic routine that will open all of
 * the indices defined on a given catalog a return the relation descriptors
 * associated with them.
 */
void
CatalogOpenIndices(int nIndices, char *names[], Relation idescs[])
{
	int			i;

	for (i = 0; i < nIndices; i++)
		idescs[i] = index_openr(names[i]);
}

/*
 * This is the inverse routine to CatalogOpenIndices()
 */
void
CatalogCloseIndices(int nIndices, Relation *idescs)
{
	int			i;

	for (i = 0; i < nIndices; i++)
		index_close(idescs[i]);
}


/*
 * For the same reasons outlined above CatalogOpenIndices() we need a routine
 * that takes a new catalog tuple and inserts an associated index tuple into
 * each catalog index.
 */
void
CatalogIndexInsert(Relation *idescs,
				   int nIndices,
				   Relation heapRelation,
				   HeapTuple heapTuple)
{
	HeapTuple	pgIndexTup;
	TupleDesc	heapDescriptor;
	IndexTupleForm pgIndexP;
	Datum		datum;
	int			natts;
	AttrNumber *attnumP;
	FuncIndexInfo finfo,
			   *finfoP;
	char		nulls[INDEX_MAX_KEYS];
	int			i;

	heapDescriptor = RelationGetTupleDescriptor(heapRelation);

	for (i = 0; i < nIndices; i++)
	{
		TupleDesc	indexDescriptor;
		InsertIndexResult indexRes;

		indexDescriptor = RelationGetTupleDescriptor(idescs[i]);
		pgIndexTup = SearchSysCacheTuple(INDEXRELID,
										 ObjectIdGetDatum(idescs[i]->rd_id),
										 0, 0, 0);
		Assert(pgIndexTup);
		pgIndexP = (IndexTupleForm) GETSTRUCT(pgIndexTup);

		/*
		 * Compute the number of attributes we are indexing upon. very
		 * important - can't assume one if this is a functional index.
		 */
		for (attnumP = (&pgIndexP->indkey[0]), natts = 0;
			 *attnumP != InvalidAttrNumber;
			 attnumP++, natts++)
			;

		if (pgIndexP->indproc != InvalidOid)
		{
			FIgetnArgs(&finfo) = natts;
			natts = 1;
			FIgetProcOid(&finfo) = pgIndexP->indproc;
			*(FIgetname(&finfo)) = '\0';
			finfoP = &finfo;
		}
		else
			finfoP = (FuncIndexInfo *) NULL;

		FormIndexDatum(natts,
					   (AttrNumber *) &pgIndexP->indkey[0],
					   heapTuple,
					   heapDescriptor,
					   &datum,
					   nulls,
					   finfoP);

		indexRes = index_insert(idescs[i], &datum, nulls,
								&heapTuple->t_ctid, heapRelation);
		if (indexRes)
			pfree(indexRes);
	}
}

/*
 * This is needed at initialization when reldescs for some of the crucial
 * system catalogs are created and nailed into the cache.
 */
bool
CatalogHasIndex(char *catName, Oid catId)
{
	Relation	pg_class;
	HeapTuple	htup;
	Form_pg_class pgRelP;
	int			i;

	Assert(IsSystemRelationName(catName));

	/*
	 * If we're bootstraping we don't have pg_class (or any indices).
	 */
	if (IsBootstrapProcessingMode())
		return false;

	if (IsInitProcessingMode())
	{
		for (i = 0; IndexedCatalogNames[i] != NULL; i++)
		{
			if (strcmp(IndexedCatalogNames[i], catName) == 0)
				return (true);
		}
		return (false);
	}

	pg_class = heap_openr(RelationRelationName);
	htup = ClassOidIndexScan(pg_class, catId);
	heap_close(pg_class);

	if (!HeapTupleIsValid(htup))
	{
		elog(NOTICE, "CatalogHasIndex: no relation with oid %d", catId);
		return false;
	}

	pgRelP = (Form_pg_class) GETSTRUCT(htup);
	return (pgRelP->relhasindex);
}

/*
 *	CatalogIndexFetchTuple() -- Get a tuple that satisfies a scan key
 *								from a catalog relation.
 *
 *		Since the index may contain pointers to dead tuples, we need to
 *		iterate until we find a tuple that's valid and satisfies the scan
 *		key.
 */
static HeapTuple
CatalogIndexFetchTuple(Relation heapRelation,
					   Relation idesc,
					   ScanKey skey,
					   int16 num_keys)
{
	IndexScanDesc sd;
	RetrieveIndexResult indexRes;
	HeapTuple	tuple;
	Buffer		buffer;
	
	sd = index_beginscan(idesc, false, num_keys, skey);
	tuple = (HeapTuple) NULL;
	do
	{
		indexRes = index_getnext(sd, ForwardScanDirection);
		if (indexRes)
		{
			ItemPointer iptr;

			iptr = &indexRes->heap_iptr;
			tuple = heap_fetch(heapRelation, SnapshotNow, iptr, &buffer);
			pfree(indexRes);
		}
		else
			break;
	} while (!HeapTupleIsValid(tuple));

	if (HeapTupleIsValid(tuple))
		tuple = heap_copytuple(tuple);

	if (BufferIsValid(buffer))
		ReleaseBuffer(buffer);

	index_endscan(sd);
	pfree(sd);
	return (tuple);
}

/*
 * The remainder of the file is for individual index scan routines.  Each
 * index should be scanned according to how it was defined during bootstrap
 * (that is, functional or normal) and what arguments the cache lookup
 * requires.  Each routine returns the heap tuple that qualifies.
 */
HeapTuple
AttributeNameIndexScan(Relation heapRelation,
					   Oid relid,
					   char *attname)
{
	Relation	idesc;
	ScanKeyData skey[2];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure)F_OIDEQ,
						   Int32GetDatum(relid));

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure)F_NAMEEQ,
						   NameGetDatum(attname));

	idesc = index_openr(AttributeNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 2);

	index_close(idesc);

	return tuple;
}

HeapTuple
AttributeNumIndexScan(Relation heapRelation,
					  Oid relid,
					  AttrNumber attnum)
{
	Relation	idesc;
	ScanKeyData skey[2];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure)F_OIDEQ,
						   Int32GetDatum(relid));

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure)F_INT2EQ,
						   Int16GetDatum(attnum));

	idesc = index_openr(AttributeNumIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 2);

	index_close(idesc);

	return tuple;
}

HeapTuple
ProcedureOidIndexScan(Relation heapRelation, Oid procId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   (Datum) procId);

	idesc = index_openr(ProcedureOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);

	return tuple;
}



HeapTuple
ProcedureNameIndexScan(Relation heapRelation,
					   char *procName,
					   int2 nargs,
					   Oid *argTypes)
{
	Relation	idesc;
	ScanKeyData skey[3];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   (Datum) procName);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_INT2EQ,
						   Int16GetDatum(nargs));

	ScanKeyEntryInitialize(&skey[2],
						   (bits16) 0x0,
						   (AttrNumber) 3,
						   (RegProcedure) F_OID8EQ,
						   (Datum) argTypes);

	idesc = index_openr(ProcedureNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 3);

	index_close(idesc);

	return tuple;
}



HeapTuple
ProcedureSrcIndexScan(Relation heapRelation, text *procSrc)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_TEXTEQ,
						   (Datum) procSrc);

	idesc = index_openr(ProcedureSrcIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);

	return tuple;
}

HeapTuple
TypeOidIndexScan(Relation heapRelation, Oid typeId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   (Datum) typeId);

	idesc = index_openr(TypeOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);

	return tuple;
}

HeapTuple
TypeNameIndexScan(Relation heapRelation, char *typeName)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   (Datum) typeName);

	idesc = index_openr(TypeNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);

	return tuple;
}

HeapTuple
ClassNameIndexScan(Relation heapRelation, char *relName)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   (Datum) relName);

	idesc = index_openr(ClassNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}

HeapTuple
ClassOidIndexScan(Relation heapRelation, Oid relId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   (Datum) relId);

	idesc = index_openr(ClassOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);

	return tuple;
}
