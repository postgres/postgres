/*-------------------------------------------------------------------------
 *
 * pg_combinebackup.c
 *		Combine incremental backups with prior backups.
 *
 * Copyright (c) 2017-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/pg_combinebackup/pg_combinebackup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>

#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

#include "backup_label.h"
#include "common/blkreftable.h"
#include "common/checksum_helper.h"
#include "common/controldata_utils.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "copy_file.h"
#include "fe_utils/option_utils.h"
#include "getopt_long.h"
#include "lib/stringinfo.h"
#include "load_manifest.h"
#include "reconstruct.h"
#include "write_manifest.h"

/* Incremental file naming convention. */
#define INCREMENTAL_PREFIX			"INCREMENTAL."
#define INCREMENTAL_PREFIX_LENGTH	(sizeof(INCREMENTAL_PREFIX) - 1)

/*
 * Tracking for directories that need to be removed, or have their contents
 * removed, if the operation fails.
 */
typedef struct cb_cleanup_dir
{
	char	   *target_path;
	bool		rmtopdir;
	struct cb_cleanup_dir *next;
} cb_cleanup_dir;

/*
 * Stores a tablespace mapping provided using -T, --tablespace-mapping.
 */
typedef struct cb_tablespace_mapping
{
	char		old_dir[MAXPGPATH];
	char		new_dir[MAXPGPATH];
	struct cb_tablespace_mapping *next;
} cb_tablespace_mapping;

/*
 * Stores data parsed from all command-line options.
 */
typedef struct cb_options
{
	bool		debug;
	char	   *output;
	bool		dry_run;
	bool		no_sync;
	cb_tablespace_mapping *tsmappings;
	pg_checksum_type manifest_checksums;
	bool		no_manifest;
	DataDirSyncMethod sync_method;
	CopyMethod	copy_method;
} cb_options;

/*
 * Data about a tablespace.
 *
 * Every normal tablespace needs a tablespace mapping, but in-place tablespaces
 * don't, so the list of tablespaces can contain more entries than the list of
 * tablespace mappings.
 */
typedef struct cb_tablespace
{
	Oid			oid;
	bool		in_place;
	char		old_dir[MAXPGPATH];
	char		new_dir[MAXPGPATH];
	struct cb_tablespace *next;
} cb_tablespace;

/* Directories to be removed if we exit uncleanly. */
static cb_cleanup_dir *cleanup_dir_list = NULL;

static void add_tablespace_mapping(cb_options *opt, char *arg);
static StringInfo check_backup_label_files(int n_backups, char **backup_dirs);
static uint64 check_control_files(int n_backups, char **backup_dirs);
static void check_input_dir_permissions(char *dir);
static void cleanup_directories_atexit(void);
static void create_output_directory(char *dirname, cb_options *opt);
static void help(const char *progname);
static bool parse_oid(char *s, Oid *result);
static void process_directory_recursively(Oid tsoid,
										  char *input_directory,
										  char *output_directory,
										  char *relative_path,
										  int n_prior_backups,
										  char **prior_backup_dirs,
										  manifest_data **manifests,
										  manifest_writer *mwriter,
										  cb_options *opt);
static int	read_pg_version_file(char *directory);
static void remember_to_cleanup_directory(char *target_path, bool rmtopdir);
static void reset_directory_cleanup_list(void);
static cb_tablespace *scan_for_existing_tablespaces(char *pathname,
													cb_options *opt);
static void slurp_file(int fd, char *filename, StringInfo buf, int maxlen);

/*
 * Main program.
 */
int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"debug", no_argument, NULL, 'd'},
		{"dry-run", no_argument, NULL, 'n'},
		{"no-sync", no_argument, NULL, 'N'},
		{"output", required_argument, NULL, 'o'},
		{"tablespace-mapping", required_argument, NULL, 'T'},
		{"manifest-checksums", required_argument, NULL, 1},
		{"no-manifest", no_argument, NULL, 2},
		{"sync-method", required_argument, NULL, 3},
		{"clone", no_argument, NULL, 4},
		{"copy", no_argument, NULL, 5},
		{"copy-file-range", no_argument, NULL, 6},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	char	   *last_input_dir;
	int			i;
	int			optindex;
	int			c;
	int			n_backups;
	int			n_prior_backups;
	int			version;
	uint64		system_identifier;
	char	  **prior_backup_dirs;
	cb_options	opt;
	cb_tablespace *tablespaces;
	cb_tablespace *ts;
	StringInfo	last_backup_label;
	manifest_data **manifests;
	manifest_writer *mwriter;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_combinebackup"));
	handle_help_version_opts(argc, argv, progname, help);

	memset(&opt, 0, sizeof(opt));
	opt.manifest_checksums = CHECKSUM_TYPE_CRC32C;
	opt.sync_method = DATA_DIR_SYNC_METHOD_FSYNC;
	opt.copy_method = COPY_METHOD_COPY;

	/* process command-line options */
	while ((c = getopt_long(argc, argv, "dnNo:T:",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'd':
				opt.debug = true;
				pg_logging_increase_verbosity();
				break;
			case 'n':
				opt.dry_run = true;
				break;
			case 'N':
				opt.no_sync = true;
				break;
			case 'o':
				opt.output = optarg;
				break;
			case 'T':
				add_tablespace_mapping(&opt, optarg);
				break;
			case 1:
				if (!pg_checksum_parse_type(optarg,
											&opt.manifest_checksums))
					pg_fatal("unrecognized checksum algorithm: \"%s\"",
							 optarg);
				break;
			case 2:
				opt.no_manifest = true;
				break;
			case 3:
				if (!parse_sync_method(optarg, &opt.sync_method))
					exit(1);
				break;
			case 4:
				opt.copy_method = COPY_METHOD_CLONE;
				break;
			case 5:
				opt.copy_method = COPY_METHOD_COPY;
				break;
			case 6:
				opt.copy_method = COPY_METHOD_COPY_FILE_RANGE;
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (optind >= argc)
	{
		pg_log_error("no input directories specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (opt.output == NULL)
		pg_fatal("no output directory specified");

	/* If no manifest is needed, no checksums are needed, either. */
	if (opt.no_manifest)
		opt.manifest_checksums = CHECKSUM_TYPE_NONE;

	/* Check that the platform supports the requested copy method. */
	if (opt.copy_method == COPY_METHOD_CLONE)
	{
#if (defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)) || \
	(defined(__linux__) && defined(FICLONE))

		if (opt.dry_run)
			pg_log_debug("would use cloning to copy files");
		else
			pg_log_debug("will use cloning to copy files");

#else
		pg_fatal("file cloning not supported on this platform");
#endif
	}
	else if (opt.copy_method == COPY_METHOD_COPY_FILE_RANGE)
	{
#if defined(HAVE_COPY_FILE_RANGE)

		if (opt.dry_run)
			pg_log_debug("would use copy_file_range to copy blocks");
		else
			pg_log_debug("will use copy_file_range to copy blocks");

#else
		pg_fatal("copy_file_range not supported on this platform");
#endif
	}

	/* Read the server version from the final backup. */
	version = read_pg_version_file(argv[argc - 1]);

	/* Sanity-check control files. */
	n_backups = argc - optind;
	system_identifier = check_control_files(n_backups, argv + optind);

	/* Sanity-check backup_label files, and get the contents of the last one. */
	last_backup_label = check_backup_label_files(n_backups, argv + optind);

	/*
	 * We'll need the pathnames to the prior backups. By "prior" we mean all
	 * but the last one listed on the command line.
	 */
	n_prior_backups = argc - optind - 1;
	prior_backup_dirs = argv + optind;

	/* Load backup manifests. */
	manifests = load_backup_manifests(n_backups, prior_backup_dirs);

	/*
	 * Validate the manifest system identifier against the backup system
	 * identifier.
	 */
	for (i = 0; i < n_backups; i++)
	{
		if (manifests[i] &&
			manifests[i]->system_identifier != system_identifier)
		{
			char	   *controlpath;

			controlpath = psprintf("%s/%s", prior_backup_dirs[i], "global/pg_control");

			pg_fatal("%s: manifest system identifier is %llu, but control file has %llu",
					 controlpath,
					 (unsigned long long) manifests[i]->system_identifier,
					 (unsigned long long) system_identifier);
		}
	}

	/* Figure out which tablespaces are going to be included in the output. */
	last_input_dir = argv[argc - 1];
	check_input_dir_permissions(last_input_dir);
	tablespaces = scan_for_existing_tablespaces(last_input_dir, &opt);

	/*
	 * Create output directories.
	 *
	 * We create one output directory for the main data directory plus one for
	 * each non-in-place tablespace. create_output_directory() will arrange
	 * for those directories to be cleaned up on failure. In-place tablespaces
	 * aren't handled at this stage because they're located beneath the main
	 * output directory, and thus the cleanup of that directory will get rid
	 * of them. Plus, the pg_tblspc directory that needs to contain them
	 * doesn't exist yet.
	 */
	atexit(cleanup_directories_atexit);
	create_output_directory(opt.output, &opt);
	for (ts = tablespaces; ts != NULL; ts = ts->next)
		if (!ts->in_place)
			create_output_directory(ts->new_dir, &opt);

	/* If we need to write a backup_manifest, prepare to do so. */
	if (!opt.dry_run && !opt.no_manifest)
	{
		mwriter = create_manifest_writer(opt.output, system_identifier);

		/*
		 * Verify that we have a backup manifest for the final backup; else we
		 * won't have the WAL ranges for the resulting manifest.
		 */
		if (manifests[n_prior_backups] == NULL)
			pg_fatal("cannot generate a manifest because no manifest is available for the final input backup");
	}
	else
		mwriter = NULL;

	/* Write backup label into output directory. */
	if (opt.dry_run)
		pg_log_debug("would generate \"%s/backup_label\"", opt.output);
	else
	{
		pg_log_debug("generating \"%s/backup_label\"", opt.output);
		last_backup_label->cursor = 0;
		write_backup_label(opt.output, last_backup_label,
						   opt.manifest_checksums, mwriter);
	}

	/* Process everything that's not part of a user-defined tablespace. */
	pg_log_debug("processing backup directory \"%s\"", last_input_dir);
	process_directory_recursively(InvalidOid, last_input_dir, opt.output,
								  NULL, n_prior_backups, prior_backup_dirs,
								  manifests, mwriter, &opt);

	/* Process user-defined tablespaces. */
	for (ts = tablespaces; ts != NULL; ts = ts->next)
	{
		pg_log_debug("processing tablespace directory \"%s\"", ts->old_dir);

		/*
		 * If it's a normal tablespace, we need to set up a symbolic link from
		 * pg_tblspc/${OID} to the target directory; if it's an in-place
		 * tablespace, we need to create a directory at pg_tblspc/${OID}.
		 */
		if (!ts->in_place)
		{
			char		linkpath[MAXPGPATH];

			snprintf(linkpath, MAXPGPATH, "%s/pg_tblspc/%u", opt.output,
					 ts->oid);

			if (opt.dry_run)
				pg_log_debug("would create symbolic link from \"%s\" to \"%s\"",
							 linkpath, ts->new_dir);
			else
			{
				pg_log_debug("creating symbolic link from \"%s\" to \"%s\"",
							 linkpath, ts->new_dir);
				if (symlink(ts->new_dir, linkpath) != 0)
					pg_fatal("could not create symbolic link from \"%s\" to \"%s\": %m",
							 linkpath, ts->new_dir);
			}
		}
		else
		{
			if (opt.dry_run)
				pg_log_debug("would create directory \"%s\"", ts->new_dir);
			else
			{
				pg_log_debug("creating directory \"%s\"", ts->new_dir);
				if (pg_mkdir_p(ts->new_dir, pg_dir_create_mode) == -1)
					pg_fatal("could not create directory \"%s\": %m",
							 ts->new_dir);
			}
		}

		/* OK, now handle the directory contents. */
		process_directory_recursively(ts->oid, ts->old_dir, ts->new_dir,
									  NULL, n_prior_backups, prior_backup_dirs,
									  manifests, mwriter, &opt);
	}

	/* Finalize the backup_manifest, if we're generating one. */
	if (mwriter != NULL)
		finalize_manifest(mwriter,
						  manifests[n_prior_backups]->first_wal_range);

	/* fsync that output directory unless we've been told not to do so */
	if (!opt.no_sync)
	{
		if (opt.dry_run)
			pg_log_debug("would recursively fsync \"%s\"", opt.output);
		else
		{
			pg_log_debug("recursively fsyncing \"%s\"", opt.output);
			sync_pgdata(opt.output, version * 10000, opt.sync_method);
		}
	}

	/* It's a success, so don't remove the output directories. */
	reset_directory_cleanup_list();
	exit(0);
}

/*
 * Process the option argument for the -T, --tablespace-mapping switch.
 */
static void
add_tablespace_mapping(cb_options *opt, char *arg)
{
	cb_tablespace_mapping *tsmap = pg_malloc0(sizeof(cb_tablespace_mapping));
	char	   *dst;
	char	   *dst_ptr;
	char	   *arg_ptr;

	/*
	 * Basically, we just want to copy everything before the equals sign to
	 * tsmap->old_dir and everything afterwards to tsmap->new_dir, but if
	 * there's more or less than one equals sign, that's an error, and if
	 * there's an equals sign preceded by a backslash, don't treat it as a
	 * field separator but instead copy a literal equals sign.
	 */
	dst_ptr = dst = tsmap->old_dir;
	for (arg_ptr = arg; *arg_ptr != '\0'; arg_ptr++)
	{
		if (dst_ptr - dst >= MAXPGPATH)
			pg_fatal("directory name too long");

		if (*arg_ptr == '\\' && *(arg_ptr + 1) == '=')
			;					/* skip backslash escaping = */
		else if (*arg_ptr == '=' && (arg_ptr == arg || *(arg_ptr - 1) != '\\'))
		{
			if (tsmap->new_dir[0] != '\0')
				pg_fatal("multiple \"=\" signs in tablespace mapping");
			else
				dst = dst_ptr = tsmap->new_dir;
		}
		else
			*dst_ptr++ = *arg_ptr;
	}
	if (!tsmap->old_dir[0] || !tsmap->new_dir[0])
		pg_fatal("invalid tablespace mapping format \"%s\", must be \"OLDDIR=NEWDIR\"", arg);

	/*
	 * All tablespaces are created with absolute directories, so specifying a
	 * non-absolute path here would never match, possibly confusing users.
	 *
	 * In contrast to pg_basebackup, both the old and new directories are on
	 * the local machine, so the local machine's definition of an absolute
	 * path is the only relevant one.
	 */
	if (!is_absolute_path(tsmap->old_dir))
		pg_fatal("old directory is not an absolute path in tablespace mapping: %s",
				 tsmap->old_dir);

	if (!is_absolute_path(tsmap->new_dir))
		pg_fatal("old directory is not an absolute path in tablespace mapping: %s",
				 tsmap->new_dir);

	/* Canonicalize paths to avoid spurious failures when comparing. */
	canonicalize_path(tsmap->old_dir);
	canonicalize_path(tsmap->new_dir);

	/* Add it to the list. */
	tsmap->next = opt->tsmappings;
	opt->tsmappings = tsmap;
}

/*
 * Check that the backup_label files form a coherent backup chain, and return
 * the contents of the backup_label file from the latest backup.
 */
static StringInfo
check_backup_label_files(int n_backups, char **backup_dirs)
{
	StringInfo	buf = makeStringInfo();
	StringInfo	lastbuf = buf;
	int			i;
	TimeLineID	check_tli = 0;
	XLogRecPtr	check_lsn = InvalidXLogRecPtr;

	/* Try to read each backup_label file in turn, last to first. */
	for (i = n_backups - 1; i >= 0; --i)
	{
		char		pathbuf[MAXPGPATH];
		int			fd;
		TimeLineID	start_tli;
		TimeLineID	previous_tli;
		XLogRecPtr	start_lsn;
		XLogRecPtr	previous_lsn;

		/* Open the backup_label file. */
		snprintf(pathbuf, MAXPGPATH, "%s/backup_label", backup_dirs[i]);
		pg_log_debug("reading \"%s\"", pathbuf);
		if ((fd = open(pathbuf, O_RDONLY, 0)) < 0)
			pg_fatal("could not open file \"%s\": %m", pathbuf);

		/*
		 * Slurp the whole file into memory.
		 *
		 * The exact size limit that we impose here doesn't really matter --
		 * most of what's supposed to be in the file is fixed size and quite
		 * short. However, the length of the backup_label is limited (at least
		 * by some parts of the code) to MAXPGPATH, so include that value in
		 * the maximum length that we tolerate.
		 */
		slurp_file(fd, pathbuf, buf, 10000 + MAXPGPATH);

		/* Close the file. */
		if (close(fd) != 0)
			pg_fatal("could not close file \"%s\": %m", pathbuf);

		/* Parse the file contents. */
		parse_backup_label(pathbuf, buf, &start_tli, &start_lsn,
						   &previous_tli, &previous_lsn);

		/*
		 * Sanity checks.
		 *
		 * XXX. It's actually not required that start_lsn == check_lsn. It
		 * would be OK if start_lsn > check_lsn provided that start_lsn is
		 * less than or equal to the relevant switchpoint. But at the moment
		 * we don't have that information.
		 */
		if (i > 0 && previous_tli == 0)
			pg_fatal("backup at \"%s\" is a full backup, but only the first backup should be a full backup",
					 backup_dirs[i]);
		if (i == 0 && previous_tli != 0)
			pg_fatal("backup at \"%s\" is an incremental backup, but the first backup should be a full backup",
					 backup_dirs[i]);
		if (i < n_backups - 1 && start_tli != check_tli)
			pg_fatal("backup at \"%s\" starts on timeline %u, but expected %u",
					 backup_dirs[i], start_tli, check_tli);
		if (i < n_backups - 1 && start_lsn != check_lsn)
			pg_fatal("backup at \"%s\" starts at LSN %X/%X, but expected %X/%X",
					 backup_dirs[i],
					 LSN_FORMAT_ARGS(start_lsn),
					 LSN_FORMAT_ARGS(check_lsn));
		check_tli = previous_tli;
		check_lsn = previous_lsn;

		/*
		 * The last backup label in the chain needs to be saved for later use,
		 * while the others are only needed within this loop.
		 */
		if (lastbuf == buf)
			buf = makeStringInfo();
		else
			resetStringInfo(buf);
	}

	/* Free memory that we don't need any more. */
	if (lastbuf != buf)
		destroyStringInfo(buf);

	/*
	 * Return the data from the first backup_info that we read (which is the
	 * backup_label from the last directory specified on the command line).
	 */
	return lastbuf;
}

/*
 * Sanity check control files and return system_identifier.
 */
static uint64
check_control_files(int n_backups, char **backup_dirs)
{
	int			i;
	uint64		system_identifier = 0;	/* placate compiler */
	uint32		data_checksum_version = 0;	/* placate compiler */
	bool		data_checksum_mismatch = false;

	/* Try to read each control file in turn, last to first. */
	for (i = n_backups - 1; i >= 0; --i)
	{
		ControlFileData *control_file;
		bool		crc_ok;
		char	   *controlpath;

		controlpath = psprintf("%s/%s", backup_dirs[i], "global/pg_control");
		pg_log_debug("reading \"%s\"", controlpath);
		control_file = get_controlfile_by_exact_path(controlpath, &crc_ok);

		/* Control file contents not meaningful if CRC is bad. */
		if (!crc_ok)
			pg_fatal("%s: CRC is incorrect", controlpath);

		/* Can't interpret control file if not current version. */
		if (control_file->pg_control_version != PG_CONTROL_VERSION)
			pg_fatal("%s: unexpected control file version",
					 controlpath);

		/* System identifiers should all match. */
		if (i == n_backups - 1)
			system_identifier = control_file->system_identifier;
		else if (system_identifier != control_file->system_identifier)
			pg_fatal("%s: expected system identifier %llu, but found %llu",
					 controlpath, (unsigned long long) system_identifier,
					 (unsigned long long) control_file->system_identifier);

		/*
		 * Detect checksum mismatches, but only if the last backup in the
		 * chain has checksums enabled.
		 */
		if (i == n_backups - 1)
			data_checksum_version = control_file->data_checksum_version;
		else if (data_checksum_version != 0 &&
				 data_checksum_version != control_file->data_checksum_version)
			data_checksum_mismatch = true;

		/* Release memory. */
		pfree(control_file);
		pfree(controlpath);
	}

	/*
	 * If debug output is enabled, make a note of the system identifier that
	 * we found in all of the relevant control files.
	 */
	pg_log_debug("system identifier is %llu",
				 (unsigned long long) system_identifier);

	/*
	 * Warn the user if not all backups are in the same state with regards to
	 * checksums.
	 */
	if (data_checksum_mismatch)
	{
		pg_log_warning("only some backups have checksums enabled");
		pg_log_warning_hint("Disable, and optionally reenable, checksums on the output directory to avoid failures.");
	}

	return system_identifier;
}

/*
 * Set default permissions for new files and directories based on the
 * permissions of the given directory. The intent here is that the output
 * directory should use the same permissions scheme as the final input
 * directory.
 */
static void
check_input_dir_permissions(char *dir)
{
	struct stat st;

	if (stat(dir, &st) != 0)
		pg_fatal("could not stat file \"%s\": %m", dir);

	SetDataDirectoryCreatePerm(st.st_mode);
}

/*
 * Clean up output directories before exiting.
 */
static void
cleanup_directories_atexit(void)
{
	while (cleanup_dir_list != NULL)
	{
		cb_cleanup_dir *dir = cleanup_dir_list;

		if (dir->rmtopdir)
		{
			pg_log_info("removing output directory \"%s\"", dir->target_path);
			if (!rmtree(dir->target_path, dir->rmtopdir))
				pg_log_error("failed to remove output directory");
		}
		else
		{
			pg_log_info("removing contents of output directory \"%s\"",
						dir->target_path);
			if (!rmtree(dir->target_path, dir->rmtopdir))
				pg_log_error("failed to remove contents of output directory");
		}

		cleanup_dir_list = cleanup_dir_list->next;
		pfree(dir);
	}
}

/*
 * Create the named output directory, unless it already exists or we're in
 * dry-run mode. If it already exists but is not empty, that's a fatal error.
 *
 * Adds the created directory to the list of directories to be cleaned up
 * at process exit.
 */
static void
create_output_directory(char *dirname, cb_options *opt)
{
	switch (pg_check_dir(dirname))
	{
		case 0:
			if (opt->dry_run)
			{
				pg_log_debug("would create directory \"%s\"", dirname);
				return;
			}
			pg_log_debug("creating directory \"%s\"", dirname);
			if (pg_mkdir_p(dirname, pg_dir_create_mode) == -1)
				pg_fatal("could not create directory \"%s\": %m", dirname);
			remember_to_cleanup_directory(dirname, true);
			break;

		case 1:
			pg_log_debug("using existing directory \"%s\"", dirname);
			remember_to_cleanup_directory(dirname, false);
			break;

		case 2:
		case 3:
		case 4:
			pg_fatal("directory \"%s\" exists but is not empty", dirname);

		case -1:
			pg_fatal("could not access directory \"%s\": %m", dirname);
	}
}

/*
 * help
 *
 * Prints help page for the program
 *
 * progname: the name of the executed program, such as "pg_combinebackup"
 */
static void
help(const char *progname)
{
	printf(_("%s reconstructs full backups from incrementals.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... DIRECTORY...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -d, --debug               generate lots of debugging output\n"));
	printf(_("  -n, --dry-run             do not actually do anything\n"));
	printf(_("  -N, --no-sync             do not wait for changes to be written safely to disk\n"));
	printf(_("  -o, --output=DIRECTORY    output directory\n"));
	printf(_("  -T, --tablespace-mapping=OLDDIR=NEWDIR\n"
			 "                            relocate tablespace in OLDDIR to NEWDIR\n"));
	printf(_("      --clone               clone (reflink) files instead of copying\n"));
	printf(_("      --copy                copy files (default)\n"));
	printf(_("      --copy-file-range     copy using copy_file_range() system call\n"));
	printf(_("      --manifest-checksums=SHA{224,256,384,512}|CRC32C|NONE\n"
			 "                            use algorithm for manifest checksums\n"));
	printf(_("      --no-manifest         suppress generation of backup manifest\n"));
	printf(_("      --sync-method=METHOD  set method for syncing files to disk\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("  -?, --help                show this help, then exit\n"));

	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Try to parse a string as a non-zero OID without leading zeroes.
 *
 * If it works, return true and set *result to the answer, else return false.
 */
static bool
parse_oid(char *s, Oid *result)
{
	Oid			oid;
	char	   *ep;

	errno = 0;
	oid = strtoul(s, &ep, 10);
	if (errno != 0 || *ep != '\0' || oid < 1 || oid > PG_UINT32_MAX)
		return false;

	*result = oid;
	return true;
}

/*
 * Copy files from the input directory to the output directory, reconstructing
 * full files from incremental files as required.
 *
 * If processing is a user-defined tablespace, the tsoid should be the OID
 * of that tablespace and input_directory and output_directory should be the
 * toplevel input and output directories for that tablespace. Otherwise,
 * tsoid should be InvalidOid and input_directory and output_directory should
 * be the main input and output directories.
 *
 * relative_path is the path beneath the given input and output directories
 * that we are currently processing. If NULL, it indicates that we're
 * processing the input and output directories themselves.
 *
 * n_prior_backups is the number of prior backups that we have available.
 * This doesn't count the very last backup, which is referenced by
 * output_directory, just the older ones. prior_backup_dirs is an array of
 * the locations of those previous backups.
 */
static void
process_directory_recursively(Oid tsoid,
							  char *input_directory,
							  char *output_directory,
							  char *relative_path,
							  int n_prior_backups,
							  char **prior_backup_dirs,
							  manifest_data **manifests,
							  manifest_writer *mwriter,
							  cb_options *opt)
{
	char		ifulldir[MAXPGPATH];
	char		ofulldir[MAXPGPATH];
	char		manifest_prefix[MAXPGPATH];
	DIR		   *dir;
	struct dirent *de;
	bool		is_pg_tblspc = false;
	bool		is_pg_wal = false;
	bool		is_incremental_dir = false;
	manifest_data *latest_manifest = manifests[n_prior_backups];
	pg_checksum_type checksum_type;

	/*
	 * Classify this directory.
	 *
	 * We set is_pg_tblspc only for the toplevel pg_tblspc directory, because
	 * the symlinks in that specific directory require special handling.
	 *
	 * We set is_pg_wal for the toplevel WAL directory and all of its
	 * subdirectories, because those files are not included in the backup
	 * manifest and hence need special treatement. (Since incremental backup
	 * does not exist in pre-v10 versions, we don't have to worry about the
	 * old pg_xlog naming.)
	 *
	 * We set is_incremental_dir for directories that can contain incremental
	 * files requiring reconstruction. If such files occur outside these
	 * directories, we want to just copy them straight to the output
	 * directory. This is to protect against a user creating a file with a
	 * strange name like INCREMENTAL.config and then complaining that
	 * incremental backups don't work properly. The test here is a bit tricky:
	 * incremental files occur in subdirectories of base, in pg_global itself,
	 * and in subdirectories of pg_tblspc only if in-place tablespaces are
	 * used.
	 */
	if (OidIsValid(tsoid))
		is_incremental_dir = true;
	else if (relative_path != NULL)
	{
		is_pg_tblspc = strcmp(relative_path, "pg_tblspc") == 0;
		is_pg_wal = (strcmp(relative_path, "pg_wal") == 0 ||
					 strncmp(relative_path, "pg_wal/", 7) == 0);
		is_incremental_dir = strncmp(relative_path, "base/", 5) == 0 ||
			strcmp(relative_path, "global") == 0 ||
			strncmp(relative_path, "pg_tblspc/", 10) == 0;
	}

	/*
	 * If we're under pg_wal, then we don't need checksums, because these
	 * files aren't included in the backup manifest. Otherwise use whatever
	 * type of checksum is configured.
	 */
	if (!is_pg_wal)
		checksum_type = opt->manifest_checksums;
	else
		checksum_type = CHECKSUM_TYPE_NONE;

	/*
	 * Append the relative path to the input and output directories, and
	 * figure out the appropriate prefix to add to files in this directory
	 * when looking them up in a backup manifest.
	 */
	if (relative_path == NULL)
	{
		strlcpy(ifulldir, input_directory, MAXPGPATH);
		strlcpy(ofulldir, output_directory, MAXPGPATH);
		if (OidIsValid(tsoid))
			snprintf(manifest_prefix, MAXPGPATH, "pg_tblspc/%u/", tsoid);
		else
			manifest_prefix[0] = '\0';
	}
	else
	{
		snprintf(ifulldir, MAXPGPATH, "%s/%s", input_directory,
				 relative_path);
		snprintf(ofulldir, MAXPGPATH, "%s/%s", output_directory,
				 relative_path);
		if (OidIsValid(tsoid))
			snprintf(manifest_prefix, MAXPGPATH, "pg_tblspc/%u/%s/",
					 tsoid, relative_path);
		else
			snprintf(manifest_prefix, MAXPGPATH, "%s/", relative_path);
	}

	/*
	 * Toplevel output directories have already been created by the time this
	 * function is called, but any subdirectories are our responsibility.
	 */
	if (relative_path != NULL)
	{
		if (opt->dry_run)
			pg_log_debug("would create directory \"%s\"", ofulldir);
		else
		{
			pg_log_debug("creating directory \"%s\"", ofulldir);
			if (mkdir(ofulldir, pg_dir_create_mode) == -1)
				pg_fatal("could not create directory \"%s\": %m", ofulldir);
		}
	}

	/* It's time to scan the directory. */
	if ((dir = opendir(ifulldir)) == NULL)
		pg_fatal("could not open directory \"%s\": %m", ifulldir);
	while (errno = 0, (de = readdir(dir)) != NULL)
	{
		PGFileType	type;
		char		ifullpath[MAXPGPATH];
		char		ofullpath[MAXPGPATH];
		char		manifest_path[MAXPGPATH];
		Oid			oid = InvalidOid;
		int			checksum_length = 0;
		uint8	   *checksum_payload = NULL;
		pg_checksum_context checksum_ctx;

		/* Ignore "." and ".." entries. */
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		/* Construct input path. */
		snprintf(ifullpath, MAXPGPATH, "%s/%s", ifulldir, de->d_name);

		/* Figure out what kind of directory entry this is. */
		type = get_dirent_type(ifullpath, de, false, PG_LOG_ERROR);
		if (type == PGFILETYPE_ERROR)
			exit(1);

		/*
		 * If we're processing pg_tblspc, then check whether the filename
		 * looks like it could be a tablespace OID. If so, and if the
		 * directory entry is a symbolic link or a directory, skip it.
		 *
		 * Our goal here is to ignore anything that would have been considered
		 * by scan_for_existing_tablespaces to be a tablespace.
		 */
		if (is_pg_tblspc && parse_oid(de->d_name, &oid) &&
			(type == PGFILETYPE_LNK || type == PGFILETYPE_DIR))
			continue;

		/* If it's a directory, recurse. */
		if (type == PGFILETYPE_DIR)
		{
			char		new_relative_path[MAXPGPATH];

			/* Append new pathname component to relative path. */
			if (relative_path == NULL)
				strlcpy(new_relative_path, de->d_name, MAXPGPATH);
			else
				snprintf(new_relative_path, MAXPGPATH, "%s/%s", relative_path,
						 de->d_name);

			/* And recurse. */
			process_directory_recursively(tsoid,
										  input_directory, output_directory,
										  new_relative_path,
										  n_prior_backups, prior_backup_dirs,
										  manifests, mwriter, opt);
			continue;
		}

		/* Skip anything that's not a regular file. */
		if (type != PGFILETYPE_REG)
		{
			if (type == PGFILETYPE_LNK)
				pg_log_warning("skipping symbolic link \"%s\"", ifullpath);
			else
				pg_log_warning("skipping special file \"%s\"", ifullpath);
			continue;
		}

		/*
		 * Skip the backup_label and backup_manifest files; they require
		 * special handling and are handled elsewhere.
		 */
		if (relative_path == NULL &&
			(strcmp(de->d_name, "backup_label") == 0 ||
			 strcmp(de->d_name, "backup_manifest") == 0))
			continue;

		/*
		 * If it's an incremental file, hand it off to the reconstruction
		 * code, which will figure out what to do.
		 */
		if (is_incremental_dir &&
			strncmp(de->d_name, INCREMENTAL_PREFIX,
					INCREMENTAL_PREFIX_LENGTH) == 0)
		{
			/* Output path should not include "INCREMENTAL." prefix. */
			snprintf(ofullpath, MAXPGPATH, "%s/%s", ofulldir,
					 de->d_name + INCREMENTAL_PREFIX_LENGTH);


			/* Manifest path likewise omits incremental prefix. */
			snprintf(manifest_path, MAXPGPATH, "%s%s", manifest_prefix,
					 de->d_name + INCREMENTAL_PREFIX_LENGTH);

			/* Reconstruction logic will do the rest. */
			reconstruct_from_incremental_file(ifullpath, ofullpath,
											  manifest_prefix,
											  de->d_name + INCREMENTAL_PREFIX_LENGTH,
											  n_prior_backups,
											  prior_backup_dirs,
											  manifests,
											  manifest_path,
											  checksum_type,
											  &checksum_length,
											  &checksum_payload,
											  opt->copy_method,
											  opt->debug,
											  opt->dry_run);
		}
		else
		{
			/* Construct the path that the backup_manifest will use. */
			snprintf(manifest_path, MAXPGPATH, "%s%s", manifest_prefix,
					 de->d_name);

			/*
			 * It's not an incremental file, so we need to copy the entire
			 * file to the output directory.
			 *
			 * If a checksum of the required type already exists in the
			 * backup_manifest for the final input directory, we can save some
			 * work by reusing that checksum instead of computing a new one.
			 */
			if (checksum_type != CHECKSUM_TYPE_NONE &&
				latest_manifest != NULL)
			{
				manifest_file *mfile;

				mfile = manifest_files_lookup(latest_manifest->files,
											  manifest_path);
				if (mfile == NULL)
				{
					char	   *bmpath;

					/*
					 * The directory is out of sync with the backup_manifest,
					 * so emit a warning.
					 */
					bmpath = psprintf("%s/%s", input_directory,
									  "backup_manifest");
					pg_log_warning("manifest file \"%s\" contains no entry for file \"%s\"",
								   bmpath, manifest_path);
					pfree(bmpath);
				}
				else if (mfile->checksum_type == checksum_type)
				{
					checksum_length = mfile->checksum_length;
					checksum_payload = mfile->checksum_payload;
				}
			}

			/*
			 * If we're reusing a checksum, then we don't need copy_file() to
			 * compute one for us, but otherwise, it needs to compute whatever
			 * type of checksum we need.
			 */
			if (checksum_length != 0)
				pg_checksum_init(&checksum_ctx, CHECKSUM_TYPE_NONE);
			else
				pg_checksum_init(&checksum_ctx, checksum_type);

			/* Actually copy the file. */
			snprintf(ofullpath, MAXPGPATH, "%s/%s", ofulldir, de->d_name);
			copy_file(ifullpath, ofullpath, &checksum_ctx,
					  opt->copy_method, opt->dry_run);

			/*
			 * If copy_file() performed a checksum calculation for us, then
			 * save the results (except in dry-run mode, when there's no
			 * point).
			 */
			if (checksum_ctx.type != CHECKSUM_TYPE_NONE && !opt->dry_run)
			{
				checksum_payload = pg_malloc(PG_CHECKSUM_MAX_LENGTH);
				checksum_length = pg_checksum_final(&checksum_ctx,
													checksum_payload);
			}
		}

		/* Generate manifest entry, if needed. */
		if (mwriter != NULL)
		{
			struct stat sb;

			/*
			 * In order to generate a manifest entry, we need the file size
			 * and mtime. We have no way to know the correct mtime except to
			 * stat() the file, so just do that and get the size as well.
			 *
			 * If we didn't need the mtime here, we could try to obtain the
			 * file size from the reconstruction or file copy process above,
			 * although that is actually not convenient in all cases. If we
			 * write the file ourselves then clearly we can keep a count of
			 * bytes, but if we use something like CopyFile() then it's
			 * trickier. Since we have to stat() anyway to get the mtime,
			 * there's no point in worrying about it.
			 */
			if (stat(ofullpath, &sb) < 0)
				pg_fatal("could not stat file \"%s\": %m", ofullpath);

			/* OK, now do the work. */
			add_file_to_manifest(mwriter, manifest_path,
								 sb.st_size, sb.st_mtime,
								 checksum_type, checksum_length,
								 checksum_payload);
		}

		/* Avoid leaking memory. */
		if (checksum_payload != NULL)
			pfree(checksum_payload);
	}

	closedir(dir);
}

/*
 * Read the version number from PG_VERSION and convert it to the usual server
 * version number format. (e.g. If PG_VERSION contains "14\n" this function
 * will return 140000)
 */
static int
read_pg_version_file(char *directory)
{
	char		filename[MAXPGPATH];
	StringInfoData buf;
	int			fd;
	int			version;
	char	   *ep;

	/* Construct pathname. */
	snprintf(filename, MAXPGPATH, "%s/PG_VERSION", directory);

	/* Open file. */
	if ((fd = open(filename, O_RDONLY, 0)) < 0)
		pg_fatal("could not open file \"%s\": %m", filename);

	/* Read into memory. Length limit of 128 should be more than generous. */
	initStringInfo(&buf);
	slurp_file(fd, filename, &buf, 128);

	/* Close the file. */
	if (close(fd) != 0)
		pg_fatal("could not close file \"%s\": %m", filename);

	/* Convert to integer. */
	errno = 0;
	version = strtoul(buf.data, &ep, 10);
	if (errno != 0 || *ep != '\n')
	{
		/*
		 * Incremental backup is not relevant to very old server versions that
		 * used multi-part version number (e.g. 9.6, or 8.4). So if we see
		 * what looks like the beginning of such a version number, just bail
		 * out.
		 */
		if (version < 10 && *ep == '.')
			pg_fatal("%s: server version too old", filename);
		pg_fatal("%s: could not parse version number", filename);
	}

	/* Debugging output. */
	pg_log_debug("read server version %d from file \"%s\"", version, filename);

	/* Release memory and return result. */
	pfree(buf.data);
	return version * 10000;
}

/*
 * Add a directory to the list of output directories to clean up.
 */
static void
remember_to_cleanup_directory(char *target_path, bool rmtopdir)
{
	cb_cleanup_dir *dir = pg_malloc(sizeof(cb_cleanup_dir));

	dir->target_path = target_path;
	dir->rmtopdir = rmtopdir;
	dir->next = cleanup_dir_list;
	cleanup_dir_list = dir;
}

/*
 * Empty out the list of directories scheduled for cleanup at exit.
 *
 * We want to remove the output directories only on a failure, so call this
 * function when we know that the operation has succeeded.
 *
 * Since we only expect this to be called when we're about to exit, we could
 * just set cleanup_dir_list to NULL and be done with it, but we free the
 * memory to be tidy.
 */
static void
reset_directory_cleanup_list(void)
{
	while (cleanup_dir_list != NULL)
	{
		cb_cleanup_dir *dir = cleanup_dir_list;

		cleanup_dir_list = cleanup_dir_list->next;
		pfree(dir);
	}
}

/*
 * Scan the pg_tblspc directory of the final input backup to get a canonical
 * list of what tablespaces are part of the backup.
 *
 * 'pathname' should be the path to the toplevel backup directory for the
 * final backup in the backup chain.
 */
static cb_tablespace *
scan_for_existing_tablespaces(char *pathname, cb_options *opt)
{
	char		pg_tblspc[MAXPGPATH];
	DIR		   *dir;
	struct dirent *de;
	cb_tablespace *tslist = NULL;

	snprintf(pg_tblspc, MAXPGPATH, "%s/pg_tblspc", pathname);
	pg_log_debug("scanning \"%s\"", pg_tblspc);

	if ((dir = opendir(pg_tblspc)) == NULL)
		pg_fatal("could not open directory \"%s\": %m", pg_tblspc);

	while (errno = 0, (de = readdir(dir)) != NULL)
	{
		Oid			oid;
		char		tblspcdir[MAXPGPATH];
		char		link_target[MAXPGPATH];
		int			link_length;
		cb_tablespace *ts;
		cb_tablespace *otherts;
		PGFileType	type;

		/* Silently ignore "." and ".." entries. */
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		/* Construct full pathname. */
		snprintf(tblspcdir, MAXPGPATH, "%s/%s", pg_tblspc, de->d_name);

		/* Ignore any file name that doesn't look like a proper OID. */
		if (!parse_oid(de->d_name, &oid))
		{
			pg_log_debug("skipping \"%s\" because the filename is not a legal tablespace OID",
						 tblspcdir);
			continue;
		}

		/* Only symbolic links and directories are tablespaces. */
		type = get_dirent_type(tblspcdir, de, false, PG_LOG_ERROR);
		if (type == PGFILETYPE_ERROR)
			exit(1);
		if (type != PGFILETYPE_LNK && type != PGFILETYPE_DIR)
		{
			pg_log_debug("skipping \"%s\" because it is neither a symbolic link nor a directory",
						 tblspcdir);
			continue;
		}

		/* Create a new tablespace object. */
		ts = pg_malloc0(sizeof(cb_tablespace));
		ts->oid = oid;

		/*
		 * If it's a link, it's not an in-place tablespace. Otherwise, it must
		 * be a directory, and thus an in-place tablespace.
		 */
		if (type == PGFILETYPE_LNK)
		{
			cb_tablespace_mapping *tsmap;

			/* Read the link target. */
			link_length = readlink(tblspcdir, link_target, sizeof(link_target));
			if (link_length < 0)
				pg_fatal("could not read symbolic link \"%s\": %m",
						 tblspcdir);
			if (link_length >= sizeof(link_target))
				pg_fatal("target of symbolic link \"%s\" is too long", tblspcdir);
			link_target[link_length] = '\0';
			if (!is_absolute_path(link_target))
				pg_fatal("target of symbolic link \"%s\" is relative", tblspcdir);

			/* Canonicalize the link target. */
			canonicalize_path(link_target);

			/*
			 * Find the corresponding tablespace mapping and copy the relevant
			 * details into the new tablespace entry.
			 */
			for (tsmap = opt->tsmappings; tsmap != NULL; tsmap = tsmap->next)
			{
				if (strcmp(tsmap->old_dir, link_target) == 0)
				{
					strlcpy(ts->old_dir, tsmap->old_dir, MAXPGPATH);
					strlcpy(ts->new_dir, tsmap->new_dir, MAXPGPATH);
					ts->in_place = false;
					break;
				}
			}

			/* Every non-in-place tablespace must be mapped. */
			if (tsmap == NULL)
				pg_fatal("tablespace at \"%s\" has no tablespace mapping",
						 link_target);
		}
		else
		{
			/*
			 * For an in-place tablespace, there's no separate directory, so
			 * we just record the paths within the data directories.
			 */
			snprintf(ts->old_dir, MAXPGPATH, "%s/%s", pg_tblspc, de->d_name);
			snprintf(ts->new_dir, MAXPGPATH, "%s/pg_tblspc/%s", opt->output,
					 de->d_name);
			ts->in_place = true;
		}

		/* Tablespaces should not share a directory. */
		for (otherts = tslist; otherts != NULL; otherts = otherts->next)
			if (strcmp(ts->new_dir, otherts->new_dir) == 0)
				pg_fatal("tablespaces with OIDs %u and %u both point at directory \"%s\"",
						 otherts->oid, oid, ts->new_dir);

		/* Add this tablespace to the list. */
		ts->next = tslist;
		tslist = ts;
	}

	if (closedir(dir) != 0)
		pg_fatal("could not close directory \"%s\": %m", pg_tblspc);

	return tslist;
}

/*
 * Read a file into a StringInfo.
 *
 * fd is used for the actual file I/O, filename for error reporting purposes.
 * A file longer than maxlen is a fatal error.
 */
static void
slurp_file(int fd, char *filename, StringInfo buf, int maxlen)
{
	struct stat st;
	ssize_t		rb;

	/* Check file size, and complain if it's too large. */
	if (fstat(fd, &st) != 0)
		pg_fatal("could not stat file \"%s\": %m", filename);
	if (st.st_size > maxlen)
		pg_fatal("file \"%s\" is too large", filename);

	/* Make sure we have enough space. */
	enlargeStringInfo(buf, st.st_size);

	/* Read the data. */
	rb = read(fd, &buf->data[buf->len], st.st_size);

	/*
	 * We don't expect any concurrent changes, so we should read exactly the
	 * expected number of bytes.
	 */
	if (rb != st.st_size)
	{
		if (rb < 0)
			pg_fatal("could not read file \"%s\": %m", filename);
		else
			pg_fatal("could not read file \"%s\": read %zd of %lld",
					 filename, rb, (long long int) st.st_size);
	}

	/* Adjust buffer length for new data and restore trailing-\0 invariant */
	buf->len += rb;
	buf->data[buf->len] = '\0';
}
