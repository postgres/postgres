/*------------------------------------------------------------------------
 *
 * Query cancellation support for frontend code
 *
 * Assorted utility functions to control query cancellation with signal
 * handler for SIGINT.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/fe-utils/cancel.c
 *
 *------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#include "common/connect.h"
#include "fe_utils/cancel.h"
#include "fe_utils/string_utils.h"


/*
 * Write a simple string to stderr --- must be safe in a signal handler.
 * We ignore the write() result since there's not much we could do about it.
 * Certain compilers make that harder than it ought to be.
 */
#define write_stderr(str) \
	do { \
		const char *str_ = (str); \
		int		rc_; \
		rc_ = write(fileno(stderr), str_, strlen(str_)); \
		(void) rc_; \
	} while (0)

/*
 * Contains all the information needed to cancel a query issued from
 * a database connection to the backend.
 */
static PGcancel *volatile cancelConn = NULL;

/*
 * CancelRequested is set when we receive SIGINT (or local equivalent).
 * There is no provision in this module for resetting it; but applications
 * might choose to clear it after successfully recovering from a cancel.
 * Note that there is no guarantee that we successfully sent a Cancel request,
 * or that the request will have any effect if we did send it.
 */
volatile sig_atomic_t CancelRequested = false;

#ifdef WIN32
static CRITICAL_SECTION cancelConnLock;
#endif

/*
 * Additional callback for cancellations.
 */
static void (*cancel_callback) (void) = NULL;


/*
 * SetCancelConn
 *
 * Set cancelConn to point to the current database connection.
 */
void
SetCancelConn(PGconn *conn)
{
	PGcancel   *oldCancelConn;

#ifdef WIN32
	EnterCriticalSection(&cancelConnLock);
#endif

	/* Free the old one if we have one */
	oldCancelConn = cancelConn;

	/* be sure handle_sigint doesn't use pointer while freeing */
	cancelConn = NULL;

	if (oldCancelConn != NULL)
		PQfreeCancel(oldCancelConn);

	cancelConn = PQgetCancel(conn);

#ifdef WIN32
	LeaveCriticalSection(&cancelConnLock);
#endif
}

/*
 * ResetCancelConn
 *
 * Free the current cancel connection, if any, and set to NULL.
 */
void
ResetCancelConn(void)
{
	PGcancel   *oldCancelConn;

#ifdef WIN32
	EnterCriticalSection(&cancelConnLock);
#endif

	oldCancelConn = cancelConn;

	/* be sure handle_sigint doesn't use pointer while freeing */
	cancelConn = NULL;

	if (oldCancelConn != NULL)
		PQfreeCancel(oldCancelConn);

#ifdef WIN32
	LeaveCriticalSection(&cancelConnLock);
#endif
}


/*
 * Code to support query cancellation
 *
 * Note that sending the cancel directly from the signal handler is safe
 * because PQcancel() is written to make it so.  We use write() to report
 * to stderr because it's better to use simple facilities in a signal
 * handler.
 *
 * On Windows, the signal canceling happens on a separate thread, because
 * that's how SetConsoleCtrlHandler works.  The PQcancel function is safe
 * for this (unlike PQrequestCancel).  However, a CRITICAL_SECTION is required
 * to protect the PGcancel structure against being changed while the signal
 * thread is using it.
 */

#ifndef WIN32

/*
 * handle_sigint
 *
 * Handle interrupt signals by canceling the current command, if cancelConn
 * is set.
 */
static void
handle_sigint(SIGNAL_ARGS)
{
	int			save_errno = errno;
	char		errbuf[256];

	CancelRequested = true;

	if (cancel_callback != NULL)
		cancel_callback();

	/* Send QueryCancel if we are processing a database query */
	if (cancelConn != NULL)
	{
		if (PQcancel(cancelConn, errbuf, sizeof(errbuf)))
		{
			write_stderr(_("Cancel request sent\n"));
		}
		else
		{
			write_stderr(_("Could not send cancel request: "));
			write_stderr(errbuf);
		}
	}

	errno = save_errno;			/* just in case the write changed it */
}

/*
 * setup_cancel_handler
 *
 * Register query cancellation callback for SIGINT.
 */
void
setup_cancel_handler(void (*callback) (void))
{
	cancel_callback = callback;
	pqsignal(SIGINT, handle_sigint);
}

#else							/* WIN32 */

static BOOL WINAPI
consoleHandler(DWORD dwCtrlType)
{
	char		errbuf[256];

	if (dwCtrlType == CTRL_C_EVENT ||
		dwCtrlType == CTRL_BREAK_EVENT)
	{
		CancelRequested = true;

		if (cancel_callback != NULL)
			cancel_callback();

		/* Send QueryCancel if we are processing a database query */
		EnterCriticalSection(&cancelConnLock);
		if (cancelConn != NULL)
		{
			if (PQcancel(cancelConn, errbuf, sizeof(errbuf)))
			{
				write_stderr(_("Cancel request sent\n"));
			}
			else
			{
				write_stderr(_("Could not send cancel request: "));
				write_stderr(errbuf);
			}
		}

		LeaveCriticalSection(&cancelConnLock);

		return TRUE;
	}
	else
		/* Return FALSE for any signals not being handled */
		return FALSE;
}

void
setup_cancel_handler(void (*callback) (void))
{
	cancel_callback = callback;

	InitializeCriticalSection(&cancelConnLock);

	SetConsoleCtrlHandler(consoleHandler, TRUE);
}

#endif							/* WIN32 */
