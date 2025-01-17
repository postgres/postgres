# config/programs.m4


# PGAC_PATH_PROGS
# ---------------
# This wrapper for AC_PATH_PROGS behaves like that macro except when
# VARIABLE is already set; in that case we just accept the value verbatim.
# (AC_PATH_PROGS would accept it only if it looks like an absolute path.)
# A desirable future improvement would be to convert a non-absolute-path
# input into absolute form.
AC_DEFUN([PGAC_PATH_PROGS],
[if test -z "$$1"; then
  AC_PATH_PROGS($@)
else
  # Report the value of $1 in configure's output in all cases.
  AC_MSG_CHECKING([for $1])
  AC_MSG_RESULT([$$1])
fi
])


# PGAC_PATH_BISON
# ---------------
# Look for Bison, set the output variable BISON to its path if found.
# Reject versions before 2.3 (the earliest version in the buildfarm
# as of 2022). Note we do not accept other implementations of yacc.

AC_DEFUN([PGAC_PATH_BISON],
[PGAC_PATH_PROGS(BISON, bison)

if test "$BISON"; then
  pgac_bison_version=`$BISON --version 2>/dev/null | sed q`
  AC_MSG_NOTICE([using $pgac_bison_version])
  if echo "$pgac_bison_version" | $AWK '{ if ([$]4 < 2.3) exit 0; else exit 1;}'
  then
    AC_MSG_ERROR([
*** The installed version of Bison, $BISON, is too old to use with PostgreSQL.
*** Bison version 2.3 or later is required, but this is $pgac_bison_version.])
  fi
  # Bison >=3.0 issues warnings about %name-prefix="base_yy", instead
  # of the now preferred %name-prefix "base_yy", but the latter
  # doesn't work with Bison 2.3 or less.  So for now we silence the
  # deprecation warnings.
  if echo "$pgac_bison_version" | $AWK '{ if ([$]4 >= 3) exit 0; else exit 1;}'
  then
    BISONFLAGS="$BISONFLAGS -Wno-deprecated"
  fi
fi

if test -z "$BISON"; then
  AC_MSG_ERROR([bison not found])
fi
dnl We don't need AC_SUBST(BISON) because PGAC_PATH_PROGS did it
AC_SUBST(BISONFLAGS)
])# PGAC_PATH_BISON



# PGAC_PATH_FLEX
# --------------
# Look for Flex, set the output variable FLEX to its path if found.

AC_DEFUN([PGAC_PATH_FLEX],
[PGAC_PATH_PROGS(FLEX, flex)
if test -z "$FLEX"; then
  AC_MSG_ERROR([flex not found])
fi

pgac_flex_version=`$FLEX --version 2>/dev/null`
AC_MSG_NOTICE([using $pgac_flex_version])

AC_SUBST(FLEX)
AC_SUBST(FLEXFLAGS)
])# PGAC_PATH_FLEX



# PGAC_LDAP_SAFE
# --------------
# PostgreSQL sometimes loads libldap_r and plain libldap into the same
# process.  Check for OpenLDAP versions known not to tolerate doing so; assume
# non-OpenLDAP implementations are safe.  The dblink test suite exercises the
# hazardous interaction directly.

AC_DEFUN([PGAC_LDAP_SAFE],
[AC_CACHE_CHECK([for compatible LDAP implementation], [pgac_cv_ldap_safe],
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[#include <ldap.h>
#if !defined(LDAP_VENDOR_VERSION) || \
     (defined(LDAP_API_FEATURE_X_OPENLDAP) && \
      LDAP_VENDOR_VERSION >= 20424 && LDAP_VENDOR_VERSION <= 20431)
choke me
#endif], [])],
[pgac_cv_ldap_safe=yes],
[pgac_cv_ldap_safe=no])])

if test "$pgac_cv_ldap_safe" != yes; then
  AC_MSG_WARN([
*** With OpenLDAP versions 2.4.24 through 2.4.31, inclusive, each backend
*** process that loads libpq (via WAL receiver, dblink, or postgres_fdw) and
*** also uses LDAP will crash on exit.])
fi])



# PGAC_CHECK_READLINE
# -------------------
# Check for the readline library and dependent libraries, either
# termcap or curses.  Also try libedit, since NetBSD's is compatible.
# Add the required flags to LIBS, define HAVE_LIBREADLINE.

AC_DEFUN([PGAC_CHECK_READLINE],
[AC_REQUIRE([AC_CANONICAL_HOST])

AC_CACHE_CHECK([for library containing readline], [pgac_cv_check_readline],
[pgac_cv_check_readline=no
pgac_save_LIBS=$LIBS
if test x"$with_libedit_preferred" != x"yes"
then	READLINE_ORDER="-lreadline -ledit"
else	READLINE_ORDER="-ledit -lreadline"
fi
for pgac_rllib in $READLINE_ORDER ; do
  for pgac_lib in "" " -ltermcap" " -lncurses" " -lcurses" ; do
    LIBS="${pgac_rllib}${pgac_lib} $pgac_save_LIBS"
    AC_TRY_LINK_FUNC([readline], [[
      # Older NetBSD and OpenBSD have a broken linker that does not
      # recognize dependent libraries; assume curses is needed if we didn't
      # find any dependency.
      case $host_os in
        netbsd* | openbsd*)
          if test x"$pgac_lib" = x"" ; then
            pgac_lib=" -lcurses"
          fi ;;
      esac

      pgac_cv_check_readline="${pgac_rllib}${pgac_lib}"
      break
    ]])
  done
  if test "$pgac_cv_check_readline" != no ; then
    break
  fi
done
LIBS=$pgac_save_LIBS
])[]dnl AC_CACHE_CHECK

if test "$pgac_cv_check_readline" != no ; then
  LIBS="$pgac_cv_check_readline $LIBS"
  AC_DEFINE(HAVE_LIBREADLINE, 1, [Define if you have a function readline library])
fi

])# PGAC_CHECK_READLINE



# PGAC_READLINE_VARIABLES
# -----------------------
# Some Readline versions lack rl_completion_suppress_quote.
# Libedit lacks rl_filename_quote_characters and rl_filename_quoting_function

AC_DEFUN([PGAC_READLINE_VARIABLES],
[AC_CACHE_CHECK([for rl_completion_suppress_quote], pgac_cv_var_rl_completion_suppress_quote,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <stdio.h>
#if defined(HAVE_READLINE_READLINE_H)
#include <readline/readline.h>
#elif defined(HAVE_EDITLINE_READLINE_H)
#include <editline/readline.h>
#elif defined(HAVE_READLINE_H)
#include <readline.h>
#endif
],
[rl_completion_suppress_quote = 1;])],
[pgac_cv_var_rl_completion_suppress_quote=yes],
[pgac_cv_var_rl_completion_suppress_quote=no])])
if test x"$pgac_cv_var_rl_completion_suppress_quote" = x"yes"; then
AC_DEFINE(HAVE_RL_COMPLETION_SUPPRESS_QUOTE, 1,
          [Define to 1 if you have the global variable 'rl_completion_suppress_quote'.])
fi
AC_CACHE_CHECK([for rl_filename_quote_characters], pgac_cv_var_rl_filename_quote_characters,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <stdio.h>
#if defined(HAVE_READLINE_READLINE_H)
#include <readline/readline.h>
#elif defined(HAVE_EDITLINE_READLINE_H)
#include <editline/readline.h>
#elif defined(HAVE_READLINE_H)
#include <readline.h>
#endif
],
[rl_filename_quote_characters = "x";])],
[pgac_cv_var_rl_filename_quote_characters=yes],
[pgac_cv_var_rl_filename_quote_characters=no])])
if test x"$pgac_cv_var_rl_filename_quote_characters" = x"yes"; then
AC_DEFINE(HAVE_RL_FILENAME_QUOTE_CHARACTERS, 1,
          [Define to 1 if you have the global variable 'rl_filename_quote_characters'.])
fi
AC_CACHE_CHECK([for rl_filename_quoting_function], pgac_cv_var_rl_filename_quoting_function,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <stdio.h>
#if defined(HAVE_READLINE_READLINE_H)
#include <readline/readline.h>
#elif defined(HAVE_EDITLINE_READLINE_H)
#include <editline/readline.h>
#elif defined(HAVE_READLINE_H)
#include <readline.h>
#endif
],
[rl_filename_quoting_function = 0;])],
[pgac_cv_var_rl_filename_quoting_function=yes],
[pgac_cv_var_rl_filename_quoting_function=no])])
if test x"$pgac_cv_var_rl_filename_quoting_function" = x"yes"; then
AC_DEFINE(HAVE_RL_FILENAME_QUOTING_FUNCTION, 1,
          [Define to 1 if you have the global variable 'rl_filename_quoting_function'.])
fi
])# PGAC_READLINE_VARIABLES



# PGAC_CHECK_GETTEXT
# ------------------
# We check for bind_textdomain_codeset() not just gettext().  GNU gettext
# before 0.10.36 does not have that function, and is generally too incomplete
# to be usable.

AC_DEFUN([PGAC_CHECK_GETTEXT],
[
  AC_SEARCH_LIBS(bind_textdomain_codeset, intl, [],
                 [AC_MSG_ERROR([a gettext implementation is required for NLS])])
  AC_CHECK_HEADER([libintl.h], [],
                  [AC_MSG_ERROR([header file <libintl.h> is required for NLS])])
  PGAC_PATH_PROGS(MSGFMT, msgfmt)
  AC_ARG_VAR(MSGFMT, [msgfmt program for NLS])dnl
  if test -z "$MSGFMT"; then
    AC_MSG_ERROR([msgfmt is required for NLS])
  fi
  AC_CACHE_CHECK([for msgfmt flags], pgac_cv_msgfmt_flags,
[if test x"$MSGFMT" != x"" && "$MSGFMT" --version 2>&1 | grep "GNU" >/dev/null; then
    pgac_cv_msgfmt_flags=-c
fi])
  AC_SUBST(MSGFMT_FLAGS, $pgac_cv_msgfmt_flags)
  PGAC_PATH_PROGS(MSGMERGE, msgmerge)
  PGAC_PATH_PROGS(XGETTEXT, xgettext)
])# PGAC_CHECK_GETTEXT



# PGAC_CHECK_STRIP
# ----------------
# Check for a 'strip' program, and figure out if that program can
# strip libraries.

AC_DEFUN([PGAC_CHECK_STRIP],
[
  AC_CHECK_TOOL(STRIP, strip, :)

  AC_MSG_CHECKING([whether it is possible to strip libraries])
  if test x"$STRIP" != x"" && "$STRIP" -V 2>&1 | grep "GNU strip" >/dev/null; then
    STRIP_STATIC_LIB="$STRIP --strip-unneeded"
    STRIP_SHARED_LIB="$STRIP --strip-unneeded"
    AC_MSG_RESULT(yes)
  else
    case $host_os in
      darwin*)
        STRIP="$STRIP -x"
        STRIP_STATIC_LIB=$STRIP
        STRIP_SHARED_LIB=$STRIP
        AC_MSG_RESULT(yes)
        ;;
      *)
        STRIP_STATIC_LIB=:
        STRIP_SHARED_LIB=:
        AC_MSG_RESULT(no)
        ;;
    esac
  fi
  AC_SUBST(STRIP_STATIC_LIB)
  AC_SUBST(STRIP_SHARED_LIB)
])# PGAC_CHECK_STRIP
