/*-------------------------------------------------------------------------
 *
 * generic-acc.h
 *	  Atomic operations support when using HPs acc on HPUX
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES:
 *
 * Documentation:
 * * inline assembly for Itanium-based HP-UX:
 *   http://h21007.www2.hp.com/portal/download/files/unprot/Itanium/inline_assem_ERS.pdf
 * * Implementing Spinlocks on the Intel (R) Itanium (R) Architecture and PA-RISC
 *   http://h21007.www2.hp.com/portal/download/files/unprot/itanium/spinlocks.pdf
 *
 * Itanium only supports a small set of numbers (6, -8, -4, -1, 1, 4, 8, 16)
 * for atomic add/sub, so we just implement everything but compare_exchange
 * via the compare_exchange fallbacks in atomics/generic.h.
 *
 * src/include/port/atomics/generic-acc.h
 *
 * -------------------------------------------------------------------------
 */

#include <machine/sys/inline.h>

#define pg_compiler_barrier_impl()	_Asm_sched_fence()

#if defined(HAVE_ATOMICS)

/* IA64 always has 32/64 bit atomics */

#define PG_HAVE_ATOMIC_U32_SUPPORT
typedef struct pg_atomic_uint32
{
	volatile uint32 value;
} pg_atomic_uint32;

#define PG_HAVE_ATOMIC_U64_SUPPORT
typedef struct pg_atomic_uint64
{
	/*
	 * Alignment is guaranteed to be 64bit. Search for "Well-behaved
	 * application restrictions" => "Data alignment and data sharing" on HP's
	 * website. Unfortunately the URL doesn't seem to stable enough to
	 * include.
	 */
	volatile uint64 value;
} pg_atomic_uint64;


#define MINOR_FENCE (_Asm_fence) (_UP_CALL_FENCE | _UP_SYS_FENCE | \
								 _DOWN_CALL_FENCE | _DOWN_SYS_FENCE )

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U32
static inline bool
pg_atomic_compare_exchange_u32_impl(volatile pg_atomic_uint32 *ptr,
									uint32 *expected, uint32 newval)
{
	bool	ret;
	uint32	current;

	_Asm_mov_to_ar(_AREG_CCV, *expected, MINOR_FENCE);
	/*
	 * We want a barrier, not just release/acquire semantics.
	 */
	_Asm_mf();
	/*
	 * Notes:
	 * DOWN_MEM_FENCE | _UP_MEM_FENCE prevents reordering by the compiler
	 */
	current =  _Asm_cmpxchg(_SZ_W, /* word */
							_SEM_REL,
							&ptr->value,
							newval, _LDHINT_NONE,
							_DOWN_MEM_FENCE | _UP_MEM_FENCE);
	ret = current == *expected;
	*expected = current;
	return ret;
}


#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U64
static inline bool
pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
									uint64 *expected, uint64 newval)
{
	bool	ret;
	uint64	current;

	_Asm_mov_to_ar(_AREG_CCV, *expected, MINOR_FENCE);
	_Asm_mf();
	current =  _Asm_cmpxchg(_SZ_D, /* doubleword */
							_SEM_REL,
							&ptr->value,
							newval, _LDHINT_NONE,
							_DOWN_MEM_FENCE | _UP_MEM_FENCE);
	ret = current == *expected;
	*expected = current;
	return ret;
}

#undef MINOR_FENCE

#endif /* defined(HAVE_ATOMICS) */
