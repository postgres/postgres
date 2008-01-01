/*-------------------------------------------------------------------------
 *
 * ts_locale.h
 *		locale compatibility layer for tsearch
 *
 * Copyright (c) 1998-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/tsearch/ts_locale.h,v 1.5 2008/01/01 19:45:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef __TSLOCALE_H__
#define __TSLOCALE_H__

#include <ctype.h>
#include <limits.h>

#include "utils/pg_locale.h"
#include "mb/pg_wchar.h"

/*
 * towlower() and friends should be in <wctype.h>, but some pre-C99 systems
 * declare them in <wchar.h>.
 */
#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif
#ifdef HAVE_WCTYPE_H
#include <wctype.h>
#endif

#if defined(HAVE_WCSTOMBS) && defined(HAVE_TOWLOWER)
#define TS_USE_WIDE
#endif

#define TOUCHAR(x)	(*((const unsigned char *) (x)))

#ifdef TS_USE_WIDE

extern size_t wchar2char(char *to, const wchar_t *from, size_t tolen);
extern size_t char2wchar(wchar_t *to, size_t tolen, const char *from, size_t fromlen);

extern int	t_isdigit(const char *ptr);
extern int	t_isspace(const char *ptr);
extern int	t_isalpha(const char *ptr);
extern int	t_isprint(const char *ptr);

/* The second argument of t_iseq() must be a plain ASCII character */
#define t_iseq(x,c)		(TOUCHAR(x) == (unsigned char) (c))

#define COPYCHAR(d,s)	memcpy(d, s, pg_mblen(s))
#else							/* not TS_USE_WIDE */

#define t_isdigit(x)	isdigit(TOUCHAR(x))
#define t_isspace(x)	isspace(TOUCHAR(x))
#define t_isalpha(x)	isalpha(TOUCHAR(x))
#define t_isprint(x)	isprint(TOUCHAR(x))
#define t_iseq(x,c)		(TOUCHAR(x) == (unsigned char) (c))

#define COPYCHAR(d,s)	(*((unsigned char *) (d)) = TOUCHAR(s))
#endif   /* TS_USE_WIDE */

extern char *lowerstr(const char *str);
extern char *lowerstr_with_len(const char *str, int len);
extern char *t_readline(FILE *fp);

#endif   /* __TSLOCALE_H__ */
