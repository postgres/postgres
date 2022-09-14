/*-------------------------------------------------------------------------
 *
 * walmethods.c - implementations of different ways to write received wal
 *
 * NOTE! The caller must ensure that only one method is instantiated in
 *		 any given program, and that it's only instantiated once!
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/walmethods.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef USE_LZ4
#include <lz4frame.h>
#endif
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "pgtar.h"
#include "receivelog.h"
#include "streamutil.h"

/* Size of zlib buffer for .tar.gz */
#define ZLIB_OUT_SIZE 4096

/* Size of LZ4 input chunk for .lz4 */
#define LZ4_IN_SIZE  4096

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
	pg_compress_algorithm compression_algorithm;
	int			compression_level;
	bool		sync;
	const char *lasterrstring;	/* if set, takes precedence over lasterrno */
	int			lasterrno;
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
#ifdef USE_LZ4
	LZ4F_compressionContext_t ctx;
	size_t		lz4bufsize;
	void	   *lz4buf;
#endif
} DirectoryMethodFile;

#define dir_clear_error() \
	(dir_data->lasterrstring = NULL, dir_data->lasterrno = 0)
#define dir_set_error(msg) \
	(dir_data->lasterrstring = _(msg))

static const char *
dir_getlasterror(void)
{
	if (dir_data->lasterrstring)
		return dir_data->lasterrstring;
	return strerror(dir_data->lasterrno);
}

static char *
dir_get_file_name(const char *pathname, const char *temp_suffix)
{
	char	   *filename = pg_malloc0(MAXPGPATH * sizeof(char));

	snprintf(filename, MAXPGPATH, "%s%s%s",
			 pathname,
			 dir_data->compression_algorithm == PG_COMPRESSION_GZIP ? ".gz" :
			 dir_data->compression_algorithm == PG_COMPRESSION_LZ4 ? ".lz4" : "",
			 temp_suffix ? temp_suffix : "");

	return filename;
}

static Walfile
dir_open_for_write(const char *pathname, const char *temp_suffix, size_t pad_to_size)
{
	char		tmppath[MAXPGPATH];
	char	   *filename;
	int			fd;
	DirectoryMethodFile *f;
#ifdef HAVE_LIBZ
	gzFile		gzfp = NULL;
#endif
#ifdef USE_LZ4
	LZ4F_compressionContext_t ctx = NULL;
	size_t		lz4bufsize = 0;
	void	   *lz4buf = NULL;
#endif

	dir_clear_error();

	filename = dir_get_file_name(pathname, temp_suffix);
	snprintf(tmppath, sizeof(tmppath), "%s/%s",
			 dir_data->basedir, filename);
	pg_free(filename);

	/*
	 * Open a file for non-compressed as well as compressed files. Tracking
	 * the file descriptor is important for dir_sync() method as gzflush()
	 * does not do any system calls to fsync() to make changes permanent on
	 * disk.
	 */
	fd = open(tmppath, O_WRONLY | O_CREAT | PG_BINARY, pg_file_create_mode);
	if (fd < 0)
	{
		dir_data->lasterrno = errno;
		return NULL;
	}

#ifdef HAVE_LIBZ
	if (dir_data->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		gzfp = gzdopen(fd, "wb");
		if (gzfp == NULL)
		{
			dir_data->lasterrno = errno;
			close(fd);
			return NULL;
		}

		if (gzsetparams(gzfp, dir_data->compression_level,
						Z_DEFAULT_STRATEGY) != Z_OK)
		{
			dir_data->lasterrno = errno;
			gzclose(gzfp);
			return NULL;
		}
	}
#endif
#ifdef USE_LZ4
	if (dir_data->compression_algorithm == PG_COMPRESSION_LZ4)
	{
		size_t		ctx_out;
		size_t		header_size;
		LZ4F_preferences_t prefs;

		ctx_out = LZ4F_createCompressionContext(&ctx, LZ4F_VERSION);
		if (LZ4F_isError(ctx_out))
		{
			dir_data->lasterrstring = LZ4F_getErrorName(ctx_out);
			close(fd);
			return NULL;
		}

		lz4bufsize = LZ4F_compressBound(LZ4_IN_SIZE, NULL);
		lz4buf = pg_malloc0(lz4bufsize);

		/* assign the compression level, default is 0 */
		memset(&prefs, 0, sizeof(prefs));
		prefs.compressionLevel = dir_data->compression_level;

		/* add the header */
		header_size = LZ4F_compressBegin(ctx, lz4buf, lz4bufsize, &prefs);
		if (LZ4F_isError(header_size))
		{
			dir_data->lasterrstring = LZ4F_getErrorName(header_size);
			(void) LZ4F_freeCompressionContext(ctx);
			pg_free(lz4buf);
			close(fd);
			return NULL;
		}

		errno = 0;
		if (write(fd, lz4buf, header_size) != header_size)
		{
			/* If write didn't set errno, assume problem is no disk space */
			dir_data->lasterrno = errno ? errno : ENOSPC;
			(void) LZ4F_freeCompressionContext(ctx);
			pg_free(lz4buf);
			close(fd);
			return NULL;
		}
	}
#endif

	/* Do pre-padding on non-compressed files */
	if (pad_to_size && dir_data->compression_algorithm == PG_COMPRESSION_NONE)
	{
		PGAlignedXLogBlock zerobuf;
		int			bytes;

		memset(zerobuf.data, 0, XLOG_BLCKSZ);
		for (bytes = 0; bytes < pad_to_size; bytes += XLOG_BLCKSZ)
		{
			errno = 0;
			if (write(fd, zerobuf.data, XLOG_BLCKSZ) != XLOG_BLCKSZ)
			{
				/* If write didn't set errno, assume problem is no disk space */
				dir_data->lasterrno = errno ? errno : ENOSPC;
				close(fd);
				return NULL;
			}
		}

		if (lseek(fd, 0, SEEK_SET) != 0)
		{
			dir_data->lasterrno = errno;
			close(fd);
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
			dir_data->lasterrno = errno;
#ifdef HAVE_LIBZ
			if (dir_data->compression_algorithm == PG_COMPRESSION_GZIP)
				gzclose(gzfp);
			else
#endif
#ifdef USE_LZ4
			if (dir_data->compression_algorithm == PG_COMPRESSION_LZ4)
			{
				(void) LZ4F_compressEnd(ctx, lz4buf, lz4bufsize, NULL);
				(void) LZ4F_freeCompressionContext(ctx);
				pg_free(lz4buf);
				close(fd);
			}
			else
#endif
				close(fd);
			return NULL;
		}
	}

	f = pg_malloc0(sizeof(DirectoryMethodFile));
#ifdef HAVE_LIBZ
	if (dir_data->compression_algorithm == PG_COMPRESSION_GZIP)
		f->gzfp = gzfp;
#endif
#ifdef USE_LZ4
	if (dir_data->compression_algorithm == PG_COMPRESSION_LZ4)
	{
		f->ctx = ctx;
		f->lz4buf = lz4buf;
		f->lz4bufsize = lz4bufsize;
	}
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
	dir_clear_error();

#ifdef HAVE_LIBZ
	if (dir_data->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		errno = 0;
		r = (ssize_t) gzwrite(df->gzfp, buf, count);
		if (r != count)
		{
			/* If write didn't set errno, assume problem is no disk space */
			dir_data->lasterrno = errno ? errno : ENOSPC;
		}
	}
	else
#endif
#ifdef USE_LZ4
	if (dir_data->compression_algorithm == PG_COMPRESSION_LZ4)
	{
		size_t		chunk;
		size_t		remaining;
		const void *inbuf = buf;

		remaining = count;
		while (remaining > 0)
		{
			size_t		compressed;

			if (remaining > LZ4_IN_SIZE)
				chunk = LZ4_IN_SIZE;
			else
				chunk = remaining;

			remaining -= chunk;
			compressed = LZ4F_compressUpdate(df->ctx,
											 df->lz4buf, df->lz4bufsize,
											 inbuf, chunk,
											 NULL);

			if (LZ4F_isError(compressed))
			{
				dir_data->lasterrstring = LZ4F_getErrorName(compressed);
				return -1;
			}

			errno = 0;
			if (write(df->fd, df->lz4buf, compressed) != compressed)
			{
				/* If write didn't set errno, assume problem is no disk space */
				dir_data->lasterrno = errno ? errno : ENOSPC;
				return -1;
			}

			inbuf = ((char *) inbuf) + chunk;
		}

		/* Our caller keeps track of the uncompressed size. */
		r = (ssize_t) count;
	}
	else
#endif
	{
		errno = 0;
		r = write(df->fd, buf, count);
		if (r != count)
		{
			/* If write didn't set errno, assume problem is no disk space */
			dir_data->lasterrno = errno ? errno : ENOSPC;
		}
	}
	if (r > 0)
		df->currpos += r;
	return r;
}

static off_t
dir_get_current_pos(Walfile f)
{
	Assert(f != NULL);
	dir_clear_error();

	/* Use a cached value to prevent lots of reseeks */
	return ((DirectoryMethodFile *) f)->currpos;
}

static int
dir_close(Walfile f, WalCloseMethod method)
{
	int			r;
	DirectoryMethodFile *df = (DirectoryMethodFile *) f;
	char		tmppath[MAXPGPATH];
	char		tmppath2[MAXPGPATH];

	Assert(f != NULL);
	dir_clear_error();

#ifdef HAVE_LIBZ
	if (dir_data->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		errno = 0;				/* in case gzclose() doesn't set it */
		r = gzclose(df->gzfp);
	}
	else
#endif
#ifdef USE_LZ4
	if (dir_data->compression_algorithm == PG_COMPRESSION_LZ4)
	{
		size_t		compressed;

		compressed = LZ4F_compressEnd(df->ctx,
									  df->lz4buf, df->lz4bufsize,
									  NULL);

		if (LZ4F_isError(compressed))
		{
			dir_data->lasterrstring = LZ4F_getErrorName(compressed);
			return -1;
		}

		errno = 0;
		if (write(df->fd, df->lz4buf, compressed) != compressed)
		{
			/* If write didn't set errno, assume problem is no disk space */
			dir_data->lasterrno = errno ? errno : ENOSPC;
			return -1;
		}

		r = close(df->fd);
	}
	else
#endif
		r = close(df->fd);

	if (r == 0)
	{
		/* Build path to the current version of the file */
		if (method == CLOSE_NORMAL && df->temp_suffix)
		{
			char	   *filename;
			char	   *filename2;

			/*
			 * If we have a temp prefix, normal operation is to rename the
			 * file.
			 */
			filename = dir_get_file_name(df->pathname, df->temp_suffix);
			snprintf(tmppath, sizeof(tmppath), "%s/%s",
					 dir_data->basedir, filename);
			pg_free(filename);

			/* permanent name, so no need for the prefix */
			filename2 = dir_get_file_name(df->pathname, NULL);
			snprintf(tmppath2, sizeof(tmppath2), "%s/%s",
					 dir_data->basedir, filename2);
			pg_free(filename2);
			if (dir_data->sync)
				r = durable_rename(tmppath, tmppath2);
			else
			{
				if (rename(tmppath, tmppath2) != 0)
				{
					pg_log_error("could not rename file \"%s\" to \"%s\": %m",
								 tmppath, tmppath2);
					r = -1;
				}
			}
		}
		else if (method == CLOSE_UNLINK)
		{
			char	   *filename;

			/* Unlink the file once it's closed */
			filename = dir_get_file_name(df->pathname, df->temp_suffix);
			snprintf(tmppath, sizeof(tmppath), "%s/%s",
					 dir_data->basedir, filename);
			pg_free(filename);
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

	if (r != 0)
		dir_data->lasterrno = errno;

#ifdef USE_LZ4
	pg_free(df->lz4buf);
	/* supports free on NULL */
	LZ4F_freeCompressionContext(df->ctx);
#endif

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
	int			r;

	Assert(f != NULL);
	dir_clear_error();

	if (!dir_data->sync)
		return 0;

#ifdef HAVE_LIBZ
	if (dir_data->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		if (gzflush(((DirectoryMethodFile *) f)->gzfp, Z_SYNC_FLUSH) != Z_OK)
		{
			dir_data->lasterrno = errno;
			return -1;
		}
	}
#endif
#ifdef USE_LZ4
	if (dir_data->compression_algorithm == PG_COMPRESSION_LZ4)
	{
		DirectoryMethodFile *df = (DirectoryMethodFile *) f;
		size_t		compressed;

		/* Flush any internal buffers */
		compressed = LZ4F_flush(df->ctx, df->lz4buf, df->lz4bufsize, NULL);
		if (LZ4F_isError(compressed))
		{
			dir_data->lasterrstring = LZ4F_getErrorName(compressed);
			return -1;
		}

		errno = 0;
		if (write(df->fd, df->lz4buf, compressed) != compressed)
		{
			/* If write didn't set errno, assume problem is no disk space */
			dir_data->lasterrno = errno ? errno : ENOSPC;
			return -1;
		}
	}
#endif

	r = fsync(((DirectoryMethodFile *) f)->fd);
	if (r < 0)
		dir_data->lasterrno = errno;
	return r;
}

static ssize_t
dir_get_file_size(const char *pathname)
{
	struct stat statbuf;
	char		tmppath[MAXPGPATH];

	snprintf(tmppath, sizeof(tmppath), "%s/%s",
			 dir_data->basedir, pathname);

	if (stat(tmppath, &statbuf) != 0)
	{
		dir_data->lasterrno = errno;
		return -1;
	}

	return statbuf.st_size;
}

static pg_compress_algorithm
dir_compression_algorithm(void)
{
	return dir_data->compression_algorithm;
}

static bool
dir_existsfile(const char *pathname)
{
	char		tmppath[MAXPGPATH];
	int			fd;

	dir_clear_error();

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
	dir_clear_error();

	if (dir_data->sync)
	{
		/*
		 * Files are fsynced when they are closed, but we need to fsync the
		 * directory entry here as well.
		 */
		if (fsync_fname(dir_data->basedir, true) != 0)
		{
			dir_data->lasterrno = errno;
			return false;
		}
	}
	return true;
}


WalWriteMethod *
CreateWalDirectoryMethod(const char *basedir,
						 pg_compress_algorithm compression_algorithm,
						 int compression_level, bool sync)
{
	WalWriteMethod *method;

	method = pg_malloc0(sizeof(WalWriteMethod));
	method->open_for_write = dir_open_for_write;
	method->write = dir_write;
	method->get_current_pos = dir_get_current_pos;
	method->get_file_size = dir_get_file_size;
	method->get_file_name = dir_get_file_name;
	method->compression_algorithm = dir_compression_algorithm;
	method->close = dir_close;
	method->sync = dir_sync;
	method->existsfile = dir_existsfile;
	method->finish = dir_finish;
	method->getlasterror = dir_getlasterror;

	dir_data = pg_malloc0(sizeof(DirectoryMethodData));
	dir_data->compression_algorithm = compression_algorithm;
	dir_data->compression_level = compression_level;
	dir_data->basedir = pg_strdup(basedir);
	dir_data->sync = sync;

	return method;
}

void
FreeWalDirectoryMethod(void)
{
	pg_free(dir_data->basedir);
	pg_free(dir_data);
	dir_data = NULL;
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
	pg_compress_algorithm compression_algorithm;
	int			compression_level;
	bool		sync;
	TarMethodFile *currentfile;
	const char *lasterrstring;	/* if set, takes precedence over lasterrno */
	int			lasterrno;
#ifdef HAVE_LIBZ
	z_streamp	zp;
	void	   *zlibOut;
#endif
} TarMethodData;
static TarMethodData *tar_data = NULL;

#define tar_clear_error() \
	(tar_data->lasterrstring = NULL, tar_data->lasterrno = 0)
#define tar_set_error(msg) \
	(tar_data->lasterrstring = _(msg))

static const char *
tar_getlasterror(void)
{
	if (tar_data->lasterrstring)
		return tar_data->lasterrstring;
	return strerror(tar_data->lasterrno);
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
				/* If write didn't set errno, assume problem is no disk space */
				tar_data->lasterrno = errno ? errno : ENOSPC;
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
	if (tar_data->compression_algorithm == PG_COMPRESSION_NONE)
	{
		errno = 0;
		r = write(tar_data->fd, buf, count);
		if (r != count)
		{
			/* If write didn't set errno, assume problem is no disk space */
			tar_data->lasterrno = errno ? errno : ENOSPC;
			return -1;
		}
		((TarMethodFile *) f)->currpos += r;
		return r;
	}
#ifdef HAVE_LIBZ
	else if (tar_data->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		if (!tar_write_compressed_data(unconstify(void *, buf), count, false))
			return -1;
		((TarMethodFile *) f)->currpos += count;
		return count;
	}
#endif
	else
	{
		/* Can't happen - compression enabled with no method set */
		tar_data->lasterrno = ENOSYS;
		return -1;
	}
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

static char *
tar_get_file_name(const char *pathname, const char *temp_suffix)
{
	char	   *filename = pg_malloc0(MAXPGPATH * sizeof(char));

	snprintf(filename, MAXPGPATH, "%s%s",
			 pathname, temp_suffix ? temp_suffix : "");

	return filename;
}

static Walfile
tar_open_for_write(const char *pathname, const char *temp_suffix, size_t pad_to_size)
{
	char	   *tmppath;

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
		{
			tar_data->lasterrno = errno;
			return NULL;
		}

#ifdef HAVE_LIBZ
		if (tar_data->compression_algorithm == PG_COMPRESSION_GZIP)
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
			if (deflateInit2(tar_data->zp, tar_data->compression_level,
							 Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
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

	if (tar_data->currentfile != NULL)
	{
		tar_set_error("implementation error: tar files can't have more than one open file");
		return NULL;
	}

	tar_data->currentfile = pg_malloc0(sizeof(TarMethodFile));

	tmppath = tar_get_file_name(pathname, temp_suffix);

	/* Create a header with size set to 0 - we will fill out the size on close */
	if (tarCreateHeader(tar_data->currentfile->header, tmppath, NULL, 0, S_IRUSR | S_IWUSR, 0, 0, time(NULL)) != TAR_OK)
	{
		pg_free(tar_data->currentfile);
		pg_free(tmppath);
		tar_data->currentfile = NULL;
		tar_set_error("could not create tar header");
		return NULL;
	}

	pg_free(tmppath);

#ifdef HAVE_LIBZ
	if (tar_data->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		/* Flush existing data */
		if (!tar_write_compressed_data(NULL, 0, true))
			return NULL;

		/* Turn off compression for header */
		if (deflateParams(tar_data->zp, 0, Z_DEFAULT_STRATEGY) != Z_OK)
		{
			tar_set_error("could not change compression parameters");
			return NULL;
		}
	}
#endif

	tar_data->currentfile->ofs_start = lseek(tar_data->fd, 0, SEEK_CUR);
	if (tar_data->currentfile->ofs_start == -1)
	{
		tar_data->lasterrno = errno;
		pg_free(tar_data->currentfile);
		tar_data->currentfile = NULL;
		return NULL;
	}
	tar_data->currentfile->currpos = 0;

	if (tar_data->compression_algorithm == PG_COMPRESSION_NONE)
	{
		errno = 0;
		if (write(tar_data->fd, tar_data->currentfile->header,
				  TAR_BLOCK_SIZE) != TAR_BLOCK_SIZE)
		{
			/* If write didn't set errno, assume problem is no disk space */
			tar_data->lasterrno = errno ? errno : ENOSPC;
			pg_free(tar_data->currentfile);
			tar_data->currentfile = NULL;
			return NULL;
		}
	}
#ifdef HAVE_LIBZ
	else if (tar_data->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		/* Write header through the zlib APIs but with no compression */
		if (!tar_write_compressed_data(tar_data->currentfile->header,
									   TAR_BLOCK_SIZE, true))
			return NULL;

		/* Re-enable compression for the rest of the file */
		if (deflateParams(tar_data->zp, tar_data->compression_level,
						  Z_DEFAULT_STRATEGY) != Z_OK)
		{
			tar_set_error("could not change compression parameters");
			return NULL;
		}
	}
#endif
	else
	{
		/* not reachable */
		Assert(false);
	}

	tar_data->currentfile->pathname = pg_strdup(pathname);

	/*
	 * Uncompressed files are padded on creation, but for compression we can't
	 * do that
	 */
	if (pad_to_size)
	{
		tar_data->currentfile->pad_to_size = pad_to_size;
		if (tar_data->compression_algorithm == PG_COMPRESSION_NONE)
		{
			/* Uncompressed, so pad now */
			if (!tar_write_padding_data(tar_data->currentfile, pad_to_size))
				return NULL;
			/* Seek back to start */
			if (lseek(tar_data->fd,
					  tar_data->currentfile->ofs_start + TAR_BLOCK_SIZE,
					  SEEK_SET) != tar_data->currentfile->ofs_start + TAR_BLOCK_SIZE)
			{
				tar_data->lasterrno = errno;
				return NULL;
			}

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
	tar_data->lasterrno = ENOSYS;
	return -1;
}

static pg_compress_algorithm
tar_compression_algorithm(void)
{
	return tar_data->compression_algorithm;
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
	int			r;

	Assert(f != NULL);
	tar_clear_error();

	if (!tar_data->sync)
		return 0;

	/*
	 * Always sync the whole tarfile, because that's all we can do. This makes
	 * no sense on compressed files, so just ignore those.
	 */
	if (tar_data->compression_algorithm != PG_COMPRESSION_NONE)
		return 0;

	r = fsync(tar_data->fd);
	if (r < 0)
		tar_data->lasterrno = errno;
	return r;
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
		if (tar_data->compression_algorithm != PG_COMPRESSION_NONE)
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
		{
			tar_data->lasterrno = errno;
			return -1;
		}

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
		if (tar_data->compression_algorithm == PG_COMPRESSION_GZIP)
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
	if (tar_data->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		/* Flush the current buffer */
		if (!tar_write_compressed_data(NULL, 0, true))
			return -1;
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
	{
		tar_data->lasterrno = errno;
		return -1;
	}
	if (tar_data->compression_algorithm == PG_COMPRESSION_NONE)
	{
		errno = 0;
		if (write(tar_data->fd, tf->header, TAR_BLOCK_SIZE) != TAR_BLOCK_SIZE)
		{
			/* If write didn't set errno, assume problem is no disk space */
			tar_data->lasterrno = errno ? errno : ENOSPC;
			return -1;
		}
	}
#ifdef HAVE_LIBZ
	else if (tar_data->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		/* Turn off compression */
		if (deflateParams(tar_data->zp, 0, Z_DEFAULT_STRATEGY) != Z_OK)
		{
			tar_set_error("could not change compression parameters");
			return -1;
		}

		/* Overwrite the header, assuming the size will be the same */
		if (!tar_write_compressed_data(tar_data->currentfile->header,
									   TAR_BLOCK_SIZE, true))
			return -1;

		/* Turn compression back on */
		if (deflateParams(tar_data->zp, tar_data->compression_level,
						  Z_DEFAULT_STRATEGY) != Z_OK)
		{
			tar_set_error("could not change compression parameters");
			return -1;
		}
	}
#endif
	else
	{
		/* not reachable */
		Assert(false);
	}

	/* Move file pointer back down to end, so we can write the next file */
	if (lseek(tar_data->fd, 0, SEEK_END) < 0)
	{
		tar_data->lasterrno = errno;
		return -1;
	}

	/* Always fsync on close, so the padding gets fsynced */
	if (tar_sync(f) < 0)
	{
		/* XXX this seems pretty bogus; why is only this case fatal? */
		pg_fatal("could not fsync file \"%s\": %s",
				 tf->pathname, tar_getlasterror());
	}

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
	if (tar_data->compression_algorithm == PG_COMPRESSION_NONE)
	{
		errno = 0;
		if (write(tar_data->fd, zerobuf, sizeof(zerobuf)) != sizeof(zerobuf))
		{
			/* If write didn't set errno, assume problem is no disk space */
			tar_data->lasterrno = errno ? errno : ENOSPC;
			return false;
		}
	}
#ifdef HAVE_LIBZ
	else if (tar_data->compression_algorithm == PG_COMPRESSION_GZIP)
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
					tar_data->lasterrno = errno ? errno : ENOSPC;
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
	else
	{
		/* not reachable */
		Assert(false);
	}

	/* sync the empty blocks as well, since they're after the last file */
	if (tar_data->sync)
	{
		if (fsync(tar_data->fd) != 0)
		{
			tar_data->lasterrno = errno;
			return false;
		}
	}

	if (close(tar_data->fd) != 0)
	{
		tar_data->lasterrno = errno;
		return false;
	}

	tar_data->fd = -1;

	if (tar_data->sync)
	{
		if (fsync_fname(tar_data->tarfilename, false) != 0 ||
			fsync_parent_path(tar_data->tarfilename) != 0)
		{
			tar_data->lasterrno = errno;
			return false;
		}
	}

	return true;
}

/*
 * The argument compression_algorithm is currently ignored. It is in place for
 * symmetry with CreateWalDirectoryMethod which uses it for distinguishing
 * between the different compression methods. CreateWalTarMethod and its family
 * of functions handle only zlib compression.
 */
WalWriteMethod *
CreateWalTarMethod(const char *tarbase,
				   pg_compress_algorithm compression_algorithm,
				   int compression_level, bool sync)
{
	WalWriteMethod *method;
	const char *suffix = (compression_algorithm == PG_COMPRESSION_GZIP) ?
	".tar.gz" : ".tar";

	method = pg_malloc0(sizeof(WalWriteMethod));
	method->open_for_write = tar_open_for_write;
	method->write = tar_write;
	method->get_current_pos = tar_get_current_pos;
	method->get_file_size = tar_get_file_size;
	method->get_file_name = tar_get_file_name;
	method->compression_algorithm = tar_compression_algorithm;
	method->close = tar_close;
	method->sync = tar_sync;
	method->existsfile = tar_existsfile;
	method->finish = tar_finish;
	method->getlasterror = tar_getlasterror;

	tar_data = pg_malloc0(sizeof(TarMethodData));
	tar_data->tarfilename = pg_malloc0(strlen(tarbase) + strlen(suffix) + 1);
	sprintf(tar_data->tarfilename, "%s%s", tarbase, suffix);
	tar_data->fd = -1;
	tar_data->compression_algorithm = compression_algorithm;
	tar_data->compression_level = compression_level;
	tar_data->sync = sync;
#ifdef HAVE_LIBZ
	if (compression_algorithm == PG_COMPRESSION_GZIP)
		tar_data->zlibOut = (char *) pg_malloc(ZLIB_OUT_SIZE + 1);
#endif

	return method;
}

void
FreeWalTarMethod(void)
{
	pg_free(tar_data->tarfilename);
#ifdef HAVE_LIBZ
	if (tar_data->compression_algorithm == PG_COMPRESSION_GZIP)
		pg_free(tar_data->zlibOut);
#endif
	pg_free(tar_data);
	tar_data = NULL;
}
