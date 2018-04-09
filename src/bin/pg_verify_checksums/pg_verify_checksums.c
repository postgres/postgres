/*
 * pg_verify_checksums
 *
 * Verifies page level checksums in an offline cluster
 *
 *	Copyright (c) 2010-2018, PostgreSQL Global Development Group
 *
 *	src/bin/pg_verify_checksums/pg_verify_checksums.c
 */

#define FRONTEND 1

#include "postgres.h"
#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "pg_getopt.h"


static int64 files = 0;
static int64 blocks = 0;
static int64 badblocks = 0;
static ControlFileData *ControlFile;

static char *only_relfilenode = NULL;
static bool debug = false;

static const char *progname;

static void
usage()
{
	printf(_("%s verifies page level checksums in offline PostgreSQL database cluster.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION] [DATADIR]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_(" [-D] DATADIR    data directory\n"));
	printf(_("  -f,            force check even if checksums are disabled\n"));
	printf(_("  -r relfilenode check only relation with specified relfilenode\n"));
	printf(_("  -d             debug output, listing all checked blocks\n"));
	printf(_("  -V, --version  output version information, then exit\n"));
	printf(_("  -?, --help     show this help, then exit\n"));
	printf(_("\nIf no data directory (DATADIR) is specified, "
			 "the environment variable PGDATA\nis used.\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}

static const char *skip[] = {
	"pg_control",
	"pg_filenode.map",
	"pg_internal.init",
	"PG_VERSION",
	NULL,
};

static bool
skipfile(char *fn)
{
	const char **f;

	if (strcmp(fn, ".") == 0 ||
		strcmp(fn, "..") == 0)
		return true;

	for (f = skip; *f; f++)
		if (strcmp(*f, fn) == 0)
			return true;
	return false;
}

static void
scan_file(char *fn, int segmentno)
{
	char		buf[BLCKSZ];
	PageHeader	header = (PageHeader) buf;
	int			f;
	int			blockno;

	f = open(fn, 0);
	if (f < 0)
	{
		fprintf(stderr, _("%s: could not open file \"%s\": %m\n"), progname, fn);
		exit(1);
	}

	files++;

	for (blockno = 0;; blockno++)
	{
		uint16		csum;
		int			r = read(f, buf, BLCKSZ);

		if (r == 0)
			break;
		if (r != BLCKSZ)
		{
			fprintf(stderr, _("%s: short read of block %d in file \"%s\", got only %d bytes\n"),
					progname, blockno, fn, r);
			exit(1);
		}
		blocks++;

		csum = pg_checksum_page(buf, blockno + segmentno * RELSEG_SIZE);
		if (csum != header->pd_checksum)
		{
			if (ControlFile->data_checksum_version == PG_DATA_CHECKSUM_VERSION)
				fprintf(stderr, _("%s: checksum verification failed in file \"%s\", block %d: calculated checksum %X but expected %X\n"),
						progname, fn, blockno, csum, header->pd_checksum);
			badblocks++;
		}
		else if (debug)
			fprintf(stderr, _("%s: checksum verified in file \"%s\", block %d: %X\n"),
					progname, fn, blockno, csum);
	}

	close(f);
}

static void
scan_directory(char *basedir, char *subdir)
{
	char		path[MAXPGPATH];
	DIR		   *dir;
	struct dirent *de;

	snprintf(path, sizeof(path), "%s/%s", basedir, subdir);
	dir = opendir(path);
	if (!dir)
	{
		fprintf(stderr, _("%s: could not open directory \"%s\": %m\n"),
				progname, path);
		exit(1);
	}
	while ((de = readdir(dir)) != NULL)
	{
		char		fn[MAXPGPATH + 1];
		struct stat st;

		if (skipfile(de->d_name))
			continue;

		snprintf(fn, sizeof(fn), "%s/%s", path, de->d_name);
		if (lstat(fn, &st) < 0)
		{
			fprintf(stderr, _("%s: could not stat file \"%s\": %m\n"),
					progname, fn);
			exit(1);
		}
		if (S_ISREG(st.st_mode))
		{
			char	   *forkpath,
					   *segmentpath;
			int			segmentno = 0;

			/*
			 * Cut off at the segment boundary (".") to get the segment number
			 * in order to mix it into the checksum. Then also cut off at the
			 * fork boundary, to get the relfilenode the file belongs to for
			 * filtering.
			 */
			segmentpath = strchr(de->d_name, '.');
			if (segmentpath != NULL)
			{
				*segmentpath++ = '\0';
				segmentno = atoi(segmentpath);
				if (segmentno == 0)
				{
					fprintf(stderr, _("%s: invalid segment number %d in filename \"%s\"\n"),
							progname, segmentno, fn);
					exit(1);
				}
			}

			forkpath = strchr(de->d_name, '_');
			if (forkpath != NULL)
				*forkpath++ = '\0';

			if (only_relfilenode && strcmp(only_relfilenode, de->d_name) != 0)
				/* Relfilenode not to be included */
				continue;

			scan_file(fn, segmentno);
		}
#ifndef WIN32
		else if (S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
#else
		else if (S_ISDIR(st.st_mode) || pgwin32_is_junction(fn))
#endif
			scan_directory(path, de->d_name);
	}
	closedir(dir);
}

int
main(int argc, char *argv[])
{
	char	   *DataDir = NULL;
	bool		force = false;
	int			c;
	bool		crc_ok;

	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_verify_checksums"));

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
			puts("pg_verify_checksums (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt(argc, argv, "D:fr:d")) != -1)
	{
		switch (c)
		{
			case 'd':
				debug = true;
				break;
			case 'D':
				DataDir = optarg;
				break;
			case 'f':
				force = true;
				break;
			case 'r':
				if (atoi(optarg) <= 0)
				{
					fprintf(stderr, _("%s: invalid relfilenode: %s\n"), progname, optarg);
					exit(1);
				}
				only_relfilenode = pstrdup(optarg);
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	if (DataDir == NULL)
	{
		if (optind < argc)
			DataDir = argv[optind++];
		else
			DataDir = getenv("PGDATA");

		/* If no DataDir was specified, and none could be found, error out */
		if (DataDir == NULL)
		{
			fprintf(stderr, _("%s: no data directory specified\n"), progname);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
			exit(1);
		}
	}

	/* Complain if any arguments remain */
	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/* Check if cluster is running */
	ControlFile = get_controlfile(DataDir, progname, &crc_ok);
	if (!crc_ok)
	{
		fprintf(stderr, _("%s: pg_control CRC value is incorrect.\n"), progname);
		exit(1);
	}

	if (ControlFile->state != DB_SHUTDOWNED &&
		ControlFile->state != DB_SHUTDOWNED_IN_RECOVERY)
	{
		fprintf(stderr, _("%s: cluster must be shut down to verify checksums.\n"), progname);
		exit(1);
	}

	if (ControlFile->data_checksum_version == 0 && !force)
	{
		fprintf(stderr, _("%s: data checksums are not enabled in cluster.\n"), progname);
		exit(1);
	}

	/* Scan all files */
	scan_directory(DataDir, "global");
	scan_directory(DataDir, "base");
	scan_directory(DataDir, "pg_tblspc");

	printf(_("Checksum scan completed\n"));
	printf(_("Data checksum version: %d\n"), ControlFile->data_checksum_version);
	printf(_("Files scanned:  %" INT64_MODIFIER "d\n"), files);
	printf(_("Blocks scanned: %" INT64_MODIFIER "d\n"), blocks);
	printf(_("Bad checksums:  %" INT64_MODIFIER "d\n"), badblocks);

	if (badblocks > 0)
		return 1;

	return 0;
}
