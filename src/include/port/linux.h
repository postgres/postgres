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

/*
 * As of July 2007, all known versions of the Linux kernel will sometimes
 * return EIDRM for a shmctl() operation when EINVAL is correct (it happens
 * when the low-order 15 bits of the supplied shm ID match the slot number
 * assigned to a newer shmem segment).  We deal with this by assuming that
 * EIDRM means EINVAL in PGSharedMemoryIsInUse().  This is reasonably safe
 * since in fact Linux has no excuse for ever returning EIDRM; it doesn't
 * track removed segments in a way that would allow distinguishing them from
 * private ones.  But someday that code might get upgraded, and we'd have
 * to have a kernel version test here.
 */
#define HAVE_LINUX_EIDRM_BUG
