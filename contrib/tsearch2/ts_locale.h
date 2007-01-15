#ifndef __TSLOCALE_H__
#define __TSLOCALE_H__

#include "postgres.h"
#include "utils/pg_locale.h"
#include "mb/pg_wchar.h"

#include <ctype.h>
#include <limits.h>

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

#ifdef TS_USE_WIDE
#endif   /* TS_USE_WIDE */


#define TOUCHAR(x)	(*((unsigned char*)(x)))

#ifdef TS_USE_WIDE
size_t		char2wchar(wchar_t *to, const char *from, size_t len);

#ifdef WIN32

size_t		wchar2char(char *to, const wchar_t *from, size_t len);

#else							/* WIN32 */

/* correct wcstombs */
#define wchar2char wcstombs

#endif   /* WIN32 */

#define t_isdigit(x)	( pg_mblen(x)==1 && isdigit( TOUCHAR(x) ) )
#define t_isspace(x)	( pg_mblen(x)==1 && isspace( TOUCHAR(x) ) )
extern int	_t_isalpha(const char *ptr);

#define t_isalpha(x)	( (pg_mblen(x)==1) ? isalpha( TOUCHAR(x) ) : _t_isalpha(x) )
extern int	_t_isprint(const char *ptr);

#define t_isprint(x)	( (pg_mblen(x)==1) ? isprint( TOUCHAR(x) ) : _t_isprint(x) )
/*
 * t_iseq() should be called only for ASCII symbols
 */
#define t_iseq(x,c) ( (pg_mblen(x)==1) ? ( TOUCHAR(x) == ((unsigned char)(c)) ) : false )

#define COPYCHAR(d,s)	do {					\
	int lll = pg_mblen( s );					\
												\
	while( lll-- )								\
		TOUCHAR((d)+lll) = TOUCHAR((s)+lll);	\
} while(0)

#else							/* not def TS_USE_WIDE */

#define t_isdigit(x)	isdigit( TOUCHAR(x) )
#define t_isspace(x)	isspace( TOUCHAR(x) )
#define t_isalpha(x)	isalpha( TOUCHAR(x) )
#define t_isprint(x)	isprint( TOUCHAR(x) )
#define t_iseq(x,c) ( TOUCHAR(x) == ((unsigned char)(c)) )

#define COPYCHAR(d,s)	TOUCHAR(d) = TOUCHAR(s)
#endif

char	   *lowerstr(char *str);

#endif   /* __TSLOCALE_H__ */
