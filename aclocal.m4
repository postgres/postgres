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
# $Header: /cvsroot/pgsql/aclocal.m4,v 1.1 2000/06/10 18:01:34 petere Exp $
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

