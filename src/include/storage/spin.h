/*-------------------------------------------------------------------------
 *
 * spin.h
 *	  synchronization routines
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: spin.h,v 1.15 2001/03/22 04:01:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPIN_H
#define SPIN_H

#include "storage/ipc.h"

/*
 * two implementations of spin locks
 *
 * Where TAS instruction is available: real spin locks.
 * See src/storage/ipc/s_lock.c for details.
 *
 * Otherwise: fake spin locks using semaphores.  see spin.c
 */

typedef int SPINLOCK;

#ifdef LOCK_DEBUG
extern bool Trace_spinlocks;

#endif


extern int	SLockShmemSize(void);
extern void CreateSpinlocks(PGShmemHeader *seghdr);

extern void SpinAcquire(SPINLOCK lockid);
extern void SpinRelease(SPINLOCK lockid);

#endif	 /* SPIN_H */
