/*-------------------------------------------------------------------------
 *
 * plancat.c
 *	   routines for accessing the system catalogs
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/plancat.c,v 1.87 2003/08/04 02:40:01 momjian Exp $
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
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/plancat.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
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
 *
 * Given the Oid of the relation, return the following info into fields
 * of the RelOptInfo struct:
 *
 *	min_attr	lowest valid AttrNumber
 *	max_attr	highest valid AttrNumber
 *	indexlist	list of IndexOptInfos for relation's indexes
 *	pages		number of pages
 *	tuples		number of tuples
 */
void
get_relation_info(Oid relationObjectId, RelOptInfo *rel)
{
	Index		varno = rel->relid;
	Relation	relation;
	bool		hasindex;
	List	   *indexinfos = NIL;

	relation = heap_open(relationObjectId, AccessShareLock);

	rel->min_attr = FirstLowInvalidHeapAttributeNumber + 1;
	rel->max_attr = RelationGetNumberOfAttributes(relation);

	/*
	 * Make list of indexes.  Ignore indexes on system catalogs if told
	 * to.
	 */
	if (IsIgnoringSystemIndexes() && IsSystemClass(relation->rd_rel))
		hasindex = false;
	else
		hasindex = relation->rd_rel->relhasindex;

	if (hasindex)
	{
		List	   *indexoidlist,
				   *indexoidscan;

		indexoidlist = RelationGetIndexList(relation);

		foreach(indexoidscan, indexoidlist)
		{
			Oid			indexoid = lfirsto(indexoidscan);
			Relation	indexRelation;
			Form_pg_index index;
			IndexOptInfo *info;
			int			ncolumns;
			int			i;
			int16		amorderstrategy;

			/* Extract info from the relation descriptor for the index */
			indexRelation = index_open(indexoid);
			index = indexRelation->rd_index;

			info = makeNode(IndexOptInfo);

			info->indexoid = index->indexrelid;
			info->ncolumns = ncolumns = index->indnatts;

			/*
			 * Need to make classlist and ordering arrays large enough to
			 * put a terminating 0 at the end of each one.
			 */
			info->indexkeys = (int *) palloc(sizeof(int) * ncolumns);
			info->classlist = (Oid *) palloc0(sizeof(Oid) * (ncolumns + 1));
			info->ordering = (Oid *) palloc0(sizeof(Oid) * (ncolumns + 1));

			for (i = 0; i < ncolumns; i++)
			{
				info->classlist[i] = index->indclass[i];
				info->indexkeys[i] = index->indkey[i];
			}

			info->relam = indexRelation->rd_rel->relam;
			info->pages = indexRelation->rd_rel->relpages;
			info->tuples = indexRelation->rd_rel->reltuples;
			info->amcostestimate = index_cost_estimator(indexRelation);

			/*
			 * Fetch the ordering operators associated with the index, if
			 * any.
			 */
			amorderstrategy = indexRelation->rd_am->amorderstrategy;
			if (amorderstrategy != 0)
			{
				int			oprindex = amorderstrategy - 1;

				for (i = 0; i < ncolumns; i++)
				{
					info->ordering[i] = indexRelation->rd_operator[oprindex];
					oprindex += indexRelation->rd_am->amstrategies;
				}
			}

			/*
			 * Fetch the index expressions and predicate, if any.  We must
			 * modify the copies we obtain from the relcache to have the
			 * correct varno for the parent relation, so that they match
			 * up correctly against qual clauses.
			 */
			info->indexprs = RelationGetIndexExpressions(indexRelation);
			info->indpred = RelationGetIndexPredicate(indexRelation);
			if (info->indexprs && varno != 1)
				ChangeVarNodes((Node *) info->indexprs, 1, varno, 0);
			if (info->indpred && varno != 1)
				ChangeVarNodes((Node *) info->indpred, 1, varno, 0);
			info->unique = index->indisunique;

			/* initialize cached join info to empty */
			info->outer_relids = NULL;
			info->inner_paths = NIL;

			index_close(indexRelation);

			indexinfos = lcons(info, indexinfos);
		}

		freeList(indexoidlist);
	}

	rel->indexlist = indexinfos;

	rel->pages = relation->rd_rel->relpages;
	rel->tuples = relation->rd_rel->reltuples;

	/* XXX keep the lock here? */
	heap_close(relation, AccessShareLock);
}

/*
 * build_physical_tlist
 *
 * Build a targetlist consisting of exactly the relation's user attributes,
 * in order.  The executor can special-case such tlists to avoid a projection
 * step at runtime, so we use such tlists preferentially for scan nodes.
 *
 * Exception: if there are any dropped columns, we punt and return NIL.
 * Ideally we would like to handle the dropped-column case too.  However this
 * creates problems for ExecTypeFromTL, which may be asked to build a tupdesc
 * for a tlist that includes vars of no-longer-existent types.	In theory we
 * could dig out the required info from the pg_attribute entries of the
 * relation, but that data is not readily available to ExecTypeFromTL.
 * For now, we don't apply the physical-tlist optimization when there are
 * dropped cols.
 */
List *
build_physical_tlist(Query *root, RelOptInfo *rel)
{
	Index		varno = rel->relid;
	RangeTblEntry *rte = rt_fetch(varno, root->rtable);
	Relation	relation;
	FastList	tlist;
	int			attrno,
				numattrs;

	FastListInit(&tlist);

	Assert(rte->rtekind == RTE_RELATION);

	relation = heap_open(rte->relid, AccessShareLock);

	numattrs = RelationGetNumberOfAttributes(relation);

	for (attrno = 1; attrno <= numattrs; attrno++)
	{
		Form_pg_attribute att_tup = relation->rd_att->attrs[attrno - 1];

		if (att_tup->attisdropped)
		{
			/* found a dropped col, so punt */
			FastListInit(&tlist);
			break;
		}

		FastAppend(&tlist,
				   create_tl_element(makeVar(varno,
											 attrno,
											 att_tup->atttypid,
											 att_tup->atttypmod,
											 0),
									 attrno));
	}

	heap_close(relation, AccessShareLock);

	return FastListValue(&tlist);
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
		elog(ERROR, "invalid restriction selectivity: %f", result);

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
		elog(ERROR, "invalid join selectivity: %f", result);

	return (Selectivity) result;
}

/*
 * find_inheritance_children
 *
 * Returns a list containing the OIDs of all relations which
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
		list = lappendo(list, inhrelid);
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
		elog(ERROR, "cache lookup failed for relation %u", relationId);

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
		 * Note: ignore partial indexes, since they don't allow us to
		 * conclude that all attr values are distinct.	We don't take any
		 * interest in expressional indexes either. Also, a multicolumn
		 * unique index doesn't allow us to conclude that just the
		 * specified attr is unique.
		 */
		if (index->unique &&
			index->ncolumns == 1 &&
			index->indexkeys[0] == attno &&
			index->indpred == NIL)
			return true;
	}
	return false;
}
