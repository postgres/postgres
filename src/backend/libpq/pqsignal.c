/*-------------------------------------------------------------------------
 *
 * pqsignal.c--
 *	  reliable BSD-style signal(2) routine stolen from RWW who stole it
 *	  from Stevens...
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/pqsignal.c,v 1.9 1998/09/01 03:22:52 momjian Exp $
 *
 * NOTES
 *		This shouldn't be in libpq, but the monitor and some other
 *		things need it...
 *
 *	A NOTE ABOUT SIGNAL HANDLING ACROSS THE VARIOUS PLATFORMS.
 *
 *	config.h defines the macro USE_POSIX_SIGNALS for some platforms and
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
#include <postgres.h>

#include <signal.h>

#include <libpq/pqsignal.h>

pqsigfunc
pqsignal(int signo, pqsigfunc func)
{
#if !defined(USE_POSIX_SIGNALS)
	return signal(signo, func);
#else
	struct sigaction act,
				oact;

	act.sa_handler = func;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	if (signo != SIGALRM)
		act.sa_flags |= SA_RESTART;
	if (sigaction(signo, &act, &oact) < 0)
		return SIG_ERR;
	return oact.sa_handler;
#endif							/* !USE_POSIX_SIGNALS */
}
