#
# Autoconf macros for configuring the build of Java JDBC Tools
#
# $Header: /cvsroot/pgsql/config/Attic/java.m4,v 1.1 2001/03/05 10:02:35 peter Exp $
#

# PGAC_PROG_ANT
# -------------
# Look for the ANT tool and set the output variable 'ANT' to 'ant'
# if found, empty otherwise
AC_DEFUN([PGAC_PROG_ANT],
         [AC_PATH_PROGS(ANT, [ant ant.sh ant.bat])])
AC_SUBST(ANT)
#AC_DEFUN([PGAC_PROG_ANT],[AC_CHECK_PROG(ANT, ant, ant)
#])

