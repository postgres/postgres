/*-------------------------------------------------------------------------
 *
 * plancat.c
 *	   routines for accessing the system catalogs
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/plancat.c,v 1.76 2003/01/28 22:13:35 tgl Exp $
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
#include "optimizer/clauses.h"
#include "optimizer/plancat.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "catalog/catalog.h"
#include "miscadmin.h"


/*
 * get_relation_info -
 *	  Retrieves catalog information for a given relation.
 *	  Given the Oid of the relation, return the following info:
 *				whether the relation has secondary indices
 *				number of pages
 *				number of tuples
 */
void
get_relation_info(Oid relationObjectId,
				  bool *hasindex, long *pages, double *tuples)
{
	HeapTuple	relationTuple;
	Form_pg_class relation;

	relationTuple = SearchSysCache(RELOID,
								   ObjectIdGetDatum(relationObjectId),
								   0, 0, 0);
	if (!HeapTupleIsValid(relationTuple))
		elog(ERROR, "get_relation_info: Relation %u not found",
			 relationObjectId);
	relation = (Form_pg_class) GETSTRUCT(relationTuple);

	if (IsIgnoringSystemIndexes() && IsSystemClass(relation))
		*hasindex = false;
	else
		*hasindex = relation->relhasindex;

	*pages = relation->relpages;
	*tuples = relation->reltuples;

	ReleaseSysCache(relationTuple);
}

/*
 * find_secondary_indexes
 *	  Creates a list of IndexOptInfo nodes containing information for each
 *	  secondary index defined on the specified relation.
 *
 * 'relationObjectId' is the OID of the relation for which indices are wanted
 *
 * Returns a list of new IndexOptInfo nodes.
 */
List *
find_secondary_indexes(Oid relationObjectId)
{
	List	   *indexinfos = NIL;
	List	   *indexoidlist,
			   *indexoidscan;
	Relation	relation;

	/*
	 * We used to scan pg_index directly, but now the relcache offers a
	 * cached list of OID indexes for each relation.  So, get that list
	 * and then use the syscache to obtain pg_index entries.
	 */
	relation = heap_open(relationObjectId, AccessShareLock);
	indexoidlist = RelationGetIndexList(relation);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirsti(indexoidscan);
		Relation	indexRelation;
		Form_pg_index index;
		IndexOptInfo *info;
		int			i;
		int16		amorderstrategy;

		/* Extract info from the relation descriptor for the index */
		indexRelation = index_open(indexoid);

		info = makeNode(IndexOptInfo);

		/*
		 * Need to make these arrays large enough to be sure there is room
		 * for a terminating 0 at the end of each one.
		 */
		info->classlist = (Oid *) palloc(sizeof(Oid) * (INDEX_MAX_KEYS + 1));
		info->indexkeys = (int *) palloc(sizeof(int) * (INDEX_MAX_KEYS + 1));
		info->ordering = (Oid *) palloc(sizeof(Oid) * (INDEX_MAX_KEYS + 1));

		/* Extract info from the pg_index tuple */
		index = indexRelation->rd_index;
		info->indexoid = index->indexrelid;
		info->indproc = index->indproc; /* functional index ?? */
		if (VARSIZE(&index->indpred) > VARHDRSZ)		/* partial index ?? */
		{
			char	   *predString;

			predString = DatumGetCString(DirectFunctionCall1(textout,
									  PointerGetDatum(&index->indpred)));
			info->indpred = (List *) stringToNode(predString);
			pfree(predString);
		}
		else
			info->indpred = NIL;
		info->unique = index->indisunique;

		for (i = 0; i < INDEX_MAX_KEYS; i++)
		{
			if (index->indclass[i] == (Oid) 0)
				break;
			info->classlist[i] = index->indclass[i];
		}
		info->classlist[i] = (Oid) 0;
		info->ncolumns = i;

		for (i = 0; i < INDEX_MAX_KEYS; i++)
		{
			if (index->indkey[i] == 0)
				break;
			info->indexkeys[i] = index->indkey[i];
		}
		info->indexkeys[i] = 0;
		info->nkeys = i;

		info->relam = indexRelation->rd_rel->relam;
		info->pages = indexRelation->rd_rel->relpages;
		info->tuples = indexRelation->rd_rel->reltuples;
		info->amcostestimate = index_cost_estimator(indexRelation);
		amorderstrategy = indexRelation->rd_am->amorderstrategy;

		/*
		 * Fetch the ordering operators associated with the index, if any.
		 */
		MemSet(info->ordering, 0, sizeof(Oid) * (INDEX_MAX_KEYS + 1));
		if (amorderstrategy != 0)
		{
			int			oprindex = amorderstrategy - 1;

			for (i = 0; i < info->ncolumns; i++)
			{
				info->ordering[i] = indexRelation->rd_operator[oprindex];
				oprindex += indexRelation->rd_am->amstrategies;
			}
		}

		/* initialize cached join info to empty */
		info->outer_relids = NIL;
		info->inner_paths = NIL;

		index_close(indexRelation);

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
 * Returns the selectivity of a specified restriction operator clause.
 * This code executes registered procedures stored in the
 * operator relation, by calling the function manager.
 *
 * See clause_selectivity() for the meaning of the additional parameters.
 */
Selectivity
restriction_selectivity(Query *root,
						Oid operator,
						List *args,
						int varRelid)
{
	RegProcedure oprrest = get_oprrest(operator);
	float8		result;

	/*
	 * if the oprrest procedure is missing for whatever reason, use a
	 * selectivity of 0.5
	 */
	if (!oprrest)
		return (Selectivity) 0.5;

	result = DatumGetFloat8(OidFunctionCall4(oprrest,
											 PointerGetDatum(root),
											 ObjectIdGetDatum(operator),
											 PointerGetDatum(args),
											 Int32GetDatum(varRelid)));

	if (result < 0.0 || result > 1.0)
		elog(ERROR, "restriction_selectivity: bad value %f", result);

	return (Selectivity) result;
}

/*
 * join_selectivity
 *
 * Returns the selectivity of a specified join operator clause.
 * This code executes registered procedures stored in the
 * operator relation, by calling the function manager.
 */
Selectivity
join_selectivity(Query *root,
				 Oid operator,
				 List *args,
				 JoinType jointype)
{
	RegProcedure oprjoin = get_oprjoin(operator);
	float8		result;

	/*
	 * if the oprjoin procedure is missing for whatever reason, use a
	 * selectivity of 0.5
	 */
	if (!oprjoin)
		return (Selectivity) 0.5;

	result = DatumGetFloat8(OidFunctionCall4(oprjoin,
											 PointerGetDatum(root),
											 ObjectIdGetDatum(operator),
											 PointerGetDatum(args),
											 Int16GetDatum(jointype)));

	if (result < 0.0 || result > 1.0)
		elog(ERROR, "join_selectivity: bad value %f", result);

	return (Selectivity) result;
}

/*
 * find_inheritance_children
 *
 * Returns an integer list containing the OIDs of all relations which
 * inherit *directly* from the relation with OID 'inhparent'.
 *
 * XXX might be a good idea to create an index on pg_inherits' inhparent
 * field, so that we can use an indexscan instead of sequential scan here.
 * However, in typical databases pg_inherits won't have enough entries to
 * justify an indexscan...
 */
List *
find_inheritance_children(Oid inhparent)
{
	List	   *list = NIL;
	Relation	relation;
	HeapScanDesc scan;
	HeapTuple	inheritsTuple;
	Oid			inhrelid;
	ScanKeyData key[1];

	/*
	 * Can skip the scan if pg_class shows the relation has never had a
	 * subclass.
	 */
	if (!has_subclass(inhparent))
		return NIL;

	ScanKeyEntryInitialize(&key[0],
						   (bits16) 0x0,
						   (AttrNumber) Anum_pg_inherits_inhparent,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(inhparent));
	relation = heap_openr(InheritsRelationName, AccessShareLock);
	scan = heap_beginscan(relation, SnapshotNow, 1, key);
	while ((inheritsTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		inhrelid = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhrelid;
		list = lappendi(list, inhrelid);
	}
	heap_endscan(scan);
	heap_close(relation, AccessShareLock);
	return list;
}

/*
 * has_subclass
 *
 * In the current implementation, has_subclass returns whether a
 * particular class *might* have a subclass. It will not return the
 * correct result if a class had a subclass which was later dropped.
 * This is because relhassubclass in pg_class is not updated when a
 * subclass is dropped, primarily because of concurrency concerns.
 *
 * Currently has_subclass is only used as an efficiency hack to skip
 * unnecessary inheritance searches, so this is OK.
 */
bool
has_subclass(Oid relationId)
{
	HeapTuple	tuple;
	bool		result;

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relationId),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "has_subclass: Relation %u not found", relationId);

	result = ((Form_pg_class) GETSTRUCT(tuple))->relhassubclass;
	ReleaseSysCache(tuple);
	return result;
}

/*
 * has_unique_index
 *
 * Detect whether there is a unique index on the specified attribute
 * of the specified relation, thus allowing us to conclude that all
 * the (non-null) values of the attribute are distinct.
 */
bool
has_unique_index(RelOptInfo *rel, AttrNumber attno)
{
	List	   *ilist;

	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);

		/*
		 * Note: ignore functional and partial indexes, since they don't
		 * allow us to conclude that all attr values are distinct. Also, a
		 * multicolumn unique index doesn't allow us to conclude that just
		 * the specified attr is unique.
		 */
		if (index->unique &&
			index->nkeys == 1 &&
			index->indexkeys[0] == attno &&
			index->indproc == InvalidOid &&
			index->indpred == NIL)
			return true;
	}
	return false;
}
