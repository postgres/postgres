/*
 * pg_controldata
 *
 * reads the data from $PGDATA/global/pg_control
 *
 * copyright (c) Oliver Elphick <olly@lfix.co.uk>, 2001;
 * licence: BSD
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

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "pg_getopt.h"


static void
usage(const char *progname)
{
	printf(_("%s displays control information of a PostgreSQL database cluster.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION] [DATADIR]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_(" [-D] DATADIR    data directory\n"));
	printf(_("  -V, --version  output version information, then exit\n"));
	printf(_("  -?, --help     show this help, then exit\n"));
	printf(_("\nIf no data directory (DATADIR) is specified, "
			 "the environment variable PGDATA\nis used.\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
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
	return _("unrecognized wal_level");
}


int
main(int argc, char *argv[])
{
	ControlFileData *ControlFile;
	char	   *DataDir = NULL;
	time_t		time_tmp;
	char		pgctime_str[128];
	char		ckpttime_str[128];
	char		sysident_str[32];
	const char *strftime_fmt = "%c";
	const char *progname;
	XLogSegNo	segno;
	char		xlogfilename[MAXFNAMELEN];
	int			c;

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

	while ((c = getopt(argc, argv, "D:")) != -1)
	{
		switch (c)
		{
			case 'D':
				DataDir = optarg;
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
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
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (DataDir == NULL)
	{
		fprintf(stderr, _("%s: no data directory specified\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	/* get a copy of the control file */
	ControlFile = get_controlfile(DataDir, progname);

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
	strftime(pgctime_str, sizeof(pgctime_str), strftime_fmt,
			 localtime(&time_tmp));
	time_tmp = (time_t) ControlFile->checkPointCopy.time;
	strftime(ckpttime_str, sizeof(ckpttime_str), strftime_fmt,
			 localtime(&time_tmp));

	/*
	 * Calculate name of the WAL file containing the latest checkpoint's REDO
	 * start point.
	 */
	XLByteToSeg(ControlFile->checkPointCopy.redo, segno);
	XLogFileName(xlogfilename, ControlFile->checkPointCopy.ThisTimeLineID, segno);

	/*
	 * Format system_identifier separately to keep platform-dependent format
	 * code out of the translatable message string.
	 */
	snprintf(sysident_str, sizeof(sysident_str), UINT64_FORMAT,
			 ControlFile->system_identifier);

	printf(_("pg_control version number:            %u\n"),
		   ControlFile->pg_control_version);
	printf(_("Catalog version number:               %u\n"),
		   ControlFile->catalog_version_no);
	printf(_("Database system identifier:           %s\n"),
		   sysident_str);
	printf(_("Database cluster state:               %s\n"),
		   dbState(ControlFile->state));
	printf(_("pg_control last modified:             %s\n"),
		   pgctime_str);
	printf(_("Latest checkpoint location:           %X/%X\n"),
		   (uint32) (ControlFile->checkPoint >> 32),
		   (uint32) ControlFile->checkPoint);
	printf(_("Prior checkpoint location:            %X/%X\n"),
		   (uint32) (ControlFile->prevCheckPoint >> 32),
		   (uint32) ControlFile->prevCheckPoint);
	printf(_("Latest checkpoint's REDO location:    %X/%X\n"),
		   (uint32) (ControlFile->checkPointCopy.redo >> 32),
		   (uint32) ControlFile->checkPointCopy.redo);
	printf(_("Latest checkpoint's REDO WAL file:    %s\n"),
		   xlogfilename);
	printf(_("Latest checkpoint's TimeLineID:       %u\n"),
		   ControlFile->checkPointCopy.ThisTimeLineID);
	printf(_("Latest checkpoint's PrevTimeLineID:   %u\n"),
		   ControlFile->checkPointCopy.PrevTimeLineID);
	printf(_("Latest checkpoint's full_page_writes: %s\n"),
		   ControlFile->checkPointCopy.fullPageWrites ? _("on") : _("off"));
	printf(_("Latest checkpoint's NextXID:          %u:%u\n"),
		   ControlFile->checkPointCopy.nextXidEpoch,
		   ControlFile->checkPointCopy.nextXid);
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
		   (uint32) (ControlFile->unloggedLSN >> 32),
		   (uint32) ControlFile->unloggedLSN);
	printf(_("Minimum recovery ending location:     %X/%X\n"),
		   (uint32) (ControlFile->minRecoveryPoint >> 32),
		   (uint32) ControlFile->minRecoveryPoint);
	printf(_("Min recovery ending loc's timeline:   %u\n"),
		   ControlFile->minRecoveryPointTLI);
	printf(_("Backup start location:                %X/%X\n"),
		   (uint32) (ControlFile->backupStartPoint >> 32),
		   (uint32) ControlFile->backupStartPoint);
	printf(_("Backup end location:                  %X/%X\n"),
		   (uint32) (ControlFile->backupEndPoint >> 32),
		   (uint32) ControlFile->backupEndPoint);
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
	printf(_("Date/time type storage:               %s\n"),
		   (ControlFile->enableIntTimes ? _("64-bit integers") : _("floating-point numbers")));
	printf(_("Float4 argument passing:              %s\n"),
		   (ControlFile->float4ByVal ? _("by value") : _("by reference")));
	printf(_("Float8 argument passing:              %s\n"),
		   (ControlFile->float8ByVal ? _("by value") : _("by reference")));
	printf(_("Data page checksum version:           %u\n"),
		   ControlFile->data_checksum_version);
	return 0;
}
