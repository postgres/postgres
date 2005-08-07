/*-------------------------------------------------------------------------
 *
 * fd.c
 *	  Virtual file descriptor code.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/file/fd.c,v 1.102.2.3 2005/08/07 18:48:00 tgl Exp $
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

#include "postgres.h"

#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/ipc.h"


/* Filename components for OpenTemporaryFile */
#define PG_TEMP_FILES_DIR "pgsql_tmp"
#define PG_TEMP_FILE_PREFIX "pgsql_tmp"


/*
 * We must leave some file descriptors free for system(), the dynamic loader,
 * and other code that tries to open files without consulting fd.c.  This
 * is the number left free.  (While we can be pretty sure we won't get
 * EMFILE, there's never any guarantee that we won't get ENFILE due to
 * other processes chewing up FDs.  So it's a bad idea to try to open files
 * without consulting fd.c.  Nonetheless we cannot control all code.)
 *
 * Because this is just a fixed setting, we are effectively assuming that
 * no such code will leave FDs open over the long term; otherwise the slop
 * is likely to be insufficient.  Note in particular that we expect that
 * loading a shared library does not result in any permanent increase in
 * the number of open files.  (This appears to be true on most if not
 * all platforms as of Feb 2004.)
 */
#define NUM_RESERVED_FDS		10

/*
 * If we have fewer than this many usable FDs after allowing for the reserved
 * ones, choke.
 */
#define FD_MINFREE				10


/*
 * A number of platforms allow individual processes to open many more files
 * than they can really support when *many* processes do the same thing.
 * This GUC parameter lets the DBA limit max_safe_fds to something less than
 * what the postmaster's initial probe suggests will work.
 */
int			max_files_per_process = 1000;

/*
 * Maximum number of file descriptors to open for either VFD entries or
 * AllocateFile/AllocateDir operations.  This is initialized to a conservative
 * value, and remains that way indefinitely in bootstrap or standalone-backend
 * cases.  In normal postmaster operation, the postmaster calls
 * set_max_safe_fds() late in initialization to update the value, and that
 * value is then inherited by forked subprocesses.
 *
 * Note: the value of max_files_per_process is taken into account while
 * setting this variable, and so need not be tested separately.
 */
static int	max_safe_fds = 32;			/* default if not changed */


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

#define FileUnknownPos (-1L)

/* these are the assigned bits in fdstate below: */
#define FD_TEMPORARY		(1 << 0)	/* T = delete when closed */
#define FD_XACT_TEMPORARY	(1 << 1)	/* T = delete at eoXact */

typedef struct vfd
{
	signed short fd;			/* current FD, or VFD_CLOSED if none */
	unsigned short fdstate;		/* bitflags for VFD's state */
	File		nextFree;		/* link to next free VFD, if in freelist */
	File		lruMoreRecently;	/* doubly linked recency-of-use list */
	File		lruLessRecently;
	long		seekPos;		/* current logical file position */
	char	   *fileName;		/* name of file, or NULL for unused VFD */
	/* NB: fileName is malloc'd, and must be free'd when closing the VFD */
	int			fileFlags;		/* open(2) flags for (re)opening the file */
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
 * List of <dirent.h> DIRs opened with AllocateDir.
 *
 * Since we don't have heavy use of AllocateDir, it seems OK to put a pretty
 * small maximum limit on the number of simultaneously allocated dirs.
 */
#define MAX_ALLOCATED_DIRS  10

static int	numAllocatedDirs = 0;
static DIR *allocatedDirs[MAX_ALLOCATED_DIRS];

/*
 * Number of temporary files opened during the current session;
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
static bool ReleaseLruFile(void);
static File AllocateVfd(void);
static void FreeVfd(File file);

static int	FileAccess(File file);
static File fileNameOpenFile(FileName fileName, int fileFlags, int fileMode);
static char *filepath(const char *filename);
static void AtProcExit_Files(void);
static void CleanupTempFiles(bool isProcExit);


/*
 * pg_fsync --- same as fsync except does nothing if enableFsync is off
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
 * pg_fdatasync --- same as fdatasync except does nothing if enableFsync is off
 *
 * Not all platforms have fdatasync; treat as fsync if not available.
 */
int
pg_fdatasync(int fd)
{
	if (enableFsync)
	{
#ifdef HAVE_FDATASYNC
		return fdatasync(fd);
#else
		return fsync(fd);
#endif
	}
	else
		return 0;
}

/*
 * count_usable_fds --- count how many FDs the system will let us open,
 *		and estimate how many are already open.
 *
 * We stop counting if usable_fds reaches max_to_probe.  Note: a small
 * value of max_to_probe might result in an underestimate of already_open;
 * we must fill in any "gaps" in the set of used FDs before the calculation
 * of already_open will give the right answer.  In practice, max_to_probe
 * of a couple of dozen should be enough to ensure good results.
 *
 * We assume stdin (FD 0) is available for dup'ing
 */
static void
count_usable_fds(int max_to_probe, int *usable_fds, int *already_open)
{
	int		   *fd;
	int			size;
	int			used = 0;
	int			highestfd = 0;
	int			j;

	size = 1024;
	fd = (int *) palloc(size * sizeof(int));

	/* dup until failure or probe limit reached */
	for (;;)
	{
		int		thisfd;

		thisfd = dup(0);
		if (thisfd < 0)
		{
			/* Expect EMFILE or ENFILE, else it's fishy */
			if (errno != EMFILE && errno != ENFILE)
				elog(WARNING, "dup(0) failed after %d successes: %m", used);
			break;
		}

		if (used >= size)
		{
			size *= 2;
			fd = (int *) repalloc(fd, size * sizeof(int));
		}
		fd[used++] = thisfd;

		if (highestfd < thisfd)
			highestfd = thisfd;

		if (used >= max_to_probe)
			break;
	}

	/* release the files we opened */
	for (j = 0; j < used; j++)
		close(fd[j]);

	pfree(fd);

	/*
	 * Return results.  usable_fds is just the number of successful dups.
	 * We assume that the system limit is highestfd+1 (remember 0 is a legal
	 * FD number) and so already_open is highestfd+1 - usable_fds.
	 */
	*usable_fds = used;
	*already_open = highestfd+1 - used;
}

/*
 * set_max_safe_fds
 *		Determine number of filedescriptors that fd.c is allowed to use
 */
void
set_max_safe_fds(void)
{
	int			usable_fds;
	int			already_open;

	/*
	 * We want to set max_safe_fds to
	 *			MIN(usable_fds, max_files_per_process - already_open)
	 * less the slop factor for files that are opened without consulting
	 * fd.c.  This ensures that we won't exceed either max_files_per_process
	 * or the experimentally-determined EMFILE limit.
	 */
	count_usable_fds(max_files_per_process,
					 &usable_fds, &already_open);

	max_safe_fds = Min(usable_fds, max_files_per_process - already_open);

	/*
	 * Take off the FDs reserved for system() etc.
	 */
	max_safe_fds -= NUM_RESERVED_FDS;

	/*
	 * Make sure we still have enough to get by.
	 */
	if (max_safe_fds < FD_MINFREE)
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("insufficient file descriptors available to start server process"),
				 errdetail("System allows %d, we need at least %d.",
						   max_safe_fds + NUM_RESERVED_FDS,
						   FD_MINFREE + NUM_RESERVED_FDS)));

	elog(DEBUG2, "max_safe_fds = %d, usable_fds = %d, already_open = %d",
		 max_safe_fds, usable_fds, already_open);
}

/*
 * BasicOpenFile --- same as open(2) except can free other FDs if needed
 *
 * This is exported for use by places that really want a plain kernel FD,
 * but need to be proof against running out of FDs.  Once an FD has been
 * successfully returned, it is the caller's responsibility to ensure that
 * it will not be leaked on ereport()!	Most users should *not* call this
 * routine directly, but instead use the VFD abstraction level, which
 * provides protection against descriptor leaks as well as management of
 * files that need to be open for more than a short period of time.
 *
 * Ideally this should be the *only* direct call of open() in the backend.
 * In practice, the postmaster calls open() directly, and there are some
 * direct open() calls done early in backend startup.  Those are OK since
 * this module wouldn't have any open files to close at that point anyway.
 */
int
BasicOpenFile(FileName fileName, int fileFlags, int fileMode)
{
	int			fd;

tryAgain:
	fd = open(fileName, fileFlags, fileMode);

	if (fd >= 0)
		return fd;				/* success! */

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
			  errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto tryAgain;
		errno = save_errno;
	}

	return -1;					/* failure */
}

#if defined(FDDEBUG)

static void
_dump_lru(void)
{
	int			mru = VfdCache[0].lruLessRecently;
	Vfd		   *vfdP = &VfdCache[mru];
	char		buf[2048];

	snprintf(buf, sizeof(buf), "LRU: MOST %d ", mru);
	while (mru != 0)
	{
		mru = vfdP->lruLessRecently;
		vfdP = &VfdCache[mru];
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d ", mru);
	}
	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "LEAST");
	elog(LOG, buf);
}
#endif   /* FDDEBUG */

static void
Delete(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "Delete %d (%s)",
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

	Assert(file != 0);

	DO_DB(elog(LOG, "LruDelete %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	/* delete the vfd record from the LRU ring */
	Delete(file);

	/* save the seek position */
	vfdP->seekPos = (long) lseek(vfdP->fd, 0L, SEEK_CUR);
	Assert(vfdP->seekPos != -1L);

	/* close the file */
	if (close(vfdP->fd))
		elog(LOG, "failed to close \"%s\": %m",
			 vfdP->fileName);

	--nfile;
	vfdP->fd = VFD_CLOSED;
}

static void
Insert(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "Insert %d (%s)",
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

	Assert(file != 0);

	DO_DB(elog(LOG, "LruInsert %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (FileIsNotOpen(file))
	{
		while (nfile + numAllocatedFiles + numAllocatedDirs >= max_safe_fds)
		{
			if (!ReleaseLruFile())
				break;
		}

		/*
		 * The open could still fail for lack of file descriptors, eg due
		 * to overall system file table being full.  So, be prepared to
		 * release another FD if necessary...
		 */
		vfdP->fd = BasicOpenFile(vfdP->fileName, vfdP->fileFlags,
								 vfdP->fileMode);
		if (vfdP->fd < 0)
		{
			DO_DB(elog(LOG, "RE_OPEN FAILED: %d", errno));
			return vfdP->fd;
		}
		else
		{
			DO_DB(elog(LOG, "RE_OPEN SUCCESS"));
			++nfile;
		}

		/* seek to the right position */
		if (vfdP->seekPos != 0L)
		{
			long		returnValue;

			returnValue = (long) lseek(vfdP->fd, vfdP->seekPos, SEEK_SET);
			Assert(returnValue != -1L);
		}
	}

	/*
	 * put it at the head of the Lru ring
	 */

	Insert(file);

	return 0;
}

static bool
ReleaseLruFile(void)
{
	DO_DB(elog(LOG, "ReleaseLruFile. Opened %d", nfile));

	if (nfile > 0)
	{
		/*
		 * There are opened files and so there should be at least one used
		 * vfd in the ring.
		 */
		Assert(VfdCache[0].lruMoreRecently != 0);
		LruDelete(VfdCache[0].lruMoreRecently);
		return true;			/* freed a file */
	}
	return false;				/* no files available to free */
}

static File
AllocateVfd(void)
{
	Index		i;
	File		file;

	DO_DB(elog(LOG, "AllocateVfd. Size %d", SizeVfdCache));

	if (SizeVfdCache == 0)
	{
		/* initialize header entry first time through */
		VfdCache = (Vfd *) malloc(sizeof(Vfd));
		if (VfdCache == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		MemSet((char *) &(VfdCache[0]), 0, sizeof(Vfd));
		VfdCache->fd = VFD_CLOSED;

		SizeVfdCache = 1;

		/*
		 * register proc-exit call to ensure temp files are dropped at
		 * exit
		 */
		on_proc_exit(AtProcExit_Files, 0);
	}

	if (VfdCache[0].nextFree == 0)
	{
		/*
		 * The free list is empty so it is time to increase the size of
		 * the array.  We choose to double it each time this happens.
		 * However, there's not much point in starting *real* small.
		 */
		Size		newCacheSize = SizeVfdCache * 2;
		Vfd		   *newVfdCache;

		if (newCacheSize < 32)
			newCacheSize = 32;

		/*
		 * Be careful not to clobber VfdCache ptr if realloc fails.
		 */
		newVfdCache = (Vfd *) realloc(VfdCache, sizeof(Vfd) * newCacheSize);
		if (newVfdCache == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		VfdCache = newVfdCache;

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

	DO_DB(elog(LOG, "FreeVfd: %d (%s)",
			   file, vfdP->fileName ? vfdP->fileName : ""));

	if (vfdP->fileName != NULL)
	{
		free(vfdP->fileName);
		vfdP->fileName = NULL;
	}
	vfdP->fdstate = 0x0;

	vfdP->nextFree = VfdCache[0].nextFree;
	VfdCache[0].nextFree = file;
}

/* filepath()
 * Convert given pathname to absolute.
 *
 * Result is a palloc'd string.
 *
 * (Generally, this isn't actually necessary, considering that we
 * should be cd'd into the database directory.  Presently it is only
 * necessary to do it in "bootstrap" mode.	Maybe we should change
 * bootstrap mode to do the cd, and save a few cycles/bytes here.)
 */
static char *
filepath(const char *filename)
{
	char	   *buf;

	/* Not an absolute path name? Then fill in with database path... */
	if (!is_absolute_path(filename))
	{
		buf = (char *) palloc(strlen(DatabasePath) + strlen(filename) + 2);
		sprintf(buf, "%s/%s", DatabasePath, filename);
	}
	else
		buf = pstrdup(filename);

#ifdef FILEDEBUG
	printf("filepath: path is %s\n", buf);
#endif

	return buf;
}

static int
FileAccess(File file)
{
	int			returnValue;

	DO_DB(elog(LOG, "FileAccess %d (%s)",
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
	char	   *fnamecopy;
	File		file;
	Vfd		   *vfdP;

	DO_DB(elog(LOG, "fileNameOpenFile: %s %x %o",
			   fileName, fileFlags, fileMode));

	/*
	 * We need a malloc'd copy of the file name; fail cleanly if no room.
	 */
	fnamecopy = strdup(fileName);
	if (fnamecopy == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	file = AllocateVfd();
	vfdP = &VfdCache[file];

	while (nfile + numAllocatedFiles + numAllocatedDirs >= max_safe_fds)
	{
		if (!ReleaseLruFile())
			break;
	}

	vfdP->fd = BasicOpenFile(fileName, fileFlags, fileMode);

	if (vfdP->fd < 0)
	{
		FreeVfd(file);
		free(fnamecopy);
		return -1;
	}
	++nfile;
	DO_DB(elog(LOG, "fileNameOpenFile: success %d",
			   vfdP->fd));

	Insert(file);

	vfdP->fileName = fnamecopy;
	/* Saved flags are adjusted to be OK for re-opening file */
	vfdP->fileFlags = fileFlags & ~(O_CREAT | O_TRUNC | O_EXCL);
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
 *
 * interXact: if true, don't close the file at end-of-transaction. In
 * most cases, you don't want temporary files to outlive the transaction
 * that created them, so this should be false -- but if you need
 * "somewhat" temporary storage, this might be useful. In either case,
 * the file is removed when the File is explicitly closed.
 */
File
OpenTemporaryFile(bool interXact)
{
	char		tempfilepath[MAXPGPATH];
	File		file;

	/*
	 * Generate a tempfile name that should be unique within the current
	 * database instance.
	 */
	snprintf(tempfilepath, sizeof(tempfilepath),
			 "%s/%s%d.%ld", PG_TEMP_FILES_DIR, PG_TEMP_FILE_PREFIX,
			 MyProcPid, tempFileCounter++);

	/*
	 * Open the file.  Note: we don't use O_EXCL, in case there is an
	 * orphaned temp file that can be reused.
	 */
	file = FileNameOpenFile(tempfilepath,
							O_RDWR | O_CREAT | O_TRUNC | PG_BINARY,
							0600);
	if (file <= 0)
	{
		char	   *dirpath;

		/*
		 * We might need to create the pg_tempfiles subdirectory, if no
		 * one has yet done so.
		 *
		 * Don't check for error from mkdir; it could fail if someone else
		 * just did the same thing.  If it doesn't work then we'll bomb
		 * out on the second create attempt, instead.
		 */
		dirpath = filepath(PG_TEMP_FILES_DIR);
		mkdir(dirpath, S_IRWXU);
		pfree(dirpath);

		file = FileNameOpenFile(tempfilepath,
								O_RDWR | O_CREAT | O_TRUNC | PG_BINARY,
								0600);
		if (file <= 0)
			elog(ERROR, "could not create temporary file \"%s\": %m",
				 tempfilepath);
	}

	/* Mark it for deletion at close */
	VfdCache[file].fdstate |= FD_TEMPORARY;

	/* Mark it for deletion at EOXact */
	if (!interXact)
		VfdCache[file].fdstate |= FD_XACT_TEMPORARY;

	return file;
}

/*
 * close a file when done with it
 */
void
FileClose(File file)
{
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileClose: %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (!FileIsNotOpen(file))
	{
		/* remove the file from the lru ring */
		Delete(file);

		/* close the file */
		if (close(vfdP->fd))
			elog(LOG, "failed to close \"%s\": %m",
				 vfdP->fileName);

		--nfile;
		vfdP->fd = VFD_CLOSED;
	}

	/*
	 * Delete the file if it was temporary
	 */
	if (vfdP->fdstate & FD_TEMPORARY)
	{
		/* reset flag so that die() interrupt won't cause problems */
		vfdP->fdstate &= ~FD_TEMPORARY;
		if (unlink(vfdP->fileName))
			elog(LOG, "failed to unlink \"%s\": %m",
				 vfdP->fileName);
	}

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

	DO_DB(elog(LOG, "FileUnlink: %d (%s)",
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

	DO_DB(elog(LOG, "FileRead: %d (%s) %ld %d %p",
			   file, VfdCache[file].fileName,
			   VfdCache[file].seekPos, amount, buffer));

	FileAccess(file);
	returnCode = read(VfdCache[file].fd, buffer, amount);
	if (returnCode > 0)
		VfdCache[file].seekPos += returnCode;
	else
		VfdCache[file].seekPos = FileUnknownPos;

	return returnCode;
}

int
FileWrite(File file, char *buffer, int amount)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileWrite: %d (%s) %ld %d %p",
			   file, VfdCache[file].fileName,
			   VfdCache[file].seekPos, amount, buffer));

	FileAccess(file);

	errno = 0;
	returnCode = write(VfdCache[file].fd, buffer, amount);

	/* if write didn't set errno, assume problem is no disk space */
	if (returnCode != amount && errno == 0)
		errno = ENOSPC;

	if (returnCode > 0)
		VfdCache[file].seekPos += returnCode;
	else
		VfdCache[file].seekPos = FileUnknownPos;

	return returnCode;
}

long
FileSeek(File file, long offset, int whence)
{
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSeek: %d (%s) %ld %ld %d",
			   file, VfdCache[file].fileName,
			   VfdCache[file].seekPos, offset, whence));

	if (FileIsNotOpen(file))
	{
		switch (whence)
		{
			case SEEK_SET:
				if (offset < 0)
					elog(ERROR, "invalid seek offset: %ld", offset);
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
				elog(ERROR, "invalid whence: %d", whence);
				break;
		}
	}
	else
	{
		switch (whence)
		{
			case SEEK_SET:
				if (offset < 0)
					elog(ERROR, "invalid seek offset: %ld", offset);
				if (VfdCache[file].seekPos != offset)
					VfdCache[file].seekPos = lseek(VfdCache[file].fd, offset, whence);
				break;
			case SEEK_CUR:
				if (offset != 0 || VfdCache[file].seekPos == FileUnknownPos)
					VfdCache[file].seekPos = lseek(VfdCache[file].fd, offset, whence);
				break;
			case SEEK_END:
				VfdCache[file].seekPos = lseek(VfdCache[file].fd, offset, whence);
				break;
			default:
				elog(ERROR, "invalid whence: %d", whence);
				break;
		}
	}
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
	DO_DB(elog(LOG, "FileTell %d (%s)",
			   file, VfdCache[file].fileName));
	return VfdCache[file].seekPos;
}
#endif

int
FileTruncate(File file, long offset)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileTruncate %d (%s)",
			   file, VfdCache[file].fileName));

	FileAccess(file);
	returnCode = ftruncate(VfdCache[file].fd, (size_t) offset);
	return returnCode;
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
 * that calls AllocateFile is terminated prematurely by ereport(ERROR).
 *
 * Ideally this should be the *only* direct call of fopen() in the backend.
 */
FILE *
AllocateFile(char *name, char *mode)
{
	FILE	   *file;

	DO_DB(elog(LOG, "AllocateFile: Allocated %d", numAllocatedFiles));

	/*
	 * The test against MAX_ALLOCATED_FILES prevents us from overflowing
	 * allocatedFiles[]; the test against max_safe_fds prevents AllocateFile
	 * from hogging every one of the available FDs, which'd lead to infinite
	 * looping.
	 */
	if (numAllocatedFiles >= MAX_ALLOCATED_FILES ||
		numAllocatedFiles + numAllocatedDirs >= max_safe_fds - 1)
		elog(ERROR, "too many private files demanded");

TryAgain:
	if ((file = fopen(name, mode)) != NULL)
	{
		allocatedFiles[numAllocatedFiles] = file;
		numAllocatedFiles++;
		return file;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
			  errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

void
FreeFile(FILE *file)
{
	int			i;

	DO_DB(elog(LOG, "FreeFile: Allocated %d", numAllocatedFiles));

	/* Remove file from list of allocated files, if it's present */
	for (i = numAllocatedFiles; --i >= 0;)
	{
		if (allocatedFiles[i] == file)
		{
			numAllocatedFiles--;
			allocatedFiles[i] = allocatedFiles[numAllocatedFiles];
			break;
		}
	}
	if (i < 0)
		elog(WARNING, "file passed to FreeFile was not obtained from AllocateFile");

	fclose(file);
}


/*
 * Routines that want to use <dirent.h> (ie, DIR*) should use AllocateDir
 * rather than plain opendir().  This lets fd.c deal with freeing FDs if
 * necessary to open the directory, and with closing it after an elog.
 * When done, call FreeDir rather than closedir.
 *
 * Ideally this should be the *only* direct call of opendir() in the backend.
 */
DIR *
AllocateDir(const char *dirname)
{
	DIR	   *dir;

	DO_DB(elog(LOG, "AllocateDir: Allocated %d", numAllocatedDirs));

	/*
	 * The test against MAX_ALLOCATED_DIRS prevents us from overflowing
	 * allocatedDirs[]; the test against max_safe_fds prevents AllocateDir
	 * from hogging every one of the available FDs, which'd lead to infinite
	 * looping.
	 */
	if (numAllocatedDirs >= MAX_ALLOCATED_DIRS ||
		numAllocatedDirs + numAllocatedFiles >= max_safe_fds - 1)
		elog(ERROR, "too many private dirs demanded");

TryAgain:
	if ((dir = opendir(dirname)) != NULL)
	{
		allocatedDirs[numAllocatedDirs] = dir;
		numAllocatedDirs++;
		return dir;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
			  errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * Close a directory opened with AllocateDir.
 *
 * Note we do not check closedir's return value --- it is up to the caller
 * to handle close errors.
 */
int
FreeDir(DIR *dir)
{
	int			i;

	DO_DB(elog(LOG, "FreeDir: Allocated %d", numAllocatedDirs));

	/* Remove dir from list of allocated dirs, if it's present */
	for (i = numAllocatedDirs; --i >= 0;)
	{
		if (allocatedDirs[i] == dir)
		{
			numAllocatedDirs--;
			allocatedDirs[i] = allocatedDirs[numAllocatedDirs];
			break;
		}
	}
	if (i < 0)
		elog(WARNING, "dir passed to FreeDir was not obtained from AllocateDir");

	return closedir(dir);
}


/*
 * closeAllVfds
 *
 * Force all VFDs into the physically-closed state, so that the fewest
 * possible number of kernel file descriptors are in use.  There is no
 * change in the logical state of the VFDs.
 */
void
closeAllVfds(void)
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
 * This routine is called during transaction commit or abort (it doesn't
 * particularly care which).  All still-open per-transaction temporary file
 * VFDs are closed, which also causes the underlying files to be
 * deleted. Furthermore, all "allocated" stdio files are closed.
 */
void
AtEOXact_Files(void)
{
	CleanupTempFiles(false);
}

/*
 * AtProcExit_Files
 *
 * on_proc_exit hook to clean up temp files during backend shutdown.
 * Here, we want to clean up *all* temp files including interXact ones.
 */
static void
AtProcExit_Files(void)
{
	CleanupTempFiles(true);
}

/*
 * Close temporary files and delete their underlying files.
 *
 * isProcExit: if true, this is being called as the backend process is
 * exiting. If that's the case, we should remove all temporary files; if
 * that's not the case, we are being called for transaction commit/abort
 * and should only remove transaction-local temp files.  In either case,
 * also clean up "allocated" stdio files and dirs.
 */
static void
CleanupTempFiles(bool isProcExit)
{
	Index		i;

	if (SizeVfdCache > 0)
	{
		Assert(FileIsNotOpen(0));		/* Make sure ring not corrupted */
		for (i = 1; i < SizeVfdCache; i++)
		{
			unsigned short fdstate = VfdCache[i].fdstate;

			if ((fdstate & FD_TEMPORARY) && VfdCache[i].fileName != NULL)
			{
				/*
				 * If we're in the process of exiting a backend process,
				 * close all temporary files. Otherwise, only close
				 * temporary files local to the current transaction.
				 */
				if (isProcExit || (fdstate & FD_XACT_TEMPORARY))
					FileClose(i);
			}
		}
	}

	while (numAllocatedFiles > 0)
		FreeFile(allocatedFiles[0]);

	while (numAllocatedDirs > 0)
		FreeDir(allocatedDirs[0]);
}


/*
 * Remove temporary files left over from a prior postmaster session
 *
 * This should be called during postmaster startup.  It will forcibly
 * remove any leftover files created by OpenTemporaryFile.
 *
 * NOTE: we could, but don't, call this during a post-backend-crash restart
 * cycle.  The argument for not doing it is that someone might want to examine
 * the temp files for debugging purposes.  This does however mean that
 * OpenTemporaryFile had better allow for collision with an existing temp
 * file name.
 */
void
RemovePgTempFiles(void)
{
	char		db_path[MAXPGPATH];
	char		temp_path[MAXPGPATH];
	char		rm_path[MAXPGPATH];
	DIR		   *db_dir;
	DIR		   *temp_dir;
	struct dirent *db_de;
	struct dirent *temp_de;

	/*
	 * Cycle through pg_tempfiles for all databases and remove old temp
	 * files.
	 */
	snprintf(db_path, sizeof(db_path), "%s/base", DataDir);
	if ((db_dir = AllocateDir(db_path)) != NULL)
	{
		while ((db_de = readdir(db_dir)) != NULL)
		{
			if (strcmp(db_de->d_name, ".") == 0 ||
				strcmp(db_de->d_name, "..") == 0)
				continue;

			snprintf(temp_path, sizeof(temp_path),
					 "%s/%s/%s",
					 db_path, db_de->d_name,
					 PG_TEMP_FILES_DIR);
			if ((temp_dir = AllocateDir(temp_path)) != NULL)
			{
				while ((temp_de = readdir(temp_dir)) != NULL)
				{
					if (strcmp(temp_de->d_name, ".") == 0 ||
						strcmp(temp_de->d_name, "..") == 0)
						continue;

					snprintf(rm_path, sizeof(temp_path),
							 "%s/%s/%s/%s",
							 db_path, db_de->d_name,
							 PG_TEMP_FILES_DIR,
							 temp_de->d_name);

					if (strncmp(temp_de->d_name,
								PG_TEMP_FILE_PREFIX,
								strlen(PG_TEMP_FILE_PREFIX)) == 0)
						unlink(rm_path);
					else
						elog(LOG,
							 "unexpected file found in temporary-files directory: \"%s\"",
							 rm_path);
				}
				FreeDir(temp_dir);
			}
		}
		FreeDir(db_dir);
	}
}
