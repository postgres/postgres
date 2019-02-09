# config/llvm.m4

# PGAC_LLVM_SUPPORT
# -----------------
#
# Look for the LLVM installation, check that it's new enough, set the
# corresponding LLVM_{CFLAGS,CXXFLAGS,BINPATH} and LDFLAGS
# variables. Also verify that CLANG is available, to transform C
# into bitcode.
#
AC_DEFUN([PGAC_LLVM_SUPPORT],
[
  AC_REQUIRE([AC_PROG_AWK])

  AC_ARG_VAR(LLVM_CONFIG, [path to llvm-config command])
  PGAC_PATH_PROGS(LLVM_CONFIG, llvm-config llvm-config-7 llvm-config-6.0 llvm-config-5.0 llvm-config-4.0 llvm-config-3.9)

  # no point continuing if llvm wasn't found
  if test -z "$LLVM_CONFIG"; then
    AC_MSG_ERROR([llvm-config not found, but required when compiling --with-llvm, specify with LLVM_CONFIG=])
  fi
  # check if detected $LLVM_CONFIG is executable
  pgac_llvm_version="$($LLVM_CONFIG --version 2> /dev/null || echo no)"
  if test "x$pgac_llvm_version" = "xno"; then
    AC_MSG_ERROR([$LLVM_CONFIG does not work])
  fi
  # and whether the version is supported
  if echo $pgac_llvm_version | $AWK -F '.' '{ if ([$]1 >= 4 || ([$]1 == 3 && [$]2 >= 9)) exit 1; else exit 0;}';then
    AC_MSG_ERROR([$LLVM_CONFIG version is $pgac_llvm_version but at least 3.9 is required])
  fi

  # need clang to create some bitcode files
  AC_ARG_VAR(CLANG, [path to clang compiler to generate bitcode])
  PGAC_PATH_PROGS(CLANG, clang clang-7 clang-6.0 clang-5.0 clang-4.0 clang-3.9)
  if test -z "$CLANG"; then
    AC_MSG_ERROR([clang not found, but required when compiling --with-llvm, specify with CLANG=])
  fi
  # make sure clang is executable
  if test "x$($CLANG --version 2> /dev/null || echo no)" = "xno"; then
    AC_MSG_ERROR([$CLANG does not work])
  fi
  # Could check clang version, but it doesn't seem that
  # important. Systems with a new enough LLVM version are usually
  # going to have a decent clang version too. It's also not entirely
  # clear what the minimum version is.

  # Collect compiler flags necessary to build the LLVM dependent
  # shared library.
  for pgac_option in `$LLVM_CONFIG --cppflags`; do
    case $pgac_option in
      -I*|-D*) LLVM_CPPFLAGS="$pgac_option $LLVM_CPPFLAGS";;
    esac
  done

  for pgac_option in `$LLVM_CONFIG --ldflags`; do
    case $pgac_option in
      -L*) LDFLAGS="$LDFLAGS $pgac_option";;
    esac
  done

  # ABI influencing options, standard influencing options
  for pgac_option in `$LLVM_CONFIG --cxxflags`; do
    case $pgac_option in
      -fno-rtti*) LLVM_CXXFLAGS="$LLVM_CXXFLAGS $pgac_option";;
      -std=*) LLVM_CXXFLAGS="$LLVM_CXXFLAGS $pgac_option";;
    esac
  done

  # Look for components we're interested in, collect necessary
  # libs. As some components are optional, we can't just list all of
  # them as it'd raise an error.
  pgac_components='';
  for pgac_component in `$LLVM_CONFIG --components`; do
    case $pgac_component in
      engine) pgac_components="$pgac_components $pgac_component";;
      debuginfodwarf) pgac_components="$pgac_components $pgac_component";;
      orcjit) pgac_components="$pgac_components $pgac_component";;
      passes) pgac_components="$pgac_components $pgac_component";;
      perfjitevents) pgac_components="$pgac_components $pgac_component";;
    esac
  done;

  # And then get the libraries that need to be linked in for the
  # selected components.  They're large libraries, we only want to
  # link them into the LLVM using shared library.
  for pgac_option in `$LLVM_CONFIG --libs --system-libs $pgac_components`; do
    case $pgac_option in
      -l*) LLVM_LIBS="$LLVM_LIBS $pgac_option";;
    esac
  done

  LLVM_BINPATH=`$LLVM_CONFIG --bindir`

dnl LLVM_CONFIG, CLANG are already output via AC_ARG_VAR
  AC_SUBST(LLVM_LIBS)
  AC_SUBST(LLVM_CPPFLAGS)
  AC_SUBST(LLVM_CFLAGS)
  AC_SUBST(LLVM_CXXFLAGS)
  AC_SUBST(LLVM_BINPATH)

])# PGAC_LLVM_SUPPORT


# PGAC_CHECK_LLVM_FUNCTIONS
# -------------------------
#
# Check presence of some optional LLVM functions.
# (This shouldn't happen until we're ready to run AC_CHECK_DECLS tests;
# because PGAC_LLVM_SUPPORT runs very early, it's not an appropriate place.)
#
AC_DEFUN([PGAC_CHECK_LLVM_FUNCTIONS],
[
  # Check which functionality is present
  SAVE_CPPFLAGS="$CPPFLAGS"
  CPPFLAGS="$CPPFLAGS $LLVM_CPPFLAGS"
  AC_CHECK_DECLS([LLVMOrcGetSymbolAddressIn], [], [], [[#include <llvm-c/OrcBindings.h>]])
  AC_CHECK_DECLS([LLVMGetHostCPUName, LLVMGetHostCPUFeatures], [], [], [[#include <llvm-c/TargetMachine.h>]])
  AC_CHECK_DECLS([LLVMCreateGDBRegistrationListener, LLVMCreatePerfJITEventListener], [], [], [[#include <llvm-c/ExecutionEngine.h>]])
  CPPFLAGS="$SAVE_CPPFLAGS"
])# PGAC_CHECK_LLVM_FUNCTIONS
