/* the purpose of this file is to reduce the use of #ifdef's through
 * the code base by those porting the software, and to facilitate the
 * eventual use of autoconf to build the server 
 */

#ifndef CONFIG_H
#define CONFIG_H

#define BLCKSZ	8192

#define HAVE_SYS_SELECT_H
#define HAVE_TERMIOS_H
#define HAVE_VALUES_H

#define HAVE_MEMMOVE
#define HAVE_SIGSETJMP
#define HAVE_KILL
#define HAVE_ISINF
#define HAVE_CBRT
#define HAVE_RINT
#define HAVE_RUSAGE

#if defined(aix)
#  undef HAVE_SYS_SELECT_H
#  undef HAVE_TERMIOS_H
#  undef HAVE_ISINF
#  define CLASS_CONFLICT 
#  define DISABLE_XOPEN_NLS 
#  define NEED_UNION_SEMUN 
#  define HAVE_TZSET
#  define HAVE_ANSI_CPP
#  define HAS_TEST_AND_SET
   typedef unsigned int slock_t;
#endif

#if defined(alpha)
#  undef HAVE_ISINF 
#  define USE_POSIX_TIME 
#  define USE_POSIX_SIGNALS
#  define DISABLE_XOPEN_NLS 
#  define HAS_LONG_LONG
#  define NEED_UNION_SEMUN 
#  define HAS_TEST_AND_SET
#  include <sys/mman.h>  /* for msemaphore */
   typedef msemaphore slock_t;
#endif

#if defined(BSD44_derived)
#  define HAVE_LIMITS_H
#  define USE_POSIX_TIME
#  define NEED_I386_TAS_ASM
#  define HAS_TEST_AND_SET
#  if defined(__mips__)
#    undef HAS_TEST_AND_SET
#  endif
   typedef unsigned char slock_t;
#endif

#if defined(bsdi)
#  if defined(i386)
#    define NEED_I386_TAS_ASM
#  endif
#  if defined(sparc)
#    define NEED_SPARC_TAS_ASM
#  endif
#  if defined(PRE_BSDI_2_1)
#    define NEED_UNION_SEMUN 
#  endif
#  define HAVE_LIMITS_H
#  define USE_POSIX_TIME
#  undef HAVE_CBRT
#  define HAS_TEST_AND_SET
   typedef unsigned char slock_t;
#endif


#if defined(dgux)
#  define LINUX_ELF
#  define NEED_UNION_SEMUN 
#  define USE_POSIX_SIGNALS
#endif

#if defined(hpux)
#  define JMP_BUF
#  define USE_POSIX_TIME
#  define HAVE_TZSET
#  undef HAVE_CBRT
#  undef HAVE_RINT
#  define NEED_UNION_SEMUN 
#  define HAS_TEST_AND_SET
   typedef struct { int sem[4]; } slock_t;
#endif

#if defined(i386_solaris) 
#  define HAVE_LIMITS_H
#  define USE_POSIX_TIME 
#  define USE_POSIX_SIGNALS
#  undef HAVE_ISINF 
#  undef HAVE_RUSAGE 
#  define NO_EMPTY_STMTS
#  define HAVE_TZSET
#  define NEED_UNION_SEMUN 
#  define SYSV_DIRENT
#  define HAS_TEST_AND_SET
   typedef unsigned char slock_t;
#endif

#if defined(irix5)
#  define USE_POSIX_TIME 
#  define USE_POSIX_SIGNALS
#  undef HAVE_ISINF 
#  define NO_EMPTY_STMTS
#  define NO_VFORK
#  define HAVE_TZSET
#  define SYSV_DIRENT
#  define HAS_TEST_AND_SET
#  include <abi_mutex.h>
   typedef abilock_t slock_t;
#endif

#if defined(linux)
/* __USE_POSIX, __USE_BSD, and __USE_BSD_SIGNAL used to be defined either
   here or with -D compile options, but __ macros should be set and used by C
   library macros, not Postgres code.  __USE_POSIX is set by features.h,
   __USE_BSD is set by bsd/signal.h, and __USE_BSD_SIGNAL appears not to
   be used.
*/
#  define JMP_BUF
#  define USE_POSIX_TIME
#  define HAVE_TZSET
#  undef HAVE_CBRT
#  define NEED_I386_TAS_ASM
#  define HAS_TEST_AND_SET
   typedef unsigned char slock_t;
#endif

#if defined(nextstep)
# undef HAVE_VALUES_H
# include <sys/ioctl.h>
# if defined(__STRICT_ANSI__)
#  define isascii(c)  ((unsigned)(c)<=0177)
# endif
  extern char* strdup (const char* string);
# ifndef _POSIX_SOURCE
  typedef unsigned short mode_t;
  typedef int sigset_t;
#  define SIG_BLOCK	00
#  define SIG_UNBLOCK	01
#  define SIG_SETMASK	02
#  define NO_SIGACTION
#  define NO_SETSID
#  define NO_SIGPROCMASK
#  undef HAVE_SIGSETJMP
# endif

# define HAVE_LIMITS_H
# define JMP_BUF
# define NO_WAITPID
  typedef struct mutex slock_t;
#endif

#if defined(sequent) 
#  define NEED_UNION_SEMUN 
#endif

#if defined(sparc_solaris)
#  define HAVE_LIMITS_H
#  define USE_POSIX_TIME 
#  define USE_POSIX_SIGNALS
#  undef HAVE_ISINF 
#  undef HAVE_RUSAGE 
#  define NO_EMPTY_STMTS
#  define USE_POSIX_TIME
#  define HAVE_TZSET
#  define NEED_UNION_SEMUN 
#  define SYSV_DIRENT
#  define HAS_TEST_AND_SET
typedef unsigned char slock_t;
#endif

#if defined(sunos4)
#  define USE_POSIX_TIME
#  undef HAVE_MEMMOVE
#endif

#if defined(svr4) 
#  define USE_POSIX_TIME 
#  define USE_POSIX_SIGNALS
#  undef HAVE_ISINF 
#  undef HAVE_RUSAGE 
#  define NO_EMPTY_STMTS
#  define HAVE_TZSET
#  define NEED_UNION_SEMUN 
#  define SYSV_DIRENT
#endif

#if defined(win32)
#  undef HAVE_KILL
#  undef HAVE_SIGSETJMP
#  undef HAVE_CBRT
#  undef HAVE_ISINF
#  define JMP_BUF
#  define NO_UNISTD_H
#  define USES_WINSOCK 
#  define NOFILE	100
#  define NEED_UNION_SEMUN
#  define HAVE_TZSET
#  ifndef MAXPATHLEN
#  define MAXPATHLEN    250
#  endif
#endif /* WIN32 */

#if defined(ultrix4)
#  undef HAVE_ISINF 
#  define USE_POSIX_TIME
#  define NEED_UNION_SEMUN 
#  define NEED_STRDUP
#endif

/* This patch changes the behavior of aclcheck for groups. Currently an user
 * can access a table only if he has the required permission for ALL the groups
 * defined for that table. With my patch he can access a table if he has the
 * permission for ONE of the groups, which seems to me a more useful thing.
 * 
 * Used in: src/backend/tcop/aclchk.c
 * Submitted by: Massimo Dal Zotto <dz@cs.unitn.it>
 */
#define	ACLGROUP_PATCH


/*
 * The following is used as the arg list for signal handlers.  Any ports
 * that take something other than an int argument should change this in
 * the port specific makefile.  Note that variable names are required
 * because it is used in both the prototypes as well as the definitions.
 * Note also the long name.  We expect that this won't collide with
 * other names causing compiler warnings.
 */ 

#ifndef       SIGNAL_ARGS
#  define SIGNAL_ARGS int postgres_signal_arg
#endif

/* 
 * DEF_PGPORT is the TCP port number on which the Postmaster listens by
 * default.  This can be overriden by command options, environment variables,
 * and the postconfig hook.
 */ 

#define DEF_PGPORT "5432"

/* turn this on if you prefer European style dates instead of American
 * style dates
 */
/* #define EUROPEAN_DATES  */

/*
 * If you do not plan to use Host based authentication,
 * comment out the following line
 */
#define HBA

/*
 * On architectures for which we have not implemented spinlocks (or
 * cannot do so), we use System V semaphores.  We also use them for
 * long locks.  For some reason union semun is never defined in the
 * System V header files so we must do it ourselves.
 */

/*  Debug and various "defines" that should be documented */

/* found in function aclparse() in src/backend/utils/adt/acl.c */
/* #define ACLDEBUG */

/* found in src/backend/utils/adt/arrayfuncs.c
   code seems broken without it, Bruce Momjian */
/* #define LOARRAY */

/* This is the time, in seconds, at which a given backend server
 * will wait on a lock before deciding to abort the transaction
 * (this is what we do in lieu of deadlock detection).
 *
 * Low numbers are not recommended as they will tend to cause
 * false aborts if many transactions are long-lived.
 */
#define DEADLOCK_TIMEOUT 60

/*
 * This flag enables the use of idexes in plans generated for function
 * executions which normally are always executed with sequential scans.
 */
#define INDEXSCAN_PATCH 

/* #define DATEDEBUG */

/*
 * Define this if you want to use date constants with a short year
 * like '01/05/96'.
 */
/* #define USE_SHORT_YEAR */

/*
 * defining unsafe floats's will make float4 and float8
 * ops faster at the cost of safety, of course!        
 */
/* #define UNSAFE_FLOATS */

/*
 * There is a bug in the function executor. The backend crashes while trying to
 * execute an sql function containing an utility command (create, notify, ...).
 * The bug is part in the planner, which returns a number of plans different
 * than the number of commands if there are utility commands in the query, and
 * in part in the function executor which assumes that all commands are normal
 * query commands and causes a SIGSEGV trying to execute commands without plan.
 */
#define FUNC_UTIL_PATCH

/*
 * Define this if you want to retrieve arrays attributes as Tcl lists instead
 * of postgres C-like arrays, for example {{"a1" "a2"} {"b1" "b2"}} instead 
 * of {{"a1","a2"},{"b1","b2"}}.
 */
#define TCL_ARRAYS

/*
 * The comparison routines for text and char data type give incorrect results
 * if the input data contains characters greater than 127.  As these routines
 * perform the comparison using signed char variables all character codes
 * greater than 127 are interpreted as less than 0.  These codes are used to
 * encode the iso8859 char sets.  Define this flag to correct the problem.
 */
#define UNSIGNED_CHAR_TEXT

/*
 * The following flag allows limiting the number of rows returned by a query.
 * You will need the loadable module utils.c to use this feature.
 */
#define QUERY_LIMIT

/*
 * The following flag allows copying tables from files with number of columns
 * different than the number of attributes setting missing attributes to NULL
 * and ignoring extra columns.  This also avoids the shift of the attributes
 * of the rest of the file if one line has a wrong column count.
 */
#define COPY_PATCH

/*
 * User locks are handled totally on the application side as long term
 * cooperative locks which extend beyond the normal transaction boundaries.
 * Their purpose is to indicate to an application that someone is `working'
 * on an item.  Define this flag to enable user locks.  You will need the
 * loadable module user-locks.c to use this feature.
 */
#define USER_LOCKS

/* Debug #defines */
/* #define IPORTAL_DEBUG  */
/* #define HEAPDEBUGALL  */
/* #define ISTRATDEBUG  */
/* #define FASTBUILD_DEBUG */
#define RTDEBUG 
#define GISTDEBUG 
/* #define PURGEDEBUG */
/* #define DEBUG_RECIPE */
/* #define ASYNC_DEBUG */
/* #define COPY_DEBUG */
/* #define VACUUM_DEBUG */
/* #define NBTINSERT_PATCH_DEBUG */


/* The following don't have any apparent purpose, but are in the
 * code.  someday, will take them out altogether, but for now, 
 * document them here
 */
/* #define OMIT_PARTIAL_INDEX */
/* #define NO_BUFFERISVALID   */
/* #define NO_SECURITY        */
/* #define TIOGA              */
/* #define OLD_REWRITE        */
/* #define NOTYET             */


/* Undocumented "features"? */
#define FASTBUILD /* access/nbtree/nbtsort.c */






#endif /* CONFIG_H */

