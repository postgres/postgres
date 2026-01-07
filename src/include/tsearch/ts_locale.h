/*-------------------------------------------------------------------------
 *
 * ts_locale.h
 *		locale compatibility layer for tsearch
 *
 * Copyright (c) 1998-2022, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_locale.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef __TSLOCALE_H__
#define __TSLOCALE_H__

#include <ctype.h>
#include <limits.h>

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "utils/pg_locale.h"

/*
 * towlower() and friends should be in <wctype.h>, but some pre-C99 systems
 * declare them in <wchar.h>, so include that too.
 */
#include <wchar.h>
#ifdef HAVE_WCTYPE_H
#include <wctype.h>
#endif

/* working state for tsearch_readline (should be a local var in caller) */
typedef struct
{
	FILE	   *fp;
	const char *filename;
	int			lineno;
	StringInfoData buf;			/* current input line, in UTF-8 */
	char	   *curline;		/* current input line, in DB's encoding */
	/* curline may be NULL, or equal to buf.data, or a palloc'd string */
	ErrorContextCallback cb;
} tsearch_readline_state;

#define TOUCHAR(x)	(*((const unsigned char *) (x)))

/* The second argument of t_iseq() must be a plain ASCII character */
#define t_iseq(x,c)		(TOUCHAR(x) == (unsigned char) (c))

/* Copy multibyte character of known byte length, return byte length. */
static inline int
ts_copychar_with_len(void *dest, const void *src, int length)
{
	memcpy(dest, src, length);
	return length;
}

/* Copy multibyte character from null-terminated string,  return byte length. */
static inline int
ts_copychar_cstr(void *dest, const void *src)
{
	return ts_copychar_with_len(dest, src, pg_mblen_cstr((const char *) src));
}

/* Historical macro for the above. */
#define COPYCHAR ts_copychar_cstr

#define GENERATE_T_ISCLASS_DECL(character_class) \
extern int	t_is##character_class##_with_len(const char *ptr, int len); \
extern int	t_is##character_class##_cstr(const char *ptr); \
extern int	t_is##character_class##_unbounded(const char *ptr); \
\
/* deprecated */ \
extern int	t_is##character_class(const char *ptr);

GENERATE_T_ISCLASS_DECL(alpha);
GENERATE_T_ISCLASS_DECL(digit);
GENERATE_T_ISCLASS_DECL(print);
GENERATE_T_ISCLASS_DECL(space);

extern char *lowerstr(const char *str);
extern char *lowerstr_with_len(const char *str, int len);

extern bool tsearch_readline_begin(tsearch_readline_state *stp,
								   const char *filename);
extern char *tsearch_readline(tsearch_readline_state *stp);
extern void tsearch_readline_end(tsearch_readline_state *stp);

#endif							/* __TSLOCALE_H__ */
