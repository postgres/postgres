# Macros to detect C compiler features
# $Header: /cvsroot/pgsql/config/c-compiler.m4,v 1.1 2000/06/11 11:39:46 petere Exp $


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
