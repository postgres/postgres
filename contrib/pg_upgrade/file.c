/*
 *	file.c
 *
 *	file system operations
 *
 *	Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/file.c
 */

#include "postgres.h"

#include "pg_upgrade.h"

#include <fcntl.h>



#ifndef WIN32
static int	copy_file(const char *fromfile, const char *tofile, bool force);
#else
static int	win32_pghardlink(const char *src, const char *dst);
#endif


/*
 * copyAndUpdateFile()
 *
 *	Copies a relation file from src to dst.  If pageConverter is non-NULL, this function
 *	uses that pageConverter to do a page-by-page conversion.
 */
const char *
copyAndUpdateFile(pageCnvCtx *pageConverter,
				  const char *src, const char *dst, bool force)
{
	if (pageConverter == NULL)
	{
		if (pg_copy_file(src, dst, force) == -1)
			return getErrorText(errno);
		else
			return NULL;
	}
	else
	{
		/*
		 * We have a pageConverter object - that implies that the
		 * PageLayoutVersion differs between the two clusters so we have to
		 * perform a page-by-page conversion.
		 *
		 * If the pageConverter can convert the entire file at once, invoke
		 * that plugin function, otherwise, read each page in the relation
		 * file and call the convertPage plugin function.
		 */

#ifdef PAGE_CONVERSION
		if (pageConverter->convertFile)
			return pageConverter->convertFile(pageConverter->pluginData,
											  dst, src);
		else
#endif
		{
			int			src_fd;
			int			dstfd;
			char		buf[BLCKSZ];
			ssize_t		bytesRead;
			const char *msg = NULL;

			if ((src_fd = open(src, O_RDONLY, 0)) < 0)
				return "could not open source file";

			if ((dstfd = open(dst, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR)) < 0)
			{
				close(src_fd);
				return "could not create destination file";
			}

			while ((bytesRead = read(src_fd, buf, BLCKSZ)) == BLCKSZ)
			{
#ifdef PAGE_CONVERSION
				if ((msg = pageConverter->convertPage(pageConverter->pluginData, buf, buf)) != NULL)
					break;
#endif
				if (write(dstfd, buf, BLCKSZ) != BLCKSZ)
				{
					msg = "could not write new page to destination";
					break;
				}
			}

			close(src_fd);
			close(dstfd);

			if (msg)
				return msg;
			else if (bytesRead != 0)
				return "found partial page in source file";
			else
				return NULL;
		}
	}
}


/*
 * linkAndUpdateFile()
 *
 * Creates a symbolic link between the given relation files. We use
 * this function to perform a true in-place update. If the on-disk
 * format of the new cluster is bit-for-bit compatible with the on-disk
 * format of the old cluster, we can simply symlink each relation
 * instead of copying the data from the old cluster to the new cluster.
 */
const char *
linkAndUpdateFile(pageCnvCtx *pageConverter,
				  const char *src, const char *dst)
{
	if (pageConverter != NULL)
		return "Cannot in-place update this cluster, page-by-page conversion is required";

	if (pg_link_file(src, dst) == -1)
		return getErrorText(errno);
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

	if ((srcfile == NULL) || (dstfile == NULL))
		return -1;

	if ((src_fd = open(srcfile, O_RDONLY, 0)) < 0)
		return -1;

	if ((dest_fd = open(dstfile, O_RDWR | O_CREAT | (force ? 0 : O_EXCL), S_IRUSR | S_IWUSR)) < 0)
	{
		if (src_fd != 0)
			close(src_fd);

		return -1;
	}

	buffer = (char *) malloc(COPY_BUF_SIZE);

	if (buffer == NULL)
	{
		if (src_fd != 0)
			close(src_fd);

		if (dest_fd != 0)
			close(dest_fd);

		return -1;
	}

	/* perform data copying i.e read src source, write to destination */
	while (true)
	{
		ssize_t		nbytes = read(src_fd, buffer, COPY_BUF_SIZE);

		if (nbytes < 0)
		{
			int			save_errno = errno;

			if (buffer != NULL)
				free(buffer);

			if (src_fd != 0)
				close(src_fd);

			if (dest_fd != 0)
				close(dest_fd);

			errno = save_errno;
			return -1;
		}

		if (nbytes == 0)
			break;

		errno = 0;

		if (write(dest_fd, buffer, nbytes) != nbytes)
		{
			/* if write didn't set errno, assume problem is no disk space */
			int			save_errno = errno ? errno : ENOSPC;

			if (buffer != NULL)
				free(buffer);

			if (src_fd != 0)
				close(src_fd);

			if (dest_fd != 0)
				close(dest_fd);

			errno = save_errno;
			return -1;
		}
	}

	if (buffer != NULL)
		free(buffer);

	if (src_fd != 0)
		close(src_fd);

	if (dest_fd != 0)
		close(dest_fd);

	return 1;
}
#endif


/*
 * load_directory()
 *
 * Returns count of files that meet the selection criteria coded in
 * the function pointed to by selector.  Creates an array of pointers
 * to dirent structures.  Address of array returned in namelist.
 *
 * Note that the number of dirent structures needed is dynamically
 * allocated using realloc.  Realloc can be inefficient if invoked a
 * large number of times.
 */
int
load_directory(const char *dirname, struct dirent *** namelist)
{
	DIR		   *dirdesc;
	struct dirent *direntry;
	int			count = 0;
	int			name_num = 0;
	size_t		entrysize;

	if ((dirdesc = opendir(dirname)) == NULL)
		pg_log(PG_FATAL, "could not open directory \"%s\": %s\n", dirname, getErrorText(errno));

	*namelist = NULL;

	while ((direntry = readdir(dirdesc)) != NULL)
	{
		count++;

		*namelist = (struct dirent **) realloc((void *) (*namelist),
						(size_t) ((name_num + 1) * sizeof(struct dirent *)));

		if (*namelist == NULL)
		{
			closedir(dirdesc);
			return -1;
		}

		entrysize = sizeof(struct dirent) - sizeof(direntry->d_name) +
			strlen(direntry->d_name) + 1;

		(*namelist)[name_num] = (struct dirent *) malloc(entrysize);

		if ((*namelist)[name_num] == NULL)
		{
			closedir(dirdesc);
			return -1;
		}

		memcpy((*namelist)[name_num], direntry, entrysize);

		name_num++;
	}

	closedir(dirdesc);

	return count;
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
		pg_log(PG_FATAL,
			   "Could not create hard link between old and new data directories: %s\n"
			   "In link mode the old and new data directories must be on the same file system volume.\n",
			   getErrorText(errno));
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
