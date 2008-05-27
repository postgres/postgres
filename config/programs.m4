# $PostgreSQL: pgsql/config/programs.m4,v 1.20.2.1 2008/05/27 22:18:18 tgl Exp $


# PGAC_PATH_FLEX
# --------------
# Look for Flex, set the output variable FLEX to its path if found.
# Avoid the buggy version 2.5.3. Also find Flex if its installed
# under `lex', but do not accept other Lex programs.

AC_DEFUN([PGAC_PATH_FLEX],
[AC_CACHE_CHECK([for flex], pgac_cv_path_flex,
[# Let the user override the test
if test -n "$FLEX"; then
  pgac_cv_path_flex=$FLEX
else
  pgac_save_IFS=$IFS
  IFS=$PATH_SEPARATOR
  for pgac_dir in $PATH; do
    IFS=$pgac_save_IFS
    if test -z "$pgac_dir" || test x"$pgac_dir" = x"."; then
      pgac_dir=`pwd`
    fi
    for pgac_prog in flex lex; do
      pgac_candidate="$pgac_dir/$pgac_prog"
      if test -f "$pgac_candidate" \
        && $pgac_candidate --version </dev/null >/dev/null 2>&1
      then
        echo '%%'  > conftest.l
        if $pgac_candidate -t conftest.l 2>/dev/null | grep FLEX_SCANNER >/dev/null 2>&1; then
          if $pgac_candidate --version | grep ' 2\.5\.3$' >/dev/null 2>&1; then
            pgac_broken_flex=$pgac_candidate
            continue
          fi

          pgac_cv_path_flex=$pgac_candidate
          break 2
        fi
      fi
    done
  done
  rm -f conftest.l lex.yy.c
  : ${pgac_cv_path_flex=no}
fi
])[]dnl AC_CACHE_CHECK

if test x"$pgac_cv_path_flex" = x"no"; then
  if test -n "$pgac_broken_flex"; then
    AC_MSG_WARN([
*** The Flex version 2.5.3 you have at $pgac_broken_flex contains a bug. You
*** should get version 2.5.4 or later.])
  fi

  AC_MSG_WARN([
*** Without Flex you will not be able to build PostgreSQL from CVS or
*** change any of the scanner definition files.  You can obtain Flex from
*** a GNU mirror site.  (If you are using the official distribution of
*** PostgreSQL then you do not need to worry about this because the Flex
*** output is pre-generated.)])
fi

if test x"$pgac_cv_path_flex" = x"no"; then
  FLEX=
else
  FLEX=$pgac_cv_path_flex
fi

AC_SUBST(FLEX)
AC_SUBST(FLEXFLAGS)
])# PGAC_PATH_FLEX



# PGAC_CHECK_READLINE
# -------------------
# Check for the readline library and dependent libraries, either
# termcap or curses.  Also try libedit, since NetBSD's is compatible.
# Add the required flags to LIBS, define HAVE_LIBREADLINE.

AC_DEFUN([PGAC_CHECK_READLINE],
[AC_REQUIRE([AC_CANONICAL_HOST])

AC_CACHE_VAL([pgac_cv_check_readline],
[pgac_cv_check_readline=no
pgac_save_LIBS=$LIBS
if test x"$with_libedit_preferred" != x"yes"
then	READLINE_ORDER="-lreadline -ledit"
else	READLINE_ORDER="-ledit -lreadline"
fi
for pgac_rllib in $READLINE_ORDER ; do
  AC_MSG_CHECKING([for ${pgac_rllib}])
  for pgac_lib in "" " -ltermcap" " -lncurses" " -lcurses" ; do
    LIBS="${pgac_rllib}${pgac_lib} $pgac_save_LIBS"
    AC_TRY_LINK_FUNC([readline], [[
      # Older NetBSD, OpenBSD, and Irix have a broken linker that does not
      # recognize dependent libraries; assume curses is needed if we didn't
      # find any dependency.
      case $host_os in
        netbsd* | openbsd* | irix*)
          if test x"$pgac_lib" = x"" ; then
            pgac_lib=" -lcurses"
          fi ;;
      esac

      pgac_cv_check_readline="${pgac_rllib}${pgac_lib}"
      break
    ]])
  done
  if test "$pgac_cv_check_readline" != no ; then
    AC_MSG_RESULT([yes ($pgac_cv_check_readline)])
    break
  else
    AC_MSG_RESULT(no)
  fi
done
LIBS=$pgac_save_LIBS
])[]dnl AC_CACHE_VAL

if test "$pgac_cv_check_readline" != no ; then
  LIBS="$pgac_cv_check_readline $LIBS"
  AC_DEFINE(HAVE_LIBREADLINE, 1, [Define if you have a function readline library])
fi

])# PGAC_CHECK_READLINE



# PGAC_VAR_RL_COMPLETION_APPEND_CHARACTER
# ---------------------------------------
# Readline versions < 2.1 don't have rl_completion_append_character

AC_DEFUN([PGAC_VAR_RL_COMPLETION_APPEND_CHARACTER],
[AC_MSG_CHECKING([for rl_completion_append_character])
AC_TRY_LINK([#include <stdio.h>
#ifdef HAVE_READLINE_READLINE_H
# include <readline/readline.h>
#elif defined(HAVE_READLINE_H)
# include <readline.h>
#endif
],
[rl_completion_append_character = 'x';],
[AC_MSG_RESULT(yes)
AC_DEFINE(HAVE_RL_COMPLETION_APPEND_CHARACTER, 1,
          [Define to 1 if you have the global variable 'rl_completion_append_character'.])],
[AC_MSG_RESULT(no)])])# PGAC_VAR_RL_COMPLETION_APPEND_CHARACTER



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
  AC_CHECK_PROGS(MSGFMT, msgfmt)
  if test -z "$MSGFMT"; then
    AC_MSG_ERROR([msgfmt is required for NLS])
  fi
  AC_CHECK_PROGS(MSGMERGE, msgmerge)
  AC_CHECK_PROGS(XGETTEXT, xgettext)

  # Note: share/locale is always the default, independent of $datadir
  localedir='${prefix}/share/locale'
  AC_SUBST(localedir)
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
    STRIP_STATIC_LIB="$STRIP -x"
    STRIP_SHARED_LIB="$STRIP --strip-unneeded"
    AC_MSG_RESULT(yes)
  else
    STRIP_STATIC_LIB=:
    STRIP_SHARED_LIB=:
    AC_MSG_RESULT(no)
  fi
  AC_SUBST(STRIP_STATIC_LIB)
  AC_SUBST(STRIP_SHARED_LIB)
])# PGAC_CHECK_STRIP
