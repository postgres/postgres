# config/ac_func_accept_argtypes.m4
# This comes from the official Autoconf macro archive at
# <http://research.cys.de/autoconf-archive/>


dnl @synopsis AC_FUNC_ACCEPT_ARGTYPES
dnl
dnl Checks the data types of the three arguments to accept(). Results are
dnl placed into the symbols ACCEPT_TYPE_RETURN and ACCEPT_TYPE_ARG[123],
dnl consistent with the following example:
dnl
dnl       #define ACCEPT_TYPE_RETURN int
dnl       #define ACCEPT_TYPE_ARG1 int
dnl       #define ACCEPT_TYPE_ARG2 struct sockaddr *
dnl       #define ACCEPT_TYPE_ARG3 socklen_t
dnl
dnl NOTE: This is just a modified version of the AC_FUNC_SELECT_ARGTYPES
dnl macro. Credit for that one goes to David MacKenzie et. al.
dnl
dnl @version $Id: ac_func_accept_argtypes.m4,v 1.1 1999/12/03 11:29:29 simons Exp $
dnl @author Daniel Richard G. <skunk@mit.edu>
dnl

# PostgreSQL local changes: In the original version ACCEPT_TYPE_ARG3
# is a pointer type. That's kind of useless because then you can't
# use the macro to define a corresponding variable. We also make the
# reasonable(?) assumption that you can use arg3 for getsocktype etc.
# as well (i.e., anywhere POSIX.2 has socklen_t).
#
# arg2 can also be `const' (e.g., RH 4.2). Change the order of tests
# for arg3 so that `int' is first, in case there is no prototype at all.
#
# Solaris 7 and 8 have arg3 as 'void *' (disguised as 'Psocklen_t'
# which is *not* 'socklen_t *').  If we detect that, then we assume
# 'int' as the result, because that ought to work best.
#
# On Win32, accept() returns 'unsigned int PASCAL'
# Win64 uses SOCKET for return and arg1

AC_DEFUN([AC_FUNC_ACCEPT_ARGTYPES],
[AC_MSG_CHECKING([types of arguments for accept()])
 AC_CACHE_VAL(ac_cv_func_accept_return,dnl
 [AC_CACHE_VAL(ac_cv_func_accept_arg1,dnl
  [AC_CACHE_VAL(ac_cv_func_accept_arg2,dnl
   [AC_CACHE_VAL(ac_cv_func_accept_arg3,dnl
    [for ac_cv_func_accept_return in 'int' 'unsigned int PASCAL' 'SOCKET WSAAPI'; do
      for ac_cv_func_accept_arg1 in 'int' 'unsigned int' 'SOCKET'; do
       for ac_cv_func_accept_arg2 in 'struct sockaddr *' 'const struct sockaddr *' 'void *'; do
        for ac_cv_func_accept_arg3 in 'int' 'size_t' 'socklen_t' 'unsigned int' 'void'; do
         AC_COMPILE_IFELSE([AC_LANG_SOURCE(
[#include <sys/types.h>
#include <sys/socket.h>
extern $ac_cv_func_accept_return accept ($ac_cv_func_accept_arg1, $ac_cv_func_accept_arg2, $ac_cv_func_accept_arg3 *);])],
         [ac_not_found=no; break 4], [ac_not_found=yes])
       done
      done
     done
    done
    if test "$ac_not_found" = yes; then
      AC_MSG_ERROR([could not determine argument types])
    fi
    if test "$ac_cv_func_accept_arg3" = "void"; then
      ac_cv_func_accept_arg3=int
    fi
    ])dnl AC_CACHE_VAL
   ])dnl AC_CACHE_VAL
  ])dnl AC_CACHE_VAL
 ])dnl AC_CACHE_VAL
 AC_MSG_RESULT([$ac_cv_func_accept_return, $ac_cv_func_accept_arg1, $ac_cv_func_accept_arg2, $ac_cv_func_accept_arg3 *])
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_RETURN, $ac_cv_func_accept_return,
                    [Define to the return type of 'accept'])
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_ARG1, $ac_cv_func_accept_arg1,
                    [Define to the type of arg 1 of 'accept'])
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_ARG2, $ac_cv_func_accept_arg2,
                    [Define to the type of arg 2 of 'accept'])
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_ARG3, $ac_cv_func_accept_arg3,
                    [Define to the type of arg 3 of 'accept'])
])
