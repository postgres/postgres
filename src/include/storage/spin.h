/*-------------------------------------------------------------------------
 *
 * spin.h
 *	  synchronization routines
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: spin.h,v 1.12 2000/05/31 00:28:38 petere Exp $
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

#ifdef LOCK_DEBUG
extern bool Trace_spinlocks;
#endif

extern void CreateSpinlocks(IPCKey key);
extern void InitSpinLocks(void);
extern void SpinAcquire(SPINLOCK lockid);
extern void SpinRelease(SPINLOCK lockid);

#endif	 /* SPIN_H */
