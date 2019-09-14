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

#define PG_HAVE_ATOMIC_U32_SUPPORT
typedef struct pg_atomic_uint32
{
	volatile uint32 value;
} pg_atomic_uint32;

/* 64bit atomics are only supported in 64bit mode */
#ifdef __64BIT__
#define PG_HAVE_ATOMIC_U64_SUPPORT
typedef struct pg_atomic_uint64
{
	volatile uint64 value pg_attribute_aligned(8);
} pg_atomic_uint64;

#endif /* __64BIT__ */

#define PG_HAVE_ATOMIC_FETCH_ADD_U32
static inline uint32
pg_atomic_fetch_add_u32_impl(volatile pg_atomic_uint32 *ptr, int32 add_)
{
	uint32 _t;
	uint32 res;

	/*
	 * xlc has a no-longer-documented __fetch_and_add() intrinsic.  In xlc
	 * 12.01.0000.0000, it emits a leading "sync" and trailing "isync".  In
	 * xlc 13.01.0003.0004, it emits neither.  Hence, using the intrinsic
	 * would add redundant syncs on xlc 12.
	 */
#ifdef HAVE_I_CONSTRAINT__BUILTIN_CONSTANT_P
	if (__builtin_constant_p(add_) &&
		add_ <= PG_INT16_MAX && add_ >= PG_INT16_MIN)
		__asm__ __volatile__(
			"	sync				\n"
			"	lwarx   %1,0,%4		\n"
			"	addi    %0,%1,%3	\n"
			"	stwcx.  %0,0,%4		\n"
			"	bne     $-12		\n"		/* branch to lwarx */
			"	isync				\n"
:			"=&r"(_t), "=&r"(res), "+m"(ptr->value)
:			"i"(add_), "r"(&ptr->value)
:			"memory", "cc");
	else
#endif
		__asm__ __volatile__(
			"	sync				\n"
			"	lwarx   %1,0,%4		\n"
			"	add     %0,%1,%3	\n"
			"	stwcx.  %0,0,%4		\n"
			"	bne     $-12		\n"		/* branch to lwarx */
			"	isync				\n"
:			"=&r"(_t), "=&r"(res), "+m"(ptr->value)
:			"r"(add_), "r"(&ptr->value)
:			"memory", "cc");

	return res;
}

#ifdef PG_HAVE_ATOMIC_U64_SUPPORT
#define PG_HAVE_ATOMIC_FETCH_ADD_U64
static inline uint64
pg_atomic_fetch_add_u64_impl(volatile pg_atomic_uint64 *ptr, int64 add_)
{
	uint64 _t;
	uint64 res;

	/* Like u32, but s/lwarx/ldarx/; s/stwcx/stdcx/ */
#ifdef HAVE_I_CONSTRAINT__BUILTIN_CONSTANT_P
	if (__builtin_constant_p(add_) &&
		add_ <= PG_INT16_MAX && add_ >= PG_INT16_MIN)
		__asm__ __volatile__(
			"	sync				\n"
			"	ldarx   %1,0,%4		\n"
			"	addi    %0,%1,%3	\n"
			"	stdcx.  %0,0,%4		\n"
			"	bne     $-12		\n"		/* branch to ldarx */
			"	isync				\n"
:			"=&r"(_t), "=&r"(res), "+m"(ptr->value)
:			"i"(add_), "r"(&ptr->value)
:			"memory", "cc");
	else
#endif
		__asm__ __volatile__(
			"	sync				\n"
			"	ldarx   %1,0,%4		\n"
			"	add     %0,%1,%3	\n"
			"	stdcx.  %0,0,%4		\n"
			"	bne     $-12		\n"		/* branch to ldarx */
			"	isync				\n"
:			"=&r"(_t), "=&r"(res), "+m"(ptr->value)
:			"r"(add_), "r"(&ptr->value)
:			"memory", "cc");

	return res;
}

#endif /* PG_HAVE_ATOMIC_U64_SUPPORT */

/* per architecture manual doubleword accesses have single copy atomicity */
#define PG_HAVE_8BYTE_SINGLE_COPY_ATOMICITY
