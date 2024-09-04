/*-------------------------------------------------------------------------
 *
 * pg_rewind.c
 *	  Synchronizes a PostgreSQL data directory to a new timeline
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "access/timeline.h"
#include "access/xlog_internal.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "common/file_perm.h"
#include "common/restricted_token.h"
#include "common/string.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/recovery_gen.h"
#include "fe_utils/string_utils.h"
#include "file_ops.h"
#include "filemap.h"
#include "getopt_long.h"
#include "pg_rewind.h"
#include "rewind_source.h"
#include "storage/bufpage.h"

static void usage(const char *progname);

static void perform_rewind(filemap_t *filemap, rewind_source *source,
						   XLogRecPtr chkptrec,
						   TimeLineID chkpttli,
						   XLogRecPtr chkptredo);

static void createBackupLabel(XLogRecPtr startpoint, TimeLineID starttli,
							  XLogRecPtr checkpointloc);

static void digestControlFile(ControlFileData *ControlFile,
							  const char *content, size_t size);
static void getRestoreCommand(const char *argv0);
static void sanityChecks(void);
static TimeLineHistoryEntry *getTimelineHistory(TimeLineID tli, bool is_source,
												int *nentries);
static void findCommonAncestorTimeline(TimeLineHistoryEntry *a_history,
									   int a_nentries,
									   TimeLineHistoryEntry *b_history,
									   int b_nentries,
									   XLogRecPtr *recptr, int *tliIndex);
static void ensureCleanShutdown(const char *argv0);
static void disconnect_atexit(void);

static ControlFileData ControlFile_target;
static ControlFileData ControlFile_source;
static ControlFileData ControlFile_source_after;

static const char *progname;
int			WalSegSz;

/* Configuration options */
char	   *datadir_target = NULL;
static char *datadir_source = NULL;
static char *connstr_source = NULL;
static char *restore_command = NULL;
static char *config_file = NULL;

static bool debug = false;
bool		showprogress = false;
bool		dry_run = false;
bool		do_sync = true;
static bool restore_wal = false;
DataDirSyncMethod sync_method = DATA_DIR_SYNC_METHOD_FSYNC;

/* Target history */
TimeLineHistoryEntry *targetHistory;
int			targetNentries;

/* Progress counters */
uint64		fetch_size;
uint64		fetch_done;

static PGconn *conn;
static rewind_source *source;

static void
usage(const char *progname)
{
	printf(_("%s resynchronizes a PostgreSQL cluster with another copy of the cluster.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -c, --restore-target-wal       use \"restore_command\" in target configuration to\n"
			 "                                 retrieve WAL files from archives\n"));
	printf(_("  -D, --target-pgdata=DIRECTORY  existing data directory to modify\n"));
	printf(_("      --source-pgdata=DIRECTORY  source data directory to synchronize with\n"));
	printf(_("      --source-server=CONNSTR    source server to synchronize with\n"));
	printf(_("  -n, --dry-run                  stop before modifying anything\n"));
	printf(_("  -N, --no-sync                  do not wait for changes to be written\n"
			 "                                 safely to disk\n"));
	printf(_("  -P, --progress                 write progress messages\n"));
	printf(_("  -R, --write-recovery-conf      write configuration for replication\n"
			 "                                 (requires --source-server)\n"));
	printf(_("      --config-file=FILENAME     use specified main server configuration\n"
			 "                                 file when running target cluster\n"));
	printf(_("      --debug                    write a lot of debug messages\n"));
	printf(_("      --no-ensure-shutdown       do not automatically fix unclean shutdown\n"));
	printf(_("      --sync-method=METHOD       set method for syncing files to disk\n"));
	printf(_("  -V, --version                  output version information, then exit\n"));
	printf(_("  -?, --help                     show this help, then exit\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"target-pgdata", required_argument, NULL, 'D'},
		{"write-recovery-conf", no_argument, NULL, 'R'},
		{"source-pgdata", required_argument, NULL, 1},
		{"source-server", required_argument, NULL, 2},
		{"no-ensure-shutdown", no_argument, NULL, 4},
		{"config-file", required_argument, NULL, 5},
		{"version", no_argument, NULL, 'V'},
		{"restore-target-wal", no_argument, NULL, 'c'},
		{"dry-run", no_argument, NULL, 'n'},
		{"no-sync", no_argument, NULL, 'N'},
		{"progress", no_argument, NULL, 'P'},
		{"debug", no_argument, NULL, 3},
		{"sync-method", required_argument, NULL, 6},
		{NULL, 0, NULL, 0}
	};
	int			option_index;
	int			c;
	XLogRecPtr	divergerec;
	int			lastcommontliIndex;
	XLogRecPtr	chkptrec;
	TimeLineID	chkpttli;
	XLogRecPtr	chkptredo;
	TimeLineID	source_tli;
	TimeLineID	target_tli;
	XLogRecPtr	target_wal_endrec;
	size_t		size;
	char	   *buffer;
	bool		no_ensure_shutdown = false;
	bool		rewind_needed;
	bool		writerecoveryconf = false;
	filemap_t  *filemap;

	pg_logging_init(argv[0]);
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

	while ((c = getopt_long(argc, argv, "cD:nNPR", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'c':
				restore_wal = true;
				break;

			case 'P':
				showprogress = true;
				break;

			case 'n':
				dry_run = true;
				break;

			case 'N':
				do_sync = false;
				break;

			case 'R':
				writerecoveryconf = true;
				break;

			case 3:
				debug = true;
				pg_logging_increase_verbosity();
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

			case 4:
				no_ensure_shutdown = true;
				break;

			case 5:
				config_file = pg_strdup(optarg);
				break;

			case 6:
				if (!parse_sync_method(optarg, &sync_method))
					exit(1);
				break;

			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (datadir_source == NULL && connstr_source == NULL)
	{
		pg_log_error("no source specified (--source-pgdata or --source-server)");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (datadir_source != NULL && connstr_source != NULL)
	{
		pg_log_error("only one of --source-pgdata or --source-server can be specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (datadir_target == NULL)
	{
		pg_log_error("no target data directory specified (--target-pgdata)");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (writerecoveryconf && connstr_source == NULL)
	{
		pg_log_error("no source server information (--source-server) specified for --write-recovery-conf");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
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
		pg_log_error("cannot be executed by \"root\"");
		pg_log_error_hint("You must run %s as the PostgreSQL superuser.",
						  progname);
		exit(1);
	}
#endif

	get_restricted_token();

	/* Set mask based on PGDATA permissions */
	if (!GetDataDirectoryCreatePerm(datadir_target))
		pg_fatal("could not read permissions of directory \"%s\": %m",
				 datadir_target);

	umask(pg_mode_mask);

	getRestoreCommand(argv[0]);

	atexit(disconnect_atexit);

	/*
	 * Ok, we have all the options and we're ready to start. First, connect to
	 * remote server.
	 */
	if (connstr_source)
	{
		conn = PQconnectdb(connstr_source);

		if (PQstatus(conn) == CONNECTION_BAD)
			pg_fatal("%s", PQerrorMessage(conn));

		if (showprogress)
			pg_log_info("connected to server");

		source = init_libpq_source(conn);
	}
	else
		source = init_local_source(datadir_source);

	/*
	 * Check the status of the target instance.
	 *
	 * If the target instance was not cleanly shut down, start and stop the
	 * target cluster once in single-user mode to enforce recovery to finish,
	 * ensuring that the cluster can be used by pg_rewind.  Note that if
	 * no_ensure_shutdown is specified, pg_rewind ignores this step, and users
	 * need to make sure by themselves that the target cluster is in a clean
	 * state.
	 */
	buffer = slurpFile(datadir_target, "global/pg_control", &size);
	digestControlFile(&ControlFile_target, buffer, size);
	pg_free(buffer);

	if (!no_ensure_shutdown &&
		ControlFile_target.state != DB_SHUTDOWNED &&
		ControlFile_target.state != DB_SHUTDOWNED_IN_RECOVERY)
	{
		ensureCleanShutdown(argv[0]);

		buffer = slurpFile(datadir_target, "global/pg_control", &size);
		digestControlFile(&ControlFile_target, buffer, size);
		pg_free(buffer);
	}

	buffer = source->fetch_file(source, "global/pg_control", &size);
	digestControlFile(&ControlFile_source, buffer, size);
	pg_free(buffer);

	sanityChecks();

	/*
	 * Usually, the TLI can be found in the latest checkpoint record. But if
	 * the source server is just being promoted (or it's a standby that's
	 * following a primary that's just being promoted), and the checkpoint
	 * requested by the promotion hasn't completed yet, the latest timeline is
	 * in minRecoveryPoint. So we check which is later, the TLI of the
	 * minRecoveryPoint or the latest checkpoint.
	 */
	source_tli = Max(ControlFile_source.minRecoveryPointTLI,
					 ControlFile_source.checkPointCopy.ThisTimeLineID);

	/* Similarly for the target. */
	target_tli = Max(ControlFile_target.minRecoveryPointTLI,
					 ControlFile_target.checkPointCopy.ThisTimeLineID);

	/*
	 * Find the common ancestor timeline between the clusters.
	 *
	 * If both clusters are already on the same timeline, there's nothing to
	 * do.
	 */
	if (target_tli == source_tli)
	{
		pg_log_info("source and target cluster are on the same timeline");
		rewind_needed = false;
		target_wal_endrec = 0;
	}
	else
	{
		XLogRecPtr	chkptendrec;
		TimeLineHistoryEntry *sourceHistory;
		int			sourceNentries;

		/*
		 * Retrieve timelines for both source and target, and find the point
		 * where they diverged.
		 */
		sourceHistory = getTimelineHistory(source_tli, true, &sourceNentries);
		targetHistory = getTimelineHistory(target_tli, false, &targetNentries);

		findCommonAncestorTimeline(sourceHistory, sourceNentries,
								   targetHistory, targetNentries,
								   &divergerec, &lastcommontliIndex);

		pg_log_info("servers diverged at WAL location %X/%X on timeline %u",
					LSN_FORMAT_ARGS(divergerec),
					targetHistory[lastcommontliIndex].tli);

		/*
		 * Don't need the source history anymore. The target history is still
		 * needed by the routines in parsexlog.c, when we read the target WAL.
		 */
		pfree(sourceHistory);


		/*
		 * Determine the end-of-WAL on the target.
		 *
		 * The WAL ends at the last shutdown checkpoint, or at
		 * minRecoveryPoint if it was a standby. (If we supported rewinding a
		 * server that was not shut down cleanly, we would need to replay
		 * until we reach the first invalid record, like crash recovery does.)
		 */

		/* read the checkpoint record on the target to see where it ends. */
		chkptendrec = readOneRecord(datadir_target,
									ControlFile_target.checkPoint,
									targetNentries - 1,
									restore_command);

		if (ControlFile_target.minRecoveryPoint > chkptendrec)
		{
			target_wal_endrec = ControlFile_target.minRecoveryPoint;
		}
		else
		{
			target_wal_endrec = chkptendrec;
		}

		/*
		 * Check for the possibility that the target is in fact a direct
		 * ancestor of the source. In that case, there is no divergent history
		 * in the target that needs rewinding.
		 */
		if (target_wal_endrec > divergerec)
		{
			rewind_needed = true;
		}
		else
		{
			/* the last common checkpoint record must be part of target WAL */
			Assert(target_wal_endrec == divergerec);

			rewind_needed = false;
		}
	}

	if (!rewind_needed)
	{
		pg_log_info("no rewind required");
		if (writerecoveryconf && !dry_run)
			WriteRecoveryConfig(conn, datadir_target,
								GenerateRecoveryConfig(conn, NULL, NULL));
		exit(0);
	}

	findLastCheckpoint(datadir_target, divergerec, lastcommontliIndex,
					   &chkptrec, &chkpttli, &chkptredo, restore_command);
	pg_log_info("rewinding from last common checkpoint at %X/%X on timeline %u",
				LSN_FORMAT_ARGS(chkptrec), chkpttli);

	/* Initialize the hash table to track the status of each file */
	filehash_init();

	/*
	 * Collect information about all files in the both data directories.
	 */
	if (showprogress)
		pg_log_info("reading source file list");
	source->traverse_files(source, &process_source_file);

	if (showprogress)
		pg_log_info("reading target file list");
	traverse_datadir(datadir_target, &process_target_file);

	/*
	 * Read the target WAL from last checkpoint before the point of fork, to
	 * extract all the pages that were modified on the target cluster after
	 * the fork.
	 */
	if (showprogress)
		pg_log_info("reading WAL in target");
	extractPageMap(datadir_target, chkptrec, lastcommontliIndex,
				   target_wal_endrec, restore_command);

	/*
	 * We have collected all information we need from both systems. Decide
	 * what to do with each file.
	 */
	filemap = decide_file_actions();
	if (showprogress)
		calculate_totals(filemap);

	/* this is too verbose even for verbose mode */
	if (debug)
		print_filemap(filemap);

	/*
	 * Ok, we're ready to start copying things over.
	 */
	if (showprogress)
	{
		pg_log_info("need to copy %lu MB (total source directory size is %lu MB)",
					(unsigned long) (filemap->fetch_size / (1024 * 1024)),
					(unsigned long) (filemap->total_size / (1024 * 1024)));

		fetch_size = filemap->fetch_size;
		fetch_done = 0;
	}

	/*
	 * We have now collected all the information we need from both systems,
	 * and we are ready to start modifying the target directory.
	 *
	 * This is the point of no return. Once we start copying things, there is
	 * no turning back!
	 */
	perform_rewind(filemap, source, chkptrec, chkpttli, chkptredo);

	if (showprogress)
		pg_log_info("syncing target data directory");
	sync_target_dir();

	/* Also update the standby configuration, if requested. */
	if (writerecoveryconf && !dry_run)
		WriteRecoveryConfig(conn, datadir_target,
							GenerateRecoveryConfig(conn, NULL, NULL));

	/* don't need the source connection anymore */
	source->destroy(source);
	if (conn)
	{
		PQfinish(conn);
		conn = NULL;
	}

	pg_log_info("Done!");

	return 0;
}

/*
 * Perform the rewind.
 *
 * We have already collected all the information we need from the
 * target and the source.
 */
static void
perform_rewind(filemap_t *filemap, rewind_source *source,
			   XLogRecPtr chkptrec,
			   TimeLineID chkpttli,
			   XLogRecPtr chkptredo)
{
	XLogRecPtr	endrec;
	TimeLineID	endtli;
	ControlFileData ControlFile_new;
	size_t		size;
	char	   *buffer;

	/*
	 * Execute the actions in the file map, fetching data from the source
	 * system as needed.
	 */
	for (int i = 0; i < filemap->nentries; i++)
	{
		file_entry_t *entry = filemap->entries[i];

		/*
		 * If this is a relation file, copy the modified blocks.
		 *
		 * This is in addition to any other changes.
		 */
		if (entry->target_pages_to_overwrite.bitmapsize > 0)
		{
			datapagemap_iterator_t *iter;
			BlockNumber blkno;
			off_t		offset;

			iter = datapagemap_iterate(&entry->target_pages_to_overwrite);
			while (datapagemap_next(iter, &blkno))
			{
				offset = blkno * BLCKSZ;
				source->queue_fetch_range(source, entry->path, offset, BLCKSZ);
			}
			pg_free(iter);
		}

		switch (entry->action)
		{
			case FILE_ACTION_NONE:
				/* nothing else to do */
				break;

			case FILE_ACTION_COPY:
				source->queue_fetch_file(source, entry->path, entry->source_size);
				break;

			case FILE_ACTION_TRUNCATE:
				truncate_target_file(entry->path, entry->source_size);
				break;

			case FILE_ACTION_COPY_TAIL:
				source->queue_fetch_range(source, entry->path,
										  entry->target_size,
										  entry->source_size - entry->target_size);
				break;

			case FILE_ACTION_REMOVE:
				remove_target(entry);
				break;

			case FILE_ACTION_CREATE:
				create_target(entry);
				break;

			case FILE_ACTION_UNDECIDED:
				pg_fatal("no action decided for file \"%s\"", entry->path);
				break;
		}
	}

	/* Complete any remaining range-fetches that we queued up above. */
	source->finish_fetch(source);

	close_target_file();

	progress_report(true);

	/*
	 * Fetch the control file from the source last. This ensures that the
	 * minRecoveryPoint is up-to-date.
	 */
	buffer = source->fetch_file(source, "global/pg_control", &size);
	digestControlFile(&ControlFile_source_after, buffer, size);
	pg_free(buffer);

	/*
	 * Sanity check: If the source is a local system, the control file should
	 * not have changed since we started.
	 *
	 * XXX: We assume it hasn't been modified, but actually, what could go
	 * wrong? The logic handles a libpq source that's modified concurrently,
	 * why not a local datadir?
	 */
	if (datadir_source &&
		memcmp(&ControlFile_source, &ControlFile_source_after,
			   sizeof(ControlFileData)) != 0)
	{
		pg_fatal("source system was modified while pg_rewind was running");
	}

	if (showprogress)
		pg_log_info("creating backup label and updating control file");

	/*
	 * Create a backup label file, to tell the target where to begin the WAL
	 * replay. Normally, from the last common checkpoint between the source
	 * and the target. But if the source is a standby server, it's possible
	 * that the last common checkpoint is *after* the standby's restartpoint.
	 * That implies that the source server has applied the checkpoint record,
	 * but hasn't performed a corresponding restartpoint yet. Make sure we
	 * start at the restartpoint's redo point in that case.
	 *
	 * Use the old version of the source's control file for this. The server
	 * might have finished the restartpoint after we started copying files,
	 * but we must begin from the redo point at the time that started copying.
	 */
	if (ControlFile_source.checkPointCopy.redo < chkptredo)
	{
		chkptredo = ControlFile_source.checkPointCopy.redo;
		chkpttli = ControlFile_source.checkPointCopy.ThisTimeLineID;
		chkptrec = ControlFile_source.checkPoint;
	}
	createBackupLabel(chkptredo, chkpttli, chkptrec);

	/*
	 * Update control file of target, to tell the target how far it must
	 * replay the WAL (minRecoveryPoint).
	 */
	if (connstr_source)
	{
		/*
		 * The source is a live server. Like in an online backup, it's
		 * important that we recover all the WAL that was generated while we
		 * were copying files.
		 */
		if (ControlFile_source_after.state == DB_IN_ARCHIVE_RECOVERY)
		{
			/*
			 * Source is a standby server. We must replay to its
			 * minRecoveryPoint.
			 */
			endrec = ControlFile_source_after.minRecoveryPoint;
			endtli = ControlFile_source_after.minRecoveryPointTLI;
		}
		else
		{
			/*
			 * Source is a production, non-standby, server. We must replay to
			 * the last WAL insert location.
			 */
			if (ControlFile_source_after.state != DB_IN_PRODUCTION)
				pg_fatal("source system was in unexpected state at end of rewind");

			endrec = source->get_current_wal_insert_lsn(source);
			endtli = Max(ControlFile_source_after.checkPointCopy.ThisTimeLineID,
						 ControlFile_source_after.minRecoveryPointTLI);
		}
	}
	else
	{
		/*
		 * Source is a local data directory. It should've shut down cleanly,
		 * and we must replay to the latest shutdown checkpoint.
		 */
		endrec = ControlFile_source_after.checkPoint;
		endtli = ControlFile_source_after.checkPointCopy.ThisTimeLineID;
	}

	memcpy(&ControlFile_new, &ControlFile_source_after, sizeof(ControlFileData));
	ControlFile_new.minRecoveryPoint = endrec;
	ControlFile_new.minRecoveryPointTLI = endtli;
	ControlFile_new.state = DB_IN_ARCHIVE_RECOVERY;
	if (!dry_run)
		update_controlfile(datadir_target, &ControlFile_new, do_sync);
}

static void
sanityChecks(void)
{
	/* TODO Check that there's no backup_label in either cluster */

	/* Check system_identifier match */
	if (ControlFile_target.system_identifier != ControlFile_source.system_identifier)
		pg_fatal("source and target clusters are from different systems");

	/* check version */
	if (ControlFile_target.pg_control_version != PG_CONTROL_VERSION ||
		ControlFile_source.pg_control_version != PG_CONTROL_VERSION ||
		ControlFile_target.catalog_version_no != CATALOG_VERSION_NO ||
		ControlFile_source.catalog_version_no != CATALOG_VERSION_NO)
	{
		pg_fatal("clusters are not compatible with this version of pg_rewind");
	}

	/*
	 * Target cluster need to use checksums or hint bit wal-logging, this to
	 * prevent from data corruption that could occur because of hint bits.
	 */
	if (ControlFile_target.data_checksum_version != PG_DATA_CHECKSUM_VERSION &&
		!ControlFile_target.wal_log_hints)
	{
		pg_fatal("target server needs to use either data checksums or \"wal_log_hints = on\"");
	}

	/*
	 * Target cluster better not be running. This doesn't guard against
	 * someone starting the cluster concurrently. Also, this is probably more
	 * strict than necessary; it's OK if the target node was not shut down
	 * cleanly, as long as it isn't running at the moment.
	 */
	if (ControlFile_target.state != DB_SHUTDOWNED &&
		ControlFile_target.state != DB_SHUTDOWNED_IN_RECOVERY)
		pg_fatal("target server must be shut down cleanly");

	/*
	 * When the source is a data directory, also require that the source
	 * server is shut down. There isn't any very strong reason for this
	 * limitation, but better safe than sorry.
	 */
	if (datadir_source &&
		ControlFile_source.state != DB_SHUTDOWNED &&
		ControlFile_source.state != DB_SHUTDOWNED_IN_RECOVERY)
		pg_fatal("source data directory must be shut down cleanly");
}

/*
 * Print a progress report based on the fetch_size and fetch_done variables.
 *
 * Progress report is written at maximum once per second, except that the
 * last progress report is always printed.
 *
 * If finished is set to true, this is the last progress report. The cursor
 * is moved to the next line.
 */
void
progress_report(bool finished)
{
	static pg_time_t last_progress_report = 0;
	int			percent;
	char		fetch_done_str[32];
	char		fetch_size_str[32];
	pg_time_t	now;

	if (!showprogress)
		return;

	now = time(NULL);
	if (now == last_progress_report && !finished)
		return;					/* Max once per second */

	last_progress_report = now;
	percent = fetch_size ? (int) ((fetch_done) * 100 / fetch_size) : 0;

	/*
	 * Avoid overflowing past 100% or the full size. This may make the total
	 * size number change as we approach the end of the backup (the estimate
	 * will always be wrong if WAL is included), but that's better than having
	 * the done column be bigger than the total.
	 */
	if (percent > 100)
		percent = 100;
	if (fetch_done > fetch_size)
		fetch_size = fetch_done;

	snprintf(fetch_done_str, sizeof(fetch_done_str), UINT64_FORMAT,
			 fetch_done / 1024);
	snprintf(fetch_size_str, sizeof(fetch_size_str), UINT64_FORMAT,
			 fetch_size / 1024);

	fprintf(stderr, _("%*s/%s kB (%d%%) copied"),
			(int) strlen(fetch_size_str), fetch_done_str, fetch_size_str,
			percent);

	/*
	 * Stay on the same line if reporting to a terminal and we're not done
	 * yet.
	 */
	fputc((!finished && isatty(fileno(stderr))) ? '\r' : '\n', stderr);
}

/*
 * Find minimum from two WAL locations assuming InvalidXLogRecPtr means
 * infinity as src/include/access/timeline.h states. This routine should
 * be used only when comparing WAL locations related to history files.
 */
static XLogRecPtr
MinXLogRecPtr(XLogRecPtr a, XLogRecPtr b)
{
	if (XLogRecPtrIsInvalid(a))
		return b;
	else if (XLogRecPtrIsInvalid(b))
		return a;
	else
		return Min(a, b);
}

/*
 * Retrieve timeline history for the source or target system.
 */
static TimeLineHistoryEntry *
getTimelineHistory(TimeLineID tli, bool is_source, int *nentries)
{
	TimeLineHistoryEntry *history;

	/*
	 * Timeline 1 does not have a history file, so there is no need to check
	 * and fake an entry with infinite start and end positions.
	 */
	if (tli == 1)
	{
		history = (TimeLineHistoryEntry *) pg_malloc(sizeof(TimeLineHistoryEntry));
		history->tli = tli;
		history->begin = history->end = InvalidXLogRecPtr;
		*nentries = 1;
	}
	else
	{
		char		path[MAXPGPATH];
		char	   *histfile;

		TLHistoryFilePath(path, tli);

		/* Get history file from appropriate source */
		if (is_source)
			histfile = source->fetch_file(source, path, NULL);
		else
			histfile = slurpFile(datadir_target, path, NULL);

		history = rewind_parseTimeLineHistory(histfile, tli, nentries);
		pg_free(histfile);
	}

	/* In debugging mode, print what we read */
	if (debug)
	{
		int			i;

		if (is_source)
			pg_log_debug("Source timeline history:");
		else
			pg_log_debug("Target timeline history:");

		for (i = 0; i < *nentries; i++)
		{
			TimeLineHistoryEntry *entry;

			entry = &history[i];
			pg_log_debug("%u: %X/%X - %X/%X", entry->tli,
						 LSN_FORMAT_ARGS(entry->begin),
						 LSN_FORMAT_ARGS(entry->end));
		}
	}

	return history;
}

/*
 * Determine the TLI of the last common timeline in the timeline history of
 * two clusters. *tliIndex is set to the index of last common timeline in
 * the arrays, and *recptr is set to the position where the timeline history
 * diverged (ie. the first WAL record that's not the same in both clusters).
 */
static void
findCommonAncestorTimeline(TimeLineHistoryEntry *a_history, int a_nentries,
						   TimeLineHistoryEntry *b_history, int b_nentries,
						   XLogRecPtr *recptr, int *tliIndex)
{
	int			i,
				n;

	/*
	 * Trace the history forward, until we hit the timeline diverge. It may
	 * still be possible that the source and target nodes used the same
	 * timeline number in their history but with different start position
	 * depending on the history files that each node has fetched in previous
	 * recovery processes. Hence check the start position of the new timeline
	 * as well and move down by one extra timeline entry if they do not match.
	 */
	n = Min(a_nentries, b_nentries);
	for (i = 0; i < n; i++)
	{
		if (a_history[i].tli != b_history[i].tli ||
			a_history[i].begin != b_history[i].begin)
			break;
	}

	if (i > 0)
	{
		i--;
		*recptr = MinXLogRecPtr(a_history[i].end, b_history[i].end);
		*tliIndex = i;
		return;
	}
	else
	{
		pg_fatal("could not find common ancestor of the source and target cluster's timelines");
	}
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

	XLByteToSeg(startpoint, startsegno, WalSegSz);
	XLogFileName(xlogfilename, starttli, startsegno, WalSegSz);

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
				   LSN_FORMAT_ARGS(startpoint), xlogfilename,
				   LSN_FORMAT_ARGS(checkpointloc),
				   strfbuf);
	if (len >= sizeof(buf))
		pg_fatal("backup label buffer too small");	/* shouldn't happen */

	/* TODO: move old file out of the way, if any. */
	open_target_file("backup_label", true); /* BACKUP_LABEL_FILE */
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
		pg_fatal("unexpected control file CRC");
}

/*
 * Verify control file contents in the buffer 'content', and copy it to
 * *ControlFile.
 */
static void
digestControlFile(ControlFileData *ControlFile, const char *content,
				  size_t size)
{
	if (size != PG_CONTROL_FILE_SIZE)
		pg_fatal("unexpected control file size %d, expected %d",
				 (int) size, PG_CONTROL_FILE_SIZE);

	memcpy(ControlFile, content, sizeof(ControlFileData));

	/* set and validate WalSegSz */
	WalSegSz = ControlFile->xlog_seg_size;

	if (!IsValidWalSegSize(WalSegSz))
	{
		pg_log_error(ngettext("invalid WAL segment size in control file (%d byte)",
							  "invalid WAL segment size in control file (%d bytes)",
							  WalSegSz),
					 WalSegSz);
		pg_log_error_detail("The WAL segment size must be a power of two between 1 MB and 1 GB.");
		exit(1);
	}

	/* Additional checks on control file */
	checkControlFile(ControlFile);
}

/*
 * Get value of GUC parameter restore_command from the target cluster.
 *
 * This uses a logic based on "postgres -C" to get the value from the
 * cluster.
 */
static void
getRestoreCommand(const char *argv0)
{
	int			rc;
	char		postgres_exec_path[MAXPGPATH];
	PQExpBuffer postgres_cmd;

	if (!restore_wal)
		return;

	/* find postgres executable */
	rc = find_other_exec(argv0, "postgres",
						 PG_BACKEND_VERSIONSTR,
						 postgres_exec_path);

	if (rc < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (rc == -1)
			pg_fatal("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"",
					 "postgres", progname, full_path);
		else
			pg_fatal("program \"%s\" was found by \"%s\" but was not the same version as %s",
					 "postgres", full_path, progname);
	}

	/*
	 * Build a command able to retrieve the value of GUC parameter
	 * restore_command, if set.
	 */
	postgres_cmd = createPQExpBuffer();

	/* path to postgres, properly quoted */
	appendShellString(postgres_cmd, postgres_exec_path);

	/* add -D switch, with properly quoted data directory */
	appendPQExpBufferStr(postgres_cmd, " -D ");
	appendShellString(postgres_cmd, datadir_target);

	/* add custom configuration file only if requested */
	if (config_file != NULL)
	{
		appendPQExpBufferStr(postgres_cmd, " -c config_file=");
		appendShellString(postgres_cmd, config_file);
	}

	/* add -C switch, for restore_command */
	appendPQExpBufferStr(postgres_cmd, " -C restore_command");

	restore_command = pipe_read_line(postgres_cmd->data);
	if (restore_command == NULL)
		pg_fatal("could not read \"restore_command\" from target cluster");

	(void) pg_strip_crlf(restore_command);

	if (strcmp(restore_command, "") == 0)
		pg_fatal("\"restore_command\" is not set in the target cluster");

	pg_log_debug("using for rewind \"restore_command = \'%s\'\"",
				 restore_command);

	destroyPQExpBuffer(postgres_cmd);
}


/*
 * Ensure clean shutdown of target instance by launching single-user mode
 * postgres to do crash recovery.
 */
static void
ensureCleanShutdown(const char *argv0)
{
	int			ret;
	char		exec_path[MAXPGPATH];
	PQExpBuffer postgres_cmd;

	/* locate postgres binary */
	if ((ret = find_other_exec(argv0, "postgres",
							   PG_BACKEND_VERSIONSTR,
							   exec_path)) < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			pg_fatal("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"",
					 "postgres", progname, full_path);
		else
			pg_fatal("program \"%s\" was found by \"%s\" but was not the same version as %s",
					 "postgres", full_path, progname);
	}

	pg_log_info("executing \"%s\" for target server to complete crash recovery",
				exec_path);

	/*
	 * Skip processing if requested, but only after ensuring presence of
	 * postgres.
	 */
	if (dry_run)
		return;

	/*
	 * Finally run postgres in single-user mode.  There is no need to use
	 * fsync here.  This makes the recovery faster, and the target data folder
	 * is synced at the end anyway.
	 */
	postgres_cmd = createPQExpBuffer();

	/* path to postgres, properly quoted */
	appendShellString(postgres_cmd, exec_path);

	/* add set of options with properly quoted data directory */
	appendPQExpBufferStr(postgres_cmd, " --single -F -D ");
	appendShellString(postgres_cmd, datadir_target);

	/* add custom configuration file only if requested */
	if (config_file != NULL)
	{
		appendPQExpBufferStr(postgres_cmd, " -c config_file=");
		appendShellString(postgres_cmd, config_file);
	}

	/* finish with the database name, and a properly quoted redirection */
	appendPQExpBufferStr(postgres_cmd, " template1 < ");
	appendShellString(postgres_cmd, DEVNULL);

	fflush(NULL);
	if (system(postgres_cmd->data) != 0)
	{
		pg_log_error("postgres single-user mode in target cluster failed");
		pg_log_error_detail("Command was: %s", postgres_cmd->data);
		exit(1);
	}

	destroyPQExpBuffer(postgres_cmd);
}

static void
disconnect_atexit(void)
{
	if (conn != NULL)
		PQfinish(conn);
}
