# Macros to detect certain C++ features
# $Header: /cvsroot/pgsql/config/Attic/cxx.m4,v 1.1 2000/06/11 11:39:46 petere Exp $


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
