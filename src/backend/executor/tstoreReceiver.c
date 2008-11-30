/*-------------------------------------------------------------------------
 *
 * tstoreReceiver.c
 *	  an implementation of DestReceiver that stores the result tuples in
 *	  a Tuplestore
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/tstoreReceiver.c,v 1.20 2008/11/30 20:51:25 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/tstoreReceiver.h"


typedef struct
{
	DestReceiver pub;
	Tuplestorestate *tstore;
	MemoryContext cxt;
} TStoreState;


/*
 * Prepare to receive tuples from executor.
 */
static void
tstoreStartupReceiver(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	/* do nothing */
}

/*
 * Receive a tuple from the executor and store it in the tuplestore.
 */
static void
tstoreReceiveSlot(TupleTableSlot *slot, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;
	MemoryContext oldcxt = MemoryContextSwitchTo(myState->cxt);

	tuplestore_puttupleslot(myState->tstore, slot);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Clean up at end of an executor run
 */
static void
tstoreShutdownReceiver(DestReceiver *self)
{
	/* do nothing */
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

	self->pub.receiveSlot = tstoreReceiveSlot;
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
								MemoryContext tContext)
{
	TStoreState *myState = (TStoreState *) self;

	Assert(myState->pub.mydest == DestTuplestore);
	myState->tstore = tStore;
	myState->cxt = tContext;
}
