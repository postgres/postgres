/*-------------------------------------------------------------------------
 *
 * buffile.c
 *	  Management of large buffered files, primarily temporary files.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/file/buffile.c,v 1.1 1999/10/13 15:02:29 tgl Exp $
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
 * by elog(ERROR).  To avoid confusion, the caller should take care that
 * all calls for a single BufFile are made in the same palloc context.
 *
 * BufFile also supports temporary files that exceed the OS file size limit
 * (by opening multiple fd.c temporary files).  This is an essential feature
 * for sorts and hashjoins on large amounts of data.  It is possible to have
 * more than one BufFile reading/writing the same temp file, although the
 * caller is responsible for avoiding ill effects from buffer overlap when
 * this is done.
 *-------------------------------------------------------------------------
 */

#include <errno.h>

#include "postgres.h"

#include "storage/buffile.h"

/*
 * The maximum safe file size is presumed to be RELSEG_SIZE * BLCKSZ.
 * Note we adhere to this limit whether or not LET_OS_MANAGE_FILESIZE
 * is defined, although md.c ignores it when that symbol is defined.
 */
#define MAX_PHYSICAL_FILESIZE  (RELSEG_SIZE * BLCKSZ)

/*
 * To handle multiple BufFiles on a single logical temp file, we use this
 * data structure representing a logical file (which can be made up of
 * multiple physical files to get around the OS file size limit).
 */
typedef struct LogicalFile
{
	int			refCount;		/* number of BufFiles using me */
	bool		isTemp;			/* can only add files if this is TRUE */
	int			numFiles;		/* number of physical files in set */
	/* all files except the last have length exactly MAX_PHYSICAL_FILESIZE */

	File	   *files;			/* palloc'd array with numFiles entries */
	long	   *offsets;		/* palloc'd array with numFiles entries */
	/* offsets[i] is the current seek position of files[i].  We use this
	 * to avoid making redundant FileSeek calls.
	 */
} LogicalFile;

/*
 * A single file buffer looks like this.
 */
struct BufFile
{
	LogicalFile *logFile;		/* the underlying LogicalFile */
	bool		dirty;			/* does buffer need to be written? */
	/*
	 * "current pos" is position of start of buffer within LogicalFile.
	 * Position as seen by user of BufFile is (curFile, curOffset + pos).
	 */
	int			curFile;		/* file index (0..n) part of current pos */
	int			curOffset;		/* offset part of current pos */
	int			pos;			/* next read/write position in buffer */
	int			nbytes;			/* total # of valid bytes in buffer */
	char		buffer[BLCKSZ];
};

static LogicalFile *makeLogicalFile(File firstfile);
static void extendLogicalFile(LogicalFile *file);
static void deleteLogicalFile(LogicalFile *file);
static void BufFileLoadBuffer(BufFile *file);
static void BufFileDumpBuffer(BufFile *file);
static int	BufFileFlush(BufFile *file);


/*
 * Create a LogicalFile with one component file and refcount 1.
 * NOTE: caller must set isTemp true if appropriate.
 */
static LogicalFile *
makeLogicalFile(File firstfile)
{
	LogicalFile *file = (LogicalFile *) palloc(sizeof(LogicalFile));

	file->refCount = 1;
	file->isTemp = false;
	file->numFiles = 1;
	file->files = (File *) palloc(sizeof(File));
	file->files[0] = firstfile;
	file->offsets = (long *) palloc(sizeof(long));
	file->offsets[0] = 0L;

	return file;
}

/*
 * Add another component temp file.
 */
static void
extendLogicalFile(LogicalFile *file)
{
	File		pfile;

	Assert(file->isTemp);
	pfile = OpenTemporaryFile();
	Assert(pfile >= 0);

	file->files = (File *) repalloc(file->files,
									(file->numFiles+1) * sizeof(File));
	file->offsets = (long *) repalloc(file->offsets,
									  (file->numFiles+1) * sizeof(long));
	file->files[file->numFiles] = pfile;
	file->offsets[file->numFiles] = 0L;
	file->numFiles++;
}

/*
 * Close and delete a LogicalFile when its refCount has gone to zero.
 */
static void
deleteLogicalFile(LogicalFile *file)
{
	int i;

	for (i = 0; i < file->numFiles; i++)
		FileClose(file->files[i]);
	pfree(file->files);
	pfree(file->offsets);
	pfree(file);
}

/*
 * Create a BufFile for a new temporary file (which will expand to become
 * multiple temporary files if more than MAX_PHYSICAL_FILESIZE bytes are
 * written to it).
 */
BufFile *
BufFileCreateTemp(void)
{
	BufFile    *bfile = (BufFile *) palloc(sizeof(BufFile));
	File		pfile;
	LogicalFile *lfile;

	pfile = OpenTemporaryFile();
	Assert(pfile >= 0);

	lfile = makeLogicalFile(pfile);
	lfile->isTemp = true;

	bfile->logFile = lfile;
	bfile->dirty = false;
	bfile->curFile = 0;
	bfile->curOffset = 0L;
	bfile->pos = 0;
	bfile->nbytes = 0;

	return bfile;
}

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
	BufFile    *bfile = (BufFile *) palloc(sizeof(BufFile));
	LogicalFile *lfile;

	lfile = makeLogicalFile(file);

	bfile->logFile = lfile;
	bfile->dirty = false;
	bfile->curFile = 0;
	bfile->curOffset = 0L;
	bfile->pos = 0;
	bfile->nbytes = 0;

	return bfile;
}

/*
 * Create an additional BufFile accessing the same underlying file as an
 * existing BufFile.  This is useful for having multiple read/write access
 * positions in a single temporary file.  Note the caller is responsible
 * for avoiding trouble due to overlapping buffer positions!  (Caller may
 * assume that buffer size is BLCKSZ...)
 */
BufFile *
BufFileReaccess(BufFile *file)
{
	BufFile    *bfile = (BufFile *) palloc(sizeof(BufFile));

	bfile->logFile = file->logFile;
	bfile->logFile->refCount++;
	bfile->dirty = false;
	bfile->curFile = 0;
	bfile->curOffset = 0L;
	bfile->pos = 0;
	bfile->nbytes = 0;

	return bfile;
}

/*
 * Close a BufFile
 *
 * Like fclose(), this also implicitly FileCloses the underlying File.
 */
void
BufFileClose(BufFile *file)
{
	/* flush any unwritten data */
	BufFileFlush(file);
	/* close the underlying (with delete if it's a temp file) */
	if (--(file->logFile->refCount) <= 0)
		deleteLogicalFile(file->logFile);
	/* release the buffer space */
	pfree(file);
}

/* BufFileLoadBuffer
 *
 * Load some data into buffer, if possible, starting from curOffset.
 * At call, must have dirty = false, pos and nbytes = 0.
 * On exit, nbytes is number of bytes loaded.
 */
static void
BufFileLoadBuffer(BufFile *file)
{
	LogicalFile *lfile = file->logFile;
	File	thisfile;

	/*
	 * Advance to next component file if necessary and possible.
	 *
	 * This path can only be taken if there is more than one component,
	 * so it won't interfere with reading a non-temp file that is over
	 * MAX_PHYSICAL_FILESIZE.
	 */
	if (file->curOffset >= MAX_PHYSICAL_FILESIZE &&
		file->curFile+1 < lfile->numFiles)
	{
		file->curFile++;
		file->curOffset = 0L;
	}
	thisfile = lfile->files[file->curFile];
	/*
	 * May need to reposition physical file, if more than one BufFile
	 * is using it.
	 */
	if (file->curOffset != lfile->offsets[file->curFile])
	{
		if (FileSeek(thisfile, file->curOffset, SEEK_SET) != file->curOffset)
			return;				/* seek failed, read nothing */
		lfile->offsets[file->curFile] = file->curOffset;
	}
	file->nbytes = FileRead(thisfile, file->buffer, sizeof(file->buffer));
	if (file->nbytes < 0)
		file->nbytes = 0;
	lfile->offsets[file->curFile] += file->nbytes;
	/* we choose not to advance curOffset here */
}

/* BufFileDumpBuffer
 *
 * Dump buffer contents starting at curOffset.
 * At call, should have dirty = true, nbytes > 0.
 * On exit, dirty is cleared if successful write, and curOffset is advanced.
 */
static void
BufFileDumpBuffer(BufFile *file)
{
	LogicalFile *lfile = file->logFile;
	int			wpos = 0;
	int			bytestowrite;
	File		thisfile;

	/*
	 * Unlike BufFileLoadBuffer, we must dump the whole buffer even if
	 * it crosses a component-file boundary; so we need a loop.
	 */
	while (wpos < file->nbytes)
	{
		/*
		 * Advance to next component file if necessary and possible.
		 */
		if (file->curOffset >= MAX_PHYSICAL_FILESIZE && lfile->isTemp)
		{
			while (file->curFile+1 >= lfile->numFiles)
				extendLogicalFile(lfile);
			file->curFile++;
			file->curOffset = 0L;
		}
		/*
		 * Enforce per-file size limit only for temp files, else just try
		 * to write as much as asked...
		 */
		bytestowrite = file->nbytes - wpos;
		if (lfile->isTemp)
		{
			long	availbytes = MAX_PHYSICAL_FILESIZE - file->curOffset;

			if ((long) bytestowrite > availbytes)
				bytestowrite = (int) availbytes;
		}
		thisfile = lfile->files[file->curFile];
		/*
		 * May need to reposition physical file, if more than one BufFile
		 * is using it.
		 */
		if (file->curOffset != lfile->offsets[file->curFile])
		{
			if (FileSeek(thisfile, file->curOffset, SEEK_SET) != file->curOffset)
				return;			/* seek failed, give up */
			lfile->offsets[file->curFile] = file->curOffset;
		}
		bytestowrite = FileWrite(thisfile, file->buffer, bytestowrite);
		if (bytestowrite <= 0)
			return;				/* failed to write */
		lfile->offsets[file->curFile] += bytestowrite;
		file->curOffset += bytestowrite;
		wpos += bytestowrite;
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
	/* Now we can set the buffer empty without changing the logical position */
	file->pos = 0;
	file->nbytes = 0;
}

/* BufFileRead
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
		Assert(! file->dirty);
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

/* BufFileWrite
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

/* BufFileFlush
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

/* BufFileSeek
 *
 * Like fseek().  Result is 0 if OK, EOF if not.
 */
int
BufFileSeek(BufFile *file, int fileno, long offset, int whence)
{
	int newFile;
	long newOffset;
	switch (whence)
	{
		case SEEK_SET:
			if (fileno < 0 || fileno >= file->logFile->numFiles ||
				offset < 0)
				return EOF;
			newFile = fileno;
			newOffset = offset;
			break;
		case SEEK_CUR:
			/*
			 * Relative seek considers only the signed offset, ignoring fileno.
			 * Note that large offsets (> 1 gig) risk overflow.
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
			elog(ERROR, "BufFileSeek: invalid whence: %d", whence);
			return EOF;
	}
	while (newOffset < 0)
	{
		if (--newFile < 0)
			return EOF;
		newOffset += MAX_PHYSICAL_FILESIZE;
	}
	if (file->logFile->isTemp)
	{
		while (newOffset > MAX_PHYSICAL_FILESIZE)
		{
			if (++newFile >= file->logFile->numFiles)
				return EOF;
			newOffset -= MAX_PHYSICAL_FILESIZE;
		}
	}
	if (newFile == file->curFile &&
		newOffset >= file->curOffset &&
		newOffset <= file->curOffset + file->nbytes)
	{
		/*
		 * Seek is to a point within existing buffer; we can just adjust
		 * pos-within-buffer, without flushing buffer.  Note this is OK
		 * whether reading or writing, but buffer remains dirty if we
		 * were writing.
		 */
		file->pos = (int) (newOffset - file->curOffset);
		return 0;
	}
	/* Otherwise, must reposition buffer, so flush any dirty data */
	if (BufFileFlush(file) != 0)
		return EOF;
	file->curFile = newFile;
	file->curOffset = newOffset;
	file->pos = 0;
	file->nbytes = 0;
	return 0;
}

extern void
BufFileTell(BufFile *file, int *fileno, long *offset)
{
	*fileno = file->curFile;
	*offset = file->curOffset + file->pos;
}
