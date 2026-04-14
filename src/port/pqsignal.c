/*-------------------------------------------------------------------------
 *
 * pqsignal.c
 *	  reliable BSD-style signal(2) routine stolen from RWW who stole it
 *	  from Stevens...
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
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
#include <unistd.h>
#endif

#ifndef FRONTEND
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#endif

#ifdef PG_SIGNAL_COUNT			/* Windows */
#define PG_NSIG (PG_SIGNAL_COUNT)
#elif defined(NSIG)
#define PG_NSIG (NSIG)
#else
#define PG_NSIG (64)			/* XXX: wild guess */
#endif

#if !(defined(WIN32) && defined(FRONTEND))
#define USE_SIGACTION
#endif

#if defined(USE_SIGACTION) && defined(HAVE_SA_SIGINFO)
#define USE_SIGINFO
#endif

/* Check a couple of common signals to make sure PG_NSIG is accurate. */
StaticAssertDecl(SIGUSR2 < PG_NSIG, "SIGUSR2 >= PG_NSIG");
StaticAssertDecl(SIGHUP < PG_NSIG, "SIGHUP >= PG_NSIG");
StaticAssertDecl(SIGTERM < PG_NSIG, "SIGTERM >= PG_NSIG");
StaticAssertDecl(SIGALRM < PG_NSIG, "SIGALRM >= PG_NSIG");

static volatile pqsigfunc pqsignal_handlers[PG_NSIG];

/*
 * Except when called with SIG_IGN or SIG_DFL, pqsignal() sets up this function
 * as the handler for all signals.  This wrapper handler function checks that
 * it is called within a process that knew to maintain MyProcPid, and not a
 * child process forked by system(3), etc.  This check ensures that such child
 * processes do not modify shared memory, which is often detrimental.  If the
 * check succeeds, the function originally provided to pqsignal() is called.
 * Otherwise, the default signal handler is installed and then called.
 *
 * This wrapper also handles restoring the value of errno.
 */
#if defined(USE_SIGACTION) && defined(USE_SIGINFO)
static void
wrapper_handler(int postgres_signal_arg, siginfo_t * info, void *context)
#else							/* no USE_SIGINFO */
static void
wrapper_handler(int postgres_signal_arg)
#endif
{
	int			save_errno = errno;
	pg_signal_info pg_info;

	Assert(postgres_signal_arg > 0);
	Assert(postgres_signal_arg < PG_NSIG);

#ifndef FRONTEND

	/*
	 * We expect processes to set MyProcPid before calling pqsignal() or
	 * before accepting signals.
	 */
	Assert(MyProcPid);
	Assert(MyProcPid != PostmasterPid || !IsUnderPostmaster);

	if (unlikely(MyProcPid != (int) getpid()))
	{
		pqsignal(postgres_signal_arg, PG_SIG_DFL);
		raise(postgres_signal_arg);
		return;
	}
#endif

#ifdef HAVE_SA_SIGINFO

	/*
	 * If supported by the system, forward interesting information from the
	 * system's extended signal information to our platform independent
	 * format.
	 */
	pg_info.pid = info->si_pid;
	pg_info.uid = info->si_uid;
#else

	/*
	 * Otherwise forward values indicating that we do not have the
	 * information.
	 */
	pg_info.pid = 0;
	pg_info.uid = 0;
#endif

	(*pqsignal_handlers[postgres_signal_arg]) (postgres_signal_arg, &pg_info);

	errno = save_errno;
}

/*
 * Set up a signal handler, with SA_RESTART, for signal "signo"
 *
 * Note: the actual name of this function is either pqsignal_fe when
 * compiled with -DFRONTEND, or pqsignal_be when compiled without that.
 * This is to avoid a name collision with libpq's legacy-pqsignal.c.
 */
void
pqsignal(int signo, pqsigfunc func)
{
#ifdef USE_SIGACTION
	struct sigaction act;
#else
	void		(*wrapper_func_ptr) (int);
#endif
	bool		is_ign = func == PG_SIG_IGN;
	bool		is_dfl = func == PG_SIG_DFL;

	Assert(signo > 0);
	Assert(signo < PG_NSIG);

	/* set up indirection handler */
	if (!(is_ign || is_dfl))
	{
		pqsignal_handlers[signo] = func;	/* assumed atomic */
	}

	/*
	 * Configure system to either ignore/reset the signal handler, or to
	 * forward it to wrapper_handler.
	 */
#ifdef USE_SIGACTION
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;

	if (is_ign)
		act.sa_handler = SIG_IGN;
	else if (is_dfl)
		act.sa_handler = SIG_DFL;
#ifdef USE_SIGINFO
	else
	{
		act.sa_sigaction = wrapper_handler;
		act.sa_flags |= SA_SIGINFO;
	}
#else
	else
		act.sa_handler = wrapper_handler;
#endif

#ifdef SA_NOCLDSTOP
	if (signo == SIGCHLD)
		act.sa_flags |= SA_NOCLDSTOP;
#endif
	if (sigaction(signo, &act, NULL) < 0)
		Assert(false);			/* probably indicates coding error */
#else							/* no USE_SIGACTION */

	/*
	 * Forward to Windows native signal system, we need to send this though
	 * wrapper handler as it it needs to take single argument only.
	 */
	if (is_ign)
		wrapper_func_ptr = SIG_IGN;
	else if (is_dfl)
		wrapper_func_ptr = SIG_DFL;
	else
		wrapper_func_ptr = wrapper_handler;

	if (signal(signo, wrapper_func_ptr) == SIG_ERR)
		Assert(false);			/* probably indicates coding error */
#endif
}
