/* __USE_POSIX, __USE_BSD, and __USE_BSD_SIGNAL used to be defined either
   here or with -D compile options, but __ macros should be set and used by C
   library macros, not Postgres code.  __USE_POSIX is set by features.h,
   __USE_BSD is set by bsd/signal.h, and __USE_BSD_SIGNAL appears not to
   be used.
*/
#define JMP_BUF
#define USE_POSIX_TIME
#define USE_POSIX_SIGNALS
#define NEED_I386_TAS_ASM
#define HAS_TEST_AND_SET

#if defined(PPC)
typedef unsigned int slock_t;

#endif

#if defined(PPC)
#undef NEED_I386_TAS_ASM
#undef HAVE_INT_TIMEZONE
#endif

#if defined(sparc)
#undef NEED_I386_TAS_ASM
#endif


#if defined(__alpha__)
#undef NEED_I386_TAS_ASM
#endif
#if defined(__alpha__)
typedef long int slock_t;

#else
typedef unsigned char slock_t;

#endif
