/*
 * Utility functions for handling cvecs
 * This file is #included by regcomp.c.
 *
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
 * src/backend/regex/regc_cvec.c
 *
 */

/*
 * Notes:
 * Only (selected) functions in _this_ file should treat chr* as non-constant.
 */

/*
 * newcvec - allocate a new cvec
 */
static struct cvec *
newcvec(int nchrs,				/* to hold this many chrs... */
		int nranges)			/* ... and this many ranges */
{
	size_t		nc = (size_t) nchrs + (size_t) nranges * 2;
	size_t		n = sizeof(struct cvec) + nc * sizeof(chr);
	struct cvec *cv = (struct cvec *) MALLOC(n);

	if (cv == NULL)
		return NULL;
	cv->chrspace = nchrs;
	cv->chrs = (chr *) (((char *) cv) + sizeof(struct cvec));
	cv->ranges = cv->chrs + nchrs;
	cv->rangespace = nranges;
	return clearcvec(cv);
}

/*
 * clearcvec - clear a possibly-new cvec
 * Returns pointer as convenience.
 */
static struct cvec *
clearcvec(struct cvec * cv)
{
	assert(cv != NULL);
	cv->nchrs = 0;
	cv->nranges = 0;
	return cv;
}

/*
 * addchr - add a chr to a cvec
 */
static void
addchr(struct cvec * cv,		/* character vector */
	   chr c)					/* character to add */
{
	assert(cv->nchrs < cv->chrspace);
	cv->chrs[cv->nchrs++] = (chr) c;
}

/*
 * addrange - add a range to a cvec
 */
static void
addrange(struct cvec * cv,		/* character vector */
		 chr from,				/* first character of range */
		 chr to)				/* last character of range */
{
	assert(cv->nranges < cv->rangespace);
	cv->ranges[cv->nranges * 2] = (chr) from;
	cv->ranges[cv->nranges * 2 + 1] = (chr) to;
	cv->nranges++;
}

/*
 * getcvec - get a transient cvec, initialized to empty
 *
 * The returned cvec is valid only until the next call of getcvec, which
 * typically will recycle the space.  Callers should *not* free the cvec
 * explicitly; it will be cleaned up when the struct vars is destroyed.
 *
 * This is typically used while interpreting bracket expressions.  In that
 * usage the cvec is only needed momentarily until we build arcs from it,
 * so transientness is a convenient behavior.
 */
static struct cvec *
getcvec(struct vars * v,		/* context */
		int nchrs,				/* to hold this many chrs... */
		int nranges)			/* ... and this many ranges */
{
	/* recycle existing transient cvec if large enough */
	if (v->cv != NULL && nchrs <= v->cv->chrspace &&
		nranges <= v->cv->rangespace)
		return clearcvec(v->cv);

	/* nope, make a new one */
	if (v->cv != NULL)
		freecvec(v->cv);
	v->cv = newcvec(nchrs, nranges);
	if (v->cv == NULL)
		ERR(REG_ESPACE);

	return v->cv;
}

/*
 * freecvec - free a cvec
 */
static void
freecvec(struct cvec * cv)
{
	FREE(cv);
}
