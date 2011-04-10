/*-------------------------------------------------------------------------
 *
 * latch.h
 *	  Routines for interprocess latches
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
extern void InitLatch(volatile Latch *latch);
extern void InitSharedLatch(volatile Latch *latch);
extern void OwnLatch(volatile Latch *latch);
extern void DisownLatch(volatile Latch *latch);
extern bool WaitLatch(volatile Latch *latch, long timeout);
extern int WaitLatchOrSocket(volatile Latch *latch, pgsocket sock,
				  bool forRead, bool forWrite, long timeout);
extern void SetLatch(volatile Latch *latch);
extern void ResetLatch(volatile Latch *latch);

#define TestLatch(latch) (((volatile Latch *) latch)->is_set)

/*
 * Unix implementation uses SIGUSR1 for inter-process signaling, Win32 doesn't
 * need this.
 */
#ifndef WIN32
extern void latch_sigusr1_handler(void);
#else
#define latch_sigusr1_handler()
#endif

#endif   /* LATCH_H */
