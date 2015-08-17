# Macros to detect C compiler features
# config/c-compiler.m4


# PGAC_C_SIGNED
# -------------
# Check if the C compiler understands signed types.
AC_DEFUN([PGAC_C_SIGNED],
[AC_CACHE_CHECK(for signed types, pgac_cv_c_signed,
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],
[signed char c; signed short s; signed int i;])],
[pgac_cv_c_signed=yes],
[pgac_cv_c_signed=no])])
if test x"$pgac_cv_c_signed" = xno ; then
  AC_DEFINE(signed,, [Define to empty if the C compiler does not understand signed types.])
fi])# PGAC_C_SIGNED



# PGAC_C_PRINTF_ARCHETYPE
# -----------------------
# Set the format archetype used by gcc to check printf type functions.  We
# prefer "gnu_printf", which includes what glibc uses, such as %m for error
# strings and %lld for 64 bit long longs.  GCC 4.4 introduced it.  It makes a
# dramatic difference on Windows.
AC_DEFUN([PGAC_PRINTF_ARCHETYPE],
[AC_CACHE_CHECK([for printf format archetype], pgac_cv_printf_archetype,
[ac_save_c_werror_flag=$ac_c_werror_flag
ac_c_werror_flag=yes
AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[extern int
pgac_write(int ignore, const char *fmt,...)
__attribute__((format(gnu_printf, 2, 3)));], [])],
                  [pgac_cv_printf_archetype=gnu_printf],
                  [pgac_cv_printf_archetype=printf])
ac_c_werror_flag=$ac_save_c_werror_flag])
AC_DEFINE_UNQUOTED([PG_PRINTF_ATTRIBUTE], [$pgac_cv_printf_archetype],
                   [Define to gnu_printf if compiler supports it, else printf.])
])# PGAC_PRINTF_ARCHETYPE


# PGAC_TYPE_64BIT_INT(TYPE)
# -------------------------
# Check if TYPE is a working 64 bit integer type. Set HAVE_TYPE_64 to
# yes or no respectively, and define HAVE_TYPE_64 if yes.
AC_DEFUN([PGAC_TYPE_64BIT_INT],
[define([Ac_define], [translit([have_$1_64], [a-z *], [A-Z_P])])dnl
define([Ac_cachevar], [translit([pgac_cv_type_$1_64], [ *], [_p])])dnl
AC_CACHE_CHECK([whether $1 is 64 bits], [Ac_cachevar],
[AC_RUN_IFELSE([AC_LANG_SOURCE(
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
}])],
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


# PGAC_TYPE_128BIT_INT
# ---------------------
# Check if __int128 is a working 128 bit integer type, and if so
# define PG_INT128_TYPE to that typename.  This currently only detects
# a GCC/clang extension, but support for different environments may be
# added in the future.
#
# For the moment we only test for support for 128bit math; support for
# 128bit literals and snprintf is not required.
AC_DEFUN([PGAC_TYPE_128BIT_INT],
[AC_CACHE_CHECK([for __int128], [pgac_cv__128bit_int],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([
/*
 * These are globals to discourage the compiler from folding all the
 * arithmetic tests down to compile-time constants.  We do not have
 * convenient support for 64bit literals at this point...
 */
__int128 a = 48828125;
__int128 b = 97656255;
],[
__int128 c,d;
a = (a << 12) + 1; /* 200000000001 */
b = (b << 12) + 5; /* 400000000005 */
/* use the most relevant arithmetic ops */
c = a * b;
d = (c + b) / b;
/* return different values, to prevent optimizations */
if (d != a+1)
  return 0;
return 1;
])],
[pgac_cv__128bit_int=yes],
[pgac_cv__128bit_int=no])])
if test x"$pgac_cv__128bit_int" = xyes ; then
  AC_DEFINE(PG_INT128_TYPE, __int128, [Define to the name of a signed 128-bit integer type.])
fi])# PGAC_TYPE_128BIT_INT


# PGAC_C_FUNCNAME_SUPPORT
# -----------------------
# Check if the C compiler understands __func__ (C99) or __FUNCTION__ (gcc).
# Define HAVE_FUNCNAME__FUNC or HAVE_FUNCNAME__FUNCTION accordingly.
AC_DEFUN([PGAC_C_FUNCNAME_SUPPORT],
[AC_CACHE_CHECK(for __func__, pgac_cv_funcname_func_support,
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <stdio.h>],
[printf("%s\n", __func__);])],
[pgac_cv_funcname_func_support=yes],
[pgac_cv_funcname_func_support=no])])
if test x"$pgac_cv_funcname_func_support" = xyes ; then
AC_DEFINE(HAVE_FUNCNAME__FUNC, 1,
          [Define to 1 if your compiler understands __func__.])
else
AC_CACHE_CHECK(for __FUNCTION__, pgac_cv_funcname_function_support,
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <stdio.h>],
[printf("%s\n", __FUNCTION__);])],
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
[AC_LINK_IFELSE([AC_LANG_PROGRAM([],
[({ _Static_assert(1, "foo"); })])],
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
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],
[[ int x; static int y[__builtin_types_compatible_p(__typeof__(x), int)]; ]])],
[pgac_cv__types_compatible=yes],
[pgac_cv__types_compatible=no])])
if test x"$pgac_cv__types_compatible" = xyes ; then
AC_DEFINE(HAVE__BUILTIN_TYPES_COMPATIBLE_P, 1,
          [Define to 1 if your compiler understands __builtin_types_compatible_p.])
fi])# PGAC_C_TYPES_COMPATIBLE



# PGAC_C_BUILTIN_BSWAP32
# -------------------------
# Check if the C compiler understands __builtin_bswap32(),
# and define HAVE__BUILTIN_BSWAP32 if so.
AC_DEFUN([PGAC_C_BUILTIN_BSWAP32],
[AC_CACHE_CHECK(for __builtin_bswap32, pgac_cv__builtin_bswap32,
[AC_COMPILE_IFELSE([AC_LANG_SOURCE(
[static unsigned long int x = __builtin_bswap32(0xaabbccdd);]
)],
[pgac_cv__builtin_bswap32=yes],
[pgac_cv__builtin_bswap32=no])])
if test x"$pgac_cv__builtin_bswap32" = xyes ; then
AC_DEFINE(HAVE__BUILTIN_BSWAP32, 1,
          [Define to 1 if your compiler understands __builtin_bswap32.])
fi])# PGAC_C_BUILTIN_BSWAP32



# PGAC_C_BUILTIN_CONSTANT_P
# -------------------------
# Check if the C compiler understands __builtin_constant_p(),
# and define HAVE__BUILTIN_CONSTANT_P if so.
AC_DEFUN([PGAC_C_BUILTIN_CONSTANT_P],
[AC_CACHE_CHECK(for __builtin_constant_p, pgac_cv__builtin_constant_p,
[AC_COMPILE_IFELSE([AC_LANG_SOURCE(
[[static int x; static int y[__builtin_constant_p(x) ? x : 1];]]
)],
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
[AC_LINK_IFELSE([AC_LANG_PROGRAM([],
[__builtin_unreachable();])],
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
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include <stdio.h>],
[#define debug(...) fprintf(stderr, __VA_ARGS__)
debug("%s", "blarg");
])],
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

# PGAC_HAVE_GCC__SYNC_CHAR_TAS
# -------------------------
# Check if the C compiler understands __sync_lock_test_and_set(char),
# and define HAVE_GCC__SYNC_CHAR_TAS
#
# NB: There are platforms where test_and_set is available but compare_and_swap
# is not, so test this separately.
# NB: Some platforms only do 32bit tas, others only do 8bit tas. Test both.
AC_DEFUN([PGAC_HAVE_GCC__SYNC_CHAR_TAS],
[AC_CACHE_CHECK(for builtin __sync char locking functions, pgac_cv_gcc_sync_char_tas,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([],
  [char lock = 0;
   __sync_lock_test_and_set(&lock, 1);
   __sync_lock_release(&lock);])],
  [pgac_cv_gcc_sync_char_tas="yes"],
  [pgac_cv_gcc_sync_char_tas="no"])])
if test x"$pgac_cv_gcc_sync_char_tas" = x"yes"; then
  AC_DEFINE(HAVE_GCC__SYNC_CHAR_TAS, 1, [Define to 1 if you have __sync_lock_test_and_set(char *) and friends.])
fi])# PGAC_HAVE_GCC__SYNC_CHAR_TAS

# PGAC_HAVE_GCC__SYNC_INT32_TAS
# -------------------------
# Check if the C compiler understands __sync_lock_test_and_set(),
# and define HAVE_GCC__SYNC_INT32_TAS
AC_DEFUN([PGAC_HAVE_GCC__SYNC_INT32_TAS],
[AC_CACHE_CHECK(for builtin __sync int32 locking functions, pgac_cv_gcc_sync_int32_tas,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([],
  [int lock = 0;
   __sync_lock_test_and_set(&lock, 1);
   __sync_lock_release(&lock);])],
  [pgac_cv_gcc_sync_int32_tas="yes"],
  [pgac_cv_gcc_sync_int32_tas="no"])])
if test x"$pgac_cv_gcc_sync_int32_tas" = x"yes"; then
  AC_DEFINE(HAVE_GCC__SYNC_INT32_TAS, 1, [Define to 1 if you have __sync_lock_test_and_set(int *) and friends.])
fi])# PGAC_HAVE_GCC__SYNC_INT32_TAS

# PGAC_HAVE_GCC__SYNC_INT32_CAS
# -------------------------
# Check if the C compiler understands __sync_compare_and_swap() for 32bit
# types, and define HAVE_GCC__SYNC_INT32_CAS if so.
AC_DEFUN([PGAC_HAVE_GCC__SYNC_INT32_CAS],
[AC_CACHE_CHECK(for builtin __sync int32 atomic operations, pgac_cv_gcc_sync_int32_cas,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([],
  [int val = 0;
   __sync_val_compare_and_swap(&val, 0, 37);])],
  [pgac_cv_gcc_sync_int32_cas="yes"],
  [pgac_cv_gcc_sync_int32_cas="no"])])
if test x"$pgac_cv_gcc_sync_int32_cas" = x"yes"; then
  AC_DEFINE(HAVE_GCC__SYNC_INT32_CAS, 1, [Define to 1 if you have __sync_compare_and_swap(int *, int, int).])
fi])# PGAC_HAVE_GCC__SYNC_INT32_CAS

# PGAC_HAVE_GCC__SYNC_INT64_CAS
# -------------------------
# Check if the C compiler understands __sync_compare_and_swap() for 64bit
# types, and define HAVE_GCC__SYNC_INT64_CAS if so.
AC_DEFUN([PGAC_HAVE_GCC__SYNC_INT64_CAS],
[AC_CACHE_CHECK(for builtin __sync int64 atomic operations, pgac_cv_gcc_sync_int64_cas,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([],
  [PG_INT64_TYPE lock = 0;
   __sync_val_compare_and_swap(&lock, 0, (PG_INT64_TYPE) 37);])],
  [pgac_cv_gcc_sync_int64_cas="yes"],
  [pgac_cv_gcc_sync_int64_cas="no"])])
if test x"$pgac_cv_gcc_sync_int64_cas" = x"yes"; then
  AC_DEFINE(HAVE_GCC__SYNC_INT64_CAS, 1, [Define to 1 if you have __sync_compare_and_swap(int64 *, int64, int64).])
fi])# PGAC_HAVE_GCC__SYNC_INT64_CAS

# PGAC_HAVE_GCC__ATOMIC_INT32_CAS
# -------------------------
# Check if the C compiler understands __atomic_compare_exchange_n() for 32bit
# types, and define HAVE_GCC__ATOMIC_INT32_CAS if so.
AC_DEFUN([PGAC_HAVE_GCC__ATOMIC_INT32_CAS],
[AC_CACHE_CHECK(for builtin __atomic int32 atomic operations, pgac_cv_gcc_atomic_int32_cas,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([],
  [int val = 0;
   int expect = 0;
   __atomic_compare_exchange_n(&val, &expect, 37, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);])],
  [pgac_cv_gcc_atomic_int32_cas="yes"],
  [pgac_cv_gcc_atomic_int32_cas="no"])])
if test x"$pgac_cv_gcc_atomic_int32_cas" = x"yes"; then
  AC_DEFINE(HAVE_GCC__ATOMIC_INT32_CAS, 1, [Define to 1 if you have __atomic_compare_exchange_n(int *, int *, int).])
fi])# PGAC_HAVE_GCC__ATOMIC_INT32_CAS

# PGAC_HAVE_GCC__ATOMIC_INT64_CAS
# -------------------------
# Check if the C compiler understands __atomic_compare_exchange_n() for 64bit
# types, and define HAVE_GCC__ATOMIC_INT64_CAS if so.
AC_DEFUN([PGAC_HAVE_GCC__ATOMIC_INT64_CAS],
[AC_CACHE_CHECK(for builtin __atomic int64 atomic operations, pgac_cv_gcc_atomic_int64_cas,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([],
  [PG_INT64_TYPE val = 0;
   PG_INT64_TYPE expect = 0;
   __atomic_compare_exchange_n(&val, &expect, 37, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);])],
  [pgac_cv_gcc_atomic_int64_cas="yes"],
  [pgac_cv_gcc_atomic_int64_cas="no"])])
if test x"$pgac_cv_gcc_atomic_int64_cas" = x"yes"; then
  AC_DEFINE(HAVE_GCC__ATOMIC_INT64_CAS, 1, [Define to 1 if you have __atomic_compare_exchange_n(int64 *, int *, int64).])
fi])# PGAC_HAVE_GCC__ATOMIC_INT64_CAS

# PGAC_SSE42_CRC32_INTRINSICS
# -----------------------
# Check if the compiler supports the x86 CRC instructions added in SSE 4.2,
# using the _mm_crc32_u8 and _mm_crc32_u32 intrinsic functions. (We don't
# test the 8-byte variant, _mm_crc32_u64, but it is assumed to be present if
# the other ones are, on x86-64 platforms)
#
# An optional compiler flag can be passed as argument (e.g. -msse4.2). If the
# intrinsics are supported, sets pgac_sse42_crc32_intrinsics, and CFLAGS_SSE42.
AC_DEFUN([PGAC_SSE42_CRC32_INTRINSICS],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_sse42_crc32_intrinsics_$1])])dnl
AC_CACHE_CHECK([for _mm_crc32_u8 and _mm_crc32_u32 with CFLAGS=$1], [Ac_cachevar],
[pgac_save_CFLAGS=$CFLAGS
CFLAGS="$pgac_save_CFLAGS $1"
AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <nmmintrin.h>],
  [unsigned int crc = 0;
   crc = _mm_crc32_u8(crc, 0);
   crc = _mm_crc32_u32(crc, 0);
   /* return computed value, to prevent the above being optimized away */
   return crc == 0;])],
  [Ac_cachevar=yes],
  [Ac_cachevar=no])
CFLAGS="$pgac_save_CFLAGS"])
if test x"$Ac_cachevar" = x"yes"; then
  CFLAGS_SSE42="$1"
  pgac_sse42_crc32_intrinsics=yes
fi
undefine([Ac_cachevar])dnl
])# PGAC_SSE42_CRC32_INTRINSICS
