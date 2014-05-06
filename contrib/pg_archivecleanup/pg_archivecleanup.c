/*
 * $PostgreSQL: pgsql/contrib/pg_archivecleanup/pg_archivecleanup.c,v 1.3.2.1 2010/08/23 02:56:29 tgl Exp $
 *
 * pg_archivecleanup.c
 *
 * Production-ready example of an archive_cleanup_command
 * used to clean an archive when using standby_mode = on in 9.0
 * or for standalone use for any version of PostgreSQL 8.0+.
 *
 * Original author:		Simon Riggs  simon@2ndquadrant.com
 * Current maintainer:	Simon Riggs
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

#ifndef WIN32
#include <sys/time.h>
#include <unistd.h>

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#else	/* WIN32 */
extern int	getopt(int argc, char *const argv[], const char *optstring);
#endif	/* ! WIN32 */

extern char *optarg;
extern int	optind;

const char *progname;

/* Options and defaults */
bool		debug = false;		/* are we debugging? */

char	   *archiveLocation;	/* where to find the archive? */
char	   *restartWALFileName; /* the file from which we can restart restore */
char		WALFilePath[MAXPGPATH];		/* the file path including archive */
char		exclusiveCleanupFileName[MAXPGPATH];		/* the oldest file we
														 * want to remain in
														 * archive */


/* =====================================================================
 *
 *		  Customizable section
 *
 * =====================================================================
 *
 *	Currently, this section assumes that the Archive is a locally
 *	accessible directory. If you want to make other assumptions,
 *	such as using a vendor-specific archive and access API, these
 *	routines are the ones you'll need to change. You're
 *	enouraged to submit any changes to pgsql-hackers@postgresql.org
 *	or personally to the current maintainer. Those changes may be
 *	folded in to later versions of this program.
 */

#define XLOG_DATA_FNAME_LEN		24
/* Reworked from access/xlog_internal.h */
#define XLogFileName(fname, tli, log, seg)	\
	snprintf(fname, XLOG_DATA_FNAME_LEN + 1, "%08X%08X%08X", tli, log, seg)
#define XLOG_BACKUP_FNAME_LEN	40

/*
 *	Initialize allows customized commands into the archive cleanup program.
 *
 *	You may wish to add code to check for tape libraries, etc..
 */
static void
Initialize(void)
{
	/*
	 * This code assumes that archiveLocation is a directory, so we use stat
	 * to test if it's accessible.
	 */
	struct stat stat_buf;

	if (stat(archiveLocation, &stat_buf) != 0 ||
		!S_ISDIR(stat_buf.st_mode))
	{
		fprintf(stderr, "%s: archiveLocation \"%s\" does not exist\n",
				progname, archiveLocation);
		exit(2);
	}
}

static void
CleanupPriorWALFiles(void)
{
	int			rc;
	DIR		   *xldir;
	struct dirent *xlde;

	if ((xldir = opendir(archiveLocation)) != NULL)
	{
		while (errno = 0, (xlde = readdir(xldir)) != NULL)
		{
			/*
			 * We ignore the timeline part of the XLOG segment identifiers in
			 * deciding whether a segment is still needed.  This ensures that
			 * we won't prematurely remove a segment from a parent timeline.
			 * We could probably be a little more proactive about removing
			 * segments of non-parent timelines, but that would be a whole lot
			 * more complicated.
			 *
			 * We use the alphanumeric sorting property of the filenames to
			 * decide which ones are earlier than the exclusiveCleanupFileName
			 * file. Note that this means files are not removed in the order
			 * they were originally written, in case this worries you.
			 */
			if (strlen(xlde->d_name) == XLOG_DATA_FNAME_LEN &&
			strspn(xlde->d_name, "0123456789ABCDEF") == XLOG_DATA_FNAME_LEN &&
				strcmp(xlde->d_name + 8, exclusiveCleanupFileName + 8) < 0)
			{
				snprintf(WALFilePath, MAXPGPATH, "%s/%s",
						 archiveLocation, xlde->d_name);
				if (debug)
					fprintf(stderr, "%s: removing file \"%s\"\n",
							progname, WALFilePath);

				rc = unlink(WALFilePath);
				if (rc != 0)
				{
					fprintf(stderr, "%s: ERROR: could not remove file \"%s\": %s\n",
							progname, WALFilePath, strerror(errno));
					break;
				}
			}
		}

#ifdef WIN32
		/* Bug in old Mingw dirent.c;  fixed in mingw-runtime-3.2, 2003-10-10 */
		if (GetLastError() == ERROR_NO_MORE_FILES)
			errno = 0;
#endif

		if (errno)
			fprintf(stderr, "%s: could not read archive location \"%s\": %s\n",
					progname, archiveLocation, strerror(errno));
		if (closedir(xldir))
			fprintf(stderr, "%s: could not close archive location \"%s\": %s\n",
					progname, archiveLocation, strerror(errno));
	}
	else
		fprintf(stderr, "%s: could not open archiveLocation \"%s\": %s\n",
				progname, archiveLocation, strerror(errno));
}

/*
 * SetWALFileNameForCleanup()
 *
 *	  Set the earliest WAL filename that we want to keep on the archive
 *	  and decide whether we need_cleanup
 */
static void
SetWALFileNameForCleanup(void)
{
	bool		fnameOK = false;

	/*
	 * If restartWALFileName is a WAL file name then just use it directly. If
	 * restartWALFileName is a .backup filename, make sure we use the prefix
	 * of the filename, otherwise we will remove wrong files since
	 * 000000010000000000000010.00000020.backup is after
	 * 000000010000000000000010.
	 */
	if (strlen(restartWALFileName) == XLOG_DATA_FNAME_LEN &&
		strspn(restartWALFileName, "0123456789ABCDEF") == XLOG_DATA_FNAME_LEN)
	{
		strcpy(exclusiveCleanupFileName, restartWALFileName);
		fnameOK = true;
	}
	else if (strlen(restartWALFileName) == XLOG_BACKUP_FNAME_LEN)
	{
		int			args;
		uint32		tli = 1,
					log = 0,
					seg = 0,
					offset = 0;

		args = sscanf(restartWALFileName, "%08X%08X%08X.%08X.backup", &tli, &log, &seg, &offset);
		if (args == 4)
		{
			fnameOK = true;

			/*
			 * Use just the prefix of the filename, ignore everything after
			 * first period
			 */
			XLogFileName(exclusiveCleanupFileName, tli, log, seg);
		}
	}

	if (!fnameOK)
	{
		fprintf(stderr, "%s: invalid filename input\n", progname);
		fprintf(stderr, "Try \"%s --help\" for more information.\n", progname);
		exit(2);
	}
}

/* =====================================================================
 *		  End of Customizable section
 * =====================================================================
 */

static void
usage(void)
{
	printf("%s removes older WAL files from PostgreSQL archives.\n\n", progname);
	printf("Usage:\n");
	printf("  %s [OPTION]... ARCHIVELOCATION OLDESTKEPTWALFILE\n", progname);
	printf("\n"
		   "for use as an archive_cleanup_command in the recovery.conf when standby_mode = on:\n"
		   "  archive_cleanup_command = 'pg_archivecleanup [OPTION]... ARCHIVELOCATION %%r'\n"
		   "e.g.\n"
		   "  archive_cleanup_command = 'pg_archivecleanup /mnt/server/archiverdir %%r'\n");
	printf("\n"
		   "or for use as a standalone archive cleaner:\n"
		   "e.g.\n"
		   "  pg_archivecleanup /mnt/server/archiverdir 000000010000000000000010.00000020.backup\n");
	printf("\nOptions:\n");
	printf("  -d                 generates debug output (verbose mode)\n");
	printf("  --help             show this help, then exit\n");
	printf("  --version          output version information, then exit\n");
	printf("\nReport bugs to <pgsql-bugs@postgresql.org>.\n");
}

/*------------ MAIN ----------------------------------------*/
int
main(int argc, char **argv)
{
	int			c;

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
			puts("pg_archivecleanup (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt(argc, argv, "d")) != -1)
	{
		switch (c)
		{
			case 'd':			/* Debug mode */
				debug = true;
				break;
			default:
				fprintf(stderr, "Try \"%s --help\" for more information.\n", progname);
				exit(2);
				break;
		}
	}

	/*
	 * We will go to the archiveLocation to check restartWALFileName.
	 * restartWALFileName may not exist anymore, which would not be an error,
	 * so we separate the archiveLocation and restartWALFileName so we can
	 * check separately whether archiveLocation exists, if not that is an
	 * error
	 */
	if (optind < argc)
	{
		archiveLocation = argv[optind];
		optind++;
	}
	else
	{
		fprintf(stderr, "%s: must specify archive location\n", progname);
		fprintf(stderr, "Try \"%s --help\" for more information.\n", progname);
		exit(2);
	}

	if (optind < argc)
	{
		restartWALFileName = argv[optind];
		optind++;
	}
	else
	{
		fprintf(stderr, "%s: must specify restartfilename\n", progname);
		fprintf(stderr, "Try \"%s --help\" for more information.\n", progname);
		exit(2);
	}

	if (optind < argc)
	{
		fprintf(stderr, "%s: too many parameters\n", progname);
		fprintf(stderr, "Try \"%s --help\" for more information.\n", progname);
		exit(2);
	}

	/*
	 * Check archive exists and other initialization if required.
	 */
	Initialize();

	/*
	 * Check filename is a valid name, then process to find cut-off
	 */
	SetWALFileNameForCleanup();

	if (debug)
	{
		snprintf(WALFilePath, MAXPGPATH, "%s/%s",
				 archiveLocation, exclusiveCleanupFileName);
		fprintf(stderr, "%s: keep WAL file \"%s\" and later\n",
				progname, WALFilePath);
	}

	/*
	 * Remove WAL files older than cut-off
	 */
	CleanupPriorWALFiles();

	exit(0);
}
