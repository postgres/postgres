/*-------------------------------------------------------------------------
 *
 * pg_basebackup.c - receive a base backup using streaming replication protocol
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_basebackup.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "common/string.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "pgtar.h"
#include "pgtime.h"
#include "receivelog.h"
#include "replication/basebackup.h"
#include "streamutil.h"


#define atooid(x)  ((Oid) strtoul((x), NULL, 10))

typedef struct TablespaceListCell
{
	struct TablespaceListCell *next;
	char		old_dir[MAXPGPATH];
	char		new_dir[MAXPGPATH];
} TablespaceListCell;

typedef struct TablespaceList
{
	TablespaceListCell *head;
	TablespaceListCell *tail;
} TablespaceList;

/* Global options */
static char *basedir = NULL;
static TablespaceList tablespace_dirs = {NULL, NULL};
static char *xlog_dir = "";
static char format = 'p';		/* p(lain)/t(ar) */
static char *label = "pg_basebackup base backup";
static bool noclean = false;
static bool showprogress = false;
static int	verbose = 0;
static int	compresslevel = 0;
static bool includewal = false;
static bool streamwal = false;
static bool fastcheckpoint = false;
static bool writerecoveryconf = false;
static int	standby_message_timeout = 10 * 1000;		/* 10 sec = default */
static pg_time_t last_progress_report = 0;
static int32 maxrate = 0;		/* no limit by default */

static bool success = false;
static bool made_new_pgdata = false;
static bool found_existing_pgdata = false;
static bool made_new_xlogdir = false;
static bool found_existing_xlogdir = false;
static bool made_tablespace_dirs = false;
static bool found_tablespace_dirs = false;

/* Progress counters */
static uint64 totalsize;
static uint64 totaldone;
static int	tablespacecount;

/* Pipe to communicate with background wal receiver process */
#ifndef WIN32
static int	bgpipe[2] = {-1, -1};
#endif

/* Handle to child process */
static pid_t bgchild = -1;
static bool in_log_streamer = false;

/* End position for xlog streaming, empty string if unknown yet */
static XLogRecPtr xlogendptr;

#ifndef WIN32
static int	has_xlogendptr = 0;
#else
static volatile LONG has_xlogendptr = 0;
#endif

/* Contents of recovery.conf to be generated */
static PQExpBuffer recoveryconfcontents = NULL;

/* Function headers */
static void usage(void);
static void disconnect_and_exit(int code);
static void verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found);
static void progress_report(int tablespacenum, const char *filename, bool force);

static void ReceiveTarFile(PGconn *conn, PGresult *res, int rownum);
static void ReceiveAndUnpackTarFile(PGconn *conn, PGresult *res, int rownum);
static void GenerateRecoveryConf(PGconn *conn);
static void WriteRecoveryConf(void);
static void BaseBackup(void);

static bool reached_end_position(XLogRecPtr segendpos, uint32 timeline,
					 bool segment_finished);

static const char *get_tablespace_mapping(const char *dir);
static void tablespace_list_append(const char *arg);


static void
cleanup_directories_atexit(void)
{
	if (success || in_log_streamer)
		return;

	if (!noclean)
	{
		if (made_new_pgdata)
		{
			fprintf(stderr, _("%s: removing data directory \"%s\"\n"),
					progname, basedir);
			if (!rmtree(basedir, true))
				fprintf(stderr, _("%s: failed to remove data directory\n"),
						progname);
		}
		else if (found_existing_pgdata)
		{
			fprintf(stderr,
					_("%s: removing contents of data directory \"%s\"\n"),
					progname, basedir);
			if (!rmtree(basedir, false))
				fprintf(stderr, _("%s: failed to remove contents of data directory\n"),
						progname);
		}

		if (made_new_xlogdir)
		{
			fprintf(stderr, _("%s: removing transaction log directory \"%s\"\n"),
					progname, xlog_dir);
			if (!rmtree(xlog_dir, true))
				fprintf(stderr, _("%s: failed to remove transaction log directory\n"),
						progname);
		}
		else if (found_existing_xlogdir)
		{
			fprintf(stderr,
					_("%s: removing contents of transaction log directory \"%s\"\n"),
					progname, xlog_dir);
			if (!rmtree(xlog_dir, false))
				fprintf(stderr, _("%s: failed to remove contents of transaction log directory\n"),
						progname);
		}
	}
	else
	{
		if (made_new_pgdata || found_existing_pgdata)
			fprintf(stderr,
			  _("%s: data directory \"%s\" not removed at user's request\n"),
					progname, basedir);

		if (made_new_xlogdir || found_existing_xlogdir)
			fprintf(stderr,
					_("%s: transaction log directory \"%s\" not removed at user's request\n"),
					progname, xlog_dir);
	}

	if (made_tablespace_dirs || found_tablespace_dirs)
		fprintf(stderr,
				_("%s: changes to tablespace directories will not be undone"),
				progname);
}

static void
disconnect_and_exit(int code)
{
	if (conn != NULL)
		PQfinish(conn);

#ifndef WIN32

	/*
	 * On windows, our background thread dies along with the process. But on
	 * Unix, if we have started a subprocess, we want to kill it off so it
	 * doesn't remain running trying to stream data.
	 */
	if (bgchild > 0)
		kill(bgchild, SIGTERM);
#endif

	exit(code);
}


/*
 * Split argument into old_dir and new_dir and append to tablespace mapping
 * list.
 */
static void
tablespace_list_append(const char *arg)
{
	TablespaceListCell *cell = (TablespaceListCell *) pg_malloc0(sizeof(TablespaceListCell));
	char	   *dst;
	char	   *dst_ptr;
	const char *arg_ptr;

	dst_ptr = dst = cell->old_dir;
	for (arg_ptr = arg; *arg_ptr; arg_ptr++)
	{
		if (dst_ptr - dst >= MAXPGPATH)
		{
			fprintf(stderr, _("%s: directory name too long\n"), progname);
			exit(1);
		}

		if (*arg_ptr == '\\' && *(arg_ptr + 1) == '=')
			;					/* skip backslash escaping = */
		else if (*arg_ptr == '=' && (arg_ptr == arg || *(arg_ptr - 1) != '\\'))
		{
			if (*cell->new_dir)
			{
				fprintf(stderr, _("%s: multiple \"=\" signs in tablespace mapping\n"), progname);
				exit(1);
			}
			else
				dst = dst_ptr = cell->new_dir;
		}
		else
			*dst_ptr++ = *arg_ptr;
	}

	if (!*cell->old_dir || !*cell->new_dir)
	{
		fprintf(stderr,
				_("%s: invalid tablespace mapping format \"%s\", must be \"OLDDIR=NEWDIR\"\n"),
				progname, arg);
		exit(1);
	}

	/*
	 * This check isn't absolutely necessary.  But all tablespaces are created
	 * with absolute directories, so specifying a non-absolute path here would
	 * just never match, possibly confusing users.  It's also good to be
	 * consistent with the new_dir check.
	 */
	if (!is_absolute_path(cell->old_dir))
	{
		fprintf(stderr, _("%s: old directory is not an absolute path in tablespace mapping: %s\n"),
				progname, cell->old_dir);
		exit(1);
	}

	if (!is_absolute_path(cell->new_dir))
	{
		fprintf(stderr, _("%s: new directory is not an absolute path in tablespace mapping: %s\n"),
				progname, cell->new_dir);
		exit(1);
	}

	canonicalize_path(cell->old_dir);
	canonicalize_path(cell->new_dir);

	if (tablespace_dirs.tail)
		tablespace_dirs.tail->next = cell;
	else
		tablespace_dirs.head = cell;
	tablespace_dirs.tail = cell;
}


#ifdef HAVE_LIBZ
static const char *
get_gz_error(gzFile gzf)
{
	int			errnum;
	const char *errmsg;

	errmsg = gzerror(gzf, &errnum);
	if (errnum == Z_ERRNO)
		return strerror(errno);
	else
		return errmsg;
}
#endif

static void
usage(void)
{
	printf(_("%s takes a base backup of a running PostgreSQL server.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions controlling the output:\n"));
	printf(_("  -D, --pgdata=DIRECTORY receive base backup into directory\n"));
	printf(_("  -F, --format=p|t       output format (plain (default), tar)\n"));
	printf(_("  -r, --max-rate=RATE    maximum transfer rate to transfer data directory\n"
	  "                         (in kB/s, or use suffix \"k\" or \"M\")\n"));
	printf(_("  -R, --write-recovery-conf\n"
			 "                         write recovery.conf after backup\n"));
	printf(_("  -S, --slot=SLOTNAME    replication slot to use\n"));
	printf(_("  -T, --tablespace-mapping=OLDDIR=NEWDIR\n"
	  "                         relocate tablespace in OLDDIR to NEWDIR\n"));
	printf(_("  -x, --xlog             include required WAL files in backup (fetch mode)\n"));
	printf(_("  -X, --xlog-method=fetch|stream\n"
			 "                         include required WAL files with specified method\n"));
	printf(_("      --xlogdir=XLOGDIR  location for the transaction log directory\n"));
	printf(_("  -z, --gzip             compress tar output\n"));
	printf(_("  -Z, --compress=0-9     compress tar output with given compression level\n"));
	printf(_("\nGeneral options:\n"));
	printf(_("  -c, --checkpoint=fast|spread\n"
			 "                         set fast or spread checkpointing\n"));
	printf(_("  -l, --label=LABEL      set backup label\n"));
	printf(_("  -n, --noclean          do not clean up after errors\n"));
	printf(_("  -P, --progress         show progress information\n"));
	printf(_("  -v, --verbose          output verbose messages\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=CONNSTR   connection string\n"));
	printf(_("  -h, --host=HOSTNAME    database server host or socket directory\n"));
	printf(_("  -p, --port=PORT        database server port number\n"));
	printf(_("  -s, --status-interval=INTERVAL\n"
			 "                         time between status packets sent to server (in seconds)\n"));
	printf(_("  -U, --username=NAME    connect as specified database user\n"));
	printf(_("  -w, --no-password      never prompt for password\n"));
	printf(_("  -W, --password         force password prompt (should happen automatically)\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}


/*
 * Called in the background process every time data is received.
 * On Unix, we check to see if there is any data on our pipe
 * (which would mean we have a stop position), and if it is, check if
 * it is time to stop.
 * On Windows, we are in a single process, so we can just check if it's
 * time to stop.
 */
static bool
reached_end_position(XLogRecPtr segendpos, uint32 timeline,
					 bool segment_finished)
{
	if (!has_xlogendptr)
	{
#ifndef WIN32
		fd_set		fds;
		struct timeval tv;
		int			r;

		/*
		 * Don't have the end pointer yet - check our pipe to see if it has
		 * been sent yet.
		 */
		FD_ZERO(&fds);
		FD_SET(bgpipe[0], &fds);

		MemSet(&tv, 0, sizeof(tv));

		r = select(bgpipe[0] + 1, &fds, NULL, NULL, &tv);
		if (r == 1)
		{
			char		xlogend[64];
			uint32		hi,
						lo;

			MemSet(xlogend, 0, sizeof(xlogend));
			r = read(bgpipe[0], xlogend, sizeof(xlogend) - 1);
			if (r < 0)
			{
				fprintf(stderr, _("%s: could not read from ready pipe: %s\n"),
						progname, strerror(errno));
				exit(1);
			}

			if (sscanf(xlogend, "%X/%X", &hi, &lo) != 2)
			{
				fprintf(stderr,
				  _("%s: could not parse transaction log location \"%s\"\n"),
						progname, xlogend);
				exit(1);
			}
			xlogendptr = ((uint64) hi) << 32 | lo;
			has_xlogendptr = 1;

			/*
			 * Fall through to check if we've reached the point further
			 * already.
			 */
		}
		else
		{
			/*
			 * No data received on the pipe means we don't know the end
			 * position yet - so just say it's not time to stop yet.
			 */
			return false;
		}
#else

		/*
		 * On win32, has_xlogendptr is set by the main thread, so if it's not
		 * set here, we just go back and wait until it shows up.
		 */
		return false;
#endif
	}

	/*
	 * At this point we have an end pointer, so compare it to the current
	 * position to figure out if it's time to stop.
	 */
	if (segendpos >= xlogendptr)
		return true;

	/*
	 * Have end pointer, but haven't reached it yet - so tell the caller to
	 * keep streaming.
	 */
	return false;
}

typedef struct
{
	PGconn	   *bgconn;
	XLogRecPtr	startptr;
	char		xlogdir[MAXPGPATH];
	char	   *sysidentifier;
	int			timeline;
} logstreamer_param;

static int
LogStreamerMain(logstreamer_param *param)
{
	StreamCtl	stream;

	in_log_streamer = true;

	MemSet(&stream, 0, sizeof(stream));
	stream.startpos = param->startptr;
	stream.timeline = param->timeline;
	stream.sysidentifier = param->sysidentifier;
	stream.stream_stop = reached_end_position;
	stream.standby_message_timeout = standby_message_timeout;
	stream.synchronous = false;
	stream.mark_done = true;
	stream.basedir = param->xlogdir;
	stream.partial_suffix = NULL;

	if (!ReceiveXlogStream(param->bgconn, &stream))

		/*
		 * Any errors will already have been reported in the function process,
		 * but we need to tell the parent that we didn't shutdown in a nice
		 * way.
		 */
		return 1;

	PQfinish(param->bgconn);
	return 0;
}

/*
 * Initiate background process for receiving xlog during the backup.
 * The background stream will use its own database connection so we can
 * stream the logfile in parallel with the backups.
 */
static void
StartLogStreamer(char *startpos, uint32 timeline, char *sysidentifier)
{
	logstreamer_param *param;
	uint32		hi,
				lo;
	char		statusdir[MAXPGPATH];

	param = pg_malloc0(sizeof(logstreamer_param));
	param->timeline = timeline;
	param->sysidentifier = sysidentifier;

	/* Convert the starting position */
	if (sscanf(startpos, "%X/%X", &hi, &lo) != 2)
	{
		fprintf(stderr,
				_("%s: could not parse transaction log location \"%s\"\n"),
				progname, startpos);
		disconnect_and_exit(1);
	}
	param->startptr = ((uint64) hi) << 32 | lo;
	/* Round off to even segment position */
	param->startptr -= param->startptr % XLOG_SEG_SIZE;

#ifndef WIN32
	/* Create our background pipe */
	if (pipe(bgpipe) < 0)
	{
		fprintf(stderr,
				_("%s: could not create pipe for background process: %s\n"),
				progname, strerror(errno));
		disconnect_and_exit(1);
	}
#endif

	/* Get a second connection */
	param->bgconn = GetConnection();
	if (!param->bgconn)
		/* Error message already written in GetConnection() */
		exit(1);

	snprintf(param->xlogdir, sizeof(param->xlogdir), "%s/pg_xlog", basedir);

	/*
	 * Create pg_xlog/archive_status (and thus pg_xlog) so we can write to
	 * basedir/pg_xlog as the directory entry in the tar file may arrive
	 * later.
	 */
	snprintf(statusdir, sizeof(statusdir), "%s/pg_xlog/archive_status",
			 basedir);

	if (pg_mkdir_p(statusdir, S_IRWXU) != 0 && errno != EEXIST)
	{
		fprintf(stderr,
				_("%s: could not create directory \"%s\": %s\n"),
				progname, statusdir, strerror(errno));
		disconnect_and_exit(1);
	}

	/*
	 * Start a child process and tell it to start streaming. On Unix, this is
	 * a fork(). On Windows, we create a thread.
	 */
#ifndef WIN32
	bgchild = fork();
	if (bgchild == 0)
	{
		/* in child process */
		exit(LogStreamerMain(param));
	}
	else if (bgchild < 0)
	{
		fprintf(stderr, _("%s: could not create background process: %s\n"),
				progname, strerror(errno));
		disconnect_and_exit(1);
	}

	/*
	 * Else we are in the parent process and all is well.
	 */
#else							/* WIN32 */
	bgchild = _beginthreadex(NULL, 0, (void *) LogStreamerMain, param, 0, NULL);
	if (bgchild == 0)
	{
		fprintf(stderr, _("%s: could not create background thread: %s\n"),
				progname, strerror(errno));
		disconnect_and_exit(1);
	}
#endif
}

/*
 * Verify that the given directory exists and is empty. If it does not
 * exist, it is created. If it exists but is not empty, an error will
 * be give and the process ended.
 */
static void
verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found)
{
	switch (pg_check_dir(dirname))
	{
		case 0:

			/*
			 * Does not exist, so create
			 */
			if (pg_mkdir_p(dirname, S_IRWXU) == -1)
			{
				fprintf(stderr,
						_("%s: could not create directory \"%s\": %s\n"),
						progname, dirname, strerror(errno));
				disconnect_and_exit(1);
			}
			if (created)
				*created = true;
			return;
		case 1:

			/*
			 * Exists, empty
			 */
			if (found)
				*found = true;
			return;
		case 2:
		case 3:
		case 4:

			/*
			 * Exists, not empty
			 */
			fprintf(stderr,
					_("%s: directory \"%s\" exists but is not empty\n"),
					progname, dirname);
			disconnect_and_exit(1);
		case -1:

			/*
			 * Access problem
			 */
			fprintf(stderr, _("%s: could not access directory \"%s\": %s\n"),
					progname, dirname, strerror(errno));
			disconnect_and_exit(1);
	}
}


/*
 * Print a progress report based on the global variables. If verbose output
 * is enabled, also print the current file name.
 *
 * Progress report is written at maximum once per second, unless the
 * force parameter is set to true.
 */
static void
progress_report(int tablespacenum, const char *filename, bool force)
{
	int			percent;
	char		totaldone_str[32];
	char		totalsize_str[32];
	pg_time_t	now;

	if (!showprogress)
		return;

	now = time(NULL);
	if (now == last_progress_report && !force)
		return;					/* Max once per second */

	last_progress_report = now;
	percent = totalsize ? (int) ((totaldone / 1024) * 100 / totalsize) : 0;

	/*
	 * Avoid overflowing past 100% or the full size. This may make the total
	 * size number change as we approach the end of the backup (the estimate
	 * will always be wrong if WAL is included), but that's better than having
	 * the done column be bigger than the total.
	 */
	if (percent > 100)
		percent = 100;
	if (totaldone / 1024 > totalsize)
		totalsize = totaldone / 1024;

	/*
	 * Separate step to keep platform-dependent format code out of
	 * translatable strings.  And we only test for INT64_FORMAT availability
	 * in snprintf, not fprintf.
	 */
	snprintf(totaldone_str, sizeof(totaldone_str), INT64_FORMAT,
			 totaldone / 1024);
	snprintf(totalsize_str, sizeof(totalsize_str), INT64_FORMAT, totalsize);

#define VERBOSE_FILENAME_LENGTH 35
	if (verbose)
	{
		if (!filename)

			/*
			 * No filename given, so clear the status line (used for last
			 * call)
			 */
			fprintf(stderr,
					ngettext("%*s/%s kB (100%%), %d/%d tablespace %*s",
							 "%*s/%s kB (100%%), %d/%d tablespaces %*s",
							 tablespacecount),
					(int) strlen(totalsize_str),
					totaldone_str, totalsize_str,
					tablespacenum, tablespacecount,
					VERBOSE_FILENAME_LENGTH + 5, "");
		else
		{
			bool		truncate = (strlen(filename) > VERBOSE_FILENAME_LENGTH);

			fprintf(stderr,
					ngettext("%*s/%s kB (%d%%), %d/%d tablespace (%s%-*.*s)",
							 "%*s/%s kB (%d%%), %d/%d tablespaces (%s%-*.*s)",
							 tablespacecount),
					(int) strlen(totalsize_str),
					totaldone_str, totalsize_str, percent,
					tablespacenum, tablespacecount,
			/* Prefix with "..." if we do leading truncation */
					truncate ? "..." : "",
			truncate ? VERBOSE_FILENAME_LENGTH - 3 : VERBOSE_FILENAME_LENGTH,
			truncate ? VERBOSE_FILENAME_LENGTH - 3 : VERBOSE_FILENAME_LENGTH,
			/* Truncate filename at beginning if it's too long */
					truncate ? filename + strlen(filename) - VERBOSE_FILENAME_LENGTH + 3 : filename);
		}
	}
	else
		fprintf(stderr,
				ngettext("%*s/%s kB (%d%%), %d/%d tablespace",
						 "%*s/%s kB (%d%%), %d/%d tablespaces",
						 tablespacecount),
				(int) strlen(totalsize_str),
				totaldone_str, totalsize_str, percent,
				tablespacenum, tablespacecount);

	fprintf(stderr, "\r");
}

static int32
parse_max_rate(char *src)
{
	double		result;
	char	   *after_num;
	char	   *suffix = NULL;

	errno = 0;
	result = strtod(src, &after_num);
	if (src == after_num)
	{
		fprintf(stderr,
				_("%s: transfer rate \"%s\" is not a valid value\n"),
				progname, src);
		exit(1);
	}
	if (errno != 0)
	{
		fprintf(stderr,
				_("%s: invalid transfer rate \"%s\": %s\n"),
				progname, src, strerror(errno));
		exit(1);
	}

	if (result <= 0)
	{
		/*
		 * Reject obviously wrong values here.
		 */
		fprintf(stderr, _("%s: transfer rate must be greater than zero\n"),
				progname);
		exit(1);
	}

	/*
	 * Evaluate suffix, after skipping over possible whitespace. Lack of
	 * suffix means kilobytes.
	 */
	while (*after_num != '\0' && isspace((unsigned char) *after_num))
		after_num++;

	if (*after_num != '\0')
	{
		suffix = after_num;
		if (*after_num == 'k')
		{
			/* kilobyte is the expected unit. */
			after_num++;
		}
		else if (*after_num == 'M')
		{
			after_num++;
			result *= 1024.0;
		}
	}

	/* The rest can only consist of white space. */
	while (*after_num != '\0' && isspace((unsigned char) *after_num))
		after_num++;

	if (*after_num != '\0')
	{
		fprintf(stderr,
				_("%s: invalid --max-rate unit: \"%s\"\n"),
				progname, suffix);
		exit(1);
	}

	/* Valid integer? */
	if ((uint64) result != (uint64) ((uint32) result))
	{
		fprintf(stderr,
				_("%s: transfer rate \"%s\" exceeds integer range\n"),
				progname, src);
		exit(1);
	}

	/*
	 * The range is checked on the server side too, but avoid the server
	 * connection if a nonsensical value was passed.
	 */
	if (result < MAX_RATE_LOWER || result > MAX_RATE_UPPER)
	{
		fprintf(stderr,
				_("%s: transfer rate \"%s\" is out of range\n"),
				progname, src);
		exit(1);
	}

	return (int32) result;
}

/*
 * Write a piece of tar data
 */
static void
writeTarData(
#ifdef HAVE_LIBZ
			 gzFile ztarfile,
#endif
			 FILE *tarfile, char *buf, int r, char *current_file)
{
#ifdef HAVE_LIBZ
	if (ztarfile != NULL)
	{
		if (gzwrite(ztarfile, buf, r) != r)
		{
			fprintf(stderr,
					_("%s: could not write to compressed file \"%s\": %s\n"),
					progname, current_file, get_gz_error(ztarfile));
			disconnect_and_exit(1);
		}
	}
	else
#endif
	{
		if (fwrite(buf, r, 1, tarfile) != 1)
		{
			fprintf(stderr, _("%s: could not write to file \"%s\": %s\n"),
					progname, current_file, strerror(errno));
			disconnect_and_exit(1);
		}
	}
}

#ifdef HAVE_LIBZ
#define WRITE_TAR_DATA(buf, sz) writeTarData(ztarfile, tarfile, buf, sz, filename)
#else
#define WRITE_TAR_DATA(buf, sz) writeTarData(tarfile, buf, sz, filename)
#endif

/*
 * Receive a tar format file from the connection to the server, and write
 * the data from this file directly into a tar file. If compression is
 * enabled, the data will be compressed while written to the file.
 *
 * The file will be named base.tar[.gz] if it's for the main data directory
 * or <tablespaceoid>.tar[.gz] if it's for another tablespace.
 *
 * No attempt to inspect or validate the contents of the file is done.
 */
static void
ReceiveTarFile(PGconn *conn, PGresult *res, int rownum)
{
	char		filename[MAXPGPATH];
	char	   *copybuf = NULL;
	FILE	   *tarfile = NULL;
	char		tarhdr[512];
	bool		basetablespace = PQgetisnull(res, rownum, 0);
	bool		in_tarhdr = true;
	bool		skip_file = false;
	size_t		tarhdrsz = 0;
	pgoff_t		filesz = 0;

#ifdef HAVE_LIBZ
	gzFile		ztarfile = NULL;
#endif

	if (basetablespace)
	{
		/*
		 * Base tablespaces
		 */
		if (strcmp(basedir, "-") == 0)
		{
#ifdef HAVE_LIBZ
			if (compresslevel != 0)
			{
				ztarfile = gzdopen(dup(fileno(stdout)), "wb");
				if (gzsetparams(ztarfile, compresslevel,
								Z_DEFAULT_STRATEGY) != Z_OK)
				{
					fprintf(stderr,
							_("%s: could not set compression level %d: %s\n"),
							progname, compresslevel, get_gz_error(ztarfile));
					disconnect_and_exit(1);
				}
			}
			else
#endif
				tarfile = stdout;
			strcpy(filename, "-");
		}
		else
		{
#ifdef HAVE_LIBZ
			if (compresslevel != 0)
			{
				snprintf(filename, sizeof(filename), "%s/base.tar.gz", basedir);
				ztarfile = gzopen(filename, "wb");
				if (gzsetparams(ztarfile, compresslevel,
								Z_DEFAULT_STRATEGY) != Z_OK)
				{
					fprintf(stderr,
							_("%s: could not set compression level %d: %s\n"),
							progname, compresslevel, get_gz_error(ztarfile));
					disconnect_and_exit(1);
				}
			}
			else
#endif
			{
				snprintf(filename, sizeof(filename), "%s/base.tar", basedir);
				tarfile = fopen(filename, "wb");
			}
		}
	}
	else
	{
		/*
		 * Specific tablespace
		 */
#ifdef HAVE_LIBZ
		if (compresslevel != 0)
		{
			snprintf(filename, sizeof(filename), "%s/%s.tar.gz", basedir,
					 PQgetvalue(res, rownum, 0));
			ztarfile = gzopen(filename, "wb");
			if (gzsetparams(ztarfile, compresslevel,
							Z_DEFAULT_STRATEGY) != Z_OK)
			{
				fprintf(stderr,
						_("%s: could not set compression level %d: %s\n"),
						progname, compresslevel, get_gz_error(ztarfile));
				disconnect_and_exit(1);
			}
		}
		else
#endif
		{
			snprintf(filename, sizeof(filename), "%s/%s.tar", basedir,
					 PQgetvalue(res, rownum, 0));
			tarfile = fopen(filename, "wb");
		}
	}

#ifdef HAVE_LIBZ
	if (compresslevel != 0)
	{
		if (!ztarfile)
		{
			/* Compression is in use */
			fprintf(stderr,
					_("%s: could not create compressed file \"%s\": %s\n"),
					progname, filename, get_gz_error(ztarfile));
			disconnect_and_exit(1);
		}
	}
	else
#endif
	{
		/* Either no zlib support, or zlib support but compresslevel = 0 */
		if (!tarfile)
		{
			fprintf(stderr, _("%s: could not create file \"%s\": %s\n"),
					progname, filename, strerror(errno));
			disconnect_and_exit(1);
		}
	}

	/*
	 * Get the COPY data stream
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
	{
		fprintf(stderr, _("%s: could not get COPY data stream: %s"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}

	while (1)
	{
		int			r;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		r = PQgetCopyData(conn, &copybuf, 0);
		if (r == -1)
		{
			/*
			 * End of chunk. If requested, and this is the base tablespace,
			 * write recovery.conf into the tarfile. When done, close the file
			 * (but not stdout).
			 *
			 * Also, write two completely empty blocks at the end of the tar
			 * file, as required by some tar programs.
			 */
			char		zerobuf[1024];

			MemSet(zerobuf, 0, sizeof(zerobuf));

			if (basetablespace && writerecoveryconf)
			{
				char		header[512];
				int			padding;

				tarCreateHeader(header, "recovery.conf", NULL,
								recoveryconfcontents->len,
								0600, 04000, 02000,
								time(NULL));

				padding = ((recoveryconfcontents->len + 511) & ~511) - recoveryconfcontents->len;

				WRITE_TAR_DATA(header, sizeof(header));
				WRITE_TAR_DATA(recoveryconfcontents->data, recoveryconfcontents->len);
				if (padding)
					WRITE_TAR_DATA(zerobuf, padding);
			}

			/* 2 * 512 bytes empty data at end of file */
			WRITE_TAR_DATA(zerobuf, sizeof(zerobuf));

#ifdef HAVE_LIBZ
			if (ztarfile != NULL)
			{
				if (gzclose(ztarfile) != 0)
				{
					fprintf(stderr,
					   _("%s: could not close compressed file \"%s\": %s\n"),
							progname, filename, get_gz_error(ztarfile));
					disconnect_and_exit(1);
				}
			}
			else
#endif
			{
				if (strcmp(basedir, "-") != 0)
				{
					if (fclose(tarfile) != 0)
					{
						fprintf(stderr,
								_("%s: could not close file \"%s\": %s\n"),
								progname, filename, strerror(errno));
						disconnect_and_exit(1);
					}
				}
			}

			break;
		}
		else if (r == -2)
		{
			fprintf(stderr, _("%s: could not read COPY data: %s"),
					progname, PQerrorMessage(conn));
			disconnect_and_exit(1);
		}

		if (!writerecoveryconf || !basetablespace)
		{
			/*
			 * When not writing recovery.conf, or when not working on the base
			 * tablespace, we never have to look for an existing recovery.conf
			 * file in the stream.
			 */
			WRITE_TAR_DATA(copybuf, r);
		}
		else
		{
			/*
			 * Look for a recovery.conf in the existing tar stream. If it's
			 * there, we must skip it so we can later overwrite it with our
			 * own version of the file.
			 *
			 * To do this, we have to process the individual files inside the
			 * TAR stream. The stream consists of a header and zero or more
			 * chunks, all 512 bytes long. The stream from the server is
			 * broken up into smaller pieces, so we have to track the size of
			 * the files to find the next header structure.
			 */
			int			rr = r;
			int			pos = 0;

			while (rr > 0)
			{
				if (in_tarhdr)
				{
					/*
					 * We're currently reading a header structure inside the
					 * TAR stream, i.e. the file metadata.
					 */
					if (tarhdrsz < 512)
					{
						/*
						 * Copy the header structure into tarhdr in case the
						 * header is not aligned to 512 bytes or it's not
						 * returned in whole by the last PQgetCopyData call.
						 */
						int			hdrleft;
						int			bytes2copy;

						hdrleft = 512 - tarhdrsz;
						bytes2copy = (rr > hdrleft ? hdrleft : rr);

						memcpy(&tarhdr[tarhdrsz], copybuf + pos, bytes2copy);

						rr -= bytes2copy;
						pos += bytes2copy;
						tarhdrsz += bytes2copy;
					}
					else
					{
						/*
						 * We have the complete header structure in tarhdr,
						 * look at the file metadata: - the subsequent file
						 * contents have to be skipped if the filename is
						 * recovery.conf - find out the size of the file
						 * padded to the next multiple of 512
						 */
						int			padding;

						skip_file = (strcmp(&tarhdr[0], "recovery.conf") == 0);

						filesz = read_tar_number(&tarhdr[124], 12);

						padding = ((filesz + 511) & ~511) - filesz;
						filesz += padding;

						/* Next part is the file, not the header */
						in_tarhdr = false;

						/*
						 * If we're not skipping the file, write the tar
						 * header unmodified.
						 */
						if (!skip_file)
							WRITE_TAR_DATA(tarhdr, 512);
					}
				}
				else
				{
					/*
					 * We're processing a file's contents.
					 */
					if (filesz > 0)
					{
						/*
						 * We still have data to read (and possibly write).
						 */
						int			bytes2write;

						bytes2write = (filesz > rr ? rr : filesz);

						if (!skip_file)
							WRITE_TAR_DATA(copybuf + pos, bytes2write);

						rr -= bytes2write;
						pos += bytes2write;
						filesz -= bytes2write;
					}
					else
					{
						/*
						 * No more data in the current file, the next piece of
						 * data (if any) will be a new file header structure.
						 */
						in_tarhdr = true;
						skip_file = false;
						tarhdrsz = 0;
						filesz = 0;
					}
				}
			}
		}
		totaldone += r;
		progress_report(rownum, filename, false);
	}							/* while (1) */
	progress_report(rownum, filename, true);

	if (copybuf != NULL)
		PQfreemem(copybuf);
}


/*
 * Retrieve tablespace path, either relocated or original depending on whether
 * -T was passed or not.
 */
static const char *
get_tablespace_mapping(const char *dir)
{
	TablespaceListCell *cell;

	for (cell = tablespace_dirs.head; cell; cell = cell->next)
		if (strcmp(dir, cell->old_dir) == 0)
			return cell->new_dir;

	return dir;
}


/*
 * Receive a tar format stream from the connection to the server, and unpack
 * the contents of it into a directory. Only files, directories and
 * symlinks are supported, no other kinds of special files.
 *
 * If the data is for the main data directory, it will be restored in the
 * specified directory. If it's for another tablespace, it will be restored
 * in the original or mapped directory.
 */
static void
ReceiveAndUnpackTarFile(PGconn *conn, PGresult *res, int rownum)
{
	char		current_path[MAXPGPATH];
	char		filename[MAXPGPATH];
	const char *mapped_tblspc_path;
	pgoff_t		current_len_left = 0;
	int			current_padding = 0;
	bool		basetablespace;
	char	   *copybuf = NULL;
	FILE	   *file = NULL;

	basetablespace = PQgetisnull(res, rownum, 0);
	if (basetablespace)
		strlcpy(current_path, basedir, sizeof(current_path));
	else
		strlcpy(current_path,
				get_tablespace_mapping(PQgetvalue(res, rownum, 1)),
				sizeof(current_path));

	/*
	 * Get the COPY data
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
	{
		fprintf(stderr, _("%s: could not get COPY data stream: %s"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}

	while (1)
	{
		int			r;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		r = PQgetCopyData(conn, &copybuf, 0);

		if (r == -1)
		{
			/*
			 * End of chunk
			 */
			if (file)
				fclose(file);

			break;
		}
		else if (r == -2)
		{
			fprintf(stderr, _("%s: could not read COPY data: %s"),
					progname, PQerrorMessage(conn));
			disconnect_and_exit(1);
		}

		if (file == NULL)
		{
			int			filemode;

			/*
			 * No current file, so this must be the header for a new file
			 */
			if (r != 512)
			{
				fprintf(stderr, _("%s: invalid tar block header size: %d\n"),
						progname, r);
				disconnect_and_exit(1);
			}
			totaldone += 512;

			current_len_left = read_tar_number(&copybuf[124], 12);

			/* Set permissions on the file */
			filemode = read_tar_number(&copybuf[100], 8);

			/*
			 * All files are padded up to 512 bytes
			 */
			current_padding =
				((current_len_left + 511) & ~511) - current_len_left;

			/*
			 * First part of header is zero terminated filename
			 */
			snprintf(filename, sizeof(filename), "%s/%s", current_path,
					 copybuf);
			if (filename[strlen(filename) - 1] == '/')
			{
				/*
				 * Ends in a slash means directory or symlink to directory
				 */
				if (copybuf[156] == '5')
				{
					/*
					 * Directory
					 */
					filename[strlen(filename) - 1] = '\0';		/* Remove trailing slash */
					if (mkdir(filename, S_IRWXU) != 0)
					{
						/*
						 * When streaming WAL, pg_xlog will have been created
						 * by the wal receiver process. Also, when transaction
						 * log directory location was specified, pg_xlog has
						 * already been created as a symbolic link before
						 * starting the actual backup. So just ignore creation
						 * failures on related directories.
						 */
						if (!((pg_str_endswith(filename, "/pg_xlog") ||
							 pg_str_endswith(filename, "/archive_status")) &&
							  errno == EEXIST))
						{
							fprintf(stderr,
							_("%s: could not create directory \"%s\": %s\n"),
									progname, filename, strerror(errno));
							disconnect_and_exit(1);
						}
					}
#ifndef WIN32
					if (chmod(filename, (mode_t) filemode))
						fprintf(stderr,
								_("%s: could not set permissions on directory \"%s\": %s\n"),
								progname, filename, strerror(errno));
#endif
				}
				else if (copybuf[156] == '2')
				{
					/*
					 * Symbolic link
					 *
					 * It's most likely a link in pg_tblspc directory, to the
					 * location of a tablespace. Apply any tablespace mapping
					 * given on the command line (--tablespace-mapping). (We
					 * blindly apply the mapping without checking that the
					 * link really is inside pg_tblspc. We don't expect there
					 * to be other symlinks in a data directory, but if there
					 * are, you can call it an undocumented feature that you
					 * can map them too.)
					 */
					filename[strlen(filename) - 1] = '\0';		/* Remove trailing slash */

					mapped_tblspc_path = get_tablespace_mapping(&copybuf[157]);
					if (symlink(mapped_tblspc_path, filename) != 0)
					{
						fprintf(stderr,
								_("%s: could not create symbolic link from \"%s\" to \"%s\": %s\n"),
								progname, filename, mapped_tblspc_path,
								strerror(errno));
						disconnect_and_exit(1);
					}
				}
				else
				{
					fprintf(stderr,
							_("%s: unrecognized link indicator \"%c\"\n"),
							progname, copybuf[156]);
					disconnect_and_exit(1);
				}
				continue;		/* directory or link handled */
			}

			/*
			 * regular file
			 */
			file = fopen(filename, "wb");
			if (!file)
			{
				fprintf(stderr, _("%s: could not create file \"%s\": %s\n"),
						progname, filename, strerror(errno));
				disconnect_and_exit(1);
			}

#ifndef WIN32
			if (chmod(filename, (mode_t) filemode))
				fprintf(stderr, _("%s: could not set permissions on file \"%s\": %s\n"),
						progname, filename, strerror(errno));
#endif

			if (current_len_left == 0)
			{
				/*
				 * Done with this file, next one will be a new tar header
				 */
				fclose(file);
				file = NULL;
				continue;
			}
		}						/* new file */
		else
		{
			/*
			 * Continuing blocks in existing file
			 */
			if (current_len_left == 0 && r == current_padding)
			{
				/*
				 * Received the padding block for this file, ignore it and
				 * close the file, then move on to the next tar header.
				 */
				fclose(file);
				file = NULL;
				totaldone += r;
				continue;
			}

			if (fwrite(copybuf, r, 1, file) != 1)
			{
				fprintf(stderr, _("%s: could not write to file \"%s\": %s\n"),
						progname, filename, strerror(errno));
				disconnect_and_exit(1);
			}
			totaldone += r;
			progress_report(rownum, filename, false);

			current_len_left -= r;
			if (current_len_left == 0 && current_padding == 0)
			{
				/*
				 * Received the last block, and there is no padding to be
				 * expected. Close the file and move on to the next tar
				 * header.
				 */
				fclose(file);
				file = NULL;
				continue;
			}
		}						/* continuing data in existing file */
	}							/* loop over all data blocks */
	progress_report(rownum, filename, true);

	if (file != NULL)
	{
		fprintf(stderr,
				_("%s: COPY stream ended before last file was finished\n"),
				progname);
		disconnect_and_exit(1);
	}

	if (copybuf != NULL)
		PQfreemem(copybuf);

	if (basetablespace && writerecoveryconf)
		WriteRecoveryConf();
}

/*
 * Escape a string so that it can be used as a value in a key-value pair
 * a configuration file.
 */
static char *
escape_quotes(const char *src)
{
	char	   *result = escape_single_quotes_ascii(src);

	if (!result)
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		exit(1);
	}
	return result;
}

/*
 * Create a recovery.conf file in memory using a PQExpBuffer
 */
static void
GenerateRecoveryConf(PGconn *conn)
{
	PQconninfoOption *connOptions;
	PQconninfoOption *option;
	PQExpBufferData conninfo_buf;
	char	   *escaped;

	recoveryconfcontents = createPQExpBuffer();
	if (!recoveryconfcontents)
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		disconnect_and_exit(1);
	}

	connOptions = PQconninfo(conn);
	if (connOptions == NULL)
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		disconnect_and_exit(1);
	}

	appendPQExpBufferStr(recoveryconfcontents, "standby_mode = 'on'\n");

	initPQExpBuffer(&conninfo_buf);
	for (option = connOptions; option && option->keyword; option++)
	{
		/*
		 * Do not emit this setting if: - the setting is "replication",
		 * "dbname" or "fallback_application_name", since these would be
		 * overridden by the libpqwalreceiver module anyway. - not set or
		 * empty.
		 */
		if (strcmp(option->keyword, "replication") == 0 ||
			strcmp(option->keyword, "dbname") == 0 ||
			strcmp(option->keyword, "fallback_application_name") == 0 ||
			(option->val == NULL) ||
			(option->val != NULL && option->val[0] == '\0'))
			continue;

		/* Separate key-value pairs with spaces */
		if (conninfo_buf.len != 0)
			appendPQExpBufferChar(&conninfo_buf, ' ');

		/*
		 * Write "keyword=value" pieces, the value string is escaped and/or
		 * quoted if necessary.
		 */
		appendPQExpBuffer(&conninfo_buf, "%s=", option->keyword);
		appendConnStrVal(&conninfo_buf, option->val);
	}

	/*
	 * Escape the connection string, so that it can be put in the config file.
	 * Note that this is different from the escaping of individual connection
	 * options above!
	 */
	escaped = escape_quotes(conninfo_buf.data);
	appendPQExpBuffer(recoveryconfcontents, "primary_conninfo = '%s'\n", escaped);
	free(escaped);

	if (replication_slot)
	{
		escaped = escape_quotes(replication_slot);
		appendPQExpBuffer(recoveryconfcontents, "primary_slot_name = '%s'\n", replication_slot);
		free(escaped);
	}

	if (PQExpBufferBroken(recoveryconfcontents) ||
		PQExpBufferDataBroken(conninfo_buf))
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		disconnect_and_exit(1);
	}

	termPQExpBuffer(&conninfo_buf);

	PQconninfoFree(connOptions);
}


/*
 * Write a recovery.conf file into the directory specified in basedir,
 * with the contents already collected in memory.
 */
static void
WriteRecoveryConf(void)
{
	char		filename[MAXPGPATH];
	FILE	   *cf;

	sprintf(filename, "%s/recovery.conf", basedir);

	cf = fopen(filename, "w");
	if (cf == NULL)
	{
		fprintf(stderr, _("%s: could not create file \"%s\": %s\n"), progname, filename, strerror(errno));
		disconnect_and_exit(1);
	}

	if (fwrite(recoveryconfcontents->data, recoveryconfcontents->len, 1, cf) != 1)
	{
		fprintf(stderr,
				_("%s: could not write to file \"%s\": %s\n"),
				progname, filename, strerror(errno));
		disconnect_and_exit(1);
	}

	fclose(cf);
}


static void
BaseBackup(void)
{
	PGresult   *res;
	char	   *sysidentifier;
	TimeLineID	latesttli;
	TimeLineID	starttli;
	char	   *basebkp;
	char		escaped_label[MAXPGPATH];
	char	   *maxrate_clause = NULL;
	int			i;
	char		xlogstart[64];
	char		xlogend[64];
	int			minServerMajor,
				maxServerMajor;
	int			serverMajor;

	/*
	 * Connect in replication mode to the server
	 */
	conn = GetConnection();
	if (!conn)
		/* Error message already written in GetConnection() */
		exit(1);

	/*
	 * Check server version. BASE_BACKUP command was introduced in 9.1, so we
	 * can't work with servers older than 9.1.
	 */
	minServerMajor = 901;
	maxServerMajor = PG_VERSION_NUM / 100;
	serverMajor = PQserverVersion(conn) / 100;
	if (serverMajor < minServerMajor || serverMajor > maxServerMajor)
	{
		const char *serverver = PQparameterStatus(conn, "server_version");

		fprintf(stderr, _("%s: incompatible server version %s\n"),
				progname, serverver ? serverver : "'unknown'");
		disconnect_and_exit(1);
	}

	/*
	 * If WAL streaming was requested, also check that the server is new
	 * enough for that.
	 */
	if (streamwal && !CheckServerVersionForStreaming(conn))
	{
		/* Error message already written in CheckServerVersionForStreaming() */
		disconnect_and_exit(1);
	}

	/*
	 * Build contents of recovery.conf if requested
	 */
	if (writerecoveryconf)
		GenerateRecoveryConf(conn);

	/*
	 * Run IDENTIFY_SYSTEM so we can get the timeline
	 */
	if (!RunIdentifySystem(conn, &sysidentifier, &latesttli, NULL, NULL))
		disconnect_and_exit(1);

	/*
	 * Start the actual backup
	 */
	PQescapeStringConn(conn, escaped_label, label, sizeof(escaped_label), &i);

	if (maxrate > 0)
		maxrate_clause = psprintf("MAX_RATE %u", maxrate);

	basebkp =
		psprintf("BASE_BACKUP LABEL '%s' %s %s %s %s %s %s",
				 escaped_label,
				 showprogress ? "PROGRESS" : "",
				 includewal && !streamwal ? "WAL" : "",
				 fastcheckpoint ? "FAST" : "",
				 includewal ? "NOWAIT" : "",
				 maxrate_clause ? maxrate_clause : "",
				 format == 't' ? "TABLESPACE_MAP" : "");

	if (PQsendQuery(conn, basebkp) == 0)
	{
		fprintf(stderr, _("%s: could not send replication command \"%s\": %s"),
				progname, "BASE_BACKUP", PQerrorMessage(conn));
		disconnect_and_exit(1);
	}

	/*
	 * Get the starting xlog position
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, _("%s: could not initiate base backup: %s"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}
	if (PQntuples(res) != 1)
	{
		fprintf(stderr,
				_("%s: server returned unexpected response to BASE_BACKUP command; got %d rows and %d fields, expected %d rows and %d fields\n"),
				progname, PQntuples(res), PQnfields(res), 1, 2);
		disconnect_and_exit(1);
	}

	strlcpy(xlogstart, PQgetvalue(res, 0, 0), sizeof(xlogstart));

	/*
	 * 9.3 and later sends the TLI of the starting point. With older servers,
	 * assume it's the same as the latest timeline reported by
	 * IDENTIFY_SYSTEM.
	 */
	if (PQnfields(res) >= 2)
		starttli = atoi(PQgetvalue(res, 0, 1));
	else
		starttli = latesttli;
	PQclear(res);
	MemSet(xlogend, 0, sizeof(xlogend));

	if (verbose && includewal)
		fprintf(stderr, _("transaction log start point: %s on timeline %u\n"),
				xlogstart, starttli);

	/*
	 * Get the header
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, _("%s: could not get backup header: %s"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}
	if (PQntuples(res) < 1)
	{
		fprintf(stderr, _("%s: no data returned from server\n"), progname);
		disconnect_and_exit(1);
	}

	/*
	 * Sum up the total size, for progress reporting
	 */
	totalsize = totaldone = 0;
	tablespacecount = PQntuples(res);
	for (i = 0; i < PQntuples(res); i++)
	{
		totalsize += atol(PQgetvalue(res, i, 2));

		/*
		 * Verify tablespace directories are empty. Don't bother with the
		 * first once since it can be relocated, and it will be checked before
		 * we do anything anyway.
		 */
		if (format == 'p' && !PQgetisnull(res, i, 1))
		{
			char	   *path = (char *) get_tablespace_mapping(PQgetvalue(res, i, 1));

			verify_dir_is_empty_or_create(path, &made_tablespace_dirs, &found_tablespace_dirs);
		}
	}

	/*
	 * When writing to stdout, require a single tablespace
	 */
	if (format == 't' && strcmp(basedir, "-") == 0 && PQntuples(res) > 1)
	{
		fprintf(stderr,
				_("%s: can only write single tablespace to stdout, database has %d\n"),
				progname, PQntuples(res));
		disconnect_and_exit(1);
	}

	/*
	 * If we're streaming WAL, start the streaming session before we start
	 * receiving the actual data chunks.
	 */
	if (streamwal)
	{
		if (verbose)
			fprintf(stderr, _("%s: starting background WAL receiver\n"),
					progname);
		StartLogStreamer(xlogstart, starttli, sysidentifier);
	}

	/*
	 * Start receiving chunks
	 */
	for (i = 0; i < PQntuples(res); i++)
	{
		if (format == 't')
			ReceiveTarFile(conn, res, i);
		else
			ReceiveAndUnpackTarFile(conn, res, i);
	}							/* Loop over all tablespaces */

	if (showprogress)
	{
		progress_report(PQntuples(res), NULL, true);
		fprintf(stderr, "\n");	/* Need to move to next line */
	}

	PQclear(res);

	/*
	 * Get the stop position
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr,
		 _("%s: could not get transaction log end position from server: %s"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}
	if (PQntuples(res) != 1)
	{
		fprintf(stderr,
			 _("%s: no transaction log end position returned from server\n"),
				progname);
		disconnect_and_exit(1);
	}
	strlcpy(xlogend, PQgetvalue(res, 0, 0), sizeof(xlogend));
	if (verbose && includewal)
		fprintf(stderr, "transaction log end point: %s\n", xlogend);
	PQclear(res);

	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: final receive failed: %s"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}

	if (bgchild > 0)
	{
#ifndef WIN32
		int			status;
		int			r;
#else
		DWORD		status;

		/*
		 * get a pointer sized version of bgchild to avoid warnings about
		 * casting to a different size on WIN64.
		 */
		intptr_t	bgchild_handle = bgchild;
		uint32		hi,
					lo;
#endif

		if (verbose)
			fprintf(stderr,
					_("%s: waiting for background process to finish streaming ...\n"), progname);

#ifndef WIN32
		if (write(bgpipe[1], xlogend, strlen(xlogend)) != strlen(xlogend))
		{
			fprintf(stderr,
					_("%s: could not send command to background pipe: %s\n"),
					progname, strerror(errno));
			disconnect_and_exit(1);
		}

		/* Just wait for the background process to exit */
		r = waitpid(bgchild, &status, 0);
		if (r == -1)
		{
			fprintf(stderr, _("%s: could not wait for child process: %s\n"),
					progname, strerror(errno));
			disconnect_and_exit(1);
		}
		if (r != bgchild)
		{
			fprintf(stderr, _("%s: child %d died, expected %d\n"),
					progname, r, (int) bgchild);
			disconnect_and_exit(1);
		}
		if (!WIFEXITED(status))
		{
			fprintf(stderr, _("%s: child process did not exit normally\n"),
					progname);
			disconnect_and_exit(1);
		}
		if (WEXITSTATUS(status) != 0)
		{
			fprintf(stderr, _("%s: child process exited with error %d\n"),
					progname, WEXITSTATUS(status));
			disconnect_and_exit(1);
		}
		/* Exited normally, we're happy! */
#else							/* WIN32 */

		/*
		 * On Windows, since we are in the same process, we can just store the
		 * value directly in the variable, and then set the flag that says
		 * it's there.
		 */
		if (sscanf(xlogend, "%X/%X", &hi, &lo) != 2)
		{
			fprintf(stderr,
				  _("%s: could not parse transaction log location \"%s\"\n"),
					progname, xlogend);
			disconnect_and_exit(1);
		}
		xlogendptr = ((uint64) hi) << 32 | lo;
		InterlockedIncrement(&has_xlogendptr);

		/* First wait for the thread to exit */
		if (WaitForSingleObjectEx((HANDLE) bgchild_handle, INFINITE, FALSE) !=
			WAIT_OBJECT_0)
		{
			_dosmaperr(GetLastError());
			fprintf(stderr, _("%s: could not wait for child thread: %s\n"),
					progname, strerror(errno));
			disconnect_and_exit(1);
		}
		if (GetExitCodeThread((HANDLE) bgchild_handle, &status) == 0)
		{
			_dosmaperr(GetLastError());
			fprintf(stderr, _("%s: could not get child thread exit status: %s\n"),
					progname, strerror(errno));
			disconnect_and_exit(1);
		}
		if (status != 0)
		{
			fprintf(stderr, _("%s: child thread exited with error %u\n"),
					progname, (unsigned int) status);
			disconnect_and_exit(1);
		}
		/* Exited normally, we're happy */
#endif
	}

	/* Free the recovery.conf contents */
	destroyPQExpBuffer(recoveryconfcontents);

	/*
	 * End of copy data. Final result is already checked inside the loop.
	 */
	PQclear(res);
	PQfinish(conn);

	if (verbose)
		fprintf(stderr, "%s: base backup completed\n", progname);
}


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"pgdata", required_argument, NULL, 'D'},
		{"format", required_argument, NULL, 'F'},
		{"checkpoint", required_argument, NULL, 'c'},
		{"max-rate", required_argument, NULL, 'r'},
		{"write-recovery-conf", no_argument, NULL, 'R'},
		{"slot", required_argument, NULL, 'S'},
		{"tablespace-mapping", required_argument, NULL, 'T'},
		{"xlog", no_argument, NULL, 'x'},
		{"xlog-method", required_argument, NULL, 'X'},
		{"gzip", no_argument, NULL, 'z'},
		{"compress", required_argument, NULL, 'Z'},
		{"label", required_argument, NULL, 'l'},
		{"noclean", no_argument, NULL, 'n'},
		{"dbname", required_argument, NULL, 'd'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"status-interval", required_argument, NULL, 's'},
		{"verbose", no_argument, NULL, 'v'},
		{"progress", no_argument, NULL, 'P'},
		{"xlogdir", required_argument, NULL, 1},
		{NULL, 0, NULL, 0}
	};
	int			c;

	int			option_index;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_basebackup"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		else if (strcmp(argv[1], "-V") == 0
				 || strcmp(argv[1], "--version") == 0)
		{
			puts("pg_basebackup (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	atexit(cleanup_directories_atexit);

	while ((c = getopt_long(argc, argv, "D:F:r:RT:xX:l:nzZ:d:c:h:p:U:s:S:wWvP",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'D':
				basedir = pg_strdup(optarg);
				break;
			case 'F':
				if (strcmp(optarg, "p") == 0 || strcmp(optarg, "plain") == 0)
					format = 'p';
				else if (strcmp(optarg, "t") == 0 || strcmp(optarg, "tar") == 0)
					format = 't';
				else
				{
					fprintf(stderr,
							_("%s: invalid output format \"%s\", must be \"plain\" or \"tar\"\n"),
							progname, optarg);
					exit(1);
				}
				break;
			case 'r':
				maxrate = parse_max_rate(optarg);
				break;
			case 'R':
				writerecoveryconf = true;
				break;
			case 'S':
				replication_slot = pg_strdup(optarg);
				break;
			case 'T':
				tablespace_list_append(optarg);
				break;
			case 'x':
				if (includewal)
				{
					fprintf(stderr,
					 _("%s: cannot specify both --xlog and --xlog-method\n"),
							progname);
					exit(1);
				}

				includewal = true;
				streamwal = false;
				break;
			case 'X':
				if (includewal)
				{
					fprintf(stderr,
					 _("%s: cannot specify both --xlog and --xlog-method\n"),
							progname);
					exit(1);
				}

				includewal = true;
				if (strcmp(optarg, "f") == 0 ||
					strcmp(optarg, "fetch") == 0)
					streamwal = false;
				else if (strcmp(optarg, "s") == 0 ||
						 strcmp(optarg, "stream") == 0)
					streamwal = true;
				else
				{
					fprintf(stderr,
							_("%s: invalid xlog-method option \"%s\", must be \"fetch\" or \"stream\"\n"),
							progname, optarg);
					exit(1);
				}
				break;
			case 1:
				xlog_dir = pg_strdup(optarg);
				break;
			case 'l':
				label = pg_strdup(optarg);
				break;
			case 'n':
				noclean = true;
				break;
			case 'z':
#ifdef HAVE_LIBZ
				compresslevel = Z_DEFAULT_COMPRESSION;
#else
				compresslevel = 1;		/* will be rejected below */
#endif
				break;
			case 'Z':
				compresslevel = atoi(optarg);
				if (compresslevel < 0 || compresslevel > 9)
				{
					fprintf(stderr, _("%s: invalid compression level \"%s\"\n"),
							progname, optarg);
					exit(1);
				}
				break;
			case 'c':
				if (pg_strcasecmp(optarg, "fast") == 0)
					fastcheckpoint = true;
				else if (pg_strcasecmp(optarg, "spread") == 0)
					fastcheckpoint = false;
				else
				{
					fprintf(stderr, _("%s: invalid checkpoint argument \"%s\", must be \"fast\" or \"spread\"\n"),
							progname, optarg);
					exit(1);
				}
				break;
			case 'd':
				connection_string = pg_strdup(optarg);
				break;
			case 'h':
				dbhost = pg_strdup(optarg);
				break;
			case 'p':
				dbport = pg_strdup(optarg);
				break;
			case 'U':
				dbuser = pg_strdup(optarg);
				break;
			case 'w':
				dbgetpassword = -1;
				break;
			case 'W':
				dbgetpassword = 1;
				break;
			case 's':
				standby_message_timeout = atoi(optarg) * 1000;
				if (standby_message_timeout < 0)
				{
					fprintf(stderr, _("%s: invalid status interval \"%s\"\n"),
							progname, optarg);
					exit(1);
				}
				break;
			case 'v':
				verbose++;
				break;
			case 'P':
				showprogress = true;
				break;
			default:

				/*
				 * getopt_long already emitted a complaint
				 */
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
		}
	}

	/*
	 * Any non-option arguments?
	 */
	if (optind < argc)
	{
		fprintf(stderr,
				_("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Required arguments
	 */
	if (basedir == NULL)
	{
		fprintf(stderr, _("%s: no target directory specified\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Mutually exclusive arguments
	 */
	if (format == 'p' && compresslevel != 0)
	{
		fprintf(stderr,
				_("%s: only tar mode backups can be compressed\n"),
				progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (format != 'p' && streamwal)
	{
		fprintf(stderr,
				_("%s: WAL streaming can only be used in plain mode\n"),
				progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (replication_slot && !streamwal)
	{
		fprintf(stderr,
			_("%s: replication slots can only be used with WAL streaming\n"),
				progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (strcmp(xlog_dir, "") != 0)
	{
		if (format != 'p')
		{
			fprintf(stderr,
					_("%s: transaction log directory location can only be specified in plain mode\n"),
					progname);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}

		/* clean up xlog directory name, check it's absolute */
		canonicalize_path(xlog_dir);
		if (!is_absolute_path(xlog_dir))
		{
			fprintf(stderr, _("%s: transaction log directory location must be "
							  "an absolute path\n"), progname);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}
	}

#ifndef HAVE_LIBZ
	if (compresslevel != 0)
	{
		fprintf(stderr,
				_("%s: this build does not support compression\n"),
				progname);
		exit(1);
	}
#endif

	/*
	 * Verify that the target directory exists, or create it. For plaintext
	 * backups, always require the directory. For tar backups, require it
	 * unless we are writing to stdout.
	 */
	if (format == 'p' || strcmp(basedir, "-") != 0)
		verify_dir_is_empty_or_create(basedir, &made_new_pgdata, &found_existing_pgdata);

	/* Create transaction log symlink, if required */
	if (strcmp(xlog_dir, "") != 0)
	{
		char	   *linkloc;

		verify_dir_is_empty_or_create(xlog_dir, &made_new_xlogdir, &found_existing_xlogdir);

		/* form name of the place where the symlink must go */
		linkloc = psprintf("%s/pg_xlog", basedir);

#ifdef HAVE_SYMLINK
		if (symlink(xlog_dir, linkloc) != 0)
		{
			fprintf(stderr, _("%s: could not create symbolic link \"%s\": %s\n"),
					progname, linkloc, strerror(errno));
			exit(1);
		}
#else
		fprintf(stderr, _("%s: symlinks are not supported on this platform\n"));
		exit(1);
#endif
		free(linkloc);
	}

	BaseBackup();

	success = true;
	return 0;
}
