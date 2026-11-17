/*-------------------------------------------------------------------------
 * execAsync.h
 *		Support functions for asynchronous execution
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/executor/execAsync.h
 *-------------------------------------------------------------------------
 */

#ifndef EXECASYNC_H
#define EXECASYNC_H

#include "nodes/execnodes.h"
#include "storage/latch.h"

extern void ExecAsyncRequest(AsyncRequest *areq);
extern void ExecAsyncConfigureWait(AsyncRequest *areq);
extern void ExecAsyncNotify(AsyncRequest *areq);
extern void ExecAsyncResponse(AsyncRequest *areq);
extern void ExecAsyncRequestDone(AsyncRequest *areq, TupleTableSlot *result);
extern void ExecAsyncRequestPending(AsyncRequest *areq);
extern WaitEventSet *ExecAsyncGetRequestorEventSet(PlanState *requestor);
extern void ExecAsyncEventWait(Bitmapset *asyncplans,
							   AsyncRequest **asyncrequests,
							   WaitEventSet *eventset,
							   int nevents,
							   long timeout,
							   uint32 wait_event_info);

#endif							/* EXECASYNC_H */
