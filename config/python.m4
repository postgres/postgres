#
# Autoconf macros for configuring the build of Python extension modules
#
# config/python.m4
#

# PGAC_PATH_PYTHON
# ----------------
# Look for Python and set the output variable 'PYTHON' if found,
# fail otherwise.
#
# As the Python 3 transition happens and PEP 394 isn't updated, we
# need to cater to systems that don't have unversioned "python" by
# default.  Some systems ship with "python3" by default and perhaps
# have "python" in an optional package.  Some systems only have
# "python2" and "python3", in which case it's reasonable to prefer the
# newer version.
AC_DEFUN([PGAC_PATH_PYTHON],
[PGAC_PATH_PROGS(PYTHON, [python python3 python2])
AC_ARG_VAR(PYTHON, [Python program])dnl
if test x"$PYTHON" = x""; then
  AC_MSG_ERROR([Python not found])
fi
])


# _PGAC_CHECK_PYTHON_DIRS
# -----------------------
# Determine the name of various directories of a given Python installation,
# as well as the Python version.
AC_DEFUN([_PGAC_CHECK_PYTHON_DIRS],
[AC_REQUIRE([PGAC_PATH_PYTHON])
python_fullversion=`${PYTHON} -c "import sys; print(sys.version)" | sed q`
AC_MSG_NOTICE([using python $python_fullversion])
# python_fullversion is typically n.n.n plus some trailing junk
python_majorversion=`echo "$python_fullversion" | sed '[s/^\([0-9]*\).*/\1/]'`
python_minorversion=`echo "$python_fullversion" | sed '[s/^[0-9]*\.\([0-9]*\).*/\1/]'`
python_version=`echo "$python_fullversion" | sed '[s/^\([0-9]*\.[0-9]*\).*/\1/]'`
# Reject unsupported Python versions as soon as practical.
if test "$python_majorversion" -lt 3 -a "$python_minorversion" -lt 6; then
  AC_MSG_ERROR([Python version $python_version is too old (version 2.6 or later is required)])
fi

AC_MSG_CHECKING([for Python distutils module])
if "${PYTHON}" -c 'import distutils' 2>&AS_MESSAGE_LOG_FD
then
    AC_MSG_RESULT(yes)
else
    AC_MSG_RESULT(no)
    AC_MSG_ERROR([distutils module not found])
fi

AC_MSG_CHECKING([Python configuration directory])
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
# so that LIBDIR is irrelevant.  Also, some packagers put the .so symlink for
# the shlib in ${python_configdir} even though Python itself never does.
# We must also check that what we found is a shared library not a plain
# library, which we do by checking its extension.  (We used to rely on
# Py_ENABLE_SHARED, but that only tells us that a shlib exists, not that
# we found it.)
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
	found_shlib=1
else
	# Otherwise, guess the base name of the shlib.
	# LDVERSION was added in Python 3.2, before that use VERSION,
	# or failing that, $python_version from _PGAC_CHECK_PYTHON_DIRS.
	python_ldversion=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LDVERSION'))))"`
	if test x"${python_ldversion}" != x""; then
		ldlibrary="python${python_ldversion}"
	else
		python_version_var=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('VERSION'))))"`
		if test x"${python_version_var}" != x""; then
			ldlibrary="python${python_version_var}"
		else
			ldlibrary="python${python_version}"
		fi
	fi
	# Search for a likely-looking file.
	found_shlib=0
	for d in "${python_libdir}" "${python_configdir}" /usr/lib64 /usr/lib
	do
		# We don't know the platform DLSUFFIX here, so check 'em all.
		for e in .so .dll .dylib .sl; do
			if test -e "$d/lib${ldlibrary}$e"; then
				python_libdir="$d"
				found_shlib=1
				break 2
			fi
		done
	done
	# Some platforms (OpenBSD) require us to accept a bare versioned shlib
	# (".so.n.n") as well. However, check this only after failing to find
	# ".so" anywhere, because yet other platforms (Debian) put the .so
	# symlink in a different directory from the underlying versioned shlib.
	if test "$found_shlib" != 1; then
		for d in "${python_libdir}" "${python_configdir}" /usr/lib64 /usr/lib
		do
			for f in "$d/lib${ldlibrary}.so."* ; do
				if test -e "$f"; then
					python_libdir="$d"
					found_shlib=1
					break 2
				fi
			done
		done
	fi
	# As usual, Windows has its own ideas.  Possible default library
	# locations include c:/Windows/System32 and (for Cygwin) /usr/bin,
	# and the "lib" prefix might not be there.
	if test "$found_shlib" != 1 -a \( "$PORTNAME" = win32 -o "$PORTNAME" = cygwin \); then
		for d in "${python_libdir}" "${python_configdir}" c:/Windows/System32 /usr/bin
		do
			for f in "$d/lib${ldlibrary}.dll" "$d/${ldlibrary}.dll" ; do
				if test -e "$f"; then
					python_libdir="$d"
					found_shlib=1
					break 2
				fi
			done
		done
	fi
fi
if test "$found_shlib" != 1; then
	AC_MSG_ERROR([could not find shared library for Python
You might have to rebuild your Python installation.  Refer to the
documentation for details.  Use --without-python to disable building
PL/Python.])
fi
python_libspec="-L${python_libdir} -l${ldlibrary}"

python_additional_libs=`${PYTHON} -c "import distutils.sysconfig; print(' '.join(filter(None,distutils.sysconfig.get_config_vars('LIBS','LIBC','LIBM','BASEMODLIBS'))))"`

AC_MSG_RESULT([${python_libspec} ${python_additional_libs}])

AC_SUBST(python_libdir)[]dnl
AC_SUBST(python_libspec)[]dnl
AC_SUBST(python_additional_libs)[]dnl

])# PGAC_CHECK_PYTHON_EMBED_SETUP
