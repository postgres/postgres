/*-------------------------------------------------------------------------
 *
 * tstoreReceiver.h
 *	  prototypes for tstoreReceiver.c
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/tstoreReceiver.h,v 1.13 2009/01/01 17:23:59 momjian Exp $
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
											bool detoast);

#endif   /* TSTORE_RECEIVER_H */
