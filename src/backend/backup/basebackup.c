/*-------------------------------------------------------------------------
 *
 * basebackup.c
 *	  code for taking a base backup and streaming it to a standby
 *
 * Portions Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "access/xlog_internal.h"
#include "backup/backup_manifest.h"
#include "backup/basebackup.h"
#include "backup/basebackup_sink.h"
#include "backup/basebackup_target.h"
#include "commands/defrem.h"
#include "common/compression.h"
#include "common/file_perm.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "pgtar.h"
#include "port.h"
#include "postmaster/syslogger.h"
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

/*
 * How much data do we want to send in one CopyData message? Note that
 * this may also result in reading the underlying files in chunks of this
 * size.
 *
 * NB: The buffer size is required to be a multiple of the system block
 * size, so use that value instead if it's bigger than our preference.
 */
#define SINK_BUFFER_LENGTH			Max(32768, BLCKSZ)

typedef struct
{
	const char *label;
	bool		progress;
	bool		fastcheckpoint;
	bool		nowait;
	bool		includewal;
	uint32		maxrate;
	bool		sendtblspcmapfile;
	bool		send_to_client;
	bool		use_copytblspc;
	BaseBackupTargetHandle *target_handle;
	backup_manifest_option manifest;
	pg_compress_algorithm compression;
	pg_compress_specification compression_specification;
	pg_checksum_type manifest_checksum_type;
} basebackup_options;

static int64 sendTablespace(bbsink *sink, char *path, char *oid, bool sizeonly,
							struct backup_manifest_info *manifest);
static int64 sendDir(bbsink *sink, const char *path, int basepathlen, bool sizeonly,
					 List *tablespaces, bool sendtblspclinks,
					 backup_manifest_info *manifest, const char *spcoid);
static bool sendFile(bbsink *sink, const char *readfilename, const char *tarfilename,
					 struct stat *statbuf, bool missing_ok, Oid dboid,
					 backup_manifest_info *manifest, const char *spcoid);
static void sendFileWithContent(bbsink *sink, const char *filename,
								const char *content,
								backup_manifest_info *manifest);
static int64 _tarWriteHeader(bbsink *sink, const char *filename,
							 const char *linktarget, struct stat *statbuf,
							 bool sizeonly);
static void _tarWritePadding(bbsink *sink, int len);
static void convert_link_to_directory(const char *pathbuf, struct stat *statbuf);
static void perform_base_backup(basebackup_options *opt, bbsink *sink);
static void parse_basebackup_options(List *options, basebackup_options *opt);
static int	compareWalFileNames(const ListCell *a, const ListCell *b);
static bool is_checksummed_file(const char *fullpath, const char *filename);
static int	basebackup_read_file(int fd, char *buf, size_t nbytes, off_t offset,
								 const char *filename, bool partial_read_ok);

/* Was the backup currently in-progress initiated in recovery mode? */
static bool backup_started_in_recovery = false;

/* Total number of checksum failures during base backup. */
static long long int total_checksum_failures;

/* Do not verify checksums. */
static bool noverify_checksums = false;

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
	 * Skip temporary statistics files. PG_STAT_TMP_DIR must be skipped
	 * because extensions like pg_stat_statements store data there.
	 */
	PG_STAT_TMP_DIR,

	/*
	 * It is generally not useful to backup the contents of this directory
	 * even if the intention is to restore to another primary. See backup.sgml
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
	 * backup_label and tablespace_map should not exist in a running cluster
	 * capable of doing an online backup, but exclude them just in case.
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
perform_base_backup(basebackup_options *opt, bbsink *sink)
{
	bbsink_state state;
	XLogRecPtr	endptr;
	TimeLineID	endtli;
	StringInfo	labelfile;
	StringInfo	tblspc_map_file;
	backup_manifest_info manifest;

	/* Initial backup state, insofar as we know it now. */
	state.tablespaces = NIL;
	state.tablespace_num = 0;
	state.bytes_done = 0;
	state.bytes_total = 0;
	state.bytes_total_is_valid = false;

	/* we're going to use a BufFile, so we need a ResourceOwner */
	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "base backup");

	backup_started_in_recovery = RecoveryInProgress();

	labelfile = makeStringInfo();
	tblspc_map_file = makeStringInfo();
	InitializeBackupManifest(&manifest, opt->manifest,
							 opt->manifest_checksum_type);

	total_checksum_failures = 0;

	basebackup_progress_wait_checkpoint();
	state.startptr = do_pg_backup_start(opt->label, opt->fastcheckpoint,
										&state.starttli,
										labelfile, &state.tablespaces,
										tblspc_map_file);

	/*
	 * Once do_pg_backup_start has been called, ensure that any failure causes
	 * us to abort the backup so we don't "leak" a backup counter. For this
	 * reason, *all* functionality between do_pg_backup_start() and the end of
	 * do_pg_backup_stop() should be inside the error cleanup block!
	 */

	PG_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, BoolGetDatum(false));
	{
		ListCell   *lc;
		tablespaceinfo *ti;

		/* Add a node for the base directory at the end */
		ti = palloc0(sizeof(tablespaceinfo));
		ti->size = -1;
		state.tablespaces = lappend(state.tablespaces, ti);

		/*
		 * Calculate the total backup size by summing up the size of each
		 * tablespace
		 */
		if (opt->progress)
		{
			basebackup_progress_estimate_backup_size();

			foreach(lc, state.tablespaces)
			{
				tablespaceinfo *tmp = (tablespaceinfo *) lfirst(lc);

				if (tmp->path == NULL)
					tmp->size = sendDir(sink, ".", 1, true, state.tablespaces,
										true, NULL, NULL);
				else
					tmp->size = sendTablespace(sink, tmp->path, tmp->oid, true,
											   NULL);
				state.bytes_total += tmp->size;
			}
			state.bytes_total_is_valid = true;
		}

		/* notify basebackup sink about start of backup */
		bbsink_begin_backup(sink, &state, SINK_BUFFER_LENGTH);

		/* Send off our tablespaces one by one */
		foreach(lc, state.tablespaces)
		{
			tablespaceinfo *ti = (tablespaceinfo *) lfirst(lc);

			if (ti->path == NULL)
			{
				struct stat statbuf;
				bool		sendtblspclinks = true;

				bbsink_begin_archive(sink, "base.tar");

				/* In the main tar, include the backup_label first... */
				sendFileWithContent(sink, BACKUP_LABEL_FILE, labelfile->data,
									&manifest);

				/* Then the tablespace_map file, if required... */
				if (opt->sendtblspcmapfile)
				{
					sendFileWithContent(sink, TABLESPACE_MAP, tblspc_map_file->data,
										&manifest);
					sendtblspclinks = false;
				}

				/* Then the bulk of the files... */
				sendDir(sink, ".", 1, false, state.tablespaces,
						sendtblspclinks, &manifest, NULL);

				/* ... and pg_control after everything else. */
				if (lstat(XLOG_CONTROL_FILE, &statbuf) != 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not stat file \"%s\": %m",
									XLOG_CONTROL_FILE)));
				sendFile(sink, XLOG_CONTROL_FILE, XLOG_CONTROL_FILE, &statbuf,
						 false, InvalidOid, &manifest, NULL);
			}
			else
			{
				char	   *archive_name = psprintf("%s.tar", ti->oid);

				bbsink_begin_archive(sink, archive_name);

				sendTablespace(sink, ti->path, ti->oid, false, &manifest);
			}

			/*
			 * If we're including WAL, and this is the main data directory we
			 * don't treat this as the end of the tablespace. Instead, we will
			 * include the xlog files below and stop afterwards. This is safe
			 * since the main data directory is always sent *last*.
			 */
			if (opt->includewal && ti->path == NULL)
			{
				Assert(lnext(state.tablespaces, lc) == NULL);
			}
			else
			{
				/* Properly terminate the tarfile. */
				StaticAssertStmt(2 * TAR_BLOCK_SIZE <= BLCKSZ,
								 "BLCKSZ too small for 2 tar blocks");
				memset(sink->bbs_buffer, 0, 2 * TAR_BLOCK_SIZE);
				bbsink_archive_contents(sink, 2 * TAR_BLOCK_SIZE);

				/* OK, that's the end of the archive. */
				bbsink_end_archive(sink);
			}
		}

		basebackup_progress_wait_wal_archive(&state);
		endptr = do_pg_backup_stop(labelfile->data, !opt->nowait, &endtli);
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

		basebackup_progress_transfer_wal();

		/*
		 * I'd rather not worry about timelines here, so scan pg_wal and
		 * include all WAL files in the range between 'startptr' and 'endptr',
		 * regardless of the timeline the file is stamped with. If there are
		 * some spurious WAL files belonging to timelines that don't belong in
		 * this server's history, they will be included too. Normally there
		 * shouldn't be such files, but if there are, there's little harm in
		 * including them.
		 */
		XLByteToSeg(state.startptr, startsegno, wal_segment_size);
		XLogFileName(firstoff, state.starttli, startsegno, wal_segment_size);
		XLByteToPrevSeg(endptr, endsegno, wal_segment_size);
		XLogFileName(lastoff, endtli, endsegno, wal_segment_size);

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
		CheckXLogRemoved(startsegno, state.starttli);

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

			XLogFileName(startfname, state.starttli, startsegno,
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

				XLogFileName(nextfname, tli, nextsegno, wal_segment_size);
				ereport(ERROR,
						(errmsg("could not find WAL file \"%s\"", nextfname)));
			}
		}
		if (segno != endsegno)
		{
			char		endfname[MAXFNAMELEN];

			XLogFileName(endfname, endtli, endsegno, wal_segment_size);
			ereport(ERROR,
					(errmsg("could not find WAL file \"%s\"", endfname)));
		}

		/* Ok, we have everything we need. Send the WAL files. */
		foreach(lc, walFileList)
		{
			char	   *walFileName = (char *) lfirst(lc);
			int			fd;
			size_t		cnt;
			pgoff_t		len = 0;

			snprintf(pathbuf, MAXPGPATH, XLOGDIR "/%s", walFileName);
			XLogFromFileName(walFileName, &tli, &segno, wal_segment_size);

			fd = OpenTransientFile(pathbuf, O_RDONLY | PG_BINARY);
			if (fd < 0)
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

			if (fstat(fd, &statbuf) != 0)
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
			_tarWriteHeader(sink, pathbuf, NULL, &statbuf, false);

			while ((cnt = basebackup_read_file(fd, sink->bbs_buffer,
											   Min(sink->bbs_buffer_length,
												   wal_segment_size - len),
											   len, pathbuf, true)) > 0)
			{
				CheckXLogRemoved(segno, tli);
				bbsink_archive_contents(sink, cnt);

				len += cnt;

				if (len == wal_segment_size)
					break;
			}

			if (len != wal_segment_size)
			{
				CheckXLogRemoved(segno, tli);
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("unexpected WAL file size \"%s\"", walFileName)));
			}

			/*
			 * wal_segment_size is a multiple of TAR_BLOCK_SIZE, so no need
			 * for padding.
			 */
			Assert(wal_segment_size % TAR_BLOCK_SIZE == 0);

			CloseTransientFile(fd);

			/*
			 * Mark file as archived, otherwise files can get archived again
			 * after promotion of a new node. This is in line with
			 * walreceiver.c always doing an XLogArchiveForceDone() after a
			 * complete segment.
			 */
			StatusFilePath(pathbuf, walFileName, ".done");
			sendFileWithContent(sink, pathbuf, "", &manifest);
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

			sendFile(sink, pathbuf, pathbuf, &statbuf, false, InvalidOid,
					 &manifest, NULL);

			/* unconditionally mark file as archived */
			StatusFilePath(pathbuf, fname, ".done");
			sendFileWithContent(sink, pathbuf, "", &manifest);
		}

		/* Properly terminate the tar file. */
		StaticAssertStmt(2 * TAR_BLOCK_SIZE <= BLCKSZ,
						 "BLCKSZ too small for 2 tar blocks");
		memset(sink->bbs_buffer, 0, 2 * TAR_BLOCK_SIZE);
		bbsink_archive_contents(sink, 2 * TAR_BLOCK_SIZE);

		/* OK, that's the end of the archive. */
		bbsink_end_archive(sink);
	}

	AddWALInfoToBackupManifest(&manifest, state.startptr, state.starttli,
							   endptr, endtli);

	SendBackupManifest(&manifest, sink);

	bbsink_end_backup(sink, endptr, endtli);

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

	/*
	 * Make sure to free the manifest before the resource owners as manifests
	 * use cryptohash contexts that may depend on resource owners (like
	 * OpenSSL).
	 */
	FreeBackupManifest(&manifest);

	/* clean up the resource owner we created */
	WalSndResourceCleanup(true);

	basebackup_progress_done();
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
	bool		o_checkpoint = false;
	bool		o_nowait = false;
	bool		o_wal = false;
	bool		o_maxrate = false;
	bool		o_tablespace_map = false;
	bool		o_noverify_checksums = false;
	bool		o_manifest = false;
	bool		o_manifest_checksums = false;
	bool		o_target = false;
	bool		o_target_detail = false;
	char	   *target_str = NULL;
	char	   *target_detail_str = NULL;
	bool		o_compression = false;
	bool		o_compression_detail = false;
	char	   *compression_detail_str = NULL;

	MemSet(opt, 0, sizeof(*opt));
	opt->manifest = MANIFEST_OPTION_NO;
	opt->manifest_checksum_type = CHECKSUM_TYPE_CRC32C;
	opt->compression = PG_COMPRESSION_NONE;
	opt->compression_specification.algorithm = PG_COMPRESSION_NONE;

	foreach(lopt, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lopt);

		if (strcmp(defel->defname, "label") == 0)
		{
			if (o_label)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->label = defGetString(defel);
			o_label = true;
		}
		else if (strcmp(defel->defname, "progress") == 0)
		{
			if (o_progress)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->progress = defGetBoolean(defel);
			o_progress = true;
		}
		else if (strcmp(defel->defname, "checkpoint") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_checkpoint)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (pg_strcasecmp(optval, "fast") == 0)
				opt->fastcheckpoint = true;
			else if (pg_strcasecmp(optval, "spread") == 0)
				opt->fastcheckpoint = false;
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized checkpoint type: \"%s\"",
								optval)));
			o_checkpoint = true;
		}
		else if (strcmp(defel->defname, "wait") == 0)
		{
			if (o_nowait)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->nowait = !defGetBoolean(defel);
			o_nowait = true;
		}
		else if (strcmp(defel->defname, "wal") == 0)
		{
			if (o_wal)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->includewal = defGetBoolean(defel);
			o_wal = true;
		}
		else if (strcmp(defel->defname, "max_rate") == 0)
		{
			int64		maxrate;

			if (o_maxrate)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));

			maxrate = defGetInt64(defel);
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
			opt->sendtblspcmapfile = defGetBoolean(defel);
			o_tablespace_map = true;
		}
		else if (strcmp(defel->defname, "verify_checksums") == 0)
		{
			if (o_noverify_checksums)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			noverify_checksums = !defGetBoolean(defel);
			o_noverify_checksums = true;
		}
		else if (strcmp(defel->defname, "manifest") == 0)
		{
			char	   *optval = defGetString(defel);
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
			char	   *optval = defGetString(defel);

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
		else if (strcmp(defel->defname, "target") == 0)
		{
			if (o_target)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			target_str = defGetString(defel);
			o_target = true;
		}
		else if (strcmp(defel->defname, "target_detail") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_target_detail)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			target_detail_str = optval;
			o_target_detail = true;
		}
		else if (strcmp(defel->defname, "compression") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_compression)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (!parse_compress_algorithm(optval, &opt->compression))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized compression algorithm: \"%s\"",
								optval)));
			o_compression = true;
		}
		else if (strcmp(defel->defname, "compression_detail") == 0)
		{
			if (o_compression_detail)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			compression_detail_str = defGetString(defel);
			o_compression_detail = true;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized base backup option: \"%s\"",
							defel->defname)));
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

	if (target_str == NULL)
	{
		if (target_detail_str != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("target detail cannot be used without target")));
		opt->use_copytblspc = true;
		opt->send_to_client = true;
	}
	else if (strcmp(target_str, "client") == 0)
	{
		if (target_detail_str != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("target \"%s\" does not accept a target detail",
							target_str)));
		opt->send_to_client = true;
	}
	else
		opt->target_handle =
			BaseBackupGetTargetHandle(target_str, target_detail_str);

	if (o_compression_detail && !o_compression)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("compression detail cannot be specified unless compression is enabled")));

	if (o_compression)
	{
		char	   *error_detail;

		parse_compress_specification(opt->compression, compression_detail_str,
									 &opt->compression_specification);
		error_detail =
			validate_compress_specification(&opt->compression_specification);
		if (error_detail != NULL)
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("invalid compression specification: %s",
						   error_detail));
	}
}


/*
 * SendBaseBackup() - send a complete base backup.
 *
 * The function will put the system into backup mode like pg_backup_start()
 * does, so that the backup is consistent even though we read directly from
 * the filesystem, bypassing the buffer cache.
 */
void
SendBaseBackup(BaseBackupCmd *cmd)
{
	basebackup_options opt;
	bbsink	   *sink;
	SessionBackupState status = get_backup_status();

	if (status == SESSION_BACKUP_RUNNING)
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

	/*
	 * If the target is specifically 'client' then set up to stream the backup
	 * to the client; otherwise, it's being sent someplace else and should not
	 * be sent to the client. BaseBackupGetSink has the job of setting up a
	 * sink to send the backup data wherever it needs to go.
	 */
	sink = bbsink_copystream_new(opt.send_to_client);
	if (opt.target_handle != NULL)
		sink = BaseBackupGetSink(opt.target_handle, sink);

	/* Set up network throttling, if client requested it */
	if (opt.maxrate > 0)
		sink = bbsink_throttle_new(sink, opt.maxrate);

	/* Set up server-side compression, if client requested it */
	if (opt.compression == PG_COMPRESSION_GZIP)
		sink = bbsink_gzip_new(sink, &opt.compression_specification);
	else if (opt.compression == PG_COMPRESSION_LZ4)
		sink = bbsink_lz4_new(sink, &opt.compression_specification);
	else if (opt.compression == PG_COMPRESSION_ZSTD)
		sink = bbsink_zstd_new(sink, &opt.compression_specification);

	/* Set up progress reporting. */
	sink = bbsink_progress_new(sink, opt.progress);

	/*
	 * Perform the base backup, but make sure we clean up the bbsink even if
	 * an error occurs.
	 */
	PG_TRY();
	{
		perform_base_backup(&opt, sink);
	}
	PG_FINALLY();
	{
		bbsink_cleanup(sink);
	}
	PG_END_TRY();
}

/*
 * Inject a file with given name and content in the output tar stream.
 */
static void
sendFileWithContent(bbsink *sink, const char *filename, const char *content,
					backup_manifest_info *manifest)
{
	struct stat statbuf;
	int			bytes_done = 0,
				len;
	pg_checksum_context checksum_ctx;

	if (pg_checksum_init(&checksum_ctx, manifest->checksum_type) < 0)
		elog(ERROR, "could not initialize checksum of file \"%s\"",
			 filename);

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

	_tarWriteHeader(sink, filename, NULL, &statbuf, false);

	if (pg_checksum_update(&checksum_ctx, (uint8 *) content, len) < 0)
		elog(ERROR, "could not update checksum of file \"%s\"",
			 filename);

	while (bytes_done < len)
	{
		size_t		remaining = len - bytes_done;
		size_t		nbytes = Min(sink->bbs_buffer_length, remaining);

		memcpy(sink->bbs_buffer, content, nbytes);
		bbsink_archive_contents(sink, nbytes);
		bytes_done += nbytes;
		content += nbytes;
	}

	_tarWritePadding(sink, len);

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
static int64
sendTablespace(bbsink *sink, char *path, char *spcoid, bool sizeonly,
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

	size = _tarWriteHeader(sink, TABLESPACE_VERSION_DIRECTORY, NULL, &statbuf,
						   sizeonly);

	/* Send all the files in the tablespace version directory */
	size += sendDir(sink, pathbuf, strlen(path), sizeonly, NIL, true, manifest,
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
sendDir(bbsink *sink, const char *path, int basepathlen, bool sizeonly,
		List *tablespaces, bool sendtblspclinks, backup_manifest_info *manifest,
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
		 * do_pg_backup_stop() will check that too, but it's better to stop
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
				convert_link_to_directory(pathbuf, &statbuf);
				size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, NULL,
										&statbuf, sizeonly);
				excludeFound = true;
				break;
			}
		}

		if (excludeFound)
			continue;

		/*
		 * We can skip pg_wal, the WAL segments need to be fetched from the
		 * WAL archive anyway. But include it as an empty directory anyway, so
		 * we get permissions right.
		 */
		if (strcmp(pathbuf, "./pg_wal") == 0)
		{
			/* If pg_wal is a symlink, write it as a directory anyway */
			convert_link_to_directory(pathbuf, &statbuf);
			size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, NULL,
									&statbuf, sizeonly);

			/*
			 * Also send archive_status directory (by hackishly reusing
			 * statbuf from above ...).
			 */
			size += _tarWriteHeader(sink, "./pg_wal/archive_status", NULL,
									&statbuf, sizeonly);

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

			size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, linkpath,
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
			size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, NULL, &statbuf,
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
				size += sendDir(sink, pathbuf, basepathlen, sizeonly, tablespaces,
								sendtblspclinks, manifest, spcoid);
		}
		else if (S_ISREG(statbuf.st_mode))
		{
			bool		sent = false;

			if (!sizeonly)
				sent = sendFile(sink, pathbuf, pathbuf + basepathlen + 1, &statbuf,
								true, isDbDir ? atooid(lastDir + 1) : InvalidOid,
								manifest, spcoid);

			if (sent || sizeonly)
			{
				/* Add size. */
				size += statbuf.st_size;

				/* Pad to a multiple of the tar block size. */
				size += tarPaddingBytesRequired(statbuf.st_size);

				/* Size of the header for the file. */
				size += TAR_BLOCK_SIZE;
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
 * If dboid is anything other than InvalidOid then any checksum failures
 * detected will get reported to the cumulative stats system.
 *
 * Returns true if the file was successfully sent, false if 'missing_ok',
 * and the file did not exist.
 */
static bool
sendFile(bbsink *sink, const char *readfilename, const char *tarfilename,
		 struct stat *statbuf, bool missing_ok, Oid dboid,
		 backup_manifest_info *manifest, const char *spcoid)
{
	int			fd;
	BlockNumber blkno = 0;
	bool		block_retry = false;
	uint16		checksum;
	int			checksum_failures = 0;
	off_t		cnt;
	int			i;
	pgoff_t		len = 0;
	char	   *page;
	PageHeader	phdr;
	int			segmentno = 0;
	char	   *segmentpath;
	bool		verify_checksum = false;
	pg_checksum_context checksum_ctx;

	if (pg_checksum_init(&checksum_ctx, manifest->checksum_type) < 0)
		elog(ERROR, "could not initialize checksum of file \"%s\"",
			 readfilename);

	fd = OpenTransientFile(readfilename, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT && missing_ok)
			return false;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", readfilename)));
	}

	_tarWriteHeader(sink, tarfilename, NULL, statbuf, false);

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

	/*
	 * Loop until we read the amount of data the caller told us to expect. The
	 * file could be longer, if it was extended while we were sending it, but
	 * for a base backup we can ignore such extended data. It will be restored
	 * from WAL.
	 */
	while (len < statbuf->st_size)
	{
		size_t		remaining = statbuf->st_size - len;

		/* Try to read some more data. */
		cnt = basebackup_read_file(fd, sink->bbs_buffer,
								   Min(sink->bbs_buffer_length, remaining),
								   len, readfilename, true);

		/*
		 * The checksums are verified at block level, so we iterate over the
		 * buffer in chunks of BLCKSZ, after making sure that
		 * TAR_SEND_SIZE/buf is divisible by BLCKSZ and we read a multiple of
		 * BLCKSZ bytes.
		 */
		Assert((sink->bbs_buffer_length % BLCKSZ) == 0);

		if (verify_checksum && (cnt % BLCKSZ != 0))
		{
			ereport(WARNING,
					(errmsg("could not verify checksum in file \"%s\", block "
							"%u: read buffer size %d and page size %d "
							"differ",
							readfilename, blkno, (int) cnt, BLCKSZ)));
			verify_checksum = false;
		}

		if (verify_checksum)
		{
			for (i = 0; i < cnt / BLCKSZ; i++)
			{
				page = sink->bbs_buffer + BLCKSZ * i;

				/*
				 * Only check pages which have not been modified since the
				 * start of the base backup. Otherwise, they might have been
				 * written only halfway and the checksum would not be valid.
				 * However, replaying WAL would reinstate the correct page in
				 * this case. We also skip completely new pages, since they
				 * don't have a checksum yet.
				 */
				if (!PageIsNew(page) && PageGetLSN(page) < sink->bbs_state->startptr)
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
							int			reread_cnt;

							/* Reread the failed block */
							reread_cnt =
								basebackup_read_file(fd,
													 sink->bbs_buffer + BLCKSZ * i,
													 BLCKSZ, len + BLCKSZ * i,
													 readfilename,
													 false);
							if (reread_cnt == 0)
							{
								/*
								 * If we hit end-of-file, a concurrent
								 * truncation must have occurred, so break out
								 * of this loop just as if the initial fread()
								 * returned 0. We'll drop through to the same
								 * code that handles that case. (We must fix
								 * up cnt first, though.)
								 */
								cnt = BLCKSZ * i;
								break;
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
											"file \"%s\", block %u: calculated "
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

		/*
		 * If we hit end-of-file, a concurrent truncation must have occurred.
		 * That's not an error condition, because WAL replay will fix things
		 * up.
		 */
		if (cnt == 0)
			break;

		/* Archive the data we just read. */
		bbsink_archive_contents(sink, cnt);

		/* Also feed it to the checksum machinery. */
		if (pg_checksum_update(&checksum_ctx,
							   (uint8 *) sink->bbs_buffer, cnt) < 0)
			elog(ERROR, "could not update checksum of base backup");

		len += cnt;
	}

	/* If the file was truncated while we were sending it, pad it with zeros */
	while (len < statbuf->st_size)
	{
		size_t		remaining = statbuf->st_size - len;
		size_t		nbytes = Min(sink->bbs_buffer_length, remaining);

		MemSet(sink->bbs_buffer, 0, nbytes);
		if (pg_checksum_update(&checksum_ctx,
							   (uint8 *) sink->bbs_buffer,
							   nbytes) < 0)
			elog(ERROR, "could not update checksum of base backup");
		bbsink_archive_contents(sink, nbytes);
		len += nbytes;
	}

	/*
	 * Pad to a block boundary, per tar format requirements. (This small piece
	 * of data is probably not worth throttling, and is not checksummed
	 * because it's not actually part of the file.)
	 */
	_tarWritePadding(sink, len);

	CloseTransientFile(fd);

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
_tarWriteHeader(bbsink *sink, const char *filename, const char *linktarget,
				struct stat *statbuf, bool sizeonly)
{
	enum tarError rc;

	if (!sizeonly)
	{
		/*
		 * As of this writing, the smallest supported block size is 1kB, which
		 * is twice TAR_BLOCK_SIZE. Since the buffer size is required to be a
		 * multiple of BLCKSZ, it should be safe to assume that the buffer is
		 * large enough to fit an entire tar block. We double-check by means
		 * of these assertions.
		 */
		StaticAssertStmt(TAR_BLOCK_SIZE <= BLCKSZ,
						 "BLCKSZ too small for tar block");
		Assert(sink->bbs_buffer_length >= TAR_BLOCK_SIZE);

		rc = tarCreateHeader(sink->bbs_buffer, filename, linktarget,
							 statbuf->st_size, statbuf->st_mode,
							 statbuf->st_uid, statbuf->st_gid,
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

		bbsink_archive_contents(sink, TAR_BLOCK_SIZE);
	}

	return TAR_BLOCK_SIZE;
}

/*
 * Pad with zero bytes out to a multiple of TAR_BLOCK_SIZE.
 */
static void
_tarWritePadding(bbsink *sink, int len)
{
	int			pad = tarPaddingBytesRequired(len);

	/*
	 * As in _tarWriteHeader, it should be safe to assume that the buffer is
	 * large enough that we don't need to do this in multiple chunks.
	 */
	Assert(sink->bbs_buffer_length >= TAR_BLOCK_SIZE);
	Assert(pad <= TAR_BLOCK_SIZE);

	if (pad > 0)
	{
		MemSet(sink->bbs_buffer, 0, pad);
		bbsink_archive_contents(sink, pad);
	}
}

/*
 * If the entry in statbuf is a link, then adjust statbuf to make it look like a
 * directory, so that it will be written that way.
 */
static void
convert_link_to_directory(const char *pathbuf, struct stat *statbuf)
{
	/* If symlink, write it as a directory anyway */
#ifndef WIN32
	if (S_ISLNK(statbuf->st_mode))
#else
	if (pgwin32_is_junction(pathbuf))
#endif
		statbuf->st_mode = S_IFDIR | pg_dir_create_mode;
}

/*
 * Read some data from a file, setting a wait event and reporting any error
 * encountered.
 *
 * If partial_read_ok is false, also report an error if the number of bytes
 * read is not equal to the number of bytes requested.
 *
 * Returns the number of bytes read.
 */
static int
basebackup_read_file(int fd, char *buf, size_t nbytes, off_t offset,
					 const char *filename, bool partial_read_ok)
{
	int			rc;

	pgstat_report_wait_start(WAIT_EVENT_BASEBACKUP_READ);
	rc = pg_pread(fd, buf, nbytes, offset);
	pgstat_report_wait_end();

	if (rc < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", filename)));
	if (!partial_read_ok && rc > 0 && rc != nbytes)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": read %d of %zu",
						filename, rc, nbytes)));

	return rc;
}
