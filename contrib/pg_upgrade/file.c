/*
 *	file.c
 *
 *	file system operations
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade/file.c,v 1.13.2.2 2010/07/13 20:15:51 momjian Exp $
 */

#include "pg_upgrade.h"

#include <fcntl.h>


static int	copy_file(const char *fromfile, const char *tofile, bool force);

#ifdef WIN32
static int	win32_pghardlink(const char *src, const char *dst);
#endif
#ifdef NOT_USED
static int	copy_dir(const char *from, const char *to, bool force);
#endif

#ifndef HAVE_SCANDIR
static int pg_scandir_internal(migratorContext *ctx, const char *dirname,
					struct dirent *** namelist,
					int (*selector) (const struct dirent *));
#endif


/*
 * copyAndUpdateFile()
 *
 *	Copies a relation file from src to dst.  If pageConverter is non-NULL, this function
 *	uses that pageConverter to do a page-by-page conversion.
 */
const char *
copyAndUpdateFile(migratorContext *ctx, pageCnvCtx *pageConverter,
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
				return "can't open source file";

			if ((dstfd = open(dst, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR)) < 0)
			{
				close(src_fd);
				return "can't create destination file";
			}

			while ((bytesRead = read(src_fd, buf, BLCKSZ)) == BLCKSZ)
			{
#ifdef PAGE_CONVERSION
				if ((msg = pageConverter->convertPage(pageConverter->pluginData, buf, buf)) != NULL)
					break;
#endif
				if (write(dstfd, buf, BLCKSZ) != BLCKSZ)
				{
					msg = "can't write new page to destination";
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
linkAndUpdateFile(migratorContext *ctx, pageCnvCtx *pageConverter,
				  const char *src, const char *dst)
{
	if (pageConverter != NULL)
		return "Can't in-place update this cluster, page-by-page conversion is required";

	if (pg_link_file(src, dst) == -1)
		return getErrorText(errno);
	else
		return NULL;
}


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
			int save_errno = errno;
			
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
			int save_errno = errno ? errno : ENOSPC;

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


/*
 * pg_scandir()
 *
 * Wrapper for portable scandir functionality
 */
int
pg_scandir(migratorContext *ctx, const char *dirname,
		   struct dirent *** namelist,
		   int (*selector) (const struct dirent *))
{
#ifndef HAVE_SCANDIR
	return pg_scandir_internal(ctx, dirname, namelist, selector);

	/*
	 * scandir() is originally from BSD 4.3, which had the third argument as
	 * non-const. Linux and other C libraries have updated it to use a const.
	 * http://unix.derkeiler.com/Mailing-Lists/FreeBSD/questions/2005-12/msg002
	 * 14.html
	 *
	 * Here we try to guess which libc's need const, and which don't. The net
	 * goal here is to try to suppress a compiler warning due to a prototype
	 * mismatch of const usage. Ideally we would do this via autoconf, but
	 * autoconf doesn't have a suitable builtin test and it seems overkill to
	 * add one just to avoid a warning.
	 */
#elif defined(__FreeBSD__) || defined(__bsdi__) || defined(__darwin__) || defined(__OpenBSD__)
	/* no const */
	return scandir(dirname, namelist, (int (*) (struct dirent *)) selector, NULL);
#else
	/* use const */
	return scandir(dirname, namelist, selector, NULL);
#endif
}


#ifndef HAVE_SCANDIR
/*
 * pg_scandir_internal()
 *
 * Implement our own scandir() on platforms that don't have it.
 *
 * Returns count of files that meet the selection criteria coded in
 * the function pointed to by selector.  Creates an array of pointers
 * to dirent structures.  Address of array returned in namelist.
 *
 * Note that the number of dirent structures needed is dynamically
 * allocated using realloc.  Realloc can be inefficient if invoked a
 * large number of times.  Its use in pg_upgrade is to find filesystem
 * filenames that have extended beyond the initial segment (file.1,
 * .2, etc.) and should therefore be invoked a small number of times.
 */
static int
pg_scandir_internal(migratorContext *ctx, const char *dirname,
		 struct dirent *** namelist, int (*selector) (const struct dirent *))
{
	DIR		   *dirdesc;
	struct dirent *direntry;
	int			count = 0;
	int			name_num = 0;
	size_t		entrysize;

	if ((dirdesc = opendir(dirname)) == NULL)
		pg_log(ctx, PG_FATAL, "Could not open directory \"%s\": %s\n", dirname, getErrorText(errno));

	*namelist = NULL;

	while (errno = 0, (direntry = readdir(dirdesc)) != NULL)
	{
		/* Invoke the selector function to see if the direntry matches */
		if ((*selector) (direntry))
		{
			count++;

			*namelist = (struct dirent **) realloc((void *) (*namelist),
						(size_t) ((name_num + 1) * sizeof(struct dirent *)));

			if (*namelist == NULL)
				return -1;

			entrysize = sizeof(struct dirent) - sizeof(direntry->d_name) +
				strlen(direntry->d_name) + 1;

			(*namelist)[name_num] = (struct dirent *) malloc(entrysize);

			if ((*namelist)[name_num] == NULL)
				return -1;

			memcpy((*namelist)[name_num], direntry, entrysize);

			name_num++;
		}
	}

#ifdef WIN32
	/* Bug in old Mingw dirent.c;  fixed in mingw-runtime-3.2, 2003-10-10 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif

	if (errno)
		pg_log(ctx, PG_FATAL, "Could not read directory \"%s\": %s\n", dirname, getErrorText(errno));

	if (closedir(dirdesc))
		pg_log(ctx, PG_FATAL, "Could not close directory \"%s\": %s\n", dirname, getErrorText(errno));

	return count;
}
#endif


/*
 *	dir_matching_filenames
 *
 *	Return only matching file names during directory scan
 */
int
dir_matching_filenames(const struct dirent * scan_ent)
{
	/* we only compare for string length because the number suffix varies */
	if (!strncmp(scandir_file_pattern, scan_ent->d_name, strlen(scandir_file_pattern)))
		return 1;

	return 0;
}


void
check_hard_link(migratorContext *ctx)
{
	char		existing_file[MAXPGPATH];
	char		new_link_file[MAXPGPATH];

	snprintf(existing_file, sizeof(existing_file), "%s/PG_VERSION", ctx->old.pgdata);
	snprintf(new_link_file, sizeof(new_link_file), "%s/PG_VERSION.linktest", ctx->new.pgdata);
	unlink(new_link_file);		/* might fail */

	if (pg_link_file(existing_file, new_link_file) == -1)
	{
		pg_log(ctx, PG_FATAL,
			   "Could not create hard link between old and new data directories:  %s\n"
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


#ifdef NOT_USED
/*
 * copy_dir()
 *
 *	Copies either a directory or a single file within a directory.	If the
 *	source argument names a directory, we recursively copy that directory,
 *	otherwise we copy a single file.
 */
static int
copy_dir(const char *src, const char *dst, bool force)
{
	DIR		   *srcdir;
	struct dirent *de = NULL;
	struct stat fst;

	if (src == NULL || dst == NULL)
		return -1;

	/*
	 * Try to open the source directory - if it turns out not to be a
	 * directory, assume that it's a file and copy that instead.
	 */
	if ((srcdir = opendir(src)) == NULL)
	{
		if (errno == ENOTDIR)
			return copy_file(src, dst, true);
		return -1;
	}

	if (mkdir(dst, S_IRWXU) != 0)
	{
		/*
		 * ignore directory already exist error
		 */
		if (errno != EEXIST)
			return -1;
	}

	while (errno = 0, (de = readdir(srcdir)) != NULL)
	{
		char		src_file[MAXPGPATH];
		char		dest_file[MAXPGPATH];

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		memset(src_file, 0, sizeof(src_file));
		memset(dest_file, 0, sizeof(dest_file));

		snprintf(src_file, sizeof(src_file), "%s/%s", src, de->d_name);
		snprintf(dest_file, sizeof(dest_file), "%s/%s", dst, de->d_name);

		if (stat(src_file, &fst) < 0)
		{
			if (srcdir != NULL)
			{
				closedir(srcdir);
				srcdir = NULL;
			}

			return -1;
		}

		if (fst.st_mode & S_IFDIR)
		{
			/* recurse to handle subdirectories */
			if (force)
				copy_dir(src_file, dest_file, true);
		}
		else if (fst.st_mode & S_IFREG)
		{
			if ((copy_file(src_file, dest_file, 1)) == -1)
			{
				if (srcdir != NULL)
				{
					closedir(srcdir);
					srcdir = NULL;
				}
				return -1;
			}
		}
	}

#ifdef WIN32
	/* Bug in old Mingw dirent.c;  fixed in mingw-runtime-3.2, 2003-10-10 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif

	if (errno)
		return -1;

	if (srcdir != NULL)
	{
		if (closedir(srcdir))
			return -1;
		srcdir = NULL;
	}
	return 1;
}

#endif
