/*-------------------------------------------------------------------------
 *
 * fd.c
 *	  Virtual file descriptor code.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/file/fd.c,v 1.57 2000/05/31 00:28:27 petere Exp $
 *
 * NOTES:
 *
 * This code manages a cache of 'virtual' file descriptors (VFDs).
 * The server opens many file descriptors for a variety of reasons,
 * including base tables, scratch files (e.g., sort and hash spool
 * files), and random calls to C library routines like system(3); it
 * is quite easy to exceed system limits on the number of open files a
 * single process can have.  (This is around 256 on many modern
 * operating systems, but can be as low as 32 on others.)
 *
 * VFDs are managed as an LRU pool, with actual OS file descriptors
 * being opened and closed as needed.  Obviously, if a routine is
 * opened using these interfaces, all subsequent operations must also
 * be through these interfaces (the File type is not a real file
 * descriptor).
 *
 * For this scheme to work, most (if not all) routines throughout the
 * server should use these interfaces instead of calling the C library
 * routines (e.g., open(2) and fopen(3)) themselves.  Otherwise, we
 * may find ourselves short of real file descriptors anyway.
 *
 * This file used to contain a bunch of stuff to support RAID levels 0
 * (jbod), 1 (duplex) and 5 (xor parity).  That stuff is all gone
 * because the parallel query processing code that called it is all
 * gone.  If you really need it you could get it from the original
 * POSTGRES source.
 *-------------------------------------------------------------------------
 */

#include <sys/types.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "postgres.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/ipc.h"

/*
 * Problem: Postgres does a system(ld...) to do dynamic loading.
 * This will open several extra files in addition to those used by
 * Postgres.  We need to guarantee that there are file descriptors free
 * for ld to use.
 *
 * The current solution is to limit the number of file descriptors
 * that this code will allocate at one time: it leaves RESERVE_FOR_LD free.
 *
 * (Even though most dynamic loaders now use dlopen(3) or the
 * equivalent, the OS must still open several files to perform the
 * dynamic loading.  Keep this here.)
 */
#ifndef RESERVE_FOR_LD
#define RESERVE_FOR_LD	10
#endif

/*
 * We need to ensure that we have at least some file descriptors
 * available to postgreSQL after we've reserved the ones for LD,
 * so we set that value here.
 *
 * I think 10 is an appropriate value so that's what it'll be
 * for now.
 */
#ifndef FD_MINFREE
#define FD_MINFREE 10
#endif

/* Debugging.... */

#ifdef FDDEBUG
#define DO_DB(A) A
#else
#define DO_DB(A)				/* A */
#endif

#define VFD_CLOSED (-1)

#define FileIsValid(file) \
	((file) > 0 && (file) < (int) SizeVfdCache && VfdCache[file].fileName != NULL)

#define FileIsNotOpen(file) (VfdCache[file].fd == VFD_CLOSED)

typedef struct vfd
{
	signed short fd;			/* current FD, or VFD_CLOSED if none */
	unsigned short fdstate;		/* bitflags for VFD's state */

/* these are the assigned bits in fdstate: */
#define FD_DIRTY		(1 << 0)/* written to, but not yet fsync'd */
#define FD_TEMPORARY	(1 << 1)/* should be unlinked when closed */

	File		nextFree;		/* link to next free VFD, if in freelist */
	File		lruMoreRecently;/* doubly linked recency-of-use list */
	File		lruLessRecently;
	long		seekPos;		/* current logical file position */
	char	   *fileName;		/* name of file, or NULL for unused VFD */
	/* NB: fileName is malloc'd, and must be free'd when closing the VFD */
	int			fileFlags;		/* open(2) flags for opening the file */
	int			fileMode;		/* mode to pass to open(2) */
} Vfd;

/*
 * Virtual File Descriptor array pointer and size.	This grows as
 * needed.	'File' values are indexes into this array.
 * Note that VfdCache[0] is not a usable VFD, just a list header.
 */
static Vfd *VfdCache;
static Size SizeVfdCache = 0;

/*
 * Number of file descriptors known to be in use by VFD entries.
 */
static int	nfile = 0;

/*
 * List of stdio FILEs opened with AllocateFile.
 *
 * Since we don't want to encourage heavy use of AllocateFile, it seems
 * OK to put a pretty small maximum limit on the number of simultaneously
 * allocated files.
 */
#define MAX_ALLOCATED_FILES  32

static int	numAllocatedFiles = 0;
static FILE *allocatedFiles[MAX_ALLOCATED_FILES];

/*
 * Number of temporary files opened during the current transaction;
 * this is used in generation of tempfile names.
 */
static long tempFileCounter = 0;


/*--------------------
 *
 * Private Routines
 *
 * Delete		   - delete a file from the Lru ring
 * LruDelete	   - remove a file from the Lru ring and close its FD
 * Insert		   - put a file at the front of the Lru ring
 * LruInsert	   - put a file at the front of the Lru ring and open it
 * ReleaseLruFile  - Release an fd by closing the last entry in the Lru ring
 * AllocateVfd	   - grab a free (or new) file record (from VfdArray)
 * FreeVfd		   - free a file record
 *
 * The Least Recently Used ring is a doubly linked list that begins and
 * ends on element zero.  Element zero is special -- it doesn't represent
 * a file and its "fd" field always == VFD_CLOSED.	Element zero is just an
 * anchor that shows us the beginning/end of the ring.
 * Only VFD elements that are currently really open (have an FD assigned) are
 * in the Lru ring.  Elements that are "virtually" open can be recognized
 * by having a non-null fileName field.
 *
 * example:
 *
 *	   /--less----\				   /---------\
 *	   v		   \			  v			  \
 *	 #0 --more---> LeastRecentlyUsed --more-\ \
 *	  ^\									| |
 *	   \\less--> MostRecentlyUsedFile	<---/ |
 *		\more---/					 \--less--/
 *
 *--------------------
 */
static void Delete(File file);
static void LruDelete(File file);
static void Insert(File file);
static int	LruInsert(File file);
static void ReleaseLruFile(void);
static File AllocateVfd(void);
static void FreeVfd(File file);

static int	FileAccess(File file);
static File fileNameOpenFile(FileName fileName, int fileFlags, int fileMode);
static char *filepath(char *filename);
static long pg_nofile(void);

/*
 * pg_fsync --- same as fsync except does nothing if -F switch was given
 */
int
pg_fsync(int fd)
{
	if (enableFsync)
		return fsync(fd);
	else
		return 0;
}

/*
 * pg_nofile: determine number of filedescriptors that fd.c is allowed to use
 */
static long
pg_nofile(void)
{
	static long no_files = 0;

	if (no_files == 0)
	{
		/* need do this calculation only once */
#ifndef HAVE_SYSCONF
		no_files = (long) NOFILE;
#else
		no_files = sysconf(_SC_OPEN_MAX);
		if (no_files == -1)
		{
			elog(DEBUG, "pg_nofile: Unable to get _SC_OPEN_MAX using sysconf(); using %d", NOFILE);
			no_files = (long) NOFILE;
		}
#endif

		if ((no_files - RESERVE_FOR_LD) < FD_MINFREE)
			elog(FATAL, "pg_nofile: insufficient File Descriptors in postmaster to start backend (%ld).\n"
				 "                   O/S allows %ld, Postmaster reserves %d, We need %d (MIN) after that.",
				 no_files - RESERVE_FOR_LD, no_files, RESERVE_FOR_LD, FD_MINFREE);

		no_files -= RESERVE_FOR_LD;
	}

	return no_files;
}

#if defined(FDDEBUG)

static void
_dump_lru()
{
	int			mru = VfdCache[0].lruLessRecently;
	Vfd		   *vfdP = &VfdCache[mru];
	char		buf[2048];

	sprintf(buf, "LRU: MOST %d ", mru);
	while (mru != 0)
	{
		mru = vfdP->lruLessRecently;
		vfdP = &VfdCache[mru];
		sprintf(buf + strlen(buf), "%d ", mru);
	}
	sprintf(buf + strlen(buf), "LEAST");
	elog(DEBUG, buf);
}

#endif	 /* FDDEBUG */

static void
Delete(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(DEBUG, "Delete %d (%s)",
			   file, VfdCache[file].fileName));
	DO_DB(_dump_lru());

	vfdP = &VfdCache[file];

	VfdCache[vfdP->lruLessRecently].lruMoreRecently = vfdP->lruMoreRecently;
	VfdCache[vfdP->lruMoreRecently].lruLessRecently = vfdP->lruLessRecently;

	DO_DB(_dump_lru());
}

static void
LruDelete(File file)
{
	Vfd		   *vfdP;
	int			returnValue;

	Assert(file != 0);

	DO_DB(elog(DEBUG, "LruDelete %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	/* delete the vfd record from the LRU ring */
	Delete(file);

	/* save the seek position */
	vfdP->seekPos = (long) lseek(vfdP->fd, 0L, SEEK_CUR);
	Assert(vfdP->seekPos != -1);

	/* if we have written to the file, sync it before closing */
	if (vfdP->fdstate & FD_DIRTY)
	{
		returnValue = pg_fsync(vfdP->fd);
		Assert(returnValue != -1);
		vfdP->fdstate &= ~FD_DIRTY;
	}

	/* close the file */
	returnValue = close(vfdP->fd);
	Assert(returnValue != -1);

	--nfile;
	vfdP->fd = VFD_CLOSED;
}

static void
Insert(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(DEBUG, "Insert %d (%s)",
			   file, VfdCache[file].fileName));
	DO_DB(_dump_lru());

	vfdP = &VfdCache[file];

	vfdP->lruMoreRecently = 0;
	vfdP->lruLessRecently = VfdCache[0].lruLessRecently;
	VfdCache[0].lruLessRecently = file;
	VfdCache[vfdP->lruLessRecently].lruMoreRecently = file;

	DO_DB(_dump_lru());
}

static int
LruInsert(File file)
{
	Vfd		   *vfdP;
	int			returnValue;

	Assert(file != 0);

	DO_DB(elog(DEBUG, "LruInsert %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (FileIsNotOpen(file))
	{

		while (nfile + numAllocatedFiles >= pg_nofile())
			ReleaseLruFile();

		/*
		 * The open could still fail for lack of file descriptors, eg due
		 * to overall system file table being full.  So, be prepared to
		 * release another FD if necessary...
		 */
tryAgain:
		vfdP->fd = open(vfdP->fileName, vfdP->fileFlags, vfdP->fileMode);
		if (vfdP->fd < 0 && (errno == EMFILE || errno == ENFILE))
		{
			errno = 0;
			ReleaseLruFile();
			goto tryAgain;
		}

		if (vfdP->fd < 0)
		{
			DO_DB(elog(DEBUG, "RE_OPEN FAILED: %d",
					   errno));
			return vfdP->fd;
		}
		else
		{
			DO_DB(elog(DEBUG, "RE_OPEN SUCCESS"));
			++nfile;
		}

		/* seek to the right position */
		if (vfdP->seekPos != 0L)
		{
			returnValue = lseek(vfdP->fd, vfdP->seekPos, SEEK_SET);
			Assert(returnValue != -1);
		}
	}

	/*
	 * put it at the head of the Lru ring
	 */

	Insert(file);

	return 0;
}

static void
ReleaseLruFile()
{
	DO_DB(elog(DEBUG, "ReleaseLruFile. Opened %d", nfile));

	if (nfile <= 0)
		elog(ERROR, "ReleaseLruFile: No open files available to be closed");

	/*
	 * There are opened files and so there should be at least one used vfd
	 * in the ring.
	 */
	Assert(VfdCache[0].lruMoreRecently != 0);
	LruDelete(VfdCache[0].lruMoreRecently);
}

/*
 * Force one kernel file descriptor to be released (temporarily).
 */
bool
ReleaseDataFile()
{
	DO_DB(elog(DEBUG, "ReleaseDataFile. Opened %d", nfile));

	if (nfile <= 0)
		return (false);
	Assert(VfdCache[0].lruMoreRecently != 0);
	LruDelete(VfdCache[0].lruMoreRecently);

	return (true);
}

static File
AllocateVfd()
{
	Index		i;
	File		file;

	DO_DB(elog(DEBUG, "AllocateVfd. Size %d", SizeVfdCache));

	if (SizeVfdCache == 0)
	{
		/* initialize header entry first time through */
		VfdCache = (Vfd *) malloc(sizeof(Vfd));
		Assert(VfdCache != NULL);
		MemSet((char *) &(VfdCache[0]), 0, sizeof(Vfd));
		VfdCache->fd = VFD_CLOSED;

		SizeVfdCache = 1;

		/*
		 * register proc-exit call to ensure temp files are dropped at
		 * exit
		 */
		on_proc_exit(AtEOXact_Files, NULL);
	}

	if (VfdCache[0].nextFree == 0)
	{

		/*
		 * The free list is empty so it is time to increase the size of
		 * the array.  We choose to double it each time this happens.
		 * However, there's not much point in starting *real* small.
		 */
		Size		newCacheSize = SizeVfdCache * 2;

		if (newCacheSize < 32)
			newCacheSize = 32;

		VfdCache = (Vfd *) realloc(VfdCache, sizeof(Vfd) * newCacheSize);
		Assert(VfdCache != NULL);

		/*
		 * Initialize the new entries and link them into the free list.
		 */

		for (i = SizeVfdCache; i < newCacheSize; i++)
		{
			MemSet((char *) &(VfdCache[i]), 0, sizeof(Vfd));
			VfdCache[i].nextFree = i + 1;
			VfdCache[i].fd = VFD_CLOSED;
		}
		VfdCache[newCacheSize - 1].nextFree = 0;
		VfdCache[0].nextFree = SizeVfdCache;

		/*
		 * Record the new size
		 */

		SizeVfdCache = newCacheSize;
	}

	file = VfdCache[0].nextFree;

	VfdCache[0].nextFree = VfdCache[file].nextFree;

	return file;
}

static void
FreeVfd(File file)
{
	Vfd		   *vfdP = &VfdCache[file];

	DO_DB(elog(DEBUG, "FreeVfd: %d (%s)",
			   file, vfdP->fileName ? vfdP->fileName : ""));

	if (vfdP->fileName != NULL)
	{
		free(vfdP->fileName);
		vfdP->fileName = NULL;
	}

	vfdP->nextFree = VfdCache[0].nextFree;
	VfdCache[0].nextFree = file;
}

/* filepath()
 * Convert given pathname to absolute.
 *
 * (Generally, this isn't actually necessary, considering that we
 * should be cd'd into the database directory.  Presently it is only
 * necessary to do it in "bootstrap" mode.	Maybe we should change
 * bootstrap mode to do the cd, and save a few cycles/bytes here.)
 */
static char *
filepath(char *filename)
{
	char	   *buf;
	int			len;

	/* Not an absolute path name? Then fill in with database path... */
	if (*filename != SEP_CHAR)
	{
		len = strlen(DatabasePath) + strlen(filename) + 2;
		buf = (char *) palloc(len);
		sprintf(buf, "%s%c%s", DatabasePath, SEP_CHAR, filename);
	}
	else
	{
		buf = (char *) palloc(strlen(filename) + 1);
		strcpy(buf, filename);
	}

#ifdef FILEDEBUG
	printf("filepath: path is %s\n", buf);
#endif

	return buf;
}

static int
FileAccess(File file)
{
	int			returnValue;

	DO_DB(elog(DEBUG, "FileAccess %d (%s)",
			   file, VfdCache[file].fileName));

	/*
	 * Is the file open?  If not, open it and put it at the head of the
	 * LRU ring (possibly closing the least recently used file to get an
	 * FD).
	 */

	if (FileIsNotOpen(file))
	{
		returnValue = LruInsert(file);
		if (returnValue != 0)
			return returnValue;
	}
	else if (VfdCache[0].lruLessRecently != file)
	{

		/*
		 * We now know that the file is open and that it is not the last
		 * one accessed, so we need to move it to the head of the Lru
		 * ring.
		 */

		Delete(file);
		Insert(file);
	}

	return 0;
}

/*
 *	Called when we get a shared invalidation message on some relation.
 */
#ifdef NOT_USED
void
FileInvalidate(File file)
{
	Assert(FileIsValid(file));
	if (!FileIsNotOpen(file))
		LruDelete(file);
}

#endif

static File
fileNameOpenFile(FileName fileName,
				 int fileFlags,
				 int fileMode)
{
	File		file;
	Vfd		   *vfdP;

	if (fileName == NULL)
		elog(ERROR, "fileNameOpenFile: NULL fname");

	DO_DB(elog(DEBUG, "fileNameOpenFile: %s %x %o",
			   fileName, fileFlags, fileMode));

	file = AllocateVfd();
	vfdP = &VfdCache[file];

	while (nfile + numAllocatedFiles >= pg_nofile())
		ReleaseLruFile();

tryAgain:
	vfdP->fd = open(fileName, fileFlags, fileMode);
	if (vfdP->fd < 0 && (errno == EMFILE || errno == ENFILE))
	{
		DO_DB(elog(DEBUG, "fileNameOpenFile: not enough descs, retry, er= %d",
				   errno));
		errno = 0;
		ReleaseLruFile();
		goto tryAgain;
	}

	if (vfdP->fd < 0)
	{
		FreeVfd(file);
		return -1;
	}
	++nfile;
	DO_DB(elog(DEBUG, "fileNameOpenFile: success %d",
			   vfdP->fd));

	Insert(file);

	vfdP->fileName = malloc(strlen(fileName) + 1);
	strcpy(vfdP->fileName, fileName);

	vfdP->fileFlags = fileFlags & ~(O_TRUNC | O_EXCL);
	vfdP->fileMode = fileMode;
	vfdP->seekPos = 0;
	vfdP->fdstate = 0x0;

	return file;
}

/*
 * open a file in the database directory ($PGDATA/base/...)
 */
File
FileNameOpenFile(FileName fileName, int fileFlags, int fileMode)
{
	File		fd;
	char	   *fname;

	fname = filepath(fileName);
	fd = fileNameOpenFile(fname, fileFlags, fileMode);
	pfree(fname);
	return fd;
}

/*
 * open a file in an arbitrary directory
 */
File
PathNameOpenFile(FileName fileName, int fileFlags, int fileMode)
{
	return fileNameOpenFile(fileName, fileFlags, fileMode);
}

/*
 * Open a temporary file that will disappear when we close it.
 *
 * This routine takes care of generating an appropriate tempfile name.
 * There's no need to pass in fileFlags or fileMode either, since only
 * one setting makes any sense for a temp file.
 */
File
OpenTemporaryFile(void)
{
	char		tempfilename[64];
	File		file;

	/*
	 * Generate a tempfile name that's unique within the current
	 * transaction
	 */
	snprintf(tempfilename, sizeof(tempfilename),
			 "pg_sorttemp%d.%ld", MyProcPid, tempFileCounter++);

	/* Open the file */
#ifndef __CYGWIN32__
	file = FileNameOpenFile(tempfilename,
							O_RDWR | O_CREAT | O_TRUNC, 0600);
#else
	file = FileNameOpenFile(tempfilename,
							O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0600);
#endif

	if (file <= 0)
		elog(ERROR, "Failed to create temporary file %s", tempfilename);

	/* Mark it for deletion at close or EOXact */
	VfdCache[file].fdstate |= FD_TEMPORARY;

	return file;
}

/*
 * close a file when done with it
 */
void
FileClose(File file)
{
	int			returnValue;

	Assert(FileIsValid(file));

	DO_DB(elog(DEBUG, "FileClose: %d (%s)",
			   file, VfdCache[file].fileName));

	if (!FileIsNotOpen(file))
	{

		/* remove the file from the lru ring */
		Delete(file);

		/* if we did any writes, sync the file before closing */
		if (VfdCache[file].fdstate & FD_DIRTY)
		{
			returnValue = pg_fsync(VfdCache[file].fd);
			Assert(returnValue != -1);
			VfdCache[file].fdstate &= ~FD_DIRTY;
		}

		/* close the file */
		returnValue = close(VfdCache[file].fd);
		Assert(returnValue != -1);

		--nfile;
		VfdCache[file].fd = VFD_CLOSED;
	}

	/*
	 * Delete the file if it was temporary
	 */
	if (VfdCache[file].fdstate & FD_TEMPORARY)
		unlink(VfdCache[file].fileName);

	/*
	 * Return the Vfd slot to the free list
	 */
	FreeVfd(file);
}

/*
 * close a file and forcibly delete the underlying Unix file
 */
void
FileUnlink(File file)
{
	Assert(FileIsValid(file));

	DO_DB(elog(DEBUG, "FileUnlink: %d (%s)",
			   file, VfdCache[file].fileName));

	/* force FileClose to delete it */
	VfdCache[file].fdstate |= FD_TEMPORARY;

	FileClose(file);
}

int
FileRead(File file, char *buffer, int amount)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(DEBUG, "FileRead: %d (%s) %d %p",
			   file, VfdCache[file].fileName, amount, buffer));

	FileAccess(file);
	returnCode = read(VfdCache[file].fd, buffer, amount);
	if (returnCode > 0)
		VfdCache[file].seekPos += returnCode;

	return returnCode;
}

int
FileWrite(File file, char *buffer, int amount)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(DEBUG, "FileWrite: %d (%s) %d %p",
			   file, VfdCache[file].fileName, amount, buffer));

	FileAccess(file);
	returnCode = write(VfdCache[file].fd, buffer, amount);
	if (returnCode > 0)
		VfdCache[file].seekPos += returnCode;

	/* mark the file as needing fsync */
	VfdCache[file].fdstate |= FD_DIRTY;

	return returnCode;
}

long
FileSeek(File file, long offset, int whence)
{
	Assert(FileIsValid(file));

	DO_DB(elog(DEBUG, "FileSeek: %d (%s) %ld %d",
			   file, VfdCache[file].fileName, offset, whence));

	if (FileIsNotOpen(file))
	{
		switch (whence)
		{
			case SEEK_SET:
				VfdCache[file].seekPos = offset;
				break;
			case SEEK_CUR:
				VfdCache[file].seekPos += offset;
				break;
			case SEEK_END:
				FileAccess(file);
				VfdCache[file].seekPos = lseek(VfdCache[file].fd, offset, whence);
				break;
			default:
				elog(ERROR, "FileSeek: invalid whence: %d", whence);
				break;
		}
	}
	else
		VfdCache[file].seekPos = lseek(VfdCache[file].fd, offset, whence);
	return VfdCache[file].seekPos;
}

/*
 * XXX not actually used but here for completeness
 */
#ifdef NOT_USED
long
FileTell(File file)
{
	Assert(FileIsValid(file));
	DO_DB(elog(DEBUG, "FileTell %d (%s)",
			   file, VfdCache[file].fileName));
	return VfdCache[file].seekPos;
}

#endif

int
FileTruncate(File file, long offset)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(DEBUG, "FileTruncate %d (%s)",
			   file, VfdCache[file].fileName));

	FileSync(file);
	FileAccess(file);
	returnCode = ftruncate(VfdCache[file].fd, (size_t) offset);
	return returnCode;
}

/*
 * FileSync --- if a file is marked as dirty, fsync it.
 *
 * The FD_DIRTY bit is slightly misnamed: it doesn't mean that we need to
 * write the file, but that we *have* written it and need to execute an
 * fsync() to ensure the changes are down on disk before we mark the current
 * transaction committed.
 *
 * FD_DIRTY is set by FileWrite or by an explicit FileMarkDirty() call.
 * It is cleared after successfully fsync'ing the file.  FileClose() will
 * fsync a dirty File that is about to be closed, since there will be no
 * other place to remember the need to fsync after the VFD is gone.
 *
 * Note that the DIRTY bit is logically associated with the actual disk file,
 * not with any particular kernel FD we might have open for it.  We assume
 * that fsync will force out any dirty buffers for that file, whether or not
 * they were written through the FD being used for the fsync call --- they
 * might even have been written by some other backend!
 *
 * Note also that LruDelete currently fsyncs a dirty file that it is about
 * to close the kernel file descriptor for.  The idea there is to avoid
 * having to re-open the kernel descriptor later.  But it's not real clear
 * that this is a performance win; we could end up fsyncing the same file
 * multiple times in a transaction, which would probably cost more time
 * than is saved by avoiding an open() call.  This should be studied.
 *
 * This routine used to think it could skip the fsync if the file is
 * physically closed, but that is now WRONG; see comments for FileMarkDirty.
 */
int
FileSync(File file)
{
	int			returnCode;

	Assert(FileIsValid(file));

	if (!(VfdCache[file].fdstate & FD_DIRTY))
	{
		/* Need not sync if file is not dirty. */
		returnCode = 0;
	}
	else if (!enableFsync)
	{
		/* Don't force the file open if pg_fsync isn't gonna sync it. */
		returnCode = 0;
		VfdCache[file].fdstate &= ~FD_DIRTY;
	}
	else
	{

		/*
		 * We don't use FileAccess() because we don't want to force the
		 * file to the front of the LRU ring; we aren't expecting to
		 * access it again soon.
		 */
		if (FileIsNotOpen(file))
		{
			returnCode = LruInsert(file);
			if (returnCode != 0)
				return returnCode;
		}
		returnCode = pg_fsync(VfdCache[file].fd);
		if (returnCode == 0)
			VfdCache[file].fdstate &= ~FD_DIRTY;
	}

	return returnCode;
}

/*
 * FileMarkDirty --- mark a file as needing fsync at transaction commit.
 *
 * Since FileWrite marks the file dirty, this routine is not needed in
 * normal use.	It is called when the buffer manager detects that some other
 * backend has written out a shared buffer that this backend dirtied (but
 * didn't write) in the current xact.  In that scenario, we need to fsync
 * the file before we can commit.  We cannot assume that the other backend
 * has fsync'd the file yet; we need to do our own fsync to ensure that
 * (a) the disk page is written and (b) this backend's commit is delayed
 * until the write is complete.
 *
 * Note we are assuming that an fsync issued by this backend will write
 * kernel disk buffers that were dirtied by another backend.  Furthermore,
 * it doesn't matter whether we currently have the file physically open;
 * we must fsync even if we have to re-open the file to do it.
 */
void
FileMarkDirty(File file)
{
	Assert(FileIsValid(file));

	DO_DB(elog(DEBUG, "FileMarkDirty: %d (%s)",
			   file, VfdCache[file].fileName));

	VfdCache[file].fdstate |= FD_DIRTY;
}


/*
 * Routines that want to use stdio (ie, FILE*) should use AllocateFile
 * rather than plain fopen().  This lets fd.c deal with freeing FDs if
 * necessary to open the file.	When done, call FreeFile rather than fclose.
 *
 * Note that files that will be open for any significant length of time
 * should NOT be handled this way, since they cannot share kernel file
 * descriptors with other files; there is grave risk of running out of FDs
 * if anyone locks down too many FDs.  Most callers of this routine are
 * simply reading a config file that they will read and close immediately.
 *
 * fd.c will automatically close all files opened with AllocateFile at
 * transaction commit or abort; this prevents FD leakage if a routine
 * that calls AllocateFile is terminated prematurely by elog(ERROR).
 */

FILE *
AllocateFile(char *name, char *mode)
{
	FILE	   *file;

	DO_DB(elog(DEBUG, "AllocateFile: Allocated %d.", numAllocatedFiles));

	if (numAllocatedFiles >= MAX_ALLOCATED_FILES)
		elog(ERROR, "AllocateFile: too many private FDs demanded");

TryAgain:
	if ((file = fopen(name, mode)) == NULL)
	{
		if (errno == EMFILE || errno == ENFILE)
		{
			DO_DB(elog(DEBUG, "AllocateFile: not enough descs, retry, er= %d",
					   errno));
			errno = 0;
			ReleaseLruFile();
			goto TryAgain;
		}
	}
	else
		allocatedFiles[numAllocatedFiles++] = file;
	return file;
}

void
FreeFile(FILE *file)
{
	int			i;

	DO_DB(elog(DEBUG, "FreeFile: Allocated %d.", numAllocatedFiles));

	/* Remove file from list of allocated files, if it's present */
	for (i = numAllocatedFiles; --i >= 0;)
	{
		if (allocatedFiles[i] == file)
		{
			allocatedFiles[i] = allocatedFiles[--numAllocatedFiles];
			break;
		}
	}
	if (i < 0)
		elog(NOTICE, "FreeFile: file was not obtained from AllocateFile");

	fclose(file);
}

/*
 * closeAllVfds
 *
 * Force all VFDs into the physically-closed state, so that the fewest
 * possible number of kernel file descriptors are in use.  There is no
 * change in the logical state of the VFDs.
 */
void
closeAllVfds()
{
	Index		i;

	if (SizeVfdCache > 0)
	{
		Assert(FileIsNotOpen(0));		/* Make sure ring not corrupted */
		for (i = 1; i < SizeVfdCache; i++)
		{
			if (!FileIsNotOpen(i))
				LruDelete(i);
		}
	}
}

/*
 * AtEOXact_Files
 *
 * This routine is called during transaction commit or abort or backend
 * exit (it doesn't particularly care which).  All still-open temporary-file
 * VFDs are closed, which also causes the underlying files to be deleted.
 * Furthermore, all "allocated" stdio files are closed.
 *
 * This routine is not involved in fsync'ing non-temporary files at xact
 * commit; that is done by FileSync under control of the buffer manager.
 * During a commit, that is done *before* control gets here.  If we still
 * have any needs-fsync bits set when we get here, we assume this is abort
 * and clear them.
 */
void
AtEOXact_Files(void)
{
	Index		i;

	if (SizeVfdCache > 0)
	{
		Assert(FileIsNotOpen(0));		/* Make sure ring not corrupted */
		for (i = 1; i < SizeVfdCache; i++)
		{
			if ((VfdCache[i].fdstate & FD_TEMPORARY) &&
				VfdCache[i].fileName != NULL)
				FileClose(i);
			else
				VfdCache[i].fdstate &= ~FD_DIRTY;
		}
	}

	while (numAllocatedFiles > 0)
		FreeFile(allocatedFiles[0]);

	/*
	 * Reset the tempfile name counter to 0; not really necessary, but
	 * helps keep the names from growing unreasonably long.
	 */
	tempFileCounter = 0;
}
