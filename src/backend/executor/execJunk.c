/*-------------------------------------------------------------------------
 *
 * junk.c--
 *	  Junk attribute support stuff....
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execJunk.c,v 1.8 1997/09/08 21:42:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>

#include "postgres.h"

#include "utils/palloc.h"
#include "access/heapam.h"
#include "executor/executor.h"
#include "nodes/relation.h"
#include "optimizer/tlist.h"	/* for MakeTLE */

/*-------------------------------------------------------------------------
 *		XXX this stuff should be rewritten to take advantage
 *			of ExecProject() and the ProjectionInfo node.
 *			-cim 6/3/91
 *
 * An attribute of a tuple living inside the executor, can be
 * either a normal attribute or a "junk" attribute. "junk" attributes
 * never make it out of the executor, i.e. they are never printed,
 * returned or stored in disk. Their only purpose in life is to
 * store some information useful only to the executor, mainly the values
 * of some system attributes like "ctid" or rule locks.
 *
 * The general idea is the following: A target list consists of a list of
 * Resdom nodes & expression pairs. Each Resdom node has an attribute
 * called 'resjunk'. If the value of this attribute is 1 then the
 * corresponding attribute is a "junk" attribute.
 *
 * When we initialize a plan  we call 'ExecInitJunkFilter' to create
 * and store the appropriate information in the 'es_junkFilter' attribute of
 * EState.
 *
 * We then execute the plan ignoring the "resjunk" attributes.
 *
 * Finally, when at the top level we get back a tuple, we can call
 * 'ExecGetJunkAttribute' to retrieve the value of the junk attributes we
 * are interested in, and 'ExecRemoveJunk' to remove all the junk attributes
 * from a tuple. This new "clean" tuple is then printed, replaced, deleted
 * or inserted.
 *
 *-------------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * ExecInitJunkFilter
 *
 * Initialize the Junk filter.
 *-------------------------------------------------------------------------
 */
JunkFilter *
ExecInitJunkFilter(List *targetList)
{
	JunkFilter *junkfilter;
	List	   *cleanTargetList;
	int			len,
				cleanLength;
	TupleDesc	tupType,
				cleanTupType;
	List	   *t;
	TargetEntry *tle;
	Resdom	   *resdom,
			   *cleanResdom;
	int			resjunk;
	AttrNumber	cleanResno;
	AttrNumber *cleanMap;
	Size		size;
	Node	   *expr;

	/* ---------------------
	 * First find the "clean" target list, i.e. all the entries
	 * in the original target list which have a zero 'resjunk'
	 * NOTE: make copy of the Resdom nodes, because we have
	 * to change the 'resno's...
	 * ---------------------
	 */
	cleanTargetList = NIL;
	cleanResno = 1;

	foreach(t, targetList)
	{
		TargetEntry *rtarget = lfirst(t);

		if (rtarget->resdom != NULL)
		{
			resdom = rtarget->resdom;
			expr = rtarget->expr;
			resjunk = resdom->resjunk;
			if (resjunk == 0)
			{

				/*
				 * make a copy of the resdom node, changing its resno.
				 */
				cleanResdom = (Resdom *) copyObject(resdom);
				cleanResdom->resno = cleanResno;
				cleanResno++;

				/*
				 * create a new target list entry
				 */
				tle = makeNode(TargetEntry);
				tle->resdom = cleanResdom;
				tle->expr = expr;
				cleanTargetList = lappend(cleanTargetList, tle);
			}
		}
		else
		{
#ifdef SETS_FIXED
			List	   *fjListP;
			Fjoin	   *cleanFjoin;
			List	   *cleanFjList;
			List	   *fjList = lfirst(t);
			Fjoin	   *fjNode = (Fjoin *) tl_node(fjList);

			cleanFjoin = (Fjoin) copyObject((Node) fjNode);
			cleanFjList = lcons(cleanFjoin, NIL);

			resdom = (Resdom) lfirst(get_fj_innerNode(fjNode));
			expr = lsecond(get_fj_innerNode(fjNode));
			cleanResdom = (Resdom) copyObject((Node) resdom);
			set_resno(cleanResdom, cleanResno);
			cleanResno++;
			tle = (List) MakeTLE(cleanResdom, (Expr) expr);
			set_fj_innerNode(cleanFjoin, tle);

			foreach(fjListP, lnext(fjList))
			{
				TargetEntry *tle = lfirst(fjListP);

				resdom = tle->resdom;
				expr = tle->expr;
				cleanResdom = (Resdom *) copyObject((Node) resdom);
				cleanResno++;
				cleanResdom->Resno = cleanResno;

				/*
				 * create a new target list entry
				 */
				tle = (List) MakeTLE(cleanResdom, (Expr) expr);
				cleanFjList = lappend(cleanFjList, tle);
			}
			lappend(cleanTargetList, cleanFjList);
#endif
		}
	}

	/* ---------------------
	 * Now calculate the tuple types for the original and the clean tuple
	 *
	 * XXX ExecTypeFromTL should be used sparingly.  Don't we already
	 *	   have the tupType corresponding to the targetlist we are passed?
	 *	   -cim 5/31/91
	 * ---------------------
	 */
	tupType = (TupleDesc) ExecTypeFromTL(targetList);
	cleanTupType = (TupleDesc) ExecTypeFromTL(cleanTargetList);

	len = ExecTargetListLength(targetList);
	cleanLength = ExecTargetListLength(cleanTargetList);

	/* ---------------------
	 * Now calculate the "map" between the original tuples attributes
	 * and the "clean" tuple's attributes.
	 *
	 * The "map" is an array of "cleanLength" attribute numbers, i.e.
	 * one entry for every attribute of the "clean" tuple.
	 * The value of this entry is the attribute number of the corresponding
	 * attribute of the "original" tuple.
	 * ---------------------
	 */
	if (cleanLength > 0)
	{
		size = cleanLength * sizeof(AttrNumber);
		cleanMap = (AttrNumber *) palloc(size);
		cleanResno = 1;
		foreach(t, targetList)
		{
			TargetEntry *tle = lfirst(t);

			if (tle->resdom != NULL)
			{
				resdom = tle->resdom;
				expr = tle->expr;
				resjunk = resdom->resjunk;
				if (resjunk == 0)
				{
					cleanMap[cleanResno - 1] = resdom->resno;
					cleanResno++;
				}
			}
			else
			{
#ifdef SETS_FIXED
				List		fjListP;
				List		fjList = lfirst(t);
				Fjoin		fjNode = (Fjoin) lfirst(fjList);

				/* what the hell is this????? */
				resdom = (Resdom) lfirst(get_fj_innerNode(fjNode));
#endif

				cleanMap[cleanResno - 1] = tle->resdom->resno;
				cleanResno++;

#ifdef SETS_FIXED
				foreach(fjListP, lnext(fjList))
				{
					TargetEntry *tle = lfirst(fjListP);

					resdom = tle->resdom;
					cleanMap[cleanResno - 1] = resdom->resno;
					cleanResno++;
				}
#endif
			}
		}
	}
	else
	{
		cleanMap = NULL;
	}

	/* ---------------------
	 * Finally create and initialize the JunkFilter.
	 * ---------------------
	 */
	junkfilter = makeNode(JunkFilter);

	junkfilter->jf_targetList = targetList;
	junkfilter->jf_length = len;
	junkfilter->jf_tupType = tupType;
	junkfilter->jf_cleanTargetList = cleanTargetList;
	junkfilter->jf_cleanLength = cleanLength;
	junkfilter->jf_cleanTupType = cleanTupType;
	junkfilter->jf_cleanMap = cleanMap;

	return (junkfilter);

}

/*-------------------------------------------------------------------------
 * ExecGetJunkAttribute
 *
 * Given a tuple (slot), the junk filter and a junk attribute's name,
 * extract & return the value of this attribute.
 *
 * It returns false iff no junk attribute with such name was found.
 *
 * NOTE: isNull might be NULL !
 *-------------------------------------------------------------------------
 */
bool
ExecGetJunkAttribute(JunkFilter *junkfilter,
					 TupleTableSlot *slot,
					 char *attrName,
					 Datum *value,
					 bool *isNull)
{
	List	   *targetList;
	List	   *t;
	Resdom	   *resdom;
	AttrNumber	resno;
	char	   *resname;
	int			resjunk;
	TupleDesc	tupType;
	HeapTuple	tuple;

	/* ---------------------
	 * first look in the junkfilter's target list for
	 * an attribute with the given name
	 * ---------------------
	 */
	resno = InvalidAttrNumber;
	targetList = junkfilter->jf_targetList;

	foreach(t, targetList)
	{
		TargetEntry *tle = lfirst(t);

		resdom = tle->resdom;
		resname = resdom->resname;
		resjunk = resdom->resjunk;
		if (resjunk != 0 && (strcmp(resname, attrName) == 0))
		{
			/* We found it ! */
			resno = resdom->resno;
			break;
		}
	}

	if (resno == InvalidAttrNumber)
	{
		/* Ooops! We couldn't find this attribute... */
		return (false);
	}

	/* ---------------------
	 * Now extract the attribute value from the tuple.
	 * ---------------------
	 */
	tuple = slot->val;
	tupType = (TupleDesc) junkfilter->jf_tupType;

	*value = (Datum)
		heap_getattr(tuple, InvalidBuffer, resno, tupType, isNull);

	return true;
}

/*-------------------------------------------------------------------------
 * ExecRemoveJunk
 *
 * Construct and return a tuple with all the junk attributes removed.
 *-------------------------------------------------------------------------
 */
HeapTuple
ExecRemoveJunk(JunkFilter *junkfilter, TupleTableSlot *slot)
{
	HeapTuple	tuple;
	HeapTuple	cleanTuple;
	AttrNumber *cleanMap;
	TupleDesc	cleanTupType;
	TupleDesc	tupType;
	int			cleanLength;
	bool		isNull;
	int			i;
	Size		size;
	Datum	   *values;
	char	   *nulls;
	Datum		values_array[64];
	char		nulls_array[64];

	/* ----------------
	 *	get info from the slot and the junk filter
	 * ----------------
	 */
	tuple = slot->val;

	tupType = (TupleDesc) junkfilter->jf_tupType;
	cleanTupType = (TupleDesc) junkfilter->jf_cleanTupType;
	cleanLength = junkfilter->jf_cleanLength;
	cleanMap = junkfilter->jf_cleanMap;

	/* ---------------------
	 *	Handle the trivial case first.
	 * ---------------------
	 */
	if (cleanLength == 0)
		return (HeapTuple) NULL;

	/* ---------------------
	 * Create the arrays that will hold the attribute values
	 * and the null information for the new "clean" tuple.
	 *
	 * Note: we use memory on the stack to optimize things when
	 *		 we are dealing with a small number of tuples.
	 *		 for large tuples we just use palloc.
	 * ---------------------
	 */
	if (cleanLength > 64)
	{
		size = cleanLength * sizeof(Datum);
		values = (Datum *) palloc(size);

		size = cleanLength * sizeof(char);
		nulls = (char *) palloc(size);
	}
	else
	{
		values = values_array;
		nulls = nulls_array;
	}

	/* ---------------------
	 * Exctract one by one all the values of the "clean" tuple.
	 * ---------------------
	 */
	for (i = 0; i < cleanLength; i++)
	{
		Datum		d = (Datum)
		heap_getattr(tuple, InvalidBuffer, cleanMap[i], tupType, &isNull);

		values[i] = d;

		if (isNull)
			nulls[i] = 'n';
		else
			nulls[i] = ' ';
	}

	/* ---------------------
	 * Now form the new tuple.
	 * ---------------------
	 */
	cleanTuple = heap_formtuple(cleanTupType,
								values,
								nulls);

	/* ---------------------
	 * We are done.  Free any space allocated for 'values' and 'nulls'
	 * and return the new tuple.
	 * ---------------------
	 */
	if (cleanLength > 64)
	{
		pfree(values);
		pfree(nulls);
	}

	return (cleanTuple);
}
