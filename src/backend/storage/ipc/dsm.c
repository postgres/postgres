/*-------------------------------------------------------------------------
 *
 * dsm.c
 *	  manage dynamic shared memory segments
 *
 * This file provides a set of services to make programming with dynamic
 * shared memory segments more convenient.  Unlike the low-level
 * facilities provided by dsm_impl.h and dsm_impl.c, mappings and segments
 * created using this module will be cleaned up automatically.  Mappings
 * will be removed when the resource owner under which they were created
 * is cleaned up, unless dsm_pin_mapping() is used, in which case they
 * have session lifespan.  Segments will be removed when there are no
 * remaining mappings, or at postmaster shutdown in any case.  After a
 * hard postmaster crash, remaining segments will be removed, if they
 * still exist, at the next postmaster startup.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <sys/stat.h>

#include "common/pg_prng.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "utils/freepage.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

#define PG_DYNSHMEM_CONTROL_MAGIC		0x9a503d32

#define PG_DYNSHMEM_FIXED_SLOTS			64
#define PG_DYNSHMEM_SLOTS_PER_BACKEND	5

#define INVALID_CONTROL_SLOT		((uint32) -1)

/* Backend-local tracking for on-detach callbacks. */
typedef struct dsm_segment_detach_callback
{
	on_dsm_detach_callback function;
	Datum		arg;
	slist_node	node;
} dsm_segment_detach_callback;

/* Backend-local state for a dynamic shared memory segment. */
struct dsm_segment
{
	dlist_node	node;			/* List link in dsm_segment_list. */
	ResourceOwner resowner;		/* Resource owner. */
	dsm_handle	handle;			/* Segment name. */
	uint32		control_slot;	/* Slot in control segment. */
	void	   *impl_private;	/* Implementation-specific private data. */
	void	   *mapped_address; /* Mapping address, or NULL if unmapped. */
	Size		mapped_size;	/* Size of our mapping. */
	slist_head	on_detach;		/* On-detach callbacks. */
};

/* Shared-memory state for a dynamic shared memory segment. */
typedef struct dsm_control_item
{
	dsm_handle	handle;
	uint32		refcnt;			/* 2+ = active, 1 = moribund, 0 = gone */
	size_t		first_page;
	size_t		npages;
	void	   *impl_private_pm_handle; /* only needed on Windows */
	bool		pinned;
} dsm_control_item;

/* Layout of the dynamic shared memory control segment. */
typedef struct dsm_control_header
{
	uint32		magic;
	uint32		nitems;
	uint32		maxitems;
	dsm_control_item item[FLEXIBLE_ARRAY_MEMBER];
} dsm_control_header;

static void dsm_cleanup_for_mmap(void);
static void dsm_postmaster_shutdown(int code, Datum arg);
static dsm_segment *dsm_create_descriptor(void);
static bool dsm_control_segment_sane(dsm_control_header *control,
									 Size mapped_size);
static uint64 dsm_control_bytes_needed(uint32 nitems);
static inline dsm_handle make_main_region_dsm_handle(int slot);
static inline bool is_main_region_dsm_handle(dsm_handle handle);

/* Has this backend initialized the dynamic shared memory system yet? */
static bool dsm_init_done = false;

/* Preallocated DSM space in the main shared memory region. */
static void *dsm_main_space_begin = NULL;

/*
 * List of dynamic shared memory segments used by this backend.
 *
 * At process exit time, we must decrement the reference count of each
 * segment we have attached; this list makes it possible to find all such
 * segments.
 *
 * This list should always be empty in the postmaster.  We could probably
 * allow the postmaster to map dynamic shared memory segments before it
 * begins to start child processes, provided that each process adjusted
 * the reference counts for those segments in the control segment at
 * startup time, but there's no obvious need for such a facility, which
 * would also be complex to handle in the EXEC_BACKEND case.  Once the
 * postmaster has begun spawning children, there's an additional problem:
 * each new mapping would require an update to the control segment,
 * which requires locking, in which the postmaster must not be involved.
 */
static dlist_head dsm_segment_list = DLIST_STATIC_INIT(dsm_segment_list);

/*
 * Control segment information.
 *
 * Unlike ordinary shared memory segments, the control segment is not
 * reference counted; instead, it lasts for the postmaster's entire
 * life cycle.  For simplicity, it doesn't have a dsm_segment object either.
 */
static dsm_handle dsm_control_handle;
static dsm_control_header *dsm_control;
static Size dsm_control_mapped_size = 0;
static void *dsm_control_impl_private = NULL;


/* ResourceOwner callbacks to hold DSM segments */
static void ResOwnerReleaseDSM(Datum res);
static char *ResOwnerPrintDSM(Datum res);

static const ResourceOwnerDesc dsm_resowner_desc =
{
	.name = "dynamic shared memory segment",
	.release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
	.release_priority = RELEASE_PRIO_DSMS,
	.ReleaseResource = ResOwnerReleaseDSM,
	.DebugPrint = ResOwnerPrintDSM
};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void
ResourceOwnerRememberDSM(ResourceOwner owner, dsm_segment *seg)
{
	ResourceOwnerRemember(owner, PointerGetDatum(seg), &dsm_resowner_desc);
}
static inline void
ResourceOwnerForgetDSM(ResourceOwner owner, dsm_segment *seg)
{
	ResourceOwnerForget(owner, PointerGetDatum(seg), &dsm_resowner_desc);
}

/*
 * Start up the dynamic shared memory system.
 *
 * This is called just once during each cluster lifetime, at postmaster
 * startup time.
 */
void
dsm_postmaster_startup(PGShmemHeader *shim)
{
	void	   *dsm_control_address = NULL;
	uint32		maxitems;
	Size		segsize;

	Assert(!IsUnderPostmaster);

	/*
	 * If we're using the mmap implementations, clean up any leftovers.
	 * Cleanup isn't needed on Windows, and happens earlier in startup for
	 * POSIX and System V shared memory, via a direct call to
	 * dsm_cleanup_using_control_segment.
	 */
	if (dynamic_shared_memory_type == DSM_IMPL_MMAP)
		dsm_cleanup_for_mmap();

	/* Determine size for new control segment. */
	maxitems = PG_DYNSHMEM_FIXED_SLOTS
		+ PG_DYNSHMEM_SLOTS_PER_BACKEND * MaxBackends;
	elog(DEBUG2, "dynamic shared memory system will support %u segments",
		 maxitems);
	segsize = dsm_control_bytes_needed(maxitems);

	/*
	 * Loop until we find an unused identifier for the new control segment. We
	 * sometimes use DSM_HANDLE_INVALID as a sentinel value indicating "no
	 * control segment", so avoid generating that value for a real handle.
	 */
	for (;;)
	{
		Assert(dsm_control_address == NULL);
		Assert(dsm_control_mapped_size == 0);
		/* Use even numbers only */
		dsm_control_handle = pg_prng_uint32(&pg_global_prng_state) << 1;
		if (dsm_control_handle == DSM_HANDLE_INVALID)
			continue;
		if (dsm_impl_op(DSM_OP_CREATE, dsm_control_handle, segsize,
						&dsm_control_impl_private, &dsm_control_address,
						&dsm_control_mapped_size, ERROR))
			break;
	}
	dsm_control = dsm_control_address;
	on_shmem_exit(dsm_postmaster_shutdown, PointerGetDatum(shim));
	elog(DEBUG2,
		 "created dynamic shared memory control segment %u (%zu bytes)",
		 dsm_control_handle, segsize);
	shim->dsm_control = dsm_control_handle;

	/* Initialize control segment. */
	dsm_control->magic = PG_DYNSHMEM_CONTROL_MAGIC;
	dsm_control->nitems = 0;
	dsm_control->maxitems = maxitems;
}

/*
 * Determine whether the control segment from the previous postmaster
 * invocation still exists.  If so, remove the dynamic shared memory
 * segments to which it refers, and then the control segment itself.
 */
void
dsm_cleanup_using_control_segment(dsm_handle old_control_handle)
{
	void	   *mapped_address = NULL;
	void	   *junk_mapped_address = NULL;
	void	   *impl_private = NULL;
	void	   *junk_impl_private = NULL;
	Size		mapped_size = 0;
	Size		junk_mapped_size = 0;
	uint32		nitems;
	uint32		i;
	dsm_control_header *old_control;

	/*
	 * Try to attach the segment.  If this fails, it probably just means that
	 * the operating system has been rebooted and the segment no longer
	 * exists, or an unrelated process has used the same shm ID.  So just fall
	 * out quietly.
	 */
	if (!dsm_impl_op(DSM_OP_ATTACH, old_control_handle, 0, &impl_private,
					 &mapped_address, &mapped_size, DEBUG1))
		return;

	/*
	 * We've managed to reattach it, but the contents might not be sane. If
	 * they aren't, we disregard the segment after all.
	 */
	old_control = (dsm_control_header *) mapped_address;
	if (!dsm_control_segment_sane(old_control, mapped_size))
	{
		dsm_impl_op(DSM_OP_DETACH, old_control_handle, 0, &impl_private,
					&mapped_address, &mapped_size, LOG);
		return;
	}

	/*
	 * OK, the control segment looks basically valid, so we can use it to get
	 * a list of segments that need to be removed.
	 */
	nitems = old_control->nitems;
	for (i = 0; i < nitems; ++i)
	{
		dsm_handle	handle;
		uint32		refcnt;

		/* If the reference count is 0, the slot is actually unused. */
		refcnt = old_control->item[i].refcnt;
		if (refcnt == 0)
			continue;

		/* If it was using the main shmem area, there is nothing to do. */
		handle = old_control->item[i].handle;
		if (is_main_region_dsm_handle(handle))
			continue;

		/* Log debugging information. */
		elog(DEBUG2, "cleaning up orphaned dynamic shared memory with ID %u (reference count %u)",
			 handle, refcnt);

		/* Destroy the referenced segment. */
		dsm_impl_op(DSM_OP_DESTROY, handle, 0, &junk_impl_private,
					&junk_mapped_address, &junk_mapped_size, LOG);
	}

	/* Destroy the old control segment, too. */
	elog(DEBUG2,
		 "cleaning up dynamic shared memory control segment with ID %u",
		 old_control_handle);
	dsm_impl_op(DSM_OP_DESTROY, old_control_handle, 0, &impl_private,
				&mapped_address, &mapped_size, LOG);
}

/*
 * When we're using the mmap shared memory implementation, "shared memory"
 * segments might even manage to survive an operating system reboot.
 * But there's no guarantee as to exactly what will survive: some segments
 * may survive, and others may not, and the contents of some may be out
 * of date.  In particular, the control segment may be out of date, so we
 * can't rely on it to figure out what to remove.  However, since we know
 * what directory contains the files we used as shared memory, we can simply
 * scan the directory and blow everything away that shouldn't be there.
 */
static void
dsm_cleanup_for_mmap(void)
{
	DIR		   *dir;
	struct dirent *dent;

	/* Scan the directory for something with a name of the correct format. */
	dir = AllocateDir(PG_DYNSHMEM_DIR);

	while ((dent = ReadDir(dir, PG_DYNSHMEM_DIR)) != NULL)
	{
		if (strncmp(dent->d_name, PG_DYNSHMEM_MMAP_FILE_PREFIX,
					strlen(PG_DYNSHMEM_MMAP_FILE_PREFIX)) == 0)
		{
			char		buf[MAXPGPATH + sizeof(PG_DYNSHMEM_DIR)];

			snprintf(buf, sizeof(buf), PG_DYNSHMEM_DIR "/%s", dent->d_name);

			elog(DEBUG2, "removing file \"%s\"", buf);

			/* We found a matching file; so remove it. */
			if (unlink(buf) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", buf)));
		}
	}

	/* Cleanup complete. */
	FreeDir(dir);
}

/*
 * At shutdown time, we iterate over the control segment and remove all
 * remaining dynamic shared memory segments.  We avoid throwing errors here;
 * the postmaster is shutting down either way, and this is just non-critical
 * resource cleanup.
 */
static void
dsm_postmaster_shutdown(int code, Datum arg)
{
	uint32		nitems;
	uint32		i;
	void	   *dsm_control_address;
	void	   *junk_mapped_address = NULL;
	void	   *junk_impl_private = NULL;
	Size		junk_mapped_size = 0;
	PGShmemHeader *shim = (PGShmemHeader *) DatumGetPointer(arg);

	/*
	 * If some other backend exited uncleanly, it might have corrupted the
	 * control segment while it was dying.  In that case, we warn and ignore
	 * the contents of the control segment.  This may end up leaving behind
	 * stray shared memory segments, but there's not much we can do about that
	 * if the metadata is gone.
	 */
	nitems = dsm_control->nitems;
	if (!dsm_control_segment_sane(dsm_control, dsm_control_mapped_size))
	{
		ereport(LOG,
				(errmsg("dynamic shared memory control segment is corrupt")));
		return;
	}

	/* Remove any remaining segments. */
	for (i = 0; i < nitems; ++i)
	{
		dsm_handle	handle;

		/* If the reference count is 0, the slot is actually unused. */
		if (dsm_control->item[i].refcnt == 0)
			continue;

		handle = dsm_control->item[i].handle;
		if (is_main_region_dsm_handle(handle))
			continue;

		/* Log debugging information. */
		elog(DEBUG2, "cleaning up orphaned dynamic shared memory with ID %u",
			 handle);

		/* Destroy the segment. */
		dsm_impl_op(DSM_OP_DESTROY, handle, 0, &junk_impl_private,
					&junk_mapped_address, &junk_mapped_size, LOG);
	}

	/* Remove the control segment itself. */
	elog(DEBUG2,
		 "cleaning up dynamic shared memory control segment with ID %u",
		 dsm_control_handle);
	dsm_control_address = dsm_control;
	dsm_impl_op(DSM_OP_DESTROY, dsm_control_handle, 0,
				&dsm_control_impl_private, &dsm_control_address,
				&dsm_control_mapped_size, LOG);
	dsm_control = dsm_control_address;
	shim->dsm_control = 0;
}

/*
 * Prepare this backend for dynamic shared memory usage.  Under EXEC_BACKEND,
 * we must reread the state file and map the control segment; in other cases,
 * we'll have inherited the postmaster's mapping and global variables.
 */
static void
dsm_backend_startup(void)
{
#ifdef EXEC_BACKEND
	if (IsUnderPostmaster)
	{
		void	   *control_address = NULL;

		/* Attach control segment. */
		Assert(dsm_control_handle != 0);
		dsm_impl_op(DSM_OP_ATTACH, dsm_control_handle, 0,
					&dsm_control_impl_private, &control_address,
					&dsm_control_mapped_size, ERROR);
		dsm_control = control_address;
		/* If control segment doesn't look sane, something is badly wrong. */
		if (!dsm_control_segment_sane(dsm_control, dsm_control_mapped_size))
		{
			dsm_impl_op(DSM_OP_DETACH, dsm_control_handle, 0,
						&dsm_control_impl_private, &control_address,
						&dsm_control_mapped_size, WARNING);
			ereport(FATAL,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("dynamic shared memory control segment is not valid")));
		}
	}
#endif

	dsm_init_done = true;
}

#ifdef EXEC_BACKEND
/*
 * When running under EXEC_BACKEND, we get a callback here when the main
 * shared memory segment is re-attached, so that we can record the control
 * handle retrieved from it.
 */
void
dsm_set_control_handle(dsm_handle h)
{
	Assert(dsm_control_handle == 0 && h != 0);
	dsm_control_handle = h;
}
#endif

/*
 * Reserve some space in the main shared memory segment for DSM segments.
 */
size_t
dsm_estimate_size(void)
{
	return 1024 * 1024 * (size_t) min_dynamic_shared_memory;
}

/*
 * Initialize space in the main shared memory segment for DSM segments.
 */
void
dsm_shmem_init(void)
{
	size_t		size = dsm_estimate_size();
	bool		found;

	if (size == 0)
		return;

	dsm_main_space_begin = ShmemInitStruct("Preallocated DSM", size, &found);
	if (!found)
	{
		FreePageManager *fpm = (FreePageManager *) dsm_main_space_begin;
		size_t		first_page = 0;
		size_t		pages;

		/* Reserve space for the FreePageManager. */
		while (first_page * FPM_PAGE_SIZE < sizeof(FreePageManager))
			++first_page;

		/* Initialize it and give it all the rest of the space. */
		FreePageManagerInitialize(fpm, dsm_main_space_begin);
		pages = (size / FPM_PAGE_SIZE) - first_page;
		FreePageManagerPut(fpm, first_page, pages);
	}
}

/*
 * Create a new dynamic shared memory segment.
 *
 * If there is a non-NULL CurrentResourceOwner, the new segment is associated
 * with it and must be detached before the resource owner releases, or a
 * warning will be logged.  If CurrentResourceOwner is NULL, the segment
 * remains attached until explicitly detached or the session ends.
 * Creating with a NULL CurrentResourceOwner is equivalent to creating
 * with a non-NULL CurrentResourceOwner and then calling dsm_pin_mapping.
 */
dsm_segment *
dsm_create(Size size, int flags)
{
	dsm_segment *seg;
	uint32		i;
	uint32		nitems;
	size_t		npages = 0;
	size_t		first_page = 0;
	FreePageManager *dsm_main_space_fpm = dsm_main_space_begin;
	bool		using_main_dsm_region = false;

	/*
	 * Unsafe in postmaster. It might seem pointless to allow use of dsm in
	 * single user mode, but otherwise some subsystems will need dedicated
	 * single user mode code paths.
	 */
	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);

	if (!dsm_init_done)
		dsm_backend_startup();

	/* Create a new segment descriptor. */
	seg = dsm_create_descriptor();

	/*
	 * Lock the control segment while we try to allocate from the main shared
	 * memory area, if configured.
	 */
	if (dsm_main_space_fpm)
	{
		npages = size / FPM_PAGE_SIZE;
		if (size % FPM_PAGE_SIZE > 0)
			++npages;

		LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
		if (FreePageManagerGet(dsm_main_space_fpm, npages, &first_page))
		{
			/* We can carve out a piece of the main shared memory segment. */
			seg->mapped_address = (char *) dsm_main_space_begin +
				first_page * FPM_PAGE_SIZE;
			seg->mapped_size = npages * FPM_PAGE_SIZE;
			using_main_dsm_region = true;
			/* We'll choose a handle below. */
		}
	}

	if (!using_main_dsm_region)
	{
		/*
		 * We need to create a new memory segment.  Loop until we find an
		 * unused segment identifier.
		 */
		if (dsm_main_space_fpm)
			LWLockRelease(DynamicSharedMemoryControlLock);
		for (;;)
		{
			Assert(seg->mapped_address == NULL && seg->mapped_size == 0);
			/* Use even numbers only */
			seg->handle = pg_prng_uint32(&pg_global_prng_state) << 1;
			if (seg->handle == DSM_HANDLE_INVALID)	/* Reserve sentinel */
				continue;
			if (dsm_impl_op(DSM_OP_CREATE, seg->handle, size, &seg->impl_private,
							&seg->mapped_address, &seg->mapped_size, ERROR))
				break;
		}
		LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	}

	/* Search the control segment for an unused slot. */
	nitems = dsm_control->nitems;
	for (i = 0; i < nitems; ++i)
	{
		if (dsm_control->item[i].refcnt == 0)
		{
			if (using_main_dsm_region)
			{
				seg->handle = make_main_region_dsm_handle(i);
				dsm_control->item[i].first_page = first_page;
				dsm_control->item[i].npages = npages;
			}
			else
				Assert(!is_main_region_dsm_handle(seg->handle));
			dsm_control->item[i].handle = seg->handle;
			/* refcnt of 1 triggers destruction, so start at 2 */
			dsm_control->item[i].refcnt = 2;
			dsm_control->item[i].impl_private_pm_handle = NULL;
			dsm_control->item[i].pinned = false;
			seg->control_slot = i;
			LWLockRelease(DynamicSharedMemoryControlLock);
			return seg;
		}
	}

	/* Verify that we can support an additional mapping. */
	if (nitems >= dsm_control->maxitems)
	{
		if (using_main_dsm_region)
			FreePageManagerPut(dsm_main_space_fpm, first_page, npages);
		LWLockRelease(DynamicSharedMemoryControlLock);
		if (!using_main_dsm_region)
			dsm_impl_op(DSM_OP_DESTROY, seg->handle, 0, &seg->impl_private,
						&seg->mapped_address, &seg->mapped_size, WARNING);
		if (seg->resowner != NULL)
			ResourceOwnerForgetDSM(seg->resowner, seg);
		dlist_delete(&seg->node);
		pfree(seg);

		if ((flags & DSM_CREATE_NULL_IF_MAXSEGMENTS) != 0)
			return NULL;
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("too many dynamic shared memory segments")));
	}

	/* Enter the handle into a new array slot. */
	if (using_main_dsm_region)
	{
		seg->handle = make_main_region_dsm_handle(nitems);
		dsm_control->item[i].first_page = first_page;
		dsm_control->item[i].npages = npages;
	}
	dsm_control->item[nitems].handle = seg->handle;
	/* refcnt of 1 triggers destruction, so start at 2 */
	dsm_control->item[nitems].refcnt = 2;
	dsm_control->item[nitems].impl_private_pm_handle = NULL;
	dsm_control->item[nitems].pinned = false;
	seg->control_slot = nitems;
	dsm_control->nitems++;
	LWLockRelease(DynamicSharedMemoryControlLock);

	return seg;
}

/*
 * Attach a dynamic shared memory segment.
 *
 * See comments for dsm_segment_handle() for an explanation of how this
 * is intended to be used.
 *
 * This function will return NULL if the segment isn't known to the system.
 * This can happen if we're asked to attach the segment, but then everyone
 * else detaches it (causing it to be destroyed) before we get around to
 * attaching it.
 *
 * If there is a non-NULL CurrentResourceOwner, the attached segment is
 * associated with it and must be detached before the resource owner releases,
 * or a warning will be logged.  Otherwise the segment remains attached until
 * explicitly detached or the session ends.  See the note atop dsm_create().
 */
dsm_segment *
dsm_attach(dsm_handle h)
{
	dsm_segment *seg;
	dlist_iter	iter;
	uint32		i;
	uint32		nitems;

	/* Unsafe in postmaster (and pointless in a stand-alone backend). */
	Assert(IsUnderPostmaster);

	if (!dsm_init_done)
		dsm_backend_startup();

	/*
	 * Since this is just a debugging cross-check, we could leave it out
	 * altogether, or include it only in assert-enabled builds.  But since the
	 * list of attached segments should normally be very short, let's include
	 * it always for right now.
	 *
	 * If you're hitting this error, you probably want to attempt to find an
	 * existing mapping via dsm_find_mapping() before calling dsm_attach() to
	 * create a new one.
	 */
	dlist_foreach(iter, &dsm_segment_list)
	{
		seg = dlist_container(dsm_segment, node, iter.cur);
		if (seg->handle == h)
			elog(ERROR, "can't attach the same segment more than once");
	}

	/* Create a new segment descriptor. */
	seg = dsm_create_descriptor();
	seg->handle = h;

	/* Bump reference count for this segment in shared memory. */
	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	nitems = dsm_control->nitems;
	for (i = 0; i < nitems; ++i)
	{
		/*
		 * If the reference count is 0, the slot is actually unused.  If the
		 * reference count is 1, the slot is still in use, but the segment is
		 * in the process of going away; even if the handle matches, another
		 * slot may already have started using the same handle value by
		 * coincidence so we have to keep searching.
		 */
		if (dsm_control->item[i].refcnt <= 1)
			continue;

		/* If the handle doesn't match, it's not the slot we want. */
		if (dsm_control->item[i].handle != seg->handle)
			continue;

		/* Otherwise we've found a match. */
		dsm_control->item[i].refcnt++;
		seg->control_slot = i;
		if (is_main_region_dsm_handle(seg->handle))
		{
			seg->mapped_address = (char *) dsm_main_space_begin +
				dsm_control->item[i].first_page * FPM_PAGE_SIZE;
			seg->mapped_size = dsm_control->item[i].npages * FPM_PAGE_SIZE;
		}
		break;
	}
	LWLockRelease(DynamicSharedMemoryControlLock);

	/*
	 * If we didn't find the handle we're looking for in the control segment,
	 * it probably means that everyone else who had it mapped, including the
	 * original creator, died before we got to this point. It's up to the
	 * caller to decide what to do about that.
	 */
	if (seg->control_slot == INVALID_CONTROL_SLOT)
	{
		dsm_detach(seg);
		return NULL;
	}

	/* Here's where we actually try to map the segment. */
	if (!is_main_region_dsm_handle(seg->handle))
		dsm_impl_op(DSM_OP_ATTACH, seg->handle, 0, &seg->impl_private,
					&seg->mapped_address, &seg->mapped_size, ERROR);

	return seg;
}

/*
 * At backend shutdown time, detach any segments that are still attached.
 * (This is similar to dsm_detach_all, except that there's no reason to
 * unmap the control segment before exiting, so we don't bother.)
 */
void
dsm_backend_shutdown(void)
{
	while (!dlist_is_empty(&dsm_segment_list))
	{
		dsm_segment *seg;

		seg = dlist_head_element(dsm_segment, node, &dsm_segment_list);
		dsm_detach(seg);
	}
}

/*
 * Detach all shared memory segments, including the control segments.  This
 * should be called, along with PGSharedMemoryDetach, in processes that
 * might inherit mappings but are not intended to be connected to dynamic
 * shared memory.
 */
void
dsm_detach_all(void)
{
	void	   *control_address = dsm_control;

	while (!dlist_is_empty(&dsm_segment_list))
	{
		dsm_segment *seg;

		seg = dlist_head_element(dsm_segment, node, &dsm_segment_list);
		dsm_detach(seg);
	}

	if (control_address != NULL)
		dsm_impl_op(DSM_OP_DETACH, dsm_control_handle, 0,
					&dsm_control_impl_private, &control_address,
					&dsm_control_mapped_size, ERROR);
}

/*
 * Detach from a shared memory segment, destroying the segment if we
 * remove the last reference.
 *
 * This function should never fail.  It will often be invoked when aborting
 * a transaction, and a further error won't serve any purpose.  It's not a
 * complete disaster if we fail to unmap or destroy the segment; it means a
 * resource leak, but that doesn't necessarily preclude further operations.
 */
void
dsm_detach(dsm_segment *seg)
{
	/*
	 * Invoke registered callbacks.  Just in case one of those callbacks
	 * throws a further error that brings us back here, pop the callback
	 * before invoking it, to avoid infinite error recursion.  Don't allow
	 * interrupts while running the individual callbacks in non-error code
	 * paths, to avoid leaving cleanup work unfinished if we're interrupted by
	 * a statement timeout or similar.
	 */
	HOLD_INTERRUPTS();
	while (!slist_is_empty(&seg->on_detach))
	{
		slist_node *node;
		dsm_segment_detach_callback *cb;
		on_dsm_detach_callback function;
		Datum		arg;

		node = slist_pop_head_node(&seg->on_detach);
		cb = slist_container(dsm_segment_detach_callback, node, node);
		function = cb->function;
		arg = cb->arg;
		pfree(cb);

		function(seg, arg);
	}
	RESUME_INTERRUPTS();

	/*
	 * Try to remove the mapping, if one exists.  Normally, there will be, but
	 * maybe not, if we failed partway through a create or attach operation.
	 * We remove the mapping before decrementing the reference count so that
	 * the process that sees a zero reference count can be certain that no
	 * remaining mappings exist.  Even if this fails, we pretend that it
	 * works, because retrying is likely to fail in the same way.
	 */
	if (seg->mapped_address != NULL)
	{
		if (!is_main_region_dsm_handle(seg->handle))
			dsm_impl_op(DSM_OP_DETACH, seg->handle, 0, &seg->impl_private,
						&seg->mapped_address, &seg->mapped_size, WARNING);
		seg->impl_private = NULL;
		seg->mapped_address = NULL;
		seg->mapped_size = 0;
	}

	/* Reduce reference count, if we previously increased it. */
	if (seg->control_slot != INVALID_CONTROL_SLOT)
	{
		uint32		refcnt;
		uint32		control_slot = seg->control_slot;

		LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
		Assert(dsm_control->item[control_slot].handle == seg->handle);
		Assert(dsm_control->item[control_slot].refcnt > 1);
		refcnt = --dsm_control->item[control_slot].refcnt;
		seg->control_slot = INVALID_CONTROL_SLOT;
		LWLockRelease(DynamicSharedMemoryControlLock);

		/* If new reference count is 1, try to destroy the segment. */
		if (refcnt == 1)
		{
			/* A pinned segment should never reach 1. */
			Assert(!dsm_control->item[control_slot].pinned);

			/*
			 * If we fail to destroy the segment here, or are killed before we
			 * finish doing so, the reference count will remain at 1, which
			 * will mean that nobody else can attach to the segment.  At
			 * postmaster shutdown time, or when a new postmaster is started
			 * after a hard kill, another attempt will be made to remove the
			 * segment.
			 *
			 * The main case we're worried about here is being killed by a
			 * signal before we can finish removing the segment.  In that
			 * case, it's important to be sure that the segment still gets
			 * removed. If we actually fail to remove the segment for some
			 * other reason, the postmaster may not have any better luck than
			 * we did.  There's not much we can do about that, though.
			 */
			if (is_main_region_dsm_handle(seg->handle) ||
				dsm_impl_op(DSM_OP_DESTROY, seg->handle, 0, &seg->impl_private,
							&seg->mapped_address, &seg->mapped_size, WARNING))
			{
				LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
				if (is_main_region_dsm_handle(seg->handle))
					FreePageManagerPut((FreePageManager *) dsm_main_space_begin,
									   dsm_control->item[control_slot].first_page,
									   dsm_control->item[control_slot].npages);
				Assert(dsm_control->item[control_slot].handle == seg->handle);
				Assert(dsm_control->item[control_slot].refcnt == 1);
				dsm_control->item[control_slot].refcnt = 0;
				LWLockRelease(DynamicSharedMemoryControlLock);
			}
		}
	}

	/* Clean up our remaining backend-private data structures. */
	if (seg->resowner != NULL)
		ResourceOwnerForgetDSM(seg->resowner, seg);
	dlist_delete(&seg->node);
	pfree(seg);
}

/*
 * Keep a dynamic shared memory mapping until end of session.
 *
 * By default, mappings are owned by the current resource owner, which
 * typically means they stick around for the duration of the current query
 * only.
 */
void
dsm_pin_mapping(dsm_segment *seg)
{
	if (seg->resowner != NULL)
	{
		ResourceOwnerForgetDSM(seg->resowner, seg);
		seg->resowner = NULL;
	}
}

/*
 * Arrange to remove a dynamic shared memory mapping at cleanup time.
 *
 * dsm_pin_mapping() can be used to preserve a mapping for the entire
 * lifetime of a process; this function reverses that decision, making
 * the segment owned by the current resource owner.  This may be useful
 * just before performing some operation that will invalidate the segment
 * for future use by this backend.
 */
void
dsm_unpin_mapping(dsm_segment *seg)
{
	Assert(seg->resowner == NULL);
	ResourceOwnerEnlarge(CurrentResourceOwner);
	seg->resowner = CurrentResourceOwner;
	ResourceOwnerRememberDSM(seg->resowner, seg);
}

/*
 * Keep a dynamic shared memory segment until postmaster shutdown, or until
 * dsm_unpin_segment is called.
 *
 * This function should not be called more than once per segment, unless the
 * segment is explicitly unpinned with dsm_unpin_segment in between calls.
 *
 * Note that this function does not arrange for the current process to
 * keep the segment mapped indefinitely; if that behavior is desired,
 * dsm_pin_mapping() should be used from each process that needs to
 * retain the mapping.
 */
void
dsm_pin_segment(dsm_segment *seg)
{
	void	   *handle = NULL;

	/*
	 * Bump reference count for this segment in shared memory. This will
	 * ensure that even if there is no session which is attached to this
	 * segment, it will remain until postmaster shutdown or an explicit call
	 * to unpin.
	 */
	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	if (dsm_control->item[seg->control_slot].pinned)
		elog(ERROR, "cannot pin a segment that is already pinned");
	if (!is_main_region_dsm_handle(seg->handle))
		dsm_impl_pin_segment(seg->handle, seg->impl_private, &handle);
	dsm_control->item[seg->control_slot].pinned = true;
	dsm_control->item[seg->control_slot].refcnt++;
	dsm_control->item[seg->control_slot].impl_private_pm_handle = handle;
	LWLockRelease(DynamicSharedMemoryControlLock);
}

/*
 * Unpin a dynamic shared memory segment that was previously pinned with
 * dsm_pin_segment.  This function should not be called unless dsm_pin_segment
 * was previously called for this segment.
 *
 * The argument is a dsm_handle rather than a dsm_segment in case you want
 * to unpin a segment to which you haven't attached.  This turns out to be
 * useful if, for example, a reference to one shared memory segment is stored
 * within another shared memory segment.  You might want to unpin the
 * referenced segment before destroying the referencing segment.
 */
void
dsm_unpin_segment(dsm_handle handle)
{
	uint32		control_slot = INVALID_CONTROL_SLOT;
	bool		destroy = false;
	uint32		i;

	/* Find the control slot for the given handle. */
	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	for (i = 0; i < dsm_control->nitems; ++i)
	{
		/* Skip unused slots and segments that are concurrently going away. */
		if (dsm_control->item[i].refcnt <= 1)
			continue;

		/* If we've found our handle, we can stop searching. */
		if (dsm_control->item[i].handle == handle)
		{
			control_slot = i;
			break;
		}
	}

	/*
	 * We should definitely have found the slot, and it should not already be
	 * in the process of going away, because this function should only be
	 * called on a segment which is pinned.
	 */
	if (control_slot == INVALID_CONTROL_SLOT)
		elog(ERROR, "cannot unpin unknown segment handle");
	if (!dsm_control->item[control_slot].pinned)
		elog(ERROR, "cannot unpin a segment that is not pinned");
	Assert(dsm_control->item[control_slot].refcnt > 1);

	/*
	 * Allow implementation-specific code to run.  We have to do this before
	 * releasing the lock, because impl_private_pm_handle may get modified by
	 * dsm_impl_unpin_segment.
	 */
	if (!is_main_region_dsm_handle(handle))
		dsm_impl_unpin_segment(handle,
							   &dsm_control->item[control_slot].impl_private_pm_handle);

	/* Note that 1 means no references (0 means unused slot). */
	if (--dsm_control->item[control_slot].refcnt == 1)
		destroy = true;
	dsm_control->item[control_slot].pinned = false;

	/* Now we can release the lock. */
	LWLockRelease(DynamicSharedMemoryControlLock);

	/* Clean up resources if that was the last reference. */
	if (destroy)
	{
		void	   *junk_impl_private = NULL;
		void	   *junk_mapped_address = NULL;
		Size		junk_mapped_size = 0;

		/*
		 * For an explanation of how error handling works in this case, see
		 * comments in dsm_detach.  Note that if we reach this point, the
		 * current process certainly does not have the segment mapped, because
		 * if it did, the reference count would have still been greater than 1
		 * even after releasing the reference count held by the pin.  The fact
		 * that there can't be a dsm_segment for this handle makes it OK to
		 * pass the mapped size, mapped address, and private data as NULL
		 * here.
		 */
		if (is_main_region_dsm_handle(handle) ||
			dsm_impl_op(DSM_OP_DESTROY, handle, 0, &junk_impl_private,
						&junk_mapped_address, &junk_mapped_size, WARNING))
		{
			LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
			if (is_main_region_dsm_handle(handle))
				FreePageManagerPut((FreePageManager *) dsm_main_space_begin,
								   dsm_control->item[control_slot].first_page,
								   dsm_control->item[control_slot].npages);
			Assert(dsm_control->item[control_slot].handle == handle);
			Assert(dsm_control->item[control_slot].refcnt == 1);
			dsm_control->item[control_slot].refcnt = 0;
			LWLockRelease(DynamicSharedMemoryControlLock);
		}
	}
}

/*
 * Find an existing mapping for a shared memory segment, if there is one.
 */
dsm_segment *
dsm_find_mapping(dsm_handle handle)
{
	dlist_iter	iter;
	dsm_segment *seg;

	dlist_foreach(iter, &dsm_segment_list)
	{
		seg = dlist_container(dsm_segment, node, iter.cur);
		if (seg->handle == handle)
			return seg;
	}

	return NULL;
}

/*
 * Get the address at which a dynamic shared memory segment is mapped.
 */
void *
dsm_segment_address(dsm_segment *seg)
{
	Assert(seg->mapped_address != NULL);
	return seg->mapped_address;
}

/*
 * Get the size of a mapping.
 */
Size
dsm_segment_map_length(dsm_segment *seg)
{
	Assert(seg->mapped_address != NULL);
	return seg->mapped_size;
}

/*
 * Get a handle for a mapping.
 *
 * To establish communication via dynamic shared memory between two backends,
 * one of them should first call dsm_create() to establish a new shared
 * memory mapping.  That process should then call dsm_segment_handle() to
 * obtain a handle for the mapping, and pass that handle to the
 * coordinating backend via some means (e.g. bgw_main_arg, or via the
 * main shared memory segment).  The recipient, once in possession of the
 * handle, should call dsm_attach().
 */
dsm_handle
dsm_segment_handle(dsm_segment *seg)
{
	return seg->handle;
}

/*
 * Register an on-detach callback for a dynamic shared memory segment.
 */
void
on_dsm_detach(dsm_segment *seg, on_dsm_detach_callback function, Datum arg)
{
	dsm_segment_detach_callback *cb;

	cb = MemoryContextAlloc(TopMemoryContext,
							sizeof(dsm_segment_detach_callback));
	cb->function = function;
	cb->arg = arg;
	slist_push_head(&seg->on_detach, &cb->node);
}

/*
 * Unregister an on-detach callback for a dynamic shared memory segment.
 */
void
cancel_on_dsm_detach(dsm_segment *seg, on_dsm_detach_callback function,
					 Datum arg)
{
	slist_mutable_iter iter;

	slist_foreach_modify(iter, &seg->on_detach)
	{
		dsm_segment_detach_callback *cb;

		cb = slist_container(dsm_segment_detach_callback, node, iter.cur);
		if (cb->function == function && cb->arg == arg)
		{
			slist_delete_current(&iter);
			pfree(cb);
			break;
		}
	}
}

/*
 * Discard all registered on-detach callbacks without executing them.
 */
void
reset_on_dsm_detach(void)
{
	dlist_iter	iter;

	dlist_foreach(iter, &dsm_segment_list)
	{
		dsm_segment *seg = dlist_container(dsm_segment, node, iter.cur);

		/* Throw away explicit on-detach actions one by one. */
		while (!slist_is_empty(&seg->on_detach))
		{
			slist_node *node;
			dsm_segment_detach_callback *cb;

			node = slist_pop_head_node(&seg->on_detach);
			cb = slist_container(dsm_segment_detach_callback, node, node);
			pfree(cb);
		}

		/*
		 * Decrementing the reference count is a sort of implicit on-detach
		 * action; make sure we don't do that, either.
		 */
		seg->control_slot = INVALID_CONTROL_SLOT;
	}
}

/*
 * Create a segment descriptor.
 */
static dsm_segment *
dsm_create_descriptor(void)
{
	dsm_segment *seg;

	if (CurrentResourceOwner)
		ResourceOwnerEnlarge(CurrentResourceOwner);

	seg = MemoryContextAlloc(TopMemoryContext, sizeof(dsm_segment));
	dlist_push_head(&dsm_segment_list, &seg->node);

	/* seg->handle must be initialized by the caller */
	seg->control_slot = INVALID_CONTROL_SLOT;
	seg->impl_private = NULL;
	seg->mapped_address = NULL;
	seg->mapped_size = 0;

	seg->resowner = CurrentResourceOwner;
	if (CurrentResourceOwner)
		ResourceOwnerRememberDSM(CurrentResourceOwner, seg);

	slist_init(&seg->on_detach);

	return seg;
}

/*
 * Sanity check a control segment.
 *
 * The goal here isn't to detect everything that could possibly be wrong with
 * the control segment; there's not enough information for that.  Rather, the
 * goal is to make sure that someone can iterate over the items in the segment
 * without overrunning the end of the mapping and crashing.  We also check
 * the magic number since, if that's messed up, this may not even be one of
 * our segments at all.
 */
static bool
dsm_control_segment_sane(dsm_control_header *control, Size mapped_size)
{
	if (mapped_size < offsetof(dsm_control_header, item))
		return false;			/* Mapped size too short to read header. */
	if (control->magic != PG_DYNSHMEM_CONTROL_MAGIC)
		return false;			/* Magic number doesn't match. */
	if (dsm_control_bytes_needed(control->maxitems) > mapped_size)
		return false;			/* Max item count won't fit in map. */
	if (control->nitems > control->maxitems)
		return false;			/* Overfull. */
	return true;
}

/*
 * Compute the number of control-segment bytes needed to store a given
 * number of items.
 */
static uint64
dsm_control_bytes_needed(uint32 nitems)
{
	return offsetof(dsm_control_header, item)
		+ sizeof(dsm_control_item) * (uint64) nitems;
}

static inline dsm_handle
make_main_region_dsm_handle(int slot)
{
	dsm_handle	handle;

	/*
	 * We need to create a handle that doesn't collide with any existing extra
	 * segment created by dsm_impl_op(), so we'll make it odd.  It also
	 * mustn't collide with any other main area pseudo-segment, so we'll
	 * include the slot number in some of the bits.  We also want to make an
	 * effort to avoid newly created and recently destroyed handles from being
	 * confused, so we'll make the rest of the bits random.
	 */
	handle = 1;
	handle |= slot << 1;
	handle |= pg_prng_uint32(&pg_global_prng_state) << (pg_leftmost_one_pos32(dsm_control->maxitems) + 1);
	return handle;
}

static inline bool
is_main_region_dsm_handle(dsm_handle handle)
{
	return handle & 1;
}

/* ResourceOwner callbacks */

static void
ResOwnerReleaseDSM(Datum res)
{
	dsm_segment *seg = (dsm_segment *) DatumGetPointer(res);

	seg->resowner = NULL;
	dsm_detach(seg);
}
static char *
ResOwnerPrintDSM(Datum res)
{
	dsm_segment *seg = (dsm_segment *) DatumGetPointer(res);

	return psprintf("dynamic shared memory segment %u",
					dsm_segment_handle(seg));
}
