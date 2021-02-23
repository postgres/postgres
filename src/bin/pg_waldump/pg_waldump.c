/*-------------------------------------------------------------------------
 *
 * pg_waldump.c - decode and display WAL
 *
 * Copyright (c) 2013-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_waldump/pg_waldump.c
 *-------------------------------------------------------------------------
 */

#define FRONTEND 1
#include "postgres.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "common/fe_memutils.h"
#include "common/logging.h"
#include "getopt_long.h"
#include "rmgrdesc.h"

static const char *progname;

static int	WalSegSz;

typedef struct XLogDumpPrivate
{
	TimeLineID	timeline;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	bool		endptr_reached;
} XLogDumpPrivate;

typedef struct XLogDumpConfig
{
	/* display options */
	bool		quiet;
	bool		bkp_details;
	int			stop_after_records;
	int			already_displayed_records;
	bool		follow;
	bool		stats;
	bool		stats_per_record;

	/* filter options */
	int			filter_by_rmgr;
	TransactionId filter_by_xid;
	bool		filter_by_xid_enabled;
} XLogDumpConfig;

typedef struct Stats
{
	uint64		count;
	uint64		rec_len;
	uint64		fpi_len;
} Stats;

#define MAX_XLINFO_TYPES 16

typedef struct XLogDumpStats
{
	uint64		count;
	Stats		rmgr_stats[RM_NEXT_ID];
	Stats		record_stats[RM_NEXT_ID][MAX_XLINFO_TYPES];
} XLogDumpStats;

#define fatal_error(...) do { pg_log_fatal(__VA_ARGS__); exit(EXIT_FAILURE); } while(0)

static void
print_rmgr_list(void)
{
	int			i;

	for (i = 0; i <= RM_MAX_ID; i++)
	{
		printf("%s\n", RmgrDescTable[i].rm_name);
	}
}

/*
 * Check whether directory exists and whether we can open it. Keep errno set so
 * that the caller can report errors somewhat more accurately.
 */
static bool
verify_directory(const char *directory)
{
	DIR		   *dir = opendir(directory);

	if (dir == NULL)
		return false;
	closedir(dir);
	return true;
}

/*
 * Split a pathname as dirname(1) and basename(1) would.
 *
 * XXX this probably doesn't do very well on Windows.  We probably need to
 * apply canonicalize_path(), at the very least.
 */
static void
split_path(const char *path, char **dir, char **fname)
{
	char	   *sep;

	/* split filepath into directory & filename */
	sep = strrchr(path, '/');

	/* directory path */
	if (sep != NULL)
	{
		*dir = pnstrdup(path, sep - path);
		*fname = pg_strdup(sep + 1);
	}
	/* local directory */
	else
	{
		*dir = NULL;
		*fname = pg_strdup(path);
	}
}

/*
 * Open the file in the valid target directory.
 *
 * return a read only fd
 */
static int
open_file_in_directory(const char *directory, const char *fname)
{
	int			fd = -1;
	char		fpath[MAXPGPATH];

	Assert(directory != NULL);

	snprintf(fpath, MAXPGPATH, "%s/%s", directory, fname);
	fd = open(fpath, O_RDONLY | PG_BINARY, 0);

	if (fd < 0 && errno != ENOENT)
		fatal_error("could not open file \"%s\": %m", fname);
	return fd;
}

/*
 * Try to find fname in the given directory. Returns true if it is found,
 * false otherwise. If fname is NULL, search the complete directory for any
 * file with a valid WAL file name. If file is successfully opened, set the
 * wal segment size.
 */
static bool
search_directory(const char *directory, const char *fname)
{
	int			fd = -1;
	DIR		   *xldir;

	/* open file if valid filename is provided */
	if (fname != NULL)
		fd = open_file_in_directory(directory, fname);

	/*
	 * A valid file name is not passed, so search the complete directory.  If
	 * we find any file whose name is a valid WAL file name then try to open
	 * it.  If we cannot open it, bail out.
	 */
	else if ((xldir = opendir(directory)) != NULL)
	{
		struct dirent *xlde;

		while ((xlde = readdir(xldir)) != NULL)
		{
			if (IsXLogFileName(xlde->d_name))
			{
				fd = open_file_in_directory(directory, xlde->d_name);
				fname = xlde->d_name;
				break;
			}
		}

		closedir(xldir);
	}

	/* set WalSegSz if file is successfully opened */
	if (fd >= 0)
	{
		PGAlignedXLogBlock buf;
		int			r;

		r = read(fd, buf.data, XLOG_BLCKSZ);
		if (r == XLOG_BLCKSZ)
		{
			XLogLongPageHeader longhdr = (XLogLongPageHeader) buf.data;

			WalSegSz = longhdr->xlp_seg_size;

			if (!IsValidWalSegSize(WalSegSz))
				fatal_error(ngettext("WAL segment size must be a power of two between 1 MB and 1 GB, but the WAL file \"%s\" header specifies %d byte",
									 "WAL segment size must be a power of two between 1 MB and 1 GB, but the WAL file \"%s\" header specifies %d bytes",
									 WalSegSz),
							fname, WalSegSz);
		}
		else
		{
			if (errno != 0)
				fatal_error("could not read file \"%s\": %m",
							fname);
			else
				fatal_error("could not read file \"%s\": read %d of %zu",
							fname, r, (Size) XLOG_BLCKSZ);
		}
		close(fd);
		return true;
	}

	return false;
}

/*
 * Identify the target directory.
 *
 * Try to find the file in several places:
 * if directory != NULL:
 *	 directory /
 *	 directory / XLOGDIR /
 * else
 *	 .
 *	 XLOGDIR /
 *	 $PGDATA / XLOGDIR /
 *
 * The valid target directory is returned.
 */
static char *
identify_target_directory(char *directory, char *fname)
{
	char		fpath[MAXPGPATH];

	if (directory != NULL)
	{
		if (search_directory(directory, fname))
			return pg_strdup(directory);

		/* directory / XLOGDIR */
		snprintf(fpath, MAXPGPATH, "%s/%s", directory, XLOGDIR);
		if (search_directory(fpath, fname))
			return pg_strdup(fpath);
	}
	else
	{
		const char *datadir;

		/* current directory */
		if (search_directory(".", fname))
			return pg_strdup(".");
		/* XLOGDIR */
		if (search_directory(XLOGDIR, fname))
			return pg_strdup(XLOGDIR);

		datadir = getenv("PGDATA");
		/* $PGDATA / XLOGDIR */
		if (datadir != NULL)
		{
			snprintf(fpath, MAXPGPATH, "%s/%s", datadir, XLOGDIR);
			if (search_directory(fpath, fname))
				return pg_strdup(fpath);
		}
	}

	/* could not locate WAL file */
	if (fname)
		fatal_error("could not locate WAL file \"%s\"", fname);
	else
		fatal_error("could not find any WAL file");

	return NULL;				/* not reached */
}

/* pg_waldump's XLogReaderRoutine->segment_open callback */
static void
WALDumpOpenSegment(XLogReaderState *state, XLogSegNo nextSegNo,
				   TimeLineID *tli_p)
{
	TimeLineID	tli = *tli_p;
	char		fname[MAXPGPATH];
	int			tries;

	XLogFileName(fname, tli, nextSegNo, state->segcxt.ws_segsize);

	/*
	 * In follow mode there is a short period of time after the server has
	 * written the end of the previous file before the new file is available.
	 * So we loop for 5 seconds looking for the file to appear before giving
	 * up.
	 */
	for (tries = 0; tries < 10; tries++)
	{
		state->seg.ws_file = open_file_in_directory(state->segcxt.ws_dir, fname);
		if (state->seg.ws_file >= 0)
			return;
		if (errno == ENOENT)
		{
			int			save_errno = errno;

			/* File not there yet, try again */
			pg_usleep(500 * 1000);

			errno = save_errno;
			continue;
		}
		/* Any other error, fall through and fail */
		break;
	}

	fatal_error("could not find file \"%s\": %m", fname);
}

/*
 * pg_waldump's XLogReaderRoutine->segment_close callback.  Same as
 * wal_segment_close
 */
static void
WALDumpCloseSegment(XLogReaderState *state)
{
	close(state->seg.ws_file);
	/* need to check errno? */
	state->seg.ws_file = -1;
}

/* pg_waldump's XLogReaderRoutine->page_read callback */
static int
WALDumpReadPage(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
				XLogRecPtr targetPtr, char *readBuff)
{
	XLogDumpPrivate *private = state->private_data;
	int			count = XLOG_BLCKSZ;
	WALReadError errinfo;

	if (private->endptr != InvalidXLogRecPtr)
	{
		if (targetPagePtr + XLOG_BLCKSZ <= private->endptr)
			count = XLOG_BLCKSZ;
		else if (targetPagePtr + reqLen <= private->endptr)
			count = private->endptr - targetPagePtr;
		else
		{
			private->endptr_reached = true;
			return -1;
		}
	}

	if (!WALRead(state, readBuff, targetPagePtr, count, private->timeline,
				 &errinfo))
	{
		WALOpenSegment *seg = &errinfo.wre_seg;
		char		fname[MAXPGPATH];

		XLogFileName(fname, seg->ws_tli, seg->ws_segno,
					 state->segcxt.ws_segsize);

		if (errinfo.wre_errno != 0)
		{
			errno = errinfo.wre_errno;
			fatal_error("could not read from file %s, offset %u: %m",
						fname, errinfo.wre_off);
		}
		else
			fatal_error("could not read from file %s, offset %u: read %d of %zu",
						fname, errinfo.wre_off, errinfo.wre_read,
						(Size) errinfo.wre_req);
	}

	return count;
}

/*
 * Calculate the size of a record, split into !FPI and FPI parts.
 */
static void
XLogDumpRecordLen(XLogReaderState *record, uint32 *rec_len, uint32 *fpi_len)
{
	int			block_id;

	/*
	 * Calculate the amount of FPI data in the record.
	 *
	 * XXX: We peek into xlogreader's private decoded backup blocks for the
	 * bimg_len indicating the length of FPI data. It doesn't seem worth it to
	 * add an accessor macro for this.
	 */
	*fpi_len = 0;
	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		if (XLogRecHasBlockImage(record, block_id))
			*fpi_len += record->blocks[block_id].bimg_len;
	}

	/*
	 * Calculate the length of the record as the total length - the length of
	 * all the block images.
	 */
	*rec_len = XLogRecGetTotalLen(record) - *fpi_len;
}

/*
 * Store per-rmgr and per-record statistics for a given record.
 */
static void
XLogDumpCountRecord(XLogDumpConfig *config, XLogDumpStats *stats,
					XLogReaderState *record)
{
	RmgrId		rmid;
	uint8		recid;
	uint32		rec_len;
	uint32		fpi_len;

	stats->count++;

	rmid = XLogRecGetRmid(record);

	XLogDumpRecordLen(record, &rec_len, &fpi_len);

	/* Update per-rmgr statistics */

	stats->rmgr_stats[rmid].count++;
	stats->rmgr_stats[rmid].rec_len += rec_len;
	stats->rmgr_stats[rmid].fpi_len += fpi_len;

	/*
	 * Update per-record statistics, where the record is identified by a
	 * combination of the RmgrId and the four bits of the xl_info field that
	 * are the rmgr's domain (resulting in sixteen possible entries per
	 * RmgrId).
	 */

	recid = XLogRecGetInfo(record) >> 4;

	stats->record_stats[rmid][recid].count++;
	stats->record_stats[rmid][recid].rec_len += rec_len;
	stats->record_stats[rmid][recid].fpi_len += fpi_len;
}

/*
 * Print a record to stdout
 */
static void
XLogDumpDisplayRecord(XLogDumpConfig *config, XLogReaderState *record)
{
	const char *id;
	const RmgrDescData *desc = &RmgrDescTable[XLogRecGetRmid(record)];
	uint32		rec_len;
	uint32		fpi_len;
	RelFileNode rnode;
	ForkNumber	forknum;
	BlockNumber blk;
	int			block_id;
	uint8		info = XLogRecGetInfo(record);
	XLogRecPtr	xl_prev = XLogRecGetPrev(record);
	StringInfoData s;

	XLogDumpRecordLen(record, &rec_len, &fpi_len);

	printf("rmgr: %-11s len (rec/tot): %6u/%6u, tx: %10u, lsn: %X/%08X, prev %X/%08X, ",
		   desc->rm_name,
		   rec_len, XLogRecGetTotalLen(record),
		   XLogRecGetXid(record),
		   LSN_FORMAT_ARGS(record->ReadRecPtr),
		   LSN_FORMAT_ARGS(xl_prev));

	id = desc->rm_identify(info);
	if (id == NULL)
		printf("desc: UNKNOWN (%x) ", info & ~XLR_INFO_MASK);
	else
		printf("desc: %s ", id);

	initStringInfo(&s);
	desc->rm_desc(&s, record);
	printf("%s", s.data);
	pfree(s.data);

	if (!config->bkp_details)
	{
		/* print block references (short format) */
		for (block_id = 0; block_id <= record->max_block_id; block_id++)
		{
			if (!XLogRecHasBlockRef(record, block_id))
				continue;

			XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blk);
			if (forknum != MAIN_FORKNUM)
				printf(", blkref #%u: rel %u/%u/%u fork %s blk %u",
					   block_id,
					   rnode.spcNode, rnode.dbNode, rnode.relNode,
					   forkNames[forknum],
					   blk);
			else
				printf(", blkref #%u: rel %u/%u/%u blk %u",
					   block_id,
					   rnode.spcNode, rnode.dbNode, rnode.relNode,
					   blk);
			if (XLogRecHasBlockImage(record, block_id))
			{
				if (XLogRecBlockImageApply(record, block_id))
					printf(" FPW");
				else
					printf(" FPW for WAL verification");
			}
		}
		putchar('\n');
	}
	else
	{
		/* print block references (detailed format) */
		putchar('\n');
		for (block_id = 0; block_id <= record->max_block_id; block_id++)
		{
			if (!XLogRecHasBlockRef(record, block_id))
				continue;

			XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blk);
			printf("\tblkref #%u: rel %u/%u/%u fork %s blk %u",
				   block_id,
				   rnode.spcNode, rnode.dbNode, rnode.relNode,
				   forkNames[forknum],
				   blk);
			if (XLogRecHasBlockImage(record, block_id))
			{
				if (record->blocks[block_id].bimg_info &
					BKPIMAGE_IS_COMPRESSED)
				{
					printf(" (FPW%s); hole: offset: %u, length: %u, "
						   "compression saved: %u",
						   XLogRecBlockImageApply(record, block_id) ?
						   "" : " for WAL verification",
						   record->blocks[block_id].hole_offset,
						   record->blocks[block_id].hole_length,
						   BLCKSZ -
						   record->blocks[block_id].hole_length -
						   record->blocks[block_id].bimg_len);
				}
				else
				{
					printf(" (FPW%s); hole: offset: %u, length: %u",
						   XLogRecBlockImageApply(record, block_id) ?
						   "" : " for WAL verification",
						   record->blocks[block_id].hole_offset,
						   record->blocks[block_id].hole_length);
				}
			}
			putchar('\n');
		}
	}
}

/*
 * Display a single row of record counts and sizes for an rmgr or record.
 */
static void
XLogDumpStatsRow(const char *name,
				 uint64 n, uint64 total_count,
				 uint64 rec_len, uint64 total_rec_len,
				 uint64 fpi_len, uint64 total_fpi_len,
				 uint64 tot_len, uint64 total_len)
{
	double		n_pct,
				rec_len_pct,
				fpi_len_pct,
				tot_len_pct;

	n_pct = 0;
	if (total_count != 0)
		n_pct = 100 * (double) n / total_count;

	rec_len_pct = 0;
	if (total_rec_len != 0)
		rec_len_pct = 100 * (double) rec_len / total_rec_len;

	fpi_len_pct = 0;
	if (total_fpi_len != 0)
		fpi_len_pct = 100 * (double) fpi_len / total_fpi_len;

	tot_len_pct = 0;
	if (total_len != 0)
		tot_len_pct = 100 * (double) tot_len / total_len;

	printf("%-27s "
		   "%20" INT64_MODIFIER "u (%6.02f) "
		   "%20" INT64_MODIFIER "u (%6.02f) "
		   "%20" INT64_MODIFIER "u (%6.02f) "
		   "%20" INT64_MODIFIER "u (%6.02f)\n",
		   name, n, n_pct, rec_len, rec_len_pct, fpi_len, fpi_len_pct,
		   tot_len, tot_len_pct);
}


/*
 * Display summary statistics about the records seen so far.
 */
static void
XLogDumpDisplayStats(XLogDumpConfig *config, XLogDumpStats *stats)
{
	int			ri,
				rj;
	uint64		total_count = 0;
	uint64		total_rec_len = 0;
	uint64		total_fpi_len = 0;
	uint64		total_len = 0;
	double		rec_len_pct,
				fpi_len_pct;

	/*
	 * Each row shows its percentages of the total, so make a first pass to
	 * calculate column totals.
	 */

	for (ri = 0; ri < RM_NEXT_ID; ri++)
	{
		total_count += stats->rmgr_stats[ri].count;
		total_rec_len += stats->rmgr_stats[ri].rec_len;
		total_fpi_len += stats->rmgr_stats[ri].fpi_len;
	}
	total_len = total_rec_len + total_fpi_len;

	/*
	 * 27 is strlen("Transaction/COMMIT_PREPARED"), 20 is strlen(2^64), 8 is
	 * strlen("(100.00%)")
	 */

	printf("%-27s %20s %8s %20s %8s %20s %8s %20s %8s\n"
		   "%-27s %20s %8s %20s %8s %20s %8s %20s %8s\n",
		   "Type", "N", "(%)", "Record size", "(%)", "FPI size", "(%)", "Combined size", "(%)",
		   "----", "-", "---", "-----------", "---", "--------", "---", "-------------", "---");

	for (ri = 0; ri < RM_NEXT_ID; ri++)
	{
		uint64		count,
					rec_len,
					fpi_len,
					tot_len;
		const RmgrDescData *desc = &RmgrDescTable[ri];

		if (!config->stats_per_record)
		{
			count = stats->rmgr_stats[ri].count;
			rec_len = stats->rmgr_stats[ri].rec_len;
			fpi_len = stats->rmgr_stats[ri].fpi_len;
			tot_len = rec_len + fpi_len;

			XLogDumpStatsRow(desc->rm_name,
							 count, total_count, rec_len, total_rec_len,
							 fpi_len, total_fpi_len, tot_len, total_len);
		}
		else
		{
			for (rj = 0; rj < MAX_XLINFO_TYPES; rj++)
			{
				const char *id;

				count = stats->record_stats[ri][rj].count;
				rec_len = stats->record_stats[ri][rj].rec_len;
				fpi_len = stats->record_stats[ri][rj].fpi_len;
				tot_len = rec_len + fpi_len;

				/* Skip undefined combinations and ones that didn't occur */
				if (count == 0)
					continue;

				/* the upper four bits in xl_info are the rmgr's */
				id = desc->rm_identify(rj << 4);
				if (id == NULL)
					id = psprintf("UNKNOWN (%x)", rj << 4);

				XLogDumpStatsRow(psprintf("%s/%s", desc->rm_name, id),
								 count, total_count, rec_len, total_rec_len,
								 fpi_len, total_fpi_len, tot_len, total_len);
			}
		}
	}

	printf("%-27s %20s %8s %20s %8s %20s %8s %20s\n",
		   "", "--------", "", "--------", "", "--------", "", "--------");

	/*
	 * The percentages in earlier rows were calculated against the column
	 * total, but the ones that follow are against the row total. Note that
	 * these are displayed with a % symbol to differentiate them from the
	 * earlier ones, and are thus up to 9 characters long.
	 */

	rec_len_pct = 0;
	if (total_len != 0)
		rec_len_pct = 100 * (double) total_rec_len / total_len;

	fpi_len_pct = 0;
	if (total_len != 0)
		fpi_len_pct = 100 * (double) total_fpi_len / total_len;

	printf("%-27s "
		   "%20" INT64_MODIFIER "u %-9s"
		   "%20" INT64_MODIFIER "u %-9s"
		   "%20" INT64_MODIFIER "u %-9s"
		   "%20" INT64_MODIFIER "u %-6s\n",
		   "Total", stats->count, "",
		   total_rec_len, psprintf("[%.02f%%]", rec_len_pct),
		   total_fpi_len, psprintf("[%.02f%%]", fpi_len_pct),
		   total_len, "[100%]");
}

static void
usage(void)
{
	printf(_("%s decodes and displays PostgreSQL write-ahead logs for debugging.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [STARTSEG [ENDSEG]]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -b, --bkp-details      output detailed information about backup blocks\n"));
	printf(_("  -e, --end=RECPTR       stop reading at WAL location RECPTR\n"));
	printf(_("  -f, --follow           keep retrying after reaching end of WAL\n"));
	printf(_("  -n, --limit=N          number of records to display\n"));
	printf(_("  -p, --path=PATH        directory in which to find log segment files or a\n"
			 "                         directory with a ./pg_wal that contains such files\n"
			 "                         (default: current directory, ./pg_wal, $PGDATA/pg_wal)\n"));
	printf(_("  -q, --quiet            do not print any output, except for errors\n"));
	printf(_("  -r, --rmgr=RMGR        only show records generated by resource manager RMGR;\n"
			 "                         use --rmgr=list to list valid resource manager names\n"));
	printf(_("  -s, --start=RECPTR     start reading at WAL location RECPTR\n"));
	printf(_("  -t, --timeline=TLI     timeline from which to read log records\n"
			 "                         (default: 1 or the value used in STARTSEG)\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -x, --xid=XID          only show records with transaction ID XID\n"));
	printf(_("  -z, --stats[=record]   show statistics instead of records\n"
			 "                         (optionally, show per-record statistics)\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

int
main(int argc, char **argv)
{
	uint32		xlogid;
	uint32		xrecoff;
	XLogReaderState *xlogreader_state;
	XLogDumpPrivate private;
	XLogDumpConfig config;
	XLogDumpStats stats;
	XLogRecord *record;
	XLogRecPtr	first_record;
	char	   *waldir = NULL;
	char	   *errormsg;

	static struct option long_options[] = {
		{"bkp-details", no_argument, NULL, 'b'},
		{"end", required_argument, NULL, 'e'},
		{"follow", no_argument, NULL, 'f'},
		{"help", no_argument, NULL, '?'},
		{"limit", required_argument, NULL, 'n'},
		{"path", required_argument, NULL, 'p'},
		{"quiet", no_argument, NULL, 'q'},
		{"rmgr", required_argument, NULL, 'r'},
		{"start", required_argument, NULL, 's'},
		{"timeline", required_argument, NULL, 't'},
		{"xid", required_argument, NULL, 'x'},
		{"version", no_argument, NULL, 'V'},
		{"stats", optional_argument, NULL, 'z'},
		{NULL, 0, NULL, 0}
	};

	int			option;
	int			optindex = 0;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_waldump"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_waldump (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	memset(&private, 0, sizeof(XLogDumpPrivate));
	memset(&config, 0, sizeof(XLogDumpConfig));
	memset(&stats, 0, sizeof(XLogDumpStats));

	private.timeline = 1;
	private.startptr = InvalidXLogRecPtr;
	private.endptr = InvalidXLogRecPtr;
	private.endptr_reached = false;

	config.quiet = false;
	config.bkp_details = false;
	config.stop_after_records = -1;
	config.already_displayed_records = 0;
	config.follow = false;
	config.filter_by_rmgr = -1;
	config.filter_by_xid = InvalidTransactionId;
	config.filter_by_xid_enabled = false;
	config.stats = false;
	config.stats_per_record = false;

	if (argc <= 1)
	{
		pg_log_error("no arguments specified");
		goto bad_argument;
	}

	while ((option = getopt_long(argc, argv, "be:fn:p:qr:s:t:x:z",
								 long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'b':
				config.bkp_details = true;
				break;
			case 'e':
				if (sscanf(optarg, "%X/%X", &xlogid, &xrecoff) != 2)
				{
					pg_log_error("could not parse end WAL location \"%s\"",
								 optarg);
					goto bad_argument;
				}
				private.endptr = (uint64) xlogid << 32 | xrecoff;
				break;
			case 'f':
				config.follow = true;
				break;
			case 'n':
				if (sscanf(optarg, "%d", &config.stop_after_records) != 1)
				{
					pg_log_error("could not parse limit \"%s\"", optarg);
					goto bad_argument;
				}
				break;
			case 'p':
				waldir = pg_strdup(optarg);
				break;
			case 'q':
				config.quiet = true;
				break;
			case 'r':
				{
					int			i;

					if (pg_strcasecmp(optarg, "list") == 0)
					{
						print_rmgr_list();
						exit(EXIT_SUCCESS);
					}

					for (i = 0; i <= RM_MAX_ID; i++)
					{
						if (pg_strcasecmp(optarg, RmgrDescTable[i].rm_name) == 0)
						{
							config.filter_by_rmgr = i;
							break;
						}
					}

					if (config.filter_by_rmgr == -1)
					{
						pg_log_error("resource manager \"%s\" does not exist",
									 optarg);
						goto bad_argument;
					}
				}
				break;
			case 's':
				if (sscanf(optarg, "%X/%X", &xlogid, &xrecoff) != 2)
				{
					pg_log_error("could not parse start WAL location \"%s\"",
								 optarg);
					goto bad_argument;
				}
				else
					private.startptr = (uint64) xlogid << 32 | xrecoff;
				break;
			case 't':
				if (sscanf(optarg, "%d", &private.timeline) != 1)
				{
					pg_log_error("could not parse timeline \"%s\"", optarg);
					goto bad_argument;
				}
				break;
			case 'x':
				if (sscanf(optarg, "%u", &config.filter_by_xid) != 1)
				{
					pg_log_error("could not parse \"%s\" as a transaction ID",
								 optarg);
					goto bad_argument;
				}
				config.filter_by_xid_enabled = true;
				break;
			case 'z':
				config.stats = true;
				config.stats_per_record = false;
				if (optarg)
				{
					if (strcmp(optarg, "record") == 0)
						config.stats_per_record = true;
					else if (strcmp(optarg, "rmgr") != 0)
					{
						pg_log_error("unrecognized argument to --stats: %s",
									 optarg);
						goto bad_argument;
					}
				}
				break;
			default:
				goto bad_argument;
		}
	}

	if ((optind + 2) < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind + 2]);
		goto bad_argument;
	}

	if (waldir != NULL)
	{
		/* validate path points to directory */
		if (!verify_directory(waldir))
		{
			pg_log_error("could not open directory \"%s\": %m", waldir);
			goto bad_argument;
		}
	}

	/* parse files as start/end boundaries, extract path if not specified */
	if (optind < argc)
	{
		char	   *directory = NULL;
		char	   *fname = NULL;
		int			fd;
		XLogSegNo	segno;

		split_path(argv[optind], &directory, &fname);

		if (waldir == NULL && directory != NULL)
		{
			waldir = directory;

			if (!verify_directory(waldir))
				fatal_error("could not open directory \"%s\": %m", waldir);
		}

		waldir = identify_target_directory(waldir, fname);
		fd = open_file_in_directory(waldir, fname);
		if (fd < 0)
			fatal_error("could not open file \"%s\"", fname);
		close(fd);

		/* parse position from file */
		XLogFromFileName(fname, &private.timeline, &segno, WalSegSz);

		if (XLogRecPtrIsInvalid(private.startptr))
			XLogSegNoOffsetToRecPtr(segno, 0, WalSegSz, private.startptr);
		else if (!XLByteInSeg(private.startptr, segno, WalSegSz))
		{
			pg_log_error("start WAL location %X/%X is not inside file \"%s\"",
						 LSN_FORMAT_ARGS(private.startptr),
						 fname);
			goto bad_argument;
		}

		/* no second file specified, set end position */
		if (!(optind + 1 < argc) && XLogRecPtrIsInvalid(private.endptr))
			XLogSegNoOffsetToRecPtr(segno + 1, 0, WalSegSz, private.endptr);

		/* parse ENDSEG if passed */
		if (optind + 1 < argc)
		{
			XLogSegNo	endsegno;

			/* ignore directory, already have that */
			split_path(argv[optind + 1], &directory, &fname);

			fd = open_file_in_directory(waldir, fname);
			if (fd < 0)
				fatal_error("could not open file \"%s\"", fname);
			close(fd);

			/* parse position from file */
			XLogFromFileName(fname, &private.timeline, &endsegno, WalSegSz);

			if (endsegno < segno)
				fatal_error("ENDSEG %s is before STARTSEG %s",
							argv[optind + 1], argv[optind]);

			if (XLogRecPtrIsInvalid(private.endptr))
				XLogSegNoOffsetToRecPtr(endsegno + 1, 0, WalSegSz,
										private.endptr);

			/* set segno to endsegno for check of --end */
			segno = endsegno;
		}


		if (!XLByteInSeg(private.endptr, segno, WalSegSz) &&
			private.endptr != (segno + 1) * WalSegSz)
		{
			pg_log_error("end WAL location %X/%X is not inside file \"%s\"",
						 LSN_FORMAT_ARGS(private.endptr),
						 argv[argc - 1]);
			goto bad_argument;
		}
	}
	else
		waldir = identify_target_directory(waldir, NULL);

	/* we don't know what to print */
	if (XLogRecPtrIsInvalid(private.startptr))
	{
		pg_log_error("no start WAL location given");
		goto bad_argument;
	}

	/* done with argument parsing, do the actual work */

	/* we have everything we need, start reading */
	xlogreader_state =
		XLogReaderAllocate(WalSegSz, waldir,
						   XL_ROUTINE(.page_read = WALDumpReadPage,
									  .segment_open = WALDumpOpenSegment,
									  .segment_close = WALDumpCloseSegment),
						   &private);
	if (!xlogreader_state)
		fatal_error("out of memory");

	/* first find a valid recptr to start from */
	first_record = XLogFindNextRecord(xlogreader_state, private.startptr);

	if (first_record == InvalidXLogRecPtr)
		fatal_error("could not find a valid record after %X/%X",
					LSN_FORMAT_ARGS(private.startptr));

	/*
	 * Display a message that we're skipping data if `from` wasn't a pointer
	 * to the start of a record and also wasn't a pointer to the beginning of
	 * a segment (e.g. we were used in file mode).
	 */
	if (first_record != private.startptr &&
		XLogSegmentOffset(private.startptr, WalSegSz) != 0)
		printf(ngettext("first record is after %X/%X, at %X/%X, skipping over %u byte\n",
						"first record is after %X/%X, at %X/%X, skipping over %u bytes\n",
						(first_record - private.startptr)),
			   LSN_FORMAT_ARGS(private.startptr),
			   LSN_FORMAT_ARGS(first_record),
			   (uint32) (first_record - private.startptr));

	for (;;)
	{
		/* try to read the next record */
		record = XLogReadRecord(xlogreader_state, &errormsg);
		if (!record)
		{
			if (!config.follow || private.endptr_reached)
				break;
			else
			{
				pg_usleep(1000000L);	/* 1 second */
				continue;
			}
		}

		/* apply all specified filters */
		if (config.filter_by_rmgr != -1 &&
			config.filter_by_rmgr != record->xl_rmid)
			continue;

		if (config.filter_by_xid_enabled &&
			config.filter_by_xid != record->xl_xid)
			continue;

		/* perform any per-record work */
		if (!config.quiet)
		{
			if (config.stats == true)
				XLogDumpCountRecord(&config, &stats, xlogreader_state);
			else
				XLogDumpDisplayRecord(&config, xlogreader_state);
		}

		/* check whether we printed enough */
		config.already_displayed_records++;
		if (config.stop_after_records > 0 &&
			config.already_displayed_records >= config.stop_after_records)
			break;
	}

	if (config.stats == true && !config.quiet)
		XLogDumpDisplayStats(&config, &stats);

	if (errormsg)
		fatal_error("error in WAL record at %X/%X: %s",
					LSN_FORMAT_ARGS(xlogreader_state->ReadRecPtr),
					errormsg);

	XLogReaderFree(xlogreader_state);

	return EXIT_SUCCESS;

bad_argument:
	fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
	return EXIT_FAILURE;
}
