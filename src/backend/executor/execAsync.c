/*-------------------------------------------------------------------------
 *
 * execAsync.c
 *	  Support routines for asynchronous execution
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
#include "executor/nodeAppend.h"
#include "executor/nodeForeignscan.h"

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
