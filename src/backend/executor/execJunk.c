/*-------------------------------------------------------------------------
 *
 * junk.c
 *	  Junk attribute support stuff....
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execJunk.c,v 1.36.4.1 2004/04/07 18:46:20 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"

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
 * called 'resjunk'. If the value of this attribute is true then the
 * corresponding attribute is a "junk" attribute.
 *
 * When we initialize a plan we call 'ExecInitJunkFilter' to create
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
 *
 * The initial targetlist and associated tuple descriptor are passed in.
 * An optional resultSlot can be passed as well.
 *-------------------------------------------------------------------------
 */
JunkFilter *
ExecInitJunkFilter(List *targetList, TupleDesc tupType,
				   TupleTableSlot *slot)
{
	JunkFilter *junkfilter;
	List	   *cleanTargetList;
	int			len,
				cleanLength;
	TupleDesc	cleanTupType;
	List	   *t;
	TargetEntry *tle;
	Resdom	   *resdom,
			   *cleanResdom;
	bool		resjunk;
	AttrNumber	cleanResno;
	AttrNumber *cleanMap;
	Expr	   *expr;

	/*
	 * First find the "clean" target list, i.e. all the entries in the
	 * original target list which have a false 'resjunk' NOTE: make copy
	 * of the Resdom nodes, because we have to change the 'resno's...
	 */
	cleanTargetList = NIL;
	cleanResno = 1;

	foreach(t, targetList)
	{
		TargetEntry *rtarget = lfirst(t);

		resdom = rtarget->resdom;
		expr = rtarget->expr;
		resjunk = resdom->resjunk;
		if (!resjunk)
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
			tle = makeTargetEntry(cleanResdom, expr);
			cleanTargetList = lappend(cleanTargetList, tle);
		}
	}

	/*
	 * Now calculate the tuple type for the cleaned tuple (we were already
	 * given the type for the original targetlist).
	 */
	cleanTupType = ExecTypeFromTL(cleanTargetList, tupType->tdhasoid);

	len = ExecTargetListLength(targetList);
	cleanLength = ExecTargetListLength(cleanTargetList);

	/*
	 * Now calculate the "map" between the original tuple's attributes and
	 * the "clean" tuple's attributes.
	 *
	 * The "map" is an array of "cleanLength" attribute numbers, i.e. one
	 * entry for every attribute of the "clean" tuple. The value of this
	 * entry is the attribute number of the corresponding attribute of the
	 * "original" tuple.
	 */
	if (cleanLength > 0)
	{
		cleanMap = (AttrNumber *) palloc(cleanLength * sizeof(AttrNumber));
		cleanResno = 1;
		foreach(t, targetList)
		{
			TargetEntry *tle = lfirst(t);

			resdom = tle->resdom;
			resjunk = resdom->resjunk;
			if (!resjunk)
			{
				cleanMap[cleanResno - 1] = resdom->resno;
				cleanResno++;
			}
		}
	}
	else
		cleanMap = NULL;

	/*
	 * Finally create and initialize the JunkFilter struct.
	 */
	junkfilter = makeNode(JunkFilter);

	junkfilter->jf_targetList = targetList;
	junkfilter->jf_length = len;
	junkfilter->jf_tupType = tupType;
	junkfilter->jf_cleanTargetList = cleanTargetList;
	junkfilter->jf_cleanLength = cleanLength;
	junkfilter->jf_cleanTupType = cleanTupType;
	junkfilter->jf_cleanMap = cleanMap;
	junkfilter->jf_resultSlot = slot;

	if (slot)
		ExecSetSlotDescriptor(slot, cleanTupType, false);

	return junkfilter;
}

/*-------------------------------------------------------------------------
 * ExecGetJunkAttribute
 *
 * Given a tuple (slot), the junk filter and a junk attribute's name,
 * extract & return the value and isNull flag of this attribute.
 *
 * It returns false iff no junk attribute with such name was found.
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
	AttrNumber	resno;
	TupleDesc	tupType;
	HeapTuple	tuple;

	/*
	 * first look in the junkfilter's target list for an attribute with
	 * the given name
	 */
	resno = InvalidAttrNumber;
	targetList = junkfilter->jf_targetList;

	foreach(t, targetList)
	{
		TargetEntry *tle = lfirst(t);
		Resdom	   *resdom = tle->resdom;

		if (resdom->resjunk && resdom->resname &&
			(strcmp(resdom->resname, attrName) == 0))
		{
			/* We found it ! */
			resno = resdom->resno;
			break;
		}
	}

	if (resno == InvalidAttrNumber)
	{
		/* Ooops! We couldn't find this attribute... */
		return false;
	}

	/*
	 * Now extract the attribute value from the tuple.
	 */
	tuple = slot->val;
	tupType = junkfilter->jf_tupType;

	*value = heap_getattr(tuple, resno, tupType, isNull);

	return true;
}

/*-------------------------------------------------------------------------
 * ExecRemoveJunk
 *
 * Construct and return a tuple with all the junk attributes removed.
 *
 * Note: for historical reasons, this does not store the constructed
 * tuple into the junkfilter's resultSlot.  The caller should do that
 * if it wants to.
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
	Datum	   *values;
	char	   *nulls;
	Datum		values_array[64];
	char		nulls_array[64];

	/*
	 * get info from the slot and the junk filter
	 */
	tuple = slot->val;

	tupType = junkfilter->jf_tupType;
	cleanTupType = junkfilter->jf_cleanTupType;
	cleanLength = junkfilter->jf_cleanLength;
	cleanMap = junkfilter->jf_cleanMap;

	/*
	 * Create the arrays that will hold the attribute values and the null
	 * information for the new "clean" tuple.
	 *
	 * Note: we use memory on the stack to optimize things when we are
	 * dealing with a small number of attributes. for large tuples we just
	 * use palloc.
	 */
	if (cleanLength > 64)
	{
		values = (Datum *) palloc(cleanLength * sizeof(Datum));
		nulls = (char *) palloc(cleanLength * sizeof(char));
	}
	else
	{
		values = values_array;
		nulls = nulls_array;
	}

	/*
	 * Exctract one by one all the values of the "clean" tuple.
	 */
	for (i = 0; i < cleanLength; i++)
	{
		values[i] = heap_getattr(tuple, cleanMap[i], tupType, &isNull);

		if (isNull)
			nulls[i] = 'n';
		else
			nulls[i] = ' ';
	}

	/*
	 * Now form the new tuple.
	 */
	cleanTuple = heap_formtuple(cleanTupType,
								values,
								nulls);

	/*
	 * We are done.  Free any space allocated for 'values' and 'nulls' and
	 * return the new tuple.
	 */
	if (cleanLength > 64)
	{
		pfree(values);
		pfree(nulls);
	}

	return cleanTuple;
}
