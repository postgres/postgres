#ifndef __TSLOCALE_H__
#define __TSLOCALE_H__

#include "postgres.h"

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

#ifdef WIN32

size_t wchar2char( const char *to, const wchar_t *from, size_t len );
size_t char2wchar( const wchar_t *to, const char *from, size_t len );

#else /* WIN32 */

/* correct mbstowcs */
#define char2wchar mbstowcs
#define wchar2char wcstombs

#endif /* WIN32 */
 
#endif /* defined(HAVE_WCSTOMBS) && defined(HAVE_TOWLOWER) */ 

#endif  /* __TSLOCALE_H__ */
