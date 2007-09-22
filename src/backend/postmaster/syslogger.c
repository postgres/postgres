/*-------------------------------------------------------------------------
 *
 * syslogger.c
 *
 * The system logger (syslogger) is new in Postgres 8.0. It catches all
 * stderr output from the postmaster, backends, and other subprocesses
 * by redirecting to a pipe, and writes it to a set of logfiles.
 * It's possible to have size and age limits for the logfile configured
 * in postgresql.conf. If these limits are reached or passed, the
 * current logfile is closed and a new one is created (rotated).
 * The logfiles are stored in a subdirectory (configurable in
 * postgresql.conf), using an internal naming scheme that mangles
 * creation time and current postmaster pid.
 *
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * Copyright (c) 2004-2006, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/postmaster/syslogger.c,v 1.29.2.4 2007/09/22 18:19:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgtime.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/timestamp.h"

/*
 * We really want line-buffered mode for logfile output, but Windows does
 * not have it, and interprets _IOLBF as _IOFBF (bozos).  So use _IONBF
 * instead on Windows.
 */
#ifdef WIN32
#define LBF_MODE	_IONBF
#else
#define LBF_MODE	_IOLBF
#endif

/* 
 * We read() into a temp buffer twice as big as a chunk, so that any fragment
 * left after processing can be moved down to the front and we'll still have
 * room to read a full chunk.
 */
#define READ_BUF_SIZE (2 * PIPE_CHUNK_SIZE)


/*
 * GUC parameters.	Redirect_stderr cannot be changed after postmaster
 * start, but the rest can change at SIGHUP.
 */
bool		Redirect_stderr = false;
int			Log_RotationAge = HOURS_PER_DAY * MINS_PER_HOUR;
int			Log_RotationSize = 10 * 1024;
char	   *Log_directory = NULL;
char	   *Log_filename = NULL;
bool		Log_truncate_on_rotation = false;

/*
 * Globally visible state (used by elog.c)
 */
bool		am_syslogger = false;

extern bool redirection_done;

/*
 * Private state
 */
static pg_time_t next_rotation_time;
static bool pipe_eof_seen = false;
static FILE *syslogFile = NULL;
static char *last_file_name = NULL;

/* 
 * Buffers for saving partial messages from different backends. We don't expect
 * that there will be very many outstanding at one time, so 20 seems plenty of
 * leeway. If this array gets full we won't lose messages, but we will lose
 * the protocol protection against them being partially written or interleaved.
 *
 * An inactive buffer has pid == 0 and undefined contents of data.
 */
typedef struct
{
	int32	pid;				/* PID of source process */
	StringInfoData data;		/* accumulated data, as a StringInfo */
} save_buffer;

#define CHUNK_SLOTS 20
static save_buffer saved_chunks[CHUNK_SLOTS];

/* These must be exported for EXEC_BACKEND case ... annoying */
#ifndef WIN32
int			syslogPipe[2] = {-1, -1};
#else
HANDLE		syslogPipe[2] = {0, 0};
#endif

#ifdef WIN32
static HANDLE threadHandle = 0;
static CRITICAL_SECTION sysfileSection;
#endif
static void process_pipe_input(char *logbuffer, int *bytes_in_logbuffer);
static void flush_pipe_input(char *logbuffer, int *bytes_in_logbuffer);

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t rotation_requested = false;


/* Local subroutines */
#ifdef EXEC_BACKEND
static pid_t syslogger_forkexec(void);
static void syslogger_parseArgs(int argc, char *argv[]);
#endif
static void write_syslogger_file_binary(const char *buffer, int count);

#ifdef WIN32
static unsigned int __stdcall pipeThread(void *arg);
#endif
static void logfile_rotate(bool time_based_rotation);
static char *logfile_getname(pg_time_t timestamp);
static void set_next_rotation_time(void);
static void sigHupHandler(SIGNAL_ARGS);
static void sigUsr1Handler(SIGNAL_ARGS);


/*
 * Main entry point for syslogger process
 * argc/argv parameters are valid only in EXEC_BACKEND case.
 */
NON_EXEC_STATIC void
SysLoggerMain(int argc, char *argv[])
{
#ifndef WIN32
	char		logbuffer[READ_BUF_SIZE];
	int			bytes_in_logbuffer = 0;
#endif
	char	   *currentLogDir;
	char	   *currentLogFilename;
	int			currentLogRotationAge;

	IsUnderPostmaster = true;	/* we are a postmaster subprocess now */

	MyProcPid = getpid();		/* reset MyProcPid */

#ifdef EXEC_BACKEND
	syslogger_parseArgs(argc, argv);
#endif   /* EXEC_BACKEND */

	am_syslogger = true;

	init_ps_display("logger process", "", "", "");

	/*
	 * If we restarted, our stderr is already redirected into our own input
	 * pipe.  This is of course pretty useless, not to mention that it
	 * interferes with detecting pipe EOF.	Point stderr to /dev/null. This
	 * assumes that all interesting messages generated in the syslogger will
	 * come through elog.c and will be sent to write_syslogger_file.
	 */
	if (redirection_done)
	{
		int			fd = open(NULL_DEV, O_WRONLY, 0);

		/*
		 * The closes might look redundant, but they are not: we want to be
		 * darn sure the pipe gets closed even if the open failed.	We can
		 * survive running with stderr pointing nowhere, but we can't afford
		 * to have extra pipe input descriptors hanging around.
		 */
		close(fileno(stdout));
		close(fileno(stderr));
		dup2(fd, fileno(stdout));
		dup2(fd, fileno(stderr));
		close(fd);
	}

	/* Syslogger's own stderr can't be the syslogPipe, so set it back to
	 * text mode if we didn't just close it. 
	 * (It was set to binary in SubPostmasterMain).
	 */
#ifdef WIN32
	else
		_setmode(_fileno(stderr),_O_TEXT);
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
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.  (syslogger probably never has
	 * any child processes, but for consistency we make all postmaster
	 * child processes do this.)
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * Note: we ignore all termination signals, and instead exit only when all
	 * upstream processes are gone, to ensure we don't miss any dying gasps of
	 * broken backends...
	 */

	pqsignal(SIGHUP, sigHupHandler);	/* set flag to read config file */
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
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);

	PG_SETMASK(&UnBlockSig);

#ifdef WIN32
	/* Fire up separate data transfer thread */
	InitializeCriticalSection(&sysfileSection);

	{
		unsigned int tid;

		threadHandle = (HANDLE) _beginthreadex(0, 0, pipeThread, 0, 0, &tid);
	}
#endif   /* WIN32 */

	/* remember active logfile parameters */
	currentLogDir = pstrdup(Log_directory);
	currentLogFilename = pstrdup(Log_filename);
	currentLogRotationAge = Log_RotationAge;
	/* set next planned rotation time */
	set_next_rotation_time();

	/* main worker loop */
	for (;;)
	{
		bool		time_based_rotation = false;

#ifndef WIN32
		int			bytesRead;
		int			rc;
		fd_set		rfds;
		struct timeval timeout;
#endif

		if (got_SIGHUP)
		{
			got_SIGHUP = false;
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
			}
			if (strcmp(Log_filename, currentLogFilename) != 0)
			{
				pfree(currentLogFilename);
				currentLogFilename = pstrdup(Log_filename);
				rotation_requested = true;
			}

			/*
			 * If rotation time parameter changed, reset next rotation time,
			 * but don't immediately force a rotation.
			 */
			if (currentLogRotationAge != Log_RotationAge)
			{
				currentLogRotationAge = Log_RotationAge;
				set_next_rotation_time();
			}
		}

		if (!rotation_requested && Log_RotationAge > 0)
		{
			/* Do a logfile rotation if it's time */
			pg_time_t	now = time(NULL);

			if (now >= next_rotation_time)
				rotation_requested = time_based_rotation = true;
		}

		if (!rotation_requested && Log_RotationSize > 0)
		{
			/* Do a rotation if file is too big */
			if (ftell(syslogFile) >= Log_RotationSize * 1024L)
				rotation_requested = true;
		}

		if (rotation_requested)
			logfile_rotate(time_based_rotation);

#ifndef WIN32

		/*
		 * Wait for some data, timing out after 1 second
		 */
		FD_ZERO(&rfds);
		FD_SET(syslogPipe[0], &rfds);
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		rc = select(syslogPipe[0] + 1, &rfds, NULL, NULL, &timeout);

		if (rc < 0)
		{
			if (errno != EINTR)
				ereport(LOG,
						(errcode_for_socket_access(),
						 errmsg("select() failed in logger process: %m")));
		}
		else if (rc > 0 && FD_ISSET(syslogPipe[0], &rfds))
		{
			bytesRead = piperead(syslogPipe[0],
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
		 * detect pipe EOF.  The main thread just wakes up once a second to
		 * check for SIGHUP and rotation conditions.
		 */
		pg_usleep(1000000L);
#endif   /* WIN32 */

		if (pipe_eof_seen)
		{
			ereport(LOG,
					(errmsg("logger shutting down")));

			/*
			 * Normal exit from the syslogger is here.	Note that we
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

	if (!Redirect_stderr)
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
	 */
#ifndef WIN32
	if (syslogPipe[0] < 0)
	{
		if (pgpipe(syslogPipe) < 0)
			ereport(FATAL,
					(errcode_for_socket_access(),
					 (errmsg("could not create pipe for syslog: %m"))));
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
					 (errmsg("could not create pipe for syslog: %m"))));
	}
#endif

	/*
	 * Create log directory if not present; ignore errors
	 */
	mkdir(Log_directory, 0700);

	/*
	 * The initial logfile is created right in the postmaster, to verify that
	 * the Log_directory is writable.
	 */
	filename = logfile_getname(time(NULL));

	syslogFile = fopen(filename, "a");

	if (!syslogFile)
		ereport(FATAL,
				(errcode_for_file_access(),
				 (errmsg("could not create log file \"%s\": %m",
						 filename))));

	setvbuf(syslogFile, NULL, LBF_MODE, 0);

	pfree(filename);

#ifdef EXEC_BACKEND
	switch ((sysloggerPid = syslogger_forkexec()))
#else
	switch ((sysloggerPid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork system logger: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(true);

			/* Lose the postmaster's on-exit routines */
			on_exit_reset();

			/* Drop our connection to postmaster's shared memory, as well */
			PGSharedMemoryDetach();

			/* do the work */
			SysLoggerMain(0, NULL);
			break;
#endif

		default:
			/* success, in postmaster */

			/* now we redirect stderr, if not done already */
			if (!redirection_done)
			{
#ifndef WIN32
				fflush(stdout);
				if (dup2(syslogPipe[1], fileno(stdout)) < 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not redirect stdout: %m")));
				fflush(stderr);
				if (dup2(syslogPipe[1], fileno(stderr)) < 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not redirect stderr: %m")));
				/* Now we are done with the write end of the pipe. */
				close(syslogPipe[1]);
				syslogPipe[1] = -1;
#else
				int			fd;

				/*
				 * open the pipe in binary mode and make sure
				 * stderr is binary after it's been dup'ed into, to avoid
				 * disturbing the pipe chunking protocol.
				 */
				fflush(stderr);
				fd = _open_osfhandle((long) syslogPipe[1],
									 _O_APPEND | _O_BINARY);
				if (dup2(fd, _fileno(stderr)) < 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not redirect stderr: %m")));
				close(fd);
				_setmode(_fileno(stderr),_O_BINARY);
				/* Now we are done with the write end of the pipe. */
				CloseHandle(syslogPipe[1]);
				syslogPipe[1] = 0;
#endif
				redirection_done = true;
			}

			/* postmaster will never write the file; close it */
			fclose(syslogFile);
			syslogFile = NULL;
			return (int) sysloggerPid;
	}

	/* we should never reach here */
	return 0;
}


#ifdef EXEC_BACKEND

/*
 * syslogger_forkexec() -
 *
 * Format up the arglist for, then fork and exec, a syslogger process
 */
static pid_t
syslogger_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;
	char		filenobuf[32];

	av[ac++] = "postgres";
	av[ac++] = "--forklog";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */

	/* static variables (those not passed by write_backend_variables) */
#ifndef WIN32
	if (syslogFile != NULL)
		snprintf(filenobuf, sizeof(filenobuf), "%d",
				 fileno(syslogFile));
	else
		strcpy(filenobuf, "-1");
#else							/* WIN32 */
	if (syslogFile != NULL)
		snprintf(filenobuf, sizeof(filenobuf), "%ld",
				 _get_osfhandle(_fileno(syslogFile)));
	else
		strcpy(filenobuf, "0");
#endif   /* WIN32 */
	av[ac++] = filenobuf;

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}

/*
 * syslogger_parseArgs() -
 *
 * Extract data from the arglist for exec'ed syslogger process
 */
static void
syslogger_parseArgs(int argc, char *argv[])
{
	int			fd;

	Assert(argc == 4);
	argv += 3;

#ifndef WIN32
	fd = atoi(*argv++);
	if (fd != -1)
	{
		syslogFile = fdopen(fd, "a");
		setvbuf(syslogFile, NULL, LBF_MODE, 0);
	}
#else							/* WIN32 */
	fd = atoi(*argv++);
	if (fd != 0)
	{
		fd = _open_osfhandle(fd, _O_APPEND | _O_TEXT);
		if (fd > 0)
		{
			syslogFile = fdopen(fd, "a");
			setvbuf(syslogFile, NULL, LBF_MODE, 0);
		}
	}
#endif   /* WIN32 */
}
#endif   /* EXEC_BACKEND */


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
	char   *cursor = logbuffer;
	int		count = *bytes_in_logbuffer;

	/* While we have enough for a header, process data... */
	while (count >= (int) sizeof(PipeProtoHeader))
	{
		PipeProtoHeader p;
		int		chunklen;

		/* Do we have a valid header? */
		memcpy(&p, cursor, sizeof(PipeProtoHeader));
		if (p.nuls[0] == '\0' && p.nuls[1] == '\0' &&
			p.len > 0 && p.len <= PIPE_MAX_PAYLOAD &&
			p.pid != 0 &&
			(p.is_last == 't' || p.is_last == 'f'))
		{
			chunklen = PIPE_HEADER_SIZE + p.len;

			/* Fall out of loop if we don't have the whole chunk yet */
			if (count < chunklen)
				break;

			if (p.is_last == 'f')
			{
				/* 
				 * Save a complete non-final chunk in the per-pid buffer 
				 * if possible - if not just write it out.
				 */
				int free_slot = -1, existing_slot = -1;
				int i;
				StringInfo str;

				for (i = 0; i < CHUNK_SLOTS; i++)
				{
					if (saved_chunks[i].pid == p.pid)
					{
						existing_slot = i;
						break;
					}
					if (free_slot < 0 && saved_chunks[i].pid == 0)
						free_slot = i;
				}
				if (existing_slot >= 0)
				{
					str = &(saved_chunks[existing_slot].data);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE, 
										   p.len);
				}
				else if (free_slot >= 0)
				{
					saved_chunks[free_slot].pid = p.pid;
					str = &(saved_chunks[free_slot].data);
					initStringInfo(str);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE, 
										   p.len);
				}
				else
				{
					/* 
					 * If there is no free slot we'll just have to take our
					 * chances and write out a partial message and hope that
					 * it's not followed by something from another pid.
					 */
					write_syslogger_file(cursor + PIPE_HEADER_SIZE, p.len);
				}
			}
			else
			{
				/* 
				 * Final chunk --- add it to anything saved for that pid, and
				 * either way write the whole thing out.
				 */
				int existing_slot = -1;
				int i;
				StringInfo str;

				for (i = 0; i < CHUNK_SLOTS; i++)
				{
					if (saved_chunks[i].pid == p.pid)
					{
						existing_slot = i;
						break;
					}
				}
				if (existing_slot >= 0)
				{
					str = &(saved_chunks[existing_slot].data);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
					write_syslogger_file(str->data, str->len);
					saved_chunks[existing_slot].pid = 0;
					pfree(str->data);
				}
				else
				{
					/* The whole message was one chunk, evidently. */
					write_syslogger_file(cursor + PIPE_HEADER_SIZE, p.len);
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
			 * fall out of the loop.  (Note: we want to dump it all if
			 * at all possible, so as to avoid dividing non-protocol messages
			 * across logfiles.  We expect that in many scenarios, a
			 * non-protocol message will arrive all in one read(), and we
			 * want to respect the read() boundary if possible.)
			 */
			for (chunklen = 1; chunklen < count; chunklen++)
			{
				if (cursor[chunklen] == '\0')
					break;
			}
			write_syslogger_file(cursor, chunklen);
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
	int i;
	StringInfo str;

	/* Dump any incomplete protocol messages */
	for (i = 0; i < CHUNK_SLOTS; i++)
	{
		if (saved_chunks[i].pid != 0)
		{
			str = &(saved_chunks[i].data);
			write_syslogger_file(str->data, str->len);
			saved_chunks[i].pid = 0;
			pfree(str->data);
		}
	}
	/*
	 * Force out any remaining pipe data as-is; we don't bother trying to
	 * remove any protocol headers that may exist in it.
	 */
	if (*bytes_in_logbuffer > 0)
		write_syslogger_file(logbuffer, *bytes_in_logbuffer);
	*bytes_in_logbuffer = 0;
}


/* --------------------------------
 *		logfile routines
 * --------------------------------
 */

/*
 * Write text to the currently open logfile
 *
 * This is exported so that elog.c can call it when am_syslogger is true.
 * This allows the syslogger process to record elog messages of its own,
 * even though its stderr does not point at the syslog pipe.
 */
void
write_syslogger_file(const char *buffer, int count)
{
#ifdef WIN32

	/*
	 * On Windows we need to do our own newline-to-CRLF translation.
	 */
	char		convbuf[256];
	char	   *p;
	int			n;

	p = convbuf;
	n = 0;
	while (count-- > 0)
	{
		if (*buffer == '\n')
		{
			*p++ = '\r';
			n++;
		}
		*p++ = *buffer++;
		n++;
		if (n >= sizeof(convbuf) - 1)
		{
			write_syslogger_file_binary(convbuf, n);
			p = convbuf;
			n = 0;
		}
	}
	if (n > 0)
		write_syslogger_file_binary(convbuf, n);
#else							/* !WIN32 */
	write_syslogger_file_binary(buffer, count);
#endif
}

/*
 * Write binary data to the currently open logfile
 *
 * On Windows the data arriving in the pipe already has CR/LF newlines,
 * so we must send it to the file without further translation.
 */
static void
write_syslogger_file_binary(const char *buffer, int count)
{
	int			rc;

#ifndef WIN32
	rc = fwrite(buffer, 1, count, syslogFile);
#else
	EnterCriticalSection(&sysfileSection);
	rc = fwrite(buffer, 1, count, syslogFile);
	LeaveCriticalSection(&sysfileSection);
#endif

	/* can't use ereport here because of possible recursion */
	if (rc != count)
		write_stderr("could not write to log file: %s\n", strerror(errno));
}

#ifdef WIN32

/*
 * Worker thread to transfer data from the pipe to the current logfile.
 *
 * We need this because on Windows, WaitForSingleObject does not work on
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

		if (!ReadFile(syslogPipe[0],
					  logbuffer + bytes_in_logbuffer,
					  sizeof(logbuffer) - bytes_in_logbuffer,
					  &bytesRead, 0))
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
	}

	/* We exit the above loop only upon detecting pipe EOF */
	pipe_eof_seen = true;

	/* if there's any data left then force it out now */
	flush_pipe_input(logbuffer, &bytes_in_logbuffer);
	
	_endthread();
	return 0;
}
#endif   /* WIN32 */

/*
 * perform logfile rotation
 */
static void
logfile_rotate(bool time_based_rotation)
{
	char	   *filename;
	FILE	   *fh;

	rotation_requested = false;

	/*
	 * When doing a time-based rotation, invent the new logfile name based on
	 * the planned rotation time, not current time, to avoid "slippage" in the
	 * file name when we don't do the rotation immediately.
	 */
	if (time_based_rotation)
		filename = logfile_getname(next_rotation_time);
	else
		filename = logfile_getname(time(NULL));

	/*
	 * Decide whether to overwrite or append.  We can overwrite if (a)
	 * Log_truncate_on_rotation is set, (b) the rotation was triggered by
	 * elapsed time and not something else, and (c) the computed file name is
	 * different from what we were previously logging into.
	 *
	 * Note: during the first rotation after forking off from the postmaster,
	 * last_file_name will be NULL.  (We don't bother to set it in the
	 * postmaster because it ain't gonna work in the EXEC_BACKEND case.) So we
	 * will always append in that situation, even though truncating would
	 * usually be safe.
	 */
	if (Log_truncate_on_rotation && time_based_rotation &&
		last_file_name != NULL && strcmp(filename, last_file_name) != 0)
		fh = fopen(filename, "w");
	else
		fh = fopen(filename, "a");

	if (!fh)
	{
		int			saveerrno = errno;

		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open new log file \"%s\": %m",
						filename)));

		/*
		 * ENFILE/EMFILE are not too surprising on a busy system; just keep
		 * using the old file till we manage to get a new one. Otherwise,
		 * assume something's wrong with Log_directory and stop trying to
		 * create files.
		 */
		if (saveerrno != ENFILE && saveerrno != EMFILE)
		{
			ereport(LOG,
					(errmsg("disabling automatic rotation (use SIGHUP to reenable)")));
			Log_RotationAge = 0;
			Log_RotationSize = 0;
		}
		pfree(filename);
		return;
	}

	setvbuf(fh, NULL, LBF_MODE, 0);

#ifdef WIN32
	_setmode(_fileno(fh), _O_TEXT); /* use CRLF line endings on Windows */
#endif

	/* On Windows, need to interlock against data-transfer thread */
#ifdef WIN32
	EnterCriticalSection(&sysfileSection);
#endif
	fclose(syslogFile);
	syslogFile = fh;
#ifdef WIN32
	LeaveCriticalSection(&sysfileSection);
#endif

	set_next_rotation_time();

	/* instead of pfree'ing filename, remember it for next time */
	if (last_file_name != NULL)
		pfree(last_file_name);
	last_file_name = filename;
}


/*
 * construct logfile name using timestamp information
 *
 * Result is palloc'd.
 */
static char *
logfile_getname(pg_time_t timestamp)
{
	char	   *filename;
	int			len;
	struct pg_tm *tm;

	filename = palloc(MAXPGPATH);

	snprintf(filename, MAXPGPATH, "%s/", Log_directory);

	len = strlen(filename);

	if (strchr(Log_filename, '%'))
	{
		/* treat it as a strftime pattern */
		tm = pg_localtime(&timestamp, global_timezone);
		pg_strftime(filename + len, MAXPGPATH - len, Log_filename, tm);
	}
	else
	{
		/* no strftime escapes, so append timestamp to new filename */
		snprintf(filename + len, MAXPGPATH - len, "%s.%lu",
				 Log_filename, (unsigned long) timestamp);
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
	 * fairly loosely.	In this version we align to local time rather than
	 * GMT.
	 */
	rotinterval = Log_RotationAge * SECS_PER_MINUTE;	/* convert to seconds */
	now = time(NULL);
	tm = pg_localtime(&now, global_timezone);
	now += tm->tm_gmtoff;
	now -= now % rotinterval;
	now += rotinterval;
	now -= tm->tm_gmtoff;
	next_rotation_time = now;
}

/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/* SIGHUP: set flag to reload config file */
static void
sigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
}

/* SIGUSR1: set flag to rotate logfile */
static void
sigUsr1Handler(SIGNAL_ARGS)
{
	rotation_requested = true;
}
