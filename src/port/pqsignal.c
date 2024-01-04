/*-------------------------------------------------------------------------
 *
 * pqsignal.c
 *	  reliable BSD-style signal(2) routine stolen from RWW who stole it
 *	  from Stevens...
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pqsignal.c
 *
 *	This is the signal() implementation from "Advanced Programming in the UNIX
 *	Environment", with minor changes.  It was originally a replacement needed
 *	for old SVR4 systems whose signal() behaved as if sa_flags = SA_RESETHAND |
 *	SA_NODEFER, also known as "unreliable" signals due to races when the
 *	handler was reset.
 *
 *	By now, all known modern Unix systems have a "reliable" signal() call.
 *	We still don't want to use it though, because it remains
 *	implementation-defined by both C99 and POSIX whether the handler is reset
 *	or signals are blocked when the handler runs, and default restart behavior
 *	is also unspecified.  Therefore we take POSIX's advice and call sigaction()
 *	so we can provide explicit sa_flags, but wrap it in this more convenient
 *	traditional interface style.  It also provides a place to set any extra
 *	flags we want everywhere, such as SA_NOCLDSTOP.
 *
 *	Windows, of course, is resolutely in a class by itself.  In the backend,
 *	this relies on pqsigaction() in src/backend/port/win32/signal.c, which
 *	provides limited emulation of reliable signals.
 *
 *	Frontend programs can use this version of pqsignal() to forward to the
 *	native Windows signal() call if they wish, but beware that Windows signals
 *	behave quite differently.  Only the 6 signals required by C are supported.
 *	SIGINT handlers run in another thread instead of interrupting an existing
 *	thread, and the others don't interrupt system calls either, so SA_RESTART
 *	is moot.  All except SIGFPE have SA_RESETHAND semantics, meaning the
 *	handler is reset to SIG_DFL each time it runs.  The set of things you are
 *	allowed to do in a handler is also much more restricted than on Unix,
 *	according to the documentation.
 *
 * ------------------------------------------------------------------------
 */

#include "c.h"

#include <signal.h>

#ifndef FRONTEND
#include "libpq/pqsignal.h"
#endif

/*
 * Set up a signal handler, with SA_RESTART, for signal "signo"
 *
 * Returns the previous handler.
 */
pqsigfunc
pqsignal(int signo, pqsigfunc func)
{
#if !(defined(WIN32) && defined(FRONTEND))
	struct sigaction act,
				oact;

	act.sa_handler = func;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;
#ifdef SA_NOCLDSTOP
	if (signo == SIGCHLD)
		act.sa_flags |= SA_NOCLDSTOP;
#endif
	if (sigaction(signo, &act, &oact) < 0)
		return SIG_ERR;
	return oact.sa_handler;
#else
	/* Forward to Windows native signal system. */
	return signal(signo, func);
#endif
}
