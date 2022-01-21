/*-------------------------------------------------------------------------
 *
 * pqsignal.c
 *	  Backend signal(2) support (see also src/port/pqsignal.c)
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/pqsignal.c
 *
 * ------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqsignal.h"


/* Global variables */
sigset_t	UnBlockSig,
			BlockSig,
			StartupBlockSig;


/*
 * Initialize BlockSig, UnBlockSig, and StartupBlockSig.
 *
 * BlockSig is the set of signals to block when we are trying to block
 * signals.  This includes all signals we normally expect to get, but NOT
 * signals that should never be turned off.
 *
 * StartupBlockSig is the set of signals to block during startup packet
 * collection; it's essentially BlockSig minus SIGTERM, SIGQUIT, SIGALRM.
 *
 * UnBlockSig is the set of signals to block when we don't want to block
 * signals.
 */
void
pqinitmask(void)
{
	sigemptyset(&UnBlockSig);

	/* Note: InitializeLatchSupport() modifies UnBlockSig. */

	/* First set all signals, then clear some. */
	sigfillset(&BlockSig);
	sigfillset(&StartupBlockSig);

	/*
	 * Unmark those signals that should never be blocked. Some of these signal
	 * names don't exist on all platforms.  Most do, but might as well ifdef
	 * them all for consistency...
	 */
#ifdef SIGTRAP
	sigdelset(&BlockSig, SIGTRAP);
	sigdelset(&StartupBlockSig, SIGTRAP);
#endif
#ifdef SIGABRT
	sigdelset(&BlockSig, SIGABRT);
	sigdelset(&StartupBlockSig, SIGABRT);
#endif
#ifdef SIGILL
	sigdelset(&BlockSig, SIGILL);
	sigdelset(&StartupBlockSig, SIGILL);
#endif
#ifdef SIGFPE
	sigdelset(&BlockSig, SIGFPE);
	sigdelset(&StartupBlockSig, SIGFPE);
#endif
#ifdef SIGSEGV
	sigdelset(&BlockSig, SIGSEGV);
	sigdelset(&StartupBlockSig, SIGSEGV);
#endif
#ifdef SIGBUS
	sigdelset(&BlockSig, SIGBUS);
	sigdelset(&StartupBlockSig, SIGBUS);
#endif
#ifdef SIGSYS
	sigdelset(&BlockSig, SIGSYS);
	sigdelset(&StartupBlockSig, SIGSYS);
#endif
#ifdef SIGCONT
	sigdelset(&BlockSig, SIGCONT);
	sigdelset(&StartupBlockSig, SIGCONT);
#endif

/* Signals unique to startup */
#ifdef SIGQUIT
	sigdelset(&StartupBlockSig, SIGQUIT);
#endif
#ifdef SIGTERM
	sigdelset(&StartupBlockSig, SIGTERM);
#endif
#ifdef SIGALRM
	sigdelset(&StartupBlockSig, SIGALRM);
#endif
}

/*
 * Set up a postmaster signal handler for signal "signo"
 *
 * Returns the previous handler.
 *
 * This is used only in the postmaster, which has its own odd approach to
 * signal handling.  For signals with handlers, we block all signals for the
 * duration of signal handler execution.  We also do not set the SA_RESTART
 * flag; this should be safe given the tiny range of code in which the
 * postmaster ever unblocks signals.
 *
 * pqinitmask() must have been invoked previously.
 *
 * On Windows, this function is just an alias for pqsignal()
 * (and note that it's calling the code in src/backend/port/win32/signal.c,
 * not src/port/pqsignal.c).  On that platform, the postmaster's signal
 * handlers still have to block signals for themselves.
 */
pqsigfunc
pqsignal_pm(int signo, pqsigfunc func)
{
#ifndef WIN32
	struct sigaction act,
				oact;

	act.sa_handler = func;
	if (func == SIG_IGN || func == SIG_DFL)
	{
		/* in these cases, act the same as pqsignal() */
		sigemptyset(&act.sa_mask);
		act.sa_flags = SA_RESTART;
	}
	else
	{
		act.sa_mask = BlockSig;
		act.sa_flags = 0;
	}
#ifdef SA_NOCLDSTOP
	if (signo == SIGCHLD)
		act.sa_flags |= SA_NOCLDSTOP;
#endif
	if (sigaction(signo, &act, &oact) < 0)
		return SIG_ERR;
	return oact.sa_handler;
#else							/* WIN32 */
	return pqsignal(signo, func);
#endif
}
