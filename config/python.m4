#
# Autoconf macros for configuring the build of Python extension modules
#
# config/python.m4
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
# Determine the name of various directories of a given Python installation.
AC_DEFUN([_PGAC_CHECK_PYTHON_DIRS],
[AC_REQUIRE([PGAC_PATH_PYTHON])
AC_MSG_CHECKING([for Python distutils module])
if "${PYTHON}" -c 'import distutils' 2>&AS_MESSAGE_LOG_FD
then
    AC_MSG_RESULT(yes)
else
    AC_MSG_RESULT(no)
    AC_MSG_ERROR([distutils module not found])
fi
AC_MSG_CHECKING([Python configuration directory])
python_majorversion=`${PYTHON} -c "import sys; print(sys.version[[0]])"`
python_version=`${PYTHON} -c "import sys; print(sys.version[[:3]])"`
python_configdir=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LIBPL'))))"`
AC_MSG_RESULT([$python_configdir])

AC_MSG_CHECKING([Python include directories])
python_includespec=`${PYTHON} -c "
import distutils.sysconfig
a = '-I' + distutils.sysconfig.get_python_inc(False)
b = '-I' + distutils.sysconfig.get_python_inc(True)
if a == b:
    print(a)
else:
    print(a + ' ' + b)"`
AC_MSG_RESULT([$python_includespec])

AC_SUBST(python_majorversion)[]dnl
AC_SUBST(python_version)[]dnl
AC_SUBST(python_includespec)[]dnl
])# _PGAC_CHECK_PYTHON_DIRS


# PGAC_CHECK_PYTHON_EMBED_SETUP
# -----------------------------
#
# Note: selecting libpython from python_configdir works in all Python
# releases, but it generally finds a non-shared library, which means
# that we are binding the python interpreter right into libplpython.so.
# In Python 2.3 and up there should be a shared library available in
# the main library location.
AC_DEFUN([PGAC_CHECK_PYTHON_EMBED_SETUP],
[AC_REQUIRE([_PGAC_CHECK_PYTHON_DIRS])
AC_MSG_CHECKING([how to link an embedded Python application])

python_libdir=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LIBDIR'))))"`
python_ldlibrary=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LDLIBRARY'))))"`
python_so=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('SO'))))"`
ldlibrary=`echo "${python_ldlibrary}" | sed "s/${python_so}$//"`
python_framework=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('PYTHONFRAMEWORK'))))"`
python_enable_shared=`${PYTHON} -c "import distutils.sysconfig; print(distutils.sysconfig.get_config_vars().get('Py_ENABLE_SHARED',0))"`

if test -n "$python_framework"; then
	python_frameworkprefix=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('PYTHONFRAMEWORKPREFIX'))))"`
	python_libspec="-F${python_frameworkprefix} -framework $python_framework"
	python_enable_shared=1
elif test x"${python_libdir}" != x"" -a x"${python_ldlibrary}" != x"" -a x"${python_ldlibrary}" != x"${ldlibrary}"
then
	# New way: use the official shared library
	ldlibrary=`echo "${ldlibrary}" | sed "s/^lib//"`
	python_libspec="-L${python_libdir} -l${ldlibrary}"
else
	# Old way: use libpython from python_configdir
	python_libdir="${python_configdir}"
	# LDVERSION was introduced in Python 3.2.
	python_ldversion=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LDVERSION'))))"`
	if test x"${python_ldversion}" = x""; then
		python_ldversion=$python_version
	fi
	python_libspec="-L${python_libdir} -lpython${python_ldversion}"
fi

if test -z "$python_framework"; then
	python_additional_libs=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LIBS','LIBC','LIBM','BASEMODLIBS'))))"`
fi

AC_MSG_RESULT([${python_libspec} ${python_additional_libs}])

AC_SUBST(python_libdir)[]dnl
AC_SUBST(python_libspec)[]dnl
AC_SUBST(python_additional_libs)[]dnl
AC_SUBST(python_enable_shared)[]dnl

# threaded python is not supported on OpenBSD
AC_MSG_CHECKING(whether Python is compiled with thread support)
pythreads=`${PYTHON} -c "import sys; print(int('thread' in sys.builtin_module_names))"`
if test "$pythreads" = "1"; then
  AC_MSG_RESULT(yes)
  case $host_os in
  openbsd*)
    AC_MSG_ERROR([threaded Python not supported on this platform])
    ;;
  esac
else
  AC_MSG_RESULT(no)
fi

])# PGAC_CHECK_PYTHON_EMBED_SETUP
