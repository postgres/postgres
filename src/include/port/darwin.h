#define __darwin__	1

#if defined(__ppc__)
#define HAS_TEST_AND_SET
#endif

#if defined(__ppc__)
typedef unsigned int slock_t;

#else
typedef unsigned char slock_t;

#endif
