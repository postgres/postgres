# $PostgreSQL: pgsql/config/perl.m4,v 1.3 2003/11/29 19:51:17 pgsql Exp $


# PGAC_PATH_PERL
# --------------
AC_DEFUN([PGAC_PATH_PERL],
[AC_PATH_PROG(PERL, perl)])


# PGAC_CHECK_PERL_CONFIG(NAME)
# ----------------------------
AC_DEFUN([PGAC_CHECK_PERL_CONFIG],
[AC_REQUIRE([PGAC_PATH_PERL])
AC_MSG_CHECKING([for Perl $1])
perl_$1=`$PERL -MConfig -e 'print $Config{$1}'`
AC_SUBST(perl_$1)dnl
AC_MSG_RESULT([$perl_$1])])


# PGAC_CHECK_PERL_CONFIGS(NAMES)
# ------------------------------
AC_DEFUN([PGAC_CHECK_PERL_CONFIGS],
[m4_foreach([pgac_item], [$1], [PGAC_CHECK_PERL_CONFIG(pgac_item)])])


# PGAC_CHECK_PERL_EMBED_LDFLAGS
# -----------------------------
AC_DEFUN([PGAC_CHECK_PERL_EMBED_LDFLAGS],
[AC_REQUIRE([PGAC_PATH_PERL])
AC_MSG_CHECKING(for flags to link embedded Perl)
pgac_tmp1=`$PERL -MExtUtils::Embed -e ldopts`
pgac_tmp2=`$PERL -MConfig -e 'print $Config{ccdlflags}'`
perl_embed_ldflags=`echo X"$pgac_tmp1" | sed "s/^X//;s%$pgac_tmp2%%"`
AC_SUBST(perl_embed_ldflags)dnl
AC_MSG_RESULT([$perl_embed_ldflags])])
