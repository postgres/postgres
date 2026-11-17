/*-------------------------------------------------------------------------
 *
 * execAsync.c
 *	  Support routines for asynchronous execution
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execAsync.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execAsync.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodeAppend.h"
#include "executor/nodeForeignscan.h"
#include "executor/nodeMergeAppend.h"
#include "miscadmin.h"

#define EVENT_BUFFER_SIZE			16

/*
 * Asynchronously request a tuple from a designed async-capable node.
 */
void
ExecAsyncRequest(AsyncRequest *areq)
{
	if (areq->requestee->chgParam != NULL)	/* something changed? */
		ExecReScan(areq->requestee);	/* let ReScan handle this */

	/* must provide our own instrumentation support */
	if (areq->requestee->instrument)
		InstrStartNode(areq->requestee->instrument);

	switch (nodeTag(areq->requestee))
	{
		case T_ForeignScanState:
			ExecAsyncForeignScanRequest(areq);
			break;
		default:
			/* If the node doesn't support async, caller messed up. */
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(areq->requestee));
	}

	ExecAsyncResponse(areq);

	/* must provide our own instrumentation support */
	if (areq->requestee->instrument)
		InstrStopNode(areq->requestee->instrument,
					  TupIsNull(areq->result) ? 0.0 : 1.0);
}

/*
 * Give the asynchronous node a chance to configure the file descriptor event
 * for which it wishes to wait.  We expect the node-type specific callback to
 * make a single call of the following form:
 *
 * AddWaitEventToSet(set, WL_SOCKET_READABLE, fd, NULL, areq);
 */
void
ExecAsyncConfigureWait(AsyncRequest *areq)
{
	/* must provide our own instrumentation support */
	if (areq->requestee->instrument)
		InstrStartNode(areq->requestee->instrument);

	switch (nodeTag(areq->requestee))
	{
		case T_ForeignScanState:
			ExecAsyncForeignScanConfigureWait(areq);
			break;
		default:
			/* If the node doesn't support async, caller messed up. */
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(areq->requestee));
	}

	/* must provide our own instrumentation support */
	if (areq->requestee->instrument)
		InstrStopNode(areq->requestee->instrument, 0.0);
}

/*
 * Call the asynchronous node back when a relevant event has occurred.
 */
void
ExecAsyncNotify(AsyncRequest *areq)
{
	/* must provide our own instrumentation support */
	if (areq->requestee->instrument)
		InstrStartNode(areq->requestee->instrument);

	switch (nodeTag(areq->requestee))
	{
		case T_ForeignScanState:
			ExecAsyncForeignScanNotify(areq);
			break;
		default:
			/* If the node doesn't support async, caller messed up. */
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(areq->requestee));
	}

	ExecAsyncResponse(areq);

	/* must provide our own instrumentation support */
	if (areq->requestee->instrument)
		InstrStopNode(areq->requestee->instrument,
					  TupIsNull(areq->result) ? 0.0 : 1.0);
}

/*
 * Call the requestor back when an asynchronous node has produced a result.
 */
void
ExecAsyncResponse(AsyncRequest *areq)
{
	switch (nodeTag(areq->requestor))
	{
		case T_AppendState:
			ExecAsyncAppendResponse(areq);
			break;
		case T_MergeAppendState:
			ExecAsyncMergeAppendResponse(areq);
			break;
		default:
			/* If the node doesn't support async, caller messed up. */
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(areq->requestor));
	}
}

/*
 * A requestee node should call this function to deliver the tuple to its
 * requestor node.  The requestee node can call this from its ExecAsyncRequest
 * or ExecAsyncNotify callback.
 */
void
ExecAsyncRequestDone(AsyncRequest *areq, TupleTableSlot *result)
{
	areq->request_complete = true;
	areq->result = result;
}

/*
 * A requestee node should call this function to indicate that it is pending
 * for a callback.  The requestee node can call this from its ExecAsyncRequest
 * or ExecAsyncNotify callback.
 */
void
ExecAsyncRequestPending(AsyncRequest *areq)
{
	areq->callback_pending = true;
	areq->request_complete = false;
	areq->result = NULL;
}

/*
 * Fetch the WaitEventSet associated with an async-aware plan.
 */
WaitEventSet *
ExecAsyncGetRequestorEventSet(PlanState *requestor)
{
	switch (nodeTag(requestor))
	{
		case T_AppendState:
			return ((AppendState *) requestor)->as_eventset;
		case T_MergeAppendState:
			return ((MergeAppendState *) requestor)->ms_eventset;
		default:
			/* If the node doesn't support async, caller messed up. */
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(requestor));
			return NULL;
	}
}

/*
 * Workhorse function for Exec{Merge}AppendAsyncEventWait.
 */
void
ExecAsyncEventWait(Bitmapset *asyncplans,
				   AsyncRequest **asyncrequests,
				   WaitEventSet *eventset,
				   int nevents,
				   long timeout,
				   uint32 wait_event_info)
{
	WaitEvent	occurred_event[EVENT_BUFFER_SIZE];
	int			noccurred;
	int			i;

	/*
	 * Add WL_EXIT_ON_PM_DEATH to the set, to exit immediately if the
	 * postmaster dies.
	 */
	AddWaitEventToSet(eventset, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
					  NULL, NULL);

	/* Give each waiting subplan a chance to add an event. */
	i = -1;
	while ((i = bms_next_member(asyncplans, i)) >= 0)
	{
		AsyncRequest *areq = asyncrequests[i];

		if (areq->callback_pending)
			ExecAsyncConfigureWait(areq);
	}

	/*
	 * No need for further processing if none of the subplans configured any
	 * events.
	 */
	if (GetNumRegisteredWaitEvents(eventset) == 1)
		return;

	/*
	 * Add the process latch to the set, so that we wake up to process the
	 * standard interrupts with CHECK_FOR_INTERRUPTS().
	 *
	 * NOTE: For historical reasons, it's important that this is added to the
	 * WaitEventSet after the ExecAsyncConfigureWait() calls.  Namely,
	 * postgres_fdw calls "GetNumRegisteredWaitEvents(set) == 1" to check if
	 * any other events are in the set.  That's a poor design, it's
	 * questionable for postgres_fdw to be doing that in the first place, but
	 * we cannot change it now.  The pattern has possibly been copied to other
	 * extensions too.
	 */
	AddWaitEventToSet(eventset, WL_LATCH_SET, PGINVALID_SOCKET,
					  MyLatch, NULL);

	/* Return at most EVENT_BUFFER_SIZE events in one call. */
	if (nevents > EVENT_BUFFER_SIZE)
		nevents = EVENT_BUFFER_SIZE;

	/* Wait or poll for file descriptor events. */
	noccurred = WaitEventSetWait(eventset, timeout, occurred_event, nevents,
								 wait_event_info);
	if (noccurred == 0)
		return;

	/* Deliver notifications. */
	for (i = 0; i < noccurred; i++)
	{
		WaitEvent  *w = &occurred_event[i];

		/*
		 * Each waiting subplan should have registered its wait event with
		 * user_data pointing back to its AsyncRequest.
		 */
		if ((w->events & WL_SOCKET_READABLE) != 0)
		{
			AsyncRequest *areq = (AsyncRequest *) w->user_data;

			if (areq->callback_pending)
			{
				/*
				 * Mark it as no longer needing a callback.  We must do this
				 * before dispatching the callback in case the callback resets
				 * the flag.
				 */
				areq->callback_pending = false;

				/* Do the actual work. */
				ExecAsyncNotify(areq);
			}
		}

		/* Handle standard interrupts */
		if ((w->events & WL_LATCH_SET) != 0)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}
}
