/*-------------------------------------------------------------------------
 *
 * parallel.c
 *
 *	Parallel support for pg_dump and pg_restore
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/parallel.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * Parallel operation works like this:
 *
 * The original, leader process calls ParallelBackupStart(), which forks off
 * the desired number of worker processes, which each enter WaitForCommands().
 *
 * The leader process dispatches an individual work item to one of the worker
 * processes in DispatchJobForTocEntry().  We send a command string such as
 * "DUMP 1234" or "RESTORE 1234", where 1234 is the TocEntry ID.
 * The worker process receives and decodes the command and passes it to the
 * routine pointed to by AH->WorkerJobDumpPtr or AH->WorkerJobRestorePtr,
 * which are routines of the current archive format.  That routine performs
 * the required action (dump or restore) and returns an integer status code.
 * This is passed back to the leader where we pass it to the
 * ParallelCompletionPtr callback function that was passed to
 * DispatchJobForTocEntry().  The callback function does state updating
 * for the leader control logic in pg_backup_archiver.c.
 *
 * In principle additional archive-format-specific information might be needed
 * in commands or worker status responses, but so far that hasn't proved
 * necessary, since workers have full copies of the ArchiveHandle/TocEntry
 * data structures.  Remember that we have forked off the workers only after
 * we have read in the catalog.  That's why our worker processes can also
 * access the catalog information.  (In the Windows case, the workers are
 * threads in the same process.  To avoid problems, they work with cloned
 * copies of the Archive data structure; see RunWorker().)
 *
 * In the leader process, the workerStatus field for each worker has one of
 * the following values:
 *		WRKR_NOT_STARTED: we've not yet forked this worker
 *		WRKR_IDLE: it's waiting for a command
 *		WRKR_WORKING: it's working on a command
 *		WRKR_TERMINATED: process ended
 * The pstate->te[] entry for each worker is valid when it's in WRKR_WORKING
 * state, and must be NULL in other states.
 */

#include "postgres_fe.h"

#ifndef WIN32
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include "fe_utils/string_utils.h"
#include "parallel.h"
#include "pg_backup_utils.h"
#ifdef WIN32
#include "port/pg_bswap.h"
#endif

/* Mnemonic macros for indexing the fd array returned by pipe(2) */
#define PIPE_READ							0
#define PIPE_WRITE							1

#define NO_SLOT (-1)			/* Failure result for GetIdleWorker() */

/* Worker process statuses */
typedef enum
{
	WRKR_NOT_STARTED = 0,
	WRKR_IDLE,
	WRKR_WORKING,
	WRKR_TERMINATED,
} T_WorkerStatus;

#define WORKER_IS_RUNNING(workerStatus) \
	((workerStatus) == WRKR_IDLE || (workerStatus) == WRKR_WORKING)

/*
 * Private per-parallel-worker state (typedef for this is in parallel.h).
 *
 * Much of this is valid only in the leader process (or, on Windows, should
 * be touched only by the leader thread).  But the AH field should be touched
 * only by workers.  The pipe descriptors are valid everywhere.
 */
struct ParallelSlot
{
	T_WorkerStatus workerStatus;	/* see enum above */

	/* These fields are valid if workerStatus == WRKR_WORKING: */
	ParallelCompletionPtr callback; /* function to call on completion */
	void	   *callback_data;	/* passthrough data for it */

	ArchiveHandle *AH;			/* Archive data worker is using */

	int			pipeRead;		/* leader's end of the pipes */
	int			pipeWrite;
	int			pipeRevRead;	/* child's end of the pipes */
	int			pipeRevWrite;

	/* Child process/thread identity info: */
#ifdef WIN32
	uintptr_t	hThread;
	unsigned int threadId;
#else
	pid_t		pid;
#endif
};

#ifdef WIN32

/*
 * Structure to hold info passed by _beginthreadex() to the function it calls
 * via its single allowed argument.
 */
typedef struct
{
	ArchiveHandle *AH;			/* leader database connection */
	ParallelSlot *slot;			/* this worker's parallel slot */
} WorkerInfo;

/* Windows implementation of pipe access */
static int	pgpipe(int handles[2]);
#define piperead(a,b,c)		recv(a,b,c,0)
#define pipewrite(a,b,c)	send(a,b,c,0)

#else							/* !WIN32 */

/* Non-Windows implementation of pipe access */
#define pgpipe(a)			pipe(a)
#define piperead(a,b,c)		read(a,b,c)
#define pipewrite(a,b,c)	write(a,b,c)

#endif							/* WIN32 */

/*
 * State info for archive_close_connection() shutdown callback.
 */
typedef struct ShutdownInformation
{
	ParallelState *pstate;
	Archive    *AHX;
} ShutdownInformation;

static ShutdownInformation shutdown_info;

/*
 * State info for signal handling.
 * We assume signal_info initializes to zeroes.
 *
 * On Unix, myAH is the leader DB connection in the leader process, and the
 * worker's own connection in worker processes.  On Windows, we have only one
 * instance of signal_info, so myAH is the leader connection and the worker
 * connections must be dug out of pstate->parallelSlot[].
 */
typedef struct DumpSignalInformation
{
	ArchiveHandle *myAH;		/* database connection to issue cancel for */
	ParallelState *pstate;		/* parallel state, if any */
	bool		handler_set;	/* signal handler set up in this process? */
#ifndef WIN32
	bool		am_worker;		/* am I a worker process? */
#endif
} DumpSignalInformation;

static volatile DumpSignalInformation signal_info;

#ifdef WIN32
static CRITICAL_SECTION signal_info_lock;
#endif

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


#ifdef WIN32
/* file-scope variables */
static DWORD tls_index;

/* globally visible variables (needed by exit_nicely) */
bool		parallel_init_done = false;
DWORD		mainThreadId;
#endif							/* WIN32 */

/* Local function prototypes */
static ParallelSlot *GetMyPSlot(ParallelState *pstate);
static void archive_close_connection(int code, void *arg);
static void ShutdownWorkersHard(ParallelState *pstate);
static void WaitForTerminatingWorkers(ParallelState *pstate);
static void set_cancel_handler(void);
static void set_cancel_pstate(ParallelState *pstate);
static void set_cancel_slot_archive(ParallelSlot *slot, ArchiveHandle *AH);
static void RunWorker(ArchiveHandle *AH, ParallelSlot *slot);
static int	GetIdleWorker(ParallelState *pstate);
static bool HasEveryWorkerTerminated(ParallelState *pstate);
static void lockTableForWorker(ArchiveHandle *AH, TocEntry *te);
static void WaitForCommands(ArchiveHandle *AH, int pipefd[2]);
static bool ListenToWorkers(ArchiveHandle *AH, ParallelState *pstate,
							bool do_wait);
static char *getMessageFromLeader(int pipefd[2]);
static void sendMessageToLeader(int pipefd[2], const char *str);
static int	select_loop(int maxFd, fd_set *workerset);
static char *getMessageFromWorker(ParallelState *pstate,
								  bool do_wait, int *worker);
static void sendMessageToWorker(ParallelState *pstate,
								int worker, const char *str);
static char *readMessageFromPipe(int fd);

#define messageStartsWith(msg, prefix) \
	(strncmp(msg, prefix, strlen(prefix)) == 0)


/*
 * Initialize parallel dump support --- should be called early in process
 * startup.  (Currently, this is called whether or not we intend parallel
 * activity.)
 */
void
init_parallel_dump_utils(void)
{
#ifdef WIN32
	if (!parallel_init_done)
	{
		WSADATA		wsaData;
		int			err;

		/* Prepare for threaded operation */
		tls_index = TlsAlloc();
		mainThreadId = GetCurrentThreadId();

		/* Initialize socket access */
		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err != 0)
			pg_fatal("%s() failed: error code %d", "WSAStartup", err);

		parallel_init_done = true;
	}
#endif
}

/*
 * Find the ParallelSlot for the current worker process or thread.
 *
 * Returns NULL if no matching slot is found (this implies we're the leader).
 */
static ParallelSlot *
GetMyPSlot(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
#ifdef WIN32
		if (pstate->parallelSlot[i].threadId == GetCurrentThreadId())
#else
		if (pstate->parallelSlot[i].pid == getpid())
#endif
			return &(pstate->parallelSlot[i]);
	}

	return NULL;
}

/*
 * A thread-local version of getLocalPQExpBuffer().
 *
 * Non-reentrant but reduces memory leakage: we'll consume one buffer per
 * thread, which is much better than one per fmtId/fmtQualifiedId call.
 */
#ifdef WIN32
static PQExpBuffer
getThreadLocalPQExpBuffer(void)
{
	/*
	 * The Tls code goes awry if we use a static var, so we provide for both
	 * static and auto, and omit any use of the static var when using Tls. We
	 * rely on TlsGetValue() to return 0 if the value is not yet set.
	 */
	static PQExpBuffer s_id_return = NULL;
	PQExpBuffer id_return;

	if (parallel_init_done)
		id_return = (PQExpBuffer) TlsGetValue(tls_index);
	else
		id_return = s_id_return;

	if (id_return)				/* first time through? */
	{
		/* same buffer, just wipe contents */
		resetPQExpBuffer(id_return);
	}
	else
	{
		/* new buffer */
		id_return = createPQExpBuffer();
		if (parallel_init_done)
			TlsSetValue(tls_index, id_return);
		else
			s_id_return = id_return;
	}

	return id_return;
}
#endif							/* WIN32 */

/*
 * pg_dump and pg_restore call this to register the cleanup handler
 * as soon as they've created the ArchiveHandle.
 */
void
on_exit_close_archive(Archive *AHX)
{
	shutdown_info.AHX = AHX;
	on_exit_nicely(archive_close_connection, &shutdown_info);
}

/*
 * on_exit_nicely handler for shutting down database connections and
 * worker processes cleanly.
 */
static void
archive_close_connection(int code, void *arg)
{
	ShutdownInformation *si = (ShutdownInformation *) arg;

	if (si->pstate)
	{
		/* In parallel mode, must figure out who we are */
		ParallelSlot *slot = GetMyPSlot(si->pstate);

		if (!slot)
		{
			/*
			 * We're the leader.  Forcibly shut down workers, then close our
			 * own database connection, if any.
			 */
			ShutdownWorkersHard(si->pstate);

			if (si->AHX)
				DisconnectDatabase(si->AHX);
		}
		else
		{
			/*
			 * We're a worker.  Shut down our own DB connection if any.  On
			 * Windows, we also have to close our communication sockets, to
			 * emulate what will happen on Unix when the worker process exits.
			 * (Without this, if this is a premature exit, the leader would
			 * fail to detect it because there would be no EOF condition on
			 * the other end of the pipe.)
			 */
			if (slot->AH)
				DisconnectDatabase(&(slot->AH->public));

#ifdef WIN32
			closesocket(slot->pipeRevRead);
			closesocket(slot->pipeRevWrite);
#endif
		}
	}
	else
	{
		/* Non-parallel operation: just kill the leader DB connection */
		if (si->AHX)
			DisconnectDatabase(si->AHX);
	}
}

/*
 * Forcibly shut down any remaining workers, waiting for them to finish.
 *
 * Note that we don't expect to come here during normal exit (the workers
 * should be long gone, and the ParallelState too).  We're only here in a
 * pg_fatal() situation, so intervening to cancel active commands is
 * appropriate.
 */
static void
ShutdownWorkersHard(ParallelState *pstate)
{
	int			i;

	/*
	 * Close our write end of the sockets so that any workers waiting for
	 * commands know they can exit.  (Note: some of the pipeWrite fields might
	 * still be zero, if we failed to initialize all the workers.  Hence, just
	 * ignore errors here.)
	 */
	for (i = 0; i < pstate->numWorkers; i++)
		closesocket(pstate->parallelSlot[i].pipeWrite);

	/*
	 * Force early termination of any commands currently in progress.
	 */
#ifndef WIN32
	/* On non-Windows, send SIGTERM to each worker process. */
	for (i = 0; i < pstate->numWorkers; i++)
	{
		pid_t		pid = pstate->parallelSlot[i].pid;

		if (pid != 0)
			kill(pid, SIGTERM);
	}
#else

	/*
	 * On Windows, send query cancels directly to the workers' backends.  Use
	 * a critical section to ensure worker threads don't change state.
	 */
	EnterCriticalSection(&signal_info_lock);
	for (i = 0; i < pstate->numWorkers; i++)
	{
		ArchiveHandle *AH = pstate->parallelSlot[i].AH;
		char		errbuf[1];

		if (AH != NULL && AH->connCancel != NULL)
			(void) PQcancel(AH->connCancel, errbuf, sizeof(errbuf));
	}
	LeaveCriticalSection(&signal_info_lock);
#endif

	/* Now wait for them to terminate. */
	WaitForTerminatingWorkers(pstate);
}

/*
 * Wait for all workers to terminate.
 */
static void
WaitForTerminatingWorkers(ParallelState *pstate)
{
	while (!HasEveryWorkerTerminated(pstate))
	{
		ParallelSlot *slot = NULL;
		int			j;

#ifndef WIN32
		/* On non-Windows, use wait() to wait for next worker to end */
		int			status;
		pid_t		pid = wait(&status);

		/* Find dead worker's slot, and clear the PID field */
		for (j = 0; j < pstate->numWorkers; j++)
		{
			slot = &(pstate->parallelSlot[j]);
			if (slot->pid == pid)
			{
				slot->pid = 0;
				break;
			}
		}
#else							/* WIN32 */
		/* On Windows, we must use WaitForMultipleObjects() */
		HANDLE	   *lpHandles = pg_malloc(sizeof(HANDLE) * pstate->numWorkers);
		int			nrun = 0;
		DWORD		ret;
		uintptr_t	hThread;

		for (j = 0; j < pstate->numWorkers; j++)
		{
			if (WORKER_IS_RUNNING(pstate->parallelSlot[j].workerStatus))
			{
				lpHandles[nrun] = (HANDLE) pstate->parallelSlot[j].hThread;
				nrun++;
			}
		}
		ret = WaitForMultipleObjects(nrun, lpHandles, false, INFINITE);
		Assert(ret != WAIT_FAILED);
		hThread = (uintptr_t) lpHandles[ret - WAIT_OBJECT_0];
		free(lpHandles);

		/* Find dead worker's slot, and clear the hThread field */
		for (j = 0; j < pstate->numWorkers; j++)
		{
			slot = &(pstate->parallelSlot[j]);
			if (slot->hThread == hThread)
			{
				/* For cleanliness, close handles for dead threads */
				CloseHandle((HANDLE) slot->hThread);
				slot->hThread = (uintptr_t) INVALID_HANDLE_VALUE;
				break;
			}
		}
#endif							/* WIN32 */

		/* On all platforms, update workerStatus and te[] as well */
		Assert(j < pstate->numWorkers);
		slot->workerStatus = WRKR_TERMINATED;
		pstate->te[j] = NULL;
	}
}


/*
 * Code for responding to cancel interrupts (SIGINT, control-C, etc)
 *
 * This doesn't quite belong in this module, but it needs access to the
 * ParallelState data, so there's not really a better place either.
 *
 * When we get a cancel interrupt, we could just die, but in pg_restore that
 * could leave a SQL command (e.g., CREATE INDEX on a large table) running
 * for a long time.  Instead, we try to send a cancel request and then die.
 * pg_dump probably doesn't really need this, but we might as well use it
 * there too.  Note that sending the cancel directly from the signal handler
 * is safe because PQcancel() is written to make it so.
 *
 * In parallel operation on Unix, each process is responsible for canceling
 * its own connection (this must be so because nobody else has access to it).
 * Furthermore, the leader process should attempt to forward its signal to
 * each child.  In simple manual use of pg_dump/pg_restore, forwarding isn't
 * needed because typing control-C at the console would deliver SIGINT to
 * every member of the terminal process group --- but in other scenarios it
 * might be that only the leader gets signaled.
 *
 * On Windows, the cancel handler runs in a separate thread, because that's
 * how SetConsoleCtrlHandler works.  We make it stop worker threads, send
 * cancels on all active connections, and then return FALSE, which will allow
 * the process to die.  For safety's sake, we use a critical section to
 * protect the PGcancel structures against being changed while the signal
 * thread runs.
 */

#ifndef WIN32

/*
 * Signal handler (Unix only)
 */
static void
sigTermHandler(SIGNAL_ARGS)
{
	int			i;
	char		errbuf[1];

	/*
	 * Some platforms allow delivery of new signals to interrupt an active
	 * signal handler.  That could muck up our attempt to send PQcancel, so
	 * disable the signals that set_cancel_handler enabled.
	 */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, SIG_IGN);

	/*
	 * If we're in the leader, forward signal to all workers.  (It seems best
	 * to do this before PQcancel; killing the leader transaction will result
	 * in invalid-snapshot errors from active workers, which maybe we can
	 * quiet by killing workers first.)  Ignore any errors.
	 */
	if (signal_info.pstate != NULL)
	{
		for (i = 0; i < signal_info.pstate->numWorkers; i++)
		{
			pid_t		pid = signal_info.pstate->parallelSlot[i].pid;

			if (pid != 0)
				kill(pid, SIGTERM);
		}
	}

	/*
	 * Send QueryCancel if we have a connection to send to.  Ignore errors,
	 * there's not much we can do about them anyway.
	 */
	if (signal_info.myAH != NULL && signal_info.myAH->connCancel != NULL)
		(void) PQcancel(signal_info.myAH->connCancel, errbuf, sizeof(errbuf));

	/*
	 * Report we're quitting, using nothing more complicated than write(2).
	 * When in parallel operation, only the leader process should do this.
	 */
	if (!signal_info.am_worker)
	{
		if (progname)
		{
			write_stderr(progname);
			write_stderr(": ");
		}
		write_stderr("terminated by user\n");
	}

	/*
	 * And die, using _exit() not exit() because the latter will invoke atexit
	 * handlers that can fail if we interrupted related code.
	 */
	_exit(1);
}

/*
 * Enable cancel interrupt handler, if not already done.
 */
static void
set_cancel_handler(void)
{
	/*
	 * When forking, signal_info.handler_set will propagate into the new
	 * process, but that's fine because the signal handler state does too.
	 */
	if (!signal_info.handler_set)
	{
		signal_info.handler_set = true;

		pqsignal(SIGINT, sigTermHandler);
		pqsignal(SIGTERM, sigTermHandler);
		pqsignal(SIGQUIT, sigTermHandler);
	}
}

#else							/* WIN32 */

/*
 * Console interrupt handler --- runs in a newly-started thread.
 *
 * After stopping other threads and sending cancel requests on all open
 * connections, we return FALSE which will allow the default ExitProcess()
 * action to be taken.
 */
static BOOL WINAPI
consoleHandler(DWORD dwCtrlType)
{
	int			i;
	char		errbuf[1];

	if (dwCtrlType == CTRL_C_EVENT ||
		dwCtrlType == CTRL_BREAK_EVENT)
	{
		/* Critical section prevents changing data we look at here */
		EnterCriticalSection(&signal_info_lock);

		/*
		 * If in parallel mode, stop worker threads and send QueryCancel to
		 * their connected backends.  The main point of stopping the worker
		 * threads is to keep them from reporting the query cancels as errors,
		 * which would clutter the user's screen.  We needn't stop the leader
		 * thread since it won't be doing much anyway.  Do this before
		 * canceling the main transaction, else we might get invalid-snapshot
		 * errors reported before we can stop the workers.  Ignore errors,
		 * there's not much we can do about them anyway.
		 */
		if (signal_info.pstate != NULL)
		{
			for (i = 0; i < signal_info.pstate->numWorkers; i++)
			{
				ParallelSlot *slot = &(signal_info.pstate->parallelSlot[i]);
				ArchiveHandle *AH = slot->AH;
				HANDLE		hThread = (HANDLE) slot->hThread;

				/*
				 * Using TerminateThread here may leave some resources leaked,
				 * but it doesn't matter since we're about to end the whole
				 * process.
				 */
				if (hThread != INVALID_HANDLE_VALUE)
					TerminateThread(hThread, 0);

				if (AH != NULL && AH->connCancel != NULL)
					(void) PQcancel(AH->connCancel, errbuf, sizeof(errbuf));
			}
		}

		/*
		 * Send QueryCancel to leader connection, if enabled.  Ignore errors,
		 * there's not much we can do about them anyway.
		 */
		if (signal_info.myAH != NULL && signal_info.myAH->connCancel != NULL)
			(void) PQcancel(signal_info.myAH->connCancel,
							errbuf, sizeof(errbuf));

		LeaveCriticalSection(&signal_info_lock);

		/*
		 * Report we're quitting, using nothing more complicated than
		 * write(2).  (We might be able to get away with using pg_log_*()
		 * here, but since we terminated other threads uncleanly above, it
		 * seems better to assume as little as possible.)
		 */
		if (progname)
		{
			write_stderr(progname);
			write_stderr(": ");
		}
		write_stderr("terminated by user\n");
	}

	/* Always return FALSE to allow signal handling to continue */
	return FALSE;
}

/*
 * Enable cancel interrupt handler, if not already done.
 */
static void
set_cancel_handler(void)
{
	if (!signal_info.handler_set)
	{
		signal_info.handler_set = true;

		InitializeCriticalSection(&signal_info_lock);

		SetConsoleCtrlHandler(consoleHandler, TRUE);
	}
}

#endif							/* WIN32 */


/*
 * set_archive_cancel_info
 *
 * Fill AH->connCancel with cancellation info for the specified database
 * connection; or clear it if conn is NULL.
 */
void
set_archive_cancel_info(ArchiveHandle *AH, PGconn *conn)
{
	PGcancel   *oldConnCancel;

	/*
	 * Activate the interrupt handler if we didn't yet in this process.  On
	 * Windows, this also initializes signal_info_lock; therefore it's
	 * important that this happen at least once before we fork off any
	 * threads.
	 */
	set_cancel_handler();

	/*
	 * On Unix, we assume that storing a pointer value is atomic with respect
	 * to any possible signal interrupt.  On Windows, use a critical section.
	 */

#ifdef WIN32
	EnterCriticalSection(&signal_info_lock);
#endif

	/* Free the old one if we have one */
	oldConnCancel = AH->connCancel;
	/* be sure interrupt handler doesn't use pointer while freeing */
	AH->connCancel = NULL;

	if (oldConnCancel != NULL)
		PQfreeCancel(oldConnCancel);

	/* Set the new one if specified */
	if (conn)
		AH->connCancel = PQgetCancel(conn);

	/*
	 * On Unix, there's only ever one active ArchiveHandle per process, so we
	 * can just set signal_info.myAH unconditionally.  On Windows, do that
	 * only in the main thread; worker threads have to make sure their
	 * ArchiveHandle appears in the pstate data, which is dealt with in
	 * RunWorker().
	 */
#ifndef WIN32
	signal_info.myAH = AH;
#else
	if (mainThreadId == GetCurrentThreadId())
		signal_info.myAH = AH;
#endif

#ifdef WIN32
	LeaveCriticalSection(&signal_info_lock);
#endif
}

/*
 * set_cancel_pstate
 *
 * Set signal_info.pstate to point to the specified ParallelState, if any.
 * We need this mainly to have an interlock against Windows signal thread.
 */
static void
set_cancel_pstate(ParallelState *pstate)
{
#ifdef WIN32
	EnterCriticalSection(&signal_info_lock);
#endif

	signal_info.pstate = pstate;

#ifdef WIN32
	LeaveCriticalSection(&signal_info_lock);
#endif
}

/*
 * set_cancel_slot_archive
 *
 * Set ParallelSlot's AH field to point to the specified archive, if any.
 * We need this mainly to have an interlock against Windows signal thread.
 */
static void
set_cancel_slot_archive(ParallelSlot *slot, ArchiveHandle *AH)
{
#ifdef WIN32
	EnterCriticalSection(&signal_info_lock);
#endif

	slot->AH = AH;

#ifdef WIN32
	LeaveCriticalSection(&signal_info_lock);
#endif
}


/*
 * This function is called by both Unix and Windows variants to set up
 * and run a worker process.  Caller should exit the process (or thread)
 * upon return.
 */
static void
RunWorker(ArchiveHandle *AH, ParallelSlot *slot)
{
	int			pipefd[2];

	/* fetch child ends of pipes */
	pipefd[PIPE_READ] = slot->pipeRevRead;
	pipefd[PIPE_WRITE] = slot->pipeRevWrite;

	/*
	 * Clone the archive so that we have our own state to work with, and in
	 * particular our own database connection.
	 *
	 * We clone on Unix as well as Windows, even though technically we don't
	 * need to because fork() gives us a copy in our own address space
	 * already.  But CloneArchive resets the state information and also clones
	 * the database connection which both seem kinda helpful.
	 */
	AH = CloneArchive(AH);

	/* Remember cloned archive where signal handler can find it */
	set_cancel_slot_archive(slot, AH);

	/*
	 * Call the setup worker function that's defined in the ArchiveHandle.
	 */
	(AH->SetupWorkerPtr) ((Archive *) AH);

	/*
	 * Execute commands until done.
	 */
	WaitForCommands(AH, pipefd);

	/*
	 * Disconnect from database and clean up.
	 */
	set_cancel_slot_archive(slot, NULL);
	DisconnectDatabase(&(AH->public));
	DeCloneArchive(AH);
}

/*
 * Thread base function for Windows
 */
#ifdef WIN32
static unsigned __stdcall
init_spawned_worker_win32(WorkerInfo *wi)
{
	ArchiveHandle *AH = wi->AH;
	ParallelSlot *slot = wi->slot;

	/* Don't need WorkerInfo anymore */
	free(wi);

	/* Run the worker ... */
	RunWorker(AH, slot);

	/* Exit the thread */
	_endthreadex(0);
	return 0;
}
#endif							/* WIN32 */

/*
 * This function starts a parallel dump or restore by spawning off the worker
 * processes.  For Windows, it creates a number of threads; on Unix the
 * workers are created with fork().
 */
ParallelState *
ParallelBackupStart(ArchiveHandle *AH)
{
	ParallelState *pstate;
	int			i;

	Assert(AH->public.numWorkers > 0);

	pstate = (ParallelState *) pg_malloc(sizeof(ParallelState));

	pstate->numWorkers = AH->public.numWorkers;
	pstate->te = NULL;
	pstate->parallelSlot = NULL;

	if (AH->public.numWorkers == 1)
		return pstate;

	/* Create status arrays, being sure to initialize all fields to 0 */
	pstate->te = (TocEntry **)
		pg_malloc0(pstate->numWorkers * sizeof(TocEntry *));
	pstate->parallelSlot = (ParallelSlot *)
		pg_malloc0(pstate->numWorkers * sizeof(ParallelSlot));

#ifdef WIN32
	/* Make fmtId() and fmtQualifiedId() use thread-local storage */
	getLocalPQExpBuffer = getThreadLocalPQExpBuffer;
#endif

	/*
	 * Set the pstate in shutdown_info, to tell the exit handler that it must
	 * clean up workers as well as the main database connection.  But we don't
	 * set this in signal_info yet, because we don't want child processes to
	 * inherit non-NULL signal_info.pstate.
	 */
	shutdown_info.pstate = pstate;

	/*
	 * Temporarily disable query cancellation on the leader connection.  This
	 * ensures that child processes won't inherit valid AH->connCancel
	 * settings and thus won't try to issue cancels against the leader's
	 * connection.  No harm is done if we fail while it's disabled, because
	 * the leader connection is idle at this point anyway.
	 */
	set_archive_cancel_info(AH, NULL);

	/* Ensure stdio state is quiesced before forking */
	fflush(NULL);

	/* Create desired number of workers */
	for (i = 0; i < pstate->numWorkers; i++)
	{
#ifdef WIN32
		WorkerInfo *wi;
		uintptr_t	handle;
#else
		pid_t		pid;
#endif
		ParallelSlot *slot = &(pstate->parallelSlot[i]);
		int			pipeMW[2],
					pipeWM[2];

		/* Create communication pipes for this worker */
		if (pgpipe(pipeMW) < 0 || pgpipe(pipeWM) < 0)
			pg_fatal("could not create communication channels: %m");

		/* leader's ends of the pipes */
		slot->pipeRead = pipeWM[PIPE_READ];
		slot->pipeWrite = pipeMW[PIPE_WRITE];
		/* child's ends of the pipes */
		slot->pipeRevRead = pipeMW[PIPE_READ];
		slot->pipeRevWrite = pipeWM[PIPE_WRITE];

#ifdef WIN32
		/* Create transient structure to pass args to worker function */
		wi = (WorkerInfo *) pg_malloc(sizeof(WorkerInfo));

		wi->AH = AH;
		wi->slot = slot;

		handle = _beginthreadex(NULL, 0, (void *) &init_spawned_worker_win32,
								wi, 0, &(slot->threadId));
		slot->hThread = handle;
		slot->workerStatus = WRKR_IDLE;
#else							/* !WIN32 */
		pid = fork();
		if (pid == 0)
		{
			/* we are the worker */
			int			j;

			/* this is needed for GetMyPSlot() */
			slot->pid = getpid();

			/* instruct signal handler that we're in a worker now */
			signal_info.am_worker = true;

			/* close read end of Worker -> Leader */
			closesocket(pipeWM[PIPE_READ]);
			/* close write end of Leader -> Worker */
			closesocket(pipeMW[PIPE_WRITE]);

			/*
			 * Close all inherited fds for communication of the leader with
			 * previously-forked workers.
			 */
			for (j = 0; j < i; j++)
			{
				closesocket(pstate->parallelSlot[j].pipeRead);
				closesocket(pstate->parallelSlot[j].pipeWrite);
			}

			/* Run the worker ... */
			RunWorker(AH, slot);

			/* We can just exit(0) when done */
			exit(0);
		}
		else if (pid < 0)
		{
			/* fork failed */
			pg_fatal("could not create worker process: %m");
		}

		/* In Leader after successful fork */
		slot->pid = pid;
		slot->workerStatus = WRKR_IDLE;

		/* close read end of Leader -> Worker */
		closesocket(pipeMW[PIPE_READ]);
		/* close write end of Worker -> Leader */
		closesocket(pipeWM[PIPE_WRITE]);
#endif							/* WIN32 */
	}

	/*
	 * Having forked off the workers, disable SIGPIPE so that leader isn't
	 * killed if it tries to send a command to a dead worker.  We don't want
	 * the workers to inherit this setting, though.
	 */
#ifndef WIN32
	pqsignal(SIGPIPE, SIG_IGN);
#endif

	/*
	 * Re-establish query cancellation on the leader connection.
	 */
	set_archive_cancel_info(AH, AH->connection);

	/*
	 * Tell the cancel signal handler to forward signals to worker processes,
	 * too.  (As with query cancel, we did not need this earlier because the
	 * workers have not yet been given anything to do; if we die before this
	 * point, any already-started workers will see EOF and quit promptly.)
	 */
	set_cancel_pstate(pstate);

	return pstate;
}

/*
 * Close down a parallel dump or restore.
 */
void
ParallelBackupEnd(ArchiveHandle *AH, ParallelState *pstate)
{
	int			i;

	/* No work if non-parallel */
	if (pstate->numWorkers == 1)
		return;

	/* There should not be any unfinished jobs */
	Assert(IsEveryWorkerIdle(pstate));

	/* Close the sockets so that the workers know they can exit */
	for (i = 0; i < pstate->numWorkers; i++)
	{
		closesocket(pstate->parallelSlot[i].pipeRead);
		closesocket(pstate->parallelSlot[i].pipeWrite);
	}

	/* Wait for them to exit */
	WaitForTerminatingWorkers(pstate);

	/*
	 * Unlink pstate from shutdown_info, so the exit handler will not try to
	 * use it; and likewise unlink from signal_info.
	 */
	shutdown_info.pstate = NULL;
	set_cancel_pstate(NULL);

	/* Release state (mere neatnik-ism, since we're about to terminate) */
	free(pstate->te);
	free(pstate->parallelSlot);
	free(pstate);
}

/*
 * These next four functions handle construction and parsing of the command
 * strings and response strings for parallel workers.
 *
 * Currently, these can be the same regardless of which archive format we are
 * processing.  In future, we might want to let format modules override these
 * functions to add format-specific data to a command or response.
 */

/*
 * buildWorkerCommand: format a command string to send to a worker.
 *
 * The string is built in the caller-supplied buffer of size buflen.
 */
static void
buildWorkerCommand(ArchiveHandle *AH, TocEntry *te, T_Action act,
				   char *buf, int buflen)
{
	if (act == ACT_DUMP)
		snprintf(buf, buflen, "DUMP %d", te->dumpId);
	else if (act == ACT_RESTORE)
		snprintf(buf, buflen, "RESTORE %d", te->dumpId);
	else
		Assert(false);
}

/*
 * parseWorkerCommand: interpret a command string in a worker.
 */
static void
parseWorkerCommand(ArchiveHandle *AH, TocEntry **te, T_Action *act,
				   const char *msg)
{
	DumpId		dumpId;
	int			nBytes;

	if (messageStartsWith(msg, "DUMP "))
	{
		*act = ACT_DUMP;
		sscanf(msg, "DUMP %d%n", &dumpId, &nBytes);
		Assert(nBytes == strlen(msg));
		*te = getTocEntryByDumpId(AH, dumpId);
		Assert(*te != NULL);
	}
	else if (messageStartsWith(msg, "RESTORE "))
	{
		*act = ACT_RESTORE;
		sscanf(msg, "RESTORE %d%n", &dumpId, &nBytes);
		Assert(nBytes == strlen(msg));
		*te = getTocEntryByDumpId(AH, dumpId);
		Assert(*te != NULL);
	}
	else
		pg_fatal("unrecognized command received from leader: \"%s\"",
				 msg);
}

/*
 * buildWorkerResponse: format a response string to send to the leader.
 *
 * The string is built in the caller-supplied buffer of size buflen.
 */
static void
buildWorkerResponse(ArchiveHandle *AH, TocEntry *te, T_Action act, int status,
					char *buf, int buflen)
{
	snprintf(buf, buflen, "OK %d %d %d",
			 te->dumpId,
			 status,
			 status == WORKER_IGNORED_ERRORS ? AH->public.n_errors : 0);
}

/*
 * parseWorkerResponse: parse the status message returned by a worker.
 *
 * Returns the integer status code, and may update fields of AH and/or te.
 */
static int
parseWorkerResponse(ArchiveHandle *AH, TocEntry *te,
					const char *msg)
{
	DumpId		dumpId;
	int			nBytes,
				n_errors;
	int			status = 0;

	if (messageStartsWith(msg, "OK "))
	{
		sscanf(msg, "OK %d %d %d%n", &dumpId, &status, &n_errors, &nBytes);

		Assert(dumpId == te->dumpId);
		Assert(nBytes == strlen(msg));

		AH->public.n_errors += n_errors;
	}
	else
		pg_fatal("invalid message received from worker: \"%s\"",
				 msg);

	return status;
}

/*
 * Dispatch a job to some free worker.
 *
 * te is the TocEntry to be processed, act is the action to be taken on it.
 * callback is the function to call on completion of the job.
 *
 * If no worker is currently available, this will block, and previously
 * registered callback functions may be called.
 */
void
DispatchJobForTocEntry(ArchiveHandle *AH,
					   ParallelState *pstate,
					   TocEntry *te,
					   T_Action act,
					   ParallelCompletionPtr callback,
					   void *callback_data)
{
	int			worker;
	char		buf[256];

	/* Get a worker, waiting if none are idle */
	while ((worker = GetIdleWorker(pstate)) == NO_SLOT)
		WaitForWorkers(AH, pstate, WFW_ONE_IDLE);

	/* Construct and send command string */
	buildWorkerCommand(AH, te, act, buf, sizeof(buf));

	sendMessageToWorker(pstate, worker, buf);

	/* Remember worker is busy, and which TocEntry it's working on */
	pstate->parallelSlot[worker].workerStatus = WRKR_WORKING;
	pstate->parallelSlot[worker].callback = callback;
	pstate->parallelSlot[worker].callback_data = callback_data;
	pstate->te[worker] = te;
}

/*
 * Find an idle worker and return its slot number.
 * Return NO_SLOT if none are idle.
 */
static int
GetIdleWorker(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus == WRKR_IDLE)
			return i;
	}
	return NO_SLOT;
}

/*
 * Return true iff no worker is running.
 */
static bool
HasEveryWorkerTerminated(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (WORKER_IS_RUNNING(pstate->parallelSlot[i].workerStatus))
			return false;
	}
	return true;
}

/*
 * Return true iff every worker is in the WRKR_IDLE state.
 */
bool
IsEveryWorkerIdle(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus != WRKR_IDLE)
			return false;
	}
	return true;
}

/*
 * Acquire lock on a table to be dumped by a worker process.
 *
 * The leader process is already holding an ACCESS SHARE lock.  Ordinarily
 * it's no problem for a worker to get one too, but if anything else besides
 * pg_dump is running, there's a possible deadlock:
 *
 * 1) Leader dumps the schema and locks all tables in ACCESS SHARE mode.
 * 2) Another process requests an ACCESS EXCLUSIVE lock (which is not granted
 *	  because the leader holds a conflicting ACCESS SHARE lock).
 * 3) A worker process also requests an ACCESS SHARE lock to read the table.
 *	  The worker is enqueued behind the ACCESS EXCLUSIVE lock request.
 * 4) Now we have a deadlock, since the leader is effectively waiting for
 *	  the worker.  The server cannot detect that, however.
 *
 * To prevent an infinite wait, prior to touching a table in a worker, request
 * a lock in ACCESS SHARE mode but with NOWAIT.  If we don't get the lock,
 * then we know that somebody else has requested an ACCESS EXCLUSIVE lock and
 * so we have a deadlock.  We must fail the backup in that case.
 */
static void
lockTableForWorker(ArchiveHandle *AH, TocEntry *te)
{
	const char *qualId;
	PQExpBuffer query;
	PGresult   *res;

	/* Nothing to do for BLOBS */
	if (strcmp(te->desc, "BLOBS") == 0)
		return;

	query = createPQExpBuffer();

	qualId = fmtQualifiedId(te->namespace, te->tag);

	appendPQExpBuffer(query, "LOCK TABLE %s IN ACCESS SHARE MODE NOWAIT",
					  qualId);

	res = PQexec(AH->connection, query->data);

	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("could not obtain lock on relation \"%s\"\n"
				 "This usually means that someone requested an ACCESS EXCLUSIVE lock "
				 "on the table after the pg_dump parent process had gotten the "
				 "initial ACCESS SHARE lock on the table.", qualId);

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * WaitForCommands: main routine for a worker process.
 *
 * Read and execute commands from the leader until we see EOF on the pipe.
 */
static void
WaitForCommands(ArchiveHandle *AH, int pipefd[2])
{
	char	   *command;
	TocEntry   *te;
	T_Action	act;
	int			status = 0;
	char		buf[256];

	for (;;)
	{
		if (!(command = getMessageFromLeader(pipefd)))
		{
			/* EOF, so done */
			return;
		}

		/* Decode the command */
		parseWorkerCommand(AH, &te, &act, command);

		if (act == ACT_DUMP)
		{
			/* Acquire lock on this table within the worker's session */
			lockTableForWorker(AH, te);

			/* Perform the dump command */
			status = (AH->WorkerJobDumpPtr) (AH, te);
		}
		else if (act == ACT_RESTORE)
		{
			/* Perform the restore command */
			status = (AH->WorkerJobRestorePtr) (AH, te);
		}
		else
			Assert(false);

		/* Return status to leader */
		buildWorkerResponse(AH, te, act, status, buf, sizeof(buf));

		sendMessageToLeader(pipefd, buf);

		/* command was pg_malloc'd and we are responsible for free()ing it. */
		free(command);
	}
}

/*
 * Check for status messages from workers.
 *
 * If do_wait is true, wait to get a status message; otherwise, just return
 * immediately if there is none available.
 *
 * When we get a status message, we pass the status code to the callback
 * function that was specified to DispatchJobForTocEntry, then reset the
 * worker status to IDLE.
 *
 * Returns true if we collected a status message, else false.
 *
 * XXX is it worth checking for more than one status message per call?
 * It seems somewhat unlikely that multiple workers would finish at exactly
 * the same time.
 */
static bool
ListenToWorkers(ArchiveHandle *AH, ParallelState *pstate, bool do_wait)
{
	int			worker;
	char	   *msg;

	/* Try to collect a status message */
	msg = getMessageFromWorker(pstate, do_wait, &worker);

	if (!msg)
	{
		/* If do_wait is true, we must have detected EOF on some socket */
		if (do_wait)
			pg_fatal("a worker process died unexpectedly");
		return false;
	}

	/* Process it and update our idea of the worker's status */
	if (messageStartsWith(msg, "OK "))
	{
		ParallelSlot *slot = &pstate->parallelSlot[worker];
		TocEntry   *te = pstate->te[worker];
		int			status;

		status = parseWorkerResponse(AH, te, msg);
		slot->callback(AH, te, status, slot->callback_data);
		slot->workerStatus = WRKR_IDLE;
		pstate->te[worker] = NULL;
	}
	else
		pg_fatal("invalid message received from worker: \"%s\"",
				 msg);

	/* Free the string returned from getMessageFromWorker */
	free(msg);

	return true;
}

/*
 * Check for status results from workers, waiting if necessary.
 *
 * Available wait modes are:
 * WFW_NO_WAIT: reap any available status, but don't block
 * WFW_GOT_STATUS: wait for at least one more worker to finish
 * WFW_ONE_IDLE: wait for at least one worker to be idle
 * WFW_ALL_IDLE: wait for all workers to be idle
 *
 * Any received results are passed to the callback specified to
 * DispatchJobForTocEntry.
 *
 * This function is executed in the leader process.
 */
void
WaitForWorkers(ArchiveHandle *AH, ParallelState *pstate, WFW_WaitOption mode)
{
	bool		do_wait = false;

	/*
	 * In GOT_STATUS mode, always block waiting for a message, since we can't
	 * return till we get something.  In other modes, we don't block the first
	 * time through the loop.
	 */
	if (mode == WFW_GOT_STATUS)
	{
		/* Assert that caller knows what it's doing */
		Assert(!IsEveryWorkerIdle(pstate));
		do_wait = true;
	}

	for (;;)
	{
		/*
		 * Check for status messages, even if we don't need to block.  We do
		 * not try very hard to reap all available messages, though, since
		 * there's unlikely to be more than one.
		 */
		if (ListenToWorkers(AH, pstate, do_wait))
		{
			/*
			 * If we got a message, we are done by definition for GOT_STATUS
			 * mode, and we can also be certain that there's at least one idle
			 * worker.  So we're done in all but ALL_IDLE mode.
			 */
			if (mode != WFW_ALL_IDLE)
				return;
		}

		/* Check whether we must wait for new status messages */
		switch (mode)
		{
			case WFW_NO_WAIT:
				return;			/* never wait */
			case WFW_GOT_STATUS:
				Assert(false);	/* can't get here, because we waited */
				break;
			case WFW_ONE_IDLE:
				if (GetIdleWorker(pstate) != NO_SLOT)
					return;
				break;
			case WFW_ALL_IDLE:
				if (IsEveryWorkerIdle(pstate))
					return;
				break;
		}

		/* Loop back, and this time wait for something to happen */
		do_wait = true;
	}
}

/*
 * Read one command message from the leader, blocking if necessary
 * until one is available, and return it as a malloc'd string.
 * On EOF, return NULL.
 *
 * This function is executed in worker processes.
 */
static char *
getMessageFromLeader(int pipefd[2])
{
	return readMessageFromPipe(pipefd[PIPE_READ]);
}

/*
 * Send a status message to the leader.
 *
 * This function is executed in worker processes.
 */
static void
sendMessageToLeader(int pipefd[2], const char *str)
{
	int			len = strlen(str) + 1;

	if (pipewrite(pipefd[PIPE_WRITE], str, len) != len)
		pg_fatal("could not write to the communication channel: %m");
}

/*
 * Wait until some descriptor in "workerset" becomes readable.
 * Returns -1 on error, else the number of readable descriptors.
 */
static int
select_loop(int maxFd, fd_set *workerset)
{
	int			i;
	fd_set		saveSet = *workerset;

	for (;;)
	{
		*workerset = saveSet;
		i = select(maxFd + 1, workerset, NULL, NULL, NULL);

#ifndef WIN32
		if (i < 0 && errno == EINTR)
			continue;
#else
		if (i == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
			continue;
#endif
		break;
	}

	return i;
}


/*
 * Check for messages from worker processes.
 *
 * If a message is available, return it as a malloc'd string, and put the
 * index of the sending worker in *worker.
 *
 * If nothing is available, wait if "do_wait" is true, else return NULL.
 *
 * If we detect EOF on any socket, we'll return NULL.  It's not great that
 * that's hard to distinguish from the no-data-available case, but for now
 * our one caller is okay with that.
 *
 * This function is executed in the leader process.
 */
static char *
getMessageFromWorker(ParallelState *pstate, bool do_wait, int *worker)
{
	int			i;
	fd_set		workerset;
	int			maxFd = -1;
	struct timeval nowait = {0, 0};

	/* construct bitmap of socket descriptors for select() */
	FD_ZERO(&workerset);
	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (!WORKER_IS_RUNNING(pstate->parallelSlot[i].workerStatus))
			continue;
		FD_SET(pstate->parallelSlot[i].pipeRead, &workerset);
		if (pstate->parallelSlot[i].pipeRead > maxFd)
			maxFd = pstate->parallelSlot[i].pipeRead;
	}

	if (do_wait)
	{
		i = select_loop(maxFd, &workerset);
		Assert(i != 0);
	}
	else
	{
		if ((i = select(maxFd + 1, &workerset, NULL, NULL, &nowait)) == 0)
			return NULL;
	}

	if (i < 0)
		pg_fatal("%s() failed: %m", "select");

	for (i = 0; i < pstate->numWorkers; i++)
	{
		char	   *msg;

		if (!WORKER_IS_RUNNING(pstate->parallelSlot[i].workerStatus))
			continue;
		if (!FD_ISSET(pstate->parallelSlot[i].pipeRead, &workerset))
			continue;

		/*
		 * Read the message if any.  If the socket is ready because of EOF,
		 * we'll return NULL instead (and the socket will stay ready, so the
		 * condition will persist).
		 *
		 * Note: because this is a blocking read, we'll wait if only part of
		 * the message is available.  Waiting a long time would be bad, but
		 * since worker status messages are short and are always sent in one
		 * operation, it shouldn't be a problem in practice.
		 */
		msg = readMessageFromPipe(pstate->parallelSlot[i].pipeRead);
		*worker = i;
		return msg;
	}
	Assert(false);
	return NULL;
}

/*
 * Send a command message to the specified worker process.
 *
 * This function is executed in the leader process.
 */
static void
sendMessageToWorker(ParallelState *pstate, int worker, const char *str)
{
	int			len = strlen(str) + 1;

	if (pipewrite(pstate->parallelSlot[worker].pipeWrite, str, len) != len)
	{
		pg_fatal("could not write to the communication channel: %m");
	}
}

/*
 * Read one message from the specified pipe (fd), blocking if necessary
 * until one is available, and return it as a malloc'd string.
 * On EOF, return NULL.
 *
 * A "message" on the channel is just a null-terminated string.
 */
static char *
readMessageFromPipe(int fd)
{
	char	   *msg;
	int			msgsize,
				bufsize;
	int			ret;

	/*
	 * In theory, if we let piperead() read multiple bytes, it might give us
	 * back fragments of multiple messages.  (That can't actually occur, since
	 * neither leader nor workers send more than one message without waiting
	 * for a reply, but we don't wish to assume that here.)  For simplicity,
	 * read a byte at a time until we get the terminating '\0'.  This method
	 * is a bit inefficient, but since this is only used for relatively short
	 * command and status strings, it shouldn't matter.
	 */
	bufsize = 64;				/* could be any number */
	msg = (char *) pg_malloc(bufsize);
	msgsize = 0;
	for (;;)
	{
		Assert(msgsize < bufsize);
		ret = piperead(fd, msg + msgsize, 1);
		if (ret <= 0)
			break;				/* error or connection closure */

		Assert(ret == 1);

		if (msg[msgsize] == '\0')
			return msg;			/* collected whole message */

		msgsize++;
		if (msgsize == bufsize) /* enlarge buffer if needed */
		{
			bufsize += 16;		/* could be any number */
			msg = (char *) pg_realloc(msg, bufsize);
		}
	}

	/* Other end has closed the connection */
	pg_free(msg);
	return NULL;
}

#ifdef WIN32

/*
 * This is a replacement version of pipe(2) for Windows which allows the pipe
 * handles to be used in select().
 *
 * Reads and writes on the pipe must go through piperead()/pipewrite().
 *
 * For consistency with Unix we declare the returned handles as "int".
 * This is okay even on WIN64 because system handles are not more than
 * 32 bits wide, but we do have to do some casting.
 */
static int
pgpipe(int handles[2])
{
	pgsocket	s,
				tmp_sock;
	struct sockaddr_in serv_addr;
	int			len = sizeof(serv_addr);

	/* We have to use the Unix socket invalid file descriptor value here. */
	handles[0] = handles[1] = -1;

	/*
	 * setup listen socket
	 */
	if ((s = socket(AF_INET, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
	{
		pg_log_error("pgpipe: could not create socket: error code %d",
					 WSAGetLastError());
		return -1;
	}

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = pg_hton16(0);
	serv_addr.sin_addr.s_addr = pg_hton32(INADDR_LOOPBACK);
	if (bind(s, (SOCKADDR *) &serv_addr, len) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: could not bind: error code %d",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}
	if (listen(s, 1) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: could not listen: error code %d",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}
	if (getsockname(s, (SOCKADDR *) &serv_addr, &len) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: %s() failed: error code %d", "getsockname",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}

	/*
	 * setup pipe handles
	 */
	if ((tmp_sock = socket(AF_INET, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
	{
		pg_log_error("pgpipe: could not create second socket: error code %d",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}
	handles[1] = (int) tmp_sock;

	if (connect(handles[1], (SOCKADDR *) &serv_addr, len) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: could not connect socket: error code %d",
					 WSAGetLastError());
		closesocket(handles[1]);
		handles[1] = -1;
		closesocket(s);
		return -1;
	}
	if ((tmp_sock = accept(s, (SOCKADDR *) &serv_addr, &len)) == PGINVALID_SOCKET)
	{
		pg_log_error("pgpipe: could not accept connection: error code %d",
					 WSAGetLastError());
		closesocket(handles[1]);
		handles[1] = -1;
		closesocket(s);
		return -1;
	}
	handles[0] = (int) tmp_sock;

	closesocket(s);
	return 0;
}

#endif							/* WIN32 */
