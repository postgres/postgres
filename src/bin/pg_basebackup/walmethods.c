/*-------------------------------------------------------------------------
 *
 * walmethods.c - implementations of different ways to write received wal
 *
 * NOTE! The caller must ensure that only one method is instantiated in
 *		 any given program, and that it's only instantiated once!
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/walmethods.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "common/file_perm.h"
#include "common/file_utils.h"
#include "pgtar.h"
#include "receivelog.h"
#include "streamutil.h"

/* Size of zlib buffer for .tar.gz */
#define ZLIB_OUT_SIZE 4096

/*-------------------------------------------------------------------------
 * WalDirectoryMethod - write wal to a directory looking like pg_wal
 *-------------------------------------------------------------------------
 */

/*
 * Global static data for this method
 */
typedef struct DirectoryMethodData
{
	char	   *basedir;
	int			compression;
	bool		sync;
} DirectoryMethodData;
static DirectoryMethodData *dir_data = NULL;

/*
 * Local file handle
 */
typedef struct DirectoryMethodFile
{
	int			fd;
	off_t		currpos;
	char	   *pathname;
	char	   *fullpath;
	char	   *temp_suffix;
#ifdef HAVE_LIBZ
	gzFile		gzfp;
#endif
} DirectoryMethodFile;

static const char *
dir_getlasterror(void)
{
	/* Directory method always sets errno, so just use strerror */
	return strerror(errno);
}

static Walfile
dir_open_for_write(const char *pathname, const char *temp_suffix, size_t pad_to_size)
{
	static char tmppath[MAXPGPATH];
	int			fd;
	DirectoryMethodFile *f;
#ifdef HAVE_LIBZ
	gzFile		gzfp = NULL;
#endif

	snprintf(tmppath, sizeof(tmppath), "%s/%s%s%s",
			 dir_data->basedir, pathname,
			 dir_data->compression > 0 ? ".gz" : "",
			 temp_suffix ? temp_suffix : "");

	/*
	 * Open a file for non-compressed as well as compressed files. Tracking
	 * the file descriptor is important for dir_sync() method as gzflush()
	 * does not do any system calls to fsync() to make changes permanent on
	 * disk.
	 */
	fd = open(tmppath, O_WRONLY | O_CREAT | PG_BINARY, pg_file_create_mode);
	if (fd < 0)
		return NULL;

#ifdef HAVE_LIBZ
	if (dir_data->compression > 0)
	{
		gzfp = gzdopen(fd, "wb");
		if (gzfp == NULL)
		{
			close(fd);
			return NULL;
		}

		if (gzsetparams(gzfp, dir_data->compression,
						Z_DEFAULT_STRATEGY) != Z_OK)
		{
			gzclose(gzfp);
			return NULL;
		}
	}
#endif

	/* Do pre-padding on non-compressed files */
	if (pad_to_size && dir_data->compression == 0)
	{
		PGAlignedXLogBlock zerobuf;
		int			bytes;

		memset(zerobuf.data, 0, XLOG_BLCKSZ);
		for (bytes = 0; bytes < pad_to_size; bytes += XLOG_BLCKSZ)
		{
			errno = 0;
			if (write(fd, zerobuf.data, XLOG_BLCKSZ) != XLOG_BLCKSZ)
			{
				int			save_errno = errno;

				close(fd);

				/*
				 * If write didn't set errno, assume problem is no disk space.
				 */
				errno = save_errno ? save_errno : ENOSPC;
				return NULL;
			}
		}

		if (lseek(fd, 0, SEEK_SET) != 0)
		{
			int			save_errno = errno;

			close(fd);
			errno = save_errno;
			return NULL;
		}
	}

	/*
	 * fsync WAL file and containing directory, to ensure the file is
	 * persistently created and zeroed (if padded). That's particularly
	 * important when using synchronous mode, where the file is modified and
	 * fsynced in-place, without a directory fsync.
	 */
	if (dir_data->sync)
	{
		if (fsync_fname(tmppath, false) != 0 ||
			fsync_parent_path(tmppath) != 0)
		{
#ifdef HAVE_LIBZ
			if (dir_data->compression > 0)
				gzclose(gzfp);
			else
#endif
				close(fd);
			return NULL;
		}
	}

	f = pg_malloc0(sizeof(DirectoryMethodFile));
#ifdef HAVE_LIBZ
	if (dir_data->compression > 0)
		f->gzfp = gzfp;
#endif
	f->fd = fd;
	f->currpos = 0;
	f->pathname = pg_strdup(pathname);
	f->fullpath = pg_strdup(tmppath);
	if (temp_suffix)
		f->temp_suffix = pg_strdup(temp_suffix);

	return f;
}

static ssize_t
dir_write(Walfile f, const void *buf, size_t count)
{
	ssize_t		r;
	DirectoryMethodFile *df = (DirectoryMethodFile *) f;

	Assert(f != NULL);

#ifdef HAVE_LIBZ
	if (dir_data->compression > 0)
		r = (ssize_t) gzwrite(df->gzfp, buf, count);
	else
#endif
		r = write(df->fd, buf, count);
	if (r > 0)
		df->currpos += r;
	return r;
}

static off_t
dir_get_current_pos(Walfile f)
{
	Assert(f != NULL);

	/* Use a cached value to prevent lots of reseeks */
	return ((DirectoryMethodFile *) f)->currpos;
}

static int
dir_close(Walfile f, WalCloseMethod method)
{
	int			r;
	DirectoryMethodFile *df = (DirectoryMethodFile *) f;
	static char tmppath[MAXPGPATH];
	static char tmppath2[MAXPGPATH];

	Assert(f != NULL);

#ifdef HAVE_LIBZ
	if (dir_data->compression > 0)
		r = gzclose(df->gzfp);
	else
#endif
		r = close(df->fd);

	if (r == 0)
	{
		/* Build path to the current version of the file */
		if (method == CLOSE_NORMAL && df->temp_suffix)
		{
			/*
			 * If we have a temp prefix, normal operation is to rename the
			 * file.
			 */
			snprintf(tmppath, sizeof(tmppath), "%s/%s%s%s",
					 dir_data->basedir, df->pathname,
					 dir_data->compression > 0 ? ".gz" : "",
					 df->temp_suffix);
			snprintf(tmppath2, sizeof(tmppath2), "%s/%s%s",
					 dir_data->basedir, df->pathname,
					 dir_data->compression > 0 ? ".gz" : "");
			r = durable_rename(tmppath, tmppath2);
		}
		else if (method == CLOSE_UNLINK)
		{
			/* Unlink the file once it's closed */
			snprintf(tmppath, sizeof(tmppath), "%s/%s%s%s",
					 dir_data->basedir, df->pathname,
					 dir_data->compression > 0 ? ".gz" : "",
					 df->temp_suffix ? df->temp_suffix : "");
			r = unlink(tmppath);
		}
		else
		{
			/*
			 * Else either CLOSE_NORMAL and no temp suffix, or
			 * CLOSE_NO_RENAME. In this case, fsync the file and containing
			 * directory if sync mode is requested.
			 */
			if (dir_data->sync)
			{
				r = fsync_fname(df->fullpath, false);
				if (r == 0)
					r = fsync_parent_path(df->fullpath);
			}
		}
	}

	pg_free(df->pathname);
	pg_free(df->fullpath);
	if (df->temp_suffix)
		pg_free(df->temp_suffix);
	pg_free(df);

	return r;
}

static int
dir_sync(Walfile f)
{
	Assert(f != NULL);

	if (!dir_data->sync)
		return 0;

#ifdef HAVE_LIBZ
	if (dir_data->compression > 0)
	{
		if (gzflush(((DirectoryMethodFile *) f)->gzfp, Z_SYNC_FLUSH) != Z_OK)
			return -1;
	}
#endif

	return fsync(((DirectoryMethodFile *) f)->fd);
}

static ssize_t
dir_get_file_size(const char *pathname)
{
	struct stat statbuf;
	static char tmppath[MAXPGPATH];

	snprintf(tmppath, sizeof(tmppath), "%s/%s",
			 dir_data->basedir, pathname);

	if (stat(tmppath, &statbuf) != 0)
		return -1;

	return statbuf.st_size;
}

static bool
dir_existsfile(const char *pathname)
{
	static char tmppath[MAXPGPATH];
	int			fd;

	snprintf(tmppath, sizeof(tmppath), "%s/%s",
			 dir_data->basedir, pathname);

	fd = open(tmppath, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
		return false;
	close(fd);
	return true;
}

static bool
dir_finish(void)
{
	if (dir_data->sync)
	{
		/*
		 * Files are fsynced when they are closed, but we need to fsync the
		 * directory entry here as well.
		 */
		if (fsync_fname(dir_data->basedir, true) != 0)
			return false;
	}
	return true;
}


WalWriteMethod *
CreateWalDirectoryMethod(const char *basedir, int compression, bool sync)
{
	WalWriteMethod *method;

	method = pg_malloc0(sizeof(WalWriteMethod));
	method->open_for_write = dir_open_for_write;
	method->write = dir_write;
	method->get_current_pos = dir_get_current_pos;
	method->get_file_size = dir_get_file_size;
	method->close = dir_close;
	method->sync = dir_sync;
	method->existsfile = dir_existsfile;
	method->finish = dir_finish;
	method->getlasterror = dir_getlasterror;

	dir_data = pg_malloc0(sizeof(DirectoryMethodData));
	dir_data->compression = compression;
	dir_data->basedir = pg_strdup(basedir);
	dir_data->sync = sync;

	return method;
}

void
FreeWalDirectoryMethod(void)
{
	pg_free(dir_data->basedir);
	pg_free(dir_data);
}


/*-------------------------------------------------------------------------
 * WalTarMethod - write wal to a tar file containing pg_wal contents
 *-------------------------------------------------------------------------
 */

typedef struct TarMethodFile
{
	off_t		ofs_start;		/* Where does the *header* for this file start */
	off_t		currpos;
	char		header[TAR_BLOCK_SIZE];
	char	   *pathname;
	size_t		pad_to_size;
} TarMethodFile;

typedef struct TarMethodData
{
	char	   *tarfilename;
	int			fd;
	int			compression;
	bool		sync;
	TarMethodFile *currentfile;
	char		lasterror[1024];
#ifdef HAVE_LIBZ
	z_streamp	zp;
	void	   *zlibOut;
#endif
} TarMethodData;
static TarMethodData *tar_data = NULL;

#define tar_clear_error() tar_data->lasterror[0] = '\0'
#define tar_set_error(msg) strlcpy(tar_data->lasterror, _(msg), sizeof(tar_data->lasterror))

static const char *
tar_getlasterror(void)
{
	/*
	 * If a custom error is set, return that one. Otherwise, assume errno is
	 * set and return that one.
	 */
	if (tar_data->lasterror[0])
		return tar_data->lasterror;
	return strerror(errno);
}

#ifdef HAVE_LIBZ
static bool
tar_write_compressed_data(void *buf, size_t count, bool flush)
{
	tar_data->zp->next_in = buf;
	tar_data->zp->avail_in = count;

	while (tar_data->zp->avail_in || flush)
	{
		int			r;

		r = deflate(tar_data->zp, flush ? Z_FINISH : Z_NO_FLUSH);
		if (r == Z_STREAM_ERROR)
		{
			tar_set_error("could not compress data");
			return false;
		}

		if (tar_data->zp->avail_out < ZLIB_OUT_SIZE)
		{
			size_t		len = ZLIB_OUT_SIZE - tar_data->zp->avail_out;

			errno = 0;
			if (write(tar_data->fd, tar_data->zlibOut, len) != len)
			{
				/*
				 * If write didn't set errno, assume problem is no disk space.
				 */
				if (errno == 0)
					errno = ENOSPC;
				return false;
			}

			tar_data->zp->next_out = tar_data->zlibOut;
			tar_data->zp->avail_out = ZLIB_OUT_SIZE;
		}

		if (r == Z_STREAM_END)
			break;
	}

	if (flush)
	{
		/* Reset the stream for writing */
		if (deflateReset(tar_data->zp) != Z_OK)
		{
			tar_set_error("could not reset compression stream");
			return false;
		}
	}

	return true;
}
#endif

static ssize_t
tar_write(Walfile f, const void *buf, size_t count)
{
	ssize_t		r;

	Assert(f != NULL);
	tar_clear_error();

	/* Tarfile will always be positioned at the end */
	if (!tar_data->compression)
	{
		r = write(tar_data->fd, buf, count);
		if (r > 0)
			((TarMethodFile *) f)->currpos += r;
		return r;
	}
#ifdef HAVE_LIBZ
	else
	{
		if (!tar_write_compressed_data(unconstify(void *, buf), count, false))
			return -1;
		((TarMethodFile *) f)->currpos += count;
		return count;
	}
#else
	else
		/* Can't happen - compression enabled with no libz */
		return -1;
#endif
}

static bool
tar_write_padding_data(TarMethodFile *f, size_t bytes)
{
	PGAlignedXLogBlock zerobuf;
	size_t		bytesleft = bytes;

	memset(zerobuf.data, 0, XLOG_BLCKSZ);
	while (bytesleft)
	{
		size_t		bytestowrite = Min(bytesleft, XLOG_BLCKSZ);
		ssize_t		r = tar_write(f, zerobuf.data, bytestowrite);

		if (r < 0)
			return false;
		bytesleft -= r;
	}

	return true;
}

static Walfile
tar_open_for_write(const char *pathname, const char *temp_suffix, size_t pad_to_size)
{
	int			save_errno;
	static char tmppath[MAXPGPATH];

	tar_clear_error();

	if (tar_data->fd < 0)
	{
		/*
		 * We open the tar file only when we first try to write to it.
		 */
		tar_data->fd = open(tar_data->tarfilename,
							O_WRONLY | O_CREAT | PG_BINARY,
							pg_file_create_mode);
		if (tar_data->fd < 0)
			return NULL;

#ifdef HAVE_LIBZ
		if (tar_data->compression)
		{
			tar_data->zp = (z_streamp) pg_malloc(sizeof(z_stream));
			tar_data->zp->zalloc = Z_NULL;
			tar_data->zp->zfree = Z_NULL;
			tar_data->zp->opaque = Z_NULL;
			tar_data->zp->next_out = tar_data->zlibOut;
			tar_data->zp->avail_out = ZLIB_OUT_SIZE;

			/*
			 * Initialize deflation library. Adding the magic value 16 to the
			 * default 15 for the windowBits parameter makes the output be
			 * gzip instead of zlib.
			 */
			if (deflateInit2(tar_data->zp, tar_data->compression, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
			{
				pg_free(tar_data->zp);
				tar_data->zp = NULL;
				tar_set_error("could not initialize compression library");
				return NULL;
			}
		}
#endif

		/* There's no tar header itself, the file starts with regular files */
	}

	Assert(tar_data->currentfile == NULL);
	if (tar_data->currentfile != NULL)
	{
		tar_set_error("implementation error: tar files can't have more than one open file");
		return NULL;
	}

	tar_data->currentfile = pg_malloc0(sizeof(TarMethodFile));

	snprintf(tmppath, sizeof(tmppath), "%s%s",
			 pathname, temp_suffix ? temp_suffix : "");

	/* Create a header with size set to 0 - we will fill out the size on close */
	if (tarCreateHeader(tar_data->currentfile->header, tmppath, NULL, 0, S_IRUSR | S_IWUSR, 0, 0, time(NULL)) != TAR_OK)
	{
		pg_free(tar_data->currentfile);
		tar_data->currentfile = NULL;
		tar_set_error("could not create tar header");
		return NULL;
	}

#ifdef HAVE_LIBZ
	if (tar_data->compression)
	{
		/* Flush existing data */
		if (!tar_write_compressed_data(NULL, 0, true))
			return NULL;

		/* Turn off compression for header */
		if (deflateParams(tar_data->zp, 0, 0) != Z_OK)
		{
			tar_set_error("could not change compression parameters");
			return NULL;
		}
	}
#endif

	tar_data->currentfile->ofs_start = lseek(tar_data->fd, 0, SEEK_CUR);
	if (tar_data->currentfile->ofs_start == -1)
	{
		save_errno = errno;
		pg_free(tar_data->currentfile);
		tar_data->currentfile = NULL;
		errno = save_errno;
		return NULL;
	}
	tar_data->currentfile->currpos = 0;

	if (!tar_data->compression)
	{
		errno = 0;
		if (write(tar_data->fd, tar_data->currentfile->header,
				  TAR_BLOCK_SIZE) != TAR_BLOCK_SIZE)
		{
			save_errno = errno;
			pg_free(tar_data->currentfile);
			tar_data->currentfile = NULL;
			/* if write didn't set errno, assume problem is no disk space */
			errno = save_errno ? save_errno : ENOSPC;
			return NULL;
		}
	}
#ifdef HAVE_LIBZ
	else
	{
		/* Write header through the zlib APIs but with no compression */
		if (!tar_write_compressed_data(tar_data->currentfile->header,
									   TAR_BLOCK_SIZE, true))
			return NULL;

		/* Re-enable compression for the rest of the file */
		if (deflateParams(tar_data->zp, tar_data->compression, 0) != Z_OK)
		{
			tar_set_error("could not change compression parameters");
			return NULL;
		}
	}
#endif

	tar_data->currentfile->pathname = pg_strdup(pathname);

	/*
	 * Uncompressed files are padded on creation, but for compression we can't
	 * do that
	 */
	if (pad_to_size)
	{
		tar_data->currentfile->pad_to_size = pad_to_size;
		if (!tar_data->compression)
		{
			/* Uncompressed, so pad now */
			tar_write_padding_data(tar_data->currentfile, pad_to_size);
			/* Seek back to start */
			if (lseek(tar_data->fd,
					  tar_data->currentfile->ofs_start + TAR_BLOCK_SIZE,
					  SEEK_SET) != tar_data->currentfile->ofs_start + TAR_BLOCK_SIZE)
				return NULL;

			tar_data->currentfile->currpos = 0;
		}
	}

	return tar_data->currentfile;
}

static ssize_t
tar_get_file_size(const char *pathname)
{
	tar_clear_error();

	/* Currently not used, so not supported */
	errno = ENOSYS;
	return -1;
}

static off_t
tar_get_current_pos(Walfile f)
{
	Assert(f != NULL);
	tar_clear_error();

	return ((TarMethodFile *) f)->currpos;
}

static int
tar_sync(Walfile f)
{
	Assert(f != NULL);
	tar_clear_error();

	if (!tar_data->sync)
		return 0;

	/*
	 * Always sync the whole tarfile, because that's all we can do. This makes
	 * no sense on compressed files, so just ignore those.
	 */
	if (tar_data->compression)
		return 0;

	return fsync(tar_data->fd);
}

static int
tar_close(Walfile f, WalCloseMethod method)
{
	ssize_t		filesize;
	int			padding;
	TarMethodFile *tf = (TarMethodFile *) f;

	Assert(f != NULL);
	tar_clear_error();

	if (method == CLOSE_UNLINK)
	{
		if (tar_data->compression)
		{
			tar_set_error("unlink not supported with compression");
			return -1;
		}

		/*
		 * Unlink the file that we just wrote to the tar. We do this by
		 * truncating it to the start of the header. This is safe as we only
		 * allow writing of the very last file.
		 */
		if (ftruncate(tar_data->fd, tf->ofs_start) != 0)
			return -1;

		pg_free(tf->pathname);
		pg_free(tf);
		tar_data->currentfile = NULL;

		return 0;
	}

	/*
	 * Pad the file itself with zeroes if necessary. Note that this is
	 * different from the tar format padding -- this is the padding we asked
	 * for when the file was opened.
	 */
	if (tf->pad_to_size)
	{
		if (tar_data->compression)
		{
			/*
			 * A compressed tarfile is padded on close since we cannot know
			 * the size of the compressed output until the end.
			 */
			size_t		sizeleft = tf->pad_to_size - tf->currpos;

			if (sizeleft)
			{
				if (!tar_write_padding_data(tf, sizeleft))
					return -1;
			}
		}
		else
		{
			/*
			 * An uncompressed tarfile was padded on creation, so just adjust
			 * the current position as if we seeked to the end.
			 */
			tf->currpos = tf->pad_to_size;
		}
	}

	/*
	 * Get the size of the file, and pad out to a multiple of the tar block
	 * size.
	 */
	filesize = tar_get_current_pos(f);
	padding = tarPaddingBytesRequired(filesize);
	if (padding)
	{
		char		zerobuf[TAR_BLOCK_SIZE];

		MemSet(zerobuf, 0, padding);
		if (tar_write(f, zerobuf, padding) != padding)
			return -1;
	}


#ifdef HAVE_LIBZ
	if (tar_data->compression)
	{
		/* Flush the current buffer */
		if (!tar_write_compressed_data(NULL, 0, true))
		{
			errno = EINVAL;
			return -1;
		}
	}
#endif

	/*
	 * Now go back and update the header with the correct filesize and
	 * possibly also renaming the file. We overwrite the entire current header
	 * when done, including the checksum.
	 */
	print_tar_number(&(tf->header[124]), 12, filesize);

	if (method == CLOSE_NORMAL)

		/*
		 * We overwrite it with what it was before if we have no tempname,
		 * since we're going to write the buffer anyway.
		 */
		strlcpy(&(tf->header[0]), tf->pathname, 100);

	print_tar_number(&(tf->header[148]), 8, tarChecksum(((TarMethodFile *) f)->header));
	if (lseek(tar_data->fd, tf->ofs_start, SEEK_SET) != ((TarMethodFile *) f)->ofs_start)
		return -1;
	if (!tar_data->compression)
	{
		errno = 0;
		if (write(tar_data->fd, tf->header, TAR_BLOCK_SIZE) != TAR_BLOCK_SIZE)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			return -1;
		}
	}
#ifdef HAVE_LIBZ
	else
	{
		/* Turn off compression */
		if (deflateParams(tar_data->zp, 0, 0) != Z_OK)
		{
			tar_set_error("could not change compression parameters");
			return -1;
		}

		/* Overwrite the header, assuming the size will be the same */
		if (!tar_write_compressed_data(tar_data->currentfile->header,
									   TAR_BLOCK_SIZE, true))
			return -1;

		/* Turn compression back on */
		if (deflateParams(tar_data->zp, tar_data->compression, 0) != Z_OK)
		{
			tar_set_error("could not change compression parameters");
			return -1;
		}
	}
#endif

	/* Move file pointer back down to end, so we can write the next file */
	if (lseek(tar_data->fd, 0, SEEK_END) < 0)
		return -1;

	/* Always fsync on close, so the padding gets fsynced */
	if (tar_sync(f) < 0)
		exit(1);

	/* Clean up and done */
	pg_free(tf->pathname);
	pg_free(tf);
	tar_data->currentfile = NULL;

	return 0;
}

static bool
tar_existsfile(const char *pathname)
{
	tar_clear_error();
	/* We only deal with new tarfiles, so nothing externally created exists */
	return false;
}

static bool
tar_finish(void)
{
	char		zerobuf[1024];

	tar_clear_error();

	if (tar_data->currentfile)
	{
		if (tar_close(tar_data->currentfile, CLOSE_NORMAL) != 0)
			return false;
	}

	/* A tarfile always ends with two empty blocks */
	MemSet(zerobuf, 0, sizeof(zerobuf));
	if (!tar_data->compression)
	{
		errno = 0;
		if (write(tar_data->fd, zerobuf, sizeof(zerobuf)) != sizeof(zerobuf))
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			return false;
		}
	}
#ifdef HAVE_LIBZ
	else
	{
		if (!tar_write_compressed_data(zerobuf, sizeof(zerobuf), false))
			return false;

		/* Also flush all data to make sure the gzip stream is finished */
		tar_data->zp->next_in = NULL;
		tar_data->zp->avail_in = 0;
		while (true)
		{
			int			r;

			r = deflate(tar_data->zp, Z_FINISH);

			if (r == Z_STREAM_ERROR)
			{
				tar_set_error("could not compress data");
				return false;
			}
			if (tar_data->zp->avail_out < ZLIB_OUT_SIZE)
			{
				size_t		len = ZLIB_OUT_SIZE - tar_data->zp->avail_out;

				errno = 0;
				if (write(tar_data->fd, tar_data->zlibOut, len) != len)
				{
					/*
					 * If write didn't set errno, assume problem is no disk
					 * space.
					 */
					if (errno == 0)
						errno = ENOSPC;
					return false;
				}
			}
			if (r == Z_STREAM_END)
				break;
		}

		if (deflateEnd(tar_data->zp) != Z_OK)
		{
			tar_set_error("could not close compression stream");
			return false;
		}
	}
#endif

	/* sync the empty blocks as well, since they're after the last file */
	if (tar_data->sync)
	{
		if (fsync(tar_data->fd) != 0)
			return false;
	}

	if (close(tar_data->fd) != 0)
		return false;

	tar_data->fd = -1;

	if (tar_data->sync)
	{
		if (fsync_fname(tar_data->tarfilename, false) != 0)
			return false;
		if (fsync_parent_path(tar_data->tarfilename) != 0)
			return false;
	}

	return true;
}

WalWriteMethod *
CreateWalTarMethod(const char *tarbase, int compression, bool sync)
{
	WalWriteMethod *method;
	const char *suffix = (compression != 0) ? ".tar.gz" : ".tar";

	method = pg_malloc0(sizeof(WalWriteMethod));
	method->open_for_write = tar_open_for_write;
	method->write = tar_write;
	method->get_current_pos = tar_get_current_pos;
	method->get_file_size = tar_get_file_size;
	method->close = tar_close;
	method->sync = tar_sync;
	method->existsfile = tar_existsfile;
	method->finish = tar_finish;
	method->getlasterror = tar_getlasterror;

	tar_data = pg_malloc0(sizeof(TarMethodData));
	tar_data->tarfilename = pg_malloc0(strlen(tarbase) + strlen(suffix) + 1);
	sprintf(tar_data->tarfilename, "%s%s", tarbase, suffix);
	tar_data->fd = -1;
	tar_data->compression = compression;
	tar_data->sync = sync;
#ifdef HAVE_LIBZ
	if (compression)
		tar_data->zlibOut = (char *) pg_malloc(ZLIB_OUT_SIZE + 1);
#endif

	return method;
}

void
FreeWalTarMethod(void)
{
	pg_free(tar_data->tarfilename);
#ifdef HAVE_LIBZ
	if (tar_data->compression)
		pg_free(tar_data->zlibOut);
#endif
	pg_free(tar_data);
}
