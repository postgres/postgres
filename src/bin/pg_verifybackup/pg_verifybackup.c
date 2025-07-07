/*-------------------------------------------------------------------------
 *
 * pg_verifybackup.c
 *	  Verify a backup against a backup manifest.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_verifybackup/pg_verifybackup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>

#include "access/xlog_internal.h"
#include "common/logging.h"
#include "common/parse_manifest.h"
#include "fe_utils/simple_list.h"
#include "getopt_long.h"
#include "pg_verifybackup.h"
#include "pgtime.h"

/*
 * For efficiency, we'd like our hash table containing information about the
 * manifest to start out with approximately the correct number of entries.
 * There's no way to know the exact number of entries without reading the whole
 * file, but we can get an estimate by dividing the file size by the estimated
 * number of bytes per line.
 *
 * This could be off by about a factor of two in either direction, because the
 * checksum algorithm has a big impact on the line lengths; e.g. a SHA512
 * checksum is 128 hex bytes, whereas a CRC-32C value is only 8, and there
 * might be no checksum at all.
 */
#define ESTIMATED_BYTES_PER_MANIFEST_LINE	100

/*
 * How many bytes should we try to read from a file at once?
 */
#define READ_CHUNK_SIZE				(128 * 1024)

/*
 * Tar file information needed for content verification.
 */
typedef struct tar_file
{
	char	   *relpath;
	Oid			tblspc_oid;
	pg_compress_algorithm compress_algorithm;
} tar_file;

static manifest_data *parse_manifest_file(char *manifest_path);
static void verifybackup_version_cb(JsonManifestParseContext *context,
									int manifest_version);
static void verifybackup_system_identifier(JsonManifestParseContext *context,
										   uint64 manifest_system_identifier);
static void verifybackup_per_file_cb(JsonManifestParseContext *context,
									 const char *pathname, uint64 size,
									 pg_checksum_type checksum_type,
									 int checksum_length,
									 uint8 *checksum_payload);
static void verifybackup_per_wal_range_cb(JsonManifestParseContext *context,
										  TimeLineID tli,
										  XLogRecPtr start_lsn,
										  XLogRecPtr end_lsn);
pg_noreturn static void report_manifest_error(JsonManifestParseContext *context,
											  const char *fmt,...)
			pg_attribute_printf(2, 3);

static void verify_tar_backup(verifier_context *context, DIR *dir);
static void verify_plain_backup_directory(verifier_context *context,
										  char *relpath, char *fullpath,
										  DIR *dir);
static void verify_plain_backup_file(verifier_context *context, char *relpath,
									 char *fullpath);
static void verify_control_file(const char *controlpath,
								uint64 manifest_system_identifier);
static void precheck_tar_backup_file(verifier_context *context, char *relpath,
									 char *fullpath, SimplePtrList *tarfiles);
static void verify_tar_file(verifier_context *context, char *relpath,
							char *fullpath, astreamer *streamer);
static void report_extra_backup_files(verifier_context *context);
static void verify_backup_checksums(verifier_context *context);
static void verify_file_checksum(verifier_context *context,
								 manifest_file *m, char *fullpath,
								 uint8 *buffer);
static void parse_required_wal(verifier_context *context,
							   char *pg_waldump_path,
							   char *wal_directory);
static astreamer *create_archive_verifier(verifier_context *context,
										  char *archive_name,
										  Oid tblspc_oid,
										  pg_compress_algorithm compress_algo);

static void progress_report(bool finished);
static void usage(void);

static const char *progname;

/* is progress reporting enabled? */
static bool show_progress = false;

/* Progress indicators */
static uint64 total_size = 0;
static uint64 done_size = 0;

/*
 * Main entry point.
 */
int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"exit-on-error", no_argument, NULL, 'e'},
		{"ignore", required_argument, NULL, 'i'},
		{"manifest-path", required_argument, NULL, 'm'},
		{"format", required_argument, NULL, 'F'},
		{"no-parse-wal", no_argument, NULL, 'n'},
		{"progress", no_argument, NULL, 'P'},
		{"quiet", no_argument, NULL, 'q'},
		{"skip-checksums", no_argument, NULL, 's'},
		{"wal-directory", required_argument, NULL, 'w'},
		{NULL, 0, NULL, 0}
	};

	int			c;
	verifier_context context;
	char	   *manifest_path = NULL;
	bool		no_parse_wal = false;
	bool		quiet = false;
	char	   *wal_directory = NULL;
	char	   *pg_waldump_path = NULL;
	DIR		   *dir;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_verifybackup"));
	progname = get_progname(argv[0]);

	memset(&context, 0, sizeof(context));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_verifybackup (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/*
	 * Skip certain files in the toplevel directory.
	 *
	 * Ignore the backup_manifest file, because it's not included in the
	 * backup manifest.
	 *
	 * Ignore the pg_wal directory, because those files are not included in
	 * the backup manifest either, since they are fetched separately from the
	 * backup itself, and verified via a separate mechanism.
	 *
	 * Ignore postgresql.auto.conf, recovery.signal, and standby.signal,
	 * because we expect that those files may sometimes be created or changed
	 * as part of the backup process. For example, pg_basebackup -R will
	 * modify postgresql.auto.conf and create standby.signal.
	 */
	simple_string_list_append(&context.ignore_list, "backup_manifest");
	simple_string_list_append(&context.ignore_list, "pg_wal");
	simple_string_list_append(&context.ignore_list, "postgresql.auto.conf");
	simple_string_list_append(&context.ignore_list, "recovery.signal");
	simple_string_list_append(&context.ignore_list, "standby.signal");

	while ((c = getopt_long(argc, argv, "eF:i:m:nPqsw:", long_options, NULL)) != -1)
	{
		switch (c)
		{
			case 'e':
				context.exit_on_error = true;
				break;
			case 'i':
				{
					char	   *arg = pstrdup(optarg);

					canonicalize_path(arg);
					simple_string_list_append(&context.ignore_list, arg);
					break;
				}
			case 'm':
				manifest_path = pstrdup(optarg);
				canonicalize_path(manifest_path);
				break;
			case 'F':
				if (strcmp(optarg, "p") == 0 || strcmp(optarg, "plain") == 0)
					context.format = 'p';
				else if (strcmp(optarg, "t") == 0 || strcmp(optarg, "tar") == 0)
					context.format = 't';
				else
					pg_fatal("invalid backup format \"%s\", must be \"plain\" or \"tar\"",
							 optarg);
				break;
			case 'n':
				no_parse_wal = true;
				break;
			case 'P':
				show_progress = true;
				break;
			case 'q':
				quiet = true;
				break;
			case 's':
				context.skip_checksums = true;
				break;
			case 'w':
				wal_directory = pstrdup(optarg);
				canonicalize_path(wal_directory);
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	/* Get backup directory name */
	if (optind >= argc)
	{
		pg_log_error("no backup directory specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	context.backup_directory = pstrdup(argv[optind++]);
	canonicalize_path(context.backup_directory);

	/* Complain if any arguments remain */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* Complain if the specified arguments conflict */
	if (show_progress && quiet)
		pg_fatal("cannot specify both %s and %s",
				 "-P/--progress", "-q/--quiet");

	/* Unless --no-parse-wal was specified, we will need pg_waldump. */
	if (!no_parse_wal)
	{
		int			ret;

		pg_waldump_path = pg_malloc(MAXPGPATH);
		ret = find_other_exec(argv[0], "pg_waldump",
							  "pg_waldump (PostgreSQL) " PG_VERSION "\n",
							  pg_waldump_path);
		if (ret < 0)
		{
			char		full_path[MAXPGPATH];

			if (find_my_exec(argv[0], full_path) < 0)
				strlcpy(full_path, progname, sizeof(full_path));

			if (ret == -1)
				pg_fatal("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"",
						 "pg_waldump", "pg_verifybackup", full_path);
			else
				pg_fatal("program \"%s\" was found by \"%s\" but was not the same version as %s",
						 "pg_waldump", full_path, "pg_verifybackup");
		}
	}

	/* By default, look for the manifest in the backup directory. */
	if (manifest_path == NULL)
		manifest_path = psprintf("%s/backup_manifest",
								 context.backup_directory);

	/* By default, look for the WAL in the backup directory, too. */
	if (wal_directory == NULL)
		wal_directory = psprintf("%s/pg_wal", context.backup_directory);

	/*
	 * Try to read the manifest. We treat any errors encountered while parsing
	 * the manifest as fatal; there doesn't seem to be much point in trying to
	 * verify the backup directory against a corrupted manifest.
	 */
	context.manifest = parse_manifest_file(manifest_path);

	/*
	 * If the backup directory cannot be found, treat this as a fatal error.
	 */
	dir = opendir(context.backup_directory);
	if (dir == NULL)
		report_fatal_error("could not open directory \"%s\": %m",
						   context.backup_directory);

	/*
	 * At this point, we know that the backup directory exists, so it's now
	 * reasonable to check for files immediately inside it. Thus, before going
	 * further, if the user did not specify the backup format, check for
	 * PG_VERSION to distinguish between tar and plain format.
	 */
	if (context.format == '\0')
	{
		struct stat sb;
		char	   *path;

		path = psprintf("%s/%s", context.backup_directory, "PG_VERSION");
		if (stat(path, &sb) == 0)
			context.format = 'p';
		else if (errno != ENOENT)
		{
			pg_log_error("could not stat file \"%s\": %m", path);
			exit(1);
		}
		else
		{
			/* No PG_VERSION, so assume tar format. */
			context.format = 't';
		}
		pfree(path);
	}

	/*
	 * XXX: In the future, we should consider enhancing pg_waldump to read WAL
	 * files from an archive.
	 */
	if (!no_parse_wal && context.format == 't')
	{
		pg_log_error("pg_waldump cannot read tar files");
		pg_log_error_hint("You must use -n/--no-parse-wal when verifying a tar-format backup.");
		exit(1);
	}

	/*
	 * Perform the appropriate type of verification appropriate based on the
	 * backup format. This will close 'dir'.
	 */
	if (context.format == 'p')
		verify_plain_backup_directory(&context, NULL, context.backup_directory,
									  dir);
	else
		verify_tar_backup(&context, dir);

	/*
	 * The "matched" flag should now be set on every entry in the hash table.
	 * Any entries for which the bit is not set are files mentioned in the
	 * manifest that don't exist on disk (or in the relevant tar files).
	 */
	report_extra_backup_files(&context);

	/*
	 * If this is a tar-format backup, checksums were already verified above;
	 * but if it's a plain-format backup, we postpone it until this point,
	 * since the earlier checks can be performed just by knowing which files
	 * are present, without needing to read all of them.
	 */
	if (context.format == 'p' && !context.skip_checksums)
		verify_backup_checksums(&context);

	/*
	 * Try to parse the required ranges of WAL records, unless we were told
	 * not to do so.
	 */
	if (!no_parse_wal)
		parse_required_wal(&context, pg_waldump_path, wal_directory);

	/*
	 * If everything looks OK, tell the user this, unless we were asked to
	 * work quietly.
	 */
	if (!context.saw_any_error && !quiet)
		printf(_("backup successfully verified\n"));

	return context.saw_any_error ? 1 : 0;
}

/*
 * Parse a manifest file and return a data structure describing the contents.
 */
static manifest_data *
parse_manifest_file(char *manifest_path)
{
	int			fd;
	struct stat statbuf;
	off_t		estimate;
	uint32		initial_size;
	manifest_files_hash *ht;
	char	   *buffer;
	int			rc;
	JsonManifestParseContext context;
	manifest_data *result;

	int			chunk_size = READ_CHUNK_SIZE;

	/* Open the manifest file. */
	if ((fd = open(manifest_path, O_RDONLY | PG_BINARY, 0)) < 0)
		report_fatal_error("could not open file \"%s\": %m", manifest_path);

	/* Figure out how big the manifest is. */
	if (fstat(fd, &statbuf) != 0)
		report_fatal_error("could not stat file \"%s\": %m", manifest_path);

	/* Guess how large to make the hash table based on the manifest size. */
	estimate = statbuf.st_size / ESTIMATED_BYTES_PER_MANIFEST_LINE;
	initial_size = Min(PG_UINT32_MAX, Max(estimate, 256));

	/* Create the hash table. */
	ht = manifest_files_create(initial_size, NULL);

	result = pg_malloc0(sizeof(manifest_data));
	result->files = ht;
	context.private_data = result;
	context.version_cb = verifybackup_version_cb;
	context.system_identifier_cb = verifybackup_system_identifier;
	context.per_file_cb = verifybackup_per_file_cb;
	context.per_wal_range_cb = verifybackup_per_wal_range_cb;
	context.error_cb = report_manifest_error;

	/*
	 * Parse the file, in chunks if necessary.
	 */
	if (statbuf.st_size <= chunk_size)
	{
		buffer = pg_malloc(statbuf.st_size);
		rc = read(fd, buffer, statbuf.st_size);
		if (rc != statbuf.st_size)
		{
			if (rc < 0)
				pg_fatal("could not read file \"%s\": %m", manifest_path);
			else
				pg_fatal("could not read file \"%s\": read %d of %lld",
						 manifest_path, rc, (long long int) statbuf.st_size);
		}

		/* Close the manifest file. */
		close(fd);

		/* Parse the manifest. */
		json_parse_manifest(&context, buffer, statbuf.st_size);
	}
	else
	{
		int			bytes_left = statbuf.st_size;
		JsonManifestParseIncrementalState *inc_state;

		inc_state = json_parse_manifest_incremental_init(&context);

		buffer = pg_malloc(chunk_size + 1);

		while (bytes_left > 0)
		{
			int			bytes_to_read = chunk_size;

			/*
			 * Make sure that the last chunk is sufficiently large. (i.e. at
			 * least half the chunk size) so that it will contain fully the
			 * piece at the end with the checksum.
			 */
			if (bytes_left < chunk_size)
				bytes_to_read = bytes_left;
			else if (bytes_left < 2 * chunk_size)
				bytes_to_read = bytes_left / 2;
			rc = read(fd, buffer, bytes_to_read);
			if (rc != bytes_to_read)
			{
				if (rc < 0)
					pg_fatal("could not read file \"%s\": %m", manifest_path);
				else
					pg_fatal("could not read file \"%s\": read %lld of %lld",
							 manifest_path,
							 (long long int) (statbuf.st_size + rc - bytes_left),
							 (long long int) statbuf.st_size);
			}
			bytes_left -= rc;
			json_parse_manifest_incremental_chunk(inc_state, buffer, rc,
												  bytes_left == 0);
		}

		/* Release the incremental state memory */
		json_parse_manifest_incremental_shutdown(inc_state);

		close(fd);
	}

	/* Done with the buffer. */
	pfree(buffer);

	return result;
}

/*
 * Report an error while parsing the manifest.
 *
 * We consider all such errors to be fatal errors. The manifest parser
 * expects this function not to return.
 */
static void
report_manifest_error(JsonManifestParseContext *context, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	pg_log_generic_v(PG_LOG_ERROR, PG_LOG_PRIMARY, gettext(fmt), ap);
	va_end(ap);

	exit(1);
}

/*
 * Record details extracted from the backup manifest.
 */
static void
verifybackup_version_cb(JsonManifestParseContext *context,
						int manifest_version)
{
	manifest_data *manifest = context->private_data;

	/* Validation will be at the later stage */
	manifest->version = manifest_version;
}

/*
 * Record details extracted from the backup manifest.
 */
static void
verifybackup_system_identifier(JsonManifestParseContext *context,
							   uint64 manifest_system_identifier)
{
	manifest_data *manifest = context->private_data;

	/* Validation will be at the later stage */
	manifest->system_identifier = manifest_system_identifier;
}

/*
 * Record details extracted from the backup manifest for one file.
 */
static void
verifybackup_per_file_cb(JsonManifestParseContext *context,
						 const char *pathname, uint64 size,
						 pg_checksum_type checksum_type,
						 int checksum_length, uint8 *checksum_payload)
{
	manifest_data *manifest = context->private_data;
	manifest_files_hash *ht = manifest->files;
	manifest_file *m;
	bool		found;

	/* Make a new entry in the hash table for this file. */
	m = manifest_files_insert(ht, pathname, &found);
	if (found)
		report_fatal_error("duplicate path name in backup manifest: \"%s\"",
						   pathname);

	/* Initialize the entry. */
	m->size = size;
	m->checksum_type = checksum_type;
	m->checksum_length = checksum_length;
	m->checksum_payload = checksum_payload;
	m->matched = false;
	m->bad = false;
}

/*
 * Record details extracted from the backup manifest for one WAL range.
 */
static void
verifybackup_per_wal_range_cb(JsonManifestParseContext *context,
							  TimeLineID tli,
							  XLogRecPtr start_lsn, XLogRecPtr end_lsn)
{
	manifest_data *manifest = context->private_data;
	manifest_wal_range *range;

	/* Allocate and initialize a struct describing this WAL range. */
	range = palloc(sizeof(manifest_wal_range));
	range->tli = tli;
	range->start_lsn = start_lsn;
	range->end_lsn = end_lsn;
	range->prev = manifest->last_wal_range;
	range->next = NULL;

	/* Add it to the end of the list. */
	if (manifest->first_wal_range == NULL)
		manifest->first_wal_range = range;
	else
		manifest->last_wal_range->next = range;
	manifest->last_wal_range = range;
}

/*
 * Verify one directory of a plain-format backup.
 *
 * 'relpath' is NULL if we are to verify the top-level backup directory,
 * and otherwise the relative path to the directory that is to be verified.
 *
 * 'fullpath' is the backup directory with 'relpath' appended; i.e. the actual
 * filesystem path at which it can be found.
 *
 * 'dir' is an open directory handle, or NULL if the caller wants us to
 * open it. If the caller chooses to pass a handle, we'll close it when
 * we're done with it.
 */
static void
verify_plain_backup_directory(verifier_context *context, char *relpath,
							  char *fullpath, DIR *dir)
{
	struct dirent *dirent;

	/* Open the directory unless the caller did it. */
	if (dir == NULL && ((dir = opendir(fullpath)) == NULL))
	{
		report_backup_error(context,
							"could not open directory \"%s\": %m", fullpath);
		simple_string_list_append(&context->ignore_list, relpath);

		return;
	}

	while (errno = 0, (dirent = readdir(dir)) != NULL)
	{
		char	   *filename = dirent->d_name;
		char	   *newfullpath = psprintf("%s/%s", fullpath, filename);
		char	   *newrelpath;

		/* Skip "." and ".." */
		if (filename[0] == '.' && (filename[1] == '\0'
								   || strcmp(filename, "..") == 0))
			continue;

		if (relpath == NULL)
			newrelpath = pstrdup(filename);
		else
			newrelpath = psprintf("%s/%s", relpath, filename);

		if (!should_ignore_relpath(context, newrelpath))
			verify_plain_backup_file(context, newrelpath, newfullpath);

		pfree(newfullpath);
		pfree(newrelpath);
	}

	if (closedir(dir))
	{
		report_backup_error(context,
							"could not close directory \"%s\": %m", fullpath);
		return;
	}
}

/*
 * Verify one file (which might actually be a directory or a symlink).
 *
 * The arguments to this function have the same meaning as the similarly named
 * arguments to verify_plain_backup_directory.
 */
static void
verify_plain_backup_file(verifier_context *context, char *relpath,
						 char *fullpath)
{
	struct stat sb;
	manifest_file *m;

	if (stat(fullpath, &sb) != 0)
	{
		report_backup_error(context,
							"could not stat file or directory \"%s\": %m",
							relpath);

		/*
		 * Suppress further errors related to this path name and, if it's a
		 * directory, anything underneath it.
		 */
		simple_string_list_append(&context->ignore_list, relpath);

		return;
	}

	/* If it's a directory, just recurse. */
	if (S_ISDIR(sb.st_mode))
	{
		verify_plain_backup_directory(context, relpath, fullpath, NULL);
		return;
	}

	/* If it's not a directory, it should be a regular file. */
	if (!S_ISREG(sb.st_mode))
	{
		report_backup_error(context,
							"\"%s\" is not a regular file or directory",
							relpath);
		return;
	}

	/* Check whether there's an entry in the manifest hash. */
	m = manifest_files_lookup(context->manifest->files, relpath);
	if (m == NULL)
	{
		report_backup_error(context,
							"\"%s\" is present on disk but not in the manifest",
							relpath);
		return;
	}

	/* Flag this entry as having been encountered in the filesystem. */
	m->matched = true;

	/* Check that the size matches. */
	if (m->size != sb.st_size)
	{
		report_backup_error(context,
							"\"%s\" has size %llu on disk but size %llu in the manifest",
							relpath, (unsigned long long) sb.st_size,
							(unsigned long long) m->size);
		m->bad = true;
	}

	/*
	 * Validate the manifest system identifier, not available in manifest
	 * version 1.
	 */
	if (context->manifest->version != 1 &&
		strcmp(relpath, XLOG_CONTROL_FILE) == 0)
		verify_control_file(fullpath, context->manifest->system_identifier);

	/* Update statistics for progress report, if necessary */
	if (show_progress && !context->skip_checksums &&
		should_verify_checksum(m))
		total_size += m->size;

	/*
	 * We don't verify checksums at this stage. We first finish verifying that
	 * we have the expected set of files with the expected sizes, and only
	 * afterwards verify the checksums. That's because computing checksums may
	 * take a while, and we'd like to report more obvious problems quickly.
	 */
}

/*
 * Sanity check control file and validate system identifier against manifest
 * system identifier.
 */
static void
verify_control_file(const char *controlpath, uint64 manifest_system_identifier)
{
	ControlFileData *control_file;
	bool		crc_ok;

	pg_log_debug("reading \"%s\"", controlpath);
	control_file = get_controlfile_by_exact_path(controlpath, &crc_ok);

	/* Control file contents not meaningful if CRC is bad. */
	if (!crc_ok)
		report_fatal_error("%s: CRC is incorrect", controlpath);

	/* Can't interpret control file if not current version. */
	if (control_file->pg_control_version != PG_CONTROL_VERSION)
		report_fatal_error("%s: unexpected control file version",
						   controlpath);

	/* System identifiers should match. */
	if (manifest_system_identifier != control_file->system_identifier)
		report_fatal_error("%s: manifest system identifier is %" PRIu64 ", but control file has %" PRIu64,
						   controlpath,
						   manifest_system_identifier,
						   control_file->system_identifier);

	/* Release memory. */
	pfree(control_file);
}

/*
 * Verify tar backup.
 *
 * The caller should pass a handle to the target directory, which we will
 * close when we're done with it.
 */
static void
verify_tar_backup(verifier_context *context, DIR *dir)
{
	struct dirent *dirent;
	SimplePtrList tarfiles = {NULL, NULL};
	SimplePtrListCell *cell;

	Assert(context->format != 'p');

	progress_report(false);

	/* First pass: scan the directory for tar files. */
	while (errno = 0, (dirent = readdir(dir)) != NULL)
	{
		char	   *filename = dirent->d_name;

		/* Skip "." and ".." */
		if (filename[0] == '.' && (filename[1] == '\0'
								   || strcmp(filename, "..") == 0))
			continue;

		/*
		 * Unless it's something we should ignore, perform prechecks and add
		 * it to the list.
		 */
		if (!should_ignore_relpath(context, filename))
		{
			char	   *fullpath;

			fullpath = psprintf("%s/%s", context->backup_directory, filename);
			precheck_tar_backup_file(context, filename, fullpath, &tarfiles);
			pfree(fullpath);
		}
	}

	if (closedir(dir))
	{
		report_backup_error(context,
							"could not close directory \"%s\": %m",
							context->backup_directory);
		return;
	}

	/* Second pass: Perform the final verification of the tar contents. */
	for (cell = tarfiles.head; cell != NULL; cell = cell->next)
	{
		tar_file   *tar = (tar_file *) cell->ptr;
		astreamer  *streamer;
		char	   *fullpath;

		/*
		 * Prepares the archive streamer stack according to the tar
		 * compression format.
		 */
		streamer = create_archive_verifier(context,
										   tar->relpath,
										   tar->tblspc_oid,
										   tar->compress_algorithm);

		/* Compute the full pathname to the target file. */
		fullpath = psprintf("%s/%s", context->backup_directory,
							tar->relpath);

		/* Invoke the streamer for reading, decompressing, and verifying. */
		verify_tar_file(context, tar->relpath, fullpath, streamer);

		/* Cleanup. */
		pfree(tar->relpath);
		pfree(tar);
		pfree(fullpath);

		astreamer_finalize(streamer);
		astreamer_free(streamer);
	}
	simple_ptr_list_destroy(&tarfiles);

	progress_report(true);
}

/*
 * Preparatory steps for verifying files in tar format backups.
 *
 * Carries out basic validation of the tar format backup file, detects the
 * compression type, and appends that information to the tarfiles list. An
 * error will be reported if the tar file is inaccessible, or if the file type,
 * name, or compression type is not as expected.
 *
 * The arguments to this function are mostly the same as the
 * verify_plain_backup_file. The additional argument outputs a list of valid
 * tar files.
 */
static void
precheck_tar_backup_file(verifier_context *context, char *relpath,
						 char *fullpath, SimplePtrList *tarfiles)
{
	struct stat sb;
	Oid			tblspc_oid = InvalidOid;
	pg_compress_algorithm compress_algorithm;
	tar_file   *tar;
	char	   *suffix = NULL;

	/* Should be tar format backup */
	Assert(context->format == 't');

	/* Get file information */
	if (stat(fullpath, &sb) != 0)
	{
		report_backup_error(context,
							"could not stat file or directory \"%s\": %m",
							relpath);
		return;
	}

	/* In a tar format backup, we expect only regular files. */
	if (!S_ISREG(sb.st_mode))
	{
		report_backup_error(context,
							"file \"%s\" is not a regular file",
							relpath);
		return;
	}

	/*
	 * We expect tar files for backing up the main directory, tablespace, and
	 * pg_wal directory.
	 *
	 * pg_basebackup writes the main data directory to an archive file named
	 * base.tar, the pg_wal directory to pg_wal.tar, and the tablespace
	 * directory to <tablespaceoid>.tar, each followed by a compression type
	 * extension such as .gz, .lz4, or .zst.
	 */
	if (strncmp("base", relpath, 4) == 0)
		suffix = relpath + 4;
	else if (strncmp("pg_wal", relpath, 6) == 0)
		suffix = relpath + 6;
	else
	{
		/* Expected a <tablespaceoid>.tar file here. */
		uint64		num = strtoul(relpath, &suffix, 10);

		/*
		 * Report an error if we didn't consume at least one character, if the
		 * result is 0, or if the value is too large to be a valid OID.
		 */
		if (suffix == NULL || num <= 0 || num > OID_MAX)
		{
			report_backup_error(context,
								"file \"%s\" is not expected in a tar format backup",
								relpath);
			return;
		}
		tblspc_oid = (Oid) num;
	}

	/* Now, check the compression type of the tar */
	if (strcmp(suffix, ".tar") == 0)
		compress_algorithm = PG_COMPRESSION_NONE;
	else if (strcmp(suffix, ".tgz") == 0)
		compress_algorithm = PG_COMPRESSION_GZIP;
	else if (strcmp(suffix, ".tar.gz") == 0)
		compress_algorithm = PG_COMPRESSION_GZIP;
	else if (strcmp(suffix, ".tar.lz4") == 0)
		compress_algorithm = PG_COMPRESSION_LZ4;
	else if (strcmp(suffix, ".tar.zst") == 0)
		compress_algorithm = PG_COMPRESSION_ZSTD;
	else
	{
		report_backup_error(context,
							"file \"%s\" is not expected in a tar format backup",
							relpath);
		return;
	}

	/*
	 * Ignore WALs, as reading and verification will be handled through
	 * pg_waldump.
	 */
	if (strncmp("pg_wal", relpath, 6) == 0)
		return;

	/*
	 * Append the information to the list for complete verification at a later
	 * stage.
	 */
	tar = pg_malloc(sizeof(tar_file));
	tar->relpath = pstrdup(relpath);
	tar->tblspc_oid = tblspc_oid;
	tar->compress_algorithm = compress_algorithm;

	simple_ptr_list_append(tarfiles, tar);

	/* Update statistics for progress report, if necessary */
	if (show_progress)
		total_size += sb.st_size;
}

/*
 * Verification of a single tar file content.
 *
 * It reads a given tar archive in predefined chunks and passes it to the
 * streamer, which initiates routines for decompression (if necessary) and then
 * verifies each member within the tar file.
 */
static void
verify_tar_file(verifier_context *context, char *relpath, char *fullpath,
				astreamer *streamer)
{
	int			fd;
	int			rc;
	char	   *buffer;

	pg_log_debug("reading \"%s\"", fullpath);

	/* Open the target file. */
	if ((fd = open(fullpath, O_RDONLY | PG_BINARY, 0)) < 0)
	{
		report_backup_error(context, "could not open file \"%s\": %m",
							relpath);
		return;
	}

	buffer = pg_malloc(READ_CHUNK_SIZE * sizeof(uint8));

	/* Perform the reads */
	while ((rc = read(fd, buffer, READ_CHUNK_SIZE)) > 0)
	{
		astreamer_content(streamer, NULL, buffer, rc, ASTREAMER_UNKNOWN);

		/* Report progress */
		done_size += rc;
		progress_report(false);
	}

	pg_free(buffer);

	if (rc < 0)
		report_backup_error(context, "could not read file \"%s\": %m",
							relpath);

	/* Close the file. */
	if (close(fd) != 0)
		report_backup_error(context, "could not close file \"%s\": %m",
							relpath);
}

/*
 * Scan the hash table for entries where the 'matched' flag is not set; report
 * that such files are present in the manifest but not on disk.
 */
static void
report_extra_backup_files(verifier_context *context)
{
	manifest_data *manifest = context->manifest;
	manifest_files_iterator it;
	manifest_file *m;

	manifest_files_start_iterate(manifest->files, &it);
	while ((m = manifest_files_iterate(manifest->files, &it)) != NULL)
		if (!m->matched && !should_ignore_relpath(context, m->pathname))
			report_backup_error(context,
								"\"%s\" is present in the manifest but not on disk",
								m->pathname);
}

/*
 * Verify checksums for hash table entries that are otherwise unproblematic.
 * If we've already reported some problem related to a hash table entry, or
 * if it has no checksum, just skip it.
 */
static void
verify_backup_checksums(verifier_context *context)
{
	manifest_data *manifest = context->manifest;
	manifest_files_iterator it;
	manifest_file *m;
	uint8	   *buffer;

	progress_report(false);

	buffer = pg_malloc(READ_CHUNK_SIZE * sizeof(uint8));

	manifest_files_start_iterate(manifest->files, &it);
	while ((m = manifest_files_iterate(manifest->files, &it)) != NULL)
	{
		if (should_verify_checksum(m) &&
			!should_ignore_relpath(context, m->pathname))
		{
			char	   *fullpath;

			/* Compute the full pathname to the target file. */
			fullpath = psprintf("%s/%s", context->backup_directory,
								m->pathname);

			/* Do the actual checksum verification. */
			verify_file_checksum(context, m, fullpath, buffer);

			/* Avoid leaking memory. */
			pfree(fullpath);
		}
	}

	pfree(buffer);

	progress_report(true);
}

/*
 * Verify the checksum of a single file.
 */
static void
verify_file_checksum(verifier_context *context, manifest_file *m,
					 char *fullpath, uint8 *buffer)
{
	pg_checksum_context checksum_ctx;
	const char *relpath = m->pathname;
	int			fd;
	int			rc;
	uint64		bytes_read = 0;
	uint8		checksumbuf[PG_CHECKSUM_MAX_LENGTH];
	int			checksumlen;

	/* Open the target file. */
	if ((fd = open(fullpath, O_RDONLY | PG_BINARY, 0)) < 0)
	{
		report_backup_error(context, "could not open file \"%s\": %m",
							relpath);
		return;
	}

	/* Initialize checksum context. */
	if (pg_checksum_init(&checksum_ctx, m->checksum_type) < 0)
	{
		report_backup_error(context, "could not initialize checksum of file \"%s\"",
							relpath);
		close(fd);
		return;
	}

	/* Read the file chunk by chunk, updating the checksum as we go. */
	while ((rc = read(fd, buffer, READ_CHUNK_SIZE)) > 0)
	{
		bytes_read += rc;
		if (pg_checksum_update(&checksum_ctx, buffer, rc) < 0)
		{
			report_backup_error(context, "could not update checksum of file \"%s\"",
								relpath);
			close(fd);
			return;
		}

		/* Report progress */
		done_size += rc;
		progress_report(false);
	}
	if (rc < 0)
		report_backup_error(context, "could not read file \"%s\": %m",
							relpath);

	/* Close the file. */
	if (close(fd) != 0)
	{
		report_backup_error(context, "could not close file \"%s\": %m",
							relpath);
		return;
	}

	/* If we didn't manage to read the whole file, bail out now. */
	if (rc < 0)
		return;

	/*
	 * Double-check that we read the expected number of bytes from the file.
	 * Normally, mismatches would be caught in verify_plain_backup_file and
	 * this check would never be reached, but this provides additional safety
	 * and clarity in the event of concurrent modifications or filesystem
	 * misbehavior.
	 */
	if (bytes_read != m->size)
	{
		report_backup_error(context,
							"file \"%s\" should contain %" PRIu64 " bytes, but read %" PRIu64,
							relpath, m->size, bytes_read);
		return;
	}

	/* Get the final checksum. */
	checksumlen = pg_checksum_final(&checksum_ctx, checksumbuf);
	if (checksumlen < 0)
	{
		report_backup_error(context,
							"could not finalize checksum of file \"%s\"",
							relpath);
		return;
	}

	/* And check it against the manifest. */
	if (checksumlen != m->checksum_length)
		report_backup_error(context,
							"file \"%s\" has checksum of length %d, but expected %d",
							relpath, m->checksum_length, checksumlen);
	else if (memcmp(checksumbuf, m->checksum_payload, checksumlen) != 0)
		report_backup_error(context,
							"checksum mismatch for file \"%s\"",
							relpath);
}

/*
 * Attempt to parse the WAL files required to restore from backup using
 * pg_waldump.
 */
static void
parse_required_wal(verifier_context *context, char *pg_waldump_path,
				   char *wal_directory)
{
	manifest_data *manifest = context->manifest;
	manifest_wal_range *this_wal_range = manifest->first_wal_range;

	while (this_wal_range != NULL)
	{
		char	   *pg_waldump_cmd;

		pg_waldump_cmd = psprintf("\"%s\" --quiet --path=\"%s\" --timeline=%u --start=%X/%08X --end=%X/%08X\n",
								  pg_waldump_path, wal_directory, this_wal_range->tli,
								  LSN_FORMAT_ARGS(this_wal_range->start_lsn),
								  LSN_FORMAT_ARGS(this_wal_range->end_lsn));
		fflush(NULL);
		if (system(pg_waldump_cmd) != 0)
			report_backup_error(context,
								"WAL parsing failed for timeline %u",
								this_wal_range->tli);

		this_wal_range = this_wal_range->next;
	}
}

/*
 * Report a problem with the backup.
 *
 * Update the context to indicate that we saw an error, and exit if the
 * context says we should.
 */
void
report_backup_error(verifier_context *context, const char *pg_restrict fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	pg_log_generic_v(PG_LOG_ERROR, PG_LOG_PRIMARY, gettext(fmt), ap);
	va_end(ap);

	context->saw_any_error = true;
	if (context->exit_on_error)
		exit(1);
}

/*
 * Report a fatal error and exit
 */
void
report_fatal_error(const char *pg_restrict fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	pg_log_generic_v(PG_LOG_ERROR, PG_LOG_PRIMARY, gettext(fmt), ap);
	va_end(ap);

	exit(1);
}

/*
 * Is the specified relative path, or some prefix of it, listed in the set
 * of paths to ignore?
 *
 * Note that by "prefix" we mean a parent directory; for this purpose,
 * "aa/bb" is not a prefix of "aa/bbb", but it is a prefix of "aa/bb/cc".
 */
bool
should_ignore_relpath(verifier_context *context, const char *relpath)
{
	SimpleStringListCell *cell;

	for (cell = context->ignore_list.head; cell != NULL; cell = cell->next)
	{
		const char *r = relpath;
		char	   *v = cell->val;

		while (*v != '\0' && *r == *v)
			++r, ++v;

		if (*v == '\0' && (*r == '\0' || *r == '/'))
			return true;
	}

	return false;
}

/*
 * Create a chain of archive streamers appropriate for verifying a given
 * archive.
 */
static astreamer *
create_archive_verifier(verifier_context *context, char *archive_name,
						Oid tblspc_oid, pg_compress_algorithm compress_algo)
{
	astreamer  *streamer = NULL;

	/* Should be here only for tar backup */
	Assert(context->format == 't');

	/* Last step is the actual verification. */
	streamer = astreamer_verify_content_new(streamer, context, archive_name,
											tblspc_oid);

	/* Before that we must parse the tar file. */
	streamer = astreamer_tar_parser_new(streamer);

	/* Before that we must decompress, if archive is compressed. */
	if (compress_algo == PG_COMPRESSION_GZIP)
		streamer = astreamer_gzip_decompressor_new(streamer);
	else if (compress_algo == PG_COMPRESSION_LZ4)
		streamer = astreamer_lz4_decompressor_new(streamer);
	else if (compress_algo == PG_COMPRESSION_ZSTD)
		streamer = astreamer_zstd_decompressor_new(streamer);

	return streamer;
}

/*
 * Print a progress report based on the global variables.
 *
 * Progress report is written at maximum once per second, unless the finished
 * parameter is set to true.
 *
 * If finished is set to true, this is the last progress report. The cursor
 * is moved to the next line.
 */
static void
progress_report(bool finished)
{
	static pg_time_t last_progress_report = 0;
	pg_time_t	now;
	int			percent_size = 0;
	char		totalsize_str[32];
	char		donesize_str[32];

	if (!show_progress)
		return;

	now = time(NULL);
	if (now == last_progress_report && !finished)
		return;					/* Max once per second */

	last_progress_report = now;
	percent_size = total_size ? (int) ((done_size * 100 / total_size)) : 0;

	snprintf(totalsize_str, sizeof(totalsize_str), UINT64_FORMAT,
			 total_size / 1024);
	snprintf(donesize_str, sizeof(donesize_str), UINT64_FORMAT,
			 done_size / 1024);

	fprintf(stderr,
			_("%*s/%s kB (%d%%) verified"),
			(int) strlen(totalsize_str),
			donesize_str, totalsize_str, percent_size);

	/*
	 * Stay on the same line if reporting to a terminal and we're not done
	 * yet.
	 */
	fputc((!finished && isatty(fileno(stderr))) ? '\r' : '\n', stderr);
}

/*
 * Print out usage information and exit.
 */
static void
usage(void)
{
	printf(_("%s verifies a backup against the backup manifest.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]... BACKUPDIR\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -e, --exit-on-error         exit immediately on error\n"));
	printf(_("  -F, --format=p|t            backup format (plain, tar)\n"));
	printf(_("  -i, --ignore=RELATIVE_PATH  ignore indicated path\n"));
	printf(_("  -m, --manifest-path=PATH    use specified path for manifest\n"));
	printf(_("  -n, --no-parse-wal          do not try to parse WAL files\n"));
	printf(_("  -P, --progress              show progress information\n"));
	printf(_("  -q, --quiet                 do not print any output, except for errors\n"));
	printf(_("  -s, --skip-checksums        skip checksum verification\n"));
	printf(_("  -w, --wal-directory=PATH    use specified path for WAL files\n"));
	printf(_("  -V, --version               output version information, then exit\n"));
	printf(_("  -?, --help                  show this help, then exit\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
