#define USE_POSIX_TIME

#if defined(i386)
#define NEED_I386_TAS_ASM
#define HAS_TEST_AND_SET
#endif

#if defined(sparc)
#define NEED_SPARC_TAS_ASM
#define HAS_TEST_AND_SET
#endif

#if defined(vax)
#define NEED_VAX_TAS_ASM
#define HAS_TEST_AND_SET
#endif

#if defined(__ns32k__)
#define NEED_NS32K_TAS_ASM
#define HAS_TEST_AND_SET
#endif

#if defined(__m68k__)
#define HAS_TEST_AND_SET
#endif

#if defined(__mips__)
/* #	undef HAS_TEST_AND_SET */
#endif
typedef unsigned char slock_t;
