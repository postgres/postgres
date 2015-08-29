/*-------------------------------------------------------------------------
 *
 * barrier.h
 *	  Memory barrier operations.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/barrier.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BARRIER_H
#define BARRIER_H

#include "storage/s_lock.h"

extern slock_t dummy_spinlock;

/*
 * A compiler barrier need not (and preferably should not) emit any actual
 * machine code, but must act as an optimization fence: the compiler must not
 * reorder loads or stores to main memory around the barrier.  However, the
 * CPU may still reorder loads or stores at runtime, if the architecture's
 * memory model permits this.
 *
 * A memory barrier must act as a compiler barrier, and in addition must
 * guarantee that all loads and stores issued prior to the barrier are
 * completed before any loads or stores issued after the barrier.  Unless
 * loads and stores are totally ordered (which is not the case on most
 * architectures) this requires issuing some sort of memory fencing
 * instruction.
 *
 * A read barrier must act as a compiler barrier, and in addition must
 * guarantee that any loads issued prior to the barrier are completed before
 * any loads issued after the barrier.  Similarly, a write barrier acts
 * as a compiler barrier, and also orders stores.  Read and write barriers
 * are thus weaker than a full memory barrier, but stronger than a compiler
 * barrier.  In practice, on machines with strong memory ordering, read and
 * write barriers may require nothing more than a compiler barrier.
 *
 * For an introduction to using memory barriers within the PostgreSQL backend,
 * see src/backend/storage/lmgr/README.barrier
 */

#if defined(DISABLE_BARRIERS)

/*
 * Fall through to the spinlock-based implementation.
 */
#elif defined(__INTEL_COMPILER)

/*
 * icc defines __GNUC__, but doesn't support gcc's inline asm syntax
 */
#if defined(__ia64__) || defined(__ia64)
#define pg_memory_barrier()		__mf()
#elif defined(__i386__) || defined(__x86_64__)
#define pg_memory_barrier()		_mm_mfence()
#endif

#define pg_compiler_barrier()	__memory_barrier()
#elif defined(__GNUC__)

/* This works on any architecture, since it's only talking to GCC itself. */
#define pg_compiler_barrier()	__asm__ __volatile__("" : : : "memory")

#if defined(__i386__)

/*
 * i386 does not allow loads to be reordered with other loads, or stores to be
 * reordered with other stores, but a load can be performed before a subsequent
 * store.
 *
 * "lock; addl" has worked for longer than "mfence".
 */
#define pg_memory_barrier()		\
	__asm__ __volatile__ ("lock; addl $0,0(%%esp)" : : : "memory", "cc")
#define pg_read_barrier()		pg_compiler_barrier()
#define pg_write_barrier()		pg_compiler_barrier()
#elif defined(__x86_64__)		/* 64 bit x86 */

/*
 * x86_64 has similar ordering characteristics to i386.
 *
 * Technically, some x86-ish chips support uncached memory access and/or
 * special instructions that are weakly ordered.  In those cases we'd need
 * the read and write barriers to be lfence and sfence.  But since we don't
 * do those things, a compiler barrier should be enough.
 */
#define pg_memory_barrier()		\
	__asm__ __volatile__ ("lock; addl $0,0(%%rsp)" : : : "memory", "cc")
#define pg_read_barrier()		pg_compiler_barrier()
#define pg_write_barrier()		pg_compiler_barrier()
#elif defined(__ia64__) || defined(__ia64)

/*
 * Itanium is weakly ordered, so read and write barriers require a full
 * fence.
 */
#define pg_memory_barrier()		__asm__ __volatile__ ("mf" : : : "memory")
#elif defined(__ppc__) || defined(__powerpc__) || defined(__ppc64__) || defined(__powerpc64__)

/*
 * lwsync orders loads with respect to each other, and similarly with stores.
 * But a load can be performed before a subsequent store, so sync must be used
 * for a full memory barrier.
 */
#define pg_memory_barrier()		__asm__ __volatile__ ("sync" : : : "memory")
#define pg_read_barrier()		__asm__ __volatile__ ("lwsync" : : : "memory")
#define pg_write_barrier()		__asm__ __volatile__ ("lwsync" : : : "memory")
#elif defined(__alpha) || defined(__alpha__)	/* Alpha */

/*
 * Unlike all other known architectures, Alpha allows dependent reads to be
 * reordered, but we don't currently find it necessary to provide a conditional
 * read barrier to cover that case.  We might need to add that later.
 */
#define pg_memory_barrier()		__asm__ __volatile__ ("mb" : : : "memory")
#define pg_read_barrier()		__asm__ __volatile__ ("mb" : : : "memory")
#define pg_write_barrier()		__asm__ __volatile__ ("wmb" : : : "memory")
#elif defined(__hppa) || defined(__hppa__)		/* HP PA-RISC */

/* HPPA doesn't do either read or write reordering */
#define pg_memory_barrier()		pg_compiler_barrier()
#elif __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)

/*
 * If we're on GCC 4.1.0 or higher, we should be able to get a memory
 * barrier out of this compiler built-in.  But we prefer to rely on our
 * own definitions where possible, and use this only as a fallback.
 */
#define pg_memory_barrier()		__sync_synchronize()
#endif
#elif defined(__ia64__) || defined(__ia64)

#define pg_compiler_barrier()	_Asm_sched_fence()
#define pg_memory_barrier()		_Asm_mf()
#elif defined(WIN32_ONLY_COMPILER)

/* Should work on both MSVC and Borland. */
#include <intrin.h>
#pragma intrinsic(_ReadWriteBarrier)
#define pg_compiler_barrier()	_ReadWriteBarrier()
#define pg_memory_barrier()		MemoryBarrier()
#endif

/*
 * If we have no memory barrier implementation for this architecture, we
 * fall back to acquiring and releasing a spinlock.  This might, in turn,
 * fall back to the semaphore-based spinlock implementation, which will be
 * amazingly slow.
 *
 * It's not self-evident that every possible legal implementation of a
 * spinlock acquire-and-release would be equivalent to a full memory barrier.
 * For example, I'm not sure that Itanium's acq and rel add up to a full
 * fence.  But all of our actual implementations seem OK in this regard.
 */
#if !defined(pg_memory_barrier)
#define pg_memory_barrier() \
	do { S_LOCK(&dummy_spinlock); S_UNLOCK(&dummy_spinlock); } while (0)
#endif

/*
 * If read or write barriers are undefined, we upgrade them to full memory
 * barriers.
 *
 * If a compiler barrier is unavailable, you probably don't want a full
 * memory barrier instead, so if you have a use case for a compiler barrier,
 * you'd better use #ifdef.
 */
#if !defined(pg_read_barrier)
#define pg_read_barrier()			pg_memory_barrier()
#endif
#if !defined(pg_write_barrier)
#define pg_write_barrier()			pg_memory_barrier()
#endif

#endif   /* BARRIER_H */
