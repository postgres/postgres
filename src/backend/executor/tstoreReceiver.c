/*-------------------------------------------------------------------------
 *
 * tstoreReceiver.c
 *	  An implementation of DestReceiver that stores the result tuples in
 *	  a Tuplestore.
 *
 * Optionally, we can force detoasting (but not decompression) of out-of-line
 * toasted values.	This is to support cursors WITH HOLD, which must retain
 * data even if the underlying table is dropped.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/tstoreReceiver.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tuptoaster.h"
#include "executor/tstoreReceiver.h"


typedef struct
{
	DestReceiver pub;
	/* parameters: */
	Tuplestorestate *tstore;	/* where to put the data */
	MemoryContext cxt;			/* context containing tstore */
	bool		detoast;		/* were we told to detoast? */
	/* workspace: */
	Datum	   *outvalues;		/* values array for result tuple */
	Datum	   *tofree;			/* temp values to be pfree'd */
} TStoreState;


static void tstoreReceiveSlot_notoast(TupleTableSlot *slot, DestReceiver *self);
static void tstoreReceiveSlot_detoast(TupleTableSlot *slot, DestReceiver *self);


/*
 * Prepare to receive tuples from executor.
 */
static void
tstoreStartupReceiver(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	TStoreState *myState = (TStoreState *) self;
	bool		needtoast = false;
	Form_pg_attribute *attrs = typeinfo->attrs;
	int			natts = typeinfo->natts;
	int			i;

	/* Check if any columns require detoast work */
	if (myState->detoast)
	{
		for (i = 0; i < natts; i++)
		{
			if (attrs[i]->attisdropped)
				continue;
			if (attrs[i]->attlen == -1)
			{
				needtoast = true;
				break;
			}
		}
	}

	/* Set up appropriate callback */
	if (needtoast)
	{
		myState->pub.receiveSlot = tstoreReceiveSlot_detoast;
		/* Create workspace */
		myState->outvalues = (Datum *)
			MemoryContextAlloc(myState->cxt, natts * sizeof(Datum));
		myState->tofree = (Datum *)
			MemoryContextAlloc(myState->cxt, natts * sizeof(Datum));
	}
	else
	{
		myState->pub.receiveSlot = tstoreReceiveSlot_notoast;
		myState->outvalues = NULL;
		myState->tofree = NULL;
	}
}

/*
 * Receive a tuple from the executor and store it in the tuplestore.
 * This is for the easy case where we don't have to detoast.
 */
static void
tstoreReceiveSlot_notoast(TupleTableSlot *slot, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;

	tuplestore_puttupleslot(myState->tstore, slot);
}

/*
 * Receive a tuple from the executor and store it in the tuplestore.
 * This is for the case where we have to detoast any toasted values.
 */
static void
tstoreReceiveSlot_detoast(TupleTableSlot *slot, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	Form_pg_attribute *attrs = typeinfo->attrs;
	int			natts = typeinfo->natts;
	int			nfree;
	int			i;
	MemoryContext oldcxt;

	/* Make sure the tuple is fully deconstructed */
	slot_getallattrs(slot);

	/*
	 * Fetch back any out-of-line datums.  We build the new datums array in
	 * myState->outvalues[] (but we can re-use the slot's isnull array). Also,
	 * remember the fetched values to free afterwards.
	 */
	nfree = 0;
	for (i = 0; i < natts; i++)
	{
		Datum		val = slot->tts_values[i];

		if (!attrs[i]->attisdropped &&
			attrs[i]->attlen == -1 &&
			!slot->tts_isnull[i])
		{
			if (VARATT_IS_EXTERNAL(DatumGetPointer(val)))
			{
				val = PointerGetDatum(heap_tuple_fetch_attr((struct varlena *)
													  DatumGetPointer(val)));
				myState->tofree[nfree++] = val;
			}
		}

		myState->outvalues[i] = val;
	}

	/*
	 * Push the modified tuple into the tuplestore.
	 */
	oldcxt = MemoryContextSwitchTo(myState->cxt);
	tuplestore_putvalues(myState->tstore, typeinfo,
						 myState->outvalues, slot->tts_isnull);
	MemoryContextSwitchTo(oldcxt);

	/* And release any temporary detoasted values */
	for (i = 0; i < nfree; i++)
		pfree(DatumGetPointer(myState->tofree[i]));
}

/*
 * Clean up at end of an executor run
 */
static void
tstoreShutdownReceiver(DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;

	/* Release workspace if any */
	if (myState->outvalues)
		pfree(myState->outvalues);
	myState->outvalues = NULL;
	if (myState->tofree)
		pfree(myState->tofree);
	myState->tofree = NULL;
}

/*
 * Destroy receiver when done with it
 */
static void
tstoreDestroyReceiver(DestReceiver *self)
{
	pfree(self);
}

/*
 * Initially create a DestReceiver object.
 */
DestReceiver *
CreateTuplestoreDestReceiver(void)
{
	TStoreState *self = (TStoreState *) palloc0(sizeof(TStoreState));

	self->pub.receiveSlot = tstoreReceiveSlot_notoast;	/* might change */
	self->pub.rStartup = tstoreStartupReceiver;
	self->pub.rShutdown = tstoreShutdownReceiver;
	self->pub.rDestroy = tstoreDestroyReceiver;
	self->pub.mydest = DestTuplestore;

	/* private fields will be set by SetTuplestoreDestReceiverParams */

	return (DestReceiver *) self;
}

/*
 * Set parameters for a TuplestoreDestReceiver
 */
void
SetTuplestoreDestReceiverParams(DestReceiver *self,
								Tuplestorestate *tStore,
								MemoryContext tContext,
								bool detoast)
{
	TStoreState *myState = (TStoreState *) self;

	Assert(myState->pub.mydest == DestTuplestore);
	myState->tstore = tStore;
	myState->cxt = tContext;
	myState->detoast = detoast;
}
