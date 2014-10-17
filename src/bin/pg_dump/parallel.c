/*-------------------------------------------------------------------------
 *
 * parallel.c
 *
 *	Parallel support for the pg_dump archiver
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/parallel.c
 *
 *-------------------------------------------------------------------------
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

#define PIPE_READ							0
#define PIPE_WRITE							1

/* file-scope variables */
#ifdef WIN32
static unsigned int tMasterThreadId = 0;
static HANDLE termEvent = INVALID_HANDLE_VALUE;
static int	pgpipe(int handles[2]);
static int	piperead(int s, char *buf, int len);

/*
 * Structure to hold info passed by _beginthreadex() to the function it calls
 * via its single allowed argument.
 */
typedef struct
{
	ArchiveHandle *AH;
	RestoreOptions *ropt;
	DumpOptions *dopt;
	int			worker;
	int			pipeRead;
	int			pipeWrite;
} WorkerInfo;

#define pipewrite(a,b,c)	send(a,b,c,0)
#else
/*
 * aborting is only ever used in the master, the workers are fine with just
 * wantAbort.
 */
static bool aborting = false;
static volatile sig_atomic_t wantAbort = 0;

#define pgpipe(a)			pipe(a)
#define piperead(a,b,c)		read(a,b,c)
#define pipewrite(a,b,c)	write(a,b,c)
#endif

typedef struct ShutdownInformation
{
	ParallelState *pstate;
	Archive    *AHX;
} ShutdownInformation;

static ShutdownInformation shutdown_info;

static const char *modulename = gettext_noop("parallel archiver");

static ParallelSlot *GetMyPSlot(ParallelState *pstate);
static void
parallel_msg_master(ParallelSlot *slot, const char *modulename,
					const char *fmt, va_list ap)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 3, 0)));
static void archive_close_connection(int code, void *arg);
static void ShutdownWorkersHard(ParallelState *pstate);
static void WaitForTerminatingWorkers(ParallelState *pstate);

#ifndef WIN32
static void sigTermHandler(int signum);
#endif
static void SetupWorker(ArchiveHandle *AH, int pipefd[2], int worker,
			DumpOptions *dopt,
			RestoreOptions *ropt);
static bool HasEveryWorkerTerminated(ParallelState *pstate);

static void lockTableNoWait(ArchiveHandle *AH, TocEntry *te);
static void WaitForCommands(ArchiveHandle *AH, DumpOptions *dopt, int pipefd[2]);
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

#ifdef WIN32
static void shutdown_parallel_dump_utils(int code, void *unused);
bool		parallel_init_done = false;
static DWORD tls_index;
DWORD		mainThreadId;
#endif


#ifdef WIN32
static void
shutdown_parallel_dump_utils(int code, void *unused)
{
	/* Call the cleanup function only from the main thread */
	if (mainThreadId == GetCurrentThreadId())
		WSACleanup();
}
#endif

void
init_parallel_dump_utils(void)
{
#ifdef WIN32
	if (!parallel_init_done)
	{
		WSADATA		wsaData;
		int			err;

		tls_index = TlsAlloc();
		mainThreadId = GetCurrentThreadId();
		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err != 0)
		{
			fprintf(stderr, _("%s: WSAStartup failed: %d\n"), progname, err);
			exit_nicely(1);
		}
		on_exit_nicely(shutdown_parallel_dump_utils, NULL);
		parallel_init_done = true;
	}
#endif
}

static ParallelSlot *
GetMyPSlot(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
#ifdef WIN32
		if (pstate->parallelSlot[i].threadId == GetCurrentThreadId())
#else
		if (pstate->parallelSlot[i].pid == getpid())
#endif
			return &(pstate->parallelSlot[i]);

	return NULL;
}

/*
 * Fail and die, with a message to stderr.  Parameters as for write_msg.
 *
 * This is defined in parallel.c, because in parallel mode, things are more
 * complicated. If the worker process does exit_horribly(), we forward its
 * last words to the master process. The master process then does
 * exit_horribly() with this error message itself and prints it normally.
 * After printing the message, exit_horribly() on the master will shut down
 * the remaining worker processes.
 */
void
exit_horribly(const char *modulename, const char *fmt,...)
{
	va_list		ap;
	ParallelState *pstate = shutdown_info.pstate;
	ParallelSlot *slot;

	va_start(ap, fmt);

	if (pstate == NULL)
	{
		/* Not in parallel mode, just write to stderr */
		vwrite_msg(modulename, fmt, ap);
	}
	else
	{
		slot = GetMyPSlot(pstate);

		if (!slot)
			/* We're the parent, just write the message out */
			vwrite_msg(modulename, fmt, ap);
		else
			/* If we're a worker process, send the msg to the master process */
			parallel_msg_master(slot, modulename, fmt, ap);
	}

	va_end(ap);

	exit_nicely(1);
}

/* Sends the error message from the worker to the master process */
static void
parallel_msg_master(ParallelSlot *slot, const char *modulename,
					const char *fmt, va_list ap)
{
	char		buf[512];
	int			pipefd[2];

	pipefd[PIPE_READ] = slot->pipeRevRead;
	pipefd[PIPE_WRITE] = slot->pipeRevWrite;

	strcpy(buf, "ERROR ");
	vsnprintf(buf + strlen("ERROR "),
			  sizeof(buf) - strlen("ERROR "), fmt, ap);

	sendMessageToMaster(pipefd, buf);
}

/*
 * A thread-local version of getLocalPQExpBuffer().
 *
 * Non-reentrant but reduces memory leakage. (On Windows the memory leakage
 * will be one buffer per thread, which is at least better than one per call).
 */
static PQExpBuffer
getThreadLocalPQExpBuffer(void)
{
	/*
	 * The Tls code goes awry if we use a static var, so we provide for both
	 * static and auto, and omit any use of the static var when using Tls.
	 */
	static PQExpBuffer s_id_return = NULL;
	PQExpBuffer id_return;

#ifdef WIN32
	if (parallel_init_done)
		id_return = (PQExpBuffer) TlsGetValue(tls_index);		/* 0 when not set */
	else
		id_return = s_id_return;
#else
	id_return = s_id_return;
#endif

	if (id_return)				/* first time through? */
	{
		/* same buffer, just wipe contents */
		resetPQExpBuffer(id_return);
	}
	else
	{
		/* new buffer */
		id_return = createPQExpBuffer();
#ifdef WIN32
		if (parallel_init_done)
			TlsSetValue(tls_index, id_return);
		else
			s_id_return = id_return;
#else
		s_id_return = id_return;
#endif

	}

	return id_return;
}

/*
 * pg_dump and pg_restore register the Archive pointer for the exit handler
 * (called from exit_horribly). This function mainly exists so that we can
 * keep shutdown_info in file scope only.
 */
void
on_exit_close_archive(Archive *AHX)
{
	shutdown_info.AHX = AHX;
	on_exit_nicely(archive_close_connection, &shutdown_info);
}

/*
 * This function can close archives in both the parallel and non-parallel
 * case.
 */
static void
archive_close_connection(int code, void *arg)
{
	ShutdownInformation *si = (ShutdownInformation *) arg;

	if (si->pstate)
	{
		ParallelSlot *slot = GetMyPSlot(si->pstate);

		if (!slot)
		{
			/*
			 * We're the master: We have already printed out the message
			 * passed to exit_horribly() either from the master itself or from
			 * a worker process. Now we need to close our own database
			 * connection (only open during parallel dump but not restore) and
			 * shut down the remaining workers.
			 */
			DisconnectDatabase(si->AHX);
#ifndef WIN32

			/*
			 * Setting aborting to true switches to best-effort-mode
			 * (send/receive but ignore errors) in communicating with our
			 * workers.
			 */
			aborting = true;
#endif
			ShutdownWorkersHard(si->pstate);
		}
		else if (slot->args->AH)
			DisconnectDatabase(&(slot->args->AH->public));
	}
	else if (si->AHX)
		DisconnectDatabase(si->AHX);
}

/*
 * If we have one worker that terminates for some reason, we'd like the other
 * threads to terminate as well (and not finish with their 70 GB table dump
 * first...). Now in UNIX we can just kill these processes, and let the signal
 * handler set wantAbort to 1. In Windows we set a termEvent and this serves
 * as the signal for everyone to terminate.
 */
void
checkAborting(ArchiveHandle *AH)
{
#ifdef WIN32
	if (WaitForSingleObject(termEvent, 0) == WAIT_OBJECT_0)
#else
	if (wantAbort)
#endif
		exit_horribly(modulename, "worker is terminating\n");
}

/*
 * Shut down any remaining workers, this has an implicit do_wait == true.
 *
 * The fastest way we can make the workers terminate gracefully is when
 * they are listening for new commands and we just tell them to terminate.
 */
static void
ShutdownWorkersHard(ParallelState *pstate)
{
#ifndef WIN32
	int			i;

	signal(SIGPIPE, SIG_IGN);

	/*
	 * Close our write end of the sockets so that the workers know they can
	 * exit.
	 */
	for (i = 0; i < pstate->numWorkers; i++)
		closesocket(pstate->parallelSlot[i].pipeWrite);

	for (i = 0; i < pstate->numWorkers; i++)
		kill(pstate->parallelSlot[i].pid, SIGTERM);
#else
	/* The workers monitor this event via checkAborting(). */
	SetEvent(termEvent);
#endif

	WaitForTerminatingWorkers(pstate);
}

/*
 * Wait for the termination of the processes using the OS-specific method.
 */
static void
WaitForTerminatingWorkers(ParallelState *pstate)
{
	while (!HasEveryWorkerTerminated(pstate))
	{
		ParallelSlot *slot = NULL;
		int			j;

#ifndef WIN32
		int			status;
		pid_t		pid = wait(&status);

		for (j = 0; j < pstate->numWorkers; j++)
			if (pstate->parallelSlot[j].pid == pid)
				slot = &(pstate->parallelSlot[j]);
#else
		uintptr_t	hThread;
		DWORD		ret;
		uintptr_t  *lpHandles = pg_malloc(sizeof(HANDLE) * pstate->numWorkers);
		int			nrun = 0;

		for (j = 0; j < pstate->numWorkers; j++)
			if (pstate->parallelSlot[j].workerStatus != WRKR_TERMINATED)
			{
				lpHandles[nrun] = pstate->parallelSlot[j].hThread;
				nrun++;
			}
		ret = WaitForMultipleObjects(nrun, (HANDLE *) lpHandles, false, INFINITE);
		Assert(ret != WAIT_FAILED);
		hThread = lpHandles[ret - WAIT_OBJECT_0];

		for (j = 0; j < pstate->numWorkers; j++)
			if (pstate->parallelSlot[j].hThread == hThread)
				slot = &(pstate->parallelSlot[j]);

		free(lpHandles);
#endif
		Assert(slot);

		slot->workerStatus = WRKR_TERMINATED;
	}
	Assert(HasEveryWorkerTerminated(pstate));
}

#ifndef WIN32
/* Signal handling (UNIX only) */
static void
sigTermHandler(int signum)
{
	wantAbort = 1;
}
#endif

/*
 * This function is called by both UNIX and Windows variants to set up a
 * worker process.
 */
static void
SetupWorker(ArchiveHandle *AH, int pipefd[2], int worker,
			DumpOptions *dopt,
			RestoreOptions *ropt)
{
	/*
	 * Call the setup worker function that's defined in the ArchiveHandle.
	 *
	 * We get the raw connection only for the reason that we can close it
	 * properly when we shut down. This happens only that way when it is
	 * brought down because of an error.
	 */
	(AH->SetupWorkerPtr) ((Archive *) AH, dopt, ropt);

	Assert(AH->connection != NULL);

	WaitForCommands(AH, dopt, pipefd);

	closesocket(pipefd[PIPE_READ]);
	closesocket(pipefd[PIPE_WRITE]);
}

#ifdef WIN32
static unsigned __stdcall
init_spawned_worker_win32(WorkerInfo *wi)
{
	ArchiveHandle *AH;
	int			pipefd[2] = {wi->pipeRead, wi->pipeWrite};
	int			worker = wi->worker;
	DumpOptions *dopt = wi->dopt;
	RestoreOptions *ropt = wi->ropt;

	AH = CloneArchive(wi->AH);

	free(wi);
	SetupWorker(AH, pipefd, worker, dopt, ropt);

	DeCloneArchive(AH);
	_endthreadex(0);
	return 0;
}
#endif

/*
 * This function starts the parallel dump or restore by spawning off the
 * worker processes in both Unix and Windows. For Windows, it creates a number
 * of threads while it does a fork() on Unix.
 */
ParallelState *
ParallelBackupStart(ArchiveHandle *AH, DumpOptions *dopt, RestoreOptions *ropt)
{
	ParallelState *pstate;
	int			i;
	const size_t slotSize = AH->public.numWorkers * sizeof(ParallelSlot);

	Assert(AH->public.numWorkers > 0);

	/* Ensure stdio state is quiesced before forking */
	fflush(NULL);

	pstate = (ParallelState *) pg_malloc(sizeof(ParallelState));

	pstate->numWorkers = AH->public.numWorkers;
	pstate->parallelSlot = NULL;

	if (AH->public.numWorkers == 1)
		return pstate;

	pstate->parallelSlot = (ParallelSlot *) pg_malloc(slotSize);
	memset((void *) pstate->parallelSlot, 0, slotSize);

	/*
	 * Set the pstate in the shutdown_info. The exit handler uses pstate if
	 * set and falls back to AHX otherwise.
	 */
	shutdown_info.pstate = pstate;
	getLocalPQExpBuffer = getThreadLocalPQExpBuffer;

#ifdef WIN32
	tMasterThreadId = GetCurrentThreadId();
	termEvent = CreateEvent(NULL, true, false, "Terminate");
#else
	signal(SIGTERM, sigTermHandler);
	signal(SIGINT, sigTermHandler);
	signal(SIGQUIT, sigTermHandler);
#endif

	for (i = 0; i < pstate->numWorkers; i++)
	{
#ifdef WIN32
		WorkerInfo *wi;
		uintptr_t	handle;
#else
		pid_t		pid;
#endif
		int			pipeMW[2],
					pipeWM[2];

		if (pgpipe(pipeMW) < 0 || pgpipe(pipeWM) < 0)
			exit_horribly(modulename,
						  "could not create communication channels: %s\n",
						  strerror(errno));

		pstate->parallelSlot[i].workerStatus = WRKR_IDLE;
		pstate->parallelSlot[i].args = (ParallelArgs *) pg_malloc(sizeof(ParallelArgs));
		pstate->parallelSlot[i].args->AH = NULL;
		pstate->parallelSlot[i].args->te = NULL;
#ifdef WIN32
		/* Allocate a new structure for every worker */
		wi = (WorkerInfo *) pg_malloc(sizeof(WorkerInfo));

		wi->ropt = ropt;
		wi->dopt = dopt;
		wi->worker = i;
		wi->AH = AH;
		wi->pipeRead = pstate->parallelSlot[i].pipeRevRead = pipeMW[PIPE_READ];
		wi->pipeWrite = pstate->parallelSlot[i].pipeRevWrite = pipeWM[PIPE_WRITE];

		handle = _beginthreadex(NULL, 0, (void *) &init_spawned_worker_win32,
								wi, 0, &(pstate->parallelSlot[i].threadId));
		pstate->parallelSlot[i].hThread = handle;
#else
		pid = fork();
		if (pid == 0)
		{
			/* we are the worker */
			int			j;
			int			pipefd[2];

			pipefd[0] = pipeMW[PIPE_READ];
			pipefd[1] = pipeWM[PIPE_WRITE];

			/*
			 * Store the fds for the reverse communication in pstate. Actually
			 * we only use this in case of an error and don't use pstate
			 * otherwise in the worker process. On Windows we write to the
			 * global pstate, in Unix we write to our process-local copy but
			 * that's also where we'd retrieve this information back from.
			 */
			pstate->parallelSlot[i].pipeRevRead = pipefd[PIPE_READ];
			pstate->parallelSlot[i].pipeRevWrite = pipefd[PIPE_WRITE];
			pstate->parallelSlot[i].pid = getpid();

			/*
			 * Call CloneArchive on Unix as well even though technically we
			 * don't need to because fork() gives us a copy in our own address
			 * space already. But CloneArchive resets the state information
			 * and also clones the database connection (for parallel dump)
			 * which both seem kinda helpful.
			 */
			pstate->parallelSlot[i].args->AH = CloneArchive(AH);

			/* close read end of Worker -> Master */
			closesocket(pipeWM[PIPE_READ]);
			/* close write end of Master -> Worker */
			closesocket(pipeMW[PIPE_WRITE]);

			/*
			 * Close all inherited fds for communication of the master with
			 * the other workers.
			 */
			for (j = 0; j < i; j++)
			{
				closesocket(pstate->parallelSlot[j].pipeRead);
				closesocket(pstate->parallelSlot[j].pipeWrite);
			}

			SetupWorker(pstate->parallelSlot[i].args->AH, pipefd, i, dopt, ropt);

			exit(0);
		}
		else if (pid < 0)
			/* fork failed */
			exit_horribly(modulename,
						  "could not create worker process: %s\n",
						  strerror(errno));

		/* we are the Master, pid > 0 here */
		Assert(pid > 0);

		/* close read end of Master -> Worker */
		closesocket(pipeMW[PIPE_READ]);
		/* close write end of Worker -> Master */
		closesocket(pipeWM[PIPE_WRITE]);

		pstate->parallelSlot[i].pid = pid;
#endif

		pstate->parallelSlot[i].pipeRead = pipeWM[PIPE_READ];
		pstate->parallelSlot[i].pipeWrite = pipeMW[PIPE_WRITE];
	}

	return pstate;
}

/*
 * Tell all of our workers to terminate.
 *
 * Pretty straightforward routine, first we tell everyone to terminate, then
 * we listen to the workers' replies and finally close the sockets that we
 * have used for communication.
 */
void
ParallelBackupEnd(ArchiveHandle *AH, ParallelState *pstate)
{
	int			i;

	if (pstate->numWorkers == 1)
		return;

	Assert(IsEveryWorkerIdle(pstate));

	/* close the sockets so that the workers know they can exit */
	for (i = 0; i < pstate->numWorkers; i++)
	{
		closesocket(pstate->parallelSlot[i].pipeRead);
		closesocket(pstate->parallelSlot[i].pipeWrite);
	}
	WaitForTerminatingWorkers(pstate);

	/*
	 * Remove the pstate again, so the exit handler in the parent will now
	 * again fall back to closing AH->connection (if connected).
	 */
	shutdown_info.pstate = NULL;

	free(pstate->parallelSlot);
	free(pstate);
}


/*
 * The sequence is the following (for dump, similar for restore):
 *
 * The master process starts the parallel backup in ParllelBackupStart, this
 * forks the worker processes which enter WaitForCommand().
 *
 * The master process dispatches an individual work item to one of the worker
 * processes in DispatchJobForTocEntry(). It calls
 * AH->MasterStartParallelItemPtr, a routine of the output format. This
 * function's arguments are the parents archive handle AH (containing the full
 * catalog information), the TocEntry that the worker should work on and a
 * T_Action act indicating whether this is a backup or a restore item.  The
 * function then converts the TocEntry assignment into a string that is then
 * sent over to the worker process. In the simplest case that would be
 * something like "DUMP 1234", with 1234 being the TocEntry id.
 *
 * The worker receives the message in the routine pointed to by
 * WorkerJobDumpPtr or WorkerJobRestorePtr. These are also pointers to
 * corresponding routines of the respective output format, e.g.
 * _WorkerJobDumpDirectory().
 *
 * Remember that we have forked off the workers only after we have read in the
 * catalog. That's why our worker processes can also access the catalog
 * information. Now they re-translate the textual representation to a TocEntry
 * on their side and do the required action (restore or dump).
 *
 * The result is again a textual string that is sent back to the master and is
 * interpreted by AH->MasterEndParallelItemPtr. This function can update state
 * or catalog information on the master's side, depending on the reply from
 * the worker process. In the end it returns status which is 0 for successful
 * execution.
 *
 * ---------------------------------------------------------------------
 * Master									Worker
 *
 *											enters WaitForCommands()
 * DispatchJobForTocEntry(...te...)
 *
 * [ Worker is IDLE ]
 *
 * arg = (MasterStartParallelItemPtr)()
 * send: DUMP arg
 *											receive: DUMP arg
 *											str = (WorkerJobDumpPtr)(arg)
 * [ Worker is WORKING ]					... gets te from arg ...
 *											... dump te ...
 *											send: OK DUMP info
 *
 * In ListenToWorkers():
 *
 * [ Worker is FINISHED ]
 * receive: OK DUMP info
 * status = (MasterEndParallelItemPtr)(info)
 *
 * In ReapWorkerStatus(&ptr):
 * *ptr = status;
 * [ Worker is IDLE ]
 * ---------------------------------------------------------------------
 */
void
DispatchJobForTocEntry(ArchiveHandle *AH, ParallelState *pstate, TocEntry *te,
					   T_Action act)
{
	int			worker;
	char	   *arg;

	/* our caller makes sure that at least one worker is idle */
	Assert(GetIdleWorker(pstate) != NO_SLOT);
	worker = GetIdleWorker(pstate);
	Assert(worker != NO_SLOT);

	arg = (AH->MasterStartParallelItemPtr) (AH, te, act);

	sendMessageToWorker(pstate, worker, arg);

	pstate->parallelSlot[worker].workerStatus = WRKR_WORKING;
	pstate->parallelSlot[worker].args->te = te;
}

/*
 * Find the first free parallel slot (if any).
 */
int
GetIdleWorker(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
		if (pstate->parallelSlot[i].workerStatus == WRKR_IDLE)
			return i;
	return NO_SLOT;
}

/*
 * Return true iff every worker process is in the WRKR_TERMINATED state.
 */
static bool
HasEveryWorkerTerminated(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
		if (pstate->parallelSlot[i].workerStatus != WRKR_TERMINATED)
			return false;
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
		if (pstate->parallelSlot[i].workerStatus != WRKR_IDLE)
			return false;
	return true;
}

/*
 * ---------------------------------------------------------------------
 * One danger of the parallel backup is a possible deadlock:
 *
 * 1) Master dumps the schema and locks all tables in ACCESS SHARE mode.
 * 2) Another process requests an ACCESS EXCLUSIVE lock (which is not granted
 *	  because the master holds a conflicting ACCESS SHARE lock).
 * 3) The worker process also requests an ACCESS SHARE lock to read the table.
 *	  The worker's not granted that lock but is enqueued behind the ACCESS
 *	  EXCLUSIVE lock request.
 * ---------------------------------------------------------------------
 *
 * Now what we do here is to just request a lock in ACCESS SHARE but with
 * NOWAIT in the worker prior to touching the table. If we don't get the lock,
 * then we know that somebody else has requested an ACCESS EXCLUSIVE lock and
 * are good to just fail the whole backup because we have detected a deadlock.
 */
static void
lockTableNoWait(ArchiveHandle *AH, TocEntry *te)
{
	Archive    *AHX = (Archive *) AH;
	const char *qualId;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;

	Assert(AH->format == archDirectory);
	Assert(strcmp(te->desc, "BLOBS") != 0);

	appendPQExpBuffer(query,
					  "SELECT pg_namespace.nspname,"
					  "       pg_class.relname "
					  "  FROM pg_class "
					"  JOIN pg_namespace on pg_namespace.oid = relnamespace "
					  " WHERE pg_class.oid = %d", te->catalogId.oid);

	res = PQexec(AH->connection, query->data);

	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
		exit_horribly(modulename,
					  "could not get relation name for OID %u: %s\n",
					  te->catalogId.oid, PQerrorMessage(AH->connection));

	resetPQExpBuffer(query);

	qualId = fmtQualifiedId(AHX->remoteVersion,
							PQgetvalue(res, 0, 0),
							PQgetvalue(res, 0, 1));

	appendPQExpBuffer(query, "LOCK TABLE %s IN ACCESS SHARE MODE NOWAIT",
					  qualId);
	PQclear(res);

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
 * That's the main routine for the worker.
 * When it starts up it enters this routine and waits for commands from the
 * master process. After having processed a command it comes back to here to
 * wait for the next command. Finally it will receive a TERMINATE command and
 * exit.
 */
static void
WaitForCommands(ArchiveHandle *AH, DumpOptions *dopt, int pipefd[2])
{
	char	   *command;
	DumpId		dumpId;
	int			nBytes;
	char	   *str = NULL;
	TocEntry   *te;

	for (;;)
	{
		if (!(command = getMessageFromMaster(pipefd)))
		{
			PQfinish(AH->connection);
			AH->connection = NULL;
			return;
		}

		if (messageStartsWith(command, "DUMP "))
		{
			Assert(AH->format == archDirectory);
			sscanf(command + strlen("DUMP "), "%d%n", &dumpId, &nBytes);
			Assert(nBytes == strlen(command) - strlen("DUMP "));

			te = getTocEntryByDumpId(AH, dumpId);
			Assert(te != NULL);

			/*
			 * Lock the table but with NOWAIT. Note that the parent is already
			 * holding a lock. If we cannot acquire another ACCESS SHARE MODE
			 * lock, then somebody else has requested an exclusive lock in the
			 * meantime.  lockTableNoWait dies in this case to prevent a
			 * deadlock.
			 */
			if (strcmp(te->desc, "BLOBS") != 0)
				lockTableNoWait(AH, te);

			/*
			 * The message we return here has been pg_malloc()ed and we are
			 * responsible for free()ing it.
			 */
			str = (AH->WorkerJobDumpPtr) (AH, dopt, te);
			Assert(AH->connection != NULL);
			sendMessageToMaster(pipefd, str);
			free(str);
		}
		else if (messageStartsWith(command, "RESTORE "))
		{
			Assert(AH->format == archDirectory || AH->format == archCustom);
			Assert(AH->connection != NULL);

			sscanf(command + strlen("RESTORE "), "%d%n", &dumpId, &nBytes);
			Assert(nBytes == strlen(command) - strlen("RESTORE "));

			te = getTocEntryByDumpId(AH, dumpId);
			Assert(te != NULL);

			/*
			 * The message we return here has been pg_malloc()ed and we are
			 * responsible for free()ing it.
			 */
			str = (AH->WorkerJobRestorePtr) (AH, te);
			Assert(AH->connection != NULL);
			sendMessageToMaster(pipefd, str);
			free(str);
		}
		else
			exit_horribly(modulename,
					   "unrecognized command on communication channel: %s\n",
						  command);

		/* command was pg_malloc'd and we are responsible for free()ing it. */
		free(command);
	}
}

/*
 * ---------------------------------------------------------------------
 * Note the status change:
 *
 * DispatchJobForTocEntry		WRKR_IDLE -> WRKR_WORKING
 * ListenToWorkers				WRKR_WORKING -> WRKR_FINISHED / WRKR_TERMINATED
 * ReapWorkerStatus				WRKR_FINISHED -> WRKR_IDLE
 * ---------------------------------------------------------------------
 *
 * Just calling ReapWorkerStatus() when all workers are working might or might
 * not give you an idle worker because you need to call ListenToWorkers() in
 * between and only thereafter ReapWorkerStatus(). This is necessary in order
 * to get and deal with the status (=result) of the worker's execution.
 */
void
ListenToWorkers(ArchiveHandle *AH, ParallelState *pstate, bool do_wait)
{
	int			worker;
	char	   *msg;

	msg = getMessageFromWorker(pstate, do_wait, &worker);

	if (!msg)
	{
		if (do_wait)
			exit_horribly(modulename, "a worker process died unexpectedly\n");
		return;
	}

	if (messageStartsWith(msg, "OK "))
	{
		char	   *statusString;
		TocEntry   *te;

		pstate->parallelSlot[worker].workerStatus = WRKR_FINISHED;
		te = pstate->parallelSlot[worker].args->te;
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
						  "invalid message received from worker: %s\n", msg);
	}
	else if (messageStartsWith(msg, "ERROR "))
	{
		Assert(AH->format == archDirectory || AH->format == archCustom);
		pstate->parallelSlot[worker].workerStatus = WRKR_TERMINATED;
		exit_horribly(modulename, "%s", msg + strlen("ERROR "));
	}
	else
		exit_horribly(modulename, "invalid message received from worker: %s\n", msg);

	/* both Unix and Win32 return pg_malloc()ed space, so we free it */
	free(msg);
}

/*
 * This function is executed in the master process.
 *
 * This function is used to get the return value of a terminated worker
 * process. If a process has terminated, its status is stored in *status and
 * the id of the worker is returned.
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
 * This function is executed in the master process.
 *
 * It looks for an idle worker process and only returns if there is one.
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
 * This function is executed in the master process.
 *
 * It waits for all workers to terminate.
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
 * This function is executed in the worker process.
 *
 * It returns the next message on the communication channel, blocking until it
 * becomes available.
 */
static char *
getMessageFromMaster(int pipefd[2])
{
	return readMessageFromPipe(pipefd[PIPE_READ]);
}

/*
 * This function is executed in the worker process.
 *
 * It sends a message to the master on the communication channel.
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
 * A select loop that repeats calling select until a descriptor in the read
 * set becomes readable. On Windows we have to check for the termination event
 * from time to time, on Unix we can just block forever.
 */
static int
select_loop(int maxFd, fd_set *workerset)
{
	int			i;
	fd_set		saveSet = *workerset;

#ifdef WIN32
	/* should always be the master */
	Assert(tMasterThreadId == GetCurrentThreadId());

	for (;;)
	{
		/*
		 * sleep a quarter of a second before checking if we should terminate.
		 */
		struct timeval tv = {0, 250000};

		*workerset = saveSet;
		i = select(maxFd + 1, workerset, NULL, NULL, &tv);

		if (i == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
			continue;
		if (i)
			break;
	}
#else							/* UNIX */

	for (;;)
	{
		*workerset = saveSet;
		i = select(maxFd + 1, workerset, NULL, NULL, NULL);

		/*
		 * If we Ctrl-C the master process , it's likely that we interrupt
		 * select() here. The signal handler will set wantAbort == true and
		 * the shutdown journey starts from here. Note that we'll come back
		 * here later when we tell all workers to terminate and read their
		 * responses. But then we have aborting set to true.
		 */
		if (wantAbort && !aborting)
			exit_horribly(modulename, "terminated by user\n");

		if (i < 0 && errno == EINTR)
			continue;
		break;
	}
#endif

	return i;
}


/*
 * This function is executed in the master process.
 *
 * It returns the next message from the worker on the communication channel,
 * optionally blocking (do_wait) until it becomes available.
 *
 * The id of the worker is returned in *worker.
 */
static char *
getMessageFromWorker(ParallelState *pstate, bool do_wait, int *worker)
{
	int			i;
	fd_set		workerset;
	int			maxFd = -1;
	struct timeval nowait = {0, 0};

	FD_ZERO(&workerset);

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus == WRKR_TERMINATED)
			continue;
		FD_SET(pstate->parallelSlot[i].pipeRead, &workerset);
		/* actually WIN32 ignores the first parameter to select()... */
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
		exit_horribly(modulename, "error in ListenToWorkers(): %s\n", strerror(errno));

	for (i = 0; i < pstate->numWorkers; i++)
	{
		char	   *msg;

		if (!FD_ISSET(pstate->parallelSlot[i].pipeRead, &workerset))
			continue;

		msg = readMessageFromPipe(pstate->parallelSlot[i].pipeRead);
		*worker = i;
		return msg;
	}
	Assert(false);
	return NULL;
}

/*
 * This function is executed in the master process.
 *
 * It sends a message to a certain worker on the communication channel.
 */
static void
sendMessageToWorker(ParallelState *pstate, int worker, const char *str)
{
	int			len = strlen(str) + 1;

	if (pipewrite(pstate->parallelSlot[worker].pipeWrite, str, len) != len)
	{
		/*
		 * If we're already aborting anyway, don't care if we succeed or not.
		 * The child might have gone already.
		 */
#ifndef WIN32
		if (!aborting)
#endif
			exit_horribly(modulename,
						"could not write to the communication channel: %s\n",
						  strerror(errno));
	}
}

/*
 * The underlying function to read a message from the communication channel
 * (fd) with optional blocking (do_wait).
 */
static char *
readMessageFromPipe(int fd)
{
	char	   *msg;
	int			msgsize,
				bufsize;
	int			ret;

	/*
	 * The problem here is that we need to deal with several possibilites: we
	 * could receive only a partial message or several messages at once. The
	 * caller expects us to return exactly one message however.
	 *
	 * We could either read in as much as we can and keep track of what we
	 * delivered back to the caller or we just read byte by byte. Once we see
	 * (char) 0, we know that it's the message's end. This would be quite
	 * inefficient for more data but since we are reading only on the command
	 * channel, the performance loss does not seem worth the trouble of
	 * keeping internal states for different file descriptors.
	 */
	bufsize = 64;				/* could be any number */
	msg = (char *) pg_malloc(bufsize);

	msgsize = 0;
	for (;;)
	{
		Assert(msgsize <= bufsize);
		ret = piperead(fd, msg + msgsize, 1);

		/* worker has closed the connection or another error happened */
		if (ret <= 0)
			break;

		Assert(ret == 1);

		if (msg[msgsize] == '\0')
			return msg;

		msgsize++;
		if (msgsize == bufsize)
		{
			/* could be any number */
			bufsize += 16;
			msg = (char *) realloc(msg, bufsize);
		}
	}

	/*
	 * Worker has closed the connection, make sure to clean up before return
	 * since we are not returning msg (but did allocate it).
	 */
	free(msg);

	return NULL;
}

#ifdef WIN32
/*
 * This is a replacement version of pipe for Win32 which allows returned
 * handles to be used in select(). Note that read/write calls must be replaced
 * with recv/send.  "handles" have to be integers so we check for errors then
 * cast to integers.
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

static int
piperead(int s, char *buf, int len)
{
	int			ret = recv(s, buf, len, 0);

	if (ret < 0 && WSAGetLastError() == WSAECONNRESET)
		/* EOF on the pipe! (win32 socket based implementation) */
		ret = 0;
	return ret;
}

#endif
