/*-------------------------------------------------------------------------
 *
 * file_ops.c
 *	  Helper functions for operating on files.
 *
 * Most of the functions in this file are helper functions for writing to
 * the target data directory. The functions check the --dry-run flag, and
 * do nothing if it's enabled. You should avoid accessing the target files
 * directly but if you do, make sure you honor the --dry-run mode!
 *
 * Portions Copyright (c) 2013-2024, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "common/file_utils.h"
#include "file_ops.h"
#include "filemap.h"
#include "pg_rewind.h"

/*
 * Currently open target file.
 */
static int	dstfd = -1;
static char dstpath[MAXPGPATH] = "";

static void create_target_dir(const char *path);
static void remove_target_dir(const char *path);
static void create_target_symlink(const char *path, const char *link);
static void remove_target_symlink(const char *path);

static void recurse_dir(const char *datadir, const char *parentpath,
						process_file_callback_t callback);

/*
 * Open a target file for writing. If 'trunc' is true and the file already
 * exists, it will be truncated.
 */
void
open_target_file(const char *path, bool trunc)
{
	int			mode;

	if (dry_run)
		return;

	if (dstfd != -1 && !trunc &&
		strcmp(path, &dstpath[strlen(datadir_target) + 1]) == 0)
		return;					/* already open */

	close_target_file();

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);

	mode = O_WRONLY | O_CREAT | PG_BINARY;
	if (trunc)
		mode |= O_TRUNC;
	dstfd = open(dstpath, mode, pg_file_create_mode);
	if (dstfd < 0)
		pg_fatal("could not open target file \"%s\": %m",
				 dstpath);
}

/*
 * Close target file, if it's open.
 */
void
close_target_file(void)
{
	if (dstfd == -1)
		return;

	if (close(dstfd) != 0)
		pg_fatal("could not close target file \"%s\": %m",
				 dstpath);

	dstfd = -1;
}

void
write_target_range(char *buf, off_t begin, size_t size)
{
	size_t		writeleft;
	char	   *p;

	/* update progress report */
	fetch_done += size;
	progress_report(false);

	if (dry_run)
		return;

	if (lseek(dstfd, begin, SEEK_SET) == -1)
		pg_fatal("could not seek in target file \"%s\": %m",
				 dstpath);

	writeleft = size;
	p = buf;
	while (writeleft > 0)
	{
		ssize_t		writelen;

		errno = 0;
		writelen = write(dstfd, p, writeleft);
		if (writelen < 0)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			pg_fatal("could not write file \"%s\": %m",
					 dstpath);
		}

		p += writelen;
		writeleft -= writelen;
	}

	/* keep the file open, in case we need to copy more blocks in it */
}


void
remove_target(file_entry_t *entry)
{
	Assert(entry->action == FILE_ACTION_REMOVE);
	Assert(entry->target_exists);

	switch (entry->target_type)
	{
		case FILE_TYPE_DIRECTORY:
			remove_target_dir(entry->path);
			break;

		case FILE_TYPE_REGULAR:
			remove_target_file(entry->path, false);
			break;

		case FILE_TYPE_SYMLINK:
			remove_target_symlink(entry->path);
			break;

		case FILE_TYPE_UNDEFINED:
			pg_fatal("undefined file type for \"%s\"", entry->path);
			break;
	}
}

void
create_target(file_entry_t *entry)
{
	Assert(entry->action == FILE_ACTION_CREATE);
	Assert(!entry->target_exists);

	switch (entry->source_type)
	{
		case FILE_TYPE_DIRECTORY:
			create_target_dir(entry->path);
			break;

		case FILE_TYPE_SYMLINK:
			create_target_symlink(entry->path, entry->source_link_target);
			break;

		case FILE_TYPE_REGULAR:
			/* can't happen. Regular files are created with open_target_file. */
			pg_fatal("invalid action (CREATE) for regular file");
			break;

		case FILE_TYPE_UNDEFINED:
			pg_fatal("undefined file type for \"%s\"", entry->path);
			break;
	}
}

/*
 * Remove a file from target data directory.  If missing_ok is true, it
 * is fine for the target file to not exist.
 */
void
remove_target_file(const char *path, bool missing_ok)
{
	char		dstpath[MAXPGPATH];

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);
	if (unlink(dstpath) != 0)
	{
		if (errno == ENOENT && missing_ok)
			return;

		pg_fatal("could not remove file \"%s\": %m",
				 dstpath);
	}
}

void
truncate_target_file(const char *path, off_t newsize)
{
	char		dstpath[MAXPGPATH];
	int			fd;

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);

	fd = open(dstpath, O_WRONLY, pg_file_create_mode);
	if (fd < 0)
		pg_fatal("could not open file \"%s\" for truncation: %m",
				 dstpath);

	if (ftruncate(fd, newsize) != 0)
		pg_fatal("could not truncate file \"%s\" to %u: %m",
				 dstpath, (unsigned int) newsize);

	close(fd);
}

static void
create_target_dir(const char *path)
{
	char		dstpath[MAXPGPATH];

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);
	if (mkdir(dstpath, pg_dir_create_mode) != 0)
		pg_fatal("could not create directory \"%s\": %m",
				 dstpath);
}

static void
remove_target_dir(const char *path)
{
	char		dstpath[MAXPGPATH];

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);
	if (rmdir(dstpath) != 0)
		pg_fatal("could not remove directory \"%s\": %m",
				 dstpath);
}

static void
create_target_symlink(const char *path, const char *link)
{
	char		dstpath[MAXPGPATH];

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);
	if (symlink(link, dstpath) != 0)
		pg_fatal("could not create symbolic link at \"%s\": %m",
				 dstpath);
}

static void
remove_target_symlink(const char *path)
{
	char		dstpath[MAXPGPATH];

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);
	if (unlink(dstpath) != 0)
		pg_fatal("could not remove symbolic link \"%s\": %m",
				 dstpath);
}

/*
 * Sync target data directory to ensure that modifications are safely on disk.
 *
 * We do this once, for the whole data directory, for performance reasons.  At
 * the end of pg_rewind's run, the kernel is likely to already have flushed
 * most dirty buffers to disk.  Additionally sync_pgdata uses a two-pass
 * approach when fsync is specified (only initiating writeback in the first
 * pass), which often reduces the overall amount of IO noticeably.
 */
void
sync_target_dir(void)
{
	if (!do_sync || dry_run)
		return;

	sync_pgdata(datadir_target, PG_VERSION_NUM, sync_method);
}


/*
 * Read a file into memory. The file to be read is <datadir>/<path>.
 * The file contents are returned in a malloc'd buffer, and *filesize
 * is set to the length of the file.
 *
 * The returned buffer is always zero-terminated; the size of the returned
 * buffer is actually *filesize + 1. That's handy when reading a text file.
 * This function can be used to read binary files as well, you can just
 * ignore the zero-terminator in that case.
 */
char *
slurpFile(const char *datadir, const char *path, size_t *filesize)
{
	int			fd;
	char	   *buffer;
	struct stat statbuf;
	char		fullpath[MAXPGPATH];
	int			len;
	int			r;

	snprintf(fullpath, sizeof(fullpath), "%s/%s", datadir, path);

	if ((fd = open(fullpath, O_RDONLY | PG_BINARY, 0)) == -1)
		pg_fatal("could not open file \"%s\" for reading: %m",
				 fullpath);

	if (fstat(fd, &statbuf) < 0)
		pg_fatal("could not open file \"%s\" for reading: %m",
				 fullpath);

	len = statbuf.st_size;

	buffer = pg_malloc(len + 1);

	r = read(fd, buffer, len);
	if (r != len)
	{
		if (r < 0)
			pg_fatal("could not read file \"%s\": %m",
					 fullpath);
		else
			pg_fatal("could not read file \"%s\": read %d of %zu",
					 fullpath, r, (Size) len);
	}
	close(fd);

	/* Zero-terminate the buffer. */
	buffer[len] = '\0';

	if (filesize)
		*filesize = len;
	return buffer;
}

/*
 * Traverse through all files in a data directory, calling 'callback'
 * for each file.
 */
void
traverse_datadir(const char *datadir, process_file_callback_t callback)
{
	recurse_dir(datadir, NULL, callback);
}

/*
 * recursive part of traverse_datadir
 *
 * parentpath is the current subdirectory's path relative to datadir,
 * or NULL at the top level.
 */
static void
recurse_dir(const char *datadir, const char *parentpath,
			process_file_callback_t callback)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		fullparentpath[MAXPGPATH];

	if (parentpath)
		snprintf(fullparentpath, MAXPGPATH, "%s/%s", datadir, parentpath);
	else
		snprintf(fullparentpath, MAXPGPATH, "%s", datadir);

	xldir = opendir(fullparentpath);
	if (xldir == NULL)
		pg_fatal("could not open directory \"%s\": %m",
				 fullparentpath);

	while (errno = 0, (xlde = readdir(xldir)) != NULL)
	{
		struct stat fst;
		char		fullpath[MAXPGPATH * 2];
		char		path[MAXPGPATH * 2];

		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(fullpath, sizeof(fullpath), "%s/%s", fullparentpath, xlde->d_name);

		if (lstat(fullpath, &fst) < 0)
		{
			if (errno == ENOENT)
			{
				/*
				 * File doesn't exist anymore. This is ok, if the new primary
				 * is running and the file was just removed. If it was a data
				 * file, there should be a WAL record of the removal. If it
				 * was something else, it couldn't have been anyway.
				 *
				 * TODO: But complain if we're processing the target dir!
				 */
			}
			else
				pg_fatal("could not stat file \"%s\": %m",
						 fullpath);
		}

		if (parentpath)
			snprintf(path, sizeof(path), "%s/%s", parentpath, xlde->d_name);
		else
			snprintf(path, sizeof(path), "%s", xlde->d_name);

		if (S_ISREG(fst.st_mode))
			callback(path, FILE_TYPE_REGULAR, fst.st_size, NULL);
		else if (S_ISDIR(fst.st_mode))
		{
			callback(path, FILE_TYPE_DIRECTORY, 0, NULL);
			/* recurse to handle subdirectories */
			recurse_dir(datadir, path, callback);
		}
		else if (S_ISLNK(fst.st_mode))
		{
			char		link_target[MAXPGPATH];
			int			len;

			len = readlink(fullpath, link_target, sizeof(link_target));
			if (len < 0)
				pg_fatal("could not read symbolic link \"%s\": %m",
						 fullpath);
			if (len >= sizeof(link_target))
				pg_fatal("symbolic link \"%s\" target is too long",
						 fullpath);
			link_target[len] = '\0';

			callback(path, FILE_TYPE_SYMLINK, 0, link_target);

			/*
			 * If it's a symlink within pg_tblspc, we need to recurse into it,
			 * to process all the tablespaces.  We also follow a symlink if
			 * it's for pg_wal.  Symlinks elsewhere are ignored.
			 */
			if ((parentpath && strcmp(parentpath, PG_TBLSPC_DIR) == 0) ||
				strcmp(path, "pg_wal") == 0)
				recurse_dir(datadir, path, callback);
		}
	}

	if (errno)
		pg_fatal("could not read directory \"%s\": %m",
				 fullparentpath);

	if (closedir(xldir))
		pg_fatal("could not close directory \"%s\": %m",
				 fullparentpath);
}
