
/* the purpose of this file is to reduce the use of #ifdef's through
 * the code base by those porting the software, an dto facilitate the
 * eventual use of autoconf to build the server 
 */

#ifndef CONFIG_H
#define CONFIG_H

#define BLCKSZ	8192


#if defined(win32)
#  define WIN32
#  define NO_UNISTD_H
#  define USES_WINSOCK 
#  define NOFILE	100
#  define NEED_UNION_SEMUN
#endif /* WIN32 */

#if defined(__FreeBSD__) || \
    defined(__NetBSD__) || \
    defined(bsdi)
#  define USE_LIMITS_H
#endif

#if defined(aix)
#  define NEED_SYS_SELECT_H
#endif

#if defined(irix5)
#  define NO_VFORK
#endif

/*
 * The following is used as the arg list for signal handlers.  Any ports
 * that take something other than an int argument should change this in
 * the port specific makefile.  Note that variable names are required
 * because it is used in both the prototypes as well as the definitions.
 * Note also the long name.  We expect that this won't collide with
 * other names causing compiler warnings.
*/ 

#ifndef       SIGNAL_ARGS
#define SIGNAL_ARGS int postgres_signal_arg
#endif

# NAMEDATALEN is the max length for system identifiers (e.g. table names,
# attribute names, function names, etc.)
#
# These MUST be set here.  DO NOT COMMENT THESE OUT
# Setting these too high will result in excess space usage for system catalogs
# Setting them too low will make the system unusable.
# values between 16 and 64 that are multiples of four are recommended.
#
# NOTE also that databases with different NAMEDATALEN's cannot interoperate!
# 
#define NAMEDATALEN 32
# OIDNAMELEN should be set to NAMEDATALEN + sizeof(Oid) 
#define OIDNAMELEN  36

/*
 * On architectures for which we have not implemented spinlocks (or
 * cannot do so), we use System V semaphores.  We also use them for
 * long locks.  For some reason union semun is never defined in the
 * System V header files so we must do it ourselves.
 */
#if defined(sequent) || \
    defined(PORTNAME_aix) || \
    defined(PORTNAME_alpha) || \
    defined(PORTNAME_bsdi) || \
    defined(PORTNAME_hpux) || \
    defined(PORTNAME_dgux) || \
    defined(PORTNAME_i386_solaris) || \
    defined(PORTNAME_sparc_solaris) || \
    defined(PORTNAME_ultrix4) || \
    defined(PORTNAME_svr4)
#define NEED_UNION_SEMUN 
#endif

/*  Debug and various "defines" that should be documented */

/* found in function aclparse() in src/backend/utils/adt/acl.c */
/* #define ACLDEBUG */

/* found in src/backend/utils/adt/arrayfuncs.c */
/* #define LOARRAY */
#define ESCAPE_PATCH
#define ARRAY_PATCH

/* Fixes use of indexes infunctions */
#define INDEXSCAN_PATCH 

/* found in src/backend/utils/adt/date.c */
/* #define DATEDEBUG */

/* found in src/backend/utils/adt/datetimes.c */
/* #define USE_SHORT_YEAR */
/* #define AMERICAN_STYLE */

/*----------------------------------------*/
/* found in src/backend/utils/adt/float.c */
/*------------------------------------------------------*/
/* defining unsafe floats's will make float4 and float8 */
/* ops faster at the cost of safety, of course!         */
/*------------------------------------------------------*/
/* #define UNSAFE_FLOATS */

/*

There is a bug in the function executor. The backend crashes while trying to
execute an sql function containing an utility command (create, notify, ...).
The bug is part in the planner, which returns a number of plans different
than the number of commands if there are utility commands in the query, and
in part in the function executor which assumes that all commands are normal
query commands and causes a SIGSEGV trying to execute commands without plan.

*/
#define FUNC_UTIL_PATCH

/*

Async notifies received while a backend is in the middle of a begin/end
transaction block are lost by libpq when the final end command is issued.

The bug is in the routine PQexec of libpq. The routine throws away any
message from the backend when a message of type 'C' is received. This
type of message is sent when the result of a portal query command with
no tuples is returned. Unfortunately this is the case of the end command.
As all async notification are sent only when the transaction is finished,
if they are received in the middle of a transaction they are lost in the
libpq library. I added some tracing code to PQexec and this is the output:

*/
#define PQ_NOTIFY_PATCH
#endif /* CONFIG_H */

