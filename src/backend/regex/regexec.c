/*-
 * Copyright (c) 1992, 1993, 1994 Henry Spencer.
 * Copyright (c) 1992, 1993, 1994
 *		The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Henry Spencer.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *	  must display the following acknowledgement:
 *		This product includes software developed by the University of
 *		California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *		@(#)regexec.c	8.3 (Berkeley) 3/20/94
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)regexec.c	8.3 (Berkeley) 3/20/94";

#endif	 /* LIBC_SCCS and not lint */

#include "postgres.h"

/*
 * the outer shell of regexec()
 *
 * This file includes engine.c *twice*, after muchos fiddling with the
 * macros that code uses.  This lets the same code operate on two different
 * representations for state sets.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <assert.h>

#include "regex/regex.h"
#include "regex/utils.h"
#include "regex/regex2.h"

static int	nope = 0;			/* for use in asserts; shuts lint up */

/* macros for manipulating states, small version */
#define states	long
#define states1 states			/* for later use in regexec() decision */
#define CLEAR(v)		((v) = 0)
#define SET0(v, n)		((v) &= ~(1 << (n)))
#define SET1(v, n)		((v) |= 1 << (n))
#define ISSET(v, n)		((v) & (1 << (n)))
#define ASSIGN(d, s)	((d) = (s))
#define EQ(a, b)		((a) == (b))
#define STATEVARS		int dummy		/* dummy version */
#define STATESETUP(m, n)		/* nothing */
#define STATETEARDOWN(m)		/* nothing */
#define SETUP(v)		((v) = 0)
#define onestate		int
#define INIT(o, n)		((o) = (unsigned)1 << (n))
#define INC(o)	((o) <<= 1)
#define ISSTATEIN(v, o) ((v) & (o))
/* some abbreviations; note that some of these know variable names! */
/* do "if I'm here, I can also be there" etc without branches */
#define FWD(dst, src, n)		((dst) |= ((unsigned)(src)&(here)) << (n))
#define BACK(dst, src, n)		((dst) |= ((unsigned)(src)&(here)) >> (n))
#define ISSETBACK(v, n) ((v) & ((unsigned)here >> (n)))
/* function names */
#define SNAMES					/* engine.c looks after details */

#include "engine.c"

/* now undo things */
#undef	states
#undef	CLEAR
#undef	SET0
#undef	SET1
#undef	ISSET
#undef	ASSIGN
#undef	EQ
#undef	STATEVARS
#undef	STATESETUP
#undef	STATETEARDOWN
#undef	SETUP
#undef	onestate
#undef	INIT
#undef	INC
#undef	ISSTATEIN
#undef	FWD
#undef	BACK
#undef	ISSETBACK
#undef	SNAMES

/* macros for manipulating states, large version */
#define states	char *
#define CLEAR(v)		memset(v, 0, m->g->nstates)
#define SET0(v, n)		((v)[n] = 0)
#define SET1(v, n)		((v)[n] = 1)
#define ISSET(v, n)		((v)[n])
#define ASSIGN(d, s)	memcpy(d, s, m->g->nstates)
#define EQ(a, b)		(memcmp(a, b, m->g->nstates) == 0)
#define STATEVARS		int vn; char *space
#define STATESETUP(m, nv)		{ (m)->space = malloc((nv)*(m)->g->nstates); \
								if ((m)->space == NULL) return(REG_ESPACE); \
								(m)->vn = 0; }
#define STATETEARDOWN(m)		{ free((m)->space); }
#define SETUP(v)		((v) = &m->space[m->vn++ * m->g->nstates])
#define onestate		int
#define INIT(o, n)		((o) = (n))
#define INC(o)	((o)++)
#define ISSTATEIN(v, o) ((v)[o])
/* some abbreviations; note that some of these know variable names! */
/* do "if I'm here, I can also be there" etc without branches */
#define FWD(dst, src, n)		((dst)[here+(n)] |= (src)[here])
#define BACK(dst, src, n)		((dst)[here-(n)] |= (src)[here])
#define ISSETBACK(v, n) ((v)[here - (n)])
/* function names */
#define LNAMES					/* flag */

#include "engine.c"

/*
 - regexec - interface for matching
 = extern int regexec(const regex_t *, const char *, size_t, \
 =										regmatch_t [], int);
 = #define		REG_NOTBOL		00001
 = #define		REG_NOTEOL		00002
 = #define		REG_STARTEND	00004
 = #define		REG_TRACE		00400	// tracing of execution
 = #define		REG_LARGE		01000	// force large representation
 = #define		REG_BACKR		02000	// force use of backref code
 *
 * We put this here so we can exploit knowledge of the state representation
 * when choosing which matcher to call.  Also, by this point the matchers
 * have been prototyped.
 */
int								/* 0 success, REG_NOMATCH failure */
pg95_regexec(preg, string, nmatch, pmatch, eflags)
const regex_t *preg;
const char *string;
size_t		nmatch;
regmatch_t *pmatch;
int			eflags;
{
	struct re_guts *g = preg->re_g;

#ifdef MULTIBYTE
	pg_wchar   *str;
	int			sts;

#endif

#ifdef REDEBUG
#define  GOODFLAGS(f)	 (f)
#else
#define  GOODFLAGS(f)	 ((f)&(REG_NOTBOL|REG_NOTEOL|REG_STARTEND))
#endif

	if (preg->re_magic != MAGIC1 || g->magic != MAGIC2)
		return REG_BADPAT;
	assert(!(g->iflags & BAD));
	if (g->iflags & BAD)		/* backstop for no-debug case */
		return REG_BADPAT;
	eflags = GOODFLAGS(eflags);

#ifdef MULTIBYTE
	str = (pg_wchar *) malloc((strlen(string) + 1) * sizeof(pg_wchar));
	if (!str)
		return (REG_ESPACE);
	(void) pg_mb2wchar((unsigned char *) string, str);
	if (g->nstates <= CHAR_BIT * sizeof(states1) && !(eflags & REG_LARGE))
		sts = smatcher(g, str, nmatch, pmatch, eflags);
	else
		sts = lmatcher(g, str, nmatch, pmatch, eflags);
	free((char *) str);
	return (sts);

#else

	if (g->nstates <= CHAR_BIT * sizeof(states1) && !(eflags & REG_LARGE))
		return smatcher(g, (pg_wchar *) string, nmatch, pmatch, eflags);
	else
		return lmatcher(g, (pg_wchar *) string, nmatch, pmatch, eflags);
#endif
}
