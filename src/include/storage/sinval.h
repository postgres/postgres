/*-------------------------------------------------------------------------
 *
 * sinval.h
 *	  POSTGRES shared cache invalidation communication definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sinval.h,v 1.13 1999/09/24 00:25:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVAL_H
#define SINVAL_H

#include "storage/itemptr.h"
#include "storage/spin.h"

extern SPINLOCK SInvalLock;

extern void CreateSharedInvalidationState(IPCKey key, int maxBackends);
extern void AttachSharedInvalidationState(IPCKey key);
extern void InitSharedInvalidationState(void);
extern void RegisterSharedInvalid(int cacheId, Index hashIndex,
					  ItemPointer pointer);
extern void InvalidateSharedInvalid(void (*invalFunction) (),
												void (*resetFunction) ());

extern bool DatabaseHasActiveBackends(Oid databaseId);
extern bool TransactionIdIsInProgress(TransactionId xid);
extern void GetXmaxRecent(TransactionId *XmaxRecent);


#endif	 /* SINVAL_H */
