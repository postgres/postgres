/*-------------------------------------------------------------------------
 *
 * dsm_impl.c
 *	  manage dynamic shared memory segments
 *
 * This file provides low-level APIs for creating and destroying shared
 * memory segments using several different possible techniques.  We refer
 * to these segments as dynamic because they can be created, altered, and
 * destroyed at any point during the server life cycle.  This is unlike
 * the main shared memory segment, of which there is always exactly one
 * and which is always mapped at a fixed address in every PostgreSQL
 * background process.
 *
 * Because not all systems provide the same primitives in this area, nor
 * do all primitives behave the same way on all systems, we provide
 * several implementations of this facility.  Many systems implement
 * POSIX shared memory (shm_open etc.), which is well-suited to our needs
 * in this area, with the exception that shared memory identifiers live
 * in a flat system-wide namespace, raising the uncomfortable prospect of
 * name collisions with other processes (including other copies of
 * PostgreSQL) running on the same system.  Some systems only support
 * the older System V shared memory interface (shmget etc.) which is
 * also usable; however, the default allocation limits are often quite
 * small, and the namespace is even more restricted.
 *
 * We also provide an mmap-based shared memory implementation.  This may
 * be useful on systems that provide shared memory via a special-purpose
 * filesystem; by opting for this implementation, the user can even
 * control precisely where their shared memory segments are placed.  It
 * can also be used as a fallback for systems where shm_open and shmget
 * are not available or can't be used for some reason.  Of course,
 * mapping a file residing on an actual spinning disk is a fairly poor
 * approximation for shared memory because writeback may hurt performance
 * substantially, but there should be few systems where we must make do
 * with such poor tools.
 *
 * As ever, Windows requires its own implemetation.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#ifdef HAVE_SYS_IPC_H
#include <sys/ipc.h>
#endif
#ifdef HAVE_SYS_SHM_H
#include <sys/shm.h>
#endif

#include "portability/mem.h"
#include "storage/dsm_impl.h"
#include "storage/fd.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "postmaster/postmaster.h"

#ifdef USE_DSM_POSIX
static bool dsm_impl_posix(dsm_op op, dsm_handle handle, Size request_size,
			   void **impl_private, void **mapped_address,
			   Size *mapped_size, int elevel);
#endif
#ifdef USE_DSM_SYSV
static bool dsm_impl_sysv(dsm_op op, dsm_handle handle, Size request_size,
			  void **impl_private, void **mapped_address,
			  Size *mapped_size, int elevel);
#endif
#ifdef USE_DSM_WINDOWS
static bool dsm_impl_windows(dsm_op op, dsm_handle handle, Size request_size,
				 void **impl_private, void **mapped_address,
				 Size *mapped_size, int elevel);
#endif
#ifdef USE_DSM_MMAP
static bool dsm_impl_mmap(dsm_op op, dsm_handle handle, Size request_size,
			  void **impl_private, void **mapped_address,
			  Size *mapped_size, int elevel);
#endif
static int	errcode_for_dynamic_shared_memory(void);

const struct config_enum_entry dynamic_shared_memory_options[] = {
#ifdef USE_DSM_POSIX
	{"posix", DSM_IMPL_POSIX, false},
#endif
#ifdef USE_DSM_SYSV
	{"sysv", DSM_IMPL_SYSV, false},
#endif
#ifdef USE_DSM_WINDOWS
	{"windows", DSM_IMPL_WINDOWS, false},
#endif
#ifdef USE_DSM_MMAP
	{"mmap", DSM_IMPL_MMAP, false},
#endif
	{"none", DSM_IMPL_NONE, false},
	{NULL, 0, false}
};

/* Implementation selector. */
int			dynamic_shared_memory_type;

/* Size of buffer to be used for zero-filling. */
#define ZBUFFER_SIZE				8192

#define SEGMENT_NAME_PREFIX			"Global/PostgreSQL"

/*------
 * Perform a low-level shared memory operation in a platform-specific way,
 * as dictated by the selected implementation.  Each implementation is
 * required to implement the following primitives.
 *
 * DSM_OP_CREATE.  Create a segment whose size is the request_size and
 * map it.
 *
 * DSM_OP_ATTACH.  Map the segment, whose size must be the request_size.
 * The segment may already be mapped; any existing mapping should be removed
 * before creating a new one.
 *
 * DSM_OP_DETACH.  Unmap the segment.
 *
 * DSM_OP_RESIZE.  Resize the segment to the given request_size and
 * remap the segment at that new size.
 *
 * DSM_OP_DESTROY.  Unmap the segment, if it is mapped.  Destroy the
 * segment.
 *
 * Arguments:
 *	 op: The operation to be performed.
 *	 handle: The handle of an existing object, or for DSM_OP_CREATE, the
 *	   a new handle the caller wants created.
 *	 request_size: For DSM_OP_CREATE, the requested size.  For DSM_OP_RESIZE,
 *	   the new size.  Otherwise, 0.
 *	 impl_private: Private, implementation-specific data.  Will be a pointer
 *	   to NULL for the first operation on a shared memory segment within this
 *	   backend; thereafter, it will point to the value to which it was set
 *	   on the previous call.
 *	 mapped_address: Pointer to start of current mapping; pointer to NULL
 *	   if none.  Updated with new mapping address.
 *	 mapped_size: Pointer to size of current mapping; pointer to 0 if none.
 *	   Updated with new mapped size.
 *	 elevel: Level at which to log errors.
 *
 * Return value: true on success, false on failure.  When false is returned,
 * a message should first be logged at the specified elevel, except in the
 * case where DSM_OP_CREATE experiences a name collision, which should
 * silently return false.
 *-----
 */
bool
dsm_impl_op(dsm_op op, dsm_handle handle, Size request_size,
			void **impl_private, void **mapped_address, Size *mapped_size,
			int elevel)
{
	Assert(op == DSM_OP_CREATE || op == DSM_OP_RESIZE || request_size == 0);
	Assert((op != DSM_OP_CREATE && op != DSM_OP_ATTACH) ||
		   (*mapped_address == NULL && *mapped_size == 0));

	switch (dynamic_shared_memory_type)
	{
#ifdef USE_DSM_POSIX
		case DSM_IMPL_POSIX:
			return dsm_impl_posix(op, handle, request_size, impl_private,
								  mapped_address, mapped_size, elevel);
#endif
#ifdef USE_DSM_SYSV
		case DSM_IMPL_SYSV:
			return dsm_impl_sysv(op, handle, request_size, impl_private,
								 mapped_address, mapped_size, elevel);
#endif
#ifdef USE_DSM_WINDOWS
		case DSM_IMPL_WINDOWS:
			return dsm_impl_windows(op, handle, request_size, impl_private,
									mapped_address, mapped_size, elevel);
#endif
#ifdef USE_DSM_MMAP
		case DSM_IMPL_MMAP:
			return dsm_impl_mmap(op, handle, request_size, impl_private,
								 mapped_address, mapped_size, elevel);
#endif
		default:
			elog(ERROR, "unexpected dynamic shared memory type: %d",
				 dynamic_shared_memory_type);
			return false;
	}
}

/*
 * Does the current dynamic shared memory implementation support resizing
 * segments?  (The answer here could be platform-dependent in the future,
 * since AIX allows shmctl(shmid, SHM_RESIZE, &buffer), though you apparently
 * can't resize segments to anything larger than 256MB that way.  For now,
 * we keep it simple.)
 */
bool
dsm_impl_can_resize(void)
{
	switch (dynamic_shared_memory_type)
	{
		case DSM_IMPL_NONE:
			return false;
		case DSM_IMPL_POSIX:
			return true;
		case DSM_IMPL_SYSV:
			return false;
		case DSM_IMPL_WINDOWS:
			return false;
		case DSM_IMPL_MMAP:
			return true;
		default:
			return false;		/* should not happen */
	}
}

#ifdef USE_DSM_POSIX
/*
 * Operating system primitives to support POSIX shared memory.
 *
 * POSIX shared memory segments are created and attached using shm_open()
 * and shm_unlink(); other operations, such as sizing or mapping the
 * segment, are performed as if the shared memory segments were files.
 *
 * Indeed, on some platforms, they may be implemented that way.  While
 * POSIX shared memory segments seem intended to exist in a flat namespace,
 * some operating systems may implement them as files, even going so far
 * to treat a request for /xyz as a request to create a file by that name
 * in the root directory.  Users of such broken platforms should select
 * a different shared memory implementation.
 */
static bool
dsm_impl_posix(dsm_op op, dsm_handle handle, Size request_size,
			   void **impl_private, void **mapped_address, Size *mapped_size,
			   int elevel)
{
	char		name[64];
	int			flags;
	int			fd;
	char	   *address;

	snprintf(name, 64, "/PostgreSQL.%u", handle);

	/* Handle teardown cases. */
	if (op == DSM_OP_DETACH || op == DSM_OP_DESTROY)
	{
		if (*mapped_address != NULL
			&& munmap(*mapped_address, *mapped_size) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				   errmsg("could not unmap shared memory segment \"%s\": %m",
						  name)));
			return false;
		}
		*mapped_address = NULL;
		*mapped_size = 0;
		if (op == DSM_OP_DESTROY && shm_unlink(name) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				  errmsg("could not remove shared memory segment \"%s\": %m",
						 name)));
			return false;
		}
		return true;
	}

	/*
	 * Create new segment or open an existing one for attach or resize.
	 *
	 * Even though we're not going through fd.c, we should be safe against
	 * running out of file descriptors, because of NUM_RESERVED_FDS.  We're
	 * only opening one extra descriptor here, and we'll close it before
	 * returning.
	 */
	flags = O_RDWR | (op == DSM_OP_CREATE ? O_CREAT | O_EXCL : 0);
	if ((fd = shm_open(name, flags, 0600)) == -1)
	{
		if (errno != EEXIST)
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not open shared memory segment \"%s\": %m",
							name)));
		return false;
	}

	/*
	 * If we're attaching the segment, determine the current size; if we are
	 * creating or resizing the segment, set the size to the requested value.
	 */
	if (op == DSM_OP_ATTACH)
	{
		struct stat st;

		if (fstat(fd, &st) != 0)
		{
			int			save_errno;

			/* Back out what's already been done. */
			save_errno = errno;
			close(fd);
			errno = save_errno;

			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not stat shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		request_size = st.st_size;
	}
	else if (*mapped_size != request_size && ftruncate(fd, request_size))
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		close(fd);
		if (op == DSM_OP_CREATE)
			shm_unlink(name);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not resize shared memory segment \"%s\" to %zu bytes: %m",
						name, request_size)));
		return false;
	}

	/*
	 * If we're reattaching or resizing, we must remove any existing mapping,
	 * unless we've already got the right thing mapped.
	 */
	if (*mapped_address != NULL)
	{
		if (*mapped_size == request_size)
			return true;
		if (munmap(*mapped_address, *mapped_size) != 0)
		{
			int			save_errno;

			/* Back out what's already been done. */
			save_errno = errno;
			close(fd);
			if (op == DSM_OP_CREATE)
				shm_unlink(name);
			errno = save_errno;

			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				   errmsg("could not unmap shared memory segment \"%s\": %m",
						  name)));
			return false;
		}
		*mapped_address = NULL;
		*mapped_size = 0;
	}

	/* Map it. */
	address = mmap(NULL, request_size, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_HASSEMAPHORE | MAP_NOSYNC, fd, 0);
	if (address == MAP_FAILED)
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		close(fd);
		if (op == DSM_OP_CREATE)
			shm_unlink(name);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	*mapped_address = address;
	*mapped_size = request_size;
	close(fd);

	return true;
}
#endif

#ifdef USE_DSM_SYSV
/*
 * Operating system primitives to support System V shared memory.
 *
 * System V shared memory segments are manipulated using shmget(), shmat(),
 * shmdt(), and shmctl().  There's no portable way to resize such
 * segments.  As the default allocation limits for System V shared memory
 * are usually quite low, the POSIX facilities may be preferable; but
 * those are not supported everywhere.
 */
static bool
dsm_impl_sysv(dsm_op op, dsm_handle handle, Size request_size,
			  void **impl_private, void **mapped_address, Size *mapped_size,
			  int elevel)
{
	key_t		key;
	int			ident;
	char	   *address;
	char		name[64];
	int		   *ident_cache;

	/* Resize is not supported for System V shared memory. */
	if (op == DSM_OP_RESIZE)
	{
		elog(elevel, "System V shared memory segments cannot be resized");
		return false;
	}

	/* Since resize isn't supported, reattach is a no-op. */
	if (op == DSM_OP_ATTACH && *mapped_address != NULL)
		return true;

	/*
	 * POSIX shared memory and mmap-based shared memory identify segments with
	 * names.  To avoid needless error message variation, we use the handle as
	 * the name.
	 */
	snprintf(name, 64, "%u", handle);

	/*
	 * The System V shared memory namespace is very restricted; names are of
	 * type key_t, which is expected to be some sort of integer data type, but
	 * not necessarily the same one as dsm_handle.  Since we use dsm_handle to
	 * identify shared memory segments across processes, this might seem like
	 * a problem, but it's really not.  If dsm_handle is bigger than key_t,
	 * the cast below might truncate away some bits from the handle the
	 * user-provided, but it'll truncate exactly the same bits away in exactly
	 * the same fashion every time we use that handle, which is all that
	 * really matters.  Conversely, if dsm_handle is smaller than key_t, we
	 * won't use the full range of available key space, but that's no big deal
	 * either.
	 *
	 * We do make sure that the key isn't negative, because that might not be
	 * portable.
	 */
	key = (key_t) handle;
	if (key < 1)				/* avoid compiler warning if type is unsigned */
		key = -key;

	/*
	 * There's one special key, IPC_PRIVATE, which can't be used.  If we end
	 * up with that value by chance during a create operation, just pretend it
	 * already exists, so that caller will retry.  If we run into it anywhere
	 * else, the caller has passed a handle that doesn't correspond to
	 * anything we ever created, which should not happen.
	 */
	if (key == IPC_PRIVATE)
	{
		if (op != DSM_OP_CREATE)
			elog(DEBUG4, "System V shared memory key may not be IPC_PRIVATE");
		errno = EEXIST;
		return false;
	}

	/*
	 * Before we can do anything with a shared memory segment, we have to map
	 * the shared memory key to a shared memory identifier using shmget(). To
	 * avoid repeated lookups, we store the key using impl_private.
	 */
	if (*impl_private != NULL)
	{
		ident_cache = *impl_private;
		ident = *ident_cache;
	}
	else
	{
		int			flags = IPCProtection;
		size_t		segsize;

		/*
		 * Allocate the memory BEFORE acquiring the resource, so that we don't
		 * leak the resource if memory allocation fails.
		 */
		ident_cache = MemoryContextAlloc(TopMemoryContext, sizeof(int));

		/*
		 * When using shmget to find an existing segment, we must pass the
		 * size as 0.  Passing a non-zero size which is greater than the
		 * actual size will result in EINVAL.
		 */
		segsize = 0;

		if (op == DSM_OP_CREATE)
		{
			flags |= IPC_CREAT | IPC_EXCL;
			segsize = request_size;
		}

		if ((ident = shmget(key, segsize, flags)) == -1)
		{
			if (errno != EEXIST)
			{
				int			save_errno = errno;

				pfree(ident_cache);
				errno = save_errno;
				ereport(elevel,
						(errcode_for_dynamic_shared_memory(),
						 errmsg("could not get shared memory segment: %m")));
			}
			return false;
		}

		*ident_cache = ident;
		*impl_private = ident_cache;
	}

	/* Handle teardown cases. */
	if (op == DSM_OP_DETACH || op == DSM_OP_DESTROY)
	{
		pfree(ident_cache);
		*impl_private = NULL;
		if (*mapped_address != NULL && shmdt(*mapped_address) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				   errmsg("could not unmap shared memory segment \"%s\": %m",
						  name)));
			return false;
		}
		*mapped_address = NULL;
		*mapped_size = 0;
		if (op == DSM_OP_DESTROY && shmctl(ident, IPC_RMID, NULL) < 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				  errmsg("could not remove shared memory segment \"%s\": %m",
						 name)));
			return false;
		}
		return true;
	}

	/* If we're attaching it, we must use IPC_STAT to determine the size. */
	if (op == DSM_OP_ATTACH)
	{
		struct shmid_ds shm;

		if (shmctl(ident, IPC_STAT, &shm) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not stat shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		request_size = shm.shm_segsz;
	}

	/* Map it. */
	address = shmat(ident, NULL, PG_SHMAT_FLAGS);
	if (address == (void *) -1)
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		if (op == DSM_OP_CREATE)
			shmctl(ident, IPC_RMID, NULL);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	*mapped_address = address;
	*mapped_size = request_size;

	return true;
}
#endif

#ifdef USE_DSM_WINDOWS
/*
 * Operating system primitives to support Windows shared memory.
 *
 * Windows shared memory implementation is done using file mapping
 * which can be backed by either physical file or system paging file.
 * Current implementation uses system paging file as other effects
 * like performance are not clear for physical file and it is used in similar
 * way for main shared memory in windows.
 *
 * A memory mapping object is a kernel object - they always get deleted when
 * the last reference to them goes away, either explicitly via a CloseHandle or
 * when the process containing the reference exits.
 */
static bool
dsm_impl_windows(dsm_op op, dsm_handle handle, Size request_size,
				 void **impl_private, void **mapped_address,
				 Size *mapped_size, int elevel)
{
	char	   *address;
	HANDLE		hmap;
	char		name[64];
	MEMORY_BASIC_INFORMATION info;

	/* Resize is not supported for Windows shared memory. */
	if (op == DSM_OP_RESIZE)
	{
		elog(elevel, "Windows shared memory segments cannot be resized");
		return false;
	}

	/* Since resize isn't supported, reattach is a no-op. */
	if (op == DSM_OP_ATTACH && *mapped_address != NULL)
		return true;

	/*
	 * Storing the shared memory segment in the Global\ namespace, can allow
	 * any process running in any session to access that file mapping object
	 * provided that the caller has the required access rights. But to avoid
	 * issues faced in main shared memory, we are using the naming convention
	 * similar to main shared memory. We can change here once issue mentioned
	 * in GetSharedMemName is resolved.
	 */
	snprintf(name, 64, "%s.%u", SEGMENT_NAME_PREFIX, handle);

	/*
	 * Handle teardown cases.  Since Windows automatically destroys the object
	 * when no references reamin, we can treat it the same as detach.
	 */
	if (op == DSM_OP_DETACH || op == DSM_OP_DESTROY)
	{
		if (*mapped_address != NULL
			&& UnmapViewOfFile(*mapped_address) == 0)
		{
			_dosmaperr(GetLastError());
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				   errmsg("could not unmap shared memory segment \"%s\": %m",
						  name)));
			return false;
		}
		if (*impl_private != NULL
			&& CloseHandle(*impl_private) == 0)
		{
			_dosmaperr(GetLastError());
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				  errmsg("could not remove shared memory segment \"%s\": %m",
						 name)));
			return false;
		}

		*impl_private = NULL;
		*mapped_address = NULL;
		*mapped_size = 0;
		return true;
	}

	/* Create new segment or open an existing one for attach. */
	if (op == DSM_OP_CREATE)
	{
		DWORD		size_high;
		DWORD		size_low;

		/* Shifts >= the width of the type are undefined. */
#ifdef _WIN64
		size_high = request_size >> 32;
#else
		size_high = 0;
#endif
		size_low = (DWORD) request_size;

		hmap = CreateFileMapping(INVALID_HANDLE_VALUE,	/* Use the pagefile */
								 NULL,	/* Default security attrs */
								 PAGE_READWRITE,		/* Memory is read/write */
								 size_high,		/* Upper 32 bits of size */
								 size_low,		/* Lower 32 bits of size */
								 name);
		if (!hmap)
		{
			_dosmaperr(GetLastError());
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				  errmsg("could not create shared memory segment \"%s\": %m",
						 name)));
			return false;
		}
		_dosmaperr(GetLastError());
		if (errno == EEXIST)
		{
			/*
			 * On Windows, when the segment already exists, a handle for the
			 * existing segment is returned.  We must close it before
			 * returning.  We don't do _dosmaperr here, so errno won't be
			 * modified.
			 */
			CloseHandle(hmap);
			return false;
		}
	}
	else
	{
		hmap = OpenFileMapping(FILE_MAP_WRITE | FILE_MAP_READ,
							   FALSE,	/* do not inherit the name */
							   name);	/* name of mapping object */
		if (!hmap)
		{
			_dosmaperr(GetLastError());
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not open shared memory segment \"%s\": %m",
							name)));
			return false;
		}
	}

	/* Map it. */
	address = MapViewOfFile(hmap, FILE_MAP_WRITE | FILE_MAP_READ,
							0, 0, 0);
	if (!address)
	{
		int			save_errno;

		_dosmaperr(GetLastError());
		/* Back out what's already been done. */
		save_errno = errno;
		CloseHandle(hmap);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	/*
	 * VirtualQuery gives size in page_size units, which is 4K for Windows. We
	 * need size only when we are attaching, but it's better to get the size
	 * when creating new segment to keep size consistent both for
	 * DSM_OP_CREATE and DSM_OP_ATTACH.
	 */
	if (VirtualQuery(address, &info, sizeof(info)) == 0)
	{
		int			save_errno;

		_dosmaperr(GetLastError());
		/* Back out what's already been done. */
		save_errno = errno;
		UnmapViewOfFile(address);
		CloseHandle(hmap);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not stat shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	*mapped_address = address;
	*mapped_size = info.RegionSize;
	*impl_private = hmap;

	return true;
}
#endif

#ifdef USE_DSM_MMAP
/*
 * Operating system primitives to support mmap-based shared memory.
 *
 * Calling this "shared memory" is somewhat of a misnomer, because what
 * we're really doing is creating a bunch of files and mapping them into
 * our address space.  The operating system may feel obliged to
 * synchronize the contents to disk even if nothing is being paged out,
 * which will not serve us well.  The user can relocate the pg_dynshmem
 * directory to a ramdisk to avoid this problem, if available.
 */
static bool
dsm_impl_mmap(dsm_op op, dsm_handle handle, Size request_size,
			  void **impl_private, void **mapped_address, Size *mapped_size,
			  int elevel)
{
	char		name[64];
	int			flags;
	int			fd;
	char	   *address;

	snprintf(name, 64, PG_DYNSHMEM_DIR "/" PG_DYNSHMEM_MMAP_FILE_PREFIX "%u",
			 handle);

	/* Handle teardown cases. */
	if (op == DSM_OP_DETACH || op == DSM_OP_DESTROY)
	{
		if (*mapped_address != NULL
			&& munmap(*mapped_address, *mapped_size) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				   errmsg("could not unmap shared memory segment \"%s\": %m",
						  name)));
			return false;
		}
		*mapped_address = NULL;
		*mapped_size = 0;
		if (op == DSM_OP_DESTROY && unlink(name) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				  errmsg("could not remove shared memory segment \"%s\": %m",
						 name)));
			return false;
		}
		return true;
	}

	/* Create new segment or open an existing one for attach or resize. */
	flags = O_RDWR | (op == DSM_OP_CREATE ? O_CREAT | O_EXCL : 0);
	if ((fd = OpenTransientFile(name, flags, 0600)) == -1)
	{
		if (errno != EEXIST)
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not open shared memory segment \"%s\": %m",
							name)));
		return false;
	}

	/*
	 * If we're attaching the segment, determine the current size; if we are
	 * creating or resizing the segment, set the size to the requested value.
	 */
	if (op == DSM_OP_ATTACH)
	{
		struct stat st;

		if (fstat(fd, &st) != 0)
		{
			int			save_errno;

			/* Back out what's already been done. */
			save_errno = errno;
			CloseTransientFile(fd);
			errno = save_errno;

			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not stat shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		request_size = st.st_size;
	}
	else if (*mapped_size > request_size && ftruncate(fd, request_size))
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		close(fd);
		if (op == DSM_OP_CREATE)
			unlink(name);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not resize shared memory segment \"%s\" to %zu bytes: %m",
						name, request_size)));
		return false;
	}
	else if (*mapped_size < request_size)
	{
		/*
		 * Allocate a buffer full of zeros.
		 *
		 * Note: palloc zbuffer, instead of just using a local char array, to
		 * ensure it is reasonably well-aligned; this may save a few cycles
		 * transferring data to the kernel.
		 */
		char	   *zbuffer = (char *) palloc0(ZBUFFER_SIZE);
		uint32		remaining = request_size;
		bool		success = true;

		/*
		 * Zero-fill the file. We have to do this the hard way to ensure that
		 * all the file space has really been allocated, so that we don't
		 * later seg fault when accessing the memory mapping.  This is pretty
		 * pessimal.
		 */
		while (success && remaining > 0)
		{
			Size		goal = remaining;

			if (goal > ZBUFFER_SIZE)
				goal = ZBUFFER_SIZE;
			if (write(fd, zbuffer, goal) == goal)
				remaining -= goal;
			else
				success = false;
		}

		if (!success)
		{
			int			save_errno;

			/* Back out what's already been done. */
			save_errno = errno;
			CloseTransientFile(fd);
			if (op == DSM_OP_CREATE)
				unlink(name);
			errno = save_errno ? save_errno : ENOSPC;

			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not resize shared memory segment \"%s\" to %zu bytes: %m",
							name, request_size)));
			return false;
		}
	}

	/*
	 * If we're reattaching or resizing, we must remove any existing mapping,
	 * unless we've already got the right thing mapped.
	 */
	if (*mapped_address != NULL)
	{
		if (*mapped_size == request_size)
			return true;
		if (munmap(*mapped_address, *mapped_size) != 0)
		{
			int			save_errno;

			/* Back out what's already been done. */
			save_errno = errno;
			CloseTransientFile(fd);
			if (op == DSM_OP_CREATE)
				unlink(name);
			errno = save_errno;

			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
				   errmsg("could not unmap shared memory segment \"%s\": %m",
						  name)));
			return false;
		}
		*mapped_address = NULL;
		*mapped_size = 0;
	}

	/* Map it. */
	address = mmap(NULL, request_size, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_HASSEMAPHORE | MAP_NOSYNC, fd, 0);
	if (address == MAP_FAILED)
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		CloseTransientFile(fd);
		if (op == DSM_OP_CREATE)
			unlink(name);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	*mapped_address = address;
	*mapped_size = request_size;
	CloseTransientFile(fd);

	return true;
}
#endif

/*
 * Implementation-specific actions that must be performed when a segment
 * is to be preserved until postmaster shutdown.
 *
 * Except on Windows, we don't need to do anything at all.  But since Windows
 * cleans up segments automatically when no references remain, we duplicate
 * the segment handle into the postmaster process.  The postmaster needn't
 * do anything to receive the handle; Windows transfers it automatically.
 */
void
dsm_impl_pin_segment(dsm_handle handle, void *impl_private)
{
	switch (dynamic_shared_memory_type)
	{
#ifdef USE_DSM_WINDOWS
		case DSM_IMPL_WINDOWS:
			{
				HANDLE		hmap;

				if (!DuplicateHandle(GetCurrentProcess(), impl_private,
									 PostmasterHandle, &hmap, 0, FALSE,
									 DUPLICATE_SAME_ACCESS))
				{
					char		name[64];

					snprintf(name, 64, "%s.%u", SEGMENT_NAME_PREFIX, handle);
					_dosmaperr(GetLastError());
					ereport(ERROR,
							(errcode_for_dynamic_shared_memory(),
						  errmsg("could not duplicate handle for \"%s\": %m",
								 name)));
				}
				break;
			}
#endif
		default:
			break;
	}
}

static int
errcode_for_dynamic_shared_memory(void)
{
	if (errno == EFBIG || errno == ENOMEM)
		return errcode(ERRCODE_OUT_OF_MEMORY);
	else
		return errcode_for_file_access();
}
