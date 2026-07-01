/* Private header for tzdb code.  */

#ifndef PRIVATE_H

#define PRIVATE_H

/*
 * This file is in the public domain, so clarified as of
 * 1996-06-05 by Arthur David Olson.
 *
 * IDENTIFICATION
 *	  src/timezone/private.h
 */

/*
 * This header is for use ONLY with the time conversion code.
 * There is no guarantee that it will remain unchanged,
 * or that it will remain at all.
 * Do NOT copy it to any system include directory.
 * Thank you!
 */

/*
 * IANA now expects this symbol to be defined via a compiler switch,
 * but in PG we don't want to do it that way.
 */
#define TZDEFAULT	"/etc/localtime"

/*
 * IANA messes with some feature-test macros here, but in PG we want pretty
 * much all of that to be done by PG's configure script and c.h header.
 */

/*
 * For pre-C23 compilers, a substitute for static_assert.
 * Some of these compilers may warn if it is used outside the top level.
 */
#if __STDC_VERSION__ < 202311 && !defined static_assert
#define static_assert(cond) extern int static_assert_check[(cond) ? 1 : -1]
#endif

/* This string was in the Factory zone through version 2016f.  */
#ifndef GRANDPARENTED
#define GRANDPARENTED	"Local time zone must be set--see zic manual page"
#endif

/*
 * IANA has a bunch of HAVE_FOO #defines here, but in PG we want pretty
 * much all of that to be done by PG's configure script and c.h header.
 */

#define ATTRIBUTE_FALLTHROUGH pg_fallthrough
#define ATTRIBUTE_MAYBE_UNUSED pg_attribute_unused()
#define ATTRIBUTE_NORETURN pg_noreturn
#define ATTRIBUTE_PURE_114833
#define ATTRIBUTE_PURE_114833_HACK

/*
 * Nested includes
 * In PG, much of this was already done by c.h.
 */

#include "pgtime.h"

#include <limits.h>				/* for CHAR_BIT et al. */
#include <unistd.h>				/* for F_OK and R_OK */

#ifndef EINVAL
#define EINVAL ERANGE
#endif

#ifndef ELOOP
#define ELOOP EINVAL
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG EINVAL
#endif
#ifndef ENOMEM
#define ENOMEM EINVAL
#endif
#ifndef ENOTCAPABLE
#define ENOTCAPABLE EINVAL
#endif
#ifndef ENOTSUP
#define ENOTSUP EINVAL
#endif
#ifndef EOVERFLOW
#define EOVERFLOW EINVAL
#endif

/*
 * The maximum size of any created object, as a signed integer.
 * Although the C standard does not outright prohibit larger objects,
 * behavior is undefined if the result of pointer subtraction does not
 * fit into ptrdiff_t, and the code assumes in several places that
 * pointer subtraction works.  As a practical matter it's OK to not
 * support objects larger than this.
 */
#define INDEX_MAX ((ptrdiff_t) min(PTRDIFF_MAX, SIZE_MAX))

/*
 * Support ckd_add, ckd_sub, ckd_mul on C23 or recent-enough GCC-like
 * hosts, unless compiled with -DHAVE_STDCKDINT_H=0 or with pre-C23 EDG.
 */
#if !defined HAVE_STDCKDINT_H && defined __has_include
#if __has_include(<stdckdint.h>)
#define HAVE_STDCKDINT_H 1
#endif
#endif
#ifdef HAVE_STDCKDINT_H
#if HAVE_STDCKDINT_H
#include <stdckdint.h>
#endif
#elif defined __EDG__
/* Do nothing, to work around EDG bug <https://bugs.gnu.org/53256>.  */
#elif defined __has_builtin
#if __has_builtin(__builtin_add_overflow)
#define ckd_add(r, a, b) __builtin_add_overflow(a, b, r)
#endif
#if __has_builtin(__builtin_sub_overflow)
#define ckd_sub(r, a, b) __builtin_sub_overflow(a, b, r)
#endif
#if __has_builtin(__builtin_mul_overflow)
#define ckd_mul(r, a, b) __builtin_mul_overflow(a, b, r)
#endif
#elif 7 <= __GNUC__
#define ckd_add(r, a, b) __builtin_add_overflow(a, b, r)
#define ckd_sub(r, a, b) __builtin_sub_overflow(a, b, r)
#define ckd_mul(r, a, b) __builtin_mul_overflow(a, b, r)
#endif


/*
 * In PG, we always have these fields in struct pg_tm.
 */
#define TM_GMTOFF tm_gmtoff
#define TM_ZONE tm_zone


/*
 * Finally, some convenience items.
 */

#define TYPE_BIT(type) (CHAR_BIT * (ptrdiff_t) sizeof(type))
#define TYPE_SIGNED(type) (((type) -1) < 0)
#define TWOS_COMPLEMENT(type) (TYPE_SIGNED (type) && (! ~ (type) -1))

/*
 * Minimum and maximum of two values.  Use lower case to avoid
 * naming clashes with standard include files.
 */
#undef max
#undef min
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

/*
 * Max and min values of the integer type T, of which only the bottom
 * B bits are used, and where the highest-order used bit is considered
 * to be a sign bit if T is signed.
 */
#define MAXVAL(t, b)						\
  ((t) (((t) 1 << ((b) - 1 - TYPE_SIGNED(t)))			\
	- 1 + ((t) 1 << ((b) - 1 - TYPE_SIGNED(t)))))
#define MINVAL(t, b)						\
  ((t) (TYPE_SIGNED(t) ? - TWOS_COMPLEMENT(t) - MAXVAL(t, b) : 0))

/* The extreme time values, assuming no padding.  */
#define TIME_T_MIN MINVAL(pg_time_t, TYPE_BIT(pg_time_t))
#define TIME_T_MAX MAXVAL(pg_time_t, TYPE_BIT(pg_time_t))

/*
 * 302 / 1000 is log10(2.0) rounded up.
 * Subtract one for the sign bit if the type is signed;
 * add one for integer division truncation;
 * add one more for a minus sign if the type is signed.
 */
#define INT_STRLEN_MAXIMUM(type) \
	((TYPE_BIT(type) - TYPE_SIGNED(type)) * 302 / 1000 + \
	1 + TYPE_SIGNED(type))

/*
 * INITIALIZE(x)
 */
#define INITIALIZE(x)	((x) = 0)

/* Some platforms provide unreachable(), but let's rely on our own version */
#undef unreachable
#define unreachable() pg_unreachable()

/*
 * For the benefit of GNU folk...
 * '_(MSGID)' uses the current locale's message library string for MSGID.
 * The default is to use gettext if available, and use MSGID otherwise.
 * For PG's purposes, there is no need to support localization in zic.
 */

#undef _
#define _(msgid) (msgid)
#define N_(msgid) (msgid)

/* Handy constants that are independent of tzfile implementation.  */

/* 2**31 - 1 as a signed integer, and usable in #if.  */
#define TWO_31_MINUS_1 2147483647

enum
{
	SECSPERMIN = 60,
	MINSPERHOUR = 60,
	SECSPERHOUR = SECSPERMIN * MINSPERHOUR,
	HOURSPERDAY = 24,
	DAYSPERWEEK = 7,
	DAYSPERNYEAR = 365,
	DAYSPERLYEAR = DAYSPERNYEAR + 1,
	MONSPERYEAR = 12,
	YEARSPERREPEAT = 400		/* years before a Gregorian repeat */
};

#define SECSPERDAY	((int_fast32_t) SECSPERHOUR * HOURSPERDAY)

#define DAYSPERREPEAT		((int_fast32_t) 400 * 365 + 100 - 4 + 1)
#define SECSPERREPEAT		((int_fast64_t) DAYSPERREPEAT * SECSPERDAY)
#define AVGSECSPERYEAR		(SECSPERREPEAT / YEARSPERREPEAT)

/*
 * How many years to generate (in zic.c) or search through (in localtime.c).
 * This is two years larger than the obvious 400, to avoid edge cases.
 * E.g., suppose a rule applies from 2012 on with transitions
 * in March and September, plus one-off transitions in November 2013,
 * and suppose the rule cannot be expressed as a proleptic TZ string.
 * If zic looked only at the last 400 years, it would set max_year=2413,
 * with the intent that the 400 years 2014 through 2413 will be repeated.
 * The last transition listed in the tzfile would be in 2413-09,
 * less than 400 years after the last one-off transition in 2013-11.
 * Two years is not overkill for localtime.c, as a one-year bump
 * would mishandle 2023d's America/Ciudad_Juarez for November 2422.
 */
enum
{
years_of_observations = YEARSPERREPEAT + 2};

enum
{
	TM_SUNDAY,
	TM_MONDAY,
	TM_TUESDAY,
	TM_WEDNESDAY,
	TM_THURSDAY,
	TM_FRIDAY,
	TM_SATURDAY
};

enum
{
	TM_JANUARY,
	TM_FEBRUARY,
	TM_MARCH,
	TM_APRIL,
	TM_MAY,
	TM_JUNE,
	TM_JULY,
	TM_AUGUST,
	TM_SEPTEMBER,
	TM_OCTOBER,
	TM_NOVEMBER,
	TM_DECEMBER
};

enum
{
	TM_YEAR_BASE = 1900,
	TM_WDAY_BASE = TM_MONDAY,
	EPOCH_YEAR = 1970,
	EPOCH_WDAY = TM_THURSDAY
};

#define isleap(y) (((y) % 4) == 0 && (((y) % 100) != 0 || ((y) % 400) == 0))

/*
 * Since everything in isleap is modulo 400 (or a factor of 400), we know that
 *	isleap(y) == isleap(y % 400)
 * and so
 *	isleap(a + b) == isleap((a + b) % 400)
 * or
 *	isleap(a + b) == isleap(a % 400 + b % 400)
 * This is true even if % means modulo rather than Fortran remainder
 * (which is allowed by C89 but not by C99 or later).
 * We use this to avoid addition overflow problems.
 */

#define isleap_sum(a, b)	isleap((a) % 400 + (b) % 400)

#endif							/* !defined PRIVATE_H */
