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
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/contrib/pg_resetxlog/Attic/pg_resetxlog.c,v 1.19 2002/08/15 02:58:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#ifdef USE_LOCALE
#include <locale.h>
#endif

#include "access/xlog.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"


/******************** stuff copied from xlog.c ********************/

/* Increment an xlogid/segment pair */
#define NextLogSeg(logId, logSeg)	\
	do { \
		if ((logSeg) >= XLogSegsPerFile-1) \
		{ \
			(logId)++; \
			(logSeg) = 0; \
		} \
		else \
			(logSeg)++; \
	} while (0)

#define XLogFileName(path, log, seg)	\
			snprintf(path, MAXPGPATH, "%s/%08X%08X",	\
					 XLogDir, log, seg)

/******************** end of stuff copied from xlog.c ********************/


static char *DataDir;			/* locations of important stuff */
static char XLogDir[MAXPGPATH];
static char ControlFilePath[MAXPGPATH];

static ControlFileData ControlFile;		/* pg_control values */
static uint32 newXlogId,
			newXlogSeg;			/* ID/Segment of new XLOG segment */
static bool guessed = false;	/* T if we had to guess at any values */


/*
 * Try to read the existing pg_control file.
 *
 * This routine is also responsible for updating old pg_control versions
 * to the current format.
 */
static bool
ReadControlFile(void)
{
	int			fd;
	int			len;
	char	   *buffer;
	crc64		crc;

	if ((fd = open(ControlFilePath, O_RDONLY)) < 0)
	{
		/*
		 * If pg_control is not there at all, or we can't read it, the
		 * odds are we've been handed a bad DataDir path, so give up. User
		 * can do "touch pg_control" to force us to proceed.
		 */
		perror("Failed to open $PGDATA/global/pg_control for reading");
		if (errno == ENOENT)
			fprintf(stderr, "If you're sure the PGDATA path is correct, do\n"
					"  touch %s\n"
					"and try again.\n", ControlFilePath);
		exit(1);
	}

	/* Use malloc to ensure we have a maxaligned buffer */
	buffer = (char *) malloc(BLCKSZ);

	len = read(fd, buffer, BLCKSZ);
	if (len < 0)
	{
		perror("Failed to read $PGDATA/global/pg_control");
		exit(1);
	}
	close(fd);

	if (len >= sizeof(ControlFileData) &&
		((ControlFileData *) buffer)->pg_control_version == PG_CONTROL_VERSION)
	{
		/* Check the CRC. */
		INIT_CRC64(crc);
		COMP_CRC64(crc,
				   buffer + sizeof(crc64),
				   sizeof(ControlFileData) - sizeof(crc64));
		FIN_CRC64(crc);

		if (EQ_CRC64(crc, ((ControlFileData *) buffer)->crc))
		{
			/* Valid data... */
			memcpy(&ControlFile, buffer, sizeof(ControlFile));
			return true;
		}

		fprintf(stderr, "pg_control exists but has invalid CRC; proceed with caution.\n");
		/* We will use the data anyway, but treat it as guessed. */
		memcpy(&ControlFile, buffer, sizeof(ControlFile));
		guessed = true;
		return true;
	}

	/* Looks like it's a mess. */
	fprintf(stderr, "pg_control exists but is broken or unknown version; ignoring it.\n");
	return false;
}


/*
 * Guess at pg_control values when we can't read the old ones.
 */
static void
GuessControlValues(void)
{
#ifdef USE_LOCALE
	char	   *localeptr;
#endif

	/*
	 * Set up a completely default set of pg_control values.
	 */
	guessed = true;
	memset(&ControlFile, 0, sizeof(ControlFile));

	ControlFile.pg_control_version = PG_CONTROL_VERSION;
	ControlFile.catalog_version_no = CATALOG_VERSION_NO;

	ControlFile.checkPointCopy.redo.xlogid = 0;
	ControlFile.checkPointCopy.redo.xrecoff = SizeOfXLogPHD;
	ControlFile.checkPointCopy.undo = ControlFile.checkPointCopy.redo;
	ControlFile.checkPointCopy.ThisStartUpID = 0;
	ControlFile.checkPointCopy.nextXid = (TransactionId) 514;	/* XXX */
	ControlFile.checkPointCopy.nextOid = BootstrapObjectIdData;
	ControlFile.checkPointCopy.time = time(NULL);

	ControlFile.state = DB_SHUTDOWNED;
	ControlFile.time = time(NULL);
	ControlFile.logId = 0;
	ControlFile.logSeg = 1;
	ControlFile.checkPoint = ControlFile.checkPointCopy.redo;

	ControlFile.blcksz = BLCKSZ;
	ControlFile.relseg_size = RELSEG_SIZE;
#ifdef USE_LOCALE
	localeptr = setlocale(LC_COLLATE, "");
	if (!localeptr)
	{
		fprintf(stderr, "Invalid LC_COLLATE setting\n");
		exit(1);
	}
	StrNCpy(ControlFile.lc_collate, localeptr, LOCALE_NAME_BUFLEN);
	localeptr = setlocale(LC_CTYPE, "");
	if (!localeptr)
	{
		fprintf(stderr, "Invalid LC_CTYPE setting\n");
		exit(1);
	}
	StrNCpy(ControlFile.lc_ctype, localeptr, LOCALE_NAME_BUFLEN);
#else
	strcpy(ControlFile.lc_collate, "C");
	strcpy(ControlFile.lc_ctype, "C");
#endif

	/*
	 * XXX eventually, should try to grovel through old XLOG to develop
	 * more accurate values for startupid, nextXID, and nextOID.
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
	printf("%spg_control values:\n\n"
		   "pg_control version number:            %u\n"
		   "Catalog version number:               %u\n"
		   "Current log file id:                  %u\n"
		   "Next log file segment:                %u\n"
		   "Latest checkpoint's StartUpID:        %u\n"
		   "Latest checkpoint's NextXID:          %u\n"
		   "Latest checkpoint's NextOID:          %u\n"
		   "Database block size:                  %u\n"
		   "Blocks per segment of large relation: %u\n"
		   "LC_COLLATE:                           %s\n"
		   "LC_CTYPE:                             %s\n",

		   (guessed ? "Guessed-at " : ""),
		   ControlFile.pg_control_version,
		   ControlFile.catalog_version_no,
		   ControlFile.logId,
		   ControlFile.logSeg,
		   ControlFile.checkPointCopy.ThisStartUpID,
		   ControlFile.checkPointCopy.nextXid,
		   ControlFile.checkPointCopy.nextOid,
		   ControlFile.blcksz,
		   ControlFile.relseg_size,
		   ControlFile.lc_collate,
		   ControlFile.lc_ctype);
}


/*
 * Write out the new pg_control file.
 */
static void
RewriteControlFile(void)
{
	int			fd;
	char		buffer[BLCKSZ]; /* need not be aligned */

	/*
	 * Adjust fields as needed to force an empty XLOG starting at the next
	 * available segment.
	 */
	newXlogId = ControlFile.logId;
	newXlogSeg = ControlFile.logSeg;
	/* be sure we wrap around correctly at end of a logfile */
	NextLogSeg(newXlogId, newXlogSeg);

	ControlFile.checkPointCopy.redo.xlogid = newXlogId;
	ControlFile.checkPointCopy.redo.xrecoff =
		newXlogSeg * XLogSegSize + SizeOfXLogPHD;
	ControlFile.checkPointCopy.undo = ControlFile.checkPointCopy.redo;
	ControlFile.checkPointCopy.time = time(NULL);

	ControlFile.state = DB_SHUTDOWNED;
	ControlFile.time = time(NULL);
	ControlFile.logId = newXlogId;
	ControlFile.logSeg = newXlogSeg + 1;
	ControlFile.checkPoint = ControlFile.checkPointCopy.redo;
	ControlFile.prevCheckPoint.xlogid = 0;
	ControlFile.prevCheckPoint.xrecoff = 0;

	/* Contents are protected with a CRC */
	INIT_CRC64(ControlFile.crc);
	COMP_CRC64(ControlFile.crc,
			   (char *) &ControlFile + sizeof(crc64),
			   sizeof(ControlFileData) - sizeof(crc64));
	FIN_CRC64(ControlFile.crc);

	/*
	 * We write out BLCKSZ bytes into pg_control, zero-padding the excess
	 * over sizeof(ControlFileData).  This reduces the odds of
	 * premature-EOF errors when reading pg_control.  We'll still fail
	 * when we check the contents of the file, but hopefully with a more
	 * specific error than "couldn't read pg_control".
	 */
	if (sizeof(ControlFileData) > BLCKSZ)
	{
		fprintf(stderr, "sizeof(ControlFileData) is too large ... fix xlog.c\n");
		exit(1);
	}

	memset(buffer, 0, BLCKSZ);
	memcpy(buffer, &ControlFile, sizeof(ControlFileData));

	unlink(ControlFilePath);

	fd = open(ControlFilePath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		perror("RewriteControlFile failed to create pg_control file");
		exit(1);
	}

	errno = 0;
	if (write(fd, buffer, BLCKSZ) != BLCKSZ)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		perror("RewriteControlFile failed to write pg_control file");
		exit(1);
	}

	if (fsync(fd) != 0)
	{
		perror("fsync");
		exit(1);
	}

	close(fd);
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

	xldir = opendir(XLogDir);
	if (xldir == NULL)
	{
		perror("KillExistingXLOG: cannot open $PGDATA/pg_xlog directory");
		exit(1);
	}

	errno = 0;
	while ((xlde = readdir(xldir)) != NULL)
	{
		if (strlen(xlde->d_name) == 16 &&
			strspn(xlde->d_name, "0123456789ABCDEF") == 16)
		{
			snprintf(path, MAXPGPATH, "%s/%s", XLogDir, xlde->d_name);
			if (unlink(path) < 0)
			{
				perror(path);
				exit(1);
			}
		}
		errno = 0;
	}
	if (errno)
	{
		perror("KillExistingXLOG: cannot read $PGDATA/pg_xlog directory");
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
	XLogRecord *record;
	crc64		crc;
	char		path[MAXPGPATH];
	int			fd;
	int			nbytes;

	/* Use malloc() to ensure buffer is MAXALIGNED */
	buffer = (char *) malloc(BLCKSZ);
	page = (XLogPageHeader) buffer;

	/* Set up the first page with initial record */
	memset(buffer, 0, BLCKSZ);
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = 0;
	page->xlp_sui = ControlFile.checkPointCopy.ThisStartUpID;
	page->xlp_pageaddr.xlogid =
		ControlFile.checkPointCopy.redo.xlogid;
	page->xlp_pageaddr.xrecoff =
		ControlFile.checkPointCopy.redo.xrecoff - SizeOfXLogPHD;
	record = (XLogRecord *) ((char *) page + SizeOfXLogPHD);
	record->xl_prev.xlogid = 0;
	record->xl_prev.xrecoff = 0;
	record->xl_xact_prev = record->xl_prev;
	record->xl_xid = InvalidTransactionId;
	record->xl_len = sizeof(CheckPoint);
	record->xl_info = XLOG_CHECKPOINT_SHUTDOWN;
	record->xl_rmid = RM_XLOG_ID;
	memcpy(XLogRecGetData(record), &ControlFile.checkPointCopy,
		   sizeof(CheckPoint));

	INIT_CRC64(crc);
	COMP_CRC64(crc, &ControlFile.checkPointCopy, sizeof(CheckPoint));
	COMP_CRC64(crc, (char *) record + sizeof(crc64),
			   SizeOfXLogRecord - sizeof(crc64));
	FIN_CRC64(crc);
	record->xl_crc = crc;

	/* Write the first page */
	XLogFileName(path, newXlogId, newXlogSeg);

	unlink(path);

	fd = open(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
			  S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		perror(path);
		exit(1);
	}

	errno = 0;
	if (write(fd, buffer, BLCKSZ) != BLCKSZ)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		perror("WriteEmptyXLOG: failed to write xlog file");
		exit(1);
	}

	/* Fill the rest of the file with zeroes */
	memset(buffer, 0, BLCKSZ);
	for (nbytes = BLCKSZ; nbytes < XLogSegSize; nbytes += BLCKSZ)
	{
		errno = 0;
		if (write(fd, buffer, BLCKSZ) != BLCKSZ)
		{
			if (errno == 0)
				errno = ENOSPC;
			perror("WriteEmptyXLOG: failed to write xlog file");
			exit(1);
		}
	}

	if (fsync(fd) != 0)
	{
		perror("fsync");
		exit(1);
	}

	close(fd);
}


static void
usage(void)
{
	fprintf(stderr, "Usage: pg_resetxlog [-f] [-n] [-x xid] [ -l fileid seg ] PGDataDirectory\n"
			"  -f\t\tforce update to be done\n"
			"  -n\t\tno update, just show extracted pg_control values (for testing)\n"
			"  -x xid\tset next transaction ID\n"
			"  -l fileid seg\tforce minimum WAL starting location for new xlog\n");
	exit(1);
}


int
main(int argc, char **argv)
{
	int			argn;
	bool		force = false;
	bool		noupdate = false;
	TransactionId set_xid = 0;
	uint32		minXlogId = 0,
				minXlogSeg = 0;
	int			fd;
	char		path[MAXPGPATH];

	for (argn = 1; argn < argc; argn++)
	{
		if (argv[argn][0] != '-')
			break;				/* end of switches */
		if (strcmp(argv[argn], "-f") == 0)
			force = true;
		else if (strcmp(argv[argn], "-n") == 0)
			noupdate = true;
		else if (strcmp(argv[argn], "-x") == 0)
		{
			argn++;
			if (argn == argc)
				usage();
			set_xid = strtoul(argv[argn], NULL, 0);
			if (set_xid == 0)
			{
				fprintf(stderr, "XID can not be 0.\n");
				exit(1);
			}
		}
		else if (strcmp(argv[argn], "-l") == 0)
		{
			argn++;
			if (argn == argc)
				usage();
			minXlogId = strtoul(argv[argn], NULL, 0);
			argn++;
			if (argn == argc)
				usage();
			minXlogSeg = strtoul(argv[argn], NULL, 0);
		}
		else
			usage();
	}

	if (argn != argc - 1)		/* one required non-switch argument */
		usage();

	DataDir = argv[argn++];

	snprintf(XLogDir, MAXPGPATH, "%s/pg_xlog", DataDir);

	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);

	/*
	 * Check for a postmaster lock file --- if there is one, refuse to
	 * proceed, on grounds we might be interfering with a live
	 * installation.
	 */
	snprintf(path, MAXPGPATH, "%s/postmaster.pid", DataDir);

	if ((fd = open(path, O_RDONLY)) < 0)
	{
		if (errno != ENOENT)
		{
			perror("Failed to open $PGDATA/postmaster.pid for reading");
			exit(1);
		}
	}
	else
	{
		fprintf(stderr, "Lock file '%s' exists --- is a postmaster running?\n"
				"If not, delete the lock file and try again.\n",
				path);
		exit(1);
	}

	/*
	 * Attempt to read the existing pg_control file
	 */
	if (!ReadControlFile())
		GuessControlValues();

	/*
	 * If we had to guess anything, and -f was not given, just print the
	 * guessed values and exit.  Also print if -n is given.
	 */
	if ((guessed && !force) || noupdate)
	{
		PrintControlValues(guessed);
		if (!noupdate)
		{
			printf("\nIf these values seem acceptable, use -f to force reset.\n");
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
		printf("The database was not shut down cleanly.\n"
			   "Resetting the xlog may cause data to be lost!\n"
			   "If you want to proceed anyway, use -f to force reset.\n");
		exit(1);
	}

	/*
	 * Else, do the dirty deed.
	 *
	 * First adjust fields if required by switches.
	 */
	if (set_xid != 0)
		ControlFile.checkPointCopy.nextXid = set_xid;

	if (minXlogId > ControlFile.logId ||
		(minXlogId == ControlFile.logId && minXlogSeg > ControlFile.logSeg))
	{
		ControlFile.logId = minXlogId;
		ControlFile.logSeg = minXlogSeg;
	}

	RewriteControlFile();
	KillExistingXLOG();
	WriteEmptyXLOG();

	printf("XLOG reset.\n");
	return 0;
}
