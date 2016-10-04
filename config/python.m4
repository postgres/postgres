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
if test "$PORTNAME" = win32 ; then
    python_includespec=`echo $python_includespec | sed 's,[[\]],/,g'`
fi
AC_MSG_RESULT([$python_includespec])

AC_SUBST(python_majorversion)[]dnl
AC_SUBST(python_version)[]dnl
AC_SUBST(python_includespec)[]dnl
])# _PGAC_CHECK_PYTHON_DIRS


# PGAC_CHECK_PYTHON_EMBED_SETUP
# -----------------------------
#
# Set python_libdir to the path of the directory containing the Python shared
# library.  Set python_libspec to the -L/-l linker switches needed to link it.
# Set python_additional_libs to contain any additional linker switches needed
# for subsidiary libraries.
#
# In modern, well-configured Python installations, LIBDIR gives the correct
# directory name and LDLIBRARY is the file name of the shlib.  But in older
# installations LDLIBRARY is frequently a useless path fragment, and it's also
# possible that the shlib is in a standard library directory such as /usr/lib
# so that LIBDIR is of no interest.  We must also check that what we found is
# a shared library not a plain library, which we do by checking its extension.
# (We used to rely on Py_ENABLE_SHARED, but that only tells us that a shlib
# exists, not that we found it.)
AC_DEFUN([PGAC_CHECK_PYTHON_EMBED_SETUP],
[AC_REQUIRE([_PGAC_CHECK_PYTHON_DIRS])
AC_MSG_CHECKING([how to link an embedded Python application])

python_libdir=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LIBDIR'))))"`
python_ldlibrary=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LDLIBRARY'))))"`

# If LDLIBRARY exists and has a shlib extension, use it verbatim.
ldlibrary=`echo "${python_ldlibrary}" | sed -e 's/\.so$//' -e 's/\.dll$//' -e 's/\.dylib$//' -e 's/\.sl$//'`
if test -e "${python_libdir}/${python_ldlibrary}" -a x"${python_ldlibrary}" != x"${ldlibrary}"
then
	ldlibrary=`echo "${ldlibrary}" | sed "s/^lib//"`
else
	# Otherwise, guess the base name of the shlib.
	# LDVERSION was added in Python 3.2, before that use $python_version.
	python_ldversion=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LDVERSION'))))"`
	if test x"${python_ldversion}" != x""; then
		ldlibrary="python${python_ldversion}"
	else
		ldlibrary="python${python_version}"
	fi
	# Search for a likely-looking file.
	found_shlib=0
	for d in "${python_libdir}" /usr/lib64 /usr/lib; do
		for e in .so .dll .dylib .sl; do
			if test -e "$d/lib${ldlibrary}$e"; then
				python_libdir="$d"
				found_shlib=1
				break 2
			fi
		done
	done
	if test "$found_shlib" != 1; then
		AC_MSG_ERROR([could not find shared library for Python
You might have to rebuild your Python installation.  Refer to the
documentation for details.  Use --without-python to disable building
PL/Python.])
	fi
fi
python_libspec="-L${python_libdir} -l${ldlibrary}"

python_additional_libs=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LIBS','LIBC','LIBM','BASEMODLIBS'))))"`

AC_MSG_RESULT([${python_libspec} ${python_additional_libs}])

AC_SUBST(python_libdir)[]dnl
AC_SUBST(python_libspec)[]dnl
AC_SUBST(python_additional_libs)[]dnl

])# PGAC_CHECK_PYTHON_EMBED_SETUP
