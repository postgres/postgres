# $PostgreSQL: pgsql/config/docbook.m4,v 1.7.8.1 2007/08/09 02:34:07 tgl Exp $

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
  for pgac_prefix in /usr /usr/local /opt; do
    for pgac_infix in share lib; do
      for pgac_postfix in \
        sgml/stylesheets/nwalsh-modular \
        sgml/stylesheets/docbook \
        sgml/stylesheets/dsssl/docbook \
        sgml/docbook-dsssl \
        sgml/docbook/dsssl/modular \
        sgml/docbook/stylesheet/dsssl/modular \
        sgml/docbook/dsssl-stylesheets
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
