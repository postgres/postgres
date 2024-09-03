/*-------------------------------------------------------------------------
 *
 * File-processing utility routines.
 *
 * Assorted utility functions to work on files.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
#include "common/relpath.h"
#include "port/pg_iovec.h"

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

#ifdef HAVE_SYNCFS

/*
 * do_syncfs -- Try to syncfs a file system
 *
 * Reports errors trying to open the path.  syncfs() errors are fatal.
 */
static void
do_syncfs(const char *path)
{
	int			fd;

	fd = open(path, O_RDONLY, 0);

	if (fd < 0)
	{
		pg_log_error("could not open file \"%s\": %m", path);
		return;
	}

	if (syncfs(fd) < 0)
	{
		pg_log_error("could not synchronize file system for file \"%s\": %m", path);
		(void) close(fd);
		exit(EXIT_FAILURE);
	}

	(void) close(fd);
}

#endif							/* HAVE_SYNCFS */

/*
 * Synchronize PGDATA and all its contents.
 *
 * We sync regular files and directories wherever they are, but we follow
 * symlinks only for pg_wal (or pg_xlog) and immediately under pg_tblspc.
 * Other symlinks are presumed to point at files we're not responsible for
 * syncing, and might not have privileges to write at all.
 *
 * serverVersion indicates the version of the server to be sync'd.
 */
void
sync_pgdata(const char *pg_data,
			int serverVersion,
			DataDirSyncMethod sync_method)
{
	bool		xlog_is_symlink;
	char		pg_wal[MAXPGPATH];
	char		pg_tblspc[MAXPGPATH];

	/* handle renaming of pg_xlog to pg_wal in post-10 clusters */
	snprintf(pg_wal, MAXPGPATH, "%s/%s", pg_data,
			 serverVersion < MINIMUM_VERSION_FOR_PG_WAL ? "pg_xlog" : "pg_wal");
	snprintf(pg_tblspc, MAXPGPATH, "%s/%s", pg_data, PG_TBLSPC_DIR);

	/*
	 * If pg_wal is a symlink, we'll need to recurse into it separately,
	 * because the first walkdir below will ignore it.
	 */
	xlog_is_symlink = false;

	{
		struct stat st;

		if (lstat(pg_wal, &st) < 0)
			pg_log_error("could not stat file \"%s\": %m", pg_wal);
		else if (S_ISLNK(st.st_mode))
			xlog_is_symlink = true;
	}

	switch (sync_method)
	{
		case DATA_DIR_SYNC_METHOD_SYNCFS:
			{
#ifndef HAVE_SYNCFS
				pg_log_error("this build does not support sync method \"%s\"",
							 "syncfs");
				exit(EXIT_FAILURE);
#else
				DIR		   *dir;
				struct dirent *de;

				/*
				 * On Linux, we don't have to open every single file one by
				 * one.  We can use syncfs() to sync whole filesystems.  We
				 * only expect filesystem boundaries to exist where we
				 * tolerate symlinks, namely pg_wal and the tablespaces, so we
				 * call syncfs() for each of those directories.
				 */

				/* Sync the top level pgdata directory. */
				do_syncfs(pg_data);

				/* If any tablespaces are configured, sync each of those. */
				dir = opendir(pg_tblspc);
				if (dir == NULL)
					pg_log_error("could not open directory \"%s\": %m",
								 pg_tblspc);
				else
				{
					while (errno = 0, (de = readdir(dir)) != NULL)
					{
						char		subpath[MAXPGPATH * 2];

						if (strcmp(de->d_name, ".") == 0 ||
							strcmp(de->d_name, "..") == 0)
							continue;

						snprintf(subpath, sizeof(subpath), "%s/%s",
								 pg_tblspc, de->d_name);
						do_syncfs(subpath);
					}

					if (errno)
						pg_log_error("could not read directory \"%s\": %m",
									 pg_tblspc);

					(void) closedir(dir);
				}

				/* If pg_wal is a symlink, process that too. */
				if (xlog_is_symlink)
					do_syncfs(pg_wal);
#endif							/* HAVE_SYNCFS */
			}
			break;

		case DATA_DIR_SYNC_METHOD_FSYNC:
			{
				/*
				 * If possible, hint to the kernel that we're soon going to
				 * fsync the data directory and its contents.
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
				 * The main call ignores symlinks, so in addition to specially
				 * processing pg_wal if it's a symlink, pg_tblspc has to be
				 * visited separately with process_symlinks = true.  Note that
				 * if there are any plain directories in pg_tblspc, they'll
				 * get fsync'd twice. That's not an expected case so we don't
				 * worry about optimizing it.
				 */
				walkdir(pg_data, fsync_fname, false);
				if (xlog_is_symlink)
					walkdir(pg_wal, fsync_fname, false);
				walkdir(pg_tblspc, fsync_fname, true);
			}
			break;
	}
}

/*
 * Synchronize the given directory and all its contents.
 *
 * This is a convenient wrapper on top of walkdir() and do_syncfs().
 */
void
sync_dir_recurse(const char *dir, DataDirSyncMethod sync_method)
{
	switch (sync_method)
	{
		case DATA_DIR_SYNC_METHOD_SYNCFS:
			{
#ifndef HAVE_SYNCFS
				pg_log_error("this build does not support sync method \"%s\"",
							 "syncfs");
				exit(EXIT_FAILURE);
#else
				/*
				 * On Linux, we don't have to open every single file one by
				 * one.  We can use syncfs() to sync the whole filesystem.
				 */
				do_syncfs(dir);
#endif							/* HAVE_SYNCFS */
			}
			break;

		case DATA_DIR_SYNC_METHOD_FSYNC:
			{
				/*
				 * If possible, hint to the kernel that we're soon going to
				 * fsync the data directory and its contents.
				 */
#ifdef PG_FLUSH_DATA_WORKS
				walkdir(dir, pre_sync_fname, false);
#endif

				walkdir(dir, fsync_fname, false);
			}
			break;
	}
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
		pg_log_error("could not fsync file \"%s\": %m", fname);
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
			pg_log_error("could not fsync file \"%s\": %m", newfile);
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
			pg_log_generic(elevel, PG_LOG_PRIMARY, "could not stat file \"%s\": %m", path);
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
		else if (S_ISLNK(fst.st_mode))
			result = PGFILETYPE_LNK;
	}

	return result;
}

/*
 * Compute what remains to be done after a possibly partial vectored read or
 * write.  The part of 'source' beginning after 'transferred' bytes is copied
 * to 'destination', and its length is returned.  'source' and 'destination'
 * may point to the same array, for in-place adjustment.  A return value of
 * zero indicates completion (for callers without a cheaper way to know that).
 */
int
compute_remaining_iovec(struct iovec *destination,
						const struct iovec *source,
						int iovcnt,
						size_t transferred)
{
	Assert(iovcnt > 0);

	/* Skip wholly transferred iovecs. */
	while (source->iov_len <= transferred)
	{
		transferred -= source->iov_len;
		source++;
		iovcnt--;

		/* All iovecs transferred? */
		if (iovcnt == 0)
		{
			/*
			 * We don't expect the kernel to transfer more than we asked it
			 * to, or something is out of sync.
			 */
			Assert(transferred == 0);
			return 0;
		}
	}

	/* Copy the remaining iovecs to the front of the array. */
	if (source != destination)
		memmove(destination, source, sizeof(*source) * iovcnt);

	/* Adjust leading iovec, which may have been partially transferred. */
	Assert(destination->iov_len > transferred);
	destination->iov_base = (char *) destination->iov_base + transferred;
	destination->iov_len -= transferred;

	return iovcnt;
}

/*
 * pg_pwritev_with_retry
 *
 * Convenience wrapper for pg_pwritev() that retries on partial write.  If an
 * error is returned, it is unspecified how much has been written.
 */
ssize_t
pg_pwritev_with_retry(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	struct iovec iov_copy[PG_IOV_MAX];
	ssize_t		sum = 0;
	ssize_t		part;

	/* We'd better have space to make a copy, in case we need to retry. */
	if (iovcnt > PG_IOV_MAX)
	{
		errno = EINVAL;
		return -1;
	}

	do
	{
		/* Write as much as we can. */
		part = pg_pwritev(fd, iov, iovcnt, offset);
		if (part < 0)
			return -1;

#ifdef SIMULATE_SHORT_WRITE
		part = Min(part, 4096);
#endif

		/* Count our progress. */
		sum += part;
		offset += part;

		/*
		 * See what is left.  On the first loop we used the caller's array,
		 * but in later loops we'll use our local copy that we are allowed to
		 * mutate.
		 */
		iovcnt = compute_remaining_iovec(iov_copy, iov, iovcnt, part);
		iov = iov_copy;
	} while (iovcnt > 0);

	return sum;
}

/*
 * pg_pwrite_zeros
 *
 * Writes zeros to file worth "size" bytes at "offset" (from the start of the
 * file), using vectored I/O.
 *
 * Returns the total amount of data written.  On failure, a negative value
 * is returned with errno set.
 */
ssize_t
pg_pwrite_zeros(int fd, size_t size, off_t offset)
{
	static const PGIOAlignedBlock zbuffer = {{0}};	/* worth BLCKSZ */
	void	   *zerobuf_addr = unconstify(PGIOAlignedBlock *, &zbuffer)->data;
	struct iovec iov[PG_IOV_MAX];
	size_t		remaining_size = size;
	ssize_t		total_written = 0;

	/* Loop, writing as many blocks as we can for each system call. */
	while (remaining_size > 0)
	{
		int			iovcnt = 0;
		ssize_t		written;

		for (; iovcnt < PG_IOV_MAX && remaining_size > 0; iovcnt++)
		{
			size_t		this_iov_size;

			iov[iovcnt].iov_base = zerobuf_addr;

			if (remaining_size < BLCKSZ)
				this_iov_size = remaining_size;
			else
				this_iov_size = BLCKSZ;

			iov[iovcnt].iov_len = this_iov_size;
			remaining_size -= this_iov_size;
		}

		written = pg_pwritev_with_retry(fd, iov, iovcnt, offset);

		if (written < 0)
			return written;

		offset += written;
		total_written += written;
	}

	Assert(total_written == size);

	return total_written;
}
