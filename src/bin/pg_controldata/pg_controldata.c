/*
 * pg_controldata
 *
 * reads the data from $PGDATA/global/pg_control
 *
 * copyright (c) Oliver Elphick <olly@lfix.co.uk>, 2001;
 * licence: BSD
 *
 * $Header: /cvsroot/pgsql/src/bin/pg_controldata/pg_controldata.c,v 1.11 2003/08/04 00:43:27 momjian Exp $
 */
#include "postgres.h"

#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <locale.h>
#include <errno.h>

#include "catalog/pg_control.h"

#define _(x) gettext((x))


static void
usage(const char *progname)
{
	printf(_("%s displays control information of a PostgreSQL database cluster.\n\n"), progname);
	printf
		(
		 _(
		   "Usage:\n"
		   "  %s [OPTION]\n\n"
		   "Options:\n"
		"  DATADIR        show cluster control information for DATADIR\n"
		   "  --help         show this help, then exit\n"
		   "  --version      output version information, then exit\n"
		   ),
		 progname
		);
	printf(_("\nIf no data directory is specified, the environment variable PGDATA\nis used.\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}


static const char *
dbState(DBState state)
{
	switch (state)
	{
		case DB_STARTUP:
			return _("starting up");
		case DB_SHUTDOWNED:
			return _("shut down");
		case DB_SHUTDOWNING:
			return _("shutting down");
		case DB_IN_RECOVERY:
			return _("in recovery");
		case DB_IN_PRODUCTION:
			return _("in production");
	}
	return _("unrecognized status code");
}


int
main(int argc, char *argv[])
{
	ControlFileData ControlFile;
	int			fd;
	char		ControlFilePath[MAXPGPATH];
	char	   *DataDir;
	crc64		crc;
	char		pgctime_str[32];
	char		ckpttime_str[32];
	char	   *strftime_fmt = "%c";
	char	   *progname;

	setlocale(LC_ALL, "");
#ifdef ENABLE_NLS
	bindtextdomain("pg_controldata", LOCALEDIR);
	textdomain("pg_controldata");
#endif

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_controldata (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	if (argc > 1)
		DataDir = argv[1];
	else
		DataDir = getenv("PGDATA");
	if (DataDir == NULL)
	{
		fprintf(stderr, _("%s: no data directory specified\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);

	if ((fd = open(ControlFilePath, O_RDONLY)) == -1)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, ControlFilePath, strerror(errno));
		exit(2);
	}

	if (read(fd, &ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
	{
		fprintf(stderr, _("%s: could not read file \"%s\": %s\n"),
				progname, ControlFilePath, strerror(errno));
		exit(2);
	}
	close(fd);

	/* Check the CRC. */
	INIT_CRC64(crc);
	COMP_CRC64(crc,
			   (char *) &ControlFile + sizeof(crc64),
			   sizeof(ControlFileData) - sizeof(crc64));
	FIN_CRC64(crc);

	if (!EQ_CRC64(crc, ControlFile.crc))
		printf(_("WARNING: Calculated CRC checksum does not match value stored in file.\n"
				 "Either the file is corrupt, or it has a different layout than this program\n"
			 "is expecting.  The results below are untrustworthy.\n\n"));

	/*
	 * Use variable for format to suppress overly-anal-retentive gcc
	 * warning about %c
	 */
	strftime(pgctime_str, sizeof(pgctime_str), strftime_fmt,
			 localtime(&(ControlFile.time)));
	strftime(ckpttime_str, sizeof(ckpttime_str), strftime_fmt,
			 localtime(&(ControlFile.checkPointCopy.time)));

	printf(_("pg_control version number:            %u\n"), ControlFile.pg_control_version);
	printf(_("Catalog version number:               %u\n"), ControlFile.catalog_version_no);
	printf(_("Database cluster state:               %s\n"), dbState(ControlFile.state));
	printf(_("pg_control last modified:             %s\n"), pgctime_str);
	printf(_("Current log file ID:                  %u\n"), ControlFile.logId);
	printf(_("Next log file segment:                %u\n"), ControlFile.logSeg);
	printf(_("Latest checkpoint location:           %X/%X\n"),
		   ControlFile.checkPoint.xlogid, ControlFile.checkPoint.xrecoff);
	printf(_("Prior checkpoint location:            %X/%X\n"),
		   ControlFile.prevCheckPoint.xlogid, ControlFile.prevCheckPoint.xrecoff);
	printf(_("Latest checkpoint's REDO location:    %X/%X\n"),
		   ControlFile.checkPointCopy.redo.xlogid, ControlFile.checkPointCopy.redo.xrecoff);
	printf(_("Latest checkpoint's UNDO location:    %X/%X\n"),
		   ControlFile.checkPointCopy.undo.xlogid, ControlFile.checkPointCopy.undo.xrecoff);
	printf(_("Latest checkpoint's StartUpID:        %u\n"), ControlFile.checkPointCopy.ThisStartUpID);
	printf(_("Latest checkpoint's NextXID:          %u\n"), ControlFile.checkPointCopy.nextXid);
	printf(_("Latest checkpoint's NextOID:          %u\n"), ControlFile.checkPointCopy.nextOid);
	printf(_("Time of latest checkpoint:            %s\n"), ckpttime_str);
	printf(_("Database block size:                  %u\n"), ControlFile.blcksz);
	printf(_("Blocks per segment of large relation: %u\n"), ControlFile.relseg_size);
	printf(_("Maximum length of identifiers:        %u\n"), ControlFile.nameDataLen);
	printf(_("Maximum number of function arguments: %u\n"), ControlFile.funcMaxArgs);
	printf(_("Date/time type storage:               %s\n"),
		   (ControlFile.enableIntTimes ? _("64-bit integers") : _("floating-point numbers")));
	printf(_("Maximum length of locale name:        %u\n"), ControlFile.localeBuflen);
	printf(_("LC_COLLATE:                           %s\n"), ControlFile.lc_collate);
	printf(_("LC_CTYPE:                             %s\n"), ControlFile.lc_ctype);

	return 0;
}
