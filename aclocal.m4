dnl aclocal.m4 generated automatically by aclocal 1.4

dnl Copyright (C) 1994, 1995-8, 1999 Free Software Foundation, Inc.
dnl This file is free software; the Free Software Foundation
dnl gives unlimited permission to copy and/or distribute it,
dnl with or without modifications, as long as this notice is preserved.

dnl This program is distributed in the hope that it will be useful,
dnl but WITHOUT ANY WARRANTY, to the extent permitted by law; without
dnl even the implied warranty of MERCHANTABILITY or FITNESS FOR A
dnl PARTICULAR PURPOSE.

#
# Autoconf macros for configuring the build of Python extension modules
#
# $Header: /cvsroot/pgsql/aclocal.m4,v 1.2 2000/06/11 11:39:45 petere Exp $
#

# PGAC_PROG_PYTHON
# ----------------
# Look for Python and set the output variable `PYTHON'
# to `python' if found, empty otherwise.
AC_DEFUN([PGAC_PROG_PYTHON],
[AC_CHECK_PROG(PYTHON, python, python)])


# PGAC_PATH_PYTHONDIR
# -------------------
# Finds the names of various install dirs and helper files
# necessary to build a Python extension module.
#
# It would be nice if we could check whether the current setup allows
# the build of the shared module. Future project.
AC_DEFUN([PGAC_PATH_PYTHONDIR],
[AC_REQUIRE([PGAC_PROG_PYTHON])
[if test "${PYTHON+set}" = set ; then
  python_version=`${PYTHON} -c "import sys; print sys.version[:3]"`
  python_prefix=`${PYTHON} -c "import sys; print sys.prefix"`
  python_execprefix=`${PYTHON} -c "import sys; print sys.exec_prefix"`
  python_configdir="${python_execprefix}/lib/python${python_version}/config"
  python_moduledir="${python_prefix}/lib/python${python_version}"
  python_extmakefile="${python_configdir}/Makefile.pre.in"]

  AC_MSG_CHECKING(for Python extension makefile)
  if test -f "${python_extmakefile}" ; then
    AC_MSG_RESULT(found)
  else
    AC_MSG_RESULT(no)
    AC_MSG_ERROR(
[The Python extension makefile was expected at \`${python_extmakefile}\'
but does not exist. This means the Python module cannot be built automatically.])
  fi

  AC_SUBST(python_version)
  AC_SUBST(python_prefix)
  AC_SUBST(python_execprefix)
  AC_SUBST(python_configdir)
  AC_SUBST(python_moduledir)
  AC_SUBST(python_extmakefile)
else
  AC_MSG_ERROR([Python not found])
fi])# PGAC_PATH_PYTHONDIR

# Macros to detect certain C++ features
# $Header: /cvsroot/pgsql/aclocal.m4,v 1.2 2000/06/11 11:39:45 petere Exp $


# PGAC_CLASS_STRING
# -----------------
# Look for class `string'. First look for the <string> header. If this
# is found a <string> header then it's probably safe to assume that
# class string exists.  If not, check to make sure that <string.h>
# defines class `string'.
AC_DEFUN([PGAC_CLASS_STRING],
[AC_LANG_SAVE
AC_LANG_CPLUSPLUS
AC_CHECK_HEADER(string,
  [AC_DEFINE(HAVE_CXX_STRING_HEADER)])

if test x"$ac_cv_header_string" != xyes ; then
  AC_CACHE_CHECK([for class string in <string.h>],
    [pgac_cv_class_string_in_string_h],
    [AC_TRY_COMPILE([#include <stdio.h>
#include <stdlib.h>
#include <string.h>
],
      [string foo = "test"],
      [pgac_cv_class_string_in_string_h=yes],
      [pgac_cv_class_string_in_string_h=no])])

  if test x"$pgac_cv_class_string_in_string_h" != xyes ; then
    AC_MSG_ERROR([neither <string> nor <string.h> seem to define the C++ class \`string\'])
  fi
fi
AC_LANG_RESTORE])# PGAC_CLASS_STRING


# PGAC_CXX_NAMESPACE_STD
# ----------------------
# Check whether the C++ compiler understands `using namespace std'.
#
# Note 1: On at least some compilers, it will not work until you've
# included a header that mentions namespace std. Thus, include the
# usual suspects before trying it.
#
# Note 2: This test does not actually reveal whether the C++ compiler
# properly understands namespaces in all generality. (GNU C++ 2.8.1
# is one that doesn't.) However, we don't care.
AC_DEFUN([PGAC_CXX_NAMESPACE_STD],
[AC_REQUIRE([PGAC_CLASS_STRING])
AC_CACHE_CHECK([for namespace std in C++],
pgac_cv_cxx_namespace_std,
[
AC_LANG_SAVE
AC_LANG_CPLUSPLUS
AC_TRY_COMPILE(
[#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_CXX_STRING_HEADER
#include <string>
#endif
using namespace std;
], [],
[pgac_cv_cxx_namespace_std=yes],
[pgac_cv_cxx_namespace_std=no])
AC_LANG_RESTORE])

if test $pgac_cv_cxx_namespace_std = yes ; then
    AC_DEFINE(HAVE_NAMESPACE_STD, 1, [Define to 1 if the C++ compiler understands `using namespace std'])
fi])# PGAC_CXX_NAMESPACE_STD

dnl AM_MISSING_PROG(NAME, PROGRAM, DIRECTORY)
dnl The program must properly implement --version.
AC_DEFUN(AM_MISSING_PROG,
[AC_MSG_CHECKING(for working $2)
# Run test in a subshell; some versions of sh will print an error if
# an executable is not found, even if stderr is redirected.
# Redirect stdin to placate older versions of autoconf.  Sigh.
if ($2 --version) < /dev/null > /dev/null 2>&1; then
   $1=$2
   AC_MSG_RESULT(found)
else
   $1="$3/missing $2"
   AC_MSG_RESULT(missing)
fi
AC_SUBST($1)])

# Macros to detect C compiler features
# $Header: /cvsroot/pgsql/aclocal.m4,v 1.2 2000/06/11 11:39:45 petere Exp $


# PGAC_C_SIGNED
# -------------
# Check if the C compiler understands signed types.
# (Of course any ISO C compiler should, what is this still doing here?)
AC_DEFUN([PGAC_C_SIGNED],
[AC_CACHE_CHECK(for signed types, pgac_cv_c_signed,
[AC_TRY_COMPILE([],
[signed char c; signed short s; signed int i;],
[pgac_cv_c_signed=yes],
[pgac_cv_c_signed=no])])
if test x"$pgac_cv_c_signed" = xno ; then
  AC_DEFINE(signed,, [Define empty if the C compiler does not understand signed types])
fi])# PGAC_C_SIGNED



# PGAC_C_VOLATILE
# ---------------
# Check if the C compiler understands `volatile'. Note that if it doesn't
# then this will potentially break the program semantics.
AC_DEFUN([PGAC_C_VOLATILE],
[AC_CACHE_CHECK(for volatile, pgac_cv_c_volatile,
[AC_TRY_COMPILE([],
[extern volatile int i;],
[pgac_cv_c_volatile=yes],
[pgac_cv_c_volatile=no])])
if test x"$pgac_cv_c_volatile" = xno ; then
  AC_DEFINE(volatile,, [Define empty if the C compiler does not understand `volatile'])
fi])# PGAC_C_VOLATILE



# PGAC_TYPE_64BIT_INT(TYPE)
# -------------------------
# Check if TYPE is a working 64 bit integer type. Set HAVE_TYPE_64 to
# yes or no respectively, and define HAVE_TYPE_64 if yes.
AC_DEFUN([PGAC_TYPE_64BIT_INT],
[define([Ac_define], [translit([have_$1_64], [a-z *], [A-Z_P])])dnl
define([Ac_cachevar], [translit([pgac_cv_type_$1_64], [ *], [_p])])dnl
AC_CACHE_CHECK([whether $1 is 64 bits], [Ac_cachevar],
[AC_TRY_RUN(
[typedef $1 int64;

/*
 * These are globals to discourage the compiler from folding all the
 * arithmetic tests down to compile-time constants.
 */
int64 a = 20000001;
int64 b = 40000005;

int does_int64_work()
{
  int64 c,d;

  if (sizeof(int64) != 8)
    return 0;			/* definitely not the right size */

  /* Do perfunctory checks to see if 64-bit arithmetic seems to work */
  c = a * b;
  d = (c + b) / b;
  if (d != a+1)
    return 0;
  return 1;
}
main() {
  exit(! does_int64_work());
}],
[Ac_cachevar=yes],
[Ac_cachevar=no],
[Ac_cachevar=no
dnl We will do better here with Autoconf 2.50
AC_MSG_WARN([64 bit arithmetic disabled when cross-compiling])])])

Ac_define=$Ac_cachevar
if test x"$Ac_cachevar" = xyes ; then
  AC_DEFINE(Ac_define,, [Set to 1 if `]$1[' is 64 bits])
fi
undefine([Ac_define])dnl
undefine([Ac_cachevar])dnl
])# PGAC_TYPE_64BIT_INT



# PGAC_CHECK_ALIGNOF(TYPE)
# ------------------------
# Find the alignment requirement of the given type. Define the result
# as ALIGNOF_TYPE. If cross-compiling, sizeof(type) is used as a
# default assumption.
#
# This is modeled on the standard autoconf macro AC_CHECK_SIZEOF.
# That macro never got any points for style.
AC_DEFUN([PGAC_CHECK_ALIGNOF],
[changequote(<<, >>)dnl
dnl The name to #define.
define(<<AC_TYPE_NAME>>, translit(alignof_$1, [a-z *], [A-Z_P]))dnl
dnl The cache variable name.
define(<<AC_CV_NAME>>, translit(pgac_cv_alignof_$1, [ *], [_p]))dnl
changequote([, ])dnl
AC_MSG_CHECKING(alignment of $1)
AC_CACHE_VAL(AC_CV_NAME,
[AC_TRY_RUN([#include <stdio.h>
struct { char filler; $1 field; } mystruct;
main()
{
  FILE *f=fopen("conftestval", "w");
  if (!f) exit(1);
  fprintf(f, "%d\n", ((char*) & mystruct.field) - ((char*) & mystruct));
  exit(0);
}], AC_CV_NAME=`cat conftestval`,
AC_CV_NAME='sizeof($1)',
AC_CV_NAME='sizeof($1)')])dnl
AC_MSG_RESULT($AC_CV_NAME)
AC_DEFINE_UNQUOTED(AC_TYPE_NAME, $AC_CV_NAME, [The alignment requirement of a `]$1['])
undefine([AC_TYPE_NAME])dnl
undefine([AC_CV_NAME])dnl
])# PGAC_CHECK_ALIGNOF

# $Header: /cvsroot/pgsql/aclocal.m4,v 1.2 2000/06/11 11:39:45 petere Exp $
# This comes from the official Autoconf macro archive at
# <http://research.cys.de/autoconf-archive/>
# (I removed the $ before the Id CVS keyword below.)


dnl @synopsis AC_FUNC_ACCEPT_ARGTYPES
dnl
dnl Checks the data types of the three arguments to accept(). Results are
dnl placed into the symbols ACCEPT_TYPE_ARG[123], consistent with the
dnl following example:
dnl
dnl       #define ACCEPT_TYPE_ARG1 int
dnl       #define ACCEPT_TYPE_ARG2 struct sockaddr *
dnl       #define ACCEPT_TYPE_ARG3 socklen_t
dnl
dnl This macro requires AC_CHECK_HEADERS to have already verified the
dnl presence or absence of sys/types.h and sys/socket.h.
dnl
dnl NOTE: This is just a modified version of the AC_FUNC_SELECT_ARGTYPES
dnl macro. Credit for that one goes to David MacKenzie et. al.
dnl
dnl @version Id: ac_func_accept_argtypes.m4,v 1.1 1999/12/03 11:29:29 simons Exp $
dnl @author Daniel Richard G. <skunk@mit.edu>
dnl

# PostgreSQL local changes: In the original version ACCEPT_TYPE_ARG3
# is a pointer type. That's kind of useless because then you can't
# use the macro to define a corresponding variable. We also make the
# reasonable(?) assumption that you can use arg3 for getsocktype etc.
# as well (i.e., anywhere POSIX.2 has socklen_t).

AC_DEFUN(AC_FUNC_ACCEPT_ARGTYPES,
[AC_MSG_CHECKING([types of arguments for accept()])
 AC_CACHE_VAL(ac_cv_func_accept_arg1,dnl
 [AC_CACHE_VAL(ac_cv_func_accept_arg2,dnl
  [AC_CACHE_VAL(ac_cv_func_accept_arg3,dnl
   [for ac_cv_func_accept_arg1 in 'int' 'unsigned int'; do
     for ac_cv_func_accept_arg2 in 'struct sockaddr *' 'void *'; do
      for ac_cv_func_accept_arg3 in 'socklen_t' 'size_t' 'unsigned int' 'int'; do
       AC_TRY_COMPILE(dnl
[#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
extern accept ($ac_cv_func_accept_arg1, $ac_cv_func_accept_arg2, $ac_cv_func_accept_arg3 *);],,dnl
        [ac_not_found=no ; break 3], ac_not_found=yes)
      done
     done
    done
   ])dnl AC_CACHE_VAL
  ])dnl AC_CACHE_VAL
 ])dnl AC_CACHE_VAL
 if test "$ac_not_found" = yes; then
  ac_cv_func_accept_arg1=int
  ac_cv_func_accept_arg2='struct sockaddr *'
  ac_cv_func_accept_arg3='socklen_t'
 fi
 AC_MSG_RESULT([$ac_cv_func_accept_arg1, $ac_cv_func_accept_arg2, $ac_cv_func_accept_arg3 *])
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_ARG1,$ac_cv_func_accept_arg1)
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_ARG2,$ac_cv_func_accept_arg2)
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_ARG3,$ac_cv_func_accept_arg3)
])

# Macros that test various C library quirks
# $Header: /cvsroot/pgsql/aclocal.m4,v 1.2 2000/06/11 11:39:45 petere Exp $


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
  AC_DEFINE(HAVE_UNION_SEMUN,, [Set to 1 if you have `union semun'])
fi])# PGAC_UNION_SEMUN


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

