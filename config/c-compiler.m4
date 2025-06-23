# Macros to detect C compiler features
# config/c-compiler.m4


# PGAC_PRINTF_ARCHETYPE
# ---------------------
# Select the format archetype to be used by gcc to check printf-type functions.
# We prefer "gnu_printf", as that most closely matches the features supported
# by src/port/snprintf.c (particularly the %m conversion spec).  However,
# on some NetBSD versions, that doesn't work while "__syslog__" does.
# If all else fails, use "printf".
AC_DEFUN([PGAC_PRINTF_ARCHETYPE],
[AC_CACHE_CHECK([for printf format archetype], pgac_cv_printf_archetype,
[pgac_cv_printf_archetype=gnu_printf
PGAC_TEST_PRINTF_ARCHETYPE
if [[ "$ac_archetype_ok" = no ]]; then
  pgac_cv_printf_archetype=__syslog__
  PGAC_TEST_PRINTF_ARCHETYPE
  if [[ "$ac_archetype_ok" = no ]]; then
    pgac_cv_printf_archetype=printf
  fi
fi])
AC_DEFINE_UNQUOTED([PG_PRINTF_ATTRIBUTE], [$pgac_cv_printf_archetype],
[Define to best printf format archetype, usually gnu_printf if available.])
])# PGAC_PRINTF_ARCHETYPE

# Subroutine: test $pgac_cv_printf_archetype, set $ac_archetype_ok to yes or no
AC_DEFUN([PGAC_TEST_PRINTF_ARCHETYPE],
[ac_save_c_werror_flag=$ac_c_werror_flag
ac_c_werror_flag=yes
AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[extern void pgac_write(int ignore, const char *fmt,...)
__attribute__((format($pgac_cv_printf_archetype, 2, 3)));],
[pgac_write(0, "error %s: %m", "foo");])],
                  [ac_archetype_ok=yes],
                  [ac_archetype_ok=no])
ac_c_werror_flag=$ac_save_c_werror_flag
])# PGAC_TEST_PRINTF_ARCHETYPE


# PGAC_TYPE_128BIT_INT
# --------------------
# Check if __int128 is a working 128 bit integer type, and if so
# define PG_INT128_TYPE to that typename, and define ALIGNOF_PG_INT128_TYPE
# as its alignment requirement.
#
# This currently only detects a GCC/clang extension, but support for other
# environments may be added in the future.
#
# For the moment we only test for support for 128bit math; support for
# 128bit literals and snprintf is not required.
AC_DEFUN([PGAC_TYPE_128BIT_INT],
[AC_CACHE_CHECK([for __int128], [pgac_cv__128bit_int],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([
/*
 * We don't actually run this test, just link it to verify that any support
 * functions needed for __int128 are present.
 *
 * These are globals to discourage the compiler from folding all the
 * arithmetic tests down to compile-time constants.  We do not have
 * convenient support for 128bit literals at this point...
 */
__int128 a = 48828125;
__int128 b = 97656250;
],[
__int128 c,d;
a = (a << 12) + 1; /* 200000000001 */
b = (b << 12) + 5; /* 400000000005 */
/* try the most relevant arithmetic ops */
c = a * b;
d = (c + b) / b;
/* must use the results, else compiler may optimize arithmetic away */
if (d != a+1)
  return 1;
])],
[pgac_cv__128bit_int=yes],
[pgac_cv__128bit_int=no])])
if test x"$pgac_cv__128bit_int" = xyes ; then
  # Use of non-default alignment with __int128 tickles bugs in some compilers.
  # If not cross-compiling, we can test for bugs and disable use of __int128
  # with buggy compilers.  If cross-compiling, hope for the best.
  # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=83925
  AC_CACHE_CHECK([for __int128 alignment bug], [pgac_cv__128bit_int_bug],
  [AC_RUN_IFELSE([AC_LANG_PROGRAM([
/* This must match the corresponding code in c.h: */
#if defined(__GNUC__) || defined(__SUNPRO_C)
#define pg_attribute_aligned(a) __attribute__((aligned(a)))
#elif defined(_MSC_VER)
#define pg_attribute_aligned(a) __declspec(align(a))
#endif
typedef __int128 int128a
#if defined(pg_attribute_aligned)
pg_attribute_aligned(8)
#endif
;
int128a holder;
void pass_by_val(void *buffer, int128a par) { holder = par; }
],[
long int i64 = 97656225L << 12;
int128a q;
pass_by_val(main, (int128a) i64);
q = (int128a) i64;
if (q != holder)
  return 1;
])],
  [pgac_cv__128bit_int_bug=ok],
  [pgac_cv__128bit_int_bug=broken],
  [pgac_cv__128bit_int_bug="assuming ok"])])
  if test x"$pgac_cv__128bit_int_bug" != xbroken ; then
    AC_DEFINE(PG_INT128_TYPE, __int128, [Define to the name of a signed 128-bit integer type.])
    AC_CHECK_ALIGNOF(PG_INT128_TYPE)
  fi
fi])# PGAC_TYPE_128BIT_INT



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



# PGAC_C_TYPEOF
# -------------
# Check if the C compiler understands typeof or a variant.  Define
# HAVE_TYPEOF if so, and define 'typeof' to the actual key word.
#
AC_DEFUN([PGAC_C_TYPEOF],
[AC_CACHE_CHECK(for typeof, pgac_cv_c_typeof,
[pgac_cv_c_typeof=no
for pgac_kw in typeof __typeof__; do
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],
[int x = 0;
$pgac_kw(x) y;
y = x;
return y;])],
[pgac_cv_c_typeof=$pgac_kw])
  test "$pgac_cv_c_typeof" != no && break
done])
if test "$pgac_cv_c_typeof" != no; then
  AC_DEFINE(HAVE_TYPEOF, 1,
            [Define to 1 if your compiler understands `typeof' or something similar.])
  if test "$pgac_cv_c_typeof" != typeof; then
    AC_DEFINE_UNQUOTED(typeof, $pgac_cv_c_typeof, [Define to how the compiler spells `typeof'.])
  fi
fi])# PGAC_C_TYPEOF



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


# PGAC_C_BUILTIN_CONSTANT_P
# -------------------------
# Check if the C compiler understands __builtin_constant_p(),
# and define HAVE__BUILTIN_CONSTANT_P if so.
# We need __builtin_constant_p("string literal") to be true, but some older
# compilers don't think that, so test for that case explicitly.
AC_DEFUN([PGAC_C_BUILTIN_CONSTANT_P],
[AC_CACHE_CHECK(for __builtin_constant_p, pgac_cv__builtin_constant_p,
[AC_COMPILE_IFELSE([AC_LANG_SOURCE(
[[static int x;
  static int y[__builtin_constant_p(x) ? x : 1];
  static int z[__builtin_constant_p("string literal") ? 1 : x];
]]
)],
[pgac_cv__builtin_constant_p=yes],
[pgac_cv__builtin_constant_p=no])])
if test x"$pgac_cv__builtin_constant_p" = xyes ; then
AC_DEFINE(HAVE__BUILTIN_CONSTANT_P, 1,
          [Define to 1 if your compiler understands __builtin_constant_p.])
fi])# PGAC_C_BUILTIN_CONSTANT_P



# PGAC_C_BUILTIN_OP_OVERFLOW
# --------------------------
# Check if the C compiler understands __builtin_$op_overflow(),
# and define HAVE__BUILTIN_OP_OVERFLOW if so.
#
# Check for the most complicated case, 64 bit multiplication, as a
# proxy for all of the operations.  To detect the case where the compiler
# knows the function but library support is missing, we must link not just
# compile, and store the results in global variables so the compiler doesn't
# optimize away the call.
AC_DEFUN([PGAC_C_BUILTIN_OP_OVERFLOW],
[AC_CACHE_CHECK(for __builtin_mul_overflow, pgac_cv__builtin_op_overflow,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([
#include <stdint.h>
int64_t a = 1;
int64_t b = 1;
int64_t result;
int oflo;
],
[oflo = __builtin_mul_overflow(a, b, &result);])],
[pgac_cv__builtin_op_overflow=yes],
[pgac_cv__builtin_op_overflow=no])])
if test x"$pgac_cv__builtin_op_overflow" = xyes ; then
AC_DEFINE(HAVE__BUILTIN_OP_OVERFLOW, 1,
          [Define to 1 if your compiler understands __builtin_$op_overflow.])
fi])# PGAC_C_BUILTIN_OP_OVERFLOW



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



# PGAC_C_COMPUTED_GOTO
# --------------------
# Check if the C compiler knows computed gotos (gcc extension, also
# available in at least clang).  If so, define HAVE_COMPUTED_GOTO.
#
# Checking whether computed gotos are supported syntax-wise ought to
# be enough, as the syntax is otherwise illegal.
AC_DEFUN([PGAC_C_COMPUTED_GOTO],
[AC_CACHE_CHECK(for computed goto support, pgac_cv_computed_goto,
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],
[[void *labeladdrs[] = {&&my_label};
  goto *labeladdrs[0];
  my_label:
  return 1;
]])],
[pgac_cv_computed_goto=yes],
[pgac_cv_computed_goto=no])])
if test x"$pgac_cv_computed_goto" = xyes ; then
AC_DEFINE(HAVE_COMPUTED_GOTO, 1,
          [Define to 1 if your compiler handles computed gotos.])
fi])# PGAC_C_COMPUTED_GOTO



# PGAC_CHECK_BUILTIN_FUNC
# -----------------------
# This is similar to AC_CHECK_FUNCS(), except that it will work for compiler
# builtin functions, as that usually fails to.
# The first argument is the function name, eg [__builtin_clzl], and the
# second is its argument list, eg [unsigned long x].  The current coding
# works only for a single argument named x; we might generalize that later.
# It's assumed that the function's result type is coercible to int.
# On success, we define "HAVEfuncname" (there's usually more than enough
# underscores already, so we don't add another one).
AC_DEFUN([PGAC_CHECK_BUILTIN_FUNC],
[AC_CACHE_CHECK(for $1, pgac_cv$1,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([
int
call$1($2)
{
    return $1(x);
}], [])],
[pgac_cv$1=yes],
[pgac_cv$1=no])])
if test x"${pgac_cv$1}" = xyes ; then
AC_DEFINE_UNQUOTED(AS_TR_CPP([HAVE$1]), 1,
                   [Define to 1 if your compiler understands $1.])
fi])# PGAC_CHECK_BUILTIN_FUNC



# PGAC_CHECK_BUILTIN_FUNC_PTR
# -----------------------
# Like PGAC_CHECK_BUILTIN_FUNC, except that the function is assumed to
# return a pointer type, and the argument(s) should be given literally.
# This handles some cases that PGAC_CHECK_BUILTIN_FUNC doesn't.
AC_DEFUN([PGAC_CHECK_BUILTIN_FUNC_PTR],
[AC_CACHE_CHECK(for $1, pgac_cv$1,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([
void *
call$1(void)
{
    return $1($2);
}], [])],
[pgac_cv$1=yes],
[pgac_cv$1=no])])
if test x"${pgac_cv$1}" = xyes ; then
AC_DEFINE_UNQUOTED(AS_TR_CPP([HAVE$1]), 1,
                   [Define to 1 if your compiler understands $1.])
fi])# PGAC_CHECK_BUILTIN_FUNC_PTR



# PGAC_PROG_VARCC_VARFLAGS_OPT
# ----------------------------
# Given a compiler, variable name and a string, check if the compiler
# supports the string as a command-line option. If it does, add the
# string to the given variable.
AC_DEFUN([PGAC_PROG_VARCC_VARFLAGS_OPT],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_prog_$1_cflags_$3])])dnl
AC_CACHE_CHECK([whether ${$1} supports $3, for $2], [Ac_cachevar],
[pgac_save_CFLAGS=$CFLAGS
pgac_save_CC=$CC
CC=${$1}
CFLAGS="${$2} $3"
ac_save_c_werror_flag=$ac_c_werror_flag
ac_c_werror_flag=yes
_AC_COMPILE_IFELSE([AC_LANG_PROGRAM()],
                   [Ac_cachevar=yes],
                   [Ac_cachevar=no])
ac_c_werror_flag=$ac_save_c_werror_flag
CFLAGS="$pgac_save_CFLAGS"
CC="$pgac_save_CC"])
if test x"$Ac_cachevar" = x"yes"; then
  $2="${$2} $3"
fi
undefine([Ac_cachevar])dnl
])# PGAC_PROG_VARCC_VARFLAGS_OPT



# PGAC_PROG_CC_CFLAGS_OPT
# -----------------------
# Given a string, check if the compiler supports the string as a
# command-line option. If it does, add the string to CFLAGS.
AC_DEFUN([PGAC_PROG_CC_CFLAGS_OPT], [
PGAC_PROG_VARCC_VARFLAGS_OPT(CC, CFLAGS, $1)
])# PGAC_PROG_CC_CFLAGS_OPT



# PGAC_PROG_CC_VAR_OPT
# --------------------
# Given a variable name and a string, check if the compiler supports
# the string as a command-line option. If it does, add the string to
# the given variable.
AC_DEFUN([PGAC_PROG_CC_VAR_OPT],
[PGAC_PROG_VARCC_VARFLAGS_OPT(CC, $1, $2)
])# PGAC_PROG_CC_VAR_OPT



# PGAC_PROG_VARCXX_VARFLAGS_OPT
# -----------------------------
# Given a compiler, variable name and a string, check if the compiler
# supports the string as a command-line option. If it does, add the
# string to the given variable.
AC_DEFUN([PGAC_PROG_VARCXX_VARFLAGS_OPT],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_prog_$1_cxxflags_$3])])dnl
AC_CACHE_CHECK([whether ${$1} supports $3, for $2], [Ac_cachevar],
[pgac_save_CXXFLAGS=$CXXFLAGS
pgac_save_CXX=$CXX
CXX=${$1}
CXXFLAGS="${$2} $3"
ac_save_cxx_werror_flag=$ac_cxx_werror_flag
ac_cxx_werror_flag=yes
AC_LANG_PUSH(C++)
_AC_COMPILE_IFELSE([AC_LANG_PROGRAM()],
                   [Ac_cachevar=yes],
                   [Ac_cachevar=no])
AC_LANG_POP([])
ac_cxx_werror_flag=$ac_save_cxx_werror_flag
CXXFLAGS="$pgac_save_CXXFLAGS"
CXX="$pgac_save_CXX"])
if test x"$Ac_cachevar" = x"yes"; then
  $2="${$2} $3"
fi
undefine([Ac_cachevar])dnl
])# PGAC_PROG_VARCXX_VARFLAGS_OPT



# PGAC_PROG_CXX_CFLAGS_OPT
# ------------------------
# Given a string, check if the compiler supports the string as a
# command-line option. If it does, add the string to CXXFLAGS.
AC_DEFUN([PGAC_PROG_CXX_CFLAGS_OPT],
[PGAC_PROG_VARCXX_VARFLAGS_OPT(CXX, CXXFLAGS, $1)
])# PGAC_PROG_CXX_CFLAGS_OPT



# PGAC_PROG_CC_LD_VARFLAGS_OPT
# ------------------------
# Given a string, check if the compiler supports the string as a
# command-line option. If it does, add to the given variable.
# For reasons you'd really rather not know about, this checks whether
# you can link to a particular function, not just whether you can link.
# In fact, we must actually check that the resulting program runs :-(
AC_DEFUN([PGAC_PROG_CC_LD_VARFLAGS_OPT],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_prog_cc_$1_$2])])dnl
AC_CACHE_CHECK([whether $CC supports $2, for $1], [Ac_cachevar],
[pgac_save_LDFLAGS=$LDFLAGS
LDFLAGS="$pgac_save_LDFLAGS $2"
AC_RUN_IFELSE([AC_LANG_PROGRAM([extern void $3 (); void (*fptr) () = $3;],[])],
              [Ac_cachevar=yes],
              [Ac_cachevar=no],
              [Ac_cachevar="assuming no"])
LDFLAGS="$pgac_save_LDFLAGS"])
if test x"$Ac_cachevar" = x"yes"; then
  $1="${$1} $2"
fi
undefine([Ac_cachevar])dnl
])# PGAC_PROG_CC_LD_VARFLAGS_OPT

# PGAC_PROG_CC_LDFLAGS_OPT
# ------------------------
# Convenience wrapper around PGAC_PROG_CC_LD_VARFLAGS_OPT that adds to
# LDFLAGS.
AC_DEFUN([PGAC_PROG_CC_LDFLAGS_OPT],
[PGAC_PROG_CC_LD_VARFLAGS_OPT(LDFLAGS, [$1], [$2])
])# PGAC_PROG_CC_LDFLAGS_OPT


# PGAC_HAVE_GCC__SYNC_CHAR_TAS
# ----------------------------
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
# -----------------------------
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
# -----------------------------
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
  AC_DEFINE(HAVE_GCC__SYNC_INT32_CAS, 1, [Define to 1 if you have __sync_val_compare_and_swap(int *, int, int).])
fi])# PGAC_HAVE_GCC__SYNC_INT32_CAS

# PGAC_HAVE_GCC__SYNC_INT64_CAS
# -----------------------------
# Check if the C compiler understands __sync_compare_and_swap() for 64bit
# types, and define HAVE_GCC__SYNC_INT64_CAS if so.
AC_DEFUN([PGAC_HAVE_GCC__SYNC_INT64_CAS],
[AC_CACHE_CHECK(for builtin __sync int64 atomic operations, pgac_cv_gcc_sync_int64_cas,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <stdint.h>],
  [int64_t lock = 0;
   __sync_val_compare_and_swap(&lock, 0, (int64_t) 37);])],
  [pgac_cv_gcc_sync_int64_cas="yes"],
  [pgac_cv_gcc_sync_int64_cas="no"])])
if test x"$pgac_cv_gcc_sync_int64_cas" = x"yes"; then
  AC_DEFINE(HAVE_GCC__SYNC_INT64_CAS, 1, [Define to 1 if you have __sync_val_compare_and_swap(int64_t *, int64_t, int64_t).])
fi])# PGAC_HAVE_GCC__SYNC_INT64_CAS

# PGAC_HAVE_GCC__ATOMIC_INT32_CAS
# -------------------------------
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
# -------------------------------
# Check if the C compiler understands __atomic_compare_exchange_n() for 64bit
# types, and define HAVE_GCC__ATOMIC_INT64_CAS if so.
AC_DEFUN([PGAC_HAVE_GCC__ATOMIC_INT64_CAS],
[AC_CACHE_CHECK(for builtin __atomic int64 atomic operations, pgac_cv_gcc_atomic_int64_cas,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <stdint.h>],
  [int64_t val = 0;
   int64_t expect = 0;
   __atomic_compare_exchange_n(&val, &expect, 37, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);])],
  [pgac_cv_gcc_atomic_int64_cas="yes"],
  [pgac_cv_gcc_atomic_int64_cas="no"])])
if test x"$pgac_cv_gcc_atomic_int64_cas" = x"yes"; then
  AC_DEFINE(HAVE_GCC__ATOMIC_INT64_CAS, 1, [Define to 1 if you have __atomic_compare_exchange_n(int64 *, int64 *, int64).])
fi])# PGAC_HAVE_GCC__ATOMIC_INT64_CAS

# PGAC_SSE42_CRC32_INTRINSICS
# ---------------------------
# Check if the compiler supports the x86 CRC instructions added in SSE 4.2,
# using the _mm_crc32_u8 and _mm_crc32_u32 intrinsic functions. (We don't
# test the 8-byte variant, _mm_crc32_u64, but it is assumed to be present if
# the other ones are, on x86-64 platforms)
#
# If the intrinsics are supported, sets pgac_sse42_crc32_intrinsics.
#
# To detect the case where the compiler knows the function but library support
# is missing, we must link not just compile, and store the results in global
# variables so the compiler doesn't optimize away the call.
AC_DEFUN([PGAC_SSE42_CRC32_INTRINSICS],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_sse42_crc32_intrinsics])])dnl
AC_CACHE_CHECK([for _mm_crc32_u8 and _mm_crc32_u32], [Ac_cachevar],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <nmmintrin.h>
    unsigned int crc;
    #if defined(__has_attribute) && __has_attribute (target)
    __attribute__((target("sse4.2")))
    #endif
    static int crc32_sse42_test(void)
    {
      crc = _mm_crc32_u8(crc, 0);
      crc = _mm_crc32_u32(crc, 0);
      /* return computed value, to prevent the above being optimized away */
      return crc == 0;
    }],
  [return crc32_sse42_test();])],
  [Ac_cachevar=yes],
  [Ac_cachevar=no])])
if test x"$Ac_cachevar" = x"yes"; then
  pgac_sse42_crc32_intrinsics=yes
fi
undefine([Ac_cachevar])dnl
])# PGAC_SSE42_CRC32_INTRINSICS

# PGAC_AVX512_PCLMUL_INTRINSICS
# ---------------------------
# Check if the compiler supports AVX-512 carryless multiplication
# and three-way exclusive-or instructions used for computing CRC.
# AVX-512F is assumed to be supported if the above are.
#
# If the intrinsics are supported, sets pgac_avx512_pclmul_intrinsics.
AC_DEFUN([PGAC_AVX512_PCLMUL_INTRINSICS],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_avx512_pclmul_intrinsics])])dnl
AC_CACHE_CHECK([for _mm512_clmulepi64_epi128], [Ac_cachevar],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <immintrin.h>
    __m512i x;
    __m512i y;

    #if defined(__has_attribute) && __has_attribute (target)
    __attribute__((target("vpclmulqdq,avx512vl")))
    #endif
    static int avx512_pclmul_test(void)
    {
      __m128i z;

      x = _mm512_xor_si512(_mm512_zextsi128_si512(_mm_cvtsi32_si128(0)), x);
      y = _mm512_clmulepi64_epi128(x, y, 0);
      z = _mm_ternarylogic_epi64(
                _mm512_castsi512_si128(y),
                _mm512_extracti32x4_epi32(y, 1),
                _mm512_extracti32x4_epi32(y, 2),
                0x96);
      return _mm_crc32_u64(0, _mm_extract_epi64(z, 0));
    }],
  [return avx512_pclmul_test();])],
  [Ac_cachevar=yes],
  [Ac_cachevar=no])])
if test x"$Ac_cachevar" = x"yes"; then
  pgac_avx512_pclmul_intrinsics=yes
fi
undefine([Ac_cachevar])dnl
])# PGAC_AVX512_PCLMUL_INTRINSICS

# PGAC_ARMV8_CRC32C_INTRINSICS
# ----------------------------
# Check if the compiler supports the CRC32C instructions using the __crc32cb,
# __crc32ch, __crc32cw, and __crc32cd intrinsic functions. These instructions
# were first introduced in ARMv8 in the optional CRC Extension, and became
# mandatory in ARMv8.1.
#
# An optional compiler flag can be passed as argument (e.g.
# -march=armv8-a+crc). If the intrinsics are supported, sets
# pgac_armv8_crc32c_intrinsics, and CFLAGS_CRC.
AC_DEFUN([PGAC_ARMV8_CRC32C_INTRINSICS],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_armv8_crc32c_intrinsics_$1])])dnl
AC_CACHE_CHECK([for __crc32cb, __crc32ch, __crc32cw, and __crc32cd with CFLAGS=$1], [Ac_cachevar],
[pgac_save_CFLAGS=$CFLAGS
CFLAGS="$pgac_save_CFLAGS $1"
AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <arm_acle.h>
unsigned int crc;],
  [crc = __crc32cb(crc, 0);
   crc = __crc32ch(crc, 0);
   crc = __crc32cw(crc, 0);
   crc = __crc32cd(crc, 0);
   /* return computed value, to prevent the above being optimized away */
   return crc == 0;])],
  [Ac_cachevar=yes],
  [Ac_cachevar=no])
CFLAGS="$pgac_save_CFLAGS"])
if test x"$Ac_cachevar" = x"yes"; then
  CFLAGS_CRC="$1"
  pgac_armv8_crc32c_intrinsics=yes
fi
undefine([Ac_cachevar])dnl
])# PGAC_ARMV8_CRC32C_INTRINSICS

# PGAC_LOONGARCH_CRC32C_INTRINSICS
# ---------------------------
# Check if the compiler supports the LoongArch CRCC instructions, using
# __builtin_loongarch_crcc_w_b_w, __builtin_loongarch_crcc_w_h_w,
# __builtin_loongarch_crcc_w_w_w and __builtin_loongarch_crcc_w_d_w
# intrinsic functions.
#
# We test for the 8-byte variant since platforms capable of running
# Postgres are 64-bit only (as of PG17), and we know CRC instructions
# are available there without a runtime check.
#
# If the intrinsics are supported, sets pgac_loongarch_crc32c_intrinsics.
AC_DEFUN([PGAC_LOONGARCH_CRC32C_INTRINSICS],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_loongarch_crc32c_intrinsics])])dnl
AC_CACHE_CHECK(
  [for __builtin_loongarch_crcc_w_b_w, __builtin_loongarch_crcc_w_h_w, __builtin_loongarch_crcc_w_w_w and __builtin_loongarch_crcc_w_d_w],
  [Ac_cachevar],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([unsigned int crc;],
  [crc = __builtin_loongarch_crcc_w_b_w(0, crc);
   crc = __builtin_loongarch_crcc_w_h_w(0, crc);
   crc = __builtin_loongarch_crcc_w_w_w(0, crc);
   crc = __builtin_loongarch_crcc_w_d_w(0, crc);
   /* return computed value, to prevent the above being optimized away */
   return crc == 0;])],
  [Ac_cachevar=yes],
  [Ac_cachevar=no])])
if test x"$Ac_cachevar" = x"yes"; then
  pgac_loongarch_crc32c_intrinsics=yes
fi
undefine([Ac_cachevar])dnl
])# PGAC_LOONGARCH_CRC32C_INTRINSICS

# PGAC_XSAVE_INTRINSICS
# ---------------------
# Check if the compiler supports the XSAVE instructions using the _xgetbv
# intrinsic function.
#
# If the intrinsics are supported, sets pgac_xsave_intrinsics.
AC_DEFUN([PGAC_XSAVE_INTRINSICS],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_xsave_intrinsics])])dnl
AC_CACHE_CHECK([for _xgetbv], [Ac_cachevar],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <immintrin.h>
    #if defined(__has_attribute) && __has_attribute (target)
    __attribute__((target("xsave")))
    #endif
    static int xsave_test(void)
    {
      return _xgetbv(0) & 0xe0;
    }],
  [return xsave_test();])],
  [Ac_cachevar=yes],
  [Ac_cachevar=no])])
if test x"$Ac_cachevar" = x"yes"; then
  pgac_xsave_intrinsics=yes
fi
undefine([Ac_cachevar])dnl
])# PGAC_XSAVE_INTRINSICS

# PGAC_AVX512_POPCNT_INTRINSICS
# -----------------------------
# Check if the compiler supports the AVX-512 popcount instructions using the
# _mm512_setzero_si512, _mm512_maskz_loadu_epi8, _mm512_popcnt_epi64,
# _mm512_add_epi64, and _mm512_reduce_add_epi64 intrinsic functions.
#
# If the intrinsics are supported, sets pgac_avx512_popcnt_intrinsics.
AC_DEFUN([PGAC_AVX512_POPCNT_INTRINSICS],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_avx512_popcnt_intrinsics])])dnl
AC_CACHE_CHECK([for _mm512_popcnt_epi64], [Ac_cachevar],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <immintrin.h>
    #include <stdint.h>
    char buf[sizeof(__m512i)];

    #if defined(__has_attribute) && __has_attribute (target)
    __attribute__((target("avx512vpopcntdq,avx512bw")))
    #endif
    static int popcount_test(void)
    {
      int64_t popcnt = 0;
      __m512i accum = _mm512_setzero_si512();
      __m512i val = _mm512_maskz_loadu_epi8((__mmask64) 0xf0f0f0f0f0f0f0f0, (const __m512i *) buf);
      __m512i cnt = _mm512_popcnt_epi64(val);
      accum = _mm512_add_epi64(accum, cnt);
      popcnt = _mm512_reduce_add_epi64(accum);
      return (int) popcnt;
    }]],
  [return popcount_test();])],
  [Ac_cachevar=yes],
  [Ac_cachevar=no])])
if test x"$Ac_cachevar" = x"yes"; then
  pgac_avx512_popcnt_intrinsics=yes
fi
undefine([Ac_cachevar])dnl
])# PGAC_AVX512_POPCNT_INTRINSICS

# PGAC_SVE_POPCNT_INTRINSICS
# --------------------------
# Check if the compiler supports the SVE popcount instructions using the
# svptrue_b64, svdup_u64, svcntb, svld1_u64, svld1_u8, svadd_u64_x,
# svcnt_u64_x, svcnt_u8_x, svaddv_u64, svaddv_u8, svwhilelt_b8_s32,
# svand_n_u64_x, and svand_n_u8_x intrinsic functions.
#
# If the intrinsics are supported, sets pgac_sve_popcnt_intrinsics.
AC_DEFUN([PGAC_SVE_POPCNT_INTRINSICS],
[define([Ac_cachevar], [AS_TR_SH([pgac_cv_sve_popcnt_intrinsics])])dnl
AC_CACHE_CHECK([for svcnt_x], [Ac_cachevar],
[AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <arm_sve.h>

	char buf[128];

	#if defined(__has_attribute) && __has_attribute (target)
	__attribute__((target("arch=armv8-a+sve")))
	#endif
	static int popcount_test(void)
	{
		svbool_t	pred = svptrue_b64();
		svuint8_t	vec8;
		svuint64_t	accum1 = svdup_u64(0),
					accum2 = svdup_u64(0),
					vec64;
		char	   *p = buf;
		uint64_t	popcnt,
					mask = 0x5555555555555555;

		vec64 = svand_n_u64_x(pred, svld1_u64(pred, (const uint64_t *) p), mask);
		accum1 = svadd_u64_x(pred, accum1, svcnt_u64_x(pred, vec64));
		p += svcntb();

		vec64 = svand_n_u64_x(pred, svld1_u64(pred, (const uint64_t *) p), mask);
		accum2 = svadd_u64_x(pred, accum2, svcnt_u64_x(pred, vec64));
		p += svcntb();

		popcnt = svaddv_u64(pred, svadd_u64_x(pred, accum1, accum2));

		pred = svwhilelt_b8_s32(0, sizeof(buf));
		vec8 = svand_n_u8_x(pred, svld1_u8(pred, (const uint8_t *) p), 0x55);
		return (int) (popcnt + svaddv_u8(pred, svcnt_u8_x(pred, vec8)));
	}]],
  [return popcount_test();])],
  [Ac_cachevar=yes],
  [Ac_cachevar=no])])
if test x"$Ac_cachevar" = x"yes"; then
  pgac_sve_popcnt_intrinsics=yes
fi
undefine([Ac_cachevar])dnl
])# PGAC_SVE_POPCNT_INTRINSICS
