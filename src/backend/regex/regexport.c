/*-------------------------------------------------------------------------
 *
 * regexport.c
 *	  Functions for exporting info about a regex's NFA
 *
 * In this implementation, the NFA defines a necessary but not sufficient
 * condition for a string to match the regex: that is, there can be strings
 * that match the NFA but don't match the full regex, but not vice versa.
 * Thus, for example, it is okay for the functions below to treat lookaround
 * constraints as no-ops, since they merely constrain the string some more.
 *
 * Notice that these functions return info into caller-provided arrays
 * rather than doing their own malloc's.  This simplifies the APIs by
 * eliminating a class of error conditions, and in the case of colors
 * allows the caller to decide how big is too big to bother with.
 *
 *
 * Portions Copyright (c) 2013-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1998, 1999 Henry Spencer
 *
 * IDENTIFICATION
 *	  src/backend/regex/regexport.c
 *
 *-------------------------------------------------------------------------
 */

#include "regex/regguts.h"

#include "regex/regexport.h"


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
 * pg_reg_getnumoutarcs() and pg_reg_getoutarcs() mask the existence of LACON
 * arcs from the caller, treating any LACON as being automatically satisfied.
 * Since the output representation does not support arcs that consume no
 * character when traversed, we have to recursively traverse LACON arcs here,
 * and report whatever normal arcs are reachable by traversing LACON arcs.
 * Note that this wouldn't work if it were possible to reach the final state
 * via LACON traversal, but the regex library never builds NFAs that have
 * LACON arcs leading directly to the final state.  (This is because the
 * regex executor is designed to consume one character beyond the nominal
 * match end --- possibly an EOS indicator --- so there is always a set of
 * ordinary arcs leading to the final state.)
 *
 * traverse_lacons is a recursive subroutine used by both exported functions
 * to count and then emit the reachable regular arcs.  *arcs_count is
 * incremented by the number of reachable arcs, and as many as will fit in
 * arcs_len (possibly 0) are emitted into arcs[].
 */
static void
traverse_lacons(struct cnfa *cnfa, int st,
				int *arcs_count,
				regex_arc_t *arcs, int arcs_len)
{
	struct carc *ca;

	/*
	 * Since this function recurses, it could theoretically be driven to stack
	 * overflow.  In practice, this is mostly useful to backstop against a
	 * failure of the regex compiler to remove a loop of LACON arcs.
	 */
	check_stack_depth();

	for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
	{
		if (ca->co < cnfa->ncolors)
		{
			/* Ordinary arc, so count and possibly emit it */
			int			ndx = (*arcs_count)++;

			if (ndx < arcs_len)
			{
				arcs[ndx].co = ca->co;
				arcs[ndx].to = ca->to;
			}
		}
		else
		{
			/* LACON arc --- assume it's satisfied and recurse... */
			/* ... but first, assert it doesn't lead directly to post state */
			Assert(ca->to != cnfa->post);

			traverse_lacons(cnfa, ca->to, arcs_count, arcs, arcs_len);
		}
	}
}

/*
 * Get number of outgoing NFA arcs of state number "st".
 */
int
pg_reg_getnumoutarcs(const regex_t *regex, int st)
{
	struct cnfa *cnfa;
	int			arcs_count;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	if (st < 0 || st >= cnfa->nstates)
		return 0;
	arcs_count = 0;
	traverse_lacons(cnfa, st, &arcs_count, NULL, 0);
	return arcs_count;
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
	int			arcs_count;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	if (st < 0 || st >= cnfa->nstates || arcs_len <= 0)
		return;
	arcs_count = 0;
	traverse_lacons(cnfa, st, &arcs_count, arcs, arcs_len);
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
 * color (WHITE, RAINBOW, or a pseudocolor), or if the number of members is
 * uncertain.
 * Callers should not try to extract the members if -1 is returned.
 */
int
pg_reg_getnumcharacters(const regex_t *regex, int co)
{
	struct colormap *cm;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cm = &((struct guts *) regex->re_guts)->cmap;

	if (co <= 0 || co > cm->max)	/* <= 0 rejects WHITE and RAINBOW */
		return -1;
	if (cm->cd[co].flags & PSEUDO)	/* also pseudocolors (BOS etc) */
		return -1;

	/*
	 * If the color appears anywhere in the high colormap, treat its number of
	 * members as uncertain.  In principle we could determine all the specific
	 * chrs corresponding to each such entry, but it would be expensive
	 * (particularly if character class tests are required) and it doesn't
	 * seem worth it.
	 */
	if (cm->cd[co].nuchrs != 0)
		return -1;

	/* OK, return the known number of member chrs */
	return cm->cd[co].nschrs;
}

/*
 * Write array of member chrs of color number "co" into chars[],
 * whose length chars_len must be at least as long as indicated by
 * pg_reg_getnumcharacters(), else not all chars will be returned.
 *
 * Fetching the members of WHITE, RAINBOW, or a pseudocolor is not supported.
 *
 * Caution: this is a relatively expensive operation.
 */
void
pg_reg_getcharacters(const regex_t *regex, int co,
					 pg_wchar *chars, int chars_len)
{
	struct colormap *cm;
	chr			c;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cm = &((struct guts *) regex->re_guts)->cmap;

	if (co <= 0 || co > cm->max || chars_len <= 0)
		return;
	if (cm->cd[co].flags & PSEUDO)
		return;

	/*
	 * We need only examine the low character map; there should not be any
	 * matching entries in the high map.
	 */
	for (c = CHR_MIN; c <= MAX_SIMPLE_CHR; c++)
	{
		if (cm->locolormap[c - CHR_MIN] == co)
		{
			*chars++ = c;
			if (--chars_len == 0)
				break;
		}
	}
}
