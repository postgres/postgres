#if defined(__i386__) || defined(__x86_64__)
#define NEED_I386_TAS_ASM
typedef unsigned char slock_t;
#endif
#if defined(__ia64)
typedef unsigned int slock_t;
#endif
#if defined(__sparc__)
#define NEED_SPARC_TAS_ASM
typedef unsigned char slock_t;
#endif

#define HAS_TEST_AND_SET

