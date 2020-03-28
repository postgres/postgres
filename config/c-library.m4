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
# `tm_zone' member, define `HAVE_STRUCT_TM_TM_ZONE'.  Unlike the
# standard macro AC_STRUCT_TIMEZONE, we don't check for `tzname[]' if
# not found, since we don't use it.  (We use `int timezone' as a
# fallback.)
AC_DEFUN([PGAC_STRUCT_TIMEZONE],
[AC_CHECK_MEMBERS([struct tm.tm_zone],,,[#include <sys/types.h>
#include <time.h>
])
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
# Check if strerror_r() returns int (POSIX) rather than char * (GNU libc).
# If so, define STRERROR_R_INT.
# The result is uncertain if strerror_r() isn't provided,
# but we don't much care.
AC_DEFUN([PGAC_FUNC_STRERROR_R_INT],
[AC_CACHE_CHECK(whether strerror_r returns int,
pgac_cv_func_strerror_r_int,
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <string.h>],
[[char buf[100];
  switch (strerror_r(1, buf, sizeof(buf)))
  { case 0: break; default: break; }
]])],
[pgac_cv_func_strerror_r_int=yes],
[pgac_cv_func_strerror_r_int=no])])
if test x"$pgac_cv_func_strerror_r_int" = xyes ; then
  AC_DEFINE(STRERROR_R_INT, 1,
            [Define to 1 if strerror_r() returns int.])
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
#ifdef HAVE_SYS_IPC_H
#include <sys/ipc.h>
#endif
#ifdef HAVE_SYS_SEM_H
#include <sys/sem.h>
#endif])])# PGAC_UNION_SEMUN


# PGAC_STRUCT_SOCKADDR_UN
# -----------------------
# If `struct sockaddr_un' exists, define HAVE_STRUCT_SOCKADDR_UN.
# If it is missing then one could define it.
# (Requires test for <sys/un.h>!)
AC_DEFUN([PGAC_STRUCT_SOCKADDR_UN],
[AC_CHECK_TYPES([struct sockaddr_un], [], [],
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
#include <sys/socket.h>
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
#include <sys/socket.h>
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


# PGAC_TYPE_LOCALE_T
# ------------------
# Check for the locale_t type and find the right header file.  macOS
# needs xlocale.h; standard is locale.h, but glibc also has an
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
