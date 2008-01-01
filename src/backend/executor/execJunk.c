/*-------------------------------------------------------------------------
 *
 * execJunk.c
 *	  Junk attribute support stuff....
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/execJunk.c,v 1.57 2008/01/01 19:45:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"

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
 * of system attributes like "ctid", or sort key columns that are not to
 * be output.
 *
 * The general idea is the following: A target list consists of a list of
 * TargetEntry nodes containing expressions. Each TargetEntry has a field
 * called 'resjunk'. If the value of this field is true then the
 * corresponding attribute is a "junk" attribute.
 *
 * When we initialize a plan we call ExecInitJunkFilter to create
 * and store the appropriate information in the es_junkFilter attribute of
 * EState.
 *
 * We then execute the plan, treating the resjunk attributes like any others.
 *
 * Finally, when at the top level we get back a tuple, we can call
 * ExecFindJunkAttribute/ExecGetJunkAttribute to retrieve the values of the
 * junk attributes we are interested in, and ExecFilterJunk or ExecRemoveJunk
 * to remove all the junk attributes from a tuple. This new "clean" tuple is
 * then printed, replaced, deleted or inserted.
 *
 *-------------------------------------------------------------------------
 */

/*
 * ExecInitJunkFilter
 *
 * Initialize the Junk filter.
 *
 * The source targetlist is passed in.	The output tuple descriptor is
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
	 * Use the given slot, or make a new slot if we weren't given one.
	 */
	if (slot)
		ExecSetSlotDescriptor(slot, cleanTupType);
	else
		slot = MakeSingleTupleTableSlot(cleanTupType);

	/*
	 * Now calculate the mapping between the original tuple's attributes and
	 * the "clean" tuple's attributes.
	 *
	 * The "map" is an array of "cleanLength" attribute numbers, i.e. one
	 * entry for every attribute of the "clean" tuple. The value of this entry
	 * is the attribute number of the corresponding attribute of the
	 * "original" tuple.  (Zero indicates a NULL output attribute, but we do
	 * not use that feature in this routine.)
	 */
	cleanLength = cleanTupType->natts;
	if (cleanLength > 0)
	{
		cleanMap = (AttrNumber *) palloc(cleanLength * sizeof(AttrNumber));
		cleanResno = 1;
		foreach(t, targetList)
		{
			TargetEntry *tle = lfirst(t);

			if (!tle->resjunk)
			{
				cleanMap[cleanResno - 1] = tle->resno;
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
	 * Use the given slot, or make a new slot if we weren't given one.
	 */
	if (slot)
		ExecSetSlotDescriptor(slot, cleanTupType);
	else
		slot = MakeSingleTupleTableSlot(cleanTupType);

	/*
	 * Calculate the mapping between the original tuple's attributes and the
	 * "clean" tuple's attributes.
	 *
	 * The "map" is an array of "cleanLength" attribute numbers, i.e. one
	 * entry for every attribute of the "clean" tuple. The value of this entry
	 * is the attribute number of the corresponding attribute of the
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

				t = lnext(t);
				if (!tle->resjunk)
				{
					cleanMap[i] = tle->resno;
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

	return junkfilter;
}

/*
 * ExecFindJunkAttribute
 *
 * Locate the specified junk attribute in the junk filter's targetlist,
 * and return its resno.  Returns InvalidAttrNumber if not found.
 */
AttrNumber
ExecFindJunkAttribute(JunkFilter *junkfilter, const char *attrName)
{
	ListCell   *t;

	foreach(t, junkfilter->jf_targetList)
	{
		TargetEntry *tle = lfirst(t);

		if (tle->resjunk && tle->resname &&
			(strcmp(tle->resname, attrName) == 0))
		{
			/* We found it ! */
			return tle->resno;
		}
	}

	return InvalidAttrNumber;
}

/*
 * ExecGetJunkAttribute
 *
 * Given a junk filter's input tuple (slot) and a junk attribute's number
 * previously found by ExecFindJunkAttribute, extract & return the value and
 * isNull flag of the attribute.
 */
Datum
ExecGetJunkAttribute(TupleTableSlot *slot, AttrNumber attno,
					 bool *isNull)
{
	Assert(attno > 0);

	return slot_getattr(slot, attno, isNull);
}

/*
 * ExecFilterJunk
 *
 * Construct and return a slot with all the junk attributes removed.
 */
TupleTableSlot *
ExecFilterJunk(JunkFilter *junkfilter, TupleTableSlot *slot)
{
	TupleTableSlot *resultSlot;
	AttrNumber *cleanMap;
	TupleDesc	cleanTupType;
	int			cleanLength;
	int			i;
	Datum	   *values;
	bool	   *isnull;
	Datum	   *old_values;
	bool	   *old_isnull;

	/*
	 * Extract all the values of the old tuple.
	 */
	slot_getallattrs(slot);
	old_values = slot->tts_values;
	old_isnull = slot->tts_isnull;

	/*
	 * get info from the junk filter
	 */
	cleanTupType = junkfilter->jf_cleanTupType;
	cleanLength = cleanTupType->natts;
	cleanMap = junkfilter->jf_cleanMap;
	resultSlot = junkfilter->jf_resultSlot;

	/*
	 * Prepare to build a virtual result tuple.
	 */
	ExecClearTuple(resultSlot);
	values = resultSlot->tts_values;
	isnull = resultSlot->tts_isnull;

	/*
	 * Transpose data into proper fields of the new tuple.
	 */
	for (i = 0; i < cleanLength; i++)
	{
		int			j = cleanMap[i];

		if (j == 0)
		{
			values[i] = (Datum) 0;
			isnull[i] = true;
		}
		else
		{
			values[i] = old_values[j - 1];
			isnull[i] = old_isnull[j - 1];
		}
	}

	/*
	 * And return the virtual tuple.
	 */
	return ExecStoreVirtualTuple(resultSlot);
}

/*
 * ExecRemoveJunk
 *
 * Convenience routine to generate a physical clean tuple,
 * rather than just a virtual slot.
 */
HeapTuple
ExecRemoveJunk(JunkFilter *junkfilter, TupleTableSlot *slot)
{
	return ExecCopySlotTuple(ExecFilterJunk(junkfilter, slot));
}
