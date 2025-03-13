/*-------------------------------------------------------------------------
 *
 * Load data from a backup manifest into memory.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_combinebackup/load_manifest.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <unistd.h>

#include "common/hashfn_unstable.h"
#include "common/logging.h"
#include "common/parse_manifest.h"
#include "load_manifest.h"

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
 * size of json chunk to be read in
 *
 */
#define READ_CHUNK_SIZE (128  * 1024)

/*
 * Define a hash table which we can use to store information about the files
 * mentioned in the backup manifest.
 */
#define SH_PREFIX		manifest_files
#define SH_ELEMENT_TYPE	manifest_file
#define SH_KEY_TYPE		const char *
#define	SH_KEY			pathname
#define SH_HASH_KEY(tb, key)	hash_string(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define	SH_SCOPE		extern
#define SH_RAW_ALLOCATOR	pg_malloc0
#define SH_DEFINE
#include "lib/simplehash.h"

static void combinebackup_version_cb(JsonManifestParseContext *context,
									 int manifest_version);
static void combinebackup_system_identifier_cb(JsonManifestParseContext *context,
											   uint64 manifest_system_identifier);
static void combinebackup_per_file_cb(JsonManifestParseContext *context,
									  const char *pathname, uint64 size,
									  pg_checksum_type checksum_type,
									  int checksum_length,
									  uint8 *checksum_payload);
static void combinebackup_per_wal_range_cb(JsonManifestParseContext *context,
										   TimeLineID tli,
										   XLogRecPtr start_lsn,
										   XLogRecPtr end_lsn);
pg_noreturn static void report_manifest_error(JsonManifestParseContext *context,
											  const char *fmt,...)
			pg_attribute_printf(2, 3);

/*
 * Load backup_manifest files from an array of backups and produces an array
 * of manifest_data objects.
 *
 * NB: Since load_backup_manifest() can return NULL, the resulting array could
 * contain NULL entries.
 */
manifest_data **
load_backup_manifests(int n_backups, char **backup_directories)
{
	manifest_data **result;
	int			i;

	result = pg_malloc(sizeof(manifest_data *) * n_backups);
	for (i = 0; i < n_backups; ++i)
		result[i] = load_backup_manifest(backup_directories[i]);

	return result;
}

/*
 * Parse the backup_manifest file in the named backup directory. Construct a
 * hash table with information about all the files it mentions, and a linked
 * list of all the WAL ranges it mentions.
 *
 * If the backup_manifest file simply doesn't exist, logs a warning and returns
 * NULL. Any other error, or any error parsing the contents of the file, is
 * fatal.
 */
manifest_data *
load_backup_manifest(char *backup_directory)
{
	char		pathname[MAXPGPATH];
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
	snprintf(pathname, MAXPGPATH, "%s/backup_manifest", backup_directory);
	if ((fd = open(pathname, O_RDONLY | PG_BINARY, 0)) < 0)
	{
		if (errno == ENOENT)
		{
			pg_log_warning("file \"%s\" does not exist", pathname);
			return NULL;
		}
		pg_fatal("could not open file \"%s\": %m", pathname);
	}

	/* Figure out how big the manifest is. */
	if (fstat(fd, &statbuf) != 0)
		pg_fatal("could not stat file \"%s\": %m", pathname);

	/* Guess how large to make the hash table based on the manifest size. */
	estimate = statbuf.st_size / ESTIMATED_BYTES_PER_MANIFEST_LINE;
	initial_size = Min(PG_UINT32_MAX, Max(estimate, 256));

	/* Create the hash table. */
	ht = manifest_files_create(initial_size, NULL);

	result = pg_malloc0(sizeof(manifest_data));
	result->files = ht;
	context.private_data = result;
	context.version_cb = combinebackup_version_cb;
	context.system_identifier_cb = combinebackup_system_identifier_cb;
	context.per_file_cb = combinebackup_per_file_cb;
	context.per_wal_range_cb = combinebackup_per_wal_range_cb;
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
				pg_fatal("could not read file \"%s\": %m", pathname);
			else
				pg_fatal("could not read file \"%s\": read %d of %lld",
						 pathname, rc, (long long int) statbuf.st_size);
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
					pg_fatal("could not read file \"%s\": %m", pathname);
				else
					pg_fatal("could not read file \"%s\": read %lld of %lld",
							 pathname,
							 (long long int) (statbuf.st_size + rc - bytes_left),
							 (long long int) statbuf.st_size);
			}
			bytes_left -= rc;
			json_parse_manifest_incremental_chunk(inc_state, buffer, rc, bytes_left == 0);
		}

		/* Release the incremental state memory */
		json_parse_manifest_incremental_shutdown(inc_state);

		close(fd);
	}

	/* All done. */
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
 * This callback to validate the manifest version number for incremental backup.
 */
static void
combinebackup_version_cb(JsonManifestParseContext *context,
						 int manifest_version)
{
	/* Incremental backups supported on manifest version 2 or later */
	if (manifest_version == 1)
		pg_fatal("backup manifest version 1 does not support incremental backup");
}

/*
 * Record system identifier extracted from the backup manifest.
 */
static void
combinebackup_system_identifier_cb(JsonManifestParseContext *context,
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
combinebackup_per_file_cb(JsonManifestParseContext *context,
						  const char *pathname, uint64 size,
						  pg_checksum_type checksum_type,
						  int checksum_length, uint8 *checksum_payload)
{
	manifest_data *manifest = context->private_data;
	manifest_file *m;
	bool		found;

	/* Make a new entry in the hash table for this file. */
	m = manifest_files_insert(manifest->files, pathname, &found);
	if (found)
		pg_fatal("duplicate path name in backup manifest: \"%s\"", pathname);

	/* Initialize the entry. */
	m->size = size;
	m->checksum_type = checksum_type;
	m->checksum_length = checksum_length;
	m->checksum_payload = checksum_payload;
}

/*
 * Record details extracted from the backup manifest for one WAL range.
 */
static void
combinebackup_per_wal_range_cb(JsonManifestParseContext *context,
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
