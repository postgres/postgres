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
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/pqsignal.c,v 1.8 1998/09/01 03:28:56 momjian Exp $
 *
 * NOTES
 *		This shouldn't be in libpq, but the monitor and some other
 *		things need it...
 *
 *-------------------------------------------------------------------------
 */
#include <stdlib.h>
#include <signal.h>

#include "pqsignal.h"

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
