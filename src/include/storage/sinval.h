/*-------------------------------------------------------------------------
 *
 * sinval.h
 *	  POSTGRES shared cache invalidation communication definitions.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sinval.h,v 1.15 2000/11/12 20:51:52 tgl Exp $
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

extern bool DatabaseHasActiveBackends(Oid databaseId, bool ignoreMyself);
extern bool TransactionIdIsInProgress(TransactionId xid);
extern void GetXmaxRecent(TransactionId *XmaxRecent);


#endif	 /* SINVAL_H */
