/*-------------------------------------------------------------------------
 *
 * tstoreReceiver.c
 *	  an implementation of DestReceiver that stores the result tuples in
 *	  a Tuplestore
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/tstoreReceiver.c,v 1.7 2003/08/04 00:43:18 momjian Exp $
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
}	TStoreState;


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
tstoreReceiveTuple(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;
	MemoryContext oldcxt = MemoryContextSwitchTo(myState->cxt);

	tuplestore_puttuple(myState->tstore, tuple);

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
CreateTuplestoreDestReceiver(Tuplestorestate *tStore,
							 MemoryContext tContext)
{
	TStoreState *self = (TStoreState *) palloc(sizeof(TStoreState));

	self->pub.receiveTuple = tstoreReceiveTuple;
	self->pub.startup = tstoreStartupReceiver;
	self->pub.shutdown = tstoreShutdownReceiver;
	self->pub.destroy = tstoreDestroyReceiver;
	self->pub.mydest = Tuplestore;

	self->tstore = tStore;
	self->cxt = tContext;

	return (DestReceiver *) self;
}
