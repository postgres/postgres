/*-------------------------------------------------------------------------
 *
 * legacy-pqsignal.c
 *	  reliable BSD-style signal(2) routine stolen from RWW who stole it
 *	  from Stevens...
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/legacy-pqsignal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <signal.h>


/*
 * This version of pqsignal() exists only because pre-9.3 releases
 * of libpq exported pqsignal(), and some old client programs still
 * depend on that.  (Since 9.3, clients are supposed to get it from
 * libpgport instead.)
 *
 * Because it is only intended for backwards compatibility, we freeze it
 * with the semantics it had in 9.2; in particular, this has different
 * behavior for SIGALRM than the version in src/port/pqsignal.c.
 *
 * libpq itself uses this only for SIGPIPE (and even then, only in
 * non-ENABLE_THREAD_SAFETY builds), so the incompatibility isn't
 * troublesome for internal references.
 */
pqsigfunc
pqsignal(int signo, pqsigfunc func)
{
#ifndef WIN32
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
#else							/* WIN32 */
	return signal(signo, func);
#endif
}
