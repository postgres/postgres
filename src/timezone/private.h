#ifndef PRIVATE_H
#define PRIVATE_H

/*
 * This file is in the public domain, so clarified as of
 * 1996-06-05 by Arthur David Olson (arthur_david_olson@nih.gov).
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/private.h,v 1.11 2005/02/23 04:34:21 momjian Exp $
 */

/*
 * This header is for use ONLY with the time conversion code.
 * There is no guarantee that it will remain unchanged,
 * or that it will remain at all.
 * Do NOT copy it to any system include directory.
 * Thank you!
 */

#include <limits.h>				/* for CHAR_BIT */
#include <sys/wait.h>			/* for WIFEXITED and WEXITSTATUS */
#include <unistd.h>				/* for F_OK and R_OK */

#include "pgtime.h"


#ifndef WIFEXITED
#define WIFEXITED(status)	(((status) & 0xff) == 0)
#endif   /* !defined WIFEXITED */
#ifndef WEXITSTATUS
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#endif   /* !defined WEXITSTATUS */

/* Unlike <ctype.h>'s isdigit, this also works if c < 0 | c > UCHAR_MAX. */
#define is_digit(c) ((unsigned)(c) - '0' <= 9)

/*
 * SunOS 4.1.1 headers lack EXIT_SUCCESS.
 */

#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS	0
#endif   /* !defined EXIT_SUCCESS */

/*
 * SunOS 4.1.1 headers lack EXIT_FAILURE.
 */

#ifndef EXIT_FAILURE
#define EXIT_FAILURE	1
#endif   /* !defined EXIT_FAILURE */

/*
 * SunOS 4.1.1 libraries lack remove.
 */

#ifndef remove
extern int	unlink(const char *filename);

#define remove	unlink
#endif   /* !defined remove */

/*
 * Private function declarations.
 */
extern char *icalloc(int nelem, int elsize);
extern char *icatalloc(char *old, const char *new);
extern char *icpyalloc(const char *string);
extern char *imalloc(int n);
extern void *irealloc(void *pointer, int size);
extern void icfree(char *pointer);
extern void ifree(char *pointer);
extern char *scheck(const char *string, const char *format);


/*
 * Finally, some convenience items.
 */

#ifndef TRUE
#define TRUE	1
#endif   /* !defined TRUE */

#ifndef FALSE
#define FALSE	0
#endif   /* !defined FALSE */

#ifndef TYPE_BIT
#define TYPE_BIT(type)	(sizeof (type) * CHAR_BIT)
#endif   /* !defined TYPE_BIT */

#ifndef TYPE_SIGNED
#define TYPE_SIGNED(type) (((type) -1) < 0)
#endif   /* !defined TYPE_SIGNED */

#ifndef INT_STRLEN_MAXIMUM
/*
 * 302 / 1000 is log10(2.0) rounded up.
 * Subtract one for the sign bit if the type is signed;
 * add one for integer division truncation;
 * add one more for a minus sign if the type is signed.
 */
#define INT_STRLEN_MAXIMUM(type) \
	((TYPE_BIT(type) - TYPE_SIGNED(type)) * 302 / 1000 + 1 + TYPE_SIGNED(type))
#endif   /* !defined INT_STRLEN_MAXIMUM */

#undef _
#define _(msgid) (msgid)

/*
 * UNIX was a registered trademark of The Open Group in 2003.
 */

#endif   /* !defined PRIVATE_H */
