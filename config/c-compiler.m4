# Macros to detect C compiler features
# config/c-compiler.m4


# PGAC_C_SIGNED
# -------------
# Check if the C compiler understands signed types.
AC_DEFUN([PGAC_C_SIGNED],
[AC_CACHE_CHECK(for signed types, pgac_cv_c_signed,
[AC_TRY_COMPILE([],
[signed char c; signed short s; signed int i;],
[pgac_cv_c_signed=yes],
[pgac_cv_c_signed=no])])
if test x"$pgac_cv_c_signed" = xno ; then
  AC_DEFINE(signed,, [Define to empty if the C compiler does not understand signed types.])
fi])# PGAC_C_SIGNED



# PGAC_C_INLINE
# -------------
# Check if the C compiler understands inline functions.
# Defines: inline, PG_USE_INLINE
AC_DEFUN([PGAC_C_INLINE],
[AC_C_INLINE
AC_CACHE_CHECK([for quiet inline (no complaint if unreferenced)], pgac_cv_c_inline_quietly,
  [pgac_cv_c_inline_quietly=no
  if test "$ac_cv_c_inline" != no; then
    pgac_c_inline_save_werror=$ac_c_werror_flag
    ac_c_werror_flag=yes
    AC_LINK_IFELSE([AC_LANG_PROGRAM([static inline int fun () {return 0;}],[])],
                   [pgac_cv_c_inline_quietly=yes])
    ac_c_werror_flag=$pgac_c_inline_save_werror
  fi])
if test "$pgac_cv_c_inline_quietly" != no; then
  AC_DEFINE_UNQUOTED([PG_USE_INLINE], 1,
    [Define to 1 if "static inline" works without unwanted warnings from ]
    [compilations where static inline functions are defined but not called.])
fi
])# PGAC_C_INLINE



# PGAC_TYPE_64BIT_INT(TYPE)
# -------------------------
# Check if TYPE is a working 64 bit integer type. Set HAVE_TYPE_64 to
# yes or no respectively, and define HAVE_TYPE_64 if yes.
AC_DEFUN([PGAC_TYPE_64BIT_INT],
[define([Ac_define], [translit([have_$1_64], [a-z *], [A-Z_P])])dnl
define([Ac_cachevar], [translit([pgac_cv_type_$1_64], [ *], [_p])])dnl
AC_CACHE_CHECK([whether $1 is 64 bits], [Ac_cachevar],
[AC_TRY_RUN(
[typedef $1 ac_int64;

/*
 * These are globals to discourage the compiler from folding all the
 * arithmetic tests down to compile-time constants.
 */
ac_int64 a = 20000001;
ac_int64 b = 40000005;

int does_int64_work()
{
  ac_int64 c,d;

  if (sizeof(ac_int64) != 8)
    return 0;			/* definitely not the right size */

  /* Do perfunctory checks to see if 64-bit arithmetic seems to work */
  c = a * b;
  d = (c + b) / b;
  if (d != a+1)
    return 0;
  return 1;
}
main() {
  exit(! does_int64_work());
}],
[Ac_cachevar=yes],
[Ac_cachevar=no],
[# If cross-compiling, check the size reported by the compiler and
# trust that the arithmetic works.
AC_COMPILE_IFELSE([AC_LANG_BOOL_COMPILE_TRY([], [sizeof($1) == 8])],
                  Ac_cachevar=yes,
                  Ac_cachevar=no)])])

Ac_define=$Ac_cachevar
if test x"$Ac_cachevar" = xyes ; then
  AC_DEFINE(Ac_define, 1, [Define to 1 if `]$1[' works and is 64 bits.])
fi
undefine([Ac_define])dnl
undefine([Ac_cachevar])dnl
])# PGAC_TYPE_64BIT_INT



# PGAC_C_FUNCNAME_SUPPORT
# -----------------------
# Check if the C compiler understands __func__ (C99) or __FUNCTION__ (gcc).
# Define HAVE_FUNCNAME__FUNC or HAVE_FUNCNAME__FUNCTION accordingly.
AC_DEFUN([PGAC_C_FUNCNAME_SUPPORT],
[AC_CACHE_CHECK(for __func__, pgac_cv_funcname_func_support,
[AC_TRY_COMPILE([#include <stdio.h>],
[printf("%s\n", __func__);],
[pgac_cv_funcname_func_support=yes],
[pgac_cv_funcname_func_support=no])])
if test x"$pgac_cv_funcname_func_support" = xyes ; then
AC_DEFINE(HAVE_FUNCNAME__FUNC, 1,
          [Define to 1 if your compiler understands __func__.])
else
AC_CACHE_CHECK(for __FUNCTION__, pgac_cv_funcname_function_support,
[AC_TRY_COMPILE([#include <stdio.h>],
[printf("%s\n", __FUNCTION__);],
[pgac_cv_funcname_function_support=yes],
[pgac_cv_funcname_function_support=no])])
if test x"$pgac_cv_funcname_function_support" = xyes ; then
AC_DEFINE(HAVE_FUNCNAME__FUNCTION, 1,
          [Define to 1 if your compiler understands __FUNCTION__.])
fi
fi])# PGAC_C_FUNCNAME_SUPPORT



# PGAC_C_STATIC_ASSERT
# --------------------
# Check if the C compiler understands _Static_assert(),
# and define HAVE__STATIC_ASSERT if so.
#
# We actually check the syntax ({ _Static_assert(...) }), because we need
# gcc-style compound expressions to be able to wrap the thing into macros.
AC_DEFUN([PGAC_C_STATIC_ASSERT],
[AC_CACHE_CHECK(for _Static_assert, pgac_cv__static_assert,
[AC_TRY_LINK([],
[({ _Static_assert(1, "foo"); })],
[pgac_cv__static_assert=yes],
[pgac_cv__static_assert=no])])
if test x"$pgac_cv__static_assert" = xyes ; then
AC_DEFINE(HAVE__STATIC_ASSERT, 1,
          [Define to 1 if your compiler understands _Static_assert.])
fi])# PGAC_C_STATIC_ASSERT



# PGAC_C_TYPES_COMPATIBLE
# -----------------------
# Check if the C compiler understands __builtin_types_compatible_p,
# and define HAVE__BUILTIN_TYPES_COMPATIBLE_P if so.
#
# We check usage with __typeof__, though it's unlikely any compiler would
# have the former and not the latter.
AC_DEFUN([PGAC_C_TYPES_COMPATIBLE],
[AC_CACHE_CHECK(for __builtin_types_compatible_p, pgac_cv__types_compatible,
[AC_TRY_COMPILE([],
[ int x; static int y[__builtin_types_compatible_p(__typeof__(x), int)]; ],
[pgac_cv__types_compatible=yes],
[pgac_cv__types_compatible=no])])
if test x"$pgac_cv__types_compatible" = xyes ; then
AC_DEFINE(HAVE__BUILTIN_TYPES_COMPATIBLE_P, 1,
          [Define to 1 if your compiler understands __builtin_types_compatible_p.])
fi])# PGAC_C_TYPES_COMPATIBLE



# PGAC_C_BUILTIN_CONSTANT_P
# -------------------------
# Check if the C compiler understands __builtin_constant_p(),
# and define HAVE__BUILTIN_CONSTANT_P if so.
AC_DEFUN([PGAC_C_BUILTIN_CONSTANT_P],
[AC_CACHE_CHECK(for __builtin_constant_p, pgac_cv__builtin_constant_p,
[AC_TRY_COMPILE([static int x; static int y[__builtin_constant_p(x) ? x : 1];],
[],
[pgac_cv__builtin_constant_p=yes],
[pgac_cv__builtin_constant_p=no])])
if test x"$pgac_cv__builtin_constant_p" = xyes ; then
AC_DEFINE(HAVE__BUILTIN_CONSTANT_P, 1,
          [Define to 1 if your compiler understands __builtin_constant_p.])
fi])# PGAC_C_BUILTIN_CONSTANT_P



# PGAC_C_BUILTIN_UNREACHABLE
# --------------------------
# Check if the C compiler understands __builtin_unreachable(),
# and define HAVE__BUILTIN_UNREACHABLE if so.
#
# NB: Don't get the idea of putting a for(;;); or such before the
# __builtin_unreachable() call.  Some compilers would remove it before linking
# and only a warning instead of an error would be produced.
AC_DEFUN([PGAC_C_BUILTIN_UNREACHABLE],
[AC_CACHE_CHECK(for __builtin_unreachable, pgac_cv__builtin_unreachable,
[AC_TRY_LINK([],
[__builtin_unreachable();],
[pgac_cv__builtin_unreachable=yes],
[pgac_cv__builtin_unreachable=no])])
if test x"$pgac_cv__builtin_unreachable" = xyes ; then
AC_DEFINE(HAVE__BUILTIN_UNREACHABLE, 1,
          [Define to 1 if your compiler understands __builtin_unreachable.])
fi])# PGAC_C_BUILTIN_UNREACHABLE



# PGAC_C_VA_ARGS
# --------------
# Check if the C compiler understands C99-style variadic macros,
# and define HAVE__VA_ARGS if so.
AC_DEFUN([PGAC_C_VA_ARGS],
[AC_CACHE_CHECK(for __VA_ARGS__, pgac_cv__va_args,
[AC_TRY_COMPILE([#include <stdio.h>],
[#define debug(...) fprintf(stderr, __VA_ARGS__)
debug("%s", "blarg");
],
[pgac_cv__va_args=yes],
[pgac_cv__va_args=no])])
if test x"$pgac_cv__va_args" = xyes ; then
AC_DEFINE(HAVE__VA_ARGS, 1,
          [Define to 1 if your compiler understands __VA_ARGS__ in macros.])
fi])# PGAC_C_VA_ARGS



# PGAC_PROG_CC_CFLAGS_OPT
# -----------------------
# Given a string, check if the compiler supports the string as a
# command-line option. If it does, add the string to CFLAGS.
AC_DEFUN([PGAC_PROG_CC_CFLAGS_OPT],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_prog_cc_cflags_$1])])dnl
AC_CACHE_CHECK([whether $CC supports $1], [Ac_cachevar],
[pgac_save_CFLAGS=$CFLAGS
CFLAGS="$pgac_save_CFLAGS $1"
ac_save_c_werror_flag=$ac_c_werror_flag
ac_c_werror_flag=yes
_AC_COMPILE_IFELSE([AC_LANG_PROGRAM()],
                   [Ac_cachevar=yes],
                   [Ac_cachevar=no])
ac_c_werror_flag=$ac_save_c_werror_flag
CFLAGS="$pgac_save_CFLAGS"])
if test x"$Ac_cachevar" = x"yes"; then
  CFLAGS="$CFLAGS $1"
fi
undefine([Ac_cachevar])dnl
])# PGAC_PROG_CC_CFLAGS_OPT



# PGAC_PROG_CC_VAR_OPT
# -----------------------
# Given a variable name and a string, check if the compiler supports
# the string as a command-line option. If it does, add the string to
# the given variable.
AC_DEFUN([PGAC_PROG_CC_VAR_OPT],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_prog_cc_cflags_$2])])dnl
AC_CACHE_CHECK([whether $CC supports $2], [Ac_cachevar],
[pgac_save_CFLAGS=$CFLAGS
CFLAGS="$pgac_save_CFLAGS $2"
ac_save_c_werror_flag=$ac_c_werror_flag
ac_c_werror_flag=yes
_AC_COMPILE_IFELSE([AC_LANG_PROGRAM()],
                   [Ac_cachevar=yes],
                   [Ac_cachevar=no])
ac_c_werror_flag=$ac_save_c_werror_flag
CFLAGS="$pgac_save_CFLAGS"])
if test x"$Ac_cachevar" = x"yes"; then
  $1="${$1} $2"
fi
undefine([Ac_cachevar])dnl
])# PGAC_PROG_CC_CFLAGS_OPT



# PGAC_PROG_CC_LDFLAGS_OPT
# ------------------------
# Given a string, check if the compiler supports the string as a
# command-line option. If it does, add the string to LDFLAGS.
# For reasons you'd really rather not know about, this checks whether
# you can link to a particular function, not just whether you can link.
# In fact, we must actually check that the resulting program runs :-(
AC_DEFUN([PGAC_PROG_CC_LDFLAGS_OPT],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_prog_cc_ldflags_$1])])dnl
AC_CACHE_CHECK([whether $CC supports $1], [Ac_cachevar],
[pgac_save_LDFLAGS=$LDFLAGS
LDFLAGS="$pgac_save_LDFLAGS $1"
AC_RUN_IFELSE([AC_LANG_PROGRAM([extern void $2 (); void (*fptr) () = $2;],[])],
              [Ac_cachevar=yes],
              [Ac_cachevar=no],
              [Ac_cachevar="assuming no"])
LDFLAGS="$pgac_save_LDFLAGS"])
if test x"$Ac_cachevar" = x"yes"; then
  LDFLAGS="$LDFLAGS $1"
fi
undefine([Ac_cachevar])dnl
])# PGAC_PROG_CC_LDFLAGS_OPT
