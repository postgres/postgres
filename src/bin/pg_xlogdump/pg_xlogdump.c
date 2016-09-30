/*-------------------------------------------------------------------------
 *
 * pg_xlogdump.c - decode and display WAL
 *
 * Copyright (c) 2013-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_xlogdump/pg_xlogdump.c
 *-------------------------------------------------------------------------
 */

#define FRONTEND 1
#include "postgres.h"

#include <dirent.h>
#include <unistd.h>

#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "access/xlog_internal.h"
#include "access/transam.h"
#include "common/fe_memutils.h"
#include "getopt_long.h"
#include "rmgrdesc.h"


static const char *progname;

typedef struct XLogDumpPrivate
{
	TimeLineID	timeline;
	char	   *inpath;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	bool		endptr_reached;
} XLogDumpPrivate;

typedef struct XLogDumpConfig
{
	/* display options */
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

static void fatal_error(const char *fmt,...) pg_attribute_printf(1, 2);

/*
 * Big red button to push when things go horribly wrong.
 */
static void
fatal_error(const char *fmt,...)
{
	va_list		args;

	fflush(stdout);

	fprintf(stderr, "%s: FATAL:  ", progname);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);

	exit(EXIT_FAILURE);
}

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
		*dir = pg_strdup(path);
		(*dir)[(sep - path) + 1] = '\0';		/* no strndup */
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
 * Try to find the file in several places:
 * if directory == NULL:
 *	 fname
 *	 XLOGDIR / fname
 *	 $PGDATA / XLOGDIR / fname
 * else
 *	 directory / fname
 *	 directory / XLOGDIR / fname
 *
 * return a read only fd
 */
static int
fuzzy_open_file(const char *directory, const char *fname)
{
	int			fd = -1;
	char		fpath[MAXPGPATH];

	if (directory == NULL)
	{
		const char *datadir;

		/* fname */
		fd = open(fname, O_RDONLY | PG_BINARY, 0);
		if (fd < 0 && errno != ENOENT)
			return -1;
		else if (fd >= 0)
			return fd;

		/* XLOGDIR / fname */
		snprintf(fpath, MAXPGPATH, "%s/%s",
				 XLOGDIR, fname);
		fd = open(fpath, O_RDONLY | PG_BINARY, 0);
		if (fd < 0 && errno != ENOENT)
			return -1;
		else if (fd >= 0)
			return fd;

		datadir = getenv("PGDATA");
		/* $PGDATA / XLOGDIR / fname */
		if (datadir != NULL)
		{
			snprintf(fpath, MAXPGPATH, "%s/%s/%s",
					 datadir, XLOGDIR, fname);
			fd = open(fpath, O_RDONLY | PG_BINARY, 0);
			if (fd < 0 && errno != ENOENT)
				return -1;
			else if (fd >= 0)
				return fd;
		}
	}
	else
	{
		/* directory / fname */
		snprintf(fpath, MAXPGPATH, "%s/%s",
				 directory, fname);
		fd = open(fpath, O_RDONLY | PG_BINARY, 0);
		if (fd < 0 && errno != ENOENT)
			return -1;
		else if (fd >= 0)
			return fd;

		/* directory / XLOGDIR / fname */
		snprintf(fpath, MAXPGPATH, "%s/%s/%s",
				 directory, XLOGDIR, fname);
		fd = open(fpath, O_RDONLY | PG_BINARY, 0);
		if (fd < 0 && errno != ENOENT)
			return -1;
		else if (fd >= 0)
			return fd;
	}
	return -1;
}

/*
 * Read count bytes from a segment file in the specified directory, for the
 * given timeline, containing the specified record pointer; store the data in
 * the passed buffer.
 */
static void
XLogDumpXLogRead(const char *directory, TimeLineID timeline_id,
				 XLogRecPtr startptr, char *buf, Size count)
{
	char	   *p;
	XLogRecPtr	recptr;
	Size		nbytes;

	static int	sendFile = -1;
	static XLogSegNo sendSegNo = 0;
	static uint32 sendOff = 0;

	p = buf;
	recptr = startptr;
	nbytes = count;

	while (nbytes > 0)
	{
		uint32		startoff;
		int			segbytes;
		int			readbytes;

		startoff = recptr % XLogSegSize;

		if (sendFile < 0 || !XLByteInSeg(recptr, sendSegNo))
		{
			char		fname[MAXFNAMELEN];
			int			tries;

			/* Switch to another logfile segment */
			if (sendFile >= 0)
				close(sendFile);

			XLByteToSeg(recptr, sendSegNo);

			XLogFileName(fname, timeline_id, sendSegNo);

			/*
			 * In follow mode there is a short period of time after the
			 * server has written the end of the previous file before the
			 * new file is available. So we loop for 5 seconds looking
			 * for the file to appear before giving up.
			 */
			for (tries = 0; tries < 10; tries++)
			{
				sendFile = fuzzy_open_file(directory, fname);
				if (sendFile >= 0)
					break;
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

			if (sendFile < 0)
				fatal_error("could not find file \"%s\": %s",
							fname, strerror(errno));
			sendOff = 0;
		}

		/* Need to seek in the file? */
		if (sendOff != startoff)
		{
			if (lseek(sendFile, (off_t) startoff, SEEK_SET) < 0)
			{
				int			err = errno;
				char		fname[MAXPGPATH];

				XLogFileName(fname, timeline_id, sendSegNo);

				fatal_error("could not seek in log segment %s to offset %u: %s",
							fname, startoff, strerror(err));
			}
			sendOff = startoff;
		}

		/* How many bytes are within this segment? */
		if (nbytes > (XLogSegSize - startoff))
			segbytes = XLogSegSize - startoff;
		else
			segbytes = nbytes;

		readbytes = read(sendFile, p, segbytes);
		if (readbytes <= 0)
		{
			int			err = errno;
			char		fname[MAXPGPATH];

			XLogFileName(fname, timeline_id, sendSegNo);

			fatal_error("could not read from log segment %s, offset %d, length %d: %s",
						fname, sendOff, segbytes, strerror(err));
		}

		/* Update state for read */
		recptr += readbytes;

		sendOff += readbytes;
		nbytes -= readbytes;
		p += readbytes;
	}
}

/*
 * XLogReader read_page callback
 */
static int
XLogDumpReadPage(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
				 XLogRecPtr targetPtr, char *readBuff, TimeLineID *curFileTLI)
{
	XLogDumpPrivate *private = state->private_data;
	int			count = XLOG_BLCKSZ;

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

	XLogDumpXLogRead(private->inpath, private->timeline, targetPagePtr,
					 readBuff, count);

	return count;
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
	int			block_id;

	stats->count++;

	rmid = XLogRecGetRmid(record);
	rec_len = XLogRecGetDataLen(record) + SizeOfXLogRecord;

	/*
	 * Calculate the amount of FPI data in the record.
	 *
	 * XXX: We peek into xlogreader's private decoded backup blocks for the
	 * bimg_len indicating the length of FPI data. It doesn't seem worth it to
	 * add an accessor macro for this.
	 */
	fpi_len = 0;
	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		if (XLogRecHasBlockImage(record, block_id))
			fpi_len += record->blocks[block_id].bimg_len;
	}

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
	RelFileNode rnode;
	ForkNumber	forknum;
	BlockNumber blk;
	int			block_id;
	uint8		info = XLogRecGetInfo(record);
	XLogRecPtr	xl_prev = XLogRecGetPrev(record);

	id = desc->rm_identify(info);
	if (id == NULL)
		id = psprintf("UNKNOWN (%x)", info & ~XLR_INFO_MASK);

	printf("rmgr: %-11s len (rec/tot): %6u/%6u, tx: %10u, lsn: %X/%08X, prev %X/%08X, ",
		   desc->rm_name,
		   XLogRecGetDataLen(record), XLogRecGetTotalLen(record),
		   XLogRecGetXid(record),
		   (uint32) (record->ReadRecPtr >> 32), (uint32) record->ReadRecPtr,
		   (uint32) (xl_prev >> 32), (uint32) xl_prev);
	printf("desc: %s ", id);

	/* the desc routine will printf the description directly to stdout */
	desc->rm_desc(NULL, record);

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
				printf(" FPW");
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
					printf(" (FPW); hole: offset: %u, length: %u, compression saved: %u\n",
						   record->blocks[block_id].hole_offset,
						   record->blocks[block_id].hole_length,
						   BLCKSZ -
						   record->blocks[block_id].hole_length -
						   record->blocks[block_id].bimg_len);
				}
				else
				{
					printf(" (FPW); hole: offset: %u, length: %u\n",
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

	/* ---
	 * Make a first pass to calculate column totals:
	 * count(*),
	 * sum(xl_len+SizeOfXLogRecord),
	 * sum(xl_tot_len-xl_len-SizeOfXLogRecord), and
	 * sum(xl_tot_len).
	 * These are used to calculate percentages for each record type.
	 * ---
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
	printf("%s decodes and displays PostgreSQL transaction logs for debugging.\n\n",
		   progname);
	printf("Usage:\n");
	printf("  %s [OPTION]... [STARTSEG [ENDSEG]] \n", progname);
	printf("\nOptions:\n");
	printf("  -b, --bkp-details      output detailed information about backup blocks\n");
	printf("  -e, --end=RECPTR       stop reading at log position RECPTR\n");
	printf("  -f, --follow           keep retrying after reaching end of WAL\n");
	printf("  -n, --limit=N          number of records to display\n");
	printf("  -p, --path=PATH        directory in which to find log segment files\n");
	printf("                         (default: ./pg_xlog)\n");
	printf("  -r, --rmgr=RMGR        only show records generated by resource manager RMGR\n");
	printf("                         use --rmgr=list to list valid resource manager names\n");
	printf("  -s, --start=RECPTR     start reading at log position RECPTR\n");
	printf("  -t, --timeline=TLI     timeline from which to read log records\n");
	printf("                         (default: 1 or the value used in STARTSEG)\n");
	printf("  -V, --version          output version information, then exit\n");
	printf("  -x, --xid=XID          only show records with TransactionId XID\n");
	printf("  -z, --stats[=record]   show statistics instead of records\n");
	printf("                         (optionally, show per-record statistics)\n");
	printf("  -?, --help             show this help, then exit\n");
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
	char	   *errormsg;

	static struct option long_options[] = {
		{"bkp-details", no_argument, NULL, 'b'},
		{"end", required_argument, NULL, 'e'},
		{"follow", no_argument, NULL, 'f'},
		{"help", no_argument, NULL, '?'},
		{"limit", required_argument, NULL, 'n'},
		{"path", required_argument, NULL, 'p'},
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

	progname = get_progname(argv[0]);

	memset(&private, 0, sizeof(XLogDumpPrivate));
	memset(&config, 0, sizeof(XLogDumpConfig));
	memset(&stats, 0, sizeof(XLogDumpStats));

	private.timeline = 1;
	private.startptr = InvalidXLogRecPtr;
	private.endptr = InvalidXLogRecPtr;
	private.endptr_reached = false;

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
		fprintf(stderr, "%s: no arguments specified\n", progname);
		goto bad_argument;
	}

	while ((option = getopt_long(argc, argv, "be:?fn:p:r:s:t:Vx:z",
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
					fprintf(stderr, "%s: could not parse end log position \"%s\"\n",
							progname, optarg);
					goto bad_argument;
				}
				private.endptr = (uint64) xlogid << 32 | xrecoff;
				break;
			case 'f':
				config.follow = true;
				break;
			case '?':
				usage();
				exit(EXIT_SUCCESS);
				break;
			case 'n':
				if (sscanf(optarg, "%d", &config.stop_after_records) != 1)
				{
					fprintf(stderr, "%s: could not parse limit \"%s\"\n",
							progname, optarg);
					goto bad_argument;
				}
				break;
			case 'p':
				private.inpath = pg_strdup(optarg);
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
						fprintf(stderr, "%s: resource manager \"%s\" does not exist\n",
								progname, optarg);
						goto bad_argument;
					}
				}
				break;
			case 's':
				if (sscanf(optarg, "%X/%X", &xlogid, &xrecoff) != 2)
				{
					fprintf(stderr, "%s: could not parse start log position \"%s\"\n",
							progname, optarg);
					goto bad_argument;
				}
				else
					private.startptr = (uint64) xlogid << 32 | xrecoff;
				break;
			case 't':
				if (sscanf(optarg, "%d", &private.timeline) != 1)
				{
					fprintf(stderr, "%s: could not parse timeline \"%s\"\n",
							progname, optarg);
					goto bad_argument;
				}
				break;
			case 'V':
				puts("pg_xlogdump (PostgreSQL) " PG_VERSION);
				exit(EXIT_SUCCESS);
				break;
			case 'x':
				if (sscanf(optarg, "%u", &config.filter_by_xid) != 1)
				{
					fprintf(stderr, "%s: could not parse \"%s\" as a valid xid\n",
							progname, optarg);
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
						fprintf(stderr, "%s: unrecognised argument to --stats: %s\n",
								progname, optarg);
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
		fprintf(stderr,
				"%s: too many command-line arguments (first is \"%s\")\n",
				progname, argv[optind + 2]);
		goto bad_argument;
	}

	if (private.inpath != NULL)
	{
		/* validate path points to directory */
		if (!verify_directory(private.inpath))
		{
			fprintf(stderr,
					"%s: path \"%s\" cannot be opened: %s\n",
					progname, private.inpath, strerror(errno));
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

		if (private.inpath == NULL && directory != NULL)
		{
			private.inpath = directory;

			if (!verify_directory(private.inpath))
				fatal_error("cannot open directory \"%s\": %s",
							private.inpath, strerror(errno));
		}

		fd = fuzzy_open_file(private.inpath, fname);
		if (fd < 0)
			fatal_error("could not open file \"%s\"", fname);
		close(fd);

		/* parse position from file */
		XLogFromFileName(fname, &private.timeline, &segno);

		if (XLogRecPtrIsInvalid(private.startptr))
			XLogSegNoOffsetToRecPtr(segno, 0, private.startptr);
		else if (!XLByteInSeg(private.startptr, segno))
		{
			fprintf(stderr,
				  "%s: start log position %X/%X is not inside file \"%s\"\n",
					progname,
					(uint32) (private.startptr >> 32),
					(uint32) private.startptr,
					fname);
			goto bad_argument;
		}

		/* no second file specified, set end position */
		if (!(optind + 1 < argc) && XLogRecPtrIsInvalid(private.endptr))
			XLogSegNoOffsetToRecPtr(segno + 1, 0, private.endptr);

		/* parse ENDSEG if passed */
		if (optind + 1 < argc)
		{
			XLogSegNo	endsegno;

			/* ignore directory, already have that */
			split_path(argv[optind + 1], &directory, &fname);

			fd = fuzzy_open_file(private.inpath, fname);
			if (fd < 0)
				fatal_error("could not open file \"%s\"", fname);
			close(fd);

			/* parse position from file */
			XLogFromFileName(fname, &private.timeline, &endsegno);

			if (endsegno < segno)
				fatal_error("ENDSEG %s is before STARTSEG %s",
							argv[optind + 1], argv[optind]);

			if (XLogRecPtrIsInvalid(private.endptr))
				XLogSegNoOffsetToRecPtr(endsegno + 1, 0, private.endptr);

			/* set segno to endsegno for check of --end */
			segno = endsegno;
		}


		if (!XLByteInSeg(private.endptr, segno) &&
			private.endptr != (segno + 1) * XLogSegSize)
		{
			fprintf(stderr,
					"%s: end log position %X/%X is not inside file \"%s\"\n",
					progname,
					(uint32) (private.endptr >> 32),
					(uint32) private.endptr,
					argv[argc - 1]);
			goto bad_argument;
		}
	}

	/* we don't know what to print */
	if (XLogRecPtrIsInvalid(private.startptr))
	{
		fprintf(stderr, "%s: no start log position given.\n", progname);
		goto bad_argument;
	}

	/* done with argument parsing, do the actual work */

	/* we have everything we need, start reading */
	xlogreader_state = XLogReaderAllocate(XLogDumpReadPage, &private);
	if (!xlogreader_state)
		fatal_error("out of memory");

	/* first find a valid recptr to start from */
	first_record = XLogFindNextRecord(xlogreader_state, private.startptr);

	if (first_record == InvalidXLogRecPtr)
		fatal_error("could not find a valid record after %X/%X",
					(uint32) (private.startptr >> 32),
					(uint32) private.startptr);

	/*
	 * Display a message that we're skipping data if `from` wasn't a pointer
	 * to the start of a record and also wasn't a pointer to the beginning of
	 * a segment (e.g. we were used in file mode).
	 */
	if (first_record != private.startptr && (private.startptr % XLogSegSize) != 0)
		printf("first record is after %X/%X, at %X/%X, skipping over %u bytes\n",
			   (uint32) (private.startptr >> 32), (uint32) private.startptr,
			   (uint32) (first_record >> 32), (uint32) first_record,
			   (uint32) (first_record - private.startptr));

	for (;;)
	{
		/* try to read the next record */
		record = XLogReadRecord(xlogreader_state, first_record, &errormsg);
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

		/* after reading the first record, continue at next one */
		first_record = InvalidXLogRecPtr;

		/* apply all specified filters */
		if (config.filter_by_rmgr != -1 &&
			config.filter_by_rmgr != record->xl_rmid)
			continue;

		if (config.filter_by_xid_enabled &&
			config.filter_by_xid != record->xl_xid)
			continue;

		/* process the record */
		if (config.stats == true)
			XLogDumpCountRecord(&config, &stats, xlogreader_state);
		else
			XLogDumpDisplayRecord(&config, xlogreader_state);

		/* check whether we printed enough */
		config.already_displayed_records++;
		if (config.stop_after_records > 0 &&
			config.already_displayed_records >= config.stop_after_records)
			break;
	}

	if (config.stats == true)
		XLogDumpDisplayStats(&config, &stats);

	if (errormsg)
		fatal_error("error in WAL record at %X/%X: %s\n",
					(uint32) (xlogreader_state->ReadRecPtr >> 32),
					(uint32) xlogreader_state->ReadRecPtr,
					errormsg);

	XLogReaderFree(xlogreader_state);

	return EXIT_SUCCESS;

bad_argument:
	fprintf(stderr, "Try \"%s --help\" for more information.\n", progname);
	return EXIT_FAILURE;
}
