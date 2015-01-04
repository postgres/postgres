/*-------------------------------------------------------------------------
 *
 * pg_xlogdump.c - decode and display WAL
 *
 * Copyright (c) 2013-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/pg_xlogdump/pg_xlogdump.c
 *-------------------------------------------------------------------------
 */

#define FRONTEND 1
#include "postgres.h"

#include <dirent.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xlogreader.h"
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

	/* filter options */
	int			filter_by_rmgr;
	TransactionId filter_by_xid;
	bool		filter_by_xid_enabled;
} XLogDumpConfig;

static void
fatal_error(const char *fmt,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 1, 2)));

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

			/* Switch to another logfile segment */
			if (sendFile >= 0)
				close(sendFile);

			XLByteToSeg(recptr, sendSegNo);

			XLogFileName(fname, timeline_id, sendSegNo);

			sendFile = fuzzy_open_file(directory, fname);

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
 * Print a record to stdout
 */
static void
XLogDumpDisplayRecord(XLogDumpConfig *config, XLogRecPtr ReadRecPtr, XLogRecord *record)
{
	const RmgrDescData *desc = &RmgrDescTable[record->xl_rmid];

	if (config->filter_by_rmgr != -1 &&
		config->filter_by_rmgr != record->xl_rmid)
		return;

	if (config->filter_by_xid_enabled &&
		config->filter_by_xid != record->xl_xid)
		return;

	config->already_displayed_records++;

	printf("rmgr: %-11s len (rec/tot): %6u/%6u, tx: %10u, lsn: %X/%08X, prev %X/%08X, bkp: %u%u%u%u, desc: ",
		   desc->rm_name,
		   record->xl_len, record->xl_tot_len,
		   record->xl_xid,
		   (uint32) (ReadRecPtr >> 32), (uint32) ReadRecPtr,
		   (uint32) (record->xl_prev >> 32), (uint32) record->xl_prev,
		   !!(XLR_BKP_BLOCK(0) & record->xl_info),
		   !!(XLR_BKP_BLOCK(1) & record->xl_info),
		   !!(XLR_BKP_BLOCK(2) & record->xl_info),
		   !!(XLR_BKP_BLOCK(3) & record->xl_info));

	/* the desc routine will printf the description directly to stdout */
	desc->rm_desc(NULL, record->xl_info, XLogRecGetData(record));

	putchar('\n');

	if (config->bkp_details)
	{
		int			bkpnum;
		char	   *blk = (char *) XLogRecGetData(record) + record->xl_len;

		for (bkpnum = 0; bkpnum < XLR_MAX_BKP_BLOCKS; bkpnum++)
		{
			BkpBlock	bkpb;

			if (!(XLR_BKP_BLOCK(bkpnum) & record->xl_info))
				continue;

			memcpy(&bkpb, blk, sizeof(BkpBlock));
			blk += sizeof(BkpBlock);
			blk += BLCKSZ - bkpb.hole_length;

			printf("\tbackup bkp #%u; rel %u/%u/%u; fork: %s; block: %u; hole: offset: %u, length: %u\n",
				   bkpnum,
				   bkpb.node.spcNode, bkpb.node.dbNode, bkpb.node.relNode,
				   forkNames[bkpb.fork],
				   bkpb.block, bkpb.hole_offset, bkpb.hole_length);
		}
	}
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
		{NULL, 0, NULL, 0}
	};

	int			option;
	int			optindex = 0;

	progname = get_progname(argv[0]);

	memset(&private, 0, sizeof(XLogDumpPrivate));
	memset(&config, 0, sizeof(XLogDumpConfig));

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

	if (argc <= 1)
	{
		fprintf(stderr, "%s: no arguments specified\n", progname);
		goto bad_argument;
	}

	while ((option = getopt_long(argc, argv, "be:?fn:p:r:s:t:Vx:",
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
					"%s: path \"%s\" cannot be opened: %s",
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
		fprintf(stderr, "%s: no start log position given in range mode.\n", progname);
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
		XLogDumpDisplayRecord(&config, xlogreader_state->ReadRecPtr, record);

		/* check whether we printed enough */
		if (config.stop_after_records > 0 &&
			config.already_displayed_records >= config.stop_after_records)
			break;
	}

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
