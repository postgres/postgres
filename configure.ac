# Process this file with autoconf to produce a configure script.
# configure.ac

AC_INIT([pg_tde], [1.0.0-beta2], [https://github.com/percona/pg_tde/issues], [pg_tde])

AC_CANONICAL_HOST

# REQUIRE_LIB(name,lib,package,testfn,test_header_dir,test_include.h)
# name = The complete name of the library file without the extension.
# lib = The name of the library file without the 'lib' prefix and without the extension.
# package = The name of the package that holds the library
# testfn = One function included in the library that can be used for a test compilation.
# headerdir = The directory that contains required header.
# header = The header file to check for
# if there's a problem during the test compilation.
AC_DEFUN([REQUIRE_LIB], [ {
	AC_ARG_WITH([$1], AS_HELP_STRING([--with-$1=<path>],[Location where $6 is installed]),[],[with_$1=default])
	AS_IF([test "x$with_$1" == xdefault],
	[
		case $host_os in
			darwin*) libpathx=($(brew --cellar)/$3/*)
						tde_CPPFLAGS="$tde_CPPFLAGS -I$libpathx/include/$5"
						tde_LDFLAGS="$tde_LDFLAGS -L$libpathx/lib -l$2" ;;
				  *) tde_CPPFLAGS="$tde_CPPFLAGS -I/usr/include/$5" 
				  		tde_LDFLAGS="$tde_LDFLAGS -l$2" ;;
		esac
	], [ #AS_ELSE
		tde_CPPFLAGS="$tde_CPPFLAGS -I${with_$1}/include"
		tde_LDFLAGS="$tde_LDFLAGS -L${with_$1}/lib -l$2"
	])
	LDFLAGS="$LDFLAGS $tde_LDFLAGS"
	CPPFLAGS="$CPPFLAGS $tde_CPPFLAGS"

	AC_CHECK_HEADER($6, [],
	[
		AC_MSG_ERROR([header file <$6> is required, try specifying --with-$1])
	])


	AC_CHECK_LIB($2, $4, [],
	[
		AC_MSG_ERROR([$1 was not found, try specifying --with-$1])
	])
} ] )

 AC_CONFIG_HEADERS(src/include/config.h)
 
#=======================================

REQUIRE_LIB(libcurl, curl, curl, curl_easy_setopt, curl, curl/curl.h)

case $host_os in
	# on other systems libintl is a part of glibc
	darwin*) REQUIRE_LIB(libintl, intl, gettext, ngettext, [], libintl.h)
esac


AC_SUBST(tde_CPPFLAGS)
AC_SUBST(tde_LDFLAGS)

AC_CONFIG_FILES([Makefile])

AC_OUTPUT
