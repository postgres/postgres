/*-------------------------------------------------------------------------
 *
 * plancat.c
 *	   routines for accessing the system catalogs
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/plancat.c,v 1.43 2000/01/12 00:53:21 tgl Exp $
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
#include "optimizer/clauses.h"
#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "parser/parsetree.h"
#include "utils/syscache.h"


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
	List	   *indexes = NIL;
	Oid			indrelid = getrelid(relid, root->rtable);
	Relation	relation;
	HeapScanDesc scan;
	ScanKeyData	indexKey;
	HeapTuple	indexTuple;

	/* Scan pg_index for tuples describing indexes of this rel */
	relation = heap_openr(IndexRelationName, AccessShareLock);

	ScanKeyEntryInitialize(&indexKey, 0,
						   Anum_pg_index_indrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(indrelid));

	scan = heap_beginscan(relation, 0, SnapshotNow,
						  1, &indexKey);

	while (HeapTupleIsValid(indexTuple = heap_getnext(scan, 0)))
	{
		Form_pg_index	index = (Form_pg_index) GETSTRUCT(indexTuple);
		IndexOptInfo   *info = makeNode(IndexOptInfo);
		int				i;
		Relation		indexRelation;
		uint16			amstrategy;
		Oid				relam;

		/*
		 * Need to make these arrays large enough to be sure there is a
		 * terminating 0 at the end of each one.
		 */
		info->classlist = (Oid *) palloc(sizeof(Oid) * (INDEX_MAX_KEYS+1));
		info->indexkeys = (int *) palloc(sizeof(int) * (INDEX_MAX_KEYS+1));
		info->ordering = (Oid *) palloc(sizeof(Oid) * (INDEX_MAX_KEYS+1));

		/* Extract info from the pg_index tuple */
		info->indexoid = index->indexrelid;
		info->indproc = index->indproc;		/* functional index ?? */
		if (VARSIZE(&index->indpred) != 0)	/* partial index ?? */
		{
			char	   *predString = fmgr(F_TEXTOUT, &index->indpred);
			info->indpred = (List *) stringToNode(predString);
			pfree(predString);
		}
		else
			info->indpred = NIL;

		for (i = 0; i < INDEX_MAX_KEYS; i++)
			info->indexkeys[i] = index->indkey[i];
		info->indexkeys[INDEX_MAX_KEYS] = 0;
		for (i = 0; i < INDEX_MAX_KEYS; i++)
			info->classlist[i] = index->indclass[i];
		info->classlist[INDEX_MAX_KEYS] = (Oid) 0;

		/* Extract info from the relation descriptor for the index */
		indexRelation = index_open(index->indexrelid);
#ifdef notdef
		/* XXX should iterate through strategies -- but how?  use #1 for now */
		amstrategy = indexRelation->rd_am->amstrategies;
#endif	 /* notdef */
		amstrategy = 1;
		relam = indexRelation->rd_rel->relam;
		info->relam = relam;
		info->pages = indexRelation->rd_rel->relpages;
		info->tuples = indexRelation->rd_rel->reltuples;
		index_close(indexRelation);

		/*
		 * Fetch the ordering operators associated with the index.
		 *
		 * XXX what if it's a hash or other unordered index?
		 */
		MemSet(info->ordering, 0, sizeof(Oid) * (INDEX_MAX_KEYS+1));
		for (i = 0; i < INDEX_MAX_KEYS && index->indclass[i]; i++)
		{
			HeapTuple		amopTuple;

			amopTuple = SearchSysCacheTuple(AMOPSTRATEGY,
										ObjectIdGetDatum(relam),
										ObjectIdGetDatum(index->indclass[i]),
										UInt16GetDatum(amstrategy),
										0);
			if (!HeapTupleIsValid(amopTuple))
				elog(ERROR, "find_secondary_indexes: no amop %u %u %d",
					 relam, index->indclass[i], amstrategy);
			info->ordering[i] = ((Form_pg_amop) GETSTRUCT(amopTuple))->amopopr;
		}

		indexes = lcons(info, indexes);
	}

	heap_endscan(scan);
	heap_close(relation, AccessShareLock);

	return indexes;
}

/*
 * index_selectivity
 *	  Estimate the selectivity of an index scan with the given index quals.
 *
 *	  NOTE: an indexscan plan node can actually represent several passes,
 *	  but here we consider the cost of just one pass.
 *
 * 'root' is the query root
 * 'rel' is the relation being scanned
 * 'index' is the index to be used
 * 'indexquals' is the list of qual condition exprs (implicit AND semantics)
 * '*idxPages' receives an estimate of the number of index pages touched
 * '*idxSelec' receives an estimate of selectivity of the scan, ie fraction
 *		of the relation's tuples that will be retrieved
 */
void
index_selectivity(Query *root,
				  RelOptInfo *rel,
				  IndexOptInfo *index,
				  List *indexquals,
				  long *idxPages,
				  Selectivity *idxSelec)
{
	int			relid;
	Oid			baserelid,
				indexrelid;
	HeapTuple	indRel,
				indexTuple;
	Form_pg_class indexrelation;
	Oid			relam;
	Form_pg_index pgindex;
	int			nIndexKeys;
	float64data npages,
				select,
				fattr_select;
	bool		nphack = false;
	List	   *q;

	Assert(length(rel->relids) == 1); /* must be a base rel */
	relid = lfirsti(rel->relids);

	baserelid = getrelid(relid, root->rtable);
	indexrelid = index->indexoid;

	indRel = SearchSysCacheTuple(RELOID,
								 ObjectIdGetDatum(indexrelid),
								 0, 0, 0);
	if (!HeapTupleIsValid(indRel))
		elog(ERROR, "index_selectivity: index %u not found in pg_class",
			 indexrelid);
	indexrelation = (Form_pg_class) GETSTRUCT(indRel);
	relam = indexrelation->relam;

	indexTuple = SearchSysCacheTuple(INDEXRELID,
									 ObjectIdGetDatum(indexrelid),
									 0, 0, 0);
	if (!HeapTupleIsValid(indexTuple))
		elog(ERROR, "index_selectivity: index %u not found in pg_index",
			 indexrelid);
	pgindex = (Form_pg_index) GETSTRUCT(indexTuple);

	nIndexKeys = 1;
	while (pgindex->indclass[nIndexKeys] != InvalidOid)
		nIndexKeys++;

	/*
	 * Hack for non-functional btree npages estimation: npages =
	 * index_pages * selectivity_of_1st_attr_clause(s) - vadim 04/24/97
	 */
	if (relam == BTREE_AM_OID && pgindex->indproc == InvalidOid)
		nphack = true;

	npages = 0.0;
	select = 1.0;
	fattr_select = 1.0;

	foreach(q, indexquals)
	{
		Node	   *expr = (Node *) lfirst(q);
		Oid			opno;
		int			dummyrelid;
		AttrNumber	attno;
		Datum		value;
		int			flag;
		Oid			indclass;
		HeapTuple	amopTuple;
		Form_pg_amop amop;
		float64		amopnpages,
					amopselect;

		/*
		 * Extract info from clause.
		 */
		if (is_opclause(expr))
			opno = ((Oper *) ((Expr *) expr)->oper)->opno;
		else
			opno = InvalidOid;
		get_relattval(expr, relid, &dummyrelid, &attno, &value, &flag);

		/*
		 * Find the AM class for this key.
		 */
		if (pgindex->indproc != InvalidOid)
		{
			/*
			 * Functional index: AM class is the first one defined since
			 * functional indices have exactly one key.
			 */
			indclass = pgindex->indclass[0];
		}
		else
		{
			int			i;
			indclass = InvalidOid;
			for (i = 0; pgindex->indkey[i]; i++)
			{
				if (attno == pgindex->indkey[i])
				{
					indclass = pgindex->indclass[i];
					break;
				}
			}
		}
		if (!OidIsValid(indclass))
		{
			/*
			 * Presumably this means that we are using a functional index
			 * clause and so had no variable to match to the index key ...
			 * if not we are in trouble.
			 */
			elog(NOTICE, "index_selectivity: no key %d in index %u",
				 attno, indexrelid);
			continue;
		}

		amopTuple = SearchSysCacheTuple(AMOPOPID,
										ObjectIdGetDatum(indclass),
										ObjectIdGetDatum(opno),
										ObjectIdGetDatum(relam),
										0);
		if (!HeapTupleIsValid(amopTuple))
		{
			/*
			 * We might get here because indxpath.c selected a binary-
			 * compatible index.  Try again with the compatible operator.
			 */
			if (opno != InvalidOid)
			{
				opno = indexable_operator((Expr *) expr, indclass, relam,
										  ((flag & SEL_RIGHT) != 0));
				amopTuple = SearchSysCacheTuple(AMOPOPID,
												ObjectIdGetDatum(indclass),
												ObjectIdGetDatum(opno),
												ObjectIdGetDatum(relam),
												0);
			}
			if (!HeapTupleIsValid(amopTuple))
				elog(ERROR, "index_selectivity: no amop %u %u %u",
					 indclass, opno, relam);
		}
		amop = (Form_pg_amop) GETSTRUCT(amopTuple);

		if (!nphack)
		{
			amopnpages = (float64) fmgr(amop->amopnpages,
										(char *) opno,
										(char *) baserelid,
										(char *) (int) attno,
										(char *) value,
										(char *) flag,
										(char *) nIndexKeys,
										(char *) indexrelid);
			if (PointerIsValid(amopnpages))
				npages += *amopnpages;
		}

		amopselect = (float64) fmgr(amop->amopselect,
									(char *) opno,
									(char *) baserelid,
									(char *) (int) attno,
									(char *) value,
									(char *) flag,
									(char *) nIndexKeys,
									(char *) indexrelid);
		if (PointerIsValid(amopselect))
		{
			select *= *amopselect;
			if (nphack && attno == pgindex->indkey[0])
				fattr_select *= *amopselect;
		}
	}

	/*
	 * Estimation of npages below is hack of course, but it's better than
	 * it was before.		- vadim 04/09/97
	 */
	if (nphack)
	{
		npages = fattr_select * indexrelation->relpages;
		*idxPages = (long) ceil((double) npages);
	}
	else
	{
		if (nIndexKeys > 1)
			npages = npages / (1.0 + nIndexKeys);
		*idxPages = (long) ceil((double) (npages / nIndexKeys));
	}
	*idxSelec = select;
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
	float64		result;

	result = (float64) fmgr(functionObjectId,
							(char *) operatorObjectId,
							(char *) relationObjectId,
							(char *) (int) attributeNumber,
							(char *) constValue,
							(char *) constFlag,
							NULL);
	if (!PointerIsValid(result))
		elog(ERROR, "restriction_selectivity: bad pointer");

	if (*result < 0.0 || *result > 1.0)
		elog(ERROR, "restriction_selectivity: bad value %lf", *result);

	return (Selectivity) *result;
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
	float64		result;

	result = (float64) fmgr(functionObjectId,
							(char *) operatorObjectId,
							(char *) relationObjectId1,
							(char *) (int) attributeNumber1,
							(char *) relationObjectId2,
							(char *) (int) attributeNumber2,
							NULL);
	if (!PointerIsValid(result))
		elog(ERROR, "join_selectivity: bad pointer");

	if (*result < 0.0 || *result > 1.0)
		elog(ERROR, "join_selectivity: bad value %lf", *result);

	return (Selectivity) *result;
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
