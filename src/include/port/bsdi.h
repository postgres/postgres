#if defined(__i386__)
#define NEED_I386_TAS_ASM
#endif
#if defined(__sparc__)
#define NEED_SPARC_TAS_ASM
#endif
#define USE_POSIX_TIME
#define HAS_TEST_AND_SET
typedef unsigned char slock_t;
