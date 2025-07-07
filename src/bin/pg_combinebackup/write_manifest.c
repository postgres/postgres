/*-------------------------------------------------------------------------
 *
 * Write a new backup manifest.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_combinebackup/write_manifest.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "common/checksum_helper.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "lib/stringinfo.h"
#include "load_manifest.h"
#include "mb/pg_wchar.h"
#include "write_manifest.h"

struct manifest_writer
{
	char		pathname[MAXPGPATH];
	int			fd;
	StringInfoData buf;
	bool		first_file;
	bool		still_checksumming;
	pg_checksum_context manifest_ctx;
};

static void escape_json(StringInfo buf, const char *str);
static void flush_manifest(manifest_writer *mwriter);
static size_t hex_encode(const uint8 *src, size_t len, char *dst);

/*
 * Create a new backup manifest writer.
 *
 * The backup manifest will be written into a file named backup_manifest
 * in the specified directory.
 */
manifest_writer *
create_manifest_writer(char *directory, uint64 system_identifier)
{
	manifest_writer *mwriter = pg_malloc(sizeof(manifest_writer));

	snprintf(mwriter->pathname, MAXPGPATH, "%s/backup_manifest", directory);
	mwriter->fd = -1;
	initStringInfo(&mwriter->buf);
	mwriter->first_file = true;
	mwriter->still_checksumming = true;
	pg_checksum_init(&mwriter->manifest_ctx, CHECKSUM_TYPE_SHA256);

	appendStringInfo(&mwriter->buf,
					 "{ \"PostgreSQL-Backup-Manifest-Version\": 2,\n"
					 "\"System-Identifier\": " UINT64_FORMAT ",\n"
					 "\"Files\": [",
					 system_identifier);

	return mwriter;
}

/*
 * Add an entry for a file to a backup manifest.
 *
 * This is very similar to the backend's AddFileToBackupManifest, but
 * various adjustments are required due to frontend/backend differences
 * and other details.
 */
void
add_file_to_manifest(manifest_writer *mwriter, const char *manifest_path,
					 uint64 size, time_t mtime,
					 pg_checksum_type checksum_type,
					 int checksum_length,
					 uint8 *checksum_payload)
{
	int			pathlen = strlen(manifest_path);

	if (mwriter->first_file)
	{
		appendStringInfoChar(&mwriter->buf, '\n');
		mwriter->first_file = false;
	}
	else
		appendStringInfoString(&mwriter->buf, ",\n");

	if (pg_encoding_verifymbstr(PG_UTF8, manifest_path, pathlen) == pathlen)
	{
		appendStringInfoString(&mwriter->buf, "{ \"Path\": ");
		escape_json(&mwriter->buf, manifest_path);
		appendStringInfoString(&mwriter->buf, ", ");
	}
	else
	{
		appendStringInfoString(&mwriter->buf, "{ \"Encoded-Path\": \"");
		enlargeStringInfo(&mwriter->buf, 2 * pathlen);
		mwriter->buf.len += hex_encode((const uint8 *) manifest_path, pathlen,
									   &mwriter->buf.data[mwriter->buf.len]);
		appendStringInfoString(&mwriter->buf, "\", ");
	}

	appendStringInfo(&mwriter->buf, "\"Size\": %" PRIu64 ", ", size);

	appendStringInfoString(&mwriter->buf, "\"Last-Modified\": \"");
	enlargeStringInfo(&mwriter->buf, 128);
	mwriter->buf.len += strftime(&mwriter->buf.data[mwriter->buf.len], 128,
								 "%Y-%m-%d %H:%M:%S %Z",
								 gmtime(&mtime));
	appendStringInfoChar(&mwriter->buf, '"');

	if (mwriter->buf.len > 128 * 1024)
		flush_manifest(mwriter);

	if (checksum_length > 0)
	{
		appendStringInfo(&mwriter->buf,
						 ", \"Checksum-Algorithm\": \"%s\", \"Checksum\": \"",
						 pg_checksum_type_name(checksum_type));

		enlargeStringInfo(&mwriter->buf, 2 * checksum_length);
		mwriter->buf.len += hex_encode(checksum_payload, checksum_length,
									   &mwriter->buf.data[mwriter->buf.len]);

		appendStringInfoChar(&mwriter->buf, '"');
	}

	appendStringInfoString(&mwriter->buf, " }");

	if (mwriter->buf.len > 128 * 1024)
		flush_manifest(mwriter);
}

/*
 * Finalize the backup_manifest.
 */
void
finalize_manifest(manifest_writer *mwriter,
				  manifest_wal_range *first_wal_range)
{
	uint8		checksumbuf[PG_SHA256_DIGEST_LENGTH];
	int			len;
	manifest_wal_range *wal_range;

	/* Terminate the list of files. */
	appendStringInfoString(&mwriter->buf, "\n],\n");

	/* Start a list of LSN ranges. */
	appendStringInfoString(&mwriter->buf, "\"WAL-Ranges\": [\n");

	for (wal_range = first_wal_range; wal_range != NULL;
		 wal_range = wal_range->next)
		appendStringInfo(&mwriter->buf,
						 "%s{ \"Timeline\": %u, \"Start-LSN\": \"%X/%08X\", \"End-LSN\": \"%X/%08X\" }",
						 wal_range == first_wal_range ? "" : ",\n",
						 wal_range->tli,
						 LSN_FORMAT_ARGS(wal_range->start_lsn),
						 LSN_FORMAT_ARGS(wal_range->end_lsn));

	/* Terminate the list of WAL ranges. */
	appendStringInfoString(&mwriter->buf, "\n],\n");

	/* Flush accumulated data and update checksum calculation. */
	flush_manifest(mwriter);

	/* Checksum only includes data up to this point. */
	mwriter->still_checksumming = false;

	/* Compute and insert manifest checksum. */
	appendStringInfoString(&mwriter->buf, "\"Manifest-Checksum\": \"");
	enlargeStringInfo(&mwriter->buf, 2 * PG_SHA256_DIGEST_STRING_LENGTH);
	len = pg_checksum_final(&mwriter->manifest_ctx, checksumbuf);
	Assert(len == PG_SHA256_DIGEST_LENGTH);
	mwriter->buf.len +=
		hex_encode(checksumbuf, len, &mwriter->buf.data[mwriter->buf.len]);
	appendStringInfoString(&mwriter->buf, "\"}\n");

	/* Flush the last manifest checksum itself. */
	flush_manifest(mwriter);

	/* Close the file. */
	if (close(mwriter->fd) != 0)
		pg_fatal("could not close file \"%s\": %m", mwriter->pathname);
	mwriter->fd = -1;
}

/*
 * Produce a JSON string literal, properly escaping characters in the text.
 */
static void
escape_json(StringInfo buf, const char *str)
{
	const char *p;

	appendStringInfoCharMacro(buf, '"');
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			case '"':
				appendStringInfoString(buf, "\\\"");
				break;
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			default:
				if ((unsigned char) *p < ' ')
					appendStringInfo(buf, "\\u%04x", (int) *p);
				else
					appendStringInfoCharMacro(buf, *p);
				break;
		}
	}
	appendStringInfoCharMacro(buf, '"');
}

/*
 * Flush whatever portion of the backup manifest we have generated and
 * buffered in memory out to a file on disk.
 *
 * The first call to this function will create the file. After that, we
 * keep it open and just append more data.
 */
static void
flush_manifest(manifest_writer *mwriter)
{
	if (mwriter->fd == -1 &&
		(mwriter->fd = open(mwriter->pathname,
							O_WRONLY | O_CREAT | O_EXCL | PG_BINARY,
							pg_file_create_mode)) < 0)
		pg_fatal("could not open file \"%s\": %m", mwriter->pathname);

	if (mwriter->buf.len > 0)
	{
		ssize_t		wb;

		wb = write(mwriter->fd, mwriter->buf.data, mwriter->buf.len);
		if (wb != mwriter->buf.len)
		{
			if (wb < 0)
				pg_fatal("could not write file \"%s\": %m", mwriter->pathname);
			else
				pg_fatal("could not write file \"%s\": wrote %d of %d",
						 mwriter->pathname, (int) wb, mwriter->buf.len);
		}

		if (mwriter->still_checksumming &&
			pg_checksum_update(&mwriter->manifest_ctx,
							   (uint8 *) mwriter->buf.data,
							   mwriter->buf.len) < 0)
			pg_fatal("could not update checksum of file \"%s\"",
					 mwriter->pathname);
		resetStringInfo(&mwriter->buf);
	}
}

/*
 * Encode bytes using two hexadecimal digits for each one.
 */
static size_t
hex_encode(const uint8 *src, size_t len, char *dst)
{
	const uint8 *end = src + len;

	while (src < end)
	{
		unsigned	n1 = (*src >> 4) & 0xF;
		unsigned	n2 = *src & 0xF;

		*dst++ = n1 < 10 ? '0' + n1 : 'a' + n1 - 10;
		*dst++ = n2 < 10 ? '0' + n2 : 'a' + n2 - 10;
		++src;
	}

	return len * 2;
}
