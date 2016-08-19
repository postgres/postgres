/*
 * re_*exec and friends - match REs
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
 * src/backend/regex/regexec.c
 *
 */

#include "regex/regguts.h"



/* lazy-DFA representation */
struct arcp
{								/* "pointer" to an outarc */
	struct sset *ss;
	color		co;
};

struct sset
{								/* state set */
	unsigned   *states;			/* pointer to bitvector */
	unsigned	hash;			/* hash of bitvector */
#define  HASH(bv, nw)	 (((nw) == 1) ? *(bv) : hash(bv, nw))
#define  HIT(h,bv,ss,nw) ((ss)->hash == (h) && ((nw) == 1 || \
		memcmp(VS(bv), VS((ss)->states), (nw)*sizeof(unsigned)) == 0))
	int			flags;
#define  STARTER	 01			/* the initial state set */
#define  POSTSTATE	 02			/* includes the goal state */
#define  LOCKED		 04			/* locked in cache */
#define  NOPROGRESS  010		/* zero-progress state set */
	struct arcp ins;			/* chain of inarcs pointing here */
	chr		   *lastseen;		/* last entered on arrival here */
	struct sset **outs;			/* outarc vector indexed by color */
	struct arcp *inchain;		/* chain-pointer vector for outarcs */
};

struct dfa
{
	int			nssets;			/* size of cache */
	int			nssused;		/* how many entries occupied yet */
	int			nstates;		/* number of states */
	int			ncolors;		/* length of outarc and inchain vectors */
	int			wordsper;		/* length of state-set bitvectors */
	struct sset *ssets;			/* state-set cache */
	unsigned   *statesarea;		/* bitvector storage */
	unsigned   *work;			/* pointer to work area within statesarea */
	struct sset **outsarea;		/* outarc-vector storage */
	struct arcp *incarea;		/* inchain storage */
	struct cnfa *cnfa;
	struct colormap *cm;
	chr		   *lastpost;		/* location of last cache-flushed success */
	chr		   *lastnopr;		/* location of last cache-flushed NOPROGRESS */
	struct sset *search;		/* replacement-search-pointer memory */
	int			cptsmalloced;	/* were the areas individually malloced? */
	char	   *mallocarea;		/* self, or master malloced area, or NULL */
};

#define WORK	1				/* number of work bitvectors needed */

/* setup for non-malloc allocation for small cases */
#define FEWSTATES	20			/* must be less than UBITS */
#define FEWCOLORS	15
struct smalldfa
{
	struct dfa	dfa;
	struct sset ssets[FEWSTATES * 2];
	unsigned	statesarea[FEWSTATES * 2 + WORK];
	struct sset *outsarea[FEWSTATES * 2 * FEWCOLORS];
	struct arcp incarea[FEWSTATES * 2 * FEWCOLORS];
};

#define DOMALLOC	((struct smalldfa *)NULL)	/* force malloc */



/* internal variables, bundled for easy passing around */
struct vars
{
	regex_t    *re;
	struct guts *g;
	int			eflags;			/* copies of arguments */
	size_t		nmatch;
	regmatch_t *pmatch;
	rm_detail_t *details;
	chr		   *start;			/* start of string */
	chr		   *search_start;	/* search start of string */
	chr		   *stop;			/* just past end of string */
	int			err;			/* error code if any (0 none) */
	struct dfa **subdfas;		/* per-tree-subre DFAs */
	struct dfa **ladfas;		/* per-lacon-subre DFAs */
	struct sset **lblastcss;	/* per-lacon-subre lookbehind restart data */
	chr		  **lblastcp;		/* per-lacon-subre lookbehind restart data */
	struct smalldfa dfa1;
	struct smalldfa dfa2;
};

#define VISERR(vv)	((vv)->err != 0)	/* have we seen an error yet? */
#define ISERR() VISERR(v)
#define VERR(vv,e)	((vv)->err = ((vv)->err ? (vv)->err : (e)))
#define ERR(e)	VERR(v, e)		/* record an error */
#define NOERR() {if (ISERR()) return v->err;}	/* if error seen, return it */
#define OFF(p)	((p) - v->start)
#define LOFF(p) ((long)OFF(p))



/*
 * forward declarations
 */
/* === regexec.c === */
static struct dfa *getsubdfa(struct vars *, struct subre *);
static struct dfa *getladfa(struct vars *, int);
static int	find(struct vars *, struct cnfa *, struct colormap *);
static int	cfind(struct vars *, struct cnfa *, struct colormap *);
static int	cfindloop(struct vars *, struct cnfa *, struct colormap *, struct dfa *, struct dfa *, chr **);
static void zapallsubs(regmatch_t *, size_t);
static void zaptreesubs(struct vars *, struct subre *);
static void subset(struct vars *, struct subre *, chr *, chr *);
static int	cdissect(struct vars *, struct subre *, chr *, chr *);
static int	ccondissect(struct vars *, struct subre *, chr *, chr *);
static int	crevcondissect(struct vars *, struct subre *, chr *, chr *);
static int	cbrdissect(struct vars *, struct subre *, chr *, chr *);
static int	caltdissect(struct vars *, struct subre *, chr *, chr *);
static int	citerdissect(struct vars *, struct subre *, chr *, chr *);
static int	creviterdissect(struct vars *, struct subre *, chr *, chr *);

/* === rege_dfa.c === */
static chr *longest(struct vars *, struct dfa *, chr *, chr *, int *);
static chr *shortest(struct vars *, struct dfa *, chr *, chr *, chr *, chr **, int *);
static int	matchuntil(struct vars *, struct dfa *, chr *, struct sset **, chr **);
static chr *lastcold(struct vars *, struct dfa *);
static struct dfa *newdfa(struct vars *, struct cnfa *, struct colormap *, struct smalldfa *);
static void freedfa(struct dfa *);
static unsigned hash(unsigned *, int);
static struct sset *initialize(struct vars *, struct dfa *, chr *);
static struct sset *miss(struct vars *, struct dfa *, struct sset *, color, chr *, chr *);
static int	lacon(struct vars *, struct cnfa *, chr *, color);
static struct sset *getvacant(struct vars *, struct dfa *, chr *, chr *);
static struct sset *pickss(struct vars *, struct dfa *, chr *, chr *);


/*
 * pg_regexec - match regular expression
 */
int
pg_regexec(regex_t *re,
		   const chr *string,
		   size_t len,
		   size_t search_start,
		   rm_detail_t *details,
		   size_t nmatch,
		   regmatch_t pmatch[],
		   int flags)
{
	struct vars var;
	register struct vars *v = &var;
	int			st;
	size_t		n;
	size_t		i;
	int			backref;

#define  LOCALMAT	 20
	regmatch_t	mat[LOCALMAT];

#define  LOCALDFAS	 40
	struct dfa *subdfas[LOCALDFAS];

	/* sanity checks */
	if (re == NULL || string == NULL || re->re_magic != REMAGIC)
		return REG_INVARG;
	if (re->re_csize != sizeof(chr))
		return REG_MIXED;

	/* Initialize locale-dependent support */
	pg_set_regex_collation(re->re_collation);

	/* setup */
	v->re = re;
	v->g = (struct guts *) re->re_guts;
	if ((v->g->cflags & REG_EXPECT) && details == NULL)
		return REG_INVARG;
	if (v->g->info & REG_UIMPOSSIBLE)
		return REG_NOMATCH;
	backref = (v->g->info & REG_UBACKREF) ? 1 : 0;
	v->eflags = flags;
	if (v->g->cflags & REG_NOSUB)
		nmatch = 0;				/* override client */
	v->nmatch = nmatch;
	if (backref)
	{
		/* need work area */
		if (v->g->nsub + 1 <= LOCALMAT)
			v->pmatch = mat;
		else
			v->pmatch = (regmatch_t *) MALLOC((v->g->nsub + 1) *
											  sizeof(regmatch_t));
		if (v->pmatch == NULL)
			return REG_ESPACE;
		v->nmatch = v->g->nsub + 1;
	}
	else
		v->pmatch = pmatch;
	v->details = details;
	v->start = (chr *) string;
	v->search_start = (chr *) string + search_start;
	v->stop = (chr *) string + len;
	v->err = 0;
	v->subdfas = NULL;
	v->ladfas = NULL;
	v->lblastcss = NULL;
	v->lblastcp = NULL;
	/* below this point, "goto cleanup" will behave sanely */

	assert(v->g->ntree >= 0);
	n = (size_t) v->g->ntree;
	if (n <= LOCALDFAS)
		v->subdfas = subdfas;
	else
	{
		v->subdfas = (struct dfa **) MALLOC(n * sizeof(struct dfa *));
		if (v->subdfas == NULL)
		{
			st = REG_ESPACE;
			goto cleanup;
		}
	}
	for (i = 0; i < n; i++)
		v->subdfas[i] = NULL;

	assert(v->g->nlacons >= 0);
	n = (size_t) v->g->nlacons;
	if (n > 0)
	{
		v->ladfas = (struct dfa **) MALLOC(n * sizeof(struct dfa *));
		if (v->ladfas == NULL)
		{
			st = REG_ESPACE;
			goto cleanup;
		}
		for (i = 0; i < n; i++)
			v->ladfas[i] = NULL;
		v->lblastcss = (struct sset **) MALLOC(n * sizeof(struct sset *));
		v->lblastcp = (chr **) MALLOC(n * sizeof(chr *));
		if (v->lblastcss == NULL || v->lblastcp == NULL)
		{
			st = REG_ESPACE;
			goto cleanup;
		}
		for (i = 0; i < n; i++)
		{
			v->lblastcss[i] = NULL;
			v->lblastcp[i] = NULL;
		}
	}

	/* do it */
	assert(v->g->tree != NULL);
	if (backref)
		st = cfind(v, &v->g->tree->cnfa, &v->g->cmap);
	else
		st = find(v, &v->g->tree->cnfa, &v->g->cmap);

	/* copy (portion of) match vector over if necessary */
	if (st == REG_OKAY && v->pmatch != pmatch && nmatch > 0)
	{
		zapallsubs(pmatch, nmatch);
		n = (nmatch < v->nmatch) ? nmatch : v->nmatch;
		memcpy(VS(pmatch), VS(v->pmatch), n * sizeof(regmatch_t));
	}

	/* clean up */
cleanup:
	if (v->pmatch != pmatch && v->pmatch != mat)
		FREE(v->pmatch);
	if (v->subdfas != NULL)
	{
		n = (size_t) v->g->ntree;
		for (i = 0; i < n; i++)
		{
			if (v->subdfas[i] != NULL)
				freedfa(v->subdfas[i]);
		}
		if (v->subdfas != subdfas)
			FREE(v->subdfas);
	}
	if (v->ladfas != NULL)
	{
		n = (size_t) v->g->nlacons;
		for (i = 0; i < n; i++)
		{
			if (v->ladfas[i] != NULL)
				freedfa(v->ladfas[i]);
		}
		FREE(v->ladfas);
	}
	if (v->lblastcss != NULL)
		FREE(v->lblastcss);
	if (v->lblastcp != NULL)
		FREE(v->lblastcp);

	return st;
}

/*
 * getsubdfa - create or re-fetch the DFA for a tree subre node
 *
 * We only need to create the DFA once per overall regex execution.
 * The DFA will be freed by the cleanup step in pg_regexec().
 */
static struct dfa *
getsubdfa(struct vars * v,
		  struct subre * t)
{
	if (v->subdfas[t->id] == NULL)
	{
		v->subdfas[t->id] = newdfa(v, &t->cnfa, &v->g->cmap, DOMALLOC);
		if (ISERR())
			return NULL;
	}
	return v->subdfas[t->id];
}

/*
 * getladfa - create or re-fetch the DFA for a LACON subre node
 *
 * Same as above, but for LACONs.
 */
static struct dfa *
getladfa(struct vars * v,
		 int n)
{
	assert(n > 0 && n < v->g->nlacons && v->g->lacons != NULL);

	if (v->ladfas[n] == NULL)
	{
		struct subre *sub = &v->g->lacons[n];

		v->ladfas[n] = newdfa(v, &sub->cnfa, &v->g->cmap, DOMALLOC);
		if (ISERR())
			return NULL;
	}
	return v->ladfas[n];
}

/*
 * find - find a match for the main NFA (no-complications case)
 */
static int
find(struct vars * v,
	 struct cnfa * cnfa,
	 struct colormap * cm)
{
	struct dfa *s;
	struct dfa *d;
	chr		   *begin;
	chr		   *end = NULL;
	chr		   *cold;
	chr		   *open;			/* open and close of range of possible starts */
	chr		   *close;
	int			hitend;
	int			shorter = (v->g->tree->flags & SHORTER) ? 1 : 0;

	/* first, a shot with the search RE */
	s = newdfa(v, &v->g->search, cm, &v->dfa1);
	assert(!(ISERR() && s != NULL));
	NOERR();
	MDEBUG(("\nsearch at %ld\n", LOFF(v->start)));
	cold = NULL;
	close = shortest(v, s, v->search_start, v->search_start, v->stop,
					 &cold, (int *) NULL);
	freedfa(s);
	NOERR();
	if (v->g->cflags & REG_EXPECT)
	{
		assert(v->details != NULL);
		if (cold != NULL)
			v->details->rm_extend.rm_so = OFF(cold);
		else
			v->details->rm_extend.rm_so = OFF(v->stop);
		v->details->rm_extend.rm_eo = OFF(v->stop);		/* unknown */
	}
	if (close == NULL)			/* not found */
		return REG_NOMATCH;
	if (v->nmatch == 0)			/* found, don't need exact location */
		return REG_OKAY;

	/* find starting point and match */
	assert(cold != NULL);
	open = cold;
	cold = NULL;
	MDEBUG(("between %ld and %ld\n", LOFF(open), LOFF(close)));
	d = newdfa(v, cnfa, cm, &v->dfa1);
	assert(!(ISERR() && d != NULL));
	NOERR();
	for (begin = open; begin <= close; begin++)
	{
		MDEBUG(("\nfind trying at %ld\n", LOFF(begin)));
		if (shorter)
			end = shortest(v, d, begin, begin, v->stop,
						   (chr **) NULL, &hitend);
		else
			end = longest(v, d, begin, v->stop, &hitend);
		if (ISERR())
		{
			freedfa(d);
			return v->err;
		}
		if (hitend && cold == NULL)
			cold = begin;
		if (end != NULL)
			break;				/* NOTE BREAK OUT */
	}
	assert(end != NULL);		/* search RE succeeded so loop should */
	freedfa(d);

	/* and pin down details */
	assert(v->nmatch > 0);
	v->pmatch[0].rm_so = OFF(begin);
	v->pmatch[0].rm_eo = OFF(end);
	if (v->g->cflags & REG_EXPECT)
	{
		if (cold != NULL)
			v->details->rm_extend.rm_so = OFF(cold);
		else
			v->details->rm_extend.rm_so = OFF(v->stop);
		v->details->rm_extend.rm_eo = OFF(v->stop);		/* unknown */
	}
	if (v->nmatch == 1)			/* no need for submatches */
		return REG_OKAY;

	/* find submatches */
	zapallsubs(v->pmatch, v->nmatch);
	return cdissect(v, v->g->tree, begin, end);
}

/*
 * cfind - find a match for the main NFA (with complications)
 */
static int
cfind(struct vars * v,
	  struct cnfa * cnfa,
	  struct colormap * cm)
{
	struct dfa *s;
	struct dfa *d;
	chr		   *cold;
	int			ret;

	s = newdfa(v, &v->g->search, cm, &v->dfa1);
	NOERR();
	d = newdfa(v, cnfa, cm, &v->dfa2);
	if (ISERR())
	{
		assert(d == NULL);
		freedfa(s);
		return v->err;
	}

	ret = cfindloop(v, cnfa, cm, d, s, &cold);

	freedfa(d);
	freedfa(s);
	NOERR();
	if (v->g->cflags & REG_EXPECT)
	{
		assert(v->details != NULL);
		if (cold != NULL)
			v->details->rm_extend.rm_so = OFF(cold);
		else
			v->details->rm_extend.rm_so = OFF(v->stop);
		v->details->rm_extend.rm_eo = OFF(v->stop);		/* unknown */
	}
	return ret;
}

/*
 * cfindloop - the heart of cfind
 */
static int
cfindloop(struct vars * v,
		  struct cnfa * cnfa,
		  struct colormap * cm,
		  struct dfa * d,
		  struct dfa * s,
		  chr **coldp)			/* where to put coldstart pointer */
{
	chr		   *begin;
	chr		   *end;
	chr		   *cold;
	chr		   *open;			/* open and close of range of possible starts */
	chr		   *close;
	chr		   *estart;
	chr		   *estop;
	int			er;
	int			shorter = v->g->tree->flags & SHORTER;
	int			hitend;

	assert(d != NULL && s != NULL);
	cold = NULL;
	close = v->search_start;
	do
	{
		/* Search with the search RE for match range at/beyond "close" */
		MDEBUG(("\ncsearch at %ld\n", LOFF(close)));
		close = shortest(v, s, close, close, v->stop, &cold, (int *) NULL);
		if (ISERR())
		{
			*coldp = cold;
			return v->err;
		}
		if (close == NULL)
			break;				/* no more possible match anywhere */
		assert(cold != NULL);
		open = cold;
		cold = NULL;
		/* Search for matches starting between "open" and "close" inclusive */
		MDEBUG(("cbetween %ld and %ld\n", LOFF(open), LOFF(close)));
		for (begin = open; begin <= close; begin++)
		{
			MDEBUG(("\ncfind trying at %ld\n", LOFF(begin)));
			estart = begin;
			estop = v->stop;
			for (;;)
			{
				/* Here we use the top node's detailed RE */
				if (shorter)
					end = shortest(v, d, begin, estart,
								   estop, (chr **) NULL, &hitend);
				else
					end = longest(v, d, begin, estop,
								  &hitend);
				if (ISERR())
				{
					*coldp = cold;
					return v->err;
				}
				if (hitend && cold == NULL)
					cold = begin;
				if (end == NULL)
					break;		/* no match with this begin point, try next */
				MDEBUG(("tentative end %ld\n", LOFF(end)));
				/* Dissect the potential match to see if it really matches */
				zapallsubs(v->pmatch, v->nmatch);
				er = cdissect(v, v->g->tree, begin, end);
				if (er == REG_OKAY)
				{
					if (v->nmatch > 0)
					{
						v->pmatch[0].rm_so = OFF(begin);
						v->pmatch[0].rm_eo = OFF(end);
					}
					*coldp = cold;
					return REG_OKAY;
				}
				if (er != REG_NOMATCH)
				{
					ERR(er);
					*coldp = cold;
					return er;
				}
				/* Try next longer/shorter match with same begin point */
				if (shorter)
				{
					if (end == estop)
						break;	/* no more, so try next begin point */
					estart = end + 1;
				}
				else
				{
					if (end == begin)
						break;	/* no more, so try next begin point */
					estop = end - 1;
				}
			}					/* end loop over endpoint positions */
		}						/* end loop over beginning positions */

		/*
		 * If we get here, there is no possible match starting at or before
		 * "close", so consider matches beyond that.  We'll do a fresh search
		 * with the search RE to find a new promising match range.
		 */
		close++;
	} while (close < v->stop);

	*coldp = cold;
	return REG_NOMATCH;
}

/*
 * zapallsubs - initialize all subexpression matches to "no match"
 */
static void
zapallsubs(regmatch_t *p,
		   size_t n)
{
	size_t		i;

	for (i = n - 1; i > 0; i--)
	{
		p[i].rm_so = -1;
		p[i].rm_eo = -1;
	}
}

/*
 * zaptreesubs - initialize subexpressions within subtree to "no match"
 */
static void
zaptreesubs(struct vars * v,
			struct subre * t)
{
	if (t->op == '(')
	{
		int			n = t->subno;

		assert(n > 0);
		if ((size_t) n < v->nmatch)
		{
			v->pmatch[n].rm_so = -1;
			v->pmatch[n].rm_eo = -1;
		}
	}

	if (t->left != NULL)
		zaptreesubs(v, t->left);
	if (t->right != NULL)
		zaptreesubs(v, t->right);
}

/*
 * subset - set subexpression match data for a successful subre
 */
static void
subset(struct vars * v,
	   struct subre * sub,
	   chr *begin,
	   chr *end)
{
	int			n = sub->subno;

	assert(n > 0);
	if ((size_t) n >= v->nmatch)
		return;

	MDEBUG(("setting %d\n", n));
	v->pmatch[n].rm_so = OFF(begin);
	v->pmatch[n].rm_eo = OFF(end);
}

/*
 * cdissect - check backrefs and determine subexpression matches
 *
 * cdissect recursively processes a subre tree to check matching of backrefs
 * and/or identify submatch boundaries for capture nodes.  The proposed match
 * runs from "begin" to "end" (not including "end"), and we are basically
 * "dissecting" it to see where the submatches are.
 *
 * Before calling any level of cdissect, the caller must have run the node's
 * DFA and found that the proposed substring satisfies the DFA.  (We make
 * the caller do that because in concatenation and iteration nodes, it's
 * much faster to check all the substrings against the child DFAs before we
 * recurse.)  Also, caller must have cleared subexpression match data via
 * zaptreesubs (or zapallsubs at the top level).
 */
static int						/* regexec return code */
cdissect(struct vars * v,
		 struct subre * t,
		 chr *begin,			/* beginning of relevant substring */
		 chr *end)				/* end of same */
{
	int			er;

	assert(t != NULL);
	MDEBUG(("cdissect %ld-%ld %c\n", LOFF(begin), LOFF(end), t->op));

	/* handy place to check for operation cancel */
	if (CANCEL_REQUESTED(v->re))
		return REG_CANCEL;
	/* ... and stack overrun */
	if (STACK_TOO_DEEP(v->re))
		return REG_ETOOBIG;

	switch (t->op)
	{
		case '=':				/* terminal node */
			assert(t->left == NULL && t->right == NULL);
			er = REG_OKAY;		/* no action, parent did the work */
			break;
		case 'b':				/* back reference */
			assert(t->left == NULL && t->right == NULL);
			er = cbrdissect(v, t, begin, end);
			break;
		case '.':				/* concatenation */
			assert(t->left != NULL && t->right != NULL);
			if (t->left->flags & SHORTER)		/* reverse scan */
				er = crevcondissect(v, t, begin, end);
			else
				er = ccondissect(v, t, begin, end);
			break;
		case '|':				/* alternation */
			assert(t->left != NULL);
			er = caltdissect(v, t, begin, end);
			break;
		case '*':				/* iteration */
			assert(t->left != NULL);
			if (t->left->flags & SHORTER)		/* reverse scan */
				er = creviterdissect(v, t, begin, end);
			else
				er = citerdissect(v, t, begin, end);
			break;
		case '(':				/* capturing */
			assert(t->left != NULL && t->right == NULL);
			assert(t->subno > 0);
			er = cdissect(v, t->left, begin, end);
			if (er == REG_OKAY)
				subset(v, t, begin, end);
			break;
		default:
			er = REG_ASSERT;
			break;
	}

	/*
	 * We should never have a match failure unless backrefs lurk below;
	 * otherwise, either caller failed to check the DFA, or there's some
	 * inconsistency between the DFA and the node's innards.
	 */
	assert(er != REG_NOMATCH || (t->flags & BACKR));

	return er;
}

/*
 * ccondissect - dissect match for concatenation node
 */
static int						/* regexec return code */
ccondissect(struct vars * v,
			struct subre * t,
			chr *begin,			/* beginning of relevant substring */
			chr *end)			/* end of same */
{
	struct dfa *d;
	struct dfa *d2;
	chr		   *mid;
	int			er;

	assert(t->op == '.');
	assert(t->left != NULL && t->left->cnfa.nstates > 0);
	assert(t->right != NULL && t->right->cnfa.nstates > 0);
	assert(!(t->left->flags & SHORTER));

	d = getsubdfa(v, t->left);
	NOERR();
	d2 = getsubdfa(v, t->right);
	NOERR();
	MDEBUG(("cconcat %d\n", t->id));

	/* pick a tentative midpoint */
	mid = longest(v, d, begin, end, (int *) NULL);
	NOERR();
	if (mid == NULL)
		return REG_NOMATCH;
	MDEBUG(("tentative midpoint %ld\n", LOFF(mid)));

	/* iterate until satisfaction or failure */
	for (;;)
	{
		/* try this midpoint on for size */
		if (longest(v, d2, mid, end, (int *) NULL) == end)
		{
			er = cdissect(v, t->left, begin, mid);
			if (er == REG_OKAY)
			{
				er = cdissect(v, t->right, mid, end);
				if (er == REG_OKAY)
				{
					/* satisfaction */
					MDEBUG(("successful\n"));
					return REG_OKAY;
				}
			}
			if (er != REG_NOMATCH)
				return er;
		}
		NOERR();

		/* that midpoint didn't work, find a new one */
		if (mid == begin)
		{
			/* all possibilities exhausted */
			MDEBUG(("%d no midpoint\n", t->id));
			return REG_NOMATCH;
		}
		mid = longest(v, d, begin, mid - 1, (int *) NULL);
		NOERR();
		if (mid == NULL)
		{
			/* failed to find a new one */
			MDEBUG(("%d failed midpoint\n", t->id));
			return REG_NOMATCH;
		}
		MDEBUG(("%d: new midpoint %ld\n", t->id, LOFF(mid)));
		zaptreesubs(v, t->left);
		zaptreesubs(v, t->right);
	}

	/* can't get here */
	return REG_ASSERT;
}

/*
 * crevcondissect - dissect match for concatenation node, shortest-first
 */
static int						/* regexec return code */
crevcondissect(struct vars * v,
			   struct subre * t,
			   chr *begin,		/* beginning of relevant substring */
			   chr *end)		/* end of same */
{
	struct dfa *d;
	struct dfa *d2;
	chr		   *mid;
	int			er;

	assert(t->op == '.');
	assert(t->left != NULL && t->left->cnfa.nstates > 0);
	assert(t->right != NULL && t->right->cnfa.nstates > 0);
	assert(t->left->flags & SHORTER);

	d = getsubdfa(v, t->left);
	NOERR();
	d2 = getsubdfa(v, t->right);
	NOERR();
	MDEBUG(("crevcon %d\n", t->id));

	/* pick a tentative midpoint */
	mid = shortest(v, d, begin, begin, end, (chr **) NULL, (int *) NULL);
	NOERR();
	if (mid == NULL)
		return REG_NOMATCH;
	MDEBUG(("tentative midpoint %ld\n", LOFF(mid)));

	/* iterate until satisfaction or failure */
	for (;;)
	{
		/* try this midpoint on for size */
		if (longest(v, d2, mid, end, (int *) NULL) == end)
		{
			er = cdissect(v, t->left, begin, mid);
			if (er == REG_OKAY)
			{
				er = cdissect(v, t->right, mid, end);
				if (er == REG_OKAY)
				{
					/* satisfaction */
					MDEBUG(("successful\n"));
					return REG_OKAY;
				}
			}
			if (er != REG_NOMATCH)
				return er;
		}
		NOERR();

		/* that midpoint didn't work, find a new one */
		if (mid == end)
		{
			/* all possibilities exhausted */
			MDEBUG(("%d no midpoint\n", t->id));
			return REG_NOMATCH;
		}
		mid = shortest(v, d, begin, mid + 1, end, (chr **) NULL, (int *) NULL);
		NOERR();
		if (mid == NULL)
		{
			/* failed to find a new one */
			MDEBUG(("%d failed midpoint\n", t->id));
			return REG_NOMATCH;
		}
		MDEBUG(("%d: new midpoint %ld\n", t->id, LOFF(mid)));
		zaptreesubs(v, t->left);
		zaptreesubs(v, t->right);
	}

	/* can't get here */
	return REG_ASSERT;
}

/*
 * cbrdissect - dissect match for backref node
 */
static int						/* regexec return code */
cbrdissect(struct vars * v,
		   struct subre * t,
		   chr *begin,			/* beginning of relevant substring */
		   chr *end)			/* end of same */
{
	int			n = t->subno;
	size_t		numreps;
	size_t		tlen;
	size_t		brlen;
	chr		   *brstring;
	chr		   *p;
	int			min = t->min;
	int			max = t->max;

	assert(t != NULL);
	assert(t->op == 'b');
	assert(n >= 0);
	assert((size_t) n < v->nmatch);

	MDEBUG(("cbackref n%d %d{%d-%d}\n", t->id, n, min, max));

	/* get the backreferenced string */
	if (v->pmatch[n].rm_so == -1)
		return REG_NOMATCH;
	brstring = v->start + v->pmatch[n].rm_so;
	brlen = v->pmatch[n].rm_eo - v->pmatch[n].rm_so;

	/* special cases for zero-length strings */
	if (brlen == 0)
	{
		/*
		 * matches only if target is zero length, but any number of
		 * repetitions can be considered to be present
		 */
		if (begin == end && min <= max)
		{
			MDEBUG(("cbackref matched trivially\n"));
			return REG_OKAY;
		}
		return REG_NOMATCH;
	}
	if (begin == end)
	{
		/* matches only if zero repetitions are okay */
		if (min == 0)
		{
			MDEBUG(("cbackref matched trivially\n"));
			return REG_OKAY;
		}
		return REG_NOMATCH;
	}

	/*
	 * check target length to see if it could possibly be an allowed number of
	 * repetitions of brstring
	 */
	assert(end > begin);
	tlen = end - begin;
	if (tlen % brlen != 0)
		return REG_NOMATCH;
	numreps = tlen / brlen;
	if (numreps < min || (numreps > max && max != DUPINF))
		return REG_NOMATCH;

	/* okay, compare the actual string contents */
	p = begin;
	while (numreps-- > 0)
	{
		if ((*v->g->compare) (brstring, p, brlen) != 0)
			return REG_NOMATCH;
		p += brlen;
	}

	MDEBUG(("cbackref matched\n"));
	return REG_OKAY;
}

/*
 * caltdissect - dissect match for alternation node
 */
static int						/* regexec return code */
caltdissect(struct vars * v,
			struct subre * t,
			chr *begin,			/* beginning of relevant substring */
			chr *end)			/* end of same */
{
	struct dfa *d;
	int			er;

	/* We loop, rather than tail-recurse, to handle a chain of alternatives */
	while (t != NULL)
	{
		assert(t->op == '|');
		assert(t->left != NULL && t->left->cnfa.nstates > 0);

		MDEBUG(("calt n%d\n", t->id));

		d = getsubdfa(v, t->left);
		NOERR();
		if (longest(v, d, begin, end, (int *) NULL) == end)
		{
			MDEBUG(("calt matched\n"));
			er = cdissect(v, t->left, begin, end);
			if (er != REG_NOMATCH)
				return er;
		}
		NOERR();

		t = t->right;
	}

	return REG_NOMATCH;
}

/*
 * citerdissect - dissect match for iteration node
 */
static int						/* regexec return code */
citerdissect(struct vars * v,
			 struct subre * t,
			 chr *begin,		/* beginning of relevant substring */
			 chr *end)			/* end of same */
{
	struct dfa *d;
	chr		  **endpts;
	chr		   *limit;
	int			min_matches;
	size_t		max_matches;
	int			nverified;
	int			k;
	int			i;
	int			er;

	assert(t->op == '*');
	assert(t->left != NULL && t->left->cnfa.nstates > 0);
	assert(!(t->left->flags & SHORTER));
	assert(begin <= end);

	/*
	 * For the moment, assume the minimum number of matches is 1.  If zero
	 * matches are allowed, and the target string is empty, we are allowed to
	 * match regardless of the contents of the iter node --- but we would
	 * prefer to match once, so that capturing parens get set.  (An example of
	 * the concern here is a pattern like "()*\1", which historically this
	 * code has allowed to succeed.)  Therefore, we deal with the zero-matches
	 * case at the bottom, after failing to find any other way to match.
	 */
	min_matches = t->min;
	if (min_matches <= 0)
		min_matches = 1;

	/*
	 * We need workspace to track the endpoints of each sub-match.  Normally
	 * we consider only nonzero-length sub-matches, so there can be at most
	 * end-begin of them.  However, if min is larger than that, we will also
	 * consider zero-length sub-matches in order to find enough matches.
	 *
	 * For convenience, endpts[0] contains the "begin" pointer and we store
	 * sub-match endpoints in endpts[1..max_matches].
	 */
	max_matches = end - begin;
	if (max_matches > t->max && t->max != DUPINF)
		max_matches = t->max;
	if (max_matches < min_matches)
		max_matches = min_matches;
	endpts = (chr **) MALLOC((max_matches + 1) * sizeof(chr *));
	if (endpts == NULL)
		return REG_ESPACE;
	endpts[0] = begin;

	d = getsubdfa(v, t->left);
	if (ISERR())
	{
		FREE(endpts);
		return v->err;
	}
	MDEBUG(("citer %d\n", t->id));

	/*
	 * Our strategy is to first find a set of sub-match endpoints that are
	 * valid according to the child node's DFA, and then recursively dissect
	 * each sub-match to confirm validity.  If any validity check fails,
	 * backtrack the last sub-match and try again.  And, when we next try for
	 * a validity check, we need not recheck any successfully verified
	 * sub-matches that we didn't move the endpoints of.  nverified remembers
	 * how many sub-matches are currently known okay.
	 */

	/* initialize to consider first sub-match */
	nverified = 0;
	k = 1;
	limit = end;

	/* iterate until satisfaction or failure */
	while (k > 0)
	{
		/* try to find an endpoint for the k'th sub-match */
		endpts[k] = longest(v, d, endpts[k - 1], limit, (int *) NULL);
		if (ISERR())
		{
			FREE(endpts);
			return v->err;
		}
		if (endpts[k] == NULL)
		{
			/* no match possible, so see if we can shorten previous one */
			k--;
			goto backtrack;
		}
		MDEBUG(("%d: working endpoint %d: %ld\n",
				t->id, k, LOFF(endpts[k])));

		/* k'th sub-match can no longer be considered verified */
		if (nverified >= k)
			nverified = k - 1;

		if (endpts[k] != end)
		{
			/* haven't reached end yet, try another iteration if allowed */
			if (k >= max_matches)
			{
				/* must try to shorten some previous match */
				k--;
				goto backtrack;
			}

			/* reject zero-length match unless necessary to achieve min */
			if (endpts[k] == endpts[k - 1] &&
				(k >= min_matches || min_matches - k < end - endpts[k]))
				goto backtrack;

			k++;
			limit = end;
			continue;
		}

		/*
		 * We've identified a way to divide the string into k sub-matches that
		 * works so far as the child DFA can tell.  If k is an allowed number
		 * of matches, start the slow part: recurse to verify each sub-match.
		 * We always have k <= max_matches, needn't check that.
		 */
		if (k < min_matches)
			goto backtrack;

		MDEBUG(("%d: verifying %d..%d\n", t->id, nverified + 1, k));

		for (i = nverified + 1; i <= k; i++)
		{
			zaptreesubs(v, t->left);
			er = cdissect(v, t->left, endpts[i - 1], endpts[i]);
			if (er == REG_OKAY)
			{
				nverified = i;
				continue;
			}
			if (er == REG_NOMATCH)
				break;
			/* oops, something failed */
			FREE(endpts);
			return er;
		}

		if (i > k)
		{
			/* satisfaction */
			MDEBUG(("%d successful\n", t->id));
			FREE(endpts);
			return REG_OKAY;
		}

		/* match failed to verify, so backtrack */

backtrack:

		/*
		 * Must consider shorter versions of the current sub-match.  However,
		 * we'll only ask for a zero-length match if necessary.
		 */
		while (k > 0)
		{
			chr		   *prev_end = endpts[k - 1];

			if (endpts[k] > prev_end)
			{
				limit = endpts[k] - 1;
				if (limit > prev_end ||
					(k < min_matches && min_matches - k >= end - prev_end))
				{
					/* break out of backtrack loop, continue the outer one */
					break;
				}
			}
			/* can't shorten k'th sub-match any more, consider previous one */
			k--;
		}
	}

	/* all possibilities exhausted */
	FREE(endpts);

	/*
	 * Now consider the possibility that we can match to a zero-length string
	 * by using zero repetitions.
	 */
	if (t->min == 0 && begin == end)
	{
		MDEBUG(("%d allowing zero matches\n", t->id));
		return REG_OKAY;
	}

	MDEBUG(("%d failed\n", t->id));
	return REG_NOMATCH;
}

/*
 * creviterdissect - dissect match for iteration node, shortest-first
 */
static int						/* regexec return code */
creviterdissect(struct vars * v,
				struct subre * t,
				chr *begin,		/* beginning of relevant substring */
				chr *end)		/* end of same */
{
	struct dfa *d;
	chr		  **endpts;
	chr		   *limit;
	int			min_matches;
	size_t		max_matches;
	int			nverified;
	int			k;
	int			i;
	int			er;

	assert(t->op == '*');
	assert(t->left != NULL && t->left->cnfa.nstates > 0);
	assert(t->left->flags & SHORTER);
	assert(begin <= end);

	/*
	 * If zero matches are allowed, and target string is empty, just declare
	 * victory.  OTOH, if target string isn't empty, zero matches can't work
	 * so we pretend the min is 1.
	 */
	min_matches = t->min;
	if (min_matches <= 0)
	{
		if (begin == end)
			return REG_OKAY;
		min_matches = 1;
	}

	/*
	 * We need workspace to track the endpoints of each sub-match.  Normally
	 * we consider only nonzero-length sub-matches, so there can be at most
	 * end-begin of them.  However, if min is larger than that, we will also
	 * consider zero-length sub-matches in order to find enough matches.
	 *
	 * For convenience, endpts[0] contains the "begin" pointer and we store
	 * sub-match endpoints in endpts[1..max_matches].
	 */
	max_matches = end - begin;
	if (max_matches > t->max && t->max != DUPINF)
		max_matches = t->max;
	if (max_matches < min_matches)
		max_matches = min_matches;
	endpts = (chr **) MALLOC((max_matches + 1) * sizeof(chr *));
	if (endpts == NULL)
		return REG_ESPACE;
	endpts[0] = begin;

	d = getsubdfa(v, t->left);
	if (ISERR())
	{
		FREE(endpts);
		return v->err;
	}
	MDEBUG(("creviter %d\n", t->id));

	/*
	 * Our strategy is to first find a set of sub-match endpoints that are
	 * valid according to the child node's DFA, and then recursively dissect
	 * each sub-match to confirm validity.  If any validity check fails,
	 * backtrack the last sub-match and try again.  And, when we next try for
	 * a validity check, we need not recheck any successfully verified
	 * sub-matches that we didn't move the endpoints of.  nverified remembers
	 * how many sub-matches are currently known okay.
	 */

	/* initialize to consider first sub-match */
	nverified = 0;
	k = 1;
	limit = begin;

	/* iterate until satisfaction or failure */
	while (k > 0)
	{
		/* disallow zero-length match unless necessary to achieve min */
		if (limit == endpts[k - 1] &&
			limit != end &&
			(k >= min_matches || min_matches - k < end - limit))
			limit++;

		/* if this is the last allowed sub-match, it must reach to the end */
		if (k >= max_matches)
			limit = end;

		/* try to find an endpoint for the k'th sub-match */
		endpts[k] = shortest(v, d, endpts[k - 1], limit, end,
							 (chr **) NULL, (int *) NULL);
		if (ISERR())
		{
			FREE(endpts);
			return v->err;
		}
		if (endpts[k] == NULL)
		{
			/* no match possible, so see if we can lengthen previous one */
			k--;
			goto backtrack;
		}
		MDEBUG(("%d: working endpoint %d: %ld\n",
				t->id, k, LOFF(endpts[k])));

		/* k'th sub-match can no longer be considered verified */
		if (nverified >= k)
			nverified = k - 1;

		if (endpts[k] != end)
		{
			/* haven't reached end yet, try another iteration if allowed */
			if (k >= max_matches)
			{
				/* must try to lengthen some previous match */
				k--;
				goto backtrack;
			}

			k++;
			limit = endpts[k - 1];
			continue;
		}

		/*
		 * We've identified a way to divide the string into k sub-matches that
		 * works so far as the child DFA can tell.  If k is an allowed number
		 * of matches, start the slow part: recurse to verify each sub-match.
		 * We always have k <= max_matches, needn't check that.
		 */
		if (k < min_matches)
			goto backtrack;

		MDEBUG(("%d: verifying %d..%d\n", t->id, nverified + 1, k));

		for (i = nverified + 1; i <= k; i++)
		{
			zaptreesubs(v, t->left);
			er = cdissect(v, t->left, endpts[i - 1], endpts[i]);
			if (er == REG_OKAY)
			{
				nverified = i;
				continue;
			}
			if (er == REG_NOMATCH)
				break;
			/* oops, something failed */
			FREE(endpts);
			return er;
		}

		if (i > k)
		{
			/* satisfaction */
			MDEBUG(("%d successful\n", t->id));
			FREE(endpts);
			return REG_OKAY;
		}

		/* match failed to verify, so backtrack */

backtrack:

		/*
		 * Must consider longer versions of the current sub-match.
		 */
		while (k > 0)
		{
			if (endpts[k] < end)
			{
				limit = endpts[k] + 1;
				/* break out of backtrack loop, continue the outer one */
				break;
			}
			/* can't lengthen k'th sub-match any more, consider previous one */
			k--;
		}
	}

	/* all possibilities exhausted */
	MDEBUG(("%d failed\n", t->id));
	FREE(endpts);
	return REG_NOMATCH;
}



#include "rege_dfa.c"
