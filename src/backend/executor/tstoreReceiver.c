/*-------------------------------------------------------------------------
 *
 * tstoreReceiver.c
 *	  An implementation of DestReceiver that stores the result tuples in
 *	  a Tuplestore.
 *
 * Optionally, we can force detoasting (but not decompression) of out-of-line
 * toasted values.  This is to support cursors WITH HOLD, which must retain
 * data even if the underlying table is dropped.
 *
 * Also optionally, we can apply a tuple conversion map before storing.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/tstoreReceiver.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/tupconvert.h"
#include "executor/tstoreReceiver.h"


typedef struct
{
	DestReceiver pub;
	/* parameters: */
	Tuplestorestate *tstore;	/* where to put the data */
	MemoryContext cxt;			/* context containing tstore */
	bool		detoast;		/* were we told to detoast? */
	TupleDesc	target_tupdesc; /* target tupdesc, or NULL if none */
	const char *map_failure_msg;	/* tupdesc mapping failure message */
	/* workspace: */
	Datum	   *outvalues;		/* values array for result tuple */
	Datum	   *tofree;			/* temp values to be pfree'd */
	TupleConversionMap *tupmap; /* conversion map, if needed */
	TupleTableSlot *mapslot;	/* slot for mapped tuples */
} TStoreState;


static bool tstoreReceiveSlot_notoast(TupleTableSlot *slot, DestReceiver *self);
static bool tstoreReceiveSlot_detoast(TupleTableSlot *slot, DestReceiver *self);
static bool tstoreReceiveSlot_tupmap(TupleTableSlot *slot, DestReceiver *self);


/*
 * Prepare to receive tuples from executor.
 */
static void
tstoreStartupReceiver(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	TStoreState *myState = (TStoreState *) self;
	bool		needtoast = false;
	int			natts = typeinfo->natts;
	int			i;

	/* Check if any columns require detoast work */
	if (myState->detoast)
	{
		for (i = 0; i < natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(typeinfo, i);

			if (attr->attisdropped)
				continue;
			if (attr->attlen == -1)
			{
				needtoast = true;
				break;
			}
		}
	}

	/* Check if tuple conversion is needed */
	if (myState->target_tupdesc)
		myState->tupmap = convert_tuples_by_position(typeinfo,
													 myState->target_tupdesc,
													 myState->map_failure_msg);
	else
		myState->tupmap = NULL;

	/* Set up appropriate callback */
	if (needtoast)
	{
		Assert(!myState->tupmap);
		myState->pub.receiveSlot = tstoreReceiveSlot_detoast;
		/* Create workspace */
		myState->outvalues = (Datum *)
			MemoryContextAlloc(myState->cxt, natts * sizeof(Datum));
		myState->tofree = (Datum *)
			MemoryContextAlloc(myState->cxt, natts * sizeof(Datum));
		myState->mapslot = NULL;
	}
	else if (myState->tupmap)
	{
		myState->pub.receiveSlot = tstoreReceiveSlot_tupmap;
		myState->outvalues = NULL;
		myState->tofree = NULL;
		myState->mapslot = MakeSingleTupleTableSlot(myState->target_tupdesc,
													&TTSOpsVirtual);
	}
	else
	{
		myState->pub.receiveSlot = tstoreReceiveSlot_notoast;
		myState->outvalues = NULL;
		myState->tofree = NULL;
		myState->mapslot = NULL;
	}
}

/*
 * Receive a tuple from the executor and store it in the tuplestore.
 * This is for the easy case where we don't have to detoast nor map anything.
 */
static bool
tstoreReceiveSlot_notoast(TupleTableSlot *slot, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;

	tuplestore_puttupleslot(myState->tstore, slot);

	return true;
}

/*
 * Receive a tuple from the executor and store it in the tuplestore.
 * This is for the case where we have to detoast any toasted values.
 */
static bool
tstoreReceiveSlot_detoast(TupleTableSlot *slot, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
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
		Form_pg_attribute attr = TupleDescAttr(typeinfo, i);

		if (!attr->attisdropped && attr->attlen == -1 && !slot->tts_isnull[i])
		{
			if (VARATT_IS_EXTERNAL(DatumGetPointer(val)))
			{
				val = PointerGetDatum(detoast_external_attr((struct varlena *)
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

	return true;
}

/*
 * Receive a tuple from the executor and store it in the tuplestore.
 * This is for the case where we must apply a tuple conversion map.
 */
static bool
tstoreReceiveSlot_tupmap(TupleTableSlot *slot, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;

	execute_attr_map_slot(myState->tupmap->attrMap, slot, myState->mapslot);
	tuplestore_puttupleslot(myState->tstore, myState->mapslot);

	return true;
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
	if (myState->tupmap)
		free_conversion_map(myState->tupmap);
	myState->tupmap = NULL;
	if (myState->mapslot)
		ExecDropSingleTupleTableSlot(myState->mapslot);
	myState->mapslot = NULL;
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
 *
 * tStore: where to store the tuples
 * tContext: memory context containing tStore
 * detoast: forcibly detoast contained data?
 * target_tupdesc: if not NULL, forcibly convert tuples to this rowtype
 * map_failure_msg: error message to use if mapping to target_tupdesc fails
 *
 * We don't currently support both detoast and target_tupdesc at the same
 * time, just because no existing caller needs that combination.
 */
void
SetTuplestoreDestReceiverParams(DestReceiver *self,
								Tuplestorestate *tStore,
								MemoryContext tContext,
								bool detoast,
								TupleDesc target_tupdesc,
								const char *map_failure_msg)
{
	TStoreState *myState = (TStoreState *) self;

	Assert(!(detoast && target_tupdesc));

	Assert(myState->pub.mydest == DestTuplestore);
	myState->tstore = tStore;
	myState->cxt = tContext;
	myState->detoast = detoast;
	myState->target_tupdesc = target_tupdesc;
	myState->map_failure_msg = map_failure_msg;
}
