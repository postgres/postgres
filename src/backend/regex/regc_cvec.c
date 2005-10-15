/*
 * Utility functions for handling cvecs
 * This file is #included by regcomp.c.
 *
 * Copyright (c) 1998, 1999 Henry Spencer.	All rights reserved.
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
 * $PostgreSQL: pgsql/src/backend/regex/regc_cvec.c,v 1.5 2005/10/15 02:49:24 momjian Exp $
 *
 */

/*
 * newcvec - allocate a new cvec
 */
static struct cvec *
newcvec(int nchrs,				/* to hold this many chrs... */
		int nranges,			/* ... and this many ranges... */
		int nmcces)				/* ... and this many MCCEs */
{
	size_t		n;
	size_t		nc;
	struct cvec *cv;

	nc = (size_t) nchrs + (size_t) nmcces *(MAXMCCE + 1) + (size_t) nranges *2;

	n = sizeof(struct cvec) + (size_t) (nmcces - 1) * sizeof(chr *)
		+ nc * sizeof(chr);
	cv = (struct cvec *) MALLOC(n);
	if (cv == NULL)
		return NULL;
	cv->chrspace = nchrs;
	cv->chrs = (chr *) &cv->mcces[nmcces];		/* chrs just after MCCE ptrs */
	cv->mccespace = nmcces;
	cv->ranges = cv->chrs + nchrs + nmcces * (MAXMCCE + 1);
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
	int			i;

	assert(cv != NULL);
	cv->nchrs = 0;
	assert(cv->chrs == (chr *) &cv->mcces[cv->mccespace]);
	cv->nmcces = 0;
	cv->nmccechrs = 0;
	cv->nranges = 0;
	for (i = 0; i < cv->mccespace; i++)
		cv->mcces[i] = NULL;

	return cv;
}

/*
 * addchr - add a chr to a cvec
 */
static void
addchr(struct cvec * cv,		/* character vector */
	   chr c)					/* character to add */
{
	assert(cv->nchrs < cv->chrspace - cv->nmccechrs);
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
 * addmcce - add an MCCE to a cvec
 */
static void
addmcce(struct cvec * cv,		/* character vector */
		chr *startp,			/* beginning of text */
		chr *endp)				/* just past end of text */
{
	int			len;
	int			i;
	chr		   *s;
	chr		   *d;

	if (startp == NULL && endp == NULL)
		return;
	len = endp - startp;
	assert(len > 0);
	assert(cv->nchrs + len < cv->chrspace - cv->nmccechrs);
	assert(cv->nmcces < cv->mccespace);
	d = &cv->chrs[cv->chrspace - cv->nmccechrs - len - 1];
	cv->mcces[cv->nmcces++] = d;
	for (s = startp, i = len; i > 0; s++, i--)
		*d++ = *s;
	*d++ = 0;					/* endmarker */
	assert(d == &cv->chrs[cv->chrspace - cv->nmccechrs]);
	cv->nmccechrs += len + 1;
}

/*
 * haschr - does a cvec contain this chr?
 */
static int						/* predicate */
haschr(struct cvec * cv,		/* character vector */
	   chr c)					/* character to test for */
{
	int			i;
	chr		   *p;

	for (p = cv->chrs, i = cv->nchrs; i > 0; p++, i--)
	{
		if (*p == c)
			return 1;
	}
	for (p = cv->ranges, i = cv->nranges; i > 0; p += 2, i--)
	{
		if ((*p <= c) && (c <= *(p + 1)))
			return 1;
	}
	return 0;
}

/*
 * getcvec - get a cvec, remembering it as v->cv
 */
static struct cvec *
getcvec(struct vars * v,		/* context */
		int nchrs,				/* to hold this many chrs... */
		int nranges,			/* ... and this many ranges... */
		int nmcces)				/* ... and this many MCCEs */
{
	if (v->cv != NULL && nchrs <= v->cv->chrspace &&
		nranges <= v->cv->rangespace && nmcces <= v->cv->mccespace)
		return clearcvec(v->cv);

	if (v->cv != NULL)
		freecvec(v->cv);
	v->cv = newcvec(nchrs, nranges, nmcces);
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
