/*-------------------------------------------------------------------------
 *
 * tstoreReceiver.h
 *	  prototypes for tstoreReceiver.c
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/tstoreReceiver.h,v 1.8 2006/03/05 15:58:56 momjian Exp $
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
