/*-------------------------------------------------------------------------
 *
 * backup_manifest.c
 *	  code for generating and sending a backup manifest
 *
 * Portions Copyright (c) 2010-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/backup_manifest.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/timeline.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "replication/backup_manifest.h"
#include "utils/builtins.h"
#include "utils/json.h"

static void AppendStringToManifest(backup_manifest_info *manifest, char *s);

/*
 * Does the user want a backup manifest?
 *
 * It's simplest to always have a manifest_info object, so that we don't need
 * checks for NULL pointers in too many places. However, if the user doesn't
 * want a manifest, we set manifest->buffile to NULL.
 */
static inline bool
IsManifestEnabled(backup_manifest_info *manifest)
{
	return (manifest->buffile != NULL);
}

/*
 * Convenience macro for appending data to the backup manifest.
 */
#define AppendToManifest(manifest, ...) \
	{ \
		char *_manifest_s = psprintf(__VA_ARGS__);	\
		AppendStringToManifest(manifest, _manifest_s);	\
		pfree(_manifest_s);	\
	}

/*
 * Initialize state so that we can construct a backup manifest.
 *
 * NB: Although the checksum type for the data files is configurable, the
 * checksum for the manifest itself always uses SHA-256. See comments in
 * SendBackupManifest.
 */
void
InitializeBackupManifest(backup_manifest_info *manifest,
						 backup_manifest_option want_manifest,
						 pg_checksum_type manifest_checksum_type)
{
	if (want_manifest == MANIFEST_OPTION_NO)
		manifest->buffile = NULL;
	else
		manifest->buffile = BufFileCreateTemp(false);
	manifest->checksum_type = manifest_checksum_type;
	pg_sha256_init(&manifest->manifest_ctx);
	manifest->manifest_size = UINT64CONST(0);
	manifest->force_encode = (want_manifest == MANIFEST_OPTION_FORCE_ENCODE);
	manifest->first_file = true;
	manifest->still_checksumming = true;

	if (want_manifest != MANIFEST_OPTION_NO)
		AppendToManifest(manifest,
						 "{ \"PostgreSQL-Backup-Manifest-Version\": 1,\n"
						 "\"Files\": [");
}

/*
 * Add an entry to the backup manifest for a file.
 */
void
AddFileToBackupManifest(backup_manifest_info *manifest, const char *spcoid,
						const char *pathname, size_t size, pg_time_t mtime,
						pg_checksum_context *checksum_ctx)
{
	char		pathbuf[MAXPGPATH];
	int			pathlen;
	StringInfoData buf;

	if (!IsManifestEnabled(manifest))
		return;

	/*
	 * If this file is part of a tablespace, the pathname passed to this
	 * function will be relative to the tar file that contains it. We want the
	 * pathname relative to the data directory (ignoring the intermediate
	 * symlink traversal).
	 */
	if (spcoid != NULL)
	{
		snprintf(pathbuf, sizeof(pathbuf), "pg_tblspc/%s/%s", spcoid,
				 pathname);
		pathname = pathbuf;
	}

	/*
	 * Each file's entry needs to be separated from any entry that follows by
	 * a comma, but there's no comma before the first one or after the last
	 * one. To make that work, adding a file to the manifest starts by
	 * terminating the most recently added line, with a comma if appropriate,
	 * but does not terminate the line inserted for this file.
	 */
	initStringInfo(&buf);
	if (manifest->first_file)
	{
		appendStringInfoString(&buf, "\n");
		manifest->first_file = false;
	}
	else
		appendStringInfoString(&buf, ",\n");

	/*
	 * Write the relative pathname to this file out to the manifest. The
	 * manifest is always stored in UTF-8, so we have to encode paths that are
	 * not valid in that encoding.
	 */
	pathlen = strlen(pathname);
	if (!manifest->force_encode &&
		pg_verify_mbstr(PG_UTF8, pathname, pathlen, true))
	{
		appendStringInfoString(&buf, "{ \"Path\": ");
		escape_json(&buf, pathname);
		appendStringInfoString(&buf, ", ");
	}
	else
	{
		appendStringInfoString(&buf, "{ \"Encoded-Path\": \"");
		enlargeStringInfo(&buf, 2 * pathlen);
		buf.len += hex_encode(pathname, pathlen,
							  &buf.data[buf.len]);
		appendStringInfoString(&buf, "\", ");
	}

	appendStringInfo(&buf, "\"Size\": %zu, ", size);

	/*
	 * Convert last modification time to a string and append it to the
	 * manifest. Since it's not clear what time zone to use and since time
	 * zone definitions can change, possibly causing confusion, use GMT
	 * always.
	 */
	appendStringInfoString(&buf, "\"Last-Modified\": \"");
	enlargeStringInfo(&buf, 128);
	buf.len += pg_strftime(&buf.data[buf.len], 128, "%Y-%m-%d %H:%M:%S %Z",
						   pg_gmtime(&mtime));
	appendStringInfoString(&buf, "\"");

	/* Add checksum information. */
	if (checksum_ctx->type != CHECKSUM_TYPE_NONE)
	{
		uint8		checksumbuf[PG_CHECKSUM_MAX_LENGTH];
		int			checksumlen;

		checksumlen = pg_checksum_final(checksum_ctx, checksumbuf);

		appendStringInfo(&buf,
						 ", \"Checksum-Algorithm\": \"%s\", \"Checksum\": \"",
						 pg_checksum_type_name(checksum_ctx->type));
		enlargeStringInfo(&buf, 2 * checksumlen);
		buf.len += hex_encode((char *) checksumbuf, checksumlen,
							  &buf.data[buf.len]);
		appendStringInfoString(&buf, "\"");
	}

	/* Close out the object. */
	appendStringInfoString(&buf, " }");

	/* OK, add it to the manifest. */
	AppendStringToManifest(manifest, buf.data);

	/* Avoid leaking memory. */
	pfree(buf.data);
}

/*
 * Add information about the WAL that will need to be replayed when restoring
 * this backup to the manifest.
 */
void
AddWALInfoToBackupManifest(backup_manifest_info *manifest, XLogRecPtr startptr,
						   TimeLineID starttli, XLogRecPtr endptr,
						   TimeLineID endtli)
{
	List	   *timelines;
	ListCell   *lc;
	bool		first_wal_range = true;
	bool		found_start_timeline = false;

	if (!IsManifestEnabled(manifest))
		return;

	/* Terminate the list of files. */
	AppendStringToManifest(manifest, "\n],\n");

	/* Read the timeline history for the ending timeline. */
	timelines = readTimeLineHistory(endtli);

	/* Start a list of LSN ranges. */
	AppendStringToManifest(manifest, "\"WAL-Ranges\": [\n");

	foreach(lc, timelines)
	{
		TimeLineHistoryEntry *entry = lfirst(lc);
		XLogRecPtr	tl_beginptr;

		/*
		 * We only care about timelines that were active during the backup.
		 * Skip any that ended before the backup started. (Note that if
		 * entry->end is InvalidXLogRecPtr, it means that the timeline has not
		 * yet ended.)
		 */
		if (!XLogRecPtrIsInvalid(entry->end) && entry->end < startptr)
			continue;

		/*
		 * Because the timeline history file lists newer timelines before
		 * older ones, the first timeline we encounter that is new enough to
		 * matter ought to match the ending timeline of the backup.
		 */
		if (first_wal_range && endtli != entry->tli)
			ereport(ERROR,
					errmsg("expected end timeline %u but found timeline %u",
						   starttli, entry->tli));

		if (!XLogRecPtrIsInvalid(entry->begin))
			tl_beginptr = entry->begin;
		else
		{
			tl_beginptr = startptr;

			/*
			 * If we reach a TLI that has no valid beginning LSN, there can't
			 * be any more timelines in the history after this point, so we'd
			 * better have arrived at the expected starting TLI. If not,
			 * something's gone horribly wrong.
			 */
			if (starttli != entry->tli)
				ereport(ERROR,
						errmsg("expected start timeline %u but found timeline %u",
							   starttli, entry->tli));
		}

		AppendToManifest(manifest,
						 "%s{ \"Timeline\": %u, \"Start-LSN\": \"%X/%X\", \"End-LSN\": \"%X/%X\" }",
						 first_wal_range ? "" : ",\n",
						 entry->tli,
						 (uint32) (tl_beginptr >> 32), (uint32) tl_beginptr,
						 (uint32) (endptr >> 32), (uint32) endptr);

		if (starttli == entry->tli)
		{
			found_start_timeline = true;
			break;
		}

		endptr = entry->begin;
		first_wal_range = false;
	}

	/*
	 * The last entry in the timeline history for the ending timeline should
	 * be the ending timeline itself. Verify that this is what we observed.
	 */
	if (!found_start_timeline)
		ereport(ERROR,
				errmsg("start timeline %u not found in history of timeline %u",
					   starttli, endtli));

	/* Terminate the list of WAL ranges. */
	AppendStringToManifest(manifest, "\n],\n");
}

/*
 * Finalize the backup manifest, and send it to the client.
 */
void
SendBackupManifest(backup_manifest_info *manifest)
{
	StringInfoData protobuf;
	uint8		checksumbuf[PG_SHA256_DIGEST_LENGTH];
	char		checksumstringbuf[PG_SHA256_DIGEST_STRING_LENGTH];
	size_t		manifest_bytes_done = 0;

	if (!IsManifestEnabled(manifest))
		return;

	/*
	 * Append manifest checksum, so that the problems with the manifest itself
	 * can be detected.
	 *
	 * We always use SHA-256 for this, regardless of what algorithm is chosen
	 * for checksumming the files.  If we ever want to make the checksum
	 * algorithm used for the manifest file variable, the client will need a
	 * way to figure out which algorithm to use as close to the beginning of
	 * the manifest file as possible, to avoid having to read the whole thing
	 * twice.
	 */
	manifest->still_checksumming = false;
	pg_sha256_final(&manifest->manifest_ctx, checksumbuf);
	AppendStringToManifest(manifest, "\"Manifest-Checksum\": \"");
	hex_encode((char *) checksumbuf, sizeof checksumbuf, checksumstringbuf);
	checksumstringbuf[PG_SHA256_DIGEST_STRING_LENGTH - 1] = '\0';
	AppendStringToManifest(manifest, checksumstringbuf);
	AppendStringToManifest(manifest, "\"}\n");

	/*
	 * We've written all the data to the manifest file.  Rewind the file so
	 * that we can read it all back.
	 */
	if (BufFileSeek(manifest->buffile, 0, 0L, SEEK_SET))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind temporary file")));

	/* Send CopyOutResponse message */
	pq_beginmessage(&protobuf, 'H');
	pq_sendbyte(&protobuf, 0);	/* overall format */
	pq_sendint16(&protobuf, 0); /* natts */
	pq_endmessage(&protobuf);

	/*
	 * Send CopyData messages.
	 *
	 * We choose to read back the data from the temporary file in chunks of
	 * size BLCKSZ; this isn't necessary, but buffile.c uses that as the I/O
	 * size, so it seems to make sense to match that value here.
	 */
	while (manifest_bytes_done < manifest->manifest_size)
	{
		char		manifestbuf[BLCKSZ];
		size_t		bytes_to_read;
		size_t		rc;

		bytes_to_read = Min(sizeof(manifestbuf),
							manifest->manifest_size - manifest_bytes_done);
		rc = BufFileRead(manifest->buffile, manifestbuf, bytes_to_read);
		if (rc != bytes_to_read)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from temporary file: %m")));
		pq_putmessage('d', manifestbuf, bytes_to_read);
		manifest_bytes_done += bytes_to_read;
	}

	/* No more data, so send CopyDone message */
	pq_putemptymessage('c');

	/* Release resources */
	BufFileClose(manifest->buffile);
}

/*
 * Append a cstring to the manifest.
 */
static void
AppendStringToManifest(backup_manifest_info *manifest, char *s)
{
	int			len = strlen(s);

	Assert(manifest != NULL);
	if (manifest->still_checksumming)
		pg_sha256_update(&manifest->manifest_ctx, (uint8 *) s, len);
	BufFileWrite(manifest->buffile, s, len);
	manifest->manifest_size += len;
}
