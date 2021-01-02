/*-------------------------------------------------------------------------
 *
 * File-processing utility routines.
 *
 * Assorted utility functions to work on files.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/common/file_utils.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/file_utils.h"
#ifdef FRONTEND
#include "common/logging.h"
#endif

#ifdef FRONTEND

/* Define PG_FLUSH_DATA_WORKS if we have an implementation for pg_flush_data */
#if defined(HAVE_SYNC_FILE_RANGE)
#define PG_FLUSH_DATA_WORKS 1
#elif defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
#define PG_FLUSH_DATA_WORKS 1
#endif

/*
 * pg_xlog has been renamed to pg_wal in version 10.
 */
#define MINIMUM_VERSION_FOR_PG_WAL	100000

#ifdef PG_FLUSH_DATA_WORKS
static int	pre_sync_fname(const char *fname, bool isdir);
#endif
static void walkdir(const char *path,
					int (*action) (const char *fname, bool isdir),
					bool process_symlinks);

/*
 * Issue fsync recursively on PGDATA and all its contents.
 *
 * We fsync regular files and directories wherever they are, but we follow
 * symlinks only for pg_wal (or pg_xlog) and immediately under pg_tblspc.
 * Other symlinks are presumed to point at files we're not responsible for
 * fsyncing, and might not have privileges to write at all.
 *
 * serverVersion indicates the version of the server to be fsync'd.
 */
void
fsync_pgdata(const char *pg_data,
			 int serverVersion)
{
	bool		xlog_is_symlink;
	char		pg_wal[MAXPGPATH];
	char		pg_tblspc[MAXPGPATH];

	/* handle renaming of pg_xlog to pg_wal in post-10 clusters */
	snprintf(pg_wal, MAXPGPATH, "%s/%s", pg_data,
			 serverVersion < MINIMUM_VERSION_FOR_PG_WAL ? "pg_xlog" : "pg_wal");
	snprintf(pg_tblspc, MAXPGPATH, "%s/pg_tblspc", pg_data);

	/*
	 * If pg_wal is a symlink, we'll need to recurse into it separately,
	 * because the first walkdir below will ignore it.
	 */
	xlog_is_symlink = false;

#ifndef WIN32
	{
		struct stat st;

		if (lstat(pg_wal, &st) < 0)
			pg_log_error("could not stat file \"%s\": %m", pg_wal);
		else if (S_ISLNK(st.st_mode))
			xlog_is_symlink = true;
	}
#else
	if (pgwin32_is_junction(pg_wal))
		xlog_is_symlink = true;
#endif

	/*
	 * If possible, hint to the kernel that we're soon going to fsync the data
	 * directory and its contents.
	 */
#ifdef PG_FLUSH_DATA_WORKS
	walkdir(pg_data, pre_sync_fname, false);
	if (xlog_is_symlink)
		walkdir(pg_wal, pre_sync_fname, false);
	walkdir(pg_tblspc, pre_sync_fname, true);
#endif

	/*
	 * Now we do the fsync()s in the same order.
	 *
	 * The main call ignores symlinks, so in addition to specially processing
	 * pg_wal if it's a symlink, pg_tblspc has to be visited separately with
	 * process_symlinks = true.  Note that if there are any plain directories
	 * in pg_tblspc, they'll get fsync'd twice.  That's not an expected case
	 * so we don't worry about optimizing it.
	 */
	walkdir(pg_data, fsync_fname, false);
	if (xlog_is_symlink)
		walkdir(pg_wal, fsync_fname, false);
	walkdir(pg_tblspc, fsync_fname, true);
}

/*
 * Issue fsync recursively on the given directory and all its contents.
 *
 * This is a convenient wrapper on top of walkdir().
 */
void
fsync_dir_recurse(const char *dir)
{
	/*
	 * If possible, hint to the kernel that we're soon going to fsync the data
	 * directory and its contents.
	 */
#ifdef PG_FLUSH_DATA_WORKS
	walkdir(dir, pre_sync_fname, false);
#endif

	walkdir(dir, fsync_fname, false);
}

/*
 * walkdir: recursively walk a directory, applying the action to each
 * regular file and directory (including the named directory itself).
 *
 * If process_symlinks is true, the action and recursion are also applied
 * to regular files and directories that are pointed to by symlinks in the
 * given directory; otherwise symlinks are ignored.  Symlinks are always
 * ignored in subdirectories, ie we intentionally don't pass down the
 * process_symlinks flag to recursive calls.
 *
 * Errors are reported but not considered fatal.
 *
 * See also walkdir in fd.c, which is a backend version of this logic.
 */
static void
walkdir(const char *path,
		int (*action) (const char *fname, bool isdir),
		bool process_symlinks)
{
	DIR		   *dir;
	struct dirent *de;

	dir = opendir(path);
	if (dir == NULL)
	{
		pg_log_error("could not open directory \"%s\": %m", path);
		return;
	}

	while (errno = 0, (de = readdir(dir)) != NULL)
	{
		char		subpath[MAXPGPATH * 2];

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);

		switch (get_dirent_type(subpath, de, process_symlinks, PG_LOG_ERROR))
		{
			case PGFILETYPE_REG:
				(*action) (subpath, false);
				break;
			case PGFILETYPE_DIR:
				walkdir(subpath, action, false);
				break;
			default:

				/*
				 * Errors are already reported directly by get_dirent_type(),
				 * and any remaining symlinks and unknown file types are
				 * ignored.
				 */
				break;
		}
	}

	if (errno)
		pg_log_error("could not read directory \"%s\": %m", path);

	(void) closedir(dir);

	/*
	 * It's important to fsync the destination directory itself as individual
	 * file fsyncs don't guarantee that the directory entry for the file is
	 * synced.  Recent versions of ext4 have made the window much wider but
	 * it's been an issue for ext3 and other filesystems in the past.
	 */
	(*action) (path, true);
}

/*
 * Hint to the OS that it should get ready to fsync() this file.
 *
 * Ignores errors trying to open unreadable files, and reports other errors
 * non-fatally.
 */
#ifdef PG_FLUSH_DATA_WORKS

static int
pre_sync_fname(const char *fname, bool isdir)
{
	int			fd;

	fd = open(fname, O_RDONLY | PG_BINARY, 0);

	if (fd < 0)
	{
		if (errno == EACCES || (isdir && errno == EISDIR))
			return 0;
		pg_log_error("could not open file \"%s\": %m", fname);
		return -1;
	}

	/*
	 * We do what pg_flush_data() would do in the backend: prefer to use
	 * sync_file_range, but fall back to posix_fadvise.  We ignore errors
	 * because this is only a hint.
	 */
#if defined(HAVE_SYNC_FILE_RANGE)
	(void) sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
#elif defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
	(void) posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#else
#error PG_FLUSH_DATA_WORKS should not have been defined
#endif

	(void) close(fd);
	return 0;
}

#endif							/* PG_FLUSH_DATA_WORKS */

/*
 * fsync_fname -- Try to fsync a file or directory
 *
 * Ignores errors trying to open unreadable files, or trying to fsync
 * directories on systems where that isn't allowed/required.  All other errors
 * are fatal.
 */
int
fsync_fname(const char *fname, bool isdir)
{
	int			fd;
	int			flags;
	int			returncode;

	/*
	 * Some OSs require directories to be opened read-only whereas other
	 * systems don't allow us to fsync files opened read-only; so we need both
	 * cases here.  Using O_RDWR will cause us to fail to fsync files that are
	 * not writable by our userid, but we assume that's OK.
	 */
	flags = PG_BINARY;
	if (!isdir)
		flags |= O_RDWR;
	else
		flags |= O_RDONLY;

	/*
	 * Open the file, silently ignoring errors about unreadable files (or
	 * unsupported operations, e.g. opening a directory under Windows), and
	 * logging others.
	 */
	fd = open(fname, flags, 0);
	if (fd < 0)
	{
		if (errno == EACCES || (isdir && errno == EISDIR))
			return 0;
		pg_log_error("could not open file \"%s\": %m", fname);
		return -1;
	}

	returncode = fsync(fd);

	/*
	 * Some OSes don't allow us to fsync directories at all, so we can ignore
	 * those errors. Anything else needs to be reported.
	 */
	if (returncode != 0 && !(isdir && (errno == EBADF || errno == EINVAL)))
	{
		pg_log_fatal("could not fsync file \"%s\": %m", fname);
		(void) close(fd);
		exit(EXIT_FAILURE);
	}

	(void) close(fd);
	return 0;
}

/*
 * fsync_parent_path -- fsync the parent path of a file or directory
 *
 * This is aimed at making file operations persistent on disk in case of
 * an OS crash or power failure.
 */
int
fsync_parent_path(const char *fname)
{
	char		parentpath[MAXPGPATH];

	strlcpy(parentpath, fname, MAXPGPATH);
	get_parent_directory(parentpath);

	/*
	 * get_parent_directory() returns an empty string if the input argument is
	 * just a file name (see comments in path.c), so handle that as being the
	 * current directory.
	 */
	if (strlen(parentpath) == 0)
		strlcpy(parentpath, ".", MAXPGPATH);

	if (fsync_fname(parentpath, true) != 0)
		return -1;

	return 0;
}

/*
 * durable_rename -- rename(2) wrapper, issuing fsyncs required for durability
 *
 * Wrapper around rename, similar to the backend version.
 */
int
durable_rename(const char *oldfile, const char *newfile)
{
	int			fd;

	/*
	 * First fsync the old and target path (if it exists), to ensure that they
	 * are properly persistent on disk. Syncing the target file is not
	 * strictly necessary, but it makes it easier to reason about crashes;
	 * because it's then guaranteed that either source or target file exists
	 * after a crash.
	 */
	if (fsync_fname(oldfile, false) != 0)
		return -1;

	fd = open(newfile, PG_BINARY | O_RDWR, 0);
	if (fd < 0)
	{
		if (errno != ENOENT)
		{
			pg_log_error("could not open file \"%s\": %m", newfile);
			return -1;
		}
	}
	else
	{
		if (fsync(fd) != 0)
		{
			pg_log_fatal("could not fsync file \"%s\": %m", newfile);
			close(fd);
			exit(EXIT_FAILURE);
		}
		close(fd);
	}

	/* Time to do the real deal... */
	if (rename(oldfile, newfile) != 0)
	{
		pg_log_error("could not rename file \"%s\" to \"%s\": %m",
					 oldfile, newfile);
		return -1;
	}

	/*
	 * To guarantee renaming the file is persistent, fsync the file with its
	 * new name, and its containing directory.
	 */
	if (fsync_fname(newfile, false) != 0)
		return -1;

	if (fsync_parent_path(newfile) != 0)
		return -1;

	return 0;
}

#endif							/* FRONTEND */

/*
 * Return the type of a directory entry.
 *
 * In frontend code, elevel should be a level from logging.h; in backend code
 * it should be a level from elog.h.
 */
PGFileType
get_dirent_type(const char *path,
				const struct dirent *de,
				bool look_through_symlinks,
				int elevel)
{
	PGFileType	result;

	/*
	 * Some systems tell us the type directly in the dirent struct, but that's
	 * a BSD and Linux extension not required by POSIX.  Even when the
	 * interface is present, sometimes the type is unknown, depending on the
	 * filesystem.
	 */
#if defined(DT_REG) && defined(DT_DIR) && defined(DT_LNK)
	if (de->d_type == DT_REG)
		result = PGFILETYPE_REG;
	else if (de->d_type == DT_DIR)
		result = PGFILETYPE_DIR;
	else if (de->d_type == DT_LNK && !look_through_symlinks)
		result = PGFILETYPE_LNK;
	else
		result = PGFILETYPE_UNKNOWN;
#else
	result = PGFILETYPE_UNKNOWN;
#endif

	if (result == PGFILETYPE_UNKNOWN)
	{
		struct stat fst;
		int			sret;


		if (look_through_symlinks)
			sret = stat(path, &fst);
		else
			sret = lstat(path, &fst);

		if (sret < 0)
		{
			result = PGFILETYPE_ERROR;
#ifdef FRONTEND
			pg_log_generic(elevel, "could not stat file \"%s\": %m", path);
#else
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", path)));
#endif
		}
		else if (S_ISREG(fst.st_mode))
			result = PGFILETYPE_REG;
		else if (S_ISDIR(fst.st_mode))
			result = PGFILETYPE_DIR;
#ifdef S_ISLNK
		else if (S_ISLNK(fst.st_mode))
			result = PGFILETYPE_LNK;
#endif
	}

	return result;
}
