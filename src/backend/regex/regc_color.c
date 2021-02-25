/*
 * colorings of characters
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
 * src/backend/regex/regc_color.c
 *
 *
 * Note that there are some incestuous relationships between this code and
 * NFA arc maintenance, which perhaps ought to be cleaned up sometime.
 */



#define CISERR()	VISERR(cm->v)
#define CERR(e)		VERR(cm->v, (e))



/*
 * initcm - set up new colormap
 */
static void
initcm(struct vars *v,
	   struct colormap *cm)
{
	struct colordesc *cd;

	cm->magic = CMMAGIC;
	cm->v = v;

	cm->ncds = NINLINECDS;
	cm->cd = cm->cdspace;
	cm->max = 0;
	cm->free = 0;

	cd = cm->cd;				/* cm->cd[WHITE] */
	cd->nschrs = MAX_SIMPLE_CHR - CHR_MIN + 1;
	cd->nuchrs = 1;
	cd->sub = NOSUB;
	cd->arcs = NULL;
	cd->firstchr = CHR_MIN;
	cd->flags = 0;

	cm->locolormap = (color *)
		MALLOC((MAX_SIMPLE_CHR - CHR_MIN + 1) * sizeof(color));
	if (cm->locolormap == NULL)
	{
		CERR(REG_ESPACE);
		cm->cmranges = NULL;	/* prevent failure during freecm */
		cm->hicolormap = NULL;
		return;
	}
	/* this memset relies on WHITE being zero: */
	memset(cm->locolormap, WHITE,
		   (MAX_SIMPLE_CHR - CHR_MIN + 1) * sizeof(color));

	memset(cm->classbits, 0, sizeof(cm->classbits));
	cm->numcmranges = 0;
	cm->cmranges = NULL;
	cm->maxarrayrows = 4;		/* arbitrary initial allocation */
	cm->hiarrayrows = 1;		/* but we have only one row/col initially */
	cm->hiarraycols = 1;
	cm->hicolormap = (color *) MALLOC(cm->maxarrayrows * sizeof(color));
	if (cm->hicolormap == NULL)
	{
		CERR(REG_ESPACE);
		return;
	}
	/* initialize the "all other characters" row to WHITE */
	cm->hicolormap[0] = WHITE;
}

/*
 * freecm - free dynamically-allocated things in a colormap
 */
static void
freecm(struct colormap *cm)
{
	cm->magic = 0;
	if (cm->cd != cm->cdspace)
		FREE(cm->cd);
	if (cm->locolormap != NULL)
		FREE(cm->locolormap);
	if (cm->cmranges != NULL)
		FREE(cm->cmranges);
	if (cm->hicolormap != NULL)
		FREE(cm->hicolormap);
}

/*
 * pg_reg_getcolor - slow case of GETCOLOR()
 */
color
pg_reg_getcolor(struct colormap *cm, chr c)
{
	int			rownum,
				colnum,
				low,
				high;

	/* Should not be used for chrs in the locolormap */
	assert(c > MAX_SIMPLE_CHR);

	/*
	 * Find which row it's in.  The colormapranges are in order, so we can use
	 * binary search.
	 */
	rownum = 0;					/* if no match, use array row zero */
	low = 0;
	high = cm->numcmranges;
	while (low < high)
	{
		int			middle = low + (high - low) / 2;
		const colormaprange *cmr = &cm->cmranges[middle];

		if (c < cmr->cmin)
			high = middle;
		else if (c > cmr->cmax)
			low = middle + 1;
		else
		{
			rownum = cmr->rownum;	/* found a match */
			break;
		}
	}

	/*
	 * Find which column it's in --- this is all locale-dependent.
	 */
	if (cm->hiarraycols > 1)
	{
		colnum = cclass_column_index(cm, c);
		return cm->hicolormap[rownum * cm->hiarraycols + colnum];
	}
	else
	{
		/* fast path if no relevant cclasses */
		return cm->hicolormap[rownum];
	}
}

/*
 * maxcolor - report largest color number in use
 */
static color
maxcolor(struct colormap *cm)
{
	if (CISERR())
		return COLORLESS;

	return (color) cm->max;
}

/*
 * newcolor - find a new color (must be assigned at once)
 * Beware:	may relocate the colordescs.
 */
static color					/* COLORLESS for error */
newcolor(struct colormap *cm)
{
	struct colordesc *cd;
	size_t		n;

	if (CISERR())
		return COLORLESS;

	if (cm->free != 0)
	{
		assert(cm->free > 0);
		assert((size_t) cm->free < cm->ncds);
		cd = &cm->cd[cm->free];
		assert(UNUSEDCOLOR(cd));
		assert(cd->arcs == NULL);
		cm->free = cd->sub;
	}
	else if (cm->max < cm->ncds - 1)
	{
		cm->max++;
		cd = &cm->cd[cm->max];
	}
	else
	{
		/* oops, must allocate more */
		struct colordesc *newCd;

		if (cm->max == MAX_COLOR)
		{
			CERR(REG_ECOLORS);
			return COLORLESS;	/* too many colors */
		}

		n = cm->ncds * 2;
		if (n > MAX_COLOR + 1)
			n = MAX_COLOR + 1;
		if (cm->cd == cm->cdspace)
		{
			newCd = (struct colordesc *) MALLOC(n * sizeof(struct colordesc));
			if (newCd != NULL)
				memcpy(VS(newCd), VS(cm->cdspace), cm->ncds *
					   sizeof(struct colordesc));
		}
		else
			newCd = (struct colordesc *)
				REALLOC(cm->cd, n * sizeof(struct colordesc));
		if (newCd == NULL)
		{
			CERR(REG_ESPACE);
			return COLORLESS;
		}
		cm->cd = newCd;
		cm->ncds = n;
		assert(cm->max < cm->ncds - 1);
		cm->max++;
		cd = &cm->cd[cm->max];
	}

	cd->nschrs = 0;
	cd->nuchrs = 0;
	cd->sub = NOSUB;
	cd->arcs = NULL;
	cd->firstchr = CHR_MIN;		/* in case never set otherwise */
	cd->flags = 0;

	return (color) (cd - cm->cd);
}

/*
 * freecolor - free a color (must have no arcs or subcolor)
 */
static void
freecolor(struct colormap *cm,
		  color co)
{
	struct colordesc *cd = &cm->cd[co];
	color		pco,
				nco;			/* for freelist scan */

	assert(co >= 0);
	if (co == WHITE)
		return;

	assert(cd->arcs == NULL);
	assert(cd->sub == NOSUB);
	assert(cd->nschrs == 0);
	assert(cd->nuchrs == 0);
	cd->flags = FREECOL;

	if ((size_t) co == cm->max)
	{
		while (cm->max > WHITE && UNUSEDCOLOR(&cm->cd[cm->max]))
			cm->max--;
		assert(cm->free >= 0);
		while ((size_t) cm->free > cm->max)
			cm->free = cm->cd[cm->free].sub;
		if (cm->free > 0)
		{
			assert(cm->free < cm->max);
			pco = cm->free;
			nco = cm->cd[pco].sub;
			while (nco > 0)
				if ((size_t) nco > cm->max)
				{
					/* take this one out of freelist */
					nco = cm->cd[nco].sub;
					cm->cd[pco].sub = nco;
				}
				else
				{
					assert(nco < cm->max);
					pco = nco;
					nco = cm->cd[pco].sub;
				}
		}
	}
	else
	{
		cd->sub = cm->free;
		cm->free = (color) (cd - cm->cd);
	}
}

/*
 * pseudocolor - allocate a false color, to be managed by other means
 */
static color
pseudocolor(struct colormap *cm)
{
	color		co;
	struct colordesc *cd;

	co = newcolor(cm);
	if (CISERR())
		return COLORLESS;
	cd = &cm->cd[co];
	cd->nschrs = 0;
	cd->nuchrs = 1;				/* pretend it is in the upper map */
	cd->sub = NOSUB;
	cd->arcs = NULL;
	cd->firstchr = CHR_MIN;
	cd->flags = PSEUDO;
	return co;
}

/*
 * subcolor - allocate a new subcolor (if necessary) to this chr
 *
 * This works only for chrs that map into the low color map.
 */
static color
subcolor(struct colormap *cm, chr c)
{
	color		co;				/* current color of c */
	color		sco;			/* new subcolor */

	assert(c <= MAX_SIMPLE_CHR);

	co = cm->locolormap[c - CHR_MIN];
	sco = newsub(cm, co);
	if (CISERR())
		return COLORLESS;
	assert(sco != COLORLESS);

	if (co == sco)				/* already in an open subcolor */
		return co;				/* rest is redundant */
	cm->cd[co].nschrs--;
	if (cm->cd[sco].nschrs == 0)
		cm->cd[sco].firstchr = c;
	cm->cd[sco].nschrs++;
	cm->locolormap[c - CHR_MIN] = sco;
	return sco;
}

/*
 * subcolorhi - allocate a new subcolor (if necessary) to this colormap entry
 *
 * This is the same processing as subcolor(), but for entries in the high
 * colormap, which do not necessarily correspond to exactly one chr code.
 */
static color
subcolorhi(struct colormap *cm, color *pco)
{
	color		co;				/* current color of entry */
	color		sco;			/* new subcolor */

	co = *pco;
	sco = newsub(cm, co);
	if (CISERR())
		return COLORLESS;
	assert(sco != COLORLESS);

	if (co == sco)				/* already in an open subcolor */
		return co;				/* rest is redundant */
	cm->cd[co].nuchrs--;
	cm->cd[sco].nuchrs++;
	*pco = sco;
	return sco;
}

/*
 * newsub - allocate a new subcolor (if necessary) for a color
 */
static color
newsub(struct colormap *cm,
	   color co)
{
	color		sco;			/* new subcolor */

	sco = cm->cd[co].sub;
	if (sco == NOSUB)
	{							/* color has no open subcolor */
		/* optimization: singly-referenced color need not be subcolored */
		if ((cm->cd[co].nschrs + cm->cd[co].nuchrs) == 1)
			return co;
		sco = newcolor(cm);		/* must create subcolor */
		if (sco == COLORLESS)
		{
			assert(CISERR());
			return COLORLESS;
		}
		cm->cd[co].sub = sco;
		cm->cd[sco].sub = sco;	/* open subcolor points to self */
	}
	assert(sco != NOSUB);

	return sco;
}

/*
 * newhicolorrow - get a new row in the hicolormap, cloning it from oldrow
 *
 * Returns array index of new row.  Note the array might move.
 */
static int
newhicolorrow(struct colormap *cm,
			  int oldrow)
{
	int			newrow = cm->hiarrayrows;
	color	   *newrowptr;
	int			i;

	/* Assign a fresh array row index, enlarging storage if needed */
	if (newrow >= cm->maxarrayrows)
	{
		color	   *newarray;

		if (cm->maxarrayrows >= INT_MAX / (cm->hiarraycols * 2))
		{
			CERR(REG_ESPACE);
			return 0;
		}
		newarray = (color *) REALLOC(cm->hicolormap,
									 cm->maxarrayrows * 2 *
									 cm->hiarraycols * sizeof(color));
		if (newarray == NULL)
		{
			CERR(REG_ESPACE);
			return 0;
		}
		cm->hicolormap = newarray;
		cm->maxarrayrows *= 2;
	}
	cm->hiarrayrows++;

	/* Copy old row data */
	newrowptr = &cm->hicolormap[newrow * cm->hiarraycols];
	memcpy(newrowptr,
		   &cm->hicolormap[oldrow * cm->hiarraycols],
		   cm->hiarraycols * sizeof(color));

	/* Increase color reference counts to reflect new colormap entries */
	for (i = 0; i < cm->hiarraycols; i++)
		cm->cd[newrowptr[i]].nuchrs++;

	return newrow;
}

/*
 * newhicolorcols - create a new set of columns in the high colormap
 *
 * Essentially, extends the 2-D array to the right with a copy of itself.
 */
static void
newhicolorcols(struct colormap *cm)
{
	color	   *newarray;
	int			r,
				c;

	if (cm->hiarraycols >= INT_MAX / (cm->maxarrayrows * 2))
	{
		CERR(REG_ESPACE);
		return;
	}
	newarray = (color *) REALLOC(cm->hicolormap,
								 cm->maxarrayrows *
								 cm->hiarraycols * 2 * sizeof(color));
	if (newarray == NULL)
	{
		CERR(REG_ESPACE);
		return;
	}
	cm->hicolormap = newarray;

	/* Duplicate existing columns to the right, and increase ref counts */
	/* Must work backwards in the array because we realloc'd in place */
	for (r = cm->hiarrayrows - 1; r >= 0; r--)
	{
		color	   *oldrowptr = &newarray[r * cm->hiarraycols];
		color	   *newrowptr = &newarray[r * cm->hiarraycols * 2];
		color	   *newrowptr2 = newrowptr + cm->hiarraycols;

		for (c = 0; c < cm->hiarraycols; c++)
		{
			color		co = oldrowptr[c];

			newrowptr[c] = newrowptr2[c] = co;
			cm->cd[co].nuchrs++;
		}
	}

	cm->hiarraycols *= 2;
}

/*
 * subcolorcvec - allocate new subcolors to cvec members, fill in arcs
 *
 * For each chr "c" represented by the cvec, do the equivalent of
 * newarc(v->nfa, PLAIN, subcolor(v->cm, c), lp, rp);
 *
 * Note that in typical cases, many of the subcolors are the same.
 * While newarc() would discard duplicate arc requests, we can save
 * some cycles by not calling it repetitively to begin with.  This is
 * mechanized with the "lastsubcolor" state variable.
 */
static void
subcolorcvec(struct vars *v,
			 struct cvec *cv,
			 struct state *lp,
			 struct state *rp)
{
	struct colormap *cm = v->cm;
	color		lastsubcolor = COLORLESS;
	chr			ch,
				from,
				to;
	const chr  *p;
	int			i;

	/* ordinary characters */
	for (p = cv->chrs, i = cv->nchrs; i > 0; p++, i--)
	{
		ch = *p;
		subcoloronechr(v, ch, lp, rp, &lastsubcolor);
		NOERR();
	}

	/* and the ranges */
	for (p = cv->ranges, i = cv->nranges; i > 0; p += 2, i--)
	{
		from = *p;
		to = *(p + 1);
		if (from <= MAX_SIMPLE_CHR)
		{
			/* deal with simple chars one at a time */
			chr			lim = (to <= MAX_SIMPLE_CHR) ? to : MAX_SIMPLE_CHR;

			while (from <= lim)
			{
				color		sco = subcolor(cm, from);

				NOERR();
				if (sco != lastsubcolor)
				{
					newarc(v->nfa, PLAIN, sco, lp, rp);
					NOERR();
					lastsubcolor = sco;
				}
				from++;
			}
		}
		/* deal with any part of the range that's above MAX_SIMPLE_CHR */
		if (from < to)
			subcoloronerange(v, from, to, lp, rp, &lastsubcolor);
		else if (from == to)
			subcoloronechr(v, from, lp, rp, &lastsubcolor);
		NOERR();
	}

	/* and deal with cclass if any */
	if (cv->cclasscode >= 0)
	{
		int			classbit;
		color	   *pco;
		int			r,
					c;

		/* Enlarge array if we don't have a column bit assignment for cclass */
		if (cm->classbits[cv->cclasscode] == 0)
		{
			cm->classbits[cv->cclasscode] = cm->hiarraycols;
			newhicolorcols(cm);
			NOERR();
		}
		/* Apply subcolorhi() and make arc for each entry in relevant cols */
		classbit = cm->classbits[cv->cclasscode];
		pco = cm->hicolormap;
		for (r = 0; r < cm->hiarrayrows; r++)
		{
			for (c = 0; c < cm->hiarraycols; c++)
			{
				if (c & classbit)
				{
					color		sco = subcolorhi(cm, pco);

					NOERR();
					/* add the arc if needed */
					if (sco != lastsubcolor)
					{
						newarc(v->nfa, PLAIN, sco, lp, rp);
						NOERR();
						lastsubcolor = sco;
					}
				}
				pco++;
			}
		}
	}
}

/*
 * subcoloronechr - do subcolorcvec's work for a singleton chr
 *
 * We could just let subcoloronerange do this, but it's a bit more efficient
 * if we exploit the single-chr case.  Also, callers find it useful for this
 * to be able to handle both low and high chr codes.
 */
static void
subcoloronechr(struct vars *v,
			   chr ch,
			   struct state *lp,
			   struct state *rp,
			   color *lastsubcolor)
{
	struct colormap *cm = v->cm;
	colormaprange *newranges;
	int			numnewranges;
	colormaprange *oldrange;
	int			oldrangen;
	int			newrow;

	/* Easy case for low chr codes */
	if (ch <= MAX_SIMPLE_CHR)
	{
		color		sco = subcolor(cm, ch);

		NOERR();
		if (sco != *lastsubcolor)
		{
			newarc(v->nfa, PLAIN, sco, lp, rp);
			*lastsubcolor = sco;
		}
		return;
	}

	/*
	 * Potentially, we could need two more colormapranges than we have now, if
	 * the given chr is in the middle of some existing range.
	 */
	newranges = (colormaprange *)
		MALLOC((cm->numcmranges + 2) * sizeof(colormaprange));
	if (newranges == NULL)
	{
		CERR(REG_ESPACE);
		return;
	}
	numnewranges = 0;

	/* Ranges before target are unchanged */
	for (oldrange = cm->cmranges, oldrangen = 0;
		 oldrangen < cm->numcmranges;
		 oldrange++, oldrangen++)
	{
		if (oldrange->cmax >= ch)
			break;
		newranges[numnewranges++] = *oldrange;
	}

	/* Match target chr against current range */
	if (oldrangen >= cm->numcmranges || oldrange->cmin > ch)
	{
		/* chr does not belong to any existing range, make a new one */
		newranges[numnewranges].cmin = ch;
		newranges[numnewranges].cmax = ch;
		/* row state should be cloned from the "all others" row */
		newranges[numnewranges].rownum = newrow = newhicolorrow(cm, 0);
		numnewranges++;
	}
	else if (oldrange->cmin == oldrange->cmax)
	{
		/* we have an existing singleton range matching the chr */
		newranges[numnewranges++] = *oldrange;
		newrow = oldrange->rownum;
		/* we've now fully processed this old range */
		oldrange++, oldrangen++;
	}
	else
	{
		/* chr is a subset of this existing range, must split it */
		if (ch > oldrange->cmin)
		{
			/* emit portion of old range before chr */
			newranges[numnewranges].cmin = oldrange->cmin;
			newranges[numnewranges].cmax = ch - 1;
			newranges[numnewranges].rownum = oldrange->rownum;
			numnewranges++;
		}
		/* emit chr as singleton range, initially cloning from range */
		newranges[numnewranges].cmin = ch;
		newranges[numnewranges].cmax = ch;
		newranges[numnewranges].rownum = newrow =
			newhicolorrow(cm, oldrange->rownum);
		numnewranges++;
		if (ch < oldrange->cmax)
		{
			/* emit portion of old range after chr */
			newranges[numnewranges].cmin = ch + 1;
			newranges[numnewranges].cmax = oldrange->cmax;
			/* must clone the row if we are making two new ranges from old */
			newranges[numnewranges].rownum =
				(ch > oldrange->cmin) ? newhicolorrow(cm, oldrange->rownum) :
				oldrange->rownum;
			numnewranges++;
		}
		/* we've now fully processed this old range */
		oldrange++, oldrangen++;
	}

	/* Update colors in newrow and create arcs as needed */
	subcoloronerow(v, newrow, lp, rp, lastsubcolor);

	/* Ranges after target are unchanged */
	for (; oldrangen < cm->numcmranges; oldrange++, oldrangen++)
	{
		newranges[numnewranges++] = *oldrange;
	}

	/* Assert our original space estimate was adequate */
	assert(numnewranges <= (cm->numcmranges + 2));

	/* And finally, store back the updated list of ranges */
	if (cm->cmranges != NULL)
		FREE(cm->cmranges);
	cm->cmranges = newranges;
	cm->numcmranges = numnewranges;
}

/*
 * subcoloronerange - do subcolorcvec's work for a high range
 */
static void
subcoloronerange(struct vars *v,
				 chr from,
				 chr to,
				 struct state *lp,
				 struct state *rp,
				 color *lastsubcolor)
{
	struct colormap *cm = v->cm;
	colormaprange *newranges;
	int			numnewranges;
	colormaprange *oldrange;
	int			oldrangen;
	int			newrow;

	/* Caller should take care of non-high-range cases */
	assert(from > MAX_SIMPLE_CHR);
	assert(from < to);

	/*
	 * Potentially, if we have N non-adjacent ranges, we could need as many as
	 * 2N+1 result ranges (consider case where new range spans 'em all).
	 */
	newranges = (colormaprange *)
		MALLOC((cm->numcmranges * 2 + 1) * sizeof(colormaprange));
	if (newranges == NULL)
	{
		CERR(REG_ESPACE);
		return;
	}
	numnewranges = 0;

	/* Ranges before target are unchanged */
	for (oldrange = cm->cmranges, oldrangen = 0;
		 oldrangen < cm->numcmranges;
		 oldrange++, oldrangen++)
	{
		if (oldrange->cmax >= from)
			break;
		newranges[numnewranges++] = *oldrange;
	}

	/*
	 * Deal with ranges that (partially) overlap the target.  As we process
	 * each such range, increase "from" to remove the dealt-with characters
	 * from the target range.
	 */
	while (oldrangen < cm->numcmranges && oldrange->cmin <= to)
	{
		if (from < oldrange->cmin)
		{
			/* Handle portion of new range that corresponds to no old range */
			newranges[numnewranges].cmin = from;
			newranges[numnewranges].cmax = oldrange->cmin - 1;
			/* row state should be cloned from the "all others" row */
			newranges[numnewranges].rownum = newrow = newhicolorrow(cm, 0);
			numnewranges++;
			/* Update colors in newrow and create arcs as needed */
			subcoloronerow(v, newrow, lp, rp, lastsubcolor);
			/* We've now fully processed the part of new range before old */
			from = oldrange->cmin;
		}

		if (from <= oldrange->cmin && to >= oldrange->cmax)
		{
			/* old range is fully contained in new, process it in-place */
			newranges[numnewranges++] = *oldrange;
			newrow = oldrange->rownum;
			from = oldrange->cmax + 1;
		}
		else
		{
			/* some part of old range does not overlap new range */
			if (from > oldrange->cmin)
			{
				/* emit portion of old range before new range */
				newranges[numnewranges].cmin = oldrange->cmin;
				newranges[numnewranges].cmax = from - 1;
				newranges[numnewranges].rownum = oldrange->rownum;
				numnewranges++;
			}
			/* emit common subrange, initially cloning from old range */
			newranges[numnewranges].cmin = from;
			newranges[numnewranges].cmax =
				(to < oldrange->cmax) ? to : oldrange->cmax;
			newranges[numnewranges].rownum = newrow =
				newhicolorrow(cm, oldrange->rownum);
			numnewranges++;
			if (to < oldrange->cmax)
			{
				/* emit portion of old range after new range */
				newranges[numnewranges].cmin = to + 1;
				newranges[numnewranges].cmax = oldrange->cmax;
				/* must clone the row if we are making two new ranges from old */
				newranges[numnewranges].rownum =
					(from > oldrange->cmin) ? newhicolorrow(cm, oldrange->rownum) :
					oldrange->rownum;
				numnewranges++;
			}
			from = oldrange->cmax + 1;
		}
		/* Update colors in newrow and create arcs as needed */
		subcoloronerow(v, newrow, lp, rp, lastsubcolor);
		/* we've now fully processed this old range */
		oldrange++, oldrangen++;
	}

	if (from <= to)
	{
		/* Handle portion of new range that corresponds to no old range */
		newranges[numnewranges].cmin = from;
		newranges[numnewranges].cmax = to;
		/* row state should be cloned from the "all others" row */
		newranges[numnewranges].rownum = newrow = newhicolorrow(cm, 0);
		numnewranges++;
		/* Update colors in newrow and create arcs as needed */
		subcoloronerow(v, newrow, lp, rp, lastsubcolor);
	}

	/* Ranges after target are unchanged */
	for (; oldrangen < cm->numcmranges; oldrange++, oldrangen++)
	{
		newranges[numnewranges++] = *oldrange;
	}

	/* Assert our original space estimate was adequate */
	assert(numnewranges <= (cm->numcmranges * 2 + 1));

	/* And finally, store back the updated list of ranges */
	if (cm->cmranges != NULL)
		FREE(cm->cmranges);
	cm->cmranges = newranges;
	cm->numcmranges = numnewranges;
}

/*
 * subcoloronerow - do subcolorcvec's work for one new row in the high colormap
 */
static void
subcoloronerow(struct vars *v,
			   int rownum,
			   struct state *lp,
			   struct state *rp,
			   color *lastsubcolor)
{
	struct colormap *cm = v->cm;
	color	   *pco;
	int			i;

	/* Apply subcolorhi() and make arc for each entry in row */
	pco = &cm->hicolormap[rownum * cm->hiarraycols];
	for (i = 0; i < cm->hiarraycols; pco++, i++)
	{
		color		sco = subcolorhi(cm, pco);

		NOERR();
		/* make the arc if needed */
		if (sco != *lastsubcolor)
		{
			newarc(v->nfa, PLAIN, sco, lp, rp);
			NOERR();
			*lastsubcolor = sco;
		}
	}
}

/*
 * okcolors - promote subcolors to full colors
 */
static void
okcolors(struct nfa *nfa,
		 struct colormap *cm)
{
	struct colordesc *cd;
	struct colordesc *end = CDEND(cm);
	struct colordesc *scd;
	struct arc *a;
	color		co;
	color		sco;

	for (cd = cm->cd, co = 0; cd < end; cd++, co++)
	{
		sco = cd->sub;
		if (UNUSEDCOLOR(cd) || sco == NOSUB)
		{
			/* has no subcolor, no further action */
		}
		else if (sco == co)
		{
			/* is subcolor, let parent deal with it */
		}
		else if (cd->nschrs == 0 && cd->nuchrs == 0)
		{
			/*
			 * Parent is now empty, so just change all its arcs to the
			 * subcolor, then free the parent.
			 *
			 * It is not obvious that simply relabeling the arcs like this is
			 * OK; it appears to risk creating duplicate arcs.  We are
			 * basically relying on the assumption that processing of a
			 * bracket expression can't create arcs of both a color and its
			 * subcolor between the bracket's endpoints.
			 */
			cd->sub = NOSUB;
			scd = &cm->cd[sco];
			assert(scd->nschrs > 0 || scd->nuchrs > 0);
			assert(scd->sub == sco);
			scd->sub = NOSUB;
			while ((a = cd->arcs) != NULL)
			{
				assert(a->co == co);
				uncolorchain(cm, a);
				a->co = sco;
				colorchain(cm, a);
			}
			freecolor(cm, co);
		}
		else
		{
			/* parent's arcs must gain parallel subcolor arcs */
			cd->sub = NOSUB;
			scd = &cm->cd[sco];
			assert(scd->nschrs > 0 || scd->nuchrs > 0);
			assert(scd->sub == sco);
			scd->sub = NOSUB;
			for (a = cd->arcs; a != NULL; a = a->colorchain)
			{
				assert(a->co == co);
				newarc(nfa, a->type, sco, a->from, a->to);
			}
		}
	}
}

/*
 * colorchain - add this arc to the color chain of its color
 */
static void
colorchain(struct colormap *cm,
		   struct arc *a)
{
	struct colordesc *cd = &cm->cd[a->co];

	assert(a->co >= 0);
	if (cd->arcs != NULL)
		cd->arcs->colorchainRev = a;
	a->colorchain = cd->arcs;
	a->colorchainRev = NULL;
	cd->arcs = a;
}

/*
 * uncolorchain - delete this arc from the color chain of its color
 */
static void
uncolorchain(struct colormap *cm,
			 struct arc *a)
{
	struct colordesc *cd = &cm->cd[a->co];
	struct arc *aa = a->colorchainRev;

	assert(a->co >= 0);
	if (aa == NULL)
	{
		assert(cd->arcs == a);
		cd->arcs = a->colorchain;
	}
	else
	{
		assert(aa->colorchain == a);
		aa->colorchain = a->colorchain;
	}
	if (a->colorchain != NULL)
		a->colorchain->colorchainRev = aa;
	a->colorchain = NULL;		/* paranoia */
	a->colorchainRev = NULL;
}

/*
 * rainbow - add arcs of all full colors (but one) between specified states
 *
 * If there isn't an exception color, we now generate just a single arc
 * labeled RAINBOW, saving lots of arc-munging later on.
 */
static void
rainbow(struct nfa *nfa,
		struct colormap *cm,
		int type,
		color but,				/* COLORLESS if no exceptions */
		struct state *from,
		struct state *to)
{
	struct colordesc *cd;
	struct colordesc *end = CDEND(cm);
	color		co;

	if (but == COLORLESS)
	{
		newarc(nfa, type, RAINBOW, from, to);
		return;
	}

	/* Gotta do it the hard way.  Skip subcolors, pseudocolors, and "but" */
	for (cd = cm->cd, co = 0; cd < end && !CISERR(); cd++, co++)
		if (!UNUSEDCOLOR(cd) && cd->sub != co && co != but &&
			!(cd->flags & PSEUDO))
			newarc(nfa, type, co, from, to);
}

/*
 * colorcomplement - add arcs of complementary colors
 *
 * We add arcs of all colors that are not pseudocolors and do not match
 * any of the "of" state's PLAIN outarcs.
 *
 * The calling sequence ought to be reconciled with cloneouts().
 */
static void
colorcomplement(struct nfa *nfa,
				struct colormap *cm,
				int type,
				struct state *of,
				struct state *from,
				struct state *to)
{
	struct colordesc *cd;
	struct colordesc *end = CDEND(cm);
	color		co;
	struct arc *a;

	assert(of != from);

	/* A RAINBOW arc matches all colors, making the complement empty */
	if (findarc(of, PLAIN, RAINBOW) != NULL)
		return;

	/* Otherwise, transiently mark the colors that appear in of's out-arcs */
	for (a = of->outs; a != NULL; a = a->outchain)
	{
		if (a->type == PLAIN)
		{
			assert(a->co >= 0);
			cd = &cm->cd[a->co];
			assert(!UNUSEDCOLOR(cd));
			cd->flags |= COLMARK;
		}
	}

	/* Scan colors, clear transient marks, add arcs for unmarked colors */
	for (cd = cm->cd, co = 0; cd < end && !CISERR(); cd++, co++)
	{
		if (cd->flags & COLMARK)
			cd->flags &= ~COLMARK;
		else if (!UNUSEDCOLOR(cd) && !(cd->flags & PSEUDO))
			newarc(nfa, type, co, from, to);
	}
}


#ifdef REG_DEBUG

/*
 * dumpcolors - debugging output
 */
static void
dumpcolors(struct colormap *cm,
		   FILE *f)
{
	struct colordesc *cd;
	struct colordesc *end;
	color		co;
	chr			c;

	fprintf(f, "max %ld\n", (long) cm->max);
	end = CDEND(cm);
	for (cd = cm->cd + 1, co = 1; cd < end; cd++, co++) /* skip 0 */
	{
		if (!UNUSEDCOLOR(cd))
		{
			assert(cd->nschrs > 0 || cd->nuchrs > 0);
			if (cd->flags & PSEUDO)
				fprintf(f, "#%2ld(ps): ", (long) co);
			else
				fprintf(f, "#%2ld(%2d): ", (long) co, cd->nschrs + cd->nuchrs);

			/*
			 * Unfortunately, it's hard to do this next bit more efficiently.
			 */
			for (c = CHR_MIN; c <= MAX_SIMPLE_CHR; c++)
				if (GETCOLOR(cm, c) == co)
					dumpchr(c, f);
			fprintf(f, "\n");
		}
	}
	/* dump the high colormap if it contains anything interesting */
	if (cm->hiarrayrows > 1 || cm->hiarraycols > 1)
	{
		int			r,
					c;
		const color *rowptr;

		fprintf(f, "other:\t");
		for (c = 0; c < cm->hiarraycols; c++)
		{
			fprintf(f, "\t%ld", (long) cm->hicolormap[c]);
		}
		fprintf(f, "\n");
		for (r = 0; r < cm->numcmranges; r++)
		{
			dumpchr(cm->cmranges[r].cmin, f);
			fprintf(f, "..");
			dumpchr(cm->cmranges[r].cmax, f);
			fprintf(f, ":");
			rowptr = &cm->hicolormap[cm->cmranges[r].rownum * cm->hiarraycols];
			for (c = 0; c < cm->hiarraycols; c++)
			{
				fprintf(f, "\t%ld", (long) rowptr[c]);
			}
			fprintf(f, "\n");
		}
	}
}

/*
 * dumpchr - print a chr
 *
 * Kind of char-centric but works well enough for debug use.
 */
static void
dumpchr(chr c,
		FILE *f)
{
	if (c == '\\')
		fprintf(f, "\\\\");
	else if (c > ' ' && c <= '~')
		putc((char) c, f);
	else
		fprintf(f, "\\u%04lx", (long) c);
}

#endif							/* REG_DEBUG */
