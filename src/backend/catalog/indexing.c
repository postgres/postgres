/*-------------------------------------------------------------------------
 *
 * indexing.c
 *	  This file contains routines to support indices defined on system
 *	  catalogs.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/indexing.c,v 1.76 2001/01/24 19:42:51 momjian Exp $
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
char	   *Name_pg_largeobject_indices[Num_pg_largeobject_indices] =
{LargeObjectLOidPNIndex};
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

	if (IsIgnoringSystemIndexes() || (!heapRelation->rd_rel->relhasindex))
		return;
	heapDescriptor = RelationGetDescr(heapRelation);

	for (i = 0; i < nIndices; i++)
	{
		HeapTuple	index_tup;
		IndexInfo  *indexInfo;
		InsertIndexResult indexRes;

		index_tup = SearchSysCache(INDEXRELID,
								   ObjectIdGetDatum(idescs[i]->rd_id),
								   0, 0, 0);
		if (!HeapTupleIsValid(index_tup))
			elog(ERROR, "CatalogIndexInsert: index %u not found",
				 idescs[i]->rd_id);
		indexInfo = BuildIndexInfo(index_tup);
		ReleaseSysCache(index_tup);

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
 * The remainder of the file is for individual index scan routines.
 * These routines provide canned scanning code for certain widely-used
 * indexes.  Most indexes don't need one of these.
 */


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
