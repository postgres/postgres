#define HAS_TEST_AND_SET
#if defined(__powerpc__)
typedef unsigned int slock_t;
#else
typedef unsigned char slock_t;
#endif
