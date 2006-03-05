/*-------------------------------------------------------------------------
 *
 * pqsignal.c
 *	  reliable BSD-style signal(2) routine stolen from RWW who stole it
 *	  from Stevens...
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/libpq/pqsignal.c,v 1.42 2006/03/05 15:58:27 momjian Exp $
 *
 * NOTES
 *		This shouldn't be in libpq, but the monitor and some other
 *		things need it...
 *
 *	A NOTE ABOUT SIGNAL HANDLING ACROSS THE VARIOUS PLATFORMS.
 *
 *	pg_config.h defines the macro HAVE_POSIX_SIGNALS for some platforms and
 *	not for others.  This file and pqsignal.h use that macro to decide
 *	how to handle signalling.
 *
 *	signal(2) handling - this is here because it affects some of
 *	the frontend commands as well as the backend server.
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
 *	We devoutly hope that there aren't any systems that provide
 *	neither POSIX signals nor BSD signals.	The alternative
 *	is to do signal-handler reinstallation, which doesn't work well
 *	at all.
 * ------------------------------------------------------------------------*/

#include "postgres.h"

#include <signal.h>

#include "libpq/pqsignal.h"


#ifdef HAVE_SIGPROCMASK
sigset_t	UnBlockSig,
			BlockSig,
			AuthBlockSig;
#else
int			UnBlockSig,
			BlockSig,
			AuthBlockSig;
#endif


/*
 * Initialize BlockSig, UnBlockSig, and AuthBlockSig.
 *
 * BlockSig is the set of signals to block when we are trying to block
 * signals.  This includes all signals we normally expect to get, but NOT
 * signals that should never be turned off.
 *
 * AuthBlockSig is the set of signals to block during authentication;
 * it's essentially BlockSig minus SIGTERM, SIGQUIT, SIGALRM.
 *
 * UnBlockSig is the set of signals to block when we don't want to block
 * signals (is this ever nonzero??)
 */
void
pqinitmask(void)
{
#ifdef HAVE_SIGPROCMASK

	sigemptyset(&UnBlockSig);

	/* First set all signals, then clear some. */
	sigfillset(&BlockSig);
	sigfillset(&AuthBlockSig);

	/*
	 * Unmark those signals that should never be blocked. Some of these signal
	 * names don't exist on all platforms.  Most do, but might as well ifdef
	 * them all for consistency...
	 */
#ifdef SIGTRAP
	sigdelset(&BlockSig, SIGTRAP);
	sigdelset(&AuthBlockSig, SIGTRAP);
#endif
#ifdef SIGABRT
	sigdelset(&BlockSig, SIGABRT);
	sigdelset(&AuthBlockSig, SIGABRT);
#endif
#ifdef SIGILL
	sigdelset(&BlockSig, SIGILL);
	sigdelset(&AuthBlockSig, SIGILL);
#endif
#ifdef SIGFPE
	sigdelset(&BlockSig, SIGFPE);
	sigdelset(&AuthBlockSig, SIGFPE);
#endif
#ifdef SIGSEGV
	sigdelset(&BlockSig, SIGSEGV);
	sigdelset(&AuthBlockSig, SIGSEGV);
#endif
#ifdef SIGBUS
	sigdelset(&BlockSig, SIGBUS);
	sigdelset(&AuthBlockSig, SIGBUS);
#endif
#ifdef SIGSYS
	sigdelset(&BlockSig, SIGSYS);
	sigdelset(&AuthBlockSig, SIGSYS);
#endif
#ifdef SIGCONT
	sigdelset(&BlockSig, SIGCONT);
	sigdelset(&AuthBlockSig, SIGCONT);
#endif

/* Signals unique to Auth */
#ifdef SIGQUIT
	sigdelset(&AuthBlockSig, SIGQUIT);
#endif
#ifdef SIGTERM
	sigdelset(&AuthBlockSig, SIGTERM);
#endif
#ifdef SIGALRM
	sigdelset(&AuthBlockSig, SIGALRM);
#endif
#else
	/* Set the signals we want. */
	UnBlockSig = 0;
	BlockSig = sigmask(SIGQUIT) |
		sigmask(SIGTERM) | sigmask(SIGALRM) |
	/* common signals between two */
		sigmask(SIGHUP) |
		sigmask(SIGINT) | sigmask(SIGUSR1) |
		sigmask(SIGUSR2) | sigmask(SIGCHLD) |
		sigmask(SIGWINCH) | sigmask(SIGFPE);
	AuthBlockSig = sigmask(SIGHUP) |
		sigmask(SIGINT) | sigmask(SIGUSR1) |
		sigmask(SIGUSR2) | sigmask(SIGCHLD) |
		sigmask(SIGWINCH) | sigmask(SIGFPE);
#endif
}


/* Win32 signal handling is in backend/port/win32/signal.c */
#ifndef WIN32

/*
 * Set up a signal handler
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

#endif   /* WIN32 */
