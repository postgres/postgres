#define USE_POSIX_TIME
#if defined(i386)
#define NEED_I386_TAS_ASM
#endif
#if defined(sparc)
#define NEED_SPARC_TAS_ASM
#endif
#if defined(ns32k)
#define NEED_NS32k_TAS_ASM
#endif
#define HAS_TEST_AND_SET
#if defined(__mips__)
/* #	undef HAS_TEST_AND_SET */
#endif
typedef unsigned char slock_t;
