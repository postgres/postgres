/*-------------------------------------------------------------------------
 *
 * arch-ia64.h
 *	  Atomic operations considerations specific to intel itanium
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES:
 *
 * src/include/port/atomics/arch-ia64.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * Itanium is weakly ordered, so read and write barriers require a full
 * fence.
 */
#if defined(__INTEL_COMPILER)
#	define pg_memory_barrier_impl()		__mf()
#elif defined(__GNUC__)
#	define pg_memory_barrier_impl()		__asm__ __volatile__ ("mf" : : : "memory")
#elif defined(__hpux)
#	define pg_memory_barrier_impl()		_Asm_mf()
#endif

/* per architecture manual doubleword accesses have single copy atomicity */
#define PG_HAVE_8BYTE_SINGLE_COPY_ATOMICITY
