/*-------------------------------------------------------------------------
 *
 * basebackup.c
 *	  code for taking a base backup and streaming it to a standby
 *
 * Portions Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/basebackup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "access/xlog_internal.h"		/* for pg_start/stop_backup */
#include "catalog/catalog.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgtar.h"
#include "pgstat.h"
#include "replication/basebackup.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/dsm_impl.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/ps_status.h"
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
} basebackup_options;


static int64 sendDir(char *path, int basepathlen, bool sizeonly,
		List *tablespaces, bool sendtblspclinks);
static bool sendFile(char *readfilename, char *tarfilename,
		 struct stat * statbuf, bool missing_ok);
static void sendFileWithContent(const char *filename, const char *content);
static int64 _tarWriteHeader(const char *filename, const char *linktarget,
				struct stat * statbuf, bool sizeonly);
static int64 _tarWriteDir(const char *pathbuf, int basepathlen, struct stat *statbuf,
				bool sizeonly);
static void send_int8_string(StringInfoData *buf, int64 intval);
static void SendBackupHeader(List *tablespaces);
static void base_backup_cleanup(int code, Datum arg);
static void perform_base_backup(basebackup_options *opt, DIR *tblspcdir);
static void parse_basebackup_options(List *options, basebackup_options *opt);
static void SendXlogRecPtrResult(XLogRecPtr ptr, TimeLineID tli);
static int	compareWalFileNames(const void *a, const void *b);
static void throttle(size_t increment);

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

/* The actual number of bytes, transfer of which may cause sleep. */
static uint64 throttling_sample;

/* Amount of data already transferred but not yet throttled.  */
static int64 throttling_counter;

/* The minimum time required to transfer throttling_sample bytes. */
static int64 elapsed_min_unit;

/* The last check of the transfer rate. */
static int64 throttled_last;

/*
 * The contents of these directories are removed or recreated during server
 * start so they are not included in backups.  The directories themselves are
 * kept and included as empty to preserve access permissions.
 */
static const char *excludeDirContents[] =
{
	/*
	 * Skip temporary statistics files. PG_STAT_TMP_DIR must be skipped even
	 * when stats_temp_directory is set because PGSS_TEXT_FILE is always created
	 * there.
	 */
	PG_STAT_TMP_DIR,

	/*
	 * It is generally not useful to backup the contents of this directory even
	 * if the intention is to restore to another master. See backup.sgml for a
	 * more detailed description.
	 */
	"pg_replslot",

	/* Contents removed on startup, see dsm_cleanup_for_mmap(). */
	PG_DYNSHMEM_DIR,

	/* Contents removed on startup, see AsyncShmemInit(). */
	"pg_notify",

	/*
	 * Old contents are loaded for possible debugging but are not required for
	 * normal operation, see OldSerXidInit().
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
static const char *excludeFiles[] =
{
	/* Skip auto conf temporary file. */
	PG_AUTOCONF_FILENAME ".tmp",

	/*
	 * If there's a backup_label or tablespace_map file, it belongs to a
	 * backup started by the user with pg_start_backup().  It is *not* correct
	 * for this backup.  Our backup_label/tablespace_map is injected into the
	 * tar separately.
	 */
	BACKUP_LABEL_FILE,
	TABLESPACE_MAP,

	"postmaster.pid",
	"postmaster.opts",

	/* end of list */
	NULL
};

/*
 * Called when ERROR or FATAL happens in perform_base_backup() after
 * we have started the backup - make sure we end it!
 */
static void
base_backup_cleanup(int code, Datum arg)
{
	do_pg_abort_backup();
}

/*
 * Actually do a base backup for the specified tablespaces.
 *
 * This is split out mainly to avoid complaints about "variable might be
 * clobbered by longjmp" from stupider versions of gcc.
 */
static void
perform_base_backup(basebackup_options *opt, DIR *tblspcdir)
{
	XLogRecPtr	startptr;
	TimeLineID	starttli;
	XLogRecPtr	endptr;
	TimeLineID	endtli;
	StringInfo	labelfile;
	StringInfo	tblspc_map_file = NULL;
	int			datadirpathlen;
	List	   *tablespaces = NIL;

	datadirpathlen = strlen(DataDir);

	backup_started_in_recovery = RecoveryInProgress();

	labelfile = makeStringInfo();
	tblspc_map_file = makeStringInfo();

	startptr = do_pg_start_backup(opt->label, opt->fastcheckpoint, &starttli,
								  labelfile, tblspcdir, &tablespaces,
								  tblspc_map_file,
								  opt->progress, opt->sendtblspcmapfile);

	/*
	 * Once do_pg_start_backup has been called, ensure that any failure causes
	 * us to abort the backup so we don't "leak" a backup counter. For this
	 * reason, *all* functionality between do_pg_start_backup() and
	 * do_pg_stop_backup() should be inside the error cleanup block!
	 */

	PG_ENSURE_ERROR_CLEANUP(base_backup_cleanup, (Datum) 0);
	{
		ListCell   *lc;
		tablespaceinfo *ti;

		SendXlogRecPtrResult(startptr, starttli);

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
		ti->size = opt->progress ? sendDir(".", 1, true, tablespaces, true) : -1;
		tablespaces = lappend(tablespaces, ti);

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
			throttled_last = GetCurrentIntegerTimestamp();
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
			pq_sendbyte(&buf, 0);		/* overall format */
			pq_sendint(&buf, 0, 2);		/* natts */
			pq_endmessage(&buf);

			if (ti->path == NULL)
			{
				struct stat statbuf;

				/* In the main tar, include the backup_label first... */
				sendFileWithContent(BACKUP_LABEL_FILE, labelfile->data);

				/*
				 * Send tablespace_map file if required and then the bulk of
				 * the files.
				 */
				if (tblspc_map_file && opt->sendtblspcmapfile)
				{
					sendFileWithContent(TABLESPACE_MAP, tblspc_map_file->data);
					sendDir(".", 1, false, tablespaces, false);
				}
				else
					sendDir(".", 1, false, tablespaces, true);

				/* ... and pg_control after everything else. */
				if (lstat(XLOG_CONTROL_FILE, &statbuf) != 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not stat control file \"%s\": %m",
									XLOG_CONTROL_FILE)));
				sendFile(XLOG_CONTROL_FILE, XLOG_CONTROL_FILE, &statbuf, false);
			}
			else
				sendTablespace(ti->path, false);

			/*
			 * If we're including WAL, and this is the main data directory we
			 * don't terminate the tar stream here. Instead, we will append
			 * the xlog files below and terminate it then. This is safe since
			 * the main data directory is always sent *last*.
			 */
			if (opt->includewal && ti->path == NULL)
			{
				Assert(lnext(lc) == NULL);
			}
			else
				pq_putemptymessage('c');		/* CopyDone */
		}
	}
	PG_END_ENSURE_ERROR_CLEANUP(base_backup_cleanup, (Datum) 0);

	endptr = do_pg_stop_backup(labelfile->data, !opt->nowait, &endtli);

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
		char	  **walFiles;
		int			nWalFiles;
		char		firstoff[MAXFNAMELEN];
		char		lastoff[MAXFNAMELEN];
		DIR		   *dir;
		struct dirent *de;
		int			i;
		ListCell   *lc;
		TimeLineID	tli;

		/*
		 * I'd rather not worry about timelines here, so scan pg_xlog and
		 * include all WAL files in the range between 'startptr' and 'endptr',
		 * regardless of the timeline the file is stamped with. If there are
		 * some spurious WAL files belonging to timelines that don't belong in
		 * this server's history, they will be included too. Normally there
		 * shouldn't be such files, but if there are, there's little harm in
		 * including them.
		 */
		XLByteToSeg(startptr, startsegno);
		XLogFileName(firstoff, ThisTimeLineID, startsegno);
		XLByteToPrevSeg(endptr, endsegno);
		XLogFileName(lastoff, ThisTimeLineID, endsegno);

		dir = AllocateDir("pg_xlog");
		if (!dir)
			ereport(ERROR,
				 (errmsg("could not open directory \"%s\": %m", "pg_xlog")));
		while ((de = ReadDir(dir, "pg_xlog")) != NULL)
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
		 * Put the WAL filenames into an array, and sort. We send the files in
		 * order from oldest to newest, to reduce the chance that a file is
		 * recycled before we get a chance to send it over.
		 */
		nWalFiles = list_length(walFileList);
		walFiles = palloc(nWalFiles * sizeof(char *));
		i = 0;
		foreach(lc, walFileList)
		{
			walFiles[i++] = lfirst(lc);
		}
		qsort(walFiles, nWalFiles, sizeof(char *), compareWalFileNames);

		/*
		 * There must be at least one xlog file in the pg_xlog directory,
		 * since we are doing backup-including-xlog.
		 */
		if (nWalFiles < 1)
			ereport(ERROR,
					(errmsg("could not find any WAL files")));

		/*
		 * Sanity check: the first and last segment should cover startptr and
		 * endptr, with no gaps in between.
		 */
		XLogFromFileName(walFiles[0], &tli, &segno);
		if (segno != startsegno)
		{
			char		startfname[MAXFNAMELEN];

			XLogFileName(startfname, ThisTimeLineID, startsegno);
			ereport(ERROR,
					(errmsg("could not find WAL file \"%s\"", startfname)));
		}
		for (i = 0; i < nWalFiles; i++)
		{
			XLogSegNo	currsegno = segno;
			XLogSegNo	nextsegno = segno + 1;

			XLogFromFileName(walFiles[i], &tli, &segno);
			if (!(nextsegno == segno || currsegno == segno))
			{
				char		nextfname[MAXFNAMELEN];

				XLogFileName(nextfname, ThisTimeLineID, nextsegno);
				ereport(ERROR,
					  (errmsg("could not find WAL file \"%s\"", nextfname)));
			}
		}
		if (segno != endsegno)
		{
			char		endfname[MAXFNAMELEN];

			XLogFileName(endfname, ThisTimeLineID, endsegno);
			ereport(ERROR,
					(errmsg("could not find WAL file \"%s\"", endfname)));
		}

		/* Ok, we have everything we need. Send the WAL files. */
		for (i = 0; i < nWalFiles; i++)
		{
			FILE	   *fp;
			char		buf[TAR_SEND_SIZE];
			size_t		cnt;
			pgoff_t		len = 0;

			snprintf(pathbuf, MAXPGPATH, XLOGDIR "/%s", walFiles[i]);
			XLogFromFileName(walFiles[i], &tli, &segno);

			fp = AllocateFile(pathbuf, "rb");
			if (fp == NULL)
			{
				/*
				 * Most likely reason for this is that the file was already
				 * removed by a checkpoint, so check for that to get a better
				 * error message.
				 */
				CheckXLogRemoved(segno, tli);

				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", pathbuf)));
			}

			if (fstat(fileno(fp), &statbuf) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m",
								pathbuf)));
			if (statbuf.st_size != XLogSegSize)
			{
				CheckXLogRemoved(segno, tli);
				ereport(ERROR,
						(errcode_for_file_access(),
					errmsg("unexpected WAL file size \"%s\"", walFiles[i])));
			}

			/* send the WAL file itself */
			_tarWriteHeader(pathbuf, NULL, &statbuf, false);

			while ((cnt = fread(buf, 1, Min(sizeof(buf), XLogSegSize - len), fp)) > 0)
			{
				CheckXLogRemoved(segno, tli);
				/* Send the chunk as a CopyData message */
				if (pq_putmessage('d', buf, cnt))
					ereport(ERROR,
							(errmsg("base backup could not send data, aborting backup")));

				len += cnt;
				throttle(cnt);

				if (len == XLogSegSize)
					break;
			}

			if (len != XLogSegSize)
			{
				CheckXLogRemoved(segno, tli);
				ereport(ERROR,
						(errcode_for_file_access(),
					errmsg("unexpected WAL file size \"%s\"", walFiles[i])));
			}

			/* XLogSegSize is a multiple of 512, so no need for padding */

			FreeFile(fp);

			/*
			 * Mark file as archived, otherwise files can get archived again
			 * after promotion of a new node. This is in line with
			 * walreceiver.c always doing an XLogArchiveForceDone() after a
			 * complete segment.
			 */
			StatusFilePath(pathbuf, walFiles[i], ".done");
			sendFileWithContent(pathbuf, "");
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

			sendFile(pathbuf, pathbuf, &statbuf, false);

			/* unconditionally mark file as archived */
			StatusFilePath(pathbuf, fname, ".done");
			sendFileWithContent(pathbuf, "");
		}

		/* Send CopyDone message for the last tar file */
		pq_putemptymessage('c');
	}
	SendXlogRecPtrResult(endptr, endtli);
}

/*
 * qsort comparison function, to compare log/seg portion of WAL segment
 * filenames, ignoring the timeline portion.
 */
static int
compareWalFileNames(const void *a, const void *b)
{
	char	   *fna = *((char **) a);
	char	   *fnb = *((char **) b);

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

	MemSet(opt, 0, sizeof(*opt));
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
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}
	if (opt->label == NULL)
		opt->label = "base backup";
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
	DIR		   *dir;
	basebackup_options opt;

	parse_basebackup_options(cmd->options, &opt);

	WalSndSetState(WALSNDSTATE_BACKUP);

	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "sending backup \"%s\"",
				 opt.label);
		set_ps_display(activitymsg, false);
	}

	/* Make sure we can open the directory with tablespaces in it */
	dir = AllocateDir("pg_tblspc");
	if (!dir)
		ereport(ERROR,
				(errmsg("could not open directory \"%s\": %m", "pg_tblspc")));

	perform_base_backup(&opt, dir);

	FreeDir(dir);
}

static void
send_int8_string(StringInfoData *buf, int64 intval)
{
	char		is[32];

	sprintf(is, INT64_FORMAT, intval);
	pq_sendint(buf, strlen(is), 4);
	pq_sendbytes(buf, is, strlen(is));
}

static void
SendBackupHeader(List *tablespaces)
{
	StringInfoData buf;
	ListCell   *lc;

	/* Construct and send the directory information */
	pq_beginmessage(&buf, 'T'); /* RowDescription */
	pq_sendint(&buf, 3, 2);		/* 3 fields */

	/* First field - spcoid */
	pq_sendstring(&buf, "spcoid");
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, OIDOID, 4);	/* type oid */
	pq_sendint(&buf, 4, 2);		/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	/* Second field - spcpath */
	pq_sendstring(&buf, "spclocation");
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_sendint(&buf, TEXTOID, 4);
	pq_sendint(&buf, -1, 2);
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);

	/* Third field - size */
	pq_sendstring(&buf, "size");
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_sendint(&buf, INT8OID, 4);
	pq_sendint(&buf, 8, 2);
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_endmessage(&buf);

	foreach(lc, tablespaces)
	{
		tablespaceinfo *ti = lfirst(lc);

		/* Send one datarow message */
		pq_beginmessage(&buf, 'D');
		pq_sendint(&buf, 3, 2); /* number of columns */
		if (ti->path == NULL)
		{
			pq_sendint(&buf, -1, 4);	/* Length = -1 ==> NULL */
			pq_sendint(&buf, -1, 4);
		}
		else
		{
			Size		len;

			len = strlen(ti->oid);
			pq_sendint(&buf, len, 4);
			pq_sendbytes(&buf, ti->oid, len);

			len = strlen(ti->path);
			pq_sendint(&buf, len, 4);
			pq_sendbytes(&buf, ti->path, len);
		}
		if (ti->size >= 0)
			send_int8_string(&buf, ti->size / 1024);
		else
			pq_sendint(&buf, -1, 4);	/* NULL */

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
	pq_sendint(&buf, 2, 2);		/* 2 fields */

	/* Field headers */
	pq_sendstring(&buf, "recptr");
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, TEXTOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);

	pq_sendstring(&buf, "tli");
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */

	/*
	 * int8 may seem like a surprising data type for this, but in theory int4
	 * would not be wide enough for this, as TimeLineID is unsigned.
	 */
	pq_sendint(&buf, INT8OID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_endmessage(&buf);

	/* Data row */
	pq_beginmessage(&buf, 'D');
	pq_sendint(&buf, 2, 2);		/* number of columns */

	len = snprintf(str, sizeof(str),
				   "%X/%X", (uint32) (ptr >> 32), (uint32) ptr);
	pq_sendint(&buf, len, 4);
	pq_sendbytes(&buf, str, len);

	len = snprintf(str, sizeof(str), "%u", tli);
	pq_sendint(&buf, len, 4);
	pq_sendbytes(&buf, str, len);

	pq_endmessage(&buf);

	/* Send a CommandComplete message */
	pq_puttextmessage('C', "SELECT");
}

/*
 * Inject a file with given name and content in the output tar stream.
 */
static void
sendFileWithContent(const char *filename, const char *content)
{
	struct stat statbuf;
	int			pad,
				len;

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
	statbuf.st_mode = S_IRUSR | S_IWUSR;
	statbuf.st_size = len;

	_tarWriteHeader(filename, NULL, &statbuf, false);
	/* Send the contents as a CopyData message */
	pq_putmessage('d', content, len);

	/* Pad to 512 byte boundary, per tar format requirements */
	pad = ((len + 511) & ~511) - len;
	if (pad > 0)
	{
		char		buf[512];

		MemSet(buf, 0, pad);
		pq_putmessage('d', buf, pad);
	}
}

/*
 * Include the tablespace directory pointed to by 'path' in the output tar
 * stream.  If 'sizeonly' is true, we just calculate a total length and return
 * it, without actually sending anything.
 *
 * Only used to send auxiliary tablespaces, not PGDATA.
 */
int64
sendTablespace(char *path, bool sizeonly)
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
	size += sendDir(pathbuf, strlen(path), sizeonly, NIL, true);

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
sendDir(char *path, int basepathlen, bool sizeonly, List *tablespaces,
		bool sendtblspclinks)
{
	DIR		   *dir;
	struct dirent *de;
	char		pathbuf[MAXPGPATH];
	struct stat statbuf;
	int64		size = 0;

	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		int			excludeIdx;
		bool		excludeFound;

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
		 * dp_pg_stop_backup() will check that too, but it's better to stop
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
		for (excludeIdx = 0; excludeFiles[excludeIdx] != NULL; excludeIdx++)
		{
			if (strcmp(de->d_name, excludeFiles[excludeIdx]) == 0)
			{
				elog(DEBUG1, "file \"%s\" excluded from backup", de->d_name);
				excludeFound = true;
				break;
			}
		}

		if (excludeFound)
			continue;

		snprintf(pathbuf, MAXPGPATH, "%s/%s", path, de->d_name);

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
				size += _tarWriteDir(pathbuf, basepathlen, &statbuf,  sizeonly);
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
		 * We can skip pg_xlog, the WAL segments need to be fetched from the
		 * WAL archive anyway. But include it as an empty directory anyway, so
		 * we get permissions right.
		 */
		if (strcmp(pathbuf, "./pg_xlog") == 0)
		{
			/* If pg_xlog is a symlink, write it as a directory anyway */
			size += _tarWriteDir(pathbuf, basepathlen, &statbuf, sizeonly);

			/*
			 * Also send archive_status directory (by hackishly reusing
			 * statbuf from above ...).
			 */
			size += _tarWriteHeader("./pg_xlog/archive_status", NULL, &statbuf,
									sizeonly);

			continue;			/* don't recurse into pg_xlog */
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
#endif   /* HAVE_READLINK */
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
				size += sendDir(pathbuf, basepathlen, sizeonly, tablespaces, sendtblspclinks);
		}
		else if (S_ISREG(statbuf.st_mode))
		{
			bool		sent = false;

			if (!sizeonly)
				sent = sendFile(pathbuf, pathbuf + basepathlen + 1, &statbuf,
								true);

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
 * Returns true if the file was successfully sent, false if 'missing_ok',
 * and the file did not exist.
 */
static bool
sendFile(char *readfilename, char *tarfilename, struct stat * statbuf,
		 bool missing_ok)
{
	FILE	   *fp;
	char		buf[TAR_SEND_SIZE];
	size_t		cnt;
	pgoff_t		len = 0;
	size_t		pad;

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

	while ((cnt = fread(buf, 1, Min(sizeof(buf), statbuf->st_size - len), fp)) > 0)
	{
		/* Send the chunk as a CopyData message */
		if (pq_putmessage('d', buf, cnt))
			ereport(ERROR,
			   (errmsg("base backup could not send data, aborting backup")));

		len += cnt;
		throttle(cnt);

		if (len >= statbuf->st_size)
		{
			/*
			 * Reached end of file. The file could be longer, if it was
			 * extended while we were sending it, but for a base backup we can
			 * ignore such extended data. It will be restored from WAL.
			 */
			break;
		}
	}

	/* If the file was truncated while we were sending it, pad it with zeros */
	if (len < statbuf->st_size)
	{
		MemSet(buf, 0, sizeof(buf));
		while (len < statbuf->st_size)
		{
			cnt = Min(sizeof(buf), statbuf->st_size - len);
			pq_putmessage('d', buf, cnt);
			len += cnt;
			throttle(cnt);
		}
	}

	/*
	 * Pad to 512 byte boundary, per tar format requirements. (This small
	 * piece of data is probably not worth throttling.)
	 */
	pad = ((len + 511) & ~511) - len;
	if (pad > 0)
	{
		MemSet(buf, 0, pad);
		pq_putmessage('d', buf, pad);
	}

	FreeFile(fp);

	return true;
}


static int64
_tarWriteHeader(const char *filename, const char *linktarget,
				struct stat * statbuf, bool sizeonly)
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
		statbuf->st_mode = S_IFDIR | S_IRWXU;

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
	int64		elapsed,
				elapsed_min,
				sleep;
	int			wait_result;

	if (throttling_counter < 0)
		return;

	throttling_counter += increment;
	if (throttling_counter < throttling_sample)
		return;

	/* Time elapsed since the last measurement (and possible wake up). */
	elapsed = GetCurrentIntegerTimestamp() - throttled_last;
	/* How much should have elapsed at minimum? */
	elapsed_min = elapsed_min_unit * (throttling_counter / throttling_sample);
	sleep = elapsed_min - elapsed;
	/* Only sleep if the transfer is faster than it should be. */
	if (sleep > 0)
	{
		ResetLatch(MyLatch);

		/* We're eating a potentially set latch, so check for interrupts */
		CHECK_FOR_INTERRUPTS();

		/*
		 * (TAR_SEND_SIZE / throttling_sample * elapsed_min_unit) should be
		 * the maximum time to sleep. Thus the cast to long is safe.
		 */
		wait_result = WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
								(long) (sleep / 1000));

		if (wait_result & WL_LATCH_SET)
			CHECK_FOR_INTERRUPTS();
	}
	else
	{
		/*
		 * The actual transfer rate is below the limit.  A negative value
		 * would distort the adjustment of throttled_last.
		 */
		wait_result = 0;
		sleep = 0;
	}

	/*
	 * Only a whole multiple of throttling_sample was processed. The rest will
	 * be done during the next call of this function.
	 */
	throttling_counter %= throttling_sample;

	/* Once the (possible) sleep has ended, new period starts. */
	if (wait_result & WL_TIMEOUT)
		throttled_last += elapsed + sleep;
	else if (sleep > 0)
		/* Sleep was necessary but might have been interrupted. */
		throttled_last = GetCurrentIntegerTimestamp();
}
