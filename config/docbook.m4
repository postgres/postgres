# config/docbook.m4

# PGAC_PATH_XMLLINT
# -----------------
AC_DEFUN([PGAC_PATH_XMLLINT],
[PGAC_PATH_PROGS(XMLLINT, xmllint)])


# PGAC_CHECK_DOCBOOK(VERSION)
# ---------------------------
AC_DEFUN([PGAC_CHECK_DOCBOOK],
[AC_REQUIRE([PGAC_PATH_XMLLINT])
AC_CACHE_CHECK([for DocBook XML V$1], [pgac_cv_check_docbook],
[cat >conftest.xml <<EOF
<!DOCTYPE book PUBLIC "-//OASIS//DTD DocBook XML V$1//EN" "http://www.oasis-open.org/docbook/xml/$1/docbookx.dtd">
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

if test -n "$XMLLINT"; then
  $XMLLINT --noout --valid conftest.xml 1>&AS_MESSAGE_LOG_FD 2>&1
  if test $? -eq 0; then
    pgac_cv_check_docbook=yes
  fi
fi
rm -f conftest.xml])

have_docbook=$pgac_cv_check_docbook
AC_SUBST([have_docbook])
])# PGAC_CHECK_DOCBOOK
