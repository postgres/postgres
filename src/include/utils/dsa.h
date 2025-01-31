/*-------------------------------------------------------------------------
 *
 * dsa.h
 *	  Dynamic shared memory areas.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/utils/dsa.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DSA_H
#define DSA_H

#include "port/atomics.h"
#include "storage/dsm.h"

/* The opaque type used for an area. */
struct dsa_area;
typedef struct dsa_area dsa_area;

/*
 * If this system only uses a 32-bit value for size_t, then use the 32-bit
 * implementation of DSA.  This limits the amount of DSA that can be created
 * to something significantly less than the entire 4GB address space because
 * the DSA pointer must encode both a segment identifier and an offset, but
 * that shouldn't be a significant limitation in practice.
 *
 * If this system doesn't support atomic operations on 64-bit values, then
 * we fall back to 32-bit dsa_pointer for lack of other options.
 *
 * For testing purposes, USE_SMALL_DSA_POINTER can be defined to force the use
 * of 32-bit dsa_pointer even on systems capable of supporting a 64-bit
 * dsa_pointer.
 */
#if SIZEOF_SIZE_T == 4 || !defined(PG_HAVE_ATOMIC_U64_SUPPORT) || \
	defined(USE_SMALL_DSA_POINTER)
#define SIZEOF_DSA_POINTER 4
#else
#define SIZEOF_DSA_POINTER 8
#endif

/*
 * The type of 'relative pointers' to memory allocated by a dynamic shared
 * area.  dsa_pointer values can be shared with other processes, but must be
 * converted to backend-local pointers before they can be dereferenced.  See
 * dsa_get_address.  Also, an atomic version and appropriately sized atomic
 * operations.
 */
#if SIZEOF_DSA_POINTER == 4
typedef uint32 dsa_pointer;
typedef pg_atomic_uint32 dsa_pointer_atomic;
#define dsa_pointer_atomic_init pg_atomic_init_u32
#define dsa_pointer_atomic_read pg_atomic_read_u32
#define dsa_pointer_atomic_write pg_atomic_write_u32
#define dsa_pointer_atomic_fetch_add pg_atomic_fetch_add_u32
#define dsa_pointer_atomic_compare_exchange pg_atomic_compare_exchange_u32
#define DSA_POINTER_FORMAT "%08x"
#else
typedef uint64 dsa_pointer;
typedef pg_atomic_uint64 dsa_pointer_atomic;
#define dsa_pointer_atomic_init pg_atomic_init_u64
#define dsa_pointer_atomic_read pg_atomic_read_u64
#define dsa_pointer_atomic_write pg_atomic_write_u64
#define dsa_pointer_atomic_fetch_add pg_atomic_fetch_add_u64
#define dsa_pointer_atomic_compare_exchange pg_atomic_compare_exchange_u64
#define DSA_POINTER_FORMAT "%016" PRIx64
#endif

/* Flags for dsa_allocate_extended. */
#define DSA_ALLOC_HUGE		0x01	/* allow huge allocation (> 1 GB) */
#define DSA_ALLOC_NO_OOM	0x02	/* no failure if out-of-memory */
#define DSA_ALLOC_ZERO		0x04	/* zero allocated memory */

/* A sentinel value for dsa_pointer used to indicate failure to allocate. */
#define InvalidDsaPointer ((dsa_pointer) 0)

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
 * The default size of the initial DSM segment that backs a dsa_area created
 * by dsa_create.  After creating some number of segments of the initial size
 * we'll double this size, and so on.  Larger segments may be created if
 * necessary to satisfy large requests.
 */
#define DSA_DEFAULT_INIT_SEGMENT_SIZE ((size_t) (1 * 1024 * 1024))

/* The minimum size of a DSM segment. */
#define DSA_MIN_SEGMENT_SIZE	((size_t) (256 * 1024))

/* The maximum size of a DSM segment. */
#define DSA_MAX_SEGMENT_SIZE ((size_t) 1 << DSA_OFFSET_WIDTH)

/* Check if a dsa_pointer value is valid. */
#define DsaPointerIsValid(x) ((x) != InvalidDsaPointer)

/* Allocate uninitialized memory with error on out-of-memory. */
#define dsa_allocate(area, size) \
	dsa_allocate_extended(area, size, 0)

/* Allocate zero-initialized memory with error on out-of-memory. */
#define dsa_allocate0(area, size) \
	dsa_allocate_extended(area, size, DSA_ALLOC_ZERO)

/* Create dsa_area with default segment sizes */
#define dsa_create(tranch_id) \
	dsa_create_ext(tranch_id, DSA_DEFAULT_INIT_SEGMENT_SIZE, \
				   DSA_MAX_SEGMENT_SIZE)

/* Create dsa_area with default segment sizes in an existing share memory space */
#define dsa_create_in_place(place, size, tranch_id, segment) \
	dsa_create_in_place_ext(place, size, tranch_id, segment, \
							DSA_DEFAULT_INIT_SEGMENT_SIZE, \
							DSA_MAX_SEGMENT_SIZE)

/*
 * The type used for dsa_area handles.  dsa_handle values can be shared with
 * other processes, so that they can attach to them.  This provides a way to
 * share allocated storage with other processes.
 *
 * The handle for a dsa_area is currently implemented as the dsm_handle
 * for the first DSM segment backing this dynamic storage area, but client
 * code shouldn't assume that is true.
 */
typedef dsm_handle dsa_handle;

/* Sentinel value to use for invalid dsa_handles. */
#define DSA_HANDLE_INVALID ((dsa_handle) DSM_HANDLE_INVALID)

extern dsa_area *dsa_create_ext(int tranche_id, size_t init_segment_size,
								size_t max_segment_size);
extern dsa_area *dsa_create_in_place_ext(void *place, size_t size,
										 int tranche_id, dsm_segment *segment,
										 size_t init_segment_size,
										 size_t max_segment_size);
extern dsa_area *dsa_attach(dsa_handle handle);
extern dsa_area *dsa_attach_in_place(void *place, dsm_segment *segment);
extern void dsa_release_in_place(void *place);
extern void dsa_on_dsm_detach_release_in_place(dsm_segment *, Datum);
extern void dsa_on_shmem_exit_release_in_place(int, Datum);
extern void dsa_pin_mapping(dsa_area *area);
extern void dsa_detach(dsa_area *area);
extern void dsa_pin(dsa_area *area);
extern void dsa_unpin(dsa_area *area);
extern void dsa_set_size_limit(dsa_area *area, size_t limit);
extern size_t dsa_minimum_size(void);
extern dsa_handle dsa_get_handle(dsa_area *area);
extern dsa_pointer dsa_allocate_extended(dsa_area *area, size_t size, int flags);
extern void dsa_free(dsa_area *area, dsa_pointer dp);
extern void *dsa_get_address(dsa_area *area, dsa_pointer dp);
extern size_t dsa_get_total_size(dsa_area *area);
extern void dsa_trim(dsa_area *area);
extern void dsa_dump(dsa_area *area);

#endif							/* DSA_H */
