/*-------------------------------------------------------------------------
 *
 * pqsignal.h
 *	  prototypes for the reliable BSD-style signal(2) routine.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pqsignal.h,v 1.13 2000/06/15 00:52:11 momjian Exp $
 *
 * NOTES
 *	  This shouldn't be in libpq, but the monitor and some other
 *	  things need it...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQSIGNAL_H
#define PQSIGNAL_H

#include <signal.h>

#ifdef HAVE_SIGPROCMASK
extern sigset_t UnBlockSig,
			BlockSig;

#define PG_INITMASK()	( \
							sigemptyset(&UnBlockSig), \
							sigfillset(&BlockSig) \
						)
#define PG_SETMASK(mask)	sigprocmask(SIG_SETMASK, mask, NULL)
#else
extern int	UnBlockSig,
			BlockSig;

#define PG_INITMASK()	( \
							UnBlockSig = 0, \
							BlockSig = sigmask(SIGHUP) | sigmask(SIGQUIT) | \
										sigmask(SIGTERM) | sigmask(SIGALRM) | \
										sigmask(SIGINT) | sigmask(SIGUSR1) | \
										sigmask(SIGUSR2) | sigmask(SIGCHLD) | \
										sigmask(SIGWINCH) | sigmask(SIGFPE) \
						)
#define PG_SETMASK(mask)	sigsetmask(*((int*)(mask)))
#endif

typedef void (*pqsigfunc) (int);

extern pqsigfunc pqsignal(int signo, pqsigfunc func);

#endif	 /* PQSIGNAL_H */
