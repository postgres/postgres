/*-------------------------------------------------------------------------
 *
 * indexing.c
 *	  This file contains routines to support indices defined on system
 *	  catalogs.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/indexing.c,v 1.97 2002/07/15 16:33:31 tgl Exp $
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
 * Names of indices for each system catalog.
 */

char	   *Name_pg_aggregate_indices[Num_pg_aggregate_indices] =
{AggregateFnoidIndex};
char	   *Name_pg_am_indices[Num_pg_am_indices] =
{AmNameIndex, AmOidIndex};
char	   *Name_pg_amop_indices[Num_pg_amop_indices] =
{AccessMethodOperatorIndex, AccessMethodStrategyIndex};
char	   *Name_pg_amproc_indices[Num_pg_amproc_indices] =
{AccessMethodProcedureIndex};
char	   *Name_pg_attr_indices[Num_pg_attr_indices] =
{AttributeRelidNameIndex, AttributeRelidNumIndex};
char	   *Name_pg_attrdef_indices[Num_pg_attrdef_indices] =
{AttrDefaultIndex, AttrDefaultOidIndex};
char	   *Name_pg_class_indices[Num_pg_class_indices] =
{ClassNameNspIndex, ClassOidIndex};
char	   *Name_pg_constraint_indices[Num_pg_constraint_indices] =
{ConstraintNameNspIndex, ConstraintOidIndex, ConstraintRelidIndex};
char	   *Name_pg_conversion_indices[Num_pg_conversion_indices] =
{ConversionDefaultIndex, ConversionNameNspIndex, ConversionOidIndex};
char	   *Name_pg_database_indices[Num_pg_database_indices] =
{DatabaseNameIndex, DatabaseOidIndex};
char	   *Name_pg_depend_indices[Num_pg_depend_indices] =
{DependDependerIndex, DependReferenceIndex};
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
char	   *Name_pg_namespace_indices[Num_pg_namespace_indices] =
{NamespaceNameIndex, NamespaceOidIndex};
char	   *Name_pg_opclass_indices[Num_pg_opclass_indices] =
{OpclassAmNameNspIndex, OpclassOidIndex};
char	   *Name_pg_operator_indices[Num_pg_operator_indices] =
{OperatorOidIndex, OperatorNameNspIndex};
char	   *Name_pg_proc_indices[Num_pg_proc_indices] =
{ProcedureOidIndex, ProcedureNameNspIndex};
char	   *Name_pg_rewrite_indices[Num_pg_rewrite_indices] =
{RewriteOidIndex, RewriteRelRulenameIndex};
char	   *Name_pg_shadow_indices[Num_pg_shadow_indices] =
{ShadowNameIndex, ShadowSysidIndex};
char	   *Name_pg_statistic_indices[Num_pg_statistic_indices] =
{StatisticRelidAttnumIndex};
char	   *Name_pg_trigger_indices[Num_pg_trigger_indices] =
{TriggerRelidNameIndex, TriggerConstrNameIndex, TriggerConstrRelidIndex, TriggerOidIndex};
char	   *Name_pg_type_indices[Num_pg_type_indices] =
{TypeNameNspIndex, TypeOidIndex};
char	   *Name_pg_description_indices[Num_pg_description_indices] =
{DescriptionObjIndex};



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
 * in a given command.	See ExecOpenIndices() and related routines if you
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
		IndexInfo  *indexInfo;
		InsertIndexResult indexRes;

		indexInfo = BuildIndexInfo(idescs[i]->rd_index);

		FormIndexDatum(indexInfo,
					   heapTuple,
					   heapDescriptor,
					   CurrentMemoryContext,
					   datum,
					   nullv);

		indexRes = index_insert(idescs[i], datum, nullv,
								&heapTuple->t_self, heapRelation,
								idescs[i]->rd_uniqueindex);
		if (indexRes)
			pfree(indexRes);
		pfree(indexInfo);
	}
}
