/*-------------------------------------------------------------------------
 *
 * sinval.h--
 *    POSTGRES shared cache invalidation communication definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sinval.h,v 1.4 1996/11/10 03:06:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	SINVAL_H
#define SINVAL_H

#include <storage/itemptr.h>
#include <storage/spin.h>

extern SPINLOCK SInvalLock;

extern void CreateSharedInvalidationState(IPCKey key);
extern void AttachSharedInvalidationState(IPCKey key);
extern void InitSharedInvalidationState(void);
extern void RegisterSharedInvalid(int cacheId, Index hashIndex,
				  ItemPointer pointer);
extern void InvalidateSharedInvalid(void (*invalFunction)(),
				    void (*resetFunction)());


#endif /* SINVAL_H */
