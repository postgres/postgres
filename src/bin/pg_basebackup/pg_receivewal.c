/*-------------------------------------------------------------------------
 *
 * pg_receivewal.c - receive streaming WAL data and write it
 *					  to a local file.
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_receivewal.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "receivelog.h"
#include "streamutil.h"

/* Time to sleep between reconnection attempts */
#define RECONNECT_SLEEP_TIME 5

/* Global options */
static char *basedir = NULL;
static int	verbose = 0;
static int	compresslevel = 0;
static int	noloop = 0;
static int	standby_message_timeout = 10 * 1000;	/* 10 sec = default */
static volatile bool time_to_stop = false;
static bool do_create_slot = false;
static bool slot_exists_ok = false;
static bool do_drop_slot = false;
static bool do_sync = true;
static bool synchronous = false;
static char *replication_slot = NULL;
static XLogRecPtr endpos = InvalidXLogRecPtr;


static void usage(void);
static DIR *get_destination_dir(char *dest_folder);
static void close_destination_dir(DIR *dest_dir, char *dest_folder);
static XLogRecPtr FindStreamingStart(uint32 *tli);
static void StreamLog(void);
static bool stop_streaming(XLogRecPtr segendpos, uint32 timeline,
						   bool segment_finished);

static void
disconnect_atexit(void)
{
	if (conn != NULL)
		PQfinish(conn);
}

/* Routines to evaluate segment file format */
#define IsCompressXLogFileName(fname)	 \
	(strlen(fname) == XLOG_FNAME_LEN + strlen(".gz") && \
	 strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&		\
	 strcmp((fname) + XLOG_FNAME_LEN, ".gz") == 0)
#define IsPartialCompressXLogFileName(fname)	\
	(strlen(fname) == XLOG_FNAME_LEN + strlen(".gz.partial") && \
	 strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&		\
	 strcmp((fname) + XLOG_FNAME_LEN, ".gz.partial") == 0)

static void
usage(void)
{
	printf(_("%s receives PostgreSQL streaming write-ahead logs.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -D, --directory=DIR    receive write-ahead log files into this directory\n"));
	printf(_("  -E, --endpos=LSN       exit after receiving the specified LSN\n"));
	printf(_("      --if-not-exists    do not error if slot already exists when creating a slot\n"));
	printf(_("  -n, --no-loop          do not loop on connection lost\n"));
	printf(_("      --no-sync          do not wait for changes to be written safely to disk\n"));
	printf(_("  -s, --status-interval=SECS\n"
			 "                         time between status packets sent to server (default: %d)\n"), (standby_message_timeout / 1000));
	printf(_("  -S, --slot=SLOTNAME    replication slot to use\n"));
	printf(_("      --synchronous      flush write-ahead log immediately after writing\n"));
	printf(_("  -v, --verbose          output verbose messages\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -Z, --compress=0-9     compress logs with given compression level\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=CONNSTR   connection string\n"));
	printf(_("  -h, --host=HOSTNAME    database server host or socket directory\n"));
	printf(_("  -p, --port=PORT        database server port number\n"));
	printf(_("  -U, --username=NAME    connect as specified database user\n"));
	printf(_("  -w, --no-password      never prompt for password\n"));
	printf(_("  -W, --password         force password prompt (should happen automatically)\n"));
	printf(_("\nOptional actions:\n"));
	printf(_("      --create-slot      create a new replication slot (for the slot's name see --slot)\n"));
	printf(_("      --drop-slot        drop the replication slot (for the slot's name see --slot)\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

static bool
stop_streaming(XLogRecPtr xlogpos, uint32 timeline, bool segment_finished)
{
	static uint32 prevtimeline = 0;
	static XLogRecPtr prevpos = InvalidXLogRecPtr;

	/* we assume that we get called once at the end of each segment */
	if (verbose && segment_finished)
		pg_log_info("finished segment at %X/%X (timeline %u)",
					LSN_FORMAT_ARGS(xlogpos),
					timeline);

	if (!XLogRecPtrIsInvalid(endpos) && endpos < xlogpos)
	{
		if (verbose)
			pg_log_info("stopped log streaming at %X/%X (timeline %u)",
						LSN_FORMAT_ARGS(xlogpos),
						timeline);
		time_to_stop = true;
		return true;
	}

	/*
	 * Note that we report the previous, not current, position here. After a
	 * timeline switch, xlogpos points to the beginning of the segment because
	 * that's where we always begin streaming. Reporting the end of previous
	 * timeline isn't totally accurate, because the next timeline can begin
	 * slightly before the end of the WAL that we received on the previous
	 * timeline, but it's close enough for reporting purposes.
	 */
	if (verbose && prevtimeline != 0 && prevtimeline != timeline)
		pg_log_info("switched to timeline %u at %X/%X",
					timeline,
					LSN_FORMAT_ARGS(prevpos));

	prevtimeline = timeline;
	prevpos = xlogpos;

	if (time_to_stop)
	{
		if (verbose)
			pg_log_info("received interrupt signal, exiting");
		return true;
	}
	return false;
}


/*
 * Get destination directory.
 */
static DIR *
get_destination_dir(char *dest_folder)
{
	DIR		   *dir;

	Assert(dest_folder != NULL);
	dir = opendir(dest_folder);
	if (dir == NULL)
	{
		pg_log_error("could not open directory \"%s\": %m", basedir);
		exit(1);
	}

	return dir;
}


/*
 * Close existing directory.
 */
static void
close_destination_dir(DIR *dest_dir, char *dest_folder)
{
	Assert(dest_dir != NULL && dest_folder != NULL);
	if (closedir(dest_dir))
	{
		pg_log_error("could not close directory \"%s\": %m", dest_folder);
		exit(1);
	}
}


/*
 * Determine starting location for streaming, based on any existing xlog
 * segments in the directory. We start at the end of the last one that is
 * complete (size matches wal segment size), on the timeline with highest ID.
 *
 * If there are no WAL files in the directory, returns InvalidXLogRecPtr.
 */
static XLogRecPtr
FindStreamingStart(uint32 *tli)
{
	DIR		   *dir;
	struct dirent *dirent;
	XLogSegNo	high_segno = 0;
	uint32		high_tli = 0;
	bool		high_ispartial = false;

	dir = get_destination_dir(basedir);

	while (errno = 0, (dirent = readdir(dir)) != NULL)
	{
		uint32		tli;
		XLogSegNo	segno;
		bool		ispartial;
		bool		iscompress;

		/*
		 * Check if the filename looks like an xlog file, or a .partial file.
		 */
		if (IsXLogFileName(dirent->d_name))
		{
			ispartial = false;
			iscompress = false;
		}
		else if (IsPartialXLogFileName(dirent->d_name))
		{
			ispartial = true;
			iscompress = false;
		}
		else if (IsCompressXLogFileName(dirent->d_name))
		{
			ispartial = false;
			iscompress = true;
		}
		else if (IsPartialCompressXLogFileName(dirent->d_name))
		{
			ispartial = true;
			iscompress = true;
		}
		else
			continue;

		/*
		 * Looks like an xlog file. Parse its position.
		 */
		XLogFromFileName(dirent->d_name, &tli, &segno, WalSegSz);

		/*
		 * Check that the segment has the right size, if it's supposed to be
		 * completed.  For non-compressed segments just check the on-disk size
		 * and see if it matches a completed segment. For compressed segments,
		 * look at the last 4 bytes of the compressed file, which is where the
		 * uncompressed size is located for gz files with a size lower than
		 * 4GB, and then compare it to the size of a completed segment. The 4
		 * last bytes correspond to the ISIZE member according to
		 * http://www.zlib.org/rfc-gzip.html.
		 */
		if (!ispartial && !iscompress)
		{
			struct stat statbuf;
			char		fullpath[MAXPGPATH * 2];

			snprintf(fullpath, sizeof(fullpath), "%s/%s", basedir, dirent->d_name);
			if (stat(fullpath, &statbuf) != 0)
			{
				pg_log_error("could not stat file \"%s\": %m", fullpath);
				exit(1);
			}

			if (statbuf.st_size != WalSegSz)
			{
				pg_log_warning("segment file \"%s\" has incorrect size %lld, skipping",
							   dirent->d_name, (long long int) statbuf.st_size);
				continue;
			}
		}
		else if (!ispartial && iscompress)
		{
			int			fd;
			char		buf[4];
			int			bytes_out;
			char		fullpath[MAXPGPATH * 2];
			int			r;

			snprintf(fullpath, sizeof(fullpath), "%s/%s", basedir, dirent->d_name);

			fd = open(fullpath, O_RDONLY | PG_BINARY, 0);
			if (fd < 0)
			{
				pg_log_error("could not open compressed file \"%s\": %m",
							 fullpath);
				exit(1);
			}
			if (lseek(fd, (off_t) (-4), SEEK_END) < 0)
			{
				pg_log_error("could not seek in compressed file \"%s\": %m",
							 fullpath);
				exit(1);
			}
			r = read(fd, (char *) buf, sizeof(buf));
			if (r != sizeof(buf))
			{
				if (r < 0)
					pg_log_error("could not read compressed file \"%s\": %m",
								 fullpath);
				else
					pg_log_error("could not read compressed file \"%s\": read %d of %zu",
								 fullpath, r, sizeof(buf));
				exit(1);
			}

			close(fd);
			bytes_out = (buf[3] << 24) | (buf[2] << 16) |
				(buf[1] << 8) | buf[0];

			if (bytes_out != WalSegSz)
			{
				pg_log_warning("compressed segment file \"%s\" has incorrect uncompressed size %d, skipping",
							   dirent->d_name, bytes_out);
				continue;
			}
		}

		/* Looks like a valid segment. Remember that we saw it. */
		if ((segno > high_segno) ||
			(segno == high_segno && tli > high_tli) ||
			(segno == high_segno && tli == high_tli && high_ispartial && !ispartial))
		{
			high_segno = segno;
			high_tli = tli;
			high_ispartial = ispartial;
		}
	}

	if (errno)
	{
		pg_log_error("could not read directory \"%s\": %m", basedir);
		exit(1);
	}

	close_destination_dir(dir, basedir);

	if (high_segno > 0)
	{
		XLogRecPtr	high_ptr;

		/*
		 * Move the starting pointer to the start of the next segment, if the
		 * highest one we saw was completed. Otherwise start streaming from
		 * the beginning of the .partial segment.
		 */
		if (!high_ispartial)
			high_segno++;

		XLogSegNoOffsetToRecPtr(high_segno, 0, WalSegSz, high_ptr);

		*tli = high_tli;
		return high_ptr;
	}
	else
		return InvalidXLogRecPtr;
}

/*
 * Start the log streaming
 */
static void
StreamLog(void)
{
	XLogRecPtr	serverpos;
	TimeLineID	servertli;
	StreamCtl	stream;

	MemSet(&stream, 0, sizeof(stream));

	/*
	 * Connect in replication mode to the server
	 */
	if (conn == NULL)
		conn = GetConnection();
	if (!conn)
		/* Error message already written in GetConnection() */
		return;

	if (!CheckServerVersionForStreaming(conn))
	{
		/*
		 * Error message already written in CheckServerVersionForStreaming().
		 * There's no hope of recovering from a version mismatch, so don't
		 * retry.
		 */
		exit(1);
	}

	/*
	 * Identify server, obtaining start LSN position and current timeline ID
	 * at the same time, necessary if not valid data can be found in the
	 * existing output directory.
	 */
	if (!RunIdentifySystem(conn, NULL, &servertli, &serverpos, NULL))
		exit(1);

	/*
	 * Figure out where to start streaming.
	 */
	stream.startpos = FindStreamingStart(&stream.timeline);
	if (stream.startpos == InvalidXLogRecPtr)
	{
		stream.startpos = serverpos;
		stream.timeline = servertli;
	}

	/*
	 * Always start streaming at the beginning of a segment
	 */
	stream.startpos -= XLogSegmentOffset(stream.startpos, WalSegSz);

	/*
	 * Start the replication
	 */
	if (verbose)
		pg_log_info("starting log streaming at %X/%X (timeline %u)",
					LSN_FORMAT_ARGS(stream.startpos),
					stream.timeline);

	stream.stream_stop = stop_streaming;
	stream.stop_socket = PGINVALID_SOCKET;
	stream.standby_message_timeout = standby_message_timeout;
	stream.synchronous = synchronous;
	stream.do_sync = do_sync;
	stream.mark_done = false;
	stream.walmethod = CreateWalDirectoryMethod(basedir, compresslevel,
												stream.do_sync);
	stream.partial_suffix = ".partial";
	stream.replication_slot = replication_slot;

	ReceiveXlogStream(conn, &stream);

	if (!stream.walmethod->finish())
	{
		pg_log_info("could not finish writing WAL files: %m");
		return;
	}

	PQfinish(conn);
	conn = NULL;

	FreeWalDirectoryMethod();
	pg_free(stream.walmethod);

	conn = NULL;
}

/*
 * When sigint is called, just tell the system to exit at the next possible
 * moment.
 */
#ifndef WIN32

static void
sigint_handler(int signum)
{
	time_to_stop = true;
}
#endif

int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"directory", required_argument, NULL, 'D'},
		{"dbname", required_argument, NULL, 'd'},
		{"endpos", required_argument, NULL, 'E'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-loop", no_argument, NULL, 'n'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"status-interval", required_argument, NULL, 's'},
		{"slot", required_argument, NULL, 'S'},
		{"verbose", no_argument, NULL, 'v'},
		{"compress", required_argument, NULL, 'Z'},
/* action */
		{"create-slot", no_argument, NULL, 1},
		{"drop-slot", no_argument, NULL, 2},
		{"if-not-exists", no_argument, NULL, 3},
		{"synchronous", no_argument, NULL, 4},
		{"no-sync", no_argument, NULL, 5},
		{NULL, 0, NULL, 0}
	};

	int			c;
	int			option_index;
	char	   *db_name;
	uint32		hi,
				lo;

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
		else if (strcmp(argv[1], "-V") == 0 ||
				 strcmp(argv[1], "--version") == 0)
		{
			puts("pg_receivewal (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "D:d:E:h:p:U:s:S:nwWvZ:",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'D':
				basedir = pg_strdup(optarg);
				break;
			case 'd':
				connection_string = pg_strdup(optarg);
				break;
			case 'h':
				dbhost = pg_strdup(optarg);
				break;
			case 'p':
				if (atoi(optarg) <= 0)
				{
					pg_log_error("invalid port number \"%s\"", optarg);
					exit(1);
				}
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
					pg_log_error("invalid status interval \"%s\"", optarg);
					exit(1);
				}
				break;
			case 'S':
				replication_slot = pg_strdup(optarg);
				break;
			case 'E':
				if (sscanf(optarg, "%X/%X", &hi, &lo) != 2)
				{
					pg_log_error("could not parse end position \"%s\"", optarg);
					exit(1);
				}
				endpos = ((uint64) hi) << 32 | lo;
				break;
			case 'n':
				noloop = 1;
				break;
			case 'v':
				verbose++;
				break;
			case 'Z':
				compresslevel = atoi(optarg);
				if (compresslevel < 0 || compresslevel > 9)
				{
					pg_log_error("invalid compression level \"%s\"", optarg);
					exit(1);
				}
				break;
/* action */
			case 1:
				do_create_slot = true;
				break;
			case 2:
				do_drop_slot = true;
				break;
			case 3:
				slot_exists_ok = true;
				break;
			case 4:
				synchronous = true;
				break;
			case 5:
				do_sync = false;
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
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (do_drop_slot && do_create_slot)
	{
		pg_log_error("cannot use --create-slot together with --drop-slot");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (replication_slot == NULL && (do_drop_slot || do_create_slot))
	{
		/* translator: second %s is an option name */
		pg_log_error("%s needs a slot to be specified using --slot",
					 do_drop_slot ? "--drop-slot" : "--create-slot");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (synchronous && !do_sync)
	{
		pg_log_error("cannot use --synchronous together with --no-sync");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Required arguments
	 */
	if (basedir == NULL && !do_drop_slot && !do_create_slot)
	{
		pg_log_error("no target directory specified");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

#ifndef HAVE_LIBZ
	if (compresslevel != 0)
	{
		pg_log_error("this build does not support compression");
		exit(1);
	}
#endif

	/*
	 * Check existence of destination folder.
	 */
	if (!do_drop_slot && !do_create_slot)
	{
		DIR		   *dir = get_destination_dir(basedir);

		close_destination_dir(dir, basedir);
	}

#ifndef WIN32
	pqsignal(SIGINT, sigint_handler);
#endif

	/*
	 * Obtain a connection before doing anything.
	 */
	conn = GetConnection();
	if (!conn)
		/* error message already written in GetConnection() */
		exit(1);
	atexit(disconnect_atexit);

	/*
	 * Run IDENTIFY_SYSTEM to make sure we've successfully have established a
	 * replication connection and haven't connected using a database specific
	 * connection.
	 */
	if (!RunIdentifySystem(conn, NULL, NULL, NULL, &db_name))
		exit(1);

	/*
	 * Set umask so that directories/files are created with the same
	 * permissions as directories/files in the source data directory.
	 *
	 * pg_mode_mask is set to owner-only by default and then updated in
	 * GetConnection() where we get the mode from the server-side with
	 * RetrieveDataDirCreatePerm() and then call SetDataDirectoryCreatePerm().
	 */
	umask(pg_mode_mask);

	/* determine remote server's xlog segment size */
	if (!RetrieveWalSegSize(conn))
		exit(1);

	/*
	 * Check that there is a database associated with connection, none should
	 * be defined in this context.
	 */
	if (db_name)
	{
		pg_log_error("replication connection using slot \"%s\" is unexpectedly database specific",
					 replication_slot);
		exit(1);
	}

	/*
	 * Drop a replication slot.
	 */
	if (do_drop_slot)
	{
		if (verbose)
			pg_log_info("dropping replication slot \"%s\"", replication_slot);

		if (!DropReplicationSlot(conn, replication_slot))
			exit(1);
		exit(0);
	}

	/* Create a replication slot */
	if (do_create_slot)
	{
		if (verbose)
			pg_log_info("creating replication slot \"%s\"", replication_slot);

		if (!CreateReplicationSlot(conn, replication_slot, NULL, false, true, false,
								   slot_exists_ok))
			exit(1);
		exit(0);
	}

	/*
	 * Don't close the connection here so that subsequent StreamLog() can
	 * reuse it.
	 */

	while (true)
	{
		StreamLog();
		if (time_to_stop)
		{
			/*
			 * We've been Ctrl-C'ed or end of streaming position has been
			 * willingly reached, so exit without an error code.
			 */
			exit(0);
		}
		else if (noloop)
		{
			pg_log_error("disconnected");
			exit(1);
		}
		else
		{
			/* translator: check source for value for %d */
			pg_log_info("disconnected; waiting %d seconds to try again",
						RECONNECT_SLEEP_TIME);
			pg_usleep(RECONNECT_SLEEP_TIME * 1000000);
		}
	}
}
