dnl Process this file with autoconf to produce a configure script.
dnl configure.ac
dnl
dnl Developers, please strive to achieve this order:
dnl
dnl 0. Initialization and options processing
dnl 1. Programs
dnl 2. Libraries
dnl 3. Header files
dnl 4. Types
dnl 5. Structures
dnl 6. Compiler characteristics
dnl 7. Functions, global variables
dnl 8. System services
dnl
dnl Read the Autoconf manual for details.
dnl
m4_pattern_forbid(^PGAC_)dnl to catch undefined macros

AC_INIT([PostgreSQL], [18devel], [pgsql-bugs@lists.postgresql.org], [], [https://www.postgresql.org/])

m4_if(m4_defn([m4_PACKAGE_VERSION]), [2.69], [], [m4_fatal([Autoconf version 2.69 is required.
Untested combinations of 'autoconf' and PostgreSQL versions are not
recommended.  You can remove the check from 'configure.ac' but it is then
your responsibility whether the result works or not.])])
AC_COPYRIGHT([Copyright (c) 1996-2024, PostgreSQL Global Development Group])
AC_CONFIG_SRCDIR([src/backend/access/common/heaptuple.c])
AC_CONFIG_AUX_DIR(config)
AC_PREFIX_DEFAULT(/usr/local/pgsql)
AC_DEFINE_UNQUOTED(CONFIGURE_ARGS, ["$ac_configure_args"], [Saved arguments from configure])

[PG_MAJORVERSION=`expr "$PACKAGE_VERSION" : '\([0-9][0-9]*\)'`]
[PG_MINORVERSION=`expr "$PACKAGE_VERSION" : '.*\.\([0-9][0-9]*\)'`]
test -n "$PG_MINORVERSION" || PG_MINORVERSION=0
AC_SUBST(PG_MAJORVERSION)
AC_DEFINE_UNQUOTED(PG_MAJORVERSION, "$PG_MAJORVERSION", [PostgreSQL major version as a string])
AC_DEFINE_UNQUOTED(PG_MAJORVERSION_NUM, $PG_MAJORVERSION, [PostgreSQL major version number])
AC_DEFINE_UNQUOTED(PG_MINORVERSION_NUM, $PG_MINORVERSION, [PostgreSQL minor version number])

PGAC_ARG_REQ(with, extra-version, [STRING], [append STRING to version],
             [PG_VERSION="$PACKAGE_VERSION$withval"],
             [PG_VERSION="$PACKAGE_VERSION"])
AC_DEFINE_UNQUOTED(PG_VERSION, "$PG_VERSION", [PostgreSQL version as a string])

AC_CANONICAL_HOST

template=
AC_MSG_CHECKING([which template to use])

PGAC_ARG_REQ(with, template, [NAME], [override operating system template],
[
  case $withval in
    list)   echo; ls "$srcdir/src/template"; exit;;
    *)      if test -f "$srcdir/src/template/$with_template" ; then
              template=$withval
            else
              AC_MSG_ERROR(['$withval' is not a valid template name. Use 'list' for a list.])
            fi;;
  esac
],
[
# --with-template not given

case $host_os in
  cygwin*|msys*) template=cygwin ;;
  darwin*) template=darwin ;;
dragonfly*) template=netbsd ;;
 freebsd*) template=freebsd ;;
 linux*|gnu*|k*bsd*-gnu)
           template=linux ;;
   mingw*) template=win32 ;;
  netbsd*) template=netbsd ;;
 openbsd*) template=openbsd ;;
 solaris*) template=solaris ;;
esac

  if test x"$template" = x"" ; then
    AC_MSG_ERROR([[
*******************************************************************
PostgreSQL has apparently not been ported to your platform yet.
To try a manual configuration, look into the src/template directory
for a similar platform and use the '--with-template=' option.

Please also contact <]AC_PACKAGE_BUGREPORT[> to see about
rectifying this.  Include the above 'checking host system type...'
line.
*******************************************************************
]])
  fi

])

AC_MSG_RESULT([$template])

PORTNAME=$template
AC_SUBST(PORTNAME)

# Initialize default assumption that we do not need separate assembly code
# for TAS (test-and-set).  This can be overridden by the template file
# when it's executed.
need_tas=no
tas_file=dummy.s

# Default, works for most platforms, override in template file if needed
DLSUFFIX=".so"



##
## Command line options
##

#
# Add non-standard directories to the include path
#
PGAC_ARG_REQ(with, includes, [DIRS], [look for additional header files in DIRS])


#
# Add non-standard directories to the library search path
#
PGAC_ARG_REQ(with, libraries, [DIRS], [look for additional libraries in DIRS],
             [LIBRARY_DIRS=$withval])

PGAC_ARG_REQ(with, libs,      [DIRS], [alternative spelling of --with-libraries],
             [LIBRARY_DIRS=$withval])


#
# 64-bit integer date/time storage is now the only option, but to avoid
# unnecessary breakage of build scripts, continue to accept an explicit
# "--enable-integer-datetimes" switch.
#
PGAC_ARG_BOOL(enable, integer-datetimes, yes, [obsolete option, no longer supported],
              [],
              [AC_MSG_ERROR([--disable-integer-datetimes is no longer supported])])


#
# NLS
#
AC_MSG_CHECKING([whether NLS is wanted])
PGAC_ARG_OPTARG(enable, nls,
                [LANGUAGES], [enable Native Language Support],
                [],
                [WANTED_LANGUAGES=$enableval],
                [AC_DEFINE(ENABLE_NLS, 1,
                           [Define to 1 if you want National Language Support. (--enable-nls)])])
AC_MSG_RESULT([$enable_nls])
AC_SUBST(enable_nls)
AC_SUBST(WANTED_LANGUAGES)

#
# Default port number (--with-pgport), default 5432
#
AC_MSG_CHECKING([for default port number])
PGAC_ARG_REQ(with, pgport, [PORTNUM], [set default port number [5432]],
             [default_port=$withval],
             [default_port=5432])
AC_MSG_RESULT([$default_port])
# Need both of these because some places want an integer and some a string
AC_DEFINE_UNQUOTED(DEF_PGPORT, ${default_port},
[Define to the default TCP port number on which the server listens and
to which clients will try to connect.  This can be overridden at run-time,
but it's convenient if your clients have the right default compiled in.
(--with-pgport=PORTNUM)])
AC_DEFINE_UNQUOTED(DEF_PGPORT_STR, "${default_port}",
                   [Define to the default TCP port number as a string constant.])
AC_SUBST(default_port)

# It's worth validating port; you can get very confusing errors otherwise
if test x"$default_port" = x""; then
  AC_MSG_ERROR([invalid --with-pgport specification: empty string])
elif test ! x`echo "$default_port" | sed -e 's/[[0-9]]*//'` = x""; then
  AC_MSG_ERROR([invalid --with-pgport specification: must be a number])
elif test ! x`echo "$default_port" | sed -e 's/^0.//'` = x"$default_port"; then
  AC_MSG_ERROR([invalid --with-pgport specification: must not have leading 0])
elif test "$default_port" -lt "1" -o "$default_port" -gt "65535"; then
  AC_MSG_ERROR([invalid --with-pgport specification: must be between 1 and 65535])
fi

#
# '-rpath'-like feature can be disabled
#
PGAC_ARG_BOOL(enable, rpath, yes,
              [do not embed shared library search path in executables])
AC_SUBST(enable_rpath)

#
# --enable-debug adds -g to compiler flags
#
PGAC_ARG_BOOL(enable, debug, no,
              [build with debugging symbols (-g)])
AC_SUBST(enable_debug)

#
# --enable-profiling enables gcc profiling
#
PGAC_ARG_BOOL(enable, profiling, no,
              [build with profiling enabled ])

#
# --enable-coverage enables generation of code coverage metrics with gcov
#
PGAC_ARG_BOOL(enable, coverage, no,
              [build with coverage testing instrumentation],
[PGAC_PATH_PROGS(GCOV, gcov)
if test -z "$GCOV"; then
  AC_MSG_ERROR([gcov not found])
fi
PGAC_PATH_PROGS(LCOV, lcov)
if test -z "$LCOV"; then
  AC_MSG_ERROR([lcov not found])
fi
PGAC_PATH_PROGS(GENHTML, genhtml)
if test -z "$GENHTML"; then
  AC_MSG_ERROR([genhtml not found])
fi])
AC_SUBST(enable_coverage)

#
# DTrace
#
PGAC_ARG_BOOL(enable, dtrace, no,
              [build with DTrace support],
[PGAC_PATH_PROGS(DTRACE, dtrace)
if test -z "$DTRACE"; then
  AC_MSG_ERROR([dtrace not found])
fi
AC_SUBST(DTRACEFLAGS)])
AC_SUBST(enable_dtrace)

#
# TAP tests
#
PGAC_ARG_BOOL(enable, tap-tests, no,
              [enable TAP tests (requires Perl and IPC::Run)])
AC_SUBST(enable_tap_tests)

#
# Injection points
#
PGAC_ARG_BOOL(enable, injection-points, no, [enable injection points (for testing)],
              [AC_DEFINE([USE_INJECTION_POINTS], 1, [Define to 1 to build with injection points. (--enable-injection-points)])])
AC_SUBST(enable_injection_points)

#
# Block size
#
AC_MSG_CHECKING([for block size])
PGAC_ARG_REQ(with, blocksize, [BLOCKSIZE], [set table block size in kB [8]],
             [blocksize=$withval],
             [blocksize=8])
case ${blocksize} in
  1) BLCKSZ=1024;;
  2) BLCKSZ=2048;;
  4) BLCKSZ=4096;;
  8) BLCKSZ=8192;;
 16) BLCKSZ=16384;;
 32) BLCKSZ=32768;;
  *) AC_MSG_ERROR([Invalid block size. Allowed values are 1,2,4,8,16,32.])
esac
AC_MSG_RESULT([${blocksize}kB])

AC_DEFINE_UNQUOTED([BLCKSZ], ${BLCKSZ}, [
 Size of a disk block --- this also limits the size of a tuple.  You
 can set it bigger if you need bigger tuples (although TOAST should
 reduce the need to have large tuples, since fields can be spread
 across multiple tuples).

 BLCKSZ must be a power of 2.  The maximum possible value of BLCKSZ
 is currently 2^15 (32768).  This is determined by the 15-bit widths
 of the lp_off and lp_len fields in ItemIdData (see
 include/storage/itemid.h).

 Changing BLCKSZ requires an initdb.
])

#
# Relation segment size
#
PGAC_ARG_REQ(with, segsize, [SEGSIZE], [set table segment size in GB [1]],
             [segsize=$withval],
             [segsize=1])
PGAC_ARG_REQ(with, segsize-blocks, [SEGSIZE_BLOCKS], [set table segment size in blocks [0]],
             [segsize_blocks=$withval],
             [segsize_blocks=0])

# If --with-segsize-blocks is non-zero, it is used, --with-segsize
# otherwise. segsize-blocks is only really useful for developers wanting to
# test segment related code. Warn if both are used.
if test $segsize_blocks -ne 0 -a $segsize -ne 1; then
  AC_MSG_WARN([both --with-segsize and --with-segsize-blocks specified, --with-segsize-blocks wins])
fi

AC_MSG_CHECKING([for segment size])
if test $segsize_blocks -eq 0; then
  # this expression is set up to avoid unnecessary integer overflow
  # blocksize is already guaranteed to be a factor of 1024
  RELSEG_SIZE=`expr '(' 1024 / ${blocksize} ')' '*' ${segsize} '*' 1024`
  test $? -eq 0 || exit 1
  AC_MSG_RESULT([${segsize}GB])
else
  RELSEG_SIZE=$segsize_blocks
  AC_MSG_RESULT([${RELSEG_SIZE} blocks])
fi

AC_DEFINE_UNQUOTED([RELSEG_SIZE], ${RELSEG_SIZE}, [
 RELSEG_SIZE is the maximum number of blocks allowed in one disk file.
 Thus, the maximum size of a single file is RELSEG_SIZE * BLCKSZ;
 relations bigger than that are divided into multiple files.

 RELSEG_SIZE * BLCKSZ must be less than your OS' limit on file size.
 This is often 2 GB or 4GB in a 32-bit operating system, unless you
 have large file support enabled.  By default, we make the limit 1 GB
 to avoid any possible integer-overflow problems within the OS.
 A limit smaller than necessary only means we divide a large
 relation into more chunks than necessary, so it seems best to err
 in the direction of a small limit.

 A power-of-2 value is recommended to save a few cycles in md.c,
 but is not absolutely required.

 Changing RELSEG_SIZE requires an initdb.
])

#
# WAL block size
#
AC_MSG_CHECKING([for WAL block size])
PGAC_ARG_REQ(with, wal-blocksize, [BLOCKSIZE], [set WAL block size in kB [8]],
             [wal_blocksize=$withval],
             [wal_blocksize=8])
case ${wal_blocksize} in
  1) XLOG_BLCKSZ=1024;;
  2) XLOG_BLCKSZ=2048;;
  4) XLOG_BLCKSZ=4096;;
  8) XLOG_BLCKSZ=8192;;
 16) XLOG_BLCKSZ=16384;;
 32) XLOG_BLCKSZ=32768;;
 64) XLOG_BLCKSZ=65536;;
  *) AC_MSG_ERROR([Invalid WAL block size. Allowed values are 1,2,4,8,16,32,64.])
esac
AC_MSG_RESULT([${wal_blocksize}kB])

AC_DEFINE_UNQUOTED([XLOG_BLCKSZ], ${XLOG_BLCKSZ}, [
 Size of a WAL file block.  This need have no particular relation to BLCKSZ.
 XLOG_BLCKSZ must be a power of 2, and if your system supports O_DIRECT I/O,
 XLOG_BLCKSZ must be a multiple of the alignment requirement for direct-I/O
 buffers, else direct I/O may fail.

 Changing XLOG_BLCKSZ requires an initdb.
])

#
# C compiler
#

# If you don't specify a list of compilers to test, the AC_PROG_CC and
# AC_PROG_CXX macros test for a long list of unsupported compilers.
pgac_cc_list="gcc cc"
pgac_cxx_list="g++ c++"

AC_PROG_CC([$pgac_cc_list])
AC_PROG_CC_C99()

# Error out if the compiler does not support C99, as the codebase
# relies on that.
if test "$ac_cv_prog_cc_c99" = no; then
    AC_MSG_ERROR([C compiler "$CC" does not support C99])
fi

AC_PROG_CXX([$pgac_cxx_list])

# Check if it's Intel's compiler, which (usually) pretends to be gcc,
# but has idiosyncrasies of its own.  We assume icc will define
# __INTEL_COMPILER regardless of CFLAGS.
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [@%:@ifndef __INTEL_COMPILER
choke me
@%:@endif])], [ICC=yes], [ICC=no])

# Check if it's Sun Studio compiler. We assume that
# __SUNPRO_C will be defined for Sun Studio compilers
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [@%:@ifndef __SUNPRO_C
choke me
@%:@endif])], [SUN_STUDIO_CC=yes], [SUN_STUDIO_CC=no])

AC_SUBST(SUN_STUDIO_CC)


#
# LLVM
#
# Checked early because subsequent tests depend on it.
PGAC_ARG_BOOL(with, llvm, no, [build with LLVM based JIT support],
              [AC_DEFINE([USE_LLVM], 1, [Define to 1 to build with LLVM based JIT support. (--with-llvm)])])
AC_SUBST(with_llvm)
dnl must use AS_IF here, else AC_REQUIRES inside PGAC_LLVM_SUPPORT malfunctions
AS_IF([test "$with_llvm" = yes], [
  PGAC_LLVM_SUPPORT()
]) # fi


unset CFLAGS
unset CXXFLAGS

#
# Read the template
#
. "$srcdir/src/template/$template" || exit

# C[XX]FLAGS are selected so:
# If the user specifies something in the environment, that is used.
# else:  If the template file set something, that is used.
# else:  If coverage was enabled, don't set anything.
# else:  If the compiler is GCC, then we use -O2.
# else:  If the compiler is something else, then we use -O, unless debugging.

if test "$ac_env_CFLAGS_set" = set; then
  CFLAGS=$ac_env_CFLAGS_value
elif test "${CFLAGS+set}" = set; then
  : # (keep what template set)
elif test "$enable_coverage" = yes; then
  : # no optimization by default
elif test "$GCC" = yes; then
  CFLAGS="-O2"
else
  # if the user selected debug mode, don't use -O
  if test "$enable_debug" != yes; then
    CFLAGS="-O"
  fi
fi

if test "$ac_env_CXXFLAGS_set" = set; then
  CXXFLAGS=$ac_env_CXXFLAGS_value
elif test "${CXXFLAGS+set}" = set; then
  : # (keep what template set)
elif test "$enable_coverage" = yes; then
  : # no optimization by default
elif test "$GCC" = yes; then
  CXXFLAGS="-O2"
else
  # if the user selected debug mode, don't use -O
  if test "$enable_debug" != yes; then
    CXXFLAGS="-O"
  fi
fi

# When generating bitcode (for inlining) we always want to use -O2
# even when --enable-debug is specified. The bitcode is not going to
# be used for line-by-line debugging, and JIT inlining doesn't work
# without at least -O1 (otherwise clang will emit 'noinline'
# attributes everywhere), which is bad for testing.  Still allow the
# environment to override if done explicitly.
if test "$ac_env_BITCODE_CFLAGS_set" = set; then
  BITCODE_CFLAGS=$ac_env_BITCODE_CFLAGS_value
else
  BITCODE_CFLAGS="-O2 $BITCODE_CFLAGS"
fi
if test "$ac_env_BITCODE_CXXFLAGS_set" = set; then
  BITCODE_CXXFLAGS=$ac_env_BITCODE_CXXFLAGS_value
else
  BITCODE_CXXFLAGS="-O2 $BITCODE_CXXFLAGS"
fi

# C[XX]FLAGS we determined above will be added back at the end
user_CFLAGS=$CFLAGS
CFLAGS=""
user_CXXFLAGS=$CXXFLAGS
CXXFLAGS=""
user_BITCODE_CFLAGS=$BITCODE_CFLAGS
BITCODE_CFLAGS=""
user_BITCODE_CXXFLAGS=$BITCODE_CXXFLAGS
BITCODE_CXXFLAGS=""

# set CFLAGS_UNROLL_LOOPS and CFLAGS_VECTORIZE from the environment, if present
if test "$ac_env_CFLAGS_UNROLL_LOOPS_set" = set; then
  CFLAGS_UNROLL_LOOPS=$ac_env_CFLAGS_UNROLL_LOOPS_value
fi
if test "$ac_env_CFLAGS_VECTORIZE_set" = set; then
  CFLAGS_VECTORIZE=$ac_env_CFLAGS_VECTORIZE_value
fi

# Some versions of GCC support some additional useful warning flags.
# Check whether they are supported, and add them to CFLAGS if so.
# ICC pretends to be GCC but it's lying; it doesn't support these flags,
# but has its own.  Also check other compiler-specific flags here.

if test "$GCC" = yes -a "$ICC" = no; then
  CFLAGS="-Wall -Wmissing-prototypes -Wpointer-arith"
  CXXFLAGS="-Wall -Wpointer-arith"
  # These work in some but not all gcc versions
  save_CFLAGS=$CFLAGS
  PGAC_PROG_CC_CFLAGS_OPT([-Wdeclaration-after-statement])
  # -Wdeclaration-after-statement isn't applicable for C++.  Specific C files
  # disable it, so AC_SUBST the negative form.
  PERMIT_DECLARATION_AFTER_STATEMENT=
  if test x"$save_CFLAGS" != x"$CFLAGS"; then
    PERMIT_DECLARATION_AFTER_STATEMENT=-Wno-declaration-after-statement
  fi
  AC_SUBST(PERMIT_DECLARATION_AFTER_STATEMENT)
  # Really don't want VLAs to be used in our dialect of C
  PGAC_PROG_CC_CFLAGS_OPT([-Werror=vla])
  # On macOS, complain about usage of symbols newer than the deployment target
  PGAC_PROG_CC_CFLAGS_OPT([-Werror=unguarded-availability-new])
  PGAC_PROG_CXX_CFLAGS_OPT([-Werror=unguarded-availability-new])
  # -Wvla is not applicable for C++
  PGAC_PROG_CC_CFLAGS_OPT([-Wendif-labels])
  PGAC_PROG_CXX_CFLAGS_OPT([-Wendif-labels])
  PGAC_PROG_CC_CFLAGS_OPT([-Wmissing-format-attribute])
  PGAC_PROG_CXX_CFLAGS_OPT([-Wmissing-format-attribute])
  PGAC_PROG_CC_CFLAGS_OPT([-Wimplicit-fallthrough=3])
  PGAC_PROG_CXX_CFLAGS_OPT([-Wimplicit-fallthrough=3])
  PGAC_PROG_CC_CFLAGS_OPT([-Wcast-function-type])
  PGAC_PROG_CXX_CFLAGS_OPT([-Wcast-function-type])
  PGAC_PROG_CC_CFLAGS_OPT([-Wshadow=compatible-local])
  PGAC_PROG_CXX_CFLAGS_OPT([-Wshadow=compatible-local])
  # This was included in -Wall/-Wformat in older GCC versions
  PGAC_PROG_CC_CFLAGS_OPT([-Wformat-security])
  PGAC_PROG_CXX_CFLAGS_OPT([-Wformat-security])
  # gcc 14+, clang for a while
  # (Supported in C++ by clang but not gcc.  For consistency, omit in C++.)
  save_CFLAGS=$CFLAGS
  PGAC_PROG_CC_CFLAGS_OPT([-Wmissing-variable-declarations])
  PERMIT_MISSING_VARIABLE_DECLARATIONS=
  if test x"$save_CFLAGS" != x"$CFLAGS"; then
    PERMIT_MISSING_VARIABLE_DECLARATIONS=-Wno-missing-variable-declarations
  fi
  AC_SUBST(PERMIT_MISSING_VARIABLE_DECLARATIONS)
  # Disable strict-aliasing rules; needed for gcc 3.3+
  PGAC_PROG_CC_CFLAGS_OPT([-fno-strict-aliasing])
  PGAC_PROG_CXX_CFLAGS_OPT([-fno-strict-aliasing])
  # Disable optimizations that assume no overflow; needed for gcc 4.3+
  PGAC_PROG_CC_CFLAGS_OPT([-fwrapv])
  PGAC_PROG_CXX_CFLAGS_OPT([-fwrapv])
  # Disable FP optimizations that cause various errors on gcc 4.5+ or maybe 4.6+
  PGAC_PROG_CC_CFLAGS_OPT([-fexcess-precision=standard])
  PGAC_PROG_CXX_CFLAGS_OPT([-fexcess-precision=standard])
  # Optimization flags for specific files that benefit from loop unrolling
  PGAC_PROG_CC_VAR_OPT(CFLAGS_UNROLL_LOOPS, [-funroll-loops])
  # Optimization flags for specific files that benefit from vectorization
  PGAC_PROG_CC_VAR_OPT(CFLAGS_VECTORIZE, [-ftree-vectorize])
  #
  # The following tests want to suppress various unhelpful warnings by adding
  # -Wno-foo switches.  But gcc won't complain about unrecognized -Wno-foo
  # switches, so we have to test for the positive form and if that works,
  # add the negative form.  Note that tests of this form typically need to
  # be duplicated in the BITCODE_CFLAGS setup stanza below.
  #
  # Suppress clang's unhelpful unused-command-line-argument warnings.
  NOT_THE_CFLAGS=""
  PGAC_PROG_CC_VAR_OPT(NOT_THE_CFLAGS, [-Wunused-command-line-argument])
  if test -n "$NOT_THE_CFLAGS"; then
    CFLAGS="$CFLAGS -Wno-unused-command-line-argument"
  fi
  # Remove clang 12+'s compound-token-split-by-macro, as this causes a lot
  # of warnings when building plperl because of usages in the Perl headers.
  NOT_THE_CFLAGS=""
  PGAC_PROG_CC_VAR_OPT(NOT_THE_CFLAGS, [-Wcompound-token-split-by-macro])
  if test -n "$NOT_THE_CFLAGS"; then
    CFLAGS="$CFLAGS -Wno-compound-token-split-by-macro"
  fi
  # Similarly disable useless truncation warnings from gcc 8+
  NOT_THE_CFLAGS=""
  PGAC_PROG_CC_VAR_OPT(NOT_THE_CFLAGS, [-Wformat-truncation])
  if test -n "$NOT_THE_CFLAGS"; then
    CFLAGS="$CFLAGS -Wno-format-truncation"
  fi
  NOT_THE_CFLAGS=""
  PGAC_PROG_CC_VAR_OPT(NOT_THE_CFLAGS, [-Wstringop-truncation])
  if test -n "$NOT_THE_CFLAGS"; then
    CFLAGS="$CFLAGS -Wno-stringop-truncation"
  fi
  # Suppress clang 16's strict warnings about function casts
  NOT_THE_CFLAGS=""
  PGAC_PROG_CC_VAR_OPT(NOT_THE_CFLAGS, [-Wcast-function-type-strict])
  if test -n "$NOT_THE_CFLAGS"; then
    CFLAGS="$CFLAGS -Wno-cast-function-type-strict"
  fi
elif test "$ICC" = yes; then
  # Intel's compiler has a bug/misoptimization in checking for
  # division by NAN (NaN == 0), -mp1 fixes it, so add it to the CFLAGS.
  PGAC_PROG_CC_CFLAGS_OPT([-mp1])
  PGAC_PROG_CXX_CFLAGS_OPT([-mp1])
  # Make sure strict aliasing is off (though this is said to be the default)
  PGAC_PROG_CC_CFLAGS_OPT([-fno-strict-aliasing])
  PGAC_PROG_CXX_CFLAGS_OPT([-fno-strict-aliasing])
fi

# If the compiler knows how to hide symbols, add the switch needed for that to
# CFLAGS_SL_MODULE and define HAVE_VISIBILITY_ATTRIBUTE.
#
# This is done separately from the above because -fvisibility is supported by
# quite a few different compilers, making the required repetition bothersome.
#
# We might need to add a separate test to check if
# __attribute__((visibility("hidden"))) is supported, if we encounter a
# compiler that supports one of the supported variants of -fvisibility=hidden
# but uses a different syntax to mark a symbol as exported.
if test "$GCC" = yes -o "$SUN_STUDIO_CC" = yes ; then
  PGAC_PROG_CC_VAR_OPT(CFLAGS_SL_MODULE, [-fvisibility=hidden])
  # For C++ we additionally want -fvisibility-inlines-hidden
  PGAC_PROG_VARCXX_VARFLAGS_OPT(CXX, CXXFLAGS_SL_MODULE, [-fvisibility=hidden])
  PGAC_PROG_VARCXX_VARFLAGS_OPT(CXX, CXXFLAGS_SL_MODULE, [-fvisibility-inlines-hidden])
  have_visibility_attribute=$pgac_cv_prog_CC_cflags__fvisibility_hidden
fi

if test "$have_visibility_attribute" = "yes"; then
  AC_DEFINE([HAVE_VISIBILITY_ATTRIBUTE], 1,
            [Define to 1 if your compiler knows the visibility("hidden") attribute.])
fi

AC_SUBST(CFLAGS_UNROLL_LOOPS)
AC_SUBST(CFLAGS_VECTORIZE)
AC_SUBST(CFLAGS_SL_MODULE)
AC_SUBST(CXXFLAGS_SL_MODULE)

# Determine flags used to emit bitcode for JIT inlining.
# 1. We must duplicate any behaviour-changing compiler flags used above,
# to keep compatibility with the compiler used for normal Postgres code.
# 2. We don't bother to duplicate extra-warnings switches --- seeing a
# warning in the main build is enough.
# 3. But we must duplicate -Wno-warning flags, else we'll see those anyway.
if test "$with_llvm" = yes ; then
  CLANGXX="$CLANG -xc++"

  PGAC_PROG_VARCC_VARFLAGS_OPT(CLANG, BITCODE_CFLAGS, [-fno-strict-aliasing])
  PGAC_PROG_VARCXX_VARFLAGS_OPT(CLANGXX, BITCODE_CXXFLAGS, [-fno-strict-aliasing])
  PGAC_PROG_VARCC_VARFLAGS_OPT(CLANG, BITCODE_CFLAGS, [-fwrapv])
  PGAC_PROG_VARCXX_VARFLAGS_OPT(CLANGXX, BITCODE_CXXFLAGS, [-fwrapv])
  PGAC_PROG_VARCC_VARFLAGS_OPT(CLANG, BITCODE_CFLAGS, [-fexcess-precision=standard])
  PGAC_PROG_VARCXX_VARFLAGS_OPT(CLANGXX, BITCODE_CXXFLAGS, [-fexcess-precision=standard])

  PGAC_PROG_VARCC_VARFLAGS_OPT(CLANG, BITCODE_CFLAGS, [-Xclang -no-opaque-pointers])
  PGAC_PROG_VARCXX_VARFLAGS_OPT(CLANGXX, BITCODE_CXXFLAGS, [-Xclang -no-opaque-pointers])

  NOT_THE_CFLAGS=""
  PGAC_PROG_VARCC_VARFLAGS_OPT(CLANG, NOT_THE_CFLAGS, [-Wunused-command-line-argument])
  if test -n "$NOT_THE_CFLAGS"; then
    BITCODE_CFLAGS="$BITCODE_CFLAGS -Wno-unused-command-line-argument"
  fi
  NOT_THE_CFLAGS=""
  PGAC_PROG_VARCC_VARFLAGS_OPT(CLANG, NOT_THE_CFLAGS, [-Wcompound-token-split-by-macro])
  if test -n "$NOT_THE_CFLAGS"; then
    BITCODE_CFLAGS="$BITCODE_CFLAGS -Wno-compound-token-split-by-macro"
  fi
  NOT_THE_CFLAGS=""
  PGAC_PROG_VARCC_VARFLAGS_OPT(CLANG, NOT_THE_CFLAGS, [-Wformat-truncation])
  if test -n "$NOT_THE_CFLAGS"; then
    BITCODE_CFLAGS="$BITCODE_CFLAGS -Wno-format-truncation"
  fi
  NOT_THE_CFLAGS=""
  PGAC_PROG_VARCC_VARFLAGS_OPT(CLANG, NOT_THE_CFLAGS, [-Wstringop-truncation])
  if test -n "$NOT_THE_CFLAGS"; then
    BITCODE_CFLAGS="$BITCODE_CFLAGS -Wno-stringop-truncation"
  fi
fi

# supply -g if --enable-debug
if test "$enable_debug" = yes && test "$ac_cv_prog_cc_g" = yes; then
  CFLAGS="$CFLAGS -g"
fi

if test "$enable_debug" = yes && test "$ac_cv_prog_cxx_g" = yes; then
  CXXFLAGS="$CXXFLAGS -g"
fi

# enable code coverage if --enable-coverage
if test "$enable_coverage" = yes; then
  if test "$GCC" = yes; then
    CFLAGS="$CFLAGS -fprofile-arcs -ftest-coverage"
    CXXFLAGS="$CXXFLAGS -fprofile-arcs -ftest-coverage"
  else
    AC_MSG_ERROR([--enable-coverage is supported only when using GCC])
  fi
fi

# enable profiling if --enable-profiling
if test "$enable_profiling" = yes && test "$ac_cv_prog_cc_g" = yes; then
  if test "$GCC" = yes; then
    AC_DEFINE([PROFILE_PID_DIR], 1,
           [Define to 1 to allow profiling output to be saved separately for each process.])
    CFLAGS="$CFLAGS -pg $PLATFORM_PROFILE_FLAGS"
    CXXFLAGS="$CXXFLAGS -pg $PLATFORM_PROFILE_FLAGS"
  else
    AC_MSG_ERROR([--enable-profiling is supported only when using GCC])
  fi
fi

# On Solaris, we need this #define to get POSIX-conforming versions
# of many interfaces (sigwait, getpwuid_r, ...).
if test "$PORTNAME" = "solaris"; then
  CPPFLAGS="$CPPFLAGS -D_POSIX_PTHREAD_SEMANTICS"
fi

# We already have this in Makefile.win32, but configure needs it too
if test "$PORTNAME" = "win32"; then
  CPPFLAGS="$CPPFLAGS -I$srcdir/src/include/port/win32"
fi

# Now that we're done automatically adding stuff to C[XX]FLAGS, put back the
# user-specified flags (if any) at the end.  This lets users override
# the automatic additions.
CFLAGS="$CFLAGS $user_CFLAGS"
CXXFLAGS="$CXXFLAGS $user_CXXFLAGS"
BITCODE_CFLAGS="$BITCODE_CFLAGS $user_BITCODE_CFLAGS"
BITCODE_CXXFLAGS="$BITCODE_CXXFLAGS $user_BITCODE_CXXFLAGS"

AC_SUBST(BITCODE_CFLAGS)
AC_SUBST(BITCODE_CXXFLAGS)

# The template file must set up CFLAGS_SL; we don't support user override
AC_SUBST(CFLAGS_SL)

# Check if the compiler still works with the final flag settings
# (note, we're not checking that for CXX, which is optional)
AC_MSG_CHECKING([whether the C compiler still works])
AC_LINK_IFELSE([AC_LANG_PROGRAM([], [return 0;])],
  [AC_MSG_RESULT(yes)],
  [AC_MSG_RESULT(no)
   AC_MSG_ERROR([cannot proceed])])

# Defend against gcc -ffast-math
if test "$GCC" = yes; then
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [@%:@ifdef __FAST_MATH__
choke me
@%:@endif])], [], [AC_MSG_ERROR([do not put -ffast-math in CFLAGS])])
fi

# Defend against clang being used on x86-32 without SSE2 enabled.  As current
# versions of clang do not understand -fexcess-precision=standard, the use of
# x87 floating point operations leads to problems like isinf possibly returning
# false for a value that is infinite when converted from the 80bit register to
# the 8byte memory representation.
#
# Only perform the test if the compiler doesn't understand
# -fexcess-precision=standard, that way a potentially fixed compiler will work
# automatically.
if test "$pgac_cv_prog_CC_cflags__fexcess_precision_standard" = no; then
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [
@%:@if defined(__clang__) && defined(__i386__) && !defined(__SSE2_MATH__)
choke me
@%:@endif
])], [],
[AC_MSG_ERROR([Compiling PostgreSQL with clang, on 32bit x86, requires SSE2 support. Use -msse2 or use gcc.])])
fi

AC_PROG_CPP
AC_SUBST(GCC)


#
# Set up TAS assembly code if needed; the template file has now had its
# chance to request this.
#
AC_CONFIG_LINKS([src/backend/port/tas.s:src/backend/port/tas/${tas_file}])

if test "$need_tas" = yes ; then
  TAS=tas.o
else
  TAS=""
fi
AC_SUBST(TAS)

AC_SUBST(DLSUFFIX)dnl
AC_DEFINE_UNQUOTED([DLSUFFIX], ["$DLSUFFIX"],
                   [Define to the file name extension of dynamically-loadable modules.])

#
# Set up pkg_config in case we need it below
#
PKG_PROG_PKG_CONFIG

#
# Automatic dependency tracking
#
PGAC_ARG_BOOL(enable, depend, no, [turn on automatic dependency tracking],
              [autodepend=yes])
AC_SUBST(autodepend)


#
# Enable assert checks
#
PGAC_ARG_BOOL(enable, cassert, no, [enable assertion checks (for debugging)],
              [AC_DEFINE([USE_ASSERT_CHECKING], 1,
                         [Define to 1 to build with assertion checks. (--enable-cassert)])])


#
# Include directories
#
ac_save_IFS=$IFS
IFS="${IFS}${PATH_SEPARATOR}"
# SRCH_INC comes from the template file
for dir in $with_includes $SRCH_INC; do
  if test -d "$dir"; then
    INCLUDES="$INCLUDES -I$dir"
  else
    AC_MSG_WARN([*** Include directory $dir does not exist.])
  fi
done
IFS=$ac_save_IFS
AC_SUBST(INCLUDES)


#
# Library directories
#
ac_save_IFS=$IFS
IFS="${IFS}${PATH_SEPARATOR}"
# LIBRARY_DIRS comes from command line, SRCH_LIB from template file.
for dir in $LIBRARY_DIRS $SRCH_LIB; do
  if test -d "$dir"; then
    LIBDIRS="$LIBDIRS -L$dir"
  else
    AC_MSG_WARN([*** Library directory $dir does not exist.])
  fi
done
IFS=$ac_save_IFS

#
# ICU
#
AC_MSG_CHECKING([whether to build with ICU support])
PGAC_ARG_BOOL(with, icu, yes, [build without ICU support],
              [AC_DEFINE([USE_ICU], 1, [Define to build with ICU support. (--with-icu)])])
AC_MSG_RESULT([$with_icu])
AC_SUBST(with_icu)

if test "$with_icu" = yes; then
  PKG_CHECK_MODULES(ICU, icu-uc icu-i18n)
fi

#
# Optionally build Tcl modules (PL/Tcl)
#
AC_MSG_CHECKING([whether to build with Tcl])
PGAC_ARG_BOOL(with, tcl, no, [build Tcl modules (PL/Tcl)])
AC_MSG_RESULT([$with_tcl])
AC_SUBST([with_tcl])

# We see if the path to the Tcl/Tk configuration scripts is specified.
# This will override the use of tclsh to find the paths to search.

PGAC_ARG_REQ(with, tclconfig, [DIR], [tclConfig.sh is in DIR])

#
# Optionally build Perl modules (PL/Perl)
#
AC_MSG_CHECKING([whether to build Perl modules])
PGAC_ARG_BOOL(with, perl, no, [build Perl modules (PL/Perl)])
AC_MSG_RESULT([$with_perl])
AC_SUBST(with_perl)

#
# Optionally build Python modules (PL/Python)
#
AC_MSG_CHECKING([whether to build Python modules])
PGAC_ARG_BOOL(with, python, no, [build Python modules (PL/Python)])
AC_MSG_RESULT([$with_python])
AC_SUBST(with_python)

#
# GSSAPI
#
AC_MSG_CHECKING([whether to build with GSSAPI support])
PGAC_ARG_BOOL(with, gssapi, no, [build with GSSAPI support],
[
  AC_DEFINE(ENABLE_GSS, 1, [Define to build with GSSAPI support. (--with-gssapi)])
  krb_srvtab="FILE:\$(sysconfdir)/krb5.keytab"
])
AC_MSG_RESULT([$with_gssapi])
AC_SUBST(with_gssapi)


AC_SUBST(krb_srvtab)


#
# Kerberos configuration parameters
#
PGAC_ARG_REQ(with, krb-srvnam,
             [NAME], [default service principal name in Kerberos (GSSAPI) [postgres]],
             [],
             [with_krb_srvnam="postgres"])
AC_SUBST(with_krb_srvnam)
AC_DEFINE_UNQUOTED([PG_KRB_SRVNAM], ["$with_krb_srvnam"],
                   [Define to the name of the default PostgreSQL service principal in Kerberos (GSSAPI). (--with-krb-srvnam=NAME)])


#
# PAM
#
AC_MSG_CHECKING([whether to build with PAM support])
PGAC_ARG_BOOL(with, pam, no,
              [build with PAM support],
              [AC_DEFINE([USE_PAM], 1, [Define to 1 to build with PAM support. (--with-pam)])])
AC_MSG_RESULT([$with_pam])


#
# BSD AUTH
#
AC_MSG_CHECKING([whether to build with BSD Authentication support])
PGAC_ARG_BOOL(with, bsd-auth, no,
              [build with BSD Authentication support],
              [AC_DEFINE([USE_BSD_AUTH], 1, [Define to 1 to build with BSD Authentication support. (--with-bsd-auth)])])
AC_MSG_RESULT([$with_bsd_auth])


#
# LDAP
#
AC_MSG_CHECKING([whether to build with LDAP support])
PGAC_ARG_BOOL(with, ldap, no,
              [build with LDAP support],
              [AC_DEFINE([USE_LDAP], 1, [Define to 1 to build with LDAP support. (--with-ldap)])])
AC_MSG_RESULT([$with_ldap])
AC_SUBST(with_ldap)


#
# Bonjour
#
AC_MSG_CHECKING([whether to build with Bonjour support])
PGAC_ARG_BOOL(with, bonjour, no,
              [build with Bonjour support],
              [AC_DEFINE([USE_BONJOUR], 1, [Define to 1 to build with Bonjour support. (--with-bonjour)])])
AC_MSG_RESULT([$with_bonjour])


#
# SELinux
#
AC_MSG_CHECKING([whether to build with SELinux support])
PGAC_ARG_BOOL(with, selinux, no, [build with SELinux support])
AC_SUBST(with_selinux)
AC_MSG_RESULT([$with_selinux])

#
# Systemd
#
AC_MSG_CHECKING([whether to build with systemd support])
PGAC_ARG_BOOL(with, systemd, no, [build with systemd support],
              [AC_DEFINE([USE_SYSTEMD], 1, [Define to build with systemd support. (--with-systemd)])])
AC_SUBST(with_systemd)
AC_MSG_RESULT([$with_systemd])

#
# Readline
#
PGAC_ARG_BOOL(with, readline, yes,
              [do not use GNU Readline nor BSD Libedit for editing])
# readline on MinGW has problems with backslashes in psql and other bugs.
# This is particularly a problem with non-US code pages.
# Therefore disable its use until we understand the cause. 2004-07-20
if test "$PORTNAME" = "win32"; then
  if test "$with_readline" = yes; then
    AC_MSG_WARN([*** Readline does not work on MinGW --- disabling])
    with_readline=no
  fi
fi
AC_SUBST(with_readline)


#
# Prefer libedit
#
PGAC_ARG_BOOL(with, libedit-preferred, no,
              [prefer BSD Libedit over GNU Readline])


#
# UUID library
#
# There are at least three UUID libraries in common use: the FreeBSD/NetBSD
# library, the e2fsprogs libuuid (now part of util-linux-ng), and the OSSP
# UUID library.  More than one of these might be present on a given platform,
# so we make the user say which one she wants.
#
PGAC_ARG_REQ(with, uuid, [LIB], [build contrib/uuid-ossp using LIB (bsd,e2fs,ossp)])
if test x"$with_uuid" = x"" ; then
  with_uuid=no
fi
PGAC_ARG_BOOL(with, ossp-uuid, no, [obsolete spelling of --with-uuid=ossp])
if test "$with_ossp_uuid" = yes ; then
  with_uuid=ossp
fi

if test "$with_uuid" != no ; then
  if test "$with_uuid" = bsd ; then
    AC_DEFINE([HAVE_UUID_BSD], 1, [Define to 1 if you have BSD UUID support.])
  elif test "$with_uuid" = e2fs ; then
    AC_DEFINE([HAVE_UUID_E2FS], 1, [Define to 1 if you have E2FS UUID support.])
  elif test "$with_uuid" = ossp ; then
    AC_DEFINE([HAVE_UUID_OSSP], 1, [Define to 1 if you have OSSP UUID support.])
  else
    AC_MSG_ERROR([--with-uuid must specify one of bsd, e2fs, or ossp])
  fi
fi
AC_SUBST(with_uuid)


#
# XML
#
AC_MSG_CHECKING([whether to build with XML support])
PGAC_ARG_BOOL(with, libxml, no, [build with XML support],
              [AC_DEFINE([USE_LIBXML], 1, [Define to 1 to build with XML support. (--with-libxml)])])
AC_MSG_RESULT([$with_libxml])
AC_SUBST(with_libxml)

if test "$with_libxml" = yes ; then
  # Check pkg-config, then xml2-config.  But for backwards compatibility,
  # setting XML2_CONFIG overrides pkg-config.
  AC_ARG_VAR(XML2_CONFIG, [path to xml2-config utility])dnl
  have_libxml2_pkg_config=no
  if test -z "$XML2_CONFIG" -a -n "$PKG_CONFIG"; then
    PKG_CHECK_MODULES(XML2, [libxml-2.0 >= 2.6.23],
                      [have_libxml2_pkg_config=yes], [# do nothing])
  fi
  if test "$have_libxml2_pkg_config" = no ; then
    PGAC_PATH_PROGS(XML2_CONFIG, xml2-config)
    if test -n "$XML2_CONFIG"; then
      XML2_CFLAGS=`$XML2_CONFIG --cflags`
      XML2_LIBS=`$XML2_CONFIG --libs`
    fi
  fi
  # Note the user could also set XML2_CFLAGS/XML2_LIBS directly
  for pgac_option in $XML2_CFLAGS; do
    case $pgac_option in
      -I*|-D*) CPPFLAGS="$CPPFLAGS $pgac_option";;
    esac
  done
  for pgac_option in $XML2_LIBS; do
    case $pgac_option in
      -L*) LDFLAGS="$LDFLAGS $pgac_option";;
    esac
  done
fi


#
# XSLT
#
PGAC_ARG_BOOL(with, libxslt, no, [use XSLT support when building contrib/xml2],
              [AC_DEFINE([USE_LIBXSLT], 1, [Define to 1 to use XSLT support when building contrib/xml2. (--with-libxslt)])])


AC_SUBST(with_libxslt)

#
# tzdata
#
PGAC_ARG_REQ(with, system-tzdata,
             [DIR], [use system time zone data in DIR])
AC_SUBST(with_system_tzdata)

#
# Zlib
#
PGAC_ARG_BOOL(with, zlib, yes,
              [do not use Zlib])
AC_SUBST(with_zlib)

#
# LZ4
#
AC_MSG_CHECKING([whether to build with LZ4 support])
PGAC_ARG_BOOL(with, lz4, no, [build with LZ4 support],
              [AC_DEFINE([USE_LZ4], 1, [Define to 1 to build with LZ4 support. (--with-lz4)])])
AC_MSG_RESULT([$with_lz4])
AC_SUBST(with_lz4)

if test "$with_lz4" = yes; then
  PKG_CHECK_MODULES(LZ4, liblz4)
  # We only care about -I, -D, and -L switches;
  # note that -llz4 will be added by AC_CHECK_LIB below.
  for pgac_option in $LZ4_CFLAGS; do
    case $pgac_option in
      -I*|-D*) CPPFLAGS="$CPPFLAGS $pgac_option";;
    esac
  done
  for pgac_option in $LZ4_LIBS; do
    case $pgac_option in
      -L*) LDFLAGS="$LDFLAGS $pgac_option";;
    esac
  done
fi

#
# ZSTD
#
AC_MSG_CHECKING([whether to build with ZSTD support])
PGAC_ARG_BOOL(with, zstd, no, [build with ZSTD support],
              [AC_DEFINE([USE_ZSTD], 1, [Define to 1 to build with ZSTD support. (--with-zstd)])])
AC_MSG_RESULT([$with_zstd])
AC_SUBST(with_zstd)

if test "$with_zstd" = yes; then
  PKG_CHECK_MODULES(ZSTD, libzstd >= 1.4.0)
  # We only care about -I, -D, and -L switches;
  # note that -lzstd will be added by AC_CHECK_LIB below.
  for pgac_option in $ZSTD_CFLAGS; do
    case $pgac_option in
      -I*|-D*) CPPFLAGS="$CPPFLAGS $pgac_option";;
    esac
  done
  for pgac_option in $ZSTD_LIBS; do
    case $pgac_option in
      -L*) LDFLAGS="$LDFLAGS $pgac_option";;
    esac
  done
fi
#
# Assignments
#

CPPFLAGS="$CPPFLAGS $INCLUDES"
LDFLAGS="$LDFLAGS $LIBDIRS"

AC_ARG_VAR(LDFLAGS_EX, [extra linker flags for linking executables only])
AC_ARG_VAR(LDFLAGS_SL, [extra linker flags for linking shared libraries only])

PGAC_CHECK_STRIP
AC_CHECK_TOOL(AR, ar, ar)
if test "$PORTNAME" = "win32"; then
  AC_CHECK_TOOL(WINDRES, windres, windres)
fi

AC_PROG_INSTALL
# When Autoconf chooses install-sh as install program it tries to generate
# a relative path to it in each makefile where it substitutes it. This clashes
# with our Makefile.global concept. This workaround helps.
case $INSTALL in
  *install-sh*) install_bin='';;
  *) install_bin=$INSTALL;;
esac
AC_SUBST(install_bin)

PGAC_PATH_PROGS(TAR, tar)
AC_PROG_LN_S
AC_PROG_MKDIR_P
# When Autoconf chooses install-sh as mkdir -p program it tries to generate
# a relative path to it in each makefile where it substitutes it. This clashes
# with our Makefile.global concept. This workaround helps.
case $MKDIR_P in
  *install-sh*) MKDIR_P='\${SHELL} \${top_srcdir}/config/install-sh -c -d';;
esac

PGAC_PATH_BISON
PGAC_PATH_FLEX

PGAC_PATH_PERL
if test "$with_perl" = yes; then
  if test -z "$PERL"; then
    AC_MSG_ERROR([Perl not found])
  fi
  PGAC_CHECK_PERL_CONFIGS([archlibexp,privlibexp,useshrplib])
  if test "$perl_useshrplib" != yes && test "$perl_useshrplib" != true; then
    AC_MSG_ERROR([cannot build PL/Perl because libperl is not a shared library
You might have to rebuild your Perl installation.  Refer to the
documentation for details.  Use --without-perl to disable building
PL/Perl.])
  fi
  # On most platforms, archlibexp is also where the Perl include files live ...
  perl_includespec="-I$perl_archlibexp/CORE"
  # ... but on newer macOS versions, we must use -iwithsysroot to look
  # under $PG_SYSROOT
  if test \! -f "$perl_archlibexp/CORE/perl.h" ; then
    if test -f "$PG_SYSROOT$perl_archlibexp/CORE/perl.h" ; then
      perl_includespec="-iwithsysroot $perl_archlibexp/CORE"
    fi
  fi
  AC_SUBST(perl_includespec)dnl
  PGAC_CHECK_PERL_EMBED_CCFLAGS
  PGAC_CHECK_PERL_EMBED_LDFLAGS
fi

if test "$with_python" = yes; then
  PGAC_PATH_PYTHON
  PGAC_CHECK_PYTHON_EMBED_SETUP
fi

if test x"$cross_compiling" = x"yes" && test -z "$with_system_tzdata"; then
  PGAC_PATH_PROGS(ZIC, zic)
  if test -z "$ZIC"; then
    AC_MSG_ERROR([
When cross-compiling, either use the option --with-system-tzdata to use
existing time-zone data, or set the environment variable ZIC to a zic
program to use during the build.])
  fi
fi

#
# Pthreads
#
# For each platform, we need to know about any special compile and link
# libraries, and whether the normal C function names are thread-safe.
# WIN32 doesn't need the pthread tests;  it always uses threads
#
# These tests are run before the library-tests, because linking with the
# other libraries can pull in the pthread functions as a side-effect.  We
# want to use the -pthread or similar flags directly, and not rely on
# the side-effects of linking with some other library.

dnl note: We have to use AS_IF here rather than plain if. The AC_CHECK_HEADER
dnl invocation below is the first one in the script, and autoconf generates
dnl additional code for that, which must not be inside the if-block. AS_IF
dnl knows how to do that.
AS_IF([test "$PORTNAME" != "win32"],
[ # then
AX_PTHREAD	# set thread flags

# Some platforms use these, so just define them.  They can't hurt if they
# are not supported.
PTHREAD_CFLAGS="$PTHREAD_CFLAGS -D_REENTRANT -D_THREAD_SAFE"

# Check for *_r functions
_CFLAGS="$CFLAGS"
_LIBS="$LIBS"
CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
LIBS="$LIBS $PTHREAD_LIBS"

AC_CHECK_HEADER(pthread.h, [], [AC_MSG_ERROR([
pthread.h not found])])

AC_CHECK_FUNCS([strerror_r])

# Do test here with the proper thread flags
PGAC_FUNC_STRERROR_R_INT

CFLAGS="$_CFLAGS"
LIBS="$_LIBS"

], [ # else
# do not use values from template file
PTHREAD_CFLAGS=
PTHREAD_LIBS=
]) # fi

AC_SUBST(PTHREAD_CFLAGS)
AC_SUBST(PTHREAD_LIBS)


##
## Libraries
##
## Most libraries are included only if they demonstrably provide a function
## we need, but libm is an exception: always include it, because there are
## too many compilers that play cute optimization games that will break
## probes for standard functions such as pow().
##

AC_CHECK_LIB(m, main)
AC_SEARCH_LIBS(setproctitle, util)
# gcc/clang's sanitizer helper library provides dlopen but not dlsym, thus
# when enabling asan the dlopen check doesn't notice that -ldl is actually
# required. Just checking for dlsym() ought to suffice.
AC_SEARCH_LIBS(dlsym, dl)
AC_SEARCH_LIBS(socket, [socket ws2_32])
AC_SEARCH_LIBS(getopt_long, [getopt gnugetopt])
AC_SEARCH_LIBS(shm_open, rt)
AC_SEARCH_LIBS(shm_unlink, rt)
AC_SEARCH_LIBS(clock_gettime, rt)
# Cygwin:
AC_SEARCH_LIBS(shmget, cygipc)
# *BSD:
AC_SEARCH_LIBS(backtrace_symbols, execinfo)

AC_SEARCH_LIBS(pthread_barrier_wait, pthread)

if test "$with_readline" = yes; then
  PGAC_CHECK_READLINE
  if test x"$pgac_cv_check_readline" = x"no"; then
    AC_MSG_ERROR([readline library not found
If you have readline already installed, see config.log for details on the
failure.  It is possible the compiler isn't looking in the proper directory.
Use --without-readline to disable readline support.])
  fi
fi

if test "$with_zlib" = yes; then
  AC_CHECK_LIB(z, inflate, [],
               [AC_MSG_ERROR([zlib library not found
If you have zlib already installed, see config.log for details on the
failure.  It is possible the compiler isn't looking in the proper directory.
Use --without-zlib to disable zlib support.])])
fi

if test "$with_gssapi" = yes ; then
  if test "$PORTNAME" != "win32"; then
    AC_SEARCH_LIBS(gss_store_cred_into, [gssapi_krb5 gss 'gssapi -lkrb5 -lcrypto'], [],
                   [AC_MSG_ERROR([could not find function 'gss_store_cred_into' required for GSSAPI])])
  else
    LIBS="$LIBS -lgssapi32"
  fi
fi

#
# SSL Library
#
# There is currently only one supported SSL/TLS library: OpenSSL.
#
PGAC_ARG_REQ(with, ssl, [LIB], [use LIB for SSL/TLS support (openssl)])
if test x"$with_ssl" = x"" ; then
  with_ssl=no
fi
PGAC_ARG_BOOL(with, openssl, no, [obsolete spelling of --with-ssl=openssl])
if test "$with_openssl" = yes ; then
  with_ssl=openssl
fi

if test "$with_ssl" = openssl ; then
  dnl Order matters!
  # Minimum required OpenSSL version is 1.0.2
  AC_DEFINE(OPENSSL_API_COMPAT, [0x10002000L],
            [Define to the OpenSSL API version in use. This avoids deprecation warnings from newer OpenSSL versions.])
  if test "$PORTNAME" != "win32"; then
     AC_CHECK_LIB(crypto, CRYPTO_new_ex_data, [], [AC_MSG_ERROR([library 'crypto' is required for OpenSSL])])
     AC_CHECK_LIB(ssl,    SSL_new, [], [AC_MSG_ERROR([library 'ssl' is required for OpenSSL])])
  else
     AC_SEARCH_LIBS(CRYPTO_new_ex_data, [eay32 crypto], [], [AC_MSG_ERROR([library 'eay32' or 'crypto' is required for OpenSSL])])
     AC_SEARCH_LIBS(SSL_new, [ssleay32 ssl], [], [AC_MSG_ERROR([library 'ssleay32' or 'ssl' is required for OpenSSL])])
  fi
  # Function introduced in OpenSSL 1.0.2, not in LibreSSL.
  AC_CHECK_FUNCS([SSL_CTX_set_cert_cb])
  # Functions introduced in OpenSSL 1.1.0. We used to check for
  # OPENSSL_VERSION_NUMBER, but that didn't work with 1.1.0, because LibreSSL
  # defines OPENSSL_VERSION_NUMBER to claim version 2.0.0, even though it
  # doesn't have these OpenSSL 1.1.0 functions. So check for individual
  # functions.
  AC_CHECK_FUNCS([OPENSSL_init_ssl BIO_meth_new ASN1_STRING_get0_data HMAC_CTX_new HMAC_CTX_free])
  # OpenSSL versions before 1.1.0 required setting callback functions, for
  # thread-safety. In 1.1.0, it's no longer required, and CRYPTO_lock()
  # function was removed.
  AC_CHECK_FUNCS([CRYPTO_lock])
  # Function introduced in OpenSSL 1.1.1.
  AC_CHECK_FUNCS([X509_get_signature_info SSL_CTX_set_num_tickets])
  AC_DEFINE([USE_OPENSSL], 1, [Define to 1 to build with OpenSSL support. (--with-ssl=openssl)])
elif test "$with_ssl" != no ; then
  AC_MSG_ERROR([--with-ssl must specify openssl])
fi
AC_SUBST(with_ssl)

if test "$with_pam" = yes ; then
  AC_CHECK_LIB(pam,    pam_start, [], [AC_MSG_ERROR([library 'pam' is required for PAM])])
fi

if test "$with_libxml" = yes ; then
  AC_CHECK_LIB(xml2, xmlSaveToBuffer, [], [AC_MSG_ERROR([library 'xml2' (version >= 2.6.23) is required for XML support])])
fi

if test "$with_libxslt" = yes ; then
  AC_CHECK_LIB(xslt, xsltCleanupGlobals, [], [AC_MSG_ERROR([library 'xslt' is required for XSLT support])])
fi

if test "$with_lz4" = yes ; then
  AC_CHECK_LIB(lz4, LZ4_compress_default, [], [AC_MSG_ERROR([library 'lz4' is required for LZ4 support])])
fi

if test "$with_zstd" = yes ; then
  AC_CHECK_LIB(zstd, ZSTD_compress, [], [AC_MSG_ERROR([library 'zstd' is required for ZSTD support])])
fi

# Note: We can test for libldap_r only after we know PTHREAD_LIBS
if test "$with_ldap" = yes ; then
  _LIBS="$LIBS"
  if test "$PORTNAME" != "win32"; then
    AC_CHECK_LIB(ldap, ldap_bind, [],
		 [AC_MSG_ERROR([library 'ldap' is required for LDAP])],
		 [$EXTRA_LDAP_LIBS])
    LDAP_LIBS_BE="-lldap $EXTRA_LDAP_LIBS"
    # This test is carried out against libldap.
    AC_CHECK_FUNCS([ldap_initialize])
    # The separate ldap_r library only exists in OpenLDAP < 2.5, and if we
    # have 2.5 or later, we shouldn't even probe for ldap_r (we might find a
    # library from a separate OpenLDAP installation).  The most reliable
    # way to check that is to check for a function introduced in 2.5.
    AC_CHECK_FUNC([ldap_verify_credentials],
		  [thread_safe_libldap=yes],
		  [thread_safe_libldap=no])
    if test "$thread_safe_libldap" = no; then
      # Use ldap_r for FE if available, else assume ldap is thread-safe.
      # On some platforms ldap_r fails to link without PTHREAD_LIBS.
      LIBS="$_LIBS"
      AC_CHECK_LIB(ldap_r, ldap_bind,
		   [LDAP_LIBS_FE="-lldap_r $EXTRA_LDAP_LIBS"],
		   [LDAP_LIBS_FE="-lldap $EXTRA_LDAP_LIBS"],
		   [$PTHREAD_CFLAGS $PTHREAD_LIBS $EXTRA_LDAP_LIBS])
    else
      LDAP_LIBS_FE="-lldap $EXTRA_LDAP_LIBS"
    fi
  else
    AC_CHECK_LIB(wldap32, ldap_bind, [], [AC_MSG_ERROR([library 'wldap32' is required for LDAP])])
    LDAP_LIBS_FE="-lwldap32"
    LDAP_LIBS_BE="-lwldap32"
  fi
  LIBS="$_LIBS"
fi
AC_SUBST(LDAP_LIBS_FE)
AC_SUBST(LDAP_LIBS_BE)

# for contrib/sepgsql
if test "$with_selinux" = yes; then
  AC_CHECK_LIB(selinux, security_compute_create_name, [],
               [AC_MSG_ERROR([library 'libselinux', version 2.1.10 or newer, is required for SELinux support])])
fi

# for contrib/uuid-ossp
if test "$with_uuid" = bsd ; then
  # On BSD, the UUID functions are in libc
  AC_CHECK_FUNC(uuid_to_string,
    [UUID_LIBS=""],
    [AC_MSG_ERROR([BSD UUID functions are not present])])
elif test "$with_uuid" = e2fs ; then
  # On macOS, the UUID functions are in libc
  AC_CHECK_FUNC(uuid_generate,
    [UUID_LIBS=""],
    [AC_CHECK_LIB(uuid, uuid_generate,
      [UUID_LIBS="-luuid"],
      [AC_MSG_ERROR([library 'uuid' is required for E2FS UUID])])])
elif test "$with_uuid" = ossp ; then
  AC_CHECK_LIB(ossp-uuid, uuid_export,
    [UUID_LIBS="-lossp-uuid"],
    [AC_CHECK_LIB(uuid, uuid_export,
      [UUID_LIBS="-luuid"],
      [AC_MSG_ERROR([library 'ossp-uuid' or 'uuid' is required for OSSP UUID])])])
fi
AC_SUBST(UUID_LIBS)


##
## Header files
##

AC_HEADER_STDBOOL

AC_CHECK_HEADERS(m4_normalize([
	atomic.h
	copyfile.h
	execinfo.h
	getopt.h
	ifaddrs.h
	mbarrier.h
	sys/epoll.h
	sys/event.h
	sys/personality.h
	sys/prctl.h
	sys/procctl.h
	sys/signalfd.h
	sys/ucred.h
	termios.h
	ucred.h
]))

if expr x"$pgac_cv_check_readline" : 'x-lreadline' >/dev/null ; then
  AC_CHECK_HEADERS(readline/readline.h, [],
        [AC_CHECK_HEADERS(readline.h, [],
                [AC_MSG_ERROR([readline header not found
If you have readline already installed, see config.log for details on the
failure.  It is possible the compiler isn't looking in the proper directory.
Use --without-readline to disable readline support.])])])
  AC_CHECK_HEADERS(readline/history.h, [],
        [AC_CHECK_HEADERS(history.h, [],
                [AC_MSG_ERROR([history header not found
If you have readline already installed, see config.log for details on the
failure.  It is possible the compiler isn't looking in the proper directory.
Use --without-readline to disable readline support.])])])
fi

if expr x"$pgac_cv_check_readline" : 'x-ledit' >/dev/null ; then
# Some installations of libedit usurp /usr/include/readline/, which seems
# bad practice, since in combined installations readline will have its headers
# there.  We might have to resort to AC_EGREP checks to make sure we found
# the proper header...
  AC_CHECK_HEADERS(editline/readline.h, [],
        [AC_CHECK_HEADERS(readline.h, [],
                [AC_CHECK_HEADERS(readline/readline.h, [],
                        [AC_MSG_ERROR([readline header not found
If you have libedit already installed, see config.log for details on the
failure.  It is possible the compiler isn't looking in the proper directory.
Use --without-readline to disable libedit support.])])])])
# Note: in a libedit installation, history.h is sometimes a dummy, and may
# not be there at all.  Hence, don't complain if not found.  We must check
# though, since in yet other versions it is an independent header.
  AC_CHECK_HEADERS(editline/history.h, [],
        [AC_CHECK_HEADERS(history.h, [],
                [AC_CHECK_HEADERS(readline/history.h)])])
fi

if test "$with_zlib" = yes; then
  AC_CHECK_HEADER(zlib.h, [], [AC_MSG_ERROR([zlib header not found
If you have zlib already installed, see config.log for details on the
failure.  It is possible the compiler isn't looking in the proper directory.
Use --without-zlib to disable zlib support.])])
fi

PGAC_PATH_PROGS(LZ4, lz4)
if test "$with_lz4" = yes; then
  AC_CHECK_HEADER(lz4.h, [], [AC_MSG_ERROR([lz4.h header file is required for LZ4])])
fi

PGAC_PATH_PROGS(ZSTD, zstd)
if test "$with_zstd" = yes; then
  AC_CHECK_HEADER(zstd.h, [], [AC_MSG_ERROR([zstd.h header file is required for ZSTD])])
fi

if test "$with_gssapi" = yes ; then
  AC_CHECK_HEADERS(gssapi/gssapi.h, [],
	[AC_CHECK_HEADERS(gssapi.h, [], [AC_MSG_ERROR([gssapi.h header file is required for GSSAPI])])])
  AC_CHECK_HEADERS(gssapi/gssapi_ext.h, [],
	[AC_CHECK_HEADERS(gssapi_ext.h, [], [AC_MSG_ERROR([gssapi_ext.h header file is required for GSSAPI])])])
fi

PGAC_PATH_PROGS(OPENSSL, openssl)
pgac_openssl_version="$($OPENSSL version 2> /dev/null || echo openssl not found)"
AC_MSG_NOTICE([using openssl: $pgac_openssl_version])
if test "$with_ssl" = openssl ; then
  AC_CHECK_HEADER(openssl/ssl.h, [], [AC_MSG_ERROR([header file <openssl/ssl.h> is required for OpenSSL])])
  AC_CHECK_HEADER(openssl/err.h, [], [AC_MSG_ERROR([header file <openssl/err.h> is required for OpenSSL])])
fi

if test "$with_pam" = yes ; then
  AC_CHECK_HEADERS(security/pam_appl.h, [],
                   [AC_CHECK_HEADERS(pam/pam_appl.h, [],
                                     [AC_MSG_ERROR([header file <security/pam_appl.h> or <pam/pam_appl.h> is required for PAM.])])])
fi

if test "$with_bsd_auth" = yes ; then
  AC_CHECK_HEADER(bsd_auth.h, [], [AC_MSG_ERROR([header file <bsd_auth.h> is required for BSD Authentication support])])
fi

if test "$with_systemd" = yes ; then
  AC_CHECK_HEADER(systemd/sd-daemon.h, [], [AC_MSG_ERROR([header file <systemd/sd-daemon.h> is required for systemd support])])
fi

if test "$with_libxml" = yes ; then
  AC_CHECK_HEADER(libxml/parser.h, [], [AC_MSG_ERROR([header file <libxml/parser.h> is required for XML support])])
fi

if test "$with_libxslt" = yes ; then
  AC_CHECK_HEADER(libxslt/xslt.h, [], [AC_MSG_ERROR([header file <libxslt/xslt.h> is required for XSLT support])])
fi

if test "$with_ldap" = yes ; then
  if test "$PORTNAME" != "win32"; then
     AC_CHECK_HEADER(ldap.h, [],
                     [AC_MSG_ERROR([header file <ldap.h> is required for LDAP])])
     PGAC_LDAP_SAFE
  else
     AC_CHECK_HEADER(winldap.h, [],
                     [AC_MSG_ERROR([header file <winldap.h> is required for LDAP])],
                     [AC_INCLUDES_DEFAULT
#include <windows.h>
                     ])
  fi
fi

if test "$with_bonjour" = yes ; then
  AC_CHECK_HEADER(dns_sd.h, [], [AC_MSG_ERROR([header file <dns_sd.h> is required for Bonjour])])
dnl At some point we might add something like
dnl AC_SEARCH_LIBS(DNSServiceRegister, dns_sd)
dnl but right now, what that would mainly accomplish is to encourage
dnl people to try to use the avahi implementation, which does not work.
dnl If you want to use Apple's own Bonjour code on another platform,
dnl just add -ldns_sd to LIBS manually.
fi

# for contrib/uuid-ossp
if test "$with_uuid" = bsd ; then
  AC_CHECK_HEADERS(uuid.h,
    [AC_EGREP_HEADER([uuid_to_string], uuid.h, [],
      [AC_MSG_ERROR([header file <uuid.h> does not match BSD UUID library])])],
    [AC_MSG_ERROR([header file <uuid.h> is required for BSD UUID])])
elif test "$with_uuid" = e2fs ; then
  AC_CHECK_HEADERS(uuid/uuid.h,
    [AC_EGREP_HEADER([uuid_generate], uuid/uuid.h, [],
      [AC_MSG_ERROR([header file <uuid/uuid.h> does not match E2FS UUID library])])],
    [AC_CHECK_HEADERS(uuid.h,
      [AC_EGREP_HEADER([uuid_generate], uuid.h, [],
        [AC_MSG_ERROR([header file <uuid.h> does not match E2FS UUID library])])],
      [AC_MSG_ERROR([header file <uuid/uuid.h> or <uuid.h> is required for E2FS UUID])])])
elif test "$with_uuid" = ossp ; then
  AC_CHECK_HEADERS(ossp/uuid.h,
    [AC_EGREP_HEADER([uuid_export], ossp/uuid.h, [],
      [AC_MSG_ERROR([header file <ossp/uuid.h> does not match OSSP UUID library])])],
    [AC_CHECK_HEADERS(uuid.h,
      [AC_EGREP_HEADER([uuid_export], uuid.h, [],
        [AC_MSG_ERROR([header file <uuid.h> does not match OSSP UUID library])])],
      [AC_MSG_ERROR([header file <ossp/uuid.h> or <uuid.h> is required for OSSP UUID])])])
fi

if test "$PORTNAME" = "win32" ; then
   AC_CHECK_HEADERS(crtdefs.h)
fi

##
## Types, structures, compiler characteristics
##

m4_defun([AC_PROG_CC_STDC], []) dnl We don't want that.
AC_C_BIGENDIAN
AC_C_INLINE
PGAC_PRINTF_ARCHETYPE
PGAC_C_STATIC_ASSERT
PGAC_C_TYPEOF
PGAC_C_TYPES_COMPATIBLE
PGAC_C_BUILTIN_CONSTANT_P
PGAC_C_BUILTIN_UNREACHABLE
PGAC_C_COMPUTED_GOTO
PGAC_STRUCT_TIMEZONE
PGAC_UNION_SEMUN
AC_CHECK_TYPES(socklen_t, [], [], [#include <sys/socket.h>])
PGAC_STRUCT_SOCKADDR_SA_LEN

PGAC_TYPE_LOCALE_T

# MSVC doesn't cope well with defining restrict to __restrict, the
# spelling it understands, because it conflicts with
# __declspec(restrict). Therefore we define pg_restrict to the
# appropriate definition, which presumably won't conflict.
AC_C_RESTRICT
if test "$ac_cv_c_restrict" = "no"; then
  pg_restrict=""
else
  pg_restrict="$ac_cv_c_restrict"
fi
AC_DEFINE_UNQUOTED([pg_restrict], [$pg_restrict],
[Define to keyword to use for C99 restrict support, or to nothing if not
supported])

AC_CHECK_TYPES([struct option], [], [],
[#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif])

case $host_cpu in
  x86_64)
    # On x86_64, check if we can compile a popcntq instruction
    AC_CACHE_CHECK([whether assembler supports x86_64 popcntq],
                   [pgac_cv_have_x86_64_popcntq],
    [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],
	[long long x = 1; long long r;
	 __asm__ __volatile__ (" popcntq %1,%0\n" : "=q"(r) : "rm"(x));])],
	[pgac_cv_have_x86_64_popcntq=yes],
	[pgac_cv_have_x86_64_popcntq=no])])
    if test x"$pgac_cv_have_x86_64_popcntq" = xyes ; then
	AC_DEFINE(HAVE_X86_64_POPCNTQ, 1, [Define to 1 if the assembler supports X86_64's POPCNTQ instruction.])
    fi
  ;;
  ppc*|powerpc*)
    # On PPC, check if compiler accepts "i"(x) when __builtin_constant_p(x).
    AC_CACHE_CHECK([whether __builtin_constant_p(x) implies "i"(x) acceptance],
                   [pgac_cv_have_i_constraint__builtin_constant_p],
    [AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
    [static inline int
     addi(int ra, int si)
     {
         int res = 0;
         if (__builtin_constant_p(si))
             __asm__ __volatile__(
                 " addi %0,%1,%2\n" : "=r"(res) : "b"(ra), "i"(si));
         return res;
     }
     int test_adds(int x) { return addi(3, x) + addi(x, 5); }], [])],
    [pgac_cv_have_i_constraint__builtin_constant_p=yes],
    [pgac_cv_have_i_constraint__builtin_constant_p=no])])
    if test x"$pgac_cv_have_i_constraint__builtin_constant_p" = xyes ; then
      AC_DEFINE(HAVE_I_CONSTRAINT__BUILTIN_CONSTANT_P, 1,
                [Define to 1 if __builtin_constant_p(x) implies "i"(x) acceptance.])
    fi
  ;;
esac

# Check largefile support.  You might think this is a system service not a
# compiler characteristic, but you'd be wrong.  We must check this before
# probing existence of related functions such as fseeko, since the largefile
# defines can affect what is generated for that.
if test "$PORTNAME" != "win32"; then
   AC_SYS_LARGEFILE
   dnl Autoconf 2.69's AC_SYS_LARGEFILE believes it's a good idea to #define
   dnl _DARWIN_USE_64_BIT_INODE, but it isn't: on macOS 10.5 that activates a
   dnl bug that causes readdir() to sometimes return EINVAL.  On later macOS
   dnl versions where the feature actually works, it's on by default anyway.
   AH_VERBATIM([_DARWIN_USE_64_BIT_INODE],[])
fi

dnl Check for largefile support (must be after AC_SYS_LARGEFILE)
AC_CHECK_SIZEOF([off_t])

# If we don't have largefile support, can't handle segment size >= 2GB.
if test "$ac_cv_sizeof_off_t" -lt 8; then
  if expr $RELSEG_SIZE '*' $blocksize '>=' 2 '*' 1024 '*' 1024; then
    AC_MSG_ERROR([Large file support is not enabled. Segment size cannot be larger than 1GB.])
  fi
fi

AC_CHECK_SIZEOF([bool], [],
[#ifdef HAVE_STDBOOL_H
#include <stdbool.h>
#endif])

dnl We use <stdbool.h> if we have it and it declares type bool as having
dnl size 1.  Otherwise, c.h will fall back to declaring bool as unsigned char.
if test "$ac_cv_header_stdbool_h" = yes -a "$ac_cv_sizeof_bool" = 1; then
  AC_DEFINE([PG_USE_STDBOOL], 1,
            [Define to 1 to use <stdbool.h> to define type bool.])
fi


##
## Functions, global variables
##

PGAC_VAR_INT_TIMEZONE
PGAC_FUNC_WCSTOMBS_L

# Some versions of libedit contain strlcpy(), setproctitle(), and other
# symbols that that library has no business exposing to the world.  Pending
# acquisition of a clue by those developers, ignore libedit (including its
# possible alias of libreadline) while checking for everything else.
LIBS_including_readline="$LIBS"
LIBS=`echo "$LIBS" | sed -e 's/-ledit//g' -e 's/-lreadline//g'`

AC_CHECK_FUNCS(m4_normalize([
	backtrace_symbols
	copyfile
	copy_file_range
	getifaddrs
	getpeerucred
	inet_pton
	kqueue
	mbstowcs_l
	memset_s
	posix_fallocate
	ppoll
	pthread_is_threaded_np
	setproctitle
	setproctitle_fast
	strchrnul
	strsignal
	syncfs
	sync_file_range
	uselocale
	wcstombs_l
]))

# These typically are compiler builtins, for which AC_CHECK_FUNCS fails.
PGAC_CHECK_BUILTIN_FUNC([__builtin_bswap16], [int x])
PGAC_CHECK_BUILTIN_FUNC([__builtin_bswap32], [int x])
PGAC_CHECK_BUILTIN_FUNC([__builtin_bswap64], [long int x])
# We assume that we needn't test all widths of these explicitly:
PGAC_CHECK_BUILTIN_FUNC([__builtin_clz], [unsigned int x])
PGAC_CHECK_BUILTIN_FUNC([__builtin_ctz], [unsigned int x])
PGAC_CHECK_BUILTIN_FUNC([__builtin_popcount], [unsigned int x])
# __builtin_frame_address may draw a diagnostic for non-constant argument,
# so it needs a different test function.
PGAC_CHECK_BUILTIN_FUNC_PTR([__builtin_frame_address], [0])

# We require 64-bit fseeko() to be available, but run this check anyway
# in case it finds that _LARGEFILE_SOURCE has to be #define'd for that.
AC_FUNC_FSEEKO

# posix_fadvise() is a no-op on Solaris, so don't incur function overhead
# by calling it, 2009-04-02
# http://src.opensolaris.org/source/xref/onnv/onnv-gate/usr/src/lib/libc/port/gen/posix_fadvise.c
dnl must use AS_IF here, else AC_REQUIRES inside AC_CHECK_DECLS malfunctions
AS_IF([test "$PORTNAME" != "solaris"], [
AC_CHECK_FUNCS(posix_fadvise)
AC_CHECK_DECLS(posix_fadvise, [], [], [#include <fcntl.h>])
]) # fi

AC_CHECK_DECLS(fdatasync, [], [], [#include <unistd.h>])
AC_CHECK_DECLS([strlcat, strlcpy, strnlen, strsep])

# We can't use AC_CHECK_FUNCS to detect these functions, because it
# won't handle deployment target restrictions on macOS
AC_CHECK_DECLS([preadv], [], [], [#include <sys/uio.h>])
AC_CHECK_DECLS([pwritev], [], [], [#include <sys/uio.h>])

# This is probably only present on macOS, but may as well check always
AC_CHECK_DECLS(F_FULLFSYNC, [], [], [#include <fcntl.h>])

AC_REPLACE_FUNCS(m4_normalize([
	explicit_bzero
	getopt
	getpeereid
	inet_aton
	mkdtemp
	strlcat
	strlcpy
	strnlen
	strsep
]))

AC_REPLACE_FUNCS(pthread_barrier_wait)

if test "$PORTNAME" = "win32" -o "$PORTNAME" = "cygwin"; then
	# Cygwin and (apparently, based on test results) Mingw both
	# have a broken strtof(), so substitute its implementation.
	# That's not a perfect fix, since it doesn't avoid double-rounding,
	# but we have no better options.
	AC_LIBOBJ([strtof])
	AC_MSG_NOTICE([On $host_os we will use our strtof wrapper.])
fi

# Similarly, use system's getopt_long() only if system provides struct option.
if test x"$ac_cv_type_struct_option" = xyes ; then
  AC_REPLACE_FUNCS([getopt_long])
else
  AC_LIBOBJ(getopt_long)
fi

# On OpenBSD and Solaris, getopt() doesn't do what we want for long options
# (i.e., allow '-' as a flag character), so use our version on those platforms.
if test "$PORTNAME" = "openbsd" -o "$PORTNAME" = "solaris"; then
  AC_LIBOBJ(getopt)
fi

# mingw has adopted a GNU-centric interpretation of optind/optreset,
# so always use our version on Windows.
if test "$PORTNAME" = "win32"; then
  AC_LIBOBJ(getopt)
  AC_LIBOBJ(getopt_long)
fi

# Win32 (really MinGW) support
if test "$PORTNAME" = "win32"; then
  AC_CHECK_FUNCS(_configthreadlocale)
  AC_LIBOBJ(dirmod)
  AC_LIBOBJ(kill)
  AC_LIBOBJ(open)
  AC_LIBOBJ(system)
  AC_LIBOBJ(win32common)
  AC_LIBOBJ(win32dlopen)
  AC_LIBOBJ(win32env)
  AC_LIBOBJ(win32error)
  AC_LIBOBJ(win32fdatasync)
  AC_LIBOBJ(win32gai_strerror)
  AC_LIBOBJ(win32getrusage)
  AC_LIBOBJ(win32link)
  AC_LIBOBJ(win32ntdll)
  AC_LIBOBJ(win32pread)
  AC_LIBOBJ(win32pwrite)
  AC_LIBOBJ(win32security)
  AC_LIBOBJ(win32setlocale)
  AC_LIBOBJ(win32stat)
fi
# Cygwin needs only a bit of that
if test "$PORTNAME" = "cygwin"; then
  AC_LIBOBJ(dirmod)
fi

AC_CHECK_FUNC(syslog,
              [AC_CHECK_HEADER(syslog.h,
                               [AC_DEFINE(HAVE_SYSLOG, 1, [Define to 1 if you have the syslog interface.])])])

AC_CACHE_CHECK([for opterr], pgac_cv_var_int_opterr,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <unistd.h>],
  [extern int opterr; opterr = 1;])],
  [pgac_cv_var_int_opterr=yes],
  [pgac_cv_var_int_opterr=no])])
if test x"$pgac_cv_var_int_opterr" = x"yes"; then
  AC_DEFINE(HAVE_INT_OPTERR, 1, [Define to 1 if you have the global variable 'int opterr'.])
fi

AC_CACHE_CHECK([for optreset], pgac_cv_var_int_optreset,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <unistd.h>],
  [extern int optreset; optreset = 1;])],
  [pgac_cv_var_int_optreset=yes],
  [pgac_cv_var_int_optreset=no])])
if test x"$pgac_cv_var_int_optreset" = x"yes"; then
  AC_DEFINE(HAVE_INT_OPTRESET, 1, [Define to 1 if you have the global variable 'int optreset'.])
fi

if test "$with_icu" = yes; then
  ac_save_CPPFLAGS=$CPPFLAGS
  CPPFLAGS="$ICU_CFLAGS $CPPFLAGS"

  # Verify we have ICU's header files
  AC_CHECK_HEADER(unicode/ucol.h, [],
        [AC_MSG_ERROR([header file <unicode/ucol.h> is required for ICU])])

  CPPFLAGS=$ac_save_CPPFLAGS
fi

if test "$with_llvm" = yes; then
  PGAC_CHECK_LLVM_FUNCTIONS()
fi

# Lastly, restore full LIBS list and check for readline/libedit symbols
LIBS="$LIBS_including_readline"

if test "$with_readline" = yes; then
  PGAC_READLINE_VARIABLES
  AC_CHECK_FUNCS(m4_normalize([
	append_history
	history_truncate_file
	rl_completion_matches
	rl_filename_completion_function
	rl_reset_screen_size
	rl_variable_bind
  ]))
fi


# This test makes sure that run tests work at all.  Sometimes a shared
# library is found by the linker, but the runtime linker can't find it.
# This check should come after all modifications of compiler or linker
# variables, and before any other run tests.
AC_MSG_CHECKING([test program])
AC_RUN_IFELSE([AC_LANG_SOURCE([int main() { return 0; }])],
[AC_MSG_RESULT(ok)],
[AC_MSG_RESULT(failed)
AC_MSG_ERROR([[
Could not execute a simple test program.  This may be a problem
related to locating shared libraries.  Check the file 'config.log'
for the exact reason.]])],
[AC_MSG_RESULT([cross-compiling])])

# --------------------
# Run tests below here
# --------------------

dnl Check to see if we have a working 64-bit integer type.
dnl Since Postgres 8.4, we no longer support compilers without a working
dnl 64-bit type; but we have to determine whether that type is called
dnl "long int" or "long long int".

PGAC_TYPE_64BIT_INT([long int])

if test x"$HAVE_LONG_INT_64" = x"yes" ; then
  pg_int64_type="long int"
else
  PGAC_TYPE_64BIT_INT([long long int])
  if test x"$HAVE_LONG_LONG_INT_64" = x"yes" ; then
    pg_int64_type="long long int"
  else
    AC_MSG_ERROR([Cannot find a working 64-bit integer type.])
  fi
fi

AC_DEFINE_UNQUOTED(PG_INT64_TYPE, $pg_int64_type,
  [Define to the name of a signed 64-bit integer type.])

# Select the printf length modifier that goes with that, too.
if test x"$pg_int64_type" = x"long long int" ; then
  INT64_MODIFIER='"ll"'
else
  INT64_MODIFIER='"l"'
fi

AC_DEFINE_UNQUOTED(INT64_MODIFIER, $INT64_MODIFIER,
                   [Define to the appropriate printf length modifier for 64-bit ints.])

# has to be down here, rather than with the other builtins, because
# the test uses PG_INT64_TYPE.
PGAC_C_BUILTIN_OP_OVERFLOW

# Check size of void *, size_t (enables tweaks for > 32bit address space)
AC_CHECK_SIZEOF([void *])
AC_CHECK_SIZEOF([size_t])
AC_CHECK_SIZEOF([long])

# Determine memory alignment requirements for the basic C data types.

AC_CHECK_ALIGNOF(short)
AC_CHECK_ALIGNOF(int)
AC_CHECK_ALIGNOF(long)
if test x"$HAVE_LONG_LONG_INT_64" = x"yes" ; then
  AC_CHECK_ALIGNOF(long long int)
fi
AC_CHECK_ALIGNOF(double)

# Compute maximum alignment of any basic type.
#
# We require 'double' to have the strictest alignment among the basic types,
# because otherwise the C ABI might impose 8-byte alignment on some of the
# other C types that correspond to TYPALIGN_DOUBLE SQL types.  That could
# cause a mismatch between the tuple layout and the C struct layout of a
# catalog tuple.  We used to carefully order catalog columns such that any
# fixed-width, attalign=4 columns were at offsets divisible by 8 regardless
# of MAXIMUM_ALIGNOF to avoid that, but we no longer support any platforms
# where TYPALIGN_DOUBLE != MAXIMUM_ALIGNOF.
#
# We assume without checking that long's alignment is at least as strong as
# char, short, or int.  Note that we intentionally do not consider any types
# wider than 64 bits, as allowing MAXIMUM_ALIGNOF to exceed 8 would be too
# much of a penalty for disk and memory space.

MAX_ALIGNOF=$ac_cv_alignof_double

if test $ac_cv_alignof_long -gt $MAX_ALIGNOF ; then
  AC_MSG_ERROR([alignment of 'long' is greater than the alignment of 'double'])
fi
if test x"$HAVE_LONG_LONG_INT_64" = xyes && test $ac_cv_alignof_long_long_int -gt $MAX_ALIGNOF ; then
  AC_MSG_ERROR([alignment of 'long long int' is greater than the alignment of 'double'])
fi
AC_DEFINE_UNQUOTED(MAXIMUM_ALIGNOF, $MAX_ALIGNOF, [Define as the maximum alignment requirement of any C data type.])


# Some platforms predefine the types int8, int16, etc.  Only check
# a (hopefully) representative subset.
AC_CHECK_TYPES([int8, uint8, int64, uint64], [], [],
[#include <stdio.h>])

# Some compilers offer a 128-bit integer scalar type.
PGAC_TYPE_128BIT_INT

# Check for various atomic operations now that we have checked how to declare
# 64bit integers.
PGAC_HAVE_GCC__SYNC_CHAR_TAS
PGAC_HAVE_GCC__SYNC_INT32_TAS
PGAC_HAVE_GCC__SYNC_INT32_CAS
PGAC_HAVE_GCC__SYNC_INT64_CAS
PGAC_HAVE_GCC__ATOMIC_INT32_CAS
PGAC_HAVE_GCC__ATOMIC_INT64_CAS


# Check for x86 cpuid instruction
AC_CACHE_CHECK([for __get_cpuid], [pgac_cv__get_cpuid],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <cpuid.h>],
  [[unsigned int exx[4] = {0, 0, 0, 0};
  __get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
  ]])],
  [pgac_cv__get_cpuid="yes"],
  [pgac_cv__get_cpuid="no"])])
if test x"$pgac_cv__get_cpuid" = x"yes"; then
  AC_DEFINE(HAVE__GET_CPUID, 1, [Define to 1 if you have __get_cpuid.])
fi

AC_CACHE_CHECK([for __get_cpuid_count], [pgac_cv__get_cpuid_count],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <cpuid.h>],
  [[unsigned int exx[4] = {0, 0, 0, 0};
  __get_cpuid_count(7, 0, &exx[0], &exx[1], &exx[2], &exx[3]);
  ]])],
  [pgac_cv__get_cpuid_count="yes"],
  [pgac_cv__get_cpuid_count="no"])])
if test x"$pgac_cv__get_cpuid_count" = x"yes"; then
  AC_DEFINE(HAVE__GET_CPUID_COUNT, 1, [Define to 1 if you have __get_cpuid_count.])
fi

AC_CACHE_CHECK([for __cpuid], [pgac_cv__cpuid],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <intrin.h>],
  [[unsigned int exx[4] = {0, 0, 0, 0};
  __get_cpuid(exx[0], 1);
  ]])],
  [pgac_cv__cpuid="yes"],
  [pgac_cv__cpuid="no"])])
if test x"$pgac_cv__cpuid" = x"yes"; then
  AC_DEFINE(HAVE__CPUID, 1, [Define to 1 if you have __cpuid.])
fi

AC_CACHE_CHECK([for __cpuidex], [pgac_cv__cpuidex],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <intrin.h>],
  [[unsigned int exx[4] = {0, 0, 0, 0};
  __get_cpuidex(exx[0], 7, 0);
  ]])],
  [pgac_cv__cpuidex="yes"],
  [pgac_cv__cpuidex="no"])])
if test x"$pgac_cv__cpuidex" = x"yes"; then
  AC_DEFINE(HAVE__CPUIDEX, 1, [Define to 1 if you have __cpuidex.])
fi

# Check for XSAVE intrinsics
#
CFLAGS_XSAVE=""
PGAC_XSAVE_INTRINSICS([])
if test x"$pgac_xsave_intrinsics" != x"yes"; then
  PGAC_XSAVE_INTRINSICS([-mxsave])
fi
if test x"$pgac_xsave_intrinsics" = x"yes"; then
  AC_DEFINE(HAVE_XSAVE_INTRINSICS, 1, [Define to 1 if you have XSAVE intrinsics.])
fi
AC_SUBST(CFLAGS_XSAVE)

# Check for AVX-512 popcount intrinsics
#
CFLAGS_POPCNT=""
PG_POPCNT_OBJS=""
if test x"$host_cpu" = x"x86_64"; then
  PGAC_AVX512_POPCNT_INTRINSICS([])
  if test x"$pgac_avx512_popcnt_intrinsics" != x"yes"; then
    PGAC_AVX512_POPCNT_INTRINSICS([-mavx512vpopcntdq -mavx512bw])
  fi
  if test x"$pgac_avx512_popcnt_intrinsics" = x"yes"; then
    PG_POPCNT_OBJS="pg_popcount_avx512.o pg_popcount_avx512_choose.o"
    AC_DEFINE(USE_AVX512_POPCNT_WITH_RUNTIME_CHECK, 1, [Define to 1 to use AVX-512 popcount instructions with a runtime check.])
  fi
fi
AC_SUBST(CFLAGS_POPCNT)
AC_SUBST(PG_POPCNT_OBJS)

# Check for Intel SSE 4.2 intrinsics to do CRC calculations.
#
# First check if the _mm_crc32_u8 and _mm_crc32_u64 intrinsics can be used
# with the default compiler flags. If not, check if adding the -msse4.2
# flag helps. CFLAGS_CRC is set to -msse4.2 if that's required.
PGAC_SSE42_CRC32_INTRINSICS([])
if test x"$pgac_sse42_crc32_intrinsics" != x"yes"; then
  PGAC_SSE42_CRC32_INTRINSICS([-msse4.2])
fi

# Are we targeting a processor that supports SSE 4.2? gcc, clang and icc all
# define __SSE4_2__ in that case.
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [
#ifndef __SSE4_2__
#error __SSE4_2__ not defined
#endif
])], [SSE4_2_TARGETED=1])

# Check for ARMv8 CRC Extension intrinsics to do CRC calculations.
#
# First check if __crc32c* intrinsics can be used with the default compiler
# flags. If not, check if adding -march=armv8-a+crc flag helps.
# CFLAGS_CRC is set if the extra flag is required.
PGAC_ARMV8_CRC32C_INTRINSICS([])
if test x"$pgac_armv8_crc32c_intrinsics" != x"yes"; then
  PGAC_ARMV8_CRC32C_INTRINSICS([-march=armv8-a+crc])
fi

# Check for LoongArch CRC intrinsics to do CRC calculations.
#
# Check if __builtin_loongarch_crcc_* intrinsics can be used
# with the default compiler flags.
PGAC_LOONGARCH_CRC32C_INTRINSICS()

AC_SUBST(CFLAGS_CRC)

# Select CRC-32C implementation.
#
# If we are targeting a processor that has Intel SSE 4.2 instructions, we can
# use the special CRC instructions for calculating CRC-32C. If we're not
# targeting such a processor, but we can nevertheless produce code that uses
# the SSE intrinsics, perhaps with some extra CFLAGS, compile both
# implementations and select which one to use at runtime, depending on whether
# SSE 4.2 is supported by the processor we're running on.
#
# Similarly, if we are targeting an ARM processor that has the CRC
# instructions that are part of the ARMv8 CRC Extension, use them. And if
# we're not targeting such a processor, but can nevertheless produce code that
# uses the CRC instructions, compile both, and select at runtime.
#
# You can skip the runtime check by setting the appropriate USE_*_CRC32 flag to 1
# in the template or configure command line.
#
# If we are targeting a LoongArch processor, CRC instructions are
# always available (at least on 64 bit), so no runtime check is needed.
if test x"$USE_SLICING_BY_8_CRC32C" = x"" && test x"$USE_SSE42_CRC32C" = x"" && test x"$USE_SSE42_CRC32C_WITH_RUNTIME_CHECK" = x"" && test x"$USE_ARMV8_CRC32C" = x"" && test x"$USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK" = x"" && test x"$USE_LOONGARCH_CRC32C" = x""; then
  # Use Intel SSE 4.2 if available.
  if test x"$pgac_sse42_crc32_intrinsics" = x"yes" && test x"$SSE4_2_TARGETED" = x"1" ; then
    USE_SSE42_CRC32C=1
  else
    # Intel SSE 4.2, with runtime check? The CPUID instruction is needed for
    # the runtime check.
    if test x"$pgac_sse42_crc32_intrinsics" = x"yes" && (test x"$pgac_cv__get_cpuid" = x"yes" || test x"$pgac_cv__cpuid" = x"yes"); then
      USE_SSE42_CRC32C_WITH_RUNTIME_CHECK=1
    else
      # Use ARM CRC Extension if available.
      if test x"$pgac_armv8_crc32c_intrinsics" = x"yes" && test x"$CFLAGS_CRC" = x""; then
        USE_ARMV8_CRC32C=1
      else
        # ARM CRC Extension, with runtime check?
        if test x"$pgac_armv8_crc32c_intrinsics" = x"yes"; then
          USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK=1
        else
          # LoongArch CRCC instructions.
          if test x"$pgac_loongarch_crc32c_intrinsics" = x"yes"; then
            USE_LOONGARCH_CRC32C=1
          else
            # fall back to slicing-by-8 algorithm, which doesn't require any
            # special CPU support.
            USE_SLICING_BY_8_CRC32C=1
          fi
        fi
      fi
    fi
  fi
fi

# Set PG_CRC32C_OBJS appropriately depending on the selected implementation.
AC_MSG_CHECKING([which CRC-32C implementation to use])
if test x"$USE_SSE42_CRC32C" = x"1"; then
  AC_DEFINE(USE_SSE42_CRC32C, 1, [Define to 1 use Intel SSE 4.2 CRC instructions.])
  PG_CRC32C_OBJS="pg_crc32c_sse42.o"
  AC_MSG_RESULT(SSE 4.2)
else
  if test x"$USE_SSE42_CRC32C_WITH_RUNTIME_CHECK" = x"1"; then
    AC_DEFINE(USE_SSE42_CRC32C_WITH_RUNTIME_CHECK, 1, [Define to 1 to use Intel SSE 4.2 CRC instructions with a runtime check.])
    PG_CRC32C_OBJS="pg_crc32c_sse42.o pg_crc32c_sb8.o pg_crc32c_sse42_choose.o"
    AC_MSG_RESULT(SSE 4.2 with runtime check)
  else
    if test x"$USE_ARMV8_CRC32C" = x"1"; then
      AC_DEFINE(USE_ARMV8_CRC32C, 1, [Define to 1 to use ARMv8 CRC Extension.])
      PG_CRC32C_OBJS="pg_crc32c_armv8.o"
      AC_MSG_RESULT(ARMv8 CRC instructions)
    else
      if test x"$USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK" = x"1"; then
        AC_DEFINE(USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK, 1, [Define to 1 to use ARMv8 CRC Extension with a runtime check.])
        PG_CRC32C_OBJS="pg_crc32c_armv8.o pg_crc32c_sb8.o pg_crc32c_armv8_choose.o"
        AC_MSG_RESULT(ARMv8 CRC instructions with runtime check)
      else
        if test x"$USE_LOONGARCH_CRC32C" = x"1"; then
          AC_DEFINE(USE_LOONGARCH_CRC32C, 1, [Define to 1 to use LoongArch CRCC instructions.])
          PG_CRC32C_OBJS="pg_crc32c_loongarch.o"
          AC_MSG_RESULT(LoongArch CRCC instructions)
        else
          AC_DEFINE(USE_SLICING_BY_8_CRC32C, 1, [Define to 1 to use software CRC-32C implementation (slicing-by-8).])
          PG_CRC32C_OBJS="pg_crc32c_sb8.o"
          AC_MSG_RESULT(slicing-by-8)
        fi
      fi
    fi
  fi
fi
AC_SUBST(PG_CRC32C_OBJS)


# Select semaphore implementation type.
if test "$PORTNAME" != "win32"; then
  if test x"$PREFERRED_SEMAPHORES" = x"NAMED_POSIX" ; then
    # Need sem_open for this
    AC_SEARCH_LIBS(sem_open, [rt pthread], [USE_NAMED_POSIX_SEMAPHORES=1])
  fi
  if test x"$PREFERRED_SEMAPHORES" = x"UNNAMED_POSIX" ; then
    # Need sem_init for this
    AC_SEARCH_LIBS(sem_init, [rt pthread], [USE_UNNAMED_POSIX_SEMAPHORES=1])
  fi
  AC_MSG_CHECKING([which semaphore API to use])
  if test x"$USE_NAMED_POSIX_SEMAPHORES" = x"1" ; then
    AC_DEFINE(USE_NAMED_POSIX_SEMAPHORES, 1, [Define to select named POSIX semaphores.])
    SEMA_IMPLEMENTATION="src/backend/port/posix_sema.c"
    sematype="named POSIX"
  else
    if test x"$USE_UNNAMED_POSIX_SEMAPHORES" = x"1" ; then
      AC_DEFINE(USE_UNNAMED_POSIX_SEMAPHORES, 1, [Define to select unnamed POSIX semaphores.])
      SEMA_IMPLEMENTATION="src/backend/port/posix_sema.c"
      sematype="unnamed POSIX"
    else
      AC_DEFINE(USE_SYSV_SEMAPHORES, 1, [Define to select SysV-style semaphores.])
      SEMA_IMPLEMENTATION="src/backend/port/sysv_sema.c"
      sematype="System V"
    fi
  fi
  AC_MSG_RESULT([$sematype])
else
  AC_DEFINE(USE_WIN32_SEMAPHORES, 1, [Define to select Win32-style semaphores.])
  SEMA_IMPLEMENTATION="src/backend/port/win32_sema.c"
fi


# Select shared-memory implementation type.
if test "$PORTNAME" != "win32"; then
  AC_DEFINE(USE_SYSV_SHARED_MEMORY, 1, [Define to select SysV-style shared memory.])
  SHMEM_IMPLEMENTATION="src/backend/port/sysv_shmem.c"
else
  AC_DEFINE(USE_WIN32_SHARED_MEMORY, 1, [Define to select Win32-style shared memory.])
  SHMEM_IMPLEMENTATION="src/backend/port/win32_shmem.c"
fi

# Select random number source. If a TLS library is used then it will be the
# first choice, else the native platform sources (Windows API or /dev/urandom)
# will be used.
AC_MSG_CHECKING([which random number source to use])
if test x"$with_ssl" = x"openssl" ; then
  AC_MSG_RESULT([OpenSSL])
elif test x"$PORTNAME" = x"win32" ; then
  AC_MSG_RESULT([Windows native])
elif test x"$cross_compiling" = x"yes"; then
  AC_MSG_RESULT([assuming /dev/urandom])
else
  AC_MSG_RESULT([/dev/urandom])
  AC_CHECK_FILE([/dev/urandom], [], [])

  if test x"$ac_cv_file__dev_urandom" = x"no" ; then
    AC_MSG_ERROR([
no source of strong random numbers was found
PostgreSQL can use OpenSSL, native Windows API or /dev/urandom as a source of random numbers.])
  fi
fi

# If not set in template file, set bytes to use libc memset()
if test x"$MEMSET_LOOP_LIMIT" = x"" ; then
  MEMSET_LOOP_LIMIT=1024
fi
AC_DEFINE_UNQUOTED(MEMSET_LOOP_LIMIT, ${MEMSET_LOOP_LIMIT}, [Define bytes to use libc memset().])


if test "$enable_nls" = yes ; then
  PGAC_CHECK_GETTEXT
fi

# Check for Tcl configuration script tclConfig.sh
if test "$with_tcl" = yes; then
    PGAC_PATH_TCLCONFIGSH([$with_tclconfig])
    PGAC_EVAL_TCLCONFIGSH([$TCL_CONFIG_SH],
                          [TCL_INCLUDE_SPEC,TCL_LIBS,TCL_LIB_SPEC,TCL_SHARED_BUILD])
    if test "$TCL_SHARED_BUILD" != 1; then
      AC_MSG_ERROR([cannot build PL/Tcl because Tcl is not a shared library
Use --without-tcl to disable building PL/Tcl.])
    fi
    # now that we have TCL_INCLUDE_SPEC, we can check for <tcl.h>
    ac_save_CPPFLAGS=$CPPFLAGS
    CPPFLAGS="$TCL_INCLUDE_SPEC $CPPFLAGS"
    AC_CHECK_HEADER(tcl.h, [], [AC_MSG_ERROR([header file <tcl.h> is required for Tcl])])
    CPPFLAGS=$ac_save_CPPFLAGS
fi

# check for <perl.h>
if test "$with_perl" = yes; then
  ac_save_CPPFLAGS=$CPPFLAGS
  CPPFLAGS="$CPPFLAGS $perl_includespec"
  AC_CHECK_HEADER(perl.h, [], [AC_MSG_ERROR([header file <perl.h> is required for Perl])],
                  [#include <EXTERN.h>])
  # While we're at it, check that we can link to libperl.
  # On most platforms, if perl.h is there then libperl.so will be too, but at
  # this writing Debian packages them separately.  There is no known reason to
  # waste cycles on separate probes for the Tcl or Python libraries, though.
  # On some Red Hat platforms, the link attempt can fail if we don't use
  # CFLAGS_SL while building the test program.
  ac_save_CFLAGS=$CFLAGS
  CFLAGS="$CFLAGS $CFLAGS_SL"
  pgac_save_LIBS=$LIBS
  LIBS="$perl_embed_ldflags"
  AC_MSG_CHECKING([for libperl])
  AC_LINK_IFELSE([AC_LANG_PROGRAM([
#include <EXTERN.h>
#include <perl.h>
],  [perl_alloc();])],
    [AC_MSG_RESULT(yes)],
    [AC_MSG_RESULT(no)
     AC_MSG_ERROR([libperl library is required for Perl])])
  LIBS=$pgac_save_LIBS
  CFLAGS=$ac_save_CFLAGS
  CPPFLAGS=$ac_save_CPPFLAGS
fi

# check for <Python.h>
if test "$with_python" = yes; then
  ac_save_CPPFLAGS=$CPPFLAGS
  CPPFLAGS="$python_includespec $CPPFLAGS"
  AC_CHECK_HEADER(Python.h, [], [AC_MSG_ERROR([header file <Python.h> is required for Python])])
  CPPFLAGS=$ac_save_CPPFLAGS
fi

#
# Check for documentation-building tools
#
PGAC_PATH_PROGS(XMLLINT, xmllint)
PGAC_PATH_PROGS(XSLTPROC, xsltproc)
PGAC_PATH_PROGS(FOP, fop)
PGAC_PATH_PROGS(DBTOEPUB, dbtoepub)

#
# Check for test tools
#
if test "$enable_tap_tests" = yes; then
  # Make sure we know where prove is.
  PGAC_PATH_PROGS(PROVE, prove)
  if test -z "$PROVE"; then
    AC_MSG_ERROR([prove not found])
  fi
  # Check for necessary Perl modules.  You might think we should use
  # AX_PROG_PERL_MODULES here, but prove might be part of a different Perl
  # installation than perl, eg on MSys, so we have to check using prove.
  AC_MSG_CHECKING(for Perl modules required for TAP tests)
  __CONFIG_HOST_OS__=$host_os; export __CONFIG_HOST_OS__
  [modulestderr=`"$PROVE" "$srcdir/config/check_modules.pl" 2>&1 >/dev/null`]
  if test $? -eq 0; then
    # log the module version details, but don't show them interactively
    echo "$modulestderr" >&AS_MESSAGE_LOG_FD
    AC_MSG_RESULT(yes)
  else
    # on failure, though, show the results to the user
    AC_MSG_RESULT([$modulestderr])
    AC_MSG_ERROR([Additional Perl modules are required to run TAP tests])
  fi
fi

# If compiler will take -Wl,--as-needed (or various platform-specific
# spellings thereof) then add that to LDFLAGS.  This is much easier than
# trying to filter LIBS to the minimum for each executable.
# On (at least) some Red-Hat-derived systems, this switch breaks linking to
# libreadline; therefore we postpone testing it until we know what library
# dependencies readline has.  The test code will try to link with $LIBS.
if test "$with_readline" = yes; then
  link_test_func=readline
else
  link_test_func=exit
fi

if test "$PORTNAME" = "darwin"; then
  PGAC_PROG_CC_LDFLAGS_OPT([-Wl,-dead_strip_dylibs], $link_test_func)
elif test "$PORTNAME" = "openbsd"; then
  PGAC_PROG_CC_LDFLAGS_OPT([-Wl,-Bdynamic], $link_test_func)
else
  PGAC_PROG_CC_LDFLAGS_OPT([-Wl,--as-needed], $link_test_func)
fi

# For linkers that understand --export-dynamic, add that to the LDFLAGS_EX_BE
# (backend specific ldflags). One some platforms this will always fail (e.g.,
# windows), but on others it depends on the choice of linker (e.g., solaris).
# macOS uses -export_dynamic instead.  (On macOS, the option is only
# needed when also using -flto, but we add it anyway since it's
# harmless.)
PGAC_PROG_CC_LD_VARFLAGS_OPT(LDFLAGS_EX_BE, [-Wl,--export-dynamic], $link_test_func)
if test x"$LDFLAGS_EX_BE" = x""; then
  PGAC_PROG_CC_LD_VARFLAGS_OPT(LDFLAGS_EX_BE, [-Wl,-export_dynamic], $link_test_func)
fi
AC_SUBST(LDFLAGS_EX_BE)

# Create compiler version string
if test x"$GCC" = x"yes" ; then
  cc_string=`${CC} --version | sed q`
  case $cc_string in [[A-Za-z]]*) ;; *) cc_string="GCC $cc_string";; esac
elif test x"$SUN_STUDIO_CC" = x"yes" ; then
  cc_string=`${CC} -V 2>&1 | sed q`
else
  cc_string=$CC
fi

AC_DEFINE_UNQUOTED(PG_VERSION_STR,
                   ["PostgreSQL $PG_VERSION on $host, compiled by $cc_string, `expr $ac_cv_sizeof_void_p \* 8`-bit"],
                   [A string containing the version number, platform, and C compiler])

# Supply a numeric version string for use by 3rd party add-ons
# awk -F is a regex on some platforms, and not on others, so make "." a tab
[PG_VERSION_NUM="`echo $PG_MAJORVERSION $PG_MINORVERSION |
$AWK '{printf "%d%04d", $1, $2}'`"]
AC_DEFINE_UNQUOTED(PG_VERSION_NUM, $PG_VERSION_NUM, [PostgreSQL version as a number])
AC_SUBST(PG_VERSION_NUM)

# If we are inserting PG_SYSROOT into CPPFLAGS, do so symbolically not
# literally, so that it's possible to override it at build time using
# a command like "make ... PG_SYSROOT=path".  This has to be done after
# we've finished all configure checks that depend on CPPFLAGS.
# The same for LDFLAGS, too.
if test x"$PG_SYSROOT" != x; then
  CPPFLAGS=`echo "$CPPFLAGS" | sed -e "s| $PG_SYSROOT | \\\$(PG_SYSROOT) |"`
  LDFLAGS=`echo "$LDFLAGS" | sed -e "s| $PG_SYSROOT | \\\$(PG_SYSROOT) |"`
fi
AC_SUBST(PG_SYSROOT)


# Begin output steps

AC_MSG_NOTICE([using compiler=$cc_string])
AC_MSG_NOTICE([using CFLAGS=$CFLAGS])
AC_MSG_NOTICE([using CPPFLAGS=$CPPFLAGS])
AC_MSG_NOTICE([using LDFLAGS=$LDFLAGS])
# Currently only used when LLVM is used
if test "$with_llvm" = yes ; then
   AC_MSG_NOTICE([using CXX=$CXX])
   AC_MSG_NOTICE([using CXXFLAGS=$CXXFLAGS])
   AC_MSG_NOTICE([using CLANG=$CLANG])
   AC_MSG_NOTICE([using BITCODE_CFLAGS=$BITCODE_CFLAGS])
   AC_MSG_NOTICE([using BITCODE_CXXFLAGS=$BITCODE_CXXFLAGS])
fi

# prepare build tree if outside source tree
# Note 1: test -ef might not exist, but it's more reliable than `pwd`.
# Note 2: /bin/pwd might be better than shell's built-in at getting
#         a symlink-free name.
if ( test "$srcdir" -ef . ) >/dev/null 2>&1 || test "`cd $srcdir && /bin/pwd`" = "`/bin/pwd`"; then
  vpath_build=no
else
  vpath_build=yes
  if test "$no_create" != yes; then
    _AS_ECHO_N([preparing build tree... ])
    pgac_abs_top_srcdir=`cd "$srcdir" && pwd`
    $SHELL "$ac_aux_dir/prep_buildtree" "$pgac_abs_top_srcdir" "." \
      || AC_MSG_ERROR(failed)
    AC_MSG_RESULT(done)
  fi
fi
AC_SUBST(vpath_build)


AC_CONFIG_FILES([GNUmakefile src/Makefile.global])

AC_CONFIG_LINKS([
  src/backend/port/pg_sema.c:${SEMA_IMPLEMENTATION}
  src/backend/port/pg_shmem.c:${SHMEM_IMPLEMENTATION}
  src/include/pg_config_os.h:src/include/port/${template}.h
  src/Makefile.port:src/makefiles/Makefile.${template}
])

if test "$PORTNAME" = "win32"; then
AC_CONFIG_COMMANDS([check_win32_symlinks],[
# Links sometimes fail undetected on Mingw -
# so here we detect it and warn the user
for FILE in $CONFIG_LINKS
 do
	# test -e works for symlinks in the MinGW console
	test -e `expr "$FILE" : '\([[^:]]*\)'` || AC_MSG_WARN([*** link for $FILE -- please fix by hand])
 done
])
fi

AC_CONFIG_HEADERS([src/include/pg_config.h],
[
# Update timestamp for pg_config.h (see Makefile.global)
echo >src/include/stamp-h
])

AC_CONFIG_HEADERS([src/include/pg_config_ext.h],
[
# Update timestamp for pg_config_ext.h (see Makefile.global)
echo >src/include/stamp-ext-h
])

AC_CONFIG_HEADERS([src/interfaces/ecpg/include/ecpg_config.h],
                  [echo >src/interfaces/ecpg/include/stamp-h])

AC_OUTPUT

# Ensure that any meson build directories would reconfigure and see that
# there's a conflicting in-tree build and can error out.
if test "$vpath_build" = "no"; then
  touch meson.build
fi
