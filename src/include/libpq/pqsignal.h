/*-------------------------------------------------------------------------
 *
 * pqsignal.h
 *	  prototypes for the reliable BSD-style signal(2) routine.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/libpq/pqsignal.h,v 1.25 2004/02/08 22:28:57 neilc Exp $
 *
 * NOTES
 *	  This shouldn't be in libpq, but the monitor and some other
 *	  things need it...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQSIGNAL_H
#define PQSIGNAL_H

#ifndef WIN32
#include <signal.h>
#endif

#ifdef HAVE_SIGPROCMASK
extern sigset_t UnBlockSig,
			BlockSig,
			AuthBlockSig;

#define PG_SETMASK(mask)	sigprocmask(SIG_SETMASK, mask, NULL)
#else
extern int	UnBlockSig,
			BlockSig,
			AuthBlockSig;

#ifndef WIN32
#define PG_SETMASK(mask)	sigsetmask(*((int*)(mask)))
#else
#define PG_SETMASK(mask)        pqsigsetmask(*((int*)(mask)))
int pqsigsetmask(int mask);
#endif
#endif

typedef void (*pqsigfunc) (int);

extern void pqinitmask(void);

extern pqsigfunc pqsignal(int signo, pqsigfunc func);
extern void pg_queue_signal(int signum);

#ifdef WIN32
#define sigmask(sig) ( 1 << (sig-1) )

void pgwin32_signal_initialize(void);
extern HANDLE pgwin32_main_thread_handle;
#define PG_POLL_SIGNALS() WaitForSingleObjectEx(pgwin32_main_thread_handle,0,TRUE);

/* Signal function return values */
#undef SIG_DFL
#undef SIG_ERR
#undef SIG_IGN
#define SIG_DFL ((pqsigfunc)0)
#define SIG_ERR ((pqsigfunc)-1)
#define SIG_IGN ((pqsigfunc)1)

#endif

#endif   /* PQSIGNAL_H */
