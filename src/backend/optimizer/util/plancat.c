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
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/plancat.c,v 1.27 1999/03/08 14:01:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include "postgres.h"

#include "access/heapam.h"
#include "access/genam.h"
#include "access/htup.h"
#include "access/itup.h"

#include "catalog/catname.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_version.h"

#include "parser/parsetree.h"	/* for getrelid() */
#include "fmgr.h"

#include "optimizer/internal.h"
#include "optimizer/plancat.h"

#include "utils/syscache.h"
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif


static void IndexSelectivity(Oid indexrelid, Oid indrelid, int32 nIndexKeys,
				 Oid *AccessMethodOperatorClasses, Oid *operatorObjectIds,
	   int32 *varAttributeNumbers, char **constValues, int32 *constFlags,
				 float *idxPages, float *idxSelec);


/*
 * relation_info -
 *	  Retrieves catalog information for a given relation. Given the oid of
 *	  the relation, return the following information:
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

		*hasindex = (relation->relhasindex) ? TRUE : FALSE;
		*pages = relation->relpages;
		*tuples = relation->reltuples;
	}
	else
	{
		elog(ERROR, "RelationCatalogInformation: Relation %d not found",
			 relationObjectId);
	}

	return;
}


/*
 * index_info
 *	  Retrieves catalog information on an index on a given relation.
 *
 *	  The index relation is opened on the first invocation. The current
 *	  retrieves the next index relation within the catalog that has not
 *	  already been retrieved by a previous call.  The index catalog
 *	  is closed when no more indices for 'relid' can be found.
 *
 * 'first' is 1 if this is the first call
 *
 * Returns true if successful and false otherwise. Index info is returned
 * via the transient data structure 'info'.
 *
 */
bool
index_info(Query *root, bool first, int relid, IdxInfoRetval *info)
{
	int			i;
	HeapTuple	indexTuple,
				amopTuple;
	Form_pg_index index;
	Relation	indexRelation;
	uint16		amstrategy;
	Oid			relam;
	Oid			indrelid;

	static Relation relation = (Relation) NULL;
	static HeapScanDesc scan = (HeapScanDesc) NULL;
	static ScanKeyData indexKey;


	/* find the oid of the indexed relation */
	indrelid = getrelid(relid, root->rtable);

	MemSet(info, 0, sizeof(IdxInfoRetval));

	/*
	 * the maximum number of elements in each of the following arrays is
	 * 8. We allocate one more for a terminating 0 to indicate the end of
	 * the array.
	 */
	info->indexkeys = (int *) palloc(sizeof(int) * 9);
	MemSet(info->indexkeys, 0, sizeof(int) * 9);
	info->orderOprs = (Oid *) palloc(sizeof(Oid) * 9);
	MemSet(info->orderOprs, 0, sizeof(Oid) * 9);
	info->classlist = (Oid *) palloc(sizeof(Oid) * 9);
	MemSet(info->classlist, 0, sizeof(Oid) * 9);

	/* Find an index on the given relation */
	if (first)
	{
		if (RelationIsValid(relation))
			heap_close(relation);
		if (HeapScanIsValid(scan))
			heap_endscan(scan);

		ScanKeyEntryInitialize(&indexKey, 0,
							   Anum_pg_index_indrelid,
							   F_OIDEQ,
							   ObjectIdGetDatum(indrelid));

		relation = heap_openr(IndexRelationName);
		scan = heap_beginscan(relation, 0, SnapshotNow,
							  1, &indexKey);
	}
	if (!HeapScanIsValid(scan))
		elog(ERROR, "index_info: scan not started");
	indexTuple = heap_getnext(scan, 0);
	if (!HeapTupleIsValid(indexTuple))
	{
		heap_endscan(scan);
		heap_close(relation);
		scan = (HeapScanDesc) NULL;
		relation = (Relation) NULL;
		return 0;
	}

	/* Extract info from the index tuple */
	index = (Form_pg_index) GETSTRUCT(indexTuple);
	info->relid = index->indexrelid;	/* index relation */
	for (i = 0; i < INDEX_MAX_KEYS; i++)
		info->indexkeys[i] = index->indkey[i];
	for (i = 0; i < INDEX_MAX_KEYS; i++)
		info->classlist[i] = index->indclass[i];

	info->indproc = index->indproc;		/* functional index ?? */

	/* partial index ?? */
	if (VARSIZE(&index->indpred) != 0)
	{

		/*
		 * The memory allocated here for the predicate (in lispReadString)
		 * only needs to stay around until it's used in find_index_paths,
		 * which is all within a command, so the automatic pfree at end of
		 * transaction should be ok.
		 */
		char	   *predString;

		predString = fmgr(F_TEXTOUT, &index->indpred);
		info->indpred = (Node *) stringToNode(predString);
		pfree(predString);
	}

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
	heap_close(indexRelation);

	/*
	 * Find the index ordering keys
	 *
	 * Must use indclass to know when to stop looking since with functional
	 * indices there could be several keys (args) for one opclass. -mer 27
	 * Sept 1991
	 */
	for (i = 0; i < 8 && index->indclass[i]; ++i)
	{
		amopTuple = SearchSysCacheTuple(AMOPSTRATEGY,
										ObjectIdGetDatum(relam),
									ObjectIdGetDatum(index->indclass[i]),
										UInt16GetDatum(amstrategy),
										0);
		if (!HeapTupleIsValid(amopTuple))
			elog(ERROR, "index_info: no amop %d %d %d",
				 relam, index->indclass[i], amstrategy);
		info->orderOprs[i] = ((Form_pg_amop) GETSTRUCT(amopTuple))->amopopr;
	}
	return TRUE;
}

/*
 * index_selectivity
 *
 *	  Call util/plancat.c:IndexSelectivity with the indicated arguments.
 *
 * 'indid' is the index OID
 * 'classes' is a list of index key classes
 * 'opnos' is a list of index key operator OIDs
 * 'relid' is the OID of the relation indexed
 * 'attnos' is a list of the relation attnos which the index keys over
 * 'values' is a list of the values of the clause's constants
 * 'flags' is a list of fixnums which describe the constants
 * 'nkeys' is the number of index keys
 *
 * Returns two floats: index pages and index selectivity in 'idxPages' and
 *		'idxSelec'.
 *
 */
void
index_selectivity(Oid indid,
				  Oid *classes,
				  List *opnos,
				  Oid relid,
				  List *attnos,
				  List *values,
				  List *flags,
				  int32 nkeys,
				  float *idxPages,
				  float *idxSelec)
{
	Oid		   *opno_array;
	int		   *attno_array,
			   *flag_array;
	char	  **value_array;
	int			i = 0;
	List	   *xopno,
			   *xattno,
			   *value,
			   *flag;

	if (length(opnos) != nkeys || length(attnos) != nkeys ||
		length(values) != nkeys || length(flags) != nkeys)
	{

		*idxPages = 0.0;
		*idxSelec = 1.0;
		return;
	}

	opno_array = (Oid *) palloc(nkeys * sizeof(Oid));
	attno_array = (int *) palloc(nkeys * sizeof(int32));
	value_array = (char **) palloc(nkeys * sizeof(char *));
	flag_array = (int *) palloc(nkeys * sizeof(int32));

	i = 0;
	foreach(xopno, opnos)
		opno_array[i++] = lfirsti(xopno);

	i = 0;
	foreach(xattno, attnos)
		attno_array[i++] = lfirsti(xattno);

	i = 0;
	foreach(value, values)
		value_array[i++] = (char *) lfirst(value);

	i = 0;
	foreach(flag, flags)
		flag_array[i++] = lfirsti(flag);

	IndexSelectivity(indid,
					 relid,
					 nkeys,
					 classes,	/* not used */
					 opno_array,
					 attno_array,
					 value_array,
					 flag_array,
					 idxPages,
					 idxSelec);
	return;
}

/*
 * restriction_selectivity in lisp system.
 *
 *	  NOTE: The routine is now merged with RestrictionClauseSelectivity
 *	  as defined in plancat.c
 *
 * Returns the selectivity of a specified operator.
 * This code executes registered procedures stored in the
 * operator relation, by calling the function manager.
 *
 * XXX The assumption in the selectivity procedures is that if the
 *		relation OIDs or attribute numbers are -1, then the clause
 *		isn't of the form (op var const).
 */
Cost
restriction_selectivity(Oid functionObjectId,
						Oid operatorObjectId,
						Oid relationObjectId,
						AttrNumber attributeNumber,
						char *constValue,
						int32 constFlag)
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
 *		relation OIDs or attribute numbers are -1, then the clause
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
 * find_all_inheritors
 *
 * Returns a LISP list containing the OIDs of all relations which
 * inherits from the relation with OID 'inhparent'.
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

	key[0].sk_argument = ObjectIdGetDatum((Oid) inhparent);
	relation = heap_openr(InheritsRelationName);
	scan = heap_beginscan(relation, 0, SnapshotNow, 1, key);
	while (HeapTupleIsValid(inheritsTuple = heap_getnext(scan, 0)))
	{
		inhrelid = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhrel;
		list = lappendi(list, inhrelid);
	}
	heap_endscan(scan);
	heap_close(relation);
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
	relation = heap_openr(VersionRelationName);
	key[0].sk_argument = ObjectIdGetDatum(verrelid);
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
	heap_close(relation);
	return list;
}
#endif


/*****************************************************************************
 *
 *****************************************************************************/

/*
 * IdexSelectivity
 *
 *	  Retrieves the 'amopnpages' and 'amopselect' parameters for each
 *	  AM operator when a given index (specified by 'indexrelid') is used.
 *	  These two parameters are returned by copying them to into an array of
 *	  floats.
 *
 *	  Assumption: the attribute numbers and operator ObjectIds are in order
 *	  WRT to each other (otherwise, you have no way of knowing which
 *	  AM operator class or attribute number corresponds to which operator.
 *
 * 'varAttributeNumbers' contains attribute numbers for variables
 * 'constValues' contains the constant values
 * 'constFlags' describes how to treat the constants in each clause
 * 'nIndexKeys' describes how many keys the index actually has
 *
 * Returns 'selectivityInfo' filled with the sum of all pages touched
 * and the product of each clause's selectivity.
 *
 */
static void
IndexSelectivity(Oid indexrelid,
				 Oid indrelid,
				 int32 nIndexKeys,
				 Oid *AccessMethodOperatorClasses,		/* XXX not used? */
				 Oid *operatorObjectIds,
				 int32 *varAttributeNumbers,
				 char **constValues,
				 int32 *constFlags,
				 float *idxPages,
				 float *idxSelec)
{
	int			i,
				n;
	HeapTuple	indexTuple,
				amopTuple,
				indRel;
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
		elog(ERROR, "IndexSelectivity: index %d not found",
			 indexrelid);
	relam = ((Form_pg_class) GETSTRUCT(indRel))->relam;

	indexTuple = SearchSysCacheTuple(INDEXRELID,
									 ObjectIdGetDatum(indexrelid),
									 0, 0, 0);
	if (!HeapTupleIsValid(indexTuple))
		elog(ERROR, "IndexSelectivity: index %d not found",
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
	for (n = 0; n < nIndexKeys; ++n)
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
			elog(NOTICE, "IndexSelectivity: no key %d in index %d",
				 varAttributeNumbers[n], indexrelid);
			continue;
		}

		amopTuple = SearchSysCacheTuple(AMOPOPID,
										ObjectIdGetDatum(indclass),
								  ObjectIdGetDatum(operatorObjectIds[n]),
										ObjectIdGetDatum(relam),
										0);
		if (!HeapTupleIsValid(amopTuple))
			elog(ERROR, "IndexSelectivity: no amop %d %d",
				 indclass, operatorObjectIds[n]);
		amop = (Form_pg_amop) GETSTRUCT(amopTuple);

		if (!nphack)
		{
			amopnpages = (float64) fmgr(amop->amopnpages,
										(char *) operatorObjectIds[n],
										(char *) indrelid,
										(char *) varAttributeNumbers[n],
										(char *) constValues[n],
										(char *) constFlags[n],
										(char *) nIndexKeys,
										(char *) indexrelid);
#ifdef NOT_USED
/*
 * So cool guys! Npages for x > 10 and x < 20 is twice as
 * npages for x > 10!	- vadim 04/09/97
 */
			npages += PointerIsValid(amopnpages) ? *amopnpages : 0.0;
			if ((i = npages) < npages)	/* ceil(npages)? */
				npages += 1.0;
#endif
			npages += PointerIsValid(amopnpages) ? *amopnpages : 0.0;
		}

		amopselect = (float64) fmgr(amop->amopselect,
									(char *) operatorObjectIds[n],
									(char *) indrelid,
									(char *) varAttributeNumbers[n],
									(char *) constValues[n],
									(char *) constFlags[n],
									(char *) nIndexKeys,
									(char *) indexrelid);

		if (nphack && varAttributeNumbers[n] == index->indkey[0])
			fattr_select *= PointerIsValid(amopselect) ? *amopselect : 1.0;

		select *= PointerIsValid(amopselect) ? *amopselect : 1.0;
	}

	/*
	 * Estimation of npages below is hack of course, but it's better than
	 * it was before.		- vadim 04/09/97
	 */
	if (nphack)
	{
		npages = fattr_select * ((Form_pg_class) GETSTRUCT(indRel))->relpages;
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
