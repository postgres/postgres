# config/docbook.m4

# PGAC_PROG_NSGMLS
# ----------------
AC_DEFUN([PGAC_PROG_NSGMLS],
[PGAC_PATH_PROGS(NSGMLS, [onsgmls nsgmls])])


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
