# Macros that test various C library quirks
# $Header: /cvsroot/pgsql/config/c-library.m4,v 1.6 2001/01/09 18:40:13 petere Exp $


# PGAC_VAR_INT_TIMEZONE
# ---------------------
# Check if the global variable `timezone' exists. If so, define
# HAVE_INT_TIMEZONE.
AC_DEFUN([PGAC_VAR_INT_TIMEZONE],
[AC_CACHE_CHECK(for int timezone, pgac_cv_var_int_timezone,
[AC_TRY_LINK([#include <time.h>],
  [int res = timezone / 60;],
  [pgac_cv_var_int_timezone=yes],
  [pgac_cv_var_int_timezone=no])])
if test x"$pgac_cv_var_int_timezone" = xyes ; then
  AC_DEFINE(HAVE_INT_TIMEZONE,, [Set to 1 if you have the global variable timezone])
fi])# PGAC_VAR_INT_TIMEZONE


# PGAC_FUNC_GETTIMEOFDAY_1ARG
# ---------------------------
# Check if gettimeofday() has only one arguments. (Normal is two.)
# If so, define GETTIMEOFDAY_1ARG.
AC_DEFUN([PGAC_FUNC_GETTIMEOFDAY_1ARG],
[AC_CACHE_CHECK(whether gettimeofday takes only one argument,
pgac_cv_func_gettimeofday_1arg,
[AC_TRY_COMPILE([#include <sys/time.h>],
[struct timeval *tp;
struct timezone *tzp;
gettimeofday(tp,tzp);],
[pgac_cv_func_gettimeofday_1arg=no],
[pgac_cv_func_gettimeofday_1arg=yes])])
if test x"$pgac_cv_func_gettimeofday_1arg" = xyes ; then
  AC_DEFINE(GETTIMEOFDAY_1ARG,, [Set to 1 if gettimeofday() takes only 1 argument])
fi])# PGAC_FUNC_GETTIMEOFDAY_1ARG


# PGAC_UNION_SEMUN
# ----------------
# Check if `union semun' exists. Define HAVE_UNION_SEMUN if so.
# If it doesn't then one could define it as
# union semun { int val; struct semid_ds *buf; unsigned short *array; }
AC_DEFUN([PGAC_UNION_SEMUN],
[AC_CACHE_CHECK(for union semun, pgac_cv_union_semun,
[AC_TRY_COMPILE([#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>],
  [union semun semun;],
  [pgac_cv_union_semun=yes],
  [pgac_cv_union_semun=no])])
if test x"$pgac_cv_union_semun" = xyes ; then
  AC_DEFINE(HAVE_UNION_SEMUN, 1, [Set to 1 if you have `union semun'])
fi])# PGAC_UNION_SEMUN


# PGAC_STRUCT_SOCKADDR_UN
# -----------------------
# If `struct sockaddr_un' exists, define HAVE_STRUCT_SOCKADDR_UN. If
# it is missing then one could define it as { short int sun_family;
# char sun_path[108]; }. (Requires test for <sys/un.h>!)
AC_DEFUN([PGAC_STRUCT_SOCKADDR_UN],
[AC_CACHE_CHECK([for struct sockaddr_un], pgac_cv_struct_sockaddr_un,
[AC_TRY_COMPILE([#include <sys/types.h>
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif],
                [struct sockaddr_un un;],
                [pgac_cv_struct_sockaddr_un=yes],
                [pgac_cv_struct_sockaddr_un=no])])
if test x"$pgac_cv_struct_sockaddr_un" = xyes; then
  AC_DEFINE(HAVE_STRUCT_SOCKADDR_UN, 1, [Set to 1 if you have `struct sockaddr_un'])
fi])# PGAC_STRUCT_SOCKADDR_UN


# PGAC_FUNC_POSIX_SIGNALS
# -----------------------
# Check to see if the machine has the POSIX signal interface. Define
# HAVE_POSIX_SIGNALS if so. Also set the output variable HAVE_POSIX_SIGNALS
# to yes or no.
#
# Note that this test only compiles a test program, it doesn't check
# whether the routines actually work. If that becomes a problem, make
# a fancier check.
AC_DEFUN([PGAC_FUNC_POSIX_SIGNALS],
[AC_CACHE_CHECK(for POSIX signal interface, pgac_cv_func_posix_signals,
[AC_TRY_LINK([#include <signal.h>
],
[struct sigaction act, oact;
sigemptyset(&act.sa_mask);
act.sa_flags = SA_RESTART;
sigaction(0, &act, &oact);],
[pgac_cv_func_posix_signals=yes],
[pgac_cv_func_posix_signals=no])])
if test x"$pgac_cv_func_posix_signals" = xyes ; then
  AC_DEFINE(HAVE_POSIX_SIGNALS,, [Set to 1 if you have the POSIX signal interface])
fi
HAVE_POSIX_SIGNALS=$pgac_cv_func_posix_signals
AC_SUBST(HAVE_POSIX_SIGNALS)])# PGAC_FUNC_POSIX_SIGNALS


# PGAC_HEADER_STRING
# ------------------
# Tests whether <string.h> and <strings.h> can both be included
# (without generating warnings).  This is mostly useful if you need
# str[n]casecmp(), since this is not in the "standard" <string.h>
# on some systems.
AC_DEFUN([PGAC_HEADER_STRING],
[AC_CACHE_CHECK([whether string.h and strings.h may both be included],
                [pgac_cv_header_strings_both],
[AC_TRY_CPP(
[#include <string.h>
#include <strings.h>
],
[AC_TRY_COMPILE(
[#include <string.h>
#include <strings.h>
],
[int n = strcasecmp("a", "b");],
[pgac_cv_header_strings_both=yes],
[pgac_cv_header_strings_both=no])],
[pgac_cv_header_strings_both=no])])
if test x"$pgac_cv_header_strings_both" = x"yes"; then
  AC_DEFINE([STRING_H_WITH_STRINGS_H], 1,
            [Define if string.h and strings.h may both be included])
fi])


# PGAC_VAR_SYS_NERR
# -----------------
# Check if the global variable 'sys_nerr' exists.  If so, define
# HAVE_SYS_NERR.
AC_DEFUN([PGAC_VAR_SYS_NERR],
[AC_CACHE_CHECK([for sys_nerr], pgac_cv_var_sys_nerr,
[AC_TRY_LINK([extern int sys_nerr;],
  [int x = sys_nerr;],
  [pgac_cv_var_sys_nerr=yes],
  [pgac_cv_var_sys_nerr=no])])
if test x"$pgac_cv_var_sys_nerr" = xyes ; then
  AC_DEFINE(HAVE_SYS_NERR,, [Set to 1 if you have the global variable sys_nerr])
fi])# PGAC_VAR_SYS_NERR
