/*-------------------------------------------------------------------------
 *
 * sinvaladt.h
 *	  POSTGRES shared cache invalidation data manager.
 *
 * The shared cache invalidation manager is responsible for transmitting
 * invalidation messages between backends.  Any message sent by any backend
 * must be delivered to all already-running backends before it can be
 * forgotten.  (If we run out of space, we instead deliver a "RESET"
 * message to backends that have fallen too far behind.)
 *
 * The struct type SharedInvalidationMessage, defining the contents of
 * a single message, is defined in sinval.h.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/sinvaladt.h,v 1.53 2010/01/02 16:58:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVALADT_H
#define SINVALADT_H

#include "storage/sinval.h"

/*
 * prototypes for functions in sinvaladt.c
 */
extern Size SInvalShmemSize(void);
extern void CreateSharedInvalidationState(void);
extern void SharedInvalBackendInit(bool sendOnly);
extern bool BackendIdIsActive(int backendID);

extern void SIInsertDataEntries(const SharedInvalidationMessage *data, int n);
extern int	SIGetDataEntries(SharedInvalidationMessage *data, int datasize);
extern void SICleanupQueue(bool callerHasWriteLock, int minFree);

extern LocalTransactionId GetNextLocalTransactionId(void);

#endif   /* SINVALADT_H */
