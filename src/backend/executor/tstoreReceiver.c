/*-------------------------------------------------------------------------
 *
 * tstore_receiver.c
 *	  an implementation of DestReceiver that stores the result tuples in
 *	  a Tuplestore
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/tstoreReceiver.c,v 1.2 2003/04/29 03:21:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/tstoreReceiver.h"
#include "utils/memutils.h"
#include "utils/portal.h"

typedef struct
{
	DestReceiver		pub;
	Tuplestorestate    *tstore;
	MemoryContext		cxt;
} TStoreState;


/*
 * Prepare to receive tuples from executor.
 *
 * XXX: As currently implemented, this routine is a hack: there should
 * be no tie between this code and the portal system. Instead, the
 * receiver function that is part of DestFunction should be passed a
 * QueryDesc, so that the call site of ExecutorRun can "sub-class"
 * QueryDesc and pass in any necessary addition information (in this
 * case, the Tuplestore to use).
 */
static void
tstoreSetupReceiver(DestReceiver *self, int operation,
					const char *portalname, TupleDesc typeinfo)
{
	TStoreState *myState = (TStoreState *) self;
	Portal portal;

	if (operation != CMD_SELECT)
		elog(ERROR, "Unexpected operation type: %d", operation);

	portal = GetPortalByName(portalname);

	if (portal == NULL)
		elog(ERROR, "Specified portal does not exist: %s", portalname);

	myState->tstore = portal->holdStore;
	myState->cxt = portal->holdContext;
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
 * Clean up
 */
static void
tstoreCleanupReceiver(DestReceiver *self)
{
	/* do nothing */
}

/*
 * Initially create a DestReceiver object.
 */
DestReceiver *
tstoreReceiverCreateDR(void)
{
	TStoreState *self = (TStoreState *) palloc(sizeof(TStoreState));

	self->pub.receiveTuple = tstoreReceiveTuple;
	self->pub.setup = tstoreSetupReceiver;
	self->pub.cleanup = tstoreCleanupReceiver;

	self->tstore = NULL;
	self->cxt = NULL;

	return (DestReceiver *) self;
}
