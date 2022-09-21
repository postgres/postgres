# config/check_decls.m4

# This file redefines the standard Autoconf macro AC_CHECK_DECL,
# and adds a supporting function _AC_UNDECLARED_WARNING, to make
# AC_CHECK_DECLS behave correctly when checking for built-in library
# functions with clang.

# This is based on commit 82ef7805faffa151e724aa76c245ec590d174580
# in the Autoconf git repository, adapted to work with Autoconf 2.63.
# It's still mostly their code, so
# it's distributed under Autoconf's license:

# This file is part of Autoconf.  This program is free
# software; you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the
# Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# Under Section 7 of GPL version 3, you are granted additional
# permissions described in the Autoconf Configure Script Exception,
# version 3.0, as published by the Free Software Foundation.
#
# You should have received a copy of the GNU General Public License
# and a copy of the Autoconf Configure Script Exception along with
# this program; see the files COPYINGv3 and COPYING.EXCEPTION
# respectively.  If not, see <http://www.gnu.org/licenses/>.

# Written by David MacKenzie, with help from
# Franc,ois Pinard, Karl Berry, Richard Pixley, Ian Lance Taylor,
# Roland McGrath, Noah Friedman, david d zuhn, and many others.


# _AC_UNDECLARED_WARNING
# ----------------------
# Set ac_[]_AC_LANG_ABBREV[]_decl_warn_flag=yes if the compiler uses a warning,
# not a more-customary error, to report some undeclared identifiers.  Fail when
# an affected compiler warns also on valid input.  _AC_PROG_PREPROC_WORKS_IFELSE
# solves a related problem.
AC_DEFUN([_AC_UNDECLARED_WARNING],
[# The Clang compiler raises a warning for an undeclared identifier that matches
# a compiler builtin function.  All extant Clang versions are affected, as of
# Clang 3.6.0.  Test a builtin known to every version.  This problem affects the
# C and Objective C languages, but Clang does report an error under C++ and
# Objective C++.
#
# Passing -fno-builtin to the compiler would suppress this problem.  That
# strategy would have the advantage of being insensitive to stray warnings, but
# it would make tests less realistic.
AC_CACHE_CHECK([how $CC reports undeclared, standard C functions],
[ac_cv_[]_AC_LANG_ABBREV[]_decl_report],
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [(void) strchr;])],
  [AS_IF([test -s conftest.err], [dnl
    # For AC_CHECK_DECL to react to warnings, the compiler must be silent on
    # valid AC_CHECK_DECL input.  No library function is consistently available
    # on freestanding implementations, so test against a dummy declaration.
    # Include always-available headers on the off chance that they somehow
    # elicit warnings.
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([dnl
#include <float.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
extern void ac_decl (int, char *);],
[@%:@ifdef __cplusplus
  (void) ac_decl ((int) 0, (char *) 0);
  (void) ac_decl;
@%:@else
  (void) ac_decl;
@%:@endif
])],
      [AS_IF([test -s conftest.err],
	[AC_MSG_FAILURE([cannot detect from compiler exit status or warnings])],
	[ac_cv_[]_AC_LANG_ABBREV[]_decl_report=warning])],
      [AC_MSG_FAILURE([cannot compile a simple declaration test])])],
    [AC_MSG_FAILURE([compiler does not report undeclared identifiers])])],
  [ac_cv_[]_AC_LANG_ABBREV[]_decl_report=error])])

case $ac_cv_[]_AC_LANG_ABBREV[]_decl_report in
  warning) ac_[]_AC_LANG_ABBREV[]_decl_warn_flag=yes ;;
  *) ac_[]_AC_LANG_ABBREV[]_decl_warn_flag= ;;
esac
])# _AC_UNDECLARED_WARNING

# AC_CHECK_DECL
# -------------
# Replace Autoconf 2.63's standard definition of AC_CHECK_DECL,
# setting it up to treat warnings as errors if necessary.
m4_define([AC_CHECK_DECL],
[dnl Initialize each $ac_[]_AC_LANG_ABBREV[]_decl_warn_flag once.
AC_DEFUN([_AC_UNDECLARED_WARNING_]_AC_LANG_ABBREV,
         [_AC_UNDECLARED_WARNING])dnl
AC_REQUIRE([_AC_UNDECLARED_WARNING_]_AC_LANG_ABBREV)dnl
AS_VAR_PUSHDEF([ac_Symbol], [ac_cv_have_decl_$1])dnl
AC_CACHE_CHECK([whether $1 is declared], [ac_Symbol],
[ac_save_werror_flag=$ac_[]_AC_LANG_ABBREV[]_werror_flag
ac_[]_AC_LANG_ABBREV[]_werror_flag="$ac_[]_AC_LANG_ABBREV[]_decl_warn_flag$ac_[]_AC_LANG_ABBREV[]_werror_flag"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([AC_INCLUDES_DEFAULT([$4])],
[#ifndef $1
  (void) $1;
#endif
])],
		   [AS_VAR_SET([ac_Symbol], [yes])],
		   [AS_VAR_SET([ac_Symbol], [no])])
ac_[]_AC_LANG_ABBREV[]_werror_flag=$ac_save_werror_flag])
AS_VAR_IF([ac_Symbol], [yes], [$2], [$3])[]dnl
AS_VAR_POPDEF([ac_Symbol])dnl
])# AC_CHECK_DECL
