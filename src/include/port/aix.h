/*
 * src/include/port/aix.h
 */
#define CLASS_CONFLICT
#define DISABLE_XOPEN_NLS

/*
 * "IBM XL C/C++ for AIX, V12.1" miscompiles, for 32-bit, some inline
 * expansions of ginCompareItemPointers() "long long" arithmetic.  To take
 * advantage of inlining, build a 64-bit PostgreSQL.
 */
#if defined(__ILP32__) && defined(__IBMC__)
#define PG_FORCE_DISABLE_INLINE
#endif
