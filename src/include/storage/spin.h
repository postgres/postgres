/*-------------------------------------------------------------------------
 *
 * spin.h
 *	  synchronization routines
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: spin.h,v 1.10 1999/10/06 21:58:17 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPIN_H
#define SPIN_H

#include "storage/ipc.h"

/*
 * two implementations of spin locks
 *
 * sequent, sparc, sun3: real spin locks. uses a TAS instruction; see
 * src/storage/ipc/s_lock.c for details.
 *
 * default: fake spin locks using semaphores.  see spin.c
 *
 */

typedef int SPINLOCK;

extern void CreateSpinlocks(IPCKey key);
extern void InitSpinLocks(void);
extern void SpinAcquire(SPINLOCK lockid);
extern void SpinRelease(SPINLOCK lockid);

#endif	 /* SPIN_H */
