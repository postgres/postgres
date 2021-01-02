/*-------------------------------------------------------------------------
 *
 * tstoreReceiver.h
 *	  prototypes for tstoreReceiver.c
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/tstoreReceiver.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TSTORE_RECEIVER_H
#define TSTORE_RECEIVER_H

#include "tcop/dest.h"
#include "utils/tuplestore.h"


extern DestReceiver *CreateTuplestoreDestReceiver(void);

extern void SetTuplestoreDestReceiverParams(DestReceiver *self,
											Tuplestorestate *tStore,
											MemoryContext tContext,
											bool detoast,
											TupleDesc target_tupdesc,
											const char *map_failure_msg);

#endif							/* TSTORE_RECEIVER_H */
