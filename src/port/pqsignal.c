/*-------------------------------------------------------------------------
 *
 * pqsignal.c
 *	  reliable BSD-style signal(2) routine stolen from RWW who stole it
 *	  from Stevens...
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pqsignal.c
 *
 *	A NOTE ABOUT SIGNAL HANDLING ACROSS THE VARIOUS PLATFORMS.
 *
 *	pg_config.h defines the macro HAVE_POSIX_SIGNALS for some platforms and
 *	not for others.  We use that here to decide how to handle signalling.
 *
 *	Ultrix and SunOS provide BSD signal(2) semantics by default.
 *
 *	SVID2 and POSIX signal(2) semantics differ from BSD signal(2)
 *	semantics.	We can use the POSIX sigaction(2) on systems that
 *	allow us to request restartable signals (SA_RESTART).
 *
 *	Some systems don't allow restartable signals at all unless we
 *	link to a special BSD library.
 *
 *	We devoutly hope that there aren't any Unix-oid systems that provide
 *	neither POSIX signals nor BSD signals.	The alternative is to do
 *	signal-handler reinstallation, which doesn't work well at all.
 *
 *	Windows, of course, is resolutely in a class by itself.  This file
 *	should not get compiled at all on Windows.  We have an emulation of
 *	pqsignal() in src/backend/port/win32/signal.c for the backend
 *	environment; frontend programs are out of luck.
 * ------------------------------------------------------------------------
 */

#include "c.h"

#include <signal.h>

#ifndef WIN32

/*
 * Set up a signal handler for signal "signo"
 *
 * Returns the previous handler.  It's expected that the installed handler
 * will persist across multiple deliveries of the signal (unlike the original
 * POSIX definition of signal(2)).
 */
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

#endif /* WIN32 */
