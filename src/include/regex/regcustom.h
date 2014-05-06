/*
 * Copyright (c) 1998, 1999 Henry Spencer.  All rights reserved.
 *
 * Development of this software was funded, in part, by Cray Research Inc.,
 * UUNET Communications Services Inc., Sun Microsystems Inc., and Scriptics
 * Corporation, none of whom are responsible for the results.  The author
 * thanks all of them.
 *
 * Redistribution and use in source and binary forms -- with or without
 * modification -- are permitted for any purpose, provided that
 * redistributions in source form retain this entire copyright notice and
 * indicate the origin and nature of any modifications.
 *
 * I'd appreciate being given credit for this package in the documentation
 * of software which uses it, but that is not a requirement.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * HENRY SPENCER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $PostgreSQL: pgsql/src/include/regex/regcustom.h,v 1.8 2009/12/01 21:00:24 tgl Exp $
 */

/* headers if any */
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

#include "mb/pg_wchar.h"


/* overrides for regguts.h definitions, if any */
#define FUNCPTR(name, args) (*name) args
#define MALLOC(n)		malloc(n)
#define FREE(p)			free(VS(p))
#define REALLOC(p,n)	realloc(VS(p),n)
#define assert(x)		Assert(x)

/* internal character type and related */
typedef pg_wchar chr;			/* the type itself */
typedef unsigned uchr;			/* unsigned type that will hold a chr */
typedef int celt;				/* type to hold chr, or NOCELT */

#define NOCELT	(-1)			/* celt value which is not valid chr */
#define CHR(c)	((unsigned char) (c))	/* turn char literal into chr literal */
#define DIGITVAL(c) ((c)-'0')	/* turn chr digit into its value */
#define CHRBITS 32				/* bits in a chr; must not use sizeof */
#define CHR_MIN 0x00000000		/* smallest and largest chr; the value */
#define CHR_MAX 0xfffffffe		/* CHR_MAX-CHR_MIN+1 should fit in uchr */

/* functions operating on chr */
#define iscalnum(x) pg_wc_isalnum(x)
#define iscalpha(x) pg_wc_isalpha(x)
#define iscdigit(x) pg_wc_isdigit(x)
#define iscspace(x) pg_wc_isspace(x)

/* and pick up the standard header */
#include "regex.h"
