/*-------------------------------------------------------------------------
 *
 * pg_receivewal.c - receive streaming WAL data and write it
 *					  to a local file.
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_receivewal.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef USE_LZ4
#include <lz4frame.h>
#endif
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "access/xlog_internal.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "fe_utils/option_utils.h"
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
static bool noloop = false;
static int	standby_message_timeout = 10 * 1000;	/* 10 sec = default */
static volatile sig_atomic_t time_to_stop = false;
static bool do_create_slot = false;
static bool slot_exists_ok = false;
static bool do_drop_slot = false;
static bool do_sync = true;
static bool synchronous = false;
static char *replication_slot = NULL;
static pg_compress_algorithm compression_algorithm = PG_COMPRESSION_NONE;
static XLogRecPtr endpos = InvalidXLogRecPtr;


static void usage(void);
static DIR *get_destination_dir(char *dest_folder);
static void close_destination_dir(DIR *dest_dir, char *dest_folder);
static XLogRecPtr FindStreamingStart(uint32 *tli);
static void StreamLog(void);
static bool stop_streaming(XLogRecPtr xlogpos, uint32 timeline,
						   bool segment_finished);

static void
disconnect_atexit(void)
{
	if (conn != NULL)
		PQfinish(conn);
}

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
	printf(_("  -Z, --compress=METHOD[:DETAIL]\n"
			 "                         compress as specified\n"));
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


/*
 * Check if the filename looks like a WAL file, letting caller know if this
 * WAL segment is partial and/or compressed.
 */
static bool
is_xlogfilename(const char *filename, bool *ispartial,
				pg_compress_algorithm *wal_compression_algorithm)
{
	size_t		fname_len = strlen(filename);
	size_t		xlog_pattern_len = strspn(filename, "0123456789ABCDEF");

	/* File does not look like a WAL file */
	if (xlog_pattern_len != XLOG_FNAME_LEN)
		return false;

	/* File looks like a completed uncompressed WAL file */
	if (fname_len == XLOG_FNAME_LEN)
	{
		*ispartial = false;
		*wal_compression_algorithm = PG_COMPRESSION_NONE;
		return true;
	}

	/* File looks like a completed gzip-compressed WAL file */
	if (fname_len == XLOG_FNAME_LEN + strlen(".gz") &&
		strcmp(filename + XLOG_FNAME_LEN, ".gz") == 0)
	{
		*ispartial = false;
		*wal_compression_algorithm = PG_COMPRESSION_GZIP;
		return true;
	}

	/* File looks like a completed LZ4-compressed WAL file */
	if (fname_len == XLOG_FNAME_LEN + strlen(".lz4") &&
		strcmp(filename + XLOG_FNAME_LEN, ".lz4") == 0)
	{
		*ispartial = false;
		*wal_compression_algorithm = PG_COMPRESSION_LZ4;
		return true;
	}

	/* File looks like a partial uncompressed WAL file */
	if (fname_len == XLOG_FNAME_LEN + strlen(".partial") &&
		strcmp(filename + XLOG_FNAME_LEN, ".partial") == 0)
	{
		*ispartial = true;
		*wal_compression_algorithm = PG_COMPRESSION_NONE;
		return true;
	}

	/* File looks like a partial gzip-compressed WAL file */
	if (fname_len == XLOG_FNAME_LEN + strlen(".gz.partial") &&
		strcmp(filename + XLOG_FNAME_LEN, ".gz.partial") == 0)
	{
		*ispartial = true;
		*wal_compression_algorithm = PG_COMPRESSION_GZIP;
		return true;
	}

	/* File looks like a partial LZ4-compressed WAL file */
	if (fname_len == XLOG_FNAME_LEN + strlen(".lz4.partial") &&
		strcmp(filename + XLOG_FNAME_LEN, ".lz4.partial") == 0)
	{
		*ispartial = true;
		*wal_compression_algorithm = PG_COMPRESSION_LZ4;
		return true;
	}

	/* File does not look like something we know */
	return false;
}

static bool
stop_streaming(XLogRecPtr xlogpos, uint32 timeline, bool segment_finished)
{
	static uint32 prevtimeline = 0;
	static XLogRecPtr prevpos = InvalidXLogRecPtr;

	/* we assume that we get called once at the end of each segment */
	if (verbose && segment_finished)
		pg_log_info("finished segment at %X/%08X (timeline %u)",
					LSN_FORMAT_ARGS(xlogpos),
					timeline);

	if (!XLogRecPtrIsInvalid(endpos) && endpos < xlogpos)
	{
		if (verbose)
			pg_log_info("stopped log streaming at %X/%08X (timeline %u)",
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
		pg_log_info("switched to timeline %u at %X/%08X",
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
		pg_fatal("could not open directory \"%s\": %m", dest_folder);

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
		pg_fatal("could not close directory \"%s\": %m", dest_folder);
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
		pg_compress_algorithm wal_compression_algorithm;
		bool		ispartial;

		if (!is_xlogfilename(dirent->d_name,
							 &ispartial, &wal_compression_algorithm))
			continue;

		/*
		 * Looks like an xlog file. Parse its position.
		 */
		XLogFromFileName(dirent->d_name, &tli, &segno, WalSegSz);

		/*
		 * Check that the segment has the right size, if it's supposed to be
		 * completed.  For non-compressed segments just check the on-disk size
		 * and see if it matches a completed segment.  For gzip-compressed
		 * segments, look at the last 4 bytes of the compressed file, which is
		 * where the uncompressed size is located for files with a size lower
		 * than 4GB, and then compare it to the size of a completed segment.
		 * The 4 last bytes correspond to the ISIZE member according to
		 * http://www.zlib.org/rfc-gzip.html.
		 *
		 * For LZ4-compressed segments, uncompress the file in a throw-away
		 * buffer keeping track of the uncompressed size, then compare it to
		 * the size of a completed segment.  Per its protocol, LZ4 does not
		 * store the uncompressed size of an object by default.  contentSize
		 * is one possible way to do that, but we need to rely on a method
		 * where WAL segments could have been compressed by a different source
		 * than pg_receivewal, like an archive_command with lz4.
		 */
		if (!ispartial && wal_compression_algorithm == PG_COMPRESSION_NONE)
		{
			struct stat statbuf;
			char		fullpath[MAXPGPATH * 2];

			snprintf(fullpath, sizeof(fullpath), "%s/%s", basedir, dirent->d_name);
			if (stat(fullpath, &statbuf) != 0)
				pg_fatal("could not stat file \"%s\": %m", fullpath);

			if (statbuf.st_size != WalSegSz)
			{
				pg_log_warning("segment file \"%s\" has incorrect size %lld, skipping",
							   dirent->d_name, (long long int) statbuf.st_size);
				continue;
			}
		}
		else if (!ispartial && wal_compression_algorithm == PG_COMPRESSION_GZIP)
		{
			int			fd;
			char		buf[4];
			int			bytes_out;
			char		fullpath[MAXPGPATH * 2];
			int			r;

			snprintf(fullpath, sizeof(fullpath), "%s/%s", basedir, dirent->d_name);

			fd = open(fullpath, O_RDONLY | PG_BINARY, 0);
			if (fd < 0)
				pg_fatal("could not open compressed file \"%s\": %m",
						 fullpath);
			if (lseek(fd, (off_t) (-4), SEEK_END) < 0)
				pg_fatal("could not seek in compressed file \"%s\": %m",
						 fullpath);
			r = read(fd, buf, sizeof(buf));
			if (r != sizeof(buf))
			{
				if (r < 0)
					pg_fatal("could not read compressed file \"%s\": %m",
							 fullpath);
				else
					pg_fatal("could not read compressed file \"%s\": read %d of %zu",
							 fullpath, r, sizeof(buf));
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
		else if (!ispartial && wal_compression_algorithm == PG_COMPRESSION_LZ4)
		{
#ifdef USE_LZ4
#define LZ4_CHUNK_SZ	64 * 1024	/* 64kB as maximum chunk size read */
			int			fd;
			ssize_t		r;
			size_t		uncompressed_size = 0;
			char		fullpath[MAXPGPATH * 2];
			char	   *outbuf;
			char	   *readbuf;
			LZ4F_decompressionContext_t ctx = NULL;
			LZ4F_decompressOptions_t dec_opt;
			LZ4F_errorCode_t status;

			memset(&dec_opt, 0, sizeof(dec_opt));
			snprintf(fullpath, sizeof(fullpath), "%s/%s", basedir, dirent->d_name);

			fd = open(fullpath, O_RDONLY | PG_BINARY, 0);
			if (fd < 0)
				pg_fatal("could not open file \"%s\": %m", fullpath);

			status = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
			if (LZ4F_isError(status))
				pg_fatal("could not create LZ4 decompression context: %s",
						 LZ4F_getErrorName(status));

			outbuf = pg_malloc0(LZ4_CHUNK_SZ);
			readbuf = pg_malloc0(LZ4_CHUNK_SZ);
			do
			{
				char	   *readp;
				char	   *readend;

				r = read(fd, readbuf, LZ4_CHUNK_SZ);
				if (r < 0)
					pg_fatal("could not read file \"%s\": %m", fullpath);

				/* Done reading the file */
				if (r == 0)
					break;

				/* Process one chunk */
				readp = readbuf;
				readend = readbuf + r;
				while (readp < readend)
				{
					size_t		out_size = LZ4_CHUNK_SZ;
					size_t		read_size = readend - readp;

					memset(outbuf, 0, LZ4_CHUNK_SZ);
					status = LZ4F_decompress(ctx, outbuf, &out_size,
											 readp, &read_size, &dec_opt);
					if (LZ4F_isError(status))
						pg_fatal("could not decompress file \"%s\": %s",
								 fullpath,
								 LZ4F_getErrorName(status));

					readp += read_size;
					uncompressed_size += out_size;
				}

				/*
				 * No need to continue reading the file when the
				 * uncompressed_size exceeds WalSegSz, even if there are still
				 * data left to read. However, if uncompressed_size is equal
				 * to WalSegSz, it should verify that there is no more data to
				 * read.
				 */
			} while (uncompressed_size <= WalSegSz && r > 0);

			close(fd);
			pg_free(outbuf);
			pg_free(readbuf);

			status = LZ4F_freeDecompressionContext(ctx);
			if (LZ4F_isError(status))
				pg_fatal("could not free LZ4 decompression context: %s",
						 LZ4F_getErrorName(status));

			if (uncompressed_size != WalSegSz)
			{
				pg_log_warning("compressed segment file \"%s\" has incorrect uncompressed size %zu, skipping",
							   dirent->d_name, uncompressed_size);
				continue;
			}
#else
			pg_log_error("cannot check file \"%s\": compression with %s not supported by this build",
						 dirent->d_name, "LZ4");
			exit(1);
#endif
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
		pg_fatal("could not read directory \"%s\": %m", basedir);

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
	StreamCtl	stream = {0};
	char	   *sysidentifier;

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
	if (!RunIdentifySystem(conn, &sysidentifier, &servertli, &serverpos, NULL))
		exit(1);

	/*
	 * Figure out where to start streaming.  First scan the local directory.
	 */
	stream.startpos = FindStreamingStart(&stream.timeline);
	if (stream.startpos == InvalidXLogRecPtr)
	{
		/*
		 * Try to get the starting point from the slot if any.  This is
		 * supported in PostgreSQL 15 and newer.
		 */
		if (replication_slot != NULL &&
			PQserverVersion(conn) >= 150000)
		{
			if (!GetSlotInformation(conn, replication_slot, &stream.startpos,
									&stream.timeline))
			{
				/* Error is logged by GetSlotInformation() */
				return;
			}
		}

		/*
		 * If it the starting point is still not known, use the current WAL
		 * flush value as last resort.
		 */
		if (stream.startpos == InvalidXLogRecPtr)
		{
			stream.startpos = serverpos;
			stream.timeline = servertli;
		}
	}

	Assert(stream.startpos != InvalidXLogRecPtr &&
		   stream.timeline != 0);

	/*
	 * Always start streaming at the beginning of a segment
	 */
	stream.startpos -= XLogSegmentOffset(stream.startpos, WalSegSz);

	/*
	 * Start the replication
	 */
	if (verbose)
		pg_log_info("starting log streaming at %X/%08X (timeline %u)",
					LSN_FORMAT_ARGS(stream.startpos),
					stream.timeline);

	stream.stream_stop = stop_streaming;
	stream.stop_socket = PGINVALID_SOCKET;
	stream.standby_message_timeout = standby_message_timeout;
	stream.synchronous = synchronous;
	stream.do_sync = do_sync;
	stream.mark_done = false;
	stream.walmethod = CreateWalDirectoryMethod(basedir,
												compression_algorithm,
												compresslevel,
												stream.do_sync);
	stream.partial_suffix = ".partial";
	stream.replication_slot = replication_slot;
	stream.sysidentifier = sysidentifier;

	ReceiveXlogStream(conn, &stream);

	if (!stream.walmethod->ops->finish(stream.walmethod))
	{
		pg_log_info("could not finish writing WAL files: %m");
		return;
	}

	PQfinish(conn);
	conn = NULL;

	stream.walmethod->ops->free(stream.walmethod);
}

/*
 * When SIGINT/SIGTERM are caught, just tell the system to exit at the next
 * possible moment.
 */
#ifndef WIN32

static void
sigexit_handler(SIGNAL_ARGS)
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
	pg_compress_specification compression_spec;
	char	   *compression_detail = NULL;
	char	   *compression_algorithm_str = "none";
	char	   *error_detail = NULL;

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

	while ((c = getopt_long(argc, argv, "d:D:E:h:np:s:S:U:vwWZ:",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'd':
				connection_string = pg_strdup(optarg);
				break;
			case 'D':
				basedir = pg_strdup(optarg);
				break;
			case 'E':
				if (sscanf(optarg, "%X/%08X", &hi, &lo) != 2)
					pg_fatal("could not parse end position \"%s\"", optarg);
				endpos = ((uint64) hi) << 32 | lo;
				break;
			case 'h':
				dbhost = pg_strdup(optarg);
				break;
			case 'n':
				noloop = true;
				break;
			case 'p':
				dbport = pg_strdup(optarg);
				break;
			case 's':
				if (!option_parse_int(optarg, "-s/--status-interval", 0,
									  INT_MAX / 1000,
									  &standby_message_timeout))
					exit(1);
				standby_message_timeout *= 1000;
				break;
			case 'S':
				replication_slot = pg_strdup(optarg);
				break;
			case 'U':
				dbuser = pg_strdup(optarg);
				break;
			case 'v':
				verbose++;
				break;
			case 'w':
				dbgetpassword = -1;
				break;
			case 'W':
				dbgetpassword = 1;
				break;
			case 'Z':
				parse_compress_options(optarg, &compression_algorithm_str,
									   &compression_detail);
				break;
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

	if (do_drop_slot && do_create_slot)
	{
		pg_log_error("cannot use --create-slot together with --drop-slot");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (replication_slot == NULL && (do_drop_slot || do_create_slot))
	{
		/* translator: second %s is an option name */
		pg_log_error("%s needs a slot to be specified using --slot",
					 do_drop_slot ? "--drop-slot" : "--create-slot");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (synchronous && !do_sync)
	{
		pg_log_error("cannot use --synchronous together with --no-sync");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * Required arguments
	 */
	if (basedir == NULL && !do_drop_slot && !do_create_slot)
	{
		pg_log_error("no target directory specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * Compression options
	 */
	if (!parse_compress_algorithm(compression_algorithm_str,
								  &compression_algorithm))
		pg_fatal("unrecognized compression algorithm: \"%s\"",
				 compression_algorithm_str);

	parse_compress_specification(compression_algorithm, compression_detail,
								 &compression_spec);
	error_detail = validate_compress_specification(&compression_spec);
	if (error_detail != NULL)
		pg_fatal("invalid compression specification: %s",
				 error_detail);

	/* Extract the compression level */
	compresslevel = compression_spec.level;

	if (compression_algorithm == PG_COMPRESSION_ZSTD)
		pg_fatal("compression with %s is not yet supported", "ZSTD");

	/*
	 * Check existence of destination folder.
	 */
	if (!do_drop_slot && !do_create_slot)
	{
		DIR		   *dir = get_destination_dir(basedir);

		close_destination_dir(dir, basedir);
	}

	/*
	 * Obtain a connection before doing anything.
	 */
	conn = GetConnection();
	if (!conn)
		/* error message already written in GetConnection() */
		exit(1);
	atexit(disconnect_atexit);

	/*
	 * Trap signals.  (Don't do this until after the initial password prompt,
	 * if one is needed, in GetConnection.)
	 */
#ifndef WIN32
	pqsignal(SIGINT, sigexit_handler);
	pqsignal(SIGTERM, sigexit_handler);
#endif

	/*
	 * Run IDENTIFY_SYSTEM to make sure we've successfully have established a
	 * replication connection and haven't connected using a database specific
	 * connection.
	 */
	if (!RunIdentifySystem(conn, NULL, NULL, NULL, &db_name))
		exit(1);

	/*
	 * Check that there is a database associated with connection, none should
	 * be defined in this context.
	 */
	if (db_name)
		pg_fatal("replication connection using slot \"%s\" is unexpectedly database specific",
				 replication_slot);

	/*
	 * Set umask so that directories/files are created with the same
	 * permissions as directories/files in the source data directory.
	 *
	 * pg_mode_mask is set to owner-only by default and then updated in
	 * GetConnection() where we get the mode from the server-side with
	 * RetrieveDataDirCreatePerm() and then call SetDataDirectoryCreatePerm().
	 */
	umask(pg_mode_mask);

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
								   slot_exists_ok, false, false))
			exit(1);
		exit(0);
	}

	/* determine remote server's xlog segment size */
	if (!RetrieveWalSegSize(conn))
		exit(1);

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
			pg_fatal("disconnected");
		else
		{
			/* translator: check source for value for %d */
			pg_log_info("disconnected; waiting %d seconds to try again",
						RECONNECT_SLEEP_TIME);
			pg_usleep(RECONNECT_SLEEP_TIME * 1000000);
		}
	}
}
