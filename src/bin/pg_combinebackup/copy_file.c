/*
 * Copy entire files.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "common/logging.h"
#include "copy_file.h"

static void copy_file_blocks(const char *src, const char *dst,
							 pg_checksum_context *checksum_ctx);

#ifdef WIN32
static void copy_file_copyfile(const char *src, const char *dst);
#endif

/*
 * Copy a regular file, optionally computing a checksum, and emitting
 * appropriate debug messages. But if we're in dry-run mode, then just emit
 * the messages and don't copy anything.
 */
void
copy_file(const char *src, const char *dst,
		  pg_checksum_context *checksum_ctx, bool dry_run)
{
	/*
	 * In dry-run mode, we don't actually copy anything, nor do we read any
	 * data from the source file, but we do verify that we can open it.
	 */
	if (dry_run)
	{
		int			fd;

		if ((fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
			pg_fatal("could not open \"%s\": %m", src);
		if (close(fd) < 0)
			pg_fatal("could not close \"%s\": %m", src);
	}

	/*
	 * If we don't need to compute a checksum, then we can use any special
	 * operating system primitives that we know about to copy the file; this
	 * may be quicker than a naive block copy.
	 */
	if (checksum_ctx->type == CHECKSUM_TYPE_NONE)
	{
		char	   *strategy_name = NULL;
		void		(*strategy_implementation) (const char *, const char *) = NULL;

#ifdef WIN32
		strategy_name = "CopyFile";
		strategy_implementation = copy_file_copyfile;
#endif

		if (strategy_name != NULL)
		{
			if (dry_run)
				pg_log_debug("would copy \"%s\" to \"%s\" using strategy %s",
							 src, dst, strategy_name);
			else
			{
				pg_log_debug("copying \"%s\" to \"%s\" using strategy %s",
							 src, dst, strategy_name);
				(*strategy_implementation) (src, dst);
			}
			return;
		}
	}

	/*
	 * Fall back to the simple approach of reading and writing all the blocks,
	 * feeding them into the checksum context as we go.
	 */
	if (dry_run)
	{
		if (checksum_ctx->type == CHECKSUM_TYPE_NONE)
			pg_log_debug("would copy \"%s\" to \"%s\"",
						 src, dst);
		else
			pg_log_debug("would copy \"%s\" to \"%s\" and checksum with %s",
						 src, dst, pg_checksum_type_name(checksum_ctx->type));
	}
	else
	{
		if (checksum_ctx->type == CHECKSUM_TYPE_NONE)
			pg_log_debug("copying \"%s\" to \"%s\"",
						 src, dst);
		else
			pg_log_debug("copying \"%s\" to \"%s\" and checksumming with %s",
						 src, dst, pg_checksum_type_name(checksum_ctx->type));
		copy_file_blocks(src, dst, checksum_ctx);
	}
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
				pg_fatal("could not write file \"%s\": %m", dst);
			else
				pg_fatal("could not write file \"%s\": wrote only %d of %d bytes at offset %u",
						 dst, (int) wb, (int) rb, offset);
		}

		if (pg_checksum_update(checksum_ctx, buffer, rb) < 0)
			pg_fatal("could not update checksum of file \"%s\"", dst);

		offset += rb;
	}

	if (rb < 0)
		pg_fatal("could not read file \"%s\": %m", dst);

	pg_free(buffer);
	close(src_fd);
	close(dest_fd);
}

#ifdef WIN32
static void
copy_file_copyfile(const char *src, const char *dst)
{
	if (CopyFile(src, dst, true) == 0)
	{
		_dosmaperr(GetLastError());
		pg_fatal("could not copy \"%s\" to \"%s\": %m", src, dst);
	}
}
#endif							/* WIN32 */
