/*-------------------------------------------------------------------------
 *
 * execJunk.c
 *	  Junk attribute support stuff....
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/execJunk.c,v 1.45 2004/10/11 02:02:41 neilc Exp $
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
 * returned or stored on disk. Their only purpose in life is to
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

/*
 * ExecInitJunkFilter
 *
 * Initialize the Junk filter.
 *
 * The source targetlist is passed in.  The output tuple descriptor is
 * built from the non-junk tlist entries, plus the passed specification
 * of whether to include room for an OID or not.
 * An optional resultSlot can be passed as well.
 */
JunkFilter *
ExecInitJunkFilter(List *targetList, bool hasoid, TupleTableSlot *slot)
{
	JunkFilter *junkfilter;
	TupleDesc	cleanTupType;
	int			cleanLength;
	AttrNumber *cleanMap;
	ListCell   *t;
	AttrNumber	cleanResno;

	/*
	 * Compute the tuple descriptor for the cleaned tuple.
	 */
	cleanTupType = ExecCleanTypeFromTL(targetList, hasoid);

	/*
	 * Now calculate the mapping between the original tuple's attributes and
	 * the "clean" tuple's attributes.
	 *
	 * The "map" is an array of "cleanLength" attribute numbers, i.e. one
	 * entry for every attribute of the "clean" tuple. The value of this
	 * entry is the attribute number of the corresponding attribute of the
	 * "original" tuple.  (Zero indicates a NULL output attribute, but we
	 * do not use that feature in this routine.)
	 */
	cleanLength = cleanTupType->natts;
	if (cleanLength > 0)
	{
		cleanMap = (AttrNumber *) palloc(cleanLength * sizeof(AttrNumber));
		cleanResno = 1;
		foreach(t, targetList)
		{
			TargetEntry *tle = lfirst(t);
			Resdom	   *resdom = tle->resdom;

			if (!resdom->resjunk)
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
	junkfilter->jf_cleanTupType = cleanTupType;
	junkfilter->jf_cleanMap = cleanMap;
	junkfilter->jf_resultSlot = slot;

	if (slot)
		ExecSetSlotDescriptor(slot, cleanTupType, false);

	return junkfilter;
}

/*
 * ExecInitJunkFilterConversion
 *
 * Initialize a JunkFilter for rowtype conversions.
 *
 * Here, we are given the target "clean" tuple descriptor rather than
 * inferring it from the targetlist.  The target descriptor can contain
 * deleted columns.  It is assumed that the caller has checked that the
 * non-deleted columns match up with the non-junk columns of the targetlist.
 */
JunkFilter *
ExecInitJunkFilterConversion(List *targetList,
							 TupleDesc cleanTupType,
							 TupleTableSlot *slot)
{
	JunkFilter *junkfilter;
	int			cleanLength;
	AttrNumber *cleanMap;
	ListCell   *t;
	int			i;

	/*
	 * Calculate the mapping between the original tuple's attributes and
	 * the "clean" tuple's attributes.
	 *
	 * The "map" is an array of "cleanLength" attribute numbers, i.e. one
	 * entry for every attribute of the "clean" tuple. The value of this
	 * entry is the attribute number of the corresponding attribute of the
	 * "original" tuple.  We store zero for any deleted attributes, marking
	 * that a NULL is needed in the output tuple.
	 */
	cleanLength = cleanTupType->natts;
	if (cleanLength > 0)
	{
		cleanMap = (AttrNumber *) palloc0(cleanLength * sizeof(AttrNumber));
		t = list_head(targetList);
		for (i = 0; i < cleanLength; i++)
		{
			if (cleanTupType->attrs[i]->attisdropped)
				continue;		/* map entry is already zero */
			for (;;)
			{
				TargetEntry *tle = lfirst(t);
				Resdom	   *resdom = tle->resdom;

				t = lnext(t);
				if (!resdom->resjunk)
				{
					cleanMap[i] = resdom->resno;
					break;
				}
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
	junkfilter->jf_cleanTupType = cleanTupType;
	junkfilter->jf_cleanMap = cleanMap;
	junkfilter->jf_resultSlot = slot;

	if (slot)
		ExecSetSlotDescriptor(slot, cleanTupType, false);

	return junkfilter;
}

/*
 * ExecGetJunkAttribute
 *
 * Given a tuple (slot), the junk filter and a junk attribute's name,
 * extract & return the value and isNull flag of this attribute.
 *
 * It returns false iff no junk attribute with such name was found.
 */
bool
ExecGetJunkAttribute(JunkFilter *junkfilter,
					 TupleTableSlot *slot,
					 char *attrName,
					 Datum *value,
					 bool *isNull)
{
	List	   *targetList;
	ListCell   *t;
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
	tupType = slot->ttc_tupleDescriptor;

	*value = heap_getattr(tuple, resno, tupType, isNull);

	return true;
}

/*
 * ExecRemoveJunk
 *
 * Construct and return a tuple with all the junk attributes removed.
 *
 * Note: for historical reasons, this does not store the constructed
 * tuple into the junkfilter's resultSlot.  The caller should do that
 * if it wants to.
 */
HeapTuple
ExecRemoveJunk(JunkFilter *junkfilter, TupleTableSlot *slot)
{
#define PREALLOC_SIZE	64
	HeapTuple	tuple;
	HeapTuple	cleanTuple;
	AttrNumber *cleanMap;
	TupleDesc	cleanTupType;
	TupleDesc	tupType;
	int			cleanLength;
	int			oldLength;
	int			i;
	Datum	   *values;
	char	   *nulls;
	Datum	   *old_values;
	char	   *old_nulls;
	Datum		values_array[PREALLOC_SIZE];
	Datum		old_values_array[PREALLOC_SIZE];
	char		nulls_array[PREALLOC_SIZE];
	char		old_nulls_array[PREALLOC_SIZE];

	/*
	 * get info from the slot and the junk filter
	 */
	tuple = slot->val;
	tupType = slot->ttc_tupleDescriptor;
	oldLength = tupType->natts + 1;			/* +1 for NULL */

	cleanTupType = junkfilter->jf_cleanTupType;
	cleanLength = cleanTupType->natts;
	cleanMap = junkfilter->jf_cleanMap;

	/*
	 * Create the arrays that will hold the attribute values and the null
	 * information for the old tuple and new "clean" tuple.
	 *
	 * Note: we use memory on the stack to optimize things when we are
	 * dealing with a small number of attributes. for large tuples we just
	 * use palloc.
	 */
	if (cleanLength > PREALLOC_SIZE)
	{
		values = (Datum *) palloc(cleanLength * sizeof(Datum));
		nulls = (char *) palloc(cleanLength * sizeof(char));
	}
	else
	{
		values = values_array;
		nulls = nulls_array;
	}
	if (oldLength > PREALLOC_SIZE)
	{
		old_values = (Datum *) palloc(oldLength * sizeof(Datum));
		old_nulls = (char *) palloc(oldLength * sizeof(char));
	}
	else
	{
		old_values = old_values_array;
		old_nulls = old_nulls_array;
	}

	/*
	 * Extract all the values of the old tuple, offsetting the arrays
	 * so that old_values[0] is NULL and old_values[1] is the first
	 * source attribute; this exactly matches the numbering convention
	 * in cleanMap.
	 */
	heap_deformtuple(tuple, tupType, old_values + 1, old_nulls + 1);
	old_values[0] = (Datum) 0;
	old_nulls[0] = 'n';

	/*
	 * Transpose into proper fields of the new tuple.
	 */
	for (i = 0; i < cleanLength; i++)
	{
		int			j = cleanMap[i];

		values[i] = old_values[j];
		nulls[i] = old_nulls[j];
	}

	/*
	 * Now form the new tuple.
	 */
	cleanTuple = heap_formtuple(cleanTupType, values, nulls);

	/*
	 * We are done.  Free any space allocated for 'values' and 'nulls' and
	 * return the new tuple.
	 */
	if (values != values_array)
	{
		pfree(values);
		pfree(nulls);
	}
	if (old_values != old_values_array)
	{
		pfree(old_values);
		pfree(old_nulls);
	}

	return cleanTuple;
}
