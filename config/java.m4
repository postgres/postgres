#
# Autoconf macros for configuring the build of Java JDBC Tools
#
# $Header: /cvsroot/pgsql/config/Attic/java.m4,v 1.2 2001/03/11 11:24:59 petere Exp $
#

# PGAC_PATH_ANT
# -------------
# Look for the ANT tool and set the output variable 'ANT' to 'ant'
# if found, empty otherwise
AC_DEFUN([PGAC_PATH_ANT],
         [AC_PATH_PROGS(ANT, [ant ant.sh ant.bat])])
