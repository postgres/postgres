# Macros that test various C library quirks
# config/c-library.m4


# PGAC_VAR_INT_TIMEZONE
# ---------------------
# Check if the global variable `timezone' exists. If so, define
# HAVE_INT_TIMEZONE.
AC_DEFUN([PGAC_VAR_INT_TIMEZONE],
[AC_CACHE_CHECK(for int timezone, pgac_cv_var_int_timezone,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <time.h>
int res;],
  [#ifndef __CYGWIN__
res = timezone / 60;
#else
res = _timezone / 60;
#endif])],
  [pgac_cv_var_int_timezone=yes],
  [pgac_cv_var_int_timezone=no])])
if test x"$pgac_cv_var_int_timezone" = xyes ; then
  AC_DEFINE(HAVE_INT_TIMEZONE, 1,
            [Define to 1 if you have the global variable 'int timezone'.])
fi])# PGAC_VAR_INT_TIMEZONE


# PGAC_STRUCT_TIMEZONE
# ------------------
# Figure out how to get the current timezone.  If `struct tm' has a
# `tm_zone' member, define `HAVE_TM_ZONE'.  Also, if the
# external array `tzname' is found, define `HAVE_TZNAME'.
# This is the same as the standard macro AC_STRUCT_TIMEZONE, except that
# tzname[] is checked for regardless of whether we find tm_zone.
AC_DEFUN([PGAC_STRUCT_TIMEZONE],
[AC_REQUIRE([AC_STRUCT_TM])dnl
AC_CHECK_MEMBERS([struct tm.tm_zone],,,[#include <sys/types.h>
#include <$ac_cv_struct_tm>
])
if test "$ac_cv_member_struct_tm_tm_zone" = yes; then
  AC_DEFINE(HAVE_TM_ZONE, 1,
            [Define to 1 if your `struct tm' has `tm_zone'. Deprecated, use
             `HAVE_STRUCT_TM_TM_ZONE' instead.])
fi
AC_CACHE_CHECK(for tzname, ac_cv_var_tzname,
[AC_LINK_IFELSE([AC_LANG_PROGRAM(
[[#include <time.h>
#ifndef tzname /* For SGI.  */
extern char *tzname[]; /* RS6000 and others reject char **tzname.  */
#endif
]],
[atoi(*tzname);])], ac_cv_var_tzname=yes, ac_cv_var_tzname=no)])
if test $ac_cv_var_tzname = yes; then
    AC_DEFINE(HAVE_TZNAME, 1,
              [Define to 1 if you have the external array `tzname'.])
fi
])# PGAC_STRUCT_TIMEZONE


# PGAC_FUNC_GETTIMEOFDAY_1ARG
# ---------------------------
# Check if gettimeofday() has only one arguments. (Normal is two.)
# If so, define GETTIMEOFDAY_1ARG.
AC_DEFUN([PGAC_FUNC_GETTIMEOFDAY_1ARG],
[AC_CACHE_CHECK(whether gettimeofday takes only one argument,
pgac_cv_func_gettimeofday_1arg,
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <sys/time.h>],
[struct timeval *tp;
struct timezone *tzp;
gettimeofday(tp,tzp);])],
[pgac_cv_func_gettimeofday_1arg=no],
[pgac_cv_func_gettimeofday_1arg=yes])])
if test x"$pgac_cv_func_gettimeofday_1arg" = xyes ; then
  AC_DEFINE(GETTIMEOFDAY_1ARG, 1,
            [Define to 1 if gettimeofday() takes only 1 argument.])
fi
AH_VERBATIM(GETTIMEOFDAY_1ARG_,
[@%:@ifdef GETTIMEOFDAY_1ARG
@%:@ define gettimeofday(a,b) gettimeofday(a)
@%:@endif])dnl
])# PGAC_FUNC_GETTIMEOFDAY_1ARG


# PGAC_FUNC_STRERROR_R_INT
# ---------------------------
# Check if strerror_r() returns an int (SUSv3) rather than a char * (GNU libc)
# If so, define STRERROR_R_INT
AC_DEFUN([PGAC_FUNC_STRERROR_R_INT],
[AC_CACHE_CHECK(whether strerror_r returns int,
pgac_cv_func_strerror_r_int,
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <string.h>],
[#ifndef _AIX
int strerror_r(int, char *, size_t);
#else
/* Older AIX has 'int' for the third argument so we don't test the args. */
int strerror_r();
#endif])],
[pgac_cv_func_strerror_r_int=yes],
[pgac_cv_func_strerror_r_int=no])])
if test x"$pgac_cv_func_strerror_r_int" = xyes ; then
  AC_DEFINE(STRERROR_R_INT, 1,
            [Define to 1 if strerror_r() returns a int.])
fi
])# PGAC_FUNC_STRERROR_R_INT


# PGAC_UNION_SEMUN
# ----------------
# Check if `union semun' exists. Define HAVE_UNION_SEMUN if so.
# If it doesn't then one could define it as
# union semun { int val; struct semid_ds *buf; unsigned short *array; }
AC_DEFUN([PGAC_UNION_SEMUN],
[AC_CHECK_TYPES([union semun], [], [],
[#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>])])# PGAC_UNION_SEMUN


# PGAC_STRUCT_SOCKADDR_UN
# -----------------------
# If `struct sockaddr_un' exists, define HAVE_UNIX_SOCKETS.
# (Requires test for <sys/un.h>!)
AC_DEFUN([PGAC_STRUCT_SOCKADDR_UN],
[AC_CHECK_TYPE([struct sockaddr_un], [AC_DEFINE(HAVE_UNIX_SOCKETS, 1, [Define to 1 if you have unix sockets.])], [],
[#include <sys/types.h>
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
])])# PGAC_STRUCT_SOCKADDR_UN


# PGAC_STRUCT_SOCKADDR_STORAGE
# ----------------------------
# If `struct sockaddr_storage' exists, define HAVE_STRUCT_SOCKADDR_STORAGE.
# If it is missing then one could define it.
AC_DEFUN([PGAC_STRUCT_SOCKADDR_STORAGE],
[AC_CHECK_TYPES([struct sockaddr_storage], [], [],
[#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
])])# PGAC_STRUCT_SOCKADDR_STORAGE

# PGAC_STRUCT_SOCKADDR_STORAGE_MEMBERS
# --------------------------------------
# Check the members of `struct sockaddr_storage'.  We need to know about
# ss_family and ss_len.  (Some platforms follow RFC 2553 and call them
# __ss_family and __ss_len.)  We also check struct sockaddr's sa_len;
# if we have to define our own `struct sockaddr_storage', this tells us
# whether we need to provide an ss_len field.
AC_DEFUN([PGAC_STRUCT_SOCKADDR_STORAGE_MEMBERS],
[AC_CHECK_MEMBERS([struct sockaddr_storage.ss_family,
		   struct sockaddr_storage.__ss_family,
		   struct sockaddr_storage.ss_len,
		   struct sockaddr_storage.__ss_len,
		   struct sockaddr.sa_len], [], [],
[#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
])])# PGAC_STRUCT_SOCKADDR_STORAGE_MEMBERS


# PGAC_STRUCT_ADDRINFO
# -----------------------
# If `struct addrinfo' exists, define HAVE_STRUCT_ADDRINFO.
AC_DEFUN([PGAC_STRUCT_ADDRINFO],
[AC_CHECK_TYPES([struct addrinfo], [], [],
[#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
])])# PGAC_STRUCT_ADDRINFO


# PGAC_FUNC_SNPRINTF_LONG_LONG_INT_MODIFIER
# ---------------------------------------
# Determine which length modifier snprintf uses for long long int.  We
# handle ll, q, and I64.  The result is in shell variable
# LONG_LONG_INT_MODIFIER.
#
# MinGW uses '%I64d', though gcc throws an warning with -Wall,
# while '%lld' doesn't generate a warning, but doesn't work.
#
AC_DEFUN([PGAC_FUNC_SNPRINTF_LONG_LONG_INT_MODIFIER],
[AC_MSG_CHECKING([snprintf length modifier for long long int])
AC_CACHE_VAL(pgac_cv_snprintf_long_long_int_modifier,
[for pgac_modifier in 'll' 'q' 'I64'; do
AC_RUN_IFELSE([AC_LANG_SOURCE([[#include <stdio.h>
typedef long long int ac_int64;
#define INT64_FORMAT "%${pgac_modifier}d"

ac_int64 a = 20000001;
ac_int64 b = 40000005;

int does_int64_snprintf_work()
{
  ac_int64 c;
  char buf[100];

  if (sizeof(ac_int64) != 8)
    return 0;			/* doesn't look like the right size */

  c = a * b;
  snprintf(buf, 100, INT64_FORMAT, c);
  if (strcmp(buf, "800000140000005") != 0)
    return 0;			/* either multiply or snprintf is busted */
  return 1;
}
main() {
  exit(! does_int64_snprintf_work());
}]])],
[pgac_cv_snprintf_long_long_int_modifier=$pgac_modifier; break],
[],
[pgac_cv_snprintf_long_long_int_modifier=cross; break])
done])dnl AC_CACHE_VAL

LONG_LONG_INT_MODIFIER=''

case $pgac_cv_snprintf_long_long_int_modifier in
  cross) AC_MSG_RESULT([cannot test (not on host machine)]);;
  ?*)    AC_MSG_RESULT([$pgac_cv_snprintf_long_long_int_modifier])
         LONG_LONG_INT_MODIFIER=$pgac_cv_snprintf_long_long_int_modifier;;
  *)     AC_MSG_RESULT(none);;
esac])# PGAC_FUNC_SNPRINTF_LONG_LONG_INT_MODIFIER


# PGAC_FUNC_SNPRINTF_ARG_CONTROL
# ---------------------------------------
# Determine if snprintf supports %1$ argument selection, e.g. %5$ selects
# the fifth argument after the printf format string.
# This is not in the C99 standard, but in the Single Unix Specification (SUS).
# It is used in our language translation strings.
#
AC_DEFUN([PGAC_FUNC_SNPRINTF_ARG_CONTROL],
[AC_MSG_CHECKING([whether snprintf supports argument control])
AC_CACHE_VAL(pgac_cv_snprintf_arg_control,
[AC_RUN_IFELSE([AC_LANG_SOURCE([[#include <stdio.h>
#include <string.h>

int main()
{
  char buf[100];

  /* can it swap arguments? */
  snprintf(buf, 100, "%2\$d %1\$d", 3, 4);
  if (strcmp(buf, "4 3") != 0)
    return 1;
  return 0;
}]])],
[pgac_cv_snprintf_arg_control=yes],
[pgac_cv_snprintf_arg_control=no],
[pgac_cv_snprintf_arg_control=cross])
])dnl AC_CACHE_VAL
AC_MSG_RESULT([$pgac_cv_snprintf_arg_control])
])# PGAC_FUNC_SNPRINTF_ARG_CONTROL

# PGAC_FUNC_SNPRINTF_SIZE_T_SUPPORT
# ---------------------------------------
# Determine if snprintf supports the z length modifier for printing
# size_t-sized variables. That's supported by C99 and POSIX but not
# all platforms play ball, so we must test whether it's working.
#
AC_DEFUN([PGAC_FUNC_SNPRINTF_SIZE_T_SUPPORT],
[AC_MSG_CHECKING([whether snprintf supports the %z modifier])
AC_CACHE_VAL(pgac_cv_snprintf_size_t_support,
[AC_RUN_IFELSE([AC_LANG_SOURCE([[#include <stdio.h>
#include <string.h>

int main()
{
  char bufz[100];
  char buf64[100];

  /*
   * Print the largest unsigned number fitting in a size_t using both %zu
   * and the previously-determined format for 64-bit integers.  Note that
   * we don't run this code unless we know snprintf handles 64-bit ints.
   */
  bufz[0] = '\0';  /* in case snprintf fails to emit anything */
  snprintf(bufz, sizeof(bufz), "%zu", ~((size_t) 0));
  snprintf(buf64, sizeof(buf64), "%" INT64_MODIFIER "u",
    (unsigned PG_INT64_TYPE) ~((size_t) 0));
  if (strcmp(bufz, buf64) != 0)
    return 1;
  return 0;
}]])],
[pgac_cv_snprintf_size_t_support=yes],
[pgac_cv_snprintf_size_t_support=no],
[pgac_cv_snprintf_size_t_support=cross])
])dnl AC_CACHE_VAL
AC_MSG_RESULT([$pgac_cv_snprintf_size_t_support])
])# PGAC_FUNC_SNPRINTF_SIZE_T_SUPPORT


# PGAC_TYPE_LOCALE_T
# ------------------
# Check for the locale_t type and find the right header file.  Mac OS
# X needs xlocale.h; standard is locale.h, but glibc also has an
# xlocale.h file that we should not use.
#
AC_DEFUN([PGAC_TYPE_LOCALE_T],
[AC_CACHE_CHECK([for locale_t], pgac_cv_type_locale_t,
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[#include <locale.h>
locale_t x;],
[])],
[pgac_cv_type_locale_t=yes],
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[#include <xlocale.h>
locale_t x;],
[])],
[pgac_cv_type_locale_t='yes (in xlocale.h)'],
[pgac_cv_type_locale_t=no])])])
if test "$pgac_cv_type_locale_t" != no; then
  AC_DEFINE(HAVE_LOCALE_T, 1,
            [Define to 1 if the system has the type `locale_t'.])
fi
if test "$pgac_cv_type_locale_t" = 'yes (in xlocale.h)'; then
  AC_DEFINE(LOCALE_T_IN_XLOCALE, 1,
            [Define to 1 if `locale_t' requires <xlocale.h>.])
fi])# PGAC_TYPE_LOCALE_T


# PGAC_FUNC_WCSTOMBS_L
# --------------------
# Try to find a declaration for wcstombs_l().  It might be in stdlib.h
# (following the POSIX requirement for wcstombs()), or in locale.h, or in
# xlocale.h.  If it's in the latter, define WCSTOMBS_L_IN_XLOCALE.
#
AC_DEFUN([PGAC_FUNC_WCSTOMBS_L],
[AC_CACHE_CHECK([for wcstombs_l declaration], pgac_cv_func_wcstombs_l,
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[#include <stdlib.h>
#include <locale.h>],
[#ifndef wcstombs_l
(void) wcstombs_l;
#endif])],
[pgac_cv_func_wcstombs_l='yes'],
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[#include <stdlib.h>
#include <locale.h>
#include <xlocale.h>],
[#ifndef wcstombs_l
(void) wcstombs_l;
#endif])],
[pgac_cv_func_wcstombs_l='yes (in xlocale.h)'],
[pgac_cv_func_wcstombs_l='no'])])])
if test "$pgac_cv_func_wcstombs_l" = 'yes (in xlocale.h)'; then
  AC_DEFINE(WCSTOMBS_L_IN_XLOCALE, 1,
            [Define to 1 if `wcstombs_l' requires <xlocale.h>.])
fi])# PGAC_FUNC_WCSTOMBS_L
