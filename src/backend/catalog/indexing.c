/*-------------------------------------------------------------------------
 *
 * indexing.c
 *	  This file contains routines to support indices defined on system
 *	  catalogs.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/indexing.c,v 1.69 2000/10/08 03:53:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"

/*
 * Names of indices - they match all system caches
 */

char	   *Name_pg_aggregate_indices[Num_pg_aggregate_indices] =
{AggregateNameTypeIndex};
char	   *Name_pg_am_indices[Num_pg_am_indices] =
{AmNameIndex};
char	   *Name_pg_amop_indices[Num_pg_amop_indices] =
{AccessMethodOpidIndex, AccessMethodStrategyIndex};
char	   *Name_pg_attr_indices[Num_pg_attr_indices] =
{AttributeRelidNameIndex, AttributeRelidNumIndex};
char	   *Name_pg_attrdef_indices[Num_pg_attrdef_indices] =
{AttrDefaultIndex};
char	   *Name_pg_class_indices[Num_pg_class_indices] =
{ClassNameIndex, ClassOidIndex};
char	   *Name_pg_group_indices[Num_pg_group_indices] =
{GroupNameIndex, GroupSysidIndex};
char	   *Name_pg_index_indices[Num_pg_index_indices] =
{IndexRelidIndex, IndexIndrelidIndex};
char	   *Name_pg_inherits_indices[Num_pg_inherits_indices] =
{InheritsRelidSeqnoIndex};
char	   *Name_pg_language_indices[Num_pg_language_indices] =
{LanguageOidIndex, LanguageNameIndex};
char	   *Name_pg_listener_indices[Num_pg_listener_indices] =
{ListenerPidRelnameIndex};
char	   *Name_pg_opclass_indices[Num_pg_opclass_indices] =
{OpclassNameIndex, OpclassDeftypeIndex};
char	   *Name_pg_operator_indices[Num_pg_operator_indices] =
{OperatorOidIndex, OperatorNameIndex};
char	   *Name_pg_proc_indices[Num_pg_proc_indices] =
{ProcedureOidIndex, ProcedureNameIndex};
char	   *Name_pg_relcheck_indices[Num_pg_relcheck_indices] =
{RelCheckIndex};
char	   *Name_pg_rewrite_indices[Num_pg_rewrite_indices] =
{RewriteOidIndex, RewriteRulenameIndex};
char	   *Name_pg_shadow_indices[Num_pg_shadow_indices] =
{ShadowNameIndex, ShadowSysidIndex};
char	   *Name_pg_statistic_indices[Num_pg_statistic_indices] =
{StatisticRelidAttnumIndex};
char	   *Name_pg_trigger_indices[Num_pg_trigger_indices] =
{TriggerRelidIndex, TriggerConstrNameIndex, TriggerConstrRelidIndex};
char	   *Name_pg_type_indices[Num_pg_type_indices] =
{TypeNameIndex, TypeOidIndex};
char	   *Name_pg_description_indices[Num_pg_description_indices] =
{DescriptionObjIndex};



static HeapTuple CatalogIndexFetchTuple(Relation heapRelation,
					   Relation idesc,
					   ScanKey skey,
					   int16 num_keys);


/*
 * Changes (appends) to catalogs can and do happen at various places
 * throughout the code.  We need a generic routine that will open all of
 * the indices defined on a given catalog and return the relation descriptors
 * associated with them.
 */
void
CatalogOpenIndices(int nIndices, char **names, Relation *idescs)
{
	int			i;

	if (IsIgnoringSystemIndexes())
		return;
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

	if (IsIgnoringSystemIndexes())
		return;
	for (i = 0; i < nIndices; i++)
		index_close(idescs[i]);
}


/*
 * For the same reasons outlined above for CatalogOpenIndices(), we need a
 * routine that takes a new catalog tuple and inserts an associated index
 * tuple into each catalog index.
 *
 * NOTE: since this routine looks up all the pg_index data on each call,
 * it's relatively inefficient for inserting a large number of tuples into
 * the same catalog.  We use it only for inserting one or a few tuples
 * in a given command.  See ExecOpenIndices() and related routines if you
 * are inserting tuples in bulk.
 *
 * NOTE: we do not bother to handle partial indices.  Nor do we try to
 * be efficient for functional indices (the code should work for them,
 * but may leak memory intraquery).  This should be OK for system catalogs,
 * but don't use this routine for user tables!
 */
void
CatalogIndexInsert(Relation *idescs,
				   int nIndices,
				   Relation heapRelation,
				   HeapTuple heapTuple)
{
	TupleDesc	heapDescriptor;
	Datum		datum[INDEX_MAX_KEYS];
	char		nullv[INDEX_MAX_KEYS];
	int			i;

	if (IsIgnoringSystemIndexes())
		return;
	heapDescriptor = RelationGetDescr(heapRelation);

	for (i = 0; i < nIndices; i++)
	{
		HeapTuple	index_tup;
		IndexInfo  *indexInfo;
		InsertIndexResult indexRes;

		index_tup = SearchSysCacheTuple(INDEXRELID,
										ObjectIdGetDatum(idescs[i]->rd_id),
										0, 0, 0);
		if (!HeapTupleIsValid(index_tup))
			elog(ERROR, "CatalogIndexInsert: index %u not found",
				 idescs[i]->rd_id);
		indexInfo = BuildIndexInfo(index_tup);

		FormIndexDatum(indexInfo,
					   heapTuple,
					   heapDescriptor,
					   CurrentMemoryContext,
					   datum,
					   nullv);

		indexRes = index_insert(idescs[i], datum, nullv,
								&heapTuple->t_self, heapRelation);
		if (indexRes)
			pfree(indexRes);
		pfree(indexInfo);
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
				return true;
		}
		return false;
	}

	pg_class = heap_openr(RelationRelationName, AccessShareLock);
	htup = ClassOidIndexScan(pg_class, catId);
	heap_close(pg_class, AccessShareLock);

	if (!HeapTupleIsValid(htup))
	{
		elog(NOTICE, "CatalogHasIndex: no relation with oid %u", catId);
		return false;
	}

	pgRelP = (Form_pg_class) GETSTRUCT(htup);
	return pgRelP->relhasindex;
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
	HeapTupleData tuple;
	HeapTuple	result = NULL;
	Buffer		buffer;

	sd = index_beginscan(idesc, false, num_keys, skey);
	tuple.t_datamcxt = CurrentMemoryContext;
	tuple.t_data = NULL;
	while ((indexRes = index_getnext(sd, ForwardScanDirection)))
	{
		tuple.t_self = indexRes->heap_iptr;
		heap_fetch(heapRelation, SnapshotNow, &tuple, &buffer);
		pfree(indexRes);
		if (tuple.t_data != NULL)
			break;
	}

	if (tuple.t_data != NULL)
	{
		result = heap_copytuple(&tuple);
		ReleaseBuffer(buffer);
	}

	index_endscan(sd);

	return result;
}


/*---------------------------------------------------------------------
 *						 Class-specific index lookups
 *---------------------------------------------------------------------
 */

/*
 * The remainder of the file is for individual index scan routines.  Each
 * index should be scanned according to how it was defined during bootstrap
 * (that is, functional or normal) and what arguments the cache lookup
 * requires.  Each routine returns the heap tuple that qualifies.
 */


HeapTuple
AggregateNameTypeIndexScan(Relation heapRelation,
						   Datum aggName, Datum aggType)
{
	Relation	idesc;
	ScanKeyData skey[2];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   aggName);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_OIDEQ,
						   aggType);

	idesc = index_openr(AggregateNameTypeIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 2);

	index_close(idesc);
	return tuple;
}


HeapTuple
AmNameIndexScan(Relation heapRelation, Datum amName)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   amName);

	idesc = index_openr(AmNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
AccessMethodOpidIndexScan(Relation heapRelation,
						  Datum claid,
						  Datum opopr,
						  Datum opid)
{
	Relation	idesc;
	ScanKeyData skey[3];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   claid);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_OIDEQ,
						   opopr);

	ScanKeyEntryInitialize(&skey[2],
						   (bits16) 0x0,
						   (AttrNumber) 3,
						   (RegProcedure) F_OIDEQ,
						   opid);

	idesc = index_openr(AccessMethodOpidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 3);

	index_close(idesc);
	return tuple;
}

HeapTuple
AccessMethodStrategyIndexScan(Relation heapRelation,
							  Datum opid,
							  Datum claid,
							  Datum opstrategy)
{
	Relation	idesc;
	ScanKeyData skey[3];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   opid);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_OIDEQ,
						   claid);

	ScanKeyEntryInitialize(&skey[2],
						   (bits16) 0x0,
						   (AttrNumber) 3,
						   (RegProcedure) F_INT2EQ,
						   opstrategy);

	idesc = index_openr(AccessMethodStrategyIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 3);

	index_close(idesc);
	return tuple;
}


HeapTuple
AttributeRelidNameIndexScan(Relation heapRelation,
							Datum relid,
							Datum attname)
{
	Relation	idesc;
	ScanKeyData skey[2];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   relid);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_NAMEEQ,
						   attname);

	idesc = index_openr(AttributeRelidNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 2);

	index_close(idesc);
	return tuple;
}


HeapTuple
AttributeRelidNumIndexScan(Relation heapRelation,
						   Datum relid,
						   Datum attnum)
{
	Relation	idesc;
	ScanKeyData skey[2];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   relid);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_INT2EQ,
						   attnum);

	idesc = index_openr(AttributeRelidNumIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 2);

	index_close(idesc);
	return tuple;
}


HeapTuple
OpclassDeftypeIndexScan(Relation heapRelation, Datum defType)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   defType);

	idesc = index_openr(OpclassDeftypeIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
OpclassNameIndexScan(Relation heapRelation, Datum opcName)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   opcName);

	idesc = index_openr(OpclassNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
GroupNameIndexScan(Relation heapRelation, Datum groName)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   groName);

	idesc = index_openr(GroupNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
GroupSysidIndexScan(Relation heapRelation, Datum sysId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_INT4EQ,
						   sysId);

	idesc = index_openr(GroupSysidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
IndexRelidIndexScan(Relation heapRelation, Datum relid)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   relid);

	idesc = index_openr(IndexRelidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
InheritsRelidSeqnoIndexScan(Relation heapRelation,
							Datum relid,
							Datum seqno)
{
	Relation	idesc;
	ScanKeyData skey[2];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   relid);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_INT4EQ,
						   seqno);

	idesc = index_openr(InheritsRelidSeqnoIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 2);

	index_close(idesc);
	return tuple;
}


HeapTuple
LanguageNameIndexScan(Relation heapRelation, Datum lanName)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   lanName);

	idesc = index_openr(LanguageNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
LanguageOidIndexScan(Relation heapRelation, Datum lanId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   lanId);

	idesc = index_openr(LanguageOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
ListenerPidRelnameIndexScan(Relation heapRelation,
							Datum pid, Datum relName)
{
	Relation	idesc;
	ScanKeyData skey[2];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_INT4EQ,
						   pid);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_NAMEEQ,
						   relName);

	idesc = index_openr(ListenerPidRelnameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 2);

	index_close(idesc);
	return tuple;
}


HeapTuple
OperatorNameIndexScan(Relation heapRelation,
					  Datum oprName,
					  Datum oprLeft,
					  Datum oprRight,
					  Datum oprKind)
{
	Relation	idesc;
	ScanKeyData skey[4];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   oprName);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_OIDEQ,
						   oprLeft);

	ScanKeyEntryInitialize(&skey[2],
						   (bits16) 0x0,
						   (AttrNumber) 3,
						   (RegProcedure) F_OIDEQ,
						   oprRight);

	ScanKeyEntryInitialize(&skey[3],
						   (bits16) 0x0,
						   (AttrNumber) 4,
						   (RegProcedure) F_CHAREQ,
						   oprKind);

	idesc = index_openr(OperatorNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 4);

	index_close(idesc);
	return tuple;
}


HeapTuple
OperatorOidIndexScan(Relation heapRelation, Datum oprId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   oprId);

	idesc = index_openr(OperatorOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
ProcedureNameIndexScan(Relation heapRelation,
					   Datum procName,
					   Datum nargs,
					   Datum argTypes)
{
	Relation	idesc;
	ScanKeyData skey[3];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   procName);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_INT2EQ,
						   nargs);

	ScanKeyEntryInitialize(&skey[2],
						   (bits16) 0x0,
						   (AttrNumber) 3,
						   (RegProcedure) F_OIDVECTOREQ,
						   argTypes);

	idesc = index_openr(ProcedureNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 3);

	index_close(idesc);
	return tuple;
}


HeapTuple
ProcedureOidIndexScan(Relation heapRelation, Datum procId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   procId);

	idesc = index_openr(ProcedureOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
ClassNameIndexScan(Relation heapRelation, Datum relName)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   relName);

	idesc = index_openr(ClassNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
ClassOidIndexScan(Relation heapRelation, Datum relId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   relId);

	idesc = index_openr(ClassOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
RewriteRulenameIndexScan(Relation heapRelation, Datum ruleName)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   ruleName);

	idesc = index_openr(RewriteRulenameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
RewriteOidIndexScan(Relation heapRelation, Datum rewriteId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   rewriteId);

	idesc = index_openr(RewriteOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
ShadowNameIndexScan(Relation heapRelation, Datum useName)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   useName);

	idesc = index_openr(ShadowNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
ShadowSysidIndexScan(Relation heapRelation, Datum sysId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_INT4EQ,
						   sysId);

	idesc = index_openr(ShadowSysidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
StatisticRelidAttnumIndexScan(Relation heapRelation,
							  Datum relId,
							  Datum attNum)
{
	Relation	idesc;
	ScanKeyData skey[2];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   relId);

	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) 2,
						   (RegProcedure) F_INT2EQ,
						   attNum);

	idesc = index_openr(StatisticRelidAttnumIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 2);

	index_close(idesc);
	return tuple;
}


HeapTuple
TypeNameIndexScan(Relation heapRelation, Datum typeName)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   typeName);

	idesc = index_openr(TypeNameIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}


HeapTuple
TypeOidIndexScan(Relation heapRelation, Datum typeId)
{
	Relation	idesc;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   typeId);

	idesc = index_openr(TypeOidIndex);
	tuple = CatalogIndexFetchTuple(heapRelation, idesc, skey, 1);

	index_close(idesc);
	return tuple;
}
