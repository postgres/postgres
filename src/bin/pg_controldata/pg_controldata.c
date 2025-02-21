/*
 * pg_controldata
 *
 * reads the data from $PGDATA/global/pg_control
 *
 * copyright (c) Oliver Elphick <olly@lfix.co.uk>, 2001;
 * license: BSD
 *
 * src/bin/pg_controldata/pg_controldata.c
 */

/*
 * We have to use postgres.h not postgres_fe.h here, because there's so much
 * backend-only stuff in the XLOG include files we need.  But we need a
 * frontend-ish environment otherwise.  Hence this ugly hack.
 */
#define FRONTEND 1

#include "postgres.h"

#include <time.h>

#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "common/logging.h"
#include "getopt_long.h"
#include "pg_getopt.h"

static void
usage(const char *progname)
{
	printf(_("%s displays control information of a PostgreSQL database cluster.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION] [DATADIR]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_(" [-D, --pgdata=]DATADIR  data directory\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nIf no data directory (DATADIR) is specified, "
			 "the environment variable PGDATA\nis used.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}


static const char *
dbState(DBState state)
{
	switch (state)
	{
		case DB_STARTUP:
			return _("starting up");
		case DB_SHUTDOWNED:
			return _("shut down");
		case DB_SHUTDOWNED_IN_RECOVERY:
			return _("shut down in recovery");
		case DB_SHUTDOWNING:
			return _("shutting down");
		case DB_IN_CRASH_RECOVERY:
			return _("in crash recovery");
		case DB_IN_ARCHIVE_RECOVERY:
			return _("in archive recovery");
		case DB_IN_PRODUCTION:
			return _("in production");
	}
	return _("unrecognized status code");
}

static const char *
wal_level_str(WalLevel wal_level)
{
	switch (wal_level)
	{
		case WAL_LEVEL_MINIMAL:
			return "minimal";
		case WAL_LEVEL_REPLICA:
			return "replica";
		case WAL_LEVEL_LOGICAL:
			return "logical";
	}
	return _("unrecognized \"wal_level\"");
}


int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"pgdata", required_argument, NULL, 'D'},
		{NULL, 0, NULL, 0}
	};

	ControlFileData *ControlFile;
	bool		crc_ok;
	char	   *DataDir = NULL;
	time_t		time_tmp;
	struct tm  *tm_tmp;
	char		pgctime_str[128];
	char		ckpttime_str[128];
	char		mock_auth_nonce_str[MOCK_AUTH_NONCE_LEN * 2 + 1];
	const char *strftime_fmt = "%c";
	const char *progname;
	char		xlogfilename[MAXFNAMELEN];
	int			c;
	int			i;
	int			WalSegSz;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_controldata"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_controldata (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "D:", long_options, NULL)) != -1)
	{
		switch (c)
		{
			case 'D':
				DataDir = optarg;
				break;

			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (DataDir == NULL)
	{
		if (optind < argc)
			DataDir = argv[optind++];
		else
			DataDir = getenv("PGDATA");
	}

	/* Complain if any arguments remain */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (DataDir == NULL)
	{
		pg_log_error("no data directory specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* get a copy of the control file */
	ControlFile = get_controlfile(DataDir, &crc_ok);
	if (!crc_ok)
	{
		pg_log_warning("calculated CRC checksum does not match value stored in control file");
		pg_log_warning_detail("Either the control file is corrupt, or it has a different layout than this program "
							  "is expecting.  The results below are untrustworthy.");
	}

	/* set wal segment size */
	WalSegSz = ControlFile->xlog_seg_size;

	if (!IsValidWalSegSize(WalSegSz))
	{
		pg_log_warning(ngettext("invalid WAL segment size in control file (%d byte)",
								"invalid WAL segment size in control file (%d bytes)",
								WalSegSz),
					   WalSegSz);
		pg_log_warning_detail("The WAL segment size must be a power of two between 1 MB and 1 GB.");
		pg_log_warning_detail("The file is corrupt and the results below are untrustworthy.");
	}

	/*
	 * This slightly-chintzy coding will work as long as the control file
	 * timestamps are within the range of time_t; that should be the case in
	 * all foreseeable circumstances, so we don't bother importing the
	 * backend's timezone library into pg_controldata.
	 *
	 * Use variable for format to suppress overly-anal-retentive gcc warning
	 * about %c
	 */
	time_tmp = (time_t) ControlFile->time;
	tm_tmp = localtime(&time_tmp);

	if (tm_tmp != NULL)
		strftime(pgctime_str, sizeof(pgctime_str), strftime_fmt, tm_tmp);
	else
		snprintf(pgctime_str, sizeof(pgctime_str), _("???"));

	time_tmp = (time_t) ControlFile->checkPointCopy.time;
	tm_tmp = localtime(&time_tmp);

	if (tm_tmp != NULL)
		strftime(ckpttime_str, sizeof(ckpttime_str), strftime_fmt, tm_tmp);
	else
		snprintf(ckpttime_str, sizeof(ckpttime_str), _("???"));

	/*
	 * Calculate name of the WAL file containing the latest checkpoint's REDO
	 * start point.
	 *
	 * A corrupted control file could report a WAL segment size of 0 or
	 * negative value, and to guard against division by zero, we need to treat
	 * that specially.
	 */
	if (WalSegSz > 0)
	{
		XLogSegNo	segno;

		XLByteToSeg(ControlFile->checkPointCopy.redo, segno, WalSegSz);
		XLogFileName(xlogfilename, ControlFile->checkPointCopy.ThisTimeLineID,
					 segno, WalSegSz);
	}
	else
		strcpy(xlogfilename, _("???"));

	for (i = 0; i < MOCK_AUTH_NONCE_LEN; i++)
		snprintf(&mock_auth_nonce_str[i * 2], 3, "%02x",
				 (unsigned char) ControlFile->mock_authentication_nonce[i]);

	printf(_("pg_control version number:            %u\n"),
		   ControlFile->pg_control_version);
	printf(_("Catalog version number:               %u\n"),
		   ControlFile->catalog_version_no);
	printf(_("Database system identifier:           %llu\n"),
		   (unsigned long long) ControlFile->system_identifier);
	printf(_("Database cluster state:               %s\n"),
		   dbState(ControlFile->state));
	printf(_("pg_control last modified:             %s\n"),
		   pgctime_str);
	printf(_("Latest checkpoint location:           %X/%X\n"),
		   LSN_FORMAT_ARGS(ControlFile->checkPoint));
	printf(_("Latest checkpoint's REDO location:    %X/%X\n"),
		   LSN_FORMAT_ARGS(ControlFile->checkPointCopy.redo));
	printf(_("Latest checkpoint's REDO WAL file:    %s\n"),
		   xlogfilename);
	printf(_("Latest checkpoint's TimeLineID:       %u\n"),
		   ControlFile->checkPointCopy.ThisTimeLineID);
	printf(_("Latest checkpoint's PrevTimeLineID:   %u\n"),
		   ControlFile->checkPointCopy.PrevTimeLineID);
	printf(_("Latest checkpoint's full_page_writes: %s\n"),
		   ControlFile->checkPointCopy.fullPageWrites ? _("on") : _("off"));
	printf(_("Latest checkpoint's NextXID:          %u:%u\n"),
		   EpochFromFullTransactionId(ControlFile->checkPointCopy.nextXid),
		   XidFromFullTransactionId(ControlFile->checkPointCopy.nextXid));
	printf(_("Latest checkpoint's NextOID:          %u\n"),
		   ControlFile->checkPointCopy.nextOid);
	printf(_("Latest checkpoint's NextMultiXactId:  %u\n"),
		   ControlFile->checkPointCopy.nextMulti);
	printf(_("Latest checkpoint's NextMultiOffset:  %u\n"),
		   ControlFile->checkPointCopy.nextMultiOffset);
	printf(_("Latest checkpoint's oldestXID:        %u\n"),
		   ControlFile->checkPointCopy.oldestXid);
	printf(_("Latest checkpoint's oldestXID's DB:   %u\n"),
		   ControlFile->checkPointCopy.oldestXidDB);
	printf(_("Latest checkpoint's oldestActiveXID:  %u\n"),
		   ControlFile->checkPointCopy.oldestActiveXid);
	printf(_("Latest checkpoint's oldestMultiXid:   %u\n"),
		   ControlFile->checkPointCopy.oldestMulti);
	printf(_("Latest checkpoint's oldestMulti's DB: %u\n"),
		   ControlFile->checkPointCopy.oldestMultiDB);
	printf(_("Latest checkpoint's oldestCommitTsXid:%u\n"),
		   ControlFile->checkPointCopy.oldestCommitTsXid);
	printf(_("Latest checkpoint's newestCommitTsXid:%u\n"),
		   ControlFile->checkPointCopy.newestCommitTsXid);
	printf(_("Time of latest checkpoint:            %s\n"),
		   ckpttime_str);
	printf(_("Fake LSN counter for unlogged rels:   %X/%X\n"),
		   LSN_FORMAT_ARGS(ControlFile->unloggedLSN));
	printf(_("Minimum recovery ending location:     %X/%X\n"),
		   LSN_FORMAT_ARGS(ControlFile->minRecoveryPoint));
	printf(_("Min recovery ending loc's timeline:   %u\n"),
		   ControlFile->minRecoveryPointTLI);
	printf(_("Backup start location:                %X/%X\n"),
		   LSN_FORMAT_ARGS(ControlFile->backupStartPoint));
	printf(_("Backup end location:                  %X/%X\n"),
		   LSN_FORMAT_ARGS(ControlFile->backupEndPoint));
	printf(_("End-of-backup record required:        %s\n"),
		   ControlFile->backupEndRequired ? _("yes") : _("no"));
	printf(_("wal_level setting:                    %s\n"),
		   wal_level_str(ControlFile->wal_level));
	printf(_("wal_log_hints setting:                %s\n"),
		   ControlFile->wal_log_hints ? _("on") : _("off"));
	printf(_("max_connections setting:              %d\n"),
		   ControlFile->MaxConnections);
	printf(_("max_worker_processes setting:         %d\n"),
		   ControlFile->max_worker_processes);
	printf(_("max_wal_senders setting:              %d\n"),
		   ControlFile->max_wal_senders);
	printf(_("max_prepared_xacts setting:           %d\n"),
		   ControlFile->max_prepared_xacts);
	printf(_("max_locks_per_xact setting:           %d\n"),
		   ControlFile->max_locks_per_xact);
	printf(_("track_commit_timestamp setting:       %s\n"),
		   ControlFile->track_commit_timestamp ? _("on") : _("off"));
	printf(_("Maximum data alignment:               %u\n"),
		   ControlFile->maxAlign);
	/* we don't print floatFormat since can't say much useful about it */
	printf(_("Database block size:                  %u\n"),
		   ControlFile->blcksz);
	printf(_("Blocks per segment of large relation: %u\n"),
		   ControlFile->relseg_size);
	printf(_("WAL block size:                       %u\n"),
		   ControlFile->xlog_blcksz);
	printf(_("Bytes per WAL segment:                %u\n"),
		   ControlFile->xlog_seg_size);
	printf(_("Maximum length of identifiers:        %u\n"),
		   ControlFile->nameDataLen);
	printf(_("Maximum columns in an index:          %u\n"),
		   ControlFile->indexMaxKeys);
	printf(_("Maximum size of a TOAST chunk:        %u\n"),
		   ControlFile->toast_max_chunk_size);
	printf(_("Size of a large-object chunk:         %u\n"),
		   ControlFile->loblksize);
	/* This is no longer configurable, but users may still expect to see it: */
	printf(_("Date/time type storage:               %s\n"),
		   _("64-bit integers"));
	printf(_("Float8 argument passing:              %s\n"),
		   (ControlFile->float8ByVal ? _("by value") : _("by reference")));
	printf(_("Data page checksum version:           %u\n"),
		   ControlFile->data_checksum_version);
	printf(_("Default char data signedness:         %s\n"),
		   (ControlFile->default_char_signedness ? _("signed") : _("unsigned")));
	printf(_("Mock authentication nonce:            %s\n"),
		   mock_auth_nonce_str);
	return 0;
}
