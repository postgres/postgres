/*-------------------------------------------------------------------------
 *
 * pqsignal.c
 *	  Backend signal(2) support (see also src/port/pqsignal.c)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
