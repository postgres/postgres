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
#include <sys/ipc.h>
#include <sys/sem.h>
])])# PGAC_UNION_SEMUN


# PGAC_STRUCT_SOCKADDR_MEMBERS
# ----------------------------
# Check if struct sockaddr and subtypes have 4.4BSD-style length.
AC_DEFUN([PGAC_STRUCT_SOCKADDR_SA_LEN],
[AC_CHECK_MEMBERS([struct sockaddr.sa_len], [], [],
[#include <sys/types.h>
#include <sys/socket.h>
])])# PGAC_STRUCT_SOCKADDR_MEMBERS
