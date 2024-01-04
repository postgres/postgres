/*-------------------------------------------------------------------------
 *
 * ts_locale.h
 *		locale compatibility layer for tsearch
 *
 * Copyright (c) 1998-2024, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_locale.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef __TSLOCALE_H__
#define __TSLOCALE_H__

#include <ctype.h>
#include <limits.h>
#include <wctype.h>

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "utils/pg_locale.h"

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

#define COPYCHAR(d,s)	memcpy(d, s, pg_mblen(s))

extern int	t_isdigit(const char *ptr);
extern int	t_isspace(const char *ptr);
extern int	t_isalpha(const char *ptr);
extern int	t_isalnum(const char *ptr);
extern int	t_isprint(const char *ptr);

extern char *lowerstr(const char *str);
extern char *lowerstr_with_len(const char *str, int len);

extern bool tsearch_readline_begin(tsearch_readline_state *stp,
								   const char *filename);
extern char *tsearch_readline(tsearch_readline_state *stp);
extern void tsearch_readline_end(tsearch_readline_state *stp);

#endif							/* __TSLOCALE_H__ */
