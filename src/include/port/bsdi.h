#if defined(__i386__) || defined(__x86_64__)
typedef unsigned char slock_t;
#endif
#if defined(__ia64)
typedef unsigned int slock_t;
#endif
#if defined(__sparc__)
typedef unsigned char slock_t;
#endif

#define HAS_TEST_AND_SET

