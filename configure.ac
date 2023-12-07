# Process this file with autoconf to produce a configure script.
# configure.ac

AC_INIT([pg_tde], [0.1-mvp], [https://github.com/Percona-Lab/postgres-tde-ext/issues])

AC_CANONICAL_HOST

# REQUIRE_LIB(name,lib,testfn,test_include.h)
# name = The complete name of the library file without the extension.
# lib = The name of the library file without the 'lib' prefix and without the extension.
# testfn = One function included in the library that can be used for a test compilation.
# header = header file to check for
# if there's a problem during the test compilation.
AC_DEFUN([REQUIRE_LIB], [ {
AC_ARG_WITH([$1], AS_HELP_STRING([--with-$1=<path>],[Location where $4 is installed]),[],[with_$1=default])
	AS_IF([test "x$with_$1" == xdefault],
	[
		case $host_os in
			darwin*) libpathx=($HOMEBREW_CELLAR/$2/*)
						tde_CPPFLAGS="$tde_CPPFLAGS -I$libpathx/include/$2"
						tde_LDFLAGS="$tde_LDFLAGS -L$libpathx/lib -l$2" ;;
				  *) tde_CPPFLAGS="$tde_CPPFLAGS -I/usr/include/$2" 
				  		tde_LDFLAGS="$tde_LDFLAGS -l$2" ;;
		esac
	], [ #AS_ELSE
		tde_CPPFLAGS="$tde_CPPFLAGS -I${with_$1}/include"
		tde_LDFLAGS="$tde_LDFLAGS -L${with_$1}/lib -l$2"
	])

	LDFLAGS="$LDFLAGS $tde_LDFLAGS"
	CPPFLAGS="$CPPFLAGS $tde_CPPFLAGS"

	AC_CHECK_HEADER($4, [],
	[
		AC_MSG_ERROR([header file <$4> is required, try specifying --with-$1])
	])

	AC_CHECK_LIB($2, $3, [],
	[
		AC_MSG_ERROR([$2 was not found, try specifying --with-$1])
	])
} ] )

#=======================================

REQUIRE_LIB(jsonc, json-c, json_object_get, json_object.h)
REQUIRE_LIB(libcurl, curl, curl_easy_setopt, curl/curl.h)

AC_SUBST(tde_CPPFLAGS)
AC_SUBST(tde_LDFLAGS)

AC_CONFIG_FILES([Makefile])

AC_OUTPUT
