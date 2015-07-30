/*
 * For the raison d'etre of this file, check the comment above the definition
 * of the PGAC_C_INLINE macro in config/c-compiler.m4.
 */
static inline int
fun()
{
	return 0;
}

/*
 * "IBM XL C/C++ for AIX, V12.1" miscompiles, for 32-bit, some inline
 * expansions of ginCompareItemPointers() "long long" arithmetic.  To take
 * advantage of inlining, build a 64-bit PostgreSQL.
 */
#if defined(__ILP32__) && defined(__IBMC__)
#error "known inlining bug"
#endif
