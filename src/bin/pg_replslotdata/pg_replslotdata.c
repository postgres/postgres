/*-------------------------------------------------------------------------
 *
 * pg_replslotdata.c - provides information about the replication slots
 * from $PGDATA/pg_replslot/<slot_name>.
 *
 * Copyright (c) 2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_replslotdata/pg_replslotdata.c
 *-------------------------------------------------------------------------
 */
/*
 * We have to use postgres.h not postgres_fe.h here, because there's so much
 * backend-only stuff in the XLOG include files we need.  But we need a
 * frontend-ish environment otherwise.  Hence this ugly hack.
 */
#define FRONTEND 1

#include "postgres.h"

#include <dirent.h>
#include <sys/stat.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "common/logging.h"
#include "common/string.h"
#include "getopt_long.h"
#include "pg_getopt.h"
#include "pg_replslotdata.h"

#define PG_REPLSLOT_DIR "pg_replslot"

/*
 * Structure to hold the user-provided options.
 */
typedef struct replslotdata_opts
{
	char	*datadir;
	bool	verbose;
} replslotdata_opts;

/*
 * XXX TODO:
 * Add option to get replication slot with minimum restart_lsn.
 * Add option to get only logical or physical replication slots information.
 * Add option to get only minimum restart_lsn.
 */

static replslotdata_opts opts;

static void usage(const char *progname);
static DIR *get_destination_dir(char *dest_folder);
static void close_destination_dir(DIR *dest_dir, char *dest_folder);
static void process_replslots(void);
static void read_and_display_repl_slot(const char *name);

static void
usage(const char *progname)
{
	printf(_("%s Displays information about the replication slots from $PGDATA/pg_replslot/<slot_name>.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION] [DATADIR]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_(" [-D, --pgdata=]DATADIR  data directory\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -v, --verbose          write a lot of output\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nIf no data directory (DATADIR) is specified, "
			 "the environment variable PGDATA\nis used.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Get destination directory.
 */
static DIR *
get_destination_dir(char *dest_folder)
{
	DIR		   *dir;

	Assert(dest_folder != NULL);
	dir = opendir(dest_folder);
	if (dir == NULL)
	{
		pg_log_error("could not open directory \"%s\": %m", dest_folder);
		exit(1);
	}

	return dir;
}

/*
 * Close existing directory.
 */
static void
close_destination_dir(DIR *dest_dir, char *dest_folder)
{
	Assert(dest_dir != NULL && dest_folder != NULL);
	if (closedir(dest_dir))
	{
		pg_log_error("could not close directory \"%s\": %m", dest_folder);
		exit(1);
	}
}

/*
 * Loop over all the existing replication slots and display their information.
 */
static void
process_replslots(void)
{
	DIR	*rsdir;
	struct dirent *rsde;
	uint32	cnt = 0;

	rsdir = get_destination_dir(PG_REPLSLOT_DIR);

	printf("%-64s %9s %10s %11s %10s %12s %21s %21s %21s %21s %10s %20s\n"
		   "%-64s %9s %10s %11s %10s %12s %21s %21s %21s %21s %10s %20s\n",
		   "slot_name", "slot_type", "datoid", "persistency", "xmin", "catalog_xmin", "restart_lsn", "invalidated_at", "confirmed_flush", "two_phase_at", "two_phase", "plugin",
		   "---------", "---------", "------", "-----------", "----", "------------", "-----------", "--------------", "---------------", "------------", "---------", "------");

	while (errno = 0, (rsde = readdir(rsdir)) != NULL)
	{
		struct stat statbuf;
		char		path[MAXPGPATH];

		if (strcmp(rsde->d_name, ".") == 0 ||
			strcmp(rsde->d_name, "..") == 0)
			continue;

		snprintf(path, sizeof(path),
				 PG_REPLSLOT_DIR
				 "/%s",
				 rsde->d_name);

		/* we only care about directories here, skip if it's not one */
		if (lstat(path, &statbuf) == 0 && !S_ISDIR(statbuf.st_mode))
			continue;

		/* we crashed while a slot was being setup or deleted, clean up */
		if (pg_str_endswith(rsde->d_name, ".tmp"))
		{
			pg_log_warning("server was crashed while the slot \"%s\" was being setup or deleted",
						   rsde->d_name);
			continue;
		}

		/* looks like a slot in a normal state, decode its information */
		read_and_display_repl_slot(rsde->d_name);
		cnt++;
	}

	if (errno)
	{
		pg_log_error("could not read directory \"%s\": %m", PG_REPLSLOT_DIR);
		exit(1);
	}

	if (cnt == 0)
	{
		pg_log_info("no replication slots were found");
		exit(0);
	}

	close_destination_dir(rsdir, PG_REPLSLOT_DIR);
}

/*
 * Read given replication slot information from its disk file and display the
 * contents.
 */
static void
read_and_display_repl_slot(const char *name)
{
	ReplicationSlotOnDisk cp;
	char	slotdir[MAXPGPATH];
	char	path[MAXPGPATH];
	char	restart_lsn[NAMEDATALEN];
	char	invalidated_at[NAMEDATALEN];
	char	confirmed_flush[NAMEDATALEN];
	char	two_phase_at[NAMEDATALEN];
	char	persistency[NAMEDATALEN];
	int		fd;
	int		readBytes;
	pg_crc32c	checksum;

	/* delete temp file if it exists */
	sprintf(slotdir, PG_REPLSLOT_DIR"/%s", name);
	sprintf(path, "%s/state.tmp", slotdir);

	fd = open(path, O_RDONLY | PG_BINARY, 0);

	if (fd > 0)
	{
		pg_log_error("found temporary state file \"%s\": %m", path);
		exit(1);
	}

	sprintf(path, "%s/state", slotdir);

	if (opts.verbose)
		pg_log_info("reading replication slot from \"%s\"", path);

	fd = open(path, O_RDONLY | PG_BINARY, 0);

	/*
	 * We do not need to handle this as we are rename()ing the directory into
	 * place only after we fsync()ed the state file.
	 */
	if (fd < 0)
	{
		pg_log_error("could not open file \"%s\": %m", path);
		exit(1);
	}

	if (opts.verbose)
		pg_log_info("reading version independent replication slot state file");

	/* read part of statefile that's guaranteed to be version independent */
	readBytes = read(fd, &cp, ReplicationSlotOnDiskConstantSize);

	if (readBytes != ReplicationSlotOnDiskConstantSize)
	{
		if (readBytes < 0)
		{
			pg_log_error("could not read file \"%s\": %m", path);
			exit(1);
		}
		else
		{
			pg_log_error("could not read file \"%s\": read %d of %zu",
						 path, readBytes,
						 (Size) ReplicationSlotOnDiskConstantSize);
			exit(1);
		}
	}

	/* verify magic */
	if (cp.magic != SLOT_MAGIC)
	{
		pg_log_error("replication slot file \"%s\" has wrong magic number: %u instead of %u",
					 path, cp.magic, SLOT_MAGIC);
		exit(1);
	}

	/* verify version */
	if (cp.version != SLOT_VERSION)
	{
		pg_log_error("replication slot file \"%s\" has unsupported version %u",
					 path, cp.version);
		exit(1);
	}

	/* boundary check on length */
	if (cp.length != ReplicationSlotOnDiskV2Size)
	{
		pg_log_error("replication slot file \"%s\" has corrupted length %u",
					 path, cp.length);
		exit(1);
	}

	if (opts.verbose)
		pg_log_info("reading the entire replication slot state file");

	/* now that we know the size, read the entire file */
	readBytes = read(fd,
					 (char *) &cp + ReplicationSlotOnDiskConstantSize,
					 cp.length);

	if (readBytes != cp.length)
	{
		if (readBytes < 0)
		{
			pg_log_error("could not read file \"%s\": %m", path);
			exit(1);
		}
		else
		{
			pg_log_error("could not read file \"%s\": read %d of %zu",
						 path, readBytes, (Size) cp.length);
			exit(1);
		}
	}

	if (close(fd) != 0)
	{
		pg_log_error("could not close file \"%s\": %m", path);
		exit(1);
	}

	/* now verify the CRC */
	INIT_CRC32C(checksum);
	COMP_CRC32C(checksum,
				(char *) &cp + ReplicationSlotOnDiskNotChecksummedSize,
				ReplicationSlotOnDiskChecksummedSize);
	FIN_CRC32C(checksum);

	if (!EQ_CRC32C(checksum, cp.checksum))
	{
		pg_log_error("checksum mismatch for replication slot file \"%s\": is %u, should be %u",
					 path, checksum, cp.checksum);
		exit(1);
	}

	sprintf(restart_lsn, "%X/%X", LSN_FORMAT_ARGS(cp.slotdata.restart_lsn));
	sprintf(invalidated_at, "%X/%X", LSN_FORMAT_ARGS(cp.slotdata.invalidated_at));
	sprintf(confirmed_flush, "%X/%X", LSN_FORMAT_ARGS(cp.slotdata.confirmed_flush));
	sprintf(two_phase_at, "%X/%X", LSN_FORMAT_ARGS(cp.slotdata.two_phase_at));

	if (cp.slotdata.persistency == RS_PERSISTENT)
		sprintf(persistency, "persistent");
	else if (cp.slotdata.persistency == RS_EPHEMERAL)
		sprintf(persistency, "ephemeral");
	else if (cp.slotdata.persistency == RS_TEMPORARY)
		sprintf(persistency, "temporary");

	/* display the slot information */
	printf("%-64s %9s %10u %11s %10u %12u %21s %21s %21s %21s %10d %20s\n",
		   NameStr(cp.slotdata.name),
		   cp.slotdata.database == InvalidOid ? "physical" : "logical",
		   cp.slotdata.database,
		   persistency,
		   cp.slotdata.xmin,
		   cp.slotdata.catalog_xmin,
		   restart_lsn,
		   invalidated_at,
		   confirmed_flush,
		   two_phase_at,
		   cp.slotdata.two_phase,
		   NameStr(cp.slotdata.plugin));
}

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"pgdata", required_argument, NULL, 'D'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};

	const char	*progname;
	int	c;
	DIR	*dir;

	MemSet(&opts, 0, sizeof(opts));

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_replslotdata"));
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
			puts("pg_replslotdata (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "D:v", long_options, NULL)) != -1)
	{
		switch (c)
		{
			case 'D':
				opts.datadir = optarg;
				break;
			case 'v':
				opts.verbose = true;
				break;
			default:
				goto bad_argument;
		}
	}

	if (opts.datadir  == NULL)
	{
		if (optind < argc)
			opts.datadir  = argv[optind++];
		else
			opts.datadir  = getenv("PGDATA");
	}

	/* complain if any arguments remain */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		goto bad_argument;
	}

	if (opts.datadir == NULL)
	{
		pg_log_error("no data directory specified");
		goto bad_argument;
	}

	if (opts.verbose)
		pg_log_info("data directory is \"%s\"", opts.datadir);

	/* check existence of destination folder */
	dir = get_destination_dir(opts.datadir);
	close_destination_dir(dir, opts.datadir);

	if (chdir(opts.datadir) < 0)
	{
		pg_log_error("could not change directory to \"%s\": %m",
					 opts.datadir);
		exit(1);
	}

	/* everything looks okay so far, let's process the replication slots */
	process_replslots();

	return EXIT_SUCCESS;

bad_argument:
	fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
	return EXIT_FAILURE;
}
