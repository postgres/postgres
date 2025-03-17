/*
 * Copy entire files.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_combinebackup/copy_file.h
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "common/logging.h"
#include "copy_file.h"

static void copy_file_blocks(const char *src, const char *dst,
							 pg_checksum_context *checksum_ctx);

static void copy_file_clone(const char *src, const char *dest,
							pg_checksum_context *checksum_ctx);

static void copy_file_by_range(const char *src, const char *dest,
							   pg_checksum_context *checksum_ctx);

#ifdef WIN32
static void copy_file_copyfile(const char *src, const char *dst,
							   pg_checksum_context *checksum_ctx);
#endif

static void copy_file_link(const char *src, const char *dest,
						   pg_checksum_context *checksum_ctx);

/*
 * Copy a regular file, optionally computing a checksum, and emitting
 * appropriate debug messages. But if we're in dry-run mode, then just emit
 * the messages and don't copy anything.
 */
void
copy_file(const char *src, const char *dst,
		  pg_checksum_context *checksum_ctx,
		  CopyMethod copy_method, bool dry_run)
{
	char	   *strategy_name = NULL;
	void		(*strategy_implementation) (const char *, const char *,
											pg_checksum_context *checksum_ctx) = NULL;

	/*
	 * In dry-run mode, we don't actually copy anything, nor do we read any
	 * data from the source file, but we do verify that we can open it.
	 */
	if (dry_run)
	{
		int			fd;

		if ((fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
			pg_fatal("could not open file \"%s\": %m", src);
		if (close(fd) < 0)
			pg_fatal("could not close file \"%s\": %m", src);
	}

#ifdef WIN32

	/*
	 * We have no specific switch to enable CopyFile on Windows, because it's
	 * supported (as far as we know) on all Windows machines. So,
	 * automatically enable it unless some other strategy was selected.
	 */
	if (copy_method == COPY_METHOD_COPY)
		copy_method = COPY_METHOD_COPYFILE;
#endif

	/* Determine the name of the copy strategy for use in log messages. */
	switch (copy_method)
	{
		case COPY_METHOD_CLONE:
			strategy_name = "clone";
			strategy_implementation = copy_file_clone;
			break;
		case COPY_METHOD_COPY:
			/* leave NULL for simple block-by-block copy */
			strategy_implementation = copy_file_blocks;
			break;
		case COPY_METHOD_COPY_FILE_RANGE:
			strategy_name = "copy_file_range";
			strategy_implementation = copy_file_by_range;
			break;
#ifdef WIN32
		case COPY_METHOD_COPYFILE:
			strategy_name = "CopyFile";
			strategy_implementation = copy_file_copyfile;
			break;
#endif
		case COPY_METHOD_LINK:
			strategy_name = "link";
			strategy_implementation = copy_file_link;
			break;
	}

	if (dry_run)
	{
		if (strategy_name)
			pg_log_debug("would copy \"%s\" to \"%s\" using strategy %s",
						 src, dst, strategy_name);
		else
			pg_log_debug("would copy \"%s\" to \"%s\"",
						 src, dst);
	}
	else
	{
		if (strategy_name)
			pg_log_debug("copying \"%s\" to \"%s\" using strategy %s",
						 src, dst, strategy_name);
		else if (checksum_ctx->type == CHECKSUM_TYPE_NONE)
			pg_log_debug("copying \"%s\" to \"%s\"",
						 src, dst);
		else
			pg_log_debug("copying \"%s\" to \"%s\" and checksumming with %s",
						 src, dst, pg_checksum_type_name(checksum_ctx->type));

		strategy_implementation(src, dst, checksum_ctx);
	}
}

/*
 * Calculate checksum for the src file.
 */
static void
checksum_file(const char *src, pg_checksum_context *checksum_ctx)
{
	int			src_fd;
	uint8	   *buffer;
	const int	buffer_size = 50 * BLCKSZ;
	ssize_t		rb;

	/* bail out if no checksum needed */
	if (checksum_ctx->type == CHECKSUM_TYPE_NONE)
		return;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("could not open file \"%s\": %m", src);

	buffer = pg_malloc(buffer_size);

	while ((rb = read(src_fd, buffer, buffer_size)) > 0)
	{
		if (pg_checksum_update(checksum_ctx, buffer, rb) < 0)
			pg_fatal("could not update checksum of file \"%s\"", src);
	}

	if (rb < 0)
		pg_fatal("could not read file \"%s\": %m", src);

	pg_free(buffer);
	close(src_fd);
}

/*
 * Copy a file block by block, and optionally compute a checksum as we go.
 */
static void
copy_file_blocks(const char *src, const char *dst,
				 pg_checksum_context *checksum_ctx)
{
	int			src_fd;
	int			dest_fd;
	uint8	   *buffer;
	const int	buffer_size = 50 * BLCKSZ;
	ssize_t		rb;
	unsigned	offset = 0;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("could not open file \"%s\": %m", src);

	if ((dest_fd = open(dst, O_WRONLY | O_CREAT | O_EXCL | PG_BINARY,
						pg_file_create_mode)) < 0)
		pg_fatal("could not open file \"%s\": %m", dst);

	buffer = pg_malloc(buffer_size);

	while ((rb = read(src_fd, buffer, buffer_size)) > 0)
	{
		ssize_t		wb;

		if ((wb = write(dest_fd, buffer, rb)) != rb)
		{
			if (wb < 0)
				pg_fatal("could not write to file \"%s\": %m", dst);
			else
				pg_fatal("could not write to file \"%s\", offset %u: wrote %d of %d",
						 dst, offset, (int) wb, (int) rb);
		}

		if (pg_checksum_update(checksum_ctx, buffer, rb) < 0)
			pg_fatal("could not update checksum of file \"%s\"", dst);

		offset += rb;
	}

	if (rb < 0)
		pg_fatal("could not read from file \"%s\": %m", dst);

	pg_free(buffer);
	close(src_fd);
	close(dest_fd);
}

/*
 * copy_file_clone
 *		Clones/reflinks a file from src to dest.
 *
 * If needed, also reads the file and calculates the checksum.
 */
static void
copy_file_clone(const char *src, const char *dest,
				pg_checksum_context *checksum_ctx)
{
#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)
	if (copyfile(src, dest, NULL, COPYFILE_CLONE_FORCE) < 0)
		pg_fatal("error while cloning file \"%s\" to \"%s\": %m", src, dest);
#elif defined(__linux__) && defined(FICLONE)
	{
		int			src_fd;
		int			dest_fd;

		if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
			pg_fatal("could not open file \"%s\": %m", src);

		if ((dest_fd = open(dest, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
							pg_file_create_mode)) < 0)
			pg_fatal("could not create file \"%s\": %m", dest);

		if (ioctl(dest_fd, FICLONE, src_fd) < 0)
		{
			int			save_errno = errno;

			unlink(dest);

			pg_fatal("error while cloning file \"%s\" to \"%s\": %s",
					 src, dest, strerror(save_errno));
		}

		close(src_fd);
		close(dest_fd);
	}
#else
	pg_fatal("file cloning not supported on this platform");
#endif

	/* if needed, calculate checksum of the file */
	checksum_file(src, checksum_ctx);
}

/*
 * copy_file_by_range
 *		Copies a file from src to dest using copy_file_range system call.
 *
 * If needed, also reads the file and calculates the checksum.
 */
static void
copy_file_by_range(const char *src, const char *dest,
				   pg_checksum_context *checksum_ctx)
{
#if defined(HAVE_COPY_FILE_RANGE)
	int			src_fd;
	int			dest_fd;
	ssize_t		nbytes;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("could not open file \"%s\": %m", src);

	if ((dest_fd = open(dest, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						pg_file_create_mode)) < 0)
		pg_fatal("could not create file \"%s\": %m", dest);

	do
	{
		nbytes = copy_file_range(src_fd, NULL, dest_fd, NULL, SSIZE_MAX, 0);
		if (nbytes < 0)
			pg_fatal("error while copying file range from \"%s\" to \"%s\": %m",
					 src, dest);
	} while (nbytes > 0);

	close(src_fd);
	close(dest_fd);
#else
	pg_fatal("copy_file_range not supported on this platform");
#endif

	/* if needed, calculate checksum of the file */
	checksum_file(src, checksum_ctx);
}

#ifdef WIN32
static void
copy_file_copyfile(const char *src, const char *dst,
				   pg_checksum_context *checksum_ctx)
{
	if (CopyFile(src, dst, true) == 0)
	{
		_dosmaperr(GetLastError());
		pg_fatal("could not copy file \"%s\" to \"%s\": %m", src, dst);
	}

	/* if needed, calculate checksum of the file */
	checksum_file(src, checksum_ctx);
}
#endif							/* WIN32 */

/*
 * copy_file_link
 * 		Hard-links a file from src to dest.
 *
 * If needed, also reads the file and calculates the checksum.
 */
static void
copy_file_link(const char *src, const char *dest,
			   pg_checksum_context *checksum_ctx)
{
	if (link(src, dest) < 0)
		pg_fatal("error while linking file from \"%s\" to \"%s\": %m",
				 src, dest);

	/* if needed, calculate checksum of the file */
	checksum_file(src, checksum_ctx);
}
