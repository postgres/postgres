/*-------------------------------------------------------------------------
 *
 * spin.h--
 *    synchronization routines
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: spin.h,v 1.2 1996/11/05 06:11:08 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	SPIN_H
#define SPIN_H

#include <storage/ipc.h>

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

extern bool CreateSpinlocks(IPCKey key);
extern bool AttachSpinLocks(IPCKey key);
extern bool InitSpinLocks(int init, IPCKey key);

extern void SpinAcquire(SPINLOCK lock);
extern void SpinRelease(SPINLOCK lock);
extern bool SpinIsLocked(SPINLOCK lock);

#endif	/* SPIN_H */
