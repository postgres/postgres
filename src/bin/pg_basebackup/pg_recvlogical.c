/*-------------------------------------------------------------------------
 *
 * pg_recvlogical.c - receive data from a logical decoding slot in a streaming
 *					  fashion and write it to a local file.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_recvlogical.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <limits.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "common/logging.h"
#include "fe_utils/option_utils.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "libpq/pqsignal.h"
#include "pqexpbuffer.h"
#include "streamutil.h"

/* Time to sleep between reconnection attempts */
#define RECONNECT_SLEEP_TIME 5

typedef enum
{
	STREAM_STOP_NONE,
	STREAM_STOP_END_OF_WAL,
	STREAM_STOP_KEEPALIVE,
	STREAM_STOP_SIGNAL
} StreamStopReason;

/* Global Options */
static char *outfile = NULL;
static int	verbose = 0;
static bool two_phase = false;
static int	noloop = 0;
static int	standby_message_timeout = 10 * 1000;	/* 10 sec = default */
static int	fsync_interval = 10 * 1000; /* 10 sec = default */
static XLogRecPtr startpos = InvalidXLogRecPtr;
static XLogRecPtr endpos = InvalidXLogRecPtr;
static bool do_create_slot = false;
static bool slot_exists_ok = false;
static bool do_start_slot = false;
static bool do_drop_slot = false;
static char *replication_slot = NULL;

/* filled pairwise with option, value. value may be NULL */
static char **options;
static size_t noptions = 0;
static const char *plugin = "test_decoding";

/* Global State */
static int	outfd = -1;
static volatile sig_atomic_t time_to_abort = false;
static volatile sig_atomic_t stop_reason = STREAM_STOP_NONE;
static volatile sig_atomic_t output_reopen = false;
static bool output_isfile;
static TimestampTz output_last_fsync = -1;
static bool output_needs_fsync = false;
static XLogRecPtr output_written_lsn = InvalidXLogRecPtr;
static XLogRecPtr output_fsync_lsn = InvalidXLogRecPtr;

static void usage(void);
static void StreamLogicalLog(void);
static bool flushAndSendFeedback(PGconn *conn, TimestampTz *now);
static void prepareToTerminate(PGconn *conn, XLogRecPtr endpos,
							   StreamStopReason reason,
							   XLogRecPtr lsn);

static void
usage(void)
{
	printf(_("%s controls PostgreSQL logical decoding streams.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nAction to be performed:\n"));
	printf(_("      --create-slot      create a new replication slot (for the slot's name see --slot)\n"));
	printf(_("      --drop-slot        drop the replication slot (for the slot's name see --slot)\n"));
	printf(_("      --start            start streaming in a replication slot (for the slot's name see --slot)\n"));
	printf(_("\nOptions:\n"));
	printf(_("  -E, --endpos=LSN       exit after receiving the specified LSN\n"));
	printf(_("  -f, --file=FILE        receive log into this file, - for stdout\n"));
	printf(_("  -F  --fsync-interval=SECS\n"
			 "                         time between fsyncs to the output file (default: %d)\n"), (fsync_interval / 1000));
	printf(_("      --if-not-exists    do not error if slot already exists when creating a slot\n"));
	printf(_("  -I, --startpos=LSN     where in an existing slot should the streaming start\n"));
	printf(_("  -n, --no-loop          do not loop on connection lost\n"));
	printf(_("  -o, --option=NAME[=VALUE]\n"
			 "                         pass option NAME with optional value VALUE to the\n"
			 "                         output plugin\n"));
	printf(_("  -P, --plugin=PLUGIN    use output plugin PLUGIN (default: %s)\n"), plugin);
	printf(_("  -s, --status-interval=SECS\n"
			 "                         time between status packets sent to server (default: %d)\n"), (standby_message_timeout / 1000));
	printf(_("  -S, --slot=SLOTNAME    name of the logical replication slot\n"));
	printf(_("  -t, --two-phase        enable decoding of prepared transactions when creating a slot\n"));
	printf(_("  -v, --verbose          output verbose messages\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=DBNAME    database to connect to\n"));
	printf(_("  -h, --host=HOSTNAME    database server host or socket directory\n"));
	printf(_("  -p, --port=PORT        database server port number\n"));
	printf(_("  -U, --username=NAME    connect as specified database user\n"));
	printf(_("  -w, --no-password      never prompt for password\n"));
	printf(_("  -W, --password         force password prompt (should happen automatically)\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Send a Standby Status Update message to server.
 */
static bool
sendFeedback(PGconn *conn, TimestampTz now, bool force, bool replyRequested)
{
	static XLogRecPtr last_written_lsn = InvalidXLogRecPtr;
	static XLogRecPtr last_fsync_lsn = InvalidXLogRecPtr;

	char		replybuf[1 + 8 + 8 + 8 + 8 + 1];
	int			len = 0;

	/*
	 * we normally don't want to send superfluous feedback, but if it's
	 * because of a timeout we need to, otherwise wal_sender_timeout will kill
	 * us.
	 */
	if (!force &&
		last_written_lsn == output_written_lsn &&
		last_fsync_lsn == output_fsync_lsn)
		return true;

	if (verbose)
		pg_log_info("confirming write up to %X/%X, flush to %X/%X (slot %s)",
					LSN_FORMAT_ARGS(output_written_lsn),
					LSN_FORMAT_ARGS(output_fsync_lsn),
					replication_slot);

	replybuf[len] = 'r';
	len += 1;
	fe_sendint64(output_written_lsn, &replybuf[len]);	/* write */
	len += 8;
	fe_sendint64(output_fsync_lsn, &replybuf[len]); /* flush */
	len += 8;
	fe_sendint64(InvalidXLogRecPtr, &replybuf[len]);	/* apply */
	len += 8;
	fe_sendint64(now, &replybuf[len]);	/* sendTime */
	len += 8;
	replybuf[len] = replyRequested ? 1 : 0; /* replyRequested */
	len += 1;

	startpos = output_written_lsn;
	last_written_lsn = output_written_lsn;
	last_fsync_lsn = output_fsync_lsn;

	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		pg_log_error("could not send feedback packet: %s",
					 PQerrorMessage(conn));
		return false;
	}

	return true;
}

static void
disconnect_atexit(void)
{
	if (conn != NULL)
		PQfinish(conn);
}

static bool
OutputFsync(TimestampTz now)
{
	output_last_fsync = now;

	output_fsync_lsn = output_written_lsn;

	if (fsync_interval <= 0)
		return true;

	if (!output_needs_fsync)
		return true;

	output_needs_fsync = false;

	/* can only fsync if it's a regular file */
	if (!output_isfile)
		return true;

	if (fsync(outfd) != 0)
		pg_fatal("could not fsync file \"%s\": %m", outfile);

	return true;
}

/*
 * Start the log streaming
 */
static void
StreamLogicalLog(void)
{
	PGresult   *res;
	char	   *copybuf = NULL;
	TimestampTz last_status = -1;
	int			i;
	PQExpBuffer query;
	XLogRecPtr	cur_record_lsn;

	output_written_lsn = InvalidXLogRecPtr;
	output_fsync_lsn = InvalidXLogRecPtr;
	cur_record_lsn = InvalidXLogRecPtr;

	/*
	 * Connect in replication mode to the server
	 */
	if (!conn)
		conn = GetConnection();
	if (!conn)
		/* Error message already written in GetConnection() */
		return;

	/*
	 * Start the replication
	 */
	if (verbose)
		pg_log_info("starting log streaming at %X/%X (slot %s)",
					LSN_FORMAT_ARGS(startpos),
					replication_slot);

	/* Initiate the replication stream at specified location */
	query = createPQExpBuffer();
	appendPQExpBuffer(query, "START_REPLICATION SLOT \"%s\" LOGICAL %X/%X",
					  replication_slot, LSN_FORMAT_ARGS(startpos));

	/* print options if there are any */
	if (noptions)
		appendPQExpBufferStr(query, " (");

	for (i = 0; i < noptions; i++)
	{
		/* separator */
		if (i > 0)
			appendPQExpBufferStr(query, ", ");

		/* write option name */
		appendPQExpBuffer(query, "\"%s\"", options[(i * 2)]);

		/* write option value if specified */
		if (options[(i * 2) + 1] != NULL)
			appendPQExpBuffer(query, " '%s'", options[(i * 2) + 1]);
	}

	if (noptions)
		appendPQExpBufferChar(query, ')');

	res = PQexec(conn, query->data);
	if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 query->data, PQresultErrorMessage(res));
		PQclear(res);
		goto error;
	}
	PQclear(res);
	resetPQExpBuffer(query);

	if (verbose)
		pg_log_info("streaming initiated");

	while (!time_to_abort)
	{
		int			r;
		int			bytes_left;
		int			bytes_written;
		TimestampTz now;
		int			hdr_len;

		cur_record_lsn = InvalidXLogRecPtr;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		/*
		 * Potentially send a status message to the primary.
		 */
		now = feGetCurrentTimestamp();

		if (outfd != -1 &&
			feTimestampDifferenceExceeds(output_last_fsync, now,
										 fsync_interval))
		{
			if (!OutputFsync(now))
				goto error;
		}

		if (standby_message_timeout > 0 &&
			feTimestampDifferenceExceeds(last_status, now,
										 standby_message_timeout))
		{
			/* Time to send feedback! */
			if (!sendFeedback(conn, now, true, false))
				goto error;

			last_status = now;
		}

		/* got SIGHUP, close output file */
		if (outfd != -1 && output_reopen && strcmp(outfile, "-") != 0)
		{
			now = feGetCurrentTimestamp();
			if (!OutputFsync(now))
				goto error;
			close(outfd);
			outfd = -1;
		}
		output_reopen = false;

		/* open the output file, if not open yet */
		if (outfd == -1)
		{
			struct stat statbuf;

			if (strcmp(outfile, "-") == 0)
				outfd = fileno(stdout);
			else
				outfd = open(outfile, O_CREAT | O_APPEND | O_WRONLY | PG_BINARY,
							 S_IRUSR | S_IWUSR);
			if (outfd == -1)
			{
				pg_log_error("could not open log file \"%s\": %m", outfile);
				goto error;
			}

			if (fstat(outfd, &statbuf) != 0)
			{
				pg_log_error("could not stat file \"%s\": %m", outfile);
				goto error;
			}

			output_isfile = S_ISREG(statbuf.st_mode) && !isatty(outfd);
		}

		r = PQgetCopyData(conn, &copybuf, 1);
		if (r == 0)
		{
			/*
			 * In async mode, and no data available. We block on reading but
			 * not more than the specified timeout, so that we can send a
			 * response back to the client.
			 */
			fd_set		input_mask;
			TimestampTz message_target = 0;
			TimestampTz fsync_target = 0;
			struct timeval timeout;
			struct timeval *timeoutptr = NULL;

			if (PQsocket(conn) < 0)
			{
				pg_log_error("invalid socket: %s", PQerrorMessage(conn));
				goto error;
			}

			FD_ZERO(&input_mask);
			FD_SET(PQsocket(conn), &input_mask);

			/* Compute when we need to wakeup to send a keepalive message. */
			if (standby_message_timeout)
				message_target = last_status + (standby_message_timeout - 1) *
					((int64) 1000);

			/* Compute when we need to wakeup to fsync the output file. */
			if (fsync_interval > 0 && output_needs_fsync)
				fsync_target = output_last_fsync + (fsync_interval - 1) *
					((int64) 1000);

			/* Now compute when to wakeup. */
			if (message_target > 0 || fsync_target > 0)
			{
				TimestampTz targettime;
				long		secs;
				int			usecs;

				targettime = message_target;

				if (fsync_target > 0 && fsync_target < targettime)
					targettime = fsync_target;

				feTimestampDifference(now,
									  targettime,
									  &secs,
									  &usecs);
				if (secs <= 0)
					timeout.tv_sec = 1; /* Always sleep at least 1 sec */
				else
					timeout.tv_sec = secs;
				timeout.tv_usec = usecs;
				timeoutptr = &timeout;
			}

			r = select(PQsocket(conn) + 1, &input_mask, NULL, NULL, timeoutptr);
			if (r == 0 || (r < 0 && errno == EINTR))
			{
				/*
				 * Got a timeout or signal. Continue the loop and either
				 * deliver a status packet to the server or just go back into
				 * blocking.
				 */
				continue;
			}
			else if (r < 0)
			{
				pg_log_error("%s() failed: %m", "select");
				goto error;
			}

			/* Else there is actually data on the socket */
			if (PQconsumeInput(conn) == 0)
			{
				pg_log_error("could not receive data from WAL stream: %s",
							 PQerrorMessage(conn));
				goto error;
			}
			continue;
		}

		/* End of copy stream */
		if (r == -1)
			break;

		/* Failure while reading the copy stream */
		if (r == -2)
		{
			pg_log_error("could not read COPY data: %s",
						 PQerrorMessage(conn));
			goto error;
		}

		/* Check the message type. */
		if (copybuf[0] == 'k')
		{
			int			pos;
			bool		replyRequested;
			XLogRecPtr	walEnd;
			bool		endposReached = false;

			/*
			 * Parse the keepalive message, enclosed in the CopyData message.
			 * We just check if the server requested a reply, and ignore the
			 * rest.
			 */
			pos = 1;			/* skip msgtype 'k' */
			walEnd = fe_recvint64(&copybuf[pos]);
			output_written_lsn = Max(walEnd, output_written_lsn);

			pos += 8;			/* read walEnd */

			pos += 8;			/* skip sendTime */

			if (r < pos + 1)
			{
				pg_log_error("streaming header too small: %d", r);
				goto error;
			}
			replyRequested = copybuf[pos];

			if (endpos != InvalidXLogRecPtr && walEnd >= endpos)
			{
				/*
				 * If there's nothing to read on the socket until a keepalive
				 * we know that the server has nothing to send us; and if
				 * walEnd has passed endpos, we know nothing else can have
				 * committed before endpos.  So we can bail out now.
				 */
				endposReached = true;
			}

			/* Send a reply, if necessary */
			if (replyRequested || endposReached)
			{
				if (!flushAndSendFeedback(conn, &now))
					goto error;
				last_status = now;
			}

			if (endposReached)
			{
				stop_reason = STREAM_STOP_KEEPALIVE;
				time_to_abort = true;
				break;
			}

			continue;
		}
		else if (copybuf[0] != 'w')
		{
			pg_log_error("unrecognized streaming header: \"%c\"",
						 copybuf[0]);
			goto error;
		}

		/*
		 * Read the header of the XLogData message, enclosed in the CopyData
		 * message. We only need the WAL location field (dataStart), the rest
		 * of the header is ignored.
		 */
		hdr_len = 1;			/* msgtype 'w' */
		hdr_len += 8;			/* dataStart */
		hdr_len += 8;			/* walEnd */
		hdr_len += 8;			/* sendTime */
		if (r < hdr_len + 1)
		{
			pg_log_error("streaming header too small: %d", r);
			goto error;
		}

		/* Extract WAL location for this block */
		cur_record_lsn = fe_recvint64(&copybuf[1]);

		if (endpos != InvalidXLogRecPtr && cur_record_lsn > endpos)
		{
			/*
			 * We've read past our endpoint, so prepare to go away being
			 * cautious about what happens to our output data.
			 */
			if (!flushAndSendFeedback(conn, &now))
				goto error;
			stop_reason = STREAM_STOP_END_OF_WAL;
			time_to_abort = true;
			break;
		}

		output_written_lsn = Max(cur_record_lsn, output_written_lsn);

		bytes_left = r - hdr_len;
		bytes_written = 0;

		/* signal that a fsync is needed */
		output_needs_fsync = true;

		while (bytes_left)
		{
			int			ret;

			ret = write(outfd,
						copybuf + hdr_len + bytes_written,
						bytes_left);

			if (ret < 0)
			{
				pg_log_error("could not write %d bytes to log file \"%s\": %m",
							 bytes_left, outfile);
				goto error;
			}

			/* Write was successful, advance our position */
			bytes_written += ret;
			bytes_left -= ret;
		}

		if (write(outfd, "\n", 1) != 1)
		{
			pg_log_error("could not write %d bytes to log file \"%s\": %m",
						 1, outfile);
			goto error;
		}

		if (endpos != InvalidXLogRecPtr && cur_record_lsn == endpos)
		{
			/* endpos was exactly the record we just processed, we're done */
			if (!flushAndSendFeedback(conn, &now))
				goto error;
			stop_reason = STREAM_STOP_END_OF_WAL;
			time_to_abort = true;
			break;
		}
	}

	/* Clean up connection state if stream has been aborted */
	if (time_to_abort)
		prepareToTerminate(conn, endpos, stop_reason, cur_record_lsn);

	res = PQgetResult(conn);
	if (PQresultStatus(res) == PGRES_COPY_OUT)
	{
		PQclear(res);

		/*
		 * We're doing a client-initiated clean exit and have sent CopyDone to
		 * the server. Drain any messages, so we don't miss a last-minute
		 * ErrorResponse. The walsender stops generating XLogData records once
		 * it sees CopyDone, so expect this to finish quickly. After CopyDone,
		 * it's too late for sendFeedback(), even if this were to take a long
		 * time. Hence, use synchronous-mode PQgetCopyData().
		 */
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
				break;
			if (r == -2)
			{
				pg_log_error("could not read COPY data: %s",
							 PQerrorMessage(conn));
				time_to_abort = false;	/* unclean exit */
				goto error;
			}
		}

		res = PQgetResult(conn);
	}
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("unexpected termination of replication stream: %s",
					 PQresultErrorMessage(res));
		goto error;
	}
	PQclear(res);

	if (outfd != -1 && strcmp(outfile, "-") != 0)
	{
		TimestampTz t = feGetCurrentTimestamp();

		/* no need to jump to error on failure here, we're finishing anyway */
		OutputFsync(t);

		if (close(outfd) != 0)
			pg_log_error("could not close file \"%s\": %m", outfile);
	}
	outfd = -1;
error:
	if (copybuf != NULL)
	{
		PQfreemem(copybuf);
		copybuf = NULL;
	}
	destroyPQExpBuffer(query);
	PQfinish(conn);
	conn = NULL;
}

/*
 * Unfortunately we can't do sensible signal handling on windows...
 */
#ifndef WIN32

/*
 * When SIGINT/SIGTERM are caught, just tell the system to exit at the next
 * possible moment.
 */
static void
sigexit_handler(SIGNAL_ARGS)
{
	stop_reason = STREAM_STOP_SIGNAL;
	time_to_abort = true;
}

/*
 * Trigger the output file to be reopened.
 */
static void
sighup_handler(SIGNAL_ARGS)
{
	output_reopen = true;
}
#endif


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
/* general options */
		{"file", required_argument, NULL, 'f'},
		{"fsync-interval", required_argument, NULL, 'F'},
		{"no-loop", no_argument, NULL, 'n'},
		{"verbose", no_argument, NULL, 'v'},
		{"two-phase", no_argument, NULL, 't'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, '?'},
/* connection options */
		{"dbname", required_argument, NULL, 'd'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
/* replication options */
		{"startpos", required_argument, NULL, 'I'},
		{"endpos", required_argument, NULL, 'E'},
		{"option", required_argument, NULL, 'o'},
		{"plugin", required_argument, NULL, 'P'},
		{"status-interval", required_argument, NULL, 's'},
		{"slot", required_argument, NULL, 'S'},
/* action */
		{"create-slot", no_argument, NULL, 1},
		{"start", no_argument, NULL, 2},
		{"drop-slot", no_argument, NULL, 3},
		{"if-not-exists", no_argument, NULL, 4},
		{NULL, 0, NULL, 0}
	};
	int			c;
	int			option_index;
	uint32		hi,
				lo;
	char	   *db_name;

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
			puts("pg_recvlogical (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "E:f:F:ntvd:h:p:U:wWI:o:P:s:S:",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
/* general options */
			case 'f':
				outfile = pg_strdup(optarg);
				break;
			case 'F':
				if (!option_parse_int(optarg, "-F/--fsync-interval", 0,
									  INT_MAX / 1000,
									  &fsync_interval))
					exit(1);
				fsync_interval *= 1000;
				break;
			case 'n':
				noloop = 1;
				break;
			case 't':
				two_phase = true;
				break;
			case 'v':
				verbose++;
				break;
/* connection options */
			case 'd':
				dbname = pg_strdup(optarg);
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
/* replication options */
			case 'I':
				if (sscanf(optarg, "%X/%X", &hi, &lo) != 2)
					pg_fatal("could not parse start position \"%s\"", optarg);
				startpos = ((uint64) hi) << 32 | lo;
				break;
			case 'E':
				if (sscanf(optarg, "%X/%X", &hi, &lo) != 2)
					pg_fatal("could not parse end position \"%s\"", optarg);
				endpos = ((uint64) hi) << 32 | lo;
				break;
			case 'o':
				{
					char	   *data = pg_strdup(optarg);
					char	   *val = strchr(data, '=');

					if (val != NULL)
					{
						/* remove =; separate data from val */
						*val = '\0';
						val++;
					}

					noptions += 1;
					options = pg_realloc(options, sizeof(char *) * noptions * 2);

					options[(noptions - 1) * 2] = data;
					options[(noptions - 1) * 2 + 1] = val;
				}

				break;
			case 'P':
				plugin = pg_strdup(optarg);
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
/* action */
			case 1:
				do_create_slot = true;
				break;
			case 2:
				do_start_slot = true;
				break;
			case 3:
				do_drop_slot = true;
				break;
			case 4:
				slot_exists_ok = true;
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
	 * Required arguments
	 */
	if (replication_slot == NULL)
	{
		pg_log_error("no slot specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (do_start_slot && outfile == NULL)
	{
		pg_log_error("no target file specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (!do_drop_slot && dbname == NULL)
	{
		pg_log_error("no database specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (!do_drop_slot && !do_create_slot && !do_start_slot)
	{
		pg_log_error("at least one action needs to be specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (do_drop_slot && (do_create_slot || do_start_slot))
	{
		pg_log_error("cannot use --create-slot or --start together with --drop-slot");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (startpos != InvalidXLogRecPtr && (do_create_slot || do_drop_slot))
	{
		pg_log_error("cannot use --create-slot or --drop-slot together with --startpos");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (endpos != InvalidXLogRecPtr && !do_start_slot)
	{
		pg_log_error("--endpos may only be specified with --start");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (two_phase && !do_create_slot)
	{
		pg_log_error("--two-phase may only be specified with --create-slot");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * Obtain a connection to server.  Notably, if we need a password, we want
	 * to collect it from the user immediately.
	 */
	conn = GetConnection();
	if (!conn)
		/* Error message already written in GetConnection() */
		exit(1);
	atexit(disconnect_atexit);

	/*
	 * Trap signals.  (Don't do this until after the initial password prompt,
	 * if one is needed, in GetConnection.)
	 */
#ifndef WIN32
	pqsignal(SIGINT, sigexit_handler);
	pqsignal(SIGTERM, sigexit_handler);
	pqsignal(SIGHUP, sighup_handler);
#endif

	/*
	 * Run IDENTIFY_SYSTEM to make sure we connected using a database specific
	 * replication connection.
	 */
	if (!RunIdentifySystem(conn, NULL, NULL, NULL, &db_name))
		exit(1);

	if (db_name == NULL)
		pg_fatal("could not establish database-specific replication connection");

	/*
	 * Set umask so that directories/files are created with the same
	 * permissions as directories/files in the source data directory.
	 *
	 * pg_mode_mask is set to owner-only by default and then updated in
	 * GetConnection() where we get the mode from the server-side with
	 * RetrieveDataDirCreatePerm() and then call SetDataDirectoryCreatePerm().
	 */
	umask(pg_mode_mask);

	/* Drop a replication slot. */
	if (do_drop_slot)
	{
		if (verbose)
			pg_log_info("dropping replication slot \"%s\"", replication_slot);

		if (!DropReplicationSlot(conn, replication_slot))
			exit(1);
	}

	/* Create a replication slot. */
	if (do_create_slot)
	{
		if (verbose)
			pg_log_info("creating replication slot \"%s\"", replication_slot);

		if (!CreateReplicationSlot(conn, replication_slot, plugin, false,
								   false, false, slot_exists_ok, two_phase))
			exit(1);
		startpos = InvalidXLogRecPtr;
	}

	if (!do_start_slot)
		exit(0);

	/* Stream loop */
	while (true)
	{
		StreamLogicalLog();
		if (time_to_abort)
		{
			/*
			 * We've been Ctrl-C'ed or reached an exit limit condition. That's
			 * not an error, so exit without an errorcode.
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

/*
 * Fsync our output data, and send a feedback message to the server.  Returns
 * true if successful, false otherwise.
 *
 * If successful, *now is updated to the current timestamp just before sending
 * feedback.
 */
static bool
flushAndSendFeedback(PGconn *conn, TimestampTz *now)
{
	/* flush data to disk, so that we send a recent flush pointer */
	if (!OutputFsync(*now))
		return false;
	*now = feGetCurrentTimestamp();
	if (!sendFeedback(conn, *now, true, false))
		return false;

	return true;
}

/*
 * Try to inform the server about our upcoming demise, but don't wait around or
 * retry on failure.
 */
static void
prepareToTerminate(PGconn *conn, XLogRecPtr endpos, StreamStopReason reason,
				   XLogRecPtr lsn)
{
	(void) PQputCopyEnd(conn, NULL);
	(void) PQflush(conn);

	if (verbose)
	{
		switch (reason)
		{
			case STREAM_STOP_SIGNAL:
				pg_log_info("received interrupt signal, exiting");
				break;
			case STREAM_STOP_KEEPALIVE:
				pg_log_info("end position %X/%X reached by keepalive",
							LSN_FORMAT_ARGS(endpos));
				break;
			case STREAM_STOP_END_OF_WAL:
				Assert(!XLogRecPtrIsInvalid(lsn));
				pg_log_info("end position %X/%X reached by WAL record at %X/%X",
							LSN_FORMAT_ARGS(endpos), LSN_FORMAT_ARGS(lsn));
				break;
			case STREAM_STOP_NONE:
				Assert(false);
				break;
		}
	}
}
