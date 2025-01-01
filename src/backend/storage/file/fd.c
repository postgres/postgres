/*-------------------------------------------------------------------------
 *
 * fd.c
 *	  Virtual file descriptor code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/fd.c
 *
 * NOTES:
 *
 * This code manages a cache of 'virtual' file descriptors (VFDs).
 * The server opens many file descriptors for a variety of reasons,
 * including base tables, scratch files (e.g., sort and hash spool
 * files), and random calls to C library routines like system(3); it
 * is quite easy to exceed system limits on the number of open files a
 * single process can have.  (This is around 1024 on many modern
 * operating systems, but may be lower on others.)
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
 * INTERFACE ROUTINES
 *
 * PathNameOpenFile and OpenTemporaryFile are used to open virtual files.
 * A File opened with OpenTemporaryFile is automatically deleted when the
 * File is closed, either explicitly or implicitly at end of transaction or
 * process exit. PathNameOpenFile is intended for files that are held open
 * for a long time, like relation files. It is the caller's responsibility
 * to close them, there is no automatic mechanism in fd.c for that.
 *
 * PathName(Create|Open|Delete)Temporary(File|Dir) are used to manage
 * temporary files that have names so that they can be shared between
 * backends.  Such files are automatically closed and count against the
 * temporary file limit of the backend that creates them, but unlike anonymous
 * files they are not automatically deleted.  See sharedfileset.c for a shared
 * ownership mechanism that provides automatic cleanup for shared files when
 * the last of a group of backends detaches.
 *
 * AllocateFile, AllocateDir, OpenPipeStream and OpenTransientFile are
 * wrappers around fopen(3), opendir(3), popen(3) and open(2), respectively.
 * They behave like the corresponding native functions, except that the handle
 * is registered with the current subtransaction, and will be automatically
 * closed at abort. These are intended mainly for short operations like
 * reading a configuration file; there is a limit on the number of files that
 * can be opened using these functions at any one time.
 *
 * Finally, BasicOpenFile is just a thin wrapper around open() that can
 * release file descriptors in use by the virtual file descriptors if
 * necessary. There is no automatic cleanup of file descriptors returned by
 * BasicOpenFile, it is solely the caller's responsibility to close the file
 * descriptor by calling close(2).
 *
 * If a non-virtual file descriptor needs to be held open for any length of
 * time, report it to fd.c by calling AcquireExternalFD or ReserveExternalFD
 * (and eventually ReleaseExternalFD), so that we can take it into account
 * while deciding how many VFDs can be open.  This applies to FDs obtained
 * with BasicOpenFile as well as those obtained without use of any fd.c API.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <dirent.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/resource.h>		/* for getrlimit */
#include <sys/stat.h>
#include <sys/types.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_tablespace.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/startup.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/resowner.h"
#include "utils/varlena.h"

/* Define PG_FLUSH_DATA_WORKS if we have an implementation for pg_flush_data */
#if defined(HAVE_SYNC_FILE_RANGE)
#define PG_FLUSH_DATA_WORKS 1
#elif !defined(WIN32) && defined(MS_ASYNC)
#define PG_FLUSH_DATA_WORKS 1
#elif defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
#define PG_FLUSH_DATA_WORKS 1
#endif

/*
 * We must leave some file descriptors free for system(), the dynamic loader,
 * and other code that tries to open files without consulting fd.c.  This
 * is the number left free.  (While we try fairly hard to prevent EMFILE
 * errors, there's never any guarantee that we won't get ENFILE due to
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
 * ones, choke.  (This value is chosen to work with "ulimit -n 64", but not
 * much less than that.  Note that this value ensures numExternalFDs can be
 * at least 16; as of this writing, the contrib/postgres_fdw regression tests
 * will not pass unless that can grow to at least 14.)
 */
#define FD_MINFREE				48

/*
 * A number of platforms allow individual processes to open many more files
 * than they can really support when *many* processes do the same thing.
 * This GUC parameter lets the DBA limit max_safe_fds to something less than
 * what the postmaster's initial probe suggests will work.
 */
int			max_files_per_process = 1000;

/*
 * Maximum number of file descriptors to open for operations that fd.c knows
 * about (VFDs, AllocateFile etc, or "external" FDs).  This is initialized
 * to a conservative value, and remains that way indefinitely in bootstrap or
 * standalone-backend cases.  In normal postmaster operation, the postmaster
 * calls set_max_safe_fds() late in initialization to update the value, and
 * that value is then inherited by forked subprocesses.
 *
 * Note: the value of max_files_per_process is taken into account while
 * setting this variable, and so need not be tested separately.
 */
int			max_safe_fds = FD_MINFREE;	/* default if not changed */

/* Whether it is safe to continue running after fsync() fails. */
bool		data_sync_retry = false;

/* How SyncDataDirectory() should do its job. */
int			recovery_init_sync_method = DATA_DIR_SYNC_METHOD_FSYNC;

/* Which kinds of files should be opened with PG_O_DIRECT. */
int			io_direct_flags;

/* Debugging.... */

#ifdef FDDEBUG
#define DO_DB(A) \
	do { \
		int			_do_db_save_errno = errno; \
		A; \
		errno = _do_db_save_errno; \
	} while (0)
#else
#define DO_DB(A) \
	((void) 0)
#endif

#define VFD_CLOSED (-1)

#define FileIsValid(file) \
	((file) > 0 && (file) < (int) SizeVfdCache && VfdCache[file].fileName != NULL)

#define FileIsNotOpen(file) (VfdCache[file].fd == VFD_CLOSED)

/* these are the assigned bits in fdstate below: */
#define FD_DELETE_AT_CLOSE	(1 << 0)	/* T = delete when closed */
#define FD_CLOSE_AT_EOXACT	(1 << 1)	/* T = close at eoXact */
#define FD_TEMP_FILE_LIMIT	(1 << 2)	/* T = respect temp_file_limit */

typedef struct vfd
{
	int			fd;				/* current FD, or VFD_CLOSED if none */
	unsigned short fdstate;		/* bitflags for VFD's state */
	ResourceOwner resowner;		/* owner, for automatic cleanup */
	File		nextFree;		/* link to next free VFD, if in freelist */
	File		lruMoreRecently;	/* doubly linked recency-of-use list */
	File		lruLessRecently;
	off_t		fileSize;		/* current size of file (0 if not temporary) */
	char	   *fileName;		/* name of file, or NULL for unused VFD */
	/* NB: fileName is malloc'd, and must be free'd when closing the VFD */
	int			fileFlags;		/* open(2) flags for (re)opening the file */
	mode_t		fileMode;		/* mode to pass to open(2) */
} Vfd;

/*
 * Virtual File Descriptor array pointer and size.  This grows as
 * needed.  'File' values are indexes into this array.
 * Note that VfdCache[0] is not a usable VFD, just a list header.
 */
static Vfd *VfdCache;
static Size SizeVfdCache = 0;

/*
 * Number of file descriptors known to be in use by VFD entries.
 */
static int	nfile = 0;

/*
 * Flag to tell whether it's worth scanning VfdCache looking for temp files
 * to close
 */
static bool have_xact_temporary_files = false;

/*
 * Tracks the total size of all temporary files.  Note: when temp_file_limit
 * is being enforced, this cannot overflow since the limit cannot be more
 * than INT_MAX kilobytes.  When not enforcing, it could theoretically
 * overflow, but we don't care.
 */
static uint64 temporary_files_size = 0;

/* Temporary file access initialized and not yet shut down? */
#ifdef USE_ASSERT_CHECKING
static bool temporary_files_allowed = false;
#endif

/*
 * List of OS handles opened with AllocateFile, AllocateDir and
 * OpenTransientFile.
 */
typedef enum
{
	AllocateDescFile,
	AllocateDescPipe,
	AllocateDescDir,
	AllocateDescRawFD,
} AllocateDescKind;

typedef struct
{
	AllocateDescKind kind;
	SubTransactionId create_subid;
	union
	{
		FILE	   *file;
		DIR		   *dir;
		int			fd;
	}			desc;
} AllocateDesc;

static int	numAllocatedDescs = 0;
static int	maxAllocatedDescs = 0;
static AllocateDesc *allocatedDescs = NULL;

/*
 * Number of open "external" FDs reported to Reserve/ReleaseExternalFD.
 */
static int	numExternalFDs = 0;

/*
 * Number of temporary files opened during the current session;
 * this is used in generation of tempfile names.
 */
static long tempFileCounter = 0;

/*
 * Array of OIDs of temp tablespaces.  (Some entries may be InvalidOid,
 * indicating that the current database's default tablespace should be used.)
 * When numTempTableSpaces is -1, this has not been set in the current
 * transaction.
 */
static Oid *tempTableSpaces = NULL;
static int	numTempTableSpaces = -1;
static int	nextTempTableSpace = 0;


/*--------------------
 *
 * Private Routines
 *
 * Delete		   - delete a file from the Lru ring
 * LruDelete	   - remove a file from the Lru ring and close its FD
 * Insert		   - put a file at the front of the Lru ring
 * LruInsert	   - put a file at the front of the Lru ring and open it
 * ReleaseLruFile  - Release an fd by closing the last entry in the Lru ring
 * ReleaseLruFiles - Release fd(s) until we're under the max_safe_fds limit
 * AllocateVfd	   - grab a free (or new) file record (from VfdCache)
 * FreeVfd		   - free a file record
 *
 * The Least Recently Used ring is a doubly linked list that begins and
 * ends on element zero.  Element zero is special -- it doesn't represent
 * a file and its "fd" field always == VFD_CLOSED.  Element zero is just an
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
static void ReleaseLruFiles(void);
static File AllocateVfd(void);
static void FreeVfd(File file);

static int	FileAccess(File file);
static File OpenTemporaryFileInTablespace(Oid tblspcOid, bool rejectError);
static bool reserveAllocatedDesc(void);
static int	FreeDesc(AllocateDesc *desc);

static void BeforeShmemExit_Files(int code, Datum arg);
static void CleanupTempFiles(bool isCommit, bool isProcExit);
static void RemovePgTempRelationFiles(const char *tsdirname);
static void RemovePgTempRelationFilesInDbspace(const char *dbspacedirname);

static void walkdir(const char *path,
					void (*action) (const char *fname, bool isdir, int elevel),
					bool process_symlinks,
					int elevel);
#ifdef PG_FLUSH_DATA_WORKS
static void pre_sync_fname(const char *fname, bool isdir, int elevel);
#endif
static void datadir_fsync_fname(const char *fname, bool isdir, int elevel);
static void unlink_if_exists_fname(const char *fname, bool isdir, int elevel);

static int	fsync_parent_path(const char *fname, int elevel);


/* ResourceOwner callbacks to hold virtual file descriptors */
static void ResOwnerReleaseFile(Datum res);
static char *ResOwnerPrintFile(Datum res);

static const ResourceOwnerDesc file_resowner_desc =
{
	.name = "File",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_FILES,
	.ReleaseResource = ResOwnerReleaseFile,
	.DebugPrint = ResOwnerPrintFile
};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void
ResourceOwnerRememberFile(ResourceOwner owner, File file)
{
	ResourceOwnerRemember(owner, Int32GetDatum(file), &file_resowner_desc);
}
static inline void
ResourceOwnerForgetFile(ResourceOwner owner, File file)
{
	ResourceOwnerForget(owner, Int32GetDatum(file), &file_resowner_desc);
}

/*
 * pg_fsync --- do fsync with or without writethrough
 */
int
pg_fsync(int fd)
{
#if !defined(WIN32) && defined(USE_ASSERT_CHECKING)
	struct stat st;

	/*
	 * Some operating system implementations of fsync() have requirements
	 * about the file access modes that were used when their file descriptor
	 * argument was opened, and these requirements differ depending on whether
	 * the file descriptor is for a directory.
	 *
	 * For any file descriptor that may eventually be handed to fsync(), we
	 * should have opened it with access modes that are compatible with
	 * fsync() on all supported systems, otherwise the code may not be
	 * portable, even if it runs ok on the current system.
	 *
	 * We assert here that a descriptor for a file was opened with write
	 * permissions (either O_RDWR or O_WRONLY) and for a directory without
	 * write permissions (O_RDONLY).
	 *
	 * Ignore any fstat errors and let the follow-up fsync() do its work.
	 * Doing this sanity check here counts for the case where fsync() is
	 * disabled.
	 */
	if (fstat(fd, &st) == 0)
	{
		int			desc_flags = fcntl(fd, F_GETFL);

		/*
		 * O_RDONLY is historically 0, so just make sure that for directories
		 * no write flags are used.
		 */
		if (S_ISDIR(st.st_mode))
			Assert((desc_flags & (O_RDWR | O_WRONLY)) == 0);
		else
			Assert((desc_flags & (O_RDWR | O_WRONLY)) != 0);
	}
	errno = 0;
#endif

	/* #if is to skip the wal_sync_method test if there's no need for it */
#if defined(HAVE_FSYNC_WRITETHROUGH)
	if (wal_sync_method == WAL_SYNC_METHOD_FSYNC_WRITETHROUGH)
		return pg_fsync_writethrough(fd);
	else
#endif
		return pg_fsync_no_writethrough(fd);
}


/*
 * pg_fsync_no_writethrough --- same as fsync except does nothing if
 *	enableFsync is off
 */
int
pg_fsync_no_writethrough(int fd)
{
	int			rc;

	if (!enableFsync)
		return 0;

retry:
	rc = fsync(fd);

	if (rc == -1 && errno == EINTR)
		goto retry;

	return rc;
}

/*
 * pg_fsync_writethrough
 */
int
pg_fsync_writethrough(int fd)
{
	if (enableFsync)
	{
#if defined(F_FULLFSYNC)
		return (fcntl(fd, F_FULLFSYNC, 0) == -1) ? -1 : 0;
#else
		errno = ENOSYS;
		return -1;
#endif
	}
	else
		return 0;
}

/*
 * pg_fdatasync --- same as fdatasync except does nothing if enableFsync is off
 */
int
pg_fdatasync(int fd)
{
	int			rc;

	if (!enableFsync)
		return 0;

retry:
	rc = fdatasync(fd);

	if (rc == -1 && errno == EINTR)
		goto retry;

	return rc;
}

/*
 * pg_file_exists -- check that a file exists.
 *
 * This requires an absolute path to the file.  Returns true if the file is
 * not a directory, false otherwise.
 */
bool
pg_file_exists(const char *name)
{
	struct stat st;

	Assert(name != NULL);

	if (stat(name, &st) == 0)
		return !S_ISDIR(st.st_mode);
	else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", name)));

	return false;
}

/*
 * pg_flush_data --- advise OS that the described dirty data should be flushed
 *
 * offset of 0 with nbytes 0 means that the entire file should be flushed
 */
void
pg_flush_data(int fd, off_t offset, off_t nbytes)
{
	/*
	 * Right now file flushing is primarily used to avoid making later
	 * fsync()/fdatasync() calls have less impact. Thus don't trigger flushes
	 * if fsyncs are disabled - that's a decision we might want to make
	 * configurable at some point.
	 */
	if (!enableFsync)
		return;

	/*
	 * We compile all alternatives that are supported on the current platform,
	 * to find portability problems more easily.
	 */
#if defined(HAVE_SYNC_FILE_RANGE)
	{
		int			rc;
		static bool not_implemented_by_kernel = false;

		if (not_implemented_by_kernel)
			return;

retry:

		/*
		 * sync_file_range(SYNC_FILE_RANGE_WRITE), currently linux specific,
		 * tells the OS that writeback for the specified blocks should be
		 * started, but that we don't want to wait for completion.  Note that
		 * this call might block if too much dirty data exists in the range.
		 * This is the preferable method on OSs supporting it, as it works
		 * reliably when available (contrast to msync()) and doesn't flush out
		 * clean data (like FADV_DONTNEED).
		 */
		rc = sync_file_range(fd, offset, nbytes,
							 SYNC_FILE_RANGE_WRITE);
		if (rc != 0)
		{
			int			elevel;

			if (rc == EINTR)
				goto retry;

			/*
			 * For systems that don't have an implementation of
			 * sync_file_range() such as Windows WSL, generate only one
			 * warning and then suppress all further attempts by this process.
			 */
			if (errno == ENOSYS)
			{
				elevel = WARNING;
				not_implemented_by_kernel = true;
			}
			else
				elevel = data_sync_elevel(WARNING);

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not flush dirty data: %m")));
		}

		return;
	}
#endif
#if !defined(WIN32) && defined(MS_ASYNC)
	{
		void	   *p;
		static int	pagesize = 0;

		/*
		 * On several OSs msync(MS_ASYNC) on a mmap'ed file triggers
		 * writeback. On linux it only does so if MS_SYNC is specified, but
		 * then it does the writeback synchronously. Luckily all common linux
		 * systems have sync_file_range().  This is preferable over
		 * FADV_DONTNEED because it doesn't flush out clean data.
		 *
		 * We map the file (mmap()), tell the kernel to sync back the contents
		 * (msync()), and then remove the mapping again (munmap()).
		 */

		/* mmap() needs actual length if we want to map whole file */
		if (offset == 0 && nbytes == 0)
		{
			nbytes = lseek(fd, 0, SEEK_END);
			if (nbytes < 0)
			{
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not determine dirty data size: %m")));
				return;
			}
		}

		/*
		 * Some platforms reject partial-page mmap() attempts.  To deal with
		 * that, just truncate the request to a page boundary.  If any extra
		 * bytes don't get flushed, well, it's only a hint anyway.
		 */

		/* fetch pagesize only once */
		if (pagesize == 0)
			pagesize = sysconf(_SC_PAGESIZE);

		/* align length to pagesize, dropping any fractional page */
		if (pagesize > 0)
			nbytes = (nbytes / pagesize) * pagesize;

		/* fractional-page request is a no-op */
		if (nbytes <= 0)
			return;

		/*
		 * mmap could well fail, particularly on 32-bit platforms where there
		 * may simply not be enough address space.  If so, silently fall
		 * through to the next implementation.
		 */
		if (nbytes <= (off_t) SSIZE_MAX)
			p = mmap(NULL, nbytes, PROT_READ, MAP_SHARED, fd, offset);
		else
			p = MAP_FAILED;

		if (p != MAP_FAILED)
		{
			int			rc;

			rc = msync(p, (size_t) nbytes, MS_ASYNC);
			if (rc != 0)
			{
				ereport(data_sync_elevel(WARNING),
						(errcode_for_file_access(),
						 errmsg("could not flush dirty data: %m")));
				/* NB: need to fall through to munmap()! */
			}

			rc = munmap(p, (size_t) nbytes);
			if (rc != 0)
			{
				/* FATAL error because mapping would remain */
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not munmap() while flushing data: %m")));
			}

			return;
		}
	}
#endif
#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
	{
		int			rc;

		/*
		 * Signal the kernel that the passed in range should not be cached
		 * anymore. This has the, desired, side effect of writing out dirty
		 * data, and the, undesired, side effect of likely discarding useful
		 * clean cached blocks.  For the latter reason this is the least
		 * preferable method.
		 */

		rc = posix_fadvise(fd, offset, nbytes, POSIX_FADV_DONTNEED);

		if (rc != 0)
		{
			/* don't error out, this is just a performance optimization */
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not flush dirty data: %m")));
		}

		return;
	}
#endif
}

/*
 * Truncate an open file to a given length.
 */
static int
pg_ftruncate(int fd, off_t length)
{
	int			ret;

retry:
	ret = ftruncate(fd, length);

	if (ret == -1 && errno == EINTR)
		goto retry;

	return ret;
}

/*
 * Truncate a file to a given length by name.
 */
int
pg_truncate(const char *path, off_t length)
{
	int			ret;
#ifdef WIN32
	int			save_errno;
	int			fd;

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd >= 0)
	{
		ret = pg_ftruncate(fd, length);
		save_errno = errno;
		CloseTransientFile(fd);
		errno = save_errno;
	}
	else
		ret = -1;
#else

retry:
	ret = truncate(path, length);

	if (ret == -1 && errno == EINTR)
		goto retry;
#endif

	return ret;
}

/*
 * fsync_fname -- fsync a file or directory, handling errors properly
 *
 * Try to fsync a file or directory. When doing the latter, ignore errors that
 * indicate the OS just doesn't allow/require fsyncing directories.
 */
void
fsync_fname(const char *fname, bool isdir)
{
	fsync_fname_ext(fname, isdir, false, data_sync_elevel(ERROR));
}

/*
 * durable_rename -- rename(2) wrapper, issuing fsyncs required for durability
 *
 * This routine ensures that, after returning, the effect of renaming file
 * persists in case of a crash. A crash while this routine is running will
 * leave you with either the pre-existing or the moved file in place of the
 * new file; no mixed state or truncated files are possible.
 *
 * It does so by using fsync on the old filename and the possibly existing
 * target filename before the rename, and the target file and directory after.
 *
 * Note that rename() cannot be used across arbitrary directories, as they
 * might not be on the same filesystem. Therefore this routine does not
 * support renaming across directories.
 *
 * Log errors with the caller specified severity.
 *
 * Returns 0 if the operation succeeded, -1 otherwise. Note that errno is not
 * valid upon return.
 */
int
durable_rename(const char *oldfile, const char *newfile, int elevel)
{
	int			fd;

	/*
	 * First fsync the old and target path (if it exists), to ensure that they
	 * are properly persistent on disk. Syncing the target file is not
	 * strictly necessary, but it makes it easier to reason about crashes;
	 * because it's then guaranteed that either source or target file exists
	 * after a crash.
	 */
	if (fsync_fname_ext(oldfile, false, false, elevel) != 0)
		return -1;

	fd = OpenTransientFile(newfile, PG_BINARY | O_RDWR);
	if (fd < 0)
	{
		if (errno != ENOENT)
		{
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", newfile)));
			return -1;
		}
	}
	else
	{
		if (pg_fsync(fd) != 0)
		{
			int			save_errno;

			/* close file upon error, might not be in transaction context */
			save_errno = errno;
			CloseTransientFile(fd);
			errno = save_errno;

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m", newfile)));
			return -1;
		}

		if (CloseTransientFile(fd) != 0)
		{
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", newfile)));
			return -1;
		}
	}

	/* Time to do the real deal... */
	if (rename(oldfile, newfile) < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						oldfile, newfile)));
		return -1;
	}

	/*
	 * To guarantee renaming the file is persistent, fsync the file with its
	 * new name, and its containing directory.
	 */
	if (fsync_fname_ext(newfile, false, false, elevel) != 0)
		return -1;

	if (fsync_parent_path(newfile, elevel) != 0)
		return -1;

	return 0;
}

/*
 * durable_unlink -- remove a file in a durable manner
 *
 * This routine ensures that, after returning, the effect of removing file
 * persists in case of a crash. A crash while this routine is running will
 * leave the system in no mixed state.
 *
 * It does so by using fsync on the parent directory of the file after the
 * actual removal is done.
 *
 * Log errors with the severity specified by caller.
 *
 * Returns 0 if the operation succeeded, -1 otherwise. Note that errno is not
 * valid upon return.
 */
int
durable_unlink(const char *fname, int elevel)
{
	if (unlink(fname) < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						fname)));
		return -1;
	}

	/*
	 * To guarantee that the removal of the file is persistent, fsync its
	 * parent directory.
	 */
	if (fsync_parent_path(fname, elevel) != 0)
		return -1;

	return 0;
}

/*
 * InitFileAccess --- initialize this module during backend startup
 *
 * This is called during either normal or standalone backend start.
 * It is *not* called in the postmaster.
 *
 * Note that this does not initialize temporary file access, that is
 * separately initialized via InitTemporaryFileAccess().
 */
void
InitFileAccess(void)
{
	Assert(SizeVfdCache == 0);	/* call me only once */

	/* initialize cache header entry */
	VfdCache = (Vfd *) malloc(sizeof(Vfd));
	if (VfdCache == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	MemSet((char *) &(VfdCache[0]), 0, sizeof(Vfd));
	VfdCache->fd = VFD_CLOSED;

	SizeVfdCache = 1;
}

/*
 * InitTemporaryFileAccess --- initialize temporary file access during startup
 *
 * This is called during either normal or standalone backend start.
 * It is *not* called in the postmaster.
 *
 * This is separate from InitFileAccess() because temporary file cleanup can
 * cause pgstat reporting. As pgstat is shut down during before_shmem_exit(),
 * our reporting has to happen before that. Low level file access should be
 * available for longer, hence the separate initialization / shutdown of
 * temporary file handling.
 */
void
InitTemporaryFileAccess(void)
{
	Assert(SizeVfdCache != 0);	/* InitFileAccess() needs to have run */
	Assert(!temporary_files_allowed);	/* call me only once */

	/*
	 * Register before-shmem-exit hook to ensure temp files are dropped while
	 * we can still report stats.
	 */
	before_shmem_exit(BeforeShmemExit_Files, 0);

#ifdef USE_ASSERT_CHECKING
	temporary_files_allowed = true;
#endif
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
 * We assume stderr (FD 2) is available for dup'ing.  While the calling
 * script could theoretically close that, it would be a really bad idea,
 * since then one risks loss of error messages from, e.g., libc.
 */
static void
count_usable_fds(int max_to_probe, int *usable_fds, int *already_open)
{
	int		   *fd;
	int			size;
	int			used = 0;
	int			highestfd = 0;
	int			j;

#ifdef HAVE_GETRLIMIT
	struct rlimit rlim;
	int			getrlimit_status;
#endif

	size = 1024;
	fd = (int *) palloc(size * sizeof(int));

#ifdef HAVE_GETRLIMIT
	getrlimit_status = getrlimit(RLIMIT_NOFILE, &rlim);
	if (getrlimit_status != 0)
		ereport(WARNING, (errmsg("getrlimit failed: %m")));
#endif							/* HAVE_GETRLIMIT */

	/* dup until failure or probe limit reached */
	for (;;)
	{
		int			thisfd;

#ifdef HAVE_GETRLIMIT

		/*
		 * don't go beyond RLIMIT_NOFILE; causes irritating kernel logs on
		 * some platforms
		 */
		if (getrlimit_status == 0 && highestfd >= rlim.rlim_cur - 1)
			break;
#endif

		thisfd = dup(2);
		if (thisfd < 0)
		{
			/* Expect EMFILE or ENFILE, else it's fishy */
			if (errno != EMFILE && errno != ENFILE)
				elog(WARNING, "duplicating stderr file descriptor failed after %d successes: %m", used);
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
	 * Return results.  usable_fds is just the number of successful dups. We
	 * assume that the system limit is highestfd+1 (remember 0 is a legal FD
	 * number) and so already_open is highestfd+1 - usable_fds.
	 */
	*usable_fds = used;
	*already_open = highestfd + 1 - used;
}

/*
 * set_max_safe_fds
 *		Determine number of file descriptors that fd.c is allowed to use
 */
void
set_max_safe_fds(void)
{
	int			usable_fds;
	int			already_open;

	/*----------
	 * We want to set max_safe_fds to
	 *			MIN(usable_fds, max_files_per_process - already_open)
	 * less the slop factor for files that are opened without consulting
	 * fd.c.  This ensures that we won't exceed either max_files_per_process
	 * or the experimentally-determined EMFILE limit.
	 *----------
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
				 errdetail("System allows %d, server needs at least %d.",
						   max_safe_fds + NUM_RESERVED_FDS,
						   FD_MINFREE + NUM_RESERVED_FDS)));

	elog(DEBUG2, "max_safe_fds = %d, usable_fds = %d, already_open = %d",
		 max_safe_fds, usable_fds, already_open);
}

/*
 * Open a file with BasicOpenFilePerm() and pass default file mode for the
 * fileMode parameter.
 */
int
BasicOpenFile(const char *fileName, int fileFlags)
{
	return BasicOpenFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * BasicOpenFilePerm --- same as open(2) except can free other FDs if needed
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
BasicOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	int			fd;

tryAgain:
#ifdef PG_O_DIRECT_USE_F_NOCACHE

	/*
	 * The value we defined to stand in for O_DIRECT when simulating it with
	 * F_NOCACHE had better not collide with any of the standard flags.
	 */
	StaticAssertStmt((PG_O_DIRECT &
					  (O_APPEND |
					   O_CLOEXEC |
					   O_CREAT |
					   O_DSYNC |
					   O_EXCL |
					   O_RDWR |
					   O_RDONLY |
					   O_SYNC |
					   O_TRUNC |
					   O_WRONLY)) == 0,
					 "PG_O_DIRECT value collides with standard flag");
	fd = open(fileName, fileFlags & ~PG_O_DIRECT, fileMode);
#else
	fd = open(fileName, fileFlags, fileMode);
#endif

	if (fd >= 0)
	{
#ifdef PG_O_DIRECT_USE_F_NOCACHE
		if (fileFlags & PG_O_DIRECT)
		{
			if (fcntl(fd, F_NOCACHE, 1) < 0)
			{
				int			save_errno = errno;

				close(fd);
				errno = save_errno;
				return -1;
			}
		}
#endif

		return fd;				/* success! */
	}

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

/*
 * AcquireExternalFD - attempt to reserve an external file descriptor
 *
 * This should be used by callers that need to hold a file descriptor open
 * over more than a short interval, but cannot use any of the other facilities
 * provided by this module.
 *
 * The difference between this and the underlying ReserveExternalFD function
 * is that this will report failure (by setting errno and returning false)
 * if "too many" external FDs are already reserved.  This should be used in
 * any code where the total number of FDs to be reserved is not predictable
 * and small.
 */
bool
AcquireExternalFD(void)
{
	/*
	 * We don't want more than max_safe_fds / 3 FDs to be consumed for
	 * "external" FDs.
	 */
	if (numExternalFDs < max_safe_fds / 3)
	{
		ReserveExternalFD();
		return true;
	}
	errno = EMFILE;
	return false;
}

/*
 * ReserveExternalFD - report external consumption of a file descriptor
 *
 * This should be used by callers that need to hold a file descriptor open
 * over more than a short interval, but cannot use any of the other facilities
 * provided by this module.  This just tracks the use of the FD and closes
 * VFDs if needed to ensure we keep NUM_RESERVED_FDS FDs available.
 *
 * Call this directly only in code where failure to reserve the FD would be
 * fatal; for example, the WAL-writing code does so, since the alternative is
 * session failure.  Also, it's very unwise to do so in code that could
 * consume more than one FD per process.
 *
 * Note: as long as everybody plays nice so that NUM_RESERVED_FDS FDs remain
 * available, it doesn't matter too much whether this is called before or
 * after actually opening the FD; but doing so beforehand reduces the risk of
 * an EMFILE failure if not everybody played nice.  In any case, it's solely
 * caller's responsibility to keep the external-FD count in sync with reality.
 */
void
ReserveExternalFD(void)
{
	/*
	 * Release VFDs if needed to stay safe.  Because we do this before
	 * incrementing numExternalFDs, the final state will be as desired, i.e.,
	 * nfile + numAllocatedDescs + numExternalFDs <= max_safe_fds.
	 */
	ReleaseLruFiles();

	numExternalFDs++;
}

/*
 * ReleaseExternalFD - report release of an external file descriptor
 *
 * This is guaranteed not to change errno, so it can be used in failure paths.
 */
void
ReleaseExternalFD(void)
{
	Assert(numExternalFDs > 0);
	numExternalFDs--;
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
	elog(LOG, "%s", buf);
}
#endif							/* FDDEBUG */

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

	/*
	 * Close the file.  We aren't expecting this to fail; if it does, better
	 * to leak the FD than to mess up our internal state.
	 */
	if (close(vfdP->fd) != 0)
		elog(vfdP->fdstate & FD_TEMP_FILE_LIMIT ? LOG : data_sync_elevel(LOG),
			 "could not close file \"%s\": %m", vfdP->fileName);
	vfdP->fd = VFD_CLOSED;
	--nfile;

	/* delete the vfd record from the LRU ring */
	Delete(file);
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

/* returns 0 on success, -1 on re-open failure (with errno set) */
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
		/* Close excess kernel FDs. */
		ReleaseLruFiles();

		/*
		 * The open could still fail for lack of file descriptors, eg due to
		 * overall system file table being full.  So, be prepared to release
		 * another FD if necessary...
		 */
		vfdP->fd = BasicOpenFilePerm(vfdP->fileName, vfdP->fileFlags,
									 vfdP->fileMode);
		if (vfdP->fd < 0)
		{
			DO_DB(elog(LOG, "re-open failed: %m"));
			return -1;
		}
		else
		{
			++nfile;
		}
	}

	/*
	 * put it at the head of the Lru ring
	 */

	Insert(file);

	return 0;
}

/*
 * Release one kernel FD by closing the least-recently-used VFD.
 */
static bool
ReleaseLruFile(void)
{
	DO_DB(elog(LOG, "ReleaseLruFile. Opened %d", nfile));

	if (nfile > 0)
	{
		/*
		 * There are opened files and so there should be at least one used vfd
		 * in the ring.
		 */
		Assert(VfdCache[0].lruMoreRecently != 0);
		LruDelete(VfdCache[0].lruMoreRecently);
		return true;			/* freed a file */
	}
	return false;				/* no files available to free */
}

/*
 * Release kernel FDs as needed to get under the max_safe_fds limit.
 * After calling this, it's OK to try to open another file.
 */
static void
ReleaseLruFiles(void)
{
	while (nfile + numAllocatedDescs + numExternalFDs >= max_safe_fds)
	{
		if (!ReleaseLruFile())
			break;
	}
}

static File
AllocateVfd(void)
{
	Index		i;
	File		file;

	DO_DB(elog(LOG, "AllocateVfd. Size %zu", SizeVfdCache));

	Assert(SizeVfdCache > 0);	/* InitFileAccess not called? */

	if (VfdCache[0].nextFree == 0)
	{
		/*
		 * The free list is empty so it is time to increase the size of the
		 * array.  We choose to double it each time this happens. However,
		 * there's not much point in starting *real* small.
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

/* returns 0 on success, -1 on re-open failure (with errno set) */
static int
FileAccess(File file)
{
	int			returnValue;

	DO_DB(elog(LOG, "FileAccess %d (%s)",
			   file, VfdCache[file].fileName));

	/*
	 * Is the file open?  If not, open it and put it at the head of the LRU
	 * ring (possibly closing the least recently used file to get an FD).
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
		 * We now know that the file is open and that it is not the last one
		 * accessed, so we need to move it to the head of the Lru ring.
		 */

		Delete(file);
		Insert(file);
	}

	return 0;
}

/*
 * Called whenever a temporary file is deleted to report its size.
 */
static void
ReportTemporaryFileUsage(const char *path, off_t size)
{
	pgstat_report_tempfile(size);

	if (log_temp_files >= 0)
	{
		if ((size / 1024) >= log_temp_files)
			ereport(LOG,
					(errmsg("temporary file: path \"%s\", size %lu",
							path, (unsigned long) size)));
	}
}

/*
 * Called to register a temporary file for automatic close.
 * ResourceOwnerEnlarge(CurrentResourceOwner) must have been called
 * before the file was opened.
 */
static void
RegisterTemporaryFile(File file)
{
	ResourceOwnerRememberFile(CurrentResourceOwner, file);
	VfdCache[file].resowner = CurrentResourceOwner;

	/* Backup mechanism for closing at end of xact. */
	VfdCache[file].fdstate |= FD_CLOSE_AT_EOXACT;
	have_xact_temporary_files = true;
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

/*
 * Open a file with PathNameOpenFilePerm() and pass default file mode for the
 * fileMode parameter.
 */
File
PathNameOpenFile(const char *fileName, int fileFlags)
{
	return PathNameOpenFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * open a file in an arbitrary directory
 *
 * NB: if the passed pathname is relative (which it usually is),
 * it will be interpreted relative to the process' working directory
 * (which should always be $PGDATA when this code is running).
 */
File
PathNameOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	char	   *fnamecopy;
	File		file;
	Vfd		   *vfdP;

	DO_DB(elog(LOG, "PathNameOpenFilePerm: %s %x %o",
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

	/* Close excess kernel FDs. */
	ReleaseLruFiles();

	/*
	 * Descriptors managed by VFDs are implicitly marked O_CLOEXEC.  The
	 * client shouldn't be expected to know which kernel descriptors are
	 * currently open, so it wouldn't make sense for them to be inherited by
	 * executed subprograms.
	 */
	fileFlags |= O_CLOEXEC;

	vfdP->fd = BasicOpenFilePerm(fileName, fileFlags, fileMode);

	if (vfdP->fd < 0)
	{
		int			save_errno = errno;

		FreeVfd(file);
		free(fnamecopy);
		errno = save_errno;
		return -1;
	}
	++nfile;
	DO_DB(elog(LOG, "PathNameOpenFile: success %d",
			   vfdP->fd));

	vfdP->fileName = fnamecopy;
	/* Saved flags are adjusted to be OK for re-opening file */
	vfdP->fileFlags = fileFlags & ~(O_CREAT | O_TRUNC | O_EXCL);
	vfdP->fileMode = fileMode;
	vfdP->fileSize = 0;
	vfdP->fdstate = 0x0;
	vfdP->resowner = NULL;

	Insert(file);

	return file;
}

/*
 * Create directory 'directory'.  If necessary, create 'basedir', which must
 * be the directory above it.  This is designed for creating the top-level
 * temporary directory on demand before creating a directory underneath it.
 * Do nothing if the directory already exists.
 *
 * Directories created within the top-level temporary directory should begin
 * with PG_TEMP_FILE_PREFIX, so that they can be identified as temporary and
 * deleted at startup by RemovePgTempFiles().  Further subdirectories below
 * that do not need any particular prefix.
*/
void
PathNameCreateTemporaryDir(const char *basedir, const char *directory)
{
	if (MakePGDirectory(directory) < 0)
	{
		if (errno == EEXIST)
			return;

		/*
		 * Failed.  Try to create basedir first in case it's missing. Tolerate
		 * EEXIST to close a race against another process following the same
		 * algorithm.
		 */
		if (MakePGDirectory(basedir) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("cannot create temporary directory \"%s\": %m",
							basedir)));

		/* Try again. */
		if (MakePGDirectory(directory) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("cannot create temporary subdirectory \"%s\": %m",
							directory)));
	}
}

/*
 * Delete a directory and everything in it, if it exists.
 */
void
PathNameDeleteTemporaryDir(const char *dirname)
{
	struct stat statbuf;

	/* Silently ignore missing directory. */
	if (stat(dirname, &statbuf) != 0 && errno == ENOENT)
		return;

	/*
	 * Currently, walkdir doesn't offer a way for our passed in function to
	 * maintain state.  Perhaps it should, so that we could tell the caller
	 * whether this operation succeeded or failed.  Since this operation is
	 * used in a cleanup path, we wouldn't actually behave differently: we'll
	 * just log failures.
	 */
	walkdir(dirname, unlink_if_exists_fname, false, LOG);
}

/*
 * Open a temporary file that will disappear when we close it.
 *
 * This routine takes care of generating an appropriate tempfile name.
 * There's no need to pass in fileFlags or fileMode either, since only
 * one setting makes any sense for a temp file.
 *
 * Unless interXact is true, the file is remembered by CurrentResourceOwner
 * to ensure it's closed and deleted when it's no longer needed, typically at
 * the end-of-transaction. In most cases, you don't want temporary files to
 * outlive the transaction that created them, so this should be false -- but
 * if you need "somewhat" temporary storage, this might be useful. In either
 * case, the file is removed when the File is explicitly closed.
 */
File
OpenTemporaryFile(bool interXact)
{
	File		file = 0;

	Assert(temporary_files_allowed);	/* check temp file access is up */

	/*
	 * Make sure the current resource owner has space for this File before we
	 * open it, if we'll be registering it below.
	 */
	if (!interXact)
		ResourceOwnerEnlarge(CurrentResourceOwner);

	/*
	 * If some temp tablespace(s) have been given to us, try to use the next
	 * one.  If a given tablespace can't be found, we silently fall back to
	 * the database's default tablespace.
	 *
	 * BUT: if the temp file is slated to outlive the current transaction,
	 * force it into the database's default tablespace, so that it will not
	 * pose a threat to possible tablespace drop attempts.
	 */
	if (numTempTableSpaces > 0 && !interXact)
	{
		Oid			tblspcOid = GetNextTempTableSpace();

		if (OidIsValid(tblspcOid))
			file = OpenTemporaryFileInTablespace(tblspcOid, false);
	}

	/*
	 * If not, or if tablespace is bad, create in database's default
	 * tablespace.  MyDatabaseTableSpace should normally be set before we get
	 * here, but just in case it isn't, fall back to pg_default tablespace.
	 */
	if (file <= 0)
		file = OpenTemporaryFileInTablespace(MyDatabaseTableSpace ?
											 MyDatabaseTableSpace :
											 DEFAULTTABLESPACE_OID,
											 true);

	/* Mark it for deletion at close and temporary file size limit */
	VfdCache[file].fdstate |= FD_DELETE_AT_CLOSE | FD_TEMP_FILE_LIMIT;

	/* Register it with the current resource owner */
	if (!interXact)
		RegisterTemporaryFile(file);

	return file;
}

/*
 * Return the path of the temp directory in a given tablespace.
 */
void
TempTablespacePath(char *path, Oid tablespace)
{
	/*
	 * Identify the tempfile directory for this tablespace.
	 *
	 * If someone tries to specify pg_global, use pg_default instead.
	 */
	if (tablespace == InvalidOid ||
		tablespace == DEFAULTTABLESPACE_OID ||
		tablespace == GLOBALTABLESPACE_OID)
		snprintf(path, MAXPGPATH, "base/%s", PG_TEMP_FILES_DIR);
	else
	{
		/* All other tablespaces are accessed via symlinks */
		snprintf(path, MAXPGPATH, "%s/%u/%s/%s",
				 PG_TBLSPC_DIR, tablespace, TABLESPACE_VERSION_DIRECTORY,
				 PG_TEMP_FILES_DIR);
	}
}

/*
 * Open a temporary file in a specific tablespace.
 * Subroutine for OpenTemporaryFile, which see for details.
 */
static File
OpenTemporaryFileInTablespace(Oid tblspcOid, bool rejectError)
{
	char		tempdirpath[MAXPGPATH];
	char		tempfilepath[MAXPGPATH];
	File		file;

	TempTablespacePath(tempdirpath, tblspcOid);

	/*
	 * Generate a tempfile name that should be unique within the current
	 * database instance.
	 */
	snprintf(tempfilepath, sizeof(tempfilepath), "%s/%s%d.%ld",
			 tempdirpath, PG_TEMP_FILE_PREFIX, MyProcPid, tempFileCounter++);

	/*
	 * Open the file.  Note: we don't use O_EXCL, in case there is an orphaned
	 * temp file that can be reused.
	 */
	file = PathNameOpenFile(tempfilepath,
							O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (file <= 0)
	{
		/*
		 * We might need to create the tablespace's tempfile directory, if no
		 * one has yet done so.
		 *
		 * Don't check for an error from MakePGDirectory; it could fail if
		 * someone else just did the same thing.  If it doesn't work then
		 * we'll bomb out on the second create attempt, instead.
		 */
		(void) MakePGDirectory(tempdirpath);

		file = PathNameOpenFile(tempfilepath,
								O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
		if (file <= 0 && rejectError)
			elog(ERROR, "could not create temporary file \"%s\": %m",
				 tempfilepath);
	}

	return file;
}


/*
 * Create a new file.  The directory containing it must already exist.  Files
 * created this way are subject to temp_file_limit and are automatically
 * closed at end of transaction, but are not automatically deleted on close
 * because they are intended to be shared between cooperating backends.
 *
 * If the file is inside the top-level temporary directory, its name should
 * begin with PG_TEMP_FILE_PREFIX so that it can be identified as temporary
 * and deleted at startup by RemovePgTempFiles().  Alternatively, it can be
 * inside a directory created with PathNameCreateTemporaryDir(), in which case
 * the prefix isn't needed.
 */
File
PathNameCreateTemporaryFile(const char *path, bool error_on_failure)
{
	File		file;

	Assert(temporary_files_allowed);	/* check temp file access is up */

	ResourceOwnerEnlarge(CurrentResourceOwner);

	/*
	 * Open the file.  Note: we don't use O_EXCL, in case there is an orphaned
	 * temp file that can be reused.
	 */
	file = PathNameOpenFile(path, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (file <= 0)
	{
		if (error_on_failure)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create temporary file \"%s\": %m",
							path)));
		else
			return file;
	}

	/* Mark it for temp_file_limit accounting. */
	VfdCache[file].fdstate |= FD_TEMP_FILE_LIMIT;

	/* Register it for automatic close. */
	RegisterTemporaryFile(file);

	return file;
}

/*
 * Open a file that was created with PathNameCreateTemporaryFile, possibly in
 * another backend.  Files opened this way don't count against the
 * temp_file_limit of the caller, are automatically closed at the end of the
 * transaction but are not deleted on close.
 */
File
PathNameOpenTemporaryFile(const char *path, int mode)
{
	File		file;

	Assert(temporary_files_allowed);	/* check temp file access is up */

	ResourceOwnerEnlarge(CurrentResourceOwner);

	file = PathNameOpenFile(path, mode | PG_BINARY);

	/* If no such file, then we don't raise an error. */
	if (file <= 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open temporary file \"%s\": %m",
						path)));

	if (file > 0)
	{
		/* Register it for automatic close. */
		RegisterTemporaryFile(file);
	}

	return file;
}

/*
 * Delete a file by pathname.  Return true if the file existed, false if
 * didn't.
 */
bool
PathNameDeleteTemporaryFile(const char *path, bool error_on_failure)
{
	struct stat filestats;
	int			stat_errno;

	/* Get the final size for pgstat reporting. */
	if (stat(path, &filestats) != 0)
		stat_errno = errno;
	else
		stat_errno = 0;

	/*
	 * Unlike FileClose's automatic file deletion code, we tolerate
	 * non-existence to support BufFileDeleteFileSet which doesn't know how
	 * many segments it has to delete until it runs out.
	 */
	if (stat_errno == ENOENT)
		return false;

	if (unlink(path) < 0)
	{
		if (errno != ENOENT)
			ereport(error_on_failure ? ERROR : LOG,
					(errcode_for_file_access(),
					 errmsg("could not unlink temporary file \"%s\": %m",
							path)));
		return false;
	}

	if (stat_errno == 0)
		ReportTemporaryFileUsage(path, filestats.st_size);
	else
	{
		errno = stat_errno;
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	}

	return true;
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
		/* close the file */
		if (close(vfdP->fd) != 0)
		{
			/*
			 * We may need to panic on failure to close non-temporary files;
			 * see LruDelete.
			 */
			elog(vfdP->fdstate & FD_TEMP_FILE_LIMIT ? LOG : data_sync_elevel(LOG),
				 "could not close file \"%s\": %m", vfdP->fileName);
		}

		--nfile;
		vfdP->fd = VFD_CLOSED;

		/* remove the file from the lru ring */
		Delete(file);
	}

	if (vfdP->fdstate & FD_TEMP_FILE_LIMIT)
	{
		/* Subtract its size from current usage (do first in case of error) */
		temporary_files_size -= vfdP->fileSize;
		vfdP->fileSize = 0;
	}

	/*
	 * Delete the file if it was temporary, and make a log entry if wanted
	 */
	if (vfdP->fdstate & FD_DELETE_AT_CLOSE)
	{
		struct stat filestats;
		int			stat_errno;

		/*
		 * If we get an error, as could happen within the ereport/elog calls,
		 * we'll come right back here during transaction abort.  Reset the
		 * flag to ensure that we can't get into an infinite loop.  This code
		 * is arranged to ensure that the worst-case consequence is failing to
		 * emit log message(s), not failing to attempt the unlink.
		 */
		vfdP->fdstate &= ~FD_DELETE_AT_CLOSE;


		/* first try the stat() */
		if (stat(vfdP->fileName, &filestats))
			stat_errno = errno;
		else
			stat_errno = 0;

		/* in any case do the unlink */
		if (unlink(vfdP->fileName))
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not delete file \"%s\": %m", vfdP->fileName)));

		/* and last report the stat results */
		if (stat_errno == 0)
			ReportTemporaryFileUsage(vfdP->fileName, filestats.st_size);
		else
		{
			errno = stat_errno;
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", vfdP->fileName)));
		}
	}

	/* Unregister it from the resource owner */
	if (vfdP->resowner)
		ResourceOwnerForgetFile(vfdP->resowner, file);

	/*
	 * Return the Vfd slot to the free list
	 */
	FreeVfd(file);
}

/*
 * FilePrefetch - initiate asynchronous read of a given range of the file.
 *
 * Returns 0 on success, otherwise an errno error code (like posix_fadvise()).
 *
 * posix_fadvise() is the simplest standardized interface that accomplishes
 * this.
 */
int
FilePrefetch(File file, off_t offset, off_t amount, uint32 wait_event_info)
{
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FilePrefetch: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_WILLNEED)
	{
		int			returnCode;

		returnCode = FileAccess(file);
		if (returnCode < 0)
			return returnCode;

retry:
		pgstat_report_wait_start(wait_event_info);
		returnCode = posix_fadvise(VfdCache[file].fd, offset, amount,
								   POSIX_FADV_WILLNEED);
		pgstat_report_wait_end();

		if (returnCode == EINTR)
			goto retry;

		return returnCode;
	}
#elif defined(__darwin__)
	{
		struct radvisory
		{
			off_t		ra_offset;	/* offset into the file */
			int			ra_count;	/* size of the read     */
		}			ra;
		int			returnCode;

		returnCode = FileAccess(file);
		if (returnCode < 0)
			return returnCode;

		ra.ra_offset = offset;
		ra.ra_count = amount;
		pgstat_report_wait_start(wait_event_info);
		returnCode = fcntl(VfdCache[file].fd, F_RDADVISE, &ra);
		pgstat_report_wait_end();
		if (returnCode != -1)
			return 0;
		else
			return errno;
	}
#else
	return 0;
#endif
}

void
FileWriteback(File file, off_t offset, off_t nbytes, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileWriteback: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) nbytes));

	if (nbytes <= 0)
		return;

	if (VfdCache[file].fileFlags & PG_O_DIRECT)
		return;

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return;

	pgstat_report_wait_start(wait_event_info);
	pg_flush_data(VfdCache[file].fd, offset, nbytes);
	pgstat_report_wait_end();
}

ssize_t
FileReadV(File file, const struct iovec *iov, int iovcnt, off_t offset,
		  uint32 wait_event_info)
{
	ssize_t		returnCode;
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileReadV: %d (%s) " INT64_FORMAT " %d",
			   file, VfdCache[file].fileName,
			   (int64) offset,
			   iovcnt));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	vfdP = &VfdCache[file];

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_preadv(vfdP->fd, iov, iovcnt, offset);
	pgstat_report_wait_end();

	if (returnCode < 0)
	{
		/*
		 * Windows may run out of kernel buffers and return "Insufficient
		 * system resources" error.  Wait a bit and retry to solve it.
		 *
		 * It is rumored that EINTR is also possible on some Unix filesystems,
		 * in which case immediate retry is indicated.
		 */
#ifdef WIN32
		DWORD		error = GetLastError();

		switch (error)
		{
			case ERROR_NO_SYSTEM_RESOURCES:
				pg_usleep(1000L);
				errno = EINTR;
				break;
			default:
				_dosmaperr(error);
				break;
		}
#endif
		/* OK to retry if interrupted */
		if (errno == EINTR)
			goto retry;
	}

	return returnCode;
}

ssize_t
FileWriteV(File file, const struct iovec *iov, int iovcnt, off_t offset,
		   uint32 wait_event_info)
{
	ssize_t		returnCode;
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileWriteV: %d (%s) " INT64_FORMAT " %d",
			   file, VfdCache[file].fileName,
			   (int64) offset,
			   iovcnt));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	vfdP = &VfdCache[file];

	/*
	 * If enforcing temp_file_limit and it's a temp file, check to see if the
	 * write would overrun temp_file_limit, and throw error if so.  Note: it's
	 * really a modularity violation to throw error here; we should set errno
	 * and return -1.  However, there's no way to report a suitable error
	 * message if we do that.  All current callers would just throw error
	 * immediately anyway, so this is safe at present.
	 */
	if (temp_file_limit >= 0 && (vfdP->fdstate & FD_TEMP_FILE_LIMIT))
	{
		off_t		past_write = offset;

		for (int i = 0; i < iovcnt; ++i)
			past_write += iov[i].iov_len;

		if (past_write > vfdP->fileSize)
		{
			uint64		newTotal = temporary_files_size;

			newTotal += past_write - vfdP->fileSize;
			if (newTotal > (uint64) temp_file_limit * (uint64) 1024)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
						 errmsg("temporary file size exceeds \"temp_file_limit\" (%dkB)",
								temp_file_limit)));
		}
	}

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_pwritev(vfdP->fd, iov, iovcnt, offset);
	pgstat_report_wait_end();

	if (returnCode >= 0)
	{
		/*
		 * Some callers expect short writes to set errno, and traditionally we
		 * have assumed that they imply disk space shortage.  We don't want to
		 * waste CPU cycles adding up the total size here, so we'll just set
		 * it for all successful writes in case such a caller determines that
		 * the write was short and ereports "%m".
		 */
		errno = ENOSPC;

		/*
		 * Maintain fileSize and temporary_files_size if it's a temp file.
		 */
		if (vfdP->fdstate & FD_TEMP_FILE_LIMIT)
		{
			off_t		past_write = offset + returnCode;

			if (past_write > vfdP->fileSize)
			{
				temporary_files_size += past_write - vfdP->fileSize;
				vfdP->fileSize = past_write;
			}
		}
	}
	else
	{
		/*
		 * See comments in FileReadV()
		 */
#ifdef WIN32
		DWORD		error = GetLastError();

		switch (error)
		{
			case ERROR_NO_SYSTEM_RESOURCES:
				pg_usleep(1000L);
				errno = EINTR;
				break;
			default:
				_dosmaperr(error);
				break;
		}
#endif
		/* OK to retry if interrupted */
		if (errno == EINTR)
			goto retry;
	}

	return returnCode;
}

int
FileSync(File file, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSync: %d (%s)",
			   file, VfdCache[file].fileName));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_fsync(VfdCache[file].fd);
	pgstat_report_wait_end();

	return returnCode;
}

/*
 * Zero a region of the file.
 *
 * Returns 0 on success, -1 otherwise. In the latter case errno is set to the
 * appropriate error.
 */
int
FileZero(File file, off_t offset, off_t amount, uint32 wait_event_info)
{
	int			returnCode;
	ssize_t		written;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileZero: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	written = pg_pwrite_zeros(VfdCache[file].fd, amount, offset);
	pgstat_report_wait_end();

	if (written < 0)
		return -1;
	else if (written != amount)
	{
		/* if errno is unset, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		return -1;
	}

	return 0;
}

/*
 * Try to reserve file space with posix_fallocate(). If posix_fallocate() is
 * not implemented on the operating system or fails with EINVAL / EOPNOTSUPP,
 * use FileZero() instead.
 *
 * Note that at least glibc() implements posix_fallocate() in userspace if not
 * implemented by the filesystem. That's not the case for all environments
 * though.
 *
 * Returns 0 on success, -1 otherwise. In the latter case errno is set to the
 * appropriate error.
 */
int
FileFallocate(File file, off_t offset, off_t amount, uint32 wait_event_info)
{
#ifdef HAVE_POSIX_FALLOCATE
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileFallocate: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return -1;

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = posix_fallocate(VfdCache[file].fd, offset, amount);
	pgstat_report_wait_end();

	if (returnCode == 0)
		return 0;
	else if (returnCode == EINTR)
		goto retry;

	/* for compatibility with %m printing etc */
	errno = returnCode;

	/*
	 * Return in cases of a "real" failure, if fallocate is not supported,
	 * fall through to the FileZero() backed implementation.
	 */
	if (returnCode != EINVAL && returnCode != EOPNOTSUPP)
		return -1;
#endif

	return FileZero(file, offset, amount, wait_event_info);
}

off_t
FileSize(File file)
{
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSize %d (%s)",
			   file, VfdCache[file].fileName));

	if (FileIsNotOpen(file))
	{
		if (FileAccess(file) < 0)
			return (off_t) -1;
	}

	return lseek(VfdCache[file].fd, 0, SEEK_END);
}

int
FileTruncate(File file, off_t offset, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileTruncate %d (%s)",
			   file, VfdCache[file].fileName));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_ftruncate(VfdCache[file].fd, offset);
	pgstat_report_wait_end();

	if (returnCode == 0 && VfdCache[file].fileSize > offset)
	{
		/* adjust our state for truncation of a temp file */
		Assert(VfdCache[file].fdstate & FD_TEMP_FILE_LIMIT);
		temporary_files_size -= VfdCache[file].fileSize - offset;
		VfdCache[file].fileSize = offset;
	}

	return returnCode;
}

/*
 * Return the pathname associated with an open file.
 *
 * The returned string points to an internal buffer, which is valid until
 * the file is closed.
 */
char *
FilePathName(File file)
{
	Assert(FileIsValid(file));

	return VfdCache[file].fileName;
}

/*
 * Return the raw file descriptor of an opened file.
 *
 * The returned file descriptor will be valid until the file is closed, but
 * there are a lot of things that can make that happen.  So the caller should
 * be careful not to do much of anything else before it finishes using the
 * returned file descriptor.
 */
int
FileGetRawDesc(File file)
{
	Assert(FileIsValid(file));
	return VfdCache[file].fd;
}

/*
 * FileGetRawFlags - returns the file flags on open(2)
 */
int
FileGetRawFlags(File file)
{
	Assert(FileIsValid(file));
	return VfdCache[file].fileFlags;
}

/*
 * FileGetRawMode - returns the mode bitmask passed to open(2)
 */
mode_t
FileGetRawMode(File file)
{
	Assert(FileIsValid(file));
	return VfdCache[file].fileMode;
}

/*
 * Make room for another allocatedDescs[] array entry if needed and possible.
 * Returns true if an array element is available.
 */
static bool
reserveAllocatedDesc(void)
{
	AllocateDesc *newDescs;
	int			newMax;

	/* Quick out if array already has a free slot. */
	if (numAllocatedDescs < maxAllocatedDescs)
		return true;

	/*
	 * If the array hasn't yet been created in the current process, initialize
	 * it with FD_MINFREE / 3 elements.  In many scenarios this is as many as
	 * we will ever need, anyway.  We don't want to look at max_safe_fds
	 * immediately because set_max_safe_fds() may not have run yet.
	 */
	if (allocatedDescs == NULL)
	{
		newMax = FD_MINFREE / 3;
		newDescs = (AllocateDesc *) malloc(newMax * sizeof(AllocateDesc));
		/* Out of memory already?  Treat as fatal error. */
		if (newDescs == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		allocatedDescs = newDescs;
		maxAllocatedDescs = newMax;
		return true;
	}

	/*
	 * Consider enlarging the array beyond the initial allocation used above.
	 * By the time this happens, max_safe_fds should be known accurately.
	 *
	 * We mustn't let allocated descriptors hog all the available FDs, and in
	 * practice we'd better leave a reasonable number of FDs for VFD use.  So
	 * set the maximum to max_safe_fds / 3.  (This should certainly be at
	 * least as large as the initial size, FD_MINFREE / 3, so we aren't
	 * tightening the restriction here.)  Recall that "external" FDs are
	 * allowed to consume another third of max_safe_fds.
	 */
	newMax = max_safe_fds / 3;
	if (newMax > maxAllocatedDescs)
	{
		newDescs = (AllocateDesc *) realloc(allocatedDescs,
											newMax * sizeof(AllocateDesc));
		/* Treat out-of-memory as a non-fatal error. */
		if (newDescs == NULL)
			return false;
		allocatedDescs = newDescs;
		maxAllocatedDescs = newMax;
		return true;
	}

	/* Can't enlarge allocatedDescs[] any more. */
	return false;
}

/*
 * Routines that want to use stdio (ie, FILE*) should use AllocateFile
 * rather than plain fopen().  This lets fd.c deal with freeing FDs if
 * necessary to open the file.  When done, call FreeFile rather than fclose.
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
AllocateFile(const char *name, const char *mode)
{
	FILE	   *file;

	DO_DB(elog(LOG, "AllocateFile: Allocated %d (%s)",
			   numAllocatedDescs, name));

	/* Can we allocate another non-virtual FD? */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open file \"%s\"",
						maxAllocatedDescs, name)));

	/* Close excess kernel FDs. */
	ReleaseLruFiles();

TryAgain:
	if ((file = fopen(name, mode)) != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescFile;
		desc->desc.file = file;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.file;
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
 * Open a file with OpenTransientFilePerm() and pass default file mode for
 * the fileMode parameter.
 */
int
OpenTransientFile(const char *fileName, int fileFlags)
{
	return OpenTransientFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * Like AllocateFile, but returns an unbuffered fd like open(2)
 */
int
OpenTransientFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	int			fd;

	DO_DB(elog(LOG, "OpenTransientFile: Allocated %d (%s)",
			   numAllocatedDescs, fileName));

	/* Can we allocate another non-virtual FD? */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open file \"%s\"",
						maxAllocatedDescs, fileName)));

	/* Close excess kernel FDs. */
	ReleaseLruFiles();

	fd = BasicOpenFilePerm(fileName, fileFlags, fileMode);

	if (fd >= 0)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescRawFD;
		desc->desc.fd = fd;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;

		return fd;
	}

	return -1;					/* failure */
}

/*
 * Routines that want to initiate a pipe stream should use OpenPipeStream
 * rather than plain popen().  This lets fd.c deal with freeing FDs if
 * necessary.  When done, call ClosePipeStream rather than pclose.
 *
 * This function also ensures that the popen'd program is run with default
 * SIGPIPE processing, rather than the SIG_IGN setting the backend normally
 * uses.  This ensures desirable response to, eg, closing a read pipe early.
 */
FILE *
OpenPipeStream(const char *command, const char *mode)
{
	FILE	   *file;
	int			save_errno;

	DO_DB(elog(LOG, "OpenPipeStream: Allocated %d (%s)",
			   numAllocatedDescs, command));

	/* Can we allocate another non-virtual FD? */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to execute command \"%s\"",
						maxAllocatedDescs, command)));

	/* Close excess kernel FDs. */
	ReleaseLruFiles();

TryAgain:
	fflush(NULL);
	pqsignal(SIGPIPE, SIG_DFL);
	errno = 0;
	file = popen(command, mode);
	save_errno = errno;
	pqsignal(SIGPIPE, SIG_IGN);
	errno = save_errno;
	if (file != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescPipe;
		desc->desc.file = file;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.file;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * Free an AllocateDesc of any type.
 *
 * The argument *must* point into the allocatedDescs[] array.
 */
static int
FreeDesc(AllocateDesc *desc)
{
	int			result;

	/* Close the underlying object */
	switch (desc->kind)
	{
		case AllocateDescFile:
			result = fclose(desc->desc.file);
			break;
		case AllocateDescPipe:
			result = pclose(desc->desc.file);
			break;
		case AllocateDescDir:
			result = closedir(desc->desc.dir);
			break;
		case AllocateDescRawFD:
			result = close(desc->desc.fd);
			break;
		default:
			elog(ERROR, "AllocateDesc kind not recognized");
			result = 0;			/* keep compiler quiet */
			break;
	}

	/* Compact storage in the allocatedDescs array */
	numAllocatedDescs--;
	*desc = allocatedDescs[numAllocatedDescs];

	return result;
}

/*
 * Close a file returned by AllocateFile.
 *
 * Note we do not check fclose's return value --- it is up to the caller
 * to handle close errors.
 */
int
FreeFile(FILE *file)
{
	int			i;

	DO_DB(elog(LOG, "FreeFile: Allocated %d", numAllocatedDescs));

	/* Remove file from list of allocated files, if it's present */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescFile && desc->desc.file == file)
			return FreeDesc(desc);
	}

	/* Only get here if someone passes us a file not in allocatedDescs */
	elog(WARNING, "file passed to FreeFile was not obtained from AllocateFile");

	return fclose(file);
}

/*
 * Close a file returned by OpenTransientFile.
 *
 * Note we do not check close's return value --- it is up to the caller
 * to handle close errors.
 */
int
CloseTransientFile(int fd)
{
	int			i;

	DO_DB(elog(LOG, "CloseTransientFile: Allocated %d", numAllocatedDescs));

	/* Remove fd from list of allocated files, if it's present */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescRawFD && desc->desc.fd == fd)
			return FreeDesc(desc);
	}

	/* Only get here if someone passes us a file not in allocatedDescs */
	elog(WARNING, "fd passed to CloseTransientFile was not obtained from OpenTransientFile");

	return close(fd);
}

/*
 * Routines that want to use <dirent.h> (ie, DIR*) should use AllocateDir
 * rather than plain opendir().  This lets fd.c deal with freeing FDs if
 * necessary to open the directory, and with closing it after an elog.
 * When done, call FreeDir rather than closedir.
 *
 * Returns NULL, with errno set, on failure.  Note that failure detection
 * is commonly left to the following call of ReadDir or ReadDirExtended;
 * see the comments for ReadDir.
 *
 * Ideally this should be the *only* direct call of opendir() in the backend.
 */
DIR *
AllocateDir(const char *dirname)
{
	DIR		   *dir;

	DO_DB(elog(LOG, "AllocateDir: Allocated %d (%s)",
			   numAllocatedDescs, dirname));

	/* Can we allocate another non-virtual FD? */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open directory \"%s\"",
						maxAllocatedDescs, dirname)));

	/* Close excess kernel FDs. */
	ReleaseLruFiles();

TryAgain:
	if ((dir = opendir(dirname)) != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescDir;
		desc->desc.dir = dir;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.dir;
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
 * Read a directory opened with AllocateDir, ereport'ing any error.
 *
 * This is easier to use than raw readdir() since it takes care of some
 * otherwise rather tedious and error-prone manipulation of errno.  Also,
 * if you are happy with a generic error message for AllocateDir failure,
 * you can just do
 *
 *		dir = AllocateDir(path);
 *		while ((dirent = ReadDir(dir, path)) != NULL)
 *			process dirent;
 *		FreeDir(dir);
 *
 * since a NULL dir parameter is taken as indicating AllocateDir failed.
 * (Make sure errno isn't changed between AllocateDir and ReadDir if you
 * use this shortcut.)
 *
 * The pathname passed to AllocateDir must be passed to this routine too,
 * but it is only used for error reporting.
 */
struct dirent *
ReadDir(DIR *dir, const char *dirname)
{
	return ReadDirExtended(dir, dirname, ERROR);
}

/*
 * Alternate version of ReadDir that allows caller to specify the elevel
 * for any error report (whether it's reporting an initial failure of
 * AllocateDir or a subsequent directory read failure).
 *
 * If elevel < ERROR, returns NULL after any error.  With the normal coding
 * pattern, this will result in falling out of the loop immediately as
 * though the directory contained no (more) entries.
 */
struct dirent *
ReadDirExtended(DIR *dir, const char *dirname, int elevel)
{
	struct dirent *dent;

	/* Give a generic message for AllocateDir failure, if caller didn't */
	if (dir == NULL)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						dirname)));
		return NULL;
	}

	errno = 0;
	if ((dent = readdir(dir)) != NULL)
		return dent;

	if (errno)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not read directory \"%s\": %m",
						dirname)));
	return NULL;
}

/*
 * Close a directory opened with AllocateDir.
 *
 * Returns closedir's return value (with errno set if it's not 0).
 * Note we do not check the return value --- it is up to the caller
 * to handle close errors if wanted.
 *
 * Does nothing if dir == NULL; we assume that directory open failure was
 * already reported if desired.
 */
int
FreeDir(DIR *dir)
{
	int			i;

	/* Nothing to do if AllocateDir failed */
	if (dir == NULL)
		return 0;

	DO_DB(elog(LOG, "FreeDir: Allocated %d", numAllocatedDescs));

	/* Remove dir from list of allocated dirs, if it's present */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescDir && desc->desc.dir == dir)
			return FreeDesc(desc);
	}

	/* Only get here if someone passes us a dir not in allocatedDescs */
	elog(WARNING, "dir passed to FreeDir was not obtained from AllocateDir");

	return closedir(dir);
}


/*
 * Close a pipe stream returned by OpenPipeStream.
 */
int
ClosePipeStream(FILE *file)
{
	int			i;

	DO_DB(elog(LOG, "ClosePipeStream: Allocated %d", numAllocatedDescs));

	/* Remove file from list of allocated files, if it's present */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescPipe && desc->desc.file == file)
			return FreeDesc(desc);
	}

	/* Only get here if someone passes us a file not in allocatedDescs */
	elog(WARNING, "file passed to ClosePipeStream was not obtained from OpenPipeStream");

	return pclose(file);
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
		Assert(FileIsNotOpen(0));	/* Make sure ring not corrupted */
		for (i = 1; i < SizeVfdCache; i++)
		{
			if (!FileIsNotOpen(i))
				LruDelete(i);
		}
	}
}


/*
 * SetTempTablespaces
 *
 * Define a list (actually an array) of OIDs of tablespaces to use for
 * temporary files.  This list will be used until end of transaction,
 * unless this function is called again before then.  It is caller's
 * responsibility that the passed-in array has adequate lifespan (typically
 * it'd be allocated in TopTransactionContext).
 *
 * Some entries of the array may be InvalidOid, indicating that the current
 * database's default tablespace should be used.
 */
void
SetTempTablespaces(Oid *tableSpaces, int numSpaces)
{
	Assert(numSpaces >= 0);
	tempTableSpaces = tableSpaces;
	numTempTableSpaces = numSpaces;

	/*
	 * Select a random starting point in the list.  This is to minimize
	 * conflicts between backends that are most likely sharing the same list
	 * of temp tablespaces.  Note that if we create multiple temp files in the
	 * same transaction, we'll advance circularly through the list --- this
	 * ensures that large temporary sort files are nicely spread across all
	 * available tablespaces.
	 */
	if (numSpaces > 1)
		nextTempTableSpace = pg_prng_uint64_range(&pg_global_prng_state,
												  0, numSpaces - 1);
	else
		nextTempTableSpace = 0;
}

/*
 * TempTablespacesAreSet
 *
 * Returns true if SetTempTablespaces has been called in current transaction.
 * (This is just so that tablespaces.c doesn't need its own per-transaction
 * state.)
 */
bool
TempTablespacesAreSet(void)
{
	return (numTempTableSpaces >= 0);
}

/*
 * GetTempTablespaces
 *
 * Populate an array with the OIDs of the tablespaces that should be used for
 * temporary files.  (Some entries may be InvalidOid, indicating that the
 * current database's default tablespace should be used.)  At most numSpaces
 * entries will be filled.
 * Returns the number of OIDs that were copied into the output array.
 */
int
GetTempTablespaces(Oid *tableSpaces, int numSpaces)
{
	int			i;

	Assert(TempTablespacesAreSet());
	for (i = 0; i < numTempTableSpaces && i < numSpaces; ++i)
		tableSpaces[i] = tempTableSpaces[i];

	return i;
}

/*
 * GetNextTempTableSpace
 *
 * Select the next temp tablespace to use.  A result of InvalidOid means
 * to use the current database's default tablespace.
 */
Oid
GetNextTempTableSpace(void)
{
	if (numTempTableSpaces > 0)
	{
		/* Advance nextTempTableSpace counter with wraparound */
		if (++nextTempTableSpace >= numTempTableSpaces)
			nextTempTableSpace = 0;
		return tempTableSpaces[nextTempTableSpace];
	}
	return InvalidOid;
}


/*
 * AtEOSubXact_Files
 *
 * Take care of subtransaction commit/abort.  At abort, we close temp files
 * that the subtransaction may have opened.  At commit, we reassign the
 * files that were opened to the parent subtransaction.
 */
void
AtEOSubXact_Files(bool isCommit, SubTransactionId mySubid,
				  SubTransactionId parentSubid)
{
	Index		i;

	for (i = 0; i < numAllocatedDescs; i++)
	{
		if (allocatedDescs[i].create_subid == mySubid)
		{
			if (isCommit)
				allocatedDescs[i].create_subid = parentSubid;
			else
			{
				/* have to recheck the item after FreeDesc (ugly) */
				FreeDesc(&allocatedDescs[i--]);
			}
		}
	}
}

/*
 * AtEOXact_Files
 *
 * This routine is called during transaction commit or abort.  All still-open
 * per-transaction temporary file VFDs are closed, which also causes the
 * underlying files to be deleted (although they should've been closed already
 * by the ResourceOwner cleanup). Furthermore, all "allocated" stdio files are
 * closed. We also forget any transaction-local temp tablespace list.
 *
 * The isCommit flag is used only to decide whether to emit warnings about
 * unclosed files.
 */
void
AtEOXact_Files(bool isCommit)
{
	CleanupTempFiles(isCommit, false);
	tempTableSpaces = NULL;
	numTempTableSpaces = -1;
}

/*
 * BeforeShmemExit_Files
 *
 * before_shmem_exit hook to clean up temp files during backend shutdown.
 * Here, we want to clean up *all* temp files including interXact ones.
 */
static void
BeforeShmemExit_Files(int code, Datum arg)
{
	CleanupTempFiles(false, true);

	/* prevent further temp files from being created */
#ifdef USE_ASSERT_CHECKING
	temporary_files_allowed = false;
#endif
}

/*
 * Close temporary files and delete their underlying files.
 *
 * isCommit: if true, this is normal transaction commit, and we don't
 * expect any remaining files; warn if there are some.
 *
 * isProcExit: if true, this is being called as the backend process is
 * exiting. If that's the case, we should remove all temporary files; if
 * that's not the case, we are being called for transaction commit/abort
 * and should only remove transaction-local temp files.  In either case,
 * also clean up "allocated" stdio files, dirs and fds.
 */
static void
CleanupTempFiles(bool isCommit, bool isProcExit)
{
	Index		i;

	/*
	 * Careful here: at proc_exit we need extra cleanup, not just
	 * xact_temporary files.
	 */
	if (isProcExit || have_xact_temporary_files)
	{
		Assert(FileIsNotOpen(0));	/* Make sure ring not corrupted */
		for (i = 1; i < SizeVfdCache; i++)
		{
			unsigned short fdstate = VfdCache[i].fdstate;

			if (((fdstate & FD_DELETE_AT_CLOSE) || (fdstate & FD_CLOSE_AT_EOXACT)) &&
				VfdCache[i].fileName != NULL)
			{
				/*
				 * If we're in the process of exiting a backend process, close
				 * all temporary files. Otherwise, only close temporary files
				 * local to the current transaction. They should be closed by
				 * the ResourceOwner mechanism already, so this is just a
				 * debugging cross-check.
				 */
				if (isProcExit)
					FileClose(i);
				else if (fdstate & FD_CLOSE_AT_EOXACT)
				{
					elog(WARNING,
						 "temporary file %s not closed at end-of-transaction",
						 VfdCache[i].fileName);
					FileClose(i);
				}
			}
		}

		have_xact_temporary_files = false;
	}

	/* Complain if any allocated files remain open at commit. */
	if (isCommit && numAllocatedDescs > 0)
		elog(WARNING, "%d temporary files and directories not closed at end-of-transaction",
			 numAllocatedDescs);

	/* Clean up "allocated" stdio files, dirs and fds. */
	while (numAllocatedDescs > 0)
		FreeDesc(&allocatedDescs[0]);
}


/*
 * Remove temporary and temporary relation files left over from a prior
 * postmaster session
 *
 * This should be called during postmaster startup.  It will forcibly
 * remove any leftover files created by OpenTemporaryFile and any leftover
 * temporary relation files created by mdcreate.
 *
 * During post-backend-crash restart cycle, this routine is called when
 * remove_temp_files_after_crash GUC is enabled. Multiple crashes while
 * queries are using temp files could result in useless storage usage that can
 * only be reclaimed by a service restart. The argument against enabling it is
 * that someone might want to examine the temporary files for debugging
 * purposes. This does however mean that OpenTemporaryFile had better allow for
 * collision with an existing temp file name.
 *
 * NOTE: this function and its subroutines generally report syscall failures
 * with ereport(LOG) and keep going.  Removing temp files is not so critical
 * that we should fail to start the database when we can't do it.
 */
void
RemovePgTempFiles(void)
{
	char		temp_path[MAXPGPATH + sizeof(PG_TBLSPC_DIR) + sizeof(TABLESPACE_VERSION_DIRECTORY) + sizeof(PG_TEMP_FILES_DIR)];
	DIR		   *spc_dir;
	struct dirent *spc_de;

	/*
	 * First process temp files in pg_default ($PGDATA/base)
	 */
	snprintf(temp_path, sizeof(temp_path), "base/%s", PG_TEMP_FILES_DIR);
	RemovePgTempFilesInDir(temp_path, true, false);
	RemovePgTempRelationFiles("base");

	/*
	 * Cycle through temp directories for all non-default tablespaces.
	 */
	spc_dir = AllocateDir(PG_TBLSPC_DIR);

	while ((spc_de = ReadDirExtended(spc_dir, PG_TBLSPC_DIR, LOG)) != NULL)
	{
		if (strcmp(spc_de->d_name, ".") == 0 ||
			strcmp(spc_de->d_name, "..") == 0)
			continue;

		snprintf(temp_path, sizeof(temp_path), "%s/%s/%s/%s",
				 PG_TBLSPC_DIR, spc_de->d_name, TABLESPACE_VERSION_DIRECTORY,
				 PG_TEMP_FILES_DIR);
		RemovePgTempFilesInDir(temp_path, true, false);

		snprintf(temp_path, sizeof(temp_path), "%s/%s/%s",
				 PG_TBLSPC_DIR, spc_de->d_name, TABLESPACE_VERSION_DIRECTORY);
		RemovePgTempRelationFiles(temp_path);
	}

	FreeDir(spc_dir);

	/*
	 * In EXEC_BACKEND case there is a pgsql_tmp directory at the top level of
	 * DataDir as well.  However, that is *not* cleaned here because doing so
	 * would create a race condition.  It's done separately, earlier in
	 * postmaster startup.
	 */
}

/*
 * Process one pgsql_tmp directory for RemovePgTempFiles.
 *
 * If missing_ok is true, it's all right for the named directory to not exist.
 * Any other problem results in a LOG message.  (missing_ok should be true at
 * the top level, since pgsql_tmp directories are not created until needed.)
 *
 * At the top level, this should be called with unlink_all = false, so that
 * only files matching the temporary name prefix will be unlinked.  When
 * recursing it will be called with unlink_all = true to unlink everything
 * under a top-level temporary directory.
 *
 * (These two flags could be replaced by one, but it seems clearer to keep
 * them separate.)
 */
void
RemovePgTempFilesInDir(const char *tmpdirname, bool missing_ok, bool unlink_all)
{
	DIR		   *temp_dir;
	struct dirent *temp_de;
	char		rm_path[MAXPGPATH * 2];

	temp_dir = AllocateDir(tmpdirname);

	if (temp_dir == NULL && errno == ENOENT && missing_ok)
		return;

	while ((temp_de = ReadDirExtended(temp_dir, tmpdirname, LOG)) != NULL)
	{
		if (strcmp(temp_de->d_name, ".") == 0 ||
			strcmp(temp_de->d_name, "..") == 0)
			continue;

		snprintf(rm_path, sizeof(rm_path), "%s/%s",
				 tmpdirname, temp_de->d_name);

		if (unlink_all ||
			strncmp(temp_de->d_name,
					PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0)
		{
			PGFileType	type = get_dirent_type(rm_path, temp_de, false, LOG);

			if (type == PGFILETYPE_ERROR)
				continue;
			else if (type == PGFILETYPE_DIR)
			{
				/* recursively remove contents, then directory itself */
				RemovePgTempFilesInDir(rm_path, false, true);

				if (rmdir(rm_path) < 0)
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not remove directory \"%s\": %m",
									rm_path)));
			}
			else
			{
				if (unlink(rm_path) < 0)
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m",
									rm_path)));
			}
		}
		else
			ereport(LOG,
					(errmsg("unexpected file found in temporary-files directory: \"%s\"",
							rm_path)));
	}

	FreeDir(temp_dir);
}

/* Process one tablespace directory, look for per-DB subdirectories */
static void
RemovePgTempRelationFiles(const char *tsdirname)
{
	DIR		   *ts_dir;
	struct dirent *de;
	char		dbspace_path[MAXPGPATH * 2];

	ts_dir = AllocateDir(tsdirname);

	while ((de = ReadDirExtended(ts_dir, tsdirname, LOG)) != NULL)
	{
		/*
		 * We're only interested in the per-database directories, which have
		 * numeric names.  Note that this code will also (properly) ignore "."
		 * and "..".
		 */
		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;

		snprintf(dbspace_path, sizeof(dbspace_path), "%s/%s",
				 tsdirname, de->d_name);
		RemovePgTempRelationFilesInDbspace(dbspace_path);
	}

	FreeDir(ts_dir);
}

/* Process one per-dbspace directory for RemovePgTempRelationFiles */
static void
RemovePgTempRelationFilesInDbspace(const char *dbspacedirname)
{
	DIR		   *dbspace_dir;
	struct dirent *de;
	char		rm_path[MAXPGPATH * 2];

	dbspace_dir = AllocateDir(dbspacedirname);

	while ((de = ReadDirExtended(dbspace_dir, dbspacedirname, LOG)) != NULL)
	{
		if (!looks_like_temp_rel_name(de->d_name))
			continue;

		snprintf(rm_path, sizeof(rm_path), "%s/%s",
				 dbspacedirname, de->d_name);

		if (unlink(rm_path) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							rm_path)));
	}

	FreeDir(dbspace_dir);
}

/* t<digits>_<digits>, or t<digits>_<digits>_<forkname> */
bool
looks_like_temp_rel_name(const char *name)
{
	int			pos;
	int			savepos;

	/* Must start with "t". */
	if (name[0] != 't')
		return false;

	/* Followed by a non-empty string of digits and then an underscore. */
	for (pos = 1; isdigit((unsigned char) name[pos]); ++pos)
		;
	if (pos == 1 || name[pos] != '_')
		return false;

	/* Followed by another nonempty string of digits. */
	for (savepos = ++pos; isdigit((unsigned char) name[pos]); ++pos)
		;
	if (savepos == pos)
		return false;

	/* We might have _forkname or .segment or both. */
	if (name[pos] == '_')
	{
		int			forkchar = forkname_chars(&name[pos + 1], NULL);

		if (forkchar <= 0)
			return false;
		pos += forkchar + 1;
	}
	if (name[pos] == '.')
	{
		int			segchar;

		for (segchar = 1; isdigit((unsigned char) name[pos + segchar]); ++segchar)
			;
		if (segchar <= 1)
			return false;
		pos += segchar;
	}

	/* Now we should be at the end. */
	if (name[pos] != '\0')
		return false;
	return true;
}

#ifdef HAVE_SYNCFS
static void
do_syncfs(const char *path)
{
	int			fd;

	ereport_startup_progress("syncing data directory (syncfs), elapsed time: %ld.%02d s, current path: %s",
							 path);

	fd = OpenTransientFile(path, O_RDONLY);
	if (fd < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
		return;
	}
	if (syncfs(fd) < 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not synchronize file system for file \"%s\": %m", path)));
	CloseTransientFile(fd);
}
#endif

/*
 * Issue fsync recursively on PGDATA and all its contents, or issue syncfs for
 * all potential filesystem, depending on recovery_init_sync_method setting.
 *
 * We fsync regular files and directories wherever they are, but we
 * follow symlinks only for pg_wal and immediately under pg_tblspc.
 * Other symlinks are presumed to point at files we're not responsible
 * for fsyncing, and might not have privileges to write at all.
 *
 * Errors are logged but not considered fatal; that's because this is used
 * only during database startup, to deal with the possibility that there are
 * issued-but-unsynced writes pending against the data directory.  We want to
 * ensure that such writes reach disk before anything that's done in the new
 * run.  However, aborting on error would result in failure to start for
 * harmless cases such as read-only files in the data directory, and that's
 * not good either.
 *
 * Note that if we previously crashed due to a PANIC on fsync(), we'll be
 * rewriting all changes again during recovery.
 *
 * Note we assume we're chdir'd into PGDATA to begin with.
 */
void
SyncDataDirectory(void)
{
	bool		xlog_is_symlink;

	/* We can skip this whole thing if fsync is disabled. */
	if (!enableFsync)
		return;

	/*
	 * If pg_wal is a symlink, we'll need to recurse into it separately,
	 * because the first walkdir below will ignore it.
	 */
	xlog_is_symlink = false;

	{
		struct stat st;

		if (lstat("pg_wal", &st) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m",
							"pg_wal")));
		else if (S_ISLNK(st.st_mode))
			xlog_is_symlink = true;
	}

#ifdef HAVE_SYNCFS
	if (recovery_init_sync_method == DATA_DIR_SYNC_METHOD_SYNCFS)
	{
		DIR		   *dir;
		struct dirent *de;

		/*
		 * On Linux, we don't have to open every single file one by one.  We
		 * can use syncfs() to sync whole filesystems.  We only expect
		 * filesystem boundaries to exist where we tolerate symlinks, namely
		 * pg_wal and the tablespaces, so we call syncfs() for each of those
		 * directories.
		 */

		/* Prepare to report progress syncing the data directory via syncfs. */
		begin_startup_progress_phase();

		/* Sync the top level pgdata directory. */
		do_syncfs(".");
		/* If any tablespaces are configured, sync each of those. */
		dir = AllocateDir(PG_TBLSPC_DIR);
		while ((de = ReadDirExtended(dir, PG_TBLSPC_DIR, LOG)))
		{
			char		path[MAXPGPATH];

			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;

			snprintf(path, MAXPGPATH, "%s/%s", PG_TBLSPC_DIR, de->d_name);
			do_syncfs(path);
		}
		FreeDir(dir);
		/* If pg_wal is a symlink, process that too. */
		if (xlog_is_symlink)
			do_syncfs("pg_wal");
		return;
	}
#endif							/* !HAVE_SYNCFS */

#ifdef PG_FLUSH_DATA_WORKS
	/* Prepare to report progress of the pre-fsync phase. */
	begin_startup_progress_phase();

	/*
	 * If possible, hint to the kernel that we're soon going to fsync the data
	 * directory and its contents.  Errors in this step are even less
	 * interesting than normal, so log them only at DEBUG1.
	 */
	walkdir(".", pre_sync_fname, false, DEBUG1);
	if (xlog_is_symlink)
		walkdir("pg_wal", pre_sync_fname, false, DEBUG1);
	walkdir(PG_TBLSPC_DIR, pre_sync_fname, true, DEBUG1);
#endif

	/* Prepare to report progress syncing the data directory via fsync. */
	begin_startup_progress_phase();

	/*
	 * Now we do the fsync()s in the same order.
	 *
	 * The main call ignores symlinks, so in addition to specially processing
	 * pg_wal if it's a symlink, pg_tblspc has to be visited separately with
	 * process_symlinks = true.  Note that if there are any plain directories
	 * in pg_tblspc, they'll get fsync'd twice.  That's not an expected case
	 * so we don't worry about optimizing it.
	 */
	walkdir(".", datadir_fsync_fname, false, LOG);
	if (xlog_is_symlink)
		walkdir("pg_wal", datadir_fsync_fname, false, LOG);
	walkdir(PG_TBLSPC_DIR, datadir_fsync_fname, true, LOG);
}

/*
 * walkdir: recursively walk a directory, applying the action to each
 * regular file and directory (including the named directory itself).
 *
 * If process_symlinks is true, the action and recursion are also applied
 * to regular files and directories that are pointed to by symlinks in the
 * given directory; otherwise symlinks are ignored.  Symlinks are always
 * ignored in subdirectories, ie we intentionally don't pass down the
 * process_symlinks flag to recursive calls.
 *
 * Errors are reported at level elevel, which might be ERROR or less.
 *
 * See also walkdir in file_utils.c, which is a frontend version of this
 * logic.
 */
static void
walkdir(const char *path,
		void (*action) (const char *fname, bool isdir, int elevel),
		bool process_symlinks,
		int elevel)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(path);

	while ((de = ReadDirExtended(dir, path, elevel)) != NULL)
	{
		char		subpath[MAXPGPATH * 2];

		CHECK_FOR_INTERRUPTS();

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);

		switch (get_dirent_type(subpath, de, process_symlinks, elevel))
		{
			case PGFILETYPE_REG:
				(*action) (subpath, false, elevel);
				break;
			case PGFILETYPE_DIR:
				walkdir(subpath, action, false, elevel);
				break;
			default:

				/*
				 * Errors are already reported directly by get_dirent_type(),
				 * and any remaining symlinks and unknown file types are
				 * ignored.
				 */
				break;
		}
	}

	FreeDir(dir);				/* we ignore any error here */

	/*
	 * It's important to fsync the destination directory itself as individual
	 * file fsyncs don't guarantee that the directory entry for the file is
	 * synced.  However, skip this if AllocateDir failed; the action function
	 * might not be robust against that.
	 */
	if (dir)
		(*action) (path, true, elevel);
}


/*
 * Hint to the OS that it should get ready to fsync() this file.
 *
 * Ignores errors trying to open unreadable files, and logs other errors at a
 * caller-specified level.
 */
#ifdef PG_FLUSH_DATA_WORKS

static void
pre_sync_fname(const char *fname, bool isdir, int elevel)
{
	int			fd;

	/* Don't try to flush directories, it'll likely just fail */
	if (isdir)
		return;

	ereport_startup_progress("syncing data directory (pre-fsync), elapsed time: %ld.%02d s, current path: %s",
							 fname);

	fd = OpenTransientFile(fname, O_RDONLY | PG_BINARY);

	if (fd < 0)
	{
		if (errno == EACCES)
			return;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fname)));
		return;
	}

	/*
	 * pg_flush_data() ignores errors, which is ok because this is only a
	 * hint.
	 */
	pg_flush_data(fd, 0, 0);

	if (CloseTransientFile(fd) != 0)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fname)));
}

#endif							/* PG_FLUSH_DATA_WORKS */

static void
datadir_fsync_fname(const char *fname, bool isdir, int elevel)
{
	ereport_startup_progress("syncing data directory (fsync), elapsed time: %ld.%02d s, current path: %s",
							 fname);

	/*
	 * We want to silently ignoring errors about unreadable files.  Pass that
	 * desire on to fsync_fname_ext().
	 */
	fsync_fname_ext(fname, isdir, true, elevel);
}

static void
unlink_if_exists_fname(const char *fname, bool isdir, int elevel)
{
	if (isdir)
	{
		if (rmdir(fname) != 0 && errno != ENOENT)
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m", fname)));
	}
	else
	{
		/* Use PathNameDeleteTemporaryFile to report filesize */
		PathNameDeleteTemporaryFile(fname, false);
	}
}

/*
 * fsync_fname_ext -- Try to fsync a file or directory
 *
 * If ignore_perm is true, ignore errors upon trying to open unreadable
 * files. Logs other errors at a caller-specified level.
 *
 * Returns 0 if the operation succeeded, -1 otherwise.
 */
int
fsync_fname_ext(const char *fname, bool isdir, bool ignore_perm, int elevel)
{
	int			fd;
	int			flags;
	int			returncode;

	/*
	 * Some OSs require directories to be opened read-only whereas other
	 * systems don't allow us to fsync files opened read-only; so we need both
	 * cases here.  Using O_RDWR will cause us to fail to fsync files that are
	 * not writable by our userid, but we assume that's OK.
	 */
	flags = PG_BINARY;
	if (!isdir)
		flags |= O_RDWR;
	else
		flags |= O_RDONLY;

	fd = OpenTransientFile(fname, flags);

	/*
	 * Some OSs don't allow us to open directories at all (Windows returns
	 * EACCES), just ignore the error in that case.  If desired also silently
	 * ignoring errors about unreadable files. Log others.
	 */
	if (fd < 0 && isdir && (errno == EISDIR || errno == EACCES))
		return 0;
	else if (fd < 0 && ignore_perm && errno == EACCES)
		return 0;
	else if (fd < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fname)));
		return -1;
	}

	returncode = pg_fsync(fd);

	/*
	 * Some OSes don't allow us to fsync directories at all, so we can ignore
	 * those errors. Anything else needs to be logged.
	 */
	if (returncode != 0 && !(isdir && (errno == EBADF || errno == EINVAL)))
	{
		int			save_errno;

		/* close file upon error, might not be in transaction context */
		save_errno = errno;
		(void) CloseTransientFile(fd);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", fname)));
		return -1;
	}

	if (CloseTransientFile(fd) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fname)));
		return -1;
	}

	return 0;
}

/*
 * fsync_parent_path -- fsync the parent path of a file or directory
 *
 * This is aimed at making file operations persistent on disk in case of
 * an OS crash or power failure.
 */
static int
fsync_parent_path(const char *fname, int elevel)
{
	char		parentpath[MAXPGPATH];

	strlcpy(parentpath, fname, MAXPGPATH);
	get_parent_directory(parentpath);

	/*
	 * get_parent_directory() returns an empty string if the input argument is
	 * just a file name (see comments in path.c), so handle that as being the
	 * current directory.
	 */
	if (strlen(parentpath) == 0)
		strlcpy(parentpath, ".", MAXPGPATH);

	if (fsync_fname_ext(parentpath, true, false, elevel) != 0)
		return -1;

	return 0;
}

/*
 * Create a PostgreSQL data sub-directory
 *
 * The data directory itself, and most of its sub-directories, are created at
 * initdb time, but we do have some occasions when we create directories in
 * the backend (CREATE TABLESPACE, for example).  In those cases, we want to
 * make sure that those directories are created consistently.  Today, that means
 * making sure that the created directory has the correct permissions, which is
 * what pg_dir_create_mode tracks for us.
 *
 * Note that we also set the umask() based on what we understand the correct
 * permissions to be (see file_perm.c).
 *
 * For permissions other than the default, mkdir() can be used directly, but
 * be sure to consider carefully such cases -- a sub-directory with incorrect
 * permissions in a PostgreSQL data directory could cause backups and other
 * processes to fail.
 */
int
MakePGDirectory(const char *directoryName)
{
	return mkdir(directoryName, pg_dir_create_mode);
}

/*
 * Return the passed-in error level, or PANIC if data_sync_retry is off.
 *
 * Failure to fsync any data file is cause for immediate panic, unless
 * data_sync_retry is enabled.  Data may have been written to the operating
 * system and removed from our buffer pool already, and if we are running on
 * an operating system that forgets dirty data on write-back failure, there
 * may be only one copy of the data remaining: in the WAL.  A later attempt to
 * fsync again might falsely report success.  Therefore we must not allow any
 * further checkpoints to be attempted.  data_sync_retry can in theory be
 * enabled on systems known not to drop dirty buffered data on write-back
 * failure (with the likely outcome that checkpoints will continue to fail
 * until the underlying problem is fixed).
 *
 * Any code that reports a failure from fsync() or related functions should
 * filter the error level with this function.
 */
int
data_sync_elevel(int elevel)
{
	return data_sync_retry ? elevel : PANIC;
}

bool
check_debug_io_direct(char **newval, void **extra, GucSource source)
{
	bool		result = true;
	int			flags;

#if PG_O_DIRECT == 0
	if (strcmp(*newval, "") != 0)
	{
		GUC_check_errdetail("\"%s\" is not supported on this platform.",
							"debug_io_direct");
		result = false;
	}
	flags = 0;
#else
	List	   *elemlist;
	ListCell   *l;
	char	   *rawstring;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(*newval);

	if (!SplitGUCList(rawstring, ',', &elemlist))
	{
		GUC_check_errdetail("Invalid list syntax in parameter \"%s\".",
							"debug_io_direct");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	flags = 0;
	foreach(l, elemlist)
	{
		char	   *item = (char *) lfirst(l);

		if (pg_strcasecmp(item, "data") == 0)
			flags |= IO_DIRECT_DATA;
		else if (pg_strcasecmp(item, "wal") == 0)
			flags |= IO_DIRECT_WAL;
		else if (pg_strcasecmp(item, "wal_init") == 0)
			flags |= IO_DIRECT_WAL_INIT;
		else
		{
			GUC_check_errdetail("Invalid option \"%s\".", item);
			result = false;
			break;
		}
	}

	/*
	 * It's possible to configure block sizes smaller than our assumed I/O
	 * alignment size, which could result in invalid I/O requests.
	 */
#if XLOG_BLCKSZ < PG_IO_ALIGN_SIZE
	if (result && (flags & (IO_DIRECT_WAL | IO_DIRECT_WAL_INIT)))
	{
		GUC_check_errdetail("\"%s\" is not supported for WAL because %s is too small.",
							"debug_io_direct", "XLOG_BLCKSZ");
		result = false;
	}
#endif
#if BLCKSZ < PG_IO_ALIGN_SIZE
	if (result && (flags & IO_DIRECT_DATA))
	{
		GUC_check_errdetail("\"%s\" is not supported for WAL because %s is too small.",
							"debug_io_direct", "BLCKSZ");
		result = false;
	}
#endif

	pfree(rawstring);
	list_free(elemlist);
#endif

	if (!result)
		return result;

	/* Save the flags in *extra, for use by assign_debug_io_direct */
	*extra = guc_malloc(ERROR, sizeof(int));
	*((int *) *extra) = flags;

	return result;
}

void
assign_debug_io_direct(const char *newval, void *extra)
{
	int		   *flags = (int *) extra;

	io_direct_flags = *flags;
}

/* ResourceOwner callbacks */

static void
ResOwnerReleaseFile(Datum res)
{
	File		file = (File) DatumGetInt32(res);
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	vfdP = &VfdCache[file];
	vfdP->resowner = NULL;

	FileClose(file);
}

static char *
ResOwnerPrintFile(Datum res)
{
	return psprintf("File %d", DatumGetInt32(res));
}
