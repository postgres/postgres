/*-------------------------------------------------------------------------
 *
 * regprefix.c
 *	  Extract a common prefix, if any, from a compiled regex.
 *
 *
 * Portions Copyright (c) 2012-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1998, 1999 Henry Spencer
 *
 * IDENTIFICATION
 *	  src/backend/regex/regprefix.c
 *
 *-------------------------------------------------------------------------
 */

#include "regex/regguts.h"


/*
 * forward declarations
 */
static int findprefix(struct cnfa * cnfa, struct colormap * cm,
		   chr *string, size_t *slength);


/*
 * pg_regprefix - get common prefix for regular expression
 *
 * Returns one of:
 *	REG_NOMATCH: there is no common prefix of strings matching the regex
 *	REG_PREFIX: there is a common prefix of strings matching the regex
 *	REG_EXACT: all strings satisfying the regex must match the same string
 *	or a REG_XXX error code
 *
 * In the non-failure cases, *string is set to a malloc'd string containing
 * the common prefix or exact value, of length *slength (measured in chrs
 * not bytes!).
 *
 * This function does not analyze all complex cases (such as lookahead
 * constraints) exactly.  Therefore it is possible that some strings matching
 * the reported prefix or exact-match string do not satisfy the regex.	But
 * it should never be the case that a string satisfying the regex does not
 * match the reported prefix or exact-match string.
 */
int
pg_regprefix(regex_t *re,
			 chr **string,
			 size_t *slength)
{
	struct guts *g;
	struct cnfa *cnfa;
	int			st;

	/* sanity checks */
	if (string == NULL || slength == NULL)
		return REG_INVARG;
	*string = NULL;				/* initialize for failure cases */
	*slength = 0;
	if (re == NULL || re->re_magic != REMAGIC)
		return REG_INVARG;
	if (re->re_csize != sizeof(chr))
		return REG_MIXED;

	/* Initialize locale-dependent support */
	pg_set_regex_collation(re->re_collation);

	/* setup */
	g = (struct guts *) re->re_guts;
	if (g->info & REG_UIMPOSSIBLE)
		return REG_NOMATCH;

	/*
	 * This implementation considers only the search NFA for the topmost regex
	 * tree node.  Therefore, constraints such as backrefs are not fully
	 * applied, which is allowed per the function's API spec.
	 */
	assert(g->tree != NULL);
	cnfa = &g->tree->cnfa;

	/*
	 * Since a correct NFA should never contain any exit-free loops, it should
	 * not be possible for our traversal to return to a previously visited NFA
	 * state.  Hence we need at most nstates chrs in the output string.
	 */
	*string = (chr *) MALLOC(cnfa->nstates * sizeof(chr));
	if (*string == NULL)
		return REG_ESPACE;

	/* do it */
	st = findprefix(cnfa, &g->cmap, *string, slength);

	assert(*slength <= cnfa->nstates);

	/* clean up */
	if (st != REG_PREFIX && st != REG_EXACT)
	{
		FREE(*string);
		*string = NULL;
		*slength = 0;
	}

	return st;
}

/*
 * findprefix - extract common prefix from cNFA
 *
 * Results are returned into the preallocated chr array string[], with
 * *slength (which must be preset to zero) incremented for each chr.
 */
static int						/* regprefix return code */
findprefix(struct cnfa * cnfa,
		   struct colormap * cm,
		   chr *string,
		   size_t *slength)
{
	int			st;
	int			nextst;
	color		thiscolor;
	chr			c;
	struct carc *ca;

	/*
	 * The "pre" state must have only BOS/BOL outarcs, else pattern isn't
	 * anchored left.  If we have both BOS and BOL, they must go to the same
	 * next state.
	 */
	st = cnfa->pre;
	nextst = -1;
	for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
	{
		if (ca->co == cnfa->bos[0] || ca->co == cnfa->bos[1])
		{
			if (nextst == -1)
				nextst = ca->to;
			else if (nextst != ca->to)
				return REG_NOMATCH;
		}
		else
			return REG_NOMATCH;
	}
	if (nextst == -1)
		return REG_NOMATCH;

	/*
	 * Scan through successive states, stopping as soon as we find one with
	 * more than one acceptable transition character (either multiple colors
	 * on out-arcs, or a color with more than one member chr).
	 *
	 * We could find a state with multiple out-arcs that are all labeled with
	 * the same singleton color; this comes from patterns like "^ab(cde|cxy)".
	 * In that case we add the chr "c" to the output string but then exit the
	 * loop with nextst == -1.	This leaves a little bit on the table: if the
	 * pattern is like "^ab(cde|cdy)", we won't notice that "d" could be added
	 * to the prefix.  But chasing multiple parallel state chains doesn't seem
	 * worth the trouble.
	 */
	do
	{
		st = nextst;
		nextst = -1;
		thiscolor = COLORLESS;
		for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
		{
			/* We ignore lookahead constraints */
			if (ca->co >= cnfa->ncolors)
				continue;
			/* We can also ignore BOS/BOL arcs */
			if (ca->co == cnfa->bos[0] || ca->co == cnfa->bos[1])
				continue;
			/* ... but EOS/EOL arcs terminate the search */
			if (ca->co == cnfa->eos[0] || ca->co == cnfa->eos[1])
			{
				thiscolor = COLORLESS;
				break;
			}
			if (thiscolor == COLORLESS)
			{
				/* First plain outarc */
				thiscolor = ca->co;
				nextst = ca->to;
			}
			else if (thiscolor == ca->co)
			{
				/* Another plain outarc for same color */
				nextst = -1;
			}
			else
			{
				/* More than one plain outarc color terminates the search */
				thiscolor = COLORLESS;
				break;
			}
		}
		/* Done if we didn't find exactly one color on plain outarcs */
		if (thiscolor == COLORLESS)
			break;
		/* The color must be a singleton */
		if (cm->cd[thiscolor].nchrs != 1)
			break;

		/*
		 * Identify the color's sole member chr and add it to the prefix
		 * string.	In general the colormap data structure doesn't provide a
		 * way to find color member chrs, except by trying GETCOLOR() on each
		 * possible chr value, which won't do at all.  However, for the cases
		 * we care about it should be sufficient to test the "firstchr" value,
		 * that is the first chr ever added to the color.  There are cases
		 * where this might no longer be a member of the color (so we do need
		 * to test), but none of them are likely to arise for a character that
		 * is a member of a common prefix.	If we do hit such a corner case,
		 * we just fall out without adding anything to the prefix string.
		 */
		c = cm->cd[thiscolor].firstchr;
		if (GETCOLOR(cm, c) != thiscolor)
			break;

		string[(*slength)++] = c;

		/* Advance to next state, but only if we have a unique next state */
	} while (nextst != -1);

	/*
	 * If we ended at a state that only has EOS/EOL outarcs leading to the
	 * "post" state, then we have an exact-match string.  Note this is true
	 * even if the string is of zero length.
	 */
	nextst = -1;
	for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
	{
		if (ca->co == cnfa->eos[0] || ca->co == cnfa->eos[1])
		{
			if (nextst == -1)
				nextst = ca->to;
			else if (nextst != ca->to)
			{
				nextst = -1;
				break;
			}
		}
		else
		{
			nextst = -1;
			break;
		}
	}
	if (nextst == cnfa->post)
		return REG_EXACT;

	/*
	 * Otherwise, if we were unable to identify any prefix characters, say
	 * NOMATCH --- the pattern is anchored left, but doesn't specify any
	 * particular first character.
	 */
	if (*slength > 0)
		return REG_PREFIX;

	return REG_NOMATCH;
}
