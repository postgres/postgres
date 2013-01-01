/*-------------------------------------------------------------------------
 *
 * buffile.c
 *	  Management of large buffered files, primarily temporary files.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/buffile.c
 *
 * NOTES:
 *
 * BufFiles provide a very incomplete emulation of stdio atop virtual Files
 * (as managed by fd.c).  Currently, we only support the buffered-I/O
 * aspect of stdio: a read or write of the low-level File occurs only
 * when the buffer is filled or emptied.  This is an even bigger win
 * for virtual Files than for ordinary kernel files, since reducing the
 * frequency with which a virtual File is touched reduces "thrashing"
 * of opening/closing file descriptors.
 *
 * Note that BufFile structs are allocated with palloc(), and therefore
 * will go away automatically at transaction end.  If the underlying
 * virtual File is made with OpenTemporaryFile, then all resources for
 * the file are certain to be cleaned up even if processing is aborted
 * by ereport(ERROR).	To avoid confusion, the caller should take care that
 * all calls for a single BufFile are made in the same palloc context.
 *
 * BufFile also supports temporary files that exceed the OS file size limit
 * (by opening multiple fd.c temporary files).	This is an essential feature
 * for sorts and hashjoins on large amounts of data.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/instrument.h"
#include "storage/fd.h"
#include "storage/buffile.h"
#include "storage/buf_internals.h"

/*
 * We break BufFiles into gigabyte-sized segments, regardless of RELSEG_SIZE.
 * The reason is that we'd like large temporary BufFiles to be spread across
 * multiple tablespaces when available.
 */
#define MAX_PHYSICAL_FILESIZE	0x40000000
#define BUFFILE_SEG_SIZE		(MAX_PHYSICAL_FILESIZE / BLCKSZ)

/*
 * This data structure represents a buffered file that consists of one or
 * more physical files (each accessed through a virtual file descriptor
 * managed by fd.c).
 */
struct BufFile
{
	int			numFiles;		/* number of physical files in set */
	/* all files except the last have length exactly MAX_PHYSICAL_FILESIZE */
	File	   *files;			/* palloc'd array with numFiles entries */
	off_t	   *offsets;		/* palloc'd array with numFiles entries */

	/*
	 * offsets[i] is the current seek position of files[i].  We use this to
	 * avoid making redundant FileSeek calls.
	 */

	bool		isTemp;			/* can only add files if this is TRUE */
	bool		isInterXact;	/* keep open over transactions? */
	bool		dirty;			/* does buffer need to be written? */

	/*
	 * "current pos" is position of start of buffer within the logical file.
	 * Position as seen by user of BufFile is (curFile, curOffset + pos).
	 */
	int			curFile;		/* file index (0..n) part of current pos */
	off_t		curOffset;		/* offset part of current pos */
	int			pos;			/* next read/write position in buffer */
	int			nbytes;			/* total # of valid bytes in buffer */
	char		buffer[BLCKSZ];
};

static BufFile *makeBufFile(File firstfile);
static void extendBufFile(BufFile *file);
static void BufFileLoadBuffer(BufFile *file);
static void BufFileDumpBuffer(BufFile *file);
static int	BufFileFlush(BufFile *file);


/*
 * Create a BufFile given the first underlying physical file.
 * NOTE: caller must set isTemp and isInterXact if appropriate.
 */
static BufFile *
makeBufFile(File firstfile)
{
	BufFile    *file = (BufFile *) palloc(sizeof(BufFile));

	file->numFiles = 1;
	file->files = (File *) palloc(sizeof(File));
	file->files[0] = firstfile;
	file->offsets = (off_t *) palloc(sizeof(off_t));
	file->offsets[0] = 0L;
	file->isTemp = false;
	file->isInterXact = false;
	file->dirty = false;
	file->curFile = 0;
	file->curOffset = 0L;
	file->pos = 0;
	file->nbytes = 0;

	return file;
}

/*
 * Add another component temp file.
 */
static void
extendBufFile(BufFile *file)
{
	File		pfile;

	Assert(file->isTemp);
	pfile = OpenTemporaryFile(file->isInterXact);
	Assert(pfile >= 0);

	file->files = (File *) repalloc(file->files,
									(file->numFiles + 1) * sizeof(File));
	file->offsets = (off_t *) repalloc(file->offsets,
									   (file->numFiles + 1) * sizeof(off_t));
	file->files[file->numFiles] = pfile;
	file->offsets[file->numFiles] = 0L;
	file->numFiles++;
}

/*
 * Create a BufFile for a new temporary file (which will expand to become
 * multiple temporary files if more than MAX_PHYSICAL_FILESIZE bytes are
 * written to it).
 *
 * If interXact is true, the temp file will not be automatically deleted
 * at end of transaction.
 *
 * Note: if interXact is true, the caller had better be calling us in a
 * memory context that will survive across transaction boundaries.
 */
BufFile *
BufFileCreateTemp(bool interXact)
{
	BufFile    *file;
	File		pfile;

	pfile = OpenTemporaryFile(interXact);
	Assert(pfile >= 0);

	file = makeBufFile(pfile);
	file->isTemp = true;
	file->isInterXact = interXact;

	return file;
}

#ifdef NOT_USED
/*
 * Create a BufFile and attach it to an already-opened virtual File.
 *
 * This is comparable to fdopen() in stdio.  This is the only way at present
 * to attach a BufFile to a non-temporary file.  Note that BufFiles created
 * in this way CANNOT be expanded into multiple files.
 */
BufFile *
BufFileCreate(File file)
{
	return makeBufFile(file);
}
#endif

/*
 * Close a BufFile
 *
 * Like fclose(), this also implicitly FileCloses the underlying File.
 */
void
BufFileClose(BufFile *file)
{
	int			i;

	/* flush any unwritten data */
	BufFileFlush(file);
	/* close the underlying file(s) (with delete if it's a temp file) */
	for (i = 0; i < file->numFiles; i++)
		FileClose(file->files[i]);
	/* release the buffer space */
	pfree(file->files);
	pfree(file->offsets);
	pfree(file);
}

/*
 * BufFileLoadBuffer
 *
 * Load some data into buffer, if possible, starting from curOffset.
 * At call, must have dirty = false, pos and nbytes = 0.
 * On exit, nbytes is number of bytes loaded.
 */
static void
BufFileLoadBuffer(BufFile *file)
{
	File		thisfile;

	/*
	 * Advance to next component file if necessary and possible.
	 *
	 * This path can only be taken if there is more than one component, so it
	 * won't interfere with reading a non-temp file that is over
	 * MAX_PHYSICAL_FILESIZE.
	 */
	if (file->curOffset >= MAX_PHYSICAL_FILESIZE &&
		file->curFile + 1 < file->numFiles)
	{
		file->curFile++;
		file->curOffset = 0L;
	}

	/*
	 * May need to reposition physical file.
	 */
	thisfile = file->files[file->curFile];
	if (file->curOffset != file->offsets[file->curFile])
	{
		if (FileSeek(thisfile, file->curOffset, SEEK_SET) != file->curOffset)
			return;				/* seek failed, read nothing */
		file->offsets[file->curFile] = file->curOffset;
	}

	/*
	 * Read whatever we can get, up to a full bufferload.
	 */
	file->nbytes = FileRead(thisfile, file->buffer, sizeof(file->buffer));
	if (file->nbytes < 0)
		file->nbytes = 0;
	file->offsets[file->curFile] += file->nbytes;
	/* we choose not to advance curOffset here */

	pgBufferUsage.temp_blks_read++;
}

/*
 * BufFileDumpBuffer
 *
 * Dump buffer contents starting at curOffset.
 * At call, should have dirty = true, nbytes > 0.
 * On exit, dirty is cleared if successful write, and curOffset is advanced.
 */
static void
BufFileDumpBuffer(BufFile *file)
{
	int			wpos = 0;
	int			bytestowrite;
	File		thisfile;

	/*
	 * Unlike BufFileLoadBuffer, we must dump the whole buffer even if it
	 * crosses a component-file boundary; so we need a loop.
	 */
	while (wpos < file->nbytes)
	{
		/*
		 * Advance to next component file if necessary and possible.
		 */
		if (file->curOffset >= MAX_PHYSICAL_FILESIZE && file->isTemp)
		{
			while (file->curFile + 1 >= file->numFiles)
				extendBufFile(file);
			file->curFile++;
			file->curOffset = 0L;
		}

		/*
		 * Enforce per-file size limit only for temp files, else just try to
		 * write as much as asked...
		 */
		bytestowrite = file->nbytes - wpos;
		if (file->isTemp)
		{
			off_t		availbytes = MAX_PHYSICAL_FILESIZE - file->curOffset;

			if ((off_t) bytestowrite > availbytes)
				bytestowrite = (int) availbytes;
		}

		/*
		 * May need to reposition physical file.
		 */
		thisfile = file->files[file->curFile];
		if (file->curOffset != file->offsets[file->curFile])
		{
			if (FileSeek(thisfile, file->curOffset, SEEK_SET) != file->curOffset)
				return;			/* seek failed, give up */
			file->offsets[file->curFile] = file->curOffset;
		}
		bytestowrite = FileWrite(thisfile, file->buffer + wpos, bytestowrite);
		if (bytestowrite <= 0)
			return;				/* failed to write */
		file->offsets[file->curFile] += bytestowrite;
		file->curOffset += bytestowrite;
		wpos += bytestowrite;

		pgBufferUsage.temp_blks_written++;
	}
	file->dirty = false;

	/*
	 * At this point, curOffset has been advanced to the end of the buffer,
	 * ie, its original value + nbytes.  We need to make it point to the
	 * logical file position, ie, original value + pos, in case that is less
	 * (as could happen due to a small backwards seek in a dirty buffer!)
	 */
	file->curOffset -= (file->nbytes - file->pos);
	if (file->curOffset < 0)	/* handle possible segment crossing */
	{
		file->curFile--;
		Assert(file->curFile >= 0);
		file->curOffset += MAX_PHYSICAL_FILESIZE;
	}

	/*
	 * Now we can set the buffer empty without changing the logical position
	 */
	file->pos = 0;
	file->nbytes = 0;
}

/*
 * BufFileRead
 *
 * Like fread() except we assume 1-byte element size.
 */
size_t
BufFileRead(BufFile *file, void *ptr, size_t size)
{
	size_t		nread = 0;
	size_t		nthistime;

	if (file->dirty)
	{
		if (BufFileFlush(file) != 0)
			return 0;			/* could not flush... */
		Assert(!file->dirty);
	}

	while (size > 0)
	{
		if (file->pos >= file->nbytes)
		{
			/* Try to load more data into buffer. */
			file->curOffset += file->pos;
			file->pos = 0;
			file->nbytes = 0;
			BufFileLoadBuffer(file);
			if (file->nbytes <= 0)
				break;			/* no more data available */
		}

		nthistime = file->nbytes - file->pos;
		if (nthistime > size)
			nthistime = size;
		Assert(nthistime > 0);

		memcpy(ptr, file->buffer + file->pos, nthistime);

		file->pos += nthistime;
		ptr = (void *) ((char *) ptr + nthistime);
		size -= nthistime;
		nread += nthistime;
	}

	return nread;
}

/*
 * BufFileWrite
 *
 * Like fwrite() except we assume 1-byte element size.
 */
size_t
BufFileWrite(BufFile *file, void *ptr, size_t size)
{
	size_t		nwritten = 0;
	size_t		nthistime;

	while (size > 0)
	{
		if (file->pos >= BLCKSZ)
		{
			/* Buffer full, dump it out */
			if (file->dirty)
			{
				BufFileDumpBuffer(file);
				if (file->dirty)
					break;		/* I/O error */
			}
			else
			{
				/* Hmm, went directly from reading to writing? */
				file->curOffset += file->pos;
				file->pos = 0;
				file->nbytes = 0;
			}
		}

		nthistime = BLCKSZ - file->pos;
		if (nthistime > size)
			nthistime = size;
		Assert(nthistime > 0);

		memcpy(file->buffer + file->pos, ptr, nthistime);

		file->dirty = true;
		file->pos += nthistime;
		if (file->nbytes < file->pos)
			file->nbytes = file->pos;
		ptr = (void *) ((char *) ptr + nthistime);
		size -= nthistime;
		nwritten += nthistime;
	}

	return nwritten;
}

/*
 * BufFileFlush
 *
 * Like fflush()
 */
static int
BufFileFlush(BufFile *file)
{
	if (file->dirty)
	{
		BufFileDumpBuffer(file);
		if (file->dirty)
			return EOF;
	}

	return 0;
}

/*
 * BufFileSeek
 *
 * Like fseek(), except that target position needs two values in order to
 * work when logical filesize exceeds maximum value representable by long.
 * We do not support relative seeks across more than LONG_MAX, however.
 *
 * Result is 0 if OK, EOF if not.  Logical position is not moved if an
 * impossible seek is attempted.
 */
int
BufFileSeek(BufFile *file, int fileno, off_t offset, int whence)
{
	int			newFile;
	off_t		newOffset;

	switch (whence)
	{
		case SEEK_SET:
			if (fileno < 0)
				return EOF;
			newFile = fileno;
			newOffset = offset;
			break;
		case SEEK_CUR:

			/*
			 * Relative seek considers only the signed offset, ignoring
			 * fileno. Note that large offsets (> 1 gig) risk overflow in this
			 * add, unless we have 64-bit off_t.
			 */
			newFile = file->curFile;
			newOffset = (file->curOffset + file->pos) + offset;
			break;
#ifdef NOT_USED
		case SEEK_END:
			/* could be implemented, not needed currently */
			break;
#endif
		default:
			elog(ERROR, "invalid whence: %d", whence);
			return EOF;
	}
	while (newOffset < 0)
	{
		if (--newFile < 0)
			return EOF;
		newOffset += MAX_PHYSICAL_FILESIZE;
	}
	if (newFile == file->curFile &&
		newOffset >= file->curOffset &&
		newOffset <= file->curOffset + file->nbytes)
	{
		/*
		 * Seek is to a point within existing buffer; we can just adjust
		 * pos-within-buffer, without flushing buffer.	Note this is OK
		 * whether reading or writing, but buffer remains dirty if we were
		 * writing.
		 */
		file->pos = (int) (newOffset - file->curOffset);
		return 0;
	}
	/* Otherwise, must reposition buffer, so flush any dirty data */
	if (BufFileFlush(file) != 0)
		return EOF;

	/*
	 * At this point and no sooner, check for seek past last segment. The
	 * above flush could have created a new segment, so checking sooner would
	 * not work (at least not with this code).
	 */
	if (file->isTemp)
	{
		/* convert seek to "start of next seg" to "end of last seg" */
		if (newFile == file->numFiles && newOffset == 0)
		{
			newFile--;
			newOffset = MAX_PHYSICAL_FILESIZE;
		}
		while (newOffset > MAX_PHYSICAL_FILESIZE)
		{
			if (++newFile >= file->numFiles)
				return EOF;
			newOffset -= MAX_PHYSICAL_FILESIZE;
		}
	}
	if (newFile >= file->numFiles)
		return EOF;
	/* Seek is OK! */
	file->curFile = newFile;
	file->curOffset = newOffset;
	file->pos = 0;
	file->nbytes = 0;
	return 0;
}

void
BufFileTell(BufFile *file, int *fileno, off_t *offset)
{
	*fileno = file->curFile;
	*offset = file->curOffset + file->pos;
}

/*
 * BufFileSeekBlock --- block-oriented seek
 *
 * Performs absolute seek to the start of the n'th BLCKSZ-sized block of
 * the file.  Note that users of this interface will fail if their files
 * exceed BLCKSZ * LONG_MAX bytes, but that is quite a lot; we don't work
 * with tables bigger than that, either...
 *
 * Result is 0 if OK, EOF if not.  Logical position is not moved if an
 * impossible seek is attempted.
 */
int
BufFileSeekBlock(BufFile *file, long blknum)
{
	return BufFileSeek(file,
					   (int) (blknum / BUFFILE_SEG_SIZE),
					   (off_t) (blknum % BUFFILE_SEG_SIZE) * BLCKSZ,
					   SEEK_SET);
}

#ifdef NOT_USED
/*
 * BufFileTellBlock --- block-oriented tell
 *
 * Any fractional part of a block in the current seek position is ignored.
 */
long
BufFileTellBlock(BufFile *file)
{
	long		blknum;

	blknum = (file->curOffset + file->pos) / BLCKSZ;
	blknum += file->curFile * BUFFILE_SEG_SIZE;
	return blknum;
}

#endif
