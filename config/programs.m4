# $Header: /cvsroot/pgsql/config/programs.m4,v 1.2 2000/10/26 16:28:00 petere Exp $


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
  IFS=:
  for pgac_dir in $PATH; do
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
          if $pgac_candidate --version | grep '2\.5\.3' >/dev/null 2>&1; then
            pgac_broken_flex=$pgac_candidate
            continue
          fi

          pgac_cv_path_flex=$pgac_candidate
          break 2
        fi
      fi
    done
  done
  IFS=$pgac_save_IFS
  rm -f conftest.l
  : ${pgac_cv_path_flex=no}
fi
])[]dnl AC_CACHE_CHECK

if test x"$pgac_cv_path_flex" = x"no"; then
  if test -n "$pgac_broken_flex"; then
    AC_MSG_WARN([
***
The Flex version 2.5.3 you have at $pgac_broken_flex contains a bug. You
should get version 2.5.4 or later.
###])
  fi

  AC_MSG_WARN([
***
Without Flex you won't be able to build PostgreSQL from scratch, or change
any of the scanner definition files. You can obtain Flex from a GNU mirror
site. (If you are using the official distribution of PostgreSQL then you
do not need to worry about this because the lexer files are pre-generated.)
***])
fi

if test x"$pgac_cv_path_flex" = x"no"; then
  FLEX=
else
  FLEX=$pgac_cv_path_flex
fi

AC_SUBST(FLEX)
AC_SUBST(FLEXFLAGS)
])# PGAC_PATH_FLEX
