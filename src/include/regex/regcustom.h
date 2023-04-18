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
 * src/include/regex/regcustom.h
 */

/* headers if any */

/*
 * It's against Postgres coding conventions to include postgres.h in a
 * header file, but we allow the violation here because the regexp library
 * files specifically intend this file to supply application-dependent
 * headers, and are careful to include this file before anything else.
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <wctype.h>

#include "mb/pg_wchar.h"

#include "miscadmin.h"			/* needed by rstacktoodeep */


/* overrides for regguts.h definitions, if any */
#define FUNCPTR(name, args) (*name) args
#define MALLOC(n)		palloc_extended((n), MCXT_ALLOC_NO_OOM)
#define FREE(p)			pfree(VS(p))
#define REALLOC(p,n)	repalloc_extended(VS(p),(n), MCXT_ALLOC_NO_OOM)
#define INTERRUPT(re)	CHECK_FOR_INTERRUPTS()
#define assert(x)		Assert(x)

/* internal character type and related */
typedef pg_wchar chr;			/* the type itself */
typedef unsigned uchr;			/* unsigned type that will hold a chr */

#define CHR(c)	((unsigned char) (c))	/* turn char literal into chr literal */
#define DIGITVAL(c) ((c)-'0')	/* turn chr digit into its value */
#define CHRBITS 32				/* bits in a chr; must not use sizeof */
#define CHR_MIN 0x00000000		/* smallest and largest chr; the value */
#define CHR_MAX 0x7ffffffe		/* CHR_MAX-CHR_MIN+1 must fit in an int, and
								 * CHR_MAX+1 must fit in a chr variable */

/*
 * Check if a chr value is in range.  Ideally we'd just write this as
 *		((c) >= CHR_MIN && (c) <= CHR_MAX)
 * However, if chr is unsigned and CHR_MIN is zero, the first part of that
 * is a no-op, and certain overly-nannyish compilers give warnings about it.
 * So we leave that out here.  If you want to make chr signed and/or CHR_MIN
 * not zero, redefine this macro as above.  Callers should assume that the
 * macro may multiply evaluate its argument, even though it does not today.
 */
#define CHR_IS_IN_RANGE(c)	((c) <= CHR_MAX)

/*
 * MAX_SIMPLE_CHR is the cutoff between "simple" and "complicated" processing
 * in the color map logic.  It should usually be chosen high enough to ensure
 * that all common characters are <= MAX_SIMPLE_CHR.  However, very large
 * values will be counterproductive since they cause more regex setup time.
 * Also, small values can be helpful for testing the high-color-map logic
 * with plain old ASCII input.
 */
#define MAX_SIMPLE_CHR	0x7FF	/* suitable value for Unicode */

/* functions operating on chr */
#define iscalnum(x) pg_wc_isalnum(x)
#define iscalpha(x) pg_wc_isalpha(x)
#define iscdigit(x) pg_wc_isdigit(x)
#define iscspace(x) pg_wc_isspace(x)

/* and pick up the standard header */
#include "regex.h"
