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
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/indexing.c,v 1.12 1997/09/07 04:40:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include <postgres.h>

#include <utils/builtins.h>
#include <utils/oidcompos.h>
#include <access/heapam.h>
#include <access/genam.h>
#include <storage/bufmgr.h>
#include <nodes/execnodes.h>
#include <catalog/catalog.h>
#include <catalog/catname.h>
#include <catalog/pg_index.h>
#include <catalog/pg_proc.h>
#include <utils/syscache.h>
#include <catalog/indexing.h>
#include <catalog/index.h>
#include <miscadmin.h>

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

char		   *Name_pg_attr_indices[Num_pg_attr_indices] = {AttributeNameIndex,
	AttributeNumIndex,
AttributeRelidIndex};
char		   *Name_pg_proc_indices[Num_pg_proc_indices] = {ProcedureNameIndex,
	ProcedureOidIndex,
ProcedureSrcIndex};
char		   *Name_pg_type_indices[Num_pg_type_indices] = {TypeNameIndex,
TypeOidIndex};
char		   *Name_pg_class_indices[Num_pg_class_indices] = {ClassNameIndex,
ClassOidIndex};
char		   *Name_pg_attrdef_indices[Num_pg_attrdef_indices] = {AttrDefaultIndex};

char		   *Name_pg_relcheck_indices[Num_pg_relcheck_indices] = {RelCheckIndex};

char		   *Name_pg_trigger_indices[Num_pg_trigger_indices] = {TriggerRelidIndex};


static HeapTuple
CatalogIndexFetchTuple(Relation heapRelation,
					   Relation idesc,
					   ScanKey skey);


/*
 * Changes (appends) to catalogs can (and does) happen at various places
 * throughout the code.  We need a generic routine that will open all of
 * the indices defined on a given catalog a return the relation descriptors
 * associated with them.
 */
void
CatalogOpenIndices(int nIndices, char *names[], Relation idescs[])
{
	int				i;

	for (i = 0; i < nIndices; i++)
	{
		idescs[i] = index_openr(names[i]);
	}
}

/*
 * This is the inverse routine to CatalogOpenIndices()
 */
void
CatalogCloseIndices(int nIndices, Relation * idescs)
{
	int				i;

	for (i = 0; i < nIndices; i++)
		index_close(idescs[i]);
}


/*
 * For the same reasons outlined above CatalogOpenIndices() we need a routine
 * that takes a new catalog tuple and inserts an associated index tuple into
 * each catalog index.
 */
void
CatalogIndexInsert(Relation * idescs,
				   int nIndices,
				   Relation heapRelation,
				   HeapTuple heapTuple)
{
	HeapTuple		pgIndexTup;
	TupleDesc		heapDescriptor;
	IndexTupleForm	pgIndexP;
	Datum			datum;
	int				natts;
	AttrNumber	   *attnumP;
	FuncIndexInfo	finfo,
				   *finfoP;
	char			nulls[INDEX_MAX_KEYS];
	int				i;

	heapDescriptor = RelationGetTupleDescriptor(heapRelation);

	for (i = 0; i < nIndices; i++)
	{
		TupleDesc		indexDescriptor;
		InsertIndexResult indexRes;

		indexDescriptor = RelationGetTupleDescriptor(idescs[i]);
		pgIndexTup = SearchSysCacheTuple(INDEXRELID,
										 Int32GetDatum(idescs[i]->rd_id),
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
					   (AttrNumber *) & pgIndexP->indkey[0],
					   heapTuple,
					   heapDescriptor,
					   InvalidBuffer,
					   &datum,
					   nulls,
					   finfoP);

		indexRes = index_insert(idescs[i], &datum, nulls,
								&(heapTuple->t_ctid), heapRelation);
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
	Relation		pg_class;
	HeapTuple		htup;
	Form_pg_class	pgRelP;
	int				i;

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
static			HeapTuple
CatalogIndexFetchTuple(Relation heapRelation,
					   Relation idesc,
					   ScanKey skey)
{
	IndexScanDesc	sd;
	RetrieveIndexResult indexRes;
	HeapTuple		tuple;
	Buffer			buffer;

	sd = index_beginscan(idesc, false, 1, skey);
	tuple = (HeapTuple) NULL;

	do
	{
		indexRes = index_getnext(sd, ForwardScanDirection);
		if (indexRes)
		{
			ItemPointer		iptr;

			iptr = &indexRes->heap_iptr;
			tuple = heap_fetch(heapRelation, NowTimeQual, iptr, &buffer);
			pfree(indexRes);
		}
		else
			break;
	} while (!HeapTupleIsValid(tuple));

	if (HeapTupleIsValid(tuple))
	{
		tuple = heap_copytuple(tuple);
		ReleaseBuffer(buffer);
	}

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
	Relation		idesc;
	ScanKeyData		skey;
	OidName			keyarg;
	HeapTuple		tuple;

	keyarg = mkoidname(relid, attname);
	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) OidNameEqRegProcedure,
						   (Datum) keyarg);

	idesc = index_openr(AttributeNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, &skey);

	index_close(idesc);
	pfree(keyarg);

	return tuple;
}

HeapTuple
AttributeNumIndexScan(Relation heapRelation,
					  Oid relid,
					  AttrNumber attnum)
{
	Relation		idesc;
	ScanKeyData		skey;
	OidInt2			keyarg;
	HeapTuple		tuple;

	keyarg = mkoidint2(relid, (uint16) attnum);
	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) OidInt2EqRegProcedure,
						   (Datum) keyarg);

	idesc = index_openr(AttributeNumIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, &skey);

	index_close(idesc);
	pfree(keyarg);

	return tuple;
}

HeapTuple
ProcedureOidIndexScan(Relation heapRelation, Oid procId)
{
	Relation		idesc;
	ScanKeyData		skey;
	HeapTuple		tuple;

	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) ObjectIdEqualRegProcedure,
						   (Datum) procId);

	idesc = index_openr(ProcedureOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, &skey);

	index_close(idesc);

	return tuple;
}



HeapTuple
ProcedureNameIndexScan(Relation heapRelation,
					   char *procName,
					   int nargs,
					   Oid * argTypes)
{
	Relation		idesc;
	ScanKeyData		skey;
	HeapTuple		tuple;		/* tuple being tested */
	HeapTuple		return_tuple;		/* The tuple pointer we eventually
										 * return */
	IndexScanDesc	sd;
	RetrieveIndexResult indexRes;
	Buffer			buffer;
	Form_pg_proc	pgProcP;
	bool			ScanComplete;

	/*
	 * The index scan is complete, i.e. we've scanned everything there is
	 * to scan.
	 */
	bool			FoundMatch;

	/*
	 * In scanning pg_proc, we have found a row that meets our search
	 * criteria.
	 */

	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) NameEqualRegProcedure,
						   (Datum) procName);

	idesc = index_openr(ProcedureNameIndex);

	sd = index_beginscan(idesc, false, 1, &skey);

	/*
	 * for now, we do the work usually done by CatalogIndexFetchTuple by
	 * hand, so that we can check that the other keys match.  when
	 * multi-key indices are added, they will be used here.
	 */
	tuple = (HeapTuple) NULL;	/* initial value */
	ScanComplete = false;		/* Scan hasn't begun yet */
	FoundMatch = false;			/* No match yet; haven't even looked. */
	while (!FoundMatch && !ScanComplete)
	{
		indexRes = index_getnext(sd, ForwardScanDirection);
		if (indexRes)
		{
			ItemPointer		iptr;

			iptr = &indexRes->heap_iptr;
			tuple = heap_fetch(heapRelation, NowTimeQual, iptr, &buffer);
			pfree(indexRes);
			if (HeapTupleIsValid(tuple))
			{

				/*
				 * Here's a row for a procedure that has the sought
				 * procedure name.	To be a match, though, we need it to
				 * have the right number and type of arguments too, so we
				 * check that now.
				 */
				pgProcP = (Form_pg_proc) GETSTRUCT(tuple);
				if (pgProcP->pronargs == nargs &&
					oid8eq(&(pgProcP->proargtypes[0]), argTypes))
					FoundMatch = true;
				else
					ReleaseBuffer(buffer);
			}
		}
		else
			ScanComplete = true;
	}

	if (FoundMatch)
	{
		Assert(HeapTupleIsValid(tuple));
		return_tuple = heap_copytuple(tuple);
		ReleaseBuffer(buffer);
	}
	else
		return_tuple = (HeapTuple) NULL;

	index_endscan(sd);
	index_close(idesc);

	return return_tuple;
}



HeapTuple
ProcedureSrcIndexScan(Relation heapRelation, text * procSrc)
{
	Relation		idesc;
	IndexScanDesc	sd;
	ScanKeyData		skey;
	RetrieveIndexResult indexRes;
	HeapTuple		tuple;
	Buffer			buffer;

	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) Anum_pg_proc_prosrc,
						   (RegProcedure) TextEqualRegProcedure,
						   (Datum) procSrc);

	idesc = index_openr(ProcedureSrcIndex);
	sd = index_beginscan(idesc, false, 1, &skey);

	indexRes = index_getnext(sd, ForwardScanDirection);
	if (indexRes)
	{
		ItemPointer		iptr;

		iptr = &indexRes->heap_iptr;
		tuple = heap_fetch(heapRelation, NowTimeQual, iptr, &buffer);
		pfree(indexRes);
	}
	else
		tuple = (HeapTuple) NULL;

	if (HeapTupleIsValid(tuple))
	{
		tuple = heap_copytuple(tuple);
		ReleaseBuffer(buffer);
	}

	index_endscan(sd);

	return tuple;
}

HeapTuple
TypeOidIndexScan(Relation heapRelation, Oid typeId)
{
	Relation		idesc;
	ScanKeyData		skey;
	HeapTuple		tuple;

	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) ObjectIdEqualRegProcedure,
						   (Datum) typeId);

	idesc = index_openr(TypeOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, &skey);

	index_close(idesc);

	return tuple;
}

HeapTuple
TypeNameIndexScan(Relation heapRelation, char *typeName)
{
	Relation		idesc;
	ScanKeyData		skey;
	HeapTuple		tuple;

	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) NameEqualRegProcedure,
						   (Datum) typeName);

	idesc = index_openr(TypeNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, &skey);

	index_close(idesc);

	return tuple;
}

HeapTuple
ClassNameIndexScan(Relation heapRelation, char *relName)
{
	Relation		idesc;
	ScanKeyData		skey;
	HeapTuple		tuple;

	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) NameEqualRegProcedure,
						   (Datum) relName);

	idesc = index_openr(ClassNameIndex);

	tuple = CatalogIndexFetchTuple(heapRelation, idesc, &skey);

	index_close(idesc);
	return tuple;
}

HeapTuple
ClassOidIndexScan(Relation heapRelation, Oid relId)
{
	Relation		idesc;
	ScanKeyData		skey;
	HeapTuple		tuple;

	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) ObjectIdEqualRegProcedure,
						   (Datum) relId);

	idesc = index_openr(ClassOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, &skey);

	index_close(idesc);

	return tuple;
}
