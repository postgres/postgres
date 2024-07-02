/*-------------------------------------------------------------------------
 *
 * syslogger.c
 *
 * The system logger (syslogger) appeared in Postgres 8.0. It catches all
 * stderr output from the postmaster, backends, and other subprocesses
 * by redirecting to a pipe, and writes it to a set of logfiles.
 * It's possible to have size and age limits for the logfile configured
 * in postgresql.conf. If these limits are reached or passed, the
 * current logfile is closed and a new one is created (rotated).
 * The logfiles are stored in a subdirectory (configurable in
 * postgresql.conf), using a user-selectable naming scheme.
 *
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * Copyright (c) 2004-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/syslogger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "common/file_perm.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "pgtime.h"
#include "port/pg_bitutils.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pg_shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"

/*
 * We read() into a temp buffer twice as big as a chunk, so that any fragment
 * left after processing can be moved down to the front and we'll still have
 * room to read a full chunk.
 */
#define READ_BUF_SIZE (2 * PIPE_CHUNK_SIZE)

/* Log rotation signal file path, relative to $PGDATA */
#define LOGROTATE_SIGNAL_FILE	"logrotate"


/*
 * GUC parameters.  Logging_collector cannot be changed after postmaster
 * start, but the rest can change at SIGHUP.
 */
bool		Logging_collector = false;
int			Log_RotationAge = HOURS_PER_DAY * MINS_PER_HOUR;
int			Log_RotationSize = 10 * 1024;
char	   *Log_directory = NULL;
char	   *Log_filename = NULL;
bool		Log_truncate_on_rotation = false;
int			Log_file_mode = S_IRUSR | S_IWUSR;

/*
 * Private state
 */
static pg_time_t next_rotation_time;
static bool pipe_eof_seen = false;
static bool rotation_disabled = false;
static FILE *syslogFile = NULL;
static FILE *csvlogFile = NULL;
static FILE *jsonlogFile = NULL;
NON_EXEC_STATIC pg_time_t first_syslogger_file_time = 0;
static char *last_sys_file_name = NULL;
static char *last_csv_file_name = NULL;
static char *last_json_file_name = NULL;

/*
 * Buffers for saving partial messages from different backends.
 *
 * Keep NBUFFER_LISTS lists of these, with the entry for a given source pid
 * being in the list numbered (pid % NBUFFER_LISTS), so as to cut down on
 * the number of entries we have to examine for any one incoming message.
 * There must never be more than one entry for the same source pid.
 *
 * An inactive buffer is not removed from its list, just held for re-use.
 * An inactive buffer has pid == 0 and undefined contents of data.
 */
typedef struct
{
	int32		pid;			/* PID of source process */
	StringInfoData data;		/* accumulated data, as a StringInfo */
} save_buffer;

#define NBUFFER_LISTS 256
static List *buffer_lists[NBUFFER_LISTS];

/* These must be exported for EXEC_BACKEND case ... annoying */
#ifndef WIN32
int			syslogPipe[2] = {-1, -1};
#else
HANDLE		syslogPipe[2] = {0, 0};
#endif

#ifdef WIN32
static HANDLE threadHandle = 0;
static CRITICAL_SECTION sysloggerSection;
#endif

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t rotation_requested = false;


/* Local subroutines */
#ifdef EXEC_BACKEND
static int	syslogger_fdget(FILE *file);
static FILE *syslogger_fdopen(int fd);
#endif
static void process_pipe_input(char *logbuffer, int *bytes_in_logbuffer);
static void flush_pipe_input(char *logbuffer, int *bytes_in_logbuffer);
static FILE *logfile_open(const char *filename, const char *mode,
						  bool allow_errors);

#ifdef WIN32
static unsigned int __stdcall pipeThread(void *arg);
#endif
static void logfile_rotate(bool time_based_rotation, int size_rotation_for);
static bool logfile_rotate_dest(bool time_based_rotation,
								int size_rotation_for, pg_time_t fntime,
								int target_dest, char **last_file_name,
								FILE **logFile);
static char *logfile_getname(pg_time_t timestamp, const char *suffix);
static void set_next_rotation_time(void);
static void sigUsr1Handler(SIGNAL_ARGS);
static void update_metainfo_datafile(void);

typedef struct
{
	int			syslogFile;
	int			csvlogFile;
	int			jsonlogFile;
} SysloggerStartupData;

/*
 * Main entry point for syslogger process
 * argc/argv parameters are valid only in EXEC_BACKEND case.
 */
void
SysLoggerMain(char *startup_data, size_t startup_data_len)
{
#ifndef WIN32
	char		logbuffer[READ_BUF_SIZE];
	int			bytes_in_logbuffer = 0;
#endif
	char	   *currentLogDir;
	char	   *currentLogFilename;
	int			currentLogRotationAge;
	pg_time_t	now;
	WaitEventSet *wes;

	/*
	 * Re-open the error output files that were opened by SysLogger_Start().
	 *
	 * We expect this will always succeed, which is too optimistic, but if it
	 * fails there's not a lot we can do to report the problem anyway.  As
	 * coded, we'll just crash on a null pointer dereference after failure...
	 */
#ifdef EXEC_BACKEND
	{
		SysloggerStartupData *slsdata = (SysloggerStartupData *) startup_data;

		Assert(startup_data_len == sizeof(*slsdata));
		syslogFile = syslogger_fdopen(slsdata->syslogFile);
		csvlogFile = syslogger_fdopen(slsdata->csvlogFile);
		jsonlogFile = syslogger_fdopen(slsdata->jsonlogFile);
	}
#else
	Assert(startup_data_len == 0);
#endif

	/*
	 * Now that we're done reading the startup data, release postmaster's
	 * working memory context.
	 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	now = MyStartTime;

	MyBackendType = B_LOGGER;
	init_ps_display(NULL);

	/*
	 * If we restarted, our stderr is already redirected into our own input
	 * pipe.  This is of course pretty useless, not to mention that it
	 * interferes with detecting pipe EOF.  Point stderr to /dev/null. This
	 * assumes that all interesting messages generated in the syslogger will
	 * come through elog.c and will be sent to write_syslogger_file.
	 */
	if (redirection_done)
	{
		int			fd = open(DEVNULL, O_WRONLY, 0);

		/*
		 * The closes might look redundant, but they are not: we want to be
		 * darn sure the pipe gets closed even if the open failed.  We can
		 * survive running with stderr pointing nowhere, but we can't afford
		 * to have extra pipe input descriptors hanging around.
		 *
		 * As we're just trying to reset these to go to DEVNULL, there's not
		 * much point in checking for failure from the close/dup2 calls here,
		 * if they fail then presumably the file descriptors are closed and
		 * any writes will go into the bitbucket anyway.
		 */
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		if (fd != -1)
		{
			(void) dup2(fd, STDOUT_FILENO);
			(void) dup2(fd, STDERR_FILENO);
			close(fd);
		}
	}

	/*
	 * Syslogger's own stderr can't be the syslogPipe, so set it back to text
	 * mode if we didn't just close it. (It was set to binary in
	 * SubPostmasterMain).
	 */
#ifdef WIN32
	else
		_setmode(STDERR_FILENO, _O_TEXT);
#endif

	/*
	 * Also close our copy of the write end of the pipe.  This is needed to
	 * ensure we can detect pipe EOF correctly.  (But note that in the restart
	 * case, the postmaster already did this.)
	 */
#ifndef WIN32
	if (syslogPipe[1] >= 0)
		close(syslogPipe[1]);
	syslogPipe[1] = -1;
#else
	if (syslogPipe[1])
		CloseHandle(syslogPipe[1]);
	syslogPipe[1] = 0;
#endif

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * Note: we ignore all termination signals, and instead exit only when all
	 * upstream processes are gone, to ensure we don't miss any dying gasps of
	 * broken backends...
	 */

	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* set flag to read config
													 * file */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, SIG_IGN);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, sigUsr1Handler);	/* request log rotation */
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

#ifdef WIN32
	/* Fire up separate data transfer thread */
	InitializeCriticalSection(&sysloggerSection);
	EnterCriticalSection(&sysloggerSection);

	threadHandle = (HANDLE) _beginthreadex(NULL, 0, pipeThread, NULL, 0, NULL);
	if (threadHandle == 0)
		elog(FATAL, "could not create syslogger data transfer thread: %m");
#endif							/* WIN32 */

	/*
	 * Remember active logfiles' name(s).  We recompute 'em from the reference
	 * time because passing down just the pg_time_t is a lot cheaper than
	 * passing a whole file path in the EXEC_BACKEND case.
	 */
	last_sys_file_name = logfile_getname(first_syslogger_file_time, NULL);
	if (csvlogFile != NULL)
		last_csv_file_name = logfile_getname(first_syslogger_file_time, ".csv");
	if (jsonlogFile != NULL)
		last_json_file_name = logfile_getname(first_syslogger_file_time, ".json");

	/* remember active logfile parameters */
	currentLogDir = pstrdup(Log_directory);
	currentLogFilename = pstrdup(Log_filename);
	currentLogRotationAge = Log_RotationAge;
	/* set next planned rotation time */
	set_next_rotation_time();
	update_metainfo_datafile();

	/*
	 * Reset whereToSendOutput, as the postmaster will do (but hasn't yet, at
	 * the point where we forked).  This prevents duplicate output of messages
	 * from syslogger itself.
	 */
	whereToSendOutput = DestNone;

	/*
	 * Set up a reusable WaitEventSet object we'll use to wait for our latch,
	 * and (except on Windows) our socket.
	 *
	 * Unlike all other postmaster child processes, we'll ignore postmaster
	 * death because we want to collect final log output from all backends and
	 * then exit last.  We'll do that by running until we see EOF on the
	 * syslog pipe, which implies that all other backends have exited
	 * (including the postmaster).
	 */
	wes = CreateWaitEventSet(NULL, 2);
	AddWaitEventToSet(wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
#ifndef WIN32
	AddWaitEventToSet(wes, WL_SOCKET_READABLE, syslogPipe[0], NULL, NULL);
#endif

	/* main worker loop */
	for (;;)
	{
		bool		time_based_rotation = false;
		int			size_rotation_for = 0;
		long		cur_timeout;
		WaitEvent	event;

#ifndef WIN32
		int			rc;
#endif

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		/*
		 * Process any requests or signals received recently.
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);

			/*
			 * Check if the log directory or filename pattern changed in
			 * postgresql.conf. If so, force rotation to make sure we're
			 * writing the logfiles in the right place.
			 */
			if (strcmp(Log_directory, currentLogDir) != 0)
			{
				pfree(currentLogDir);
				currentLogDir = pstrdup(Log_directory);
				rotation_requested = true;

				/*
				 * Also, create new directory if not present; ignore errors
				 */
				(void) MakePGDirectory(Log_directory);
			}
			if (strcmp(Log_filename, currentLogFilename) != 0)
			{
				pfree(currentLogFilename);
				currentLogFilename = pstrdup(Log_filename);
				rotation_requested = true;
			}

			/*
			 * Force a rotation if CSVLOG output was just turned on or off and
			 * we need to open or close csvlogFile accordingly.
			 */
			if (((Log_destination & LOG_DESTINATION_CSVLOG) != 0) !=
				(csvlogFile != NULL))
				rotation_requested = true;

			/*
			 * Force a rotation if JSONLOG output was just turned on or off
			 * and we need to open or close jsonlogFile accordingly.
			 */
			if (((Log_destination & LOG_DESTINATION_JSONLOG) != 0) !=
				(jsonlogFile != NULL))
				rotation_requested = true;

			/*
			 * If rotation time parameter changed, reset next rotation time,
			 * but don't immediately force a rotation.
			 */
			if (currentLogRotationAge != Log_RotationAge)
			{
				currentLogRotationAge = Log_RotationAge;
				set_next_rotation_time();
			}

			/*
			 * If we had a rotation-disabling failure, re-enable rotation
			 * attempts after SIGHUP, and force one immediately.
			 */
			if (rotation_disabled)
			{
				rotation_disabled = false;
				rotation_requested = true;
			}

			/*
			 * Force rewriting last log filename when reloading configuration.
			 * Even if rotation_requested is false, log_destination may have
			 * been changed and we don't want to wait the next file rotation.
			 */
			update_metainfo_datafile();
		}

		if (Log_RotationAge > 0 && !rotation_disabled)
		{
			/* Do a logfile rotation if it's time */
			now = (pg_time_t) time(NULL);
			if (now >= next_rotation_time)
				rotation_requested = time_based_rotation = true;
		}

		if (!rotation_requested && Log_RotationSize > 0 && !rotation_disabled)
		{
			/* Do a rotation if file is too big */
			if (ftell(syslogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_STDERR;
			}
			if (csvlogFile != NULL &&
				ftell(csvlogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_CSVLOG;
			}
			if (jsonlogFile != NULL &&
				ftell(jsonlogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_JSONLOG;
			}
		}

		if (rotation_requested)
		{
			/*
			 * Force rotation when both values are zero. It means the request
			 * was sent by pg_rotate_logfile() or "pg_ctl logrotate".
			 */
			if (!time_based_rotation && size_rotation_for == 0)
				size_rotation_for = LOG_DESTINATION_STDERR |
					LOG_DESTINATION_CSVLOG |
					LOG_DESTINATION_JSONLOG;
			logfile_rotate(time_based_rotation, size_rotation_for);
		}

		/*
		 * Calculate time till next time-based rotation, so that we don't
		 * sleep longer than that.  We assume the value of "now" obtained
		 * above is still close enough.  Note we can't make this calculation
		 * until after calling logfile_rotate(), since it will advance
		 * next_rotation_time.
		 *
		 * Also note that we need to beware of overflow in calculation of the
		 * timeout: with large settings of Log_RotationAge, next_rotation_time
		 * could be more than INT_MAX msec in the future.  In that case we'll
		 * wait no more than INT_MAX msec, and try again.
		 */
		if (Log_RotationAge > 0 && !rotation_disabled)
		{
			pg_time_t	delay;

			delay = next_rotation_time - now;
			if (delay > 0)
			{
				if (delay > INT_MAX / 1000)
					delay = INT_MAX / 1000;
				cur_timeout = delay * 1000L;	/* msec */
			}
			else
				cur_timeout = 0;
		}
		else
			cur_timeout = -1L;

		/*
		 * Sleep until there's something to do
		 */
#ifndef WIN32
		rc = WaitEventSetWait(wes, cur_timeout, &event, 1,
							  WAIT_EVENT_SYSLOGGER_MAIN);

		if (rc == 1 && event.events == WL_SOCKET_READABLE)
		{
			int			bytesRead;

			bytesRead = read(syslogPipe[0],
							 logbuffer + bytes_in_logbuffer,
							 sizeof(logbuffer) - bytes_in_logbuffer);
			if (bytesRead < 0)
			{
				if (errno != EINTR)
					ereport(LOG,
							(errcode_for_socket_access(),
							 errmsg("could not read from logger pipe: %m")));
			}
			else if (bytesRead > 0)
			{
				bytes_in_logbuffer += bytesRead;
				process_pipe_input(logbuffer, &bytes_in_logbuffer);
				continue;
			}
			else
			{
				/*
				 * Zero bytes read when select() is saying read-ready means
				 * EOF on the pipe: that is, there are no longer any processes
				 * with the pipe write end open.  Therefore, the postmaster
				 * and all backends are shut down, and we are done.
				 */
				pipe_eof_seen = true;

				/* if there's any data left then force it out now */
				flush_pipe_input(logbuffer, &bytes_in_logbuffer);
			}
		}
#else							/* WIN32 */

		/*
		 * On Windows we leave it to a separate thread to transfer data and
		 * detect pipe EOF.  The main thread just wakes up to handle SIGHUP
		 * and rotation conditions.
		 *
		 * Server code isn't generally thread-safe, so we ensure that only one
		 * of the threads is active at a time by entering the critical section
		 * whenever we're not sleeping.
		 */
		LeaveCriticalSection(&sysloggerSection);

		(void) WaitEventSetWait(wes, cur_timeout, &event, 1,
								WAIT_EVENT_SYSLOGGER_MAIN);

		EnterCriticalSection(&sysloggerSection);
#endif							/* WIN32 */

		if (pipe_eof_seen)
		{
			/*
			 * seeing this message on the real stderr is annoying - so we make
			 * it DEBUG1 to suppress in normal use.
			 */
			ereport(DEBUG1,
					(errmsg_internal("logger shutting down")));

			/*
			 * Normal exit from the syslogger is here.  Note that we
			 * deliberately do not close syslogFile before exiting; this is to
			 * allow for the possibility of elog messages being generated
			 * inside proc_exit.  Regular exit() will take care of flushing
			 * and closing stdio channels.
			 */
			proc_exit(0);
		}
	}
}

/*
 * Postmaster subroutine to start a syslogger subprocess.
 */
int
SysLogger_Start(void)
{
	pid_t		sysloggerPid;
	char	   *filename;
#ifdef EXEC_BACKEND
	SysloggerStartupData startup_data;
#endif							/* EXEC_BACKEND */

	if (!Logging_collector)
		return 0;

	/*
	 * If first time through, create the pipe which will receive stderr
	 * output.
	 *
	 * If the syslogger crashes and needs to be restarted, we continue to use
	 * the same pipe (indeed must do so, since extant backends will be writing
	 * into that pipe).
	 *
	 * This means the postmaster must continue to hold the read end of the
	 * pipe open, so we can pass it down to the reincarnated syslogger. This
	 * is a bit klugy but we have little choice.
	 *
	 * Also note that we don't bother counting the pipe FDs by calling
	 * Reserve/ReleaseExternalFD.  There's no real need to account for them
	 * accurately in the postmaster or syslogger process, and both ends of the
	 * pipe will wind up closed in all other postmaster children.
	 */
#ifndef WIN32
	if (syslogPipe[0] < 0)
	{
		if (pipe(syslogPipe) < 0)
			ereport(FATAL,
					(errcode_for_socket_access(),
					 errmsg("could not create pipe for syslog: %m")));
	}
#else
	if (!syslogPipe[0])
	{
		SECURITY_ATTRIBUTES sa;

		memset(&sa, 0, sizeof(SECURITY_ATTRIBUTES));
		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle = TRUE;

		if (!CreatePipe(&syslogPipe[0], &syslogPipe[1], &sa, 32768))
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not create pipe for syslog: %m")));
	}
#endif

	/*
	 * Create log directory if not present; ignore errors
	 */
	(void) MakePGDirectory(Log_directory);

	/*
	 * The initial logfile is created right in the postmaster, to verify that
	 * the Log_directory is writable.  We save the reference time so that the
	 * syslogger child process can recompute this file name.
	 *
	 * It might look a bit strange to re-do this during a syslogger restart,
	 * but we must do so since the postmaster closed syslogFile after the
	 * previous fork (and remembering that old file wouldn't be right anyway).
	 * Note we always append here, we won't overwrite any existing file.  This
	 * is consistent with the normal rules, because by definition this is not
	 * a time-based rotation.
	 */
	first_syslogger_file_time = time(NULL);

	filename = logfile_getname(first_syslogger_file_time, NULL);

	syslogFile = logfile_open(filename, "a", false);

	pfree(filename);

	/*
	 * Likewise for the initial CSV log file, if that's enabled.  (Note that
	 * we open syslogFile even when only CSV output is nominally enabled,
	 * since some code paths will write to syslogFile anyway.)
	 */
	if (Log_destination & LOG_DESTINATION_CSVLOG)
	{
		filename = logfile_getname(first_syslogger_file_time, ".csv");

		csvlogFile = logfile_open(filename, "a", false);

		pfree(filename);
	}

	/*
	 * Likewise for the initial JSON log file, if that's enabled.  (Note that
	 * we open syslogFile even when only JSON output is nominally enabled,
	 * since some code paths will write to syslogFile anyway.)
	 */
	if (Log_destination & LOG_DESTINATION_JSONLOG)
	{
		filename = logfile_getname(first_syslogger_file_time, ".json");

		jsonlogFile = logfile_open(filename, "a", false);

		pfree(filename);
	}

#ifdef EXEC_BACKEND
	startup_data.syslogFile = syslogger_fdget(syslogFile);
	startup_data.csvlogFile = syslogger_fdget(csvlogFile);
	startup_data.jsonlogFile = syslogger_fdget(jsonlogFile);
	sysloggerPid = postmaster_child_launch(B_LOGGER, (char *) &startup_data, sizeof(startup_data), NULL);
#else
	sysloggerPid = postmaster_child_launch(B_LOGGER, NULL, 0, NULL);
#endif							/* EXEC_BACKEND */

	if (sysloggerPid == -1)
	{
		ereport(LOG,
				(errmsg("could not fork system logger: %m")));
		return 0;
	}

	/* success, in postmaster */

	/* now we redirect stderr, if not done already */
	if (!redirection_done)
	{
#ifdef WIN32
		int			fd;
#endif

		/*
		 * Leave a breadcrumb trail when redirecting, in case the user forgets
		 * that redirection is active and looks only at the original stderr
		 * target file.
		 */
		ereport(LOG,
				(errmsg("redirecting log output to logging collector process"),
				 errhint("Future log output will appear in directory \"%s\".",
						 Log_directory)));

#ifndef WIN32
		fflush(stdout);
		if (dup2(syslogPipe[1], STDOUT_FILENO) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not redirect stdout: %m")));
		fflush(stderr);
		if (dup2(syslogPipe[1], STDERR_FILENO) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not redirect stderr: %m")));
		/* Now we are done with the write end of the pipe. */
		close(syslogPipe[1]);
		syslogPipe[1] = -1;
#else

		/*
		 * open the pipe in binary mode and make sure stderr is binary after
		 * it's been dup'ed into, to avoid disturbing the pipe chunking
		 * protocol.
		 */
		fflush(stderr);
		fd = _open_osfhandle((intptr_t) syslogPipe[1],
							 _O_APPEND | _O_BINARY);
		if (dup2(fd, STDERR_FILENO) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not redirect stderr: %m")));
		close(fd);
		_setmode(STDERR_FILENO, _O_BINARY);

		/*
		 * Now we are done with the write end of the pipe.  CloseHandle() must
		 * not be called because the preceding close() closes the underlying
		 * handle.
		 */
		syslogPipe[1] = 0;
#endif
		redirection_done = true;
	}

	/* postmaster will never write the file(s); close 'em */
	fclose(syslogFile);
	syslogFile = NULL;
	if (csvlogFile != NULL)
	{
		fclose(csvlogFile);
		csvlogFile = NULL;
	}
	if (jsonlogFile != NULL)
	{
		fclose(jsonlogFile);
		jsonlogFile = NULL;
	}
	return (int) sysloggerPid;
}


#ifdef EXEC_BACKEND

/*
 * syslogger_fdget() -
 *
 * Utility wrapper to grab the file descriptor of an opened error output
 * file.  Used when building the command to fork the logging collector.
 */
static int
syslogger_fdget(FILE *file)
{
#ifndef WIN32
	if (file != NULL)
		return fileno(file);
	else
		return -1;
#else
	if (file != NULL)
		return (int) _get_osfhandle(_fileno(file));
	else
		return 0;
#endif							/* WIN32 */
}

/*
 * syslogger_fdopen() -
 *
 * Utility wrapper to re-open an error output file, using the given file
 * descriptor.  Used when parsing arguments in a forked logging collector.
 */
static FILE *
syslogger_fdopen(int fd)
{
	FILE	   *file = NULL;

#ifndef WIN32
	if (fd != -1)
	{
		file = fdopen(fd, "a");
		setvbuf(file, NULL, PG_IOLBF, 0);
	}
#else							/* WIN32 */
	if (fd != 0)
	{
		fd = _open_osfhandle(fd, _O_APPEND | _O_TEXT);
		if (fd > 0)
		{
			file = fdopen(fd, "a");
			setvbuf(file, NULL, PG_IOLBF, 0);
		}
	}
#endif							/* WIN32 */

	return file;
}
#endif							/* EXEC_BACKEND */


/* --------------------------------
 *		pipe protocol handling
 * --------------------------------
 */

/*
 * Process data received through the syslogger pipe.
 *
 * This routine interprets the log pipe protocol which sends log messages as
 * (hopefully atomic) chunks - such chunks are detected and reassembled here.
 *
 * The protocol has a header that starts with two nul bytes, then has a 16 bit
 * length, the pid of the sending process, and a flag to indicate if it is
 * the last chunk in a message. Incomplete chunks are saved until we read some
 * more, and non-final chunks are accumulated until we get the final chunk.
 *
 * All of this is to avoid 2 problems:
 * . partial messages being written to logfiles (messes rotation), and
 * . messages from different backends being interleaved (messages garbled).
 *
 * Any non-protocol messages are written out directly. These should only come
 * from non-PostgreSQL sources, however (e.g. third party libraries writing to
 * stderr).
 *
 * logbuffer is the data input buffer, and *bytes_in_logbuffer is the number
 * of bytes present.  On exit, any not-yet-eaten data is left-justified in
 * logbuffer, and *bytes_in_logbuffer is updated.
 */
static void
process_pipe_input(char *logbuffer, int *bytes_in_logbuffer)
{
	char	   *cursor = logbuffer;
	int			count = *bytes_in_logbuffer;
	int			dest = LOG_DESTINATION_STDERR;

	/* While we have enough for a header, process data... */
	while (count >= (int) (offsetof(PipeProtoHeader, data) + 1))
	{
		PipeProtoHeader p;
		int			chunklen;
		bits8		dest_flags;

		/* Do we have a valid header? */
		memcpy(&p, cursor, offsetof(PipeProtoHeader, data));
		dest_flags = p.flags & (PIPE_PROTO_DEST_STDERR |
								PIPE_PROTO_DEST_CSVLOG |
								PIPE_PROTO_DEST_JSONLOG);
		if (p.nuls[0] == '\0' && p.nuls[1] == '\0' &&
			p.len > 0 && p.len <= PIPE_MAX_PAYLOAD &&
			p.pid != 0 &&
			pg_number_of_ones[dest_flags] == 1)
		{
			List	   *buffer_list;
			ListCell   *cell;
			save_buffer *existing_slot = NULL,
					   *free_slot = NULL;
			StringInfo	str;

			chunklen = PIPE_HEADER_SIZE + p.len;

			/* Fall out of loop if we don't have the whole chunk yet */
			if (count < chunklen)
				break;

			if ((p.flags & PIPE_PROTO_DEST_STDERR) != 0)
				dest = LOG_DESTINATION_STDERR;
			else if ((p.flags & PIPE_PROTO_DEST_CSVLOG) != 0)
				dest = LOG_DESTINATION_CSVLOG;
			else if ((p.flags & PIPE_PROTO_DEST_JSONLOG) != 0)
				dest = LOG_DESTINATION_JSONLOG;
			else
			{
				/* this should never happen as of the header validation */
				Assert(false);
			}

			/* Locate any existing buffer for this source pid */
			buffer_list = buffer_lists[p.pid % NBUFFER_LISTS];
			foreach(cell, buffer_list)
			{
				save_buffer *buf = (save_buffer *) lfirst(cell);

				if (buf->pid == p.pid)
				{
					existing_slot = buf;
					break;
				}
				if (buf->pid == 0 && free_slot == NULL)
					free_slot = buf;
			}

			if ((p.flags & PIPE_PROTO_IS_LAST) == 0)
			{
				/*
				 * Save a complete non-final chunk in a per-pid buffer
				 */
				if (existing_slot != NULL)
				{
					/* Add chunk to data from preceding chunks */
					str = &(existing_slot->data);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
				}
				else
				{
					/* First chunk of message, save in a new buffer */
					if (free_slot == NULL)
					{
						/*
						 * Need a free slot, but there isn't one in the list,
						 * so create a new one and extend the list with it.
						 */
						free_slot = palloc(sizeof(save_buffer));
						buffer_list = lappend(buffer_list, free_slot);
						buffer_lists[p.pid % NBUFFER_LISTS] = buffer_list;
					}
					free_slot->pid = p.pid;
					str = &(free_slot->data);
					initStringInfo(str);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
				}
			}
			else
			{
				/*
				 * Final chunk --- add it to anything saved for that pid, and
				 * either way write the whole thing out.
				 */
				if (existing_slot != NULL)
				{
					str = &(existing_slot->data);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
					write_syslogger_file(str->data, str->len, dest);
					/* Mark the buffer unused, and reclaim string storage */
					existing_slot->pid = 0;
					pfree(str->data);
				}
				else
				{
					/* The whole message was one chunk, evidently. */
					write_syslogger_file(cursor + PIPE_HEADER_SIZE, p.len,
										 dest);
				}
			}

			/* Finished processing this chunk */
			cursor += chunklen;
			count -= chunklen;
		}
		else
		{
			/* Process non-protocol data */

			/*
			 * Look for the start of a protocol header.  If found, dump data
			 * up to there and repeat the loop.  Otherwise, dump it all and
			 * fall out of the loop.  (Note: we want to dump it all if at all
			 * possible, so as to avoid dividing non-protocol messages across
			 * logfiles.  We expect that in many scenarios, a non-protocol
			 * message will arrive all in one read(), and we want to respect
			 * the read() boundary if possible.)
			 */
			for (chunklen = 1; chunklen < count; chunklen++)
			{
				if (cursor[chunklen] == '\0')
					break;
			}
			/* fall back on the stderr log as the destination */
			write_syslogger_file(cursor, chunklen, LOG_DESTINATION_STDERR);
			cursor += chunklen;
			count -= chunklen;
		}
	}

	/* We don't have a full chunk, so left-align what remains in the buffer */
	if (count > 0 && cursor != logbuffer)
		memmove(logbuffer, cursor, count);
	*bytes_in_logbuffer = count;
}

/*
 * Force out any buffered data
 *
 * This is currently used only at syslogger shutdown, but could perhaps be
 * useful at other times, so it is careful to leave things in a clean state.
 */
static void
flush_pipe_input(char *logbuffer, int *bytes_in_logbuffer)
{
	int			i;

	/* Dump any incomplete protocol messages */
	for (i = 0; i < NBUFFER_LISTS; i++)
	{
		List	   *list = buffer_lists[i];
		ListCell   *cell;

		foreach(cell, list)
		{
			save_buffer *buf = (save_buffer *) lfirst(cell);

			if (buf->pid != 0)
			{
				StringInfo	str = &(buf->data);

				write_syslogger_file(str->data, str->len,
									 LOG_DESTINATION_STDERR);
				/* Mark the buffer unused, and reclaim string storage */
				buf->pid = 0;
				pfree(str->data);
			}
		}
	}

	/*
	 * Force out any remaining pipe data as-is; we don't bother trying to
	 * remove any protocol headers that may exist in it.
	 */
	if (*bytes_in_logbuffer > 0)
		write_syslogger_file(logbuffer, *bytes_in_logbuffer,
							 LOG_DESTINATION_STDERR);
	*bytes_in_logbuffer = 0;
}


/* --------------------------------
 *		logfile routines
 * --------------------------------
 */

/*
 * Write text to the currently open logfile
 *
 * This is exported so that elog.c can call it when MyBackendType is B_LOGGER.
 * This allows the syslogger process to record elog messages of its own,
 * even though its stderr does not point at the syslog pipe.
 */
void
write_syslogger_file(const char *buffer, int count, int destination)
{
	int			rc;
	FILE	   *logfile;

	/*
	 * If we're told to write to a structured log file, but it's not open,
	 * dump the data to syslogFile (which is always open) instead.  This can
	 * happen if structured output is enabled after postmaster start and we've
	 * been unable to open logFile.  There are also race conditions during a
	 * parameter change whereby backends might send us structured output
	 * before we open the logFile or after we close it.  Writing formatted
	 * output to the regular log file isn't great, but it beats dropping log
	 * output on the floor.
	 *
	 * Think not to improve this by trying to open logFile on-the-fly.  Any
	 * failure in that would lead to recursion.
	 */
	if ((destination & LOG_DESTINATION_CSVLOG) && csvlogFile != NULL)
		logfile = csvlogFile;
	else if ((destination & LOG_DESTINATION_JSONLOG) && jsonlogFile != NULL)
		logfile = jsonlogFile;
	else
		logfile = syslogFile;

	rc = fwrite(buffer, 1, count, logfile);

	/*
	 * Try to report any failure.  We mustn't use ereport because it would
	 * just recurse right back here, but write_stderr is OK: it will write
	 * either to the postmaster's original stderr, or to /dev/null, but never
	 * to our input pipe which would result in a different sort of looping.
	 */
	if (rc != count)
		write_stderr("could not write to log file: %m\n");
}

#ifdef WIN32

/*
 * Worker thread to transfer data from the pipe to the current logfile.
 *
 * We need this because on Windows, WaitForMultipleObjects does not work on
 * unnamed pipes: it always reports "signaled", so the blocking ReadFile won't
 * allow for SIGHUP; and select is for sockets only.
 */
static unsigned int __stdcall
pipeThread(void *arg)
{
	char		logbuffer[READ_BUF_SIZE];
	int			bytes_in_logbuffer = 0;

	for (;;)
	{
		DWORD		bytesRead;
		BOOL		result;

		result = ReadFile(syslogPipe[0],
						  logbuffer + bytes_in_logbuffer,
						  sizeof(logbuffer) - bytes_in_logbuffer,
						  &bytesRead, 0);

		/*
		 * Enter critical section before doing anything that might touch
		 * global state shared by the main thread. Anything that uses
		 * palloc()/pfree() in particular are not safe outside the critical
		 * section.
		 */
		EnterCriticalSection(&sysloggerSection);
		if (!result)
		{
			DWORD		error = GetLastError();

			if (error == ERROR_HANDLE_EOF ||
				error == ERROR_BROKEN_PIPE)
				break;
			_dosmaperr(error);
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not read from logger pipe: %m")));
		}
		else if (bytesRead > 0)
		{
			bytes_in_logbuffer += bytesRead;
			process_pipe_input(logbuffer, &bytes_in_logbuffer);
		}

		/*
		 * If we've filled the current logfile, nudge the main thread to do a
		 * log rotation.
		 */
		if (Log_RotationSize > 0)
		{
			if (ftell(syslogFile) >= Log_RotationSize * 1024L ||
				(csvlogFile != NULL && ftell(csvlogFile) >= Log_RotationSize * 1024L) ||
				(jsonlogFile != NULL && ftell(jsonlogFile) >= Log_RotationSize * 1024L))
				SetLatch(MyLatch);
		}
		LeaveCriticalSection(&sysloggerSection);
	}

	/* We exit the above loop only upon detecting pipe EOF */
	pipe_eof_seen = true;

	/* if there's any data left then force it out now */
	flush_pipe_input(logbuffer, &bytes_in_logbuffer);

	/* set the latch to waken the main thread, which will quit */
	SetLatch(MyLatch);

	LeaveCriticalSection(&sysloggerSection);
	_endthread();
	return 0;
}
#endif							/* WIN32 */

/*
 * Open a new logfile with proper permissions and buffering options.
 *
 * If allow_errors is true, we just log any open failure and return NULL
 * (with errno still correct for the fopen failure).
 * Otherwise, errors are treated as fatal.
 */
static FILE *
logfile_open(const char *filename, const char *mode, bool allow_errors)
{
	FILE	   *fh;
	mode_t		oumask;

	/*
	 * Note we do not let Log_file_mode disable IWUSR, since we certainly want
	 * to be able to write the files ourselves.
	 */
	oumask = umask((mode_t) ((~(Log_file_mode | S_IWUSR)) & (S_IRWXU | S_IRWXG | S_IRWXO)));
	fh = fopen(filename, mode);
	umask(oumask);

	if (fh)
	{
		setvbuf(fh, NULL, PG_IOLBF, 0);

#ifdef WIN32
		/* use CRLF line endings on Windows */
		_setmode(_fileno(fh), _O_TEXT);
#endif
	}
	else
	{
		int			save_errno = errno;

		ereport(allow_errors ? LOG : FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open log file \"%s\": %m",
						filename)));
		errno = save_errno;
	}

	return fh;
}

/*
 * Do logfile rotation for a single destination, as specified by target_dest.
 * The information stored in *last_file_name and *logFile is updated on a
 * successful file rotation.
 *
 * Returns false if the rotation has been stopped, or true to move on to
 * the processing of other formats.
 */
static bool
logfile_rotate_dest(bool time_based_rotation, int size_rotation_for,
					pg_time_t fntime, int target_dest,
					char **last_file_name, FILE **logFile)
{
	char	   *logFileExt = NULL;
	char	   *filename;
	FILE	   *fh;

	/*
	 * If the target destination was just turned off, close the previous file
	 * and unregister its data.  This cannot happen for stderr as syslogFile
	 * is assumed to be always opened even if stderr is disabled in
	 * log_destination.
	 */
	if ((Log_destination & target_dest) == 0 &&
		target_dest != LOG_DESTINATION_STDERR)
	{
		if (*logFile != NULL)
			fclose(*logFile);
		*logFile = NULL;
		if (*last_file_name != NULL)
			pfree(*last_file_name);
		*last_file_name = NULL;
		return true;
	}

	/*
	 * Leave if it is not time for a rotation or if the target destination has
	 * no need to do a rotation based on the size of its file.
	 */
	if (!time_based_rotation && (size_rotation_for & target_dest) == 0)
		return true;

	/* file extension depends on the destination type */
	if (target_dest == LOG_DESTINATION_STDERR)
		logFileExt = NULL;
	else if (target_dest == LOG_DESTINATION_CSVLOG)
		logFileExt = ".csv";
	else if (target_dest == LOG_DESTINATION_JSONLOG)
		logFileExt = ".json";
	else
	{
		/* cannot happen */
		Assert(false);
	}

	/* build the new file name */
	filename = logfile_getname(fntime, logFileExt);

	/*
	 * Decide whether to overwrite or append.  We can overwrite if (a)
	 * Log_truncate_on_rotation is set, (b) the rotation was triggered by
	 * elapsed time and not something else, and (c) the computed file name is
	 * different from what we were previously logging into.
	 */
	if (Log_truncate_on_rotation && time_based_rotation &&
		*last_file_name != NULL &&
		strcmp(filename, *last_file_name) != 0)
		fh = logfile_open(filename, "w", true);
	else
		fh = logfile_open(filename, "a", true);

	if (!fh)
	{
		/*
		 * ENFILE/EMFILE are not too surprising on a busy system; just keep
		 * using the old file till we manage to get a new one.  Otherwise,
		 * assume something's wrong with Log_directory and stop trying to
		 * create files.
		 */
		if (errno != ENFILE && errno != EMFILE)
		{
			ereport(LOG,
					(errmsg("disabling automatic rotation (use SIGHUP to re-enable)")));
			rotation_disabled = true;
		}

		if (filename)
			pfree(filename);
		return false;
	}

	/* fill in the new information */
	if (*logFile != NULL)
		fclose(*logFile);
	*logFile = fh;

	/* instead of pfree'ing filename, remember it for next time */
	if (*last_file_name != NULL)
		pfree(*last_file_name);
	*last_file_name = filename;

	return true;
}

/*
 * perform logfile rotation
 */
static void
logfile_rotate(bool time_based_rotation, int size_rotation_for)
{
	pg_time_t	fntime;

	rotation_requested = false;

	/*
	 * When doing a time-based rotation, invent the new logfile name based on
	 * the planned rotation time, not current time, to avoid "slippage" in the
	 * file name when we don't do the rotation immediately.
	 */
	if (time_based_rotation)
		fntime = next_rotation_time;
	else
		fntime = time(NULL);

	/* file rotation for stderr */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_STDERR, &last_sys_file_name,
							 &syslogFile))
		return;

	/* file rotation for csvlog */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_CSVLOG, &last_csv_file_name,
							 &csvlogFile))
		return;

	/* file rotation for jsonlog */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_JSONLOG, &last_json_file_name,
							 &jsonlogFile))
		return;

	update_metainfo_datafile();

	set_next_rotation_time();
}


/*
 * construct logfile name using timestamp information
 *
 * If suffix isn't NULL, append it to the name, replacing any ".log"
 * that may be in the pattern.
 *
 * Result is palloc'd.
 */
static char *
logfile_getname(pg_time_t timestamp, const char *suffix)
{
	char	   *filename;
	int			len;

	filename = palloc(MAXPGPATH);

	snprintf(filename, MAXPGPATH, "%s/", Log_directory);

	len = strlen(filename);

	/* treat Log_filename as a strftime pattern */
	pg_strftime(filename + len, MAXPGPATH - len, Log_filename,
				pg_localtime(&timestamp, log_timezone));

	if (suffix != NULL)
	{
		len = strlen(filename);
		if (len > 4 && (strcmp(filename + (len - 4), ".log") == 0))
			len -= 4;
		strlcpy(filename + len, suffix, MAXPGPATH - len);
	}

	return filename;
}

/*
 * Determine the next planned rotation time, and store in next_rotation_time.
 */
static void
set_next_rotation_time(void)
{
	pg_time_t	now;
	struct pg_tm *tm;
	int			rotinterval;

	/* nothing to do if time-based rotation is disabled */
	if (Log_RotationAge <= 0)
		return;

	/*
	 * The requirements here are to choose the next time > now that is a
	 * "multiple" of the log rotation interval.  "Multiple" can be interpreted
	 * fairly loosely.  In this version we align to log_timezone rather than
	 * GMT.
	 */
	rotinterval = Log_RotationAge * SECS_PER_MINUTE;	/* convert to seconds */
	now = (pg_time_t) time(NULL);
	tm = pg_localtime(&now, log_timezone);
	now += tm->tm_gmtoff;
	now -= now % rotinterval;
	now += rotinterval;
	now -= tm->tm_gmtoff;
	next_rotation_time = now;
}

/*
 * Store the name of the file(s) where the log collector, when enabled, writes
 * log messages.  Useful for finding the name(s) of the current log file(s)
 * when there is time-based logfile rotation.  Filenames are stored in a
 * temporary file and which is renamed into the final destination for
 * atomicity.  The file is opened with the same permissions as what gets
 * created in the data directory and has proper buffering options.
 */
static void
update_metainfo_datafile(void)
{
	FILE	   *fh;
	mode_t		oumask;

	if (!(Log_destination & LOG_DESTINATION_STDERR) &&
		!(Log_destination & LOG_DESTINATION_CSVLOG) &&
		!(Log_destination & LOG_DESTINATION_JSONLOG))
	{
		if (unlink(LOG_METAINFO_DATAFILE) < 0 && errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							LOG_METAINFO_DATAFILE)));
		return;
	}

	/* use the same permissions as the data directory for the new file */
	oumask = umask(pg_mode_mask);
	fh = fopen(LOG_METAINFO_DATAFILE_TMP, "w");
	umask(oumask);

	if (fh)
	{
		setvbuf(fh, NULL, PG_IOLBF, 0);

#ifdef WIN32
		/* use CRLF line endings on Windows */
		_setmode(_fileno(fh), _O_TEXT);
#endif
	}
	else
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						LOG_METAINFO_DATAFILE_TMP)));
		return;
	}

	if (last_sys_file_name && (Log_destination & LOG_DESTINATION_STDERR))
	{
		if (fprintf(fh, "stderr %s\n", last_sys_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}

	if (last_csv_file_name && (Log_destination & LOG_DESTINATION_CSVLOG))
	{
		if (fprintf(fh, "csvlog %s\n", last_csv_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}

	if (last_json_file_name && (Log_destination & LOG_DESTINATION_JSONLOG))
	{
		if (fprintf(fh, "jsonlog %s\n", last_json_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}
	fclose(fh);

	if (rename(LOG_METAINFO_DATAFILE_TMP, LOG_METAINFO_DATAFILE) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						LOG_METAINFO_DATAFILE_TMP, LOG_METAINFO_DATAFILE)));
}

/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/*
 * Check to see if a log rotation request has arrived.  Should be
 * called by postmaster after receiving SIGUSR1.
 */
bool
CheckLogrotateSignal(void)
{
	struct stat stat_buf;

	if (stat(LOGROTATE_SIGNAL_FILE, &stat_buf) == 0)
		return true;

	return false;
}

/*
 * Remove the file signaling a log rotation request.
 */
void
RemoveLogrotateSignalFiles(void)
{
	unlink(LOGROTATE_SIGNAL_FILE);
}

/* SIGUSR1: set flag to rotate logfile */
static void
sigUsr1Handler(SIGNAL_ARGS)
{
	rotation_requested = true;
	SetLatch(MyLatch);
}
