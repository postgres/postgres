/*-------------------------------------------------------------------------
 *
 * regexport.c
 *	  Functions for exporting info about a regex's NFA
 *
 * In this implementation, the NFA defines a necessary but not sufficient
 * condition for a string to match the regex: that is, there can be strings
 * that match the NFA but don't match the full regex, but not vice versa.
 * Thus, for example, it is okay for the functions below to ignore lookahead
 * constraints, which merely constrain the string some more.
 *
 * Notice that these functions return info into caller-provided arrays
 * rather than doing their own malloc's.  This simplifies the APIs by
 * eliminating a class of error conditions, and in the case of colors
 * allows the caller to decide how big is too big to bother with.
 *
 *
 * Portions Copyright (c) 2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1998, 1999 Henry Spencer
 *
 * IDENTIFICATION
 *	  src/backend/regex/regexport.c
 *
 *-------------------------------------------------------------------------
 */

#include "regex/regguts.h"

#include "regex/regexport.h"

static void scancolormap(struct colormap * cm, int co,
			 union tree * t, int level, chr partial,
			 pg_wchar **chars, int *chars_len);


/*
 * Get total number of NFA states.
 */
int
pg_reg_getnumstates(const regex_t *regex)
{
	struct cnfa *cnfa;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	return cnfa->nstates;
}

/*
 * Get initial state of NFA.
 */
int
pg_reg_getinitialstate(const regex_t *regex)
{
	struct cnfa *cnfa;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	return cnfa->pre;
}

/*
 * Get final state of NFA.
 */
int
pg_reg_getfinalstate(const regex_t *regex)
{
	struct cnfa *cnfa;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	return cnfa->post;
}

/*
 * Get number of outgoing NFA arcs of state number "st".
 *
 * Note: LACON arcs are ignored, both here and in pg_reg_getoutarcs().
 */
int
pg_reg_getnumoutarcs(const regex_t *regex, int st)
{
	struct cnfa *cnfa;
	struct carc *ca;
	int			count;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	if (st < 0 || st >= cnfa->nstates)
		return 0;
	count = 0;
	for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
	{
		if (ca->co < cnfa->ncolors)
			count++;
	}
	return count;
}

/*
 * Write array of outgoing NFA arcs of state number "st" into arcs[],
 * whose length arcs_len must be at least as long as indicated by
 * pg_reg_getnumoutarcs(), else not all arcs will be returned.
 */
void
pg_reg_getoutarcs(const regex_t *regex, int st,
				  regex_arc_t *arcs, int arcs_len)
{
	struct cnfa *cnfa;
	struct carc *ca;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	if (st < 0 || st >= cnfa->nstates || arcs_len <= 0)
		return;
	for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
	{
		if (ca->co < cnfa->ncolors)
		{
			arcs->co = ca->co;
			arcs->to = ca->to;
			arcs++;
			if (--arcs_len == 0)
				break;
		}
	}
}

/*
 * Get total number of colors.
 */
int
pg_reg_getnumcolors(const regex_t *regex)
{
	struct colormap *cm;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cm = &((struct guts *) regex->re_guts)->cmap;

	return cm->max + 1;
}

/*
 * Check if color is beginning of line/string.
 *
 * (We might at some point need to offer more refined handling of pseudocolors,
 * but this will do for now.)
 */
int
pg_reg_colorisbegin(const regex_t *regex, int co)
{
	struct cnfa *cnfa;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	if (co == cnfa->bos[0] || co == cnfa->bos[1])
		return true;
	else
		return false;
}

/*
 * Check if color is end of line/string.
 */
int
pg_reg_colorisend(const regex_t *regex, int co)
{
	struct cnfa *cnfa;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	if (co == cnfa->eos[0] || co == cnfa->eos[1])
		return true;
	else
		return false;
}

/*
 * Get number of member chrs of color number "co".
 *
 * Note: we return -1 if the color number is invalid, or if it is a special
 * color (WHITE or a pseudocolor), or if the number of members is uncertain.
 * The latter case cannot arise right now but is specified to allow for future
 * improvements (see musings about run-time handling of higher character codes
 * in regex/README).  Callers should not try to extract the members if -1 is
 * returned.
 */
int
pg_reg_getnumcharacters(const regex_t *regex, int co)
{
	struct colormap *cm;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cm = &((struct guts *) regex->re_guts)->cmap;

	if (co <= 0 || co > cm->max)	/* we reject 0 which is WHITE */
		return -1;
	if (cm->cd[co].flags & PSEUDO)		/* also pseudocolors (BOS etc) */
		return -1;

	return cm->cd[co].nchrs;
}

/*
 * Write array of member chrs of color number "co" into chars[],
 * whose length chars_len must be at least as long as indicated by
 * pg_reg_getnumcharacters(), else not all chars will be returned.
 *
 * Fetching the members of WHITE or a pseudocolor is not supported.
 *
 * Caution: this is a relatively expensive operation.
 */
void
pg_reg_getcharacters(const regex_t *regex, int co,
					 pg_wchar *chars, int chars_len)
{
	struct colormap *cm;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cm = &((struct guts *) regex->re_guts)->cmap;

	if (co <= 0 || co > cm->max || chars_len <= 0)
		return;
	if (cm->cd[co].flags & PSEUDO)
		return;

	/* Recursively search the colormap tree */
	scancolormap(cm, co, cm->tree, 0, 0, &chars, &chars_len);
}

/*
 * Recursively scan the colormap tree to find chrs belonging to color "co".
 * See regex/README for info about the tree structure.
 *
 * t: tree block to scan
 * level: level (from 0) of t
 * partial: partial chr code for chrs within t
 * chars, chars_len: output area
 */
static void
scancolormap(struct colormap * cm, int co,
			 union tree * t, int level, chr partial,
			 pg_wchar **chars, int *chars_len)
{
	int			i;

	if (level < NBYTS - 1)
	{
		/* non-leaf node */
		for (i = 0; i < BYTTAB; i++)
		{
			/*
			 * We do not support search for chrs of color 0 (WHITE), so
			 * all-white subtrees need not be searched.  These can be
			 * recognized because they are represented by the fill blocks in
			 * the colormap struct.  This typically allows us to avoid
			 * scanning large regions of higher-numbered chrs.
			 */
			if (t->tptr[i] == &cm->tree[level + 1])
				continue;

			/* Recursively scan next level down */
			scancolormap(cm, co,
						 t->tptr[i], level + 1,
						 (partial | (chr) i) << BYTBITS,
						 chars, chars_len);
		}
	}
	else
	{
		/* leaf node */
		for (i = 0; i < BYTTAB; i++)
		{
			if (t->tcolor[i] == co)
			{
				if (*chars_len > 0)
				{
					**chars = partial | (chr) i;
					(*chars)++;
					(*chars_len)--;
				}
			}
		}
	}
}
