/*-------------------------------------------------------------------------
 *
 * basebackup.c
 *	  code for taking a base backup and streaming it to a standby
 *
 * Portions Copyright (c) 2010-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/basebackup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "access/xlog_internal.h"	/* for pg_start/stop_backup */
#include "catalog/pg_type.h"
#include "common/file_perm.h"
#include "commands/progress.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "pgtar.h"
#include "port.h"
#include "postmaster/syslogger.h"
#include "replication/basebackup.h"
#include "replication/backup_manifest.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/dsm_impl.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/reinit.h"
#include "utils/builtins.h"
#include "utils/ps_status.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

typedef struct
{
	const char *label;
	bool		progress;
	bool		fastcheckpoint;
	bool		nowait;
	bool		includewal;
	uint32		maxrate;
	bool		sendtblspcmapfile;
	backup_manifest_option manifest;
	pg_checksum_type manifest_checksum_type;
} basebackup_options;

static int64 sendDir(const char *path, int basepathlen, bool sizeonly,
					 List *tablespaces, bool sendtblspclinks,
					 backup_manifest_info *manifest, const char *spcoid);
static bool sendFile(const char *readfilename, const char *tarfilename,
					 struct stat *statbuf, bool missing_ok, Oid dboid,
					 backup_manifest_info *manifest, const char *spcoid);
static void sendFileWithContent(const char *filename, const char *content,
								backup_manifest_info *manifest);
static int64 _tarWriteHeader(const char *filename, const char *linktarget,
							 struct stat *statbuf, bool sizeonly);
static int64 _tarWriteDir(const char *pathbuf, int basepathlen, struct stat *statbuf,
						  bool sizeonly);
static void send_int8_string(StringInfoData *buf, int64 intval);
static void SendBackupHeader(List *tablespaces);
static void perform_base_backup(basebackup_options *opt);
static void parse_basebackup_options(List *options, basebackup_options *opt);
static void SendXlogRecPtrResult(XLogRecPtr ptr, TimeLineID tli);
static int	compareWalFileNames(const ListCell *a, const ListCell *b);
static void throttle(size_t increment);
static void update_basebackup_progress(int64 delta);
static bool is_checksummed_file(const char *fullpath, const char *filename);

/* Was the backup currently in-progress initiated in recovery mode? */
static bool backup_started_in_recovery = false;

/* Relative path of temporary statistics directory */
static char *statrelpath = NULL;

/*
 * Size of each block sent into the tar stream for larger files.
 */
#define TAR_SEND_SIZE 32768

/*
 * How frequently to throttle, as a fraction of the specified rate-second.
 */
#define THROTTLING_FREQUENCY	8

/*
 * Checks whether we encountered any error in fread().  fread() doesn't give
 * any clue what has happened, so we check with ferror().  Also, neither
 * fread() nor ferror() set errno, so we just throw a generic error.
 */
#define CHECK_FREAD_ERROR(fp, filename) \
do { \
	if (ferror(fp)) \
		ereport(ERROR, \
				(errmsg("could not read from file \"%s\"", filename))); \
} while (0)

/* The actual number of bytes, transfer of which may cause sleep. */
static uint64 throttling_sample;

/* Amount of data already transferred but not yet throttled.  */
static int64 throttling_counter;

/* The minimum time required to transfer throttling_sample bytes. */
static TimeOffset elapsed_min_unit;

/* The last check of the transfer rate. */
static TimestampTz throttled_last;

/* The starting XLOG position of the base backup. */
static XLogRecPtr startptr;

/* Total number of checksum failures during base backup. */
static long long int total_checksum_failures;

/* Do not verify checksums. */
static bool noverify_checksums = false;

/*
 * Total amount of backup data that will be streamed.
 * -1 means that the size is not estimated.
 */
static int64 backup_total = 0;

/* Amount of backup data already streamed */
static int64 backup_streamed = 0;

/*
 * Definition of one element part of an exclusion list, used for paths part
 * of checksum validation or base backups.  "name" is the name of the file
 * or path to check for exclusion.  If "match_prefix" is true, any items
 * matching the name as prefix are excluded.
 */
struct exclude_list_item
{
	const char *name;
	bool		match_prefix;
};

/*
 * The contents of these directories are removed or recreated during server
 * start so they are not included in backups.  The directories themselves are
 * kept and included as empty to preserve access permissions.
 *
 * Note: this list should be kept in sync with the filter lists in pg_rewind's
 * filemap.c.
 */
static const char *const excludeDirContents[] =
{
	/*
	 * Skip temporary statistics files. PG_STAT_TMP_DIR must be skipped even
	 * when stats_temp_directory is set because PGSS_TEXT_FILE is always
	 * created there.
	 */
	PG_STAT_TMP_DIR,

	/*
	 * It is generally not useful to backup the contents of this directory
	 * even if the intention is to restore to another master. See backup.sgml
	 * for a more detailed description.
	 */
	"pg_replslot",

	/* Contents removed on startup, see dsm_cleanup_for_mmap(). */
	PG_DYNSHMEM_DIR,

	/* Contents removed on startup, see AsyncShmemInit(). */
	"pg_notify",

	/*
	 * Old contents are loaded for possible debugging but are not required for
	 * normal operation, see SerialInit().
	 */
	"pg_serial",

	/* Contents removed on startup, see DeleteAllExportedSnapshotFiles(). */
	"pg_snapshots",

	/* Contents zeroed on startup, see StartupSUBTRANS(). */
	"pg_subtrans",

	/* end of list */
	NULL
};

/*
 * List of files excluded from backups.
 */
static const struct exclude_list_item excludeFiles[] =
{
	/* Skip auto conf temporary file. */
	{PG_AUTOCONF_FILENAME ".tmp", false},

	/* Skip current log file temporary file */
	{LOG_METAINFO_DATAFILE_TMP, false},

	/*
	 * Skip relation cache because it is rebuilt on startup.  This includes
	 * temporary files.
	 */
	{RELCACHE_INIT_FILENAME, true},

	/*
	 * If there's a backup_label or tablespace_map file, it belongs to a
	 * backup started by the user with pg_start_backup().  It is *not* correct
	 * for this backup.  Our backup_label/tablespace_map is injected into the
	 * tar separately.
	 */
	{BACKUP_LABEL_FILE, false},
	{TABLESPACE_MAP, false},

	/*
	 * If there's a backup_manifest, it belongs to a backup that was used to
	 * start this server. It is *not* correct for this backup. Our
	 * backup_manifest is injected into the backup separately if users want
	 * it.
	 */
	{"backup_manifest", false},

	{"postmaster.pid", false},
	{"postmaster.opts", false},

	/* end of list */
	{NULL, false}
};

/*
 * List of files excluded from checksum validation.
 *
 * Note: this list should be kept in sync with what pg_checksums.c
 * includes.
 */
static const struct exclude_list_item noChecksumFiles[] = {
	{"pg_control", false},
	{"pg_filenode.map", false},
	{"pg_internal.init", true},
	{"PG_VERSION", false},
#ifdef EXEC_BACKEND
	{"config_exec_params", true},
#endif
	{NULL, false}
};

/*
 * Actually do a base backup for the specified tablespaces.
 *
 * This is split out mainly to avoid complaints about "variable might be
 * clobbered by longjmp" from stupider versions of gcc.
 */
static void
perform_base_backup(basebackup_options *opt)
{
	TimeLineID	starttli;
	XLogRecPtr	endptr;
	TimeLineID	endtli;
	StringInfo	labelfile;
	StringInfo	tblspc_map_file = NULL;
	backup_manifest_info manifest;
	int			datadirpathlen;
	List	   *tablespaces = NIL;

	backup_total = 0;
	backup_streamed = 0;
	pgstat_progress_start_command(PROGRESS_COMMAND_BASEBACKUP, InvalidOid);

	/*
	 * If the estimation of the total backup size is disabled, make the
	 * backup_total column in the view return NULL by setting the parameter to
	 * -1.
	 */
	if (!opt->progress)
	{
		backup_total = -1;
		pgstat_progress_update_param(PROGRESS_BASEBACKUP_BACKUP_TOTAL,
									 backup_total);
	}

	/* we're going to use a BufFile, so we need a ResourceOwner */
	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "base backup");

	datadirpathlen = strlen(DataDir);

	backup_started_in_recovery = RecoveryInProgress();

	labelfile = makeStringInfo();
	tblspc_map_file = makeStringInfo();
	InitializeBackupManifest(&manifest, opt->manifest,
							 opt->manifest_checksum_type);

	total_checksum_failures = 0;

	pgstat_progress_update_param(PROGRESS_BASEBACKUP_PHASE,
								 PROGRESS_BASEBACKUP_PHASE_WAIT_CHECKPOINT);
	startptr = do_pg_start_backup(opt->label, opt->fastcheckpoint, &starttli,
								  labelfile, &tablespaces,
								  tblspc_map_file,
								  opt->progress, opt->sendtblspcmapfile);

	/*
	 * Once do_pg_start_backup has been called, ensure that any failure causes
	 * us to abort the backup so we don't "leak" a backup counter. For this
	 * reason, *all* functionality between do_pg_start_backup() and the end of
	 * do_pg_stop_backup() should be inside the error cleanup block!
	 */

	PG_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, BoolGetDatum(false));
	{
		ListCell   *lc;
		tablespaceinfo *ti;
		int			tblspc_streamed = 0;

		/*
		 * Calculate the relative path of temporary statistics directory in
		 * order to skip the files which are located in that directory later.
		 */
		if (is_absolute_path(pgstat_stat_directory) &&
			strncmp(pgstat_stat_directory, DataDir, datadirpathlen) == 0)
			statrelpath = psprintf("./%s", pgstat_stat_directory + datadirpathlen + 1);
		else if (strncmp(pgstat_stat_directory, "./", 2) != 0)
			statrelpath = psprintf("./%s", pgstat_stat_directory);
		else
			statrelpath = pgstat_stat_directory;

		/* Add a node for the base directory at the end */
		ti = palloc0(sizeof(tablespaceinfo));
		if (opt->progress)
			ti->size = sendDir(".", 1, true, tablespaces, true, NULL, NULL);
		else
			ti->size = -1;
		tablespaces = lappend(tablespaces, ti);

		/*
		 * Calculate the total backup size by summing up the size of each
		 * tablespace
		 */
		if (opt->progress)
		{
			foreach(lc, tablespaces)
			{
				tablespaceinfo *tmp = (tablespaceinfo *) lfirst(lc);

				backup_total += tmp->size;
			}
		}

		/* Report that we are now streaming database files as a base backup */
		{
			const int	index[] = {
				PROGRESS_BASEBACKUP_PHASE,
				PROGRESS_BASEBACKUP_BACKUP_TOTAL,
				PROGRESS_BASEBACKUP_TBLSPC_TOTAL
			};
			const int64 val[] = {
				PROGRESS_BASEBACKUP_PHASE_STREAM_BACKUP,
				backup_total, list_length(tablespaces)
			};

			pgstat_progress_update_multi_param(3, index, val);
		}

		/* Send the starting position of the backup */
		SendXlogRecPtrResult(startptr, starttli);

		/* Send tablespace header */
		SendBackupHeader(tablespaces);

		/* Setup and activate network throttling, if client requested it */
		if (opt->maxrate > 0)
		{
			throttling_sample =
				(int64) opt->maxrate * (int64) 1024 / THROTTLING_FREQUENCY;

			/*
			 * The minimum amount of time for throttling_sample bytes to be
			 * transferred.
			 */
			elapsed_min_unit = USECS_PER_SEC / THROTTLING_FREQUENCY;

			/* Enable throttling. */
			throttling_counter = 0;

			/* The 'real data' starts now (header was ignored). */
			throttled_last = GetCurrentTimestamp();
		}
		else
		{
			/* Disable throttling. */
			throttling_counter = -1;
		}

		/* Send off our tablespaces one by one */
		foreach(lc, tablespaces)
		{
			tablespaceinfo *ti = (tablespaceinfo *) lfirst(lc);
			StringInfoData buf;

			/* Send CopyOutResponse message */
			pq_beginmessage(&buf, 'H');
			pq_sendbyte(&buf, 0);	/* overall format */
			pq_sendint16(&buf, 0);	/* natts */
			pq_endmessage(&buf);

			if (ti->path == NULL)
			{
				struct stat statbuf;

				/* In the main tar, include the backup_label first... */
				sendFileWithContent(BACKUP_LABEL_FILE, labelfile->data,
									&manifest);

				/*
				 * Send tablespace_map file if required and then the bulk of
				 * the files.
				 */
				if (tblspc_map_file && opt->sendtblspcmapfile)
				{
					sendFileWithContent(TABLESPACE_MAP, tblspc_map_file->data,
										&manifest);
					sendDir(".", 1, false, tablespaces, false,
							&manifest, NULL);
				}
				else
					sendDir(".", 1, false, tablespaces, true,
							&manifest, NULL);

				/* ... and pg_control after everything else. */
				if (lstat(XLOG_CONTROL_FILE, &statbuf) != 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not stat file \"%s\": %m",
									XLOG_CONTROL_FILE)));
				sendFile(XLOG_CONTROL_FILE, XLOG_CONTROL_FILE, &statbuf,
						 false, InvalidOid, &manifest, NULL);
			}
			else
				sendTablespace(ti->path, ti->oid, false, &manifest);

			/*
			 * If we're including WAL, and this is the main data directory we
			 * don't terminate the tar stream here. Instead, we will append
			 * the xlog files below and terminate it then. This is safe since
			 * the main data directory is always sent *last*.
			 */
			if (opt->includewal && ti->path == NULL)
			{
				Assert(lnext(tablespaces, lc) == NULL);
			}
			else
				pq_putemptymessage('c');	/* CopyDone */

			tblspc_streamed++;
			pgstat_progress_update_param(PROGRESS_BASEBACKUP_TBLSPC_STREAMED,
										 tblspc_streamed);
		}

		pgstat_progress_update_param(PROGRESS_BASEBACKUP_PHASE,
									 PROGRESS_BASEBACKUP_PHASE_WAIT_WAL_ARCHIVE);
		endptr = do_pg_stop_backup(labelfile->data, !opt->nowait, &endtli);
	}
	PG_END_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, BoolGetDatum(false));


	if (opt->includewal)
	{
		/*
		 * We've left the last tar file "open", so we can now append the
		 * required WAL files to it.
		 */
		char		pathbuf[MAXPGPATH];
		XLogSegNo	segno;
		XLogSegNo	startsegno;
		XLogSegNo	endsegno;
		struct stat statbuf;
		List	   *historyFileList = NIL;
		List	   *walFileList = NIL;
		char		firstoff[MAXFNAMELEN];
		char		lastoff[MAXFNAMELEN];
		DIR		   *dir;
		struct dirent *de;
		ListCell   *lc;
		TimeLineID	tli;

		pgstat_progress_update_param(PROGRESS_BASEBACKUP_PHASE,
									 PROGRESS_BASEBACKUP_PHASE_TRANSFER_WAL);

		/*
		 * I'd rather not worry about timelines here, so scan pg_wal and
		 * include all WAL files in the range between 'startptr' and 'endptr',
		 * regardless of the timeline the file is stamped with. If there are
		 * some spurious WAL files belonging to timelines that don't belong in
		 * this server's history, they will be included too. Normally there
		 * shouldn't be such files, but if there are, there's little harm in
		 * including them.
		 */
		XLByteToSeg(startptr, startsegno, wal_segment_size);
		XLogFileName(firstoff, ThisTimeLineID, startsegno, wal_segment_size);
		XLByteToPrevSeg(endptr, endsegno, wal_segment_size);
		XLogFileName(lastoff, ThisTimeLineID, endsegno, wal_segment_size);

		dir = AllocateDir("pg_wal");
		while ((de = ReadDir(dir, "pg_wal")) != NULL)
		{
			/* Does it look like a WAL segment, and is it in the range? */
			if (IsXLogFileName(de->d_name) &&
				strcmp(de->d_name + 8, firstoff + 8) >= 0 &&
				strcmp(de->d_name + 8, lastoff + 8) <= 0)
			{
				walFileList = lappend(walFileList, pstrdup(de->d_name));
			}
			/* Does it look like a timeline history file? */
			else if (IsTLHistoryFileName(de->d_name))
			{
				historyFileList = lappend(historyFileList, pstrdup(de->d_name));
			}
		}
		FreeDir(dir);

		/*
		 * Before we go any further, check that none of the WAL segments we
		 * need were removed.
		 */
		CheckXLogRemoved(startsegno, ThisTimeLineID);

		/*
		 * Sort the WAL filenames.  We want to send the files in order from
		 * oldest to newest, to reduce the chance that a file is recycled
		 * before we get a chance to send it over.
		 */
		list_sort(walFileList, compareWalFileNames);

		/*
		 * There must be at least one xlog file in the pg_wal directory, since
		 * we are doing backup-including-xlog.
		 */
		if (walFileList == NIL)
			ereport(ERROR,
					(errmsg("could not find any WAL files")));

		/*
		 * Sanity check: the first and last segment should cover startptr and
		 * endptr, with no gaps in between.
		 */
		XLogFromFileName((char *) linitial(walFileList),
						 &tli, &segno, wal_segment_size);
		if (segno != startsegno)
		{
			char		startfname[MAXFNAMELEN];

			XLogFileName(startfname, ThisTimeLineID, startsegno,
						 wal_segment_size);
			ereport(ERROR,
					(errmsg("could not find WAL file \"%s\"", startfname)));
		}
		foreach(lc, walFileList)
		{
			char	   *walFileName = (char *) lfirst(lc);
			XLogSegNo	currsegno = segno;
			XLogSegNo	nextsegno = segno + 1;

			XLogFromFileName(walFileName, &tli, &segno, wal_segment_size);
			if (!(nextsegno == segno || currsegno == segno))
			{
				char		nextfname[MAXFNAMELEN];

				XLogFileName(nextfname, ThisTimeLineID, nextsegno,
							 wal_segment_size);
				ereport(ERROR,
						(errmsg("could not find WAL file \"%s\"", nextfname)));
			}
		}
		if (segno != endsegno)
		{
			char		endfname[MAXFNAMELEN];

			XLogFileName(endfname, ThisTimeLineID, endsegno, wal_segment_size);
			ereport(ERROR,
					(errmsg("could not find WAL file \"%s\"", endfname)));
		}

		/* Ok, we have everything we need. Send the WAL files. */
		foreach(lc, walFileList)
		{
			char	   *walFileName = (char *) lfirst(lc);
			FILE	   *fp;
			char		buf[TAR_SEND_SIZE];
			size_t		cnt;
			pgoff_t		len = 0;

			snprintf(pathbuf, MAXPGPATH, XLOGDIR "/%s", walFileName);
			XLogFromFileName(walFileName, &tli, &segno, wal_segment_size);

			fp = AllocateFile(pathbuf, "rb");
			if (fp == NULL)
			{
				int			save_errno = errno;

				/*
				 * Most likely reason for this is that the file was already
				 * removed by a checkpoint, so check for that to get a better
				 * error message.
				 */
				CheckXLogRemoved(segno, tli);

				errno = save_errno;
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", pathbuf)));
			}

			if (fstat(fileno(fp), &statbuf) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m",
								pathbuf)));
			if (statbuf.st_size != wal_segment_size)
			{
				CheckXLogRemoved(segno, tli);
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("unexpected WAL file size \"%s\"", walFileName)));
			}

			/* send the WAL file itself */
			_tarWriteHeader(pathbuf, NULL, &statbuf, false);

			while ((cnt = fread(buf, 1,
								Min(sizeof(buf), wal_segment_size - len),
								fp)) > 0)
			{
				CheckXLogRemoved(segno, tli);
				/* Send the chunk as a CopyData message */
				if (pq_putmessage('d', buf, cnt))
					ereport(ERROR,
							(errmsg("base backup could not send data, aborting backup")));
				update_basebackup_progress(cnt);

				len += cnt;
				throttle(cnt);

				if (len == wal_segment_size)
					break;
			}

			CHECK_FREAD_ERROR(fp, pathbuf);

			if (len != wal_segment_size)
			{
				CheckXLogRemoved(segno, tli);
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("unexpected WAL file size \"%s\"", walFileName)));
			}

			/* wal_segment_size is a multiple of 512, so no need for padding */

			FreeFile(fp);

			/*
			 * Mark file as archived, otherwise files can get archived again
			 * after promotion of a new node. This is in line with
			 * walreceiver.c always doing an XLogArchiveForceDone() after a
			 * complete segment.
			 */
			StatusFilePath(pathbuf, walFileName, ".done");
			sendFileWithContent(pathbuf, "", &manifest);
		}

		/*
		 * Send timeline history files too. Only the latest timeline history
		 * file is required for recovery, and even that only if there happens
		 * to be a timeline switch in the first WAL segment that contains the
		 * checkpoint record, or if we're taking a base backup from a standby
		 * server and the target timeline changes while the backup is taken.
		 * But they are small and highly useful for debugging purposes, so
		 * better include them all, always.
		 */
		foreach(lc, historyFileList)
		{
			char	   *fname = lfirst(lc);

			snprintf(pathbuf, MAXPGPATH, XLOGDIR "/%s", fname);

			if (lstat(pathbuf, &statbuf) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", pathbuf)));

			sendFile(pathbuf, pathbuf, &statbuf, false, InvalidOid,
					 &manifest, NULL);

			/* unconditionally mark file as archived */
			StatusFilePath(pathbuf, fname, ".done");
			sendFileWithContent(pathbuf, "", &manifest);
		}

		/* Send CopyDone message for the last tar file */
		pq_putemptymessage('c');
	}

	AddWALInfoToBackupManifest(&manifest, startptr, starttli, endptr, endtli);

	SendBackupManifest(&manifest);

	SendXlogRecPtrResult(endptr, endtli);

	if (total_checksum_failures)
	{
		if (total_checksum_failures > 1)
			ereport(WARNING,
					(errmsg_plural("%lld total checksum verification failure",
								   "%lld total checksum verification failures",
								   total_checksum_failures,
								   total_checksum_failures)));

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("checksum verification failure during base backup")));
	}

	/* clean up the resource owner we created */
	WalSndResourceCleanup(true);

	pgstat_progress_end_command();
}

/*
 * list_sort comparison function, to compare log/seg portion of WAL segment
 * filenames, ignoring the timeline portion.
 */
static int
compareWalFileNames(const ListCell *a, const ListCell *b)
{
	char	   *fna = (char *) lfirst(a);
	char	   *fnb = (char *) lfirst(b);

	return strcmp(fna + 8, fnb + 8);
}

/*
 * Parse the base backup options passed down by the parser
 */
static void
parse_basebackup_options(List *options, basebackup_options *opt)
{
	ListCell   *lopt;
	bool		o_label = false;
	bool		o_progress = false;
	bool		o_fast = false;
	bool		o_nowait = false;
	bool		o_wal = false;
	bool		o_maxrate = false;
	bool		o_tablespace_map = false;
	bool		o_noverify_checksums = false;
	bool		o_manifest = false;
	bool		o_manifest_checksums = false;

	MemSet(opt, 0, sizeof(*opt));
	opt->manifest = MANIFEST_OPTION_NO;
	opt->manifest_checksum_type = CHECKSUM_TYPE_CRC32C;

	foreach(lopt, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lopt);

		if (strcmp(defel->defname, "label") == 0)
		{
			if (o_label)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->label = strVal(defel->arg);
			o_label = true;
		}
		else if (strcmp(defel->defname, "progress") == 0)
		{
			if (o_progress)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->progress = true;
			o_progress = true;
		}
		else if (strcmp(defel->defname, "fast") == 0)
		{
			if (o_fast)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->fastcheckpoint = true;
			o_fast = true;
		}
		else if (strcmp(defel->defname, "nowait") == 0)
		{
			if (o_nowait)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->nowait = true;
			o_nowait = true;
		}
		else if (strcmp(defel->defname, "wal") == 0)
		{
			if (o_wal)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->includewal = true;
			o_wal = true;
		}
		else if (strcmp(defel->defname, "max_rate") == 0)
		{
			long		maxrate;

			if (o_maxrate)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));

			maxrate = intVal(defel->arg);
			if (maxrate < MAX_RATE_LOWER || maxrate > MAX_RATE_UPPER)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("%d is outside the valid range for parameter \"%s\" (%d .. %d)",
								(int) maxrate, "MAX_RATE", MAX_RATE_LOWER, MAX_RATE_UPPER)));

			opt->maxrate = (uint32) maxrate;
			o_maxrate = true;
		}
		else if (strcmp(defel->defname, "tablespace_map") == 0)
		{
			if (o_tablespace_map)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->sendtblspcmapfile = true;
			o_tablespace_map = true;
		}
		else if (strcmp(defel->defname, "noverify_checksums") == 0)
		{
			if (o_noverify_checksums)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			noverify_checksums = true;
			o_noverify_checksums = true;
		}
		else if (strcmp(defel->defname, "manifest") == 0)
		{
			char	   *optval = strVal(defel->arg);
			bool		manifest_bool;

			if (o_manifest)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (parse_bool(optval, &manifest_bool))
			{
				if (manifest_bool)
					opt->manifest = MANIFEST_OPTION_YES;
				else
					opt->manifest = MANIFEST_OPTION_NO;
			}
			else if (pg_strcasecmp(optval, "force-encode") == 0)
				opt->manifest = MANIFEST_OPTION_FORCE_ENCODE;
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized manifest option: \"%s\"",
								optval)));
			o_manifest = true;
		}
		else if (strcmp(defel->defname, "manifest_checksums") == 0)
		{
			char	   *optval = strVal(defel->arg);

			if (o_manifest_checksums)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (!pg_checksum_parse_type(optval,
										&opt->manifest_checksum_type))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized checksum algorithm: \"%s\"",
								optval)));
			o_manifest_checksums = true;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}
	if (opt->label == NULL)
		opt->label = "base backup";
	if (opt->manifest == MANIFEST_OPTION_NO)
	{
		if (o_manifest_checksums)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("manifest checksums require a backup manifest")));
		opt->manifest_checksum_type = CHECKSUM_TYPE_NONE;
	}
}


/*
 * SendBaseBackup() - send a complete base backup.
 *
 * The function will put the system into backup mode like pg_start_backup()
 * does, so that the backup is consistent even though we read directly from
 * the filesystem, bypassing the buffer cache.
 */
void
SendBaseBackup(BaseBackupCmd *cmd)
{
	basebackup_options opt;
	SessionBackupState status = get_backup_status();

	if (status == SESSION_BACKUP_NON_EXCLUSIVE)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a backup is already in progress in this session")));

	parse_basebackup_options(cmd->options, &opt);

	WalSndSetState(WALSNDSTATE_BACKUP);

	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "sending backup \"%s\"",
				 opt.label);
		set_ps_display(activitymsg);
	}

	perform_base_backup(&opt);
}

static void
send_int8_string(StringInfoData *buf, int64 intval)
{
	char		is[32];

	sprintf(is, INT64_FORMAT, intval);
	pq_sendint32(buf, strlen(is));
	pq_sendbytes(buf, is, strlen(is));
}

static void
SendBackupHeader(List *tablespaces)
{
	StringInfoData buf;
	ListCell   *lc;

	/* Construct and send the directory information */
	pq_beginmessage(&buf, 'T'); /* RowDescription */
	pq_sendint16(&buf, 3);		/* 3 fields */

	/* First field - spcoid */
	pq_sendstring(&buf, "spcoid");
	pq_sendint32(&buf, 0);		/* table oid */
	pq_sendint16(&buf, 0);		/* attnum */
	pq_sendint32(&buf, OIDOID); /* type oid */
	pq_sendint16(&buf, 4);		/* typlen */
	pq_sendint32(&buf, 0);		/* typmod */
	pq_sendint16(&buf, 0);		/* format code */

	/* Second field - spclocation */
	pq_sendstring(&buf, "spclocation");
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_sendint32(&buf, TEXTOID);
	pq_sendint16(&buf, -1);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);

	/* Third field - size */
	pq_sendstring(&buf, "size");
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_sendint32(&buf, INT8OID);
	pq_sendint16(&buf, 8);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage(&buf);

	foreach(lc, tablespaces)
	{
		tablespaceinfo *ti = lfirst(lc);

		/* Send one datarow message */
		pq_beginmessage(&buf, 'D');
		pq_sendint16(&buf, 3);	/* number of columns */
		if (ti->path == NULL)
		{
			pq_sendint32(&buf, -1); /* Length = -1 ==> NULL */
			pq_sendint32(&buf, -1);
		}
		else
		{
			Size		len;

			len = strlen(ti->oid);
			pq_sendint32(&buf, len);
			pq_sendbytes(&buf, ti->oid, len);

			len = strlen(ti->path);
			pq_sendint32(&buf, len);
			pq_sendbytes(&buf, ti->path, len);
		}
		if (ti->size >= 0)
			send_int8_string(&buf, ti->size / 1024);
		else
			pq_sendint32(&buf, -1); /* NULL */

		pq_endmessage(&buf);
	}

	/* Send a CommandComplete message */
	pq_puttextmessage('C', "SELECT");
}

/*
 * Send a single resultset containing just a single
 * XLogRecPtr record (in text format)
 */
static void
SendXlogRecPtrResult(XLogRecPtr ptr, TimeLineID tli)
{
	StringInfoData buf;
	char		str[MAXFNAMELEN];
	Size		len;

	pq_beginmessage(&buf, 'T'); /* RowDescription */
	pq_sendint16(&buf, 2);		/* 2 fields */

	/* Field headers */
	pq_sendstring(&buf, "recptr");
	pq_sendint32(&buf, 0);		/* table oid */
	pq_sendint16(&buf, 0);		/* attnum */
	pq_sendint32(&buf, TEXTOID);	/* type oid */
	pq_sendint16(&buf, -1);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);

	pq_sendstring(&buf, "tli");
	pq_sendint32(&buf, 0);		/* table oid */
	pq_sendint16(&buf, 0);		/* attnum */

	/*
	 * int8 may seem like a surprising data type for this, but in theory int4
	 * would not be wide enough for this, as TimeLineID is unsigned.
	 */
	pq_sendint32(&buf, INT8OID);	/* type oid */
	pq_sendint16(&buf, -1);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage(&buf);

	/* Data row */
	pq_beginmessage(&buf, 'D');
	pq_sendint16(&buf, 2);		/* number of columns */

	len = snprintf(str, sizeof(str),
				   "%X/%X", (uint32) (ptr >> 32), (uint32) ptr);
	pq_sendint32(&buf, len);
	pq_sendbytes(&buf, str, len);

	len = snprintf(str, sizeof(str), "%u", tli);
	pq_sendint32(&buf, len);
	pq_sendbytes(&buf, str, len);

	pq_endmessage(&buf);

	/* Send a CommandComplete message */
	pq_puttextmessage('C', "SELECT");
}

/*
 * Inject a file with given name and content in the output tar stream.
 */
static void
sendFileWithContent(const char *filename, const char *content,
					backup_manifest_info *manifest)
{
	struct stat statbuf;
	int			pad,
				len;
	pg_checksum_context checksum_ctx;

	pg_checksum_init(&checksum_ctx, manifest->checksum_type);

	len = strlen(content);

	/*
	 * Construct a stat struct for the backup_label file we're injecting in
	 * the tar.
	 */
	/* Windows doesn't have the concept of uid and gid */
#ifdef WIN32
	statbuf.st_uid = 0;
	statbuf.st_gid = 0;
#else
	statbuf.st_uid = geteuid();
	statbuf.st_gid = getegid();
#endif
	statbuf.st_mtime = time(NULL);
	statbuf.st_mode = pg_file_create_mode;
	statbuf.st_size = len;

	_tarWriteHeader(filename, NULL, &statbuf, false);
	/* Send the contents as a CopyData message */
	pq_putmessage('d', content, len);
	update_basebackup_progress(len);

	/* Pad to 512 byte boundary, per tar format requirements */
	pad = ((len + 511) & ~511) - len;
	if (pad > 0)
	{
		char		buf[512];

		MemSet(buf, 0, pad);
		pq_putmessage('d', buf, pad);
		update_basebackup_progress(pad);
	}

	pg_checksum_update(&checksum_ctx, (uint8 *) content, len);
	AddFileToBackupManifest(manifest, NULL, filename, len,
							(pg_time_t) statbuf.st_mtime, &checksum_ctx);
}

/*
 * Include the tablespace directory pointed to by 'path' in the output tar
 * stream.  If 'sizeonly' is true, we just calculate a total length and return
 * it, without actually sending anything.
 *
 * Only used to send auxiliary tablespaces, not PGDATA.
 */
int64
sendTablespace(char *path, char *spcoid, bool sizeonly,
			   backup_manifest_info *manifest)
{
	int64		size;
	char		pathbuf[MAXPGPATH];
	struct stat statbuf;

	/*
	 * 'path' points to the tablespace location, but we only want to include
	 * the version directory in it that belongs to us.
	 */
	snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path,
			 TABLESPACE_VERSION_DIRECTORY);

	/*
	 * Store a directory entry in the tar file so we get the permissions
	 * right.
	 */
	if (lstat(pathbuf, &statbuf) != 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file or directory \"%s\": %m",
							pathbuf)));

		/* If the tablespace went away while scanning, it's no error. */
		return 0;
	}

	size = _tarWriteHeader(TABLESPACE_VERSION_DIRECTORY, NULL, &statbuf,
						   sizeonly);

	/* Send all the files in the tablespace version directory */
	size += sendDir(pathbuf, strlen(path), sizeonly, NIL, true, manifest,
					spcoid);

	return size;
}

/*
 * Include all files from the given directory in the output tar stream. If
 * 'sizeonly' is true, we just calculate a total length and return it, without
 * actually sending anything.
 *
 * Omit any directory in the tablespaces list, to avoid backing up
 * tablespaces twice when they were created inside PGDATA.
 *
 * If sendtblspclinks is true, we need to include symlink
 * information in the tar file. If not, we can skip that
 * as it will be sent separately in the tablespace_map file.
 */
static int64
sendDir(const char *path, int basepathlen, bool sizeonly, List *tablespaces,
		bool sendtblspclinks, backup_manifest_info *manifest,
		const char *spcoid)
{
	DIR		   *dir;
	struct dirent *de;
	char		pathbuf[MAXPGPATH * 2];
	struct stat statbuf;
	int64		size = 0;
	const char *lastDir;		/* Split last dir from parent path. */
	bool		isDbDir = false;	/* Does this directory contain relations? */

	/*
	 * Determine if the current path is a database directory that can contain
	 * relations.
	 *
	 * Start by finding the location of the delimiter between the parent path
	 * and the current path.
	 */
	lastDir = last_dir_separator(path);

	/* Does this path look like a database path (i.e. all digits)? */
	if (lastDir != NULL &&
		strspn(lastDir + 1, "0123456789") == strlen(lastDir + 1))
	{
		/* Part of path that contains the parent directory. */
		int			parentPathLen = lastDir - path;

		/*
		 * Mark path as a database directory if the parent path is either
		 * $PGDATA/base or a tablespace version path.
		 */
		if (strncmp(path, "./base", parentPathLen) == 0 ||
			(parentPathLen >= (sizeof(TABLESPACE_VERSION_DIRECTORY) - 1) &&
			 strncmp(lastDir - (sizeof(TABLESPACE_VERSION_DIRECTORY) - 1),
					 TABLESPACE_VERSION_DIRECTORY,
					 sizeof(TABLESPACE_VERSION_DIRECTORY) - 1) == 0))
			isDbDir = true;
	}

	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		int			excludeIdx;
		bool		excludeFound;
		ForkNumber	relForkNum; /* Type of fork if file is a relation */
		int			relOidChars;	/* Chars in filename that are the rel oid */

		/* Skip special stuff */
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		/* Skip temporary files */
		if (strncmp(de->d_name,
					PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0)
			continue;

		/*
		 * Check if the postmaster has signaled us to exit, and abort with an
		 * error in that case. The error handler further up will call
		 * do_pg_abort_backup() for us. Also check that if the backup was
		 * started while still in recovery, the server wasn't promoted.
		 * do_pg_stop_backup() will check that too, but it's better to stop
		 * the backup early than continue to the end and fail there.
		 */
		CHECK_FOR_INTERRUPTS();
		if (RecoveryInProgress() != backup_started_in_recovery)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the standby was promoted during online backup"),
					 errhint("This means that the backup being taken is corrupt "
							 "and should not be used. "
							 "Try taking another online backup.")));

		/* Scan for files that should be excluded */
		excludeFound = false;
		for (excludeIdx = 0; excludeFiles[excludeIdx].name != NULL; excludeIdx++)
		{
			int			cmplen = strlen(excludeFiles[excludeIdx].name);

			if (!excludeFiles[excludeIdx].match_prefix)
				cmplen++;
			if (strncmp(de->d_name, excludeFiles[excludeIdx].name, cmplen) == 0)
			{
				elog(DEBUG1, "file \"%s\" excluded from backup", de->d_name);
				excludeFound = true;
				break;
			}
		}

		if (excludeFound)
			continue;

		/* Exclude all forks for unlogged tables except the init fork */
		if (isDbDir &&
			parse_filename_for_nontemp_relation(de->d_name, &relOidChars,
												&relForkNum))
		{
			/* Never exclude init forks */
			if (relForkNum != INIT_FORKNUM)
			{
				char		initForkFile[MAXPGPATH];
				char		relOid[OIDCHARS + 1];

				/*
				 * If any other type of fork, check if there is an init fork
				 * with the same OID. If so, the file can be excluded.
				 */
				memcpy(relOid, de->d_name, relOidChars);
				relOid[relOidChars] = '\0';
				snprintf(initForkFile, sizeof(initForkFile), "%s/%s_init",
						 path, relOid);

				if (lstat(initForkFile, &statbuf) == 0)
				{
					elog(DEBUG2,
						 "unlogged relation file \"%s\" excluded from backup",
						 de->d_name);

					continue;
				}
			}
		}

		/* Exclude temporary relations */
		if (isDbDir && looks_like_temp_rel_name(de->d_name))
		{
			elog(DEBUG2,
				 "temporary relation file \"%s\" excluded from backup",
				 de->d_name);

			continue;
		}

		snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, de->d_name);

		/* Skip pg_control here to back up it last */
		if (strcmp(pathbuf, "./global/pg_control") == 0)
			continue;

		if (lstat(pathbuf, &statbuf) != 0)
		{
			if (errno != ENOENT)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file or directory \"%s\": %m",
								pathbuf)));

			/* If the file went away while scanning, it's not an error. */
			continue;
		}

		/* Scan for directories whose contents should be excluded */
		excludeFound = false;
		for (excludeIdx = 0; excludeDirContents[excludeIdx] != NULL; excludeIdx++)
		{
			if (strcmp(de->d_name, excludeDirContents[excludeIdx]) == 0)
			{
				elog(DEBUG1, "contents of directory \"%s\" excluded from backup", de->d_name);
				size += _tarWriteDir(pathbuf, basepathlen, &statbuf, sizeonly);
				excludeFound = true;
				break;
			}
		}

		if (excludeFound)
			continue;

		/*
		 * Exclude contents of directory specified by statrelpath if not set
		 * to the default (pg_stat_tmp) which is caught in the loop above.
		 */
		if (statrelpath != NULL && strcmp(pathbuf, statrelpath) == 0)
		{
			elog(DEBUG1, "contents of directory \"%s\" excluded from backup", statrelpath);
			size += _tarWriteDir(pathbuf, basepathlen, &statbuf, sizeonly);
			continue;
		}

		/*
		 * We can skip pg_wal, the WAL segments need to be fetched from the
		 * WAL archive anyway. But include it as an empty directory anyway, so
		 * we get permissions right.
		 */
		if (strcmp(pathbuf, "./pg_wal") == 0)
		{
			/* If pg_wal is a symlink, write it as a directory anyway */
			size += _tarWriteDir(pathbuf, basepathlen, &statbuf, sizeonly);

			/*
			 * Also send archive_status directory (by hackishly reusing
			 * statbuf from above ...).
			 */
			size += _tarWriteHeader("./pg_wal/archive_status", NULL, &statbuf,
									sizeonly);

			continue;			/* don't recurse into pg_wal */
		}

		/* Allow symbolic links in pg_tblspc only */
		if (strcmp(path, "./pg_tblspc") == 0 &&
#ifndef WIN32
			S_ISLNK(statbuf.st_mode)
#else
			pgwin32_is_junction(pathbuf)
#endif
			)
		{
#if defined(HAVE_READLINK) || defined(WIN32)
			char		linkpath[MAXPGPATH];
			int			rllen;

			rllen = readlink(pathbuf, linkpath, sizeof(linkpath));
			if (rllen < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read symbolic link \"%s\": %m",
								pathbuf)));
			if (rllen >= sizeof(linkpath))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("symbolic link \"%s\" target is too long",
								pathbuf)));
			linkpath[rllen] = '\0';

			size += _tarWriteHeader(pathbuf + basepathlen + 1, linkpath,
									&statbuf, sizeonly);
#else

			/*
			 * If the platform does not have symbolic links, it should not be
			 * possible to have tablespaces - clearly somebody else created
			 * them. Warn about it and ignore.
			 */
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("tablespaces are not supported on this platform")));
			continue;
#endif							/* HAVE_READLINK */
		}
		else if (S_ISDIR(statbuf.st_mode))
		{
			bool		skip_this_dir = false;
			ListCell   *lc;

			/*
			 * Store a directory entry in the tar file so we can get the
			 * permissions right.
			 */
			size += _tarWriteHeader(pathbuf + basepathlen + 1, NULL, &statbuf,
									sizeonly);

			/*
			 * Call ourselves recursively for a directory, unless it happens
			 * to be a separate tablespace located within PGDATA.
			 */
			foreach(lc, tablespaces)
			{
				tablespaceinfo *ti = (tablespaceinfo *) lfirst(lc);

				/*
				 * ti->rpath is the tablespace relative path within PGDATA, or
				 * NULL if the tablespace has been properly located somewhere
				 * else.
				 *
				 * Skip past the leading "./" in pathbuf when comparing.
				 */
				if (ti->rpath && strcmp(ti->rpath, pathbuf + 2) == 0)
				{
					skip_this_dir = true;
					break;
				}
			}

			/*
			 * skip sending directories inside pg_tblspc, if not required.
			 */
			if (strcmp(pathbuf, "./pg_tblspc") == 0 && !sendtblspclinks)
				skip_this_dir = true;

			if (!skip_this_dir)
				size += sendDir(pathbuf, basepathlen, sizeonly, tablespaces,
								sendtblspclinks, manifest, spcoid);
		}
		else if (S_ISREG(statbuf.st_mode))
		{
			bool		sent = false;

			if (!sizeonly)
				sent = sendFile(pathbuf, pathbuf + basepathlen + 1, &statbuf,
								true, isDbDir ? atooid(lastDir + 1) : InvalidOid,
								manifest, spcoid);

			if (sent || sizeonly)
			{
				/* Add size, rounded up to 512byte block */
				size += ((statbuf.st_size + 511) & ~511);
				size += 512;	/* Size of the header of the file */
			}
		}
		else
			ereport(WARNING,
					(errmsg("skipping special file \"%s\"", pathbuf)));
	}
	FreeDir(dir);
	return size;
}

/*
 * Check if a file should have its checksum validated.
 * We validate checksums on files in regular tablespaces
 * (including global and default) only, and in those there
 * are some files that are explicitly excluded.
 */
static bool
is_checksummed_file(const char *fullpath, const char *filename)
{
	/* Check that the file is in a tablespace */
	if (strncmp(fullpath, "./global/", 9) == 0 ||
		strncmp(fullpath, "./base/", 7) == 0 ||
		strncmp(fullpath, "/", 1) == 0)
	{
		int			excludeIdx;

		/* Compare file against noChecksumFiles skip list */
		for (excludeIdx = 0; noChecksumFiles[excludeIdx].name != NULL; excludeIdx++)
		{
			int			cmplen = strlen(noChecksumFiles[excludeIdx].name);

			if (!noChecksumFiles[excludeIdx].match_prefix)
				cmplen++;
			if (strncmp(filename, noChecksumFiles[excludeIdx].name,
						cmplen) == 0)
				return false;
		}

		return true;
	}
	else
		return false;
}

/*****
 * Functions for handling tar file format
 *
 * Copied from pg_dump, but modified to work with libpq for sending
 */


/*
 * Given the member, write the TAR header & send the file.
 *
 * If 'missing_ok' is true, will not throw an error if the file is not found.
 *
 * If dboid is anything other than InvalidOid then any checksum failures detected
 * will get reported to the stats collector.
 *
 * Returns true if the file was successfully sent, false if 'missing_ok',
 * and the file did not exist.
 */
static bool
sendFile(const char *readfilename, const char *tarfilename,
		 struct stat *statbuf, bool missing_ok, Oid dboid,
		 backup_manifest_info *manifest, const char *spcoid)
{
	FILE	   *fp;
	BlockNumber blkno = 0;
	bool		block_retry = false;
	char		buf[TAR_SEND_SIZE];
	uint16		checksum;
	int			checksum_failures = 0;
	off_t		cnt;
	int			i;
	pgoff_t		len = 0;
	char	   *page;
	size_t		pad;
	PageHeader	phdr;
	int			segmentno = 0;
	char	   *segmentpath;
	bool		verify_checksum = false;
	pg_checksum_context checksum_ctx;

	pg_checksum_init(&checksum_ctx, manifest->checksum_type);

	fp = AllocateFile(readfilename, "rb");
	if (fp == NULL)
	{
		if (errno == ENOENT && missing_ok)
			return false;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", readfilename)));
	}

	_tarWriteHeader(tarfilename, NULL, statbuf, false);

	if (!noverify_checksums && DataChecksumsEnabled())
	{
		char	   *filename;

		/*
		 * Get the filename (excluding path).  As last_dir_separator()
		 * includes the last directory separator, we chop that off by
		 * incrementing the pointer.
		 */
		filename = last_dir_separator(readfilename) + 1;

		if (is_checksummed_file(readfilename, filename))
		{
			verify_checksum = true;

			/*
			 * Cut off at the segment boundary (".") to get the segment number
			 * in order to mix it into the checksum.
			 */
			segmentpath = strstr(filename, ".");
			if (segmentpath != NULL)
			{
				segmentno = atoi(segmentpath + 1);
				if (segmentno == 0)
					ereport(ERROR,
							(errmsg("invalid segment number %d in file \"%s\"",
									segmentno, filename)));
			}
		}
	}

	while ((cnt = fread(buf, 1, Min(sizeof(buf), statbuf->st_size - len), fp)) > 0)
	{
		/*
		 * The checksums are verified at block level, so we iterate over the
		 * buffer in chunks of BLCKSZ, after making sure that
		 * TAR_SEND_SIZE/buf is divisible by BLCKSZ and we read a multiple of
		 * BLCKSZ bytes.
		 */
		Assert(TAR_SEND_SIZE % BLCKSZ == 0);

		if (verify_checksum && (cnt % BLCKSZ != 0))
		{
			ereport(WARNING,
					(errmsg("could not verify checksum in file \"%s\", block "
							"%d: read buffer size %d and page size %d "
							"differ",
							readfilename, blkno, (int) cnt, BLCKSZ)));
			verify_checksum = false;
		}

		if (verify_checksum)
		{
			for (i = 0; i < cnt / BLCKSZ; i++)
			{
				page = buf + BLCKSZ * i;

				/*
				 * Only check pages which have not been modified since the
				 * start of the base backup. Otherwise, they might have been
				 * written only halfway and the checksum would not be valid.
				 * However, replaying WAL would reinstate the correct page in
				 * this case. We also skip completely new pages, since they
				 * don't have a checksum yet.
				 */
				if (!PageIsNew(page) && PageGetLSN(page) < startptr)
				{
					checksum = pg_checksum_page((char *) page, blkno + segmentno * RELSEG_SIZE);
					phdr = (PageHeader) page;
					if (phdr->pd_checksum != checksum)
					{
						/*
						 * Retry the block on the first failure.  It's
						 * possible that we read the first 4K page of the
						 * block just before postgres updated the entire block
						 * so it ends up looking torn to us.  We only need to
						 * retry once because the LSN should be updated to
						 * something we can ignore on the next pass.  If the
						 * error happens again then it is a true validation
						 * failure.
						 */
						if (block_retry == false)
						{
							/* Reread the failed block */
							if (fseek(fp, -(cnt - BLCKSZ * i), SEEK_CUR) == -1)
							{
								ereport(ERROR,
										(errcode_for_file_access(),
										 errmsg("could not fseek in file \"%s\": %m",
												readfilename)));
							}

							if (fread(buf + BLCKSZ * i, 1, BLCKSZ, fp) != BLCKSZ)
							{
								/*
								 * If we hit end-of-file, a concurrent
								 * truncation must have occurred, so break out
								 * of this loop just as if the initial fread()
								 * returned 0. We'll drop through to the same
								 * code that handles that case. (We must fix
								 * up cnt first, though.)
								 */
								if (feof(fp))
								{
									cnt = BLCKSZ * i;
									break;
								}

								ereport(ERROR,
										(errcode_for_file_access(),
										 errmsg("could not reread block %d of file \"%s\": %m",
												blkno, readfilename)));
							}

							if (fseek(fp, cnt - BLCKSZ * i - BLCKSZ, SEEK_CUR) == -1)
							{
								ereport(ERROR,
										(errcode_for_file_access(),
										 errmsg("could not fseek in file \"%s\": %m",
												readfilename)));
							}

							/* Set flag so we know a retry was attempted */
							block_retry = true;

							/* Reset loop to validate the block again */
							i--;
							continue;
						}

						checksum_failures++;

						if (checksum_failures <= 5)
							ereport(WARNING,
									(errmsg("checksum verification failed in "
											"file \"%s\", block %d: calculated "
											"%X but expected %X",
											readfilename, blkno, checksum,
											phdr->pd_checksum)));
						if (checksum_failures == 5)
							ereport(WARNING,
									(errmsg("further checksum verification "
											"failures in file \"%s\" will not "
											"be reported", readfilename)));
					}
				}
				block_retry = false;
				blkno++;
			}
		}

		/* Send the chunk as a CopyData message */
		if (pq_putmessage('d', buf, cnt))
			ereport(ERROR,
					(errmsg("base backup could not send data, aborting backup")));
		update_basebackup_progress(cnt);

		/* Also feed it to the checksum machinery. */
		pg_checksum_update(&checksum_ctx, (uint8 *) buf, cnt);

		len += cnt;
		throttle(cnt);

		if (feof(fp) || len >= statbuf->st_size)
		{
			/*
			 * Reached end of file. The file could be longer, if it was
			 * extended while we were sending it, but for a base backup we can
			 * ignore such extended data. It will be restored from WAL.
			 */
			break;
		}
	}

	CHECK_FREAD_ERROR(fp, readfilename);

	/* If the file was truncated while we were sending it, pad it with zeros */
	if (len < statbuf->st_size)
	{
		MemSet(buf, 0, sizeof(buf));
		while (len < statbuf->st_size)
		{
			cnt = Min(sizeof(buf), statbuf->st_size - len);
			pq_putmessage('d', buf, cnt);
			pg_checksum_update(&checksum_ctx, (uint8 *) buf, cnt);
			update_basebackup_progress(cnt);
			len += cnt;
			throttle(cnt);
		}
	}

	/*
	 * Pad to 512 byte boundary, per tar format requirements. (This small
	 * piece of data is probably not worth throttling, and is not checksummed
	 * because it's not actually part of the file.)
	 */
	pad = ((len + 511) & ~511) - len;
	if (pad > 0)
	{
		MemSet(buf, 0, pad);
		pq_putmessage('d', buf, pad);
		update_basebackup_progress(pad);
	}

	FreeFile(fp);

	if (checksum_failures > 1)
	{
		ereport(WARNING,
				(errmsg_plural("file \"%s\" has a total of %d checksum verification failure",
							   "file \"%s\" has a total of %d checksum verification failures",
							   checksum_failures,
							   readfilename, checksum_failures)));

		pgstat_report_checksum_failures_in_db(dboid, checksum_failures);
	}

	total_checksum_failures += checksum_failures;

	AddFileToBackupManifest(manifest, spcoid, tarfilename, statbuf->st_size,
							(pg_time_t) statbuf->st_mtime, &checksum_ctx);

	return true;
}


static int64
_tarWriteHeader(const char *filename, const char *linktarget,
				struct stat *statbuf, bool sizeonly)
{
	char		h[512];
	enum tarError rc;

	if (!sizeonly)
	{
		rc = tarCreateHeader(h, filename, linktarget, statbuf->st_size,
							 statbuf->st_mode, statbuf->st_uid, statbuf->st_gid,
							 statbuf->st_mtime);

		switch (rc)
		{
			case TAR_OK:
				break;
			case TAR_NAME_TOO_LONG:
				ereport(ERROR,
						(errmsg("file name too long for tar format: \"%s\"",
								filename)));
				break;
			case TAR_SYMLINK_TOO_LONG:
				ereport(ERROR,
						(errmsg("symbolic link target too long for tar format: "
								"file name \"%s\", target \"%s\"",
								filename, linktarget)));
				break;
			default:
				elog(ERROR, "unrecognized tar error: %d", rc);
		}

		pq_putmessage('d', h, sizeof(h));
		update_basebackup_progress(sizeof(h));
	}

	return sizeof(h);
}

/*
 * Write tar header for a directory.  If the entry in statbuf is a link then
 * write it as a directory anyway.
 */
static int64
_tarWriteDir(const char *pathbuf, int basepathlen, struct stat *statbuf,
			 bool sizeonly)
{
	/* If symlink, write it as a directory anyway */
#ifndef WIN32
	if (S_ISLNK(statbuf->st_mode))
#else
	if (pgwin32_is_junction(pathbuf))
#endif
		statbuf->st_mode = S_IFDIR | pg_dir_create_mode;

	return _tarWriteHeader(pathbuf + basepathlen + 1, NULL, statbuf, sizeonly);
}

/*
 * Increment the network transfer counter by the given number of bytes,
 * and sleep if necessary to comply with the requested network transfer
 * rate.
 */
static void
throttle(size_t increment)
{
	TimeOffset	elapsed_min;

	if (throttling_counter < 0)
		return;

	throttling_counter += increment;
	if (throttling_counter < throttling_sample)
		return;

	/* How much time should have elapsed at minimum? */
	elapsed_min = elapsed_min_unit *
		(throttling_counter / throttling_sample);

	/*
	 * Since the latch could be set repeatedly because of concurrently WAL
	 * activity, sleep in a loop to ensure enough time has passed.
	 */
	for (;;)
	{
		TimeOffset	elapsed,
					sleep;
		int			wait_result;

		/* Time elapsed since the last measurement (and possible wake up). */
		elapsed = GetCurrentTimestamp() - throttled_last;

		/* sleep if the transfer is faster than it should be */
		sleep = elapsed_min - elapsed;
		if (sleep <= 0)
			break;

		ResetLatch(MyLatch);

		/* We're eating a potentially set latch, so check for interrupts */
		CHECK_FOR_INTERRUPTS();

		/*
		 * (TAR_SEND_SIZE / throttling_sample * elapsed_min_unit) should be
		 * the maximum time to sleep. Thus the cast to long is safe.
		 */
		wait_result = WaitLatch(MyLatch,
								WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								(long) (sleep / 1000),
								WAIT_EVENT_BASE_BACKUP_THROTTLE);

		if (wait_result & WL_LATCH_SET)
			CHECK_FOR_INTERRUPTS();

		/* Done waiting? */
		if (wait_result & WL_TIMEOUT)
			break;
	}

	/*
	 * As we work with integers, only whole multiple of throttling_sample was
	 * processed. The rest will be done during the next call of this function.
	 */
	throttling_counter %= throttling_sample;

	/*
	 * Time interval for the remaining amount and possible next increments
	 * starts now.
	 */
	throttled_last = GetCurrentTimestamp();
}

/*
 * Increment the counter for the amount of data already streamed
 * by the given number of bytes, and update the progress report for
 * pg_stat_progress_basebackup.
 */
static void
update_basebackup_progress(int64 delta)
{
	const int	index[] = {
		PROGRESS_BASEBACKUP_BACKUP_STREAMED,
		PROGRESS_BASEBACKUP_BACKUP_TOTAL
	};
	int64		val[2];
	int			nparam = 0;

	backup_streamed += delta;
	val[nparam++] = backup_streamed;

	/*
	 * Avoid overflowing past 100% or the full size. This may make the total
	 * size number change as we approach the end of the backup (the estimate
	 * will always be wrong if WAL is included), but that's better than having
	 * the done column be bigger than the total.
	 */
	if (backup_total > -1 && backup_streamed > backup_total)
	{
		backup_total = backup_streamed;
		val[nparam++] = backup_total;
	}

	pgstat_progress_update_multi_param(nparam, index, val);
}
