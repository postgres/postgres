/*-------------------------------------------------------------------------
 *
 * signal.c
 *	  Microsoft Windows Win32 Signal Emulation Functions
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/port/win32/signal.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqsignal.h"

/*
 * These are exported for use by the UNBLOCKED_SIGNAL_QUEUE() macro.
 * pg_signal_queue must be volatile since it is changed by the signal
 * handling thread and inspected without any lock by the main thread.
 * pg_signal_mask is only changed by main thread so shouldn't need it.
 */
volatile int pg_signal_queue;
int			pg_signal_mask;

HANDLE		pgwin32_signal_event;
HANDLE		pgwin32_initial_signal_pipe = INVALID_HANDLE_VALUE;

/*
 * pg_signal_crit_sec is used to protect only pg_signal_queue. That is the only
 * variable that can be accessed from the signal sending threads!
 */
static CRITICAL_SECTION pg_signal_crit_sec;

static pqsigfunc pg_signal_array[PG_SIGNAL_COUNT];
static pqsigfunc pg_signal_defaults[PG_SIGNAL_COUNT];


/* Signal handling thread function */
static DWORD WINAPI pg_signal_thread(LPVOID param);
static BOOL WINAPI pg_console_handler(DWORD dwCtrlType);


/*
 * pg_usleep --- delay the specified number of microseconds, but
 * stop waiting if a signal arrives.
 *
 * This replaces the non-signal-aware version provided by src/port/pgsleep.c.
 */
void
pg_usleep(long microsec)
{
	if (WaitForSingleObject(pgwin32_signal_event,
							(microsec < 500 ? 1 : (microsec + 500) / 1000))
		== WAIT_OBJECT_0)
	{
		pgwin32_dispatch_queued_signals();
		errno = EINTR;
		return;
	}
}


/* Initialization */
void
pgwin32_signal_initialize(void)
{
	int			i;
	HANDLE		signal_thread_handle;

	InitializeCriticalSection(&pg_signal_crit_sec);

	for (i = 0; i < PG_SIGNAL_COUNT; i++)
	{
		pg_signal_array[i] = SIG_DFL;
		pg_signal_defaults[i] = SIG_IGN;
	}
	pg_signal_mask = 0;
	pg_signal_queue = 0;

	/* Create the global event handle used to flag signals */
	pgwin32_signal_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (pgwin32_signal_event == NULL)
		ereport(FATAL,
				(errmsg_internal("could not create signal event: error code %lu", GetLastError())));

	/* Create thread for handling signals */
	signal_thread_handle = CreateThread(NULL, 0, pg_signal_thread, NULL, 0, NULL);
	if (signal_thread_handle == NULL)
		ereport(FATAL,
				(errmsg_internal("could not create signal handler thread")));

	/* Create console control handle to pick up Ctrl-C etc */
	if (!SetConsoleCtrlHandler(pg_console_handler, TRUE))
		ereport(FATAL,
				(errmsg_internal("could not set console control handler")));
}

/*
 * Dispatch all signals currently queued and not blocked
 * Blocked signals are ignored, and will be fired at the time of
 * the sigsetmask() call.
 */
void
pgwin32_dispatch_queued_signals(void)
{
	int			i;

	EnterCriticalSection(&pg_signal_crit_sec);
	while (UNBLOCKED_SIGNAL_QUEUE())
	{
		/* One or more unblocked signals queued for execution */
		int			exec_mask = UNBLOCKED_SIGNAL_QUEUE();

		for (i = 0; i < PG_SIGNAL_COUNT; i++)
		{
			if (exec_mask & sigmask(i))
			{
				/* Execute this signal */
				pqsigfunc	sig = pg_signal_array[i];

				if (sig == SIG_DFL)
					sig = pg_signal_defaults[i];
				pg_signal_queue &= ~sigmask(i);
				if (sig != SIG_ERR && sig != SIG_IGN && sig != SIG_DFL)
				{
					LeaveCriticalSection(&pg_signal_crit_sec);
					sig(i);
					EnterCriticalSection(&pg_signal_crit_sec);
					break;		/* Restart outer loop, in case signal mask or
								 * queue has been modified inside signal
								 * handler */
				}
			}
		}
	}
	ResetEvent(pgwin32_signal_event);
	LeaveCriticalSection(&pg_signal_crit_sec);
}

/* signal masking. Only called on main thread, no sync required */
int
pqsigsetmask(int mask)
{
	int			prevmask;

	prevmask = pg_signal_mask;
	pg_signal_mask = mask;

	/*
	 * Dispatch any signals queued up right away, in case we have unblocked
	 * one or more signals previously queued
	 */
	pgwin32_dispatch_queued_signals();

	return prevmask;
}


/*
 * Unix-like signal handler installation
 *
 * Only called on main thread, no sync required
 */
pqsigfunc
pqsignal(int signum, pqsigfunc handler)
{
	pqsigfunc	prevfunc;

	if (signum >= PG_SIGNAL_COUNT || signum < 0)
		return SIG_ERR;
	prevfunc = pg_signal_array[signum];
	pg_signal_array[signum] = handler;
	return prevfunc;
}

/* Create the signal listener pipe for specified PID */
HANDLE
pgwin32_create_signal_listener(pid_t pid)
{
	char		pipename[128];
	HANDLE		pipe;

	snprintf(pipename, sizeof(pipename), "\\\\.\\pipe\\pgsignal_%u", (int) pid);

	pipe = CreateNamedPipe(pipename, PIPE_ACCESS_DUPLEX,
					   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
						   PIPE_UNLIMITED_INSTANCES, 16, 16, 1000, NULL);

	if (pipe == INVALID_HANDLE_VALUE)
		ereport(ERROR,
				(errmsg("could not create signal listener pipe for PID %d: error code %lu",
						(int) pid, GetLastError())));

	return pipe;
}


/*
 * All functions below execute on the signal handler thread
 * and must be synchronized as such!
 * NOTE! The only global variable that can be used is
 * pg_signal_queue!
 */


void
pg_queue_signal(int signum)
{
	if (signum >= PG_SIGNAL_COUNT || signum <= 0)
		return;

	EnterCriticalSection(&pg_signal_crit_sec);
	pg_signal_queue |= sigmask(signum);
	LeaveCriticalSection(&pg_signal_crit_sec);

	SetEvent(pgwin32_signal_event);
}

/* Signal dispatching thread */
static DWORD WINAPI
pg_signal_dispatch_thread(LPVOID param)
{
	HANDLE		pipe = (HANDLE) param;
	BYTE		sigNum;
	DWORD		bytes;

	if (!ReadFile(pipe, &sigNum, 1, &bytes, NULL))
	{
		/* Client died before sending */
		CloseHandle(pipe);
		return 0;
	}
	if (bytes != 1)
	{
		/* Received <bytes> bytes over signal pipe (should be 1) */
		CloseHandle(pipe);
		return 0;
	}
	WriteFile(pipe, &sigNum, 1, &bytes, NULL);	/* Don't care if it works or
												 * not.. */
	FlushFileBuffers(pipe);
	DisconnectNamedPipe(pipe);
	CloseHandle(pipe);

	pg_queue_signal(sigNum);
	return 0;
}

/* Signal handling thread */
static DWORD WINAPI
pg_signal_thread(LPVOID param)
{
	char		pipename[128];
	HANDLE		pipe = pgwin32_initial_signal_pipe;

	snprintf(pipename, sizeof(pipename), "\\\\.\\pipe\\pgsignal_%lu", GetCurrentProcessId());

	for (;;)
	{
		BOOL		fConnected;
		HANDLE		hThread;

		if (pipe == INVALID_HANDLE_VALUE)
		{
			pipe = CreateNamedPipe(pipename, PIPE_ACCESS_DUPLEX,
					   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
							   PIPE_UNLIMITED_INSTANCES, 16, 16, 1000, NULL);

			if (pipe == INVALID_HANDLE_VALUE)
			{
				write_stderr("could not create signal listener pipe: error code %lu; retrying\n", GetLastError());
				SleepEx(500, FALSE);
				continue;
			}
		}

		fConnected = ConnectNamedPipe(pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		if (fConnected)
		{
			HANDLE		newpipe;

			/*
			 * We have a connected pipe. Pass this off to a separate thread
			 * that will do the actual processing of the pipe.
			 *
			 * We must also create a new instance of the pipe *before* we
			 * start running the new thread. If we don't, there is a race
			 * condition whereby the dispatch thread might run CloseHandle()
			 * before we have created a new instance, thereby causing a small
			 * window of time where we will miss incoming requests.
			 */
			newpipe = CreateNamedPipe(pipename, PIPE_ACCESS_DUPLEX,
					   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
							   PIPE_UNLIMITED_INSTANCES, 16, 16, 1000, NULL);
			if (newpipe == INVALID_HANDLE_VALUE)
			{
				/*
				 * This really should never fail. Just retry in case it does,
				 * even though we have a small race window in that case. There
				 * is nothing else we can do other than abort the whole
				 * process which will be even worse.
				 */
				write_stderr("could not create signal listener pipe: error code %lu; retrying\n", GetLastError());

				/*
				 * Keep going so we at least dispatch this signal. Hopefully,
				 * the call will succeed when retried in the loop soon after.
				 */
			}
			hThread = CreateThread(NULL, 0,
						  (LPTHREAD_START_ROUTINE) pg_signal_dispatch_thread,
								   (LPVOID) pipe, 0, NULL);
			if (hThread == INVALID_HANDLE_VALUE)
				write_stderr("could not create signal dispatch thread: error code %lu\n",
							 GetLastError());
			else
				CloseHandle(hThread);

			/*
			 * Background thread is running with our instance of the pipe. So
			 * replace our reference with the newly created one and loop back
			 * up for another run.
			 */
			pipe = newpipe;
		}
		else
		{
			/*
			 * Connection failed. Cleanup and try again.
			 *
			 * This should never happen. If it does, we have a small race
			 * condition until we loop up and re-create the pipe.
			 */
			CloseHandle(pipe);
			pipe = INVALID_HANDLE_VALUE;
		}
	}
	return 0;
}


/* Console control handler will execute on a thread created
   by the OS at the time of invocation */
static BOOL WINAPI
pg_console_handler(DWORD dwCtrlType)
{
	if (dwCtrlType == CTRL_C_EVENT ||
		dwCtrlType == CTRL_BREAK_EVENT ||
		dwCtrlType == CTRL_CLOSE_EVENT ||
		dwCtrlType == CTRL_SHUTDOWN_EVENT)
	{
		pg_queue_signal(SIGINT);
		return TRUE;
	}
	return FALSE;
}
