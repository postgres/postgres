#if defined(__i386__)
typedef unsigned char slock_t;
#define HAS_TEST_AND_SET
#endif

#if defined(__sparc__)
#define NEED_SPARC_TAS_ASM
#define HAS_TEST_AND_SET
#endif

#if defined(__alpha__)
typedef long int slock_t;
#define HAS_TEST_AND_SET
#endif

#if defined(__vax__)
typedef unsigned char slock_t;
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
