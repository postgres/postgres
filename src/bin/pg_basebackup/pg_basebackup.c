/*-------------------------------------------------------------------------
 *
 * pg_basebackup.c - receive a base backup using streaming replication protocol
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_basebackup.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "access/xlog_internal.h"
#include "backup/basebackup.h"
#include "bbstreamer.h"
#include "common/compression.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/recovery_gen.h"
#include "getopt_long.h"
#include "receivelog.h"
#include "streamutil.h"

#define ERRCODE_DATA_CORRUPTED	"XX001"

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

typedef struct ArchiveStreamState
{
	int			tablespacenum;
	pg_compress_specification *compress;
	bbstreamer *streamer;
	bbstreamer *manifest_inject_streamer;
	PQExpBuffer manifest_buffer;
	char		manifest_filename[MAXPGPATH];
	FILE	   *manifest_file;
} ArchiveStreamState;

typedef struct WriteTarState
{
	int			tablespacenum;
	bbstreamer *streamer;
} WriteTarState;

typedef struct WriteManifestState
{
	char		filename[MAXPGPATH];
	FILE	   *file;
} WriteManifestState;

typedef void (*WriteDataCallback) (size_t nbytes, char *buf,
								   void *callback_data);

/*
 * pg_xlog has been renamed to pg_wal in version 10.  This version number
 * should be compared with PQserverVersion().
 */
#define MINIMUM_VERSION_FOR_PG_WAL	100000

/*
 * Temporary replication slots are supported from version 10.
 */
#define MINIMUM_VERSION_FOR_TEMP_SLOTS 100000

/*
 * Backup manifests are supported from version 13.
 */
#define MINIMUM_VERSION_FOR_MANIFESTS	130000

/*
 * Before v15, tar files received from the server will be improperly
 * terminated.
 */
#define MINIMUM_VERSION_FOR_TERMINATED_TARFILE 150000

/*
 * Different ways to include WAL
 */
typedef enum
{
	NO_WAL,
	FETCH_WAL,
	STREAM_WAL
} IncludeWal;

/*
 * Different places to perform compression
 */
typedef enum
{
	COMPRESS_LOCATION_UNSPECIFIED,
	COMPRESS_LOCATION_CLIENT,
	COMPRESS_LOCATION_SERVER
} CompressionLocation;

/* Global options */
static char *basedir = NULL;
static TablespaceList tablespace_dirs = {NULL, NULL};
static char *xlog_dir = NULL;
static char format = '\0';		/* p(lain)/t(ar) */
static char *label = "pg_basebackup base backup";
static bool noclean = false;
static bool checksum_failure = false;
static bool showprogress = false;
static bool estimatesize = true;
static int	verbose = 0;
static IncludeWal includewal = STREAM_WAL;
static bool fastcheckpoint = false;
static bool writerecoveryconf = false;
static bool do_sync = true;
static int	standby_message_timeout = 10 * 1000;	/* 10 sec = default */
static pg_time_t last_progress_report = 0;
static int32 maxrate = 0;		/* no limit by default */
static char *replication_slot = NULL;
static bool temp_replication_slot = true;
static char *backup_target = NULL;
static bool create_slot = false;
static bool no_slot = false;
static bool verify_checksums = true;
static bool manifest = true;
static bool manifest_force_encode = false;
static char *manifest_checksums = NULL;

static bool success = false;
static bool made_new_pgdata = false;
static bool found_existing_pgdata = false;
static bool made_new_xlogdir = false;
static bool found_existing_xlogdir = false;
static bool made_tablespace_dirs = false;
static bool found_tablespace_dirs = false;

/* Progress indicators */
static uint64 totalsize_kb;
static uint64 totaldone;
static int	tablespacecount;
static char *progress_filename = NULL;

/* Pipe to communicate with background wal receiver process */
#ifndef WIN32
static int	bgpipe[2] = {-1, -1};
#endif

/* Handle to child process */
static pid_t bgchild = -1;
static bool in_log_streamer = false;

/* Flag to indicate if child process exited unexpectedly */
static volatile sig_atomic_t bgchild_exited = false;

/* End position for xlog streaming, empty string if unknown yet */
static XLogRecPtr xlogendptr;

#ifndef WIN32
static int	has_xlogendptr = 0;
#else
static volatile LONG has_xlogendptr = 0;
#endif

/* Contents of configuration file to be generated */
static PQExpBuffer recoveryconfcontents = NULL;

/* Function headers */
static void usage(void);
static void verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found);
static void progress_update_filename(const char *filename);
static void progress_report(int tablespacenum, bool force, bool finished);

static bbstreamer *CreateBackupStreamer(char *archive_name, char *spclocation,
										bbstreamer **manifest_inject_streamer_p,
										bool is_recovery_guc_supported,
										bool expect_unterminated_tarfile,
										pg_compress_specification *compress);
static void ReceiveArchiveStreamChunk(size_t r, char *copybuf,
									  void *callback_data);
static char GetCopyDataByte(size_t r, char *copybuf, size_t *cursor);
static char *GetCopyDataString(size_t r, char *copybuf, size_t *cursor);
static uint64 GetCopyDataUInt64(size_t r, char *copybuf, size_t *cursor);
static void GetCopyDataEnd(size_t r, char *copybuf, size_t cursor);
static void ReportCopyDataParseError(size_t r, char *copybuf);
static void ReceiveTarFile(PGconn *conn, char *archive_name, char *spclocation,
						   bool tablespacenum, pg_compress_specification *compress);
static void ReceiveTarCopyChunk(size_t r, char *copybuf, void *callback_data);
static void ReceiveBackupManifest(PGconn *conn);
static void ReceiveBackupManifestChunk(size_t r, char *copybuf,
									   void *callback_data);
static void ReceiveBackupManifestInMemory(PGconn *conn, PQExpBuffer buf);
static void ReceiveBackupManifestInMemoryChunk(size_t r, char *copybuf,
											   void *callback_data);
static void BaseBackup(char *compression_algorithm, char *compression_detail,
					   CompressionLocation compressloc,
					   pg_compress_specification *client_compress);

static bool reached_end_position(XLogRecPtr segendpos, uint32 timeline,
								 bool segment_finished);

static const char *get_tablespace_mapping(const char *dir);
static void tablespace_list_append(const char *arg);


static void
cleanup_directories_atexit(void)
{
	if (success || in_log_streamer)
		return;

	if (!noclean && !checksum_failure)
	{
		if (made_new_pgdata)
		{
			pg_log_info("removing data directory \"%s\"", basedir);
			if (!rmtree(basedir, true))
				pg_log_error("failed to remove data directory");
		}
		else if (found_existing_pgdata)
		{
			pg_log_info("removing contents of data directory \"%s\"", basedir);
			if (!rmtree(basedir, false))
				pg_log_error("failed to remove contents of data directory");
		}

		if (made_new_xlogdir)
		{
			pg_log_info("removing WAL directory \"%s\"", xlog_dir);
			if (!rmtree(xlog_dir, true))
				pg_log_error("failed to remove WAL directory");
		}
		else if (found_existing_xlogdir)
		{
			pg_log_info("removing contents of WAL directory \"%s\"", xlog_dir);
			if (!rmtree(xlog_dir, false))
				pg_log_error("failed to remove contents of WAL directory");
		}
	}
	else
	{
		if ((made_new_pgdata || found_existing_pgdata) && !checksum_failure)
			pg_log_info("data directory \"%s\" not removed at user's request", basedir);

		if (made_new_xlogdir || found_existing_xlogdir)
			pg_log_info("WAL directory \"%s\" not removed at user's request", xlog_dir);
	}

	if ((made_tablespace_dirs || found_tablespace_dirs) && !checksum_failure)
		pg_log_info("changes to tablespace directories will not be undone");
}

static void
disconnect_atexit(void)
{
	if (conn != NULL)
		PQfinish(conn);
}

#ifndef WIN32
/*
 * If the bgchild exits prematurely and raises a SIGCHLD signal, we can abort
 * processing rather than wait until the backup has finished and error out at
 * that time. On Windows, we use a background thread which can communicate
 * without the need for a signal handler.
 */
static void
sigchld_handler(SIGNAL_ARGS)
{
	bgchild_exited = true;
}

/*
 * On windows, our background thread dies along with the process. But on
 * Unix, if we have started a subprocess, we want to kill it off so it
 * doesn't remain running trying to stream data.
 */
static void
kill_bgchild_atexit(void)
{
	if (bgchild > 0 && !bgchild_exited)
		kill(bgchild, SIGTERM);
}
#endif

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
			pg_fatal("directory name too long");

		if (*arg_ptr == '\\' && *(arg_ptr + 1) == '=')
			;					/* skip backslash escaping = */
		else if (*arg_ptr == '=' && (arg_ptr == arg || *(arg_ptr - 1) != '\\'))
		{
			if (*cell->new_dir)
				pg_fatal("multiple \"=\" signs in tablespace mapping");
			else
				dst = dst_ptr = cell->new_dir;
		}
		else
			*dst_ptr++ = *arg_ptr;
	}

	if (!*cell->old_dir || !*cell->new_dir)
		pg_fatal("invalid tablespace mapping format \"%s\", must be \"OLDDIR=NEWDIR\"", arg);

	/*
	 * All tablespaces are created with absolute directories, so specifying a
	 * non-absolute path here would just never match, possibly confusing users.
	 * Since we don't know whether the remote side is Windows or not, and it
	 * might be different than the local side, permit any path that could be
	 * absolute under either set of rules.
	 *
	 * (There is little practical risk of confusion here, because someone
	 * running entirely on Linux isn't likely to have a relative path that
	 * begins with a backslash or something that looks like a drive
	 * specification. If they do, and they also incorrectly believe that
	 * a relative path is acceptable here, we'll silently fail to warn them
	 * of their mistake, and the -T option will just not get applied, same
	 * as if they'd specified -T for a nonexistent tablespace.)
	 */
	if (!is_nonwindows_absolute_path(cell->old_dir) &&
		!is_windows_absolute_path(cell->old_dir))
		pg_fatal("old directory is not an absolute path in tablespace mapping: %s",
				 cell->old_dir);

	if (!is_absolute_path(cell->new_dir))
		pg_fatal("new directory is not an absolute path in tablespace mapping: %s",
				 cell->new_dir);

	/*
	 * Comparisons done with these values should involve similarly
	 * canonicalized path values.  This is particularly sensitive on Windows
	 * where path values may not necessarily use Unix slashes.
	 */
	canonicalize_path(cell->old_dir);
	canonicalize_path(cell->new_dir);

	if (tablespace_dirs.tail)
		tablespace_dirs.tail->next = cell;
	else
		tablespace_dirs.head = cell;
	tablespace_dirs.tail = cell;
}


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
			 "                         write configuration for replication\n"));
	printf(_("  -t, --target=TARGET[:DETAIL]\n"
			 "                         backup target (if other than client)\n"));
	printf(_("  -T, --tablespace-mapping=OLDDIR=NEWDIR\n"
			 "                         relocate tablespace in OLDDIR to NEWDIR\n"));
	printf(_("      --waldir=WALDIR    location for the write-ahead log directory\n"));
	printf(_("  -X, --wal-method=none|fetch|stream\n"
			 "                         include required WAL files with specified method\n"));
	printf(_("  -z, --gzip             compress tar output\n"));
	printf(_("  -Z, --compress=[{client|server}-]METHOD[:DETAIL]\n"
			 "                         compress on client or server as specified\n"));
	printf(_("  -Z, --compress=none    do not compress tar output\n"));
	printf(_("\nGeneral options:\n"));
	printf(_("  -c, --checkpoint=fast|spread\n"
			 "                         set fast or spread checkpointing\n"));
	printf(_("  -C, --create-slot      create replication slot\n"));
	printf(_("  -l, --label=LABEL      set backup label\n"));
	printf(_("  -n, --no-clean         do not clean up after errors\n"));
	printf(_("  -N, --no-sync          do not wait for changes to be written safely to disk\n"));
	printf(_("  -P, --progress         show progress information\n"));
	printf(_("  -S, --slot=SLOTNAME    replication slot to use\n"));
	printf(_("  -v, --verbose          output verbose messages\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("      --manifest-checksums=SHA{224,256,384,512}|CRC32C|NONE\n"
			 "                         use algorithm for manifest checksums\n"));
	printf(_("      --manifest-force-encode\n"
			 "                         hex encode all file names in manifest\n"));
	printf(_("      --no-estimate-size do not estimate backup size in server side\n"));
	printf(_("      --no-manifest      suppress generation of backup manifest\n"));
	printf(_("      --no-slot          prevent creation of temporary replication slot\n"));
	printf(_("      --no-verify-checksums\n"
			 "                         do not verify checksums\n"));
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
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
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
				pg_fatal("could not read from ready pipe: %m");

			if (sscanf(xlogend, "%X/%X", &hi, &lo) != 2)
				pg_fatal("could not parse write-ahead log location \"%s\"",
						 xlogend);
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
	char		xlog[MAXPGPATH];	/* directory or tarfile depending on mode */
	char	   *sysidentifier;
	int			timeline;
	pg_compress_algorithm wal_compress_algorithm;
	int			wal_compress_level;
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
#ifndef WIN32
	stream.stop_socket = bgpipe[0];
#else
	stream.stop_socket = PGINVALID_SOCKET;
#endif
	stream.standby_message_timeout = standby_message_timeout;
	stream.synchronous = false;
	/* fsync happens at the end of pg_basebackup for all data */
	stream.do_sync = false;
	stream.mark_done = true;
	stream.partial_suffix = NULL;
	stream.replication_slot = replication_slot;
	if (format == 'p')
		stream.walmethod = CreateWalDirectoryMethod(param->xlog,
													PG_COMPRESSION_NONE, 0,
													stream.do_sync);
	else
		stream.walmethod = CreateWalTarMethod(param->xlog,
											  param->wal_compress_algorithm,
											  param->wal_compress_level,
											  stream.do_sync);

	if (!ReceiveXlogStream(param->bgconn, &stream))
	{
		/*
		 * Any errors will already have been reported in the function process,
		 * but we need to tell the parent that we didn't shutdown in a nice
		 * way.
		 */
#ifdef WIN32
		/*
		 * In order to signal the main thread of an ungraceful exit we set the
		 * same flag that we use on Unix to signal SIGCHLD.
		 */
		bgchild_exited = true;
#endif
		return 1;
	}

	if (!stream.walmethod->finish())
	{
		pg_log_error("could not finish writing WAL files: %m");
#ifdef WIN32
		bgchild_exited = true;
#endif
		return 1;
	}

	PQfinish(param->bgconn);

	if (format == 'p')
		FreeWalDirectoryMethod();
	else
		FreeWalTarMethod();
	pg_free(stream.walmethod);

	return 0;
}

/*
 * Initiate background process for receiving xlog during the backup.
 * The background stream will use its own database connection so we can
 * stream the logfile in parallel with the backups.
 */
static void
StartLogStreamer(char *startpos, uint32 timeline, char *sysidentifier,
				 pg_compress_algorithm wal_compress_algorithm,
				 int wal_compress_level)
{
	logstreamer_param *param;
	uint32		hi,
				lo;
	char		statusdir[MAXPGPATH];

	param = pg_malloc0(sizeof(logstreamer_param));
	param->timeline = timeline;
	param->sysidentifier = sysidentifier;
	param->wal_compress_algorithm = wal_compress_algorithm;
	param->wal_compress_level = wal_compress_level;

	/* Convert the starting position */
	if (sscanf(startpos, "%X/%X", &hi, &lo) != 2)
		pg_fatal("could not parse write-ahead log location \"%s\"",
				 startpos);
	param->startptr = ((uint64) hi) << 32 | lo;
	/* Round off to even segment position */
	param->startptr -= XLogSegmentOffset(param->startptr, WalSegSz);

#ifndef WIN32
	/* Create our background pipe */
	if (pipe(bgpipe) < 0)
		pg_fatal("could not create pipe for background process: %m");
#endif

	/* Get a second connection */
	param->bgconn = GetConnection();
	if (!param->bgconn)
		/* Error message already written in GetConnection() */
		exit(1);

	/* In post-10 cluster, pg_xlog has been renamed to pg_wal */
	snprintf(param->xlog, sizeof(param->xlog), "%s/%s",
			 basedir,
			 PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ?
			 "pg_xlog" : "pg_wal");

	/* Temporary replication slots are only supported in 10 and newer */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_TEMP_SLOTS)
		temp_replication_slot = false;

	/*
	 * Create replication slot if requested
	 */
	if (temp_replication_slot && !replication_slot)
		replication_slot = psprintf("pg_basebackup_%d", (int) PQbackendPID(param->bgconn));
	if (temp_replication_slot || create_slot)
	{
		if (!CreateReplicationSlot(param->bgconn, replication_slot, NULL,
								   temp_replication_slot, true, true, false, false))
			exit(1);

		if (verbose)
		{
			if (temp_replication_slot)
				pg_log_info("created temporary replication slot \"%s\"",
							replication_slot);
			else
				pg_log_info("created replication slot \"%s\"",
							replication_slot);
		}
	}

	if (format == 'p')
	{
		/*
		 * Create pg_wal/archive_status or pg_xlog/archive_status (and thus
		 * pg_wal or pg_xlog) depending on the target server so we can write
		 * to basedir/pg_wal or basedir/pg_xlog as the directory entry in the
		 * tar file may arrive later.
		 */
		snprintf(statusdir, sizeof(statusdir), "%s/%s/archive_status",
				 basedir,
				 PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ?
				 "pg_xlog" : "pg_wal");

		if (pg_mkdir_p(statusdir, pg_dir_create_mode) != 0 && errno != EEXIST)
			pg_fatal("could not create directory \"%s\": %m", statusdir);
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
		pg_fatal("could not create background process: %m");

	/*
	 * Else we are in the parent process and all is well.
	 */
	atexit(kill_bgchild_atexit);
#else							/* WIN32 */
	bgchild = _beginthreadex(NULL, 0, (void *) LogStreamerMain, param, 0, NULL);
	if (bgchild == 0)
		pg_fatal("could not create background thread: %m");
#endif
}

/*
 * Verify that the given directory exists and is empty. If it does not
 * exist, it is created. If it exists but is not empty, an error will
 * be given and the process ended.
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
			if (pg_mkdir_p(dirname, pg_dir_create_mode) == -1)
				pg_fatal("could not create directory \"%s\": %m", dirname);
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
			pg_fatal("directory \"%s\" exists but is not empty", dirname);
		case -1:

			/*
			 * Access problem
			 */
			pg_fatal("could not access directory \"%s\": %m", dirname);
	}
}

/*
 * Callback to update our notion of the current filename.
 *
 * No other code should modify progress_filename!
 */
static void
progress_update_filename(const char *filename)
{
	/* We needn't maintain this variable if not doing verbose reports. */
	if (showprogress && verbose)
	{
		if (progress_filename)
			free(progress_filename);
		if (filename)
			progress_filename = pg_strdup(filename);
		else
			progress_filename = NULL;
	}
}

/*
 * Print a progress report based on the global variables. If verbose output
 * is enabled, also print the current file name.
 *
 * Progress report is written at maximum once per second, unless the force
 * parameter is set to true.
 *
 * If finished is set to true, this is the last progress report. The cursor
 * is moved to the next line.
 */
static void
progress_report(int tablespacenum, bool force, bool finished)
{
	int			percent;
	char		totaldone_str[32];
	char		totalsize_str[32];
	pg_time_t	now;

	if (!showprogress)
		return;

	now = time(NULL);
	if (now == last_progress_report && !force && !finished)
		return;					/* Max once per second */

	last_progress_report = now;
	percent = totalsize_kb ? (int) ((totaldone / 1024) * 100 / totalsize_kb) : 0;

	/*
	 * Avoid overflowing past 100% or the full size. This may make the total
	 * size number change as we approach the end of the backup (the estimate
	 * will always be wrong if WAL is included), but that's better than having
	 * the done column be bigger than the total.
	 */
	if (percent > 100)
		percent = 100;
	if (totaldone / 1024 > totalsize_kb)
		totalsize_kb = totaldone / 1024;

	snprintf(totaldone_str, sizeof(totaldone_str), UINT64_FORMAT,
			 totaldone / 1024);
	snprintf(totalsize_str, sizeof(totalsize_str), UINT64_FORMAT, totalsize_kb);

#define VERBOSE_FILENAME_LENGTH 35
	if (verbose)
	{
		if (!progress_filename)

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
			bool		truncate = (strlen(progress_filename) > VERBOSE_FILENAME_LENGTH);

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
					truncate ? progress_filename + strlen(progress_filename) - VERBOSE_FILENAME_LENGTH + 3 : progress_filename);
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

	/*
	 * Stay on the same line if reporting to a terminal and we're not done
	 * yet.
	 */
	fputc((!finished && isatty(fileno(stderr))) ? '\r' : '\n', stderr);
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
		pg_fatal("transfer rate \"%s\" is not a valid value", src);
	if (errno != 0)
		pg_fatal("invalid transfer rate \"%s\": %m", src);

	if (result <= 0)
	{
		/*
		 * Reject obviously wrong values here.
		 */
		pg_fatal("transfer rate must be greater than zero");
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
		pg_fatal("invalid --max-rate unit: \"%s\"", suffix);

	/* Valid integer? */
	if ((uint64) result != (uint64) ((uint32) result))
		pg_fatal("transfer rate \"%s\" exceeds integer range", src);

	/*
	 * The range is checked on the server side too, but avoid the server
	 * connection if a nonsensical value was passed.
	 */
	if (result < MAX_RATE_LOWER || result > MAX_RATE_UPPER)
		pg_fatal("transfer rate \"%s\" is out of range", src);

	return (int32) result;
}

/*
 * Basic parsing of a value specified for -Z/--compress.
 *
 * We're not concerned here with understanding exactly what behavior the
 * user wants, but we do need to know whether the user is requesting client
 * or server side compression or leaving it unspecified, and we need to
 * separate the name of the compression algorithm from the detail string.
 *
 * For instance, if the user writes --compress client-lz4:6, we want to
 * separate that into (a) client-side compression, (b) algorithm "lz4",
 * and (c) detail "6". Note, however, that all the client/server prefix is
 * optional, and so is the detail. The algorithm name is required, unless
 * the whole string is an integer, in which case we assume "gzip" as the
 * algorithm and use the integer as the detail.
 *
 * We're not concerned with validation at this stage, so if the user writes
 * --compress client-turkey:sandwich, the requested algorithm is "turkey"
 * and the detail string is "sandwich". We'll sort out whether that's legal
 * at a later stage.
 */
static void
parse_compress_options(char *option, char **algorithm, char **detail,
					   CompressionLocation *locationres)
{
	char	   *sep;
	char	   *endp;

	/*
	 * Check whether the compression specification consists of a bare integer.
	 *
	 * If so, for backward compatibility, assume gzip.
	 */
	(void) strtol(option, &endp, 10);
	if (*endp == '\0')
	{
		*locationres = COMPRESS_LOCATION_UNSPECIFIED;
		*algorithm = pstrdup("gzip");
		*detail = pstrdup(option);
		return;
	}

	/* Strip off any "client-" or "server-" prefix. */
	if (strncmp(option, "server-", 7) == 0)
	{
		*locationres = COMPRESS_LOCATION_SERVER;
		option += 7;
	}
	else if (strncmp(option, "client-", 7) == 0)
	{
		*locationres = COMPRESS_LOCATION_CLIENT;
		option += 7;
	}
	else
		*locationres = COMPRESS_LOCATION_UNSPECIFIED;

	/*
	 * Check whether there is a compression detail following the algorithm
	 * name.
	 */
	sep = strchr(option, ':');
	if (sep == NULL)
	{
		*algorithm = pstrdup(option);
		*detail = NULL;
	}
	else
	{
		char	   *alg;

		alg = palloc((sep - option) + 1);
		memcpy(alg, option, sep - option);
		alg[sep - option] = '\0';

		*algorithm = alg;
		*detail = pstrdup(sep + 1);
	}
}

/*
 * Read a stream of COPY data and invoke the provided callback for each
 * chunk.
 */
static void
ReceiveCopyData(PGconn *conn, WriteDataCallback callback,
				void *callback_data)
{
	PGresult   *res;

	/* Get the COPY data stream. */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
		pg_fatal("could not get COPY data stream: %s",
				 PQerrorMessage(conn));
	PQclear(res);

	/* Loop over chunks until done. */
	while (1)
	{
		int			r;
		char	   *copybuf;

		r = PQgetCopyData(conn, &copybuf, 0);
		if (r == -1)
		{
			/* End of chunk. */
			break;
		}
		else if (r == -2)
			pg_fatal("could not read COPY data: %s",
					 PQerrorMessage(conn));

		if (bgchild_exited)
			pg_fatal("background process terminated unexpectedly");

		(*callback) (r, copybuf, callback_data);

		PQfreemem(copybuf);
	}
}

/*
 * Figure out what to do with an archive received from the server based on
 * the options selected by the user.  We may just write the results directly
 * to a file, or we might compress first, or we might extract the tar file
 * and write each member separately. This function doesn't do any of that
 * directly, but it works out what kind of bbstreamer we need to create so
 * that the right stuff happens when, down the road, we actually receive
 * the data.
 */
static bbstreamer *
CreateBackupStreamer(char *archive_name, char *spclocation,
					 bbstreamer **manifest_inject_streamer_p,
					 bool is_recovery_guc_supported,
					 bool expect_unterminated_tarfile,
					 pg_compress_specification *compress)
{
	bbstreamer *streamer = NULL;
	bbstreamer *manifest_inject_streamer = NULL;
	bool		inject_manifest;
	bool		is_tar,
				is_tar_gz,
				is_tar_lz4,
				is_tar_zstd,
				is_compressed_tar;
	bool		must_parse_archive;
	int			archive_name_len = strlen(archive_name);

	/*
	 * Normally, we emit the backup manifest as a separate file, but when
	 * we're writing a tarfile to stdout, we don't have that option, so
	 * include it in the one tarfile we've got.
	 */
	inject_manifest = (format == 't' && strcmp(basedir, "-") == 0 && manifest);

	/* Is this a tar archive? */
	is_tar = (archive_name_len > 4 &&
			  strcmp(archive_name + archive_name_len - 4, ".tar") == 0);

	/* Is this a .tar.gz archive? */
	is_tar_gz = (archive_name_len > 7 &&
				 strcmp(archive_name + archive_name_len - 7, ".tar.gz") == 0);

	/* Is this a .tar.lz4 archive? */
	is_tar_lz4 = (archive_name_len > 8 &&
				  strcmp(archive_name + archive_name_len - 8, ".tar.lz4") == 0);

	/* Is this a .tar.zst archive? */
	is_tar_zstd = (archive_name_len > 8 &&
				   strcmp(archive_name + archive_name_len - 8, ".tar.zst") == 0);

	/* Is this any kind of compressed tar? */
	is_compressed_tar = is_tar_gz || is_tar_lz4 || is_tar_zstd;

	/*
	 * Injecting the manifest into a compressed tar file would be possible if
	 * we decompressed it, parsed the tarfile, generated a new tarfile, and
	 * recompressed it, but compressing and decompressing multiple times just
	 * to inject the manifest seems inefficient enough that it's probably not
	 * what the user wants. So, instead, reject the request and tell the user
	 * to specify something more reasonable.
	 */
	if (inject_manifest && is_compressed_tar)
	{
		pg_log_error("cannot inject manifest into a compressed tar file");
		pg_log_error_hint("Use client-side compression, send the output to a directory rather than standard output, or use %s.",
						  "--no-manifest");
		exit(1);
	}

	/*
	 * We have to parse the archive if (1) we're suppose to extract it, or if
	 * (2) we need to inject backup_manifest or recovery configuration into
	 * it. However, we only know how to parse tar archives.
	 */
	must_parse_archive = (format == 'p' || inject_manifest ||
						  (spclocation == NULL && writerecoveryconf));

	/* At present, we only know how to parse tar archives. */
	if (must_parse_archive && !is_tar && !is_compressed_tar)
	{
		pg_log_error("cannot parse archive \"%s\"", archive_name);
		pg_log_error_detail("Only tar archives can be parsed.");
		if (format == 'p')
			pg_log_error_detail("Plain format requires pg_basebackup to parse the archive.");
		if (inject_manifest)
			pg_log_error_detail("Using - as the output directory requires pg_basebackup to parse the archive.");
		if (writerecoveryconf)
			pg_log_error_detail("The -R option requires pg_basebackup to parse the archive.");
		exit(1);
	}

	if (format == 'p')
	{
		const char *directory;

		/*
		 * In plain format, we must extract the archive. The data for the main
		 * tablespace will be written to the base directory, and the data for
		 * other tablespaces will be written to the directory where they're
		 * located on the server, after applying any user-specified tablespace
		 * mappings.
		 */
		directory = spclocation == NULL ? basedir
			: get_tablespace_mapping(spclocation);
		streamer = bbstreamer_extractor_new(directory,
											get_tablespace_mapping,
											progress_update_filename);
	}
	else
	{
		FILE	   *archive_file;
		char		archive_filename[MAXPGPATH];

		/*
		 * In tar format, we just write the archive without extracting it.
		 * Normally, we write it to the archive name provided by the caller,
		 * but when the base directory is "-" that means we need to write to
		 * standard output.
		 */
		if (strcmp(basedir, "-") == 0)
		{
			snprintf(archive_filename, sizeof(archive_filename), "-");
			archive_file = stdout;
		}
		else
		{
			snprintf(archive_filename, sizeof(archive_filename),
					 "%s/%s", basedir, archive_name);
			archive_file = NULL;
		}

		if (compress->algorithm == PG_COMPRESSION_NONE)
			streamer = bbstreamer_plain_writer_new(archive_filename,
												   archive_file);
		else if (compress->algorithm == PG_COMPRESSION_GZIP)
		{
			strlcat(archive_filename, ".gz", sizeof(archive_filename));
			streamer = bbstreamer_gzip_writer_new(archive_filename,
												  archive_file, compress);
		}
		else if (compress->algorithm == PG_COMPRESSION_LZ4)
		{
			strlcat(archive_filename, ".lz4", sizeof(archive_filename));
			streamer = bbstreamer_plain_writer_new(archive_filename,
												   archive_file);
			streamer = bbstreamer_lz4_compressor_new(streamer, compress);
		}
		else if (compress->algorithm == PG_COMPRESSION_ZSTD)
		{
			strlcat(archive_filename, ".zst", sizeof(archive_filename));
			streamer = bbstreamer_plain_writer_new(archive_filename,
												   archive_file);
			streamer = bbstreamer_zstd_compressor_new(streamer, compress);
		}
		else
		{
			Assert(false);		/* not reachable */
		}

		/*
		 * If we need to parse the archive for whatever reason, then we'll
		 * also need to re-archive, because, if the output format is tar, the
		 * only point of parsing the archive is to be able to inject stuff
		 * into it.
		 */
		if (must_parse_archive)
			streamer = bbstreamer_tar_archiver_new(streamer);
		progress_update_filename(archive_filename);
	}

	/*
	 * If we're supposed to inject the backup manifest into the results, it
	 * should be done here, so that the file content can be injected directly,
	 * without worrying about the details of the tar format.
	 */
	if (inject_manifest)
		manifest_inject_streamer = streamer;

	/*
	 * If this is the main tablespace and we're supposed to write recovery
	 * information, arrange to do that.
	 */
	if (spclocation == NULL && writerecoveryconf)
	{
		Assert(must_parse_archive);
		streamer = bbstreamer_recovery_injector_new(streamer,
													is_recovery_guc_supported,
													recoveryconfcontents);
	}

	/*
	 * If we're doing anything that involves understanding the contents of the
	 * archive, we'll need to parse it. If not, we can skip parsing it, but
	 * old versions of the server send improperly terminated tarfiles, so if
	 * we're talking to such a server we'll need to add the terminator here.
	 */
	if (must_parse_archive)
		streamer = bbstreamer_tar_parser_new(streamer);
	else if (expect_unterminated_tarfile)
		streamer = bbstreamer_tar_terminator_new(streamer);

	/*
	 * If the user has requested a server compressed archive along with
	 * archive extraction at client then we need to decompress it.
	 */
	if (format == 'p')
	{
		if (is_tar_gz)
			streamer = bbstreamer_gzip_decompressor_new(streamer);
		else if (is_tar_lz4)
			streamer = bbstreamer_lz4_decompressor_new(streamer);
		else if (is_tar_zstd)
			streamer = bbstreamer_zstd_decompressor_new(streamer);
	}

	/* Return the results. */
	*manifest_inject_streamer_p = manifest_inject_streamer;
	return streamer;
}

/*
 * Receive all of the archives the server wants to send - and the backup
 * manifest if present - as a single COPY stream.
 */
static void
ReceiveArchiveStream(PGconn *conn, pg_compress_specification *compress)
{
	ArchiveStreamState state;

	/* Set up initial state. */
	memset(&state, 0, sizeof(state));
	state.tablespacenum = -1;
	state.compress = compress;

	/* All the real work happens in ReceiveArchiveStreamChunk. */
	ReceiveCopyData(conn, ReceiveArchiveStreamChunk, &state);

	/* If we wrote the backup manifest to a file, close the file. */
	if (state.manifest_file !=NULL)
	{
		fclose(state.manifest_file);
		state.manifest_file = NULL;
	}

	/*
	 * If we buffered the backup manifest in order to inject it into the
	 * output tarfile, do that now.
	 */
	if (state.manifest_inject_streamer != NULL &&
		state.manifest_buffer != NULL)
	{
		bbstreamer_inject_file(state.manifest_inject_streamer,
							   "backup_manifest",
							   state.manifest_buffer->data,
							   state.manifest_buffer->len);
		destroyPQExpBuffer(state.manifest_buffer);
		state.manifest_buffer = NULL;
	}

	/* If there's still an archive in progress, end processing. */
	if (state.streamer != NULL)
	{
		bbstreamer_finalize(state.streamer);
		bbstreamer_free(state.streamer);
		state.streamer = NULL;
	}
}

/*
 * Receive one chunk of data sent by the server as part of a single COPY
 * stream that includes all archives and the manifest.
 */
static void
ReceiveArchiveStreamChunk(size_t r, char *copybuf, void *callback_data)
{
	ArchiveStreamState *state = callback_data;
	size_t		cursor = 0;

	/* Each CopyData message begins with a type byte. */
	switch (GetCopyDataByte(r, copybuf, &cursor))
	{
		case 'n':
			{
				/* New archive. */
				char	   *archive_name;
				char	   *spclocation;

				/*
				 * We force a progress report at the end of each tablespace. A
				 * new tablespace starts when the previous one ends, except in
				 * the case of the very first one.
				 */
				if (++state->tablespacenum > 0)
					progress_report(state->tablespacenum, true, false);

				/* Sanity check. */
				if (state->manifest_buffer != NULL ||
					state->manifest_file !=NULL)
					pg_fatal("archives must precede manifest");

				/* Parse the rest of the CopyData message. */
				archive_name = GetCopyDataString(r, copybuf, &cursor);
				spclocation = GetCopyDataString(r, copybuf, &cursor);
				GetCopyDataEnd(r, copybuf, cursor);

				/*
				 * Basic sanity checks on the archive name: it shouldn't be
				 * empty, it shouldn't start with a dot, and it shouldn't
				 * contain a path separator.
				 */
				if (archive_name[0] == '\0' || archive_name[0] == '.' ||
					strchr(archive_name, '/') != NULL ||
					strchr(archive_name, '\\') != NULL)
					pg_fatal("invalid archive name: \"%s\"",
							 archive_name);

				/*
				 * An empty spclocation is treated as NULL. We expect this
				 * case to occur for the data directory itself, but not for
				 * any archives that correspond to tablespaces.
				 */
				if (spclocation[0] == '\0')
					spclocation = NULL;

				/* End processing of any prior archive. */
				if (state->streamer != NULL)
				{
					bbstreamer_finalize(state->streamer);
					bbstreamer_free(state->streamer);
					state->streamer = NULL;
				}

				/*
				 * Create an appropriate backup streamer, unless a backup
				 * target was specified. In that case, it's up to the server
				 * to put the backup wherever it needs to go.
				 */
				if (backup_target == NULL)
				{
					/*
					 * We know that recovery GUCs are supported, because this
					 * protocol can only be used on v15+.
					 */
					state->streamer =
						CreateBackupStreamer(archive_name,
											 spclocation,
											 &state->manifest_inject_streamer,
											 true, false,
											 state->compress);
				}
				break;
			}

		case 'd':
			{
				/* Archive or manifest data. */
				if (state->manifest_buffer != NULL)
				{
					/* Manifest data, buffer in memory. */
					appendPQExpBuffer(state->manifest_buffer, copybuf + 1,
									  r - 1);
				}
				else if (state->manifest_file !=NULL)
				{
					/* Manifest data, write to disk. */
					if (fwrite(copybuf + 1, r - 1, 1,
							   state->manifest_file) != 1)
					{
						/*
						 * If fwrite() didn't set errno, assume that the
						 * problem is that we're out of disk space.
						 */
						if (errno == 0)
							errno = ENOSPC;
						pg_fatal("could not write to file \"%s\": %m",
								 state->manifest_filename);
					}
				}
				else if (state->streamer != NULL)
				{
					/* Archive data. */
					bbstreamer_content(state->streamer, NULL, copybuf + 1,
									   r - 1, BBSTREAMER_UNKNOWN);
				}
				else
					pg_fatal("unexpected payload data");
				break;
			}

		case 'p':
			{
				/*
				 * Progress report.
				 *
				 * The remainder of the message is expected to be an 8-byte
				 * count of bytes completed.
				 */
				totaldone = GetCopyDataUInt64(r, copybuf, &cursor);
				GetCopyDataEnd(r, copybuf, cursor);

				/*
				 * The server shouldn't send progress report messages too
				 * often, so we force an update each time we receive one.
				 */
				progress_report(state->tablespacenum, true, false);
				break;
			}

		case 'm':
			{
				/*
				 * Manifest data will be sent next. This message is not
				 * expected to have any further payload data.
				 */
				GetCopyDataEnd(r, copybuf, cursor);

				/*
				 * If a backup target was specified, figuring out where to put
				 * the manifest is the server's problem. Otherwise, we need to
				 * deal with it.
				 */
				if (backup_target == NULL)
				{
					/*
					 * If we're supposed inject the manifest into the archive,
					 * we prepare to buffer it in memory; otherwise, we
					 * prepare to write it to a temporary file.
					 */
					if (state->manifest_inject_streamer != NULL)
						state->manifest_buffer = createPQExpBuffer();
					else
					{
						snprintf(state->manifest_filename,
								 sizeof(state->manifest_filename),
								 "%s/backup_manifest.tmp", basedir);
						state->manifest_file =
							fopen(state->manifest_filename, "wb");
						if (state->manifest_file == NULL)
							pg_fatal("could not create file \"%s\": %m",
									 state->manifest_filename);
					}
				}
				break;
			}

		default:
			ReportCopyDataParseError(r, copybuf);
			break;
	}
}

/*
 * Get a single byte from a CopyData message.
 *
 * Bail out if none remain.
 */
static char
GetCopyDataByte(size_t r, char *copybuf, size_t *cursor)
{
	if (*cursor >= r)
		ReportCopyDataParseError(r, copybuf);

	return copybuf[(*cursor)++];
}

/*
 * Get a NUL-terminated string from a CopyData message.
 *
 * Bail out if the terminating NUL cannot be found.
 */
static char *
GetCopyDataString(size_t r, char *copybuf, size_t *cursor)
{
	size_t		startpos = *cursor;
	size_t		endpos = startpos;

	while (1)
	{
		if (endpos >= r)
			ReportCopyDataParseError(r, copybuf);
		if (copybuf[endpos] == '\0')
			break;
		++endpos;
	}

	*cursor = endpos + 1;
	return &copybuf[startpos];
}

/*
 * Get an unsigned 64-bit integer from a CopyData message.
 *
 * Bail out if there are not at least 8 bytes remaining.
 */
static uint64
GetCopyDataUInt64(size_t r, char *copybuf, size_t *cursor)
{
	uint64		result;

	if (*cursor + sizeof(uint64) > r)
		ReportCopyDataParseError(r, copybuf);
	memcpy(&result, &copybuf[*cursor], sizeof(uint64));
	*cursor += sizeof(uint64);
	return pg_ntoh64(result);
}

/*
 * Bail out if we didn't parse the whole message.
 */
static void
GetCopyDataEnd(size_t r, char *copybuf, size_t cursor)
{
	if (r != cursor)
		ReportCopyDataParseError(r, copybuf);
}

/*
 * Report failure to parse a CopyData message from the server. Then exit.
 *
 * As a debugging aid, we try to give some hint about what kind of message
 * provoked the failure. Perhaps this is not detailed enough, but it's not
 * clear that it's worth expending any more code on what should be a
 * can't-happen case.
 */
static void
ReportCopyDataParseError(size_t r, char *copybuf)
{
	if (r == 0)
		pg_fatal("empty COPY message");
	else
		pg_fatal("malformed COPY message of type %d, length %zu",
				 copybuf[0], r);
}

/*
 * Receive raw tar data from the server, and stream it to the appropriate
 * location. If we're writing a single tarfile to standard output, also
 * receive the backup manifest and inject it into that tarfile.
 */
static void
ReceiveTarFile(PGconn *conn, char *archive_name, char *spclocation,
			   bool tablespacenum, pg_compress_specification *compress)
{
	WriteTarState state;
	bbstreamer *manifest_inject_streamer;
	bool		is_recovery_guc_supported;
	bool		expect_unterminated_tarfile;

	/* Pass all COPY data through to the backup streamer. */
	memset(&state, 0, sizeof(state));
	is_recovery_guc_supported =
		PQserverVersion(conn) >= MINIMUM_VERSION_FOR_RECOVERY_GUC;
	expect_unterminated_tarfile =
		PQserverVersion(conn) < MINIMUM_VERSION_FOR_TERMINATED_TARFILE;
	state.streamer = CreateBackupStreamer(archive_name, spclocation,
										  &manifest_inject_streamer,
										  is_recovery_guc_supported,
										  expect_unterminated_tarfile,
										  compress);
	state.tablespacenum = tablespacenum;
	ReceiveCopyData(conn, ReceiveTarCopyChunk, &state);
	progress_update_filename(NULL);

	/*
	 * The decision as to whether we need to inject the backup manifest into
	 * the output at this stage is made by CreateBackupStreamer; if that is
	 * needed, manifest_inject_streamer will be non-NULL; otherwise, it will
	 * be NULL.
	 */
	if (manifest_inject_streamer != NULL)
	{
		PQExpBufferData buf;

		/* Slurp the entire backup manifest into a buffer. */
		initPQExpBuffer(&buf);
		ReceiveBackupManifestInMemory(conn, &buf);
		if (PQExpBufferDataBroken(buf))
			pg_fatal("out of memory");

		/* Inject it into the output tarfile. */
		bbstreamer_inject_file(manifest_inject_streamer, "backup_manifest",
							   buf.data, buf.len);

		/* Free memory. */
		termPQExpBuffer(&buf);
	}

	/* Cleanup. */
	bbstreamer_finalize(state.streamer);
	bbstreamer_free(state.streamer);

	progress_report(tablespacenum, true, false);

	/*
	 * Do not sync the resulting tar file yet, all files are synced once at
	 * the end.
	 */
}

/*
 * Receive one chunk of tar-format data from the server.
 */
static void
ReceiveTarCopyChunk(size_t r, char *copybuf, void *callback_data)
{
	WriteTarState *state = callback_data;

	bbstreamer_content(state->streamer, NULL, copybuf, r, BBSTREAMER_UNKNOWN);

	totaldone += r;
	progress_report(state->tablespacenum, false, false);
}


/*
 * Retrieve tablespace path, either relocated or original depending on whether
 * -T was passed or not.
 */
static const char *
get_tablespace_mapping(const char *dir)
{
	TablespaceListCell *cell;
	char		canon_dir[MAXPGPATH];

	/* Canonicalize path for comparison consistency */
	strlcpy(canon_dir, dir, sizeof(canon_dir));
	canonicalize_path(canon_dir);

	for (cell = tablespace_dirs.head; cell; cell = cell->next)
		if (strcmp(canon_dir, cell->old_dir) == 0)
			return cell->new_dir;

	return dir;
}

/*
 * Receive the backup manifest file and write it out to a file.
 */
static void
ReceiveBackupManifest(PGconn *conn)
{
	WriteManifestState state;

	snprintf(state.filename, sizeof(state.filename),
			 "%s/backup_manifest.tmp", basedir);
	state.file = fopen(state.filename, "wb");
	if (state.file == NULL)
		pg_fatal("could not create file \"%s\": %m", state.filename);

	ReceiveCopyData(conn, ReceiveBackupManifestChunk, &state);

	fclose(state.file);
}

/*
 * Receive one chunk of the backup manifest file and write it out to a file.
 */
static void
ReceiveBackupManifestChunk(size_t r, char *copybuf, void *callback_data)
{
	WriteManifestState *state = callback_data;

	errno = 0;
	if (fwrite(copybuf, r, 1, state->file) != 1)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to file \"%s\": %m", state->filename);
	}
}

/*
 * Receive the backup manifest file and write it out to a file.
 */
static void
ReceiveBackupManifestInMemory(PGconn *conn, PQExpBuffer buf)
{
	ReceiveCopyData(conn, ReceiveBackupManifestInMemoryChunk, buf);
}

/*
 * Receive one chunk of the backup manifest file and write it out to a file.
 */
static void
ReceiveBackupManifestInMemoryChunk(size_t r, char *copybuf,
								   void *callback_data)
{
	PQExpBuffer buf = callback_data;

	appendPQExpBuffer(buf, copybuf, r);
}

static void
BaseBackup(char *compression_algorithm, char *compression_detail,
		   CompressionLocation compressloc, pg_compress_specification *client_compress)
{
	PGresult   *res;
	char	   *sysidentifier;
	TimeLineID	latesttli;
	TimeLineID	starttli;
	char	   *basebkp;
	int			i;
	char		xlogstart[64];
	char		xlogend[64];
	int			minServerMajor,
				maxServerMajor;
	int			serverVersion,
				serverMajor;
	int			writing_to_stdout;
	bool		use_new_option_syntax = false;
	PQExpBufferData buf;

	Assert(conn != NULL);
	initPQExpBuffer(&buf);

	/*
	 * Check server version. BASE_BACKUP command was introduced in 9.1, so we
	 * can't work with servers older than 9.1.
	 */
	minServerMajor = 901;
	maxServerMajor = PG_VERSION_NUM / 100;
	serverVersion = PQserverVersion(conn);
	serverMajor = serverVersion / 100;
	if (serverMajor < minServerMajor || serverMajor > maxServerMajor)
	{
		const char *serverver = PQparameterStatus(conn, "server_version");

		pg_fatal("incompatible server version %s",
				 serverver ? serverver : "'unknown'");
	}
	if (serverMajor >= 1500)
		use_new_option_syntax = true;

	/*
	 * If WAL streaming was requested, also check that the server is new
	 * enough for that.
	 */
	if (includewal == STREAM_WAL && !CheckServerVersionForStreaming(conn))
	{
		/*
		 * Error message already written in CheckServerVersionForStreaming(),
		 * but add a hint about using -X none.
		 */
		pg_log_error_hint("Use -X none or -X fetch to disable log streaming.");
		exit(1);
	}

	/*
	 * Build contents of configuration file if requested
	 */
	if (writerecoveryconf)
		recoveryconfcontents = GenerateRecoveryConfig(conn, replication_slot);

	/*
	 * Run IDENTIFY_SYSTEM so we can get the timeline
	 */
	if (!RunIdentifySystem(conn, &sysidentifier, &latesttli, NULL, NULL))
		exit(1);

	/*
	 * Start the actual backup
	 */
	AppendStringCommandOption(&buf, use_new_option_syntax, "LABEL", label);
	if (estimatesize)
		AppendPlainCommandOption(&buf, use_new_option_syntax, "PROGRESS");
	if (includewal == FETCH_WAL)
		AppendPlainCommandOption(&buf, use_new_option_syntax, "WAL");
	if (fastcheckpoint)
	{
		if (use_new_option_syntax)
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "CHECKPOINT", "fast");
		else
			AppendPlainCommandOption(&buf, use_new_option_syntax, "FAST");
	}
	if (includewal != NO_WAL)
	{
		if (use_new_option_syntax)
			AppendIntegerCommandOption(&buf, use_new_option_syntax, "WAIT", 0);
		else
			AppendPlainCommandOption(&buf, use_new_option_syntax, "NOWAIT");
	}
	if (maxrate > 0)
		AppendIntegerCommandOption(&buf, use_new_option_syntax, "MAX_RATE",
								   maxrate);
	if (format == 't')
		AppendPlainCommandOption(&buf, use_new_option_syntax, "TABLESPACE_MAP");
	if (!verify_checksums)
	{
		if (use_new_option_syntax)
			AppendIntegerCommandOption(&buf, use_new_option_syntax,
									   "VERIFY_CHECKSUMS", 0);
		else
			AppendPlainCommandOption(&buf, use_new_option_syntax,
									 "NOVERIFY_CHECKSUMS");
	}

	if (manifest)
	{
		AppendStringCommandOption(&buf, use_new_option_syntax, "MANIFEST",
								  manifest_force_encode ? "force-encode" : "yes");
		if (manifest_checksums != NULL)
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "MANIFEST_CHECKSUMS", manifest_checksums);
	}

	if (backup_target != NULL)
	{
		char	   *colon;

		if (serverMajor < 1500)
			pg_fatal("backup targets are not supported by this server version");

		if (writerecoveryconf)
			pg_fatal("recovery configuration cannot be written when a backup target is used");

		AppendPlainCommandOption(&buf, use_new_option_syntax, "TABLESPACE_MAP");

		if ((colon = strchr(backup_target, ':')) == NULL)
		{
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "TARGET", backup_target);
		}
		else
		{
			char	   *target;

			target = pnstrdup(backup_target, colon - backup_target);
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "TARGET", target);
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "TARGET_DETAIL", colon + 1);
		}
	}
	else if (serverMajor >= 1500)
		AppendStringCommandOption(&buf, use_new_option_syntax,
								  "TARGET", "client");

	if (compressloc == COMPRESS_LOCATION_SERVER)
	{
		if (!use_new_option_syntax)
			pg_fatal("server does not support server-side compression");
		AppendStringCommandOption(&buf, use_new_option_syntax,
								  "COMPRESSION", compression_algorithm);
		if (compression_detail != NULL)
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "COMPRESSION_DETAIL",
									  compression_detail);
	}

	if (verbose)
		pg_log_info("initiating base backup, waiting for checkpoint to complete");

	if (showprogress && !verbose)
	{
		fprintf(stderr, _("waiting for checkpoint"));
		if (isatty(fileno(stderr)))
			fprintf(stderr, "\r");
		else
			fprintf(stderr, "\n");
	}

	if (use_new_option_syntax && buf.len > 0)
		basebkp = psprintf("BASE_BACKUP (%s)", buf.data);
	else
		basebkp = psprintf("BASE_BACKUP %s", buf.data);

	if (PQsendQuery(conn, basebkp) == 0)
		pg_fatal("could not send replication command \"%s\": %s",
				 "BASE_BACKUP", PQerrorMessage(conn));

	/*
	 * Get the starting WAL location
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not initiate base backup: %s",
				 PQerrorMessage(conn));
	if (PQntuples(res) != 1)
		pg_fatal("server returned unexpected response to BASE_BACKUP command; got %d rows and %d fields, expected %d rows and %d fields",
				 PQntuples(res), PQnfields(res), 1, 2);

	strlcpy(xlogstart, PQgetvalue(res, 0, 0), sizeof(xlogstart));

	if (verbose)
		pg_log_info("checkpoint completed");

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

	if (verbose && includewal != NO_WAL)
		pg_log_info("write-ahead log start point: %s on timeline %u",
					xlogstart, starttli);

	/*
	 * Get the header
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not get backup header: %s",
				 PQerrorMessage(conn));
	if (PQntuples(res) < 1)
		pg_fatal("no data returned from server");

	/*
	 * Sum up the total size, for progress reporting
	 */
	totalsize_kb = totaldone = 0;
	tablespacecount = PQntuples(res);
	for (i = 0; i < PQntuples(res); i++)
	{
		totalsize_kb += atol(PQgetvalue(res, i, 2));

		/*
		 * Verify tablespace directories are empty. Don't bother with the
		 * first once since it can be relocated, and it will be checked before
		 * we do anything anyway.
		 *
		 * Note that this is skipped for tar format backups and backups that
		 * the server is storing to a target location, since in that case we
		 * won't be storing anything into these directories and thus should
		 * not create them.
		 */
		if (backup_target == NULL && format == 'p' && !PQgetisnull(res, i, 1))
		{
			char	   *path = unconstify(char *, get_tablespace_mapping(PQgetvalue(res, i, 1)));

			verify_dir_is_empty_or_create(path, &made_tablespace_dirs, &found_tablespace_dirs);
		}
	}

	/*
	 * When writing to stdout, require a single tablespace
	 */
	writing_to_stdout = format == 't' && basedir != NULL &&
		strcmp(basedir, "-") == 0;
	if (writing_to_stdout && PQntuples(res) > 1)
		pg_fatal("can only write single tablespace to stdout, database has %d",
				 PQntuples(res));

	/*
	 * If we're streaming WAL, start the streaming session before we start
	 * receiving the actual data chunks.
	 */
	if (includewal == STREAM_WAL)
	{
		pg_compress_algorithm wal_compress_algorithm;
		int			wal_compress_level;

		if (verbose)
			pg_log_info("starting background WAL receiver");

		if (client_compress->algorithm == PG_COMPRESSION_GZIP)
		{
			wal_compress_algorithm = PG_COMPRESSION_GZIP;
			wal_compress_level = client_compress->level;
		}
		else
		{
			wal_compress_algorithm = PG_COMPRESSION_NONE;
			wal_compress_level = 0;
		}

		StartLogStreamer(xlogstart, starttli, sysidentifier,
						 wal_compress_algorithm,
						 wal_compress_level);
	}

	if (serverMajor >= 1500)
	{
		/* Receive a single tar stream with everything. */
		ReceiveArchiveStream(conn, client_compress);
	}
	else
	{
		/* Receive a tar file for each tablespace in turn */
		for (i = 0; i < PQntuples(res); i++)
		{
			char		archive_name[MAXPGPATH];
			char	   *spclocation;

			/*
			 * If we write the data out to a tar file, it will be named
			 * base.tar if it's the main data directory or <tablespaceoid>.tar
			 * if it's for another tablespace. CreateBackupStreamer() will
			 * arrange to add .gz to the archive name if pg_basebackup is
			 * performing compression.
			 */
			if (PQgetisnull(res, i, 0))
			{
				strlcpy(archive_name, "base.tar", sizeof(archive_name));
				spclocation = NULL;
			}
			else
			{
				snprintf(archive_name, sizeof(archive_name),
						 "%s.tar", PQgetvalue(res, i, 0));
				spclocation = PQgetvalue(res, i, 1);
			}

			ReceiveTarFile(conn, archive_name, spclocation, i,
						   client_compress);
		}

		/*
		 * Now receive backup manifest, if appropriate.
		 *
		 * If we're writing a tarfile to stdout, ReceiveTarFile will have
		 * already processed the backup manifest and included it in the output
		 * tarfile.  Such a configuration doesn't allow for writing multiple
		 * files.
		 *
		 * If we're talking to an older server, it won't send a backup
		 * manifest, so don't try to receive one.
		 */
		if (!writing_to_stdout && manifest)
			ReceiveBackupManifest(conn);
	}

	if (showprogress)
	{
		progress_update_filename(NULL);
		progress_report(PQntuples(res), true, true);
	}

	PQclear(res);

	/*
	 * Get the stop position
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("backup failed: %s",
				 PQerrorMessage(conn));
	if (PQntuples(res) != 1)
		pg_fatal("no write-ahead log end position returned from server");
	strlcpy(xlogend, PQgetvalue(res, 0, 0), sizeof(xlogend));
	if (verbose && includewal != NO_WAL)
		pg_log_info("write-ahead log end point: %s", xlogend);
	PQclear(res);

	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);

		if (sqlstate &&
			strcmp(sqlstate, ERRCODE_DATA_CORRUPTED) == 0)
		{
			pg_log_error("checksum error occurred");
			checksum_failure = true;
		}
		else
		{
			pg_log_error("final receive failed: %s",
						 PQerrorMessage(conn));
		}
		exit(1);
	}

	if (bgchild > 0)
	{
#ifndef WIN32
		int			status;
		pid_t		r;
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
			pg_log_info("waiting for background process to finish streaming ...");

#ifndef WIN32
		if (write(bgpipe[1], xlogend, strlen(xlogend)) != strlen(xlogend))
			pg_fatal("could not send command to background pipe: %m");

		/* Just wait for the background process to exit */
		r = waitpid(bgchild, &status, 0);
		if (r == (pid_t) -1)
			pg_fatal("could not wait for child process: %m");
		if (r != bgchild)
			pg_fatal("child %d died, expected %d", (int) r, (int) bgchild);
		if (status != 0)
			pg_fatal("%s", wait_result_to_str(status));
		/* Exited normally, we're happy! */
#else							/* WIN32 */

		/*
		 * On Windows, since we are in the same process, we can just store the
		 * value directly in the variable, and then set the flag that says
		 * it's there.
		 */
		if (sscanf(xlogend, "%X/%X", &hi, &lo) != 2)
			pg_fatal("could not parse write-ahead log location \"%s\"",
					 xlogend);
		xlogendptr = ((uint64) hi) << 32 | lo;
		InterlockedIncrement(&has_xlogendptr);

		/* First wait for the thread to exit */
		if (WaitForSingleObjectEx((HANDLE) bgchild_handle, INFINITE, FALSE) !=
			WAIT_OBJECT_0)
		{
			_dosmaperr(GetLastError());
			pg_fatal("could not wait for child thread: %m");
		}
		if (GetExitCodeThread((HANDLE) bgchild_handle, &status) == 0)
		{
			_dosmaperr(GetLastError());
			pg_fatal("could not get child thread exit status: %m");
		}
		if (status != 0)
			pg_fatal("child thread exited with error %u",
					 (unsigned int) status);
		/* Exited normally, we're happy */
#endif
	}

	/* Free the configuration file contents */
	destroyPQExpBuffer(recoveryconfcontents);

	/*
	 * End of copy data. Final result is already checked inside the loop.
	 */
	PQclear(res);
	PQfinish(conn);
	conn = NULL;

	/*
	 * Make data persistent on disk once backup is completed. For tar format
	 * sync the parent directory and all its contents as each tar file was not
	 * synced after being completed.  In plain format, all the data of the
	 * base directory is synced, taking into account all the tablespaces.
	 * Errors are not considered fatal.
	 *
	 * If, however, there's a backup target, we're not writing anything
	 * locally, so in that case we skip this step.
	 */
	if (do_sync && backup_target == NULL)
	{
		if (verbose)
			pg_log_info("syncing data to disk ...");
		if (format == 't')
		{
			if (strcmp(basedir, "-") != 0)
				(void) fsync_dir_recurse(basedir);
		}
		else
		{
			(void) fsync_pgdata(basedir, serverVersion);
		}
	}

	/*
	 * After synchronizing data to disk, perform a durable rename of
	 * backup_manifest.tmp to backup_manifest, if we wrote such a file. This
	 * way, a failure or system crash before we reach this point will leave us
	 * without a backup_manifest file, decreasing the chances that a directory
	 * we leave behind will be mistaken for a valid backup.
	 */
	if (!writing_to_stdout && manifest && backup_target == NULL)
	{
		char		tmp_filename[MAXPGPATH];
		char		filename[MAXPGPATH];

		if (verbose)
			pg_log_info("renaming backup_manifest.tmp to backup_manifest");

		snprintf(tmp_filename, MAXPGPATH, "%s/backup_manifest.tmp", basedir);
		snprintf(filename, MAXPGPATH, "%s/backup_manifest", basedir);

		if (do_sync)
		{
			/* durable_rename emits its own log message in case of failure */
			if (durable_rename(tmp_filename, filename) != 0)
				exit(1);
		}
		else
		{
			if (rename(tmp_filename, filename) != 0)
				pg_fatal("could not rename file \"%s\" to \"%s\": %m",
						 tmp_filename, filename);
		}
	}

	if (verbose)
		pg_log_info("base backup completed");
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
		{"create-slot", no_argument, NULL, 'C'},
		{"max-rate", required_argument, NULL, 'r'},
		{"write-recovery-conf", no_argument, NULL, 'R'},
		{"slot", required_argument, NULL, 'S'},
		{"target", required_argument, NULL, 't'},
		{"tablespace-mapping", required_argument, NULL, 'T'},
		{"wal-method", required_argument, NULL, 'X'},
		{"gzip", no_argument, NULL, 'z'},
		{"compress", required_argument, NULL, 'Z'},
		{"label", required_argument, NULL, 'l'},
		{"no-clean", no_argument, NULL, 'n'},
		{"no-sync", no_argument, NULL, 'N'},
		{"dbname", required_argument, NULL, 'd'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"status-interval", required_argument, NULL, 's'},
		{"verbose", no_argument, NULL, 'v'},
		{"progress", no_argument, NULL, 'P'},
		{"waldir", required_argument, NULL, 1},
		{"no-slot", no_argument, NULL, 2},
		{"no-verify-checksums", no_argument, NULL, 3},
		{"no-estimate-size", no_argument, NULL, 4},
		{"no-manifest", no_argument, NULL, 5},
		{"manifest-force-encode", no_argument, NULL, 6},
		{"manifest-checksums", required_argument, NULL, 7},
		{NULL, 0, NULL, 0}
	};
	int			c;

	int			option_index;
	char	   *compression_algorithm = "none";
	char	   *compression_detail = NULL;
	CompressionLocation compressloc = COMPRESS_LOCATION_UNSPECIFIED;
	pg_compress_specification client_compress;

	pg_logging_init(argv[0]);
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

	while ((c = getopt_long(argc, argv, "CD:F:r:RS:t:T:X:l:nNzZ:d:c:h:p:U:s:wWkvP",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'C':
				create_slot = true;
				break;
			case 'D':
				basedir = pg_strdup(optarg);
				break;
			case 'F':
				if (strcmp(optarg, "p") == 0 || strcmp(optarg, "plain") == 0)
					format = 'p';
				else if (strcmp(optarg, "t") == 0 || strcmp(optarg, "tar") == 0)
					format = 't';
				else
					pg_fatal("invalid output format \"%s\", must be \"plain\" or \"tar\"",
							 optarg);
				break;
			case 'r':
				maxrate = parse_max_rate(optarg);
				break;
			case 'R':
				writerecoveryconf = true;
				break;
			case 'S':

				/*
				 * When specifying replication slot name, use a permanent
				 * slot.
				 */
				replication_slot = pg_strdup(optarg);
				temp_replication_slot = false;
				break;
			case 2:
				no_slot = true;
				break;
			case 't':
				backup_target = pg_strdup(optarg);
				break;
			case 'T':
				tablespace_list_append(optarg);
				break;
			case 'X':
				if (strcmp(optarg, "n") == 0 ||
					strcmp(optarg, "none") == 0)
				{
					includewal = NO_WAL;
				}
				else if (strcmp(optarg, "f") == 0 ||
						 strcmp(optarg, "fetch") == 0)
				{
					includewal = FETCH_WAL;
				}
				else if (strcmp(optarg, "s") == 0 ||
						 strcmp(optarg, "stream") == 0)
				{
					includewal = STREAM_WAL;
				}
				else
					pg_fatal("invalid wal-method option \"%s\", must be \"fetch\", \"stream\", or \"none\"",
							 optarg);
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
			case 'N':
				do_sync = false;
				break;
			case 'z':
				compression_algorithm = "gzip";
				compression_detail = NULL;
				compressloc = COMPRESS_LOCATION_UNSPECIFIED;
				break;
			case 'Z':
				parse_compress_options(optarg, &compression_algorithm,
									   &compression_detail, &compressloc);
				break;
			case 'c':
				if (pg_strcasecmp(optarg, "fast") == 0)
					fastcheckpoint = true;
				else if (pg_strcasecmp(optarg, "spread") == 0)
					fastcheckpoint = false;
				else
					pg_fatal("invalid checkpoint argument \"%s\", must be \"fast\" or \"spread\"",
							 optarg);
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
				if (!option_parse_int(optarg, "-s/--status-interval", 0,
									  INT_MAX / 1000,
									  &standby_message_timeout))
					exit(1);
				standby_message_timeout *= 1000;
				break;
			case 'v':
				verbose++;
				break;
			case 'P':
				showprogress = true;
				break;
			case 3:
				verify_checksums = false;
				break;
			case 4:
				estimatesize = false;
				break;
			case 5:
				manifest = false;
				break;
			case 6:
				manifest_force_encode = true;
				break;
			case 7:
				manifest_checksums = pg_strdup(optarg);
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	/*
	 * Any non-option arguments?
	 */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * Setting the backup target to 'client' is equivalent to leaving out the
	 * option. This logic allows us to assume elsewhere that the backup is
	 * being stored locally if and only if backup_target == NULL.
	 */
	if (backup_target != NULL && strcmp(backup_target, "client") == 0)
	{
		pg_free(backup_target);
		backup_target = NULL;
	}

	/*
	 * Can't use --format with --target. Without --target, default format is
	 * tar.
	 */
	if (backup_target != NULL && format != '\0')
	{
		pg_log_error("cannot specify both format and backup target");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	if (format == '\0')
		format = 'p';

	/*
	 * Either directory or backup target should be specified, but not both
	 */
	if (basedir == NULL && backup_target == NULL)
	{
		pg_log_error("must specify output directory or backup target");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	if (basedir != NULL && backup_target != NULL)
	{
		pg_log_error("cannot specify both output directory and backup target");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * If the user has not specified where to perform backup compression,
	 * default to the client, unless the user specified --target, in which
	 * case the server is the only choice.
	 */
	if (compressloc == COMPRESS_LOCATION_UNSPECIFIED)
	{
		if (backup_target == NULL)
			compressloc = COMPRESS_LOCATION_CLIENT;
		else
			compressloc = COMPRESS_LOCATION_SERVER;
	}

	/*
	 * If any compression that we're doing is happening on the client side, we
	 * must try to parse the compression algorithm and detail, but if it's all
	 * on the server side, then we're just going to pass through whatever was
	 * requested and let the server decide what to do.
	 */
	if (compressloc == COMPRESS_LOCATION_CLIENT)
	{
		pg_compress_algorithm alg;
		char	   *error_detail;

		if (!parse_compress_algorithm(compression_algorithm, &alg))
			pg_fatal("unrecognized compression algorithm: \"%s\"",
					 compression_algorithm);

		parse_compress_specification(alg, compression_detail, &client_compress);
		error_detail = validate_compress_specification(&client_compress);
		if (error_detail != NULL)
			pg_fatal("invalid compression specification: %s",
					 error_detail);
	}
	else
	{
		Assert(compressloc == COMPRESS_LOCATION_SERVER);
		client_compress.algorithm = PG_COMPRESSION_NONE;
		client_compress.options = 0;
	}

	/*
	 * Can't perform client-side compression if the backup is not being sent
	 * to the client.
	 */
	if (backup_target != NULL && compressloc == COMPRESS_LOCATION_CLIENT)
	{
		pg_log_error("client-side compression is not possible when a backup target is specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * Client-side compression doesn't make sense unless tar format is in use.
	 */
	if (format == 'p' && compressloc == COMPRESS_LOCATION_CLIENT &&
		client_compress.algorithm != PG_COMPRESSION_NONE)
	{
		pg_log_error("only tar mode backups can be compressed");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * Sanity checks for WAL method.
	 */
	if (backup_target != NULL && includewal == STREAM_WAL)
	{
		pg_log_error("WAL cannot be streamed when a backup target is specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	if (format == 't' && includewal == STREAM_WAL && strcmp(basedir, "-") == 0)
	{
		pg_log_error("cannot stream write-ahead logs in tar mode to stdout");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (replication_slot && includewal != STREAM_WAL)
	{
		pg_log_error("replication slots can only be used with WAL streaming");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * Sanity checks for replication slot options.
	 */
	if (no_slot)
	{
		if (replication_slot)
		{
			pg_log_error("--no-slot cannot be used with slot name");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
		temp_replication_slot = false;
	}

	if (create_slot)
	{
		if (!replication_slot)
		{
			pg_log_error("%s needs a slot to be specified using --slot",
						 "--create-slot");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}

		if (no_slot)
		{
			pg_log_error("%s and %s are incompatible options",
						 "--create-slot", "--no-slot");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
	}

	/*
	 * Sanity checks on WAL directory.
	 */
	if (xlog_dir)
	{
		if (backup_target != NULL)
		{
			pg_log_error("WAL directory location cannot be specified along with a backup target");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
		if (format != 'p')
		{
			pg_log_error("WAL directory location can only be specified in plain mode");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}

		/* clean up xlog directory name, check it's absolute */
		canonicalize_path(xlog_dir);
		if (!is_absolute_path(xlog_dir))
		{
			pg_log_error("WAL directory location must be an absolute path");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
	}

	/*
	 * Sanity checks for progress reporting options.
	 */
	if (showprogress && !estimatesize)
	{
		pg_log_error("%s and %s are incompatible options",
					 "--progress", "--no-estimate-size");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * Sanity checks for backup manifest options.
	 */
	if (!manifest && manifest_checksums != NULL)
	{
		pg_log_error("%s and %s are incompatible options",
					 "--no-manifest", "--manifest-checksums");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (!manifest && manifest_force_encode)
	{
		pg_log_error("%s and %s are incompatible options",
					 "--no-manifest", "--manifest-force-encode");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* connection in replication mode to server */
	conn = GetConnection();
	if (!conn)
	{
		/* Error message already written in GetConnection() */
		exit(1);
	}
	atexit(disconnect_atexit);

#ifndef WIN32

	/*
	 * Trap SIGCHLD to be able to handle the WAL stream process exiting. There
	 * is no SIGCHLD on Windows, there we rely on the background thread
	 * setting the signal variable on unexpected but graceful exit. If the WAL
	 * stream thread crashes on Windows it will bring down the entire process
	 * as it's a thread, so there is nothing to catch should that happen. A
	 * crash on UNIX will be caught by the signal handler.
	 */
	pqsignal(SIGCHLD, sigchld_handler);
#endif

	/*
	 * Set umask so that directories/files are created with the same
	 * permissions as directories/files in the source data directory.
	 *
	 * pg_mode_mask is set to owner-only by default and then updated in
	 * GetConnection() where we get the mode from the server-side with
	 * RetrieveDataDirCreatePerm() and then call SetDataDirectoryCreatePerm().
	 */
	umask(pg_mode_mask);

	/* Backup manifests are supported in 13 and newer versions */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_MANIFESTS)
		manifest = false;

	/*
	 * If an output directory was specified, verify that it exists, or create
	 * it. Note that for a tar backup, an output directory of "-" means we are
	 * writing to stdout, so do nothing in that case.
	 */
	if (basedir != NULL && (format == 'p' || strcmp(basedir, "-") != 0))
		verify_dir_is_empty_or_create(basedir, &made_new_pgdata, &found_existing_pgdata);

	/* determine remote server's xlog segment size */
	if (!RetrieveWalSegSize(conn))
		exit(1);

	/* Create pg_wal symlink, if required */
	if (xlog_dir)
	{
		char	   *linkloc;

		verify_dir_is_empty_or_create(xlog_dir, &made_new_xlogdir, &found_existing_xlogdir);

		/*
		 * Form name of the place where the symlink must go. pg_xlog has been
		 * renamed to pg_wal in post-10 clusters.
		 */
		linkloc = psprintf("%s/%s", basedir,
						   PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ?
						   "pg_xlog" : "pg_wal");

#ifdef HAVE_SYMLINK
		if (symlink(xlog_dir, linkloc) != 0)
			pg_fatal("could not create symbolic link \"%s\": %m", linkloc);
#else
		pg_fatal("symlinks are not supported on this platform");
#endif
		free(linkloc);
	}

	BaseBackup(compression_algorithm, compression_detail, compressloc,
			   &client_compress);

	success = true;
	return 0;
}
