/*-------------------------------------------------------------------------
 *
 * arch-ppc.h
 *	  Atomic operations considerations specific to PowerPC
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES:
 *
 * src/include/port/atomics/arch-ppc.h
 *
 *-------------------------------------------------------------------------
 */

#if defined(__GNUC__)

/*
 * lwsync orders loads with respect to each other, and similarly with stores.
 * But a load can be performed before a subsequent store, so sync must be used
 * for a full memory barrier.
 */
#define pg_memory_barrier_impl()	__asm__ __volatile__ ("sync" : : : "memory")
#define pg_read_barrier_impl()		__asm__ __volatile__ ("lwsync" : : : "memory")
#define pg_write_barrier_impl()		__asm__ __volatile__ ("lwsync" : : : "memory")
#endif

/* per architecture manual doubleword accesses have single copy atomicity */
#define PG_HAVE_8BYTE_SINGLE_COPY_ATOMICITY
