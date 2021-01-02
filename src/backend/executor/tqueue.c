/*-------------------------------------------------------------------------
 *
 * tqueue.c
 *	  Use shm_mq to send & receive tuples between parallel backends
 *
 * A DestReceiver of type DestTupleQueue, which is a TQueueDestReceiver
 * under the hood, writes tuples from the executor to a shm_mq.
 *
 * A TupleQueueReader reads tuples from a shm_mq and returns the tuples.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/tqueue.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "executor/tqueue.h"

/*
 * DestReceiver object's private contents
 *
 * queue is a pointer to data supplied by DestReceiver's caller.
 */
typedef struct TQueueDestReceiver
{
	DestReceiver pub;			/* public fields */
	shm_mq_handle *queue;		/* shm_mq to send to */
} TQueueDestReceiver;

/*
 * TupleQueueReader object's private contents
 *
 * queue is a pointer to data supplied by reader's caller.
 *
 * "typedef struct TupleQueueReader TupleQueueReader" is in tqueue.h
 */
struct TupleQueueReader
{
	shm_mq_handle *queue;		/* shm_mq to receive from */
};

/*
 * Receive a tuple from a query, and send it to the designated shm_mq.
 *
 * Returns true if successful, false if shm_mq has been detached.
 */
static bool
tqueueReceiveSlot(TupleTableSlot *slot, DestReceiver *self)
{
	TQueueDestReceiver *tqueue = (TQueueDestReceiver *) self;
	MinimalTuple tuple;
	shm_mq_result result;
	bool		should_free;

	/* Send the tuple itself. */
	tuple = ExecFetchSlotMinimalTuple(slot, &should_free);
	result = shm_mq_send(tqueue->queue, tuple->t_len, tuple, false);

	if (should_free)
		pfree(tuple);

	/* Check for failure. */
	if (result == SHM_MQ_DETACHED)
		return false;
	else if (result != SHM_MQ_SUCCESS)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not send tuple to shared-memory queue")));

	return true;
}

/*
 * Prepare to receive tuples from executor.
 */
static void
tqueueStartupReceiver(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	/* do nothing */
}

/*
 * Clean up at end of an executor run
 */
static void
tqueueShutdownReceiver(DestReceiver *self)
{
	TQueueDestReceiver *tqueue = (TQueueDestReceiver *) self;

	if (tqueue->queue != NULL)
		shm_mq_detach(tqueue->queue);
	tqueue->queue = NULL;
}

/*
 * Destroy receiver when done with it
 */
static void
tqueueDestroyReceiver(DestReceiver *self)
{
	TQueueDestReceiver *tqueue = (TQueueDestReceiver *) self;

	/* We probably already detached from queue, but let's be sure */
	if (tqueue->queue != NULL)
		shm_mq_detach(tqueue->queue);
	pfree(self);
}

/*
 * Create a DestReceiver that writes tuples to a tuple queue.
 */
DestReceiver *
CreateTupleQueueDestReceiver(shm_mq_handle *handle)
{
	TQueueDestReceiver *self;

	self = (TQueueDestReceiver *) palloc0(sizeof(TQueueDestReceiver));

	self->pub.receiveSlot = tqueueReceiveSlot;
	self->pub.rStartup = tqueueStartupReceiver;
	self->pub.rShutdown = tqueueShutdownReceiver;
	self->pub.rDestroy = tqueueDestroyReceiver;
	self->pub.mydest = DestTupleQueue;
	self->queue = handle;

	return (DestReceiver *) self;
}

/*
 * Create a tuple queue reader.
 */
TupleQueueReader *
CreateTupleQueueReader(shm_mq_handle *handle)
{
	TupleQueueReader *reader = palloc0(sizeof(TupleQueueReader));

	reader->queue = handle;

	return reader;
}

/*
 * Destroy a tuple queue reader.
 *
 * Note: cleaning up the underlying shm_mq is the caller's responsibility.
 * We won't access it here, as it may be detached already.
 */
void
DestroyTupleQueueReader(TupleQueueReader *reader)
{
	pfree(reader);
}

/*
 * Fetch a tuple from a tuple queue reader.
 *
 * The return value is NULL if there are no remaining tuples or if
 * nowait = true and no tuple is ready to return.  *done, if not NULL,
 * is set to true when there are no remaining tuples and otherwise to false.
 *
 * The returned tuple, if any, is either in shared memory or a private buffer
 * and should not be freed.  The pointer is invalid after the next call to
 * TupleQueueReaderNext().
 *
 * Even when shm_mq_receive() returns SHM_MQ_WOULD_BLOCK, this can still
 * accumulate bytes from a partially-read message, so it's useful to call
 * this with nowait = true even if nothing is returned.
 */
MinimalTuple
TupleQueueReaderNext(TupleQueueReader *reader, bool nowait, bool *done)
{
	MinimalTuple tuple;
	shm_mq_result result;
	Size		nbytes;
	void	   *data;

	if (done != NULL)
		*done = false;

	/* Attempt to read a message. */
	result = shm_mq_receive(reader->queue, &nbytes, &data, nowait);

	/* If queue is detached, set *done and return NULL. */
	if (result == SHM_MQ_DETACHED)
	{
		if (done != NULL)
			*done = true;
		return NULL;
	}

	/* In non-blocking mode, bail out if no message ready yet. */
	if (result == SHM_MQ_WOULD_BLOCK)
		return NULL;
	Assert(result == SHM_MQ_SUCCESS);

	/*
	 * Return a pointer to the queue memory directly (which had better be
	 * sufficiently aligned).
	 */
	tuple = (MinimalTuple) data;
	Assert(tuple->t_len == nbytes);

	return tuple;
}
