/*-------------------------------------------------------------------------
 *
 * sinval.h
 *	  POSTGRES shared cache invalidation communication definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sinval.h,v 1.17 2001/01/24 19:43:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVAL_H
#define SINVAL_H

#include "storage/itemptr.h"
#include "storage/spin.h"

extern SPINLOCK SInvalLock;

extern int	SInvalShmemSize(int maxBackends);
extern void CreateSharedInvalidationState(int maxBackends);
extern void InitBackendSharedInvalidationState(void);
extern void RegisterSharedInvalid(int cacheId, Index hashIndex,
					  ItemPointer pointer);
extern void InvalidateSharedInvalid(void (*invalFunction) (),
												void (*resetFunction) ());

extern bool DatabaseHasActiveBackends(Oid databaseId, bool ignoreMyself);
extern bool TransactionIdIsInProgress(TransactionId xid);
extern void GetXmaxRecent(TransactionId *XmaxRecent);


#endif	 /* SINVAL_H */
