/*-------------------------------------------------------------------------
 *
 * tstoreReceiver.h
 *	  prototypes for tstoreReceiver.c
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/tstoreReceiver.h,v 1.7 2004/12/31 22:03:29 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef TSTORE_RECEIVER_H
#define TSTORE_RECEIVER_H

#include "tcop/dest.h"
#include "utils/tuplestore.h"


extern DestReceiver *CreateTuplestoreDestReceiver(Tuplestorestate *tStore,
							 MemoryContext tContext);

#endif   /* TSTORE_RECEIVER_H */
