/*-------------------------------------------------------------------------
 *
 * parallel.c
 *
 *	Parallel support for pg_dump and pg_restore
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
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
 * The original, master process calls ParallelBackupStart(), which forks off
 * the desired number of worker processes, which each enter WaitForCommands().
 *
 * The master process dispatches an individual work item to one of the worker
 * processes in DispatchJobForTocEntry().  That calls
 * AH->MasterStartParallelItemPtr, a routine of the output format.  This
 * function's arguments are the parents archive handle AH (containing the full
 * catalog information), the TocEntry that the worker should work on and a
 * T_Action value indicating whether this is a backup or a restore task.  The
 * function simply converts the TocEntry assignment into a command string that
 * is then sent over to the worker process. In the simplest case that would be
 * something like "DUMP 1234", with 1234 being the TocEntry id.
 *
 * The worker process receives and decodes the command and passes it to the
 * routine pointed to by AH->WorkerJobDumpPtr or AH->WorkerJobRestorePtr,
 * which are routines of the current archive format.  That routine performs
 * the required action (dump or restore) and returns a malloc'd status string.
 * The status string is passed back to the master where it is interpreted by
 * AH->MasterEndParallelItemPtr, another format-specific routine.  That
 * function can update state or catalog information on the master's side,
 * depending on the reply from the worker process.  In the end it returns a
 * status code, which is 0 for successful execution.
 *
 * Remember that we have forked off the workers only after we have read in
 * the catalog.  That's why our worker processes can also access the catalog
 * information.  (In the Windows case, the workers are threads in the same
 * process.  To avoid problems, they work with cloned copies of the Archive
 * data structure; see RunWorker().)
 *
 * In the master process, the workerStatus field for each worker has one of
 * the following values:
 *		WRKR_IDLE: it's waiting for a command
 *		WRKR_WORKING: it's been sent a command
 *		WRKR_FINISHED: it's returned a result
 *		WRKR_TERMINATED: process ended
 * The FINISHED state indicates that the worker is idle, but we've not yet
 * dealt with the status code it returned from the prior command.
 * ReapWorkerStatus() extracts the unhandled command status value and sets
 * the workerStatus back to WRKR_IDLE.
 */

#include "postgres_fe.h"

#include "parallel.h"
#include "pg_backup_utils.h"

#ifndef WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include "signal.h"
#include <unistd.h>
#include <fcntl.h>
#endif

/* Mnemonic macros for indexing the fd array returned by pipe(2) */
#define PIPE_READ							0
#define PIPE_WRITE							1

#ifdef WIN32

/*
 * Structure to hold info passed by _beginthreadex() to the function it calls
 * via its single allowed argument.
 */
typedef struct
{
	ArchiveHandle *AH;			/* master database connection */
	ParallelSlot *slot;			/* this worker's parallel slot */
} WorkerInfo;

/* Windows implementation of pipe access */
static int	pgpipe(int handles[2]);
static int	piperead(int s, char *buf, int len);
#define pipewrite(a,b,c)	send(a,b,c,0)

#else							/* !WIN32 */

/* Non-Windows implementation of pipe access */
#define pgpipe(a)			pipe(a)
#define piperead(a,b,c)		read(a,b,c)
#define pipewrite(a,b,c)	write(a,b,c)

#endif   /* WIN32 */

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
 * On Unix, myAH is the master DB connection in the master process, and the
 * worker's own connection in worker processes.  On Windows, we have only one
 * instance of signal_info, so myAH is the master connection and the worker
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
#endif   /* WIN32 */

static const char *modulename = gettext_noop("parallel archiver");

/* Local function prototypes */
static ParallelSlot *GetMyPSlot(ParallelState *pstate);
static void archive_close_connection(int code, void *arg);
static void ShutdownWorkersHard(ParallelState *pstate);
static void WaitForTerminatingWorkers(ParallelState *pstate);
static void setup_cancel_handler(void);
static void set_cancel_pstate(ParallelState *pstate);
static void set_cancel_slot_archive(ParallelSlot *slot, ArchiveHandle *AH);
static void RunWorker(ArchiveHandle *AH, ParallelSlot *slot);
static bool HasEveryWorkerTerminated(ParallelState *pstate);
static void lockTableForWorker(ArchiveHandle *AH, TocEntry *te);
static void WaitForCommands(ArchiveHandle *AH, int pipefd[2]);
static char *getMessageFromMaster(int pipefd[2]);
static void sendMessageToMaster(int pipefd[2], const char *str);
static int	select_loop(int maxFd, fd_set *workerset);
static char *getMessageFromWorker(ParallelState *pstate,
					 bool do_wait, int *worker);
static void sendMessageToWorker(ParallelState *pstate,
					int worker, const char *str);
static char *readMessageFromPipe(int fd);

#define messageStartsWith(msg, prefix) \
	(strncmp(msg, prefix, strlen(prefix)) == 0)
#define messageEquals(msg, pattern) \
	(strcmp(msg, pattern) == 0)


/*
 * Shutdown callback to clean up socket access
 */
#ifdef WIN32
static void
shutdown_parallel_dump_utils(int code, void *unused)
{
	/* Call the cleanup function only from the main thread */
	if (mainThreadId == GetCurrentThreadId())
		WSACleanup();
}
#endif

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
		{
			fprintf(stderr, _("%s: WSAStartup failed: %d\n"), progname, err);
			exit_nicely(1);
		}
		/* ... and arrange to shut it down at exit */
		on_exit_nicely(shutdown_parallel_dump_utils, NULL);
		parallel_init_done = true;
	}
#endif
}

/*
 * Find the ParallelSlot for the current worker process or thread.
 *
 * Returns NULL if no matching slot is found (this implies we're the master).
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
#endif   /* WIN32 */

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
			 * We're the master.  Forcibly shut down workers, then close our
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
			 * (Without this, if this is a premature exit, the master would
			 * fail to detect it because there would be no EOF condition on
			 * the other end of the pipe.)
			 */
			if (slot->args->AH)
				DisconnectDatabase(&(slot->args->AH->public));

#ifdef WIN32
			closesocket(slot->pipeRevRead);
			closesocket(slot->pipeRevWrite);
#endif
		}
	}
	else
	{
		/* Non-parallel operation: just kill the master DB connection */
		if (si->AHX)
			DisconnectDatabase(si->AHX);
	}
}

/*
 * Forcibly shut down any remaining workers, waiting for them to finish.
 *
 * Note that we don't expect to come here during normal exit (the workers
 * should be long gone, and the ParallelState too).  We're only here in an
 * exit_horribly() situation, so intervening to cancel active commands is
 * appropriate.
 */
static void
ShutdownWorkersHard(ParallelState *pstate)
{
	int			i;

	/*
	 * Close our write end of the sockets so that any workers waiting for
	 * commands know they can exit.
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
		ArchiveHandle *AH = pstate->parallelSlot[i].args->AH;
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
			if (pstate->parallelSlot[j].workerStatus != WRKR_TERMINATED)
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
#endif   /* WIN32 */

		/* On all platforms, update workerStatus as well */
		Assert(j < pstate->numWorkers);
		slot->workerStatus = WRKR_TERMINATED;
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
 * Furthermore, the master process should attempt to forward its signal to
 * each child.  In simple manual use of pg_dump/pg_restore, forwarding isn't
 * needed because typing control-C at the console would deliver SIGINT to
 * every member of the terminal process group --- but in other scenarios it
 * might be that only the master gets signaled.
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
	 * disable the signals that setup_cancel_handler enabled.
	 */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, SIG_IGN);

	/*
	 * If we're in the master, forward signal to all workers.  (It seems best
	 * to do this before PQcancel; killing the master transaction will result
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
	 * When in parallel operation, only the master process should do this.
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

	/* And die. */
	exit(1);
}

/*
 * Enable cancel interrupt handler, if not already done.
 */
static void
setup_cancel_handler(void)
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
		 * which would clutter the user's screen.  We needn't stop the master
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
				ArchiveHandle *AH = slot->args->AH;
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
		 * Send QueryCancel to master connection, if enabled.  Ignore errors,
		 * there's not much we can do about them anyway.
		 */
		if (signal_info.myAH != NULL && signal_info.myAH->connCancel != NULL)
			(void) PQcancel(signal_info.myAH->connCancel,
							errbuf, sizeof(errbuf));

		LeaveCriticalSection(&signal_info_lock);

		/*
		 * Report we're quitting, using nothing more complicated than
		 * write(2).  (We might be able to get away with using write_msg()
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
setup_cancel_handler(void)
{
	if (!signal_info.handler_set)
	{
		signal_info.handler_set = true;

		InitializeCriticalSection(&signal_info_lock);

		SetConsoleCtrlHandler(consoleHandler, TRUE);
	}
}

#endif   /* WIN32 */


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
	setup_cancel_handler();

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

	slot->args->AH = AH;

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
#endif   /* WIN32 */

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
	const size_t slotSize = AH->public.numWorkers * sizeof(ParallelSlot);

	Assert(AH->public.numWorkers > 0);

	pstate = (ParallelState *) pg_malloc(sizeof(ParallelState));

	pstate->numWorkers = AH->public.numWorkers;
	pstate->parallelSlot = NULL;

	if (AH->public.numWorkers == 1)
		return pstate;

	pstate->parallelSlot = (ParallelSlot *) pg_malloc(slotSize);
	memset((void *) pstate->parallelSlot, 0, slotSize);

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
	 * Temporarily disable query cancellation on the master connection.  This
	 * ensures that child processes won't inherit valid AH->connCancel
	 * settings and thus won't try to issue cancels against the master's
	 * connection.  No harm is done if we fail while it's disabled, because
	 * the master connection is idle at this point anyway.
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
			exit_horribly(modulename,
						  "could not create communication channels: %s\n",
						  strerror(errno));

		slot->workerStatus = WRKR_IDLE;
		slot->args = (ParallelArgs *) pg_malloc(sizeof(ParallelArgs));
		slot->args->AH = NULL;
		slot->args->te = NULL;

		/* master's ends of the pipes */
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

			/* close read end of Worker -> Master */
			closesocket(pipeWM[PIPE_READ]);
			/* close write end of Master -> Worker */
			closesocket(pipeMW[PIPE_WRITE]);

			/*
			 * Close all inherited fds for communication of the master with
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
			exit_horribly(modulename,
						  "could not create worker process: %s\n",
						  strerror(errno));
		}

		/* In Master after successful fork */
		slot->pid = pid;

		/* close read end of Master -> Worker */
		closesocket(pipeMW[PIPE_READ]);
		/* close write end of Worker -> Master */
		closesocket(pipeWM[PIPE_WRITE]);
#endif   /* WIN32 */
	}

	/*
	 * Having forked off the workers, disable SIGPIPE so that master isn't
	 * killed if it tries to send a command to a dead worker.  We don't want
	 * the workers to inherit this setting, though.
	 */
#ifndef WIN32
	pqsignal(SIGPIPE, SIG_IGN);
#endif

	/*
	 * Re-establish query cancellation on the master connection.
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
	free(pstate->parallelSlot);
	free(pstate);
}

/*
 * Dispatch a job to some free worker (caller must ensure there is one!)
 *
 * te is the TocEntry to be processed, act is the action to be taken on it.
 */
void
DispatchJobForTocEntry(ArchiveHandle *AH, ParallelState *pstate, TocEntry *te,
					   T_Action act)
{
	int			worker;
	char	   *arg;

	/* our caller makes sure that at least one worker is idle */
	worker = GetIdleWorker(pstate);
	Assert(worker != NO_SLOT);

	/* Construct and send command string */
	arg = (AH->MasterStartParallelItemPtr) (AH, te, act);

	sendMessageToWorker(pstate, worker, arg);

	/* XXX aren't we leaking string here? (no, because it's static. Ick.) */

	/* Remember worker is busy, and which TocEntry it's working on */
	pstate->parallelSlot[worker].workerStatus = WRKR_WORKING;
	pstate->parallelSlot[worker].args->te = te;
}

/*
 * Find an idle worker and return its slot number.
 * Return NO_SLOT if none are idle.
 */
int
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
 * Return true iff every worker is in the WRKR_TERMINATED state.
 */
static bool
HasEveryWorkerTerminated(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus != WRKR_TERMINATED)
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
 * The master process is already holding an ACCESS SHARE lock.  Ordinarily
 * it's no problem for a worker to get one too, but if anything else besides
 * pg_dump is running, there's a possible deadlock:
 *
 * 1) Master dumps the schema and locks all tables in ACCESS SHARE mode.
 * 2) Another process requests an ACCESS EXCLUSIVE lock (which is not granted
 *	  because the master holds a conflicting ACCESS SHARE lock).
 * 3) A worker process also requests an ACCESS SHARE lock to read the table.
 *	  The worker is enqueued behind the ACCESS EXCLUSIVE lock request.
 * 4) Now we have a deadlock, since the master is effectively waiting for
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

	qualId = fmtQualifiedId(AH->public.remoteVersion, te->namespace, te->tag);

	appendPQExpBuffer(query, "LOCK TABLE %s IN ACCESS SHARE MODE NOWAIT",
					  qualId);

	res = PQexec(AH->connection, query->data);

	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		exit_horribly(modulename,
					  "could not obtain lock on relation \"%s\"\n"
		"This usually means that someone requested an ACCESS EXCLUSIVE lock "
			  "on the table after the pg_dump parent process had gotten the "
					  "initial ACCESS SHARE lock on the table.\n", qualId);

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * WaitForCommands: main routine for a worker process.
 *
 * Read and execute commands from the master until we see EOF on the pipe.
 */
static void
WaitForCommands(ArchiveHandle *AH, int pipefd[2])
{
	char	   *command;
	DumpId		dumpId;
	int			nBytes;
	char	   *str;
	TocEntry   *te;

	for (;;)
	{
		if (!(command = getMessageFromMaster(pipefd)))
		{
			/* EOF, so done */
			return;
		}

		if (messageStartsWith(command, "DUMP "))
		{
			/* Decode the command */
			sscanf(command + strlen("DUMP "), "%d%n", &dumpId, &nBytes);
			Assert(nBytes == strlen(command) - strlen("DUMP "));
			te = getTocEntryByDumpId(AH, dumpId);
			Assert(te != NULL);

			/* Acquire lock on this table within the worker's session */
			lockTableForWorker(AH, te);

			/* Perform the dump command */
			str = (AH->WorkerJobDumpPtr) (AH, te);

			/* Return status to master */
			sendMessageToMaster(pipefd, str);

			/* we are responsible for freeing the status string */
			free(str);
		}
		else if (messageStartsWith(command, "RESTORE "))
		{
			/* Decode the command */
			sscanf(command + strlen("RESTORE "), "%d%n", &dumpId, &nBytes);
			Assert(nBytes == strlen(command) - strlen("RESTORE "));
			te = getTocEntryByDumpId(AH, dumpId);
			Assert(te != NULL);

			/* Perform the restore command */
			str = (AH->WorkerJobRestorePtr) (AH, te);

			/* Return status to master */
			sendMessageToMaster(pipefd, str);

			/* we are responsible for freeing the status string */
			free(str);
		}
		else
			exit_horribly(modulename,
					   "unrecognized command received from master: \"%s\"\n",
						  command);

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
 * When we get a status message, we let MasterEndParallelItemPtr process it,
 * then save the resulting status code and switch the worker's state to
 * WRKR_FINISHED.  Later, caller must call ReapWorkerStatus() to verify
 * that the status was "OK" and push the worker back to IDLE state.
 *
 * XXX Rube Goldberg would be proud of this API, but no one else should be.
 *
 * XXX is it worth checking for more than one status message per call?
 * It seems somewhat unlikely that multiple workers would finish at exactly
 * the same time.
 */
void
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
			exit_horribly(modulename, "a worker process died unexpectedly\n");
		return;
	}

	/* Process it and update our idea of the worker's status */
	if (messageStartsWith(msg, "OK "))
	{
		TocEntry   *te = pstate->parallelSlot[worker].args->te;
		char	   *statusString;

		if (messageStartsWith(msg, "OK RESTORE "))
		{
			statusString = msg + strlen("OK RESTORE ");
			pstate->parallelSlot[worker].status =
				(AH->MasterEndParallelItemPtr)
				(AH, te, statusString, ACT_RESTORE);
		}
		else if (messageStartsWith(msg, "OK DUMP "))
		{
			statusString = msg + strlen("OK DUMP ");
			pstate->parallelSlot[worker].status =
				(AH->MasterEndParallelItemPtr)
				(AH, te, statusString, ACT_DUMP);
		}
		else
			exit_horribly(modulename,
						  "invalid message received from worker: \"%s\"\n",
						  msg);
		pstate->parallelSlot[worker].workerStatus = WRKR_FINISHED;
	}
	else
		exit_horribly(modulename,
					  "invalid message received from worker: \"%s\"\n",
					  msg);

	/* Free the string returned from getMessageFromWorker */
	free(msg);
}

/*
 * Check to see if any worker is in WRKR_FINISHED state.  If so,
 * return its command status code into *status, reset it to IDLE state,
 * and return its slot number.  Otherwise return NO_SLOT.
 *
 * This function is executed in the master process.
 */
int
ReapWorkerStatus(ParallelState *pstate, int *status)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus == WRKR_FINISHED)
		{
			*status = pstate->parallelSlot[i].status;
			pstate->parallelSlot[i].status = 0;
			pstate->parallelSlot[i].workerStatus = WRKR_IDLE;
			return i;
		}
	}
	return NO_SLOT;
}

/*
 * Wait, if necessary, until we have at least one idle worker.
 * Reap worker status as necessary to move FINISHED workers to IDLE state.
 *
 * We assume that no extra processing is required when reaping a finished
 * command, except for checking that the status was OK (zero).
 * Caution: that assumption means that this function can only be used in
 * parallel dump, not parallel restore, because the latter has a more
 * complex set of rules about handling status.
 *
 * This function is executed in the master process.
 */
void
EnsureIdleWorker(ArchiveHandle *AH, ParallelState *pstate)
{
	int			ret_worker;
	int			work_status;

	for (;;)
	{
		int			nTerm = 0;

		while ((ret_worker = ReapWorkerStatus(pstate, &work_status)) != NO_SLOT)
		{
			if (work_status != 0)
				exit_horribly(modulename, "error processing a parallel work item\n");

			nTerm++;
		}

		/*
		 * We need to make sure that we have an idle worker before dispatching
		 * the next item. If nTerm > 0 we already have that (quick check).
		 */
		if (nTerm > 0)
			return;

		/* explicit check for an idle worker */
		if (GetIdleWorker(pstate) != NO_SLOT)
			return;

		/*
		 * If we have no idle worker, read the result of one or more workers
		 * and loop the loop to call ReapWorkerStatus() on them
		 */
		ListenToWorkers(AH, pstate, true);
	}
}

/*
 * Wait for all workers to be idle.
 * Reap worker status as necessary to move FINISHED workers to IDLE state.
 *
 * We assume that no extra processing is required when reaping a finished
 * command, except for checking that the status was OK (zero).
 * Caution: that assumption means that this function can only be used in
 * parallel dump, not parallel restore, because the latter has a more
 * complex set of rules about handling status.
 *
 * This function is executed in the master process.
 */
void
EnsureWorkersFinished(ArchiveHandle *AH, ParallelState *pstate)
{
	int			work_status;

	if (!pstate || pstate->numWorkers == 1)
		return;

	/* Waiting for the remaining worker processes to finish */
	while (!IsEveryWorkerIdle(pstate))
	{
		if (ReapWorkerStatus(pstate, &work_status) == NO_SLOT)
			ListenToWorkers(AH, pstate, true);
		else if (work_status != 0)
			exit_horribly(modulename,
						  "error processing a parallel work item\n");
	}
}

/*
 * Read one command message from the master, blocking if necessary
 * until one is available, and return it as a malloc'd string.
 * On EOF, return NULL.
 *
 * This function is executed in worker processes.
 */
static char *
getMessageFromMaster(int pipefd[2])
{
	return readMessageFromPipe(pipefd[PIPE_READ]);
}

/*
 * Send a status message to the master.
 *
 * This function is executed in worker processes.
 */
static void
sendMessageToMaster(int pipefd[2], const char *str)
{
	int			len = strlen(str) + 1;

	if (pipewrite(pipefd[PIPE_WRITE], str, len) != len)
		exit_horribly(modulename,
					  "could not write to the communication channel: %s\n",
					  strerror(errno));
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
 * This function is executed in the master process.
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
		if (pstate->parallelSlot[i].workerStatus == WRKR_TERMINATED)
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
		exit_horribly(modulename, "select() failed: %s\n", strerror(errno));

	for (i = 0; i < pstate->numWorkers; i++)
	{
		char	   *msg;

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
 * This function is executed in the master process.
 */
static void
sendMessageToWorker(ParallelState *pstate, int worker, const char *str)
{
	int			len = strlen(str) + 1;

	if (pipewrite(pstate->parallelSlot[worker].pipeWrite, str, len) != len)
	{
		exit_horribly(modulename,
					  "could not write to the communication channel: %s\n",
					  strerror(errno));
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
	 * neither master nor workers send more than one message without waiting
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
		write_msg(modulename, "pgpipe: could not create socket: error code %d\n",
				  WSAGetLastError());
		return -1;
	}

	memset((void *) &serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(0);
	serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (SOCKADDR *) &serv_addr, len) == SOCKET_ERROR)
	{
		write_msg(modulename, "pgpipe: could not bind: error code %d\n",
				  WSAGetLastError());
		closesocket(s);
		return -1;
	}
	if (listen(s, 1) == SOCKET_ERROR)
	{
		write_msg(modulename, "pgpipe: could not listen: error code %d\n",
				  WSAGetLastError());
		closesocket(s);
		return -1;
	}
	if (getsockname(s, (SOCKADDR *) &serv_addr, &len) == SOCKET_ERROR)
	{
		write_msg(modulename, "pgpipe: getsockname() failed: error code %d\n",
				  WSAGetLastError());
		closesocket(s);
		return -1;
	}

	/*
	 * setup pipe handles
	 */
	if ((tmp_sock = socket(AF_INET, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
	{
		write_msg(modulename, "pgpipe: could not create second socket: error code %d\n",
				  WSAGetLastError());
		closesocket(s);
		return -1;
	}
	handles[1] = (int) tmp_sock;

	if (connect(handles[1], (SOCKADDR *) &serv_addr, len) == SOCKET_ERROR)
	{
		write_msg(modulename, "pgpipe: could not connect socket: error code %d\n",
				  WSAGetLastError());
		closesocket(handles[1]);
		handles[1] = -1;
		closesocket(s);
		return -1;
	}
	if ((tmp_sock = accept(s, (SOCKADDR *) &serv_addr, &len)) == PGINVALID_SOCKET)
	{
		write_msg(modulename, "pgpipe: could not accept connection: error code %d\n",
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

/*
 * Windows implementation of reading from a pipe.
 */
static int
piperead(int s, char *buf, int len)
{
	int			ret = recv(s, buf, len, 0);

	if (ret < 0 && WSAGetLastError() == WSAECONNRESET)
	{
		/* EOF on the pipe! */
		ret = 0;
	}
	return ret;
}

#endif   /* WIN32 */
