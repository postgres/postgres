/*-------------------------------------------------------------------------
 *
 * pg_rewind.c
 *	  Synchronizes an old master server to a new timeline
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "pg_rewind.h"
#include "fetch.h"
#include "file_ops.h"
#include "filemap.h"
#include "logging.h"

#include "access/timeline.h"
#include "access/xlog_internal.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "common/restricted_token.h"
#include "getopt_long.h"
#include "storage/bufpage.h"

static void usage(const char *progname);

static void createBackupLabel(XLogRecPtr startpoint, TimeLineID starttli,
				  XLogRecPtr checkpointloc);

static void digestControlFile(ControlFileData *ControlFile, char *source,
				  size_t size);
static void updateControlFile(ControlFileData *ControlFile);
static void syncTargetDirectory(const char *argv0);
static void sanityChecks(void);
static void findCommonAncestorTimeline(XLogRecPtr *recptr, TimeLineID *tli);

static ControlFileData ControlFile_target;
static ControlFileData ControlFile_source;

const char *progname;

/* Configuration options */
char	   *datadir_target = NULL;
char	   *datadir_source = NULL;
char	   *connstr_source = NULL;

bool		debug = false;
bool		showprogress = false;
bool		dry_run = false;

static void
usage(const char *progname)
{
	printf(_("%s resynchronizes a PostgreSQL cluster with another copy of the cluster.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -D, --target-pgdata=DIRECTORY  existing data directory to modify\n"));
	printf(_("      --source-pgdata=DIRECTORY  source data directory to synchronize with\n"));
	printf(_("      --source-server=CONNSTR    source server to synchronize with\n"));
	printf(_("  -n, --dry-run                  stop before modifying anything\n"));
	printf(_("  -P, --progress                 write progress messages\n"));
	printf(_("      --debug                    write a lot of debug messages\n"));
	printf(_("  -V, --version                  output version information, then exit\n"));
	printf(_("  -?, --help                     show this help, then exit\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"target-pgdata", required_argument, NULL, 'D'},
		{"source-pgdata", required_argument, NULL, 1},
		{"source-server", required_argument, NULL, 2},
		{"version", no_argument, NULL, 'V'},
		{"dry-run", no_argument, NULL, 'n'},
		{"progress", no_argument, NULL, 'P'},
		{"debug", no_argument, NULL, 3},
		{NULL, 0, NULL, 0}
	};
	int			option_index;
	int			c;
	XLogRecPtr	divergerec;
	TimeLineID	lastcommontli;
	XLogRecPtr	chkptrec;
	TimeLineID	chkpttli;
	XLogRecPtr	chkptredo;
	size_t		size;
	char	   *buffer;
	bool		rewind_needed;
	XLogRecPtr	endrec;
	TimeLineID	endtli;
	ControlFileData ControlFile_new;

	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_rewind"));
	progname = get_progname(argv[0]);

	/* Process command-line arguments */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_rewind (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "D:nP", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case '?':
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);

			case 'P':
				showprogress = true;
				break;

			case 'n':
				dry_run = true;
				break;

			case 3:
				debug = true;
				break;

			case 'D':			/* -D or --target-pgdata */
				datadir_target = pg_strdup(optarg);
				break;

			case 1:				/* --source-pgdata */
				datadir_source = pg_strdup(optarg);
				break;
			case 2:				/* --source-server */
				connstr_source = pg_strdup(optarg);
				break;
		}
	}

	if (datadir_source == NULL && connstr_source == NULL)
	{
		fprintf(stderr, _("%s: no source specified (--source-pgdata or --source-server)\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	if (datadir_target == NULL)
	{
		fprintf(stderr, _("%s: no target data directory specified (--target-pgdata)\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	/*
	 * Don't allow pg_rewind to be run as root, to avoid overwriting the
	 * ownership of files in the data directory. We need only check for root
	 * -- any other user won't have sufficient permissions to modify files in
	 * the data directory.
	 */
#ifndef WIN32
	if (geteuid() == 0)
	{
		fprintf(stderr, _("cannot be executed by \"root\"\n"));
		fprintf(stderr, _("You must run %s as the PostgreSQL superuser.\n"),
				progname);
	}
#endif

	get_restricted_token(progname);

	/* Connect to remote server */
	if (connstr_source)
		libpqConnect(connstr_source);

	/*
	 * Ok, we have all the options and we're ready to start. Read in all the
	 * information we need from both clusters.
	 */
	buffer = slurpFile(datadir_target, "global/pg_control", &size);
	digestControlFile(&ControlFile_target, buffer, size);
	pg_free(buffer);

	buffer = fetchFile("global/pg_control", &size);
	digestControlFile(&ControlFile_source, buffer, size);
	pg_free(buffer);

	sanityChecks();

	/*
	 * If both clusters are already on the same timeline, there's nothing to
	 * do.
	 */
	if (ControlFile_target.checkPointCopy.ThisTimeLineID == ControlFile_source.checkPointCopy.ThisTimeLineID)
	{
		printf(_("source and target cluster are on the same timeline\n"));
		rewind_needed = false;
	}
	else
	{
		findCommonAncestorTimeline(&divergerec, &lastcommontli);
		printf(_("servers diverged at WAL position %X/%X on timeline %u\n"),
			   (uint32) (divergerec >> 32), (uint32) divergerec, lastcommontli);

		/*
		 * Check for the possibility that the target is in fact a direct ancestor
		 * of the source. In that case, there is no divergent history in the
		 * target that needs rewinding.
		 */
		if (ControlFile_target.checkPoint >= divergerec)
		{
			rewind_needed = true;
		}
		else
		{
			XLogRecPtr	chkptendrec;

			/* Read the checkpoint record on the target to see where it ends. */
			chkptendrec = readOneRecord(datadir_target,
										ControlFile_target.checkPoint,
						   ControlFile_target.checkPointCopy.ThisTimeLineID);

			/*
			 * If the histories diverged exactly at the end of the shutdown
			 * checkpoint record on the target, there are no WAL records in the
			 * target that don't belong in the source's history, and no rewind is
			 * needed.
			 */
			if (chkptendrec == divergerec)
				rewind_needed = false;
			else
				rewind_needed = true;
		}
	}

	if (!rewind_needed)
	{
		printf(_("no rewind required\n"));
		exit(0);
	}

	findLastCheckpoint(datadir_target, divergerec, lastcommontli,
					   &chkptrec, &chkpttli, &chkptredo);
	printf(_("rewinding from last common checkpoint at %X/%X on timeline %u\n"),
		   (uint32) (chkptrec >> 32), (uint32) chkptrec,
		   chkpttli);

	/*
	 * Build the filemap, by comparing the source and target data directories.
	 */
	filemap_create();
	pg_log(PG_PROGRESS, "reading source file list\n");
	fetchSourceFileList();
	pg_log(PG_PROGRESS, "reading target file list\n");
	traverse_datadir(datadir_target, &process_target_file);

	/*
	 * Read the target WAL from last checkpoint before the point of fork, to
	 * extract all the pages that were modified on the target cluster after
	 * the fork. We can stop reading after reaching the final shutdown record.
	 * XXX: If we supported rewinding a server that was not shut down cleanly,
	 * we would need to replay until the end of WAL here.
	 */
	pg_log(PG_PROGRESS, "reading WAL in target\n");
	extractPageMap(datadir_target, chkptrec, lastcommontli,
				   ControlFile_target.checkPoint);
	filemap_finalize();

	if (showprogress)
		calculate_totals();

	/* this is too verbose even for verbose mode */
	if (debug)
		print_filemap();

	/*
	 * Ok, we're ready to start copying things over.
	 */
	if (showprogress)
	{
		pg_log(PG_PROGRESS, "need to copy %lu MB (total source directory size is %lu MB)\n",
			   (unsigned long) (filemap->fetch_size / (1024 * 1024)),
			   (unsigned long) (filemap->total_size / (1024 * 1024)));

		fetch_size = filemap->fetch_size;
		fetch_done = 0;
	}

	/*
	 * This is the point of no return. Once we start copying things, we have
	 * modified the target directory and there is no turning back!
	 */

	executeFileMap();

	progress_report(true);

	pg_log(PG_PROGRESS, "\ncreating backup label and updating control file\n");
	createBackupLabel(chkptredo, chkpttli, chkptrec);

	/*
	 * Update control file of target. Make it ready to perform archive
	 * recovery when restarting.
	 *
	 * minRecoveryPoint is set to the current WAL insert location in the
	 * source server. Like in an online backup, it's important that we recover
	 * all the WAL that was generated while we copied the files over.
	 */
	memcpy(&ControlFile_new, &ControlFile_source, sizeof(ControlFileData));

	if (connstr_source)
	{
		endrec = libpqGetCurrentXlogInsertLocation();
		endtli = ControlFile_source.checkPointCopy.ThisTimeLineID;
	}
	else
	{
		endrec = ControlFile_source.checkPoint;
		endtli = ControlFile_source.checkPointCopy.ThisTimeLineID;
	}
	ControlFile_new.minRecoveryPoint = endrec;
	ControlFile_new.minRecoveryPointTLI = endtli;
	ControlFile_new.state = DB_IN_ARCHIVE_RECOVERY;
	updateControlFile(&ControlFile_new);

	pg_log(PG_PROGRESS, "syncing target data directory\n");
	syncTargetDirectory(argv[0]);

	printf(_("Done!\n"));

	return 0;
}

static void
sanityChecks(void)
{
	/* TODO Check that there's no backup_label in either cluster */

	/* Check system_id match */
	if (ControlFile_target.system_identifier != ControlFile_source.system_identifier)
		pg_fatal("source and target clusters are from different systems\n");

	/* check version */
	if (ControlFile_target.pg_control_version != PG_CONTROL_VERSION ||
		ControlFile_source.pg_control_version != PG_CONTROL_VERSION ||
		ControlFile_target.catalog_version_no != CATALOG_VERSION_NO ||
		ControlFile_source.catalog_version_no != CATALOG_VERSION_NO)
	{
		pg_fatal("clusters are not compatible with this version of pg_rewind\n");
	}

	/*
	 * Target cluster need to use checksums or hint bit wal-logging, this to
	 * prevent from data corruption that could occur because of hint bits.
	 */
	if (ControlFile_target.data_checksum_version != PG_DATA_CHECKSUM_VERSION &&
		!ControlFile_target.wal_log_hints)
	{
		pg_fatal("target server needs to use either data checksums or \"wal_log_hints = on\"\n");
	}

	/*
	 * Target cluster better not be running. This doesn't guard against
	 * someone starting the cluster concurrently. Also, this is probably more
	 * strict than necessary; it's OK if the target node was not shut down
	 * cleanly, as long as it isn't running at the moment.
	 */
	if (ControlFile_target.state != DB_SHUTDOWNED &&
		ControlFile_target.state != DB_SHUTDOWNED_IN_RECOVERY)
		pg_fatal("target server must be shut down cleanly\n");

	/*
	 * When the source is a data directory, also require that the source
	 * server is shut down. There isn't any very strong reason for this
	 * limitation, but better safe than sorry.
	 */
	if (datadir_source &&
		ControlFile_source.state != DB_SHUTDOWNED &&
		ControlFile_source.state != DB_SHUTDOWNED_IN_RECOVERY)
		pg_fatal("source data directory must be shut down cleanly\n");
}

/*
 * Determine the TLI of the last common timeline in the histories of the two
 * clusters. *tli is set to the last common timeline, and *recptr is set to
 * the position where the histories diverged (ie. the first WAL record that's
 * not the same in both clusters).
 *
 * Control files of both clusters must be read into ControlFile_target/source
 * before calling this.
 */
static void
findCommonAncestorTimeline(XLogRecPtr *recptr, TimeLineID *tli)
{
	TimeLineID	targettli;
	TimeLineHistoryEntry *sourceHistory;
	int			nentries;
	int			i;
	TimeLineID	sourcetli;

	targettli = ControlFile_target.checkPointCopy.ThisTimeLineID;
	sourcetli = ControlFile_source.checkPointCopy.ThisTimeLineID;

	/* Timeline 1 does not have a history file, so no need to check */
	if (sourcetli == 1)
	{
		sourceHistory = (TimeLineHistoryEntry *) pg_malloc(sizeof(TimeLineHistoryEntry));
		sourceHistory->tli = sourcetli;
		sourceHistory->begin = sourceHistory->end = InvalidXLogRecPtr;
		nentries = 1;
	}
	else
	{
		char		path[MAXPGPATH];
		char	   *histfile;

		TLHistoryFilePath(path, sourcetli);
		histfile = fetchFile(path, NULL);

		sourceHistory = rewind_parseTimeLineHistory(histfile,
							ControlFile_source.checkPointCopy.ThisTimeLineID,
													&nentries);
		pg_free(histfile);
	}

	/*
	 * Trace the history backwards, until we hit the target timeline.
	 *
	 * TODO: This assumes that there are no timeline switches on the target
	 * cluster after the fork.
	 */
	for (i = nentries - 1; i >= 0; i--)
	{
		TimeLineHistoryEntry *entry = &sourceHistory[i];

		if (entry->tli == targettli)
		{
			/* found it */
			*recptr = entry->end;
			*tli = entry->tli;

			pg_free(sourceHistory);
			return;
		}
	}

	pg_fatal("could not find common ancestor of the source and target cluster's timelines\n");
}


/*
 * Create a backup_label file that forces recovery to begin at the last common
 * checkpoint.
 */
static void
createBackupLabel(XLogRecPtr startpoint, TimeLineID starttli, XLogRecPtr checkpointloc)
{
	XLogSegNo	startsegno;
	time_t		stamp_time;
	char		strfbuf[128];
	char		xlogfilename[MAXFNAMELEN];
	struct tm  *tmp;
	char		buf[1000];
	int			len;

	XLByteToSeg(startpoint, startsegno);
	XLogFileName(xlogfilename, starttli, startsegno);

	/*
	 * Construct backup label file
	 */
	stamp_time = time(NULL);
	tmp = localtime(&stamp_time);
	strftime(strfbuf, sizeof(strfbuf), "%Y-%m-%d %H:%M:%S %Z", tmp);

	len = snprintf(buf, sizeof(buf),
				   "START WAL LOCATION: %X/%X (file %s)\n"
				   "CHECKPOINT LOCATION: %X/%X\n"
				   "BACKUP METHOD: pg_rewind\n"
				   "BACKUP FROM: standby\n"
				   "START TIME: %s\n",
	/* omit LABEL: line */
			  (uint32) (startpoint >> 32), (uint32) startpoint, xlogfilename,
				   (uint32) (checkpointloc >> 32), (uint32) checkpointloc,
				   strfbuf);
	if (len >= sizeof(buf))
		pg_fatal("backup label buffer too small\n");	/* shouldn't happen */

	/* TODO: move old file out of the way, if any. */
	open_target_file("backup_label", true);		/* BACKUP_LABEL_FILE */
	write_target_range(buf, 0, len);
	close_target_file();
}

/*
 * Check CRC of control file
 */
static void
checkControlFile(ControlFileData *ControlFile)
{
	pg_crc32c	crc;

	/* Calculate CRC */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (char *) ControlFile, offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	/* And simply compare it */
	if (!EQ_CRC32C(crc, ControlFile->crc))
		pg_fatal("unexpected control file CRC\n");
}

/*
 * Verify control file contents in the buffer src, and copy it to *ControlFile.
 */
static void
digestControlFile(ControlFileData *ControlFile, char *src, size_t size)
{
	if (size != PG_CONTROL_SIZE)
		pg_fatal("unexpected control file size %d, expected %d\n",
				 (int) size, PG_CONTROL_SIZE);

	memcpy(ControlFile, src, sizeof(ControlFileData));

	/* Additional checks on control file */
	checkControlFile(ControlFile);
}

/*
 * Update the target's control file.
 */
static void
updateControlFile(ControlFileData *ControlFile)
{
	char		buffer[PG_CONTROL_SIZE];

	/* Recalculate CRC of control file */
	INIT_CRC32C(ControlFile->crc);
	COMP_CRC32C(ControlFile->crc,
				(char *) ControlFile,
				offsetof(ControlFileData, crc));
	FIN_CRC32C(ControlFile->crc);

	/*
	 * Write out PG_CONTROL_SIZE bytes into pg_control by zero-padding the
	 * excess over sizeof(ControlFileData) to avoid premature EOF related
	 * errors when reading it.
	 */
	memset(buffer, 0, PG_CONTROL_SIZE);
	memcpy(buffer, ControlFile, sizeof(ControlFileData));

	open_target_file("global/pg_control", false);

	write_target_range(buffer, 0, PG_CONTROL_SIZE);

	close_target_file();
}

/*
 * Sync target data directory to ensure that modifications are safely on disk.
 *
 * We do this once, for the whole data directory, for performance reasons.  At
 * the end of pg_rewind's run, the kernel is likely to already have flushed
 * most dirty buffers to disk. Additionally initdb -S uses a two-pass approach
 * (only initiating writeback in the first pass), which often reduces the
 * overall amount of IO noticeably.
 */
static void
syncTargetDirectory(const char *argv0)
{
	int		ret;
#define MAXCMDLEN (2 * MAXPGPATH)
	char	exec_path[MAXPGPATH];
	char	cmd[MAXCMDLEN];

	/* locate initdb binary */
	if ((ret = find_other_exec(argv0, "initdb",
							   "initdb (PostgreSQL) " PG_VERSION "\n",
							   exec_path)) < 0)
	{
		char        full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			pg_fatal("The program \"initdb\" is needed by %s but was \n"
					 "not found in the same directory as \"%s\".\n"
					 "Check your installation.\n", progname, full_path);
		else
			pg_fatal("The program \"initdb\" was found by \"%s\"\n"
					 "but was not the same version as %s.\n"
					 "Check your installation.\n", full_path, progname);
	}

	/* only skip processing after ensuring presence of initdb */
	if (dry_run)
		return;

	/* finally run initdb -S */
	if (debug)
		snprintf(cmd, MAXCMDLEN, "\"%s\" -D \"%s\" -S",
				 exec_path, datadir_target);
	else
		snprintf(cmd, MAXCMDLEN, "\"%s\" -D \"%s\" -S > \"%s\"",
				 exec_path, datadir_target, DEVNULL);

	if (system(cmd) != 0)
		pg_fatal("sync of target directory failed\n");
}
