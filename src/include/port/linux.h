#if defined(__i386__) || defined(__x86_64__)
typedef unsigned char slock_t;

#define HAS_TEST_AND_SET

#elif defined(__sparc__)
typedef unsigned char slock_t;

#define HAS_TEST_AND_SET

#elif defined(__powerpc64__)
typedef unsigned long slock_t;

#define HAS_TEST_AND_SET

#elif defined(__powerpc__)
typedef unsigned int slock_t;

#define HAS_TEST_AND_SET

#elif defined(__alpha__)
typedef long int slock_t;

#define HAS_TEST_AND_SET

#elif defined(__mips__)
typedef unsigned int slock_t;

#define HAS_TEST_AND_SET

#elif defined(__arm__)
typedef unsigned char slock_t;

#define HAS_TEST_AND_SET

#elif defined(__ia64__)
typedef unsigned int slock_t;

#define HAS_TEST_AND_SET

#elif defined(__s390__) || defined(__s390x__)
typedef unsigned int slock_t;

#define HAS_TEST_AND_SET

#elif defined(__mc68000__)
typedef unsigned char slock_t;

#define HAS_TEST_AND_SET

#endif
