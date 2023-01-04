/*-------------------------------------------------------------------------
 * execAsync.h
 *		Support functions for asynchronous execution
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/executor/execAsync.h
 *-------------------------------------------------------------------------
 */

#ifndef EXECASYNC_H
#define EXECASYNC_H

#include "nodes/execnodes.h"

extern void ExecAsyncRequest(AsyncRequest *areq);
extern void ExecAsyncConfigureWait(AsyncRequest *areq);
extern void ExecAsyncNotify(AsyncRequest *areq);
extern void ExecAsyncResponse(AsyncRequest *areq);
extern void ExecAsyncRequestDone(AsyncRequest *areq, TupleTableSlot *result);
extern void ExecAsyncRequestPending(AsyncRequest *areq);

#endif							/* EXECASYNC_H */
