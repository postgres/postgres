/*-------------------------------------------------------------------------
 *
 * latch.h
 *	  Routines for interprocess latches
 *
 * A latch is a boolean variable, with operations that let processes sleep
 * until it is set. A latch can be set from another process, or a signal
 * handler within the same process.
 *
 * The latch interface is a reliable replacement for the common pattern of
 * using pg_usleep() or select() to wait until a signal arrives, where the
 * signal handler sets a flag variable. Because on some platforms an
 * incoming signal doesn't interrupt sleep, and even on platforms where it
 * does there is a race condition if the signal arrives just before
 * entering the sleep, the common pattern must periodically wake up and
 * poll the flag variable. The pselect() system call was invented to solve
 * this problem, but it is not portable enough. Latches are designed to
 * overcome these limitations, allowing you to sleep without polling and
 * ensuring quick response to signals from other processes.
 *
 * There are two kinds of latches: local and shared. A local latch is
 * initialized by InitLatch, and can only be set from the same process.
 * A local latch can be used to wait for a signal to arrive, by calling
 * SetLatch in the signal handler. A shared latch resides in shared memory,
 * and must be initialized at postmaster startup by InitSharedLatch. Before
 * a shared latch can be waited on, it must be associated with a process
 * with OwnLatch. Only the process owning the latch can wait on it, but any
 * process can set it.
 *
 * There are three basic operations on a latch:
 *
 * SetLatch		- Sets the latch
 * ResetLatch	- Clears the latch, allowing it to be set again
 * WaitLatch	- Waits for the latch to become set
 *
 * WaitLatch includes a provision for timeouts (which should hopefully not
 * be necessary once the code is fully latch-ified).
 * See unix_latch.c for detailed specifications for the exported functions.
 *
 * The correct pattern to wait for event(s) is:
 *
 * for (;;)
 * {
 *	   ResetLatch();
 *	   if (work to do)
 *		   Do Stuff();
 *	   WaitLatch();
 * }
 *
 * It's important to reset the latch *before* checking if there's work to
 * do. Otherwise, if someone sets the latch between the check and the
 * ResetLatch call, you will miss it and Wait will incorrectly block.
 *
 * To wake up the waiter, you must first set a global flag or something
 * else that the wait loop tests in the "if (work to do)" part, and call
 * SetLatch *after* that. SetLatch is designed to return quickly if the
 * latch is already set.
 *
 * Presently, when using a shared latch for interprocess signalling, the
 * flag variable(s) set by senders and inspected by the wait loop must
 * be protected by spinlocks or LWLocks, else it is possible to miss events
 * on machines with weak memory ordering (such as PPC).  This restriction
 * will be lifted in future by inserting suitable memory barriers into
 * SetLatch and ResetLatch.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/latch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LATCH_H
#define LATCH_H

#include <signal.h>

/*
 * Latch structure should be treated as opaque and only accessed through
 * the public functions. It is defined here to allow embedding Latches as
 * part of bigger structs.
 */
typedef struct
{
	sig_atomic_t is_set;
	bool		is_shared;
	int			owner_pid;
#ifdef WIN32
	HANDLE		event;
#endif
} Latch;

/*
 * prototypes for functions in latch.c
 */
extern void InitializeLatchSupport(void);
extern void InitLatch(volatile Latch *latch);
extern void InitSharedLatch(volatile Latch *latch);
extern void OwnLatch(volatile Latch *latch);
extern void DisownLatch(volatile Latch *latch);
extern bool WaitLatch(volatile Latch *latch, long timeout);
extern int WaitLatchOrSocket(volatile Latch *latch, pgsocket sock,
				  bool forRead, bool forWrite, long timeout);
extern void SetLatch(volatile Latch *latch);
extern void ResetLatch(volatile Latch *latch);

/* beware of memory ordering issues if you use this macro! */
#define TestLatch(latch) (((volatile Latch *) (latch))->is_set)

/*
 * Unix implementation uses SIGUSR1 for inter-process signaling.
 * Win32 doesn't need this.
 */
#ifndef WIN32
extern void latch_sigusr1_handler(void);
#else
#define latch_sigusr1_handler()  ((void) 0)
#endif

#endif   /* LATCH_H */
