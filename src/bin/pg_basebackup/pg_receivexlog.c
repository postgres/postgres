/*-------------------------------------------------------------------------
 *
 * pg_receivexlog.c - receive streaming transaction log data and write it
 *					  to a local file.
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_receivexlog.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libpq-fe.h"
#include "access/xlog_internal.h"
#include "getopt_long.h"

#include "receivelog.h"
#include "streamutil.h"


/* Time to sleep between reconnection attempts */
#define RECONNECT_SLEEP_TIME 5

/* Global options */
static char *basedir = NULL;
static int	verbose = 0;
static int	noloop = 0;
static int	standby_message_timeout = 10 * 1000;		/* 10 sec = default */
static volatile bool time_to_abort = false;


static void usage(void);
static XLogRecPtr FindStreamingStart(uint32 *tli);
static void StreamLog();
static bool stop_streaming(XLogRecPtr segendpos, uint32 timeline,
			   bool segment_finished);

#define disconnect_and_exit(code)				\
	{											\
	if (conn != NULL) PQfinish(conn);			\
	exit(code);									\
	}


static void
usage(void)
{
	printf(_("%s receives PostgreSQL streaming transaction logs.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -D, --directory=DIR    receive transaction log files into this directory\n"));
	printf(_("  -n, --no-loop          do not loop on connection lost\n"));
	printf(_("  -s, --status-interval=SECS\n"
			 "                         time between status packets sent to server (default: %d)\n"), (standby_message_timeout / 1000));
	printf(_("  -S, --slot=SLOTNAME    replication slot to use\n"));
	printf(_("  -v, --verbose          output verbose messages\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=CONNSTR   connection string\n"));
	printf(_("  -h, --host=HOSTNAME    database server host or socket directory\n"));
	printf(_("  -p, --port=PORT        database server port number\n"));
	printf(_("  -U, --username=NAME    connect as specified database user\n"));
	printf(_("  -w, --no-password      never prompt for password\n"));
	printf(_("  -W, --password         force password prompt (should happen automatically)\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}

static bool
stop_streaming(XLogRecPtr xlogpos, uint32 timeline, bool segment_finished)
{
	static uint32 prevtimeline = 0;
	static XLogRecPtr prevpos = InvalidXLogRecPtr;

	/* we assume that we get called once at the end of each segment */
	if (verbose && segment_finished)
		fprintf(stderr, _("%s: finished segment at %X/%X (timeline %u)\n"),
				progname, (uint32) (xlogpos >> 32), (uint32) xlogpos,
				timeline);

	/*
	 * Note that we report the previous, not current, position here. After a
	 * timeline switch, xlogpos points to the beginning of the segment because
	 * that's where we always begin streaming. Reporting the end of previous
	 * timeline isn't totally accurate, because the next timeline can begin
	 * slightly before the end of the WAL that we received on the previous
	 * timeline, but it's close enough for reporting purposes.
	 */
	if (prevtimeline != 0 && prevtimeline != timeline)
		fprintf(stderr, _("%s: switched to timeline %u at %X/%X\n"),
				progname, timeline,
				(uint32) (prevpos >> 32), (uint32) prevpos);

	prevtimeline = timeline;
	prevpos = xlogpos;

	if (time_to_abort)
	{
		fprintf(stderr, _("%s: received interrupt signal, exiting\n"),
				progname);
		return true;
	}
	return false;
}

/*
 * Determine starting location for streaming, based on any existing xlog
 * segments in the directory. We start at the end of the last one that is
 * complete (size matches XLogSegSize), on the timeline with highest ID.
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

	dir = opendir(basedir);
	if (dir == NULL)
	{
		fprintf(stderr, _("%s: could not open directory \"%s\": %s\n"),
				progname, basedir, strerror(errno));
		disconnect_and_exit(1);
	}

	while (errno = 0, (dirent = readdir(dir)) != NULL)
	{
		uint32		tli;
		XLogSegNo	segno;
		bool		ispartial;

		/*
		 * Check if the filename looks like an xlog file, or a .partial file.
		 * Xlog files are always 24 characters, and .partial files are 32
		 * characters.
		 */
		if (strlen(dirent->d_name) == 24)
		{
			if (strspn(dirent->d_name, "0123456789ABCDEF") != 24)
				continue;
			ispartial = false;
		}
		else if (strlen(dirent->d_name) == 32)
		{
			if (strspn(dirent->d_name, "0123456789ABCDEF") != 24)
				continue;
			if (strcmp(&dirent->d_name[24], ".partial") != 0)
				continue;
			ispartial = true;
		}
		else
			continue;

		/*
		 * Looks like an xlog file. Parse its position.
		 */
		XLogFromFileName(dirent->d_name, &tli, &segno);

		/*
		 * Check that the segment has the right size, if it's supposed to be
		 * completed.
		 */
		if (!ispartial)
		{
			struct stat statbuf;
			char		fullpath[MAXPGPATH];

			snprintf(fullpath, sizeof(fullpath), "%s/%s", basedir, dirent->d_name);
			if (stat(fullpath, &statbuf) != 0)
			{
				fprintf(stderr, _("%s: could not stat file \"%s\": %s\n"),
						progname, fullpath, strerror(errno));
				disconnect_and_exit(1);
			}

			if (statbuf.st_size != XLOG_SEG_SIZE)
			{
				fprintf(stderr,
						_("%s: segment file \"%s\" has incorrect size %d, skipping\n"),
						progname, dirent->d_name, (int) statbuf.st_size);
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
		fprintf(stderr, _("%s: could not read directory \"%s\": %s\n"),
				progname, basedir, strerror(errno));
		disconnect_and_exit(1);
	}

	if (closedir(dir))
	{
		fprintf(stderr, _("%s: could not close directory \"%s\": %s\n"),
				progname, basedir, strerror(errno));
		disconnect_and_exit(1);
	}

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

		XLogSegNoOffsetToRecPtr(high_segno, 0, high_ptr);

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
	PGresult   *res;
	XLogRecPtr	startpos;
	uint32		starttli;
	XLogRecPtr	serverpos;
	uint32		servertli;
	uint32		hi,
				lo;

	/*
	 * Connect in replication mode to the server
	 */
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
		disconnect_and_exit(1);
	}

	/*
	 * Run IDENTIFY_SYSTEM so we can get the timeline and current xlog
	 * position.
	 */
	res = PQexec(conn, "IDENTIFY_SYSTEM");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, _("%s: could not send replication command \"%s\": %s"),
				progname, "IDENTIFY_SYSTEM", PQerrorMessage(conn));
		disconnect_and_exit(1);
	}
	if (PQntuples(res) != 1 || PQnfields(res) < 3)
	{
		fprintf(stderr,
				_("%s: could not identify system: got %d rows and %d fields, expected %d rows and %d or more fields\n"),
				progname, PQntuples(res), PQnfields(res), 1, 3);
		disconnect_and_exit(1);
	}
	servertli = atoi(PQgetvalue(res, 0, 1));
	if (sscanf(PQgetvalue(res, 0, 2), "%X/%X", &hi, &lo) != 2)
	{
		fprintf(stderr,
				_("%s: could not parse transaction log location \"%s\"\n"),
				progname, PQgetvalue(res, 0, 2));
		disconnect_and_exit(1);
	}
	serverpos = ((uint64) hi) << 32 | lo;
	PQclear(res);

	/*
	 * Figure out where to start streaming.
	 */
	startpos = FindStreamingStart(&starttli);
	if (startpos == InvalidXLogRecPtr)
	{
		startpos = serverpos;
		starttli = servertli;
	}

	/*
	 * Always start streaming at the beginning of a segment
	 */
	startpos -= startpos % XLOG_SEG_SIZE;

	/*
	 * Start the replication
	 */
	if (verbose)
		fprintf(stderr,
				_("%s: starting log streaming at %X/%X (timeline %u)\n"),
				progname, (uint32) (startpos >> 32), (uint32) startpos,
				starttli);

	ReceiveXlogStream(conn, startpos, starttli, NULL, basedir,
					  stop_streaming, standby_message_timeout, ".partial",
					  false);

	PQfinish(conn);
}

/*
 * When sigint is called, just tell the system to exit at the next possible
 * moment.
 */
#ifndef WIN32

static void
sigint_handler(int signum)
{
	time_to_abort = true;
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
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-loop", no_argument, NULL, 'n'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"status-interval", required_argument, NULL, 's'},
		{"slot", required_argument, NULL, 'S'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};

	int			c;
	int			option_index;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_receivexlog"));

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
			puts("pg_receivexlog (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "D:d:h:p:U:s:S:nwWv",
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
					fprintf(stderr, _("%s: invalid port number \"%s\"\n"),
							progname, optarg);
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
					fprintf(stderr, _("%s: invalid status interval \"%s\"\n"),
							progname, optarg);
					exit(1);
				}
				break;
			case 'S':
				replication_slot = pg_strdup(optarg);
				break;
			case 'n':
				noloop = 1;
				break;
			case 'v':
				verbose++;
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

#ifndef WIN32
	pqsignal(SIGINT, sigint_handler);
#endif

	while (true)
	{
		StreamLog();
		if (time_to_abort)
		{
			/*
			 * We've been Ctrl-C'ed. That's not an error, so exit without an
			 * errorcode.
			 */
			exit(0);
		}
		else if (noloop)
		{
			fprintf(stderr, _("%s: disconnected\n"), progname);
			exit(1);
		}
		else
		{
			fprintf(stderr,
			/* translator: check source for value for %d */
					_("%s: disconnected; waiting %d seconds to try again\n"),
					progname, RECONNECT_SLEEP_TIME);
			pg_usleep(RECONNECT_SLEEP_TIME * 1000000);
		}
	}
}
