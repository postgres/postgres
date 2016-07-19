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

#include <limits.h>				/* for CHAR_BIT et al. */
#include <sys/wait.h>			/* for WIFEXITED and WEXITSTATUS */
#include <unistd.h>				/* for F_OK and R_OK */

#include "pgtime.h"

#define GRANDPARENTED	"Local time zone must be set--see zic manual page"

/*
 * IANA has a bunch of HAVE_FOO #defines here, but in PG we want pretty
 * much all of that to be done by PG's configure script.
 */

#ifndef ENOTSUP
#define ENOTSUP EINVAL
#endif
#ifndef EOVERFLOW
#define EOVERFLOW EINVAL
#endif

#ifndef WIFEXITED
#define WIFEXITED(status)	(((status) & 0xff) == 0)
#endif   /* !defined WIFEXITED */
#ifndef WEXITSTATUS
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#endif   /* !defined WEXITSTATUS */

/* Unlike <ctype.h>'s isdigit, this also works if c < 0 | c > UCHAR_MAX. */
#define is_digit(c) ((unsigned)(c) - '0' <= 9)

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t) -1)
#endif

/*
 * SunOS 4.1.1 libraries lack remove.
 */

#ifndef remove
extern int	unlink(const char *filename);

#define remove	unlink
#endif   /* !defined remove */


/*
 * Finally, some convenience items.
 */

#ifndef TYPE_BIT
#define TYPE_BIT(type)	(sizeof (type) * CHAR_BIT)
#endif   /* !defined TYPE_BIT */

#ifndef TYPE_SIGNED
#define TYPE_SIGNED(type) (((type) -1) < 0)
#endif   /* !defined TYPE_SIGNED */

#define TWOS_COMPLEMENT(t) ((t) ~ (t) 0 < 0)

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

#ifndef INT_STRLEN_MAXIMUM
/*
 * 302 / 1000 is log10(2.0) rounded up.
 * Subtract one for the sign bit if the type is signed;
 * add one for integer division truncation;
 * add one more for a minus sign if the type is signed.
 */
#define INT_STRLEN_MAXIMUM(type) \
	((TYPE_BIT(type) - TYPE_SIGNED(type)) * 302 / 1000 + \
	1 + TYPE_SIGNED(type))
#endif   /* !defined INT_STRLEN_MAXIMUM */

/*
 * INITIALIZE(x)
 */
#define INITIALIZE(x)  ((x) = 0)

#undef _
#define _(msgid) (msgid)

#ifndef YEARSPERREPEAT
#define YEARSPERREPEAT		400 /* years before a Gregorian repeat */
#endif   /* !defined YEARSPERREPEAT */

/*
 * The Gregorian year averages 365.2425 days, which is 31556952 seconds.
 */

#ifndef AVGSECSPERYEAR
#define AVGSECSPERYEAR		31556952L
#endif   /* !defined AVGSECSPERYEAR */

#ifndef SECSPERREPEAT
#define SECSPERREPEAT		((int64) YEARSPERREPEAT * (int64) AVGSECSPERYEAR)
#endif   /* !defined SECSPERREPEAT */

#ifndef SECSPERREPEAT_BITS
#define SECSPERREPEAT_BITS	34	/* ceil(log2(SECSPERREPEAT)) */
#endif   /* !defined SECSPERREPEAT_BITS */

#endif   /* !defined PRIVATE_H */
