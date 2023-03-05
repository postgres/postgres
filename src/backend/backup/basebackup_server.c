/*-------------------------------------------------------------------------
 *
 * basebackup_server.c
 *	  store basebackup archives on the server
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_server.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "backup/basebackup.h"
#include "backup/basebackup_sink.h"
#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

typedef struct bbsink_server
{
	/* Common information for all types of sink. */
	bbsink		base;

	/* Directory in which backup is to be stored. */
	char	   *pathname;

	/* Currently open file (or 0 if nothing open). */
	File		file;

	/* Current file position. */
	off_t		filepos;
} bbsink_server;

static void bbsink_server_begin_archive(bbsink *sink,
										const char *archive_name);
static void bbsink_server_archive_contents(bbsink *sink, size_t len);
static void bbsink_server_end_archive(bbsink *sink);
static void bbsink_server_begin_manifest(bbsink *sink);
static void bbsink_server_manifest_contents(bbsink *sink, size_t len);
static void bbsink_server_end_manifest(bbsink *sink);

static const bbsink_ops bbsink_server_ops = {
	.begin_backup = bbsink_forward_begin_backup,
	.begin_archive = bbsink_server_begin_archive,
	.archive_contents = bbsink_server_archive_contents,
	.end_archive = bbsink_server_end_archive,
	.begin_manifest = bbsink_server_begin_manifest,
	.manifest_contents = bbsink_server_manifest_contents,
	.end_manifest = bbsink_server_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_forward_cleanup
};

/*
 * Create a new 'server' bbsink.
 */
bbsink *
bbsink_server_new(bbsink *next, char *pathname)
{
	bbsink_server *sink = palloc0(sizeof(bbsink_server));

	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_server_ops;
	sink->pathname = pathname;
	sink->base.bbs_next = next;

	/* Replication permission is not sufficient in this case. */
	StartTransactionCommand();
	if (!has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser or a role with privileges of the pg_write_server_files role to create backup stored on server")));
	CommitTransactionCommand();

	/*
	 * It's not a good idea to store your backups in the same directory that
	 * you're backing up. If we allowed a relative path here, that could
	 * easily happen accidentally, so we don't. The user could still
	 * accomplish the same thing by including the absolute path to $PGDATA in
	 * the pathname, but that's likely an intentional bad decision rather than
	 * an accident.
	 */
	if (!is_absolute_path(pathname))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("relative path not allowed for backup stored on server")));

	switch (pg_check_dir(pathname))
	{
		case 0:

			/*
			 * Does not exist, so create it using the same permissions we'd
			 * use for a new subdirectory of the data directory itself.
			 */
			if (MakePGDirectory(pathname) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not create directory \"%s\": %m", pathname)));
			break;

		case 1:
			/* Exists, empty. */
			break;

		case 2:
		case 3:
		case 4:
			/* Exists, not empty. */
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_FILE),
					 errmsg("directory \"%s\" exists but is not empty",
							pathname)));
			break;

		default:
			/* Access problem. */
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access directory \"%s\": %m",
							pathname)));
	}

	return &sink->base;
}

/*
 * Open the correct output file for this archive.
 */
static void
bbsink_server_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_server *mysink = (bbsink_server *) sink;
	char	   *filename;

	Assert(mysink->file == 0);
	Assert(mysink->filepos == 0);

	filename = psprintf("%s/%s", mysink->pathname, archive_name);

	mysink->file = PathNameOpenFile(filename,
									O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (mysink->file <= 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", filename)));

	pfree(filename);

	bbsink_forward_begin_archive(sink, archive_name);
}

/*
 * Write the data to the output file.
 */
static void
bbsink_server_archive_contents(bbsink *sink, size_t len)
{
	bbsink_server *mysink = (bbsink_server *) sink;
	int			nbytes;

	nbytes = FileWrite(mysink->file, mysink->base.bbs_buffer, len,
					   mysink->filepos, WAIT_EVENT_BASEBACKUP_WRITE);

	if (nbytes != len)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							FilePathName(mysink->file)),
					 errhint("Check free disk space.")));
		/* short write: complain appropriately */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not write file \"%s\": wrote only %d of %d bytes at offset %u",
						FilePathName(mysink->file),
						nbytes, (int) len, (unsigned) mysink->filepos),
				 errhint("Check free disk space.")));
	}

	mysink->filepos += nbytes;

	bbsink_forward_archive_contents(sink, len);
}

/*
 * fsync and close the current output file.
 */
static void
bbsink_server_end_archive(bbsink *sink)
{
	bbsink_server *mysink = (bbsink_server *) sink;

	/*
	 * We intentionally don't use data_sync_elevel here, because the server
	 * shouldn't PANIC just because we can't guarantee that the backup has
	 * been written down to disk. Running recovery won't fix anything in this
	 * case anyway.
	 */
	if (FileSync(mysink->file, WAIT_EVENT_BASEBACKUP_SYNC) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						FilePathName(mysink->file))));


	/* We're done with this file now. */
	FileClose(mysink->file);
	mysink->file = 0;
	mysink->filepos = 0;

	bbsink_forward_end_archive(sink);
}

/*
 * Open the output file to which we will write the manifest.
 *
 * Just like pg_basebackup, we write the manifest first under a temporary
 * name and then rename it into place after fsync. That way, if the manifest
 * is there and under the correct name, the user can be sure that the backup
 * completed.
 */
static void
bbsink_server_begin_manifest(bbsink *sink)
{
	bbsink_server *mysink = (bbsink_server *) sink;
	char	   *tmp_filename;

	Assert(mysink->file == 0);

	tmp_filename = psprintf("%s/backup_manifest.tmp", mysink->pathname);

	mysink->file = PathNameOpenFile(tmp_filename,
									O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (mysink->file <= 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmp_filename)));

	pfree(tmp_filename);

	bbsink_forward_begin_manifest(sink);
}

/*
 * Each chunk of manifest data is sent using a CopyData message.
 */
static void
bbsink_server_manifest_contents(bbsink *sink, size_t len)
{
	bbsink_server *mysink = (bbsink_server *) sink;
	int			nbytes;

	nbytes = FileWrite(mysink->file, mysink->base.bbs_buffer, len,
					   mysink->filepos, WAIT_EVENT_BASEBACKUP_WRITE);

	if (nbytes != len)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							FilePathName(mysink->file)),
					 errhint("Check free disk space.")));
		/* short write: complain appropriately */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not write file \"%s\": wrote only %d of %d bytes at offset %u",
						FilePathName(mysink->file),
						nbytes, (int) len, (unsigned) mysink->filepos),
				 errhint("Check free disk space.")));
	}

	mysink->filepos += nbytes;

	bbsink_forward_manifest_contents(sink, len);
}

/*
 * fsync the backup manifest, close the file, and then rename it into place.
 */
static void
bbsink_server_end_manifest(bbsink *sink)
{
	bbsink_server *mysink = (bbsink_server *) sink;
	char	   *tmp_filename;
	char	   *filename;

	/* We're done with this file now. */
	FileClose(mysink->file);
	mysink->file = 0;

	/*
	 * Rename it into place. This also fsyncs the temporary file, so we don't
	 * need to do that here. We don't use data_sync_elevel here for the same
	 * reasons as in bbsink_server_end_archive.
	 */
	tmp_filename = psprintf("%s/backup_manifest.tmp", mysink->pathname);
	filename = psprintf("%s/backup_manifest", mysink->pathname);
	durable_rename(tmp_filename, filename, ERROR);
	pfree(filename);
	pfree(tmp_filename);

	bbsink_forward_end_manifest(sink);
}
