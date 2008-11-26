# $PostgreSQL: pgsql/config/docbook.m4,v 1.10 2008/11/26 11:26:54 petere Exp $

# PGAC_PROG_JADE
# --------------
AC_DEFUN([PGAC_PROG_JADE],
[AC_CHECK_PROGS([JADE], [openjade jade])])


# PGAC_PROG_NSGMLS
# ----------------
AC_DEFUN([PGAC_PROG_NSGMLS],
[AC_CHECK_PROGS([NSGMLS], [onsgmls nsgmls])])


# PGAC_CHECK_DOCBOOK(VERSION)
# ---------------------------
AC_DEFUN([PGAC_CHECK_DOCBOOK],
[AC_REQUIRE([PGAC_PROG_NSGMLS])
AC_CACHE_CHECK([for DocBook V$1], [pgac_cv_check_docbook],
[cat >conftest.sgml <<EOF
<!doctype book PUBLIC "-//OASIS//DTD DocBook V$1//EN">
<book>
 <title>test</title>
 <chapter>
  <title>random</title>
   <sect1>
    <title>testsect</title>
    <para>text</para>
  </sect1>
 </chapter>
</book>
EOF

pgac_cv_check_docbook=no

if test -n "$NSGMLS"; then
  $NSGMLS -s conftest.sgml 1>&AS_MESSAGE_LOG_FD 2>&1
  if test $? -eq 0; then
    pgac_cv_check_docbook=yes
  fi
fi
rm -f conftest.sgml])

have_docbook=$pgac_cv_check_docbook
AC_SUBST([have_docbook])
])# PGAC_CHECK_DOCBOOK


# PGAC_PATH_DOCBOOK_STYLESHEETS
# -----------------------------
AC_DEFUN([PGAC_PATH_DOCBOOK_STYLESHEETS],
[AC_ARG_VAR(DOCBOOKSTYLE, [location of DocBook stylesheets])dnl
AC_MSG_CHECKING([for DocBook stylesheets])
AC_CACHE_VAL([pgac_cv_path_stylesheets],
[if test -n "$DOCBOOKSTYLE"; then
  pgac_cv_path_stylesheets=$DOCBOOKSTYLE
else
  for pgac_prefix in /usr /usr/local /opt /sw; do
    for pgac_infix in share lib; do
      for pgac_postfix in \
        sgml/stylesheets/nwalsh-modular \
        sgml/stylesheets/docbook \
        sgml/stylesheets/dsssl/docbook \
        sgml/docbook-dsssl \
        sgml/docbook/dsssl/modular \
        sgml/docbook/stylesheet/dsssl/modular \
        sgml/docbook/dsssl-stylesheets \
        sgml/dsssl/docbook-dsssl-nwalsh
      do
        pgac_candidate=$pgac_prefix/$pgac_infix/$pgac_postfix
        if test -r "$pgac_candidate/html/docbook.dsl" \
           && test -r "$pgac_candidate/print/docbook.dsl"
        then
          pgac_cv_path_stylesheets=$pgac_candidate
          break 3
        fi
      done
    done
  done
fi])
DOCBOOKSTYLE=$pgac_cv_path_stylesheets
AC_SUBST([DOCBOOKSTYLE])
if test -n "$DOCBOOKSTYLE"; then
  AC_MSG_RESULT([$DOCBOOKSTYLE])
else
  AC_MSG_RESULT(no)
fi])# PGAC_PATH_DOCBOOK_STYLESHEETS


# PGAC_PATH_COLLATEINDEX
# ----------------------
AC_DEFUN([PGAC_PATH_COLLATEINDEX],
[AC_REQUIRE([PGAC_PATH_DOCBOOK_STYLESHEETS])dnl
if test -n "$DOCBOOKSTYLE"; then
  AC_PATH_PROGS(COLLATEINDEX, collateindex.pl, [],
                [$DOCBOOKSTYLE/bin $PATH])
else
  AC_PATH_PROGS(COLLATEINDEX, collateindex.pl)
fi])# PGAC_PATH_COLLATEINDEX


# PGAC_PATH_DOCBOOK2MAN
# ---------------------
# Find docbook2man program from the docbook2X package.  Upstream calls
# this program docbook2man, but there is also a different docbook2man
# out there from the docbook-utils package.  Thus, the program we want
# is called docbook2x-man on Debian and db2x_docbook2man on Fedora.
#
# (Consider rewriting this macro using AC_PATH_PROGS_FEATURE_CHECK
# when switching to Autoconf 2.62+.)
AC_DEFUN([PGAC_PATH_DOCBOOK2MAN],
[AC_CACHE_CHECK([for docbook2man], [ac_cv_path_DOCBOOK2MAN],
[if test -z "$DOCBOOK2MAN"; then
  _AS_PATH_WALK([],
  [for ac_prog in docbook2x-man db2x_docbook2man docbook2man; do
    ac_path="$as_dir/$ac_prog"
    AS_EXECUTABLE_P(["$ac_path"]) || continue
    if "$ac_path" --version 2>/dev/null | $GREP docbook2x >/dev/null 2>&1; then
      ac_cv_path_DOCBOOK2MAN=$ac_path
      break
    fi
  done])
else
  ac_cv_path_DOCBOOK2MAN=$DOCBOOK2MAN
fi])
DOCBOOK2MAN=$ac_cv_path_DOCBOOK2MAN
AC_SUBST(DOCBOOK2MAN)
])# PGAC_PATH_DOCBOOK2MAN
