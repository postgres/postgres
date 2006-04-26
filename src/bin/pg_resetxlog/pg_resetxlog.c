/*-------------------------------------------------------------------------
 *
 * pg_resetxlog.c
 *	  A utility to "zero out" the xlog when it's corrupt beyond recovery.
 *	  Can also rebuild pg_control if needed.
 *
 * The theory of reset operation is fairly simple:
 *	  1. Read the existing pg_control (which will include the last
 *		 checkpoint record).  If it is an old format then update to
 *		 current format.
 *	  2. If pg_control is corrupt, attempt to rebuild the values,
 *		 by scanning the old xlog; if it fail then try to guess it.
 *	  3. Modify pg_control to reflect a "shutdown" state with a checkpoint
 *		 record at the start of xlog.
 *	  4. Flush the existing xlog files and write a new segment with
 *		 just a checkpoint record in it.  The new segment is positioned
 *		 just past the end of the old xlog, so that existing LSNs in
 *		 data pages will appear to be "in the past".
 *
 * The algorithm of restoring the pg_control value from old xlog file:
 *	1. Retrieve all of the active xlog files from xlog direcotry into a list 
 *	   by increasing order, according their timeline, log id, segment id.
 *	2. Search the list to find the oldest xlog file of the lastest time line.
 *	3. Search the records from the oldest xlog file of latest time line
 *	   to the latest xlog file of latest time line, if the checkpoint record
 *	   has been found, update the latest checkpoint and previous checkpoint.
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
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

#include "access/multixact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"

#define GUESS	0
#define WAL	1

extern int	optind;
extern char *optarg;


static ControlFileData ControlFile;		/* pg_control values */
static uint32 newXlogId,
			newXlogSeg;			/* ID/Segment of new XLOG segment */
static const char *progname;
static uint64		sysidentifier=-1;

/* 
 * We use a list to store the active xlog files we had found in the 
 * xlog directory in increasing order according the time line, logid, 
 * segment id.
 * 
 */
typedef struct XLogFileName {
	TimeLineID tli; 
	uint32 logid; 
	uint32 seg;
	char fname[256];
	struct XLogFileName *next;
}	XLogFileName;

/* The list head */
static XLogFileName *xlogfilelist=NULL;

/* LastXLogfile is the latest file in the latest time line, 
   CurXLogfile is the oldest file in the lastest time line
   */
static XLogFileName *CurXLogFile, *LastXLogFile; 

/* The last checkpoint found in xlog file.*/
static CheckPoint      lastcheckpoint;

/* The last and previous checkpoint pointers found in xlog file.*/
static XLogRecPtr 	prevchkp, lastchkp; 

/* the database state.*/
static DBState	state=DB_SHUTDOWNED; 

/* the total checkpoint numbers which had been found in the xlog file.*/
static int 		found_checkpoint=0;	


static bool ReadControlFile(void);
static bool RestoreControlValues(int mode);
static void PrintControlValues(void);
static void UpdateCtlFile4Reset(void);
static void RewriteControlFile(void);
static void KillExistingXLOG(void);
static void WriteEmptyXLOG(void);
static void usage(void);

static void GetXLogFiles(void);
static bool ValidXLogFileName(char * fname);
static bool ValidXLogFileHeader(XLogFileName *segfile);
static bool ValidXLOGPageHeader(XLogPageHeader hdr, uint tli, uint id, uint seg);
static bool CmpXLogFileOT(XLogFileName * f1, XLogFileName *f2);
static bool IsNextSeg(XLogFileName *prev, XLogFileName *cur);
static void InsertXLogFile( char * fname );
static bool ReadXLogPage(void);
static bool RecordIsValid(XLogRecord *record, XLogRecPtr recptr);
static bool FetchRecord(void);
static void UpdateCheckPoint(XLogRecord *record);
static void SelectStartXLog(void);
static int SearchLastCheckpoint(void);
static int OpenXLogFile(XLogFileName *sf);
static void CleanUpList(XLogFileName *list);

int
main(int argc, char *argv[])
{
	int			c;
	bool		force = false;
	bool		restore = false;
	bool		noupdate = false;
	TransactionId set_xid = 0;
	Oid			set_oid = 0;
	MultiXactId set_mxid = 0;
	MultiXactOffset set_mxoff = -1;
	uint32		minXlogTli = 0,
				minXlogId = 0,
				minXlogSeg = 0;
	char	   *endptr;
	char	   *endptr2;
	char	   *endptr3;
	char	   *DataDir;
	int			fd;
	char		path[MAXPGPATH];
	bool		ctlcorrupted = false;
	bool		PidLocked = false;
	
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


	while ((c = getopt(argc, argv, "fl:m:no:O:x:r")) != -1)
	{
		switch (c)
		{
			case 'f':
				force = true;
				break;
				
			case 'r':
				restore = true;
				break;
				
			case 'n':
				noupdate = true;
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

	if ((fd = open(path, O_RDONLY)) < 0)
	{
		if (errno != ENOENT)
		{
			fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"), progname, path, strerror(errno));
			exit(1);
		}
	}
	else
	{
		PidLocked = true;
	}

	/*
	 * Attempt to read the existing pg_control file
	 */
	if (!ReadControlFile())
	{
		/* The control file has been corruptted.*/
		ctlcorrupted = true;
	}

	/*
	 * Adjust fields if required by switches.  (Do this now so that printout,
	 * if any, includes these values.)
	 */
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

	if (minXlogId > ControlFile.logId ||
		(minXlogId == ControlFile.logId &&
		 minXlogSeg > ControlFile.logSeg))
	{
		ControlFile.logId = minXlogId;
		ControlFile.logSeg = minXlogSeg;
	}

	/* retore the broken control file from WAL file.*/
	if (restore)
	{

		/* If the control fine is fine, don't touch it.*/
		if ( !ctlcorrupted )
		{
			printf(_("\nThe control file seems fine, not need to restore it.\n"));
			printf(_("If you want to restore it anyway, use -f option, but this also will reset the log file.\n"));
			exit(0);
		}
		
		
		/* Try to restore control values from old xlog file, or complain it.*/
		if (RestoreControlValues(WAL))
		{
			/* Success in restoring the checkpoint information from old xlog file.*/
			
			/* Print it out.*/
			PrintControlValues();

			/* In case the postmaster is crashed.
			 * But it may be dangerous for the living one.
			 * It may need a more good way.
			 */
			if (PidLocked)
			{
				ControlFile.state = DB_IN_PRODUCTION;
			}
			/* Write the new control file. */
			RewriteControlFile();
			printf(_("\nThe control file had been restored.\n"));
		} 
		else 
		{ 
			/* Fail in restoring the checkpoint information from old xlog file. */
			printf(_("\nCan not restore the control file from XLog file..\n"));
			printf(_("\nIf you want to restore it anyway, use -f option to guess the information, but this also will reset the log file.\n"));
		}

		exit(0);
		
	}	
	if (PidLocked)
	{  
		fprintf(stderr, _("%s: lock file \"%s\" exists\n"
						  "Is a server running?  If not, delete the lock file and try again.\n"),
				progname, path);
		exit(1);

	}
	/*
	* Print out the values in control file if -n is given. if the control file is 
	* corrupted, then inform user to restore it first.
	 */
	if (noupdate)
	{
		if (!ctlcorrupted)
		{
			/* The control file is fine, print the values out.*/
			PrintControlValues();
			exit(0);
		}
		else{
			/* The control file is corrupted.*/
			printf(_("The control file had been corrupted.\n"));
			printf(_("Please use -r option to restore it first.\n"));
			exit(1);
			}
	}

	/*
	 * Don't reset from a dirty pg_control without -f, either.
	 */
	if (ControlFile.state != DB_SHUTDOWNED && !force && !ctlcorrupted)
	{
		printf(_("The database server was not shut down cleanly.\n"
				 "Resetting the transaction log may cause data to be lost.\n"
				 "If you want to proceed anyway, use -f to force reset.\n"));
		exit(1);
	}

/*
	 * Try to reset the xlog file.
	 */
	 
	/* If the control file is corrupted, and -f option is given, resotre it first.*/
	if ( ctlcorrupted )
	{
		if (force)
		{
			if (!RestoreControlValues(WAL))
			{
				printf(_("fails to recover the control file from old xlog files, so we had to guess it.\n"));
				RestoreControlValues(GUESS);
			}
			printf(_("Restored the control file from old xlog files.\n"));
		}
		else
		{
			printf(_("Control file corrupted.\nIf you want to proceed anyway, use -f to force reset.\n"));
			exit(1);
			}
	} 
	
	/* Reset the xlog fille.*/
	UpdateCtlFile4Reset();
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

	if ((fd = open(XLOG_CONTROL_FILE, O_RDONLY)) < 0)
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
		return true;
	}

	/* Looks like it's a mess. */
	fprintf(stderr, _("%s: pg_control exists but is broken or unknown version; ignoring it\n"),
			progname);
	return false;
}




/*
 *  Restore the pg_control values by scanning old xlog files or by guessing it.
 *
 * Input parameter:
 *	WAL:  Restore the pg_control values by scanning old xlog files.
 *	GUESS: Restore the pg_control values by guessing.
 * Return:
 *	TRUE: success in restoring.
 *	FALSE: fail to restore the values. 
 * 
 */
static bool 
RestoreControlValues(int mode)
{
	struct timeval tv;
	char	   *localeptr;
	bool	successed=true;

	/*
	 * Set up a completely default set of pg_control values.
	 */
	memset(&ControlFile, 0, sizeof(ControlFile));

	ControlFile.pg_control_version = PG_CONTROL_VERSION;
	ControlFile.catalog_version_no = CATALOG_VERSION_NO;

	/* 
	 * update the checkpoint value in control file,by searching 
	 * xlog segment file, or just guessing it.
	 */
	 if (mode == WAL)
	 {
		int result = SearchLastCheckpoint();
		if ( result > 0 ) /* The last checkpoint had been found. */
		{
			ControlFile.checkPointCopy = lastcheckpoint;
			ControlFile.checkPoint = lastchkp;
			ControlFile.prevCheckPoint = prevchkp;
			ControlFile.logId = LastXLogFile->logid;
			ControlFile.logSeg = LastXLogFile->seg + 1;
			ControlFile.checkPointCopy.ThisTimeLineID = LastXLogFile->tli;
			ControlFile.state = state;
		} else 	successed = false;
		
		/* Clean up the list. */
		CleanUpList(xlogfilelist);		
		
	 } 	
	
	if (mode == GUESS)
	{
		ControlFile.checkPointCopy.redo.xlogid = 0;
		ControlFile.checkPointCopy.redo.xrecoff = SizeOfXLogLongPHD;
		ControlFile.checkPointCopy.undo = ControlFile.checkPointCopy.redo;
		ControlFile.checkPointCopy.nextXid = (TransactionId) 514;	/* XXX */
		ControlFile.checkPointCopy.nextOid = FirstBootstrapObjectId;
		ControlFile.checkPointCopy.nextMulti = FirstMultiXactId;
		ControlFile.checkPointCopy.nextMultiOffset = 0;
		ControlFile.checkPointCopy.time = time(NULL);
		ControlFile.checkPoint = ControlFile.checkPointCopy.redo;
		/*
		 * Create a new unique installation identifier, since we can no longer
		 * use any old XLOG records.  See notes in xlog.c about the algorithm.
		 */
		gettimeofday(&tv, NULL);
		sysidentifier = ((uint64) tv.tv_sec) << 32;
		sysidentifier |= (uint32) (tv.tv_sec | tv.tv_usec);
		ControlFile.state = DB_SHUTDOWNED;
		
	}

	ControlFile.time = time(NULL);
	ControlFile.system_identifier = sysidentifier;
	ControlFile.maxAlign = MAXIMUM_ALIGNOF;
	ControlFile.floatFormat = FLOATFORMAT_VALUE;
	ControlFile.blcksz = BLCKSZ;
	ControlFile.relseg_size = RELSEG_SIZE;
	ControlFile.xlog_blcksz = XLOG_BLCKSZ;
	ControlFile.xlog_seg_size = XLOG_SEG_SIZE;
	ControlFile.nameDataLen = NAMEDATALEN;
	ControlFile.indexMaxKeys = INDEX_MAX_KEYS;
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
	StrNCpy(ControlFile.lc_collate, localeptr, LOCALE_NAME_BUFLEN);
	localeptr = setlocale(LC_CTYPE, "");
	if (!localeptr)
	{
		fprintf(stderr, _("%s: invalid LC_CTYPE setting\n"), progname);
		exit(1);
	}
	StrNCpy(ControlFile.lc_ctype, localeptr, LOCALE_NAME_BUFLEN);

	return successed;	
}


/*
 * Print the out pg_control values.
 *
 * NB: this display should be just those fields that will not be
 * reset by RewriteControlFile().
 */
static void
PrintControlValues(void)
{
	char		sysident_str[32];

	printf(_("pg_control values:\n\n"));

	/*
	 * Format system_identifier separately to keep platform-dependent format
	 * code out of the translatable message string.
	 */
	snprintf(sysident_str, sizeof(sysident_str), UINT64_FORMAT,
			 ControlFile.system_identifier);

	printf(_("pg_control version number:            %u\n"), ControlFile.pg_control_version);
	printf(_("Catalog version number:               %u\n"), ControlFile.catalog_version_no);
	printf(_("Database system identifier:           %s\n"), sysident_str);
	printf(_("Current log file ID:                  %u\n"), ControlFile.logId);
	printf(_("Next log file segment:                %u\n"), ControlFile.logSeg);
	printf(_("Latest checkpoint's TimeLineID:       %u\n"), ControlFile.checkPointCopy.ThisTimeLineID);
	printf(_("Latest checkpoint's NextXID:          %u\n"), ControlFile.checkPointCopy.nextXid);
	printf(_("Latest checkpoint's NextOID:          %u\n"), ControlFile.checkPointCopy.nextOid);
	printf(_("Latest checkpoint's NextMultiXactId:  %u\n"), ControlFile.checkPointCopy.nextMulti);
	printf(_("Latest checkpoint's NextMultiOffset:  %u\n"), ControlFile.checkPointCopy.nextMultiOffset);
	printf(_("Maximum data alignment:               %u\n"), ControlFile.maxAlign);
	/* we don't print floatFormat since can't say much useful about it */
	printf(_("Database block size:                  %u\n"), ControlFile.blcksz);
	printf(_("Blocks per segment of large relation: %u\n"), ControlFile.relseg_size);
	printf(_("WAL block size:                       %u\n"), ControlFile.xlog_blcksz);
	printf(_("Bytes per WAL segment:                %u\n"), ControlFile.xlog_seg_size);
	printf(_("Maximum length of identifiers:        %u\n"), ControlFile.nameDataLen);
	printf(_("Maximum columns in an index:          %u\n"), ControlFile.indexMaxKeys);
	printf(_("Date/time type storage:               %s\n"),
		   (ControlFile.enableIntTimes ? _("64-bit integers") : _("floating-point numbers")));
	printf(_("Maximum length of locale name:        %u\n"), ControlFile.localeBuflen);
	printf(_("LC_COLLATE:                           %s\n"), ControlFile.lc_collate);
	printf(_("LC_CTYPE:                             %s\n"), ControlFile.lc_ctype);
}

/*
* Update the control file before reseting it.
*/
static void 
UpdateCtlFile4Reset(void)
{
	/*
	 * Adjust fields as needed to force an empty XLOG starting at the next
	 * available segment.
	 */
	newXlogId = ControlFile.logId;
	newXlogSeg = ControlFile.logSeg;

	/* adjust in case we are changing segment size */
	newXlogSeg *= ControlFile.xlog_seg_size;
	newXlogSeg = (newXlogSeg + XLogSegSize - 1) / XLogSegSize;

	/* be sure we wrap around correctly at end of a logfile */
	NextLogSeg(newXlogId, newXlogSeg);

	/* Now we can force the recorded xlog seg size to the right thing. */
	ControlFile.xlog_seg_size = XLogSegSize;

	ControlFile.checkPointCopy.redo.xlogid = newXlogId;
	ControlFile.checkPointCopy.redo.xrecoff =
		newXlogSeg * XLogSegSize + SizeOfXLogLongPHD;
	ControlFile.checkPointCopy.undo = ControlFile.checkPointCopy.redo;
	ControlFile.checkPointCopy.time = time(NULL);

	ControlFile.state = DB_SHUTDOWNED;
	ControlFile.time = time(NULL);
	ControlFile.logId = newXlogId;
	ControlFile.logSeg = newXlogSeg + 1;
	ControlFile.checkPoint = ControlFile.checkPointCopy.redo;
	ControlFile.prevCheckPoint.xlogid = 0;
	ControlFile.prevCheckPoint.xrecoff = 0;
}

/*
 * Write out the new pg_control file.
 */
static void
RewriteControlFile(void)
{
	int			fd;
	char		buffer[PG_CONTROL_SIZE]; /* need not be aligned */


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
	printf(_("  -f              force reset xlog to be done, if the control file is corrupted, then try to restore it.\n"));
	printf(_("  -r              restore the pg_control file from old XLog files, resets is not done..\n"));	
	printf(_("  -l TLI,FILE,SEG force minimum WAL starting location for new transaction log\n"));
	printf(_("  -n              show extracted control values of existing pg_control file.\n"));
	printf(_("  -m multiXID     set next multi transaction ID\n"));
	printf(_("  -o OID          set next OID\n"));
	printf(_("  -O multiOffset  set next multi transaction offset\n"));
	printf(_("  -x XID          set next transaction ID\n"));
	printf(_("  --help          show this help, then exit\n"));
	printf(_("  --version       output version information, then exit\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}



/*
 * The following routines are mainly used for getting pg_control values 
 * from the xlog file.
 */

 /* some local varaibles.*/
static int              logFd=0; /* kernel FD for current input file */
static int              logRecOff;      /* offset of next record in page */
static char             pageBuffer[BLCKSZ];     /* current page */
static XLogRecPtr       curRecPtr;      /* logical address of current record */
static XLogRecPtr       prevRecPtr;     /* logical address of previous record */
static char             *readRecordBuf = NULL; /* ReadRecord result area */
static uint32           readRecordBufSize = 0;
static int32            logPageOff;     /* offset of current page in file */
static uint32           logId;          /* current log file id */
static uint32           logSeg;         /* current log file segment */
static uint32           logTli;         /* current log file timeline */

/*
 * Get existing XLOG files
 */
static void
GetXLogFiles(void)
{
	DIR		   *xldir;
	struct dirent *xlde;

	/* Open the xlog direcotry.*/
	xldir = opendir(XLOGDIR);
	if (xldir == NULL)
	{
		fprintf(stderr, _("%s: could not open directory \"%s\": %s\n"),
				progname, XLOGDIR, strerror(errno));
		exit(1);
	}

	/* Search the directory, insert the segment files into the xlogfilelist.*/
	errno = 0;
	while ((xlde = readdir(xldir)) != NULL)
	{
		if (ValidXLogFileName(xlde->d_name)) {
			/* XLog file is found, insert it into the xlogfilelist.*/
			InsertXLogFile(xlde->d_name);
		};
		errno = 0;
	}
#ifdef WIN32
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
 * Insert a file while had been found in the xlog folder into xlogfilelist.
 * The xlogfile list is matained in a increasing order.
 * 
 * The input parameter is the name of the xlog  file, the name is assumpted
 * valid.
 */
static void 
InsertXLogFile( char * fname )
{
	XLogFileName * NewSegFile, *Curr, *Prev;
	bool append2end = false;

	/* Allocate a new node for the new file. */
	NewSegFile = (XLogFileName *) malloc(sizeof(XLogFileName));
	strcpy(NewSegFile->fname,fname); /* setup the name */
	/* extract the time line, logid, and segment number from the name.*/
	sscanf(fname, "%8x%8x%8x", &(NewSegFile->tli), &(NewSegFile->logid), &(NewSegFile->seg));
	NewSegFile->next = NULL;
	
	/* Ensure the xlog file is active and valid.*/
	if (! ValidXLogFileHeader(NewSegFile))
	{
		free(NewSegFile);
		return;
	}
	
	/* the list is empty.*/
	if ( xlogfilelist == NULL ) {
		xlogfilelist = NewSegFile;
		return;
	};

    /* try to search the list and find the insert point. */
	Prev=Curr=xlogfilelist;
	while( CmpXLogFileOT(NewSegFile, Curr))
    {
		/* the node is appended to the end of the list.*/
		if (Curr->next == NULL)
		{
			append2end = true;
			break;
		}
		Prev=Curr;
		Curr = Curr->next;
	}
	
	/* Insert the new node to the list.*/
	if ( append2end )
	{
		/* We need to append the new node to the end of the list */		
		Curr->next = NewSegFile;
	} 
	else 
	{
		NewSegFile->next = Curr;
		/* prev should not be the list head. */
		if ( Prev != NULL && Prev != xlogfilelist)
		{
			Prev->next = NewSegFile;
		}
	}
	/* Update the list head if it is needed.*/
	if ((Curr == xlogfilelist) && !append2end) 
	{
		xlogfilelist = NewSegFile;
	}
	
}

/*
 * compare two xlog file from their name to see which one is latest.
 *
 * Return true for file 2 is the lastest file.
 *
 */
static bool
CmpXLogFileOT(XLogFileName * f1, XLogFileName *f2)
{
        if (f2->tli >= f1->tli)
        {
                if (f2->logid >= f1->logid)
                {
                        if (f2->seg > f1->seg) return false;
                }
        }
        return true;

}

/* check is two segment file is continous.*/
static bool 
IsNextSeg(XLogFileName *prev, XLogFileName *cur)
{
	uint32 logid, logseg;
	
	if (prev->tli != cur->tli) return false;
	
	logid = prev->logid;
	logseg = prev->seg;
	NextLogSeg(logid, logseg);
	
	if ((logid == cur->logid) && (logseg == cur->seg)) return true;

	return false;

}


/*
* Select the oldest xlog file in the latest time line. 
*/
static void
SelectStartXLog( void )
{
	XLogFileName *tmp;
	CurXLogFile = xlogfilelist;
	
	if (xlogfilelist == NULL) 
	{
		return;
	}
	
	tmp=LastXLogFile=CurXLogFile=xlogfilelist;
	
	while(tmp->next != NULL)
	{
		
		/* 
		 * we should ensure that from the first to 
		 * the last segment file is continous.
		 * */
		if (!IsNextSeg(tmp, tmp->next)) 
		{
			CurXLogFile = tmp->next;
		}
		tmp=tmp->next;
	}

	LastXLogFile = tmp;

}

/*
 * Check if the file is a valid xlog file.
 *
 * Return true for the input file is a valid xlog file.
 * 
 * The input parameter is the name of the xlog file.
 * 
 */
static bool
ValidXLogFileName(char * fname)
{
	uint logTLI, logId, logSeg;
	if (strlen(fname) != 24 || 
	    strspn(fname, "0123456789ABCDEF") != 24 ||
	    sscanf(fname, "%8x%8x%8x", &logTLI, &logId, &logSeg) != 3)
		return false;
	return true;

}

/* Ensure the xlog file is active and valid.*/
static bool 
ValidXLogFileHeader(XLogFileName *segfile)
{
	int fd;
	char buffer[BLCKSZ];
	char		path[MAXPGPATH];
	size_t nread;
	
	snprintf(path, MAXPGPATH, "%s/%s", XLOGDIR, segfile->fname);
	fd = open(path, O_RDONLY | PG_BINARY, 0);
        if (fd < 0)
	{
		return false;
	}
	nread = read(fd, buffer, BLCKSZ);
	if (nread == BLCKSZ)
	{
		XLogPageHeader hdr = (XLogPageHeader)buffer;
		
		if (ValidXLOGPageHeader(hdr, segfile->tli, segfile->logid, segfile->seg))
		{
			return true;
		}

	}
	return false;

}
static bool
ValidXLOGPageHeader(XLogPageHeader hdr, uint tli, uint id, uint seg)
{
	XLogRecPtr	recaddr;

	if (hdr->xlp_magic != XLOG_PAGE_MAGIC)
	{
		return false;
	}
	if ((hdr->xlp_info & ~XLP_ALL_FLAGS) != 0)
	{
		return false;
	}
	if (hdr->xlp_info & XLP_LONG_HEADER)
	{
		XLogLongPageHeader longhdr = (XLogLongPageHeader) hdr;

		if (longhdr->xlp_seg_size != XLogSegSize)
		{
			return false;
		}
		/* Get the system identifier from the segment file header.*/
		sysidentifier = ((XLogLongPageHeader) pageBuffer)->xlp_sysid;
	}
		
	recaddr.xlogid = id;
	recaddr.xrecoff = seg * XLogSegSize + logPageOff;
	if (!XLByteEQ(hdr->xlp_pageaddr, recaddr))
	{
		return false;
	}

	if (hdr->xlp_tli != tli)
	{
		return false;
	}
	return true;
}


/* Read another page, if possible */
static bool
ReadXLogPage(void)
{
	size_t nread;
	
	/* Need to advance to the new segment file.*/
	if ( logPageOff >= XLogSegSize ) 
	{ 
		close(logFd);
		logFd = 0;
	}
	
	/* Need to open the segement file.*/
	if ((logFd <= 0) && (CurXLogFile != NULL))
	{
		if (OpenXLogFile(CurXLogFile) < 0)
		{
			return false;
		}
		CurXLogFile = CurXLogFile->next;
	}
	
	/* Read a page from the openning segement file.*/
	nread = read(logFd, pageBuffer, BLCKSZ);

	if (nread == BLCKSZ)
	{
		logPageOff += BLCKSZ;
		if (ValidXLOGPageHeader( (XLogPageHeader)pageBuffer, logTli, logId, logSeg))
			return true;
	} 
	
	return false;
}

/*
 * CRC-check an XLOG record.  We do not believe the contents of an XLOG
 * record (other than to the minimal extent of computing the amount of
 * data to read in) until we've checked the CRCs.
 *
 * We assume all of the record has been read into memory at *record.
 */
static bool
RecordIsValid(XLogRecord *record, XLogRecPtr recptr)
{
	pg_crc32	crc;
	int			i;
	uint32		len = record->xl_len;
	BkpBlock	bkpb;
	char	   *blk;

	/* First the rmgr data */
	INIT_CRC32(crc);
	COMP_CRC32(crc, XLogRecGetData(record), len);

	/* Add in the backup blocks, if any */
	blk = (char *) XLogRecGetData(record) + len;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		uint32	blen;

		if (!(record->xl_info & XLR_SET_BKP_BLOCK(i)))
			continue;

		memcpy(&bkpb, blk, sizeof(BkpBlock));
		if (bkpb.hole_offset + bkpb.hole_length > BLCKSZ)
		{
			return false;
		}
		blen = sizeof(BkpBlock) + BLCKSZ - bkpb.hole_length;
		COMP_CRC32(crc, blk, blen);
		blk += blen;
	}

	/* Check that xl_tot_len agrees with our calculation */
	if (blk != (char *) record + record->xl_tot_len)
	{
		return false;
	}

	/* Finally include the record header */
	COMP_CRC32(crc, (char *) record + sizeof(pg_crc32),
			   SizeOfXLogRecord - sizeof(pg_crc32));
	FIN_CRC32(crc);

	if (!EQ_CRC32(record->xl_crc, crc))
	{
		return false;
	}

	return true;
}



/*
 * Attempt to read an XLOG record into readRecordBuf.
 */
static bool
FetchRecord(void)
{
	char	   *buffer;
	XLogRecord *record;
	XLogContRecord *contrecord;
	uint32		len, total_len;


	while (logRecOff <= 0 || logRecOff > BLCKSZ - SizeOfXLogRecord)
	{
		/* Need to advance to new page */
		if (! ReadXLogPage())
		{
			return false;
		}
		
		logRecOff = XLogPageHeaderSize((XLogPageHeader) pageBuffer);
		if ((((XLogPageHeader) pageBuffer)->xlp_info & ~XLP_LONG_HEADER) != 0)
		{
			/* Check for a continuation record */
			if (((XLogPageHeader) pageBuffer)->xlp_info & XLP_FIRST_IS_CONTRECORD)
			{
				contrecord = (XLogContRecord *) (pageBuffer + logRecOff);
				logRecOff += MAXALIGN(contrecord->xl_rem_len + SizeOfXLogContRecord);
			}
		}
	}

	curRecPtr.xlogid = logId;
	curRecPtr.xrecoff = logSeg * XLogSegSize + logPageOff + logRecOff;
	record = (XLogRecord *) (pageBuffer + logRecOff);

	if (record->xl_len == 0)
	{
		return false;
	}

	total_len = record->xl_tot_len;

	/*
	 * Allocate or enlarge readRecordBuf as needed.  To avoid useless
	 * small increases, round its size to a multiple of BLCKSZ, and make
	 * sure it's at least 4*BLCKSZ to start with.  (That is enough for all
	 * "normal" records, but very large commit or abort records might need
	 * more space.)
	 */
	if (total_len > readRecordBufSize)
	{
		uint32		newSize = total_len;

		newSize += BLCKSZ - (newSize % BLCKSZ);
		newSize = Max(newSize, 4 * BLCKSZ);
		if (readRecordBuf)
			free(readRecordBuf);
		readRecordBuf = (char *) malloc(newSize);
		if (!readRecordBuf)
		{
			readRecordBufSize = 0;
			return false;
		}
		readRecordBufSize = newSize;
	}

	buffer = readRecordBuf;
	len = BLCKSZ - curRecPtr.xrecoff % BLCKSZ; /* available in block */
	if (total_len > len)
	{
		/* Need to reassemble record */
		uint32			gotlen = len;

		memcpy(buffer, record, len);
		record = (XLogRecord *) buffer;
		buffer += len;
		for (;;)
		{
			uint32	pageHeaderSize;

			if (!ReadXLogPage())
			{
				return false;
			}
			if (!(((XLogPageHeader) pageBuffer)->xlp_info & XLP_FIRST_IS_CONTRECORD))
			{
				return false;
			}
			pageHeaderSize = XLogPageHeaderSize((XLogPageHeader) pageBuffer);
			contrecord = (XLogContRecord *) (pageBuffer + pageHeaderSize);
			if (contrecord->xl_rem_len == 0 || 
				total_len != (contrecord->xl_rem_len + gotlen))
			{
				return false;
			}
			len = BLCKSZ - pageHeaderSize - SizeOfXLogContRecord;
			if (contrecord->xl_rem_len > len)
			{
				memcpy(buffer, (char *)contrecord + SizeOfXLogContRecord, len);
				gotlen += len;
				buffer += len;
				continue;
			}
			memcpy(buffer, (char *) contrecord + SizeOfXLogContRecord,
				   contrecord->xl_rem_len);
			logRecOff = MAXALIGN(pageHeaderSize + SizeOfXLogContRecord + contrecord->xl_rem_len);
			break;
		}
		if (!RecordIsValid(record, curRecPtr))
		{
			return false;
		}
		return true;
	}
	/* Record is contained in this page */
	memcpy(buffer, record, total_len);
	record = (XLogRecord *) buffer;
	logRecOff += MAXALIGN(total_len);
	if (!RecordIsValid(record, curRecPtr))
	{

		return false;
	}
	return true;
}

/*
 * if the record is checkpoint, update the lastest checkpoint record.
 */
static void
UpdateCheckPoint(XLogRecord *record)
{
	uint8	info = record->xl_info & ~XLR_INFO_MASK;
	
	if ((info == XLOG_CHECKPOINT_SHUTDOWN) ||
		(info == XLOG_CHECKPOINT_ONLINE))
	{
		 CheckPoint *chkpoint = (CheckPoint*) XLogRecGetData(record);
		 prevchkp = lastchkp;
		 lastchkp = curRecPtr;
		 lastcheckpoint = *chkpoint;
		 
		 /* update the database state.*/
		 switch(info)
		 {
			case XLOG_CHECKPOINT_SHUTDOWN:
				state = DB_SHUTDOWNED;
				break;
			case XLOG_CHECKPOINT_ONLINE:
				state = DB_IN_PRODUCTION;
				break;
		 }
		 found_checkpoint ++ ;
	}
}

static int
OpenXLogFile(XLogFileName *sf)
{

	char		path[MAXPGPATH];

	if ( logFd > 0 ) close(logFd);
	
	/* Open a  Xlog segment file. */
	snprintf(path, MAXPGPATH, "%s/%s", XLOGDIR, sf->fname);
	logFd = open(path, O_RDONLY | PG_BINARY, 0);
    
	if (logFd < 0)
	{
		fprintf(stderr, _("%s: Can not open xlog file %s.\n"), progname,path);		
		return -1;
	}
	
	/* Setup the parameter for searching. */
	logPageOff = -BLCKSZ;		/* so 1st increment in readXLogPage gives 0 */
	logRecOff = 0;
	logId = sf->logid;
	logSeg = sf->seg;
	logTli = sf->tli;
	return logFd;
}

/*
 * Search the lastest checkpoint in the lastest XLog segment file.
 *
 * The return value is the total checkpoints which had been found 
 * in the XLog segment file. 
 */
static int 
SearchLastCheckpoint(void)
{

	/* retrive all of the active xlog files from xlog direcotry 
	 * into a list by increasing order, according their timeline, 
	 * log id, segment id.
	*/
	GetXLogFiles();
	
	/* Select the oldest segment file in the lastest time line.*/
	SelectStartXLog();
	
	/* No segment file was found.*/
	if ( CurXLogFile == NULL ) 
	{
		return 0;
	}

	/* initial it . */
	logFd=logId=logSeg=logTli=0;

	/* 
	 * Search the XLog segment file from beginning to end, 
	 * if checkpoint record is found, then update the 
	 * latest check point.
	 */
	while (FetchRecord())
	{
		/* To see if the record is checkpoint record. */
		if (((XLogRecord *) readRecordBuf)->xl_rmid == RM_XLOG_ID)
			UpdateCheckPoint((XLogRecord *) readRecordBuf);
		prevRecPtr = curRecPtr;
	}

	/* We can not know clearly if we had reached the end.
	 * But just check if we reach the last segment file,
	 * if it is not, then some problem there.
	 * (We need a better way to know the abnormal broken during the search)
	 */
	if ((logId != LastXLogFile->logid) && (logSeg != LastXLogFile->seg))
	{
		return 0;
	}
	
	/* 
	 * return the checkpoints which had been found yet, 
	 * let others know how much checkpointes are found. 
	 */
	return found_checkpoint;
}

/* Clean up the allocated list.*/
static void
CleanUpList(XLogFileName *list)
{

	XLogFileName *tmp;
	tmp = list;
	while(list != NULL)
	{
		tmp=list->next;
		free(list);
		list=tmp;
	}
	
}

