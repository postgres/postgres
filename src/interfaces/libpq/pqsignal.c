/*-------------------------------------------------------------------------
 *
 * pqsignal.c
 *	  reliable BSD-style signal(2) routine stolen from RWW who stole it
 *	  from Stevens...
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/interfaces/libpq/pqsignal.c,v 1.24 2004/12/31 22:03:50 pgsql Exp $
 *
 * NOTES
 *		This shouldn't be in libpq, but the monitor and some other
 *		things need it...
 *
 *-------------------------------------------------------------------------
 */
#include "pqsignal.h"

#include <signal.h>

pqsigfunc
pqsignal(int signo, pqsigfunc func)
{
#if !defined(HAVE_POSIX_SIGNALS)
	return signal(signo, func);
#else
	struct sigaction act,
				oact;

	act.sa_handler = func;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	if (signo != SIGALRM)
		act.sa_flags |= SA_RESTART;
#ifdef SA_NOCLDSTOP
	if (signo == SIGCHLD)
		act.sa_flags |= SA_NOCLDSTOP;
#endif
	if (sigaction(signo, &act, &oact) < 0)
		return SIG_ERR;
	return oact.sa_handler;
#endif   /* !HAVE_POSIX_SIGNALS */
}

pqsigfunc
pqsignalinquire(int signo)
{
#ifndef WIN32
#if !defined(HAVE_POSIX_SIGNALS)
	pqsigfunc	old_sigfunc;
	int			old_sigmask;

	/* Prevent signal handler calls during test */
	old_sigmask = sigblock(sigmask(signo));
	old_sigfunc = signal(signo, SIG_DFL);
	signal(signo, old_sigfunc);
	sigblock(old_sigmask);
	return old_sigfunc;
#else
	struct sigaction oact;

	if (sigaction(signo, NULL, &oact) < 0)
		return SIG_ERR;
	return oact.sa_handler;
#endif   /* !HAVE_POSIX_SIGNALS */
#else
	return SIG_DFL;
#endif
}
