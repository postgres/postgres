/*-------------------------------------------------------------------------
 *
 * sinval.h--
 *    POSTGRES shared cache invalidation communication definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sinval.h,v 1.1.1.1 1996/07/09 06:21:53 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	SINVAL_H
#define SINVAL_H

#include "c.h"
#include "storage/spin.h"
#include "storage/ipc.h"
#include "storage/itemptr.h"
#include "storage/backendid.h"

extern SPINLOCK SInvalLock;

extern void CreateSharedInvalidationState(IPCKey key);
extern void AttachSharedInvalidationState(IPCKey key);
extern void InitSharedInvalidationState();
extern void RegisterSharedInvalid(int cacheId, Index hashIndex,
				  ItemPointer pointer);
extern void InvalidateSharedInvalid(void (*invalFunction)(),
				    void (*resetFunction)());


#endif /* SINVAL_H */
