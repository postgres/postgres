#
# Autoconf macros for configuring the build of Python extension modules
#
# $Header: /cvsroot/pgsql/config/python.m4,v 1.6 2003/09/01 23:01:48 petere Exp $
#

# PGAC_PATH_PYTHON
# ----------------
# Look for Python and set the output variable 'PYTHON'
# to 'python' if found, empty otherwise.
AC_DEFUN([PGAC_PATH_PYTHON],
[AC_PATH_PROG(PYTHON, python)
if test x"$PYTHON" = x""; then
  AC_MSG_ERROR([Python not found])
fi
])


# _PGAC_CHECK_PYTHON_DIRS
# -----------------------
# Determine the name of various directory of a given Python installation.
AC_DEFUN([_PGAC_CHECK_PYTHON_DIRS],
[AC_REQUIRE([PGAC_PATH_PYTHON])
AC_MSG_CHECKING([Python installation directories])
python_version=`${PYTHON} -c "import sys; print sys.version[[:3]]"`
python_prefix=`${PYTHON} -c "import sys; print sys.prefix"`
python_execprefix=`${PYTHON} -c "import sys; print sys.exec_prefix"`
python_configdir="${python_execprefix}/lib/python${python_version}/config"
python_includespec="-I${python_prefix}/include/python${python_version}"
if test "$python_prefix" != "$python_execprefix"; then
  python_includespec="-I${python_execprefix}/include/python${python_version} $python_includespec"
fi

AC_SUBST(python_prefix)[]dnl
AC_SUBST(python_execprefix)[]dnl
AC_SUBST(python_configdir)[]dnl
AC_SUBST(python_includespec)[]dnl
# This should be enough of a message.
if test "$python_prefix" != "$python_execprefix"; then
  AC_MSG_RESULT([$python_prefix/lib/python${python_version} and $python_execprefix/lib/python${python_version}])
else
  AC_MSG_RESULT([$python_prefix/lib/python${python_version}])
fi
])# _PGAC_CHECK_PYTHON_DIRS


# PGAC_CHECK_PYTHON_EMBED_SETUP
# -----------------------------
# Courtesy of the INN 2.3.1 package...
AC_DEFUN([PGAC_CHECK_PYTHON_EMBED_SETUP],
[AC_REQUIRE([_PGAC_CHECK_PYTHON_DIRS])
AC_MSG_CHECKING([how to link an embedded Python application])

if test ! -f "$python_configdir/Makefile"; then
  AC_MSG_RESULT(no)
  AC_MSG_ERROR([Python Makefile not found])
fi

_python_libs=`grep '^LIBS=' $python_configdir/Makefile | sed 's/^.*=//'`
_python_libc=`grep '^LIBC=' $python_configdir/Makefile | sed 's/^.*=//'`
_python_libm=`grep '^LIBM=' $python_configdir/Makefile | sed 's/^.*=//'`
_python_liblocalmod=`grep '^LOCALMODLIBS=' $python_configdir/Makefile | sed 's/^.*=//'`
_python_libbasemod=`grep '^BASEMODLIBS=' $python_configdir/Makefile | sed 's/^.*=//'`

pgac_tab="	" # tab character
python_libspec=`echo X"-L$python_configdir $_python_libs $_python_libc $_python_libm -lpython$python_version $_python_liblocalmod $_python_libbasemod" | sed -e 's/^X//' -e "s/[[ $pgac_tab]][[ $pgac_tab]]*/ /g"`

AC_MSG_RESULT([${python_libspec}])

AC_SUBST(python_libspec)[]dnl
])# PGAC_CHECK_PYTHON_EMBED_SETUP
