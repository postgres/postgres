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
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/plancat.c,v 1.39 1999/11/21 23:25:47 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_inherits.h"
#include "optimizer/clauses.h"
#include "optimizer/internal.h"
#include "optimizer/plancat.h"
#include "parser/parsetree.h"
#include "utils/syscache.h"


static void IndexSelectivity(Oid indexrelid, Oid baserelid, int nIndexKeys,
							 Oid *operatorObjectIds,
							 AttrNumber *varAttributeNumbers,
							 Datum *constValues,
							 int *constFlags,
							 float *idxPages,
							 float *idxSelec);


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
			  bool *hasindex, int *pages, int *tuples)
{
	HeapTuple	relationTuple;
	Form_pg_class relation;
	Oid			relationObjectId;

	relationObjectId = getrelid(relid, root->rtable);
	relationTuple = SearchSysCacheTuple(RELOID,
										ObjectIdGetDatum(relationObjectId),
										0, 0, 0);
	if (HeapTupleIsValid(relationTuple))
	{
		relation = (Form_pg_class) GETSTRUCT(relationTuple);

		*hasindex = (relation->relhasindex) ? true : false;
		*pages = relation->relpages;
		*tuples = relation->reltuples;
	}
	else
	{
		elog(ERROR, "relation_info: Relation %u not found",
			 relationObjectId);
	}
}

/*
 * find_secondary_indexes
 *	  Creates a list of RelOptInfo nodes containing information for each
 *	  secondary index defined on the given relation.
 *
 * 'relid' is the RT index of the relation for which indices are being located
 *
 * Returns a list of new index RelOptInfo nodes.
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
		RelOptInfo	   *info = makeNode(RelOptInfo);
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
		info->relids = lconsi(index->indexrelid, NIL);
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

		info->indexed = false;		/* not indexed itself */
		info->size = 0;
		info->width = 0;
		info->targetlist = NIL;
		info->pathlist = NIL;
		info->cheapestpath = NULL;
		info->pruneable = true;
		info->restrictinfo = NIL;
		info->joininfo = NIL;
		info->innerjoin = NIL;

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
 * 'relid' is the RT index of the relation being scanned
 * 'indexid' is the OID of the index to be used
 * 'indexquals' is the list of qual condition exprs (implicit AND semantics)
 * '*idxPages' receives an estimate of the number of index pages touched
 * '*idxSelec' receives an estimate of selectivity of the scan
 */
void
index_selectivity(Query *root,
				  int relid,
				  Oid indexid,
				  List *indexquals,
				  float *idxPages,
				  float *idxSelec)
{
	int			nclauses = length(indexquals);
	Oid		   *opno_array;
	AttrNumber *attno_array;
	Datum	   *value_array;
	int		   *flag_array;
	List	   *q;
	int			i;

	if (nclauses <= 0)
	{
		*idxPages = 0.0;
		*idxSelec = 1.0;
		return;
	}
	opno_array = (Oid *) palloc(nclauses * sizeof(Oid));
	attno_array = (AttrNumber *) palloc(nclauses * sizeof(AttrNumber));
	value_array = (Datum *) palloc(nclauses * sizeof(Datum));
	flag_array = (int *) palloc(nclauses * sizeof(int));

	i = 0;
	foreach(q, indexquals)
	{
		Node	   *expr = (Node *) lfirst(q);
		int			dummyrelid;

		if (is_opclause(expr))
			opno_array[i] = ((Oper *) ((Expr *) expr)->oper)->opno;
		else
			opno_array[i] = InvalidOid;

		get_relattval(expr, relid, &dummyrelid, &attno_array[i],
					  &value_array[i], &flag_array[i]);
		i++;
	}

	IndexSelectivity(indexid,
					 getrelid(relid, root->rtable),
					 nclauses,
					 opno_array,
					 attno_array,
					 value_array,
					 flag_array,
					 idxPages,
					 idxSelec);

	pfree(opno_array);
	pfree(attno_array);
	pfree(value_array);
	pfree(flag_array);
}

/*
 * restriction_selectivity
 *
 *	  NOTE: The routine is now merged with RestrictionClauseSelectivity
 *	  as defined in plancat.c
 *
 * Returns the selectivity of a specified operator.
 * This code executes registered procedures stored in the
 * operator relation, by calling the function manager.
 *
 * XXX The assumption in the selectivity procedures is that if the
 *		relation OIDs or attribute numbers are 0, then the clause
 *		isn't of the form (op var const).
 */
Cost
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
		elog(ERROR, "RestrictionClauseSelectivity: bad pointer");

	if (*result < 0.0 || *result > 1.0)
		elog(ERROR, "RestrictionClauseSelectivity: bad value %lf",
			 *result);

	return (Cost) *result;
}

/*
 * join_selectivity
 *	  Similarly, this routine is merged with JoinClauseSelectivity in
 *	  plancat.c
 *
 *	  Returns the selectivity of an operator, given the join clause
 *	  information.
 *
 * XXX The assumption in the selectivity procedures is that if the
 *		relation OIDs or attribute numbers are 0, then the clause
 *		isn't of the form (op var var).
 */
Cost
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
		elog(ERROR, "JoinClauseSelectivity: bad pointer");

	if (*result < 0.0 || *result > 1.0)
		elog(ERROR, "JoinClauseSelectivity: bad value %lf",
			 *result);

	return (Cost) *result;
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
		inhrelid = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhrel;
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


/*****************************************************************************
 *
 *****************************************************************************/

/*
 * IndexSelectivity
 *
 *	  Calls the 'amopnpages' and 'amopselect' functions for each
 *	  AM operator when a given index (specified by 'indexrelid') is used.
 *	  The total number of pages and product of the selectivities are returned.
 *
 *	  Assumption: the attribute numbers and operator ObjectIds are in order
 *	  WRT to each other (otherwise, you have no way of knowing which
 *	  AM operator class or attribute number corresponds to which operator.
 *
 * 'nIndexKeys' is the number of qual clauses in use
 * 'varAttributeNumbers' contains attribute numbers for variables
 * 'constValues' contains the constant values
 * 'constFlags' describes how to treat the constants in each clause
 */
static void
IndexSelectivity(Oid indexrelid,
				 Oid baserelid,
				 int nIndexKeys,
				 Oid *operatorObjectIds,
				 AttrNumber *varAttributeNumbers,
				 Datum *constValues,
				 int *constFlags,
				 float *idxPages,
				 float *idxSelec)
{
	int			i,
				n;
	HeapTuple	indexTuple,
				amopTuple,
				indRel;
	Form_pg_class indexrelation;
	Form_pg_index index;
	Form_pg_amop amop;
	Oid			indclass;
	float64data npages,
				select;
	float64		amopnpages,
				amopselect;
	Oid			relam;
	bool		nphack = false;
	float64data fattr_select = 1.0;

	indRel = SearchSysCacheTuple(RELOID,
								 ObjectIdGetDatum(indexrelid),
								 0, 0, 0);
	if (!HeapTupleIsValid(indRel))
		elog(ERROR, "IndexSelectivity: index %u not found",
			 indexrelid);
	indexrelation = (Form_pg_class) GETSTRUCT(indRel);
	relam = indexrelation->relam;

	indexTuple = SearchSysCacheTuple(INDEXRELID,
									 ObjectIdGetDatum(indexrelid),
									 0, 0, 0);
	if (!HeapTupleIsValid(indexTuple))
		elog(ERROR, "IndexSelectivity: index %u not found",
			 indexrelid);
	index = (Form_pg_index) GETSTRUCT(indexTuple);

	/*
	 * Hack for non-functional btree npages estimation: npages =
	 * index_pages * selectivity_of_1st_attr_clause(s) - vadim 04/24/97
	 */
	if (relam == BTREE_AM_OID &&
		varAttributeNumbers[0] != InvalidAttrNumber)
		nphack = true;

	npages = 0.0;
	select = 1.0;
	for (n = 0; n < nIndexKeys; n++)
	{
		/*
		 * Find the AM class for this key.
		 *
		 * If the first attribute number is invalid then we have a functional
		 * index, and AM class is the first one defined since functional
		 * indices have exactly one key.
		 */
		indclass = (varAttributeNumbers[0] == InvalidAttrNumber) ?
			index->indclass[0] : InvalidOid;
		i = 0;
		while ((i < nIndexKeys) && (indclass == InvalidOid))
		{
			if (varAttributeNumbers[n] == index->indkey[i])
			{
				indclass = index->indclass[i];
				break;
			}
			i++;
		}
		if (!OidIsValid(indclass))
		{

			/*
			 * Presumably this means that we are using a functional index
			 * clause and so had no variable to match to the index key ...
			 * if not we are in trouble.
			 */
			elog(NOTICE, "IndexSelectivity: no key %d in index %u",
				 varAttributeNumbers[n], indexrelid);
			continue;
		}

		amopTuple = SearchSysCacheTuple(AMOPOPID,
										ObjectIdGetDatum(indclass),
										ObjectIdGetDatum(operatorObjectIds[n]),
										ObjectIdGetDatum(relam),
										0);
		if (!HeapTupleIsValid(amopTuple))
			elog(ERROR, "IndexSelectivity: no amop %u %u %u",
				 indclass, operatorObjectIds[n], relam);
		amop = (Form_pg_amop) GETSTRUCT(amopTuple);

		if (!nphack)
		{
			amopnpages = (float64) fmgr(amop->amopnpages,
										(char *) operatorObjectIds[n],
										(char *) baserelid,
										(char *) (int) varAttributeNumbers[n],
										(char *) constValues[n],
										(char *) constFlags[n],
										(char *) nIndexKeys,
										(char *) indexrelid);
			if (PointerIsValid(amopnpages))
				npages += *amopnpages;
		}

		amopselect = (float64) fmgr(amop->amopselect,
									(char *) operatorObjectIds[n],
									(char *) baserelid,
									(char *) (int) varAttributeNumbers[n],
									(char *) constValues[n],
									(char *) constFlags[n],
									(char *) nIndexKeys,
									(char *) indexrelid);

		if (PointerIsValid(amopselect))
		{
			select *= *amopselect;
			if (nphack && varAttributeNumbers[n] == index->indkey[0])
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
		*idxPages = ceil((double) npages);
	}
	else
	{
		if (nIndexKeys > 1)
			npages = npages / (1.0 + nIndexKeys);
		*idxPages = ceil((double) (npages / nIndexKeys));
	}
	*idxSelec = select;
}
