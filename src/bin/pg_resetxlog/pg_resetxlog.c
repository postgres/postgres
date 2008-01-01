/*-------------------------------------------------------------------------
 *
 * pg_resetxlog.c
 *	  A utility to "zero out" the xlog when it's corrupt beyond recovery.
 *	  Can also rebuild pg_control if needed.
 *
 * The theory of operation is fairly simple:
 *	  1. Read the existing pg_control (which will include the last
 *		 checkpoint record).  If it is an old format then update to
 *		 current format.
 *	  2. If pg_control is corrupt, attempt to intuit reasonable values,
 *		 by scanning the old xlog if necessary.
 *	  3. Modify pg_control to reflect a "shutdown" state with a checkpoint
 *		 record at the start of xlog.
 *	  4. Flush the existing xlog files and write a new segment with
 *		 just a checkpoint record in it.  The new segment is positioned
 *		 just past the end of the old xlog, so that existing LSNs in
 *		 data pages will appear to be "in the past".
 * This is all pretty straightforward except for the intuition part of
 * step 2 ...
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/pg_resetxlog/pg_resetxlog.c,v 1.63 2008/01/01 19:45:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <dirent.h>
#include <fcntl.h>
#include <locale.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/multixact.h"
#include "access/xlog_internal.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"

extern int	optind;
extern char *optarg;


static ControlFileData ControlFile;		/* pg_control values */
static uint32 newXlogId,
			newXlogSeg;			/* ID/Segment of new XLOG segment */
static bool guessed = false;	/* T if we had to guess at any values */
static const char *progname;

static bool ReadControlFile(void);
static void GuessControlValues(void);
static void PrintControlValues(bool guessed);
static void RewriteControlFile(void);
static void FindEndOfXLOG(void);
static void KillExistingXLOG(void);
static void WriteEmptyXLOG(void);
static void usage(void);


int
main(int argc, char *argv[])
{
	int			c;
	bool		force = false;
	bool		noupdate = false;
	uint32		set_xid_epoch = (uint32) -1;
	TransactionId set_xid = 0;
	Oid			set_oid = 0;
	MultiXactId set_mxid = 0;
	MultiXactOffset set_mxoff = (MultiXactOffset) -1;
	uint32		minXlogTli = 0,
				minXlogId = 0,
				minXlogSeg = 0;
	char	   *endptr;
	char	   *endptr2;
	char	   *endptr3;
	char	   *DataDir;
	int			fd;
	char		path[MAXPGPATH];

	set_pglocale_pgservice(argv[0], "pg_resetxlog");

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
			puts("pg_resetxlog (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}


	while ((c = getopt(argc, argv, "fl:m:no:O:x:e:")) != -1)
	{
		switch (c)
		{
			case 'f':
				force = true;
				break;

			case 'n':
				noupdate = true;
				break;

			case 'e':
				set_xid_epoch = strtoul(optarg, &endptr, 0);
				if (endptr == optarg || *endptr != '\0')
				{
					fprintf(stderr, _("%s: invalid argument for option -e\n"), progname);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				if (set_xid_epoch == -1)
				{
					fprintf(stderr, _("%s: transaction ID epoch (-e) must not be -1\n"), progname);
					exit(1);
				}
				break;

			case 'x':
				set_xid = strtoul(optarg, &endptr, 0);
				if (endptr == optarg || *endptr != '\0')
				{
					fprintf(stderr, _("%s: invalid argument for option -x\n"), progname);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				if (set_xid == 0)
				{
					fprintf(stderr, _("%s: transaction ID (-x) must not be 0\n"), progname);
					exit(1);
				}
				break;

			case 'o':
				set_oid = strtoul(optarg, &endptr, 0);
				if (endptr == optarg || *endptr != '\0')
				{
					fprintf(stderr, _("%s: invalid argument for option -o\n"), progname);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				if (set_oid == 0)
				{
					fprintf(stderr, _("%s: OID (-o) must not be 0\n"), progname);
					exit(1);
				}
				break;

			case 'm':
				set_mxid = strtoul(optarg, &endptr, 0);
				if (endptr == optarg || *endptr != '\0')
				{
					fprintf(stderr, _("%s: invalid argument for option -m\n"), progname);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				if (set_mxid == 0)
				{
					fprintf(stderr, _("%s: multitransaction ID (-m) must not be 0\n"), progname);
					exit(1);
				}
				break;

			case 'O':
				set_mxoff = strtoul(optarg, &endptr, 0);
				if (endptr == optarg || *endptr != '\0')
				{
					fprintf(stderr, _("%s: invalid argument for option -O\n"), progname);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				if (set_mxoff == -1)
				{
					fprintf(stderr, _("%s: multitransaction offset (-O) must not be -1\n"), progname);
					exit(1);
				}
				break;

			case 'l':
				minXlogTli = strtoul(optarg, &endptr, 0);
				if (endptr == optarg || *endptr != ',')
				{
					fprintf(stderr, _("%s: invalid argument for option -l\n"), progname);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				minXlogId = strtoul(endptr + 1, &endptr2, 0);
				if (endptr2 == endptr + 1 || *endptr2 != ',')
				{
					fprintf(stderr, _("%s: invalid argument for option -l\n"), progname);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				minXlogSeg = strtoul(endptr2 + 1, &endptr3, 0);
				if (endptr3 == endptr2 + 1 || *endptr3 != '\0')
				{
					fprintf(stderr, _("%s: invalid argument for option -l\n"), progname);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	if (optind == argc)
	{
		fprintf(stderr, _("%s: no data directory specified\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	/*
	 * Don't allow pg_resetxlog to be run as root, to avoid overwriting the
	 * ownership of files in the data directory. We need only check for root
	 * -- any other user won't have sufficient permissions to modify files in
	 * the data directory.
	 */
#ifndef WIN32
	if (geteuid() == 0)
	{
		fprintf(stderr, _("%s: cannot be executed by \"root\"\n"),
				progname);
		fprintf(stderr, _("You must run %s as the PostgreSQL superuser.\n"),
				progname);
		exit(1);
	}
#endif

	DataDir = argv[optind];

	if (chdir(DataDir) < 0)
	{
		fprintf(stderr, _("%s: could not change directory to \"%s\": %s\n"),
				progname, DataDir, strerror(errno));
		exit(1);
	}

	/*
	 * Check for a postmaster lock file --- if there is one, refuse to
	 * proceed, on grounds we might be interfering with a live installation.
	 */
	snprintf(path, MAXPGPATH, "%s/postmaster.pid", DataDir);

	if ((fd = open(path, O_RDONLY, 0)) < 0)
	{
		if (errno != ENOENT)
		{
			fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"), progname, path, strerror(errno));
			exit(1);
		}
	}
	else
	{
		fprintf(stderr, _("%s: lock file \"%s\" exists\n"
						  "Is a server running?  If not, delete the lock file and try again.\n"),
				progname, path);
		exit(1);
	}

	/*
	 * Attempt to read the existing pg_control file
	 */
	if (!ReadControlFile())
		GuessControlValues();

	/*
	 * Also look at existing segment files to set up newXlogId/newXlogSeg
	 */
	FindEndOfXLOG();

	/*
	 * Adjust fields if required by switches.  (Do this now so that printout,
	 * if any, includes these values.)
	 */
	if (set_xid_epoch != -1)
		ControlFile.checkPointCopy.nextXidEpoch = set_xid_epoch;

	if (set_xid != 0)
		ControlFile.checkPointCopy.nextXid = set_xid;

	if (set_oid != 0)
		ControlFile.checkPointCopy.nextOid = set_oid;

	if (set_mxid != 0)
		ControlFile.checkPointCopy.nextMulti = set_mxid;

	if (set_mxoff != -1)
		ControlFile.checkPointCopy.nextMultiOffset = set_mxoff;

	if (minXlogTli > ControlFile.checkPointCopy.ThisTimeLineID)
		ControlFile.checkPointCopy.ThisTimeLineID = minXlogTli;

	if (minXlogId > newXlogId ||
		(minXlogId == newXlogId &&
		 minXlogSeg > newXlogSeg))
	{
		newXlogId = minXlogId;
		newXlogSeg = minXlogSeg;
	}

	/*
	 * If we had to guess anything, and -f was not given, just print the
	 * guessed values and exit.  Also print if -n is given.
	 */
	if ((guessed && !force) || noupdate)
	{
		PrintControlValues(guessed);
		if (!noupdate)
		{
			printf(_("\nIf these values seem acceptable, use -f to force reset.\n"));
			exit(1);
		}
		else
			exit(0);
	}

	/*
	 * Don't reset from a dirty pg_control without -f, either.
	 */
	if (ControlFile.state != DB_SHUTDOWNED && !force)
	{
		printf(_("The database server was not shut down cleanly.\n"
			   "Resetting the transaction log might cause data to be lost.\n"
				 "If you want to proceed anyway, use -f to force reset.\n"));
		exit(1);
	}

	/*
	 * Else, do the dirty deed.
	 */
	RewriteControlFile();
	KillExistingXLOG();
	WriteEmptyXLOG();

	printf(_("Transaction log reset\n"));
	return 0;
}


/*
 * Try to read the existing pg_control file.
 *
 * This routine is also responsible for updating old pg_control versions
 * to the current format.  (Currently we don't do anything of the sort.)
 */
static bool
ReadControlFile(void)
{
	int			fd;
	int			len;
	char	   *buffer;
	pg_crc32	crc;

	if ((fd = open(XLOG_CONTROL_FILE, O_RDONLY, 0)) < 0)
	{
		/*
		 * If pg_control is not there at all, or we can't read it, the odds
		 * are we've been handed a bad DataDir path, so give up. User can do
		 * "touch pg_control" to force us to proceed.
		 */
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, XLOG_CONTROL_FILE, strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr, _("If you are sure the data directory path is correct, execute\n"
							  "  touch %s\n"
							  "and try again.\n"),
					XLOG_CONTROL_FILE);
		exit(1);
	}

	/* Use malloc to ensure we have a maxaligned buffer */
	buffer = (char *) malloc(PG_CONTROL_SIZE);

	len = read(fd, buffer, PG_CONTROL_SIZE);
	if (len < 0)
	{
		fprintf(stderr, _("%s: could not read file \"%s\": %s\n"),
				progname, XLOG_CONTROL_FILE, strerror(errno));
		exit(1);
	}
	close(fd);

	if (len >= sizeof(ControlFileData) &&
	  ((ControlFileData *) buffer)->pg_control_version == PG_CONTROL_VERSION)
	{
		/* Check the CRC. */
		INIT_CRC32(crc);
		COMP_CRC32(crc,
				   buffer,
				   offsetof(ControlFileData, crc));
		FIN_CRC32(crc);

		if (EQ_CRC32(crc, ((ControlFileData *) buffer)->crc))
		{
			/* Valid data... */
			memcpy(&ControlFile, buffer, sizeof(ControlFile));
			return true;
		}

		fprintf(stderr, _("%s: pg_control exists but has invalid CRC; proceed with caution\n"),
				progname);
		/* We will use the data anyway, but treat it as guessed. */
		memcpy(&ControlFile, buffer, sizeof(ControlFile));
		guessed = true;
		return true;
	}

	/* Looks like it's a mess. */
	fprintf(stderr, _("%s: pg_control exists but is broken or unknown version; ignoring it\n"),
			progname);
	return false;
}


/*
 * Guess at pg_control values when we can't read the old ones.
 */
static void
GuessControlValues(void)
{
	uint64		sysidentifier;
	struct timeval tv;
	char	   *localeptr;

	/*
	 * Set up a completely default set of pg_control values.
	 */
	guessed = true;
	memset(&ControlFile, 0, sizeof(ControlFile));

	ControlFile.pg_control_version = PG_CONTROL_VERSION;
	ControlFile.catalog_version_no = CATALOG_VERSION_NO;

	/*
	 * Create a new unique installation identifier, since we can no longer use
	 * any old XLOG records.  See notes in xlog.c about the algorithm.
	 */
	gettimeofday(&tv, NULL);
	sysidentifier = ((uint64) tv.tv_sec) << 32;
	sysidentifier |= (uint32) (tv.tv_sec | tv.tv_usec);

	ControlFile.system_identifier = sysidentifier;

	ControlFile.checkPointCopy.redo.xlogid = 0;
	ControlFile.checkPointCopy.redo.xrecoff = SizeOfXLogLongPHD;
	ControlFile.checkPointCopy.ThisTimeLineID = 1;
	ControlFile.checkPointCopy.nextXidEpoch = 0;
	ControlFile.checkPointCopy.nextXid = (TransactionId) 514;	/* XXX */
	ControlFile.checkPointCopy.nextOid = FirstBootstrapObjectId;
	ControlFile.checkPointCopy.nextMulti = FirstMultiXactId;
	ControlFile.checkPointCopy.nextMultiOffset = 0;
	ControlFile.checkPointCopy.time = time(NULL);

	ControlFile.state = DB_SHUTDOWNED;
	ControlFile.time = time(NULL);
	ControlFile.checkPoint = ControlFile.checkPointCopy.redo;

	ControlFile.maxAlign = MAXIMUM_ALIGNOF;
	ControlFile.floatFormat = FLOATFORMAT_VALUE;
	ControlFile.blcksz = BLCKSZ;
	ControlFile.relseg_size = RELSEG_SIZE;
	ControlFile.xlog_blcksz = XLOG_BLCKSZ;
	ControlFile.xlog_seg_size = XLOG_SEG_SIZE;
	ControlFile.nameDataLen = NAMEDATALEN;
	ControlFile.indexMaxKeys = INDEX_MAX_KEYS;
	ControlFile.toast_max_chunk_size = TOAST_MAX_CHUNK_SIZE;
#ifdef HAVE_INT64_TIMESTAMP
	ControlFile.enableIntTimes = TRUE;
#else
	ControlFile.enableIntTimes = FALSE;
#endif
	ControlFile.localeBuflen = LOCALE_NAME_BUFLEN;

	localeptr = setlocale(LC_COLLATE, "");
	if (!localeptr)
	{
		fprintf(stderr, _("%s: invalid LC_COLLATE setting\n"), progname);
		exit(1);
	}
	strlcpy(ControlFile.lc_collate, localeptr, sizeof(ControlFile.lc_collate));
	localeptr = setlocale(LC_CTYPE, "");
	if (!localeptr)
	{
		fprintf(stderr, _("%s: invalid LC_CTYPE setting\n"), progname);
		exit(1);
	}
	strlcpy(ControlFile.lc_ctype, localeptr, sizeof(ControlFile.lc_ctype));

	/*
	 * XXX eventually, should try to grovel through old XLOG to develop more
	 * accurate values for TimeLineID, nextXID, etc.
	 */
}


/*
 * Print the guessed pg_control values when we had to guess.
 *
 * NB: this display should be just those fields that will not be
 * reset by RewriteControlFile().
 */
static void
PrintControlValues(bool guessed)
{
	char		sysident_str[32];

	if (guessed)
		printf(_("Guessed pg_control values:\n\n"));
	else
		printf(_("pg_control values:\n\n"));

	/*
	 * Format system_identifier separately to keep platform-dependent format
	 * code out of the translatable message string.
	 */
	snprintf(sysident_str, sizeof(sysident_str), UINT64_FORMAT,
			 ControlFile.system_identifier);

	printf(_("First log file ID after reset:        %u\n"),
		   newXlogId);
	printf(_("First log file segment after reset:   %u\n"),
		   newXlogSeg);
	printf(_("pg_control version number:            %u\n"),
		   ControlFile.pg_control_version);
	printf(_("Catalog version number:               %u\n"),
		   ControlFile.catalog_version_no);
	printf(_("Database system identifier:           %s\n"),
		   sysident_str);
	printf(_("Latest checkpoint's TimeLineID:       %u\n"),
		   ControlFile.checkPointCopy.ThisTimeLineID);
	printf(_("Latest checkpoint's NextXID:          %u/%u\n"),
		   ControlFile.checkPointCopy.nextXidEpoch,
		   ControlFile.checkPointCopy.nextXid);
	printf(_("Latest checkpoint's NextOID:          %u\n"),
		   ControlFile.checkPointCopy.nextOid);
	printf(_("Latest checkpoint's NextMultiXactId:  %u\n"),
		   ControlFile.checkPointCopy.nextMulti);
	printf(_("Latest checkpoint's NextMultiOffset:  %u\n"),
		   ControlFile.checkPointCopy.nextMultiOffset);
	printf(_("Maximum data alignment:               %u\n"),
		   ControlFile.maxAlign);
	/* we don't print floatFormat since can't say much useful about it */
	printf(_("Database block size:                  %u\n"),
		   ControlFile.blcksz);
	printf(_("Blocks per segment of large relation: %u\n"),
		   ControlFile.relseg_size);
	printf(_("WAL block size:                       %u\n"),
		   ControlFile.xlog_blcksz);
	printf(_("Bytes per WAL segment:                %u\n"),
		   ControlFile.xlog_seg_size);
	printf(_("Maximum length of identifiers:        %u\n"),
		   ControlFile.nameDataLen);
	printf(_("Maximum columns in an index:          %u\n"),
		   ControlFile.indexMaxKeys);
	printf(_("Maximum size of a TOAST chunk:        %u\n"),
		   ControlFile.toast_max_chunk_size);
	printf(_("Date/time type storage:               %s\n"),
		   (ControlFile.enableIntTimes ? _("64-bit integers") : _("floating-point numbers")));
	printf(_("Maximum length of locale name:        %u\n"),
		   ControlFile.localeBuflen);
	printf(_("LC_COLLATE:                           %s\n"),
		   ControlFile.lc_collate);
	printf(_("LC_CTYPE:                             %s\n"),
		   ControlFile.lc_ctype);
}


/*
 * Write out the new pg_control file.
 */
static void
RewriteControlFile(void)
{
	int			fd;
	char		buffer[PG_CONTROL_SIZE];		/* need not be aligned */

	/*
	 * Adjust fields as needed to force an empty XLOG starting at
	 * newXlogId/newXlogSeg.
	 */
	ControlFile.checkPointCopy.redo.xlogid = newXlogId;
	ControlFile.checkPointCopy.redo.xrecoff =
		newXlogSeg * XLogSegSize + SizeOfXLogLongPHD;
	ControlFile.checkPointCopy.time = time(NULL);

	ControlFile.state = DB_SHUTDOWNED;
	ControlFile.time = time(NULL);
	ControlFile.checkPoint = ControlFile.checkPointCopy.redo;
	ControlFile.prevCheckPoint.xlogid = 0;
	ControlFile.prevCheckPoint.xrecoff = 0;
	ControlFile.minRecoveryPoint.xlogid = 0;
	ControlFile.minRecoveryPoint.xrecoff = 0;

	/* Now we can force the recorded xlog seg size to the right thing. */
	ControlFile.xlog_seg_size = XLogSegSize;

	/* Contents are protected with a CRC */
	INIT_CRC32(ControlFile.crc);
	COMP_CRC32(ControlFile.crc,
			   (char *) &ControlFile,
			   offsetof(ControlFileData, crc));
	FIN_CRC32(ControlFile.crc);

	/*
	 * We write out PG_CONTROL_SIZE bytes into pg_control, zero-padding the
	 * excess over sizeof(ControlFileData).  This reduces the odds of
	 * premature-EOF errors when reading pg_control.  We'll still fail when we
	 * check the contents of the file, but hopefully with a more specific
	 * error than "couldn't read pg_control".
	 */
	if (sizeof(ControlFileData) > PG_CONTROL_SIZE)
	{
		fprintf(stderr,
				_("%s: internal error -- sizeof(ControlFileData) is too large ... fix PG_CONTROL_SIZE\n"),
				progname);
		exit(1);
	}

	memset(buffer, 0, PG_CONTROL_SIZE);
	memcpy(buffer, &ControlFile, sizeof(ControlFileData));

	unlink(XLOG_CONTROL_FILE);

	fd = open(XLOG_CONTROL_FILE,
			  O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
			  S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		fprintf(stderr, _("%s: could not create pg_control file: %s\n"),
				progname, strerror(errno));
		exit(1);
	}

	errno = 0;
	if (write(fd, buffer, PG_CONTROL_SIZE) != PG_CONTROL_SIZE)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		fprintf(stderr, _("%s: could not write pg_control file: %s\n"),
				progname, strerror(errno));
		exit(1);
	}

	if (fsync(fd) != 0)
	{
		fprintf(stderr, _("%s: fsync error: %s\n"), progname, strerror(errno));
		exit(1);
	}

	close(fd);
}


/*
 * Scan existing XLOG files and determine the highest existing WAL address
 *
 * On entry, ControlFile.checkPointCopy.redo and ControlFile.xlog_seg_size
 * are assumed valid (note that we allow the old xlog seg size to differ
 * from what we're using).  On exit, newXlogId and newXlogSeg are set to
 * suitable values for the beginning of replacement WAL (in our seg size).
 */
static void
FindEndOfXLOG(void)
{
	DIR		   *xldir;
	struct dirent *xlde;

	/*
	 * Initialize the max() computation using the last checkpoint address from
	 * old pg_control.	Note that for the moment we are working with segment
	 * numbering according to the old xlog seg size.
	 */
	newXlogId = ControlFile.checkPointCopy.redo.xlogid;
	newXlogSeg = ControlFile.checkPointCopy.redo.xrecoff / ControlFile.xlog_seg_size;

	/*
	 * Scan the pg_xlog directory to find existing WAL segment files. We
	 * assume any present have been used; in most scenarios this should be
	 * conservative, because of xlog.c's attempts to pre-create files.
	 */
	xldir = opendir(XLOGDIR);
	if (xldir == NULL)
	{
		fprintf(stderr, _("%s: could not open directory \"%s\": %s\n"),
				progname, XLOGDIR, strerror(errno));
		exit(1);
	}

	errno = 0;
	while ((xlde = readdir(xldir)) != NULL)
	{
		if (strlen(xlde->d_name) == 24 &&
			strspn(xlde->d_name, "0123456789ABCDEF") == 24)
		{
			unsigned int tli,
						log,
						seg;

			sscanf(xlde->d_name, "%08X%08X%08X", &tli, &log, &seg);

			/*
			 * Note: we take the max of all files found, regardless of their
			 * timelines.  Another possibility would be to ignore files of
			 * timelines other than the target TLI, but this seems safer.
			 * Better too large a result than too small...
			 */
			if (log > newXlogId ||
				(log == newXlogId && seg > newXlogSeg))
			{
				newXlogId = log;
				newXlogSeg = seg;
			}
		}
		errno = 0;
	}
#ifdef WIN32

	/*
	 * This fix is in mingw cvs (runtime/mingwex/dirent.c rev 1.4), but not in
	 * released version
	 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif

	if (errno)
	{
		fprintf(stderr, _("%s: could not read from directory \"%s\": %s\n"),
				progname, XLOGDIR, strerror(errno));
		exit(1);
	}
	closedir(xldir);

	/*
	 * Finally, convert to new xlog seg size, and advance by one to ensure we
	 * are in virgin territory.
	 */
	newXlogSeg *= ControlFile.xlog_seg_size;
	newXlogSeg = (newXlogSeg + XLogSegSize - 1) / XLogSegSize;

	/* be sure we wrap around correctly at end of a logfile */
	NextLogSeg(newXlogId, newXlogSeg);
}


/*
 * Remove existing XLOG files
 */
static void
KillExistingXLOG(void)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		path[MAXPGPATH];

	xldir = opendir(XLOGDIR);
	if (xldir == NULL)
	{
		fprintf(stderr, _("%s: could not open directory \"%s\": %s\n"),
				progname, XLOGDIR, strerror(errno));
		exit(1);
	}

	errno = 0;
	while ((xlde = readdir(xldir)) != NULL)
	{
		if (strlen(xlde->d_name) == 24 &&
			strspn(xlde->d_name, "0123456789ABCDEF") == 24)
		{
			snprintf(path, MAXPGPATH, "%s/%s", XLOGDIR, xlde->d_name);
			if (unlink(path) < 0)
			{
				fprintf(stderr, _("%s: could not delete file \"%s\": %s\n"),
						progname, path, strerror(errno));
				exit(1);
			}
		}
		errno = 0;
	}
#ifdef WIN32

	/*
	 * This fix is in mingw cvs (runtime/mingwex/dirent.c rev 1.4), but not in
	 * released version
	 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif

	if (errno)
	{
		fprintf(stderr, _("%s: could not read from directory \"%s\": %s\n"),
				progname, XLOGDIR, strerror(errno));
		exit(1);
	}
	closedir(xldir);
}


/*
 * Write an empty XLOG file, containing only the checkpoint record
 * already set up in ControlFile.
 */
static void
WriteEmptyXLOG(void)
{
	char	   *buffer;
	XLogPageHeader page;
	XLogLongPageHeader longpage;
	XLogRecord *record;
	pg_crc32	crc;
	char		path[MAXPGPATH];
	int			fd;
	int			nbytes;

	/* Use malloc() to ensure buffer is MAXALIGNED */
	buffer = (char *) malloc(XLOG_BLCKSZ);
	page = (XLogPageHeader) buffer;
	memset(buffer, 0, XLOG_BLCKSZ);

	/* Set up the XLOG page header */
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = XLP_LONG_HEADER;
	page->xlp_tli = ControlFile.checkPointCopy.ThisTimeLineID;
	page->xlp_pageaddr.xlogid =
		ControlFile.checkPointCopy.redo.xlogid;
	page->xlp_pageaddr.xrecoff =
		ControlFile.checkPointCopy.redo.xrecoff - SizeOfXLogLongPHD;
	longpage = (XLogLongPageHeader) page;
	longpage->xlp_sysid = ControlFile.system_identifier;
	longpage->xlp_seg_size = XLogSegSize;
	longpage->xlp_xlog_blcksz = XLOG_BLCKSZ;

	/* Insert the initial checkpoint record */
	record = (XLogRecord *) ((char *) page + SizeOfXLogLongPHD);
	record->xl_prev.xlogid = 0;
	record->xl_prev.xrecoff = 0;
	record->xl_xid = InvalidTransactionId;
	record->xl_tot_len = SizeOfXLogRecord + sizeof(CheckPoint);
	record->xl_len = sizeof(CheckPoint);
	record->xl_info = XLOG_CHECKPOINT_SHUTDOWN;
	record->xl_rmid = RM_XLOG_ID;
	memcpy(XLogRecGetData(record), &ControlFile.checkPointCopy,
		   sizeof(CheckPoint));

	INIT_CRC32(crc);
	COMP_CRC32(crc, &ControlFile.checkPointCopy, sizeof(CheckPoint));
	COMP_CRC32(crc, (char *) record + sizeof(pg_crc32),
			   SizeOfXLogRecord - sizeof(pg_crc32));
	FIN_CRC32(crc);
	record->xl_crc = crc;

	/* Write the first page */
	XLogFilePath(path, ControlFile.checkPointCopy.ThisTimeLineID,
				 newXlogId, newXlogSeg);

	unlink(path);

	fd = open(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
			  S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		fprintf(stderr, _("%s: could not open file \"%s\": %s\n"),
				progname, path, strerror(errno));
		exit(1);
	}

	errno = 0;
	if (write(fd, buffer, XLOG_BLCKSZ) != XLOG_BLCKSZ)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		fprintf(stderr, _("%s: could not write file \"%s\": %s\n"),
				progname, path, strerror(errno));
		exit(1);
	}

	/* Fill the rest of the file with zeroes */
	memset(buffer, 0, XLOG_BLCKSZ);
	for (nbytes = XLOG_BLCKSZ; nbytes < XLogSegSize; nbytes += XLOG_BLCKSZ)
	{
		errno = 0;
		if (write(fd, buffer, XLOG_BLCKSZ) != XLOG_BLCKSZ)
		{
			if (errno == 0)
				errno = ENOSPC;
			fprintf(stderr, _("%s: could not write file \"%s\": %s\n"),
					progname, path, strerror(errno));
			exit(1);
		}
	}

	if (fsync(fd) != 0)
	{
		fprintf(stderr, _("%s: fsync error: %s\n"), progname, strerror(errno));
		exit(1);
	}

	close(fd);
}


static void
usage(void)
{
	printf(_("%s resets the PostgreSQL transaction log.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]... DATADIR\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -f              force update to be done\n"));
	printf(_("  -l TLI,FILE,SEG force minimum WAL starting location for new transaction log\n"));
	printf(_("  -m XID          set next multitransaction ID\n"));
	printf(_("  -n              no update, just show extracted control values (for testing)\n"));
	printf(_("  -o OID          set next OID\n"));
	printf(_("  -O OFFSET       set next multitransaction offset\n"));
	printf(_("  -x XID          set next transaction ID\n"));
	printf(_("  -e XIDEPOCH     set next transaction ID epoch\n"));
	printf(_("  --help          show this help, then exit\n"));
	printf(_("  --version       output version information, then exit\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}
