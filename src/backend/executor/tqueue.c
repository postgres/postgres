/*-------------------------------------------------------------------------
 *
 * tqueue.c
 *	  Use shm_mq to send & receive tuples between parallel backends
 *
 * A DestReceiver of type DestTupleQueue, which is a TQueueDestReceiver
 * under the hood, writes tuples from the executor to a shm_mq.
 *
 * A TupleQueueFunnel helps manage the process of reading tuples from
 * one or more shm_mq objects being used as tuple queues.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
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
#include "miscadmin.h"

typedef struct
{
	DestReceiver pub;
	shm_mq_handle *handle;
}	TQueueDestReceiver;

struct TupleQueueFunnel
{
	int			nqueues;
	int			maxqueues;
	int			nextqueue;
	shm_mq_handle **queue;
};

/*
 * Receive a tuple.
 */
static void
tqueueReceiveSlot(TupleTableSlot *slot, DestReceiver *self)
{
	TQueueDestReceiver *tqueue = (TQueueDestReceiver *) self;
	HeapTuple	tuple;

	tuple = ExecMaterializeSlot(slot);
	shm_mq_send(tqueue->handle, tuple->t_len, tuple->t_data, false);
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

	shm_mq_detach(shm_mq_get_queue(tqueue->handle));
}

/*
 * Destroy receiver when done with it
 */
static void
tqueueDestroyReceiver(DestReceiver *self)
{
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
	self->handle = handle;

	return (DestReceiver *) self;
}

/*
 * Create a tuple queue funnel.
 */
TupleQueueFunnel *
CreateTupleQueueFunnel(void)
{
	TupleQueueFunnel *funnel = palloc0(sizeof(TupleQueueFunnel));

	funnel->maxqueues = 8;
	funnel->queue = palloc(funnel->maxqueues * sizeof(shm_mq_handle *));

	return funnel;
}

/*
 * Destroy a tuple queue funnel.
 */
void
DestroyTupleQueueFunnel(TupleQueueFunnel *funnel)
{
	int			i;

	for (i = 0; i < funnel->nqueues; i++)
		shm_mq_detach(shm_mq_get_queue(funnel->queue[i]));
	pfree(funnel->queue);
	pfree(funnel);
}

/*
 * Remember the shared memory queue handle in funnel.
 */
void
RegisterTupleQueueOnFunnel(TupleQueueFunnel *funnel, shm_mq_handle *handle)
{
	if (funnel->nqueues < funnel->maxqueues)
	{
		funnel->queue[funnel->nqueues++] = handle;
		return;
	}

	if (funnel->nqueues >= funnel->maxqueues)
	{
		int			newsize = funnel->nqueues * 2;

		Assert(funnel->nqueues == funnel->maxqueues);

		funnel->queue = repalloc(funnel->queue,
								 newsize * sizeof(shm_mq_handle *));
		funnel->maxqueues = newsize;
	}

	funnel->queue[funnel->nqueues++] = handle;
}

/*
 * Fetch a tuple from a tuple queue funnel.
 *
 * We try to read from the queues in round-robin fashion so as to avoid
 * the situation where some workers get their tuples read expediently while
 * others are barely ever serviced.
 *
 * Even when nowait = false, we read from the individual queues in
 * non-blocking mode.  Even when shm_mq_receive() returns SHM_MQ_WOULD_BLOCK,
 * it can still accumulate bytes from a partially-read message, so doing it
 * this way should outperform doing a blocking read on each queue in turn.
 *
 * The return value is NULL if there are no remaining queues or if
 * nowait = true and no queue returned a tuple without blocking.  *done, if
 * not NULL, is set to true when there are no remaining queues and false in
 * any other case.
 */
HeapTuple
TupleQueueFunnelNext(TupleQueueFunnel *funnel, bool nowait, bool *done)
{
	int			waitpos = funnel->nextqueue;

	/* Corner case: called before adding any queues, or after all are gone. */
	if (funnel->nqueues == 0)
	{
		if (done != NULL)
			*done = true;
		return NULL;
	}

	if (done != NULL)
		*done = false;

	for (;;)
	{
		shm_mq_handle *mqh = funnel->queue[funnel->nextqueue];
		shm_mq_result result;
		Size		nbytes;
		void	   *data;

		/* Attempt to read a message. */
		result = shm_mq_receive(mqh, &nbytes, &data, true);

		/*
		 * Normally, we advance funnel->nextqueue to the next queue at this
		 * point, but if we're pointing to a queue that we've just discovered
		 * is detached, then forget that queue and leave the pointer where it
		 * is until the number of remaining queues fall below that pointer and
		 * at that point make the pointer point to the first queue.
		 */
		if (result != SHM_MQ_DETACHED)
			funnel->nextqueue = (funnel->nextqueue + 1) % funnel->nqueues;
		else
		{
			--funnel->nqueues;
			if (funnel->nqueues == 0)
			{
				if (done != NULL)
					*done = true;
				return NULL;
			}

			memmove(&funnel->queue[funnel->nextqueue],
					&funnel->queue[funnel->nextqueue + 1],
					sizeof(shm_mq_handle *)
					* (funnel->nqueues - funnel->nextqueue));

			if (funnel->nextqueue >= funnel->nqueues)
				funnel->nextqueue = 0;

			if (funnel->nextqueue < waitpos)
				--waitpos;

			continue;
		}

		/* If we got a message, return it. */
		if (result == SHM_MQ_SUCCESS)
		{
			HeapTupleData htup;

			/*
			 * The tuple data we just read from the queue is only valid until
			 * we again attempt to read from it.  Copy the tuple into a single
			 * palloc'd chunk as callers will expect.
			 */
			ItemPointerSetInvalid(&htup.t_self);
			htup.t_tableOid = InvalidOid;
			htup.t_len = nbytes;
			htup.t_data = data;
			return heap_copytuple(&htup);
		}

		/*
		 * If we've visited all of the queues, then we should either give up
		 * and return NULL (if we're in non-blocking mode) or wait for the
		 * process latch to be set (otherwise).
		 */
		if (funnel->nextqueue == waitpos)
		{
			if (nowait)
				return NULL;
			WaitLatch(MyLatch, WL_LATCH_SET, 0);
			CHECK_FOR_INTERRUPTS();
			ResetLatch(MyLatch);
		}
	}
}
