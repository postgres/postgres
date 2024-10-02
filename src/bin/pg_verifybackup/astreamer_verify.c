/*-------------------------------------------------------------------------
 *
 * astreamer_verify.c
 *
 * Archive streamer for verification of a tar format backup (including
 * compressed tar format backups).
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/bin/pg_verifybackup/astreamer_verify.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "catalog/pg_control.h"
#include "pg_verifybackup.h"

typedef struct astreamer_verify
{
	/* These fields don't change once initialized. */
	astreamer	base;
	verifier_context *context;
	char	   *archive_name;
	Oid			tblspc_oid;

	/* These fields change for each archive member. */
	manifest_file *mfile;
	bool		verify_checksum;
	bool		verify_control_data;
	pg_checksum_context *checksum_ctx;
	uint64		checksum_bytes;
	ControlFileData control_file;
	uint64		control_file_bytes;
} astreamer_verify;

static void astreamer_verify_content(astreamer *streamer,
									 astreamer_member *member,
									 const char *data, int len,
									 astreamer_archive_context context);
static void astreamer_verify_finalize(astreamer *streamer);
static void astreamer_verify_free(astreamer *streamer);

static void member_verify_header(astreamer *streamer, astreamer_member *member);
static void member_compute_checksum(astreamer *streamer,
									astreamer_member *member,
									const char *data, int len);
static void member_verify_checksum(astreamer *streamer);
static void member_copy_control_data(astreamer *streamer,
									 astreamer_member *member,
									 const char *data, int len);
static void member_verify_control_data(astreamer *streamer);
static void member_reset_info(astreamer *streamer);

static const astreamer_ops astreamer_verify_ops = {
	.content = astreamer_verify_content,
	.finalize = astreamer_verify_finalize,
	.free = astreamer_verify_free
};

/*
 * Create an astreamer that can verify a tar file.
 */
astreamer *
astreamer_verify_content_new(astreamer *next, verifier_context *context,
							 char *archive_name, Oid tblspc_oid)
{
	astreamer_verify *streamer;

	streamer = palloc0(sizeof(astreamer_verify));
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_verify_ops;

	streamer->base.bbs_next = next;
	streamer->context = context;
	streamer->archive_name = archive_name;
	streamer->tblspc_oid = tblspc_oid;

	if (!context->skip_checksums)
		streamer->checksum_ctx = pg_malloc(sizeof(pg_checksum_context));

	return &streamer->base;
}

/*
 * Main entry point of the archive streamer for verifying tar members.
 */
static void
astreamer_verify_content(astreamer *streamer, astreamer_member *member,
						 const char *data, int len,
						 astreamer_archive_context context)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;

	Assert(context != ASTREAMER_UNKNOWN);

	switch (context)
	{
		case ASTREAMER_MEMBER_HEADER:
			/* Initial setup plus decide which checks to perform. */
			member_verify_header(streamer, member);
			break;

		case ASTREAMER_MEMBER_CONTENTS:
			/* Incremental work required to verify file contents. */
			if (mystreamer->verify_checksum)
				member_compute_checksum(streamer, member, data, len);
			if (mystreamer->verify_control_data)
				member_copy_control_data(streamer, member, data, len);
			break;

		case ASTREAMER_MEMBER_TRAILER:
			/* Now we've got all the file data. */
			if (mystreamer->verify_checksum)
				member_verify_checksum(streamer);
			if (mystreamer->verify_control_data)
				member_verify_control_data(streamer);

			/* Reset for next archive member. */
			member_reset_info(streamer);
			break;

		case ASTREAMER_ARCHIVE_TRAILER:
			break;

		default:
			/* Shouldn't happen. */
			pg_fatal("unexpected state while parsing tar file");
	}
}

/*
 * End-of-stream processing for a astreamer_verify stream.
 */
static void
astreamer_verify_finalize(astreamer *streamer)
{
	Assert(streamer->bbs_next == NULL);
}

/*
 * Free memory associated with a astreamer_verify stream.
 */
static void
astreamer_verify_free(astreamer *streamer)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;

	if (mystreamer->checksum_ctx)
		pfree(mystreamer->checksum_ctx);

	pfree(streamer);
}

/*
 * Prepare to validate the next archive member.
 */
static void
member_verify_header(astreamer *streamer, astreamer_member *member)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;
	manifest_file *m;
	char		pathname[MAXPGPATH];

	/* We are only interested in normal files. */
	if (member->is_directory || member->is_link)
		return;

	/*
	 * The backup manifest stores a relative path to the base directory for
	 * files belonging to a tablespace, while the tablespace backup tar
	 * archive does not include this path.
	 *
	 * The pathname taken from the tar file could contain '.' or '..'
	 * references, which we want to remove, so apply canonicalize_path(). It
	 * could also be an absolute pathname, which we want to treat as a
	 * relative path, so prepend "./" if we're not adding a tablespace prefix
	 * to make sure that canonicalize_path() does what we want.
	 */
	if (OidIsValid(mystreamer->tblspc_oid))
		snprintf(pathname, MAXPGPATH, "%s/%u/%s",
				 "pg_tblspc", mystreamer->tblspc_oid, member->pathname);
	else
		snprintf(pathname, MAXPGPATH, "./%s", member->pathname);
	canonicalize_path(pathname);

	/* Ignore any files that are listed in the ignore list. */
	if (should_ignore_relpath(mystreamer->context, pathname))
		return;

	/* Check whether there's an entry in the manifest hash. */
	m = manifest_files_lookup(mystreamer->context->manifest->files, pathname);
	if (m == NULL)
	{
		report_backup_error(mystreamer->context,
							"\"%s\" is present in \"%s\" but not in the manifest",
							member->pathname, mystreamer->archive_name);
		return;
	}
	mystreamer->mfile = m;

	/* Flag this entry as having been encountered in a tar archive. */
	m->matched = true;

	/* Check that the size matches. */
	if (m->size != member->size)
	{
		report_backup_error(mystreamer->context,
							"\"%s\" has size %llu in \"%s\" but size %llu in the manifest",
							member->pathname,
							(unsigned long long) member->size,
							mystreamer->archive_name,
							(unsigned long long) m->size);
		m->bad = true;
		return;
	}

	/*
	 * Decide whether we're going to verify the checksum for this file, and
	 * whether we're going to perform the additional validation that we do
	 * only for the control file.
	 */
	mystreamer->verify_checksum =
		(!mystreamer->context->skip_checksums && should_verify_checksum(m));
	mystreamer->verify_control_data =
		mystreamer->context->manifest->version != 1 &&
		!m->bad && strcmp(m->pathname, "global/pg_control") == 0;

	/* If we're going to verify the checksum, initial a checksum context. */
	if (mystreamer->verify_checksum &&
		pg_checksum_init(mystreamer->checksum_ctx, m->checksum_type) < 0)
	{
		report_backup_error(mystreamer->context,
							"%s: could not initialize checksum of file \"%s\"",
							mystreamer->archive_name, m->pathname);

		/*
		 * Checksum verification cannot be performed without proper context
		 * initialization.
		 */
		mystreamer->verify_checksum = false;
	}
}

/*
 * Computes the checksum incrementally for the received file content.
 *
 * Should have a correctly initialized checksum_ctx, which will be used for
 * incremental checksum computation.
 */
static void
member_compute_checksum(astreamer *streamer, astreamer_member *member,
						const char *data, int len)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;
	pg_checksum_context *checksum_ctx = mystreamer->checksum_ctx;
	manifest_file *m = mystreamer->mfile;

	Assert(mystreamer->verify_checksum);
	Assert(m->checksum_type == checksum_ctx->type);

	/*
	 * Update the total count of computed checksum bytes so that we can
	 * cross-check against the file size.
	 */
	mystreamer->checksum_bytes += len;

	/* Feed these bytes to the checksum calculation. */
	if (pg_checksum_update(checksum_ctx, (uint8 *) data, len) < 0)
	{
		report_backup_error(mystreamer->context,
							"could not update checksum of file \"%s\"",
							m->pathname);
		mystreamer->verify_checksum = false;
	}
}

/*
 * Perform the final computation and checksum verification after the entire
 * file content has been processed.
 */
static void
member_verify_checksum(astreamer *streamer)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;
	manifest_file *m = mystreamer->mfile;
	uint8		checksumbuf[PG_CHECKSUM_MAX_LENGTH];
	int			checksumlen;

	Assert(mystreamer->verify_checksum);

	/*
	 * It's unclear how this could fail, but let's check anyway to be safe.
	 */
	if (mystreamer->checksum_bytes != m->size)
	{
		report_backup_error(mystreamer->context,
							"file \"%s\" in \"%s\" should contain %llu bytes, but read %llu bytes",
							m->pathname, mystreamer->archive_name,
							(unsigned long long) m->size,
							(unsigned long long) mystreamer->checksum_bytes);
		return;
	}

	/* Get the final checksum. */
	checksumlen = pg_checksum_final(mystreamer->checksum_ctx, checksumbuf);
	if (checksumlen < 0)
	{
		report_backup_error(mystreamer->context,
							"could not finalize checksum of file \"%s\"",
							m->pathname);
		return;
	}

	/* And check it against the manifest. */
	if (checksumlen != m->checksum_length)
		report_backup_error(mystreamer->context,
							"file \"%s\" in \"%s\" has checksum of length %d, but expected %d",
							m->pathname, mystreamer->archive_name,
							m->checksum_length, checksumlen);
	else if (memcmp(checksumbuf, m->checksum_payload, checksumlen) != 0)
		report_backup_error(mystreamer->context,
							"checksum mismatch for file \"%s\" in \"%s\"",
							m->pathname, mystreamer->archive_name);
}

/*
 * Stores the pg_control file contents into a local buffer; we need the entire
 * control file data for verification.
 */
static void
member_copy_control_data(astreamer *streamer, astreamer_member *member,
						 const char *data, int len)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;

	/* Should be here only for control file */
	Assert(mystreamer->verify_control_data);

	/*
	 * Copy the new data into the control file buffer, but do not overrun the
	 * buffer. Note that the on-disk length of the control file is expected to
	 * be PG_CONTROL_FILE_SIZE, but the part that fits in our buffer is
	 * shorter, just sizeof(ControlFileData).
	 */
	if (mystreamer->control_file_bytes < sizeof(ControlFileData))
	{
		size_t		remaining;

		remaining = sizeof(ControlFileData) - mystreamer->control_file_bytes;
		memcpy(((char *) &mystreamer->control_file)
			   + mystreamer->control_file_bytes,
			   data, Min((size_t) len, remaining));
	}

	/* Remember how many bytes we saw, even if we didn't buffer them. */
	mystreamer->control_file_bytes += len;
}

/*
 * Performs the CRC calculation of pg_control data and then calls the routines
 * that execute the final verification of the control file information.
 */
static void
member_verify_control_data(astreamer *streamer)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;
	manifest_data *manifest = mystreamer->context->manifest;
	pg_crc32c	crc;

	/* Should be here only for control file */
	Assert(strcmp(mystreamer->mfile->pathname, "global/pg_control") == 0);
	Assert(mystreamer->verify_control_data);

	/*
	 * If the control file is not the right length, that's a big problem.
	 *
	 * NB: There is a theoretical overflow risk here from casting to int, but
	 * it isn't likely to be a real problem and this enables us to match the
	 * same format string that pg_rewind uses for this case. Perhaps both this
	 * and pg_rewind should use an unsigned 64-bit value, but for now we don't
	 * worry about it.
	 */
	if (mystreamer->control_file_bytes != PG_CONTROL_FILE_SIZE)
		report_fatal_error("unexpected control file size %d, expected %d",
						   (int) mystreamer->control_file_bytes,
						   PG_CONTROL_FILE_SIZE);

	/* Compute the CRC. */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &mystreamer->control_file,
				offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	/* Control file contents not meaningful if CRC is bad. */
	if (!EQ_CRC32C(crc, mystreamer->control_file.crc))
		report_fatal_error("%s: %s: CRC is incorrect",
						   mystreamer->archive_name,
						   mystreamer->mfile->pathname);

	/* Can't interpret control file if not current version. */
	if (mystreamer->control_file.pg_control_version != PG_CONTROL_VERSION)
		report_fatal_error("%s: %s: unexpected control file version",
						   mystreamer->archive_name,
						   mystreamer->mfile->pathname);

	/* System identifiers should match. */
	if (manifest->system_identifier !=
		mystreamer->control_file.system_identifier)
		report_fatal_error("%s: %s: manifest system identifier is %llu, but control file has %llu",
						   mystreamer->archive_name,
						   mystreamer->mfile->pathname,
						   (unsigned long long) manifest->system_identifier,
						   (unsigned long long) mystreamer->control_file.system_identifier);
}

/*
 * Reset flags and free memory allocations for member file verification.
 */
static void
member_reset_info(astreamer *streamer)
{
	astreamer_verify *mystreamer = (astreamer_verify *) streamer;

	mystreamer->mfile = NULL;
	mystreamer->verify_checksum = false;
	mystreamer->verify_control_data = false;
	mystreamer->checksum_bytes = 0;
	mystreamer->control_file_bytes = 0;
}
