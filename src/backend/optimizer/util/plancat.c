/*-------------------------------------------------------------------------
 *
 * plancat.c
 *	   routines for accessing the system catalogs
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/plancat.c,v 1.57 2000/06/17 21:48:51 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_index.h"
#include "optimizer/plancat.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "catalog/catalog.h"
#include "miscadmin.h"


/*
 * relation_info -
 *	  Retrieves catalog information for a given relation.
 *	  Given the rangetable index of the relation, return the following info:
 *				whether the relation has secondary indices
 *				number of pages
 *				number of tuples
 */
void
relation_info(Query *root, Index relid,
			  bool *hasindex, long *pages, double *tuples)
{
	Oid			relationObjectId = getrelid(relid, root->rtable);
	HeapTuple	relationTuple;
	Form_pg_class relation;

	relationTuple = SearchSysCacheTuple(RELOID,
										ObjectIdGetDatum(relationObjectId),
										0, 0, 0);
	if (!HeapTupleIsValid(relationTuple))
		elog(ERROR, "relation_info: Relation %u not found",
			 relationObjectId);
	relation = (Form_pg_class) GETSTRUCT(relationTuple);

	if (IsIgnoringSystemIndexes() && IsSystemRelationName(NameStr(relation->relname)))
		*hasindex = false;
	else
		*hasindex = (relation->relhasindex) ? true : false;
	*pages = relation->relpages;
	*tuples = relation->reltuples;
}

/*
 * find_secondary_indexes
 *	  Creates a list of IndexOptInfo nodes containing information for each
 *	  secondary index defined on the given relation.
 *
 * 'relid' is the RT index of the relation for which indices are being located
 *
 * Returns a list of new IndexOptInfo nodes.
 */
List *
find_secondary_indexes(Query *root, Index relid)
{
	List	   *indexinfos = NIL;
	List	   *indexoidlist,
			   *indexoidscan;
	Oid			indrelid = getrelid(relid, root->rtable);
	Relation	relation;

	/*
	 * We used to scan pg_index directly, but now the relcache offers
	 * a cached list of OID indexes for each relation.  So, get that list
	 * and then use the syscache to obtain pg_index entries.
	 */
	relation = heap_open(indrelid, AccessShareLock);
	indexoidlist = RelationGetIndexList(relation);

	foreach(indexoidscan, indexoidlist)
	{
		Oid		indexoid = lfirsti(indexoidscan);
		HeapTuple	indexTuple;
		Form_pg_index index;
		IndexOptInfo *info;
		int			i;
		Relation	indexRelation;
		Oid			relam;
		uint16		amorderstrategy;

		indexTuple = SearchSysCacheTupleCopy(INDEXRELID,
											 ObjectIdGetDatum(indexoid),
											 0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "find_secondary_indexes: index %u not found",
				 indexoid);
		index = (Form_pg_index) GETSTRUCT(indexTuple);
		info = makeNode(IndexOptInfo);

		/*
		 * Need to make these arrays large enough to be sure there is a
		 * terminating 0 at the end of each one.
		 */
		info->classlist = (Oid *) palloc(sizeof(Oid) * (INDEX_MAX_KEYS + 1));
		info->indexkeys = (int *) palloc(sizeof(int) * (INDEX_MAX_KEYS + 1));
		info->ordering = (Oid *) palloc(sizeof(Oid) * (INDEX_MAX_KEYS + 1));

		/* Extract info from the pg_index tuple */
		info->indexoid = index->indexrelid;
		info->indproc = index->indproc; /* functional index ?? */
		if (VARSIZE(&index->indpred) != 0)		/* partial index ?? */
		{
			char	   *predString = textout(&index->indpred);

			info->indpred = (List *) stringToNode(predString);
			pfree(predString);
		}
		else
			info->indpred = NIL;
		info->lossy = index->indislossy;

		for (i = 0; i < INDEX_MAX_KEYS; i++)
			info->indexkeys[i] = index->indkey[i];
		info->indexkeys[INDEX_MAX_KEYS] = 0;
		for (i = 0; i < INDEX_MAX_KEYS; i++)
			info->classlist[i] = index->indclass[i];
		info->classlist[INDEX_MAX_KEYS] = (Oid) 0;

		/* Extract info from the relation descriptor for the index */
		indexRelation = index_open(index->indexrelid);
		relam = indexRelation->rd_rel->relam;
		info->relam = relam;
		info->pages = indexRelation->rd_rel->relpages;
		info->tuples = indexRelation->rd_rel->reltuples;
		info->amcostestimate = index_cost_estimator(indexRelation);
		amorderstrategy = indexRelation->rd_am->amorderstrategy;
		index_close(indexRelation);

		/*
		 * Fetch the ordering operators associated with the index, if any.
		 */
		MemSet(info->ordering, 0, sizeof(Oid) * (INDEX_MAX_KEYS + 1));
		if (amorderstrategy != 0)
		{
			for (i = 0; i < INDEX_MAX_KEYS && index->indclass[i]; i++)
			{
				HeapTuple	amopTuple;
				Form_pg_amop amop;

				amopTuple =
					SearchSysCacheTuple(AMOPSTRATEGY,
										ObjectIdGetDatum(relam),
									ObjectIdGetDatum(index->indclass[i]),
										UInt16GetDatum(amorderstrategy),
										0);
				if (!HeapTupleIsValid(amopTuple))
					elog(ERROR, "find_secondary_indexes: no amop %u %u %d",
					   relam, index->indclass[i], (int) amorderstrategy);
				amop = (Form_pg_amop) GETSTRUCT(amopTuple);
				info->ordering[i] = amop->amopopr;
			}
		}

		heap_freetuple(indexTuple);

		indexinfos = lcons(info, indexinfos);
	}

	freeList(indexoidlist);

	/* XXX keep the lock here? */
	heap_close(relation, AccessShareLock);

	return indexinfos;
}

/*
 * restriction_selectivity
 *
 * Returns the selectivity of a specified operator.
 * This code executes registered procedures stored in the
 * operator relation, by calling the function manager.
 *
 * XXX The assumption in the selectivity procedures is that if the
 *		relation OIDs or attribute numbers are 0, then the clause
 *		isn't of the form (op var const).
 */
Selectivity
restriction_selectivity(Oid functionObjectId,
						Oid operatorObjectId,
						Oid relationObjectId,
						AttrNumber attributeNumber,
						Datum constValue,
						int constFlag)
{
	float8		result;

	result = DatumGetFloat8(OidFunctionCall5(functionObjectId,
							ObjectIdGetDatum(operatorObjectId),
							ObjectIdGetDatum(relationObjectId),
							Int16GetDatum(attributeNumber),
							constValue,
							Int32GetDatum(constFlag)));

	if (result < 0.0 || result > 1.0)
		elog(ERROR, "restriction_selectivity: bad value %f", result);

	return (Selectivity) result;
}

/*
 * join_selectivity
 *
 * Returns the selectivity of an operator, given the join clause
 * information.
 *
 * XXX The assumption in the selectivity procedures is that if the
 *		relation OIDs or attribute numbers are 0, then the clause
 *		isn't of the form (op var var).
 */
Selectivity
join_selectivity(Oid functionObjectId,
				 Oid operatorObjectId,
				 Oid relationObjectId1,
				 AttrNumber attributeNumber1,
				 Oid relationObjectId2,
				 AttrNumber attributeNumber2)
{
	float8		result;

	result = DatumGetFloat8(OidFunctionCall5(functionObjectId,
							ObjectIdGetDatum(operatorObjectId),
							ObjectIdGetDatum(relationObjectId1),
							Int16GetDatum(attributeNumber1),
							ObjectIdGetDatum(relationObjectId2),
							Int16GetDatum(attributeNumber2)));

	if (result < 0.0 || result > 1.0)
		elog(ERROR, "join_selectivity: bad value %f", result);

	return (Selectivity) result;
}

/*
 * find_inheritance_children
 *
 * Returns an integer list containing the OIDs of all relations which
 * inherit *directly* from the relation with OID 'inhparent'.
 */
List *
find_inheritance_children(Oid inhparent)
{
	static ScanKeyData key[1] = {
		{0, Anum_pg_inherits_inhparent, F_OIDEQ}
	};

	HeapTuple	inheritsTuple;
	Relation	relation;
	HeapScanDesc scan;
	List	   *list = NIL;
	Oid			inhrelid;

	fmgr_info(F_OIDEQ, &key[0].sk_func);
	key[0].sk_nargs = key[0].sk_func.fn_nargs;
	key[0].sk_argument = ObjectIdGetDatum(inhparent);

	relation = heap_openr(InheritsRelationName, AccessShareLock);
	scan = heap_beginscan(relation, 0, SnapshotNow, 1, key);
	while (HeapTupleIsValid(inheritsTuple = heap_getnext(scan, 0)))
	{
		inhrelid = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhrelid;
		list = lappendi(list, inhrelid);
	}
	heap_endscan(scan);
	heap_close(relation, AccessShareLock);
	return list;
}

/*
 * has_subclass -
 * In the current implementation, has_subclass returns whether a 
 * particular class *might* have a subclass. It will not return the
 * correct result if a class had a subclass which was later dropped.
 * This is because relhassubclass in pg_class is not updated,
 * possibly because of efficiency and/or concurrency concerns.
 * Currently has_subclass is only used as an efficiency hack, so this
 * is ok.
 */
bool
has_subclass(Oid relationId)
{
	HeapTuple	tuple =
		SearchSysCacheTuple(RELOID,
							ObjectIdGetDatum(relationId),
							0, 0, 0);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "has_subclass: Relation %u not found",
			 relationId);
	return ((Form_pg_class) GETSTRUCT(tuple))->relhassubclass;
}

#ifdef NOT_USED
/*
 * VersionGetParents
 *
 * Returns a LISP list containing the OIDs of all relations which are
 * base relations of the relation with OID 'verrelid'.
 */
List *
VersionGetParents(Oid verrelid)
{
	static ScanKeyData key[1] = {
		{0, Anum_pg_version_verrelid, F_OIDEQ}
	};

	HeapTuple	versionTuple;
	Relation	relation;
	HeapScanDesc scan;
	Oid			verbaseid;
	List	   *list = NIL;

	fmgr_info(F_OIDEQ, &key[0].sk_func);
	key[0].sk_nargs = key[0].sk_func.fn_nargs;
	key[0].sk_argument = ObjectIdGetDatum(verrelid);
	relation = heap_openr(VersionRelationName, AccessShareLock);
	scan = heap_beginscan(relation, 0, SnapshotNow, 1, key);
	while (HeapTupleIsValid(versionTuple = heap_getnext(scan, 0)))
	{
		verbaseid = ((Form_pg_version)
					 GETSTRUCT(versionTuple))->verbaseid;

		list = lconsi(verbaseid, list);

		key[0].sk_argument = ObjectIdGetDatum(verbaseid);
		heap_rescan(scan, 0, key);
	}
	heap_endscan(scan);
	heap_close(relation, AccessShareLock);
	return list;
}

#endif
