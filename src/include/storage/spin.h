/*-------------------------------------------------------------------------
 *
 * spin.h--
 *	  synchronization routines
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: spin.h,v 1.7 1998/09/01 04:38:38 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPIN_H
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
extern bool InitSpinLocks(int init, IPCKey key);
extern void SpinAcquire(SPINLOCK lockid);
extern void SpinRelease(SPINLOCK lockid);

#endif	 /* SPIN_H */
