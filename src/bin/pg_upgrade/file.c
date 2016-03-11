/*
 *	file.c
 *
 *	file system operations
 *
 *	Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/file.c
 */

#include "postgres_fe.h"

#include "access/visibilitymap.h"
#include "pg_upgrade.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"

#include <sys/stat.h>
#include <fcntl.h>

#define BITS_PER_HEAPBLOCK_OLD 1


#ifndef WIN32
static int	copy_file(const char *fromfile, const char *tofile, bool force);
#else
static int	win32_pghardlink(const char *src, const char *dst);
#endif


/*
 * copyFile()
 *
 *	Copies a relation file from src to dst.
 */
const char *
copyFile(const char *src, const char *dst, bool force)
{
#ifndef WIN32
		if (copy_file(src, dst, force) == -1)
#else
		if (CopyFile(src, dst, !force) == 0)
#endif
			return getErrorText();
		else
			return NULL;
}


/*
 * linkFile()
 *
 * Creates a hard link between the given relation files. We use
 * this function to perform a true in-place update. If the on-disk
 * format of the new cluster is bit-for-bit compatible with the on-disk
 * format of the old cluster, we can simply link each relation
 * instead of copying the data from the old cluster to the new cluster.
 */
const char *
linkFile(const char *src, const char *dst)
{
	if (pg_link_file(src, dst) == -1)
		return getErrorText();
	else
		return NULL;
}


#ifndef WIN32
static int
copy_file(const char *srcfile, const char *dstfile, bool force)
{
#define COPY_BUF_SIZE (50 * BLCKSZ)

	int			src_fd;
	int			dest_fd;
	char	   *buffer;
	int			ret = 0;
	int			save_errno = 0;

	if ((srcfile == NULL) || (dstfile == NULL))
	{
		errno = EINVAL;
		return -1;
	}

	if ((src_fd = open(srcfile, O_RDONLY, 0)) < 0)
		return -1;

	if ((dest_fd = open(dstfile, O_RDWR | O_CREAT | (force ? 0 : O_EXCL), S_IRUSR | S_IWUSR)) < 0)
	{
		save_errno = errno;

		if (src_fd != 0)
			close(src_fd);

		errno = save_errno;
		return -1;
	}

	buffer = (char *) pg_malloc(COPY_BUF_SIZE);

	/* perform data copying i.e read src source, write to destination */
	while (true)
	{
		ssize_t		nbytes = read(src_fd, buffer, COPY_BUF_SIZE);

		if (nbytes < 0)
		{
			save_errno = errno;
			ret = -1;
			break;
		}

		if (nbytes == 0)
			break;

		errno = 0;

		if (write(dest_fd, buffer, nbytes) != nbytes)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			save_errno = errno;
			ret = -1;
			break;
		}
	}

	pg_free(buffer);

	if (src_fd != 0)
		close(src_fd);

	if (dest_fd != 0)
		close(dest_fd);

	if (save_errno != 0)
		errno = save_errno;

	return ret;
}
#endif


/*
 * rewriteVisibilityMap()
 *
 * In versions of PostgreSQL prior to catversion 201603011, PostgreSQL's
 * visibility map included one bit per heap page; it now includes two.
 * When upgrading a cluster from before that time to a current PostgreSQL
 * version, we could refuse to copy visibility maps from the old cluster
 * to the new cluster; the next VACUUM would recreate them, but at the
 * price of scanning the entire table.  So, instead, we rewrite the old
 * visibility maps in the new format.  That way, the all-visible bit
 * remains set for the pages for which it was set previously.  The
 * all-frozen bit is never set by this conversion; we leave that to
 * VACUUM.
 */
const char *
rewriteVisibilityMap(const char *fromfile, const char *tofile, bool force)
{
	int			src_fd = 0;
	int			dst_fd = 0;
	char		buffer[BLCKSZ];
	ssize_t		bytesRead;
	ssize_t		src_filesize;
	int			rewriteVmBytesPerPage;
	BlockNumber new_blkno = 0;
	struct stat statbuf;

	/* Compute we need how many old page bytes to rewrite a new page */
	rewriteVmBytesPerPage = (BLCKSZ - SizeOfPageHeaderData) / 2;

	if ((fromfile == NULL) || (tofile == NULL))
		return "Invalid old file or new file";

	if ((src_fd = open(fromfile, O_RDONLY, 0)) < 0)
		return getErrorText();

	if (fstat(src_fd, &statbuf) != 0)
	{
		close(src_fd);
		return getErrorText();
	}

	if ((dst_fd = open(tofile, O_RDWR | O_CREAT | (force ? 0 : O_EXCL), S_IRUSR | S_IWUSR)) < 0)
	{
		close(src_fd);
		return getErrorText();
	}

	/* Save old file size */
	src_filesize = statbuf.st_size;

	/*
	 * Turn each visibility map page into 2 pages one by one. Each new page
	 * has the same page header as the old one.  If the last section of last
	 * page is empty, we skip it, mostly to avoid turning one-page visibility
	 * maps for small relations into two pages needlessly.
	 */
	while ((bytesRead = read(src_fd, buffer, BLCKSZ)) == BLCKSZ)
	{
		char	   *old_cur;
		char	   *old_break;
		char	   *old_blkend;
		PageHeaderData pageheader;
		bool		old_lastblk = ((BLCKSZ * (new_blkno + 1)) == src_filesize);

		/* Save the page header data */
		memcpy(&pageheader, buffer, SizeOfPageHeaderData);

		/*
		 * These old_* variables point to old visibility map page. old_cur
		 * points to current position on old page. old_blkend points to end of
		 * old block. old_break points to old page break position for
		 * rewriting a new page. After wrote a new page, old_break proceeds
		 * rewriteVmBytesPerPage bytes.
		 */
		old_cur = buffer + SizeOfPageHeaderData;
		old_blkend = buffer + bytesRead;
		old_break = old_cur + rewriteVmBytesPerPage;

		while (old_blkend >= old_break)
		{
			char		new_vmbuf[BLCKSZ];
			char	   *new_cur = new_vmbuf;
			bool		empty = true;
			bool		old_lastpart;

			/* Copy page header in advance */
			memcpy(new_vmbuf, &pageheader, SizeOfPageHeaderData);

			/* Rewrite the last part of the old page? */
			old_lastpart = old_lastblk && (old_blkend == old_break);

			new_cur += SizeOfPageHeaderData;

			/* Process old page bytes one by one, and turn it into new page. */
			while (old_break > old_cur)
			{
				uint16		new_vmbits = 0;
				int			i;

				/* Generate new format bits while keeping old information */
				for (i = 0; i < BITS_PER_BYTE; i++)
				{
					uint8		byte = *(uint8 *) old_cur;

					if (byte & (1 << (BITS_PER_HEAPBLOCK_OLD * i)))
					{
						empty = false;
						new_vmbits |= 1 << (BITS_PER_HEAPBLOCK * i);
					}
				}

				/* Copy new visibility map bit to new format page */
				memcpy(new_cur, &new_vmbits, BITS_PER_HEAPBLOCK);

				old_cur += BITS_PER_HEAPBLOCK_OLD;
				new_cur += BITS_PER_HEAPBLOCK;
			}

			/* If the last part of the old page is empty, skip to write it */
			if (old_lastpart && empty)
				break;

			/* Set new checksum for a visibility map page (if enabled) */
			if (old_cluster.controldata.data_checksum_version != 0 &&
				new_cluster.controldata.data_checksum_version != 0)
				((PageHeader) new_vmbuf)->pd_checksum =
					pg_checksum_page(new_vmbuf, new_blkno);

			if (write(dst_fd, new_vmbuf, BLCKSZ) != BLCKSZ)
			{
				close(dst_fd);
				close(src_fd);
				return getErrorText();
			}

			old_break += rewriteVmBytesPerPage;
			new_blkno++;
		}
	}

	/* Close files */
	close(dst_fd);
	close(src_fd);

	return NULL;

}

void
check_hard_link(void)
{
	char		existing_file[MAXPGPATH];
	char		new_link_file[MAXPGPATH];

	snprintf(existing_file, sizeof(existing_file), "%s/PG_VERSION", old_cluster.pgdata);
	snprintf(new_link_file, sizeof(new_link_file), "%s/PG_VERSION.linktest", new_cluster.pgdata);
	unlink(new_link_file);		/* might fail */

	if (pg_link_file(existing_file, new_link_file) == -1)
	{
		pg_fatal("Could not create hard link between old and new data directories: %s\n"
				 "In link mode the old and new data directories must be on the same file system volume.\n",
				 getErrorText());
	}
	unlink(new_link_file);
}

#ifdef WIN32
static int
win32_pghardlink(const char *src, const char *dst)
{
	/*
	 * CreateHardLinkA returns zero for failure
	 * http://msdn.microsoft.com/en-us/library/aa363860(VS.85).aspx
	 */
	if (CreateHardLinkA(dst, src, NULL) == 0)
		return -1;
	else
		return 0;
}
#endif


/* fopen() file with no group/other permissions */
FILE *
fopen_priv(const char *path, const char *mode)
{
	mode_t		old_umask = umask(S_IRWXG | S_IRWXO);
	FILE	   *fp;

	fp = fopen(path, mode);
	umask(old_umask);

	return fp;
}
