/*-------------------------------------------------------------------------
 *
 * generic-xlc.h
 *	  Atomic operations for IBM's CC
 *
 * Portions Copyright (c) 2013-2016, PostgreSQL Global Development Group
 *
 * NOTES:
 *
 * Documentation:
 * * Synchronization and atomic built-in functions
 *   http://publib.boulder.ibm.com/infocenter/lnxpcomp/v8v101/topic/com.ibm.xlcpp8l.doc/compiler/ref/bif_sync.htm
 *
 * src/include/port/atomics/generic-xlc.h
 *
 * -------------------------------------------------------------------------
 */

#if defined(HAVE_ATOMICS)

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

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U32
static inline bool
pg_atomic_compare_exchange_u32_impl(volatile pg_atomic_uint32 *ptr,
									uint32 *expected, uint32 newval)
{
	/*
	 * xlc's documentation tells us:
	 * "If __compare_and_swap is used as a locking primitive, insert a call to
	 * the __isync built-in function at the start of any critical sections."
	 */
	__isync();

	/*
	 * XXX: __compare_and_swap is defined to take signed parameters, but that
	 * shouldn't matter since we don't perform any arithmetic operations.
	 */
	return __compare_and_swap((volatile int*)&ptr->value,
							  (int *)expected, (int)newval);
}

#define PG_HAVE_ATOMIC_FETCH_ADD_U32
static inline uint32
pg_atomic_fetch_add_u32_impl(volatile pg_atomic_uint32 *ptr, int32 add_)
{
	return __fetch_and_add((volatile int *)&ptr->value, add_);
}

#ifdef PG_HAVE_ATOMIC_U64_SUPPORT

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U64
static inline bool
pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
									uint64 *expected, uint64 newval)
{
	__isync();

	return __compare_and_swaplp((volatile long*)&ptr->value,
								(long *)expected, (long)newval);;
}

#define PG_HAVE_ATOMIC_FETCH_ADD_U64
static inline uint64
pg_atomic_fetch_add_u64_impl(volatile pg_atomic_uint64 *ptr, int64 add_)
{
	return __fetch_and_addlp((volatile long *)&ptr->value, add_);
}

#endif /* PG_HAVE_ATOMIC_U64_SUPPORT */

#endif /* defined(HAVE_ATOMICS) */
