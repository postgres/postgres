/*-------------------------------------------------------------------------
 *
 * execTuples.c
 *	  Routines dealing with the executor tuple tables.	These are used to
 *	  ensure that the executor frees copies of tuples (made by
 *	  ExecTargetList) properly.
 *
 *	  Routines dealing with the type information for tuples. Currently,
 *	  the type information for a tuple is an array of FormData_pg_attribute.
 *	  This information is needed by routines manipulating tuples
 *	  (getattribute, formtuple, etc.).
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execTuples.c,v 1.72 2003/09/29 18:22:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *	 TABLE CREATE/DELETE
 *		ExecCreateTupleTable	- create a new tuple table
 *		ExecDropTupleTable		- destroy a table
 *
 *	 SLOT RESERVATION
 *		ExecAllocTableSlot		- find an available slot in the table
 *
 *	 SLOT ACCESSORS
 *		ExecStoreTuple			- store a tuple in the table
 *		ExecFetchTuple			- fetch a tuple from the table
 *		ExecClearTuple			- clear contents of a table slot
 *		ExecSetSlotDescriptor	- set a slot's tuple descriptor
 *		ExecSetSlotDescriptorIsNew - diddle the slot-desc-is-new flag
 *
 *	 SLOT STATUS PREDICATES
 *		TupIsNull				- true when slot contains no tuple(Macro)
 *
 *	 CONVENIENCE INITIALIZATION ROUTINES
 *		ExecInitResultTupleSlot    \	convenience routines to initialize
 *		ExecInitScanTupleSlot		\	the various tuple slots for nodes
 *		ExecInitExtraTupleSlot		/	which store copies of tuples.
 *		ExecInitNullTupleSlot	   /
 *
 *	 Routines that probably belong somewhere else:
 *		ExecTypeFromTL			- form a TupleDesc from a target list
 *
 *	 EXAMPLE OF HOW TABLE ROUTINES WORK
 *		Suppose we have a query such as retrieve (EMP.name) and we have
 *		a single SeqScan node in the query plan.
 *
 *		At ExecStart()
 *		----------------
 *		- InitPlan() calls ExecCreateTupleTable() to create the tuple
 *		  table which will hold tuples processed by the executor.
 *
 *		- ExecInitSeqScan() calls ExecInitScanTupleSlot() and
 *		  ExecInitResultTupleSlot() to reserve places in the tuple
 *		  table for the tuples returned by the access methods and the
 *		  tuples resulting from preforming target list projections.
 *
 *		During ExecRun()
 *		----------------
 *		- SeqNext() calls ExecStoreTuple() to place the tuple returned
 *		  by the access methods into the scan tuple slot.
 *
 *		- ExecSeqScan() calls ExecStoreTuple() to take the result
 *		  tuple from ExecProject() and place it into the result tuple slot.
 *
 *		- ExecutePlan() calls ExecRetrieve() which gets the tuple out of
 *		  the slot passed to it by calling ExecFetchTuple().  this tuple
 *		  is then returned.
 *
 *		At ExecEnd()
 *		----------------
 *		- EndPlan() calls ExecDropTupleTable() to clean up any remaining
 *		  tuples left over from executing the query.
 *
 *		The important thing to watch in the executor code is how pointers
 *		to the slots containing tuples are passed instead of the tuples
 *		themselves.  This facilitates the communication of related information
 *		(such as whether or not a tuple should be pfreed, what buffer contains
 *		this tuple, the tuple's tuple descriptor, etc).   Note that much of
 *		this information is also kept in the ExprContext of each node.
 *		Soon the executor will be redesigned and ExprContext's will contain
 *		only slot pointers.  -cim 3/14/91
 *
 *	 NOTES
 *		The tuple table stuff is relatively new, put here to alleviate
 *		the process growth problems in the executor.  The other routines
 *		are old (from the original lisp system) and may someday become
 *		obsolete.  -cim 6/23/90
 *
 *		In the implementation of nested-dot queries such as
 *		"retrieve (EMP.hobbies.all)", a single scan may return tuples
 *		of many types, so now we return pointers to tuple descriptors
 *		along with tuples returned via the tuple table.  This means
 *		we now have a bunch of routines to diddle the slot descriptors
 *		too.  -cim 1/18/90
 *
 *		The tuple table stuff depends on the executor/tuptable.h macros,
 *		and the TupleTableSlot node in execnodes.h.
 *
 */
#include "postgres.h"

#include "funcapi.h"
#include "access/heapam.h"
#include "executor/executor.h"
#include "utils/lsyscache.h"


/* ----------------------------------------------------------------
 *				  tuple table create/delete functions
 * ----------------------------------------------------------------
 */
/* --------------------------------
 *		ExecCreateTupleTable
 *
 *		This creates a new tuple table of the specified initial
 *		size.  If the size is insufficient, ExecAllocTableSlot()
 *		will grow the table as necessary.
 *
 *		This should be used by InitPlan() to allocate the table.
 *		The table's address will be stored in the EState structure.
 * --------------------------------
 */
TupleTable						/* return: address of table */
ExecCreateTupleTable(int initialSize)	/* initial number of slots in
										 * table */
{
	TupleTable	newtable;		/* newly allocated table */
	TupleTableSlot *array;		/* newly allocated slot array */

	/*
	 * sanity checks
	 */
	Assert(initialSize >= 1);

	/*
	 * Now allocate our new table along with space for the pointers to the
	 * tuples.
	 */

	newtable = (TupleTable) palloc(sizeof(TupleTableData));
	array = (TupleTableSlot *) palloc(initialSize * sizeof(TupleTableSlot));

	/*
	 * clean out the slots we just allocated
	 */
	MemSet(array, 0, initialSize * sizeof(TupleTableSlot));

	/*
	 * initialize the new table and return it to the caller.
	 */
	newtable->size = initialSize;
	newtable->next = 0;
	newtable->array = array;

	return newtable;
}

/* --------------------------------
 *		ExecDropTupleTable
 *
 *		This frees the storage used by the tuple table itself
 *		and optionally frees the contents of the table also.
 *		It is expected that this routine be called by EndPlan().
 * --------------------------------
 */
void
ExecDropTupleTable(TupleTable table,	/* tuple table */
				   bool shouldFree)		/* true if we should free slot
										 * contents */
{
	int			next;			/* next available slot */
	TupleTableSlot *array;		/* start of table array */
	int			i;				/* counter */

	/*
	 * sanity checks
	 */
	Assert(table != NULL);

	/*
	 * get information from the table
	 */
	array = table->array;
	next = table->next;

	/*
	 * first free all the valid pointers in the tuple array and drop
	 * refcounts of any referenced buffers, if that's what the caller
	 * wants.  (There is probably no good reason for the caller ever not
	 * to want it!)
	 */
	if (shouldFree)
	{
		for (i = 0; i < next; i++)
		{
			ExecClearTuple(&array[i]);
			if (array[i].ttc_shouldFreeDesc &&
				array[i].ttc_tupleDescriptor != NULL)
				FreeTupleDesc(array[i].ttc_tupleDescriptor);
		}
	}

	/*
	 * finally free the tuple array and the table itself.
	 */
	pfree(array);
	pfree(table);
}


/* ----------------------------------------------------------------
 *				  tuple table slot reservation functions
 * ----------------------------------------------------------------
 */
/* --------------------------------
 *		ExecAllocTableSlot
 *
 *		This routine is used to reserve slots in the table for
 *		use by the various plan nodes.	It is expected to be
 *		called by the node init routines (ex: ExecInitNestLoop)
 *		once per slot needed by the node.  Not all nodes need
 *		slots (some just pass tuples around).
 * --------------------------------
 */
TupleTableSlot *
ExecAllocTableSlot(TupleTable table)
{
	int			slotnum;		/* new slot number */
	TupleTableSlot *slot;

	/*
	 * sanity checks
	 */
	Assert(table != NULL);

	/*
	 * if our table is full we have to allocate a larger size table. Since
	 * ExecAllocTableSlot() is only called before the table is ever used
	 * to store tuples, we don't have to worry about the contents of the
	 * old table. If this changes, then we will have to preserve the
	 * contents. -cim 6/23/90
	 *
	 * Unfortunately, we *cannot* do this.	All of the nodes in the plan that
	 * have already initialized their slots will have pointers into
	 * _freed_ memory.	This leads to bad ends.  We now count the number
	 * of slots we will need and create all the slots we will need ahead
	 * of time.  The if below should never happen now.	Fail if it does.
	 * -mer 4 Aug 1992
	 */
	if (table->next >= table->size)
		elog(ERROR, "plan requires more slots than are available");

	/*
	 * at this point, space in the table is guaranteed so we reserve the
	 * next slot, initialize and return it.
	 */
	slotnum = table->next;
	table->next++;

	slot = &(table->array[slotnum]);

	/* Make sure the allocated slot is valid (and empty) */
	slot->type = T_TupleTableSlot;
	slot->val = (HeapTuple) NULL;
	slot->ttc_shouldFree = true;
	slot->ttc_descIsNew = true;
	slot->ttc_shouldFreeDesc = true;
	slot->ttc_tupleDescriptor = (TupleDesc) NULL;
	slot->ttc_buffer = InvalidBuffer;

	return slot;
}

/* --------------------------------
 *		MakeTupleTableSlot
 *
 *		This routine makes an empty standalone TupleTableSlot.
 *		It really shouldn't exist, but there are a few places
 *		that do this, so we may as well centralize the knowledge
 *		of what's in one ...
 * --------------------------------
 */
TupleTableSlot *
MakeTupleTableSlot(void)
{
	TupleTableSlot *slot = makeNode(TupleTableSlot);

	/* This should match ExecAllocTableSlot() */
	slot->val = (HeapTuple) NULL;
	slot->ttc_shouldFree = true;
	slot->ttc_descIsNew = true;
	slot->ttc_shouldFreeDesc = true;
	slot->ttc_tupleDescriptor = (TupleDesc) NULL;
	slot->ttc_buffer = InvalidBuffer;

	return slot;
}

/* ----------------------------------------------------------------
 *				  tuple table slot accessor functions
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		ExecStoreTuple
 *
 *		This function is used to store a tuple into a specified
 *		slot in the tuple table.
 *
 *		tuple:	tuple to store
 *		slot:	slot to store it in
 *		buffer: disk buffer if tuple is in a disk page, else InvalidBuffer
 *		shouldFree: true if ExecClearTuple should pfree() the tuple
 *					when done with it
 *
 * If 'buffer' is not InvalidBuffer, the tuple table code acquires a pin
 * on the buffer which is held until the slot is cleared, so that the tuple
 * won't go away on us.
 *
 * shouldFree is normally set 'true' for tuples constructed on-the-fly.
 * It must always be 'false' for tuples that are stored in disk pages,
 * since we don't want to try to pfree those.
 *
 * Another case where it is 'false' is when the referenced tuple is held
 * in a tuple table slot belonging to a lower-level executor Proc node.
 * In this case the lower-level slot retains ownership and responsibility
 * for eventually releasing the tuple.	When this method is used, we must
 * be certain that the upper-level Proc node will lose interest in the tuple
 * sooner than the lower-level one does!  If you're not certain, copy the
 * lower-level tuple with heap_copytuple and let the upper-level table
 * slot assume ownership of the copy!
 *
 * Return value is just the passed-in slot pointer.
 * --------------------------------
 */
TupleTableSlot *
ExecStoreTuple(HeapTuple tuple,
			   TupleTableSlot *slot,
			   Buffer buffer,
			   bool shouldFree)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	/* passing shouldFree=true for a tuple on a disk page is not sane */
	Assert(BufferIsValid(buffer) ? (!shouldFree) : true);

	/* clear out any old contents of the slot */
	ExecClearTuple(slot);

	/*
	 * store the new tuple into the specified slot and return the slot
	 * into which we stored the tuple.
	 */
	slot->val = tuple;
	slot->ttc_buffer = buffer;
	slot->ttc_shouldFree = shouldFree;

	/*
	 * If tuple is on a disk page, keep the page pinned as long as we hold
	 * a pointer into it.
	 */
	if (BufferIsValid(buffer))
		IncrBufferRefCount(buffer);

	return slot;
}

/* --------------------------------
 *		ExecClearTuple
 *
 *		This function is used to clear out a slot in the tuple table.
 *
 *		NB: only the tuple is cleared, not the tuple descriptor (if any).
 * --------------------------------
 */
TupleTableSlot *				/* return: slot passed */
ExecClearTuple(TupleTableSlot *slot)	/* slot in which to store tuple */
{
	HeapTuple	oldtuple;		/* prior contents of slot */

	/*
	 * sanity checks
	 */
	Assert(slot != NULL);

	/*
	 * get information from the tuple table
	 */
	oldtuple = slot->val;

	/*
	 * free the old contents of the specified slot if necessary.
	 */
	if (slot->ttc_shouldFree && oldtuple != NULL)
		heap_freetuple(oldtuple);

	slot->val = (HeapTuple) NULL;

	slot->ttc_shouldFree = true;	/* probably useless code... */

	/*
	 * Drop the pin on the referenced buffer, if there is one.
	 */
	if (BufferIsValid(slot->ttc_buffer))
		ReleaseBuffer(slot->ttc_buffer);

	slot->ttc_buffer = InvalidBuffer;

	return slot;
}

/* --------------------------------
 *		ExecSetSlotDescriptor
 *
 *		This function is used to set the tuple descriptor associated
 *		with the slot's tuple.
 * --------------------------------
 */
void
ExecSetSlotDescriptor(TupleTableSlot *slot,		/* slot to change */
					  TupleDesc tupdesc,		/* new tuple descriptor */
					  bool shouldFree)	/* is desc owned by slot? */
{
	if (slot->ttc_shouldFreeDesc &&
		slot->ttc_tupleDescriptor != NULL)
		FreeTupleDesc(slot->ttc_tupleDescriptor);

	slot->ttc_tupleDescriptor = tupdesc;
	slot->ttc_shouldFreeDesc = shouldFree;
}

/* --------------------------------
 *		ExecSetSlotDescriptorIsNew
 *
 *		This function is used to change the setting of the "isNew" flag
 * --------------------------------
 */
void
ExecSetSlotDescriptorIsNew(TupleTableSlot *slot,		/* slot to change */
						   bool isNew)	/* "isNew" setting */
{
	slot->ttc_descIsNew = isNew;
}

/* ----------------------------------------------------------------
 *				  tuple table slot status predicates
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *				convenience initialization routines
 * ----------------------------------------------------------------
 */
/* --------------------------------
 *		ExecInit{Result,Scan,Extra}TupleSlot
 *
 *		These are convenience routines to initialize the specified slot
 *		in nodes inheriting the appropriate state.	ExecInitExtraTupleSlot
 *		is used for initializing special-purpose slots.
 * --------------------------------
 */
#define INIT_SLOT_DEFS \
	TupleTable	   tupleTable; \
	TupleTableSlot*   slot

#define INIT_SLOT_ALLOC \
	tupleTable = (TupleTable) estate->es_tupleTable; \
	slot =		 ExecAllocTableSlot(tupleTable);

/* ----------------
 *		ExecInitResultTupleSlot
 * ----------------
 */
void
ExecInitResultTupleSlot(EState *estate, PlanState *planstate)
{
	INIT_SLOT_DEFS;
	INIT_SLOT_ALLOC;
	planstate->ps_ResultTupleSlot = slot;
}

/* ----------------
 *		ExecInitScanTupleSlot
 * ----------------
 */
void
ExecInitScanTupleSlot(EState *estate, ScanState *scanstate)
{
	INIT_SLOT_DEFS;
	INIT_SLOT_ALLOC;
	scanstate->ss_ScanTupleSlot = slot;
}

/* ----------------
 *		ExecInitExtraTupleSlot
 * ----------------
 */
TupleTableSlot *
ExecInitExtraTupleSlot(EState *estate)
{
	INIT_SLOT_DEFS;
	INIT_SLOT_ALLOC;
	return slot;
}

/* ----------------
 *		ExecInitNullTupleSlot
 *
 * Build a slot containing an all-nulls tuple of the given type.
 * This is used as a substitute for an input tuple when performing an
 * outer join.
 * ----------------
 */
TupleTableSlot *
ExecInitNullTupleSlot(EState *estate, TupleDesc tupType)
{
	TupleTableSlot *slot = ExecInitExtraTupleSlot(estate);

	/*
	 * Since heap_getattr() will treat attributes beyond a tuple's t_natts
	 * as being NULL, we can make an all-nulls tuple just by making it be
	 * of zero length.	However, the slot descriptor must match the real
	 * tupType.
	 */
	HeapTuple	nullTuple;
	Datum		values[1];
	char		nulls[1];
	static struct tupleDesc NullTupleDesc;		/* we assume this inits to
												 * zeroes */

	ExecSetSlotDescriptor(slot, tupType, false);

	nullTuple = heap_formtuple(&NullTupleDesc, values, nulls);

	return ExecStoreTuple(nullTuple, slot, InvalidBuffer, true);
}

/* ----------------------------------------------------------------
 *		ExecTypeFromTL
 *
 *		Generate a tuple descriptor for the result tuple of a targetlist.
 *		(A parse/plan tlist must be passed, not an ExprState tlist.)
 *		Note that resjunk columns, if any, are included in the result.
 *
 *		Currently there are about 4 different places where we create
 *		TupleDescriptors.  They should all be merged, or perhaps
 *		be rewritten to call BuildDesc().
 * ----------------------------------------------------------------
 */
TupleDesc
ExecTypeFromTL(List *targetList, bool hasoid)
{
	TupleDesc	typeInfo;
	List	   *tlitem;
	int			len;

	/*
	 * allocate a new typeInfo
	 */
	len = ExecTargetListLength(targetList);
	typeInfo = CreateTemplateTupleDesc(len, hasoid);

	/*
	 * scan list, generate type info for each entry
	 */
	foreach(tlitem, targetList)
	{
		TargetEntry *tle = lfirst(tlitem);
		Resdom	   *resdom = tle->resdom;

		TupleDescInitEntry(typeInfo,
						   resdom->resno,
						   resdom->resname,
						   resdom->restype,
						   resdom->restypmod,
						   0,
						   false);
	}

	return typeInfo;
}

/* ----------------------------------------------------------------
 *		ExecCleanTypeFromTL
 *
 *		Same as above, but resjunk columns are omitted from the result.
 * ----------------------------------------------------------------
 */
TupleDesc
ExecCleanTypeFromTL(List *targetList, bool hasoid)
{
	TupleDesc	typeInfo;
	List	   *tlitem;
	int			len;
	int			cleanresno;

	/*
	 * allocate a new typeInfo
	 */
	len = ExecCleanTargetListLength(targetList);
	typeInfo = CreateTemplateTupleDesc(len, hasoid);

	/*
	 * scan list, generate type info for each entry
	 */
	cleanresno = 1;
	foreach(tlitem, targetList)
	{
		TargetEntry *tle = lfirst(tlitem);
		Resdom	   *resdom = tle->resdom;

		if (resdom->resjunk)
			continue;
		TupleDescInitEntry(typeInfo,
						   cleanresno++,
						   resdom->resname,
						   resdom->restype,
						   resdom->restypmod,
						   0,
						   false);
	}

	return typeInfo;
}

/*
 * TupleDescGetSlot - Initialize a slot based on the supplied tupledesc
 */
TupleTableSlot *
TupleDescGetSlot(TupleDesc tupdesc)
{
	TupleTableSlot *slot;

	/* Make a standalone slot */
	slot = MakeTupleTableSlot();

	/* Bind the tuple description to the slot */
	ExecSetSlotDescriptor(slot, tupdesc, true);

	/* Return the slot */
	return slot;
}

/*
 * TupleDescGetAttInMetadata - Build an AttInMetadata structure based on the
 * supplied TupleDesc. AttInMetadata can be used in conjunction with C strings
 * to produce a properly formed tuple.
 */
AttInMetadata *
TupleDescGetAttInMetadata(TupleDesc tupdesc)
{
	int			natts = tupdesc->natts;
	int			i;
	Oid			atttypeid;
	Oid			attinfuncid;
	FmgrInfo   *attinfuncinfo;
	Oid		   *attelems;
	int32	   *atttypmods;
	AttInMetadata *attinmeta;

	attinmeta = (AttInMetadata *) palloc(sizeof(AttInMetadata));

	/*
	 * Gather info needed later to call the "in" function for each
	 * attribute
	 */
	attinfuncinfo = (FmgrInfo *) palloc0(natts * sizeof(FmgrInfo));
	attelems = (Oid *) palloc0(natts * sizeof(Oid));
	atttypmods = (int32 *) palloc0(natts * sizeof(int32));

	for (i = 0; i < natts; i++)
	{
		/* Ignore dropped attributes */
		if (!tupdesc->attrs[i]->attisdropped)
		{
			atttypeid = tupdesc->attrs[i]->atttypid;
			getTypeInputInfo(atttypeid, &attinfuncid, &attelems[i]);
			fmgr_info(attinfuncid, &attinfuncinfo[i]);
			atttypmods[i] = tupdesc->attrs[i]->atttypmod;
		}
	}
	attinmeta->tupdesc = tupdesc;
	attinmeta->attinfuncs = attinfuncinfo;
	attinmeta->attelems = attelems;
	attinmeta->atttypmods = atttypmods;

	return attinmeta;
}

/*
 * BuildTupleFromCStrings - build a HeapTuple given user data in C string form.
 * values is an array of C strings, one for each attribute of the return tuple.
 */
HeapTuple
BuildTupleFromCStrings(AttInMetadata *attinmeta, char **values)
{
	TupleDesc	tupdesc = attinmeta->tupdesc;
	int			natts = tupdesc->natts;
	Datum	   *dvalues;
	char	   *nulls;
	int			i;
	Oid			attelem;
	int32		atttypmod;
	HeapTuple	tuple;

	dvalues = (Datum *) palloc(natts * sizeof(Datum));
	nulls = (char *) palloc(natts * sizeof(char));

	/* Call the "in" function for each non-null, non-dropped attribute */
	for (i = 0; i < natts; i++)
	{
		if (!tupdesc->attrs[i]->attisdropped)
		{
			/* Non-dropped attributes */
			if (values[i] != NULL)
			{
				attelem = attinmeta->attelems[i];
				atttypmod = attinmeta->atttypmods[i];

				dvalues[i] = FunctionCall3(&attinmeta->attinfuncs[i],
										   CStringGetDatum(values[i]),
										   ObjectIdGetDatum(attelem),
										   Int32GetDatum(atttypmod));
				nulls[i] = ' ';
			}
			else
			{
				dvalues[i] = (Datum) 0;
				nulls[i] = 'n';
			}
		}
		else
		{
			/* Handle dropped attributes by setting to NULL */
			dvalues[i] = (Datum) 0;
			nulls[i] = 'n';
		}
	}

	/*
	 * Form a tuple
	 */
	tuple = heap_formtuple(tupdesc, dvalues, nulls);

	/*
	 * Release locally palloc'd space.  XXX would probably be good to
	 * pfree values of pass-by-reference datums, as well.
	 */
	pfree(dvalues);
	pfree(nulls);

	return tuple;
}

/*
 * Functions for sending tuples to the frontend (or other specified destination)
 * as though it is a SELECT result. These are used by utility commands that
 * need to project directly to the destination and don't need or want full
 * Table Function capability. Currently used by EXPLAIN and SHOW ALL
 */
TupOutputState *
begin_tup_output_tupdesc(DestReceiver *dest, TupleDesc tupdesc)
{
	TupOutputState *tstate;

	tstate = (TupOutputState *) palloc(sizeof(TupOutputState));

	tstate->metadata = TupleDescGetAttInMetadata(tupdesc);
	tstate->dest = dest;

	(*tstate->dest->rStartup) (tstate->dest, (int) CMD_SELECT, tupdesc);

	return tstate;
}

/*
 * write a single tuple
 *
 * values is a list of the external C string representations of the values
 * to be projected.
 */
void
do_tup_output(TupOutputState *tstate, char **values)
{
	/* build a tuple from the input strings using the tupdesc */
	HeapTuple	tuple = BuildTupleFromCStrings(tstate->metadata, values);

	/* send the tuple to the receiver */
	(*tstate->dest->receiveTuple) (tuple,
								   tstate->metadata->tupdesc,
								   tstate->dest);
	/* clean up */
	heap_freetuple(tuple);
}

/*
 * write a chunk of text, breaking at newline characters
 *
 * NB: scribbles on its input!
 *
 * Should only be used with a single-TEXT-attribute tupdesc.
 */
void
do_text_output_multiline(TupOutputState *tstate, char *text)
{
	while (*text)
	{
		char	   *eol;

		eol = strchr(text, '\n');
		if (eol)
			*eol++ = '\0';
		else
			eol = text +strlen(text);

		do_tup_output(tstate, &text);
		text = eol;
	}
}

void
end_tup_output(TupOutputState *tstate)
{
	(*tstate->dest->rShutdown) (tstate->dest);
	/* note that destroying the dest is not ours to do */
	/* XXX worth cleaning up the attinmetadata? */
	pfree(tstate);
}
