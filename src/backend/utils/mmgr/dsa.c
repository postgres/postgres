/*-------------------------------------------------------------------------
 *
 * dsa.c
 *	  Dynamic shared memory areas.
 *
 * This module provides dynamic shared memory areas which are built on top of
 * DSM segments.  While dsm.c allows segments of memory of shared memory to be
 * created and shared between backends, it isn't designed to deal with small
 * objects.  A DSA area is a shared memory heap usually backed by one or more
 * DSM segments which can allocate memory using dsa_allocate() and dsa_free().
 * Alternatively, it can be created in pre-existing shared memory, including a
 * DSM segment, and then create extra DSM segments as required.  Unlike the
 * regular system heap, it deals in pseudo-pointers which must be converted to
 * backend-local pointers before they are dereferenced.  These pseudo-pointers
 * can however be shared with other backends, and can be used to construct
 * shared data structures.
 *
 * Each DSA area manages a set of DSM segments, adding new segments as
 * required and detaching them when they are no longer needed.  Each segment
 * contains a number of 4KB pages, a free page manager for tracking
 * consecutive runs of free pages, and a page map for tracking the source of
 * objects allocated on each page.  Allocation requests above 8KB are handled
 * by choosing a segment and finding consecutive free pages in its free page
 * manager.  Allocation requests for smaller sizes are handled using pools of
 * objects of a selection of sizes.  Each pool consists of a number of 16 page
 * (64KB) superblocks allocated in the same way as large objects.  Allocation
 * of large objects and new superblocks is serialized by a single LWLock, but
 * allocation of small objects from pre-existing superblocks uses one LWLock
 * per pool.  Currently there is one pool, and therefore one lock, per size
 * class.  Per-core pools to increase concurrency and strategies for reducing
 * the resulting fragmentation are areas for future research.  Each superblock
 * is managed with a 'span', which tracks the superblock's freelist.  Free
 * requests are handled by looking in the page map to find which span an
 * address was allocated from, so that small objects can be returned to the
 * appropriate free list, and large object pages can be returned directly to
 * the free page map.  When allocating, simple heuristics for selecting
 * segments and superblocks try to encourage occupied memory to be
 * concentrated, increasing the likelihood that whole superblocks can become
 * empty and be returned to the free page manager, and whole segments can
 * become empty and be returned to the operating system.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/dsa.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "port/atomics.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/dsa.h"
#include "utils/freepage.h"
#include "utils/memutils.h"

/*
 * The size of the initial DSM segment that backs a dsa_area created by
 * dsa_create.  After creating some number of segments of this size we'll
 * double this size, and so on.  Larger segments may be created if necessary
 * to satisfy large requests.
 */
#define DSA_INITIAL_SEGMENT_SIZE ((size_t) (1 * 1024 * 1024))

/*
 * How many segments to create before we double the segment size.  If this is
 * low, then there is likely to be a lot of wasted space in the largest
 * segment.  If it is high, then we risk running out of segment slots (see
 * dsm.c's limits on total number of segments), or limiting the total size
 * an area can manage when using small pointers.
 */
#define DSA_NUM_SEGMENTS_AT_EACH_SIZE 2

/*
 * The number of bits used to represent the offset part of a dsa_pointer.
 * This controls the maximum size of a segment, the maximum possible
 * allocation size and also the maximum number of segments per area.
 */
#if SIZEOF_DSA_POINTER == 4
#define DSA_OFFSET_WIDTH 27		/* 32 segments of size up to 128MB */
#else
#define DSA_OFFSET_WIDTH 40		/* 1024 segments of size up to 1TB */
#endif

/*
 * The maximum number of DSM segments that an area can own, determined by
 * the number of bits remaining (but capped at 1024).
 */
#define DSA_MAX_SEGMENTS \
	Min(1024, (1 << ((SIZEOF_DSA_POINTER * 8) - DSA_OFFSET_WIDTH)))

/* The bitmask for extracting the offset from a dsa_pointer. */
#define DSA_OFFSET_BITMASK (((dsa_pointer) 1 << DSA_OFFSET_WIDTH) - 1)

/* The maximum size of a DSM segment. */
#define DSA_MAX_SEGMENT_SIZE ((size_t) 1 << DSA_OFFSET_WIDTH)

/* Number of pages (see FPM_PAGE_SIZE) per regular superblock. */
#define DSA_PAGES_PER_SUPERBLOCK		16

/*
 * A magic number used as a sanity check for following DSM segments belonging
 * to a DSA area (this number will be XORed with the area handle and
 * the segment index).
 */
#define DSA_SEGMENT_HEADER_MAGIC 0x0ce26608

/* Build a dsa_pointer given a segment number and offset. */
#define DSA_MAKE_POINTER(segment_number, offset) \
	(((dsa_pointer) (segment_number) << DSA_OFFSET_WIDTH) | (offset))

/* Extract the segment number from a dsa_pointer. */
#define DSA_EXTRACT_SEGMENT_NUMBER(dp) ((dp) >> DSA_OFFSET_WIDTH)

/* Extract the offset from a dsa_pointer. */
#define DSA_EXTRACT_OFFSET(dp) ((dp) & DSA_OFFSET_BITMASK)

/* The type used for index segment indexes (zero based). */
typedef size_t dsa_segment_index;

/* Sentinel value for dsa_segment_index indicating 'none' or 'end'. */
#define DSA_SEGMENT_INDEX_NONE (~(dsa_segment_index)0)

/*
 * How many bins of segments do we have?  The bins are used to categorize
 * segments by their largest contiguous run of free pages.
 */
#define DSA_NUM_SEGMENT_BINS 16

/*
 * What is the lowest bin that holds segments that *might* have n contiguous
 * free pages?	There is no point in looking in segments in lower bins; they
 * definitely can't service a request for n free pages.
 */
#define contiguous_pages_to_segment_bin(n) Min(fls(n), DSA_NUM_SEGMENT_BINS - 1)

/* Macros for access to locks. */
#define DSA_AREA_LOCK(area) (&area->control->lock)
#define DSA_SCLASS_LOCK(area, sclass) (&area->control->pools[sclass].lock)

/*
 * The header for an individual segment.  This lives at the start of each DSM
 * segment owned by a DSA area including the first segment (where it appears
 * as part of the dsa_area_control struct).
 */
typedef struct
{
	/* Sanity check magic value. */
	uint32		magic;
	/* Total number of pages in this segment (excluding metadata area). */
	size_t		usable_pages;
	/* Total size of this segment in bytes. */
	size_t		size;

	/*
	 * Index of the segment that precedes this one in the same segment bin, or
	 * DSA_SEGMENT_INDEX_NONE if this is the first one.
	 */
	dsa_segment_index prev;

	/*
	 * Index of the segment that follows this one in the same segment bin, or
	 * DSA_SEGMENT_INDEX_NONE if this is the last one.
	 */
	dsa_segment_index next;
	/* The index of the bin that contains this segment. */
	size_t		bin;

	/*
	 * A flag raised to indicate that this segment is being returned to the
	 * operating system and has been unpinned.
	 */
	bool		freed;
} dsa_segment_header;

/*
 * Metadata for one superblock.
 *
 * For most blocks, span objects are stored out-of-line; that is, the span
 * object is not stored within the block itself.  But, as an exception, for a
 * "span of spans", the span object is stored "inline".  The allocation is
 * always exactly one page, and the dsa_area_span object is located at
 * the beginning of that page.  The size class is DSA_SCLASS_BLOCK_OF_SPANS,
 * and the remaining fields are used just as they would be in an ordinary
 * block.  We can't allocate spans out of ordinary superblocks because
 * creating an ordinary superblock requires us to be able to allocate a span
 * *first*.  Doing it this way avoids that circularity.
 */
typedef struct
{
	dsa_pointer pool;			/* Containing pool. */
	dsa_pointer prevspan;		/* Previous span. */
	dsa_pointer nextspan;		/* Next span. */
	dsa_pointer start;			/* Starting address. */
	size_t		npages;			/* Length of span in pages. */
	uint16		size_class;		/* Size class. */
	uint16		ninitialized;	/* Maximum number of objects ever allocated. */
	uint16		nallocatable;	/* Number of objects currently allocatable. */
	uint16		firstfree;		/* First object on free list. */
	uint16		nmax;			/* Maximum number of objects ever possible. */
	uint16		fclass;			/* Current fullness class. */
} dsa_area_span;

/*
 * Given a pointer to an object in a span, access the index of the next free
 * object in the same span (ie in the span's freelist) as an L-value.
 */
#define NextFreeObjectIndex(object) (* (uint16 *) (object))

/*
 * Small allocations are handled by dividing a single block of memory into
 * many small objects of equal size.  The possible allocation sizes are
 * defined by the following array.  Larger size classes are spaced more widely
 * than smaller size classes.  We fudge the spacing for size classes >1kB to
 * avoid space wastage: based on the knowledge that we plan to allocate 64kB
 * blocks, we bump the maximum object size up to the largest multiple of
 * 8 bytes that still lets us fit the same number of objects into one block.
 *
 * NB: Because of this fudging, if we were ever to use differently-sized blocks
 * for small allocations, these size classes would need to be reworked to be
 * optimal for the new size.
 *
 * NB: The optimal spacing for size classes, as well as the size of the blocks
 * out of which small objects are allocated, is not a question that has one
 * right answer.  Some allocators (such as tcmalloc) use more closely-spaced
 * size classes than we do here, while others (like aset.c) use more
 * widely-spaced classes.  Spacing the classes more closely avoids wasting
 * memory within individual chunks, but also means a larger number of
 * potentially-unfilled blocks.
 */
static const uint16 dsa_size_classes[] = {
	sizeof(dsa_area_span), 0,	/* special size classes */
	8, 16, 24, 32, 40, 48, 56, 64,	/* 8 classes separated by 8 bytes */
	80, 96, 112, 128,			/* 4 classes separated by 16 bytes */
	160, 192, 224, 256,			/* 4 classes separated by 32 bytes */
	320, 384, 448, 512,			/* 4 classes separated by 64 bytes */
	640, 768, 896, 1024,		/* 4 classes separated by 128 bytes */
	1280, 1560, 1816, 2048,		/* 4 classes separated by ~256 bytes */
	2616, 3120, 3640, 4096,		/* 4 classes separated by ~512 bytes */
	5456, 6552, 7280, 8192		/* 4 classes separated by ~1024 bytes */
};
#define DSA_NUM_SIZE_CLASSES				lengthof(dsa_size_classes)

/* Special size classes. */
#define DSA_SCLASS_BLOCK_OF_SPANS		0
#define DSA_SCLASS_SPAN_LARGE			1

/*
 * The following lookup table is used to map the size of small objects
 * (less than 1kB) onto the corresponding size class.  To use this table,
 * round the size of the object up to the next multiple of 8 bytes, and then
 * index into this array.
 */
static const uint8 dsa_size_class_map[] = {
	2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 11, 11, 12, 12, 13, 13,
	14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 17,
	18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19,
	20, 20, 20, 20, 20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21, 21,
	22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
	23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
	24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
	25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25
};
#define DSA_SIZE_CLASS_MAP_QUANTUM	8

/*
 * Superblocks are binned by how full they are.  Generally, each fullness
 * class corresponds to one quartile, but the block being used for
 * allocations is always at the head of the list for fullness class 1,
 * regardless of how full it really is.
 */
#define DSA_FULLNESS_CLASSES		4

/*
 * A dsa_area_pool represents a set of objects of a given size class.
 *
 * Perhaps there should be multiple pools for the same size class for
 * contention avoidance, but for now there is just one!
 */
typedef struct
{
	/* A lock protecting access to this pool. */
	LWLock		lock;
	/* A set of linked lists of spans, arranged by fullness. */
	dsa_pointer spans[DSA_FULLNESS_CLASSES];
	/* Should we pad this out to a cacheline boundary? */
} dsa_area_pool;

/*
 * The control block for an area.  This lives in shared memory, at the start of
 * the first DSM segment controlled by this area.
 */
typedef struct
{
	/* The segment header for the first segment. */
	dsa_segment_header segment_header;
	/* The handle for this area. */
	dsa_handle	handle;
	/* The handles of the segments owned by this area. */
	dsm_handle	segment_handles[DSA_MAX_SEGMENTS];
	/* Lists of segments, binned by maximum contiguous run of free pages. */
	dsa_segment_index segment_bins[DSA_NUM_SEGMENT_BINS];
	/* The object pools for each size class. */
	dsa_area_pool pools[DSA_NUM_SIZE_CLASSES];
	/* The total size of all active segments. */
	size_t		total_segment_size;
	/* The maximum total size of backing storage we are allowed. */
	size_t		max_total_segment_size;
	/* Highest used segment index in the history of this area. */
	dsa_segment_index high_segment_index;
	/* The reference count for this area. */
	int			refcnt;
	/* A flag indicating that this area has been pinned. */
	bool		pinned;
	/* The number of times that segments have been freed. */
	size_t		freed_segment_counter;
	/* The LWLock tranche ID. */
	int			lwlock_tranche_id;
	/* The general lock (protects everything except object pools). */
	LWLock		lock;
} dsa_area_control;

/* Given a pointer to a pool, find a dsa_pointer. */
#define DsaAreaPoolToDsaPointer(area, p)	\
	DSA_MAKE_POINTER(0, (char *) p - (char *) area->control)

/*
 * A dsa_segment_map is stored within the backend-private memory of each
 * individual backend.  It holds the base address of the segment within that
 * backend, plus the addresses of key objects within the segment.  Those
 * could instead be derived from the base address but it's handy to have them
 * around.
 */
typedef struct
{
	dsm_segment *segment;		/* DSM segment */
	char	   *mapped_address; /* Address at which segment is mapped */
	dsa_segment_header *header; /* Header (same as mapped_address) */
	FreePageManager *fpm;		/* Free page manager within segment. */
	dsa_pointer *pagemap;		/* Page map within segment. */
} dsa_segment_map;

/*
 * Per-backend state for a storage area.  Backends obtain one of these by
 * creating an area or attaching to an existing one using a handle.  Each
 * process that needs to use an area uses its own object to track where the
 * segments are mapped.
 */
struct dsa_area
{
	/* Pointer to the control object in shared memory. */
	dsa_area_control *control;

	/* Has the mapping been pinned? */
	bool		mapping_pinned;

	/*
	 * This backend's array of segment maps, ordered by segment index
	 * corresponding to control->segment_handles.  Some of the area's segments
	 * may not be mapped in this backend yet, and some slots may have been
	 * freed and need to be detached; these operations happen on demand.
	 */
	dsa_segment_map segment_maps[DSA_MAX_SEGMENTS];

	/* The highest segment index this backend has ever mapped. */
	dsa_segment_index high_segment_index;

	/* The last observed freed_segment_counter. */
	size_t		freed_segment_counter;
};

#define DSA_SPAN_NOTHING_FREE	((uint16) -1)
#define DSA_SUPERBLOCK_SIZE (DSA_PAGES_PER_SUPERBLOCK * FPM_PAGE_SIZE)

/* Given a pointer to a segment_map, obtain a segment index number. */
#define get_segment_index(area, segment_map_ptr) \
	(segment_map_ptr - &area->segment_maps[0])

static void init_span(dsa_area *area, dsa_pointer span_pointer,
					  dsa_area_pool *pool, dsa_pointer start, size_t npages,
					  uint16 size_class);
static bool transfer_first_span(dsa_area *area, dsa_area_pool *pool,
								int fromclass, int toclass);
static inline dsa_pointer alloc_object(dsa_area *area, int size_class);
static bool ensure_active_superblock(dsa_area *area, dsa_area_pool *pool,
									 int size_class);
static dsa_segment_map *get_segment_by_index(dsa_area *area,
											 dsa_segment_index index);
static void destroy_superblock(dsa_area *area, dsa_pointer span_pointer);
static void unlink_span(dsa_area *area, dsa_area_span *span);
static void add_span_to_fullness_class(dsa_area *area, dsa_area_span *span,
									   dsa_pointer span_pointer, int fclass);
static void unlink_segment(dsa_area *area, dsa_segment_map *segment_map);
static dsa_segment_map *get_best_segment(dsa_area *area, size_t npages);
static dsa_segment_map *make_new_segment(dsa_area *area, size_t requested_pages);
static dsa_area *create_internal(void *place, size_t size,
								 int tranche_id,
								 dsm_handle control_handle,
								 dsm_segment *control_segment);
static dsa_area *attach_internal(void *place, dsm_segment *segment,
								 dsa_handle handle);
static void check_for_freed_segments(dsa_area *area);
static void check_for_freed_segments_locked(dsa_area *area);
static void rebin_segment(dsa_area *area, dsa_segment_map *segment_map);

/*
 * Create a new shared area in a new DSM segment.  Further DSM segments will
 * be allocated as required to extend the available space.
 *
 * We can't allocate a LWLock tranche_id within this function, because tranche
 * IDs are a scarce resource; there are only 64k available, using low numbers
 * when possible matters, and we have no provision for recycling them.  So,
 * we require the caller to provide one.
 */
dsa_area *
dsa_create(int tranche_id)
{
	dsm_segment *segment;
	dsa_area   *area;

	/*
	 * Create the DSM segment that will hold the shared control object and the
	 * first segment of usable space.
	 */
	segment = dsm_create(DSA_INITIAL_SEGMENT_SIZE, 0);

	/*
	 * All segments backing this area are pinned, so that DSA can explicitly
	 * control their lifetime (otherwise a newly created segment belonging to
	 * this area might be freed when the only backend that happens to have it
	 * mapped in ends, corrupting the area).
	 */
	dsm_pin_segment(segment);

	/* Create a new DSA area with the control object in this segment. */
	area = create_internal(dsm_segment_address(segment),
						   DSA_INITIAL_SEGMENT_SIZE,
						   tranche_id,
						   dsm_segment_handle(segment), segment);

	/* Clean up when the control segment detaches. */
	on_dsm_detach(segment, &dsa_on_dsm_detach_release_in_place,
				  PointerGetDatum(dsm_segment_address(segment)));

	return area;
}

/*
 * Create a new shared area in an existing shared memory space, which may be
 * either DSM or Postmaster-initialized memory.  DSM segments will be
 * allocated as required to extend the available space, though that can be
 * prevented with dsa_set_size_limit(area, size) using the same size provided
 * to dsa_create_in_place.
 *
 * Areas created in-place must eventually be released by the backend that
 * created them and all backends that attach to them.  This can be done
 * explicitly with dsa_release_in_place, or, in the special case that 'place'
 * happens to be in a pre-existing DSM segment, by passing in a pointer to the
 * segment so that a detach hook can be registered with the containing DSM
 * segment.
 *
 * See dsa_create() for a note about the tranche arguments.
 */
dsa_area *
dsa_create_in_place(void *place, size_t size,
					int tranche_id, dsm_segment *segment)
{
	dsa_area   *area;

	area = create_internal(place, size, tranche_id,
						   DSM_HANDLE_INVALID, NULL);

	/*
	 * Clean up when the control segment detaches, if a containing DSM segment
	 * was provided.
	 */
	if (segment != NULL)
		on_dsm_detach(segment, &dsa_on_dsm_detach_release_in_place,
					  PointerGetDatum(place));

	return area;
}

/*
 * Obtain a handle that can be passed to other processes so that they can
 * attach to the given area.  Cannot be called for areas created with
 * dsa_create_in_place.
 */
dsa_handle
dsa_get_handle(dsa_area *area)
{
	Assert(area->control->handle != DSM_HANDLE_INVALID);
	return area->control->handle;
}

/*
 * Attach to an area given a handle generated (possibly in another process) by
 * dsa_get_handle.  The area must have been created with dsa_create (not
 * dsa_create_in_place).
 */
dsa_area *
dsa_attach(dsa_handle handle)
{
	dsm_segment *segment;
	dsa_area   *area;

	/*
	 * An area handle is really a DSM segment handle for the first segment, so
	 * we go ahead and attach to that.
	 */
	segment = dsm_attach(handle);
	if (segment == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not attach to dynamic shared area")));

	area = attach_internal(dsm_segment_address(segment), segment, handle);

	/* Clean up when the control segment detaches. */
	on_dsm_detach(segment, &dsa_on_dsm_detach_release_in_place,
				  PointerGetDatum(dsm_segment_address(segment)));

	return area;
}

/*
 * Attach to an area that was created with dsa_create_in_place.  The caller
 * must somehow know the location in memory that was used when the area was
 * created, though it may be mapped at a different virtual address in this
 * process.
 *
 * See dsa_create_in_place for note about releasing in-place areas, and the
 * optional 'segment' argument which can be provided to allow automatic
 * release if the containing memory happens to be a DSM segment.
 */
dsa_area *
dsa_attach_in_place(void *place, dsm_segment *segment)
{
	dsa_area   *area;

	area = attach_internal(place, NULL, DSM_HANDLE_INVALID);

	/*
	 * Clean up when the control segment detaches, if a containing DSM segment
	 * was provided.
	 */
	if (segment != NULL)
		on_dsm_detach(segment, &dsa_on_dsm_detach_release_in_place,
					  PointerGetDatum(place));

	return area;
}

/*
 * Release a DSA area that was produced by dsa_create_in_place or
 * dsa_attach_in_place.  The 'segment' argument is ignored but provides an
 * interface suitable for on_dsm_detach, for the convenience of users who want
 * to create a DSA segment inside an existing DSM segment and have it
 * automatically released when the containing DSM segment is detached.
 * 'place' should be the address of the place where the area was created.
 *
 * This callback is automatically registered for the DSM segment containing
 * the control object of in-place areas when a segment is provided to
 * dsa_create_in_place or dsa_attach_in_place, and also for all areas created
 * with dsa_create.
 */
void
dsa_on_dsm_detach_release_in_place(dsm_segment *segment, Datum place)
{
	dsa_release_in_place(DatumGetPointer(place));
}

/*
 * Release a DSA area that was produced by dsa_create_in_place or
 * dsa_attach_in_place.  The 'code' argument is ignored but provides an
 * interface suitable for on_shmem_exit or before_shmem_exit, for the
 * convenience of users who want to create a DSA segment inside shared memory
 * other than a DSM segment and have it automatically release at backend exit.
 * 'place' should be the address of the place where the area was created.
 */
void
dsa_on_shmem_exit_release_in_place(int code, Datum place)
{
	dsa_release_in_place(DatumGetPointer(place));
}

/*
 * Release a DSA area that was produced by dsa_create_in_place or
 * dsa_attach_in_place.  It is preferable to use one of the 'dsa_on_XXX'
 * callbacks so that this is managed automatically, because failure to release
 * an area created in-place leaks its segments permanently.
 *
 * This is also called automatically for areas produced by dsa_create or
 * dsa_attach as an implementation detail.
 */
void
dsa_release_in_place(void *place)
{
	dsa_area_control *control = (dsa_area_control *) place;
	int			i;

	LWLockAcquire(&control->lock, LW_EXCLUSIVE);
	Assert(control->segment_header.magic ==
		   (DSA_SEGMENT_HEADER_MAGIC ^ control->handle ^ 0));
	Assert(control->refcnt > 0);
	if (--control->refcnt == 0)
	{
		for (i = 0; i <= control->high_segment_index; ++i)
		{
			dsm_handle	handle;

			handle = control->segment_handles[i];
			if (handle != DSM_HANDLE_INVALID)
				dsm_unpin_segment(handle);
		}
	}
	LWLockRelease(&control->lock);
}

/*
 * Keep a DSA area attached until end of session or explicit detach.
 *
 * By default, areas are owned by the current resource owner, which means they
 * are detached automatically when that scope ends.
 */
void
dsa_pin_mapping(dsa_area *area)
{
	int			i;

	Assert(!area->mapping_pinned);
	area->mapping_pinned = true;

	for (i = 0; i <= area->high_segment_index; ++i)
		if (area->segment_maps[i].segment != NULL)
			dsm_pin_mapping(area->segment_maps[i].segment);
}

/*
 * Allocate memory in this storage area.  The return value is a dsa_pointer
 * that can be passed to other processes, and converted to a local pointer
 * with dsa_get_address.  'flags' is a bitmap which should be constructed
 * from the following values:
 *
 * DSA_ALLOC_HUGE allows allocations >= 1GB.  Otherwise, such allocations
 * will result in an ERROR.
 *
 * DSA_ALLOC_NO_OOM causes this function to return InvalidDsaPointer when
 * no memory is available or a size limit established by dsa_set_size_limit
 * would be exceeded.  Otherwise, such allocations will result in an ERROR.
 *
 * DSA_ALLOC_ZERO causes the allocated memory to be zeroed.  Otherwise, the
 * contents of newly-allocated memory are indeterminate.
 *
 * These flags correspond to similarly named flags used by
 * MemoryContextAllocExtended().  See also the macros dsa_allocate and
 * dsa_allocate0 which expand to a call to this function with commonly used
 * flags.
 */
dsa_pointer
dsa_allocate_extended(dsa_area *area, size_t size, int flags)
{
	uint16		size_class;
	dsa_pointer start_pointer;
	dsa_segment_map *segment_map;
	dsa_pointer result;

	Assert(size > 0);

	/* Sanity check on huge individual allocation size. */
	if (((flags & DSA_ALLOC_HUGE) != 0 && !AllocHugeSizeIsValid(size)) ||
		((flags & DSA_ALLOC_HUGE) == 0 && !AllocSizeIsValid(size)))
		elog(ERROR, "invalid DSA memory alloc request size %zu", size);

	/*
	 * If bigger than the largest size class, just grab a run of pages from
	 * the free page manager, instead of allocating an object from a pool.
	 * There will still be a span, but it's a special class of span that
	 * manages this whole allocation and simply gives all pages back to the
	 * free page manager when dsa_free is called.
	 */
	if (size > dsa_size_classes[lengthof(dsa_size_classes) - 1])
	{
		size_t		npages = fpm_size_to_pages(size);
		size_t		first_page;
		dsa_pointer span_pointer;
		dsa_area_pool *pool = &area->control->pools[DSA_SCLASS_SPAN_LARGE];

		/* Obtain a span object. */
		span_pointer = alloc_object(area, DSA_SCLASS_BLOCK_OF_SPANS);
		if (!DsaPointerIsValid(span_pointer))
		{
			/* Raise error unless asked not to. */
			if ((flags & DSA_ALLOC_NO_OOM) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory"),
						 errdetail("Failed on DSA request of size %zu.",
								   size)));
			return InvalidDsaPointer;
		}

		LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);

		/* Find a segment from which to allocate. */
		segment_map = get_best_segment(area, npages);
		if (segment_map == NULL)
			segment_map = make_new_segment(area, npages);
		if (segment_map == NULL)
		{
			/* Can't make any more segments: game over. */
			LWLockRelease(DSA_AREA_LOCK(area));
			dsa_free(area, span_pointer);

			/* Raise error unless asked not to. */
			if ((flags & DSA_ALLOC_NO_OOM) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory"),
						 errdetail("Failed on DSA request of size %zu.",
								   size)));
			return InvalidDsaPointer;
		}

		/*
		 * Ask the free page manager for a run of pages.  This should always
		 * succeed, since both get_best_segment and make_new_segment should
		 * only return a non-NULL pointer if it actually contains enough
		 * contiguous freespace.  If it does fail, something in our backend
		 * private state is out of whack, so use FATAL to kill the process.
		 */
		if (!FreePageManagerGet(segment_map->fpm, npages, &first_page))
			elog(FATAL,
				 "dsa_allocate could not find %zu free pages", npages);
		LWLockRelease(DSA_AREA_LOCK(area));

		start_pointer = DSA_MAKE_POINTER(get_segment_index(area, segment_map),
										 first_page * FPM_PAGE_SIZE);

		/* Initialize span and pagemap. */
		LWLockAcquire(DSA_SCLASS_LOCK(area, DSA_SCLASS_SPAN_LARGE),
					  LW_EXCLUSIVE);
		init_span(area, span_pointer, pool, start_pointer, npages,
				  DSA_SCLASS_SPAN_LARGE);
		segment_map->pagemap[first_page] = span_pointer;
		LWLockRelease(DSA_SCLASS_LOCK(area, DSA_SCLASS_SPAN_LARGE));

		/* Zero-initialize the memory if requested. */
		if ((flags & DSA_ALLOC_ZERO) != 0)
			memset(dsa_get_address(area, start_pointer), 0, size);

		return start_pointer;
	}

	/* Map allocation to a size class. */
	if (size < lengthof(dsa_size_class_map) * DSA_SIZE_CLASS_MAP_QUANTUM)
	{
		int			mapidx;

		/* For smaller sizes we have a lookup table... */
		mapidx = ((size + DSA_SIZE_CLASS_MAP_QUANTUM - 1) /
				  DSA_SIZE_CLASS_MAP_QUANTUM) - 1;
		size_class = dsa_size_class_map[mapidx];
	}
	else
	{
		uint16		min;
		uint16		max;

		/* ... and for the rest we search by binary chop. */
		min = dsa_size_class_map[lengthof(dsa_size_class_map) - 1];
		max = lengthof(dsa_size_classes) - 1;

		while (min < max)
		{
			uint16		mid = (min + max) / 2;
			uint16		class_size = dsa_size_classes[mid];

			if (class_size < size)
				min = mid + 1;
			else
				max = mid;
		}

		size_class = min;
	}
	Assert(size <= dsa_size_classes[size_class]);
	Assert(size_class == 0 || size > dsa_size_classes[size_class - 1]);

	/* Attempt to allocate an object from the appropriate pool. */
	result = alloc_object(area, size_class);

	/* Check for failure to allocate. */
	if (!DsaPointerIsValid(result))
	{
		/* Raise error unless asked not to. */
		if ((flags & DSA_ALLOC_NO_OOM) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed on DSA request of size %zu.", size)));
		return InvalidDsaPointer;
	}

	/* Zero-initialize the memory if requested. */
	if ((flags & DSA_ALLOC_ZERO) != 0)
		memset(dsa_get_address(area, result), 0, size);

	return result;
}

/*
 * Free memory obtained with dsa_allocate.
 */
void
dsa_free(dsa_area *area, dsa_pointer dp)
{
	dsa_segment_map *segment_map;
	int			pageno;
	dsa_pointer span_pointer;
	dsa_area_span *span;
	char	   *superblock;
	char	   *object;
	size_t		size;
	int			size_class;

	/* Make sure we don't have a stale segment in the slot 'dp' refers to. */
	check_for_freed_segments(area);

	/* Locate the object, span and pool. */
	segment_map = get_segment_by_index(area, DSA_EXTRACT_SEGMENT_NUMBER(dp));
	pageno = DSA_EXTRACT_OFFSET(dp) / FPM_PAGE_SIZE;
	span_pointer = segment_map->pagemap[pageno];
	span = dsa_get_address(area, span_pointer);
	superblock = dsa_get_address(area, span->start);
	object = dsa_get_address(area, dp);
	size_class = span->size_class;
	size = dsa_size_classes[size_class];

	/*
	 * Special case for large objects that live in a special span: we return
	 * those pages directly to the free page manager and free the span.
	 */
	if (span->size_class == DSA_SCLASS_SPAN_LARGE)
	{

#ifdef CLOBBER_FREED_MEMORY
		memset(object, 0x7f, span->npages * FPM_PAGE_SIZE);
#endif

		/* Give pages back to free page manager. */
		LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
		FreePageManagerPut(segment_map->fpm,
						   DSA_EXTRACT_OFFSET(span->start) / FPM_PAGE_SIZE,
						   span->npages);

		/* Move segment to appropriate bin if necessary. */
		rebin_segment(area, segment_map);
		LWLockRelease(DSA_AREA_LOCK(area));

		/* Unlink span. */
		LWLockAcquire(DSA_SCLASS_LOCK(area, DSA_SCLASS_SPAN_LARGE),
					  LW_EXCLUSIVE);
		unlink_span(area, span);
		LWLockRelease(DSA_SCLASS_LOCK(area, DSA_SCLASS_SPAN_LARGE));
		/* Free the span object so it can be reused. */
		dsa_free(area, span_pointer);
		return;
	}

#ifdef CLOBBER_FREED_MEMORY
	memset(object, 0x7f, size);
#endif

	LWLockAcquire(DSA_SCLASS_LOCK(area, size_class), LW_EXCLUSIVE);

	/* Put the object on the span's freelist. */
	Assert(object >= superblock);
	Assert(object < superblock + DSA_SUPERBLOCK_SIZE);
	Assert((object - superblock) % size == 0);
	NextFreeObjectIndex(object) = span->firstfree;
	span->firstfree = (object - superblock) / size;
	++span->nallocatable;

	/*
	 * See if the span needs to moved to a different fullness class, or be
	 * freed so its pages can be given back to the segment.
	 */
	if (span->nallocatable == 1 && span->fclass == DSA_FULLNESS_CLASSES - 1)
	{
		/*
		 * The block was completely full and is located in the
		 * highest-numbered fullness class, which is never scanned for free
		 * chunks.  We must move it to the next-lower fullness class.
		 */
		unlink_span(area, span);
		add_span_to_fullness_class(area, span, span_pointer,
								   DSA_FULLNESS_CLASSES - 2);

		/*
		 * If this is the only span, and there is no active span, then we
		 * should probably move this span to fullness class 1.  (Otherwise if
		 * you allocate exactly all the objects in the only span, it moves to
		 * class 3, then you free them all, it moves to 2, and then is given
		 * back, leaving no active span).
		 */
	}
	else if (span->nallocatable == span->nmax &&
			 (span->fclass != 1 || span->prevspan != InvalidDsaPointer))
	{
		/*
		 * This entire block is free, and it's not the active block for this
		 * size class.  Return the memory to the free page manager. We don't
		 * do this for the active block to prevent hysteresis: if we
		 * repeatedly allocate and free the only chunk in the active block, it
		 * will be very inefficient if we deallocate and reallocate the block
		 * every time.
		 */
		destroy_superblock(area, span_pointer);
	}

	LWLockRelease(DSA_SCLASS_LOCK(area, size_class));
}

/*
 * Obtain a backend-local address for a dsa_pointer.  'dp' must point to
 * memory allocated by the given area (possibly in another process) that
 * hasn't yet been freed.  This may cause a segment to be mapped into the
 * current process if required, and may cause freed segments to be unmapped.
 */
void *
dsa_get_address(dsa_area *area, dsa_pointer dp)
{
	dsa_segment_index index;
	size_t		offset;

	/* Convert InvalidDsaPointer to NULL. */
	if (!DsaPointerIsValid(dp))
		return NULL;

	/* Process any requests to detach from freed segments. */
	check_for_freed_segments(area);

	/* Break the dsa_pointer into its components. */
	index = DSA_EXTRACT_SEGMENT_NUMBER(dp);
	offset = DSA_EXTRACT_OFFSET(dp);
	Assert(index < DSA_MAX_SEGMENTS);

	/* Check if we need to cause this segment to be mapped in. */
	if (unlikely(area->segment_maps[index].mapped_address == NULL))
	{
		/* Call for effect (we don't need the result). */
		get_segment_by_index(area, index);
	}

	return area->segment_maps[index].mapped_address + offset;
}

/*
 * Pin this area, so that it will continue to exist even if all backends
 * detach from it.  In that case, the area can still be reattached to if a
 * handle has been recorded somewhere.
 */
void
dsa_pin(dsa_area *area)
{
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	if (area->control->pinned)
	{
		LWLockRelease(DSA_AREA_LOCK(area));
		elog(ERROR, "dsa_area already pinned");
	}
	area->control->pinned = true;
	++area->control->refcnt;
	LWLockRelease(DSA_AREA_LOCK(area));
}

/*
 * Undo the effects of dsa_pin, so that the given area can be freed when no
 * backends are attached to it.  May be called only if dsa_pin has been
 * called.
 */
void
dsa_unpin(dsa_area *area)
{
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	Assert(area->control->refcnt > 1);
	if (!area->control->pinned)
	{
		LWLockRelease(DSA_AREA_LOCK(area));
		elog(ERROR, "dsa_area not pinned");
	}
	area->control->pinned = false;
	--area->control->refcnt;
	LWLockRelease(DSA_AREA_LOCK(area));
}

/*
 * Set the total size limit for this area.  This limit is checked whenever new
 * segments need to be allocated from the operating system.  If the new size
 * limit is already exceeded, this has no immediate effect.
 *
 * Note that the total virtual memory usage may be temporarily larger than
 * this limit when segments have been freed, but not yet detached by all
 * backends that have attached to them.
 */
void
dsa_set_size_limit(dsa_area *area, size_t limit)
{
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	area->control->max_total_segment_size = limit;
	LWLockRelease(DSA_AREA_LOCK(area));
}

/*
 * Aggressively free all spare memory in the hope of returning DSM segments to
 * the operating system.
 */
void
dsa_trim(dsa_area *area)
{
	int			size_class;

	/*
	 * Trim in reverse pool order so we get to the spans-of-spans last, just
	 * in case any become entirely free while processing all the other pools.
	 */
	for (size_class = DSA_NUM_SIZE_CLASSES - 1; size_class >= 0; --size_class)
	{
		dsa_area_pool *pool = &area->control->pools[size_class];
		dsa_pointer span_pointer;

		if (size_class == DSA_SCLASS_SPAN_LARGE)
		{
			/* Large object frees give back segments aggressively already. */
			continue;
		}

		/*
		 * Search fullness class 1 only.  That is where we expect to find an
		 * entirely empty superblock (entirely empty superblocks in other
		 * fullness classes are returned to the free page map by dsa_free).
		 */
		LWLockAcquire(DSA_SCLASS_LOCK(area, size_class), LW_EXCLUSIVE);
		span_pointer = pool->spans[1];
		while (DsaPointerIsValid(span_pointer))
		{
			dsa_area_span *span = dsa_get_address(area, span_pointer);
			dsa_pointer next = span->nextspan;

			if (span->nallocatable == span->nmax)
				destroy_superblock(area, span_pointer);

			span_pointer = next;
		}
		LWLockRelease(DSA_SCLASS_LOCK(area, size_class));
	}
}

/*
 * Print out debugging information about the internal state of the shared
 * memory area.
 */
void
dsa_dump(dsa_area *area)
{
	size_t		i,
				j;

	/*
	 * Note: This gives an inconsistent snapshot as it acquires and releases
	 * individual locks as it goes...
	 */

	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	check_for_freed_segments_locked(area);
	fprintf(stderr, "dsa_area handle %x:\n", area->control->handle);
	fprintf(stderr, "  max_total_segment_size: %zu\n",
			area->control->max_total_segment_size);
	fprintf(stderr, "  total_segment_size: %zu\n",
			area->control->total_segment_size);
	fprintf(stderr, "  refcnt: %d\n", area->control->refcnt);
	fprintf(stderr, "  pinned: %c\n", area->control->pinned ? 't' : 'f');
	fprintf(stderr, "  segment bins:\n");
	for (i = 0; i < DSA_NUM_SEGMENT_BINS; ++i)
	{
		if (area->control->segment_bins[i] != DSA_SEGMENT_INDEX_NONE)
		{
			dsa_segment_index segment_index;

			if (i == 0)
				fprintf(stderr,
						"    segment bin %zu (no contiguous free pages):\n", i);
			else
				fprintf(stderr,
						"    segment bin %zu (at least %d contiguous pages free):\n",
						i, 1 << (i - 1));
			segment_index = area->control->segment_bins[i];
			while (segment_index != DSA_SEGMENT_INDEX_NONE)
			{
				dsa_segment_map *segment_map;

				segment_map =
					get_segment_by_index(area, segment_index);

				fprintf(stderr,
						"      segment index %zu, usable_pages = %zu, "
						"contiguous_pages = %zu, mapped at %p\n",
						segment_index,
						segment_map->header->usable_pages,
						fpm_largest(segment_map->fpm),
						segment_map->mapped_address);
				segment_index = segment_map->header->next;
			}
		}
	}
	LWLockRelease(DSA_AREA_LOCK(area));

	fprintf(stderr, "  pools:\n");
	for (i = 0; i < DSA_NUM_SIZE_CLASSES; ++i)
	{
		bool		found = false;

		LWLockAcquire(DSA_SCLASS_LOCK(area, i), LW_EXCLUSIVE);
		for (j = 0; j < DSA_FULLNESS_CLASSES; ++j)
			if (DsaPointerIsValid(area->control->pools[i].spans[j]))
				found = true;
		if (found)
		{
			if (i == DSA_SCLASS_BLOCK_OF_SPANS)
				fprintf(stderr, "    pool for blocks of span objects:\n");
			else if (i == DSA_SCLASS_SPAN_LARGE)
				fprintf(stderr, "    pool for large object spans:\n");
			else
				fprintf(stderr,
						"    pool for size class %zu (object size %hu bytes):\n",
						i, dsa_size_classes[i]);
			for (j = 0; j < DSA_FULLNESS_CLASSES; ++j)
			{
				if (!DsaPointerIsValid(area->control->pools[i].spans[j]))
					fprintf(stderr, "      fullness class %zu is empty\n", j);
				else
				{
					dsa_pointer span_pointer = area->control->pools[i].spans[j];

					fprintf(stderr, "      fullness class %zu:\n", j);
					while (DsaPointerIsValid(span_pointer))
					{
						dsa_area_span *span;

						span = dsa_get_address(area, span_pointer);
						fprintf(stderr,
								"        span descriptor at "
								DSA_POINTER_FORMAT ", superblock at "
								DSA_POINTER_FORMAT
								", pages = %zu, objects free = %hu/%hu\n",
								span_pointer, span->start, span->npages,
								span->nallocatable, span->nmax);
						span_pointer = span->nextspan;
					}
				}
			}
		}
		LWLockRelease(DSA_SCLASS_LOCK(area, i));
	}
}

/*
 * Return the smallest size that you can successfully provide to
 * dsa_create_in_place.
 */
size_t
dsa_minimum_size(void)
{
	size_t		size;
	int			pages = 0;

	size = MAXALIGN(sizeof(dsa_area_control)) +
		MAXALIGN(sizeof(FreePageManager));

	/* Figure out how many pages we need, including the page map... */
	while (((size + FPM_PAGE_SIZE - 1) / FPM_PAGE_SIZE) > pages)
	{
		++pages;
		size += sizeof(dsa_pointer);
	}

	return pages * FPM_PAGE_SIZE;
}

/*
 * Workhorse function for dsa_create and dsa_create_in_place.
 */
static dsa_area *
create_internal(void *place, size_t size,
				int tranche_id,
				dsm_handle control_handle,
				dsm_segment *control_segment)
{
	dsa_area_control *control;
	dsa_area   *area;
	dsa_segment_map *segment_map;
	size_t		usable_pages;
	size_t		total_pages;
	size_t		metadata_bytes;
	int			i;

	/* Sanity check on the space we have to work in. */
	if (size < dsa_minimum_size())
		elog(ERROR, "dsa_area space must be at least %zu, but %zu provided",
			 dsa_minimum_size(), size);

	/* Now figure out how much space is usable */
	total_pages = size / FPM_PAGE_SIZE;
	metadata_bytes =
		MAXALIGN(sizeof(dsa_area_control)) +
		MAXALIGN(sizeof(FreePageManager)) +
		total_pages * sizeof(dsa_pointer);
	/* Add padding up to next page boundary. */
	if (metadata_bytes % FPM_PAGE_SIZE != 0)
		metadata_bytes += FPM_PAGE_SIZE - (metadata_bytes % FPM_PAGE_SIZE);
	Assert(metadata_bytes <= size);
	usable_pages = (size - metadata_bytes) / FPM_PAGE_SIZE;

	/*
	 * Initialize the dsa_area_control object located at the start of the
	 * space.
	 */
	control = (dsa_area_control *) place;
	control->segment_header.magic =
		DSA_SEGMENT_HEADER_MAGIC ^ control_handle ^ 0;
	control->segment_header.next = DSA_SEGMENT_INDEX_NONE;
	control->segment_header.prev = DSA_SEGMENT_INDEX_NONE;
	control->segment_header.usable_pages = usable_pages;
	control->segment_header.freed = false;
	control->segment_header.size = DSA_INITIAL_SEGMENT_SIZE;
	control->handle = control_handle;
	control->max_total_segment_size = (size_t) -1;
	control->total_segment_size = size;
	memset(&control->segment_handles[0], 0,
		   sizeof(dsm_handle) * DSA_MAX_SEGMENTS);
	control->segment_handles[0] = control_handle;
	for (i = 0; i < DSA_NUM_SEGMENT_BINS; ++i)
		control->segment_bins[i] = DSA_SEGMENT_INDEX_NONE;
	control->high_segment_index = 0;
	control->refcnt = 1;
	control->freed_segment_counter = 0;
	control->lwlock_tranche_id = tranche_id;

	/*
	 * Create the dsa_area object that this backend will use to access the
	 * area.  Other backends will need to obtain their own dsa_area object by
	 * attaching.
	 */
	area = palloc(sizeof(dsa_area));
	area->control = control;
	area->mapping_pinned = false;
	memset(area->segment_maps, 0, sizeof(dsa_segment_map) * DSA_MAX_SEGMENTS);
	area->high_segment_index = 0;
	area->freed_segment_counter = 0;
	LWLockInitialize(&control->lock, control->lwlock_tranche_id);
	for (i = 0; i < DSA_NUM_SIZE_CLASSES; ++i)
		LWLockInitialize(DSA_SCLASS_LOCK(area, i),
						 control->lwlock_tranche_id);

	/* Set up the segment map for this process's mapping. */
	segment_map = &area->segment_maps[0];
	segment_map->segment = control_segment;
	segment_map->mapped_address = place;
	segment_map->header = (dsa_segment_header *) place;
	segment_map->fpm = (FreePageManager *)
		(segment_map->mapped_address +
		 MAXALIGN(sizeof(dsa_area_control)));
	segment_map->pagemap = (dsa_pointer *)
		(segment_map->mapped_address +
		 MAXALIGN(sizeof(dsa_area_control)) +
		 MAXALIGN(sizeof(FreePageManager)));

	/* Set up the free page map. */
	FreePageManagerInitialize(segment_map->fpm, segment_map->mapped_address);
	/* There can be 0 usable pages if size is dsa_minimum_size(). */

	if (usable_pages > 0)
		FreePageManagerPut(segment_map->fpm, metadata_bytes / FPM_PAGE_SIZE,
						   usable_pages);

	/* Put this segment into the appropriate bin. */
	control->segment_bins[contiguous_pages_to_segment_bin(usable_pages)] = 0;
	segment_map->header->bin = contiguous_pages_to_segment_bin(usable_pages);

	return area;
}

/*
 * Workhorse function for dsa_attach and dsa_attach_in_place.
 */
static dsa_area *
attach_internal(void *place, dsm_segment *segment, dsa_handle handle)
{
	dsa_area_control *control;
	dsa_area   *area;
	dsa_segment_map *segment_map;

	control = (dsa_area_control *) place;
	Assert(control->handle == handle);
	Assert(control->segment_handles[0] == handle);
	Assert(control->segment_header.magic ==
		   (DSA_SEGMENT_HEADER_MAGIC ^ handle ^ 0));

	/* Build the backend-local area object. */
	area = palloc(sizeof(dsa_area));
	area->control = control;
	area->mapping_pinned = false;
	memset(&area->segment_maps[0], 0,
		   sizeof(dsa_segment_map) * DSA_MAX_SEGMENTS);
	area->high_segment_index = 0;

	/* Set up the segment map for this process's mapping. */
	segment_map = &area->segment_maps[0];
	segment_map->segment = segment; /* NULL for in-place */
	segment_map->mapped_address = place;
	segment_map->header = (dsa_segment_header *) segment_map->mapped_address;
	segment_map->fpm = (FreePageManager *)
		(segment_map->mapped_address + MAXALIGN(sizeof(dsa_area_control)));
	segment_map->pagemap = (dsa_pointer *)
		(segment_map->mapped_address + MAXALIGN(sizeof(dsa_area_control)) +
		 MAXALIGN(sizeof(FreePageManager)));

	/* Bump the reference count. */
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	if (control->refcnt == 0)
	{
		/* We can't attach to a DSA area that has already been destroyed. */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not attach to dynamic shared area")));
	}
	++control->refcnt;
	area->freed_segment_counter = area->control->freed_segment_counter;
	LWLockRelease(DSA_AREA_LOCK(area));

	return area;
}

/*
 * Add a new span to fullness class 1 of the indicated pool.
 */
static void
init_span(dsa_area *area,
		  dsa_pointer span_pointer,
		  dsa_area_pool *pool, dsa_pointer start, size_t npages,
		  uint16 size_class)
{
	dsa_area_span *span = dsa_get_address(area, span_pointer);
	size_t		obsize = dsa_size_classes[size_class];

	/*
	 * The per-pool lock must be held because we manipulate the span list for
	 * this pool.
	 */
	Assert(LWLockHeldByMe(DSA_SCLASS_LOCK(area, size_class)));

	/* Push this span onto the front of the span list for fullness class 1. */
	if (DsaPointerIsValid(pool->spans[1]))
	{
		dsa_area_span *head = (dsa_area_span *)
		dsa_get_address(area, pool->spans[1]);

		head->prevspan = span_pointer;
	}
	span->pool = DsaAreaPoolToDsaPointer(area, pool);
	span->nextspan = pool->spans[1];
	span->prevspan = InvalidDsaPointer;
	pool->spans[1] = span_pointer;

	span->start = start;
	span->npages = npages;
	span->size_class = size_class;
	span->ninitialized = 0;
	if (size_class == DSA_SCLASS_BLOCK_OF_SPANS)
	{
		/*
		 * A block-of-spans contains its own descriptor, so mark one object as
		 * initialized and reduce the count of allocatable objects by one.
		 * Doing this here has the side effect of also reducing nmax by one,
		 * which is important to make sure we free this object at the correct
		 * time.
		 */
		span->ninitialized = 1;
		span->nallocatable = FPM_PAGE_SIZE / obsize - 1;
	}
	else if (size_class != DSA_SCLASS_SPAN_LARGE)
		span->nallocatable = DSA_SUPERBLOCK_SIZE / obsize;
	span->firstfree = DSA_SPAN_NOTHING_FREE;
	span->nmax = span->nallocatable;
	span->fclass = 1;
}

/*
 * Transfer the first span in one fullness class to the head of another
 * fullness class.
 */
static bool
transfer_first_span(dsa_area *area,
					dsa_area_pool *pool, int fromclass, int toclass)
{
	dsa_pointer span_pointer;
	dsa_area_span *span;
	dsa_area_span *nextspan;

	/* Can't do it if source list is empty. */
	span_pointer = pool->spans[fromclass];
	if (!DsaPointerIsValid(span_pointer))
		return false;

	/* Remove span from head of source list. */
	span = dsa_get_address(area, span_pointer);
	pool->spans[fromclass] = span->nextspan;
	if (DsaPointerIsValid(span->nextspan))
	{
		nextspan = (dsa_area_span *)
			dsa_get_address(area, span->nextspan);
		nextspan->prevspan = InvalidDsaPointer;
	}

	/* Add span to head of target list. */
	span->nextspan = pool->spans[toclass];
	pool->spans[toclass] = span_pointer;
	if (DsaPointerIsValid(span->nextspan))
	{
		nextspan = (dsa_area_span *)
			dsa_get_address(area, span->nextspan);
		nextspan->prevspan = span_pointer;
	}
	span->fclass = toclass;

	return true;
}

/*
 * Allocate one object of the requested size class from the given area.
 */
static inline dsa_pointer
alloc_object(dsa_area *area, int size_class)
{
	dsa_area_pool *pool = &area->control->pools[size_class];
	dsa_area_span *span;
	dsa_pointer block;
	dsa_pointer result;
	char	   *object;
	size_t		size;

	/*
	 * Even though ensure_active_superblock can in turn call alloc_object if
	 * it needs to allocate a new span, that's always from a different pool,
	 * and the order of lock acquisition is always the same, so it's OK that
	 * we hold this lock for the duration of this function.
	 */
	Assert(!LWLockHeldByMe(DSA_SCLASS_LOCK(area, size_class)));
	LWLockAcquire(DSA_SCLASS_LOCK(area, size_class), LW_EXCLUSIVE);

	/*
	 * If there's no active superblock, we must successfully obtain one or
	 * fail the request.
	 */
	if (!DsaPointerIsValid(pool->spans[1]) &&
		!ensure_active_superblock(area, pool, size_class))
	{
		result = InvalidDsaPointer;
	}
	else
	{
		/*
		 * There should be a block in fullness class 1 at this point, and it
		 * should never be completely full.  Thus we can either pop an object
		 * from the free list or, failing that, initialize a new object.
		 */
		Assert(DsaPointerIsValid(pool->spans[1]));
		span = (dsa_area_span *)
			dsa_get_address(area, pool->spans[1]);
		Assert(span->nallocatable > 0);
		block = span->start;
		Assert(size_class < DSA_NUM_SIZE_CLASSES);
		size = dsa_size_classes[size_class];
		if (span->firstfree != DSA_SPAN_NOTHING_FREE)
		{
			result = block + span->firstfree * size;
			object = dsa_get_address(area, result);
			span->firstfree = NextFreeObjectIndex(object);
		}
		else
		{
			result = block + span->ninitialized * size;
			++span->ninitialized;
		}
		--span->nallocatable;

		/* If it's now full, move it to the highest-numbered fullness class. */
		if (span->nallocatable == 0)
			transfer_first_span(area, pool, 1, DSA_FULLNESS_CLASSES - 1);
	}

	Assert(LWLockHeldByMe(DSA_SCLASS_LOCK(area, size_class)));
	LWLockRelease(DSA_SCLASS_LOCK(area, size_class));

	return result;
}

/*
 * Ensure an active (i.e. fullness class 1) superblock, unless all existing
 * superblocks are completely full and no more can be allocated.
 *
 * Fullness classes K of 0..N are loosely intended to represent blocks whose
 * utilization percentage is at least K/N, but we only enforce this rigorously
 * for the highest-numbered fullness class, which always contains exactly
 * those blocks that are completely full.  It's otherwise acceptable for a
 * block to be in a higher-numbered fullness class than the one to which it
 * logically belongs.  In addition, the active block, which is always the
 * first block in fullness class 1, is permitted to have a higher allocation
 * percentage than would normally be allowable for that fullness class; we
 * don't move it until it's completely full, and then it goes to the
 * highest-numbered fullness class.
 *
 * It might seem odd that the active block is the head of fullness class 1
 * rather than fullness class 0, but experience with other allocators has
 * shown that it's usually better to allocate from a block that's moderately
 * full rather than one that's nearly empty.  Insofar as is reasonably
 * possible, we want to avoid performing new allocations in a block that would
 * otherwise become empty soon.
 */
static bool
ensure_active_superblock(dsa_area *area, dsa_area_pool *pool,
						 int size_class)
{
	dsa_pointer span_pointer;
	dsa_pointer start_pointer;
	size_t		obsize = dsa_size_classes[size_class];
	size_t		nmax;
	int			fclass;
	size_t		npages = 1;
	size_t		first_page;
	size_t		i;
	dsa_segment_map *segment_map;

	Assert(LWLockHeldByMe(DSA_SCLASS_LOCK(area, size_class)));

	/*
	 * Compute the number of objects that will fit in a block of this size
	 * class.  Span-of-spans blocks are just a single page, and the first
	 * object isn't available for use because it describes the block-of-spans
	 * itself.
	 */
	if (size_class == DSA_SCLASS_BLOCK_OF_SPANS)
		nmax = FPM_PAGE_SIZE / obsize - 1;
	else
		nmax = DSA_SUPERBLOCK_SIZE / obsize;

	/*
	 * If fullness class 1 is empty, try to find a span to put in it by
	 * scanning higher-numbered fullness classes (excluding the last one,
	 * whose blocks are certain to all be completely full).
	 */
	for (fclass = 2; fclass < DSA_FULLNESS_CLASSES - 1; ++fclass)
	{
		span_pointer = pool->spans[fclass];

		while (DsaPointerIsValid(span_pointer))
		{
			int			tfclass;
			dsa_area_span *span;
			dsa_area_span *nextspan;
			dsa_area_span *prevspan;
			dsa_pointer next_span_pointer;

			span = (dsa_area_span *)
				dsa_get_address(area, span_pointer);
			next_span_pointer = span->nextspan;

			/* Figure out what fullness class should contain this span. */
			tfclass = (nmax - span->nallocatable)
				* (DSA_FULLNESS_CLASSES - 1) / nmax;

			/* Look up next span. */
			if (DsaPointerIsValid(span->nextspan))
				nextspan = (dsa_area_span *)
					dsa_get_address(area, span->nextspan);
			else
				nextspan = NULL;

			/*
			 * If utilization has dropped enough that this now belongs in some
			 * other fullness class, move it there.
			 */
			if (tfclass < fclass)
			{
				/* Remove from the current fullness class list. */
				if (pool->spans[fclass] == span_pointer)
				{
					/* It was the head; remove it. */
					Assert(!DsaPointerIsValid(span->prevspan));
					pool->spans[fclass] = span->nextspan;
					if (nextspan != NULL)
						nextspan->prevspan = InvalidDsaPointer;
				}
				else
				{
					/* It was not the head. */
					Assert(DsaPointerIsValid(span->prevspan));
					prevspan = (dsa_area_span *)
						dsa_get_address(area, span->prevspan);
					prevspan->nextspan = span->nextspan;
				}
				if (nextspan != NULL)
					nextspan->prevspan = span->prevspan;

				/* Push onto the head of the new fullness class list. */
				span->nextspan = pool->spans[tfclass];
				pool->spans[tfclass] = span_pointer;
				span->prevspan = InvalidDsaPointer;
				if (DsaPointerIsValid(span->nextspan))
				{
					nextspan = (dsa_area_span *)
						dsa_get_address(area, span->nextspan);
					nextspan->prevspan = span_pointer;
				}
				span->fclass = tfclass;
			}

			/* Advance to next span on list. */
			span_pointer = next_span_pointer;
		}

		/* Stop now if we found a suitable block. */
		if (DsaPointerIsValid(pool->spans[1]))
			return true;
	}

	/*
	 * If there are no blocks that properly belong in fullness class 1, pick
	 * one from some other fullness class and move it there anyway, so that we
	 * have an allocation target.  Our last choice is to transfer a block
	 * that's almost empty (and might become completely empty soon if left
	 * alone), but even that is better than failing, which is what we must do
	 * if there are no blocks at all with freespace.
	 */
	Assert(!DsaPointerIsValid(pool->spans[1]));
	for (fclass = 2; fclass < DSA_FULLNESS_CLASSES - 1; ++fclass)
		if (transfer_first_span(area, pool, fclass, 1))
			return true;
	if (!DsaPointerIsValid(pool->spans[1]) &&
		transfer_first_span(area, pool, 0, 1))
		return true;

	/*
	 * We failed to find an existing span with free objects, so we need to
	 * allocate a new superblock and construct a new span to manage it.
	 *
	 * First, get a dsa_area_span object to describe the new superblock block
	 * ... unless this allocation is for a dsa_area_span object, in which case
	 * that's surely not going to work.  We handle that case by storing the
	 * span describing a block-of-spans inline.
	 */
	if (size_class != DSA_SCLASS_BLOCK_OF_SPANS)
	{
		span_pointer = alloc_object(area, DSA_SCLASS_BLOCK_OF_SPANS);
		if (!DsaPointerIsValid(span_pointer))
			return false;
		npages = DSA_PAGES_PER_SUPERBLOCK;
	}

	/* Find or create a segment and allocate the superblock. */
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	segment_map = get_best_segment(area, npages);
	if (segment_map == NULL)
	{
		segment_map = make_new_segment(area, npages);
		if (segment_map == NULL)
		{
			LWLockRelease(DSA_AREA_LOCK(area));
			return false;
		}
	}

	/*
	 * This shouldn't happen: get_best_segment() or make_new_segment()
	 * promised that we can successfully allocate npages.
	 */
	if (!FreePageManagerGet(segment_map->fpm, npages, &first_page))
		elog(FATAL,
			 "dsa_allocate could not find %zu free pages for superblock",
			 npages);
	LWLockRelease(DSA_AREA_LOCK(area));

	/* Compute the start of the superblock. */
	start_pointer =
		DSA_MAKE_POINTER(get_segment_index(area, segment_map),
						 first_page * FPM_PAGE_SIZE);

	/*
	 * If this is a block-of-spans, carve the descriptor right out of the
	 * allocated space.
	 */
	if (size_class == DSA_SCLASS_BLOCK_OF_SPANS)
	{
		/*
		 * We have a pointer into the segment.  We need to build a dsa_pointer
		 * from the segment index and offset into the segment.
		 */
		span_pointer = start_pointer;
	}

	/* Initialize span and pagemap. */
	init_span(area, span_pointer, pool, start_pointer, npages, size_class);
	for (i = 0; i < npages; ++i)
		segment_map->pagemap[first_page + i] = span_pointer;

	return true;
}

/*
 * Return the segment map corresponding to a given segment index, mapping the
 * segment in if necessary.  For internal segment book-keeping, this is called
 * with the area lock held.  It is also called by dsa_free and dsa_get_address
 * without any locking, relying on the fact they have a known live segment
 * index and they always call check_for_freed_segments to ensures that any
 * freed segment occupying the same slot is detached first.
 */
static dsa_segment_map *
get_segment_by_index(dsa_area *area, dsa_segment_index index)
{
	if (unlikely(area->segment_maps[index].mapped_address == NULL))
	{
		dsm_handle	handle;
		dsm_segment *segment;
		dsa_segment_map *segment_map;

		/*
		 * If we are reached by dsa_free or dsa_get_address, there must be at
		 * least one object allocated in the referenced segment.  Otherwise,
		 * their caller has a double-free or access-after-free bug, which we
		 * have no hope of detecting.  So we know it's safe to access this
		 * array slot without holding a lock; it won't change underneath us.
		 * Furthermore, we know that we can see the latest contents of the
		 * slot, as explained in check_for_freed_segments, which those
		 * functions call before arriving here.
		 */
		handle = area->control->segment_handles[index];

		/* It's an error to try to access an unused slot. */
		if (handle == DSM_HANDLE_INVALID)
			elog(ERROR,
				 "dsa_area could not attach to a segment that has been freed");

		segment = dsm_attach(handle);
		if (segment == NULL)
			elog(ERROR, "dsa_area could not attach to segment");
		if (area->mapping_pinned)
			dsm_pin_mapping(segment);
		segment_map = &area->segment_maps[index];
		segment_map->segment = segment;
		segment_map->mapped_address = dsm_segment_address(segment);
		segment_map->header =
			(dsa_segment_header *) segment_map->mapped_address;
		segment_map->fpm = (FreePageManager *)
			(segment_map->mapped_address +
			 MAXALIGN(sizeof(dsa_segment_header)));
		segment_map->pagemap = (dsa_pointer *)
			(segment_map->mapped_address +
			 MAXALIGN(sizeof(dsa_segment_header)) +
			 MAXALIGN(sizeof(FreePageManager)));

		/* Remember the highest index this backend has ever mapped. */
		if (area->high_segment_index < index)
			area->high_segment_index = index;

		Assert(segment_map->header->magic ==
			   (DSA_SEGMENT_HEADER_MAGIC ^ area->control->handle ^ index));
	}

	/*
	 * Callers of dsa_get_address() and dsa_free() don't hold the area lock,
	 * but it's a bug in the calling code and undefined behavior if the
	 * address is not live (ie if the segment might possibly have been freed,
	 * they're trying to use a dangling pointer).
	 *
	 * For dsa.c code that holds the area lock to manipulate segment_bins
	 * lists, it would be a bug if we ever reach a freed segment here.  After
	 * it's marked as freed, the only thing any backend should do with it is
	 * unmap it, and it should always have done that in
	 * check_for_freed_segments_locked() before arriving here to resolve an
	 * index to a segment_map.
	 *
	 * Either way we can assert that we aren't returning a freed segment.
	 */
	Assert(!area->segment_maps[index].header->freed);

	return &area->segment_maps[index];
}

/*
 * Return a superblock to the free page manager.  If the underlying segment
 * has become entirely free, then return it to the operating system.
 *
 * The appropriate pool lock must be held.
 */
static void
destroy_superblock(dsa_area *area, dsa_pointer span_pointer)
{
	dsa_area_span *span = dsa_get_address(area, span_pointer);
	int			size_class = span->size_class;
	dsa_segment_map *segment_map;


	/* Remove it from its fullness class list. */
	unlink_span(area, span);

	/*
	 * Note: Here we acquire the area lock while we already hold a per-pool
	 * lock.  We never hold the area lock and then take a pool lock, or we
	 * could deadlock.
	 */
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	check_for_freed_segments_locked(area);
	segment_map =
		get_segment_by_index(area, DSA_EXTRACT_SEGMENT_NUMBER(span->start));
	FreePageManagerPut(segment_map->fpm,
					   DSA_EXTRACT_OFFSET(span->start) / FPM_PAGE_SIZE,
					   span->npages);
	/* Check if the segment is now entirely free. */
	if (fpm_largest(segment_map->fpm) == segment_map->header->usable_pages)
	{
		dsa_segment_index index = get_segment_index(area, segment_map);

		/* If it's not the segment with extra control data, free it. */
		if (index != 0)
		{
			/*
			 * Give it back to the OS, and allow other backends to detect that
			 * they need to detach.
			 */
			unlink_segment(area, segment_map);
			segment_map->header->freed = true;
			Assert(area->control->total_segment_size >=
				   segment_map->header->size);
			area->control->total_segment_size -=
				segment_map->header->size;
			dsm_unpin_segment(dsm_segment_handle(segment_map->segment));
			dsm_detach(segment_map->segment);
			area->control->segment_handles[index] = DSM_HANDLE_INVALID;
			++area->control->freed_segment_counter;
			segment_map->segment = NULL;
			segment_map->header = NULL;
			segment_map->mapped_address = NULL;
		}
	}

	/* Move segment to appropriate bin if necessary. */
	if (segment_map->header != NULL)
		rebin_segment(area, segment_map);

	LWLockRelease(DSA_AREA_LOCK(area));

	/*
	 * Span-of-spans blocks store the span which describes them within the
	 * block itself, so freeing the storage implicitly frees the descriptor
	 * also.  If this is a block of any other type, we need to separately free
	 * the span object also.  This recursive call to dsa_free will acquire the
	 * span pool's lock.  We can't deadlock because the acquisition order is
	 * always some other pool and then the span pool.
	 */
	if (size_class != DSA_SCLASS_BLOCK_OF_SPANS)
		dsa_free(area, span_pointer);
}

static void
unlink_span(dsa_area *area, dsa_area_span *span)
{
	if (DsaPointerIsValid(span->nextspan))
	{
		dsa_area_span *next = dsa_get_address(area, span->nextspan);

		next->prevspan = span->prevspan;
	}
	if (DsaPointerIsValid(span->prevspan))
	{
		dsa_area_span *prev = dsa_get_address(area, span->prevspan);

		prev->nextspan = span->nextspan;
	}
	else
	{
		dsa_area_pool *pool = dsa_get_address(area, span->pool);

		pool->spans[span->fclass] = span->nextspan;
	}
}

static void
add_span_to_fullness_class(dsa_area *area, dsa_area_span *span,
						   dsa_pointer span_pointer,
						   int fclass)
{
	dsa_area_pool *pool = dsa_get_address(area, span->pool);

	if (DsaPointerIsValid(pool->spans[fclass]))
	{
		dsa_area_span *head = dsa_get_address(area,
											  pool->spans[fclass]);

		head->prevspan = span_pointer;
	}
	span->prevspan = InvalidDsaPointer;
	span->nextspan = pool->spans[fclass];
	pool->spans[fclass] = span_pointer;
	span->fclass = fclass;
}

/*
 * Detach from an area that was either created or attached to by this process.
 */
void
dsa_detach(dsa_area *area)
{
	int			i;

	/* Detach from all segments. */
	for (i = 0; i <= area->high_segment_index; ++i)
		if (area->segment_maps[i].segment != NULL)
			dsm_detach(area->segment_maps[i].segment);

	/*
	 * Note that 'detaching' (= detaching from DSM segments) doesn't include
	 * 'releasing' (= adjusting the reference count).  It would be nice to
	 * combine these operations, but client code might never get around to
	 * calling dsa_detach because of an error path, and a detach hook on any
	 * particular segment is too late to detach other segments in the area
	 * without risking a 'leak' warning in the non-error path.
	 */

	/* Free the backend-local area object. */
	pfree(area);
}

/*
 * Unlink a segment from the bin that contains it.
 */
static void
unlink_segment(dsa_area *area, dsa_segment_map *segment_map)
{
	if (segment_map->header->prev != DSA_SEGMENT_INDEX_NONE)
	{
		dsa_segment_map *prev;

		prev = get_segment_by_index(area, segment_map->header->prev);
		prev->header->next = segment_map->header->next;
	}
	else
	{
		Assert(area->control->segment_bins[segment_map->header->bin] ==
			   get_segment_index(area, segment_map));
		area->control->segment_bins[segment_map->header->bin] =
			segment_map->header->next;
	}
	if (segment_map->header->next != DSA_SEGMENT_INDEX_NONE)
	{
		dsa_segment_map *next;

		next = get_segment_by_index(area, segment_map->header->next);
		next->header->prev = segment_map->header->prev;
	}
}

/*
 * Find a segment that could satisfy a request for 'npages' of contiguous
 * memory, or return NULL if none can be found.  This may involve attaching to
 * segments that weren't previously attached so that we can query their free
 * pages map.
 */
static dsa_segment_map *
get_best_segment(dsa_area *area, size_t npages)
{
	size_t		bin;

	Assert(LWLockHeldByMe(DSA_AREA_LOCK(area)));
	check_for_freed_segments_locked(area);

	/*
	 * Start searching from the first bin that *might* have enough contiguous
	 * pages.
	 */
	for (bin = contiguous_pages_to_segment_bin(npages);
		 bin < DSA_NUM_SEGMENT_BINS;
		 ++bin)
	{
		/*
		 * The minimum contiguous size that any segment in this bin should
		 * have.  We'll re-bin if we see segments with fewer.
		 */
		size_t		threshold = (size_t) 1 << (bin - 1);
		dsa_segment_index segment_index;

		/* Search this bin for a segment with enough contiguous space. */
		segment_index = area->control->segment_bins[bin];
		while (segment_index != DSA_SEGMENT_INDEX_NONE)
		{
			dsa_segment_map *segment_map;
			dsa_segment_index next_segment_index;
			size_t		contiguous_pages;

			segment_map = get_segment_by_index(area, segment_index);
			next_segment_index = segment_map->header->next;
			contiguous_pages = fpm_largest(segment_map->fpm);

			/* Not enough for the request, still enough for this bin. */
			if (contiguous_pages >= threshold && contiguous_pages < npages)
			{
				segment_index = next_segment_index;
				continue;
			}

			/* Re-bin it if it's no longer in the appropriate bin. */
			if (contiguous_pages < threshold)
			{
				rebin_segment(area, segment_map);

				/*
				 * But fall through to see if it's enough to satisfy this
				 * request anyway....
				 */
			}

			/* Check if we are done. */
			if (contiguous_pages >= npages)
				return segment_map;

			/* Continue searching the same bin. */
			segment_index = next_segment_index;
		}
	}

	/* Not found. */
	return NULL;
}

/*
 * Create a new segment that can handle at least requested_pages.  Returns
 * NULL if the requested total size limit or maximum allowed number of
 * segments would be exceeded.
 */
static dsa_segment_map *
make_new_segment(dsa_area *area, size_t requested_pages)
{
	dsa_segment_index new_index;
	size_t		metadata_bytes;
	size_t		total_size;
	size_t		total_pages;
	size_t		usable_pages;
	dsa_segment_map *segment_map;
	dsm_segment *segment;

	Assert(LWLockHeldByMe(DSA_AREA_LOCK(area)));

	/* Find a segment slot that is not in use (linearly for now). */
	for (new_index = 1; new_index < DSA_MAX_SEGMENTS; ++new_index)
	{
		if (area->control->segment_handles[new_index] == DSM_HANDLE_INVALID)
			break;
	}
	if (new_index == DSA_MAX_SEGMENTS)
		return NULL;

	/*
	 * If the total size limit is already exceeded, then we exit early and
	 * avoid arithmetic wraparound in the unsigned expressions below.
	 */
	if (area->control->total_segment_size >=
		area->control->max_total_segment_size)
		return NULL;

	/*
	 * The size should be at least as big as requested, and at least big
	 * enough to follow a geometric series that approximately doubles the
	 * total storage each time we create a new segment.  We use geometric
	 * growth because the underlying DSM system isn't designed for large
	 * numbers of segments (otherwise we might even consider just using one
	 * DSM segment for each large allocation and for each superblock, and then
	 * we wouldn't need to use FreePageManager).
	 *
	 * We decide on a total segment size first, so that we produce tidy
	 * power-of-two sized segments.  This is a good property to have if we
	 * move to huge pages in the future.  Then we work back to the number of
	 * pages we can fit.
	 */
	total_size = DSA_INITIAL_SEGMENT_SIZE *
		((size_t) 1 << (new_index / DSA_NUM_SEGMENTS_AT_EACH_SIZE));
	total_size = Min(total_size, DSA_MAX_SEGMENT_SIZE);
	total_size = Min(total_size,
					 area->control->max_total_segment_size -
					 area->control->total_segment_size);

	total_pages = total_size / FPM_PAGE_SIZE;
	metadata_bytes =
		MAXALIGN(sizeof(dsa_segment_header)) +
		MAXALIGN(sizeof(FreePageManager)) +
		sizeof(dsa_pointer) * total_pages;

	/* Add padding up to next page boundary. */
	if (metadata_bytes % FPM_PAGE_SIZE != 0)
		metadata_bytes += FPM_PAGE_SIZE - (metadata_bytes % FPM_PAGE_SIZE);
	if (total_size <= metadata_bytes)
		return NULL;
	usable_pages = (total_size - metadata_bytes) / FPM_PAGE_SIZE;
	Assert(metadata_bytes + usable_pages * FPM_PAGE_SIZE <= total_size);

	/* See if that is enough... */
	if (requested_pages > usable_pages)
	{
		/*
		 * We'll make an odd-sized segment, working forward from the requested
		 * number of pages.
		 */
		usable_pages = requested_pages;
		metadata_bytes =
			MAXALIGN(sizeof(dsa_segment_header)) +
			MAXALIGN(sizeof(FreePageManager)) +
			usable_pages * sizeof(dsa_pointer);

		/* Add padding up to next page boundary. */
		if (metadata_bytes % FPM_PAGE_SIZE != 0)
			metadata_bytes += FPM_PAGE_SIZE - (metadata_bytes % FPM_PAGE_SIZE);
		total_size = metadata_bytes + usable_pages * FPM_PAGE_SIZE;

		/* Is that too large for dsa_pointer's addressing scheme? */
		if (total_size > DSA_MAX_SEGMENT_SIZE)
			return NULL;

		/* Would that exceed the limit? */
		if (total_size > area->control->max_total_segment_size -
			area->control->total_segment_size)
			return NULL;
	}

	/* Create the segment. */
	segment = dsm_create(total_size, 0);
	if (segment == NULL)
		return NULL;
	dsm_pin_segment(segment);
	if (area->mapping_pinned)
		dsm_pin_mapping(segment);

	/* Store the handle in shared memory to be found by index. */
	area->control->segment_handles[new_index] =
		dsm_segment_handle(segment);
	/* Track the highest segment index in the history of the area. */
	if (area->control->high_segment_index < new_index)
		area->control->high_segment_index = new_index;
	/* Track the highest segment index this backend has ever mapped. */
	if (area->high_segment_index < new_index)
		area->high_segment_index = new_index;
	/* Track total size of all segments. */
	area->control->total_segment_size += total_size;
	Assert(area->control->total_segment_size <=
		   area->control->max_total_segment_size);

	/* Build a segment map for this segment in this backend. */
	segment_map = &area->segment_maps[new_index];
	segment_map->segment = segment;
	segment_map->mapped_address = dsm_segment_address(segment);
	segment_map->header = (dsa_segment_header *) segment_map->mapped_address;
	segment_map->fpm = (FreePageManager *)
		(segment_map->mapped_address +
		 MAXALIGN(sizeof(dsa_segment_header)));
	segment_map->pagemap = (dsa_pointer *)
		(segment_map->mapped_address +
		 MAXALIGN(sizeof(dsa_segment_header)) +
		 MAXALIGN(sizeof(FreePageManager)));

	/* Set up the free page map. */
	FreePageManagerInitialize(segment_map->fpm, segment_map->mapped_address);
	FreePageManagerPut(segment_map->fpm, metadata_bytes / FPM_PAGE_SIZE,
					   usable_pages);

	/* Set up the segment header and put it in the appropriate bin. */
	segment_map->header->magic =
		DSA_SEGMENT_HEADER_MAGIC ^ area->control->handle ^ new_index;
	segment_map->header->usable_pages = usable_pages;
	segment_map->header->size = total_size;
	segment_map->header->bin = contiguous_pages_to_segment_bin(usable_pages);
	segment_map->header->prev = DSA_SEGMENT_INDEX_NONE;
	segment_map->header->next =
		area->control->segment_bins[segment_map->header->bin];
	segment_map->header->freed = false;
	area->control->segment_bins[segment_map->header->bin] = new_index;
	if (segment_map->header->next != DSA_SEGMENT_INDEX_NONE)
	{
		dsa_segment_map *next =
		get_segment_by_index(area, segment_map->header->next);

		Assert(next->header->bin == segment_map->header->bin);
		next->header->prev = new_index;
	}

	return segment_map;
}

/*
 * Check if any segments have been freed by destroy_superblock, so we can
 * detach from them in this backend.  This function is called by
 * dsa_get_address and dsa_free to make sure that a dsa_pointer they have
 * received can be resolved to the correct segment.
 *
 * The danger we want to defend against is that there could be an old segment
 * mapped into a given slot in this backend, and the dsa_pointer they have
 * might refer to some new segment in the same slot.  So those functions must
 * be sure to process all instructions to detach from a freed segment that had
 * been generated by the time this process received the dsa_pointer, before
 * they call get_segment_by_index.
 */
static void
check_for_freed_segments(dsa_area *area)
{
	size_t		freed_segment_counter;

	/*
	 * Any other process that has freed a segment has incremented
	 * free_segment_counter while holding an LWLock, and that must precede any
	 * backend creating a new segment in the same slot while holding an
	 * LWLock, and that must precede the creation of any dsa_pointer pointing
	 * into the new segment which might reach us here, and the caller must
	 * have sent the dsa_pointer to this process using appropriate memory
	 * synchronization (some kind of locking or atomic primitive or system
	 * call).  So all we need to do on the reading side is ask for the load of
	 * freed_segment_counter to follow the caller's load of the dsa_pointer it
	 * has, and we can be sure to detect any segments that had been freed as
	 * of the time that the dsa_pointer reached this process.
	 */
	pg_read_barrier();
	freed_segment_counter = area->control->freed_segment_counter;
	if (unlikely(area->freed_segment_counter != freed_segment_counter))
	{
		/* Check all currently mapped segments to find what's been freed. */
		LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
		check_for_freed_segments_locked(area);
		LWLockRelease(DSA_AREA_LOCK(area));
	}
}

/*
 * Workhorse for check_for_freed_segments(), and also used directly in path
 * where the area lock is already held.  This should be called after acquiring
 * the lock but before looking up any segment by index number, to make sure we
 * unmap any stale segments that might have previously had the same index as a
 * current segment.
 */
static void
check_for_freed_segments_locked(dsa_area *area)
{
	size_t		freed_segment_counter;
	int			i;

	Assert(LWLockHeldByMe(DSA_AREA_LOCK(area)));
	freed_segment_counter = area->control->freed_segment_counter;
	if (unlikely(area->freed_segment_counter != freed_segment_counter))
	{
		for (i = 0; i <= area->high_segment_index; ++i)
		{
			if (area->segment_maps[i].header != NULL &&
				area->segment_maps[i].header->freed)
			{
				dsm_detach(area->segment_maps[i].segment);
				area->segment_maps[i].segment = NULL;
				area->segment_maps[i].header = NULL;
				area->segment_maps[i].mapped_address = NULL;
			}
		}
		area->freed_segment_counter = freed_segment_counter;
	}
}

/*
 * Re-bin segment if it's no longer in the appropriate bin.
 */
static void
rebin_segment(dsa_area *area, dsa_segment_map *segment_map)
{
	size_t		new_bin;
	dsa_segment_index segment_index;

	new_bin = contiguous_pages_to_segment_bin(fpm_largest(segment_map->fpm));
	if (segment_map->header->bin == new_bin)
		return;

	/* Remove it from its current bin. */
	unlink_segment(area, segment_map);

	/* Push it onto the front of its new bin. */
	segment_index = get_segment_index(area, segment_map);
	segment_map->header->prev = DSA_SEGMENT_INDEX_NONE;
	segment_map->header->next = area->control->segment_bins[new_bin];
	segment_map->header->bin = new_bin;
	area->control->segment_bins[new_bin] = segment_index;
	if (segment_map->header->next != DSA_SEGMENT_INDEX_NONE)
	{
		dsa_segment_map *next;

		next = get_segment_by_index(area, segment_map->header->next);
		Assert(next->header->bin == new_bin);
		next->header->prev = segment_index;
	}
}
