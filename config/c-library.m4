# Macros that test various C library quirks
# $Header: /cvsroot/pgsql/config/c-library.m4,v 1.11 2002/02/23 04:17:45 petere Exp $


# PGAC_VAR_INT_TIMEZONE
# ---------------------
# Check if the global variable `timezone' exists. If so, define
# HAVE_INT_TIMEZONE.
AC_DEFUN([PGAC_VAR_INT_TIMEZONE],
[AC_CACHE_CHECK(for int timezone, pgac_cv_var_int_timezone,
[AC_TRY_LINK([#include <time.h>
int res;],
  [res = timezone / 60;],
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


# PGAC_FUNC_MEMCMP
# -----------
# Check if memcmp() properly handles negative bytes and returns +/-.
# SunOS does not.
# AC_FUNC_MEMCMP
AC_DEFUN(PGAC_FUNC_MEMCMP,
[AC_CACHE_CHECK(for 8-bit clean memcmp, pgac_cv_func_memcmp_clean,
[AC_TRY_RUN([
main()
{
  char c0 = 0x40, c1 = 0x80, c2 = 0x81;
  exit(memcmp(&c0, &c2, 1) < 0 && memcmp(&c1, &c2, 1) < 0 ? 0 : 1);
}
], pgac_cv_func_memcmp_clean=yes, pgac_cv_func_memcmp_clean=no,
pgac_cv_func_memcmp_clean=no)])
if test $pgac_cv_func_memcmp_clean = no ; then
  MEMCMP=memcmp.o
else
  MEMCMP=
fi
AC_SUBST(MEMCMP)dnl
])


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


# PGAC_STRUCT_CMSGCRED
# --------------------
# Check if `struct cmsgcred' exists. Define HAVE_STRUCT_CMSGCRED if so.
AC_DEFUN([PGAC_STRUCT_CMSGCRED],
[AC_CACHE_CHECK(for struct cmsgcred, pgac_cv_struct_cmsgcred,
[AC_TRY_COMPILE([#include <sys/param.h>
#include <sys/socket.h>
#include <sys/ucred.h>],
  [struct cmsgcred sockcred;],
  [pgac_cv_struct_cmsgcred=yes],
  [pgac_cv_struct_cmsgcred=no])])
if test x"$pgac_cv_struct_cmsgcred" = xyes ; then
  AC_DEFINE(HAVE_STRUCT_CMSGCRED, 1, [Set to 1 if you have `struct cmsgcred'])
fi])# PGAC_STRUCT_CMSGCRED


# PGAC_STRUCT_FCRED
# -----------------
# Check if `struct fcred' exists. Define HAVE_STRUCT_FCRED if so.
AC_DEFUN([PGAC_STRUCT_FCRED],
[AC_CACHE_CHECK(for struct fcred, pgac_cv_struct_fcred,
[AC_TRY_COMPILE([#include <sys/param.h>
#include <sys/socket.h>
#include <sys/ucred.h>],
  [struct fcred sockcred;],
  [pgac_cv_struct_fcred=yes],
  [pgac_cv_struct_fcred=no])])
if test x"$pgac_cv_struct_fcred" = xyes ; then
  AC_DEFINE(HAVE_STRUCT_FCRED, 1, [Set to 1 if you have `struct fcred'])
fi])# PGAC_STRUCT_FCRED


# PGAC_STRUCT_SOCKCRED
# --------------------
# Check if `struct sockcred' exists. Define HAVE_STRUCT_SOCKCRED if so.
AC_DEFUN([PGAC_STRUCT_SOCKCRED],
[AC_CACHE_CHECK(for struct sockcred, pgac_cv_struct_sockcred,
[AC_TRY_COMPILE([#include <sys/param.h>
#include <sys/socket.h>
#include <sys/ucred.h>],
  [struct sockcred sockcred;],
  [pgac_cv_struct_sockcred=yes],
  [pgac_cv_struct_sockcred=no])])
if test x"$pgac_cv_struct_sockcred" = xyes ; then
  AC_DEFINE(HAVE_STRUCT_SOCKCRED, 1, [Set to 1 if you have `struct sockcred'])
fi])# PGAC_STRUCT_SOCKCRED


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


# PGAC_CHECK_MEMBER(AGGREGATE.MEMBER,
#                   [ACTION-IF-FOUND], [ACTION-IF-NOT-FOUND],
#                   [INCLUDES])
# -----------------------------------------------------------

AC_DEFUN([PGAC_CHECK_MEMBER],
[changequote(<<, >>)dnl
dnl The name to #define.
define(<<pgac_define_name>>, translit(HAVE_$1, [a-z .*], [A-Z__P]))dnl
dnl The cache variable name.
define(<<pgac_cache_name>>, translit(pgac_cv_member_$1, [ .*], [__p]))dnl
changequote([, ])dnl
AC_CACHE_CHECK([for $1], [pgac_cache_name],
[AC_TRY_COMPILE([$4],
[static ]patsubst([$1], [\..*])[ pgac_var;
if (pgac_var.]patsubst([$1], [^[^.]*\.])[)
return 0;],
[pgac_cache_name=yes],
[pgac_cache_name=no])])

if test x"[$]pgac_cache_name" = x"yes"; then
  AC_DEFINE_UNQUOTED(pgac_define_name)
  $2
else
  ifelse([$3], [], :, [$3])
fi
undefine([pgac_define_name])[]dnl
undefine([pgac_cache_name])[]dnl
])
