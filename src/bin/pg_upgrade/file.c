/*
 *	file.c
 *
 *	file system operations
 *
 *	Copyright (c) 2010-2020, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/file.c
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

#include "access/visibilitymapdefs.h"
#include "common/file_perm.h"
#include "pg_upgrade.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"


/*
 * cloneFile()
 *
 * Clones/reflinks a relation file from src to dst.
 *
 * schemaName/relName are relation's SQL name (used for error messages only).
 */
void
cloneFile(const char *src, const char *dst,
		  const char *schemaName, const char *relName)
{
#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)
	if (copyfile(src, dst, NULL, COPYFILE_CLONE_FORCE) < 0)
		pg_fatal("error while cloning relation \"%s.%s\" (\"%s\" to \"%s\"): %s\n",
				 schemaName, relName, src, dst, strerror(errno));
#elif defined(__linux__) && defined(FICLONE)
	int			src_fd;
	int			dest_fd;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("error while cloning relation \"%s.%s\": could not open file \"%s\": %s\n",
				 schemaName, relName, src, strerror(errno));

	if ((dest_fd = open(dst, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						pg_file_create_mode)) < 0)
		pg_fatal("error while cloning relation \"%s.%s\": could not create file \"%s\": %s\n",
				 schemaName, relName, dst, strerror(errno));

	if (ioctl(dest_fd, FICLONE, src_fd) < 0)
	{
		int			save_errno = errno;

		unlink(dst);
		pg_fatal("error while cloning relation \"%s.%s\" (\"%s\" to \"%s\"): %s\n",
				 schemaName, relName, src, dst, strerror(save_errno));
	}

	close(src_fd);
	close(dest_fd);
#endif
}


/*
 * copyFile()
 *
 * Copies a relation file from src to dst.
 * schemaName/relName are relation's SQL name (used for error messages only).
 */
void
copyFile(const char *src, const char *dst,
		 const char *schemaName, const char *relName)
{
#ifndef WIN32
	int			src_fd;
	int			dest_fd;
	char	   *buffer;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not open file \"%s\": %s\n",
				 schemaName, relName, src, strerror(errno));

	if ((dest_fd = open(dst, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						pg_file_create_mode)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not create file \"%s\": %s\n",
				 schemaName, relName, dst, strerror(errno));

	/* copy in fairly large chunks for best efficiency */
#define COPY_BUF_SIZE (50 * BLCKSZ)

	buffer = (char *) pg_malloc(COPY_BUF_SIZE);

	/* perform data copying i.e read src source, write to destination */
	while (true)
	{
		ssize_t		nbytes = read(src_fd, buffer, COPY_BUF_SIZE);

		if (nbytes < 0)
			pg_fatal("error while copying relation \"%s.%s\": could not read file \"%s\": %s\n",
					 schemaName, relName, src, strerror(errno));

		if (nbytes == 0)
			break;

		errno = 0;
		if (write(dest_fd, buffer, nbytes) != nbytes)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			pg_fatal("error while copying relation \"%s.%s\": could not write file \"%s\": %s\n",
					 schemaName, relName, dst, strerror(errno));
		}
	}

	pg_free(buffer);
	close(src_fd);
	close(dest_fd);

#else							/* WIN32 */

	if (CopyFile(src, dst, true) == 0)
	{
		_dosmaperr(GetLastError());
		pg_fatal("error while copying relation \"%s.%s\" (\"%s\" to \"%s\"): %s\n",
				 schemaName, relName, src, dst, strerror(errno));
	}

#endif							/* WIN32 */
}


/*
 * linkFile()
 *
 * Hard-links a relation file from src to dst.
 * schemaName/relName are relation's SQL name (used for error messages only).
 */
void
linkFile(const char *src, const char *dst,
		 const char *schemaName, const char *relName)
{
	if (link(src, dst) < 0)
		pg_fatal("error while creating link for relation \"%s.%s\" (\"%s\" to \"%s\"): %s\n",
				 schemaName, relName, src, dst, strerror(errno));
}


/*
 * rewriteVisibilityMap()
 *
 * Transform a visibility map file, copying from src to dst.
 * schemaName/relName are relation's SQL name (used for error messages only).
 *
 * In versions of PostgreSQL prior to catversion 201603011, PostgreSQL's
 * visibility map included one bit per heap page; it now includes two.
 * When upgrading a cluster from before that time to a current PostgreSQL
 * version, we could refuse to copy visibility maps from the old cluster
 * to the new cluster; the next VACUUM would recreate them, but at the
 * price of scanning the entire table.  So, instead, we rewrite the old
 * visibility maps in the new format.  That way, the all-visible bits
 * remain set for the pages for which they were set previously.  The
 * all-frozen bits are never set by this conversion; we leave that to VACUUM.
 */
void
rewriteVisibilityMap(const char *fromfile, const char *tofile,
					 const char *schemaName, const char *relName)
{
	int			src_fd;
	int			dst_fd;
	PGAlignedBlock buffer;
	PGAlignedBlock new_vmbuf;
	ssize_t		totalBytesRead = 0;
	ssize_t		src_filesize;
	int			rewriteVmBytesPerPage;
	BlockNumber new_blkno = 0;
	struct stat statbuf;

	/* Compute number of old-format bytes per new page */
	rewriteVmBytesPerPage = (BLCKSZ - SizeOfPageHeaderData) / 2;

	if ((src_fd = open(fromfile, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not open file \"%s\": %s\n",
				 schemaName, relName, fromfile, strerror(errno));

	if (fstat(src_fd, &statbuf) != 0)
		pg_fatal("error while copying relation \"%s.%s\": could not stat file \"%s\": %s\n",
				 schemaName, relName, fromfile, strerror(errno));

	if ((dst_fd = open(tofile, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
					   pg_file_create_mode)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not create file \"%s\": %s\n",
				 schemaName, relName, tofile, strerror(errno));

	/* Save old file size */
	src_filesize = statbuf.st_size;

	/*
	 * Turn each visibility map page into 2 pages one by one. Each new page
	 * has the same page header as the old one.  If the last section of the
	 * last page is empty, we skip it, mostly to avoid turning one-page
	 * visibility maps for small relations into two pages needlessly.
	 */
	while (totalBytesRead < src_filesize)
	{
		ssize_t		bytesRead;
		char	   *old_cur;
		char	   *old_break;
		char	   *old_blkend;
		PageHeaderData pageheader;
		bool		old_lastblk;

		if ((bytesRead = read(src_fd, buffer.data, BLCKSZ)) != BLCKSZ)
		{
			if (bytesRead < 0)
				pg_fatal("error while copying relation \"%s.%s\": could not read file \"%s\": %s\n",
						 schemaName, relName, fromfile, strerror(errno));
			else
				pg_fatal("error while copying relation \"%s.%s\": partial page found in file \"%s\"\n",
						 schemaName, relName, fromfile);
		}

		totalBytesRead += BLCKSZ;
		old_lastblk = (totalBytesRead == src_filesize);

		/* Save the page header data */
		memcpy(&pageheader, buffer.data, SizeOfPageHeaderData);

		/*
		 * These old_* variables point to old visibility map page. old_cur
		 * points to current position on old page. old_blkend points to end of
		 * old block.  old_break is the end+1 position on the old page for the
		 * data that will be transferred to the current new page.
		 */
		old_cur = buffer.data + SizeOfPageHeaderData;
		old_blkend = buffer.data + bytesRead;
		old_break = old_cur + rewriteVmBytesPerPage;

		while (old_break <= old_blkend)
		{
			char	   *new_cur;
			bool		empty = true;
			bool		old_lastpart;

			/* First, copy old page header to new page */
			memcpy(new_vmbuf.data, &pageheader, SizeOfPageHeaderData);

			/* Rewriting the last part of the last old page? */
			old_lastpart = old_lastblk && (old_break == old_blkend);

			new_cur = new_vmbuf.data + SizeOfPageHeaderData;

			/* Process old page bytes one by one, and turn it into new page. */
			while (old_cur < old_break)
			{
				uint8		byte = *(uint8 *) old_cur;
				uint16		new_vmbits = 0;
				int			i;

				/* Generate new format bits while keeping old information */
				for (i = 0; i < BITS_PER_BYTE; i++)
				{
					if (byte & (1 << i))
					{
						empty = false;
						new_vmbits |=
							VISIBILITYMAP_ALL_VISIBLE << (BITS_PER_HEAPBLOCK * i);
					}
				}

				/* Copy new visibility map bytes to new-format page */
				new_cur[0] = (char) (new_vmbits & 0xFF);
				new_cur[1] = (char) (new_vmbits >> 8);

				old_cur++;
				new_cur += BITS_PER_HEAPBLOCK;
			}

			/* If the last part of the last page is empty, skip writing it */
			if (old_lastpart && empty)
				break;

			/* Set new checksum for visibility map page, if enabled */
			if (new_cluster.controldata.data_checksum_version != 0)
				((PageHeader) new_vmbuf.data)->pd_checksum =
					pg_checksum_page(new_vmbuf.data, new_blkno);

			errno = 0;
			if (write(dst_fd, new_vmbuf.data, BLCKSZ) != BLCKSZ)
			{
				/* if write didn't set errno, assume problem is no disk space */
				if (errno == 0)
					errno = ENOSPC;
				pg_fatal("error while copying relation \"%s.%s\": could not write file \"%s\": %s\n",
						 schemaName, relName, tofile, strerror(errno));
			}

			/* Advance for next new page */
			old_break += rewriteVmBytesPerPage;
			new_blkno++;
		}
	}

	/* Clean up */
	close(dst_fd);
	close(src_fd);
}

void
check_file_clone(void)
{
	char		existing_file[MAXPGPATH];
	char		new_link_file[MAXPGPATH];

	snprintf(existing_file, sizeof(existing_file), "%s/PG_VERSION", old_cluster.pgdata);
	snprintf(new_link_file, sizeof(new_link_file), "%s/PG_VERSION.clonetest", new_cluster.pgdata);
	unlink(new_link_file);		/* might fail */

#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)
	if (copyfile(existing_file, new_link_file, NULL, COPYFILE_CLONE_FORCE) < 0)
		pg_fatal("could not clone file between old and new data directories: %s\n",
				 strerror(errno));
#elif defined(__linux__) && defined(FICLONE)
	{
		int			src_fd;
		int			dest_fd;

		if ((src_fd = open(existing_file, O_RDONLY | PG_BINARY, 0)) < 0)
			pg_fatal("could not open file \"%s\": %s\n",
					 existing_file, strerror(errno));

		if ((dest_fd = open(new_link_file, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
							pg_file_create_mode)) < 0)
			pg_fatal("could not create file \"%s\": %s\n",
					 new_link_file, strerror(errno));

		if (ioctl(dest_fd, FICLONE, src_fd) < 0)
			pg_fatal("could not clone file between old and new data directories: %s\n",
					 strerror(errno));

		close(src_fd);
		close(dest_fd);
	}
#else
	pg_fatal("file cloning not supported on this platform\n");
#endif

	unlink(new_link_file);
}

void
check_hard_link(void)
{
	char		existing_file[MAXPGPATH];
	char		new_link_file[MAXPGPATH];

	snprintf(existing_file, sizeof(existing_file), "%s/PG_VERSION", old_cluster.pgdata);
	snprintf(new_link_file, sizeof(new_link_file), "%s/PG_VERSION.linktest", new_cluster.pgdata);
	unlink(new_link_file);		/* might fail */

	if (link(existing_file, new_link_file) < 0)
		pg_fatal("could not create hard link between old and new data directories: %s\n"
				 "In link mode the old and new data directories must be on the same file system.\n",
				 strerror(errno));

	unlink(new_link_file);
}
