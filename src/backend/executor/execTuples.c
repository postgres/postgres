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
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/execTuples.c,v 1.100 2008/01/01 19:45:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *	 TABLE CREATE/DELETE
 *		ExecCreateTupleTable	- create a new tuple table
 *		ExecDropTupleTable		- destroy a table
 *		MakeSingleTupleTableSlot - make a single-slot table
 *		ExecDropSingleTupleTableSlot - destroy same
 *
 *	 SLOT RESERVATION
 *		ExecAllocTableSlot		- find an available slot in the table
 *
 *	 SLOT ACCESSORS
 *		ExecSetSlotDescriptor	- set a slot's tuple descriptor
 *		ExecStoreTuple			- store a physical tuple in the slot
 *		ExecStoreMinimalTuple	- store a minimal physical tuple in the slot
 *		ExecClearTuple			- clear contents of a slot
 *		ExecStoreVirtualTuple	- mark slot as containing a virtual tuple
 *		ExecCopySlotTuple		- build a physical tuple from a slot
 *		ExecCopySlotMinimalTuple - build a minimal physical tuple from a slot
 *		ExecMaterializeSlot		- convert virtual to physical storage
 *		ExecCopySlot			- copy one slot's contents to another
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
 *		Suppose we have a query such as SELECT emp.name FROM emp and we have
 *		a single SeqScan node in the query plan.
 *
 *		At ExecutorStart()
 *		----------------
 *		- InitPlan() calls ExecCreateTupleTable() to create the tuple
 *		  table which will hold tuples processed by the executor.
 *
 *		- ExecInitSeqScan() calls ExecInitScanTupleSlot() and
 *		  ExecInitResultTupleSlot() to reserve places in the tuple
 *		  table for the tuples returned by the access methods and the
 *		  tuples resulting from performing target list projections.
 *
 *		During ExecutorRun()
 *		----------------
 *		- SeqNext() calls ExecStoreTuple() to place the tuple returned
 *		  by the access methods into the scan tuple slot.
 *
 *		- ExecSeqScan() calls ExecStoreTuple() to take the result
 *		  tuple from ExecProject() and place it into the result tuple slot.
 *
 *		- ExecutePlan() calls ExecSelect(), which passes the result slot
 *		  to printtup(), which uses slot_getallattrs() to extract the
 *		  individual Datums for printing.
 *
 *		At ExecutorEnd()
 *		----------------
 *		- EndPlan() calls ExecDropTupleTable() to clean up any remaining
 *		  tuples left over from executing the query.
 *
 *		The important thing to watch in the executor code is how pointers
 *		to the slots containing tuples are passed instead of the tuples
 *		themselves.  This facilitates the communication of related information
 *		(such as whether or not a tuple should be pfreed, what buffer contains
 *		this tuple, the tuple's tuple descriptor, etc).  It also allows us
 *		to avoid physically constructing projection tuples in many cases.
 */
#include "postgres.h"

#include "funcapi.h"
#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "parser/parse_expr.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


static TupleDesc ExecTypeFromTLInternal(List *targetList,
					   bool hasoid, bool skipjunk);


/* ----------------------------------------------------------------
 *				  tuple table create/delete functions
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		ExecCreateTupleTable
 *
 *		This creates a new tuple table of the specified size.
 *
 *		This should be used by InitPlan() to allocate the table.
 *		The table's address will be stored in the EState structure.
 * --------------------------------
 */
TupleTable
ExecCreateTupleTable(int tableSize)
{
	TupleTable	newtable;
	int			i;

	/*
	 * sanity checks
	 */
	Assert(tableSize >= 1);

	/*
	 * allocate the table itself
	 */
	newtable = (TupleTable) palloc(sizeof(TupleTableData) +
								   (tableSize - 1) *sizeof(TupleTableSlot));
	newtable->size = tableSize;
	newtable->next = 0;

	/*
	 * initialize all the slots to empty states
	 */
	for (i = 0; i < tableSize; i++)
	{
		TupleTableSlot *slot = &(newtable->array[i]);

		slot->type = T_TupleTableSlot;
		slot->tts_isempty = true;
		slot->tts_shouldFree = false;
		slot->tts_tuple = NULL;
		slot->tts_tupleDescriptor = NULL;
		slot->tts_mcxt = CurrentMemoryContext;
		slot->tts_buffer = InvalidBuffer;
		slot->tts_nvalid = 0;
		slot->tts_values = NULL;
		slot->tts_isnull = NULL;
		slot->tts_mintuple = NULL;
	}

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
	/*
	 * sanity checks
	 */
	Assert(table != NULL);

	/*
	 * first free all the valid pointers in the tuple array and drop refcounts
	 * of any referenced buffers, if that's what the caller wants.  (There is
	 * probably no good reason for the caller ever not to want it!)
	 */
	if (shouldFree)
	{
		int			next = table->next;
		int			i;

		for (i = 0; i < next; i++)
		{
			TupleTableSlot *slot = &(table->array[i]);

			ExecClearTuple(slot);
			if (slot->tts_tupleDescriptor)
				ReleaseTupleDesc(slot->tts_tupleDescriptor);
			if (slot->tts_values)
				pfree(slot->tts_values);
			if (slot->tts_isnull)
				pfree(slot->tts_isnull);
		}
	}

	/*
	 * finally free the tuple table itself.
	 */
	pfree(table);
}

/* --------------------------------
 *		MakeSingleTupleTableSlot
 *
 *		This is a convenience routine for operations that need a
 *		standalone TupleTableSlot not gotten from the main executor
 *		tuple table.  It makes a single slot and initializes it as
 *		though by ExecSetSlotDescriptor(slot, tupdesc).
 * --------------------------------
 */
TupleTableSlot *
MakeSingleTupleTableSlot(TupleDesc tupdesc)
{
	TupleTableSlot *slot = makeNode(TupleTableSlot);

	/* This should match ExecCreateTupleTable() */
	slot->tts_isempty = true;
	slot->tts_shouldFree = false;
	slot->tts_tuple = NULL;
	slot->tts_tupleDescriptor = NULL;
	slot->tts_mcxt = CurrentMemoryContext;
	slot->tts_buffer = InvalidBuffer;
	slot->tts_nvalid = 0;
	slot->tts_values = NULL;
	slot->tts_isnull = NULL;
	slot->tts_mintuple = NULL;

	ExecSetSlotDescriptor(slot, tupdesc);

	return slot;
}

/* --------------------------------
 *		ExecDropSingleTupleTableSlot
 *
 *		Release a TupleTableSlot made with MakeSingleTupleTableSlot.
 * --------------------------------
 */
void
ExecDropSingleTupleTableSlot(TupleTableSlot *slot)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);

	ExecClearTuple(slot);
	if (slot->tts_tupleDescriptor)
		ReleaseTupleDesc(slot->tts_tupleDescriptor);
	if (slot->tts_values)
		pfree(slot->tts_values);
	if (slot->tts_isnull)
		pfree(slot->tts_isnull);

	pfree(slot);
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

	/*
	 * sanity checks
	 */
	Assert(table != NULL);

	/*
	 * We expect that the table was made big enough to begin with. We cannot
	 * reallocate it on the fly since previous plan nodes have already got
	 * pointers to individual entries.
	 */
	if (table->next >= table->size)
		elog(ERROR, "plan requires more slots than are available");

	slotnum = table->next;
	table->next++;

	return &(table->array[slotnum]);
}

/* ----------------------------------------------------------------
 *				  tuple table slot accessor functions
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		ExecSetSlotDescriptor
 *
 *		This function is used to set the tuple descriptor associated
 *		with the slot's tuple.  The passed descriptor must have lifespan
 *		at least equal to the slot's.  If it is a reference-counted descriptor
 *		then the reference count is incremented for as long as the slot holds
 *		a reference.
 * --------------------------------
 */
void
ExecSetSlotDescriptor(TupleTableSlot *slot,		/* slot to change */
					  TupleDesc tupdesc)		/* new tuple descriptor */
{
	/* For safety, make sure slot is empty before changing it */
	ExecClearTuple(slot);

	/*
	 * Release any old descriptor.	Also release old Datum/isnull arrays if
	 * present (we don't bother to check if they could be re-used).
	 */
	if (slot->tts_tupleDescriptor)
		ReleaseTupleDesc(slot->tts_tupleDescriptor);

	if (slot->tts_values)
		pfree(slot->tts_values);
	if (slot->tts_isnull)
		pfree(slot->tts_isnull);

	/*
	 * Install the new descriptor; if it's refcounted, bump its refcount.
	 */
	slot->tts_tupleDescriptor = tupdesc;
	PinTupleDesc(tupdesc);

	/*
	 * Allocate Datum/isnull arrays of the appropriate size.  These must have
	 * the same lifetime as the slot, so allocate in the slot's own context.
	 */
	slot->tts_values = (Datum *)
		MemoryContextAlloc(slot->tts_mcxt, tupdesc->natts * sizeof(Datum));
	slot->tts_isnull = (bool *)
		MemoryContextAlloc(slot->tts_mcxt, tupdesc->natts * sizeof(bool));
}

/* --------------------------------
 *		ExecStoreTuple
 *
 *		This function is used to store a physical tuple into a specified
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
 *
 * NOTE: before PostgreSQL 8.1, this function would accept a NULL tuple
 * pointer and effectively behave like ExecClearTuple (though you could
 * still specify a buffer to pin, which would be an odd combination).
 * This saved a couple lines of code in a few places, but seemed more likely
 * to mask logic errors than to be really useful, so it's now disallowed.
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
	Assert(tuple != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	/* passing shouldFree=true for a tuple on a disk page is not sane */
	Assert(BufferIsValid(buffer) ? (!shouldFree) : true);

	/*
	 * Free any old physical tuple belonging to the slot.
	 */
	if (slot->tts_shouldFree)
	{
		if (slot->tts_mintuple)
			heap_free_minimal_tuple(slot->tts_mintuple);
		else
			heap_freetuple(slot->tts_tuple);
	}

	/*
	 * Store the new tuple into the specified slot.
	 */
	slot->tts_isempty = false;
	slot->tts_shouldFree = shouldFree;
	slot->tts_tuple = tuple;
	slot->tts_mintuple = NULL;

	/* Mark extracted state invalid */
	slot->tts_nvalid = 0;

	/*
	 * If tuple is on a disk page, keep the page pinned as long as we hold a
	 * pointer into it.  We assume the caller already has such a pin.
	 *
	 * This is coded to optimize the case where the slot previously held a
	 * tuple on the same disk page: in that case releasing and re-acquiring
	 * the pin is a waste of cycles.  This is a common situation during
	 * seqscans, so it's worth troubling over.
	 */
	if (slot->tts_buffer != buffer)
	{
		if (BufferIsValid(slot->tts_buffer))
			ReleaseBuffer(slot->tts_buffer);
		slot->tts_buffer = buffer;
		if (BufferIsValid(buffer))
			IncrBufferRefCount(buffer);
	}

	return slot;
}

/* --------------------------------
 *		ExecStoreMinimalTuple
 *
 *		Like ExecStoreTuple, but insert a "minimal" tuple into the slot.
 *
 * No 'buffer' parameter since minimal tuples are never stored in relations.
 * --------------------------------
 */
TupleTableSlot *
ExecStoreMinimalTuple(MinimalTuple mtup,
					  TupleTableSlot *slot,
					  bool shouldFree)
{
	/*
	 * sanity checks
	 */
	Assert(mtup != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);

	/*
	 * Free any old physical tuple belonging to the slot.
	 */
	if (slot->tts_shouldFree)
	{
		if (slot->tts_mintuple)
			heap_free_minimal_tuple(slot->tts_mintuple);
		else
			heap_freetuple(slot->tts_tuple);
	}

	/*
	 * Drop the pin on the referenced buffer, if there is one.
	 */
	if (BufferIsValid(slot->tts_buffer))
		ReleaseBuffer(slot->tts_buffer);

	slot->tts_buffer = InvalidBuffer;

	/*
	 * Store the new tuple into the specified slot.
	 */
	slot->tts_isempty = false;
	slot->tts_shouldFree = shouldFree;
	slot->tts_tuple = &slot->tts_minhdr;
	slot->tts_mintuple = mtup;

	slot->tts_minhdr.t_len = mtup->t_len + MINIMAL_TUPLE_OFFSET;
	slot->tts_minhdr.t_data = (HeapTupleHeader) ((char *) mtup - MINIMAL_TUPLE_OFFSET);
	/* no need to set t_self or t_tableOid since we won't allow access */

	/* Mark extracted state invalid */
	slot->tts_nvalid = 0;

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
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);

	/*
	 * Free the old physical tuple if necessary.
	 */
	if (slot->tts_shouldFree)
	{
		if (slot->tts_mintuple)
			heap_free_minimal_tuple(slot->tts_mintuple);
		else
			heap_freetuple(slot->tts_tuple);
	}

	slot->tts_tuple = NULL;
	slot->tts_mintuple = NULL;
	slot->tts_shouldFree = false;

	/*
	 * Drop the pin on the referenced buffer, if there is one.
	 */
	if (BufferIsValid(slot->tts_buffer))
		ReleaseBuffer(slot->tts_buffer);

	slot->tts_buffer = InvalidBuffer;

	/*
	 * Mark it empty.
	 */
	slot->tts_isempty = true;
	slot->tts_nvalid = 0;

	return slot;
}

/* --------------------------------
 *		ExecStoreVirtualTuple
 *			Mark a slot as containing a virtual tuple.
 *
 * The protocol for loading a slot with virtual tuple data is:
 *		* Call ExecClearTuple to mark the slot empty.
 *		* Store data into the Datum/isnull arrays.
 *		* Call ExecStoreVirtualTuple to mark the slot valid.
 * This is a bit unclean but it avoids one round of data copying.
 * --------------------------------
 */
TupleTableSlot *
ExecStoreVirtualTuple(TupleTableSlot *slot)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	Assert(slot->tts_isempty);

	slot->tts_isempty = false;
	slot->tts_nvalid = slot->tts_tupleDescriptor->natts;

	return slot;
}

/* --------------------------------
 *		ExecStoreAllNullTuple
 *			Set up the slot to contain a null in every column.
 *
 * At first glance this might sound just like ExecClearTuple, but it's
 * entirely different: the slot ends up full, not empty.
 * --------------------------------
 */
TupleTableSlot *
ExecStoreAllNullTuple(TupleTableSlot *slot)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);

	/* Clear any old contents */
	ExecClearTuple(slot);

	/*
	 * Fill all the columns of the virtual tuple with nulls
	 */
	MemSet(slot->tts_values, 0,
		   slot->tts_tupleDescriptor->natts * sizeof(Datum));
	memset(slot->tts_isnull, true,
		   slot->tts_tupleDescriptor->natts * sizeof(bool));

	return ExecStoreVirtualTuple(slot);
}

/* --------------------------------
 *		ExecCopySlotTuple
 *			Obtain a copy of a slot's regular physical tuple.  The copy is
 *			palloc'd in the current memory context.
 *
 *		This works even if the slot contains a virtual or minimal tuple;
 *		however the "system columns" of the result will not be meaningful.
 * --------------------------------
 */
HeapTuple
ExecCopySlotTuple(TupleTableSlot *slot)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(!slot->tts_isempty);

	/*
	 * If we have a physical tuple then just copy it.
	 */
	if (slot->tts_tuple)
	{
		if (slot->tts_mintuple)
			return heap_tuple_from_minimal_tuple(slot->tts_mintuple);
		else
			return heap_copytuple(slot->tts_tuple);
	}

	/*
	 * Otherwise we need to build a tuple from the Datum array.
	 */
	return heap_form_tuple(slot->tts_tupleDescriptor,
						   slot->tts_values,
						   slot->tts_isnull);
}

/* --------------------------------
 *		ExecCopySlotMinimalTuple
 *			Obtain a copy of a slot's minimal physical tuple.  The copy is
 *			palloc'd in the current memory context.
 * --------------------------------
 */
MinimalTuple
ExecCopySlotMinimalTuple(TupleTableSlot *slot)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(!slot->tts_isempty);

	/*
	 * If we have a physical tuple then just copy it.
	 */
	if (slot->tts_tuple)
	{
		if (slot->tts_mintuple)
			return heap_copy_minimal_tuple(slot->tts_mintuple);
		else
			return minimal_tuple_from_heap_tuple(slot->tts_tuple);
	}

	/*
	 * Otherwise we need to build a tuple from the Datum array.
	 */
	return heap_form_minimal_tuple(slot->tts_tupleDescriptor,
								   slot->tts_values,
								   slot->tts_isnull);
}

/* --------------------------------
 *		ExecFetchSlotTuple
 *			Fetch the slot's regular physical tuple.
 *
 *		If the slot contains a virtual tuple, we convert it to physical
 *		form.  The slot retains ownership of the physical tuple.
 *		Likewise, if it contains a minimal tuple we convert to regular form.
 *
 * The difference between this and ExecMaterializeSlot() is that this
 * does not guarantee that the contained tuple is local storage.
 * Hence, the result must be treated as read-only.
 * --------------------------------
 */
HeapTuple
ExecFetchSlotTuple(TupleTableSlot *slot)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(!slot->tts_isempty);

	/*
	 * If we have a regular physical tuple then just return it.
	 */
	if (slot->tts_tuple && slot->tts_mintuple == NULL)
		return slot->tts_tuple;

	/*
	 * Otherwise materialize the slot...
	 */
	return ExecMaterializeSlot(slot);
}

/* --------------------------------
 *		ExecFetchSlotMinimalTuple
 *			Fetch the slot's minimal physical tuple.
 *
 *		If the slot contains a virtual tuple, we convert it to minimal
 *		physical form.	The slot retains ownership of the physical tuple.
 *		Likewise, if it contains a regular tuple we convert to minimal form.
 *
 * As above, the result must be treated as read-only.
 * --------------------------------
 */
MinimalTuple
ExecFetchSlotMinimalTuple(TupleTableSlot *slot)
{
	MinimalTuple newTuple;
	MemoryContext oldContext;

	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(!slot->tts_isempty);

	/*
	 * If we have a minimal physical tuple then just return it.
	 */
	if (slot->tts_mintuple)
		return slot->tts_mintuple;

	/*
	 * Otherwise, build a minimal tuple, and then store it as the new slot
	 * value.  (Note: tts_nvalid will be reset to zero here.  There are cases
	 * in which this could be optimized but it's probably not worth worrying
	 * about.)
	 *
	 * We may be called in a context that is shorter-lived than the tuple
	 * slot, but we have to ensure that the materialized tuple will survive
	 * anyway.
	 */
	oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
	newTuple = ExecCopySlotMinimalTuple(slot);
	MemoryContextSwitchTo(oldContext);

	ExecStoreMinimalTuple(newTuple, slot, true);

	Assert(slot->tts_mintuple);
	return slot->tts_mintuple;
}

/* --------------------------------
 *		ExecMaterializeSlot
 *			Force a slot into the "materialized" state.
 *
 *		This causes the slot's tuple to be a local copy not dependent on
 *		any external storage.  A pointer to the contained tuple is returned.
 *
 *		A typical use for this operation is to prepare a computed tuple
 *		for being stored on disk.  The original data may or may not be
 *		virtual, but in any case we need a private copy for heap_insert
 *		to scribble on.
 * --------------------------------
 */
HeapTuple
ExecMaterializeSlot(TupleTableSlot *slot)
{
	HeapTuple	newTuple;
	MemoryContext oldContext;

	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(!slot->tts_isempty);

	/*
	 * If we have a regular physical tuple, and it's locally palloc'd, we have
	 * nothing to do.
	 */
	if (slot->tts_tuple && slot->tts_shouldFree && slot->tts_mintuple == NULL)
		return slot->tts_tuple;

	/*
	 * Otherwise, copy or build a tuple, and then store it as the new slot
	 * value.  (Note: tts_nvalid will be reset to zero here.  There are cases
	 * in which this could be optimized but it's probably not worth worrying
	 * about.)
	 *
	 * We may be called in a context that is shorter-lived than the tuple
	 * slot, but we have to ensure that the materialized tuple will survive
	 * anyway.
	 */
	oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
	newTuple = ExecCopySlotTuple(slot);
	MemoryContextSwitchTo(oldContext);

	ExecStoreTuple(newTuple, slot, InvalidBuffer, true);

	return slot->tts_tuple;
}

/* --------------------------------
 *		ExecCopySlot
 *			Copy the source slot's contents into the destination slot.
 *
 *		The destination acquires a private copy that will not go away
 *		if the source is cleared.
 *
 *		The caller must ensure the slots have compatible tupdescs.
 * --------------------------------
 */
TupleTableSlot *
ExecCopySlot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	HeapTuple	newTuple;
	MemoryContext oldContext;

	/*
	 * There might be ways to optimize this when the source is virtual, but
	 * for now just always build a physical copy.  Make sure it is in the
	 * right context.
	 */
	oldContext = MemoryContextSwitchTo(dstslot->tts_mcxt);
	newTuple = ExecCopySlotTuple(srcslot);
	MemoryContextSwitchTo(oldContext);

	return ExecStoreTuple(newTuple, dstslot, InvalidBuffer, true);
}


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

/* ----------------
 *		ExecInitResultTupleSlot
 * ----------------
 */
void
ExecInitResultTupleSlot(EState *estate, PlanState *planstate)
{
	planstate->ps_ResultTupleSlot = ExecAllocTableSlot(estate->es_tupleTable);
}

/* ----------------
 *		ExecInitScanTupleSlot
 * ----------------
 */
void
ExecInitScanTupleSlot(EState *estate, ScanState *scanstate)
{
	scanstate->ss_ScanTupleSlot = ExecAllocTableSlot(estate->es_tupleTable);
}

/* ----------------
 *		ExecInitExtraTupleSlot
 * ----------------
 */
TupleTableSlot *
ExecInitExtraTupleSlot(EState *estate)
{
	return ExecAllocTableSlot(estate->es_tupleTable);
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

	ExecSetSlotDescriptor(slot, tupType);

	return ExecStoreAllNullTuple(slot);
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
	return ExecTypeFromTLInternal(targetList, hasoid, false);
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
	return ExecTypeFromTLInternal(targetList, hasoid, true);
}

static TupleDesc
ExecTypeFromTLInternal(List *targetList, bool hasoid, bool skipjunk)
{
	TupleDesc	typeInfo;
	ListCell   *l;
	int			len;
	int			cur_resno = 1;

	if (skipjunk)
		len = ExecCleanTargetListLength(targetList);
	else
		len = ExecTargetListLength(targetList);
	typeInfo = CreateTemplateTupleDesc(len, hasoid);

	foreach(l, targetList)
	{
		TargetEntry *tle = lfirst(l);

		if (skipjunk && tle->resjunk)
			continue;
		TupleDescInitEntry(typeInfo,
						   cur_resno++,
						   tle->resname,
						   exprType((Node *) tle->expr),
						   exprTypmod((Node *) tle->expr),
						   0);
	}

	return typeInfo;
}

/*
 * ExecTypeFromExprList - build a tuple descriptor from a list of Exprs
 *
 * Here we must make up an arbitrary set of field names.
 */
TupleDesc
ExecTypeFromExprList(List *exprList)
{
	TupleDesc	typeInfo;
	ListCell   *l;
	int			cur_resno = 1;
	char		fldname[NAMEDATALEN];

	typeInfo = CreateTemplateTupleDesc(list_length(exprList), false);

	foreach(l, exprList)
	{
		Node	   *e = lfirst(l);

		sprintf(fldname, "f%d", cur_resno);

		TupleDescInitEntry(typeInfo,
						   cur_resno++,
						   fldname,
						   exprType(e),
						   exprTypmod(e),
						   0);
	}

	return typeInfo;
}

/*
 * BlessTupleDesc - make a completed tuple descriptor useful for SRFs
 *
 * Rowtype Datums returned by a function must contain valid type information.
 * This happens "for free" if the tupdesc came from a relcache entry, but
 * not if we have manufactured a tupdesc for a transient RECORD datatype.
 * In that case we have to notify typcache.c of the existence of the type.
 */
TupleDesc
BlessTupleDesc(TupleDesc tupdesc)
{
	if (tupdesc->tdtypeid == RECORDOID &&
		tupdesc->tdtypmod < 0)
		assign_record_type_typmod(tupdesc);

	return tupdesc;				/* just for notational convenience */
}

/*
 * TupleDescGetSlot - Initialize a slot based on the supplied tupledesc
 *
 * Note: this is obsolete; it is sufficient to call BlessTupleDesc on
 * the tupdesc.  We keep it around just for backwards compatibility with
 * existing user-written SRFs.
 */
TupleTableSlot *
TupleDescGetSlot(TupleDesc tupdesc)
{
	TupleTableSlot *slot;

	/* The useful work is here */
	BlessTupleDesc(tupdesc);

	/* Make a standalone slot */
	slot = MakeSingleTupleTableSlot(tupdesc);

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
	Oid		   *attioparams;
	int32	   *atttypmods;
	AttInMetadata *attinmeta;

	attinmeta = (AttInMetadata *) palloc(sizeof(AttInMetadata));

	/* "Bless" the tupledesc so that we can make rowtype datums with it */
	attinmeta->tupdesc = BlessTupleDesc(tupdesc);

	/*
	 * Gather info needed later to call the "in" function for each attribute
	 */
	attinfuncinfo = (FmgrInfo *) palloc0(natts * sizeof(FmgrInfo));
	attioparams = (Oid *) palloc0(natts * sizeof(Oid));
	atttypmods = (int32 *) palloc0(natts * sizeof(int32));

	for (i = 0; i < natts; i++)
	{
		/* Ignore dropped attributes */
		if (!tupdesc->attrs[i]->attisdropped)
		{
			atttypeid = tupdesc->attrs[i]->atttypid;
			getTypeInputInfo(atttypeid, &attinfuncid, &attioparams[i]);
			fmgr_info(attinfuncid, &attinfuncinfo[i]);
			atttypmods[i] = tupdesc->attrs[i]->atttypmod;
		}
	}
	attinmeta->attinfuncs = attinfuncinfo;
	attinmeta->attioparams = attioparams;
	attinmeta->atttypmods = atttypmods;

	return attinmeta;
}

/*
 * BuildTupleFromCStrings - build a HeapTuple given user data in C string form.
 * values is an array of C strings, one for each attribute of the return tuple.
 * A NULL string pointer indicates we want to create a NULL field.
 */
HeapTuple
BuildTupleFromCStrings(AttInMetadata *attinmeta, char **values)
{
	TupleDesc	tupdesc = attinmeta->tupdesc;
	int			natts = tupdesc->natts;
	Datum	   *dvalues;
	char	   *nulls;
	int			i;
	HeapTuple	tuple;

	dvalues = (Datum *) palloc(natts * sizeof(Datum));
	nulls = (char *) palloc(natts * sizeof(char));

	/* Call the "in" function for each non-dropped attribute */
	for (i = 0; i < natts; i++)
	{
		if (!tupdesc->attrs[i]->attisdropped)
		{
			/* Non-dropped attributes */
			dvalues[i] = InputFunctionCall(&attinmeta->attinfuncs[i],
										   values[i],
										   attinmeta->attioparams[i],
										   attinmeta->atttypmods[i]);
			if (values[i] != NULL)
				nulls[i] = ' ';
			else
				nulls[i] = 'n';
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
	 * Release locally palloc'd space.  XXX would probably be good to pfree
	 * values of pass-by-reference datums, as well.
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
	tstate->slot = MakeSingleTupleTableSlot(tupdesc);
	tstate->dest = dest;

	(*tstate->dest->rStartup) (tstate->dest, (int) CMD_SELECT, tupdesc);

	return tstate;
}

/*
 * write a single tuple
 *
 * values is a list of the external C string representations of the values
 * to be projected.
 *
 * XXX This could be made more efficient, since in reality we probably only
 * need a virtual tuple.
 */
void
do_tup_output(TupOutputState *tstate, char **values)
{
	/* build a tuple from the input strings using the tupdesc */
	HeapTuple	tuple = BuildTupleFromCStrings(tstate->metadata, values);

	/* put it in a slot */
	ExecStoreTuple(tuple, tstate->slot, InvalidBuffer, true);

	/* send the tuple to the receiver */
	(*tstate->dest->receiveSlot) (tstate->slot, tstate->dest);

	/* clean up */
	ExecClearTuple(tstate->slot);
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
	ExecDropSingleTupleTableSlot(tstate->slot);
	/* XXX worth cleaning up the attinmetadata? */
	pfree(tstate);
}
